#include "ScheduleTaskService.h"

#include "FormatW.h"

#include <vector>
#include <string>

#include <comdef.h>   // [R86] _bstr_t
#include <ole2.h>     // VARIANT / BSTR / CoCreateInstance 基础

#include <taskschd.h>

using namespace DuiLib;

namespace {

// HRESULT 低 32 位作为 DWORD 返回（0 = 成功）
DWORD HrToDword(HRESULT hr)
{
    return hr >= 0 ? 0 : static_cast<DWORD>(hr & 0xFFFFFFFF);
}

// [P1-C3] COM 套间初始化护栏：任意线程调用 COM 前确保已初始化，
// 析构时严格配对 CoUninitialize（RPC_E_CHANGED_MODE 表示本线程已以
// 另一套间模式初始化，此时不可反初始化，交给原拥有者）。
struct ComInitGuard
{
    bool bNeedUninit;
    ComInitGuard() : bNeedUninit(false)
    {
        HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (hr == RPC_E_CHANGED_MODE)
            hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bNeedUninit = SUCCEEDED(hr);   // S_OK / S_FALSE 都需配对 Uninit
    }
    ~ComInitGuard()
    {
        if (bNeedUninit) ::CoUninitialize();
    }
private:
    ComInitGuard(const ComInitGuard&);
    ComInitGuard& operator=(const ComInitGuard&);
};

// 路径规范化：确保以 "\" 开头
CDuiString NormalizeTaskPath(const CDuiString& sTaskPath)
{
    CDuiString sFull = sTaskPath;
    if (!sFull.IsEmpty() && sFull.GetData()[0] != L'\\')
    {
        CDuiString sTmp = L"\\";
        sTmp += sFull;
        sFull = sTmp;
    }
    return sFull;
}

// 创建并连接 ITaskService；失败返回 nullptr。调用方必须 Release。
ITaskService* CreateTaskService()
{
    ITaskService* pService = nullptr;
    HRESULT hr = ::CoCreateInstance(__uuidof(TaskScheduler), nullptr,
                                     CLSCTX_INPROC_SERVER,
                                     __uuidof(ITaskService),
                                     (void**)&pService);
    if (FAILED(hr)) return nullptr;

    hr = pService->Connect(VARIANT{}, VARIANT{}, VARIANT{}, VARIANT{});
    if (FAILED(hr))
    {
        pService->Release();
        return nullptr;
    }
    return pService;
}

// 定位任务（sTaskPath 必须以 "\" 开头）
IRegisteredTask* FindTaskByPath(ITaskService* pService, const CDuiString& sTaskPath)
{
    if (!pService || sTaskPath.IsEmpty()) return nullptr;

    CDuiString sFull = NormalizeTaskPath(sTaskPath);

    ITaskFolder* pRoot = nullptr;
    HRESULT hr = pService->GetFolder(_bstr_t(L"\\"), &pRoot);
    if (FAILED(hr) || !pRoot) return nullptr;

    IRegisteredTask* pTask = nullptr;
    hr = pRoot->GetTask(_bstr_t(sFull.GetData()), &pTask);
    pRoot->Release();
    if (FAILED(hr)) return nullptr;
    return pTask;
}

// TASK_LOGON_TYPE_* → schtasks "Logon Mode" 等价文本
const wchar_t* LogonTypeText(TASK_LOGON_TYPE t)
{
    switch (t)
    {
    case TASK_LOGON_INTERACTIVE_TOKEN:              return L"Interactive only";
    case TASK_LOGON_S4U:                            return L"S4U";
    case TASK_LOGON_PASSWORD:                       return L"Password";
    case TASK_LOGON_SERVICE_ACCOUNT:                return L"Service account";
    case TASK_LOGON_INTERACTIVE_TOKEN_OR_PASSWORD:  return L"Interactive/Password";
    case TASK_LOGON_GROUP:                          return L"Group";
    default:                                        return L"None";
    }
}

// TASK_STATE_* → 与旧 schtasks 英文状态列一致（UI 已按这些词判定）
const wchar_t* StateText(TASK_STATE st)
{
    switch (st)
    {
    case TASK_STATE_DISABLED: return L"Disabled";
    case TASK_STATE_QUEUED:   return L"Queued";
    case TASK_STATE_READY:    return L"Ready";
    case TASK_STATE_RUNNING:  return L"Running";
    default:                  return L"Unknown";
    }
}

// 递归枚举一个文件夹（含子文件夹）内全部任务
HRESULT EnumFolderRecursive(ITaskFolder* pFolder,
                            std::vector<ScheduleTaskService::TaskInfo>& vOut)
{
    if (!pFolder) return E_POINTER;

    // 1) 本文件夹任务（flags=0：包含隐藏任务）
    IRegisteredTaskCollection* pTasks = nullptr;
    HRESULT hr = pFolder->GetTasks(0, &pTasks);
    if (SUCCEEDED(hr) && pTasks)
    {
        LONG nTasks = 0;
        pTasks->get_Count(&nTasks);
        for (LONG i = 1; i <= nTasks; ++i)
        {
            IRegisteredTask* pTask = nullptr;
            {
                VARIANT varIdx;
                ::VariantInit(&varIdx);
                varIdx.vt = VT_I4;
                varIdx.lVal = i;
                hr = pTasks->get_Item(varIdx, &pTask);
            }
            if (FAILED(hr) || !pTask) continue;

            ScheduleTaskService::TaskInfo ti;
            ti.bEnabled = true;
            ti.bRunning = false;

            BSTR bstrPath = nullptr;
            if (SUCCEEDED(pTask->get_Path(&bstrPath)) && bstrPath)
            {
                ti.sName = static_cast<const wchar_t*>(_bstr_t(bstrPath, false));
                ::SysFreeString(bstrPath);
            }
            if (ti.sName.IsEmpty())
            {
                BSTR bstrName = nullptr;
                if (SUCCEEDED(pTask->get_Name(&bstrName)) && bstrName)
                {
                    CDuiString sTmp = L"\\";
                    sTmp += static_cast<const wchar_t*>(_bstr_t(bstrName, false));
                    ti.sName = sTmp;
                    ::SysFreeString(bstrName);
                }
            }

            TASK_STATE st = TASK_STATE_UNKNOWN;
            if (SUCCEEDED(pTask->get_State(&st)))
            {
                ti.sStatus = StateText(st);
                ti.bEnabled = (st != TASK_STATE_DISABLED);
                ti.bRunning = (st == TASK_STATE_RUNNING);
            }

            DATE dtNext = 0.0;
            if (SUCCEEDED(pTask->get_NextRunTime(&dtNext)) && dtNext > 0.0)
            {
                SYSTEMTIME stn = { 0 };
                if (::VariantTimeToSystemTime(dtNext, &stn))
                {
                    std::wstring sNext(32, L'\0');
                    wfmt::FormatInto(&sNext[0], sNext.size(), L"%04u/%02u/%02u %02u:%02u:%02u",
                               stn.wYear, stn.wMonth, stn.wDay,
                               stn.wHour, stn.wMinute, stn.wSecond);
                    sNext.resize(wcslen(sNext.c_str()));
                    ti.sNextRun = sNext.c_str();
                }
            }

            // Run As User / 登录模式：从任务定义的 Principal 取
            ITaskDefinition* pDef = nullptr;
            if (SUCCEEDED(pTask->get_Definition(&pDef)) && pDef)
            {
                IPrincipal* pPrincipal = nullptr;
                if (SUCCEEDED(pDef->get_Principal(&pPrincipal)) && pPrincipal)
                {
                    BSTR bstrUser = nullptr;
                    if (SUCCEEDED(pPrincipal->get_UserId(&bstrUser)) && bstrUser)
                    {
                        ti.sUser = static_cast<const wchar_t*>(_bstr_t(bstrUser, false));
                        ::SysFreeString(bstrUser);
                    }
                    TASK_LOGON_TYPE lt = TASK_LOGON_NONE;
                    if (SUCCEEDED(pPrincipal->get_LogonType(&lt)))
                        ti.sMode = LogonTypeText(lt);
                    pPrincipal->Release();
                }
                pDef->Release();
            }

            vOut.push_back(ti);
            pTask->Release();
        }
        pTasks->Release();
    }

    // 2) 子文件夹递归
    ITaskFolderCollection* pSubFolders = nullptr;
    hr = pFolder->GetFolders(0, &pSubFolders);
    if (SUCCEEDED(hr) && pSubFolders)
    {
        LONG nFolders = 0;
        pSubFolders->get_Count(&nFolders);
        for (LONG i = 1; i <= nFolders; ++i)
        {
            ITaskFolder* pSub = nullptr;
            {
                VARIANT varIdx;
                ::VariantInit(&varIdx);
                varIdx.vt = VT_I4;
                varIdx.lVal = i;
                hr = pSubFolders->get_Item(varIdx, &pSub);
            }
            if (SUCCEEDED(hr) && pSub)
            {
                EnumFolderRecursive(pSub, vOut);
                pSub->Release();
            }
        }
        pSubFolders->Release();
    }

    return S_OK;
}

// XML 特殊字符转义
void AppendXmlEscaped(std::wstring& sOut, const wchar_t* psz)
{
    for (const wchar_t* p = psz; p && *p; ++p)
    {
        switch (*p)
        {
        case L'&':  sOut += L"&amp;";  break;
        case L'<':  sOut += L"&lt;";   break;
        case L'>':  sOut += L"&gt;";   break;
        case L'\"': sOut += L"&quot;"; break;
        case L'\'': sOut += L"&apos;"; break;
        default:    sOut += *p;        break;
        }
    }
}

} // namespace

const CDuiString& ScheduleTaskService::CleanTaskName()
{
    static const CDuiString sTaskName = L"DismTool_Clone_ScheduledClean";
    return sTaskName;
}

// ============================================================
// [P1-C3] 注册每周一 03:00 静默清理任务（ITaskService XML 直连，
// 等价 schtasks /create /sc weekly /d MON /st 03:00 /f）。
// ============================================================
DWORD ScheduleTaskService::CreateWeeklyCleanSilentTask(const CDuiString& sExePath)
{
    ComInitGuard guard;

    ITaskService* pService = CreateTaskService();
    if (!pService) return 0x80004005;   // E_FAIL

    ITaskFolder* pRoot = nullptr;
    HRESULT hr = pService->GetFolder(_bstr_t(L"\\"), &pRoot);
    if (FAILED(hr) || !pRoot)
    {
        pService->Release();
        return HrToDword(hr);
    }

    ITaskDefinition* pDef = nullptr;
    hr = pService->NewTask(0, &pDef);
    if (FAILED(hr) || !pDef)
    {
        pRoot->Release();
        pService->Release();
        return HrToDword(hr);
    }

    // StartBoundary 日期取当天，时间固定 03:00:00（与 schtasks 默认行为一致）
    SYSTEMTIME st = { 0 };
    ::GetLocalTime(&st);
    std::wstring sBoundary(32, L'\0');
    wfmt::FormatTo(&sBoundary[0], sBoundary.size(), L"%04u-%02u-%02uT03:00:00",
                 st.wYear, st.wMonth, st.wDay);
    sBoundary.resize(wcslen(sBoundary.c_str()));

    std::wstring sXml;
    sXml += L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>";
    sXml += L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">";
    sXml += L"<RegistrationInfo>";
    sXml += L"<Description>DismTool clone weekly silent cleanup</Description>";
    sXml += L"</RegistrationInfo>";
    sXml += L"<Triggers>";
    sXml += L"<CalendarTrigger>";
    sXml += L"<StartBoundary>";
    sXml += sBoundary;
    sXml += L"</StartBoundary>";
    sXml += L"<Enabled>true</Enabled>";
    sXml += L"<ScheduleByWeek>";
    sXml += L"<DaysOfWeek><Monday/></DaysOfWeek>";
    sXml += L"<WeeksInterval>1</WeeksInterval>";
    sXml += L"</ScheduleByWeek>";
    sXml += L"</CalendarTrigger>";
    sXml += L"</Triggers>";
    sXml += L"<Principals>";
    sXml += L"<Principal id=\"Author\">";
    sXml += L"<LogonType>InteractiveToken</LogonType>";
    sXml += L"<RunLevel>HighestAvailable</RunLevel>";
    sXml += L"</Principal>";
    sXml += L"</Principals>";
    sXml += L"<Settings>";
    sXml += L"<MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>";
    sXml += L"<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>";
    sXml += L"<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>";
    sXml += L"<AllowHardTerminate>true</AllowHardTerminate>";
    sXml += L"<StartWhenAvailable>false</StartWhenAvailable>";
    sXml += L"<RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>";
    sXml += L"<IdleSettings><StopOnIdleEnd>false</IdleSettings><RestartOnIdle>false</IdleSettings>";
    sXml += L"<AllowStartOnDemand>true</AllowStartOnDemand>";
    sXml += L"<Enabled>true</Enabled>";
    sXml += L"<Hidden>false</Hidden>";
    sXml += L"<RunOnlyIfIdle>false</RunOnlyIfIdle>";
    sXml += L"<WakeToRun>false</WakeToRun>";
    sXml += L"<ExecutionTimeLimit>PT2H</ExecutionTimeLimit>";
    sXml += L"<Priority>7</Priority>";
    sXml += L"</Settings>";
    sXml += L"<Actions Context=\"Author\">";
    sXml += L"<Exec>";
    sXml += L"<Command>";
    AppendXmlEscaped(sXml, sExePath.GetData());
    sXml += L"</Command>";
    sXml += L"<Arguments>/silent</Arguments>";
    sXml += L"</Exec>";
    sXml += L"</Actions>";
    sXml += L"</Task>";

    hr = pDef->put_XmlText(_bstr_t(sXml.c_str()));
    if (FAILED(hr))
    {
        pDef->Release();
        pRoot->Release();
        pService->Release();
        return HrToDword(hr);
    }

    IRegisteredTask* pReg = nullptr;
    hr = pRoot->RegisterTaskDefinition(
        _bstr_t(CleanTaskName().GetData()), pDef, TASK_CREATE_OR_UPDATE,
        VARIANT{}, VARIANT{}, TASK_LOGON_INTERACTIVE_TOKEN, VARIANT{}, &pReg);
    if (pReg) pReg->Release();
    pDef->Release();
    pRoot->Release();
    pService->Release();
    return HrToDword(hr);
}

// [P1-C3] 删除定期清理任务（不存在 0x80070002 视为成功，与 /f 语义一致）
DWORD ScheduleTaskService::DeleteCleanTask()
{
    DWORD dw = DeleteTaskCOM(CleanTaskName());
    if (dw == 0x80070002 || dw == 0x80070490) return 0;
    return dw;
}

// ============================================================
// [P1-C3] 递归枚举全部任务（ITaskService 直连，替代
// schtasks /query /fo csv /v 进程 + CSV 文本解析）。
// ============================================================
int ScheduleTaskService::EnumTasks(std::vector<TaskInfo>& vOut, DWORD& dwErrOut)
{
    dwErrOut = 0;
    vOut.clear();

    ComInitGuard guard;

    ITaskService* pService = CreateTaskService();
    if (!pService) { dwErrOut = 0x80004005; return 1; }

    ITaskFolder* pRoot = nullptr;
    HRESULT hr = pService->GetFolder(_bstr_t(L"\\"), &pRoot);
    if (FAILED(hr) || !pRoot)
    {
        pService->Release();
        dwErrOut = HrToDword(hr);
        return 2;
    }

    EnumFolderRecursive(pRoot, vOut);
    pRoot->Release();
    pService->Release();
    return 0;
}

bool ScheduleTaskService::IsTaskServiceAvailable()
{
    ComInitGuard guard;
    ITaskService* pService = CreateTaskService();
    if (!pService) return false;
    pService->Release();
    return true;
}

DWORD ScheduleTaskService::SetTaskEnabledCOM(const CDuiString& sTaskPath, bool bEnable)
{
    ComInitGuard guard;
    ITaskService* pService = CreateTaskService();
    if (!pService) return 0x80004005;   // E_FAIL

    IRegisteredTask* pTask = FindTaskByPath(pService, sTaskPath);
    pService->Release();
    if (!pTask) return 0x80070490;      // ERROR_NOT_FOUND

    HRESULT hr = pTask->put_Enabled(bEnable ? VARIANT_TRUE : VARIANT_FALSE);
    pTask->Release();
    return HrToDword(hr);
}

DWORD ScheduleTaskService::RunTaskCOM(const CDuiString& sTaskPath)
{
    ComInitGuard guard;
    ITaskService* pService = CreateTaskService();
    if (!pService) return 0x80004005;

    IRegisteredTask* pTask = FindTaskByPath(pService, sTaskPath);
    pService->Release();
    if (!pTask) return 0x80070490;

    IRunningTask* pRunning = nullptr;
    HRESULT hr = pTask->Run(VARIANT{}, &pRunning);
    if (pRunning) pRunning->Release();
    pTask->Release();
    return HrToDword(hr);
}

DWORD ScheduleTaskService::DeleteTaskCOM(const CDuiString& sTaskPath)
{
    ComInitGuard guard;
    ITaskService* pService = CreateTaskService();
    if (!pService) return 0x80004005;

    CDuiString sFull = NormalizeTaskPath(sTaskPath);

    ITaskFolder* pRoot = nullptr;
    HRESULT hr = pService->GetFolder(_bstr_t(L"\\"), &pRoot);
    if (FAILED(hr) || !pRoot)
    {
        pService->Release();
        return HrToDword(hr);
    }

    hr = pRoot->DeleteTask(_bstr_t(sFull.GetData()), 0);
    pRoot->Release();
    pService->Release();
    return HrToDword(hr);
}

// ============================================================
// [R127] 计划任务 XML 导出 / 导入（ITaskDefinition 直连）
// 前置：内部已自管 COM 初始化；失败返回分级码 + dwErrOut 回传 HRESULT。
// ============================================================
int ScheduleTaskService::ExportTaskXml(const CDuiString& sTaskPath,
                                       CDuiString& sXmlOut, DWORD& dwErrOut)
{
    dwErrOut = 0;
    sXmlOut.Empty();

    ComInitGuard guard;
    ITaskService* pService = CreateTaskService();
    if (!pService) return 1;

    IRegisteredTask* pTask = FindTaskByPath(pService, sTaskPath);
    if (!pTask)
    {
        pService->Release();
        return 2;
    }

    ITaskDefinition* pDef = nullptr;
    HRESULT hr = pTask->get_Definition(&pDef);
    pTask->Release();
    if (FAILED(hr) || !pDef)
    {
        pService->Release();
        dwErrOut = HrToDword(hr);
        return 3;
    }

    BSTR bstrXml = nullptr;
    hr = pDef->get_XmlText(&bstrXml);
    pDef->Release();
    pService->Release();
    if (FAILED(hr))
    {
        dwErrOut = HrToDword(hr);
        return 3;
    }

    if (bstrXml)
    {
        sXmlOut = (const wchar_t*)(_bstr_t(bstrXml, false));
        ::SysFreeString(bstrXml);
    }
    return 0;
}

int ScheduleTaskService::ImportTaskXml(const CDuiString& sTaskPath,
                                       const CDuiString& sXml, DWORD& dwErrOut)
{
    dwErrOut = 0;
    if (sXml.IsEmpty()) return 6;

    ComInitGuard guard;
    ITaskService* pService = CreateTaskService();
    if (!pService) return 1;

    CDuiString sFull = NormalizeTaskPath(sTaskPath);

    ITaskFolder* pRoot = nullptr;
    HRESULT hr = pService->GetFolder(_bstr_t(L"\\"), &pRoot);
    if (FAILED(hr) || !pRoot)
    {
        pService->Release();
        dwErrOut = HrToDword(hr);
        return 2;
    }

    ITaskDefinition* pDef = nullptr;
    hr = pService->NewTask(0, &pDef);
    if (FAILED(hr) || !pDef)
    {
        pRoot->Release();
        pService->Release();
        dwErrOut = HrToDword(hr);
        return 3;
    }

    hr = pDef->put_XmlText(_bstr_t(sXml.GetData()));
    if (FAILED(hr))
    {
        pDef->Release();
        pRoot->Release();
        pService->Release();
        dwErrOut = HrToDword(hr);
        return 4;
    }

    IRegisteredTask* pReg = nullptr;
    hr = pRoot->RegisterTaskDefinition(
        _bstr_t(sFull.GetData()), pDef, TASK_CREATE_OR_UPDATE,
        VARIANT{}, VARIANT{}, TASK_LOGON_INTERACTIVE_TOKEN, VARIANT{}, &pReg);
    pDef->Release();
    pRoot->Release();
    pService->Release();
    if (pReg) pReg->Release();
    if (FAILED(hr))
    {
        dwErrOut = HrToDword(hr);
        return 5;
    }
    return 0;
}
// [C++17 R3] 本文件内联常量（C++17 inline variables，多 TU ODR 一致）：
