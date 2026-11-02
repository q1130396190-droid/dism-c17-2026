#include "stdafx.h"

// ============================================================
// Page_ToolsItems.cpp —— [R93] 工具箱"单项工具包装组"
//
// 从 Page_Tools.cpp 切出的 4 个成员函数（函数体零逻辑改动）：
//   ToolBackup        0 另存为映像（自定义模态弹窗，真实 DISM 捕获）
//   ToolRestoreImage  1 系统还原
//   ToolUserAccounts  2 账户管理
//   ToolHosts        10 hosts 内置编辑（静默备份 → 拉起记事本）
//
// [R93 拆分前提] 切片区间 324..424 经边界校验：
//   · 位于 Page_Tools.cpp 两个匿名 namespace（52..76、431..486）之间的
//     普通作用域内，不跨 namespace 边界；
//   · 段内无 namespace 关键字，括号平衡为 0；
//   —— R92 就是因为没做这一步，切出了没有配对的命名空间闭括号，
//      导致 13 个编译错误并整体回滚。
//
// 为什么这组可以独立：
//   四者都是"点击卡片后进入某个工具"的薄包装，形态统一
//   （RunDialog(EToolDlgKind) 或单次外部进程调用），
//   与卡片渲染层、筛选层无共享 static。
//   切片外调用点：ToolDispatchItem 的分发表，经类声明访问。
// ============================================================

#include "Page_Tools.h"
#include "Page_ToolsCommon.h"   // 跨文件共享的 RAII 守卫与 UI helper
#include "Page_ToolsHosts.h"    // hosts 编辑器辅助层
#include "NotifyBox.h"          // HostNotify / HostConfirm
#include "StringManager.h"
#include "Win32DialogService.h"
#include "ProcessRunner.h"      // ResolveSysTool

using namespace DuiLib;
using i18n::CStringManager;

using ToolSql::HostNotify;
using ToolSql::HostConfirm;
using ToolHosts::CHostEditCtx;
using ToolHosts::HOSTDLG_ID_EDIT;
using ToolHosts::Buf;
using ToolHosts::HostEditProc;
using ToolHosts::MakeHostDialogTemplate;
using ToolSql::ResolveProc;

void CPageTools::ToolBackup()

{
    RunDialog(EToolDlgKind::TD_CAPTURE);
}

// 1 — 释放映像（自定义模态弹窗，真实 DISM Apply + bcdboot）

void CPageTools::ToolRestoreImage()

{
    RunDialog(EToolDlgKind::TD_APPLY);
}

// 2 — 账户管理（自定义模态弹窗，NetUserEnum/NetUserSetInfo 真实操作）

void CPageTools::ToolUserAccounts()

{
    RunDialog(EToolDlgKind::TD_ACCOUNT);
}


// ============================================================================
//  [2026-10-08] ESD转ISO 内置化（知识库 40 §2.1 行5/§2.3：
//  wimgapi 导出 + 进程内文件组装 + 内置 ISO 写入，零 bat/oscdimg/dism）
// ============================================================================

// [rb28 2026-10-13] 后台任务进度消息：wParam=0..100 百分比；
// lParam=堆上 std::wstring* 阶段文案（接收方 delete），nullptr 表示不改文案。
// [GC-06] JOBWM_PROGRESS 已迁入 FrameWnd_Messages.h 统一注册表

// [R16-T9] CJobDoneSink（统一完成消息接收窗）与 TJobDoneEnvelope（统一信封）
// 已迁入 BackgroundJobs.h，供 Page_Tools / ToolDialogPage 共用；
// 本文件不再定义任何完成 sink。

// [rb28] ESD转ISO 的等待窗已统一为 CJobProgressWnd（带真实进度条），
//        旧的纯 STATIC E2ICreateWaitWindow 已移除。
// ============================================================================
// [R128-R] 作业层重建：CJobProgressWnd / JobProgPost / TJobWimProgress
//   本段曾在拆分为 Page_ToolsJobs.h 时移出，拆分失败回退后未完整还原，
//   导致三处符号缺失（编译期 C3861/C2065）。此处按原语义逐行等效重建。
// ============================================================================

// [rb28] 通用模态进度窗（SWM 合并 / WIM-ESD 互转 / ESD转ISO 共用）。
// 工作线程只允许 PostMessage，禁止直接碰控件；WM_CLOSE 时恢复属主并销毁。
// [B4] 高 DPI：窗口/控件坐标全部跟随属主 DPI 缩放；白底 + STATIC 透明背景。

// 10 — hosts 内置编辑（原 notepad 打开，已进程内化）

void CPageTools::ToolHosts()

{
    // [R-B82] 点击工具箱"hosts 编辑器"直接用记事本打开 hosts，不再弹任何提醒菜单。
    // 原实现用 MessageBox(MB_OKCANCEL | MB_YESNOCANCEL) 弹"1.编辑 2.恢复备份
    // 3.刷新DNS 4.取消"——两个按钮组标志按位或属非法组合，实际只显示
    // 是/否/取消，与编号说明完全错位；首次使用无备份时点"是"会报
    // "没有找到备份文件：...hosts_latest.bak"且编辑器根本不打开。
    // 现改为：静默备份当前 hosts → 直接拉起记事本（未提权时走 UAC）。
    // 需要恢复备份可用"文件浏览器"中的 hosts 备份目录手动还原。
    m_ctx.HostLog(L"[tools] hosts 编辑器打开", static_cast<int>(ELogLevel::LOG_INFO));
    CDuiString sHosts;
    wchar_t szWin[MAX_PATH] = { 0 };
    // [R95] 取不到 Windows 目录就不能拼路径：原来不检查返回值，
    //   失败时 szWin 全零 → 得到 "\System32\drivers\etc\hosts"（无盘符），
    //   后续 GetFileAttributes 必然失败，备份被静默跳过。
    if (::GetWindowsDirectoryW(szWin, MAX_PATH) == 0)
    {
        m_ctx.HostLog(L"[!] [tools] 取 Windows 目录失败，hosts 编辑器不可用",
                      static_cast<int>(ELogLevel::LOG_ERROR));
        return;
    }
    sHosts.Format(L"%s\\System32\\drivers\\etc\\hosts", szWin);
    // 编辑前静默备份到 %LOCALAPPDATA%\DismTool\HostsBackup\hosts_latest.bak（覆盖）
    CDuiString sBackup;
    {
        wchar_t szLa[MAX_PATH] = { 0 };
        // [R95] 同上：取不到就跳过备份（不阻断编辑），但要留痕。
        if (::GetEnvironmentVariableW(L"LOCALAPPDATA", szLa, MAX_PATH) == 0)
        {
            m_ctx.HostLog(L"[!] [tools] 取 LOCALAPPDATA 失败，跳过 hosts 备份",
                          static_cast<int>(ELogLevel::LOG_WARN));
            szLa[0] = 0;
        }
        sBackup.Format(L"%s\\DismTool\\HostsBackup", szLa);
        // [R95] 备份失败必须可见：原实现不看 CreateDirectory / CopyFileW 返回值，
        //   磁盘满/只读/杀软拦截时用户以为已备份，实际没有 ——
        //   与 R64 修的"密码说明写盘失败仍提示成功"是同一类静默失败。
        //   注意：备份失败**不阻断**编辑（用户可能就是想改 hosts），只记日志，
        //   因为 hosts 编辑器本身不依赖备份存在。
        bool bBackupOk = (szLa[0] != 0);
        if (::GetFileAttributes(sBackup.GetData()) == INVALID_FILE_ATTRIBUTES)
        {
            if (!::CreateDirectoryW(sBackup.GetData(), nullptr)
                && ::GetLastError() != ERROR_ALREADY_EXISTS)
                bBackupOk = false;
        }
        if (bBackupOk && ::GetFileAttributes(sHosts.GetData()) != INVALID_FILE_ATTRIBUTES)
        {
            CDuiString sLatest = sBackup + L"\\hosts_latest.bak";
            if (!::CopyFileW(sHosts.GetData(), sLatest.GetData(), FALSE))
                bBackupOk = false;
        }
        if (!bBackupOk)
        {
            CDuiString sErr;
            sErr.Format(L"[!] [tools] hosts 备份失败 err=%lu（编辑仍可继续，但无回退副本）",
                        ::GetLastError());
            m_ctx.HostLog(sErr.GetData(), static_cast<int>(ELogLevel::LOG_WARN));
        }
    }
    // [2026-10-08] 进程内编辑对话框（不再拉起 notepad，无需二次 UAC）
    CHostEditCtx ctx;
    ctx.sPath = sHosts.GetData();
    ctx.bBom = false;
    ctx.bSaved = false;
    Buf tpl = MakeHostDialogTemplate();
    // [B2 修复] 原实现不检查返回值：模板/创建失败时完全静默。
    // [B3 修复] DialogBoxIndirectParamW 失败返回 -1（0 是 EndDialog(0) 的合法
    // 返回值）；旧判断 nDlg==0 永远不会命中，失败被彻底静默。
    ::SetLastError(0);
    INT_PTR nDlg = ::DialogBoxIndirectParamW(::GetModuleHandle(nullptr),
                              (LPCDLGTEMPLATEW)&tpl[0],
                              m_ctx.HostHwnd(),
                              HostEditProc,
                              (LPARAM)&ctx);
    if (nDlg == -1)
    {
        CDuiString sErr;
        sErr.Format(L"[tools] hosts 编辑对话框创建失败 err=%lu", ::GetLastError());
        m_ctx.HostLog(sErr.GetData(), static_cast<int>(ELogLevel::LOG_INFO));
    }
}
