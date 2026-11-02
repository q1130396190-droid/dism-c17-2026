// ============================================================
// Page_UpdatePhase.cpp —— 更新页「三阶段更新流 + 下载线程上限」
// （从 Page_Update.cpp 拆出）。
//
// [R139] Page_Update.cpp 2410 行，规模分 5.89（拆分后全仓最低）。
// 本文件承接两条自成体系的链路：
//   1) 三阶段更新流 PreDownload → Install → Finalize：Info 下载校验、
//      阶段启动与状态回写、阶段状态文本映射、失败弹日志视图；
//   2) 下载并发上限：combo 填充、用户改值落 config.ini、生效值优先级。
//
// 闭包说明：全部是 CPageUpdate 的成员（声明见 Page_Update.h）。
// 随段迁出的文件级实体只有三个，且调用点全部在本段内（已逐个 grep 确认）：
//   · PUFileGuard        —— Info 文件读句柄 RAII
//   · GetOwnExePath      —— 取当前 exe 全路径（自调用三阶段用）
//   · GetUpdStateText    —— 9 态文本映射（运行时经 StringMgr 本地化）
//
// 搬迁为逐字搬运，零逻辑改动。
// ============================================================

#include "stdafx.h"

#include "Page_Update.h"

#include "StringManager.h"       // StringMgr
#include "FrameWnd_Util.h"       // FrameWnd_Util_GetTempDir
#include "ProcessRunner.h"       // [R74] RunHiddenExit
#include "UpdateMetaService.h"   // [R-B8] UpdateInfo 四字段 / wsusscn 随包
#include "ConfigReader.h"        // [R-B50] config.ini [Update] MaxThread 读写
#include "UpdPhaseRelay.h"       // [R-B51] 阶段状态文件 + 命名事件回传
#include "ToolDialogPage.h"      // [R-B51] EToolDlgKind::TD_LOGVIEW 失败定位
#include "BackgroundJobs.h"      // [FIX-P2N] 阶段子进程等待后台化

using namespace DuiLib;
using i18n::CStringManager;

// [C++17 R6] 本 TU 特征常量：三阶段数量与下载线程上限边界。
inline constexpr int kUpdPhaseCount = 3;      // PreDownload / Install / Finalize
inline constexpr int kUpdMaxThreadLimit = 128; // config.ini 写入上限
static_assert(kUpdPhaseCount == 3, "three-phase update flow is fixed");
static_assert(kUpdMaxThreadLimit > 0, "thread limit must be positive");

// [C++17 阶段2] 局部 RAII 守卫：文件句柄（见 WinHandles.h 使用规范）
struct PUFileGuard
{
    HANDLE h = INVALID_HANDLE_VALUE;
    PUFileGuard() = default;
    explicit PUFileGuard(HANDLE x) : h(x) {}
    ~PUFileGuard() { if (h && h != INVALID_HANDLE_VALUE) ::CloseHandle(h); }
    PUFileGuard(const PUFileGuard&) = delete;
    PUFileGuard& operator=(const PUFileGuard&) = delete;
    operator HANDLE() const { return h; }
    [[nodiscard]] bool valid() const { return h && h != INVALID_HANDLE_VALUE; }
};

// ============================================================
// [R-B4] 三阶段自调用：取当前 exe 完整路径（原版 Dism++OSUpgrade 等价）
// ============================================================
static CDuiString GetOwnExePath()
{
    wchar_t szPath[MAX_PATH] = {};
    DWORD dw = ::GetModuleFileNameW(nullptr, szPath, _countof(szPath));
    if (dw == 0 || dw >= _countof(szPath))
        return CDuiString(L"Win8Metro.exe");
    return CDuiString(szPath);
}

// ============================================================
// [R74] 三阶段更新流：PreDownload → Install → Finalize
// 使用 dism++ 工具执行，支持进度追踪
// ============================================================

// 阶段状态文本表（运行时经 StringMgr 本地化，不能在 static 数组里用 Get）
static const wchar_t* GetUpdStateText(int nState)
{
    switch (nState)
    {
    case 0:  return StringMgr.Get("update.state.notstart");    // 未开始
    case 1:  return StringMgr.Get("misc.s4ed23304");           // 准备中
    case 2:  return StringMgr.Get("misc.scdfbdf63");           // 下载中
    case 3:  return StringMgr.Get("misc.s77ea511e");           // 校验中
    case 4:  return StringMgr.Get("misc.sf23d3062");           // 安装中
    case 5:  return StringMgr.Get("update.state.completing");  // 完成中
    case 6:  return StringMgr.Get("misc.s35d25224");           // 完成
    case 7:  return StringMgr.Get("ui.msg.failed");            // 失败
    case 8:  return StringMgr.Get("update.state.waiting");     // 等待中
    default: return L"";
    }
}

void CPageUpdate::UpdateUpdatePhaseState(int nState)
{
    if (nState < 0 || nState > 8) return;
    m_sUpdatePhaseState = GetUpdStateText(nState);
    m_ctx.HostLog(StringMgr.Get("misc.sb924cc56"));
    m_ctx.HostLog(m_sUpdatePhaseState.GetData());
}

// [R153] StartUpdateInfoDownload 拆为三段（原 121 行 / CC23）。
// 原实现把「探测缓存 → 断点续传 → 校验大小 → 解析四字段 → Key 回传」
// 串在一个函数里，判定点密集。抽出后主函数只留阶段推进编排。
// 三段之间只通过返回值传递「是否继续」，不共享可变状态。

// 校验已下载的 UpdateInfo.zip 大小；>1KB 视为有效并转入「完成」阶段。
// 返回 false 表示文件缺失或过小（调用方须把阶段置为失败）。
bool CPageUpdate::UpdVerifyInfoZip()
{
    ULONGLONG ullSize = 0;
    PUFileGuard hFile(::CreateFile(m_sUpdateInfoPath.GetData(), GENERIC_READ, FILE_SHARE_READ,
                                   nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (hFile != INVALID_HANDLE_VALUE)
    {
        LARGE_INTEGER li; li.QuadPart = 0;
        ::GetFileSizeEx(hFile, &li);
        ullSize = static_cast<ULONGLONG>(li.QuadPart);
        // PUFileGuard 析构自动关闭
    }
    if (ullSize <= 1024)
    {
        UpdateUpdatePhaseState(7);  // 失败
        m_ctx.HostLog(StringMgr.Get("misc.se371b848"));
        return false;
    }
    UpdateUpdatePhaseState(6);  // 完成
    CDuiString sOk;
    sOk.Format(StringMgr.Get("misc.s61c18bf7"), ullSize);
    m_ctx.HostLog(sOk.GetData());
    return true;
}

// 取 <URLS> 首个地址（以 ; , CR LF 空格 TAB 分隔）作为上报端点。
// 无有效地址时保持 m_sReportEndpoint 原值不变。
void CPageUpdate::UpdResolveReportEndpoint(const wchar_t* pszUrls)
{
    if (!pszUrls || !pszUrls[0]) return;
    int nEnd = 0;
    while (pszUrls[nEnd] && pszUrls[nEnd] != L';' && pszUrls[nEnd] != L','
           && pszUrls[nEnd] != L'\r' && pszUrls[nEnd] != L'\n'
           && pszUrls[nEnd] != L' ' && pszUrls[nEnd] != L'\t')
        ++nEnd;
    if (nEnd > 0) m_sReportEndpoint = CDuiString(pszUrls).Mid(0, nEnd);
}

// 把 Key/Tag 组包成 Report XML 投递到 outbox（失败由服务层排队补发）。
void CPageUpdate::UpdPostKeyReport(const UpdateMetaService::UpdateMeta& meta)
{
    CDuiString sExe2 = GetOwnExePath();
    const int q2 = sExe2.ReverseFind(L'\\');
    CDuiString sOutbox = (q2 >= 0 ? sExe2.Left(q2) : sExe2) + L"\\Report\\outbox";
    CDuiString sBody;
    sBody.Format(L"<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                 "<Report Kind=\"Key\"><Key><![CDATA[%s]]></Key>"
                 "<Tag>%s</Tag></Report>",
                 meta.szKeys[0] ? meta.szKeys : L"",
                 meta.szTag[0] ? meta.szTag : L"");
    UpdateMetaService::PostReportEvent(m_sReportEndpoint, L"Key",
                                       sBody.GetData(), sOutbox.GetData(), 6000);
    m_ctx.HostLog(L"[update] Key 回传已提交（失败将排队 outbox）");
}

// [R-B8] 四字段解析：Keys / URLS / Information / Tag；成功后回传 Key。
void CPageUpdate::UpdParseInfoZip()
{
    UpdateMetaService::UpdateMeta meta;
    if (!UpdateMetaService::ParseUpdateInfoZip(m_sUpdateInfoPath.GetData(), meta))
    {
        m_ctx.HostLog(L"[update] UpdateInfo.xml 四字段解析失败（可能非预期格式）");
        return;
    }
    // [R-B50] 缓存 UpdateInfo 解析的 MaxDownloadThread，作为 config.ini 缺省时的回退值
    m_dwMetaMaxThread = meta.dwMaxDownloadThread;
    CDuiString sMeta;
    sMeta.Format(L"[update] UpdateInfo 四字段解析成功 | Tag=%s | MaxThread=%u",
                 meta.szTag, meta.dwMaxDownloadThread);
    m_ctx.HostLog(sMeta.GetData());
    if (meta.szKeys[0])
    {
        CDuiString sKeys;
        sKeys.Format(L"[update] UpdateInfo Keys = %s", meta.szKeys);
        m_ctx.HostLog(sKeys.GetData());
    }
    if (meta.szInfo[0])
    {
        CDuiString sInfo;
        sInfo.Format(L"[update] UpdateInfo Information = %s", meta.szInfo);
        m_ctx.HostLog(sInfo.GetData());
    }
    // [R-B78] Key 回传：端点取 <URLS> 首地址；发送失败自动落
    //          Report\outbox 排队，下次 Build 时 FlushReportOutbox 补发。
    UpdResolveReportEndpoint(meta.szUrls);
    UpdPostKeyReport(meta);
}

void CPageUpdate::StartUpdateInfoDownload()
{
    // [R-B88] 更新信息地址暂时留空，后期在此填入新地址。
    //   原地址：http://update.chuyu.me/UpdateInfo.zip
    CDuiString sUrl = L"";
    CDuiString sTmpDir = FrameWnd_Util_GetTempDir();
    m_sUpdateInfoPath = sTmpDir + L"\\UpdateInfo.zip";
    if (::GetFileAttributes(m_sUpdateInfoPath.GetData()) != INVALID_FILE_ATTRIBUTES)
    {
        m_ctx.HostLog(StringMgr.Get("misc.se7d3c6cf"));
        return;
    }
    UpdateUpdatePhaseState(2);  // 下载中
    // [R-B88] 更新地址未配置（留空）时跳过在线下载，不发起网络请求。
    if (sUrl.IsEmpty())
    {
        m_ctx.HostLog(L"更新信息地址未配置，已跳过在线下载。");
        return;
    }
    // [R-B77] 改走 Range 断点续传引擎（.pending/Range/MD5/job.xml），
    //         引擎内部失败自动降级 PowerShell Invoke-WebRequest。
    const bool bDl = UpdateMetaService::DownloadWithResume(sUrl.GetData(),
                                                           m_sUpdateInfoPath.GetData());
    const bool bExists = ::GetFileAttributes(m_sUpdateInfoPath.GetData()) != INVALID_FILE_ATTRIBUTES;
    if (!bDl || !bExists)
    {
        UpdateUpdatePhaseState(7);  // 失败
        m_ctx.HostLog(StringMgr.Get("misc.s3bd36694"));
        return;
    }
    UpdateUpdatePhaseState(3);  // 校验中
    m_ctx.HostLog(StringMgr.Get("misc.s04a7b276"));
    if (!UpdVerifyInfoZip()) return;
    UpdParseInfoZip();
}

// [R153] StartUpdatePhase 拆为三段（原 113 行 / CC19）。
// 「前置保障（cab + CBS 倒计时）」「按阶段拼命令行」「读状态文件并报告」
// 各自成函数，主函数只留顺序编排。

// 前置保障：确保 wsusscn2/3.cab 就位；阶段 0 还要走一次 CBS 独占倒计时。
// 返回 false 表示用户在倒计时里选择"暂不安装"（调用方须置失败并返回）。
bool CPageUpdate::UpdPhasePreflight(int nPhase)
{
    // [R-B8] 随包保障：在 PreDownload / Install 前探测 wsusscn2/3.cab
    //   离线源需要 cab 文件就位（缺失时从微软官方目录下载）。
    CDuiString sExePath = GetOwnExePath();
    const int p = sExePath.ReverseFind(L'\\');
    CDuiString sExeDir = (p >= 0) ? sExePath.Left(p) : sExePath;
    if (UpdateMetaService::EnsureOfflineCabs(sExeDir.GetData(), true))
        m_ctx.HostLog(L"[update] wsusscn2.cab / wsusscn3.cab 离线数据已就绪");
    else
        m_ctx.HostLog(L"[update] wsusscn2.cab / wsusscn3.cab 离线数据下载失败（在线源仍可用）");
    if (nPhase != 0) return true;   // 仅 PreDownload 前置一次，其余阶段不再打扰
    // [R-B8] CBS 独占请求倒计时对话框（"暂不安装（推荐）/继续安装"）
    const int nRet = UpdateMetaService::ShowExclusiveCountdown(
                        m_ctx.HostHwnd(),
                        L"Windows 更新 - CBS 独占请求",
                        L"系统正在请求 CBS 独占以安装更新。\n"
                        L"建议先暂不安装，等待当前任务完成后再继续。",
                        30);
    if (nRet != IDYES)
    {
        m_ctx.HostLog(L"[update] 用户选择\"暂不安装\"，CBS 独占请求被拒绝，PreDownload 跳过");
        UpdateUpdatePhaseState(7);  // 失败（用户主动中止）
        return false;
    }
    m_ctx.HostLog(L"[update] 用户选择\"继续安装\"，CBS 独占请求获得批准");
    return true;
}

// 按阶段拼子进程命令行并同步阶段状态；返回 false 表示阶段号非法。
bool CPageUpdate::UpdBuildPhaseCommand(int nPhase, const CDuiString& sPhaseFile, CDuiString& sCmd)
{
    const CDuiString sExe = GetOwnExePath();
    switch (nPhase)
    {
    case 0:
        sCmd.Format(L"\"%s\" /PreDownload /Update /path:\"%s\"",
                    sExe.GetData(), sPhaseFile.GetData());
        UpdateUpdatePhaseState(2);
        return true;
    case 1:
        sCmd.Format(L"\"%s\" /Install /Update /path:\"%s\"",
                    sExe.GetData(), sPhaseFile.GetData());
        UpdateUpdatePhaseState(4);
        return true;
    case 2:
        sCmd.Format(L"\"%s\" /Finalize /Update /noreboot /path:\"%s\"",
                    sExe.GetData(), sPhaseFile.GetData());
        UpdateUpdatePhaseState(4);
        return true;
    default:
        m_ctx.HostLog(StringMgr.Get("misc.s816bfd5c"));
        return false;
    }
}

// [R-B51] 读回子进程写的状态文件，驱动 9 态文本；再按退出码给出最终结论。
// 失败时弹出日志视图便于定位。
void CPageUpdate::UpdReportPhaseResult(DWORD dwRet, const CDuiString& sPhaseFile)
{
    int  nSt = -1, nErr = 0, bRetry = 0;
    CDuiString sMsg;
    const bool bHasFile = UpdPhaseRelay::ReadPhase(sPhaseFile.GetData(),
                                                   nSt, nErr, bRetry, sMsg);
    if (bHasFile && nSt >= 0 && nSt <= 8)
    {
        UpdateUpdatePhaseState(nSt);
        CDuiString sLog;
        sLog.Format(L"[update] phase relay: state=%d err=0x%08X retry=%d msg=%s",
                    nSt, (unsigned)nErr, bRetry,
                    sMsg.IsEmpty() ? L"" : sMsg.GetData());
        m_ctx.HostLog(sLog.GetData());
    }
    if (dwRet == 0 && (!bHasFile || nSt == static_cast<int>(EUpdState::EUPD_DONE)))
    {
        UpdateUpdatePhaseState(6);
        m_ctx.HostLog(StringMgr.Get("misc.sbfa2e877"));
        return;
    }
    UpdateUpdatePhaseState(7);
    CDuiString sErr;
    if (bHasFile)
        sErr.Format(L"[update] 阶段失败：退出码=%lu 错误码=0x%08X 消息=%s",
                    dwRet, (unsigned)nErr,
                    sMsg.IsEmpty() ? L"" : sMsg.GetData());
    else
        sErr.Format(StringMgr.Get("misc.s683452aa"), dwRet);
    m_ctx.HostLog(sErr.GetData());
    // [R-B51] 失败 → 弹日志视图定位（EToolDlgKind::TD_LOGVIEW，宿主薄转）
    m_ctx.HostShowLogView();
}

void CPageUpdate::StartUpdatePhase(int nPhase)
{
    // [FIX-P2N] 在途防重入：后台化后子进程最长 300s，期间重复点击按钮
    // 会并发两条阶段流（9 态文本互相覆盖）。
    if (m_bPhaseRunning)
    {
        m_ctx.HostLog(L"[upd] update phase already running, ignored");
        return;
    }
    if (m_sUpdateInfoPath.IsEmpty()
        || ::GetFileAttributes(m_sUpdateInfoPath.GetData()) == INVALID_FILE_ATTRIBUTES)
    {
        m_ctx.HostLog(StringMgr.Get("misc.sb25b7011"));
        StartUpdateInfoDownload();
        return;
    }
    if (!UpdPhasePreflight(nPhase)) return;
    UpdateUpdatePhaseState(1);  // 准备中
    // [R-B51] 阶段状态文件路径（主进程侧生成，/path= 传给子进程）
    CDuiString sPhaseFile = UpdPhaseRelay::MakePhaseFile(nPhase);
    CDuiString sCmd;
    if (!UpdBuildPhaseCommand(nPhase, sPhaseFile, sCmd)) return;
    m_ctx.HostLog(StringMgr.Get("misc.s61631283"));
    m_ctx.HostLog(sCmd.GetData());
    m_nUpdatePhase = nPhase + 1;
    // [R-B51] 拉起子进程（子进程内部按阶段动作并写状态文件）。
    // [FIX-P2N] 子进程等待移入 BackgroundJobs 后台线程：原 UI 线程
    // RunHiddenExit 最长 300s 输入全丢；后台化后泵保绘制、输入可用，
    // 完成经 WM_UPD_PHASEDONE 回 UI 收口（UpdReportPhaseResult）。
    m_bPhaseRunning = true;
    struct PhaseCtx
    {
        HWND         hNotify;
        int          nPhase;
        DWORD        dwRetDummy;
        std::wstring sCmd;
        std::wstring sPhaseFile;
    };
    PhaseCtx* pCtx = new PhaseCtx{ m_ctx.HostHwnd(), nPhase, 0,
                                   std::wstring(sCmd.GetData()),
                                   std::wstring(sPhaseFile.GetData()) };
    HANDLE hTh = BackgroundJobs::Start([](void* pv) -> DWORD {
        std::unique_ptr<PhaseCtx> guard(static_cast<PhaseCtx*>(pv));
        const DWORD dwRet = ProcessRunner::RunHiddenExit(guard->sCmd.c_str(), L"", 300000);
        auto pDone = std::make_unique<UpdPhaseDoneMsg>();
        pDone->nPhase = guard->nPhase;
        pDone->dwRet = dwRet;
        pDone->sPhaseFile = guard->sPhaseFile;
        BackgroundJobs::PostHeapOrDelete(guard->hNotify, WM_UPD_PHASEDONE, 0, pDone.release());
        return 0;
    }, pCtx, "upd-phase");
    if (!hTh)
    {
        m_bPhaseRunning = false;   // 线程创建失败：解标志，状态回未开始
        UpdateUpdatePhaseState(0);
        m_ctx.HostLog(L"[upd] phase thread create failed");
    }
}
// ============================================================
// [R-B50] 下载线程数上限选择（清单维度 7 缺失 1）
// ------------------------------------------------------------
// 基线要求：MaxDownloadThread=10（现实现仅解析进日志，未消费）。
// 闭环：① 更新页搜索条新增 upd_maxthread_combo（8/10/12/16/20 五档）；
//       ② BuildMaxThreadCombo 在页面 Build 时填充并选中 config.ini 值；
//       ③ OnMaxThreadChanged 在 selectchanged 时写 config.ini [Update] MaxThread；
//       ④ GetEffectiveMaxThread 优先级：config.ini > UpdateInfo 解析值 > 默认 10；
//       ⑤ OnInstallButton 把该值传入 InstallUpdates → 下载阶段真实并发闸门。
// ============================================================
void CPageUpdate::BuildMaxThreadCombo()
{
    // 确保 config.ini 已定位（GetConfigPath 依赖 Load；Load 需 exe 目录）
    {
        CDuiString sExe = GetOwnExePath();
        int p = sExe.ReverseFind(L'\\');
        CDuiString sDir = (p >= 0) ? sExe.Left(p) : sExe;
        i18n::CConfigReader::Load(sDir.GetData());
    }

    CComboUI* pCombo = static_cast<CComboUI*>(
        m_ctx.HostFindControl(L"upd_maxthread_combo"));
    if (!pCombo) return;

    pCombo->RemoveAll();
    static const wchar_t* kThreads[] = { L"8", L"10", L"12",
                                       L"16", L"20" };
    constexpr int nCount = static_cast<int>(sizeof(kThreads) / sizeof(kThreads[0]));

    DWORD dwCur = GetEffectiveMaxThread();
    int nSel = 0;
    for (int i = 0; i < nCount; ++i)
    {
        auto pItem = std::make_unique<CListLabelElementUI>(); // [C++17 R3]
        pItem->SetText(kThreads[i]);
        pCombo->Add(pItem.release());  // [C++17 R3] ownership to DuiLib
        if (_ttoi(kThreads[i]) == static_cast<int>(dwCur)) nSel = i;
    }
    pCombo->SelectItem(nSel);

    CDuiString sLog;
    sLog.Format(L"[update] MaxDownloadThread effective=%u (config or default)",
                (unsigned)dwCur);
    m_ctx.HostLog(sLog.GetData());
}

void CPageUpdate::OnMaxThreadChanged()
{
    CComboUI* pCombo = static_cast<CComboUI*>(
        m_ctx.HostFindControl(L"upd_maxthread_combo"));
    if (!pCombo) return;
    int nSel = pCombo->GetCurSel();
    if (nSel < 0) return;

    CDuiString sItem = pCombo->GetText();
    long l = _ttol(sItem.GetData());
    if (l < 1) l = 1;
    if (l > kUpdMaxThreadLimit) l = kUpdMaxThreadLimit;

    if (!UpdateMetaService::WriteEffectiveMaxThread(
            i18n::CConfigReader::GetConfigPath(), static_cast<DWORD>(l)))
    {
        m_ctx.HostLog(L"[update] MaxDownloadThread 写 config.ini 失败");
        return;
    }

    CDuiString sLog;
    sLog.Format(L"[update] MaxDownloadThread user set=%ld (persisted to config.ini)", l);
    m_ctx.HostLog(sLog.GetData());
}

DWORD CPageUpdate::GetEffectiveMaxThread()
{
    // 优先级：config.ini [Update] MaxThread > UpdateInfo 解析值 > 默认 10
    DWORD dwBase = m_dwMetaMaxThread;
    if (dwBase < 1) dwBase = 10;
    if (dwBase > kUpdMaxThreadLimit) dwBase = kUpdMaxThreadLimit;
    return UpdateMetaService::ReadEffectiveMaxThread(
                i18n::CConfigReader::GetConfigPath(), dwBase);
}
