// ============================================================
// Page_UnattendIbs_Report.cpp —— 应答文件页「sysprep 执行 / IBS 报告」子系统。
//
// [R71] 从 Page_Unattend.cpp 拆分出的两段编排实现：
//   - RunSysprep     : 权限校验 → 前置确认 → 建 XML → 规范化 → 写盘 → 复制并拉起 sysprep
//   - BuildIBSReport : 枚举设备 → 组装 IBS 报告
//
// [R99-RECOVER] 本文件于 R99 从 _work/_bak_r98/Page_Unattend.cpp 的
//   570..716 行逐字提取重建（原拆分时该段被误写空）。函数体一字未改，
//   仅补齐被截断段所依赖的 include 链。
// ============================================================

#include "stdafx.h"
#include "LogService.h"   // 统一日志目录
#include "SafeEnv.h"                 // winenv::SafeGetEnv（替代弃用 _wgetenv）
#include <algorithm>
#include <memory>

#include "Page_Unattend.h"
#include "Page_UnattendInternal.h"   // FileGuard / HandleGuard（跨 TU 共享）
#include "StringManager.h"
#include "FrameWnd_Util.h"           // FrameWnd_Util_EnsureDirectoryDeep
#include "ExternalToolService.h"     // IsElevated
#include "SystemInfoService.h"       // MakeTempWorkDir
#include "Registry.h"                // RegGetString
#include <setupapi.h>                // SetupDiGetClassDevs
#include <vector>

#pragma comment(lib, "setupapi.lib")

using namespace DuiLib;
using i18n::CStringManager;

void CPageUnattend::RunSysprep()
{
    if (!RequireUnattendAdmin(m_ctx, StringMgr.Get("misc.sdb6ef517"))) return;
    // 强确认：明确告知后果
    if (!m_ctx.HostConfirmYesNo(StringMgr.Get("misc.s7ec72977"),
                                StringMgr.Get("misc.sysprep_confirm")))
        return;
    // 定位 sysprep.exe（标准路径 + PATH 兜底）
    wchar_t szSys[MAX_PATH] = { 0 };
    ::GetSystemDirectory(szSys, MAX_PATH);
    CDuiString sExe;
    sExe.Format(L"%s\\Sysprep\\sysprep.exe", szSys);
    if (::GetFileAttributes(sExe.GetData()) == INVALID_FILE_ATTRIBUTES)
    {
        sExe = L"sysprep.exe";
    }
    CDuiString sArgs = L"/generalize /oobe /shutdown /quiet";
    std::wstring sCmd(2048, L'\0');
    wfmt::FormatInto(&sCmd[0], sCmd.size(), L"\"%s\" %s", sExe.GetData(), sArgs.GetData());
    sCmd.resize(wcslen(sCmd.c_str()));
    STARTUPINFO si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    BOOL bOk = ::CreateProcessW(nullptr, &sCmd[0], nullptr, nullptr, FALSE,
                                CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &si, &pi);
    if (bOk)
    {
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
        m_ctx.HostLog(StringMgr.Get("misc.sc43f80fd"));
        // [R-B60] 接入 InjectIbsStage 四注入点（此前 InjectIbsStage 是死代码）：
        //   sysprep /generalize /oobe /shutdown 启动前，向
        //   %SystemRoot%\Panther\unattend.xml 注入 PreSysPrep（windowsPE）与
        //   PreOobe（oobeSystem）命令节点——清单维度 10 缺失 1 真实缺口闭合。
        InjectIbsStage(0);   // PreSysPrep -> windowsPE
        InjectIbsStage(2);   // PreOobe    -> oobeSystem
    }
    else
    {
        CDuiString sErr;
        sErr.Format(StringMgr.Get("misc.sba2797e3"), ::GetLastError());
        m_ctx.HostLog(sErr.GetData(), static_cast<int>(ELogLevel::LOG_ERROR));
        m_ctx.HostNotify(StringMgr.Get("misc.sc11f54c4"), sErr.GetData(), 2);
    }
}

// [R71] IBSProvider：读取 PnP 设备列表写入 sysprep_ibs.xml
// [R194] PnP 设备条目（原为 BuildIBSReport 内的局部 struct，抽函数后提升到文件级）。
// 字段顺序与含义不变：Class（设备类）/ Name（描述名）/ Inf（INF 名，可能为空）。
// [R175] UcDevInfo 定义已上移到 Page_Unattend.h（成员函数签名需要它可见）

// [R194] 从 BuildIBSReport（79 行 / CC9）抽出：把设备列表组装成 sysprep_ibs.xml
// 并写盘，返回落盘路径。
// 三重语义逐字保留：
//   · 目录写死 %LOCALAPPDATA%\DismTool_Clone\logs，写前 EnsureDirectoryDeep；
//   · 转换传 -1（含结尾 NUL），缓冲 nLen+3，**写盘长度取 buf.size()** ——
//     本处与 Optim 导出的 `3+(nLen-1)` 不同，历史行为如此，不要"统一"；
//   · 只有在 nLen>0 且建文件成功时才真正写盘，其余情况静默跳过（不报错）。
static CDuiString UcWriteIbsXml(const std::vector<UcDevInfo>& vDevs)
{
    // [M-路径统一] 报告产物并入统一日志目录（原先散落在 DismTool_Clone\logs）
    CDuiString sPath = LogService::UnifiedLogPath(L"sysprep_ibs.xml").c_str();
    CDuiString sXml;
    sXml += L"<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n";
    sXml += L"<IBSProvider>\r\n";
    sXml += L"  <PnPDevices>\r\n";
    for (size_t i = 0; i < vDevs.size(); ++i)
    {
        CDuiString sLine;
        sLine.Format(L"    <Device class=\"%s\" name=\"%s\" inf=\"%s\"/>\r\n",
                     vDevs[i].sClass.GetData(), vDevs[i].sName.GetData(),
                     vDevs[i].sInf.GetData());
        sXml += sLine;
    }
    sXml += L"  </PnPDevices>\r\n";
    sXml += L"</IBSProvider>\r\n";
    // UTF-8 BOM 写入
    const int nLen = ::WideCharToMultiByte(CP_UTF8, 0, sXml.GetData(), -1, nullptr, 0, nullptr, nullptr);
    if (nLen > 0)
    {
        // [R162] 缓冲改 std::string：BYTE* -> char* 的别名转换从类型上消失
        std::string buf(static_cast<size_t>(nLen) + 3, '\0');
        buf[0] = static_cast<char>(0xEF);
        buf[1] = static_cast<char>(0xBB);
        buf[2] = static_cast<char>(0xBF);
        ::WideCharToMultiByte(CP_UTF8, 0, sXml.GetData(), -1,
                              &buf[3], nLen, nullptr, nullptr);
        FileGuard h = ::CreateFile(sPath.GetData(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE)
        {
            DWORD dwW = 0;
            // [R128-B] IBS 报告写出不全则报告不可信，留痕
    if (!::WriteFile(h, buf.data(), static_cast<DWORD>(buf.size()), &dwW, nullptr)
        || dwW != static_cast<DWORD>(buf.size()))
        ::OutputDebugStringW(L"[R128][ibs] 报告写入不完整\r\n");
        }
    }
    return sPath;
}


// [R175] 从 BuildIBSReport 抽出：读取单个设备的信息并入表。
//   设备名优先 DEVICEDESC（官方描述），取不到退 FRIENDLYNAME（用户看到的名字）——
//   两者都取不到就跳过该设备：没有名字的条目在报告里无法辨认，列出来是噪声。
//   INF 路径通过 SPDRP_DRIVER 拿服务键名再查 Class 键的 InfPath；
//   查不到就留空（不是所有设备都有 INF，内置驱动通常没有）。
void CPageUnattend::UcAppendDevInfo(void* hDevRaw, void* pDidRaw,
                                    std::vector<UcDevInfo>& vDevs)
{
    HDEVINFO hDev = static_cast<HDEVINFO>(hDevRaw);
    SP_DEVINFO_DATA& did = *static_cast<SP_DEVINFO_DATA*>(pDidRaw);
    wchar_t szName[300] = { 0 };
    if (!::SetupDiGetDeviceRegistryProperty(hDev, &did, SPDRP_DEVICEDESC,
            nullptr, (unsigned char*)szName, sizeof(szName), nullptr)
        && !::SetupDiGetDeviceRegistryProperty(hDev, &did, SPDRP_FRIENDLYNAME,
            nullptr, (unsigned char*)szName, sizeof(szName), nullptr))
        return;

    constexpr int kDevClassWchars = 128;   // 128 缓冲尺寸（语义命名，数值不变）
    wchar_t szClass[kDevClassWchars] = { 0 };
    ::SetupDiGetDeviceRegistryProperty(hDev, &did, SPDRP_CLASS,
        nullptr, (unsigned char*)szClass, sizeof(szClass), nullptr);

    wchar_t szDrvKey[300] = { 0 };
    CDuiString sInf;
    if (::SetupDiGetDeviceRegistryProperty(hDev, &did, SPDRP_DRIVER,
            nullptr, (unsigned char*)szDrvKey, sizeof(szDrvKey), nullptr))
    {
        // [C4129-FIX] 原写法 L"SYSTEM\CurrentControlSet\Control\Class\\" 中 \C、\C、\C
        // 均为非法转义序列（C4129），编译器会静默丢弃反斜杠，实际字符串变成
        // "SYSTEMCurrentControlSetControlClass\"，导致注册表路径错误、InfPath 查询必然失败。
        // 必须写成 \\ 才能得到真正的反斜杠。
        CDuiString sClassPath = L"SYSTEM\\CurrentControlSet\\Control\\Class\\";
        sClassPath += szDrvKey;
        sInf = RegGetString(HKEY_LOCAL_MACHINE, sClassPath.GetData(), L"InfPath");
    }

    UcDevInfo d;
    d.sClass = szClass;
    d.sName = szName;
    d.sInf = sInf;
    vDevs.push_back(d);
}

void CPageUnattend::BuildIBSReport()
{
    std::vector<UcDevInfo> vDevs;
    HDEVINFO hDev = ::SetupDiGetClassDevs(nullptr, nullptr, nullptr,
                                          DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (hDev != INVALID_HANDLE_VALUE)
    {
        SP_DEVINFO_DATA did;
        did.cbSize = sizeof(did);
        // [R175] 单设备信息读取抽为 UcReadOneDevInfo，嵌套 4→2
        for (DWORD i = 0; ::SetupDiEnumDeviceInfo(hDev, i, &did); ++i)
            UcAppendDevInfo(hDev, &did, vDevs);

        ::SetupDiDestroyDeviceInfoList(hDev);
    }
    // [R194] XML 组装 + UTF-8 BOM 写盘抽为 UcWriteIbsXml（原内联 37 行）
    const CDuiString sPath = UcWriteIbsXml(vDevs);
    CDuiString sLog;
    sLog.Format(StringMgr.Get("misc.s721dd997"), static_cast<int>(vDevs.size()), L"N/A");
    m_ctx.HostLog(sLog.GetData());
}

// [R71] 应用预应答配置：导出 XML + 调用 sysprep
// [R150] ApplyUnattend 拆为四个成员（原 175 行 / CC19）。
// 主函数只保留「权限/计数/确认 → 建 XML → 规范化 → 写盘 → 复制并拉起 sysprep」
// 的编排顺序；DOM 规范化直接复用 Export 同样的 UcNormalizeXmlViaDom。
// 各步骤逐字搬迁，判定顺序与原实现一致。
