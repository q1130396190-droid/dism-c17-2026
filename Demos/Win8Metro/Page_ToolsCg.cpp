#include "stdafx.h"

// ============================================================
// Page_ToolsCg.cpp —— 工具箱页「春哥附体」子系统。
//
// [R138-1] 从 Page_Tools.cpp（2583 行）拆出 369 行：
//   风险确认 → 强随机密码生成 → 桌面恢复说明 → 账户/UAC/自动登录改写
//   → 失败回滚 → 完成提示。
//
// 闭包说明：11 个成员函数声明见 Page_Tools.h；外部依赖为
// StringMgr / ToolSql::HostNotify / ToolSql::HostConfirm /
// ExternalToolService::WriteBatFile / NetUser* / SHGetSpecialFolderPath，
// 均经以下 include 引入。
//
// 搬迁为逐字搬运，零逻辑改动。
// ============================================================

#include "Page_Tools.h"

#include "Page_ToolsCommon.h"     // ToolSql::HostNotify / HostConfirm
#include "StringManager.h"
#include "ExternalToolService.h"  // WriteBatFile
#include "FormatW.h"
#include "SystemInfoService.h"    // GetDesktopDir / MakeTimestamp / GetBuildNumber              // FormatW / std::to_wstring 组合

#include <windows.h>
#include <shlobj.h>               // SHGetSpecialFolderPath
#include <lm.h>                   // NetUserGetInfo / NetUserSetInfo
#pragma comment(lib, "netapi32.lib")

#include <optional>
#include <random>                 // std::mt19937
#include <string>

using i18n::CStringManager;

// 恢复说明文件名的固定后缀（与桌面生成逻辑对应）
inline constexpr const wchar_t* kCgNoteSuffix = L"\\春哥附体恢复说明.txt";

bool CPageTools::CgConfirmRisk(int& nAutoLogon, int& nNoUac)
{
{

    CDuiString sMsg;

    sMsg.Format(L"%s%s",

        (const wchar_t*)(CDuiString(StringMgr.Get("misc.sff30f67c")) + StringMgr.Get("system.s3f6ada74") + StringMgr.Get("misc.s723ad6af") + StringMgr.Get("misc.sdce38be5") + StringMgr.Get("error.s542a31c9")),

        StringMgr.Get("confirm.continue"));

    int nOk = ToolSql::HostConfirm(m_ctx.HostHwnd(), sMsg.GetData(),
                          StringMgr.Get("misc.s7c2d35e4"), MB_OKCANCEL, MB_ICONWARNING);

    if (nOk != IDOK) return false;

}



nAutoLogon = ToolSql::HostConfirm(m_ctx.HostHwnd(),
    CDuiString(StringMgr.Get("confirm.s61cd811d")) + StringMgr.Get("optim.s46310799") + StringMgr.Get("misc.s7bed938a"),
    StringMgr.Get("misc.s7c2d35e4"), MB_YESNO, MB_ICONQUESTION);



// P0-5：AutoLogon + DisableUAC 组合是高危，用户同时勾选时加一次二次确认

nNoUac = ToolSql::HostConfirm(m_ctx.HostHwnd(),
    CDuiString(StringMgr.Get("confirm.s2c0e49d5")) + StringMgr.Get("system.s72a5a0d0") + StringMgr.Get("misc.saec747d2"),
    StringMgr.Get("misc.s7c2d35e4"), MB_YESNO, MB_ICONQUESTION);



if (nAutoLogon == IDYES && nNoUac == IDYES)

{

    CDuiString sMsg = CDuiString(StringMgr.Get("misc.h77f5")) + CDuiString(StringMgr.Get("misc.h58eb")) + CDuiString(StringMgr.Get("misc.h15de"));
    int nGo = ToolSql::HostConfirm(m_ctx.HostHwnd(), sMsg.GetData(), StringMgr.Get("misc.s7c2d35e4"), MB_YESNO | MB_DEFBUTTON2);

    if (nGo != IDYES)

    {

        // 用户取消其中一个：优先保留更安全的选项

        nNoUac = IDNO;

    }

}
return true;
}

void CPageTools::CgGenStrongPassword(CDuiString& sPwd)
{
{
    // 无歧义字母表（去掉了 I/l/O/0/1）
    const wchar_t* pszAlphabet =
        L"ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789!@#$%^&*";
    int nAlen = static_cast<int>(wcslen(pszAlphabet));
    constexpr int nLen = 20;
    BOOL bOk = FALSE;
    BYTE  abRand[nLen];
    HMODULE hBc = ::LoadLibraryW(L"bcrypt.dll");
    if (hBc)
    {
        using PFN_BCryptGenRandom = LONG ( WINAPI*)(void*, void*, unsigned long, unsigned char*);
        PFN_BCryptGenRandom pGen = (PFN_BCryptGenRandom)::GetProcAddress(hBc, "BCryptGenRandom");
        if (pGen && pGen(nullptr, abRand, nLen, nullptr) == 0)
            bOk = TRUE;
        ::FreeLibrary(hBc);
    }
    if (bOk)
    {
        // 用拒绝抽样避免取模偏差
        int nMax = static_cast<int>(255 - 255 % nAlen);
        for (int i = 0; i < nLen; ++i)
        {
            int v;
            do { v = abRand[i]; } while (v >= nMax);
            sPwd += pszAlphabet[v % nAlen];
        }
    }
    else
    {
        // 兜底：BCrypt 不可用时退化为 C++11 mt19937（随机数），避免工具完全不可用
        std::mt19937 rng(static_cast<unsigned int>(::GetTickCount()));
        std::uniform_int_distribution<int> dist(0, static_cast<int>(nAlen) - 1);
        for (int i = 0; i < nLen; ++i)
            sPwd += pszAlphabet[dist(rng)];
        m_ctx.HostLog(StringMgr.Get("misc.h8b54"), static_cast<int>(ELogLevel::LOG_WARN));
    }
}
}

void CPageTools::CgWriteDesktopNote(const CDuiString& sPwd, int nAutoLogon, int nNoUac)
{
// write recovery note on desktop (ANSI)

CDuiString sNotePath = SystemInfoService::GetDesktopDir()

    + StringMgr.Get("misc.s09da4333");

CDuiString sNote;

CDuiString sTmp;

sTmp.Format(CDuiString(StringMgr.Get("misc.s4cf3b22b")) + L"================================================\r\n" + StringMgr.Get("system.s51df540e") + StringMgr.Get("misc.sba98efd5"),

            SystemInfoService::MakeTimestamp().GetData(), SystemInfoService::GetWindowsFamilyName().GetData(),

            SystemInfoService::GetBuildNumber(), sPwd.GetData());

sNote += sTmp;

sNote += StringMgr.Get("misc.s9f57e6b3");

sNote += StringMgr.Get("misc.s06f3782d");

sNote += StringMgr.Get("misc.s3536b70c");

sNote += StringMgr.Get("misc.sc84bde81");

if (nAutoLogon == IDYES)

    sNote += StringMgr.Get("optim.se50a9dfa");

if (nNoUac == IDYES)

    sNote += StringMgr.Get("misc.s9a9177ac");

ExternalToolService::WriteBatFile(sNotePath, sNote);
}

void CPageTools::CgReadAdminFlags(DWORD& dwOldFlags, bool& bKnownFlags)
{
// 先记录 Administrator 现有标志（保留 UF_PASSWD_NOTREQD 等位）
{
    LPUSER_INFO_1008 pCur = nullptr;
    if (::NetUserGetInfo(nullptr, L"Administrator", 1008,
                        (BYTE**)&pCur) == NERR_Success)
    {
        dwOldFlags = pCur->usri1008_flags;
        bKnownFlags = true;
        ::NetApiBufferFree(pCur);
    }
    else
    {
        m_ctx.HostLog(L"[chunge] NetUserGetInfo failed, use default flags",
                      static_cast<int>(ELogLevel::LOG_WARN));
    }
}
}


void CPageTools::CgShowDone(int nAutoLogon, int nNoUac,
                            const std::optional<DWORD>& optNoteErr)
{
// [AUDIT-P0] 提示文案必须与真实落盘结果一致。
    // 成功 = 明确告知已保存；失败 = 明确告知未保存并要求立刻手工抄录密码，
    // 绝不能在写盘失败时仍宣称已备份（那会让用户跳过备份步骤）。
    CDuiString sDone;
    if (optNoteErr.has_value())
    {
        const std::wstring sCode = std::to_wstring(optNoteErr.value());
        sDone = FormatW(StringMgr.Get("toolbox.ui_cg_done_nofile"), sCode.c_str()).c_str();
    }
    else
    {
        sDone = StringMgr.Get("toolbox.ui_cg_done_ok");
    }
    if (nAutoLogon == IDYES) sDone += StringMgr.Get("toolbox.ui_cg_autologon");
    if (nNoUac == IDYES)     sDone += StringMgr.Get("toolbox.ui_cg_uac_off");

    ToolSql::HostNotify(m_ctx.HostHwnd(), sDone.GetData(),
                    StringMgr.Get("misc.s7c2d35e4"),
                    MB_OK | (optNoteErr.has_value() ? MB_ICONWARNING : MB_ICONINFORMATION));
}

// ============================================================
// [R170] 从 Page_Tools.cpp 迁入「引导修复特权 + 春哥附体主流程」
//
// 同属“账户/引导”业务域，随本子系统一并在本 TU 维护：
//   · BfEnablePrivilege  启用当前进程令牌特权（UEFI 引导修复写 NVRAM 前）
//   · BfGetFileSystem    读取目标盘文件系统名（UEFI ESP 必须 FAT16/FAT32）
//   · ToolChunGe         春哥附体主流程（确认 → 生成密码 → 写说明 → 改账户 → 回滚）
//
// 逐字迁入，零逻辑改动。Bf* 为文件级 static，仅本 TU 可见。
// ============================================================

// [R-B73] 启用当前进程令牌中的指定特权（如 SeSystemEnvironmentPrivilege）。
// 知识库 20_总体架构 / 70 号文档：UEFI 引导修复写固件 NVRAM 前需提权。
// bcdboot.exe 子进程自身也会申请该特权，此处保持与原版同序并留日志。
static bool BfEnablePrivilege(const wchar_t* pszPriv)
{
    PTFileGuard hToken;
    if (!::OpenProcessToken(::GetCurrentProcess(),
                            TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, hToken.out()))
        return false;
    TOKEN_PRIVILEGES tp = {};
    bool bOk = false;
    if (::LookupPrivilegeValue(nullptr, pszPriv, &tp.Privileges[0].Luid))
    {
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        bOk = ::AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), nullptr, nullptr)
              && ::GetLastError() == ERROR_SUCCESS;
    }
    return bOk;
}

// [R-B73] 读取目标盘文件系统名，用于 UEFI ESP 必须为 FAT16/FAT32 的预检。
static bool BfGetFileSystem(const CDuiString& sDrive, CDuiString& sFS)
{
    constexpr int kFileSystemNameWchars = 32;   // 32 缓冲尺寸（语义命名，数值不变）
    wchar_t szFS[kFileSystemNameWchars] = { 0 };
    if (!::GetVolumeInformationW(sDrive, nullptr, 0, nullptr, nullptr, nullptr,
                                 szFS, _countof(szFS)))
        return false;
    sFS = szFS;
    return true;
}

// [2026-10-08] 注册表直写原语（替代 reg.exe，知识库 40 §2.3）

// 3 — 引导修复（原版"引导修复-UEFI/BIOS"，弹窗内选分区 + bcdboot）

void CPageTools::ToolBootRepair()

{
    // 点击"引导修复"直接弹出原版样式的"引导修复-UEFI/BIOS"弹窗（无前置提醒）：
    // 弹窗内真实枚举物理磁盘分区，选中目标分区后由 bcdboot 重建引导。
    RunDialog(EToolDlgKind::TD_BOOTFIX);
}

// 4 — "春哥附体": enable built-in Administrator with a strong random password

void CPageTools::ToolChunGe()
{
    // 三级风险确认（首次警告 + AutoLogon/UAC 二次确认）
    int nAutoLogon = IDNO;
    int nNoUac = IDNO;
    if (!CgConfirmRisk(nAutoLogon, nNoUac)) return;
    // 生成强随机密码（BCryptGenRandom，拒绝抽样）
    CDuiString sPwd;
    CgGenStrongPassword(sPwd);
    // 桌面密码纸条（write recovery note on desktop）
    CgWriteDesktopNote(sPwd, nAutoLogon, nNoUac);
    // 读取 Administrator 现有标志（保留 UF_PASSWD_NOTREQD 等位）
    DWORD dwOldFlags = 0;
    bool  bKnownFlags = false;
    CgReadAdminFlags(dwOldFlags, bKnownFlags);
    // ① 激活账户 → ② 设置密码 → ③ 自动登录 → ④ 关闭 UAC（直写）
    DWORD dwNe = CgActivateAdmin(bKnownFlags, dwOldFlags);
    if (dwNe == NERR_Success) dwNe = CgSetPassword(sPwd);
    if (dwNe == NERR_Success && nAutoLogon == IDYES) dwNe = CgSetAutoLogon(sPwd, dwNe);
    if (dwNe == NERR_Success && nNoUac == IDYES)     dwNe = CgDisableUac(dwNe);
    // 失败回滚
    if (dwNe != NERR_Success)
    {
        CgRollbackOnFail(bKnownFlags, dwOldFlags, dwNe);
        return;
    }
    m_ctx.HostLog(L"[chunge] Administrator enabled, password set");
    // [R-B72] 桌面生成恢复说明.txt
    // [AUDIT-P0] 写入结果向下传递：失败时完成提示必须如实说明，
    // 而不是统一告知"已保存"。
    const std::optional<DWORD> optNoteErr = CgWriteRecoveryNote(nAutoLogon, nNoUac);
    if (optNoteErr.has_value())
    {
        CDuiString sErr;
        sErr.Format(L"[chunge] recovery note write failed, gle=%lu",
                    static_cast<unsigned long>(optNoteErr.value()));
        m_ctx.HostLog(sErr.GetData(), static_cast<int>(ELogLevel::LOG_WARN));
    }
    // 完成提示
    CgShowDone(nAutoLogon, nNoUac, optNoteErr);
}
