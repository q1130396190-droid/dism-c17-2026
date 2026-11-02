#pragma once

// ============================================================
// Page_ToolsE2I.h —— ESD→ISO 作业层（跨编译单元共享）。
//
// [R131-1] 从 Page_Tools.cpp 抽出 204 行 E2I 基础设施：
//   E2IMkDirChain / E2ICopy / E2IDelTree / E2IBuild
//   TE2iJob / E2iJobThreadProc
//
// 闭包证明：m_ctx 0 处、CPageTools:: 0 处、HostNotify/HostConfirm 0 处，
// 依赖仅为 Wang32 / ToolJobs 作业层 / WIMBridge / IsoWriter / SystemInfoService。
//
// WecRunConvert 定义留在 Page_Tools.cpp（WIM/ESD 互转段），本头只经
// Page_ToolsJobs.h 使用其前置声明，不重复定义。
// CPageTools::ToolEsd2Iso 依赖 m_ctx，仍留在主文件，经本头调用上述辅助。
// ============================================================

#include "stdafx.h"

#include <memory>
#include <string>

#include "WmMessages.h"
#include "MsgParam.h"
#include "WIMHelperBridge.h"
#include "IsoWriter.h"
#include "StringManager.h"
#include "BackgroundJobs.h"
#include "SystemInfoService.h"   // MakeTempWorkDir

#include "Page_ToolsJobs.h"      // CJobProgressWnd / JobProgPost / TJobWimProgress

// WecRunConvert 定义在 Page_Tools.cpp 的全局 ToolJobs 命名空间（WIM/ESD 互转段）。
namespace ToolJobs { bool WecRunConvert(const std::wstring& wSrc,
                                              const std::wstring& wDst,
                                              DWORD dwCompress, DWORD& dwErr,
                                              WIMBridge::IWimProgress* pProg,
    const WIMBridge::CancelQueryFn& fnCancel = nullptr); }

namespace ToolE2I
{

// E2I 临时工作目录名与 ISO 阶段进度分界（阶段1 导出占 0-55%，阶段2 组装占 55-100%）
inline constexpr const wchar_t* kE2iWorkDirTag = L"esd2iso";
inline constexpr DWORD kE2iExportPhaseEnd = 55;

// E2I 构建辅助复用 ToolJobs 作业层
using ToolJobs::JobProgPost;
using ToolJobs::TJobWimProgress;

// 递归建目录（忽略已存在）
static void E2IMkDirChain(const std::wstring& sPath)
{
    size_t i = sPath.find(L'\\', 3);
    while (i != std::wstring::npos)
    {
        ::CreateDirectoryW(sPath.substr(0, i).c_str(), nullptr);
        i = sPath.find(L'\\', i + 1);
    }
    ::CreateDirectoryW(sPath.c_str(), nullptr);
}

// 复制单文件（源缺失返回 false 但不报错；自动建目标目录链）
[[nodiscard]] static bool E2ICopy(const std::wstring& sSrc, const std::wstring& sDst)
{
    if (::GetFileAttributesW(sSrc.c_str()) == INVALID_FILE_ATTRIBUTES)
        return false;
    size_t slash = sDst.find_last_of(L'\\');
    if (slash != std::wstring::npos)
        E2IMkDirChain(sDst.substr(0, slash));
    return ::CopyFileW(sSrc.c_str(), sDst.c_str(), FALSE) != FALSE;
}

// 递归删除临时目录
static void E2IDelTree(const std::wstring& sPath)
{
    WIN32_FIND_DATAW fd;
    PTFindGuard h(::FindFirstFileW((sPath + L"\\*").c_str(), &fd));
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (lstrcmpW(fd.cFileName, L".") == 0 ||
                lstrcmpW(fd.cFileName, L"..") == 0) continue;
            std::wstring sp = sPath + L"\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                E2IDelTree(sp);
            else
            {
                ::SetFileAttributesW(sp.c_str(), FILE_ATTRIBUTE_NORMAL);
                ::DeleteFileW(sp.c_str());
            }
        } while (::FindNextFileW(h, &fd));
    }
    ::SetFileAttributesW(sPath.c_str(), FILE_ATTRIBUTE_NORMAL);
    ::RemoveDirectoryW(sPath.c_str());
}

// ESD转ISO 后台组装核心
[[nodiscard]] static bool E2IBuild(const std::wstring& wEsd,
                     const std::wstring& wIso,
                     const std::wstring& wEtfs,
                     const std::wstring& wEfisys,
                     const std::wstring& wBootWim,
                     bool bHasBootWim,
                     std::wstring& sErr,
                     HWND hProg = nullptr,
                     BackgroundJobs::CancelTokenPtr pCancel = nullptr)
{
    wchar_t szWin[MAX_PATH] = { 0 };
    ::GetWindowsDirectoryW(szWin, MAX_PATH);
    std::wstring wSys = szWin;

    std::wstring sWork = SystemInfoService::MakeTempWorkDir(L"esd2iso");
    if (sWork.empty()) { sErr = StringMgr.Get("toolbox.ui_temp_dir_fail"); return false; }

    // [rb28] 两阶段进度：阶段1 导出 install.wim 占 0-55%，
    //        阶段2 ISO 组装（按实际写入字节）占 55-100%。
    JobProgPost(hProg, 0,
        std::make_unique<std::wstring>(StringMgr.Get("toolbox.ui_stage1_wim"))); // [C++17 R3]

    bool bOk = false;
    do
    {
        std::wstring T = sWork + L"\\iso";
        E2IMkDirChain(T + L"\\sources");
        E2IMkDirChain(T + L"\\boot\\en-US");
        E2IMkDirChain(T + L"\\efi\\boot");
        E2IMkDirChain(T + L"\\efi\\microsoft\\boot\\en-US");

        // 1) install.wim：wimgapi 内置导出（/Compress:max = LZX）
        // [R105] 这里也接取消 —— R104 只覆盖了阶段 2（ISO 写入，55%-100%），
        //   用户在阶段 1 点取消要等整个导出结束才生效，那是本次要补的缺口。
        //   注意：wimgapi 单次导出不可打断，取消在**索引边界**生效，
        //   单个大索引开始后仍需等它走完（wimgapi 能力边界，非本实现取舍）。
        DWORD dwErr = 0;
        TJobWimProgress spExport(hProg, 0, 55);
        WIMBridge::CancelQueryFn fnCancelExport = nullptr;
        if (pCancel)
        {
            BackgroundJobs::CancelTokenPtr pTok = pCancel;
            fnCancelExport = [pTok]() { return pTok->IsRequested(); };
        }
        if (!WecRunConvert(wEsd, T + L"\\sources\\install.wim",
                           WIM_COMPRESS_LZX, dwErr, &spExport, fnCancelExport))
        {
            // 取消与真失败要分开报：用户主动取消时不应弹"导出失败"错误码文案
            if (pCancel && pCancel->IsRequested())
                sErr = StringMgr.Get("toolbox.ui_cancel_wim_export");
            else
            {
                const std::wstring sErrNo = std::to_wstring(dwErr);
                sErr = FormatW(StringMgr.Get("toolbox.ui_wim_export_fail_fmt"),
                               sErrNo.c_str());
            }
            break;
        }

        // 2) BIOS 引导文件
        static_cast<void>(E2ICopy(wSys + L"\\Boot\\PCAT\\bootmgr", T + L"\\bootmgr"));
        static_cast<void>(E2ICopy(wSys + L"\\Boot\\DVD\\PCAT\\BCD", T + L"\\boot\\bcd"));
        static_cast<void>(E2ICopy(wSys + L"\\Boot\\DVD\\PCAT\\boot.sdi", T + L"\\boot\\boot.sdi"));
        static_cast<void>(E2ICopy(wSys + L"\\Boot\\DVD\\PCAT\\en-US\\bootfix.bin",
                T + L"\\boot\\en-US\\bootfix.bin"));

        // 3) UEFI 引导文件
        static_cast<void>(E2ICopy(wSys + L"\\Boot\\EFI\\bootmgr.efi",
                T + L"\\efi\\boot\\bootx64.efi"));
        static_cast<void>(E2ICopy(wSys + L"\\Boot\\EFI\\bootmgr.efi",
                T + L"\\efi\\microsoft\\boot\\bootmgfw.efi"));
        static_cast<void>(E2ICopy(wSys + L"\\Boot\\DVD\\EFI\\BCD",
                T + L"\\efi\\microsoft\\boot\\bcd"));
        static_cast<void>(E2ICopy(wSys + L"\\Boot\\DVD\\EFI\\boot.sdi",
                T + L"\\efi\\microsoft\\boot\\boot.sdi"));

        // 4) boot.wim（用户同目录提供时）
        if (bHasBootWim)
            static_cast<void>(E2ICopy(wBootWim, T + L"\\sources\\boot.wim"));

        // 5) 内置 ISO 组装（BIOS/UEFI 双启动，等价 oscdimg -bootdata:2）
        JobProgPost(hProg, 55,
            std::make_unique<std::wstring>(StringMgr.Get("toolbox.ui_stage2_iso"))); // [C++17 R3]
        IsoWriter::Options io;
        io.sVolumeLabel = L"ESD_ISO";
        if (!wEtfs.empty() &&
            ::GetFileAttributesW(wEtfs.c_str()) != INVALID_FILE_ATTRIBUTES)
            io.sBiosBootFile = wEtfs;
        if (!wEfisys.empty() &&
            ::GetFileAttributesW(wEfisys.c_str()) != INVALID_FILE_ATTRIBUTES)
            io.sUefiBootFile = wEfisys;

        // [rb28] ISO 引擎按实际写入字节自算进度，线性映射到总区间 55-100%
        DWORD dwIsoLast = 54;
        IsoWriter::ProgressFn fnIso = [hProg, &dwIsoLast](
            ULONGLONG ullDone, ULONGLONG ullTotal)
        {
            if (ullTotal == 0) return;
            DWORD v = 55 + static_cast<DWORD>((ullDone * 45ULL) / ullTotal);
            if (v > 100) v = 100;
            if (v == dwIsoLast) return;
            dwIsoLast = v;
            if (hProg && ::IsWindow(hProg))
                ::PostMessageW(hProg, JOBWM_PROGRESS, static_cast<WPARAM>(v), static_cast<LPARAM>(0));
        };

        // [R104] 取消查询：ISO 写入逐块检查；用户点"取消"后这里返回 true，
        //   CreateImage 立刻收尾返回 false，随后下方 `if (!bOk) DeleteFileW(wIso)`
        //   会删掉半截 ISO —— 不留无法打开的残留文件。
        IsoWriter::CancelQueryFn fnCancel = nullptr;
        if (pCancel)
        {
            BackgroundJobs::CancelTokenPtr pTok = pCancel;
            fnCancel = [pTok]() { return pTok->IsRequested(); };
        }

        std::wstring werr;
        if (!IsoWriter::CreateImage(T, wIso, io, &werr, fnIso, fnCancel))
        {
            sErr = werr;
            break;
        }

        JobProgPost(hProg, 100, nullptr);   // 仅更新百分比，无文案
        bOk = true;
    } while (false);

    E2IDelTree(sWork);
    if (!bOk) ::DeleteFileW(wIso.c_str());
    return bOk;
}

// 5 — decrypt ESD and build a dual-boot (BIOS+UEFI) ISO

// P0-4（R21）：boot.wim 缺失提示 + 目标盘空间预估

// [CRASHFIX] ESD→ISO 作业上下文（原 lambda 捕获量提升为堆上下文）
struct TE2iJob
{
    std::wstring wEsd, wIso, wEtfs, wEfisys, wBootWim;
    bool         bHasBoot;
    HWND         hWait;
    HWND         hSink;
    // [R104] 取消令牌：UI 线程点"取消"后置位，作业在 ISO 写入的逐块循环里查到。
    //   用 shared_ptr 而非裸指针：UI 侧窗口销毁、作业侧提前结束都不会留下悬空。
    BackgroundJobs::CancelTokenPtr pCancel;
};

// [CRASHFIX] ESD→ISO 线程：替代裸 std::thread::detach，
// SafeCreateThread 双层异常边界 + WM_CLOSE 可等待（G-CRS-01/02）
static DWORD WINAPI E2iJobThreadProc(void* pv)
{
    std::unique_ptr<TE2iJob> spJob(static_cast<TE2iJob*>(pv));  // [C++17 R3] 线程 ctx：函数退出自动释放
    TE2iJob* j = spJob.get();
    HWND hWait = j->hWait;
    HWND hSink = j->hSink;

    std::wstring sTaskErr;
    bool bTaskOk = false;
    try
    {
        bTaskOk = E2IBuild(j->wEsd, j->wIso, j->wEtfs, j->wEfisys,
                           j->wBootWim, j->bHasBoot, sTaskErr, hWait, j->pCancel);
    }
    catch (...)
    {
        bTaskOk = false;
        if (sTaskErr.empty()) sTaskErr = StringMgr.Get("toolbox.ui_ex_esd2iso");
    }

    // [R16-T9] 统一信封投递（nKind=0：ESD→ISO）
    // [C++17 阶段3] Post 消息所有权移交 UI，Post 失败自删（PostHeapOrDelete）
    auto pEnv = std::make_unique<TJobDoneEnvelope>(); // [C++17 R3]
    pEnv->nKind = 0;
    pEnv->bOk   = bTaskOk;
    if (!bTaskOk) pEnv->sMsg = sTaskErr;

    if (hWait && ::IsWindow(hWait)) ::PostMessageW(hWait, WM_CLOSE, 0, 0);

    // [GC-07] 统一收尾：投递失败由助手就地回收，不再各写一套
    BackgroundJobs::PostHeapOrDelete(hSink, JOB_WM_DONE, 0, pEnv.release()); // [C++17 R3] ownership to helper

    // [C++17 R3] spJob unique_ptr 自动释放，所有权由启动方移交
    return 0;
}

} // namespace ToolE2I
