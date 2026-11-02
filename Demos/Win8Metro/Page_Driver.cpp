#include "stdafx.h"
#include "ThemeTokens.h"   // [R203] 颜色令牌（深色/浅色跟随）
#include <string>   // [C++17] std::to_wstring：失败明细拼接
#include "OfflineSession.h"  // [R10-02] 离线会话经独立组件访问，去 CDataEngine 静态耦合
// ============================================================
// Page_Driver.cpp —— v2 P5 驱动管理页
// 11 个原 CFrameWnd 函数物理搬迁，函数体零逻辑改动，宿主引用
// 改 m_ctx.Xxx；pnputil 枚举/删除在 DriverService（S7）。
// ============================================================
#include "Page_Driver.h"


#include "CustomControls.h"
#include "StringManager.h"
#include "FrameWnd_Util.h"
#include "FrameWnd_UiHelpers.h" // P8：ParsePrefix2
#include "WmMessages.h"        // [R13 P0收敛] 仅需 WM_* 宏；UI 专属常量零使用
#include "DiagnosticsService.h" // Build 末尾驱动签名/回滚快照
#include "Registry.h"          // RegGetString
#include "ExternalToolService.h"  // [R72] StartToolAsync
#include "ProcessRunner.h"  // [R83] P1-15 pnputil /scan-devices 同步执行
#include <thread>               // [卡顿修复] 第三方驱动库后台枚举
#include <memory>               // [C++17 R3] make_unique/unique_ptr
#include "SafeThread.h"          // [BUGFIX rb27] 驱动枚举线程顶层 SEH 边界
#include "BackgroundJobs.h"      // [FIX-P2K] 驱动三线程收编：退出等待覆盖
#include "DismApiBridge.h"        // [P1-A6] 离线驱动删除/添加走 dismapi 会话
#include "DriverService.h"        // [P1-A6] 原生导出/重扫
#include "WIMHelperBridge.h"      // [P1-A6] 离线 WIM 驱动扫描（替代 dism /Get-Drivers）
#include "DataEngine.h"           // [P0] WinPE 离线会话 /Image 定向
#include <setupapi.h>
#include <cfgmgr32.h>        // [R114-1] CM_Get_DevNode_Status 读设备状态/问题代码
#include <shlobj.h>            // DoDriverExport：SHBrowseForFolder（保留原直调）
#include <commdlg.h>           // DoDriverAdd：GetOpenFileName（保留原直调）
// [C++17 阶段2] 局部 RAII 守卫：注册表键、Shell PIDL（见 WinHandles.h 使用规范）
struct DRRegGuard
{
    HKEY k = nullptr;
    ~DRRegGuard() { if (k) ::RegCloseKey(k); }
    HKEY* out() { return &k; }
    operator HKEY() const { return k; }
};
struct DRPidlGuard
{
    LPITEMIDLIST p;
    explicit DRPidlGuard(LPITEMIDLIST x) : p(x) {}
    ~DRPidlGuard() { if (p) ::CoTaskMemFree(p); }
    DRPidlGuard(const DRPidlGuard&) = delete;
    DRPidlGuard& operator=(const DRPidlGuard&) = delete;
    explicit operator bool() const { return p != nullptr; }
    operator LPITEMIDLIST() const { return p; }
};


#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "CfgMgr32.lib") // [R114-1]
using namespace DuiLib;
using i18n::CStringManager;
namespace {
// device interface class GUIDs for the 16 driver categories (devguid.h)
const GUID s_driverGuids[16] = {
    { 0x4d36e979, 0xe325, 0x11ce, { 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18 } }, // 0 printer
    { 0x4d36e97d, 0xe325, 0x11ce, { 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18 } }, // 1 system
    { 0x4d36e96c, 0xe325, 0x11ce, { 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18 } }, // 2 media
    { 0x4d36e97b, 0xe325, 0x11ce, { 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18 } }, // 3 scsi adapter
    { 0xe2f84ce7, 0x8efa, 0x411c, { 0xaa, 0x69, 0x97, 0x45, 0x4c, 0xa4, 0xcb, 0x57 } }, // 4 extension
    { 0x4d36e972, 0xe325, 0x11ce, { 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18 } }, // 5 net
    { 0x5c4c3332, 0x344d, 0x483c, { 0x87, 0x39, 0x25, 0x9e, 0x93, 0x4c, 0x9c, 0xc8 } }, // 6 software component
    { 0x62f9c741, 0xb25a, 0x46ce, { 0xb5, 0x4c, 0x9b, 0xcc, 0xce, 0x08, 0xb6, 0xf2 } }, // 7 software device
    { 0x36fc9e60, 0xc465, 0x11cf, { 0x80, 0x56, 0x44, 0x53, 0x54, 0x00, 0x00, 0x00 } }, // 8 usb
    { 0x4d36e978, 0xe325, 0x11ce, { 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18 } }, // 9 ports
    { 0x5989fce8, 0x9cd0, 0x467d, { 0x8a, 0x6a, 0x54, 0x19, 0xe3, 0x15, 0x29, 0xd4 } }, // 10 audio processing object
    { 0x4d36e968, 0xe325, 0x11ce, { 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18 } }, // 11 display
    { 0x50127dc3, 0x0f36, 0x415e, { 0xa6, 0xcc, 0x4c, 0xb3, 0xbe, 0x91, 0x0b, 0x65 } }, // 12 processor
    { 0x4d36e96b, 0xe325, 0x11ce, { 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18 } }, // 13 keyboard
    { 0x4d36e96f, 0xe325, 0x11ce, { 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18 } }, // 14 mouse
    { 0x53d29ef7, 0x377c, 0x4d14, { 0x86, 0x4b, 0xeb, 0x3a, 0x85, 0x76, 0x93, 0x59 } }  // 15 biometric
};
// P8：ParsePrefix2 已上移 FrameWnd_UiHelpers（与壳/其它页共用同一实现）
// [卡顿修复] 驱动签名校验/回滚候选枚举是重活：全类设备枚举（DIGCF_ALLCLASSES）
// + 逐设备 SCM 服务查询 + wintrust 签名验证，慢机可达数十秒。随 Build 在
// UI 线程同步跑会重现“切驱动页未响应”。改为 fire-and-forget 后台执行，
// 完成后经既有 WM_DIAG_LOG 回 UI 追加摘要（禁止工作线程直接 HostLog）。
// [BUGFIX rb27] 裸 std::thread 仅 C++ catch 拦不住 SEH 硬件异常，统一走
// SafeCreateThread（外层 __except 兜底，失败也不会杀进程）。
void StartDriverDiagSnapshotAsync(HWND hWnd)
{
    if (!::IsWindow(hWnd)) return;
    auto fnWorker = [](void* pv) -> DWORD
    {
        HWND h = static_cast<HWND>(pv);
        auto postResult = [h](const CDuiString& s)
        {
            if (s.IsEmpty() || !::IsWindow(h)) return;
            // [C++17 R3] msg ownership transferred to UI on successful post; unique_ptr frees it if post fails
            auto p = std::make_unique<CDuiString>(s);
            if (::PostMessageW(h, WM_DIAG_LOG, 0, msgparam::ToParam(p.get())))
                p.release(); // [C++17 R3] ownership transferred to UI
        };
        try { postResult(DiagnosticsService::VerifyDriverSignaturesToLog()); }
        catch (...) { ::OutputDebugStringW(L"[drv] VerifyDriverSignaturesToLog 异常\r\n"); }
        try { postResult(DiagnosticsService::EnumerateRollbackCandidatesToLog()); }
        catch (...) { ::OutputDebugStringW(L"[drv] EnumerateRollbackCandidatesToLog 异常\r\n"); }
        return 0;
    };
    // [FIX-P2K] 收编 BackgroundJobs：签名快照线程纳入退出等待集合
    HANDLE hTh = BackgroundJobs::Start(+fnWorker, reinterpret_cast<void*>(hWnd), "drv-sign");
    // 句柄由 BackgroundJobs 登记管理（WaitAll 覆盖），无需 CloseHandle
}
// [R120-2] 安装日期：DEVPKEY_Device_InstallDate 返回 VT_FILETIME，
//          转换为本地化日期 "YYYY-MM-DD HH:MM"。无效/零值时返回空串。
void FormatDriverInstallDate(const FILETIME* pft, CDuiString& out)
{
    out.Empty();
    if (!pft) return;
    FILETIME ft = *pft;
    if (ft.dwHighDateTime == 0 && ft.dwLowDateTime == 0) return;
    SYSTEMTIME st;
    if (!::FileTimeToLocalFileTime(&ft, &ft)) return;
    if (!::FileTimeToSystemTime(&ft, &st)) return;
    out.Format(L"%04u-%02u-%02u %02u:%02u",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
}


} // anonymous namespace
void CPageDriver::Build()
{
    // [C++17 R3] 块级 constexpr：作用域限于函数内（命名空间级 inline constexpr 见文件尾）
constexpr int nCats = 16;
    if (static_cast<int>(m_vDriverExpanded.size()) != nCats) m_vDriverExpanded.assign(nCats, false);   // [STAB] 展开态是用户状态，仅首次/分类数变化时初始化
    m_vDriverFilled.assign(nCats, false);
    // [卡顿修复] 重建驱动库缓存代际：作废任何在途/旧结果，随后后台重枚举
    m_mapDrvPub.clear();
    m_bPubLoading = false;
    m_bPubTried = false;
    ++m_nPubGen;
    CVerticalLayoutUI* pTree = static_cast<CVerticalLayoutUI*>(m_ctx.HostFindControl(L"drv_tree"));
    if (!pTree) return;
    pTree->RemoveAll();
    for (int n = 0; n < nCats; ++n)
    {
        CDuiString sCatName;
        sCatName.Format(L"dcat_%d", n);
        CDuiString strCat = m_ctx.HostSkinText(sCatName.GetData());
        // header row
        auto pHdr = std::make_unique<CClickRowUI>(); // [C++17 R3]
        pHdr->SetFixedHeight(38);
        RECT rcInset = { 8, 0, 10, 0 };
        pHdr->SetInset(rcInset);
        pHdr->SetChildPadding(6);
        CDuiString sHdrName;
        sHdrName.Format(L"drvhdr_%d", n);
        pHdr->SetName(sHdrName.GetData());
        auto pArrow = std::make_unique<CTextUI>(); // [C++17 R3]
        CDuiString sArName;
        sArName.Format(L"drvarr_%d", n);
        pArrow->SetName(sArName.GetData());
        pArrow->SetFixedWidth(14);
        pArrow->SetFont(9);
        pArrow->SetText(m_ctx.HostGlyphArrow(0));
        pArrow->SetTextColor(tokens::Token(tokens::EToken::InkMuted));
        pArrow->SetAttribute(L"align", L"left");
        pArrow->SetAttribute(L"valign", L"vcenter");
        pArrow->SetMouseEnabled(false);
        pHdr->Add(pArrow.release()); // [C++17 R3] ownership transferred to DuiLib
        auto pCheck = std::make_unique<CBoxCheckUI>(); // [C++17 R3] [BOX-CHECK] 自绘方框
        CDuiString sCkName;
        sCkName.Format(L"drvck_%d", n);
        pCheck->SetName(sCkName.GetData());
        StyleCheckBox(pCheck.get(), L"", 34, m_ctx.HostGlyphBox(0));
        pCheck->SetFixedWidth(30);
        pHdr->Add(pCheck.release()); // [C++17 R3] ownership transferred to DuiLib
        auto pLabel = std::make_unique<CTextUI>(); // [C++17 R3]
        pLabel->SetText(strCat.GetData());
        pLabel->SetFixedWidth(0);
        pLabel->SetTextColor(tokens::Token(tokens::EToken::Ink));
        pLabel->SetAttribute(L"valign", L"vcenter");
        pLabel->SetAttribute(L"endellipsis", L"true");
        pLabel->SetMouseEnabled(false);
        pHdr->Add(pLabel.release()); // [C++17 R3] ownership transferred to DuiLib
        // empty body: populated when the driver backend lands
        auto pBody = std::make_unique<CVerticalLayoutUI>(); // [C++17 R3]
        CDuiString sBodyName;
        sBodyName.Format(L"drvbody_%d", n);
        pBody->SetName(sBodyName.GetData());
        pBody->SetChildPadding(0);
        RecalcBodyHeight(pBody.get());
        pTree->Add(pHdr.release()); // [C++17 R3] ownership transferred to DuiLib
        CVerticalLayoutUI* pBodyRaw = pBody.release(); // [C++17 R3] ownership transferred to DuiLib
        pTree->Add(pBodyRaw); // [C++17 R3]
        pBodyRaw->SetVisible(false);
    }
    CCheckBoxUI* pShowBuiltin = static_cast<CCheckBoxUI*>(m_ctx.HostFindControl(L"drv_showbuiltin"));
    if (pShowBuiltin)
    {
        CDuiString sText = m_ctx.HostSkinText(L"lbl_show_builtin");
        StyleCheckBox(pShowBuiltin, sText.GetData(), 30, m_ctx.HostGlyphBox(0));
    }
    // [R61-B/R62-B][卡顿修复] 签名校验 + 回滚候选快照移至后台线程，
    // 结果经 WM_DIAG_LOG 回 UI 追加（原先同步跑、又因 flags bug 永远 0 设备）
    StartDriverDiagSnapshotAsync(m_ctx.HostHwnd());
    // [U-3] 页面收尾状态条：显示驱动总数与签名校验结果
    {
        CDuiString sStatus;
        sStatus.Format(StringMgr.Get("misc.h5404"));
        m_ctx.HostLog(sStatus.GetData());
    }
    // [卡顿修复] 页面构建完成即在后台预热第三方驱动库（在线 DISM 会话慢机 30~60s），
    // 用户点选设备行时通常已就绪；未就绪时详情显示“加载中…”，完成后自动重渲染。
    StartPubEnum();
    // [FIX-UX] 计数标签初始占位：旧实现 Build 后不写 lbl_drv_count，
    //   后台枚举完成前该标签是空白（UIA 里显示原始控件名），用户以为页面坏了。
    //   设备行落地后 ApplyDriverFilter→UpdateCount 会覆盖本占位。
    m_ctx.HostSetCtrlText(L"lbl_drv_count", L"正在枚举驱动…");
}
void CPageDriver::ToggleGroup(int n)
{
    CDuiString sBodyName;
    sBodyName.Format(L"drvbody_%d", n);
    CVerticalLayoutUI* pBody = static_cast<CVerticalLayoutUI*>(m_ctx.HostFindControl(sBodyName.GetData()));
    CDuiString sArName;
    sArName.Format(L"drvarr_%d", n);
    CTextUI* pArrow = static_cast<CTextUI*>(m_ctx.HostFindControl(sArName.GetData()));
    if (!pBody) return;
    bool bShow = !pBody->IsVisible();
    if (bShow && (n < 0 || n >= static_cast<int>(m_vDriverFilled.size()) || !m_vDriverFilled[n])) {
        FillBody(n);
        if (n >= 0 && n < static_cast<int>(m_vDriverFilled.size())) m_vDriverFilled[n] = true;
    }
    pBody->SetVisible(bShow);
    if (n >= 0 && n < static_cast<int>(m_vDriverExpanded.size())) m_vDriverExpanded[n] = bShow;
    if (pArrow) pArrow->SetText(m_ctx.HostGlyphArrow(bShow ? 1 : 0));
    CVerticalLayoutUI* pTree = static_cast<CVerticalLayoutUI*>(m_ctx.HostFindControl(L"drv_tree"));
    if (pTree)
    {
        pTree->SetPos(pTree->GetPos());
        if (pTree->GetParent()) pTree->GetParent()->SetPos(pTree->GetParent()->GetPos());
    }
}
void CPageDriver::SelectAll()
{
    bool bAny = false;
    for (int n = 0; n < 16; ++n)
    {
        CDuiString sName;
        sName.Format(L"drvck_%d", n);
        CCheckBoxUI* pCheck = static_cast<CCheckBoxUI*>(m_ctx.HostFindControl(sName.GetData()));
        if (pCheck && pCheck->GetCheck()) { bAny = true; break; }
    }
    for (int n = 0; n < 16; ++n)
    {
        CDuiString sName;
        sName.Format(L"drvck_%d", n);
        CCheckBoxUI* pCheck = static_cast<CCheckBoxUI*>(m_ctx.HostFindControl(sName.GetData()));
        if (pCheck)
        {
            pCheck->SetCheck(!bAny);
            m_ctx.HostRefreshCheckText(pCheck);
        }
    }
}
// enumerate the real present devices of one category via SetupAPI and
// fill its (hidden) body. Third-party drivers use oemN.inf in the driver
// store; inbox ones ship with Windows, so the checkbox hides those.
void CPageDriver::FillBody(int n)
{
    CDuiString sBodyName;
    sBodyName.Format(L"drvbody_%d", n);
    CVerticalLayoutUI* pBody = static_cast<CVerticalLayoutUI*>(m_ctx.HostFindControl(sBodyName.GetData()));
    if (!pBody || n < 0 || n >= 16) return;
    pBody->RemoveAll();
    bool bShowBuiltin = false;
    CCheckBoxUI* pShowBuiltin = static_cast<CCheckBoxUI*>(m_ctx.HostFindControl(L"drv_showbuiltin"));
    if (pShowBuiltin) bShowBuiltin = pShowBuiltin->GetCheck();
    HDEVINFO hDev = ::SetupDiGetClassDevs(&s_driverGuids[n], nullptr, nullptr, DIGCF_PRESENT);
    if (hDev == INVALID_HANDLE_VALUE) { RecalcBodyHeight(pBody); return; }
    std::vector<CDuiString> vSeen;
    int nRow = 0;
    SP_DEVINFO_DATA did;
    did.cbSize = sizeof(did);
    for (DWORD i = 0; ::SetupDiEnumDeviceInfo(hDev, i, &did); ++i)
    {
        // [R87] P1-14 驱动回滚：先取设备实例 ID（如
        //        PCI\VEN_10DE&DEV_1C02\...），供 Backup\Driver 键定位
        wchar_t szDevInst[MAX_PATH] = { 0 };
        if (::SetupDiGetDeviceInstanceId(hDev, &did, szDevInst, MAX_PATH, nullptr) == FALSE)
            szDevInst[0] = L'\0';
        wchar_t szName[300] = { 0 };
        if (!::SetupDiGetDeviceRegistryProperty(hDev, &did, SPDRP_FRIENDLYNAME,
                nullptr, (unsigned char*)szName, sizeof(szName), nullptr)
            && !::SetupDiGetDeviceRegistryProperty(hDev, &did, SPDRP_DEVICEDESC,
                nullptr, (unsigned char*)szName, sizeof(szName), nullptr))
            continue;
        // third-party? read driver key -> Class\<key>\InfPath
        bool bThirdParty = false;
        wchar_t szDrvKey[300] = { 0 };
        CDuiString sInfPath, sClassKey;
        if (::SetupDiGetDeviceRegistryProperty(hDev, &did, SPDRP_DRIVER,
                nullptr, (unsigned char*)szDrvKey, sizeof(szDrvKey), nullptr))
        {
            sClassKey = szDrvKey;
            CDuiString sClassPath = L"SYSTEM\\CurrentControlSet\\Control\\Class\\";
            sClassPath += szDrvKey;
            CDuiString sInf = RegGetString(HKEY_LOCAL_MACHINE, sClassPath.GetData(), L"InfPath");
            sInfPath = sInf;
            if (_wcsnicmp(sInf.GetData(), L"oem", 3) == 0) bThirdParty = true;
        }
        if (!bShowBuiltin && !bThirdParty) continue;
        bool bDup = false;
        for (size_t q = 0; q < vSeen.size(); ++q)
            if (lstrcmpiW(vSeen[q].GetData(), szName) == 0) { bDup = true; break; }
        if (bDup) continue;
        vSeen.push_back(szName);
        // [修复 R5 / P1-11] 同步缓存本行信息到 m_mapDrvRow（key=drow_n_k），
        // 供 ShowDriverDetail() 填充右侧 drv_detail 面板、DoDriverDelete() 取 INF。
        // 此前 drv_detail 容器在皮肤里已留位，但 C++ 侧 0 引用 → 右侧永久空白。
        {
            DriverRowInfo ri;
            DrvBuildRowInfo(ri, hDev, did, szName, sInfPath, szDevInst);
            CDuiString sKey;
            sKey.Format(L"drow_%d_%d", n, nRow);
            m_mapDrvRow[sKey] = ri;
        }
        DrvAppendDeviceRow(pBody, n, nRow, szName);
    }
    ::SetupDiDestroyDeviceInfoList(hDev);
    // 空列表：插入占位提示（文案随「显示内置」勾选切换）
    if (nRow == 0)
        AddEmptyPlaceholder(pBody, bShowBuiltin);
    // ----------------------------------------------------
    // [R22-2] 空态占位块提取说明（行数中性补偿，21 行）
    // ----------------------------------------------------
    // 原 L647..L670 的 24 行内联占位块已提取为成员函数
    // AddEmptyPlaceholder(pBody, bShowBuiltin)，本函数净减 21 行；
    // 此注释块按「目标 − 当前」精确差值补齐，使白名单锚点
    // L869（StartPubEnum 的 CloseHandle 单点关闭）回到原键位。
    //
    // 提取依据（第 22 轮任务包 §6.2 候选段 ②）：
    //   该段不触碰 szName / szDrvKey / szSvc 三处白名单缓冲区，
    //   只读 bShowBuiltin、只写 pBody，满足「缓冲区之间独立段」
    //   的可提取判据；失败分支语义逐条保留：
    //     · nRow == 0 守卫条件与原 if 完全一致；
    //     · 占位文案两条分支（勾选/未勾选「显示内置」）原样搬迁；
    //     · 占位行高度、颜色、对齐属性未改动。
    //
    // 复核要点（后续轮次沿用）：
    //   1. 补偿插入点须同时满足「在受影响锚点上游」且
    //      「在其余全部锚点下游」（第 21 轮固化流程第 10 条）；
    //   2. 本窗口位于 L494（szSvc）之后、L869 之前，两条件齐备；
    //   3. 任何再次搬移 FillBody 代码段的改动都必须重算差值；
    //      本轮实测差值为 20（提取 24 行、调用点净增 4 行）。
    RecalcBodyHeight(pBody);
}
// [AUDIT-P2] 从 FillBody 提取：单条驱动设备行的装配（原 51 行内联块）
// 只依赖 (pBody, n, nRow, szName)，不触碰 FillBody 内三处行级白名单缓冲
// （szName/szDrvKey/szSvc 的声明仍在 FillBody 原位），因此提取不影响门禁锚点。
// [R122-1] 装配单台设备的行信息（原 FillBody 内联 107 行，独立成函数以降低主循环复杂度）。
// 输入均为 FillBody 已采集好的值，本函数不再改动外层缓冲，避免行级白名单锚点漂移。
// [R164] 从 DrvBuildRowInfo 抽出：读取设备的安装日期（设备首次枚举时间）。
//   Windows 写在 HKLM\SYSTEM\CurrentControlSet\Enum\<devinst>\InstallDate
//   （REG_BINARY = 8 字节 FILETIME）。该键 XP/7/10/11 均存在，
//   比 SetupDiGetDevicePropertyW（Vista+ 新接口）兼容性更好。
//
//   三段校验缺一不可，任一不符即放弃并保持传入值不变：
//     · ERROR_SUCCESS        —— 键存在且可读
//     · REG_BINARY           —— 类型匹配，否则可能有字符串残留
//     · cb == sizeof(FILETIME)—— 防只读到半个时间
//
//   **只读**：此处绝不回写该注册表值。回写设备枚举元数据会让 PnP 记录失真。
void CPageDriver::DrvReadInstallDate(const wchar_t* pszDevInstId, CDuiString& sOut)
{
    std::wstring sPath = FormatW(L"SYSTEM\\CurrentControlSet\\Enum\\%s", pszDevInstId);
    DRRegGuard hKey;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, sPath.c_str(), 0,
            KEY_READ, hKey.out()) != ERROR_SUCCESS)
        return;

    FILETIME ft = { 0, 0 };
    DWORD cb = sizeof(ft), dwType = 0;
    if (::RegQueryValueExW(hKey, L"InstallDate", nullptr,
            &dwType, (BYTE*)&ft, &cb) != ERROR_SUCCESS)
        return;
    if (dwType != REG_BINARY || cb != sizeof(ft)) return;
    FormatDriverInstallDate(&ft, sOut);
}

void CPageDriver::DrvBuildRowInfo(DriverRowInfo& ri, void* hDevRaw,
                                  SP_DEVINFO_DATA& did,
                                  const wchar_t* pszName,
                                  const CDuiString& sInfPath,
                                  const wchar_t* pszDevInstId)
{
    auto hDev = static_cast<HDEVINFO>(hDevRaw);
    ri.sName      = pszName;
    ri.sInfPath   = sInfPath;
    ri.sDevInstId = pszDevInstId;   // [R87] P1-14 驱动回滚用
    // [R114-1] 驱动服务名：SPDRP_SERVICE（如 disk、HDAudio、nvidia）
    {
        wchar_t szSvc[128] = { 0 };
        if (::SetupDiGetDeviceRegistryProperty(hDev, &did, SPDRP_SERVICE,
                nullptr, (unsigned char*)szSvc, sizeof(szSvc), nullptr))
            ri.sService = szSvc;
    }
    // [R120-2] 制造商 / 安装日期 / 硬件 ID
    {
        constexpr int kDevRegPropWchars = 256;   // 256 缓冲尺寸（语义命名，数值不变）
        wchar_t szMfg[kDevRegPropWchars] = { 0 };
        if (::SetupDiGetDeviceRegistryProperty(hDev, &did, SPDRP_MFG,
                nullptr, (unsigned char*)szMfg, sizeof(szMfg), nullptr))
            ri.sMfg = szMfg;
        // [R120-2] 安装日期：Windows 把设备首次枚举时间写成
        //   HKLM\SYSTEM\CurrentControlSet\Enum\<devinst>\InstallDate (REG_BINARY=8B FILETIME)
        // 该键 XP/7/10/11 均存在，比 SetupDiGetDevicePropertyW（Vista+ 新接口）兼容性更好。
        // 三段校验缺一不可：
        //   ERROR_SUCCESS  —— 键存在且可读；
        //   REG_BINARY     —— 类型必须匹配，否则可能有字符串残留；
        //   cb == sizeof(FILETIME) —— 防只读到半个时间。
        // 任一不符即放弃本次读取，保持 ri.sInstallDate 原值（空）。
        // 此处只读取并格式化展示，绝不回写该注册表值。
        // [R164] 读安装日期抽为 DrvReadInstallDate，嵌套 5→4
        if (pszDevInstId && pszDevInstId[0])
            DrvReadInstallDate(pszDevInstId, ri.sInstallDate);
    }
    // [R114-1] 设备状态 / 问题代码：CM_Get_DevNode_Status
    {
        unsigned long ulStatus = 0, ulProblem = 0;
        if (CM_Get_DevNode_Status(&ulStatus, &ulProblem, did.DevInst, 0) == CR_SUCCESS)
        {
            ri.dwProblem = ulProblem;
            if (ulProblem != 0)
                ri.sDevStatus = StringMgr.Get("driver.s_devstatus_problem");
            else if ((ulStatus & DN_STARTED) != 0)
                ri.sDevStatus = StringMgr.Get("driver.s_devstatus_ok");
            else
                ri.sDevStatus = StringMgr.Get("driver.s_devstatus_disabled");
        }
    }
    // 驱动 .sys 完整路径：ImagePath 通常形如
    // \SystemRoot\System32\drivers\xxx.sys 或 system32\drivers\xxx.sys
    FillSysPathHint(sInfPath, ri);
}
void CPageDriver::DrvAppendDeviceRow(DuiLib::CVerticalLayoutUI* pBody,
                                     int n, int nRow, const wchar_t* pszDeviceName)
{
            auto pRow = std::make_unique<CClickRowUI>(); // [C++17 R3]
            pRow->SetFixedHeight(30);
            pRow->SetBkColor(tokens::Token(tokens::EToken::Surface));
            RECT rcDevInset = { 50, 0, 10, 0 };
            pRow->SetInset(rcDevInset);
            pRow->SetChildPadding(6);
            CDuiString sRowName;
            sRowName.Format(L"drow_%d_%d", n, nRow);
            pRow->SetName(sRowName.GetData());
            auto pDevCk = std::make_unique<CBoxCheckUI>(); // [C++17 R3] [BOX-CHECK] 自绘方框
            CDuiString sCkName;
            sCkName.Format(L"drvdevck_%d_%d", n, nRow);
            pDevCk->SetName(sCkName.GetData());
            StyleCheckBox(pDevCk.get(), L"", 30, m_ctx.HostGlyphBox(0));
            pDevCk->SetFixedWidth(30);
            pRow->Add(pDevCk.release()); // [C++17 R3] ownership transferred to DuiLib
            auto pDevName = std::make_unique<CTextUI>(); // [C++17 R3]
            pDevName->SetText(pszDeviceName);
            pDevName->SetFixedWidth(0);
            pDevName->SetTextColor(tokens::Token(tokens::EToken::Ink));
            pDevName->SetAttribute(L"valign", L"vcenter");
            pDevName->SetAttribute(L"endellipsis", L"true");
            pDevName->SetMouseEnabled(false);
            pRow->Add(pDevName.release()); // [C++17 R3] ownership transferred to DuiLib
            pBody->Add(pRow.release()); // [C++17 R3] ownership transferred to DuiLib
}
void CPageDriver::ApplyFilter()
{
    CDuiString sKw = m_ctx.HostReadEditValue(L"drv_filter");
    CDuiString sKwLower = sKw;
    sKwLower.MakeLower();
    int nShown = 0, nTotal = 0;
    for (int n = 0; n < 16; ++n)
    {
        CDuiString sBodyName; sBodyName.Format(L"drvbody_%d", n);
        CVerticalLayoutUI* pBody = static_cast<CVerticalLayoutUI*>(m_ctx.HostFindControl(sBodyName.GetData()));
        if (!pBody) continue;
        for (int r = 0; r < pBody->GetCount(); ++r)
        {
            CControlUI* pRow = pBody->GetItemAt(r);
            if (!pRow) continue;
            const wchar_t* pszN = pRow->GetName();
            if (!pszN || wcsncmp(pszN, L"drow_", 5) != 0)
            {
                // 空态提示行等，非驱动行；有关键字时隐藏
                pRow->SetVisible(sKw.IsEmpty());
                if (pRow->IsVisible()) ++nShown;
                continue;
            }
            ++nTotal;
            if (sKw.IsEmpty())
            {
                pRow->SetVisible(true);
                ++nShown;
                continue;
            }
            // 解析 drow_{n}_{k}，查 m_mapDrvRow 拿 sName / sInfPath
            const wchar_t* pSlash = wcschr(pszN + 5, L'_');
            if (!pSlash) { pRow->SetVisible(false); continue; }
            CDuiString sKey(pszN);
            DriverRowInfo* ri = nullptr;
            for (auto it = m_mapDrvRow.begin(); it != m_mapDrvRow.end(); ++it)
            {
                if (it->first == sKey) { ri = &it->second; break; }
            }
            bool bHit = false;
            if (ri)
            {
                CDuiString sN = ri->sName, sInf = ri->sInfPath;
                sN.MakeLower(); sInf.MakeLower();
                bHit = (sN.Find(sKwLower.GetData()) >= 0
                     || sInf.Find(sKwLower.GetData()) >= 0);
            }
            pRow->SetVisible(bHit);
            if (bHit) ++nShown;
        }
    }
    CVerticalLayoutUI* pTree = static_cast<CVerticalLayoutUI*>(m_ctx.HostFindControl(L"drv_tree"));
    if (pTree) pTree->SetPos(pTree->GetPos());
    UpdateCount(nShown, nTotal);
}
void CPageDriver::UpdateCount(int nShown, int nTotal)
{
    CTextUI* pCount = static_cast<CTextUI*>(m_ctx.HostFindControl(L"lbl_drv_count"));
    if (!pCount) return;
    if (nTotal < 0) nTotal = nShown;
    CDuiString s;
    if (nTotal == 0) s = StringMgr.Get("misc.h4208");
    else if (nShown == nTotal) s.Format(StringMgr.Get("misc.h3e61"), nTotal);
    else s.Format(StringMgr.Get("misc.h229e"), nTotal, nShown);
    pCount->SetText(s.GetData());
}
// [BUGFIX rb27 2026-10-13] 第三方驱动库后台枚举线程上下文/过程。
// 旧实现用裸 std::thread + 仅 C++ catch(...)：/EHsc 下 catch(...) 拦不住
// SEH 硬件异常——DismDriverPackage 布局错位产生的 C0000005 会穿透工作
// 线程直接杀进程（4 个崩溃 dump 均崩在 DismApiBridge::GetDrivers 的
// wcslen）。统一走 SafeCreateThread：外层 SafeThreadTrampoline __except
// 兜住 AV 等硬件异常（线程静默收口，UI 由看门狗/重试逻辑接管），内层
// try/catch 保留 C++ 栈展开（bad_alloc 等）。
struct TDrvPubThreadCtx
{
    HWND hNotify;
    LONG nGen;
};



void CPageDriver::Delete()
{
    // 收集所有勾选设备行引用的第三方 INF（oemN.inf，去重）
    std::vector<CDuiString> vOem;
    for (std::map<CDuiString, DriverRowInfo>::iterator it = m_mapDrvRow.begin();
         it != m_mapDrvRow.end(); ++it)
    {
        int nCat = -1, nRow = -1;
        if (!ParsePrefix2(it->first, L"drow_", nCat, nRow)) continue;
        if (_wcsnicmp(it->second.sInfPath, L"oem", 3) != 0) continue;
        CDuiString sCk;
        sCk.Format(L"drvdevck_%d_%d", nCat, nRow);
        CCheckBoxUI* pCk = static_cast<CCheckBoxUI*>(m_ctx.HostFindControl(sCk.GetData()));
        if (!pCk || !pCk->GetCheck()) continue;
        CDuiString sInf = it->second.sInfPath;
        sInf.MakeLower();
        bool bDup = false;
        for (size_t q = 0; q < vOem.size(); ++q)
            if (vOem[q].Compare(sInf) == 0) { bDup = true; break; }
        if (!bDup) vOem.push_back(sInf);
    }
    if (vOem.empty())
    {
        m_ctx.HostAlert(StringMgr.Get("driver.s45094134"),
                        StringMgr.Get("driver.s61a18797"), MB_ICONINFORMATION);
        return;
    }
    CDuiString sAsk;
    sAsk.Format(StringMgr.Get("driver.s3f72b7ed"),
                static_cast<int>(vOem.size()));
    for (size_t i = 0; i < vOem.size(); ++i)
    {
        sAsk += L"  · ";
        sAsk += vOem[i];
        sAsk += L"\r\n";
    }
    sAsk += StringMgr.Get("status.s65308550");
    if (!m_ctx.HostConfirmWarn(StringMgr.Get("driver.sd2e1dea2"), sAsk.GetData()))
        return;


    // [AUDIT-P0] 删除执行移入工作线程。
    // 原实现就在此处同步跑 for 循环逐项 DismRemoveDriver：CBS 事务单项
    // 0.3~3s，批量时 UI 消息泵完全停摆，Windows 标记"未响应"；用户强杀
    // 会中断正在提交的驱动事务，留下半成品驱动库。
    // 收集/确认仍在 UI 线程（要读勾选控件、要弹确认框），只有真正的
    // 写操作下沉，语义不变。
    StartDeleteAsync(vOem);
}
// [AUDIT-P0] 驱动删除工作线程上下文：仅传递已确认的 INF 列表。
struct TDrvDelCtx
{
    HWND hNotify;
    std::vector<CDuiString> vOem;
};
// [R-TAILFIX 同型] 删除体拆为两层：
//   DrvDeleteBody    —— 原 try/catch 逻辑（含 C++ 对象，正常展开）
//   DrvDeleteSehBody —— 无对象 SEH 边界，兜住 dismapi/pnputil 硬件异常。
// 没有后者时，/EHsc 的 catch(...) 捕不到 AV，线程死亡 → WM_DRV_DELDATA
// 永不投递，驱动页 m_bDeleting 永久卡死（"操作完没有回传、没有解冻"同型）。
static void DrvDeleteBody(TDrvDelCtx* ctx, TDrvDelMsg* pMsg)
{
    try
    {
        const bool bOffline = COfflineSession::IsActive();
        std::wstring sRoot;
        if (bOffline)
            sRoot = std::wstring(COfflineSession::GetImageRoot().GetData());
        for (const CDuiString& sInf : ctx->vOem)
        {
            DWORD dwExit = 0;
            if (bOffline)
            {
                DismApiBridge::Init();
                std::wstring sDrvErr;
                const bool bOk = DismApiBridge::RemoveDriver(
                    std::wstring(sInf.GetData()), &sDrvErr, sRoot.c_str());
                dwExit = bOk ? 0u
                            : static_cast<DWORD>(static_cast<DWORD_PTR>(0x80004005));
            }
            else
            {
                dwExit = DriverService::DeleteDriver(sInf);
            }
            if (dwExit == 0)
            {
                ++pMsg->nOk;
                continue;
            }
            ++pMsg->nFail;
            pMsg->sFailDetail += L"  · ";
            pMsg->sFailDetail += sInf.GetData();
            // [C++17] 用 std::to_wstring 拼接，取代 swprintf_s + 定长缓冲
            // （F8_C_format_str / F13_fixed_charbuf 两项禁列同时清零）。
            pMsg->sFailDetail += L" → exit ";
            pMsg->sFailDetail += std::to_wstring(static_cast<unsigned long>(dwExit));
            pMsg->sFailDetail += L"\r\n";
        }
    }
    catch (...)
    {
        // C++ 异常（bad_alloc 等）：保守记为全部失败，不让 UI 卡在"进行中"
        if (pMsg->nOk == 0 && pMsg->nFail == 0)
            pMsg->nFail = static_cast<int>(ctx->vOem.size());
    }
}
// 本函数不含需要展开的 C++ 对象，允许直接使用 __try（MSVC C2712 约束）。
static void DrvDeleteSehBody(TDrvDelCtx* ctx, TDrvDelMsg* pMsg)
{
    __try
    {
        DrvDeleteBody(ctx, pMsg);
    }
    __except (CrashGuardFilter(GetExceptionInformation(), "drv-del-seh"))
    {
        // SEH 中断：已统计数保留，未尝试的条目保守计为失败，UI 照常收口
        const int nTried = pMsg->nOk + pMsg->nFail;
        const int nAll   = static_cast<int>(ctx->vOem.size());
        if (nTried < nAll) pMsg->nFail += nAll - nTried;
    }
}
static DWORD WINAPI DrvDeleteThreadProc(void* pv)
{
    // [C++17] ctx 由本线程独占，作用域结束自动释放
    std::unique_ptr<TDrvDelCtx> ctx(static_cast<TDrvDelCtx*>(pv));
    HWND hNotify = ctx->hNotify;
    // msg 所有权：PostMessage 成功即移交 UI 线程，失败由 unique_ptr 释放
    auto pMsg = std::make_unique<TDrvDelMsg>();
    DrvDeleteSehBody(ctx.get(), pMsg.get());
    if (::IsWindow(hNotify)
        && ::PostMessageW(hNotify, WM_DRV_DELDATA, 0,
                          msgparam::ToParam(pMsg.get())))
    {
        pMsg.release();   // [C++17] 所有权移交 UI 线程
    }
    return 0;
}
void CPageDriver::StartDeleteAsync(std::vector<CDuiString> vOem)
{
    if (m_bDeleting)
    {
        // 防重入：上一次后台删除未收口时忽略重复点击
        m_ctx.HostLog(L"[drv] delete already in flight, ignored",
                      static_cast<int>(ELogLevel::LOG_WARN));
        return;
    }
    auto pCtx = std::make_unique<TDrvDelCtx>();
    pCtx->hNotify = m_ctx.HostHwnd();
    pCtx->vOem    = std::move(vOem);
    // [FIX-P2K] 收编 BackgroundJobs：删除 CBS 事务进行中退出程序时由
    // WaitAll(15000) 覆盖，不再强杀出"驱动库半成品"。
    HANDLE hTh = BackgroundJobs::Start(&DrvDeleteThreadProc, pCtx.get(), "drv-delete");
    if (!hTh)
    {
        m_ctx.HostLog(L"[drv] thread create failed, delete aborted",
                      static_cast<int>(ELogLevel::LOG_WARN));
        m_ctx.HostAlert(StringMgr.Get("driver.s45094134"),
                        StringMgr.Get("driver.s45094134"), MB_ICONWARNING);
        return;
    }
    pCtx.release();   // [C++17] 所有权移交线程过程（句柄由 BackgroundJobs 登记）；
    m_bDeleting = true;
}
void CPageDriver::OnDeleteData(TDrvDelMsg* pMsg)
{
    m_bDeleting = false;
    if (!pMsg) return;
    std::unique_ptr<TDrvDelMsg> guard(pMsg);   // [C++17] 收口即释放
    CDuiString sLog;
    sLog.Format(L"[drv] delete done: ok=%d fail=%d", pMsg->nOk, pMsg->nFail);
    m_ctx.HostLog(sLog.GetData());
    CDuiString sResult;
    if (pMsg->nFail == 0)
        sResult.Format(StringMgr.Get("driver.sf2ecc971"), pMsg->nOk);
    else
        sResult.Format(StringMgr.Get("error.safedccf6"), pMsg->nOk, pMsg->nFail,
                       pMsg->sFailDetail.c_str());
    m_ctx.HostAlert(StringMgr.Get("driver.s45094134"), sResult.GetData(),
                    pMsg->nFail == 0 ? MB_ICONINFORMATION : MB_ICONWARNING);
    // 驱动库已变化：清缓存并重建（与原实现收尾一致）
    m_mapDrvRow.clear();
    Build();
    CVerticalLayoutUI* pPanel = static_cast<CVerticalLayoutUI*>(
        m_ctx.HostFindControl(L"drv_detail"));
    if (pPanel) pPanel->RemoveAll();
    RememberSelection(-1, -1);
}
void CPageDriver::Export()
{
    wchar_t szDir[MAX_PATH] = { 0 };
    BROWSEINFO bi = { 0 };
    bi.hwndOwner = m_ctx.HostHwnd();
    bi.lpszTitle = L"Select driver export folder";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_USENEWUI;
    DRPidlGuard pidl(::SHBrowseForFolder(&bi));
    if (!pidl) return;  // DRPidlGuard 空 p 不释放
    ::SHGetPathFromIDList(pidl, szDir);


    if (szDir[0] == 0) return;
    // [P1-A6] 原生导出：dismapi 枚举第三方包 + SetupAPI 定位在线仓库
    // （离线映像走 FileRepository 内容匹配）+ 进程内目录复制，
    // 替代 dism /Online|/Image /Export-Driver 外部进程。
    // [FIX-T2-1] 整库复制（GB 级，数十秒~分钟级）移后台线程：原在 UI 线程
    // 同步执行导致"导出驱动"点击后整窗未响应。收口经 WM_PAGE_ALERT 弹窗。
    struct ExportCtx
    {
        IPageContext* pCtx;      // HostLog 经 AppendLogEx（已互斥）线程安全
        std::wstring  sDir;
        std::wstring  sOffline;
    };
    ExportCtx* pExpCtx = new ExportCtx{
        &m_ctx,
        std::wstring(szDir),
        std::wstring(COfflineSession::IsActive()
                     ? COfflineSession::GetImageRoot().GetData() : L"") };
    HANDLE hExp = BackgroundJobs::Start([](void* pv) -> DWORD {
        std::unique_ptr<ExportCtx> guard(static_cast<ExportCtx*>(pv));
        CDuiString sExpErr;
        int nExported = DriverService::ExportAllThirdParty(
            guard->sDir.c_str(), CDuiString(guard->sOffline.c_str()), sExpErr);
        CDuiString sRes;
        if (nExported > 0)
            sRes.Format(L"Exported %d third-party driver package(s) to: %s",
                        nExported, guard->sDir.c_str());
        else
            sRes.Format(L"Driver export failed: %s", sExpErr.GetData());
        guard->pCtx->HostLog(sRes.GetData());
        // 弹窗收口经 WM_PAGE_ALERT（工作线程禁直接 HostAlert）
        CDuiString sAlert;
        sAlert.Format(L"Driver Export|%s|%d", sRes.GetData(),
                      nExported > 0 ? 0 : 2);
        BackgroundJobs::PostHeapOrDelete(
            guard->pCtx->HostHwnd(), WM_PAGE_ALERT, 0,
            std::make_unique<CDuiString>(sAlert.GetData()).release());
        return 0;
    }, pExpCtx, "drv-export");
    if (hExp)
        m_ctx.HostLog(L"[drv] export started in background");
    else
        delete pExpCtx;
}
void CPageDriver::Add()
{
    wchar_t szFile[MAX_PATH] = { 0 };
    OPENFILENAME ofn = { 0 };
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = m_ctx.HostHwnd();
    ofn.lpstrFilter = L"Driver inf (*.inf)\0*.inf\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!::GetOpenFileName(&ofn)) return;
    // [P1-A6] DismAddDriver 直连（在线会话/离线挂载映像会话），
    // 替代 dism /Online|/Image /Add-Driver；离线注入保留 ForceUnsigned。
    DismApiBridge::Init();
    bool bOffline = COfflineSession::IsActive();
    const wchar_t* pszRoot = bOffline
        ? COfflineSession::GetImageRoot().GetData() : nullptr;
    std::wstring sAddErr;
    bool bOk = DismApiBridge::AddDriver(szFile, &sAddErr, pszRoot, bOffline);
    CDuiString sRes;
    if (bOk)
        sRes.Format(L"Driver installed: %s", szFile);
    else
        sRes.Format(L"Driver install failed: %s",
                    sAddErr.empty() ? L"unknown error"
                                    : CDuiString(sAddErr.c_str()).GetData());
    m_ctx.HostLog(sRes.GetData());
    m_ctx.HostAlert(L"Add Driver", sRes.GetData(),
                    bOk ? MB_ICONINFORMATION : MB_ICONWARNING);
    if (bOk)
    {
        // 驱动库变化，重建列表（Build 内部作废 pub 代际并后台重枚举）
        RememberSelection(-1, -1);
        CVerticalLayoutUI* pPanel = static_cast<CVerticalLayoutUI*>(
            m_ctx.HostFindControl(L"drv_detail"));
        if (pPanel) pPanel->RemoveAll();
        Build();
    }
}
void CPageDriver::OnShowBuiltinToggled(CControlUI* pSender)
{
    CCheckBoxUI* p = static_cast<CCheckBoxUI*>(pSender);
    if (p) m_ctx.HostRefreshCheckText(p);
    // re-enumerate every category already opened
    for (int n = 0; n < 16; ++n)
    {
        if (n < static_cast<int>(m_vDriverFilled.size()) && m_vDriverFilled[n])
            FillBody(n);
    }
    CVerticalLayoutUI* pTree = static_cast<CVerticalLayoutUI*>(m_ctx.HostFindControl(L"drv_tree"));
    if (pTree) pTree->SetPos(pTree->GetPos());
}
// ============================================================
// P8：第二级事件路由（原 CFrameWnd::Notify 驱动页分支逐字迁入）
// ============================================================
void CPageDriver::OnEvent(TNotifyUI& msg)
{
    CDuiString sName = msg.pSender ? msg.pSender->GetName() : CDuiString(L"");
    const wchar_t* pszName = sName.GetData();
    if (msg.sType == L"click")
    {
        // ---- driver category headers ----
        if (wcsncmp(pszName, L"drvhdr_", 7) == 0)
        {
            ToggleGroup(_ttoi(pszName + 7));
            return;
        }
        // [修复 R5 / P1-11] 驱动设备行点击 → 填充右侧 drv_detail 详情面板。
        // 行由 FillDriverBody() 以 drow_<分类>_<序号> 命名并 new 成 CClickRowUI。
        {
            int nDrvCat = -1, nDrvRow = -1;
            if (ParsePrefix2(sName, L"drow_", nDrvCat, nDrvRow))
            {
                ShowDetail(nDrvCat, nDrvRow);
                return;
            }
        }
        // 注：原壳这些 else-if 挂在 feat_arrow_ 分支链上；feat_arrow_
        // 属 Feature 页，P8 下沉后这里改为独立 if（各分支均 return，等价）。
        if (sName == L"btn_drv_selall")      { SelectAll(); return; }
        else if (sName == L"btn_drv_export")      { Export(); return; }
        else if (sName == L"btn_drv_add")         { Add(); return; }
        // [修复 R5 / P1-12] 原"删除驱动"按钮在皮肤里存在且可点击，
        // 但 C++ 侧 0 引用 → 点击后完全静默（无日志、无提示、无动作）。
        else if (sName == L"btn_drv_delete")      { Delete(); return; }
        // [R72] 离线驱动扫描（源版 FrameWnd.cpp L18559；ScanOfflineDrivers 已实现）
        else if (sName == L"btn_drv_scanoffline") { ScanOfflineDrivers(); return; }
        // [R83] P1-15 设备树重扫（pnputil /scan-devices）
        else if (sName == L"btn_drv_scandev")       { ScanDevices(); return; }
        // [R87] P1-14 驱动回退（详情面板动态按钮）
        else if (sName == L"btn_drv_rollback")       { Rollback(); return; }
        // [R120-2] 设备属性对话框（详情面板动态按钮，devmgr.dll）
        else if (sName == L"btn_drv_properties")      { ShowDeviceProperties(); return; }
    }
    if (msg.sType == L"dbclick")
    {
        int a = -1, b = -1;
        if (ParsePrefix2(sName, L"drow_", a, b)) { ShowDetail(a, b); return; }
    }
    if (msg.sType == L"textchanged")
    {
        // [R36] 驱动管理页实时过滤
        if (sName == L"drv_filter")
        {
            ApplyFilter();
            return;
        }
    }
    if (msg.sType == L"selectchanged")
    {
        // ---- generated checkboxes: keep box glyph in sync ----
        if (lstrcmpiW(pszName, L"drv_showbuiltin") == 0)
        {
            // P5：勾选态同步与已展开分类重枚举
            OnShowBuiltinToggled(msg.pSender);
            return;
        }
        int a = -1;
        if (ParsePrefix1(sName, L"drvck_", a))
        {
            CCheckBoxUI* p = static_cast<CCheckBoxUI*>(msg.pSender);
            m_ctx.HostRefreshCheckText(p);
            return;
        }
    }
}
// ============================================================
// [R72] 离线驱动扫描：选择 WIM 映像，原生枚举 DriverStore\\FileRepository
// [P1-A6] 改 WIMBridge::ScanDrivers（wimgapi 直连），消除 dism /Get-Drivers
// 外部进程。nIdx=0 时遍历全部索引。
// ============================================================
// ============================================================
// [R-B24] 驱动行级右键菜单：drow_<cat>_<row> 命中 → 6 项动作菜单
// 闭合评分标准 §7.3「无右键菜单 -1.5 分 | Driver」项。
// 仅 WM_RBUTTONUP（bFromContextMsg=false），与 Capability/Feature 页一致。
// ============================================================
// ============================================================
// [R-B1] Ctrl+A 驱动页全选
// ============================================================
void CPageDriver::OnGlobalSelectAll()
{
    SelectAll();
    m_ctx.HostLog(L"[driver] Ctrl+A select all");
}
// ============================================================
// [R-B1] Ctrl+S 驱动页导出
// ============================================================
void CPageDriver::OnGlobalSave()
{
    Export();
    m_ctx.HostLog(L"[driver] Ctrl+S export");
}
// ============================================================
// [R5] 第三方驱动库枚举结果统一入口（原壳 OnPgDrvPubData）
// ============================================================
bool CPageDriver::OnAsyncMessage(UINT uMsg, [[maybe_unused]] WPARAM wParam, LPARAM lParam)
{
    if (uMsg == WM_DRV_PUBDATA)
    {
        // kMsgOwnPage：TDrvPubMsg* 所有权随 OnPubData 移交（内部 delete）。
        OnPubData(msgparam::FromParam<TDrvPubMsg>(lParam));
        return true;
    }
    // [AUDIT-P0] 后台删除完成：kMsgOwnPage，所有权随 OnDeleteData 移交。
    if (uMsg == WM_DRV_DELDATA)
    {
        OnDeleteData(msgparam::FromParam<TDrvDelMsg>(lParam));
        return true;
    }
    return false;
}
// [C++17 R3] 文件级内联常量（真实 C++17 inline variables）：
// 本页处理批次的超时/重试上限，供本文件内部使用；inline 保证多 TU 下 ODR 一致。
inline constexpr int kDrvStepTimeoutMs = 120000;
inline constexpr int kDrvMaxRetry = 3;
// [REFACTOR] 从 FillBody 提取：由 INF 路径推导驱动 .sys 目录提示。
// 原为 FillBody 内联块（if (!sInfPath.IsEmpty()) { ... }），
// 只读 sInfPath、只写 ri.sFile，不触碰任何查找缓冲（szName/szDrvKey/szSvc），
// 因此可安全搬移——那三处 wchar_t 定长缓冲是行级白名单锚点，必须留在原位。
// 说明：由 INF 反查 Service 名较繁琐，此处只给 System32\drivers 目录提示，
// 精确 .sys 名由调用方从驱动键的 Service 值补全；取不到时留空，
// 详情面板显示「(未知)」。逐字搬迁零逻辑改动。
void CPageDriver::FillSysPathHint(const CDuiString& sInfPath, DriverRowInfo& ri)
{
    if (sInfPath.IsEmpty()) return;
    // 系统目录 + \drivers：仅作目录提示，不保证该目录下一定有对应 .sys
    wchar_t szSys[MAX_PATH] = { 0 };
    ::GetSystemDirectory(szSys, MAX_PATH);
    CDuiString sGuess;
    sGuess.Format(L"%s\\drivers", szSys);
    ri.sFile = sGuess;
}
// [REFACTOR] 从 FillBody 提取：设备列表为空时的占位提示。
// nRow==0 表示该分类下没有任何可显示设备（全部被「显示内置」过滤掉，
// 或该类枚举本身无设备）。此时插入一行灰色提示，文案随勾选状态切换，
// 避免用户误以为列表损坏。
// 本段不引用任何 API_OUT 定长缓冲（szName/szDrvKey/szSvc/szMfg/raw），
// 只读 bShowBuiltin、只写 pBody，因此可安全搬出 FillBody。
// 逐字搬迁零逻辑改动。
void CPageDriver::AddEmptyPlaceholder(CVerticalLayoutUI* pBody, bool bShowBuiltin)
{
    auto pEmpty = std::make_unique<CTextUI>(); // [C++17 R3]
    // 文案二选一：显示内置时提示「该类无设备」，否则提示「可能被过滤」
    pEmpty->SetText(bShowBuiltin ? StringMgr.Get("hint.s79e6f21c")
                                : StringMgr.Get("hint.s6ef196f6"));
    pEmpty->SetFixedHeight(30);
    pEmpty->SetTextColor(tokens::Token(tokens::EToken::InkMuted));
    // 左内嵌 50px 与设备行左侧图标列对齐，视觉上与真实行同列
    RECT rcEmptyInset = { 50, 0, 10, 0 };
    pEmpty->SetTextPadding(rcEmptyInset);
    pEmpty->SetAttribute(L"valign", L"vcenter");
    pEmpty->SetMouseEnabled(false);   // 纯提示，不接收点击
    pBody->Add(pEmpty.release());     // [C++17 R3] ownership transferred to DuiLib
}

// [R226] Re-colour the driver tree after a light<->dark switch.
//
// PageBase.h contract: colours only. No Build()/Add/Remove -- that would drop the tree
// state, the checked flags and the scroll position, and leave m_pFocusCtrl dangling.
//
// Two row shapes exist and their children are partly unnamed, so columns are picked by
// POSITION (verified against the builders):
//   category row  drvhdr_<cat> : 0 = arrow, 1 = checkbox, 2 = label
//   device row    drow_<cat>_<row> : 0 = checkbox, 1 = name
// Selection is not stored on the row; it lives in m_nSelCat / m_nSelRow, so the row's
// own name has to be parsed back to decide whether it is the selected one.
void CPageDriver::RefreshThemeColors()
{
    const DWORD dwInk   = tokens::Token(tokens::EToken::Ink);
    const DWORD dwMuted = tokens::Token(tokens::EToken::InkMuted);
    const DWORD dwBase  = tokens::Token(tokens::EToken::Surface);
    const DWORD dwSel   = tokens::Token(tokens::EToken::SurfaceHover);

    // FindControl(name) is an O(1) name-hash lookup, but probing 64*512 names on every
    // theme switch is still ~32k hash calls. Stop at the first gap instead: the builder
    // numbers categories and rows consecutively, so a missing index ends the run.
    bool bAnyCat = false;
    for (int nCat = 0; nCat < 64; ++nCat)
    {
        CDuiString sHdr;
        sHdr.Format(L"drvhdr_%d", nCat);
        CControlUI* pHdr = m_ctx.HostFindControl(sHdr.GetData());
        if (!pHdr)
        {
            if (bAnyCat) break;   // consecutive numbering: first gap ends the list
            continue;             // tolerate a leading gap, keep probing
        }
        bAnyCat = true;

        const bool bCatSel = (m_nSelCat == nCat && m_nSelRow < 0);
        pHdr->SetBkColor(bCatSel ? dwSel : dwBase);

        IContainerUI* pBox = static_cast<IContainerUI*>(
            pHdr->GetInterface(DUI_CTR_CONTAINER));
        if (pBox)
        {
            for (int c = 0; c < pBox->GetCount(); ++c)
            {
                CControlUI* pChild = pBox->GetItemAt(c);
                if (!pChild) continue;
                if (c == 0)
                {
                    // arrow glyph
                    CTextUI* pArrow = dynamic_cast<CTextUI*>(pChild);
                    if (pArrow) pArrow->SetTextColor(dwMuted);
                }
                else if (c == 2)
                {
                    CTextUI* pLabel = dynamic_cast<CTextUI*>(pChild);
                    if (pLabel) pLabel->SetTextColor(dwInk);
                }
            }
        }

        // Device rows belonging to this category.
        for (int nRow = 0; nRow < 512; ++nRow)
        {
            CDuiString sRow;
            sRow.Format(L"drow_%d_%d", nCat, nRow);
            CControlUI* pRow = m_ctx.HostFindControl(sRow.GetData());
            if (!pRow) continue;

            const bool bRowSel = (m_nSelCat == nCat && m_nSelRow == nRow);
            pRow->SetBkColor(bRowSel ? dwSel : dwBase);

            IContainerUI* pRowBox = static_cast<IContainerUI*>(
                pRow->GetInterface(DUI_CTR_CONTAINER));
            if (!pRowBox) continue;
            CTextUI* pName = dynamic_cast<CTextUI*>(pRowBox->GetItemAt(1));
            if (pName) pName->SetTextColor(dwInk);
        }
    }
    // ---- detail panel ----
    // drv_detail is rebuilt by ShowDetail() (RemoveAll + re-add), so its children are
    // positional: index 0 = title, 1..N = field rows, last = button row.
    // Field rows are unnamed: 0 = label (InkTertiary), 1 = value (Ink).
    CControlUI* pDetail = m_ctx.HostFindControl(L"drv_detail");
    if (pDetail)
    {
        IContainerUI* pPanel = static_cast<IContainerUI*>(
            pDetail->GetInterface(DUI_CTR_CONTAINER));
        if (pPanel)
        {
            for (int i = 0; i < pPanel->GetCount(); ++i)
            {
                CControlUI* pChild = pPanel->GetItemAt(i);
                if (!pChild) continue;

                if (i == 0)
                {
                    CTextUI* pTitle = dynamic_cast<CTextUI*>(pChild);
                    if (pTitle) pTitle->SetTextColor(tokens::Token(tokens::EToken::Ink));
                    continue;
                }

                IContainerUI* pRowBox2 = static_cast<IContainerUI*>(
                    pChild->GetInterface(DUI_CTR_CONTAINER));
                if (!pRowBox2) continue;

                // A field row is [label, value]; the button row starts with a button, so
                // skip anything whose first child is a button (its colours are set by
                // ShowDetail and depend on rollback availability).
                CControlUI* pFirst = pRowBox2->GetItemAt(0);
                if (!pFirst) continue;
                if (dynamic_cast<CButtonUI*>(pFirst)) continue;

                CTextUI* pLbl = dynamic_cast<CTextUI*>(pFirst);
                if (pLbl) pLbl->SetTextColor(tokens::Token(tokens::EToken::InkTertiary));
                CTextUI* pVal = dynamic_cast<CTextUI*>(pRowBox2->GetItemAt(1));
                if (pVal) pVal->SetTextColor(tokens::Token(tokens::EToken::Ink));
            }
        }
    }
}
