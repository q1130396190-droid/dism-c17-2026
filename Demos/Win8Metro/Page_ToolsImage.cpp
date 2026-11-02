#include "stdafx.h"

// ============================================================
// Page_ToolsImage.cpp —— [R91] 工具箱"ESD 与映像工具组"
//
// 从 Page_Tools.cpp 切出的 8 个成员函数（函数体零逻辑改动）：
//   EsdCheckBootImage          boot.wim/esd 存在性检查 + 缺失确认
//   EsdCheckTargetSpace        目标盘空间预估 + 不足确认
//   ToolEsd2Iso                ESD → ISO 转换（选文件 → 起作业）
//   EsdLaunchConvertJob        提交转换作业
//   ToolImageX                 imagex 导出
//   ToolEsdDecrypt             ESD 解密
//   ToolEsdDecryptWithFile     指定文件解密
//   ToolIsoGen                 ISO 生成
//   ToolWin11ImagePreprocess   Win11 映像预处理（版本门禁/包生成/校验全在 Rust）
//
// 为什么这组可以独立：
//   全部是成员函数，跨 TU 调用无链接性问题；
//   共同的领域是"映像文件处理"，与其余工具（hosts/激活/备份）无共享 static；
//   切片外调用点：ToolDispatchItem（按 nTool 分发）、RunDialog，均经类声明访问。
//
// 拆分动因：Page_Tools.cpp 1112 行，审计报告"规模"维度仅 5.6 分
//   （A 级对照：RegClean 876 行 / Tasks 724 行）。
// ============================================================

#include "Page_Tools.h"
#include "NotifyBox.h"        // [R91] HostNotify / HostConfirm（中立头）
#include "StringManager.h"
#include "HostInterfaces.h"      // HostNotify
#include "WmMessages.h"
#include <filesystem>   // [R91] file_size（EsdCheckTargetSpace）
#include <optional>     // [R91] std::optional<DWORD>
#include <thread>
#include <memory>

#include "Page_ToolsCommon.h"  // 跨文件共享的 RAII 守卫与 UI helper
#include "Page_ToolsJobs.h"    // 作业进度窗与进度桥
#include "Page_ToolsE2I.h"     // E2I 作业层
#include "Page_ToolsWim.h"     // WIM/ESD 互转与 SWM 合并线程层
#include "FrameWnd_Util.h"
#include "Win32DialogService.h"
#include "ProcessRunner.h"
#include "WIMHelper.h"
#include "WIMHelperBridge.h"
#include "IsoWriter.h"
#include "DismCore.h"          // Rust Windows 11 镜像预处理包生成/校验
#include "BackgroundJobs.h"    // 后台作业统一收口

using namespace DuiLib;
using i18n::CStringManager;
// [R91] 从 Page_Tools.cpp 原样搬来的 using 声明（原文件 67~68 行）：
//   HostNotify / HostConfirm 定义在 ToolSql 命名空间（NotifyBox.h）。
using ToolSql::HostNotify;
using ToolSql::HostConfirm;
// [R91] 本组用到的跨文件符号（与 Page_Tools.cpp 的 using 段一致）：
namespace {
using ToolE2I::E2IBuild;
using ToolE2I::E2iJobThreadProc;
using ToolE2I::TE2iJob;
using ToolJobs::CJobProgressWnd;
using ToolJobs::JobProgPost;
using ToolJobs::TJobWimProgress;
using ToolSql::ResolveProc;
} // namespace



// [R151] ToolEsd2Iso 的两段内联逻辑外提（原 CC=22）：
//   ① boot.wim/boot.esd 存在性判定 + 缺失时的"仍要继续"确认；
//   ② 目标盘空间预估 + 不足时的确认。
// 两段都是「查条件 → 弹框 → 决定是否中止」，与主流程的"选文件 → 起作业"无耦合。
// 返回 false 表示用户选择中止。
bool CPageTools::EsdCheckBootImage(const CDuiString& sEsd, CDuiString& sBootWim, bool& bHasBootWim)
{
// --- P0-4a：boot.wim 缺失前置提示 ---
    CDuiString sEsdDir = sEsd.Mid(0, sEsd.ReverseFind(L'\\') + 1);
    sBootWim = sEsdDir + L"boot.wim";
    bHasBootWim = ::GetFileAttributes(sBootWim) != INVALID_FILE_ATTRIBUTES;
    if (!bHasBootWim)
    {
        // 也检查 boot.esd（新版 Windows 11 22H2+ 有些源里 boot 也用 esd）
        CDuiString sBootEsd = sEsdDir + L"boot.esd";
        bHasBootWim = ::GetFileAttributes(sBootEsd) != INVALID_FILE_ATTRIBUTES;
        if (bHasBootWim) sBootWim = sBootEsd;  // 用实际存在的
    }
    if (bHasBootWim) return true;
    // 用户明确知道 ISO 不可启动后仍可继续
    CDuiString sMsg = CDuiString(StringMgr.Get("misc.h8177")) + CDuiString(StringMgr.Get("misc.h1af8")) + CDuiString(StringMgr.Get("misc.h30b8")) + CDuiString(StringMgr.Get("misc.he202")) + CDuiString(StringMgr.Get("misc.hca90"));
    const int nGo = HostConfirm(m_ctx.HostHwnd(), sMsg.GetData(), StringMgr.Get("misc.h3960"), MB_YESNO | MB_DEFBUTTON2);
    return nGo == IDYES;
}

// 目标盘空间预估：ESD 解压后约为 ESD 大小的 3.5-4 倍，再叠加 boot.wim 与引导文件。
// 空间不足时二次确认；返回 false 表示用户中止。
bool CPageTools::EsdCheckTargetSpace(const CDuiString& sEsd, const CDuiString& sIso)
{
// --- P0-4b：目标盘空间预估 ---
// ESD 解压后约为 ESD 大小的 3.5 倍；ISO 再叠加 boot.wim + 引导文件
    ULARGE_INTEGER ullEsd = { 0 };
    std::error_code ec;
    auto uSize = std::filesystem::file_size(sEsd.GetData(), ec);
    if (!ec && uSize > 0) ullEsd.QuadPart = static_cast<ULONGLONG>(uSize);
    if (ullEsd.QuadPart == 0) return true;          // 取不到源大小：不阻断
    const ULONGLONG ullNeed = ullEsd.QuadPart * 4 + (1024ULL * 1024ULL * 50);  // 50MB 余量
    if (sIso.GetLength() < 3 || sIso[1] != L':') return true;   // 非盘符路径：不阻断
    CDuiString sIsoDrv; sIsoDrv.Format(L"%c:", sIso[0]);
    ULARGE_INTEGER ullFree = { 0 };
    ULARGE_INTEGER ullTotal = { 0 };
    if (!::GetDiskFreeSpaceEx(sIsoDrv.GetData(), &ullFree, &ullTotal, nullptr)) return true;
    if (ullFree.QuadPart >= ullNeed) return true;    // 空间足够
    // 转 MB 显示
    const ULONGLONG mbFree = ullFree.QuadPart / (1024 * 1024);
    const ULONGLONG mbNeed = ullNeed        / (1024 * 1024);
    CDuiString sMsg;
    sMsg.Format(StringMgr.Get("misc.h5664"),
                StringMgr.Get("misc.hbc5d"),
                StringMgr.Get("misc.hd06a"),
                StringMgr.Get("misc.hd60a"),
                sIsoDrv.GetData(), mbNeed, mbFree);
    const int nGo = HostConfirm(m_ctx.HostHwnd(), sMsg.GetData(),
                                StringMgr.Get("misc.h7d3b"), MB_YESNO, MB_ICONWARNING);
    return nGo == IDYES;
}

void CPageTools::ToolEsd2Iso()
{
    CDuiString sEsd;
    if (!Win32DialogService::DlgOpenFile(sEsd,
            StringMgr.Get("toolbox.s72051c87"),
            StringMgr.Get("misc.sbacbe5c3"))) return;
    // [2026-10-08] 不再要求 ADK oscdimg：wimgapi 导出 + IsoWriter 全内置
    std::vector<TDialogWimIndex> vIdx;
    if (!m_ctx.HostQueryWim(sEsd, vIdx, nullptr) || vIdx.empty())
    {
        HostNotify(m_ctx.HostHwnd(),
            (std::wstring(StringMgr.Get("error.s83d8bcdc")) + std::wstring(StringMgr.Get("toolbox.s886c6be8"))).c_str(),
            StringMgr.Get("misc.s771ac007"), MB_OK | MB_ICONWARNING);
        return;
    }
    int nCount = 0;
    for (size_t i = 0; i < vIdx.size(); ++i)
        if (vIdx[i].nIndex > nCount) nCount = vIdx[i].nIndex;
    // boot 映像存在性由 EsdCheckBootImage 一并回写：sBootWim 为实际路径，
    // bHasBootWim 为"确实找到 boot.wim/boot.esd"，二者不可互相推断。
    CDuiString sBootWim;
    bool bHasBootWim = false;
    if (!EsdCheckBootImage(sEsd, sBootWim, bHasBootWim)) return;
    CDuiString sIso;
    if (!Win32DialogService::DlgSaveFile(sIso,
            StringMgr.Get("toolbox.s2edcf7c8"),
            StringMgr.Get("edit.s310946f7"), L"install.iso")) return;
    if (!EsdCheckTargetSpace(sEsd, sIso)) return;
    CDuiString sEtfs = ExternalToolService::FindEtfsboot();
    CDuiString sEfisys = ExternalToolService::FindEfisys();
    // [R184] 进度窗 + 完成回调 + 启动后台作业抽为 EsdLaunchConvertJob（原内联 30 行）
    EsdLaunchConvertJob(sEsd, sIso, sBootWim, bHasBootWim, sEtfs, sEfisys);
}

// [R184] 从 ToolEsd2Iso（61 行 / CC12）抽出：建进度窗与完成回调、填充作业上下文、
// 启动后台作业。三条失败路径逐字保留：
//   · Start 失败 → 先关进度窗（否则留一个永不消失的窗）再弹 ERROR；
//   · 成功 → pJob.release()，所有权移交 E2iJobThreadProc；
//   · 失败 → unique_ptr 自动释放（不 release）。
void CPageTools::EsdLaunchConvertJob(const CDuiString& sEsd, const CDuiString& sIso,
                                     const CDuiString& sBootWim, bool bHasBootWim,
                                     const CDuiString& sEtfs, const CDuiString& sEfisys)
{
    // ---- [2026-10-08] 后台内置执行（不弹 bat 黑窗、不阻塞 UI）----
    // [rb28] 纯文字等待窗升级为带百分比的进度窗（导出 0-55% / ISO 55-100%）
    HWND hWait = CJobProgressWnd::Create(m_ctx.HostHwnd(),
        std::wstring(StringMgr.Get("toolbox.ui_esd2iso_busy")));
    CJobDoneSink::Cfg cfg;
    cfg.sOkTitle  = StringMgr.Get("toolbox.ui_esd2iso_ok_title");
    cfg.sOkMsg    = StringMgr.Get("toolbox.ui_esd2iso_ok_msg");
    cfg.sFailTitle = StringMgr.Get("toolbox.ui_esd2iso_fail_title");
    cfg.bAskOpen  = true;
    HWND hSink = CJobDoneSink::Create(m_ctx.HostHwnd(),
                                      std::wstring(sIso.GetData()), cfg);
    // [CRASHFIX] 裸 detach 迁移为受 SEH 边界保护、可被 WM_CLOSE 等待的统一作业
    // [C++17 阶段3] 线程 ctx：所有权移交 E2iJobThreadProc，Start 失败自删
    auto pJob = std::make_unique<TE2iJob>(); // [C++17 R3]
    pJob->wEsd     = std::wstring(sEsd);
    pJob->wIso     = std::wstring(sIso);
    pJob->wEtfs    = std::wstring(sEtfs);
    pJob->wEfisys  = std::wstring(sEfisys);
    pJob->wBootWim = std::wstring(sBootWim);
    pJob->bHasBoot = bHasBootWim;
    pJob->hWait    = hWait;
    pJob->hSink    = hSink;
    // [R104/R105] 取消令牌：页面与作业各持一份 shared_ptr。
    //   页面存进 m_vActiveCancels 列表，收到 JOBWM_CANCEL_REQ 时逐个置位；
    //   作业在 ISO 逐块写入处与 WIM 索引边界查 IsRequested()。
    pJob->pCancel  = BackgroundJobs::MakeCancelToken();
    RegisterCancelToken(pJob->pCancel);
    if (!BackgroundJobs::Start(&E2iJobThreadProc, pJob.get(), "esd-to-iso"))
    {
        if (hWait && ::IsWindow(hWait)) ::PostMessageW(hWait, WM_CLOSE, 0, 0);
        // [R160] 原为 clear()：会把同页其它在途作业的取消令牌一起清空，
        //   那些作业仍在写盘，用户再点"取消"将无效。改为只退自己这一份。
        UnregisterLastCancelToken();
        HostNotify(m_ctx.HostHwnd(), StringMgr.Get("toolbox.s_job_start_fail"),
                       StringMgr.Get("toolbox.ui_esd2iso_title"), MB_OK | MB_ICONERROR);
        return;   // [C++17 R3] 启动失败：unique_ptr 自动释放
    }
    pJob.release();   // [C++17 R3] ownership moved to E2iJobThreadProc
}

// 6 — WIM <-> ESD conversion (all indices)

// P1-2（R22）：4 档压缩策略选择 + 顺序索引 + 失败回滚 + 完成后询问打开目录

// [2026-10-08] WIM/ESD 互转：WIMGAPI Export 压缩切换
// （知识库 40 §2.1 行7、§2.3：内置 DLL，非外部 dism 命令）

// 转换核心：[rb28] 已切换到 WIMBridge::ExportAllImagesEx——
// 目标只创建一次承接全部索引、wimgapi 原生进度经 pProg 上报、
// 单索引 0-100 由 Bridge 内部按索引数线性折算为总进度（进度条不回跳）。
// 任一索引失败 Bridge 自行删除半成品。dwErr 仅为兼容旧错误弹窗保留。

namespace ToolJobs {
bool WecRunConvert(const std::wstring& wSrc,
                          const std::wstring& wDst,
                          DWORD dwCompress, DWORD& dwErr,
                          WIMBridge::IWimProgress* pProg,
                          const WIMBridge::CancelQueryFn& fnCancel)
{
    std::wstring sErr;
    bool bOk = false;
    try
    {
        // [R105] 取消查询透传给 Bridge：在索引边界收尾，半成品由 Bridge 自行删除。
        bOk = WIMBridge::ExportAllImagesEx(wSrc, wDst, dwCompress,
                                           &sErr, pProg, fnCancel);
    }
    catch (...)
    {
        bOk = false;
        if (sErr.empty()) sErr = StringMgr.Get("toolbox.ui_ex_convert");
    }
    if (!bOk)
    {
        dwErr = ::GetLastError();
        if (dwErr == 0) dwErr = ERROR_GEN_FAILURE;
    }
    return bOk;
}
} // namespace ToolJobs

// [rb28] 旧的纯 STATIC "转换中"提示窗已移除，统一用 CJobProgressWnd。

// [R16-T9] CWecDoneSink 已删除：WIM/ESD 互转完成改用统一 CJobDoneSink
// （BackgroundJobs.h），文案经 Cfg 表达，消息号收敛为 JOB_WM_DONE。


// 7 — 编辑映像文件（WIM 索引管理）

void CPageTools::ToolImageX()

{
    RunDialog(EToolDlgKind::TD_IMAGEX);
}

// [R-B35] 独立 ESD 解密向导（EToolDlgKind::TD_ESDWIZ）：选 esd→显示索引→选密钥→导出 WIM
void CPageTools::ToolEsdDecrypt()

{
    RunDialog(EToolDlgKind::TD_ESDWIZ);
}

// [R-B35] 外壳右键 /ESDDECRYPT：携带源 ESD 路径打开解密向导
void CPageTools::ToolEsdDecryptWithFile(const CDuiString& sFile)

{
    m_sEsdInitialFile = sFile;
    RunDialog(EToolDlgKind::TD_ESDWIZ);
}

// 9 — ISO生成器（自定义模态弹窗，oscdimg 真实执行）

void CPageTools::ToolIsoGen()

{
    RunDialog(EToolDlgKind::TD_ISO);
}

// ============================================================================
//  [2026-10-08] hosts 编辑器内置化（知识库 40 §2.1 行11）
//  内存模板模态对话框（多行 Edit），读 UTF-8/BOM，写回 hosts 并刷新 DNS。
// ============================================================================

// 12 — Windows 11 镜像处理（选镜像文件 → 自动挂载 → Rust 处理 → 提交 → 卸载）
//
// [R47] 交互重构：原实现要求用户先在别处挂载映像，再选"挂载目录"。
//   问题有三：
//     1) 用户必须懂"挂载"这一前置概念，且要自己找挂载点；
//     2) 挂载失败/占用/遗留挂载点全部由用户自行收拾；
//     3) 处理完不卸载，映像被长期锁住，后续操作连锁失败。
//   现改为：直接选 .wim/.esd 镜像 → 列索引 → 选索引 → 程序自己挂载 →
//   Rust 处理 → 提交卸载。用户全程只需两次选择，不必理解挂载。
//
// 职责边界（遵循"核心逻辑在 Rust"的既定约束）：
//   - C++ 只负责：选文件、选索引、调挂载/卸载 API、把结果转成提示；
//   - 版本门禁、预处理包生成、校验、原子写入全部在 Rust（dismcore）；
//   - 本文件不写任何 unattend.xml / 脚本内容。
// [R160] 宽字符 → UTF-8（不含结尾 NUL）。两处调用点原先各写一份等价循环，
//   合并后转换契约只维护一处：nLen 含结尾 NUL，故缓冲取 nLen-1、转换传 nLen。
static std::string WpToUtf8(const std::wstring& s)
{
    const int nLen = ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (nLen <= 1) return std::string();
    std::string out(static_cast<size_t>(nLen - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, &out[0], nLen, nullptr, nullptr);
    return out;
}

// [R160] UTF-8 → 宽字符（不含结尾 NUL）。Rust 侧返回的错误串走这条路径回到 UI。
static std::wstring WpToWide(const std::string& s)
{
    const int nNeed = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (nNeed <= 1) return std::wstring();
    std::wstring out(static_cast<size_t>(nNeed - 1), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &out[0], nNeed);
    return out;
}

void CPageTools::ToolWin11ImagePreprocess()
{
    // [R94] 段 1~3 抽到 Page_ToolsWin11Prep.cpp 的 WpPickImage/WpPickIndex，
    //   本函数只保留编排。三者都是「取输入 → 返回结果」，无状态共享。
    const wchar_t* kTitle = StringMgr.Get("toolbox.win11prep_title");

    std::wstring wim;
    if (!WpPickImage(wim)) return;

    DWORD dwIndex = 0;
    if (!WpPickIndex(wim, dwIndex)) return;

    // ---- 4. 选临时挂载点（必须与镜像同卷，否则 wimgapi 挂载失败） ----
    // [R94] 抽到 WpMakeMountDir：计算路径 + 建目录，失败弹框后返回 false。
    std::wstring mountDir;
    if (!WpMakeMountDir(wim, mountDir)) return;

    // ---- 5. 确认（明确告知会修改镜像本体，且不可逆） ----
    CDuiString sConfirm;
    sConfirm.Format(StringMgr.Get("toolbox.win11prep_confirm_image"), wim.c_str(), dwIndex);
    if (HostConfirm(m_ctx.HostHwnd(), sConfirm.GetData(), kTitle,
                    MB_YESNO | MB_ICONWARNING) != IDYES)
        return;

    // ---- 6~9. 挂载→Rust→提交/丢弃 整体后台化 ----
    // [FIX-T2-3] 可写挂载 + InstallWin11Preprocess + 提交（分钟级）原在 UI
    // 线程同步执行，大镜像时整窗未响应数分钟。移 BackgroundJobs 后台线程，
    // 收口经 WM_PAGE_ALERT 弹窗（三态：Rust 失败/卸载失败/成功）。
    struct W11Ctx
    {
        CPageTools*   pSelf;    // [FIX-T2-3] WpReadImageBuild 为成员函数，经 pSelf 调用
        IPageContext* pCtx;
        std::wstring  wim;
        std::wstring  mountDir;
        DWORD         dwIndex;
        std::wstring  sTitle;
    };
    W11Ctx* pW11 = new W11Ctx{ this, &m_ctx, wim, mountDir, dwIndex,
                               std::wstring(kTitle) };
    HANDLE hW11 = BackgroundJobs::Start([](void* pv) -> DWORD {
        std::unique_ptr<W11Ctx> guard(static_cast<W11Ctx*>(pv));
        CPageTools* pSelf = guard->pSelf;
        IPageContext* pCtx = guard->pCtx;
        const wchar_t* kT = guard->sTitle.c_str();
        // [FIX-T2-3] 弹窗统一内联经 WM_PAGE_ALERT 投递（无捕获 lambda 限制，
        // 不提取辅助 lambda）
        pCtx->HostLog(L"[tools] Win11 image: mounting for preprocessing",
                      static_cast<int>(ELogLevel::LOG_INFO));
        if (!WIMBridge::Mount(guard->wim, guard->dwIndex, guard->mountDir))
        {
            CDuiString sAlert;
            sAlert.Format(L"%s|%s|3", kT,
                          StringMgr.Get("toolbox.win11prep_mount_fail"));
            BackgroundJobs::PostHeapOrDelete(pCtx->HostHwnd(), WM_PAGE_ALERT, 0,
                std::make_unique<CDuiString>(sAlert.GetData()).release());
            return 0;
        }
        const DWORD dwBuild = pSelf->WpReadImageBuild(guard->mountDir);
        const std::string rootUtf8 = WpToUtf8(guard->mountDir);
        std::string sMsgFromRust;
        const int nRc = dismcore::InstallWin11Preprocess(rootUtf8, "ImageAdmin", dwBuild, sMsgFromRust);
        const bool bUnmountOk = WIMBridge::Unmount(guard->wim, guard->dwIndex,
                                                   guard->mountDir,
                                                   (nRc == 0) ? TRUE : FALSE);
        ::RemoveDirectoryW(guard->mountDir.c_str());   // 尽力清理；失败无碍（空目录）
        if (nRc != 0)
        {
            const std::wstring sErrW = WpToWide(sMsgFromRust);
            CDuiString sMsg;
            sMsg.Format(StringMgr.Get("toolbox.win11prep_fail"),
                        sErrW.empty() ? L"Rust install failed" : sErrW.c_str());
            CDuiString sAlert;
            sAlert.Format(L"%s|%s|3", kT, sMsg.GetData());
            BackgroundJobs::PostHeapOrDelete(pCtx->HostHwnd(), WM_PAGE_ALERT, 0,
                std::make_unique<CDuiString>(sAlert.GetData()).release());
            return 0;
        }
        if (!bUnmountOk)
        {
            // 处理成功但卸载失败：必须单独告知，不能报"全部成功"——
            // 映像仍处于挂载态时，用户后续任何操作都会失败。
            CDuiString sAlert;
            sAlert.Format(L"%s|%s|2", kT,
                          StringMgr.Get("toolbox.win11prep_unmount_fail"));
            BackgroundJobs::PostHeapOrDelete(pCtx->HostHwnd(), WM_PAGE_ALERT, 0,
                std::make_unique<CDuiString>(sAlert.GetData()).release());
            return 0;
        }
        pCtx->HostLog(L"[tools] Win11 image preprocessing committed into image",
                      static_cast<int>(ELogLevel::LOG_INFO));
        CDuiString sAlert;
        sAlert.Format(L"%s|%s|0", kT, StringMgr.Get("toolbox.win11prep_ok"));
        BackgroundJobs::PostHeapOrDelete(pCtx->HostHwnd(), WM_PAGE_ALERT, 0,
            std::make_unique<CDuiString>(sAlert.GetData()).release());
        return 0;
    }, pW11, "win11-prep");
    if (hW11)
        m_ctx.HostLog(L"[tools] Win11 image: preprocessing started in background");
    else
    {
        delete pW11;
        HostNotify(m_ctx.HostHwnd(),
                   L"后台处理线程启动失败（线程资源不足），Win11 镜像处理已中止。",
                   kTitle, MB_OK | MB_ICONERROR);
    }
}

