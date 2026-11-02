#include "stdafx.h"
#include "OfflineSession.h"
#include "WinHandles.h"
#include "ProcessRunner.h"   // [FIX-P2J] WaitHandleWithUiPump：卸载重试泵化

// ============================================================
// OfflineSession.cpp —— [R10-02] 逐字搬迁自 DataEngine_Reg.cpp 的
// WinPE 离线蜂巢会话实现（原 CDataEngine 静态成员 + 文件匿名
// namespace 辅助），逻辑零改动，仅收口为独立类。
// ============================================================

namespace {
    // [R19-FIX] std::atomic<bool>：UI 线程读 IsActive() 与 Begin/End 写并发时
    // 避免数据竞争（UB）。Begin/End 内还有注册表操作，atomic 只保证标志位安全。
    std::atomic<bool> g_bOffSess{ false };
    CDuiString g_sOffRoot;        // "D:\" 或 "D:\Mount\Win"
    CDuiString g_sOffCS;          // "ControlSet001"

    bool g_bMntSW = false, g_bMntSYS = false, g_bMntSAM = false,
         g_bMntSEC = false, g_bMntDEF = false, g_bMntNTU = false;

    const wchar_t* kMntSW  = L"DPP_OFF_SW";
    const wchar_t* kMntSYS = L"DPP_OFF_SYS";
    const wchar_t* kMntSAM = L"DPP_OFF_SAM";
    const wchar_t* kMntSEC = L"DPP_OFF_SEC";
    const wchar_t* kMntDEF = L"DPP_OFF_DEF";
    const wchar_t* kMntNTU = L"DPP_OFF_NTU";

    CDuiString OffJoin(const wchar_t* pszRoot, const wchar_t* pszRel)
    {
        CDuiString r(pszRoot ? pszRoot : L"");
        if (!r.IsEmpty() && r[r.GetLength() - 1] != L'\\') r += L'\\';
        r += pszRel;
        return r;
    }

    // 加载蜂巢；若挂载点已存在（同会话/异常残留）且可打开，视为可用。
    bool OffLoad(HKEY hUnder, const wchar_t* pszName, const wchar_t* pszFile)
    {
        if (::GetFileAttributes(pszFile) == INVALID_FILE_ATTRIBUTES) return false;
        LONG lr = ::RegLoadKeyW(hUnder, pszName, pszFile);
        if (lr == ERROR_SUCCESS) return true;
        win::RegKey hT;   // [C++17 R11] local hive probe key, auto RegCloseKey
        if (::RegOpenKeyEx(hUnder, pszName, 0, KEY_READ, hT.out()) == ERROR_SUCCESS)
        { return true; }
        return false;
    }

    void OffUnload(HKEY hUnder, const wchar_t* pszName)
    {
        // 残留句柄会导致卸载失败；重试若干轮，PE 会话结束时随注册表消失亦可接受。
        // [FIX-P2J] 裸 Sleep 重试改白名单泵：调用方可能在 UI 线程（会话卸载
        // 路径），最坏 6 蜂巢 ×6 轮 ×120ms≈4.3s 期间保持绘制；工作线程调用
        // 时泵无消息可派发，行为退化为 Sleep，两种场景均安全。
        for (int i = 0; i < 6; ++i)
        {
            if (::RegUnLoadKeyW(hUnder, pszName) == ERROR_SUCCESS) return;
            ProcessRunner::WaitHandleWithUiPump(nullptr, 120);
        }
    }

    // s 形如 "prefix\\rest"（prefix 大小写不敏感）匹配时剥离并返回 rest。
    bool OffStrip(const CDuiString& s, const wchar_t* pszPrefix, CDuiString& rest)
    {
        size_t n = wcslen(pszPrefix);
        if (static_cast<size_t>(s.GetLength()) >= n
            && _wcsnicmp(s.GetData(), pszPrefix, static_cast<int>(n)) == 0
            && (s.GetLength() == n || s[static_cast<int>(n)] == L'\\'))
        {
            rest = (s.GetLength() > n) ? s.Mid(static_cast<int>(n) + 1) : CDuiString(L"");
            return true;
        }
        return false;
    }

    // 逻辑根 + rest 组合为挂载点全路径。
    CDuiString OffUnder(HKEY hRoot, const wchar_t* pszMount)
    {
        CDuiString r;
        if (hRoot == HKEY_LOCAL_MACHINE) r = L"HKEY_LOCAL_MACHINE\\";
        else if (hRoot == HKEY_USERS)   r = L"HKEY_USERS\\";
        else                            r = L"HKEY_LOCAL_MACHINE\\";
        r += pszMount;
        return r;
    }

    CDuiString OffAppend(CDuiString base, const CDuiString& rest)
    {
        if (!rest.IsEmpty()) { base += L'\\'; base += rest; }
        return base;
    }
}

bool COfflineSession::Begin(const wchar_t* pszImageRoot)
{
    if (g_bOffSess) End();
    if (!pszImageRoot || !*pszImageRoot) return false;
    if (::GetFileAttributes(OffJoin(pszImageRoot, L"Windows\\System32\\ntoskrnl.exe").GetData())
        == INVALID_FILE_ATTRIBUTES)
        return false;   // 不是已安装 Windows 根

    const CDuiString sCfg = OffJoin(pszImageRoot, L"Windows\\System32\\config");

    g_bMntSW  = OffLoad(HKEY_LOCAL_MACHINE, kMntSW,  OffJoin(sCfg, L"SOFTWARE").GetData());
    g_bMntSYS = OffLoad(HKEY_LOCAL_MACHINE, kMntSYS, OffJoin(sCfg, L"SYSTEM").GetData());
    if (!g_bMntSW || !g_bMntSYS)
    {
        // 核心蜂巢缺一不可；已挂的部分回滚
        if (g_bMntSW)  OffUnload(HKEY_LOCAL_MACHINE, kMntSW);
        if (g_bMntSYS) OffUnload(HKEY_LOCAL_MACHINE, kMntSYS);
        return false;
    }
    // 以下为增强蜂巢，缺失不阻断（部分规则读写会得到"键不存在"，语义正确）
    g_bMntSAM = OffLoad(HKEY_LOCAL_MACHINE, kMntSAM, OffJoin(sCfg, L"SAM").GetData());
    g_bMntSEC = OffLoad(HKEY_LOCAL_MACHINE, kMntSEC, OffJoin(sCfg, L"SECURITY").GetData());
    g_bMntDEF = OffLoad(HKEY_USERS, kMntDEF, OffJoin(sCfg, L"DEFAULT").GetData());

    // Default 用户 NTUSER.DAT（HKCU 的离线对应；系统优化"当前用户"作用域）
    CDuiString sNt = OffJoin(pszImageRoot, L"Users\\Default\\NTUSER.DAT");
    g_bMntNTU = OffLoad(HKEY_USERS, kMntNTU, sNt.GetData());
    if (!g_bMntNTU)
        sNt = OffJoin(pszImageRoot, L"Documents and Settings\\Default User\\NTUSER.DAT");
    if (!g_bMntNTU)
        g_bMntNTU = OffLoad(HKEY_USERS, kMntNTU, sNt.GetData());

    // 取目标系统当前 ControlSet。
    // [R10-02] 原 CDataEngine::OpenRegPath 单点此时会话尚未置位（g_bOffSess
    // 在函数末尾才置 true），等价直连：HKLM64 下 "DPP_OFF_SYS\Select"。
    g_sOffCS = L"ControlSet001";
    win::RegKey hSel;   // [C++17 R11] local Select key, auto RegCloseKey
    if (::RegOpenKeyEx(HKEY_LOCAL_MACHINE,
                       (OffUnder(HKEY_LOCAL_MACHINE, kMntSYS) + L"\\Select").GetData(),
                       0, KEY_READ | KEY_WOW64_64KEY, hSel.out()) == ERROR_SUCCESS)
    {
        DWORD dwCur = 0, cb = sizeof(dwCur), dwType = 0;
        if (::RegQueryValueEx(hSel.get(), L"Current", nullptr, &dwType,
                              (BYTE*)&dwCur, &cb) == ERROR_SUCCESS && dwCur >= 1)
            g_sOffCS.Format(L"ControlSet%03u", dwCur);
    }

    g_sOffRoot = pszImageRoot;
    g_bOffSess = true;
    return true;
}

void COfflineSession::End()
{
    if (!g_bOffSess) return;
    g_bOffSess = false;
    if (g_bMntNTU) { OffUnload(HKEY_USERS, kMntNTU); g_bMntNTU = false; }
    if (g_bMntDEF) { OffUnload(HKEY_USERS, kMntDEF); g_bMntDEF = false; }
    if (g_bMntSEC) { OffUnload(HKEY_LOCAL_MACHINE, kMntSEC); g_bMntSEC = false; }
    if (g_bMntSAM) { OffUnload(HKEY_LOCAL_MACHINE, kMntSAM); g_bMntSAM = false; }
    if (g_bMntSYS) { OffUnload(HKEY_LOCAL_MACHINE, kMntSYS); g_bMntSYS = false; }
    if (g_bMntSW)  { OffUnload(HKEY_LOCAL_MACHINE, kMntSW);  g_bMntSW = false; }
    g_sOffRoot.Empty();
    g_sOffCS = L"ControlSet001";
}

bool COfflineSession::IsActive() { return g_bOffSess; }

// [FIX] 读取目标镜像自身版本，而不是宿主 osver::Current()。
// 离线会话已把目标 SOFTWARE 挂到 HKLM\DPP_OFF_SW，这里直读该蜂巢。
bool COfflineSession::GetTargetVersion(unsigned int& nMajor, unsigned int& nMinor, unsigned int& nBuild)
{
    nMajor = nMinor = nBuild = 0;
    if (!g_bOffSess) return false;
    win::RegKey hKey;
    if (::RegOpenKeyEx(HKEY_LOCAL_MACHINE,
            L"DPP_OFF_SW\\Microsoft\\Windows NT\\CurrentVersion",
            0, KEY_READ | KEY_WOW64_64KEY, hKey.out()) != ERROR_SUCCESS)
        return false;
    auto ReadDword = [&](const wchar_t* name, unsigned int& out) {
        DWORD v = 0, cb = sizeof(v), type = 0;
        if (::RegQueryValueEx(hKey.get(), name, nullptr, &type, (BYTE*)&v, &cb) != ERROR_SUCCESS
            || type != REG_DWORD) return false;
        out = v;
        return true;
    };
    unsigned int a = 0, b = 0;
    ReadDword(L"CurrentMajorVersionNumber", a);
    ReadDword(L"CurrentMinorVersionNumber", b);
    wchar_t szBuild[32] = { 0 };
    DWORD cb = sizeof(szBuild) - sizeof(wchar_t), type = 0;
    unsigned int c = 0;
    if (::RegQueryValueEx(hKey.get(), L"CurrentBuildNumber", nullptr, &type,
                          (BYTE*)szBuild, &cb) == ERROR_SUCCESS)
        c = static_cast<unsigned int>(_wtoi(szBuild));
    if (a == 0 && b == 0 && c == 0) return false;
    nMajor = a; nMinor = b; nBuild = c;
    return true;
}

unsigned int COfflineSession::GetTargetBuild()
{
    unsigned int a = 0, b = 0, c = 0;
    return GetTargetVersion(a, b, c) ? c : 0u;
}
CDuiString COfflineSession::GetImageRoot() { return g_sOffRoot; }

CDuiString COfflineSession::RemapPath(const wchar_t* pszFull, bool bWow32)
{
    CDuiString s(pszFull ? pszFull : L"");
    if (!g_bOffSess || s.IsEmpty()) return s;

    // 已是挂载点路径：透传，绝不二次重写
    if (s.Find(L"DPP_OFF_") >= 0) return s;

    CDuiString rest;

    // ---------------- HKCU -> 挂载的 Default NTUSER ----------------
    if (OffStrip(s, L"HKEY_CURRENT_USER", rest))
    {
        if (!g_bMntNTU) return s;   // 蜂巢未挂：保持原文（调用将失败而非误写 PE）
        return OffAppend(OffUnder(HKEY_USERS, kMntNTU), rest);
    }

    // ---------------- HKU ----------------
    if (OffStrip(s, L"HKEY_USERS", rest))
    {
        CDuiString sub;
        if (OffStrip(rest, L".DEFAULT", sub))
        {
            if (!g_bMntDEF) return s;
            return OffAppend(OffUnder(HKEY_USERS, kMntDEF), sub);
        }
        return s;   // 其它 SID 的用户蜂巢未挂载
    }

    // ---------------- HKCR -> SOFTWARE\Classes ----------------
    if (OffStrip(s, L"HKEY_CLASSES_ROOT", rest))
    {
        CDuiString base = OffUnder(HKEY_LOCAL_MACHINE, kMntSW);
        base += bWow32 ? L"\\Wow6432Node\\Classes" : L"\\Classes";
        return OffAppend(base, rest);
    }

    // ---------------- HKCC -> SYSTEM\<CS>\Hardware Profiles\Current ----------------
    if (OffStrip(s, L"HKEY_CURRENT_CONFIG", rest))
    {
        CDuiString base = OffUnder(HKEY_LOCAL_MACHINE, kMntSYS);
        base += L'\\'; base += g_sOffCS;
        base += L"\\Hardware Profiles\\Current";
        return OffAppend(base, rest);
    }

    if (OffStrip(s, L"HKEY_LOCAL_MACHINE", rest))
    {
        CDuiString sub;

        // SOFTWARE
        if (OffStrip(rest, L"SOFTWARE", sub))
        {
            CDuiString base = OffUnder(HKEY_LOCAL_MACHINE, kMntSW);
            // Wow64：显式 32 位视图且子路径未自带 Wow6432Node 时补入
            bool bAlreadyWow = (OffStrip(sub, L"Wow6432Node", sub) == true);
            if (bWow32 && !bAlreadyWow) base += L"\\Wow6432Node";
            return OffAppend(base, sub);
        }
        // SYSTEM：CurrentControlSet\<CS> 或直接 ControlSet00N
        if (OffStrip(rest, L"SYSTEM", sub))
        {
            CDuiString tail;
            CDuiString base = OffUnder(HKEY_LOCAL_MACHINE, kMntSYS);
            if (OffStrip(sub, L"CurrentControlSet", tail))
            {
                base += L'\\'; base += g_sOffCS;
                return OffAppend(base, tail);
            }
            if (_wcsnicmp(sub.GetData(), L"ControlSet", 10) == 0)
                return OffAppend(base, sub);
            return OffAppend(base, sub);   // Select / WPA / MountedDevices 等原样落位
        }
        if (OffStrip(rest, L"SAM", sub))
        {
            if (!g_bMntSAM) return s;
            return OffAppend(OffUnder(HKEY_LOCAL_MACHINE, kMntSAM), sub);
        }
        // HKLM\Control Panel 是 HKU\.DEFAULT\Control Panel 的映射键
        // （系统区域等镜像写此处），离线落到挂载的 DEFAULT 蜂巢并保留
        // "Control Panel\" 前缀。
        if (OffStrip(rest, L"Control Panel", sub))
        {
            if (!g_bMntDEF) return s;
            return OffAppend(OffUnder(HKEY_USERS, kMntDEF),
                             CDuiString(L"Control Panel\\") + sub);
        }
        if (OffStrip(rest, L"SECURITY", sub))
        {
            if (!g_bMntSEC) return s;
            return OffAppend(OffUnder(HKEY_LOCAL_MACHINE, kMntSEC), sub);
        }
        // HARDWARE / BCD00000000 等蜂巢离线未挂载：保持原文（在线不存在即失败）
        return s;
    }

    return s;
}

bool COfflineSession::ParseRegPath(const wchar_t* pszFull, HKEY& hRoot, CDuiString& strSubKey)
{
    hRoot = nullptr;
    if (!pszFull) return false;

    // [修复 P1-DEF] 原版 Data.xml 中"新用户/默认用户"蜂巢 261 处写作
    // HKEY_USERS\DEFAULT（无点），但真实注册表蜂巢名是 .DEFAULT。原版引擎
    // 在路径解析处做别名归一化；此前复刻照原样把 "DEFAULT\..." 传给
    // RegOpenKeyEx/RegCreateKeyEx，在线必然失败、离线无法映射到 DPP_OFF_DEF，
    // 导致 Default 列动作全失败、RegExist 条件恒假（Not 包裹时还会反相）。
    // 归一化放在 RemapPath 之前，使离线分支的 .DEFAULT OffStrip 同时受益。
    CDuiString sNorm(pszFull);
    if (_wcsnicmp(pszFull, L"HKEY_USERS\\", 11) == 0)
    {
        const wchar_t* pszRest = pszFull + 11;
        if (_wcsnicmp(pszRest, L"DEFAULT", 7) == 0
            && (pszRest[7] == L'\\' || pszRest[7] == 0))
        {
            sNorm = L"HKEY_USERS\\.DEFAULT";
            sNorm += (pszRest + 7);
        }
    }

    // [P0] 离线会话：默认按 64 位视图重映射；32 位视图由 OpenRegKey 在调
    // ParseRegPath 前自行重映射（含 DPP_OFF_ 的路径在此透传）。
    CDuiString sUse = RemapPath(sNorm, false);
    pszFull = sUse.GetData();

    struct { const wchar_t* psz; HKEY h; } roots[] = {
        { L"HKEY_CURRENT_USER",   HKEY_CURRENT_USER },
        { L"HKEY_LOCAL_MACHINE",  HKEY_LOCAL_MACHINE },
        { L"HKEY_CLASSES_ROOT",   HKEY_CLASSES_ROOT },
        { L"HKEY_USERS",          HKEY_USERS },
        { L"HKEY_CURRENT_CONFIG", HKEY_CURRENT_CONFIG }
    };

    for (int i = 0; i < 5; ++i)
    {
        size_t n = wcslen(roots[i].psz);
        if (_wcsnicmp(pszFull, roots[i].psz, static_cast<int>(n)) == 0
            && (pszFull[n] == L'\\' || pszFull[n] == 0))
        {
            hRoot = roots[i].h;
            strSubKey = (pszFull[n] == L'\\') ? (pszFull + n + 1) : L"";
            return true;
        }
    }
    return false;
}
// [C++17 R3] 本文件内联常量（C++17 inline variables，多 TU ODR 一致）：
