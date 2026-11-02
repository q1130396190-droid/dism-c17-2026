#include "stdafx.h"
#include "LogService.h"   // [M-8] 统一日志目录
#include "SafeEnv.h"   // [B6] 安全环境变量读取：替代弃用 _wgetenv

// #include "OfflineSession.h" // [R24-F2] 删除假依赖：0 处使用  // [R10-02] 离线会话经独立组件访问，去 CDataEngine 静态耦合

#include "Page_Cleanup.h"

#include "StringManager.h"
#include "NotifyBox.h"           // [R144] ToolSql::HostNotify / HostConfirm

#include "FrameWnd_Util.h"          // StyleCheckBox / RecalcBodyHeight / CopyTextToClipboard

#include "ProcessRunner.h"  // RunHiddenExit / RunCapture

#include "Win32DialogService.h"    // DlgSaveFile

#include "SafeThread.h"            // 碎片诊断后台线程异常边界

#include "ExternalToolService.h"  // IsElevated
// #include "DismApiBridge.h" // [R25-F1] 删除假依赖：0 处使用  // [R-B18] dismapi.dll 直连 DismRemovePackage
// #include "AppxRuntimeBridge.h" // [R25-F1] 删除假依赖：0 处使用  // [P0-A1] Appx 重注册/坏包清理 WinRT 原生

// #include "CleanExtend.h" // [R25-F1] 删除假依赖：0 处使用  // [R80] 深度扫描/残留/空文件夹/撤销引擎
// #include "VssNativeService.h" // [R25-F1] 删除假依赖：0 处使用  // [R-B31] VSS 原生 API 删除快照（vssapi.dll COM）

// #include "CbsHostClient.h" // [R23-F5] 删除假依赖：0 处使用
// #include "NCleanerClient.h" // [R25-F1] 删除假依赖：0 处使用  // [R-拆分] NCleaner 7 导出经 NCleaner.dll 动态调用
// #include "DataEngine.h" // [R23-F5] 删除假依赖：0 处使用
// #include "SystemInfoService.h" // [R23-F5] 删除假依赖：0 处使用

#include "CleanNativePrimitives.h"  // [R-B57] CreateHardLink 通用重复文件回收原语
// #include "WofCompact.h" // [R25-F1] 删除假依赖：0 处使用           // [P0-A2] CompactOS 原生 FSCTL（替代 compact.exe）
// #include "RegCleanEngine.h" // [R23-F5] 删除假依赖：0 处使用

#include <shlobj.h>                // SHGetFolderPath / SHChangeNotify
#include <winevt.h>                // [R-B17] EvtClearLog / EvtOpenChannelEnum

#pragma comment(lib, "wevtapi.lib")

#include <string>

#include <algorithm>              // [R126-1] std::stable_sort
// [C++17 阶段2] 局部 RAII 守卫：文件句柄（见 WinHandles.h 使用规范）
struct PCRFileGuard
{
    HANDLE h = INVALID_HANDLE_VALUE;
    PCRFileGuard() = default;
    explicit PCRFileGuard(HANDLE x) : h(x) {}
    ~PCRFileGuard() { if (h && h != INVALID_HANDLE_VALUE) ::CloseHandle(h); }
    PCRFileGuard(const PCRFileGuard&) = delete;
    PCRFileGuard& operator=(const PCRFileGuard&) = delete;
    operator HANDLE() const { return h; }
    bool valid() const { return h && h != INVALID_HANDLE_VALUE; }
};

using i18n::CStringManager;   // 便捷：直接写 CStringManager 不用前缀

// ============================================================
// Page_Cleanup_Report.cpp —— [R14-02] CPageCleanup 扫描报告/历史/导出切片
//   ScdUpdateScanState/ScdCancelReport/ScdFailureReport/ScdSuccessReport/
//   ScdScanBalloon/ScdFinishUi/ExportCleanReport/OcWriteCleanHistory
// 物理搬迁护栏：函数体逐字迁移，宿主引用仍走 m_ctx；类声明在 Page_Cleanup.h。
// ============================================================

// [修复 #23] UI 大小快照中“尚未扫到该项”的哨兵值（区别于扫到的真实 0 字节）
// [R14-02] 原 Page_Cleanup.cpp 匿名命名空间常量；切片后各 TU 各持 internal-linkage 副本。
static const ULONGLONG CLEAN_SIZE_UNKNOWN = static_cast<ULONGLONG>(static_cast<LONGLONG>(-1));

void CPageCleanup::ExportCleanReport()

{
    if (!m_bCleanHasReport || m_sCleanFullDetail.IsEmpty())
    {
        ToolSql::HostNotify(m_ctx.HostHwnd(), StringMgr.Get("cleanup.sfde892ce"), StringMgr.Get("export.s845b9368"), MB_OK | MB_ICONINFORMATION);
        return;
    }
    CDuiString sPath;
    if (!Win32DialogService::DlgSaveFile(sPath,
            StringMgr.Get("misc.s1f355182"),
            StringMgr.Get("cleanup.s3f33cca6"), L"CleanupReport.txt")) return;
    // 加一行导出时间头
    CDuiString sOut = m_sCleanFullDetail;
    sOut += StringMgr.Get("export.s6031c53a");
    SYSTEMTIME st; ::GetLocalTime(&st);
    CDuiString sWhen;
    sWhen.Format(L"%04d-%02d-%02d %02d:%02d:%02d\n",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    sOut += sWhen;
    int nLen = ::WideCharToMultiByte(CP_UTF8, 0, sOut.GetData(), -1,
                                     nullptr, 0, nullptr, nullptr);
    if (nLen <= 0) return;
    // nLen 含结尾 NUL，写入时剔除（内容字节数 = nLen-1），前置 UTF-8 BOM
    // nLen 含结尾 NUL：转换缓冲必须容纳 3 + nLen 字节（此前少分 1 字节，
    // WideCharToMultiByte 写结尾 NUL 时堆越界）；写文件时只写 nLen-1 内容字节。
    // [R160] 同 Page_OptimExport.cpp：缓冲以 char 承载 UTF-8 字节，
    //   从类型上取消 BYTE* -> char* 的别名转换。
    std::string buf(static_cast<size_t>(3) + static_cast<size_t>(nLen), '\0');
    buf[0] = static_cast<char>(0xEF);
    buf[1] = static_cast<char>(0xBB);
    buf[2] = static_cast<char>(0xBF);
    ::WideCharToMultiByte(CP_UTF8, 0, sOut.GetData(), -1,
                          &buf[3], nLen, nullptr, nullptr);
    PCRFileGuard h(::CreateFile(sPath.GetData(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (h == INVALID_HANDLE_VALUE)
    {
        // S-15: 复制错误详情到剪贴板（含 GetLastError）
        CDuiString sExpErr;
        sExpErr.Format(L"Export failed: path=%s err=%d\n%s",
                       sPath.GetData(), (int)::GetLastError(),
                       StringMgr.Get("error.s656c4845"));
        FrameWnd_Util_CopyTextToClipboard(m_ctx.HostHwnd(), sExpErr.GetData());
        ToolSql::HostNotify(m_ctx.HostHwnd(), StringMgr.Get("error.s656c4845"), StringMgr.Get("error.export_fail"), MB_OK | MB_ICONERROR);
        return;
    }
    DWORD nWritten = 0;
    // [R65-AUDIT] 写入结果必须校验。
    //   原实现不看返回值与字节数，随后直接提示「报告已导出」——
    //   磁盘满 / 只读 / 杀软拦截时报告为空或截断，用户以为已保存。
    //   与 Optim 配置导出、Tools 密码恢复说明同一类型。
    const DWORD cbWant = 3 + static_cast<DWORD>(nLen - 1);
    if (!::WriteFile(h, buf.data(), cbWant, &nWritten, nullptr) || nWritten != cbWant)
    {
        CDuiString sWrErr;
        sWrErr.Format(L"Export write failed: path=%s err=%d\n%s",
                      sPath.GetData(), (int)::GetLastError(),
                      StringMgr.Get("error.s656c4845"));
        FrameWnd_Util_CopyTextToClipboard(m_ctx.HostHwnd(), sWrErr.GetData());
        ToolSql::HostNotify(m_ctx.HostHwnd(), StringMgr.Get("error.s656c4845"),
                            StringMgr.Get("error.export_fail"), MB_OK | MB_ICONERROR);
        return;
    }
    // PCRFileGuard 析构自动关闭
    CDuiString sLog;
    sLog.Format(L"[clean] report exported: %s", sPath.GetData());
    AppendLog(sLog.GetData());
    ToolSql::HostNotify(m_ctx.HostHwnd(), StringMgr.Get("cleanup.s124deb08"), StringMgr.Get("export.s446e67b3"), MB_OK | MB_ICONINFORMATION);
}

void CPageCleanup::ScdUpdateScanState(int nRuleFailed, bool& bCancelled, int& nScanRules)

{
    // C-10: 工作线程通过 lParam 报告失败计数
    if (bCancelled)
        m_bCleanScanned = false;   // 未整轮扫完：仅 i<m_nCleanScannedCnt 视为已扫
    else
    {
        m_bCleanScanned = true;
        m_nCleanScannedCnt = m_ctx.CleanEngine().GetRuleCount();
    }
    // [修复 #23] 工作线程已结束（句柄已等待回收），此处读 ullSize 无竞争。
    // 取消时只补齐“已扫到”的项，未扫项保留 UNKNOWN（显示为未扫描，
    // 不能误补成 0，否则会被零字节过滤隐藏、汇总虚减）。
    nScanRules = m_ctx.CleanEngine().GetRuleCount();
    if (static_cast<int>(m_vUiSize.size()) != nScanRules)
        m_vUiSize.assign(nScanRules, CLEAN_SIZE_UNKNOWN);
    if (!bCancelled)
    {
        for (int si = 0; si < nScanRules; ++si)
        {
            if (m_vUiSize[si] == CLEAN_SIZE_UNKNOWN)
                m_vUiSize[si] = m_ctx.CleanEngine().GetRule(si).ullSize;
        }
    }
    ApplyCleanFilter();
    RefreshGroupTotals();
}

void CPageCleanup::ScdCancelReport(int nScanRules, int nRuleFailed)

{
    CDuiString sLog;
    sLog.Format(L"[clean] scan cancelled at %d/%d rules (failed=%d)",
                m_nCleanScannedCnt, nScanRules, nRuleFailed);
    AppendLog(sLog.GetData());
    m_ctx.HostSetCtrlText(L"sess_status",
                StringMgr.Get("cleanup.s1c0e79cf"));
}

// [R147] 扫描错误码 → 本地化错误名。
// 原实现里同一张 5 分支 switch 在 ScdFailureReport 内写了两遍
// （日志段与 clean_desc 段各一份），既重复计数又容易改一处漏一处。
// 提取为具名函数后，两处共用一份映射；判定表按错误码升序。
namespace
{
[[nodiscard]] CDuiString ScanErrName(ScanRuleError nErr)
{
    switch (nErr)
    {
    case ScanRuleError::SCAN_RULE_ACCESS_DENIED:  return StringMgr.Get("error.scan_access");
    case ScanRuleError::SCAN_RULE_PATH_NOT_FOUND: return StringMgr.Get("error.scan_path");
    case ScanRuleError::SCAN_RULE_IO_ERROR:       return StringMgr.Get("error.scan_io");
    case ScanRuleError::SCAN_RULE_TIMEOUT:        return StringMgr.Get("error.scan_timeout");
    default:                                      return StringMgr.Get("error.scan_unknown");
    }
}

// 错误统计里是否有任一类非零（原为 5 个 || 串接）。
// 索引 0 = SCAN_RULE_OK，故从 1 开始；上界取 SCAN_RULE_ERROR_COUNT（= 6）。
[[nodiscard]] bool ScanErrStatsAnyNonZero(const int (&stats)[static_cast<int>(ScanRuleError::SCAN_RULE_ERROR_COUNT)])
{
    for (int i = 1; i < static_cast<int>(ScanRuleError::SCAN_RULE_ERROR_COUNT); ++i)
        if (stats[i] > 0) return true;
    return false;
}
}   // namespace

// [R196] 从 ScdFailureReport（74 行 / CC9）抽出：分类统计 + 前 5 条失败明细日志。
// 两段都受 nScanRules 上界与 nLogged<5 上限约束，逐字保留：
//   · 分类统计只在 m_nScanErrStats 任一非零时输出（避免刷一排 0）；
//   · 明细只打前 5 条，防止数百条失败把日志区冲掉。
void CPageCleanup::ScdLogFailureDetails(int nScanRules)
{
// A3: 按错误类型分类统计
if (ScanErrStatsAnyNonZero(m_nScanErrStats))
{
    CDuiString sStats;
    sStats.Format(L"[clean]   error breakdown: access=%d path=%d io=%d timeout=%d unknown=%d",
                  m_nScanErrStats[static_cast<int>(ScanRuleError::SCAN_RULE_ACCESS_DENIED)],
                  m_nScanErrStats[static_cast<int>(ScanRuleError::SCAN_RULE_PATH_NOT_FOUND)],
                  m_nScanErrStats[static_cast<int>(ScanRuleError::SCAN_RULE_IO_ERROR)],
                  m_nScanErrStats[static_cast<int>(ScanRuleError::SCAN_RULE_TIMEOUT)],
                  m_nScanErrStats[static_cast<int>(ScanRuleError::SCAN_RULE_UNKNOWN)]);
    AppendLog(sStats.GetData());
}
// A4: 打印前 5 条失败规则的详细路径与错误类别
int nLogged = 0;
for (int li = 0; li < nScanRules && nLogged < 5; ++li)
{
    CleanRule& rFail = m_ctx.CleanEngine().GetRule(li);
    if (rFail.nLastError > ScanRuleError::SCAN_RULE_OK)
    {
        const CDuiString sErrName = ScanErrName(rFail.nLastError);
        CDuiString sDetail;
        sDetail.Format(L"[clean]   rule#%d \"%s\": %s | %s",
                       li, rFail.sName.GetData(),
                       sErrName.GetData(),
                       rFail.sLastErrPath.GetData());
        AppendLog(sDetail.GetData());
        ++nLogged;
    }
}
}

// [R196] 从 ScdFailureReport 抽出：把失败详情追加到 clean_desc 的 RichEdit。
// 用户关掉错误弹窗后仍能回看。段落顺序（小标题 → 条数 → 单位 → 明细）逐字保留；
// SetWordWrap(true) 必须保留：长路径不换行会把横向滚动条拉出来。
void CPageCleanup::ScdAppendFailureToDesc(int nScanRules, int nRuleFailed)
{
// S-17: 把扫描失败详情也追加到 clean_desc RichEdit
// 用户关闭错误弹窗后仍可查看；不覆盖已有报告内容（清空 + 追加）
CRichEditUI* pDesc = static_cast<CRichEditUI*>(
            m_ctx.HostFindControl(L"clean_desc"));
if (pDesc)
{
    pDesc->SetWordWrap(true);
    CDuiString sDesc;
    sDesc += StringMgr.Get("cleanup.scan_error_section");
    sDesc += StringMgr.Get("cleanup.scan_error_count");
    CDuiString sCnt;
    sCnt.Format(L"%d", nRuleFailed);
    sDesc += sCnt;
    sDesc += StringMgr.Get("cleanup.scan_error_unit");
    sDesc += L"\n";
    int nLoggedDesc = 0;
    for (int li2 = 0; li2 < nScanRules && nLoggedDesc < 5; ++li2)
    {
        CleanRule& rF = m_ctx.CleanEngine().GetRule(li2);
        if (rF.nLastError > ScanRuleError::SCAN_RULE_OK)
        {
            const CDuiString sErrName = ScanErrName(rF.nLastError);
            CDuiString sLine;
            sLine.Format(L"#%d %s\n   %s\n   %s\n\n",
                         li2, rF.sName.GetData(),
                         sErrName.GetData(),
                         rF.sLastErrPath.GetData());
            sDesc += sLine;
            ++nLoggedDesc;
        }
    }
    pDesc->SetText(sDesc.GetData());
}
}

void CPageCleanup::ScdFailureReport(int nScanRules, int nRuleFailed)

{
    // C-10: 若扫描过程中有失败规则，把状态条切换到 warning 状态
    // 而非直接显示 ready，让用户看到"扫描完成但有问题"。
    AppendLog(L"[clean] scan finished with failures");
    CDuiString sLog2;
    sLog2.Format(L"[clean] %d rule(s) reported scan errors", nRuleFailed);
    AppendLog(sLog2.GetData());
    // [R196] 分类统计 + 前 5 条明细抽为 ScdLogFailureDetails
    ScdLogFailureDetails(nScanRules);
    m_ctx.HostSetCtrlText(L"sess_status",
                StringMgr.Get("status.scan_failed"));
    // [R196] RichEdit 追加抽为 ScdAppendFailureToDesc
    ScdAppendFailureToDesc(nScanRules, nRuleFailed);
}

void CPageCleanup::ScdSuccessReport()

{
    AppendLog(L"[clean] scan finished");
    m_ctx.HostSetCtrlText(L"sess_status", m_ctx.HostStatusReady().GetData());
}

void CPageCleanup::ScdScanBalloon(int nScanRules, int nRuleFailed)

{
    // [R18 P0] 扫描完成气泡（汇总可回收空间）
    ULONGLONG ullTotal = 0;
    for (int si = 0; si < nScanRules; ++si)
    {
        if (m_vUiSize[si] != CLEAN_SIZE_UNKNOWN)
            ullTotal += m_vUiSize[si];
    }
    CDuiString sSize;
    CCleanEngine::FormatSize(ullTotal, sSize);
    CDuiString sBalloon;
    sBalloon.Format(StringMgr.Get("cleanup.sf2551ff5"),
                    nScanRules, sSize.GetData());
    const int nIcon = (nRuleFailed > 0) ? 1 : 0;   // 1=NIIF_WARNING 0=NIIF_INFO
    const wchar_t* sTipTitle = (nRuleFailed > 0)
        ? StringMgr.Get("cleanup.scan_done_with_errors")
        : StringMgr.Get("cleanup.s1df790ce");
    m_ctx.HostNotify(sTipTitle, sBalloon.GetData(), nIcon);
}

void CPageCleanup::ScdFinishUi()

{
    m_ctx.HostProgress(0, 0);   // 隐藏进度条 + 任务栏进度
    SetWorkerUiBusy(false);
}

// [R147] 磁盘可用空间采样（GetDiskFreeSpaceExW 动态解析，避免静态链接依赖）。
// 返回清理后的可用字节数；解析失败返回 0（与原实现 pfnG 为空时的行为一致）。
namespace
{
[[nodiscard]] ULONGLONG QueryFreeBytesOnC()
{
    ULARGE_INTEGER liT = { 0 }, liF = { 0 }, liA = { 0 };
    using PfnGDSE = BOOL ( WINAPI*)(wchar_t*, PULARGE_INTEGER, PULARGE_INTEGER, PULARGE_INTEGER, void*);
    // [FIX-DS-3] GetProcAddress(nullptr,...) 恒返回 NULL：C 盘剩余空间查询恒为 0。
    PfnGDSE pfnG = (PfnGDSE)::GetProcAddress(::GetModuleHandleW(L"kernel32.dll"), "GetDiskFreeSpaceExW");
    if (!pfnG) return 0;
    pfnG(L"C:\\", &liA, &liT, &liF, nullptr);
    return static_cast<ULONGLONG>(liF.QuadPart);
}

// 单条历史记录的字段上限（防止单行过长撑爆日志查看器）。
inline constexpr int kCleanHistGroupsMaxLen = 200;
inline constexpr int kCleanHistRulesMaxLen  = 300;
inline constexpr int kCleanHistPathsMaxLen  = 200;

[[nodiscard]] CDuiString TruncateLen(CDuiString s, int nMax)
{
    return (s.GetLength() > nMax) ? s.Mid(0, nMax) : s;
}
}   // namespace

// [R60-A] 清理完成后的磁盘采样与历史落盘（从 OcWriteCleanHistory 拆出：规则名/分组/失败路径收集）。
void CPageCleanup::OcCollectHistoryStats(CDuiString& sGroups, CDuiString& sRules,
                                         CDuiString& sFailedPaths)
{
    const std::vector<int>& vSelForLog = m_vLastCleanSel;
    for (size_t si = 0; si < vSelForLog.size(); ++si)
    {
        const CleanRule& r = m_ctx.CleanEngine().GetRule(vSelForLog[si]);
        if (!sRules.IsEmpty()) sRules += L";";
        sRules += r.sName;
        // 失败路径（仅记录非 OK 且路径非空者）
        if (r.nLastError != ScanRuleError::SCAN_RULE_OK && !r.sLastErrPath.IsEmpty())
        {
            if (!sFailedPaths.IsEmpty()) sFailedPaths += L";";
            sFailedPaths += r.sLastErrPath;
        }
    }
    // 分组分布：按引擎 group 顺序统计本次命中数，命中 0 的组不记录
    const int nGrpCnt = m_ctx.CleanEngine().GetGroupCount();
    for (int gi = 0; gi < nGrpCnt; ++gi)
    {
        CDuiString sGName;
        m_ctx.CleanEngine().GetGroup(gi, sGName);
        int nHit = 0;
        for (size_t si = 0; si < vSelForLog.size(); ++si)
            if (m_ctx.CleanEngine().GetRule(vSelForLog[si]).sGroup == sGName) ++nHit;
        if (nHit == 0) continue;
        if (!sGroups.IsEmpty()) sGroups += L",";
        CDuiString sGItem;
        sGItem.Format(L"%s=%d", sGName.GetData(), nHit);
        sGroups += sGItem;
    }
    sGroups      = TruncateLen(sGroups, kCleanHistGroupsMaxLen);
    sRules       = TruncateLen(sRules, kCleanHistRulesMaxLen);
    sFailedPaths = TruncateLen(sFailedPaths, kCleanHistPathsMaxLen);
}

// 把一行 UTF-8 文本追加到指定文件；文件为新建时先写 BOM，保证跨会话可读。
// 返回 false 表示打开或写入失败（调用方目前只记日志，不再回滚任何状态）。
bool CPageCleanup::OcAppendUtf8Line(const wchar_t* pszPath, const CDuiString& sLine)
{
    PCRFileGuard hF(::CreateFile(pszPath, GENERIC_WRITE, FILE_SHARE_READ,
                                 nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (hF == INVALID_HANDLE_VALUE) return false;
    DWORD dwW = 0;
    if (::GetFileSize(hF, nullptr) == 0)   // 仅新文件写 BOM
    {
        BYTE bBOM[3] = { 0xEF, 0xBB, 0xBF };
        // [R65-AUDIT] BOM 写失败说明文件不可写，直接返回 false（原实现忽略）。
        if (!::WriteFile(hF, bBOM, 3, &dwW, nullptr) || dwW != 3) return false;
    }
    ::SetFilePointer(hF, 0, nullptr, FILE_END);
    const std::wstring ws = sLine.GetData();
    const int nBytes = ::WideCharToMultiByte(CP_UTF8, 0, ws.c_str(),
                                             static_cast<int>(ws.size()),
                                             nullptr, 0, nullptr, nullptr);
    if (nBytes <= 0) return true;   // 空行无需写内容
    std::string sUtf8(nBytes, 0);
    ::WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()),
                          &sUtf8[0], nBytes, nullptr, nullptr);
    // [R99] 正文与换行也要检查返回值：上一行 BOM 已检查，这两行原实现漏了。
    //   半写状态（BOM 进去、正文没进去）比全失败更糟：下次读会被当成空行。
    if (!::WriteFile(hF, sUtf8.c_str(), nBytes, &dwW, nullptr)
        || dwW != static_cast<DWORD>(nBytes)) return false;
    if (!::WriteFile(hF, (LPCVOID)L"\n", 1, &dwW, nullptr) || dwW != 1) return false;
    return true;   // PCRFileGuard 析构自动关闭
}

void CPageCleanup::OcWriteCleanHistory(const CleanDoneMsg* m)
{
    // [R60-A] 清理完成后磁盘空间采样 + 前后对比 + clean_history.log 追加
    const ULONGLONG ullAfter = QueryFreeBytesOnC();
    const ULONGLONG ullFreed = (m_ullCleanBeforeBytes > ullAfter)
                             ? (m_ullCleanBeforeBytes - ullAfter) : 0;
    CDuiString sDelta;
    sDelta.Format(StringMgr.Get("misc.h6643"),
                  m_ullCleanBeforeBytes / (1024*1024), ullAfter / (1024*1024),
                  ullFreed / (1024*1024), (m->ullFreed / (1024*1024)));
    AppendLog(sDelta.GetData());

    // [R7] 逐卷对比：按本次规则实际涉及的卷分别计算前后可用空间变化，
    // 避免只统计 C: 时把其它卷的释放量误记为 0 或重复计入。
    ULONGLONG ullMeasuredAll = 0;
    for (size_t vi = 0; vi < m_vVolBefore.size(); ++vi)
    {
        ULARGE_INTEGER liT = { 0 }, liF = { 0 }, liA = { 0 };
        if (!::GetDiskFreeSpaceExW(m_vVolBefore[vi].sRoot.GetData(), &liA, &liT, &liF))
            continue;
        const ULONGLONG ullNow = static_cast<ULONGLONG>(liF.QuadPart);
        const ULONGLONG ullDelta = (ullNow > m_vVolBefore[vi].ullFree)
                                 ? (ullNow - m_vVolBefore[vi].ullFree) : 0;
        ullMeasuredAll += ullDelta;
        CDuiString sVolLine;
        sVolLine.Format(L"[clean] volume %s: %.2f MB -> %.2f MB (+%.2f MB)",
                        m_vVolBefore[vi].sRoot.GetData(),
                        static_cast<double>(m_vVolBefore[vi].ullFree) / (1024.0 * 1024.0),
                        static_cast<double>(ullNow) / (1024.0 * 1024.0),
                        static_cast<double>(ullDelta) / (1024.0 * 1024.0));
        AppendLog(sVolLine.GetData());
    }
    if (m_vVolBefore.size() > 1)
    {
        CDuiString sAll;
        sAll.Format(L"[clean] measured across %d volumes: %.2f MB",
                    static_cast<int>(m_vVolBefore.size()),
                    static_cast<double>(ullMeasuredAll) / (1024.0 * 1024.0));
        AppendLog(sAll.GetData());
    }

    // clean_history.log 追加（跨会话清理历史）
    // [M-8] clean_history.log 并入统一日志目录。
    //   原实现写死 %LOCALAPPDATA%\DismTool_Clone\logs，是"三套落点"的最后一处；
    //   统一目录已含"逐级建目录 + 可写性探针 + 回退链"，不必再手工 EnsureDirectory。
    CDuiString sHistPath = LogService::UnifiedLogPath(L"clean_history.log").c_str();
    SYSTEMTIME stH; ::GetLocalTime(&stH);
    // ---- B-5 增强：收集规则名 / 分组分布 / 失败路径 ----
    CDuiString sGroups, sRules, sFailedPaths;
    OcCollectHistoryStats(sGroups, sRules, sFailedPaths);
    CDuiString sHistLine;
    // [R19] 增加结构化字段，便于事后核对"是否部分完成/是否需重启"，
    // 不再只能从自由文本里人工判断。字段顺序固定，追加在行尾以兼容既有解析。
    sHistLine.Format(L"%04d-%02d-%02d %02d:%02d:%02d | rules=%d failed=%d freed_reported=%llu MB freed_measured=%llu MB | partial=%d reboot=%d | groups=[%s] rules_list=[%s] failed_paths=[%s]",
                     stH.wYear, stH.wMonth, stH.wDay, stH.wHour, stH.wMinute, stH.wSecond,
                     m ? m->nRules : 0, m ? m->nFailed : 0,
                     (m->ullFreed / (1024*1024)), (ullFreed / (1024*1024)),
                     m_bLastCleanPartial ? 1 : 0,
                     (m && m->bCleanRestartExplorer) ? 1 : 0,
                     sGroups.GetData(), sRules.GetData(), sFailedPaths.GetData());
    OcAppendUtf8Line(sHistPath.GetData(), sHistLine);
}
// [C++17 R3] 本文件内联常量（C++17 inline variables，多 TU ODR 一致）：
