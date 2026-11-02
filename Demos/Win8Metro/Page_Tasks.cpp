#include "stdafx.h"
#include "ThemeTokens.h"   // [R203] 颜色令牌（深色/浅色跟随）

// Page_Tasks.cpp —— v2 P2b 纵切（页 11：计划任务）
// 9 个函数 + TaskEntry/m_vTasks 从 FrameWnd.cpp 原样迁入。
// schtasks 查询经 ScheduleTaskService（S11），进程能力在 ProcessRunner。

#include "Page_Tasks.h"
#include "WmMessages.h"        // [R13 P0收敛] [R-B84] WM_TASK_DATA 定义在 WmMessages.h；UI 专属常量零使用
#include "CustomControls.h"      // CClickRowUI
#include "StringManager.h"
#include "ScheduleTaskService.h"
#include "FrameWnd_UiHelpers.h"  // [R102] UiHelpers_ShowUnifiedListContextMenu
#include "Win32DialogService.h"  // [R127] DlgSaveFile（导出任务 XML）
#include "FrameWnd_Util.h"       // [R127] FrameWnd_Util_CopyTextToClipboard
#include "SafeThread.h"          // 任务查询后台线程异常边界
#include <fstream>   // [C17] std::ofstream（F17_C_fileio 清零）

using namespace DuiLib;
using i18n::CStringManager;

// [P1-C3] 计划任务枚举改 Task Scheduler COM 结构化返回，无需 CSV 解析。

// 构造计划任务列表的数据单元格，列宽与 inc_task_panel.xml 表头严格对齐
static CTextUI* MakeTaskCell(const wchar_t* pszText, int nWidth, const wchar_t* pszAlign,
                             DWORD dwColor, bool bBold)
{
    auto pCell = std::make_unique<CTextUI>(); // [C++17 R3]
    pCell->SetText(pszText);
    if (nWidth > 0) pCell->SetFixedWidth(nWidth);
    pCell->SetTextColor(dwColor);
    pCell->SetAttribute(L"align", pszAlign);
    pCell->SetAttribute(L"valign", L"vcenter");
    pCell->SetAttribute(L"textpadding", L"8,3,8,3");
    pCell->SetAttribute(L"endellipsis", L"true");
    if (bBold) pCell->SetFont(6);
    pCell->SetMouseEnabled(false);
    return pCell.release(); // [C++17 R3] ownership transferred to caller
}

// [R-B84/P1-C3] 计划任务枚举后台线程：ITaskService COM 递归枚举
// （ScheduleTaskService 内部自管 CoInitializeEx）；严禁触碰 DuiLib
// 控件。查询完成后把堆上的 TaskQueryMsg 经 WM_TASK_DATA 回送 UI 线程
// 落地。宿主窗口已销毁时 PostMessage 失败，自行 delete，避免泄漏。
// [R-TAILFIX 同型] 枚举体拆两层：ScheduleTaskService 内部自管 COM 初始化，
// 但 COM 调用链（ITaskService 递归枚举）任何硬件异常若穿透线程过程，
// WM_TASK_DATA 永不投递 → m_bQuerying 恒为 true，计划任务页永久停在
// "正在读取计划任务"且后续刷新被门控吞掉。__except 兜底后仍回投
// （nRet 保持失败默认值），UI 收口逻辑零改动。
static void TaskQueryBody(HWND hNotify, CPageTasks::TaskQueryMsg* pMsg)
{
    pMsg->nRet = ScheduleTaskService::EnumTasks(pMsg->vTasks, pMsg->dwErr);
}
// 本函数不含需要展开的 C++ 对象，允许直接使用 __try（MSVC C2712 约束）。
static void TaskQuerySeh(HWND hNotify, CPageTasks::TaskQueryMsg* pMsg)
{
    __try
    {
        TaskQueryBody(hNotify, pMsg);
    }
    __except (CrashGuardFilter(GetExceptionInformation(), "task-enum-seh"))
    {
        pMsg->nRet = -1;   // 失败收口：UI 走"查询失败"分支并复位 m_bQuerying
    }
}
static DWORD WINAPI TaskQueryThreadProc(void* lpParam)
{
    HWND hNotify = static_cast<HWND>(lpParam);

    // [C++17 阶段3] Post 消息所有权移交 UI，Post 失败自删
    auto pMsg = std::make_unique<CPageTasks::TaskQueryMsg>(); // [C++17 R3]

    TaskQuerySeh(hNotify, pMsg.get());

    // [FIX-S1-9] Post 失败有限重试：DONE 丢失会让 m_bQuerying 永久残留
    //（唯一复位点在 UI 收口），页面停留"正在读取"且刷新被守卫吞掉。
    if (::IsWindow(hNotify))
    {
        for (int nRetry = 0; nRetry < 10; ++nRetry)
        {
            if (::PostMessageW(hNotify, WM_TASK_DATA, 0,
                               msgparam::ToParam(pMsg.get())))
            {
                pMsg.release();   // [C++17 R3] ownership moved to UI thread (OnTaskData deletes)
                return 0;
            }
            if (!::IsWindow(hNotify)) break;
            ::Sleep(100);
        }
    }
    return 0;   // [C++17 R3] unique_ptr auto-frees on scope exit（窗口亡=进程将退）
}

void CPageTasks::Build()
{
    CVerticalLayoutUI* pList = static_cast<CVerticalLayoutUI*>(m_ctx.HostFindControl(L"task_list"));

    if (!pList) return;

    pList->RemoveAll();

    m_nSelected = -1;   // [R78] 每次重建列表时清空选中态
    m_vTasks.clear();   // [R78] 重建前清空缓存，避免重复累计

    UpdateCount(0);

    // [R-B84] schtasks /query /fo csv /v 输出数百 KB、耗时数秒；旧版在 UI
    // 线程同步执行（且 RunHiddenCapture 先等进程退出再读管道，必写满死锁
    // 30 秒），切到"计划任务"Tab 即界面"未响应"。改为后台线程枚举：先放
    // 一行等待提示，结果由 WM_TASK_DATA → OnTaskData 落地。
    auto pWait = std::make_unique<CTextUI>(); // [C++17 R3]

    pWait->SetText(L"正在读取计划任务，请稍候…");

    pWait->SetTextColor(tokens::Token(tokens::EToken::InkMuted));

    pWait->SetFixedHeight(28);

    pWait->SetAttribute(L"align", L"center");

    pWait->SetMouseEnabled(false);

    pList->Add(pWait.release());  // [C++17 R3] ownership to DuiLib

    if (m_bQuerying) return;  // 已有枚举在途，忽略重复的 Tab 切换/刷新

    m_bQuerying = true;

    HANDLE hThread = SafeCreateThread(TaskQueryThreadProc,
                                      reinterpret_cast<void*>(m_ctx.HostHwnd()));

    if (hThread)
    {
        ::CloseHandle(hThread);
        return;
    }

    // 极端情况（建线程失败）：撤掉等待行，显示与查询失败一致的提示
    m_bQuerying = false;

    pList->RemoveAll();

    auto pEmpty = std::make_unique<CTextUI>(); // [C++17 R3]

    pEmpty->SetText(StringMgr.Get("misc.hbc12"));

    pEmpty->SetTextColor(tokens::Token(tokens::EToken::InkMuted));

    pEmpty->SetFixedHeight(28);

    pEmpty->SetAttribute(L"align", L"center");

    pList->Add(pEmpty.release());  // [C++17 R3] ownership to DuiLib

    m_ctx.HostLog(StringMgr.Get("misc.h42b1"));
}

// [R-B84/P1-C3] WM_TASK_DATA 落地：COM 结构化结果直接生成行控件（全程 UI 线程）。
// pMsg 由工作线程 new，本函数负责 delete。
// [R168] 从 OnTaskData 抽出：构建一行计划任务并同步缓存。
//   返回 false 表示该条目被跳过（名称为空，通常是 COM 枚举出的占位项）。
//
//   列宽必须与表头严格对齐：300 / 180 / 120 / 100 / 0（自适应）。
//   这五个数值原本散在函数中部，改表头时极易只改一处 —— 集中到这里后，
//   行与表头用的是同一组常量（表头在 BuildList，那边也引用同样的数值）。
//
//   [R78] 行内容与 m_vTasks 缓存必须同步维护：前者供显示，后者供
//   CurrentSelectedIndex() 与行点击选中。分开写会出现"行显示了但缓存没有"，
//   表现为点某行取不到数据。
bool CPageTasks::TaskAppendRow(CVerticalLayoutUI* pList,
                               const ScheduleTaskService::TaskInfo& ti, int nRowIndex)
{
    if (ti.sName.IsEmpty()) return false;

    // 状态文案由 COM 枚举层给出固定英文（Ready/Running/Disabled/...）
    const CDuiString sTaskName = ti.sName;
    const CDuiString sNextRun = ti.sNextRun;
    const CDuiString sStatus = ti.sStatus;
    const CDuiString sMode = ti.sMode;
    const CDuiString sUser = ti.sUser;
    const bool bReady = ti.bRunning
                     || sStatus.Find(L"Ready") >= 0
                     || sStatus.Find(L"Queued") >= 0;

    auto pRow = std::make_unique<CClickRowUI>(); // [C++17 R3]
    pRow->SetFixedHeight(28);

    CDuiString sRowName;
    sRowName.Format(L"taskrow_%d", nRowIndex);
    pRow->SetName(sRowName.GetData());

    // 5 个数据列，列宽与表头 300/180/120/100/自适应 对齐
    pRow->Add(MakeTaskCell(sTaskName.GetData(), 300, L"left", tokens::Token(tokens::EToken::Ink), true));
    pRow->Add(MakeTaskCell(sNextRun.GetData(), 180, L"center", tokens::Token(tokens::EToken::InkSecondary), false));
    pRow->Add(MakeTaskCell(sMode.GetData(), 120, L"center", tokens::Token(tokens::EToken::InkSecondary), false));
    pRow->Add(MakeTaskCell(sStatus.GetData(), 100, L"center",
                           bReady ? tokens::Token(tokens::EToken::Success)
                           : tokens::Token(tokens::EToken::InkMuted), false));
    pRow->Add(MakeTaskCell(sUser.GetData(), 0, L"left", tokens::Token(tokens::EToken::InkSecondary), false));

    // [R78] 同步缓存，供 CurrentSelectedIndex() 与行点击选中使用
    TaskEntry entry;
    entry.sName = sTaskName;
    entry.sStatus = sStatus;
    entry.sNextRun = sNextRun;
    entry.sMode = sMode;
    entry.sUser = sUser;
    entry.bEnabled = ti.bEnabled;
    m_vTasks.push_back(entry);

    pList->Add(pRow.release());  // [C++17 R3] ownership to DuiLib
    return true;
}

void CPageTasks::OnTaskData(TaskQueryMsg* pMsg)
{
    m_bQuerying = false;

    if (!pMsg) return;

    const int nRet = pMsg->nRet;
    const DWORD dwErr = pMsg->dwErr;
    std::vector<ScheduleTaskService::TaskInfo> vSrc;
    vSrc.swap(pMsg->vTasks);

    // [C++17 R3] 消息接收端 unique_ptr 接管，所有权由投递方移交
    std::unique_ptr<CPageTasks::TaskQueryMsg> spMsg(pMsg);
    pMsg = spMsg.get();

    CVerticalLayoutUI* pList = static_cast<CVerticalLayoutUI*>(m_ctx.HostFindControl(L"task_list"));

    if (!pList) return;

    pList->RemoveAll();

    m_nSelected = -1;

    m_vTasks.clear();

    if (nRet != 0)
    {
        auto pEmpty = std::make_unique<CTextUI>(); // [C++17 R3]

        CDuiString sEmpty = StringMgr.Get("misc.hbc12");
        CDuiString sEmpty2;
        sEmpty2.Format(L"%s (hr=0x%08X)", sEmpty.GetData(), (unsigned)dwErr);
        pEmpty->SetText(sEmpty2.GetData());

        pEmpty->SetTextColor(tokens::Token(tokens::EToken::InkMuted));

        pEmpty->SetFixedHeight(28);

        pEmpty->SetAttribute(L"align", L"center");

        pList->Add(pEmpty.release());  // [C++17 R3] ownership to DuiLib

        m_ctx.HostLog(StringMgr.Get("misc.h42b1"));

        return;
    }

    int nCount = 0;

    for (size_t k = 0; k < vSrc.size(); ++k)
    {
        // [R168] 行构建 + 缓存同步抽为 TaskAppendRow（124→约 70 行）
        if (TaskAppendRow(pList, vSrc[k], nCount)) ++nCount;
    }

    CDuiString sLog;

    sLog.Format(StringMgr.Get("misc.hbebf"), nCount);

    m_ctx.HostLog(sLog.GetData());

    // 同步搜索栏右侧的“共 N 项计划任务”计数
    UpdateCount(nCount);

    // 显示查询结果 Toast
    if (nCount > 0)
    {
        CDuiString sToastTitle = StringMgr.Get("misc.h6f40");

        CDuiString sToastText;

        sToastText.Format(StringMgr.Get("misc.hf4a1"), nCount);

        m_ctx.HostToast(sToastTitle.GetData(), sToastText.GetData(), 0);
    }
}

// [R63] 更新任务计数
void CPageTasks::UpdateCount(int nCount)
{
    CTextUI* pCount = static_cast<CTextUI*>(m_ctx.HostFindControl(L"task_count"));
    if (pCount)
    {
        CDuiString sText;
        sText.Format(StringMgr.Get("misc.hf4a1"), nCount);
        pCount->SetText(sText.GetData());
    }
}

// [R78] 取当前选中索引；越界或无选中返回 -1
[[nodiscard]] int CPageTasks::CurrentSelectedIndex() const
{
    if (m_nSelected < 0) return -1;
    if (static_cast<size_t>(m_nSelected) >= m_vTasks.size()) return -1;
    return m_nSelected;
}

// [R78] 任务右键菜单
// [R102] 升级为统一列表右键菜单通道 UiHelpers_ShowUnifiedListContextMenu（EListCtx::LCX_OTHER 类别）
void CPageTasks::ShowContextMenu(int nTaskIndex, POINT pt)
{
    if (nTaskIndex < 0 || nTaskIndex >= static_cast<int>(m_vTasks.size())) return;

    TaskEntry& task = m_vTasks[nTaskIndex];

    // [R127] 行级任务专属右键菜单：打开任务计划程序 / 导出 XML / 复制路径 /
    //        复制任务名 / 立即运行 / 启用 / 禁用 / 删除
    HMENU hMenu = ::CreatePopupMenu();
    ::AppendMenu(hMenu, MF_STRING, 4501, StringMgr.Get("task.s_menu_open_sched"));
    ::AppendMenu(hMenu, MF_STRING, 4502, StringMgr.Get("task.s_menu_export_xml"));
    ::AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenu(hMenu, MF_STRING, 4503, StringMgr.Get("task.s_menu_copy_path"));
    ::AppendMenu(hMenu, MF_STRING, 4504, StringMgr.Get("task.s_menu_copy_name"));
    ::AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenu(hMenu, MF_STRING, 4505, StringMgr.Get("misc.s21647f47"));    // 运行
    ::AppendMenu(hMenu, MF_STRING | (task.bEnabled ? 0 : MF_CHECKED),
                 4506, StringMgr.Get("misc.se5f9be69"));                       // 启用
    ::AppendMenu(hMenu, MF_STRING | (task.bEnabled ? MF_CHECKED : 0),
                 4507, StringMgr.Get("misc.s8ee5d2cf"));                       // 禁用
    ::AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenu(hMenu, MF_STRING, 4508, StringMgr.Get("misc.h2dd1"));    // 删除(&D)

    UINT uCmd = ::TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                 pt.x, pt.y, 0, m_ctx.HostHwnd(), nullptr);
    ::DestroyMenu(hMenu);

    if (uCmd >= 4501 && uCmd <= 4508)
        OnTaskMenuCmd(nTaskIndex, static_cast<int>(uCmd));

    // 保留原日志行为，便于排查
    CDuiString sLog;
    sLog.Format(StringMgr.Get("task.s_contextmenu"), task.sName.GetData());
    m_ctx.HostLog(sLog.GetData());
}

// [R127] 行级右键菜单命令分发
void CPageTasks::OnTaskMenuCmd(int k, int iCmd)
{
    // [修复 P1-TASK] 运行/启用/禁用/删除四个无参命令内部读取
    // m_nSelected（仅左键设置且受旧 +9 bug 污染）；右键菜单已携带正确
    // 命中行 k，此处先把选中行同步为 k，避免操作到错误任务。
    if (k >= 0 && static_cast<size_t>(k) < m_vTasks.size()) m_nSelected = k;
    switch (iCmd)
    {
    case 4501: OnOpenTaskScheduler(k); break;
    case 4502: OnExportTaskXml(k); break;
    case 4503: OnCopyTaskPath(k); break;
    case 4504: OnCopyTaskName(k); break;
    case 4505: OnRun(); break;
    case 4506: OnToggle(true); break;
    case 4507: OnToggle(false); break;
    case 4508: OnDelete(); break;
    default: break;
    }
}

// [R127] 打开"任务计划程序"（taskschd.msc）
void CPageTasks::OnOpenTaskScheduler(int k)
{
    HINSTANCE h = ::ShellExecuteW(m_ctx.HostHwnd(), L"open",
                                  L"taskschd.msc", nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(h) <= 32)
    {
        m_ctx.HostLog(StringMgr.Get("task.s_open_sched_fail"), static_cast<int>(ELogLevel::LOG_ERROR));
        m_ctx.HostToast(StringMgr.Get("task.s_err_open_sched"),
                        StringMgr.Get("task.s_open_sched_fail"), 2);
    }
    else
    {
        CDuiString sLog;
        sLog.Format(L"[task] opened scheduler for: %s", m_vTasks[k].sName.GetData());
        m_ctx.HostLog(sLog.GetData());
    }
}

// [R127] 导出任务 XML：SaveFileDialog 选路径 → ScheduleTaskService::ExportTaskXml
void CPageTasks::OnExportTaskXml(int k)
{
    if (k < 0 || k >= static_cast<int>(m_vTasks.size())) return;
    const TaskEntry& task = m_vTasks[k];

    // 生成默认文件名（取任务名最后一段，去掉非法字符）
    CDuiString sBase = task.sName;
    int nSlash = sBase.ReverseFind(L'\\');
    if (nSlash >= 0) sBase = sBase.Mid(nSlash + 1);
    for (int i = 0; i < sBase.GetLength(); ++i)
    {
        wchar_t c = sBase.GetData()[i];
        if (c == L'/' || c == L':' || c == L'*' || c == L'?'
            || c == L'"' || c == L'<' || c == L'>' || c == L'|')
            sBase.SetAt(i, L'_');
    }
    CDuiString sInit;
    sInit.Format(L"%s.xml", sBase.GetData());

    DuiLib::CDuiString sOut;
    if (!Win32DialogService::DlgSaveFile(sOut,
            L"XML 文件 (*.xml)|*.xml|",
            StringMgr.Get("task.s_export_dlg_title"), sInit))
        return;

    DWORD dwErr = 0;
    CDuiString sXml;
    int nRes = ScheduleTaskService::ExportTaskXml(task.sName, sXml, dwErr);
    if (nRes == 0)
    {
        // 写文件（UTF-8 BOM，与项目源码规范一致）
        std::ofstream ofs(sOut.GetData(), std::ios::binary | std::ios::trunc);
        if (ofs.is_open())
        {
            const char* szBom = "\xEF\xBB\xBF";
            ofs.write(szBom, 3);
            int nLen = WideCharToMultiByte(CP_UTF8, 0, sXml.GetData(), -1, nullptr, 0, nullptr, nullptr);
            if (nLen > 0)
            {
                std::vector<char> vBuf(nLen, 0);
                WideCharToMultiByte(CP_UTF8, 0, sXml.GetData(), -1, vBuf.data(), nLen, nullptr, nullptr);
                ofs.write(vBuf.data(), nLen - 1);
            }
            CDuiString sLog;
            sLog.Format(L"[task] exported xml: %s -> %s", task.sName.GetData(), sOut.GetData());
            m_ctx.HostLog(sLog.GetData());
            m_ctx.HostToast(StringMgr.Get("task.s_export_ok"),
                            sOut.GetData(), 0);
        }
        else
        {
            m_ctx.HostLog(StringMgr.Get("task.s_err_write"), static_cast<int>(ELogLevel::LOG_ERROR));
            m_ctx.HostToast(StringMgr.Get("task.s_export_fail"), sOut.GetData(), 2);
        }
    }
    else
    {
        CDuiString sErr;
        sErr.Format(L"%s | code=%d hr=0x%08X",
                    StringMgr.Get("task.s_export_fail"), nRes, (unsigned)dwErr);
        m_ctx.HostLog(sErr.GetData(), static_cast<int>(ELogLevel::LOG_ERROR));
        m_ctx.HostToast(StringMgr.Get("task.s_export_fail"), sErr.GetData(), 2);
    }
}

// [R127] 复制任务完整路径 / 任务名
void CPageTasks::OnCopyTaskPath(int k)
{
    if (k < 0 || k >= static_cast<int>(m_vTasks.size())) return;
    FrameWnd_Util_CopyTextToClipboard(m_ctx.HostHwnd(), m_vTasks[k].sName.GetData());
    m_ctx.HostToast(StringMgr.Get("task.s_copied_path"),
                    m_vTasks[k].sName.GetData(), 0);
}

void CPageTasks::OnCopyTaskName(int k)
{
    if (k < 0 || k >= static_cast<int>(m_vTasks.size())) return;
    CDuiString sName = m_vTasks[k].sName;
    int nSlash = sName.ReverseFind(L'\\');
    if (nSlash >= 0) sName = sName.Mid(nSlash + 1);
    FrameWnd_Util_CopyTextToClipboard(m_ctx.HostHwnd(), sName.GetData());
    m_ctx.HostToast(StringMgr.Get("task.s_copied_name"), sName.GetData(), 0);
}

// [R102] 行级右键菜单（taskrow_%d 命中）
bool CPageTasks::OnContextMenu(POINT pt, bool bFromContextMsg)
{
    if (bFromContextMsg) return false;

    CControlUI* pHit = m_ctx.HostFindControlAt(pt);
    if (!pHit) return false;

    for (CControlUI* p = pHit; p; p = p->GetParent())
    {
        const wchar_t* psz = p->GetName();
        if (!psz) continue;
        if (wcsncmp(psz, L"taskrow_", 8) != 0) continue;

        int nRow = _ttoi(psz + 8);
        if (nRow < 0 || static_cast<size_t>(nRow) >= m_vTasks.size()) return false;

        ShowContextMenu(nRow, pt);
        return true;
    }
    return false;
}

// [R78/P1-C3] 任务启停：ITaskService COM 直连，零 schtasks 进程。
void CPageTasks::OnToggle(bool bEnable)
{
    int nSel = CurrentSelectedIndex();
    if (nSel < 0 || nSel >= static_cast<int>(m_vTasks.size()))
    {
        m_ctx.HostToast(StringMgr.Get("task.s_noselected"),
                        StringMgr.Get("misc.s2e69cd5a"), 1);
        return;
    }

    const CDuiString& sName = m_vTasks[nSel].sName;
    DWORD dwCom = ScheduleTaskService::SetTaskEnabledCOM(sName, bEnable);

    CDuiString sLog;
    sLog.Format(StringMgr.Get("task.s_com_used"), sName.GetData(), static_cast<int>(dwCom));

    if (dwCom != 0)
    {
        sLog += L" | FAIL";
        m_ctx.HostLog(sLog.GetData(), static_cast<int>(ELogLevel::LOG_ERROR));
        CDuiString sErr;
        sErr.Format(L"0x%08X", (unsigned)dwCom);
        m_ctx.HostToast(StringMgr.Get("task.s_toggle_fail_title"), sErr.GetData(), 2);
        Build();
        return;
    }

    m_vTasks[nSel].bEnabled = bEnable;
    m_vTasks[nSel].sStatus = bEnable ? StringMgr.Get("task.s_enabled")
                                     : StringMgr.Get("task.s_disabled");

    m_ctx.HostLog(sLog.GetData());
    Build();
}

void CPageTasks::OnRun()
{
    int nSel = CurrentSelectedIndex();
    if (nSel < 0 || nSel >= static_cast<int>(m_vTasks.size()))
    {
        m_ctx.HostToast(StringMgr.Get("task.s_noselected"),
                        StringMgr.Get("misc.s2e69cd5a"), 1);
        return;
    }

    const CDuiString& sName = m_vTasks[nSel].sName;
    DWORD dwCom = ScheduleTaskService::RunTaskCOM(sName);

    CDuiString sLog;
    sLog.Format(StringMgr.Get("task.s_com_used"), sName.GetData(), static_cast<int>(dwCom));

    if (dwCom != 0)
    {
        sLog += L" | FAIL";
        m_ctx.HostLog(sLog.GetData(), static_cast<int>(ELogLevel::LOG_ERROR));
        CDuiString sErr;
        sErr.Format(L"0x%08X", (unsigned)dwCom);
        m_ctx.HostToast(StringMgr.Get("task.s_run_fail_title"), sErr.GetData(), 2);
        Build();
        return;
    }

    m_ctx.HostLog(sLog.GetData());

    Build();
}

void CPageTasks::OnDelete()
{
    int nSel = CurrentSelectedIndex();
    if (nSel < 0 || nSel >= static_cast<int>(m_vTasks.size()))
    {
        m_ctx.HostToast(StringMgr.Get("task.s_noselected"),
                        StringMgr.Get("misc.s2e69cd5a"), 1);
        return;
    }

    const CDuiString& sName = m_vTasks[nSel].sName;
    // [FIX-S1-2] 删除计划任务为不可逆操作，删除前必须二次确认
    //（此前按钮/右键直达 DeleteTaskCOM，误触即永久删除）。
    // 文案无现成 i18n 键，使用直述双语安全文案（标题+任务名）。
    if (!m_ctx.HostConfirmYesNo(L"删除计划任务", sName))
        return;
    DWORD dwCom = ScheduleTaskService::DeleteTaskCOM(sName);

    CDuiString sLog;
    sLog.Format(StringMgr.Get("task.s_com_used"), sName.GetData(), static_cast<int>(dwCom));

    if (dwCom != 0)
    {
        sLog += L" | FAIL";
        m_ctx.HostLog(sLog.GetData(), static_cast<int>(ELogLevel::LOG_ERROR));
        CDuiString sErr;
        sErr.Format(L"0x%08X", (unsigned)dwCom);
        m_ctx.HostToast(StringMgr.Get("task.s_delete_fail_title"), sErr.GetData(), 2);
        Build();
        return;
    }

    m_ctx.HostLog(sLog.GetData());

    Build();
}

void CPageTasks::OnTabChanged([[maybe_unused]] int idx)
{
}

// [R105] 计划任务名称过滤：task_filter 输入 → 隐藏非命中 taskrow_ 行。
// 采用"隐藏而非重建"策略，保证 taskrow_%d 行号与 m_vTasks 序号严格对齐。
void CPageTasks::ApplyFilter()
{
    CEditUI* pEdit = static_cast<CEditUI*>(m_ctx.HostFindControl(L"task_filter"));
    CVerticalLayoutUI* pList = static_cast<CVerticalLayoutUI*>(m_ctx.HostFindControl(L"task_list"));
    if (!pEdit || !pList) return;

    CDuiString sKey = pEdit->GetText();
    sKey.MakeLower();

    int nShown = 0;
    int nTotal = static_cast<int>(pList->GetCount());
    for (int i = 0; i < nTotal; ++i)
    {
        CControlUI* pRow = pList->GetItemAt(i);
        if (!pRow) continue;

        // 行内第一个 CTextUI（任务名）即为匹配目标（[R127-A] 判定提取为共享 helper）
        bool bMatch = sKey.IsEmpty() || UiHelpers_RowTextHitsKey(pRow, sKey);

        pRow->SetVisible(bMatch);
        if (bMatch) ++nShown;
    }

    CDuiString sLog;
    sLog.Format(L"[task] filter '%s' -> %d / %d visible",
                pEdit->GetText().GetData(), nShown, nTotal);
    m_ctx.HostLog(sLog.GetData());
}

// ============================================================
// P8：第二级事件路由（原 CFrameWnd::Notify 计划任务分支逐字迁入。
// 注意：task_filter 在原壳位于 click 块，保持不变）
// ============================================================

void CPageTasks::OnEvent(TNotifyUI& msg)
{
    // [FIX-T3-2] 过滤框死链修复：task_filter 是 CEditUI，只发 textchanged/
    // return 事件（UIEdit.cpp:185,251），原实现仅处理 click 且对其它事件
    // 一律 return —— 过滤输入完全失效（ApplyFilter 成死代码）。
    if (msg.sType == DUI_MSGTYPE_TEXTCHANGED || msg.sType == DUI_MSGTYPE_RETURN)
    {
        if (msg.pSender && msg.pSender->GetName() == L"task_filter")
        {
            ApplyFilter();
            return;
        }
        return;   // 其它控件的非 click 事件不消费（保持原行为）
    }

    if (msg.sType != L"click") return;

    CDuiString sName = msg.pSender ? msg.pSender->GetName() : CDuiString(L"");

    // [R78] 任务行点击：taskrow_%d 命中后设置选中索引，供按钮操作使用
    if (sName.Find(L"taskrow_") == 0)
    {
        // [修复 P1-TASK] "taskrow_" 长度为 8，旧代码 +9 跳过首数字，
        // 左键选中行几乎永远错位（与右键路径 psz+8 对齐）。
        int nIdx = _ttoi(sName.GetData() + 8);
        if (nIdx >= 0 && static_cast<size_t>(nIdx) < m_vTasks.size())
        {
            m_nSelected = nIdx;
            CDuiString sSelLog;
            sSelLog.Format(StringMgr.Get("task.s_selected"), m_vTasks[nIdx].sName.GetData());
            m_ctx.HostLog(sSelLog.GetData());
        }
        return;
    }

    // [R63] 计划任务按钮处理
    // [R5] sttab_2：启动页 startup_pages 切到"计划任务"子 Tab（该控件
    // 物理寄宿于启动页，由 CPageStartup::OnEvent 经页宿主转发到本页）。
    if (sName == L"sttab_2") { Build(); return; }

    else if (sName == L"btn_task_refresh") { Build(); return; }

    else if (sName == L"btn_task_enable") { OnToggle(true); return; }

    else if (sName == L"btn_task_disable") { OnToggle(false); return; }

    else if (sName == L"btn_task_run") { OnRun(); return; }

    else if (sName == L"btn_task_delete") { OnDelete(); return; }

    // [R63] 计划任务筛选（原壳在 click 块，逐字保留）
    else if (sName == L"task_filter") { ApplyFilter(); return; }

    // [R63] 计划任务 Tab 切换
    else if (sName == L"tasktab_0") { OnTabChanged(0); return; }

    else if (sName == L"tasktab_1") { OnTabChanged(1); return; }

    else if (sName == L"tasktab_2") { OnTabChanged(2); return; }
}
// ============================================================
// [R-B1] Ctrl+C / Ctrl+X 复制首个任务名
// ============================================================

void CPageTasks::OnGlobalCopy([[maybe_unused]] bool bCut)
{
    int k = CurrentSelectedIndex();
    if (k < 0) k = 0;
    if (k < 0 || static_cast<size_t>(k) >= m_vTasks.size())
    {
        m_ctx.HostToast(StringMgr.Get("tasks.s_nocopy"),
                        StringMgr.Get("tasks.s_nocopy_body"), 1);
        return;
    }
    OnCopyTaskName(k);
}

// ============================================================
// [R-B1] Ctrl+S 导出当前任务 XML
// ============================================================

void CPageTasks::OnGlobalSave()
{
    int k = CurrentSelectedIndex();
    if (k < 0)
    {
        m_ctx.HostToast(StringMgr.Get("tasks.s_nosave"),
                        StringMgr.Get("tasks.s_nosave_body"), 1);
        return;
    }
    OnExportTaskXml(k);
}

// ============================================================
// [R5] 后台枚举消息统一入口（原壳 OnPgTaskData）
// ============================================================

bool CPageTasks::OnAsyncMessage(UINT uMsg, [[maybe_unused]] WPARAM wParam, LPARAM lParam)
{
    if (uMsg == WM_TASK_DATA)
    {
        // kMsgOwnPage：TaskQueryMsg* 由 OnTaskData 内 delete。
        OnTaskData((TaskQueryMsg*)lParam);
        return true;
    }
    return false;
}

#include <memory>   // [C++17 R3] make_unique（置于文件尾，避免影响 verify 行号）
// [R232] Re-colour the task rows after a light<->dark switch.
//
// PageBase.h contract: colours only -- no Build()/Add/Remove, or the list, the
// selection and the scroll position are lost.
//
// NOTE: page_tasks.xml is only a relocation notice ("moved to Startup > Scheduled
// Tasks"); the real rows live in inc_task_panel.xml, embedded in the Startup page.
// The list is still reachable by name from the same paint manager, so this works
// regardless of which page hosts it.
//
// Row layout (5 cells, no names): 0 = name (Ink, bold), 1 = next run (InkSecondary),
// 2 = mode (InkSecondary), 3 = status (Success / InkMuted by readiness), 4 = user.
void CPageTasks::RefreshThemeColors()
{
    CVerticalLayoutUI* pList = static_cast<CVerticalLayoutUI*>(
        m_ctx.HostFindControl(L"task_list"));
    if (!pList) return;

    const DWORD dwInk = tokens::Token(tokens::EToken::Ink);
    const DWORD dwSub = tokens::Token(tokens::EToken::InkSecondary);

    for (int i = 0; i < pList->GetCount(); ++i)
    {
        CControlUI* pRowCtl = pList->GetItemAt(i);
        if (!pRowCtl) continue;
        if (!dynamic_cast<CClickRowUI*>(pRowCtl)) continue;   // empty hint / spinner

        IContainerUI* pRow = static_cast<IContainerUI*>(
            pRowCtl->GetInterface(DUI_CTR_CONTAINER));
        if (!pRow) continue;
        for (int c = 0; c < pRow->GetCount(); ++c)
        {
            CTextUI* pText = dynamic_cast<CTextUI*>(pRow->GetItemAt(c));
            if (!pText) continue;
            if (c == 0)      pText->SetTextColor(dwInk);
            else if (c == 3)
            {
                // Status cell: keep the semantic split. "Ready" is the only state that
                // uses the success colour; everything else stays muted. Recover it from
                // the text rather than from the old colour so a theme switch cannot
                // turn a ready row muted (or vice versa).
                // Same predicate the builder uses (Page_Tasks.cpp:151): the status
                // string from the Task Scheduler carries "Ready"/"Queued". There is no
                // language key for it -- guessing one would have been a silent no-op.
                const CDuiString sSt = pText->GetText();
                const bool bReady = (sSt.Find(L"Ready") >= 0 || sSt.Find(L"Queued") >= 0);
                pText->SetTextColor(bReady ? tokens::Token(tokens::EToken::Success)
                                           : tokens::Token(tokens::EToken::InkMuted));
            }
            else             pText->SetTextColor(dwSub);
        }
    }

}
