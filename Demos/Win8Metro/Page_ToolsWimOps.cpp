// ============================================================
// Page_ToolsWimOps.cpp —— 工具箱页「WIM/ESD 互转 + SWM 合并」执行族
// （从 Page_Tools.cpp 拆出）。
//
// [R141] Page_Tools.cpp 原 2188 行（空行规范化后 1676），规模分 6.49
// 为全仓最低。本文件承接映像转换相关 5 个成员：
//   · ToolSwmMerge        SWM 分卷合并到单一 WIM
//   · ToolWimEsdConvert   WIM↔ESD 互转入口（选源/查索引/选目标）
//   · WecChooseCompress   4 档压缩策略（config.ini xpress16k 优先）
//   · WecCheckTargetSpace 目标盘空间预估（不足时二次确认）
//   · WecLaunchConvert    建进度窗 + 完成回调 + 后台作业启动
//
// 闭包说明：全部是 CPageTools 的成员（声明见 Page_Tools.h）。线程层
// （TSwmMergeJob / SwmMergeThreadProc / TWecJob / WecJobThreadProc）
// 早已在 Page_ToolsWim.h 的 ToolWim 命名空间中，本文件只使用其声明。
// 段内自由函数仅 ToolSql::HostNotify / HostConfirm（Page_ToolsCommon.h）。
// WecRunConvert 定义仍在 Page_Tools.cpp（ToolJobs 命名空间，跨 TU 经
// Page_ToolsE2I.h 声明可见），搬迁后无需改动。
//
// 搬迁为逐字搬运，零逻辑改动。
// ============================================================

#include "stdafx.h"

#include <filesystem>   // [C17] std::filesystem::file_size

#include "Page_Tools.h"
#include "Page_ToolsCommon.h"   // ToolSql::HostNotify / HostConfirm
#include "Page_ToolsJobs.h"     // CJobProgressWnd / CJobDoneSink
#include "Page_ToolsWim.h"      // ToolWim 线程层（TSwmMergeJob / TWecJob）

#include "StringManager.h"
#include "Win32DialogService.h"  // DlgOpenFile / DlgSaveFile
#include "ConfigReader.h"        // [R-B32] [General] Compress
#include "WIMHelperBridge.h"     // WIMBridge::IsReady
#include "BackgroundJobs.h"      // CJobDoneSink

using namespace DuiLib;
using i18n::CStringManager;

namespace
{
using ToolJobs::CJobProgressWnd;   // 进度窗（命名空间 ToolJobs，见 Page_ToolsJobs.h）
using ToolSql::HostConfirm;
using ToolSql::HostNotify;
using ToolWim::SwmMergeThreadProc;
using ToolWim::TSwmMergeJob;
using ToolWim::TWecJob;
using ToolWim::WecJobThreadProc;
}   // namespace

// [C++17 R6] 本 TU 特征常量：空间预估倍率与余量（与原字面量同值）。
inline constexpr ULONGLONG kWecNeedFactor = 2;   // ESD -> WIM 膨胀保守按 2 倍
inline constexpr ULONGLONG kWecNeedMarginMb = 32; // 余量 32MB
static_assert(kWecNeedFactor >= 1, "space factor must not shrink the estimate");

// [CRASHFIX] SWM→WIM 合并作业上下文

// [CRASHFIX] SWM 合并线程：替代裸 std::thread::detach（G-CRS-01）

void CPageTools::ToolSwmMerge(const CDuiString& sSwm, const CDuiString& sWim)
{
    // [原生化] 替代 swm2wim.bat 中的 dism /Export-Image /SourceSWMFile。
    // wimgapi 打开首卷 .swm 时会自动按同目录标准命名发现 .swn 后续分卷，
    // 导出索引 1 到新建的单一 .wim（LZX/max 压缩，与原 dism /Compress:max 对齐）。
    if (sSwm.IsEmpty() || sWim.IsEmpty()) return;
    if (!WIMBridge::IsReady())
    {
        m_ctx.HostNotify(StringMgr.Get("toolbox.ui_swm_title"),
            StringMgr.Get("toolbox.ui_swm_unavailable"), 2);
        return;
    }
    // [rb28] SWM 合并本来就走 Bridge::ExportImageEx（wimgapi 原生进度），
    //        改动最小：把进度接到通用进度窗，不再显示无反馈的静态文字。
    HWND hWait = CJobProgressWnd::Create(m_ctx.HostHwnd(),
        std::wstring(StringMgr.Get("toolbox.ui_swm_busy")));
    CJobDoneSink::Cfg cfg;
    cfg.sOkTitle  = StringMgr.Get("toolbox.ui_swm_ok_title");
    cfg.sOkMsg    = StringMgr.Get("toolbox.ui_swm_ok_msg");
    cfg.sFailTitle = StringMgr.Get("toolbox.ui_swm_fail_title");
    cfg.bAskOpen  = true;
    HWND hSink = CJobDoneSink::Create(m_ctx.HostHwnd(),
                                      std::wstring(sWim.GetData()), cfg);
    // [CRASHFIX] 裸 detach 迁移为受 SEH 边界保护、可被 WM_CLOSE 等待的统一作业
    // [C++17 阶段3] 线程 ctx：所有权移交 SwmMergeThreadProc，Start 失败自删
    auto pJob = std::make_unique<TSwmMergeJob>(); // [C++17 R3]
    pJob->wSwm  = std::wstring(sSwm.GetData());
    pJob->wWim  = std::wstring(sWim.GetData());
    pJob->hWait = hWait;
    pJob->hSink = hSink;
    if (!BackgroundJobs::Start(&SwmMergeThreadProc, pJob.get(), "swm-merge"))
    {
        if (hWait && ::IsWindow(hWait)) ::PostMessageW(hWait, WM_CLOSE, 0, 0);
        HostNotify(m_ctx.HostHwnd(), StringMgr.Get("toolbox.s_job_start_fail"),
                       StringMgr.Get("toolbox.ui_swm_title"), MB_OK | MB_ICONERROR);
        return;   // [C++17 R3] 启动失败：unique_ptr 自动释放
    }
    pJob.release();   // [C++17 R3] ownership moved to SwmMergeThreadProc
}

// [CRASHFIX] WIM/ESD 互转作业上下文

// [CRASHFIX] WIM/ESD 互转线程：替代裸 std::thread::detach（G-CRS-01）

// [R170] 从 ToolWimEsdConvert 抽出：转换方向判定。
// nForceDir==1 → ESD→WIM；==0 → WIM→ESD；其余（-1）弹框询问，选「是」为 ESD→WIM。
// 文案与按钮组合逐字迁移；bToWim 的取值只由这三条路径决定，无其它写入点。
bool CPageTools::WecResolveDirection(int nForceDir)
{
    if (nForceDir == 1) return true;    // forced ESD → WIM
    if (nForceDir == 0) return false;   // forced WIM → ESD
    const int nDir = HostConfirm(m_ctx.HostHwnd(),
        CDuiString(StringMgr.Get("misc.s699cedf0")) + StringMgr.Get("misc.s10cb4221") + StringMgr.Get("misc.s32cc7382"),
        StringMgr.Get("misc.s83b1b6ca"), MB_YESNO, MB_ICONQUESTION);
    return (nDir == IDYES);
}

// [R170] 从 ToolWimEsdConvert 抽出：取 WIM 索引号上界（从 0 起逐项取大）。
// 调用方已保证 vIdx 非空；空容器返回 0，与迁移前初始值一致。
int CPageTools::WecMaxIndex(const std::vector<TDialogWimIndex>& vIdx)
{
    int nCount = 0;
    for (size_t i = 0; i < vIdx.size(); ++i)
        if (vIdx[i].nIndex > nCount) nCount = vIdx[i].nIndex;
    return nCount;
}

void CPageTools::ToolWimEsdConvert(int nForceDir)
{
    // [AUDIT-P2] 原 230 行 / CC=29，按阶段拆为 3 个辅助 + 内联选择段：
    //   WecChooseCompress    4 档压缩策略（含 config.ini xpress16k 优先）
    //   WecCheckTargetSpace  目标盘空间预估（不足时二次确认）
    //   WecLaunchConvert     建进度窗 + 完成回调 + 后台作业启动
    // 选源/查询索引/选目标仍内联（三段相互强耦合于 bToWim，拆出反而增加参数噪声）。
    // 弹窗文案、压缩档位映射、退出分支逐字保留。
    // [R170] 方向判定抽为 WecResolveDirection（原 3 分支 + 弹框内联）
    const bool bToWim = WecResolveDirection(nForceDir);
    CDuiString sSrc;
    if (!Win32DialogService::DlgOpenFile(sSrc,
            bToWim ? StringMgr.Get("toolbox.s72051c87")
                   : StringMgr.Get("toolbox.s3ce61375"),
            StringMgr.Get("toolbox.sd49ab690"))) return;
    std::vector<TDialogWimIndex> vIdx;
    if (!m_ctx.HostQueryWim(sSrc, vIdx, nullptr) || vIdx.empty())
    {
        HostNotify(m_ctx.HostHwnd(),
            (std::wstring(StringMgr.Get("error.s6880522a")) + std::wstring(StringMgr.Get("toolbox.s886c6be8"))).c_str(),
            StringMgr.Get("misc.s83b1b6ca"), MB_OK | MB_ICONWARNING);
        return;
    }
    // [R170] 索引上界统计抽为 WecMaxIndex（原 for + if 内联）
    const int nCount = WecMaxIndex(vIdx);
    CDuiString sDst;
    const wchar_t* pszFilter = bToWim
        ? StringMgr.Get("toolbox.s1363566d")
        : StringMgr.Get("toolbox.sa417abc0");
    if (!Win32DialogService::DlgSaveFile(sDst, pszFilter, StringMgr.Get("toolbox.sde44fa0e"),
                     bToWim ? L"converted.wim" : L"converted.esd")) return;
    const DWORD dwCompress = WecChooseCompress();
    if (!WecCheckTargetSpace(sSrc, sDst)) return;
    WecLaunchConvert(sSrc, sDst, dwCompress);
}

// [AUDIT-P2] 从 ToolWimEsdConvert 提取：4 档压缩策略选择
// [R-B48] config.ini [General] Compress=xpress16k 时优先采用并跳过弹窗
DWORD CPageTools::WecChooseCompress()
{
    // --- P1-2a：4 档压缩策略选择（MB_YESNOCANCEL 承载最常用 max/recovery，二级 MB_YESNO 覆盖 fast/none） ---
    CDuiString sCompressPrompt;
    sCompressPrompt += StringMgr.Get("confirm.compress_strategy_title");
    sCompressPrompt += StringMgr.Get("confirm.compress_strategy_max");
    sCompressPrompt += StringMgr.Get("confirm.compress_strategy_fast");
    sCompressPrompt += StringMgr.Get("confirm.compress_strategy_recovery");
    // [R-B48] 若 CLI /exe=xpress16k 已在 config.ini [General] Compress 指定，
    //        优先采用该算法（xpress16k 是基线第四算法，wimesd 互转时生效）
    DWORD dwCompress = WIM_COMPRESS_LZX;
    std::wstring wsCfgComp = i18n::CConfigReader::GetString(L"General", L"Compress", L"");
    for (auto& c : wsCfgComp) c = (wchar_t)::towlower((wchar_t)c);
    if (wsCfgComp == L"xpress16k")
    {
        // WIMGAPI 不暴露 16K 块长（CBSHost 专有参数），等价最近的是 XPRESS
        dwCompress = WIM_COMPRESS_XPRESS;
        m_ctx.HostLog(L"[compress] 采用 CLI/config 指定的 xpress16k（WIMGAPI 映射为 XPRESS），跳过选择弹窗");
    }
    else
    {
    int nComp = HostConfirm(m_ctx.HostHwnd(),
        sCompressPrompt.GetData(),
        StringMgr.Get("misc.s83b1b6ca"),
        MB_YESNOCANCEL, MB_ICONQUESTION);
    if (nComp == IDYES)       dwCompress = WIM_COMPRESS_LZX;       // max
    else if (nComp == IDCANCEL) dwCompress = WIM_COMPRESS_LZMS;    // recovery
    else                       // IDNO
    {
        int nC2 = HostConfirm(m_ctx.HostHwnd(),
            StringMgr.Get("misc.h955a"),
            StringMgr.Get("misc.s83b1b6ca"),
            MB_YESNO, MB_ICONQUESTION);
        dwCompress = (nC2 == IDYES) ? WIM_COMPRESS_XPRESS          // fast
                                    : WIM_COMPRESS_NONE;           // none
    }
    }
    return dwCompress;
}

// [AUDIT-P2] 从 ToolWimEsdConvert 提取：目标盘空间预估
// 返回 true 表示空间足够或用户选择继续；false 表示用户中止。
// [R160] 从 WecCheckTargetSpace 抽出：按源文件大小预估所需空间（含余量）。
//   ESD → WIM 通常膨胀 1.3-1.8 倍；WIM → ESD 通常缩小 0.6-0.9 倍，保守按 2 倍预估。
//   源文件读不到大小（网络路径/权限/刚删）时返回 0，表示"无从预估"，
//   调用方据此跳过空间检查而不是按 0 字节放行。
[[nodiscard]] ULONGLONG CPageTools::WecEstimateNeed(const CDuiString& sSrc)
{
    std::error_code ec2;
    auto uSize2 = std::filesystem::file_size(sSrc.GetData(), ec2);
    if (ec2 || uSize2 == 0) return 0;
    return static_cast<ULONGLONG>(uSize2) * kWecNeedFactor
           + (1024ULL * 1024ULL * kWecNeedMarginMb);  // 余量
}

// [R160] 从 WecCheckTargetSpace 抽出：目标盘剩余空间不足时向用户二次确认。
//   只在目标路径是盘符形式（X:...）时检查；UNC / 长路径拿不到盘符就跳过。
//   返回 false 仅表示"用户明确选择中止"，其余情况（无需检查、查询失败）都放行。
[[nodiscard]] bool CPageTools::WecConfirmSpaceEnough(const CDuiString& sDst,
                                                     ULONGLONG ullNeed)
{
    if (sDst.GetLength() < 2 || sDst[1] != L':') return true;
    CDuiString sDstDrv; sDstDrv.Format(L"%c:", sDst[0]);
    ULARGE_INTEGER ullFree = { 0 };
    if (!::GetDiskFreeSpaceEx(sDstDrv.GetData(), &ullFree, nullptr, nullptr)) return true;
    if (ullFree.QuadPart >= ullNeed) return true;
    ULONGLONG mbFree = ullFree.QuadPart / (1024 * 1024);
    ULONGLONG mbNeed = ullNeed        / (1024 * 1024);
    CDuiString sMsg;
    sMsg.Format(StringMgr.Get("misc.h2177"), sDstDrv.GetData(), mbNeed, mbFree);
    return HostConfirm(m_ctx.HostHwnd(), sMsg.GetData(),
                       StringMgr.Get("misc.h8ed5"), MB_YESNO, MB_ICONWARNING) == IDYES;
}

bool CPageTools::WecCheckTargetSpace(const CDuiString& sSrc, const CDuiString& sDst)
{
    // --- P1-2b：目标盘空间预估（复用 P0-4 模式） ---
    // [R160] 原实现 5 层 if 嵌套叠在同一函数里；拆为"预估"与"确认"两个语义单元后
    //   本函数只剩一条线性前置判断，两个子步骤各自可单测。
    const ULONGLONG ullNeed = WecEstimateNeed(sSrc);
    if (ullNeed == 0) return true;          // 源文件大小未知：不预估，不放行误判
    return WecConfirmSpaceEnough(sDst, ullNeed);
}

// [AUDIT-P2] 从 ToolWimEsdConvert 提取：建窗 + 启动后台转换作业
void CPageTools::WecLaunchConvert(const CDuiString& sSrc, const CDuiString& sDst,
                                  DWORD dwCompress)
{
    // [2026-10-08 复刻修正] 互转直走 wimgapi（知识库 40 §2.1 行7、§2.3），
    // 不再生成 dism bat。后台线程执行，完成后回 UI 线程弹窗。
    if (::GetFileAttributesW(sDst.GetData()) != INVALID_FILE_ATTRIBUTES)
        ::DeleteFileW(sDst.GetData());
    // [rb28] 静态等待窗 → 真实百分比进度窗（多索引按索引数线性推进）
    HWND hWait = CJobProgressWnd::Create(m_ctx.HostHwnd(),
        std::wstring(StringMgr.Get("toolbox.ui_convert_busy")));
    // [R16-T9] CWecDoneSink → 统一 CJobDoneSink（文案逐字等价迁入 Cfg）
    CJobDoneSink::Cfg cfg;
    cfg.sOkTitle   = StringMgr.Get("toolbox.ui_convert_ok_title");
    cfg.sOkMsg     = StringMgr.Get("toolbox.ui_convert_ok_msg");
    cfg.sFailTitle = StringMgr.Get("toolbox.ui_convert_fail_title");
    cfg.sFailMsg   = StringMgr.Get("toolbox.ui_convert_fail_msg");
    cfg.bAskOpen   = true;
    HWND hSink = CJobDoneSink::Create(m_ctx.HostHwnd(),
                                      std::wstring(sDst.GetData()), cfg);
    // [CRASHFIX] 裸 detach 迁移为受 SEH 边界保护、可被 WM_CLOSE 等待的统一作业
    // [C++17 阶段3] 线程 ctx：所有权移交 WecJobThreadProc，Start 失败自删
    auto pJob = std::make_unique<TWecJob>(); // [C++17 R3]
    pJob->wSrc       = std::wstring(sSrc.GetData());
    pJob->wDst       = std::wstring(sDst.GetData());
    pJob->dwCompress = dwCompress;
    pJob->hWait      = hWait;
    pJob->hSink      = hSink;
    // [R105] 互转作业接入取消；令牌进页面列表，点取消时一起置位
    pJob->pCancel    = BackgroundJobs::MakeCancelToken();
    RegisterCancelToken(pJob->pCancel);
    if (!BackgroundJobs::Start(&WecJobThreadProc, pJob.get(), "wim-esd-convert"))
    {
        if (hWait && ::IsWindow(hWait)) ::PostMessageW(hWait, WM_CLOSE, 0, 0);
        UnregisterLastCancelToken();   // [R105] 没起来就别留令牌
        HostNotify(m_ctx.HostHwnd(), StringMgr.Get("toolbox.s_job_start_fail"),
                       StringMgr.Get("toolbox.ui_convert_title"), MB_OK | MB_ICONERROR);
        return;   // [C++17 R3] 启动失败：unique_ptr 自动释放
    }
    pJob.release();   // [C++17 R3] ownership moved to WecJobThreadProc
}
