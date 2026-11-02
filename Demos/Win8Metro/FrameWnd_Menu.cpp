// ============================================================================
//  FrameWnd_Menu.cpp —— [R14-01] 菜单表驱动拆解
//
//  行为等价基线：FrameWnd.cpp ShowPopupMenu（2026-10-13）。
//  原 754 行按职责拆成 1 个薄壳 + 4 个 Build + 1 个 Track + 1 个 Dispatch
//  + 表驱动 case 处理器。[R16-T7] DispatchMenuCmd 由 44-case switch 改为
//  kMenuCmdTab{iCmd, PfnHandler} 表分发，命令 ID 裸魔数收编为 enum EMenuCmd。
//  kLangMenu / LangCodeToIndex / IsLangPackAvailable 一并从 FrameWnd.cpp 迁入，
//  避免跨文件 static 依赖。[R16-T8] 菜单层业务下沉：CBS 深链路 →
//  OfflineCbsSessionService；CompactOS 撤销线程 → WofUncompactJob
//  （BackgroundJobs 体系）；裸 MessageBox 统一 HostAlert/HostConfirm。
// ============================================================================

#include "stdafx.h"

#include "FrameWnd.h"
#include "FrameWnd_Menu.h"
#include "FrameWnd_Messages.h"  // kRestartEventName（RestartApplication 迁移用）
#include "StringManager.h"
#include "ConfigReader.h"        // i18n::CConfigReader::SetLanguageCurrent（语言切换用）
#include "Win32DialogService.h"
#include "ProcessRunner.h"
#include "OfflineSession.h"
#include "OfflineCbsSessionService.h"  // [R16-T8] 离线 CBS 深链路会话服务（业务下沉）
#include "WofUncompactJob.h"           // [R16-T8] CompactOS 撤销后台作业（BackgroundJobs 体系）
#include "SafeThread.h"                // [FIX-P1I] SafeCreateThread（bootre 后台化）
#include "WmMessages.h"                // [FIX-P1I] WM_DIAG_LOG（后台线程日志回投）

#include <windowsx.h>
// [C++17 阶段2] 局部 RAII 守卫：事件句柄（见 WinHandles.h 使用规范）
struct FMEventGuard
{
    HANDLE h = nullptr;
    FMEventGuard() = default;
    FMEventGuard(HANDLE x) : h(x) {}   // [C++17 R-P8] 补 HANDLE 构造
    ~FMEventGuard() { if (h) ::CloseHandle(h); }
    FMEventGuard(const FMEventGuard&) = delete;
    FMEventGuard& operator=(const FMEventGuard&) = delete;
    operator HANDLE() const { return h; }
};

// ----------------------------------------------------------------------------
// [R16-T7] 菜单命令 ID 符号化（值与 2026-10-13 ShowPopupMenu 基线逐一对应）
//  裸魔数 1101-1406 全部收编为具名枚举项，Build* 与 kMenuCmdTab 共用。
// ----------------------------------------------------------------------------
enum class EMenuCmd : UINT   // [C++17 R20] scoped enum; call sites cast explicitly
{
    // ---- 文件(F) ----
    MC_FILE_SESSION_CLEANUP = 1101,  // cleanup.s5b4051ae
    MC_FILE_MOUNT_WIZARD    = 1102,  // toolbox.s51836ac2（12 步挂载向导）
    MC_FILE_IMAGEX_1103     = 1103,  // toolbox.s37b5ac3b（ImageX 弹窗）
    MC_FILE_IMAGEX_1104     = 1104,  // toolbox.s37b24705（ImageX 弹窗）
    MC_FILE_IMAGEX_1105     = 1105,  // toolbox.s9b361e34
    MC_FILE_IMAGEX_1106     = 1106,  // toolbox.s3cda4305（ImageX 弹窗）
    MC_FILE_ADD_SOURCE      = 1107,  // edit.s6f70c845（添加路径→离线源目录）
    MC_ESD_WIM              = 1110,  // ESD<-->WIM
    MC_SWM_WIM              = 1112,  // SWM --> WIM
    MC_ESD_ISO              = 1120,  // ESD -->ISO
    MC_ESD_DECRYPT          = 1130,  // misc.se65d68c5（[R-B35] ESD 解密）
    MC_FILE_RELOAD          = 1150,  // misc.s421b5367（重新载入）
    MC_FILE_ABORT           = 1151,  // general.sb5083964（取消任务）
    MC_GOTO_OPTIM           = 1152,  // optim.sb2b90c67（系统优化）
    MC_GOTO_DRIVER          = 1153,  // driver.sa7f61b5e（驱动管理）
    // ---- 恢复功能(R) ----
    MC_RECOVER_SFC            = 1201,  // 校验系统文件（sfc 直连）
    MC_RECOVER_RESTOREHEALTH  = 1202,  // 在线 RestoreHealth + sfc
    MC_RECOVER_BOOT           = 1203,  // 引导修复（工具箱）
    MC_RECOVER_BACKUP         = 1204,  // 系统备份（工具箱）
    MC_RECOVER_RESTORE        = 1205,  // 系统还原（工具箱）
    MC_RECOVER_RESET_HINT     = 1206,  // 重置提示（信息弹窗）
    MC_RECOVER_BOOT_RECOVERY  = 1207,  // 重启进入恢复环境
    MC_RECOVER_UNCOMPACT      = 1208,  // 取消 CompactOS 压缩
    // ---- 选项(O) ----
    MC_OPT_DETAILS    = 1301,  // 详细设置（EToolDlgKind::TD_DETAILS）
    MC_AFTER_NONE     = 1310,  // 操作完成后：无
    MC_AFTER_REBOOT   = 1311,  // 操作完成后：重启
    MC_AFTER_SHUTDOWN = 1312,  // 操作完成后：关机
    MC_LANG_BASE      = 1320,  // 1320..1330 = kLangMenu 下标 + 1320
    // ---- 帮助(H) ----
    MC_HELP_PRODUCT  = 1401,  // 软件主页
    MC_HELP_WEBSITE  = 1402,  // 官方网站
    MC_HELP_INTRO    = 1403,  // 功能简介（信息弹窗）
    MC_HELP_FEEDBACK = 1404,  // 问题反馈
    MC_HELP_ABOUT    = 1405,  // 关于
    MC_HELP_LOGVIEW  = 1406   // 查看日志（EToolDlgKind::TD_LOGVIEW）
};

// ----------------------------------------------------------------------------
// 语言菜单表（原 FrameWnd.cpp static，随 ShowPopupMenu 迁入）
// ----------------------------------------------------------------------------
namespace
{
    struct LangMenuItem { const wchar_t* pszCode; const wchar_t* pszEndonym; };

    const LangMenuItem kLangMenu[] = {
        { L"zh-CN",   L"\u7b80\u4f53\u4e2d\u6587" },  // 简体中文
        { L"en-US",   L"English"                     },
        { L"ja-JP",   L"\u65e5\u672c\u8a9e"            },  // 日本語
        { L"ko-KR",   L"\ud55c\uad6d\uc5b4"            },  // 한국어
        { L"cs",      L"\u010ce\u0161tina"             },  // Čeština
        { L"de",      L"Deutsch"                      },
        { L"fr",      L"Fran\u00e7ais"                 },
        { L"hu",      L"Magyar"                       },
        { L"it",      L"Italiano"                     },
        { L"pl-PL",   L"Polski"                       },
        { L"ru",      L"\u0420\u0443\u0441\u0441\u043a\u0438\u0439" },  // Русский
    };
    constexpr int kLangMenuCount = static_cast<int>(sizeof(kLangMenu) / sizeof(kLangMenu[0]));

    // zh-CN 内置回退表永远可用；其余语言需要 lang\<code>.dll 真实存在
    bool IsLangPackAvailable(const std::wstring& sCode)
    {
        if (sCode == L"zh-CN") return true;
        std::vector<CDuiString> vLangs = StringMgr.ListAvailableLangs();
        for (size_t i = 0; i < vLangs.size(); ++i)
        {
            if (sCode == std::wstring(vLangs[i].GetData())) return true;
        }
        return false;
    }

    // 语言代码 → kLangMenu 下标（未命中回退 0=zh-CN）
    int LangCodeToIndex(const std::wstring& sCode)
    {
        for (int i = 0; i < kLangMenuCount; ++i)
        {
            if (sCode == kLangMenu[i].pszCode) return i;
        }
        return 0;
    }
}

int FrameWnd_Menu_LangCodeToIndex(const std::wstring& sCode) { return LangCodeToIndex(sCode); }
bool FrameWnd_Menu_IsLangPackAvailable(const std::wstring& sCode) { return IsLangPackAvailable(sCode); }
int FrameWnd_Menu_LangCount() { return kLangMenuCount; }
const wchar_t* FrameWnd_Menu_LangCodeAt(int nIdx)
{ return (nIdx >= 0 && nIdx < kLangMenuCount) ? kLangMenu[nIdx].pszCode : kLangMenu[0].pszCode; }
const wchar_t* FrameWnd_Menu_LangEndonymAt(int nIdx)
{ return (nIdx >= 0 && nIdx < kLangMenuCount) ? kLangMenu[nIdx].pszEndonym : kLangMenu[0].pszEndonym; }

// ----------------------------------------------------------------------------
// void CFrameWnd::RestartApplication()
//  语言切换后重启进程：先唤起新实例（其单实例检测会等待本事件），
//  再发信号并退出，互斥量随进程释放，新实例接管启动。
//  [R16-T8] 自匿名 namespace 收编为成员；失败提示统一走 HostAlert。
// ----------------------------------------------------------------------------
void CFrameWnd::RestartApplication()
{
    wchar_t szExe[MAX_PATH] = { 0 };
    ::GetModuleFileName(nullptr, szExe, _countof(szExe));

    FMEventGuard hEvt(::CreateEventW(nullptr, TRUE, FALSE, kRestartEventName));

    HINSTANCE hShell = ::ShellExecuteW(nullptr, L"open", szExe,
                                       nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(hShell) <= 32)
    {
        // FMEventGuard 析构自动关闭
        HostAlert(L"Win8Metro",
            L"\u91cd\u542f\u5931\u8d25\uff0c\u8bf7\u624b\u52a8\u5173\u95ed\u7a0b\u5e8f\u540e\u91cd\u65b0\u6253\u5f00\u3002",
            MB_ICONERROR);
        return;
    }

    if (hEvt)
    {
        ::SetEvent(hEvt);
        // FMEventGuard 析构自动关闭
    }
    ::PostQuitMessage(0);
}

// ----------------------------------------------------------------------------
// void CFrameWnd::BuildMenuFile(HMENU hMenu)
//  文件(F)：原 iMenu==1 分支。命令 ID 1101-1153 逐一保持。
// ----------------------------------------------------------------------------
void CFrameWnd::BuildMenuFile(HMENU hMenu)
{
    // ---- 文件(F) — 真实 Dism++ 菜单 ----
    // 灰色项为尚未接入的离线映像操作（原弹窗占位已移除）
    ::AppendMenu(hMenu, MF_STRING, static_cast<UINT>(EMenuCmd::MC_FILE_SESSION_CLEANUP), StringMgr.Get("cleanup.s5b4051ae"));
    ::AppendMenu(hMenu, MF_STRING, static_cast<UINT>(EMenuCmd::MC_FILE_MOUNT_WIZARD), StringMgr.Get("toolbox.s51836ac2"));
    ::AppendMenu(hMenu, MF_STRING, static_cast<UINT>(EMenuCmd::MC_FILE_ADD_SOURCE), StringMgr.Get("edit.s6f70c845"));
    ::AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenu(hMenu, MF_STRING, static_cast<UINT>(EMenuCmd::MC_FILE_IMAGEX_1103), StringMgr.Get("toolbox.s37b5ac3b"));
    ::AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenu(hMenu, MF_STRING, static_cast<UINT>(EMenuCmd::MC_FILE_IMAGEX_1104), StringMgr.Get("toolbox.s37b24705"));
    ::AppendMenu(hMenu, MF_STRING, static_cast<UINT>(EMenuCmd::MC_FILE_IMAGEX_1105), StringMgr.Get("toolbox.s9b361e34"));
    ::AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenu(hMenu, MF_STRING, static_cast<UINT>(EMenuCmd::MC_FILE_IMAGEX_1106), StringMgr.Get("toolbox.s3cda4305"));
    ::AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);

    HMENU hSubConvert = ::CreatePopupMenu();
    ::AppendMenu(hSubConvert, MF_STRING, static_cast<UINT>(EMenuCmd::MC_ESD_WIM), L"ESD<-->WIM");
    ::AppendMenu(hSubConvert, MF_STRING, static_cast<UINT>(EMenuCmd::MC_SWM_WIM), L"SWM --> WIM");
    ::AppendMenu(hMenu, MF_POPUP | MF_STRING, reinterpret_cast<UINT_PTR>(hSubConvert),
                 L"WIM<-->ESD/SWM");

    HMENU hSubIso = ::CreatePopupMenu();
    ::AppendMenu(hSubIso, MF_STRING, static_cast<UINT>(EMenuCmd::MC_ESD_ISO), L"ESD -->ISO");
    ::AppendMenu(hMenu, MF_POPUP | MF_STRING, reinterpret_cast<UINT_PTR>(hSubIso),
                 L"ESD -->ISO");

    ::AppendMenu(hMenu, MF_STRING, static_cast<UINT>(EMenuCmd::MC_ESD_DECRYPT), StringMgr.Get("misc.se65d68c5"));
    ::AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenu(hMenu, MF_STRING, static_cast<UINT>(EMenuCmd::MC_FILE_RELOAD), StringMgr.Get("misc.s421b5367"));
    ::AppendMenu(hMenu, MF_STRING, static_cast<UINT>(EMenuCmd::MC_FILE_ABORT), StringMgr.Get("general.sb5083964"));
    ::AppendMenu(hMenu, MF_STRING, static_cast<UINT>(EMenuCmd::MC_GOTO_OPTIM), StringMgr.Get("optim.sb2b90c67"));
    ::AppendMenu(hMenu, MF_STRING, static_cast<UINT>(EMenuCmd::MC_GOTO_DRIVER), StringMgr.Get("driver.sa7f61b5e"));
}

// ----------------------------------------------------------------------------
// void CFrameWnd::BuildMenuRecover(HMENU hMenu)
//  恢复功能(R)：原 iMenu==2 分支。命令 ID 1201-1208 逐一保持。
// ----------------------------------------------------------------------------
void CFrameWnd::BuildMenuRecover(HMENU hMenu)
{
    ::AppendMenu(hMenu, MF_STRING, static_cast<UINT>(EMenuCmd::MC_RECOVER_SFC), StringMgr.Get("misc.sd4223c8b"));
    ::AppendMenu(hMenu, MF_STRING, static_cast<UINT>(EMenuCmd::MC_RECOVER_RESTOREHEALTH), StringMgr.Get("misc.s79c0b6c7"));
    ::AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenu(hMenu, MF_STRING, static_cast<UINT>(EMenuCmd::MC_RECOVER_BOOT), StringMgr.Get("misc.sd5c9770e"));
    ::AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenu(hMenu, MF_STRING, static_cast<UINT>(EMenuCmd::MC_RECOVER_BACKUP), StringMgr.Get("system.se20db2e9"));
    ::AppendMenu(hMenu, MF_STRING, static_cast<UINT>(EMenuCmd::MC_RECOVER_RESTORE), StringMgr.Get("system.s960c244e"));
    ::AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenu(hMenu, MF_STRING, static_cast<UINT>(EMenuCmd::MC_RECOVER_RESET_HINT), StringMgr.Get("misc.seb653a34"));
    ::AppendMenu(hMenu, MF_STRING, static_cast<UINT>(EMenuCmd::MC_RECOVER_BOOT_RECOVERY), StringMgr.Get("misc.s655bc783"));
    ::AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenu(hMenu, MF_STRING, static_cast<UINT>(EMenuCmd::MC_RECOVER_UNCOMPACT), StringMgr.Get("misc.sf954cd79"));
}

// ----------------------------------------------------------------------------
// void CFrameWnd::BuildMenuOption(HMENU hMenu, UINT uChk0, UINT uChk1, UINT uChk2)
//  选项(O)：原 iMenu==3 分支。1301 + 操作完成后子菜单 + 语言子菜单。
// ----------------------------------------------------------------------------
void CFrameWnd::BuildMenuOption(HMENU hMenu, UINT uChk0, UINT uChk1, UINT uChk2)
{
    ::AppendMenu(hMenu, MF_STRING, static_cast<UINT>(EMenuCmd::MC_OPT_DETAILS), StringMgr.Get("misc.s5f637bdc"));
    ::AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);

    // [R-B46] 选项→操作完成后 真实消费：MC_AFTER_* 写 m_nAfterAction，
    // 任务完成后在 RunBatch 尾部按 one-shot 注销/重启。子项用 uChk0/1/2
    // 单选标记（MF_CHECKED/MF_UNCHECKED），不置 MF_GRAYED。
    HMENU hSubAfter = ::CreatePopupMenu();
    ::AppendMenu(hSubAfter, MF_STRING | uChk0, static_cast<UINT>(EMenuCmd::MC_AFTER_NONE), StringMgr.Get("misc.scebc6bbb"));
    ::AppendMenu(hSubAfter, MF_SEPARATOR, 0, nullptr);
    ::AppendMenu(hSubAfter, MF_STRING | uChk1, static_cast<UINT>(EMenuCmd::MC_AFTER_REBOOT), StringMgr.Get("misc.sf2eebd82"));
    ::AppendMenu(hSubAfter, MF_STRING | uChk2, static_cast<UINT>(EMenuCmd::MC_AFTER_SHUTDOWN), StringMgr.Get("misc.s01b4e06f"));
    ::AppendMenu(hMenu, MF_POPUP | MF_STRING, reinterpret_cast<UINT_PTR>(hSubAfter),
                 StringMgr.Get("misc.scae87559"));

    // ---- 语言子菜单 ----
    // endonym 固定名（简体中文/English/日本語/한국어），不随当前语言翻译；
    // 未安装对应 lang\<code>.dll 的项置灰；当前语种带勾选标记。
    HMENU hSubLang = ::CreatePopupMenu();
    for (int li = 0; li < kLangMenuCount; ++li)
    {
        UINT uLangItem = MF_STRING;
        if (m_nLanguage == li) uLangItem |= MF_CHECKED;
        if (!IsLangPackAvailable(kLangMenu[li].pszCode))
            uLangItem |= MF_GRAYED;
        ::AppendMenu(hSubLang, uLangItem, static_cast<UINT>(EMenuCmd::MC_LANG_BASE) + li, kLangMenu[li].pszEndonym);
    }
    ::AppendMenu(hMenu, MF_POPUP | MF_STRING, reinterpret_cast<UINT_PTR>(hSubLang),
                 StringMgr.Get("misc.s295bb704"));
}

// ----------------------------------------------------------------------------
// void CFrameWnd::BuildMenuHelp(HMENU hMenu)
//  帮助(H)：原 iMenu==4 分支。命令 ID 1401-1406 逐一保持。
// ----------------------------------------------------------------------------
void CFrameWnd::BuildMenuHelp(HMENU hMenu)
{
    ::AppendMenu(hMenu, MF_STRING, static_cast<UINT>(EMenuCmd::MC_HELP_PRODUCT), StringMgr.Get("misc.s409041b5"));
    ::AppendMenu(hMenu, MF_STRING, static_cast<UINT>(EMenuCmd::MC_HELP_WEBSITE), StringMgr.Get("misc.s1a8dc6f6"));
    ::AppendMenu(hMenu, MF_STRING, static_cast<UINT>(EMenuCmd::MC_HELP_INTRO), StringMgr.Get("misc.s3ff4faa6"));
    ::AppendMenu(hMenu, MF_STRING, static_cast<UINT>(EMenuCmd::MC_HELP_FEEDBACK), StringMgr.Get("misc.sa338d22f"));
    ::AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenu(hMenu, MF_STRING, static_cast<UINT>(EMenuCmd::MC_HELP_ABOUT), StringMgr.Get("misc.sc4944145"));
    ::AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenu(hMenu, MF_STRING, static_cast<UINT>(EMenuCmd::MC_HELP_LOGVIEW), StringMgr.Get("misc.logview_title"));
}

// ----------------------------------------------------------------------------
// case 处理器 —— 原 ShowPopupMenu switch 内联逻辑抽取
// ----------------------------------------------------------------------------
void CFrameWnd::MenuCmdAddSourceDir(int /*iCmd*/)
{
    // 原 case 1107：添加路径 → 离线源目录
    // [R16-T8] CBS 深链路业务下沉 OfflineCbsSessionService，菜单层仅轻委托
    CDuiString sDir;
    if (Win32DialogService::DlgBrowseFolder(sDir, StringMgr.Get("system.s9d2ba839")))
    {
        CDuiString sLog;
        sLog.Format(StringMgr.Get("edit.s75eaee4f"), sDir.GetData());
        AppendLog(sLog.GetData());

        OfflineCbsSession::TPrepareResult r =
            OfflineCbsSession::PrepareSourceDir(sDir.GetData());
        HostAlert(r.sTitle.c_str(), r.sMessage.c_str(),
                  r.bWarning ? MB_ICONWARNING : MB_ICONINFORMATION);
    }
}
void CFrameWnd::MenuCmdSwmMerge(int /*iCmd*/)
{
    // 原 case 1112：SWM → WIM（part .swm；parts 2/3 自动发现）
    CDuiString sSwm, sWim;
    if (!Win32DialogService::DlgOpenFile(sSwm, StringMgr.Get("toolbox.s95c16013"),
                     StringMgr.Get("misc.s354b1771"))) return;
    if (!Win32DialogService::DlgSaveFile(sWim, StringMgr.Get("toolbox.s1363566d"),
                     StringMgr.Get("misc.s1ecd8fa7"), L"merged.wim")) return;

    // [原生化] wimgapi 直接导出合并，不再生成 swm2wim.bat（dism /Export-Image）
    if (CPageBase* pPage = PageById(EPageId::PAGE_TOOLS)) pPage->HostSwmMerge(sSwm, sWim);
}

void CFrameWnd::MenuCmdRestoreHealth(int /*iCmd*/)
{
    // 原 case 1202：在线 RestoreHealth + sfc /scannow（离线仅映像修复）
    // [R16-T8] 裸 MessageBox 统一走 HostConfirm（OKCANCEL|QUESTION|DEFBUTTON2 等价）
    if (!HostConfirm(StringMgr.Get("misc.s5f636746"),
        (CDuiString(StringMgr.Get("misc.sc1144042")) + StringMgr.Get("misc.s71245753")
        + StringMgr.Get("system.s8f2dde90") + StringMgr.Get("confirm.s9b9a63a3")).GetData()))
        return;

    StartSystemRepair(true /*RestoreHealth*/,
                      !COfflineSession::IsActive() /*sfc*/);
}

void CFrameWnd::MenuCmdBootToRecovery(int /*iCmd*/)
{
    // 原 case 1207：reagentc /boottore → InitiateSystemShutdown（无 cmd）
    // [R16-T8] 确认/报错统一走 HostConfirmWarn/HostAlert（flag 组合逐字等价）
    if (!HostConfirmWarn(StringMgr.Get("misc.s19c5204e"),
        (std::wstring(StringMgr.Get("system.sfc054ca6"))
         + std::wstring(StringMgr.Get("edit.s394d62c3"))).c_str()))
        return;

    // [FIX-P1I] 旧实现在 UI 线程同步 RunHiddenExit 等待最长 60s（泵保绘制
    // 但输入全丢）。改为后台线程执行：成功自动 10s 重启；失败经
    // WM_DIAG_LOG（kMsgOwnShell → AppendLog）留痕。
    struct BootReCtx
    {
        HWND       hMain;        // 主窗口句柄（日志回投目标，勿用 GetForegroundWindow——
                                 // 可能是弹出控件 HWND，消息将丢失并泄漏载荷）
        CDuiString sReagent;     // 解析好的 reagentc 全路径（空=回退 System32 直呼）
        CDuiString sRebootMsg;   // 重启提示文案
    };
    auto pCtx = std::make_unique<BootReCtx>();
    pCtx->hMain = m_hWnd;
    pCtx->sReagent = ProcessRunner::ResolveSysTool(L"reagentc.exe");
    pCtx->sRebootMsg = StringMgr.Get("general.s1eab04b3");
    HANDLE hTh = SafeCreateThread([](void* pv) -> DWORD {
        std::unique_ptr<BootReCtx> ctx(static_cast<BootReCtx*>(pv));
        DWORD dwExit = 0xFFFFFFFF;
        if (!ctx->sReagent.IsEmpty())
            dwExit = ProcessRunner::RunHiddenExit(ctx->sReagent, L"/boottore", 60000);
        auto pLog = std::make_unique<CDuiString>();
        if (dwExit != 0)
            pLog->Format(L"[bootre] reagentc /boottore failed (exit=%lu)", dwExit);
        else
            pLog->Format(L"[bootre] reagentc /boottore ok; reboot in 10s");
        // WM_DIAG_LOG 注册为 kMsgOwnShell（壳侧 AppendLog 并释放 CDuiString*）
        ::PostMessageW(ctx->hMain, WM_DIAG_LOG, 0,
                       reinterpret_cast<LPARAM>(pLog.get()));
        // 壳窗口可能已关闭：兜底投自身线程消息无意义，直接放弃日志（进程将退）
        if (dwExit == 0)
        {
            const std::wstring sMsg = ctx->sRebootMsg.GetData();
            ::InitiateSystemShutdownW(nullptr, const_cast<wchar_t*>(sMsg.c_str()),
                                      10, TRUE, TRUE);
        }
        return 0;
    }, pCtx.get());
    if (hTh)
    {
        pCtx.release();   // [C++17 R3] 所有权移交后台线程
        ::CloseHandle(hTh);
        AppendLog(L"[bootre] reagentc /boottore started in background");
    }
    else
    {
        // 线程创建失败：回退原同步路径（保底可用性优先）
        CDuiString sReagent = ProcessRunner::ResolveSysTool(L"reagentc.exe");
        DWORD dwExit = 0xFFFFFFFF;
        if (!sReagent.IsEmpty())
            dwExit = ProcessRunner::RunHiddenExit(sReagent, L"/boottore", 60000);
        if (dwExit != 0)
        {
            CDuiString sErr;
            sErr.Format(L"reagentc /boottore 失败（退出码 %lu）。", dwExit);
            AppendLog(sErr.GetData());
            HostAlert(StringMgr.Get("misc.s19c5204e"), sErr.GetData(), MB_ICONERROR);
            return;
        }
        AppendLog(L"[bootre] reagentc /boottore ok; reboot in 10s");
        const std::wstring sRebootMsg = StringMgr.Get("general.s1eab04b3");
        ::InitiateSystemShutdownW(nullptr, const_cast<wchar_t*>(sRebootMsg.c_str()), 10, TRUE, TRUE);
    }
}

void CFrameWnd::MenuCmdUncompactOs(int /*iCmd*/)
{
    // 原 case 1208：取消压缩前校验当前状态（0 跳过 / -1 跳过 / 1,2 继续）
    // [R16-T8] 线程体迁入 WofUncompactJob（BackgroundJobs 体系）；确认走 HostConfirm
    int nState = WofUncompactJob::QueryState();
    CDuiString sLog;
    sLog.Format(L"[clean] pre-uncompact state=%d (0=never,1=mixed,2=optimized,-1=unknown)",
                nState);
    AppendLog(sLog.GetData());

    if (nState == 0)
    {
        AppendLog(L"[clean] CompactOS already off; skipping uncompact");
        return;
    }
    if (nState == -1)
    {
        AppendLog(L"[clean] CompactOS state query failed; skipping uncompact to be safe");
        return;
    }

    if (!HostConfirm(StringMgr.Get("misc.sfb5b714d"),
        (std::wstring(StringMgr.Get("cleanup.s64817b5a"))
         + std::wstring(StringMgr.Get("toolbox.s03f832d9"))).c_str()))
        return;

    // [P0-A2] 撤销 CompactOS：原生 FSCTL_DELETE_EXTERNAL_BACKING 递归
    AppendLog(L"[clean] starting CompactOS uncompact (native WOF, background)...");
    // [CRASHFIX] 裸 detach 迁移为受 SEH 边界保护、可被 WM_CLOSE 等待的统一作业
    if (!WofUncompactJob::Start(m_hWnd))
        AppendLog(L"[clean] 后台作业启动失败（线程资源不足）");
}

void CFrameWnd::MenuCmdSwitchLang(int nIdx)
{
    // 原 case 1320-1330：写 config.ini，确认后重启进程生效
    // [R16-T8] 提示/确认统一走 HostAlert/HostConfirm（flag 组合逐字等价）
    if (m_nLanguage == nIdx) return;

    const std::wstring sCode = kLangMenu[nIdx].pszCode;
    if (!IsLangPackAvailable(sCode))
    {
        HostAlert(StringMgr.Get("misc.s295bb704"),
            L"\u8be5\u8bed\u8a00\u5305\u672a\u5b89\u88c5\uff08lang \u76ee\u5f55\u4e0b\u7f3a\u5c11\u5bf9\u5e94 DLL\uff09\u3002",
            MB_ICONWARNING);
        return;
    }

    if (!i18n::CConfigReader::SetLanguageCurrent(sCode.c_str()))
    {
        HostAlert(StringMgr.Get("misc.s295bb704"),
            L"\u5199\u5165 config.ini \u5931\u8d25\uff0c\u8bed\u8a00\u8bbe\u7f6e\u672a\u4fdd\u5b58\u3002",
            MB_ICONERROR);
        return;
    }

    StringMgr.SwitchLang(sCode.c_str());
    m_nLanguage = nIdx;

    if (HostConfirm(StringMgr.Get("misc.s295bb704"),
        L"\u8bed\u8a00\u8bbe\u7f6e\u5df2\u4fdd\u5b58\uff0c\u91cd\u542f\u7a0b\u5e8f\u540e\u751f\u6548\u3002\n"
        L"\u662f\u5426\u7acb\u5373\u91cd\u542f\uff1f"))
        RestartApplication();
}
// ----------------------------------------------------------------------------
// void CFrameWnd::TrackMenuAndDispatch(HMENU hMenu, int iMenu)
//  TrackPopupMenu + DestroyMenu + switch 分发（原函数尾部）
// ----------------------------------------------------------------------------
void CFrameWnd::TrackMenuAndDispatch(HMENU hMenu, int iMenu)
{
    // Anchor: drop down directly below the clicked menu button (its
    // bottom-left corner), so the title bar / menu row stay visible.
    const wchar_t* pszAnchor =
        iMenu == 1 ? L"menu_file" :
        iMenu == 2 ? L"menu_recover" :
        iMenu == 3 ? L"menu_option" : L"menu_help";

    POINT pt;
    bool bAnchored = false;
    CControlUI* pAnchor = m_pm.FindControl(pszAnchor);
    if (pAnchor)
    {
        const RECT& rcBtn = pAnchor->GetPos();
        if (rcBtn.right > rcBtn.left && rcBtn.bottom > rcBtn.top)
        {
            pt.x = rcBtn.left;
            pt.y = rcBtn.bottom;
            ::ClientToScreen(m_hWnd, &pt);
            bAnchored = true;
        }
    }
    if (!bAnchored)
        ::GetCursorPos(&pt);

    int iCmd = ::TrackPopupMenu(hMenu,
        TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
        pt.x, pt.y, 0, m_hWnd, nullptr);
    ::DestroyMenu(hMenu);

    if (iCmd != 0)
        DispatchMenuCmd(iCmd);
}

// ----------------------------------------------------------------------------
// [R16-T7] 表驱动处理器 —— 原 case 体逻辑抽取（语义与基线逐一对应）
//  新增菜单项改动点：①kMenuCmdTab 增一行 ②（仅无现成处理器时）新增实现；
//  复用既有处理器时仅需 ①。
// ----------------------------------------------------------------------------
void CFrameWnd::MenuCmdRouteTools(int iCmd)
{
    // [R4] 映像/恢复命令（释放/捕获/挂载/ImageX/ESD）统一收口工具箱页
    //（1102 为独立 12 步挂载向导；1103/1104/1106 对接 ImageX 弹窗）
    if (CPageBase* p = PageById(EPageId::PAGE_TOOLS)) p->OnHostCommand(iCmd);
}

void CFrameWnd::MenuCmdRefresh(int /*iCmd*/)     { RefreshAll(); }    // 1150 重新载入
void CFrameWnd::MenuCmdAbortTask(int /*iCmd*/)   { AbortDism(); }     // 1151 取消任务
void CFrameWnd::MenuCmdGotoOptim(int /*iCmd*/)   { SwitchToPage(4); } // 1152 系统优化
void CFrameWnd::MenuCmdGotoDriver(int /*iCmd*/)  { SwitchToPage(5); } // 1153 驱动管理

void CFrameWnd::MenuCmdSfcVerify(int /*iCmd*/)
{
    // [原生化 B1] 校验系统文件：sfc.exe 直连自建控制台（无 cmd/dism）
    StartSystemRepair(false /*RestoreHealth*/, true /*sfc*/);
}

void CFrameWnd::MenuCmdResetHint(int /*iCmd*/)
{
    // [R16-T8] 裸 MessageBox 统一走 HostAlert（MB_OK|MB_ICONINFORMATION 等价）
    HostAlert(StringMgr.Get("misc.s45597372"),
        (CDuiString(StringMgr.Get("misc.s08489722")) + StringMgr.Get("program.sc71a7a1a")
        + StringMgr.Get("misc.s51ba917b")).GetData(),
        MB_ICONINFORMATION);
}

void CFrameWnd::MenuCmdDetails(int /*iCmd*/)
{
    // [R-B10] 详细设置 → 原版 DLGTEMPLATEEX ID133，含 5 选项
    if (CPageBase* p = PageById(EPageId::PAGE_TOOLS)) p->HostRunToolDialog(EToolDlgKind::TD_DETAILS);
}

void CFrameWnd::MenuCmdSetAfterAction(int iCmd)
{
    // [R-B46] 操作完成后：0=无 1=重启 2=关机（one-shot）
    m_nAfterAction = iCmd - static_cast<int>(EMenuCmd::MC_AFTER_NONE);
}

void CFrameWnd::MenuCmdLangByCmd(int iCmd)
{
    // 语言切换：写 config.ini，确认后重启进程生效
    MenuCmdSwitchLang(iCmd - static_cast<int>(EMenuCmd::MC_LANG_BASE));
}

void CFrameWnd::MenuCmdHelpUrl(int iCmd)
{
    // [R-B88] 帮助/官网/反馈网址暂时全部留空，后期统一在此填入新地址。
    static const wchar_t* kHelpUrlProduct  = L"";  // 软件主页
    static const wchar_t* kHelpUrlWebsite  = L"";  // 官方网站
    static const wchar_t* kHelpUrlFeedback = L"";  // 问题反馈
    const wchar_t* pszUrl =
        (iCmd == static_cast<int>(EMenuCmd::MC_HELP_PRODUCT))  ? kHelpUrlProduct :
        (iCmd == static_cast<int>(EMenuCmd::MC_HELP_WEBSITE))  ? kHelpUrlWebsite :
                                                                 kHelpUrlFeedback;
    if (pszUrl && *pszUrl)
        ::ShellExecute(m_hWnd, L"open", pszUrl, nullptr, nullptr, SW_SHOWNORMAL);
}

void CFrameWnd::MenuCmdHelpIntro(int /*iCmd*/)
{
    // [R16-T8] 裸 MessageBox 统一走 HostAlert（MB_OK|MB_ICONINFORMATION 等价）
    HostAlert(StringMgr.Get("misc.scc2731df"),
        (CDuiString(StringMgr.Get("misc.s0e851dd1")) + StringMgr.Get("misc.sb9c913dc")
        + StringMgr.Get("misc.s103de2e6") + StringMgr.Get("misc.s7af3f76f")).GetData(),
        MB_ICONINFORMATION);
}

void CFrameWnd::MenuCmdAbout(int /*iCmd*/)  { ShowAboutBox(); }

void CFrameWnd::MenuCmdLogView(int /*iCmd*/)
{
    // [R-B16] 查看日志 → 原版 DLGTEMPLATEEX ID134
    LARGE_INTEGER _a,_b,_fq; ::QueryPerformanceFrequency(&_fq); ::QueryPerformanceCounter(&_a);
    CPageBase* p = PageById(EPageId::PAGE_TOOLS);
    ::QueryPerformanceCounter(&_b);
    { wchar_t _s[256]={0}; swprintf_s(_s,L"[dlgperf] PageById=%.1fms p=%p\n",
        1000.0*(_b.QuadPart-_a.QuadPart)/_fq.QuadPart,(void*)p); ::OutputDebugStringW(_s);
      wchar_t _p[MAX_PATH]={0}; ::GetModuleFileNameW(nullptr,_p,MAX_PATH);
      wchar_t* _sl=::wcsrchr(_p,L'\\'); if(_sl) *(_sl+1)=L'\0';
      ::lstrcatW(_p,L"perf_probe.txt");
      HANDLE _h=::CreateFileW(_p,GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
      if(_h!=INVALID_HANDLE_VALUE){ ::SetFilePointer(_h,0,nullptr,FILE_END);
        DWORD _w=0; ::WriteFile(_h,_s,(DWORD)(wcslen(_s)*sizeof(wchar_t)),&_w,nullptr); ::CloseHandle(_h);} }
    if (p) p->HostRunToolDialog(EToolDlgKind::TD_LOGVIEW);
}

// ----------------------------------------------------------------------------
// [R16-T7] 菜单命令分发表（替代原 44-case switch，逐 ID 语义等价）
//  表置于 DispatchMenuCmd 函数体内（static 局部）：处理器为 CFrameWnd
//  private 成员，成员函数体内方可取其成员指针。
// ----------------------------------------------------------------------------
struct SMenuCmdEntry
{
    int iCmd;
    void (CFrameWnd::*pfnHandler)(int iCmd);
};

// ----------------------------------------------------------------------------
// void CFrameWnd::DispatchMenuCmd(int iCmd)
//  [R16-T7] 真表驱动：查 kMenuCmdTab 线性匹配（45 项，微秒级）；
//  未知命令保持原 default: break 语义。cc(Dispatch) < 10。
// ----------------------------------------------------------------------------
void CFrameWnd::DispatchMenuCmd(int iCmd)
{
    static const SMenuCmdEntry kMenuCmdTab[] = {
        // ---- 文件(F) ----  [C++17 R20] scoped enum; explicit casts below
        { static_cast<int>(EMenuCmd::MC_FILE_SESSION_CLEANUP), &CFrameWnd::MenuCmdRouteTools },
        { static_cast<int>(EMenuCmd::MC_FILE_MOUNT_WIZARD),    &CFrameWnd::MenuCmdRouteTools },
        { static_cast<int>(EMenuCmd::MC_FILE_IMAGEX_1103),     &CFrameWnd::MenuCmdRouteTools },
        { static_cast<int>(EMenuCmd::MC_FILE_IMAGEX_1104),     &CFrameWnd::MenuCmdRouteTools },
        { static_cast<int>(EMenuCmd::MC_FILE_IMAGEX_1105),     &CFrameWnd::MenuCmdRouteTools },
        { static_cast<int>(EMenuCmd::MC_FILE_IMAGEX_1106),     &CFrameWnd::MenuCmdRouteTools },
        { static_cast<int>(EMenuCmd::MC_FILE_ADD_SOURCE),      &CFrameWnd::MenuCmdAddSourceDir },
        { static_cast<int>(EMenuCmd::MC_ESD_WIM),              &CFrameWnd::MenuCmdRouteTools },
        { static_cast<int>(EMenuCmd::MC_SWM_WIM),              &CFrameWnd::MenuCmdSwmMerge },
        { static_cast<int>(EMenuCmd::MC_ESD_ISO),              &CFrameWnd::MenuCmdRouteTools },
        { static_cast<int>(EMenuCmd::MC_ESD_DECRYPT),          &CFrameWnd::MenuCmdRouteTools }, // [R-B35] ESD解密
        { static_cast<int>(EMenuCmd::MC_FILE_RELOAD),          &CFrameWnd::MenuCmdRefresh },
        { static_cast<int>(EMenuCmd::MC_FILE_ABORT),           &CFrameWnd::MenuCmdAbortTask },
        { static_cast<int>(EMenuCmd::MC_GOTO_OPTIM),           &CFrameWnd::MenuCmdGotoOptim },
        { static_cast<int>(EMenuCmd::MC_GOTO_DRIVER),          &CFrameWnd::MenuCmdGotoDriver },
        // ---- 恢复功能(R) ----
        { static_cast<int>(EMenuCmd::MC_RECOVER_SFC),           &CFrameWnd::MenuCmdSfcVerify },
        { static_cast<int>(EMenuCmd::MC_RECOVER_RESTOREHEALTH), &CFrameWnd::MenuCmdRestoreHealth },
        { static_cast<int>(EMenuCmd::MC_RECOVER_BOOT),          &CFrameWnd::MenuCmdRouteTools },
        { static_cast<int>(EMenuCmd::MC_RECOVER_BACKUP),        &CFrameWnd::MenuCmdRouteTools },
        { static_cast<int>(EMenuCmd::MC_RECOVER_RESTORE),       &CFrameWnd::MenuCmdRouteTools },
        { static_cast<int>(EMenuCmd::MC_RECOVER_RESET_HINT),    &CFrameWnd::MenuCmdResetHint },
        { static_cast<int>(EMenuCmd::MC_RECOVER_BOOT_RECOVERY), &CFrameWnd::MenuCmdBootToRecovery },
        { static_cast<int>(EMenuCmd::MC_RECOVER_UNCOMPACT),     &CFrameWnd::MenuCmdUncompactOs },
        // ---- 选项(O) ----
        { static_cast<int>(EMenuCmd::MC_OPT_DETAILS),    &CFrameWnd::MenuCmdDetails },
        { static_cast<int>(EMenuCmd::MC_AFTER_NONE),     &CFrameWnd::MenuCmdSetAfterAction },
        { static_cast<int>(EMenuCmd::MC_AFTER_REBOOT),   &CFrameWnd::MenuCmdSetAfterAction },
        { static_cast<int>(EMenuCmd::MC_AFTER_SHUTDOWN), &CFrameWnd::MenuCmdSetAfterAction },
        // 语言切换（1320-1330 并入同表，下标 = iCmd - MC_LANG_BASE）
        { static_cast<int>(EMenuCmd::MC_LANG_BASE) + 0,  &CFrameWnd::MenuCmdLangByCmd },
        { static_cast<int>(EMenuCmd::MC_LANG_BASE) + 1,  &CFrameWnd::MenuCmdLangByCmd },
        { static_cast<int>(EMenuCmd::MC_LANG_BASE) + 2,  &CFrameWnd::MenuCmdLangByCmd },
        { static_cast<int>(EMenuCmd::MC_LANG_BASE) + 3,  &CFrameWnd::MenuCmdLangByCmd },
        { static_cast<int>(EMenuCmd::MC_LANG_BASE) + 4,  &CFrameWnd::MenuCmdLangByCmd },
        { static_cast<int>(EMenuCmd::MC_LANG_BASE) + 5,  &CFrameWnd::MenuCmdLangByCmd },
        { static_cast<int>(EMenuCmd::MC_LANG_BASE) + 6,  &CFrameWnd::MenuCmdLangByCmd },
        { static_cast<int>(EMenuCmd::MC_LANG_BASE) + 7,  &CFrameWnd::MenuCmdLangByCmd },
        { static_cast<int>(EMenuCmd::MC_LANG_BASE) + 8,  &CFrameWnd::MenuCmdLangByCmd },
        { static_cast<int>(EMenuCmd::MC_LANG_BASE) + 9,  &CFrameWnd::MenuCmdLangByCmd },
        { static_cast<int>(EMenuCmd::MC_LANG_BASE) + 10, &CFrameWnd::MenuCmdLangByCmd },
        // ---- 帮助(H) ----
        { static_cast<int>(EMenuCmd::MC_HELP_PRODUCT),  &CFrameWnd::MenuCmdHelpUrl },
        { static_cast<int>(EMenuCmd::MC_HELP_WEBSITE),  &CFrameWnd::MenuCmdHelpUrl },
        { static_cast<int>(EMenuCmd::MC_HELP_INTRO),    &CFrameWnd::MenuCmdHelpIntro },
        { static_cast<int>(EMenuCmd::MC_HELP_FEEDBACK), &CFrameWnd::MenuCmdHelpUrl },
        { static_cast<int>(EMenuCmd::MC_HELP_ABOUT),    &CFrameWnd::MenuCmdAbout },
        { static_cast<int>(EMenuCmd::MC_HELP_LOGVIEW),  &CFrameWnd::MenuCmdLogView },
    };

    for (size_t i = 0; i < _countof(kMenuCmdTab); ++i)
    {
        if (kMenuCmdTab[i].iCmd == iCmd)
        {
            (this->*kMenuCmdTab[i].pfnHandler)(iCmd);
            return;
        }
    }
}
// [C++17 R3] 本文件内联常量（C++17 inline variables，多 TU ODR 一致）：
