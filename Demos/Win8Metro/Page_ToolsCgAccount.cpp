// ============================================================
// Page_ToolsCgAccount.cpp —— 「春哥附体」账户与注册表操作层。
//
// [R106] 从 Page_ToolsCg.cpp 拆出。原文件 497 行同时承担六件事：
//   三级风险确认 / 密码生成 / 桌面说明 / 账户操作 / 注册表写入 / 主流程编排。
//   本 TU 只保留**会改系统状态的四个动作**：
//     CgActivateAdmin    启用内置 Administrator（保留原有账户标志位）
//     CgSetPassword      设置强随机密码（NetUserSetInfo）
//     CgSetAutoLogon     写 Winlogon AutoAdminLogon 三键
//     CgDisableUac       写 EnableLUA=0
//     CgRollbackOnFail   任一失败时按原标志位回滚
//
// 为什么这样切：这五个函数是"改机器"的全部写点，集中一处便于审计
//   （也便于将来给它们套统一的权限/日志包装）；而确认弹窗、密码生成、
//   桌面说明都是"不改机器"的准备动作，留在主文件。
//
// 拆分前后函数体逐字未改，仅搬迁位置。
// ============================================================

#include "stdafx.h"

#include "Page_Tools.h"
#include "Page_ToolsCommon.h"     // PTFileGuard

#include "StringManager.h"

#include <windows.h>
#include <lm.h>                   // NetUserGetInfo / NetUserSetInfo

using DuiLib::CDuiString;
using i18n::CStringManager;

// [R110] 注册表直写原语：两个写函数原本各复制一份
//   "打开键 + RegSetValueEx + 判返回值" 的骨架，只有寄存器类型与
//   数据长度不同。这里把骨架抽成一处，两个写函数只表达差异部分。
//
//   为什么在文件内抽而不是拆文件：R110 起不再新增文件，
//   同一 TU 内消除重复即可，不改变链接性与可见性。
//
//   仍保持 static：仅本 TU 可见。不提升为跨 TU 符号 ——
//   一旦成为共享符号，有人只改一侧就会出现"两个调用点行为不一致"而编译器不报错。
static bool CgRegSetRaw(HKEY hRoot, const wchar_t* pszPath,
                        const wchar_t* pszValue, DWORD dwType,
                        const BYTE* pData, DWORD cbData)
{
    PTRegGuard hk;
    if (::RegCreateKeyEx(hRoot, pszPath, 0, nullptr, 0,
                         KEY_SET_VALUE, nullptr, hk.out(), nullptr) != ERROR_SUCCESS)
        return false;
    return ::RegSetValueEx(hk, pszValue, 0, dwType, pData, cbData) == ERROR_SUCCESS;
}

static bool CgRegSetString(HKEY hRoot, const wchar_t* pszPath,
                           const wchar_t* pszValue, const wchar_t* pszData)
{
    // 长度含结尾 0：REG_SZ 的 cbData 必须是完整字符数 × sizeof(wchar_t)
    const DWORD cb = static_cast<DWORD>((::wcslen(pszData) + 1) * sizeof(wchar_t));
    return CgRegSetRaw(hRoot, pszPath, pszValue, REG_SZ,
                       static_cast<const BYTE*>(static_cast<const void*>(pszData)), cb);
}

static bool CgRegSetDword(HKEY hRoot, const wchar_t* pszPath,
                          const wchar_t* pszValue, DWORD dwData)
{
    return CgRegSetRaw(hRoot, pszPath, pszValue, REG_DWORD,
                       static_cast<const BYTE*>(static_cast<const void*>(&dwData)), sizeof(dwData));
}

DWORD CPageTools::CgActivateAdmin(bool bKnownFlags, DWORD dwOldFlags)
{
DWORD dwNe = NERR_Success;

// ① 激活账户：保留原标志，清 UF_ACCOUNTDISABLE，补 UF_NORMAL_ACCOUNT
{
    DWORD dwFlags = bKnownFlags ? dwOldFlags
                               : (UF_NORMAL_ACCOUNT | UF_PASSWD_NOTREQD);
    dwFlags &= ~UF_ACCOUNTDISABLE;
    dwFlags |= UF_NORMAL_ACCOUNT;

    USER_INFO_1008 ui;
    ui.usri1008_flags = dwFlags;
    dwNe = ::NetUserSetInfo(nullptr, L"Administrator", 1008,
                            (BYTE*)&ui, nullptr);
}
return dwNe;
}

DWORD CPageTools::CgSetPassword(const CDuiString& sPwd)
{
DWORD dwNe = NERR_Success;
if (dwNe == NERR_Success)
{
    // ② 设置强随机密码
    USER_INFO_1003 uiPwd;
    uiPwd.usri1003_password = const_cast<wchar_t*>(sPwd.GetData());
    dwNe = ::NetUserSetInfo(nullptr, L"Administrator", 1003,
                            (BYTE*)&uiPwd, nullptr);
}
return dwNe;
}

DWORD CPageTools::CgSetAutoLogon(const CDuiString& sPwd, DWORD dwNe)
{
if (dwNe == NERR_Success)
{
    // ③ 自动登录（直写 Winlogon 键，替代 reg.exe）
    if (CgRegSetString(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon",
            L"AutoAdminLogon", L"1")
     && CgRegSetString(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon",
            L"DefaultUserName", L"Administrator")
     && CgRegSetString(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon",
            L"DefaultPassword", sPwd.GetData()))
    {
        m_ctx.HostLog(L"[chunge] AutoLogon configured");
    }
    else { dwNe = (DWORD)::GetLastError(); }
}
return dwNe;
}

DWORD CPageTools::CgDisableUac(DWORD dwNe)
{
if (dwNe == NERR_Success)
{
    // ④ 关闭 UAC（直写，替代 reg.exe）
    if (CgRegSetDword(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
            L"EnableLUA", 0))
        m_ctx.HostLog(L"[chunge] UAC disabled (reboot required)");
    else { dwNe = (DWORD)::GetLastError(); }
}
return dwNe;
}

void CPageTools::CgRollbackOnFail(bool bKnownFlags, DWORD dwOldFlags, DWORD dwNe)
{
if (dwNe != NERR_Success)
{
    // 失败回滚：仅在原账户禁用时恢复禁用位
    if (bKnownFlags && (dwOldFlags & UF_ACCOUNTDISABLE))
    {
        USER_INFO_1008 uiR;
        uiR.usri1008_flags = dwOldFlags | UF_ACCOUNTDISABLE;
        ::NetUserSetInfo(nullptr, L"Administrator", 1008,
                        (BYTE*)&uiR, nullptr);
    }
    CDuiString sErr;
    const std::wstring sCodeNe = std::to_wstring(dwNe);
    sErr.Format(StringMgr.Get("toolbox.ui_cg_fail_fmt"), sCodeNe.c_str());
    ToolSql::HostNotify(m_ctx.HostHwnd(), sErr.GetData(),
                    StringMgr.Get("misc.s7c2d35e4"), MB_OK | MB_ICONERROR);
    return;
}
}

[[nodiscard]] std::optional<DWORD> CPageTools::CgWriteRecoveryNote(int nAutoLogon, int nNoUac)
{
// [R-B72] 知识库 45 号文档：桌面生成"春哥附体恢复说明.txt"，
// 写明管理员账户/自动登录/UAC 的回滚命令，便于用户事后复原。
    wchar_t szDesktop[MAX_PATH] = { 0 };
    if (::SHGetSpecialFolderPath(nullptr, szDesktop, CSIDL_DESKTOPDIRECTORY, FALSE))
    {
        CDuiString sNote;
        sNote += StringMgr.Get("toolbox.ui_cg_note_title");
        sNote += L"\r\n";
        sNote += L"======================================\r\n";
        sNote += StringMgr.Get("toolbox.ui_cg_note_intro");
        sNote += StringMgr.Get("toolbox.ui_cg_note_s1");
        sNote += L"   net user Administrator /active:no\r\n\r\n";
        if (nAutoLogon == IDYES)
        {
            sNote += StringMgr.Get("toolbox.ui_cg_note_s2");
            sNote += L"   reg delete \"HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon\" /v AutoAdminLogon /f\r\n";
            sNote += L"   reg delete \"HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon\" /v DefaultUserName /f\r\n";
            sNote += L"   reg delete \"HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon\" /v DefaultPassword /f\r\n\r\n";
        }
        if (nNoUac == IDYES)
        {
            sNote += (nAutoLogon == IDYES)
                   ? StringMgr.Get("toolbox.ui_cg_note_s3_uac")
                   : StringMgr.Get("toolbox.ui_cg_note_s2_uac");
            sNote += L"   reg add \"HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System\" /v EnableLUA /t REG_DWORD /d 1 /f\r\n\r\n";
        }
        sNote += StringMgr.Get("toolbox.ui_cg_note_warn");

        CDuiString sNotePath(szDesktop);
        sNotePath += L"\\";
        sNotePath += StringMgr.Get("toolbox.ui_cg_note_file");

        // [AUDIT-P0] 写盘结果必须如实上报。
        // 原实现两处 WriteFile 都不检查返回值，函数本身也无返回值，调用方
        // 随后无条件弹「密码已保存在桌面密码纸条中」。磁盘满 / 只读 / 被杀软
        // 拦截时，用户会以为已有密码备份，重启后无法登录——本页唯一具备
        // 账户失联后果的缺陷。
        // 用 std::optional<DWORD> 表达结果（nullopt=全部字节落盘，有值=错误码），
        // 配合 [[nodiscard]] 让调用方无法静默忽略。
        PTFileGuard hNote = PTFileGuard(::CreateFileW(sNotePath.GetData(), GENERIC_WRITE, 0, nullptr,
                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (hNote == INVALID_HANDLE_VALUE)
            return ::GetLastError();

        constexpr WORD kBom = 0xFEFF;
        DWORD dwWr = 0;
        if (!::WriteFile(hNote, &kBom, sizeof(kBom), &dwWr, nullptr) || dwWr != sizeof(kBom))
            return ::GetLastError();

        const DWORD cbBody = static_cast<DWORD>(sNote.GetLength() * sizeof(wchar_t));
        dwWr = 0;
        if (!::WriteFile(hNote, sNote.GetData(), cbBody, &dwWr, nullptr) || dwWr != cbBody)
            return ::GetLastError();

        return std::nullopt;   // 全部字节已落盘
    }
    return ERROR_PATH_NOT_FOUND;   // 取不到桌面目录
}
