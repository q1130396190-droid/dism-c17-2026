// ============================================================
// StartupService.cpp —— S3（v2 P3b）实现
// 不使用预编译头（vcxproj 四配置 NotUsing）。
// 函数体逐字摘自 FrameWnd.cpp 的启动/服务页系统调用段，
// 仅做容器/返回值层面的剥离，逻辑零改动。
// ============================================================

#include "StartupService.h"
#include "ProcessRunner.h"   // [FIX-P1I] WaitHandleWithUiPump：服务轮询泵化



#include <vector>
#include <optional>
// [C++17 阶段2] 局部 RAII 守卫：文件/查找句柄、注册表键（见 WinHandles.h 使用规范）
struct SUFileGuard
{
    HANDLE h;
    explicit SUFileGuard(HANDLE x) : h(x) {}
    ~SUFileGuard() { if (h && h != INVALID_HANDLE_VALUE) ::CloseHandle(h); }
    SUFileGuard(const SUFileGuard&) = delete;
    SUFileGuard& operator=(const SUFileGuard&) = delete;
    operator HANDLE() const { return h; }
};

struct SUFindGuard
{
    HANDLE h;
    explicit SUFindGuard(HANDLE x) : h(x) {}
    ~SUFindGuard() { if (h && h != INVALID_HANDLE_VALUE) ::FindClose(h); }
    SUFindGuard(const SUFindGuard&) = delete;
    SUFindGuard& operator=(const SUFindGuard&) = delete;
    operator HANDLE() const { return h; }
};

struct SURegGuard
{
    HKEY k = nullptr;
    ~SURegGuard() { if (k) ::RegCloseKey(k); }
    HKEY* out() { return &k; }
    operator HKEY() const { return k; }
    explicit operator bool() const { return k != nullptr; }
};


namespace {

// [R126-2] 判断 pszName 尾部扩展名是否属于启动可执行族
//（.lnk/.exe/.com/.bat/.cmd）。零 shlwapi 依赖，与模块"纯 Win32"约定一致。
static bool StartupFileExtensionIs(const wchar_t* pszName, const wchar_t* pszExt)
{
    if (!pszName || !pszExt) return false;
    size_t nLen = ::wcslen(pszName);
    size_t nExt = ::wcslen(pszExt);
    if (nLen <= nExt) return false;
    const wchar_t* p = pszName + nLen - nExt;
    if (::lstrcmpiW(p, pszExt) != 0) return false;
    // 扩展名之前必须是 '.' 或路径分隔符，避免 "myexe" 之类误判
    const wchar_t* q = p - 1;
    return (*q == L'.' || *q == L'\\' || *q == L'/');
}

// [N07] PE 头 Machine 字段读取 + 位宽映射抽为纯函数（原 if>if>if>if>if>if 6 层压平）。
//       逐字搬迁原逻辑：e_lfanew 偏移定位 PE 签名，读 Machine，0x014C→32，
//       0x8664/0xAA64→64；任何一步失败返回 std::nullopt（外层卫语句处理）。
//       输入为已打开的 HANDLE，调用方负责句柄生命周期。
[[nodiscard]] static std::optional<int> ReadPeMachine(HANDLE hFile) noexcept
{
    if (!hFile || hFile == INVALID_HANDLE_VALUE)
        return std::nullopt;
    DWORD nRead = 0;
    WORD wDos = 0;
    if (!::ReadFile(hFile, &wDos, sizeof(wDos), &nRead, nullptr) ||
        nRead != sizeof(wDos) || wDos != 0x5A4D)
        return std::nullopt;
    DWORD dwOff = 0;
    if (!::ReadFile(hFile, &dwOff, sizeof(dwOff), &nRead, nullptr) ||
        nRead != sizeof(dwOff))
        return std::nullopt;
    LARGE_INTEGER li;
    li.QuadPart = dwOff;
    if (!::SetFilePointerEx(hFile, li, nullptr, FILE_BEGIN))
        return std::nullopt;
    DWORD dwSig = 0;
    if (!::ReadFile(hFile, &dwSig, sizeof(dwSig), &nRead, nullptr) ||
        nRead != sizeof(dwSig) || dwSig != 0x00004550)
        return std::nullopt;
    WORD wMachine = 0;
    if (!::ReadFile(hFile, &wMachine, sizeof(wMachine), &nRead, nullptr) ||
        nRead != sizeof(wMachine))
        return std::nullopt;
    // 表驱动位宽映射（等价原 if/else if/else 三态链）
    switch (wMachine)
    {
        case 0x014C: return 32;                       // IMAGE_FILE_MACHINE_I386
        case 0x8664:                                 // IMAGE_FILE_MACHINE_AMD64
        case 0xAA64: return 64;                       // ARM64 归入 64
        default:     return std::nullopt;
    }
}

// [R126-2] 解析 pszCmd 指向的真实 PE 位宽。
// 返回：0 未知 / 32 x86 / 64 x64；非 PE（如 .bat/.cmd）返回 0。
// 路径优先取引号内 / 首个空格前的令牌；存在则 ExpandEnvironmentStrings 后打开文件，
// 读 PE 头 e_lfanew + Machine 字段判断。
static int ResolveStartupArch(const wchar_t* pszCmd)
{
    if (!pszCmd || !*pszCmd) return 0;
    // 取启动命令中的第一个令牌（去掉外部引号）
    wchar_t szTok[MAX_PATH * 4] = { 0 };
    const wchar_t* p = pszCmd;
    bool bQuoted = false;
    wchar_t* d = szTok;
    size_t nCap = _countof(szTok) - 1;
    while (*p && d - szTok < static_cast<int>(nCap))
    {
        wchar_t c = *p;
        if (c == L'"') { bQuoted = !bQuoted; ++p; continue; }
        if (!bQuoted && (c == L' ' || c == L'\t')) break;
        *d++ = c;
        ++p;
    }
    *d = 0;
    if (!szTok[0]) return 0;
    wchar_t szPath[MAX_PATH] = { 0 };
    ::ExpandEnvironmentStrings(szTok, szPath, _countof(szPath));
    // 相对路径 / 无路径令牌一律不猜测位宽
    if (szPath[0] != L'\\' && !((szPath[0] >= L'A' && szPath[0] <= L'Z')
        || (szPath[0] >= L'a' && szPath[0] <= L'z')))
        return 0;
    // 排除非 PE 扩展名
    if (!StartupFileExtensionIs(szPath, L".exe")
        && !StartupFileExtensionIs(szPath, L".com"))
        return 0;
    SUFileGuard hFile = SUFileGuard(::CreateFile(szPath, GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (hFile == INVALID_HANDLE_VALUE) return 0;
    // [N07] 位宽判定抽为 ReadPeMachine 纯函数，原 6 层 if 链压平为单层卫语句
    auto nArch = ReadPeMachine(hFile);
    return nArch ? *nArch : 0;
}

// [N08] 启动文件夹单条目填充抽为独立函数（原 do>if>if>if>if>if 6 层压平）。
//       逐字搬迁原逻辑，hApp 可为 nullptr（组策略来源无 StartupApproved）。
static void StartupAppendFolderEntry(StartupSrcType nSrcType, HKEY hApp,
                                     const wchar_t* szDir,
                                     const WIN32_FIND_DATA& fd,
                                     std::vector<StartupEntry>& vOut)
{
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return;
    bool bLnk = StartupFileExtensionIs(fd.cFileName, L".lnk");
    bool bExe = StartupFileExtensionIs(fd.cFileName, L".exe")
             || StartupFileExtensionIs(fd.cFileName, L".com")
             || StartupFileExtensionIs(fd.cFileName, L".bat")
             || StartupFileExtensionIs(fd.cFileName, L".cmd");
    if (!bLnk && !bExe) return;
    // StartupFolder 值名为文件名（如 app.lnk）
    bool bEnabled = true;
    if (hApp)
    {
        BYTE abBin[12] = { 0 };
        DWORD nBin = sizeof(abBin);
        if (::RegQueryValueEx(hApp, fd.cFileName, nullptr, nullptr,
                              abBin, &nBin) == ERROR_SUCCESS)
            bEnabled = abBin[0] != 2;
    }
    StartupEntry en;
    en.nScope = nSrcType == StartupSrcType::SRC_STARTUP_FOLDER_USER ? 0 : 1;
    en.nSrcType = bExe && nSrcType == StartupSrcType::SRC_STARTUP_FOLDER_USER
        ? StartupSrcType::SRC_STARTUP_FOLDER_USER_EXE : nSrcType;
    en.sName = fd.cFileName;
    en.sLocation = szDir;
    DuiLib::CDuiString sFull = szDir;
    sFull += L"\\";
    sFull += fd.cFileName;
    en.sCommand = sFull;
    en.bEnabled = bEnabled;
    en.nArch = ResolveStartupArch(sFull.GetData());
    vOut.push_back(en);
}

// [N09] 服务恢复策略首条 SC_ACTION_RESTART 的 Delay 提取抽为独立函数（原
//       if>if>if>for>if 压平），单位毫秒 → 秒；失败保持 nRestartDelay 不变。
static void StartupQueryRestartDelay(SC_HANDLE hSvc, int& nRestartDelay)
{
    DWORD nFa = 0;
    ::QueryServiceConfig2W(hSvc, SERVICE_CONFIG_FAILURE_ACTIONS,
                           nullptr, 0, &nFa);
    if (nFa < sizeof(SERVICE_FAILURE_ACTIONSW))
        return;
    std::vector<BYTE> vFa(nFa);
    LPSERVICE_FAILURE_ACTIONSW pFa =
        reinterpret_cast<LPSERVICE_FAILURE_ACTIONSW>(vFa.data());
    if (!::QueryServiceConfig2W(hSvc, SERVICE_CONFIG_FAILURE_ACTIONS,
                                vFa.data(), nFa, &nFa)
        || pFa->cActions == 0 || !pFa->lpsaActions)
        return;
    for (DWORD a = 0; a < pFa->cActions; ++a)
    {
        if (pFa->lpsaActions[a].Type == SC_ACTION_RESTART)
        {
            nRestartDelay = static_cast<int>(pFa->lpsaActions[a].Delay / 1000);
            break;
        }
    }
}

// [C4] 轮询服务直到进入目标状态（或超时）。STOP_PENDING/START_PENDING 期间持续等待。
// [FIX-P1I] 裸 Sleep(200) 轮询在 UI 线程最长 20s（重启≈40s）不泵消息，
// 窗口 5s 即被判"未响应"。改用白名单泵轮询：期间绘制保持、输入丢弃，
// 窗口不再挂起判定（契约与 RunHiddenExit 同源）。
static bool WaitSvcState(SC_HANDLE hSvc, DWORD dwWant, DWORD dwTimeoutMs)
{
    DWORD dwWaited = 0;
    while (dwWaited <= dwTimeoutMs)
    {
        SERVICE_STATUS ss = { 0 };
        if (!::QueryServiceStatus(hSvc, &ss)) return false;
        if (ss.dwCurrentState == dwWant) return true;
        ProcessRunner::WaitHandleWithUiPump(nullptr, 200);
        dwWaited += 200;
    }
    return false;
}

} // namespace

// 原 FrameWnd.cpp 内 static StartupFindRunKey（逐字搬迁）
bool StartupService::FindRunKey(StartupSrcType nSrc, HKEY& hRootOut,
                                DuiLib::CDuiString& sPathOut, REGSAM& samOut)
{
    switch (nSrc)
    {
    case StartupSrcType::SRC_HKCU_RUN:
        hRootOut = HKEY_CURRENT_USER;
        sPathOut = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run";
        samOut = 0; return true;
    case StartupSrcType::SRC_HKCU_RUNONCE:
        hRootOut = HKEY_CURRENT_USER;
        sPathOut = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\RunOnce";
        samOut = 0; return true;
    case StartupSrcType::SRC_HKLM_RUN:
        hRootOut = HKEY_LOCAL_MACHINE;
        sPathOut = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run";
        samOut = KEY_WOW64_64KEY; return true;
    case StartupSrcType::SRC_HKLM_RUNONCE:
        hRootOut = HKEY_LOCAL_MACHINE;
        sPathOut = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\RunOnce";
        samOut = KEY_WOW64_64KEY; return true;
    case StartupSrcType::SRC_HKLM_WOW_RUN:
        hRootOut = HKEY_LOCAL_MACHINE;
        sPathOut = L"SOFTWARE\\Wow6432Node\\Microsoft\\Windows\\CurrentVersion\\Run";
        samOut = KEY_WOW64_32KEY; return true;
    case StartupSrcType::SRC_HKLM_WOW_RUNONCE:
        hRootOut = HKEY_LOCAL_MACHINE;
        sPathOut = L"SOFTWARE\\Wow6432Node\\Microsoft\\Windows\\CurrentVersion\\RunOnce";
        samOut = KEY_WOW64_32KEY; return true;
    case StartupSrcType::SRC_HKU_DEFAULT_RUN:
        hRootOut = HKEY_USERS;
        sPathOut = L".DEFAULT\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run";
        samOut = 0; return true;
    case StartupSrcType::SRC_HKU_DEFAULT_RUNONCE:
        hRootOut = HKEY_USERS;
        sPathOut = L".DEFAULT\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\RunOnce";
        samOut = 0; return true;
    case StartupSrcType::SRC_GPOLICY_RUN:
        // [R126-2] 组策略 Run 键：HKLM 策略 Run（管理员可通过组策略注入的启动项）
        hRootOut = HKEY_LOCAL_MACHINE;
        sPathOut = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer\\Run";
        samOut = KEY_WOW64_64KEY; return true;
    default:
        return false;
    }
}

// 原 BuildStartupPage 的 11 源枚举段（defs 表去掉只用于 UI 标题的
// pszTitle/pszScope 两列，枚举循环体逐字搬迁；m_startup -> vOut）
void StartupService::EnumerateStartup(std::vector<StartupEntry>& vOut)
{
    struct StartupSourceDef
    {
        StartupSrcType nSrcType;
        HKEY    hRoot;
        const wchar_t* pszRunKey;    // 来源键路径；文件夹来源时为环境变量模板
        REGSAM  samRun;       // 来源键附加注册表视图
        const wchar_t* pszApproved;  // StartupApproved 子键，nullptr=不读取
        REGSAM  samApp;       // StartupApproved 附加注册表视图
    };
    const StartupSourceDef defs[] = {
        { StartupSrcType::SRC_HKCU_RUN, HKEY_CURRENT_USER,
          L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
          L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run", 0 },
        { StartupSrcType::SRC_HKLM_RUN, HKEY_LOCAL_MACHINE,
          L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", KEY_WOW64_64KEY,
          L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run", KEY_WOW64_64KEY },
        { StartupSrcType::SRC_HKCU_RUNONCE, HKEY_CURRENT_USER,
          L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\RunOnce", 0,
          L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\RunOnce", 0 },
        { StartupSrcType::SRC_HKLM_RUNONCE, HKEY_LOCAL_MACHINE,
          L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\RunOnce", KEY_WOW64_64KEY,
          L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\RunOnce", KEY_WOW64_64KEY },
        { StartupSrcType::SRC_HKLM_WOW_RUN, HKEY_LOCAL_MACHINE,
          L"SOFTWARE\\Wow6432Node\\Microsoft\\Windows\\CurrentVersion\\Run", KEY_WOW64_32KEY,
          // 32位 Run 的启停状态仍在 64位视图的 StartupApproved\Run
          L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run", KEY_WOW64_64KEY },
        { StartupSrcType::SRC_HKLM_WOW_RUNONCE, HKEY_LOCAL_MACHINE,
          L"SOFTWARE\\Wow6432Node\\Microsoft\\Windows\\CurrentVersion\\RunOnce", KEY_WOW64_32KEY,
          L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\RunOnce", KEY_WOW64_64KEY },
        { StartupSrcType::SRC_HKU_DEFAULT_RUN, HKEY_USERS,
          L".DEFAULT\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
          L".DEFAULT\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run", 0 },
        { StartupSrcType::SRC_HKU_DEFAULT_RUNONCE, HKEY_USERS,
          L".DEFAULT\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\RunOnce", 0,
          L".DEFAULT\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\RunOnce", 0 },
        { StartupSrcType::SRC_STARTUP_FOLDER_USER, HKEY_CURRENT_USER,
          L"%AppData%\\Microsoft\\Windows\\Start Menu\\Programs\\Startup", 0,
          L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\StartupFolder", 0 },
        { StartupSrcType::SRC_STARTUP_FOLDER_COMMON, HKEY_LOCAL_MACHINE,
          L"%ProgramData%\\Microsoft\\Windows\\Start Menu\\Programs\\Startup", 0,
          L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\StartupFolder", KEY_WOW64_64KEY },
        { StartupSrcType::SRC_BOOT_EXECUTE, HKEY_LOCAL_MACHINE,
          L"SYSTEM\\CurrentControlSet\\Control\\Session Manager", KEY_WOW64_64KEY,
          nullptr, 0 },
        // [R126-2] 组策略 Run 键：管理员可通过组策略注入的启动项。
        // 该键无对应 StartupApproved（组策略项不受启停位控制），pszApproved=nullptr。
        { StartupSrcType::SRC_GPOLICY_RUN, HKEY_LOCAL_MACHINE,
          L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer\\Run",
          KEY_WOW64_64KEY, nullptr, 0 }
    };
    for (size_t d = 0; d < _countof(defs); ++d)
    {
        const StartupSourceDef& def = defs[d];
        if (def.nSrcType == StartupSrcType::SRC_STARTUP_FOLDER_USER
            || def.nSrcType == StartupSrcType::SRC_STARTUP_FOLDER_COMMON)
        {
            // 启动文件夹：展开环境变量后枚举 *.lnk
            // [R126-2] 追加非 .lnk 可执行文件（.exe/.com/.bat/.cmd），
            //          以 StartupSrcType::SRC_STARTUP_FOLDER_USER_EXE 归入"用户启动文件夹"分类展示。
            wchar_t szDir[MAX_PATH] = { 0 };
            if (!::ExpandEnvironmentStrings(def.pszRunKey, szDir, MAX_PATH))
                continue;
            SURegGuard hApp;
            if (def.pszApproved)
                ::RegOpenKeyEx(def.hRoot, def.pszApproved, 0,
                               KEY_READ | def.samApp, hApp.out());
            DuiLib::CDuiString sFind = szDir;
            sFind += L"\\*";
            WIN32_FIND_DATA fd = { 0 };
            SUFindGuard hFind(::FindFirstFile(sFind.GetData(), &fd));
            if (hFind != INVALID_HANDLE_VALUE)
            {
                do
                {
                    // [N08] 启动文件夹条目填充抽为独立函数压平 6 层嵌套
                    StartupAppendFolderEntry(def.nSrcType, hApp, szDir, fd, vOut);
                }
                while (::FindNextFile(hFind, &fd));
            }
        }
        else if (def.nSrcType == StartupSrcType::SRC_BOOT_EXECUTE)
        {
            // BootExecute：读取 REG_MULTI_SZ，每行一条
            SURegGuard hKey;
            if (::RegOpenKeyEx(def.hRoot, def.pszRunKey, 0,
                               KEY_READ | def.samRun, hKey.out()) == ERROR_SUCCESS)
            {
                constexpr int kMultiSzBufWchars = 2048;   // 2048 缓冲尺寸（语义命名，数值不变）
                wchar_t szMulti[kMultiSzBufWchars] = { 0 };
                DWORD nBytes = sizeof(szMulti);
                if (::RegQueryValueEx(hKey, L"BootExecute", nullptr, nullptr,
                                      reinterpret_cast<BYTE*>(szMulti), &nBytes) == ERROR_SUCCESS)
                {
                    for (const wchar_t* p = szMulti; *p; p += ::lstrlen(p) + 1)
                    {
                        StartupEntry en;
                        en.nScope = 1;
                        en.nSrcType = StartupSrcType::SRC_BOOT_EXECUTE;
                        en.sName = p;
                        en.sCommand = p;
                        en.sLocation = def.pszRunKey;
                        en.bEnabled = true;
                        en.nArch = ResolveStartupArch(p);
                        vOut.push_back(en);
                    }
                }
            }
        }
        else
        {
            // 注册表 Run / RunOnce
            SURegGuard hRun;
            if (::RegOpenKeyEx(def.hRoot, def.pszRunKey, 0,
                               KEY_READ | def.samRun, hRun.out()) != ERROR_SUCCESS)
                continue;
            SURegGuard hApp;
            if (def.pszApproved)
                ::RegOpenKeyEx(def.hRoot, def.pszApproved, 0,
                               KEY_READ | def.samApp, hApp.out());
            DWORD dwEnum = 0;
            for (;;)
            {
                constexpr int kRegValNameWchars = 256;   // 256 缓冲尺寸（语义命名，数值不变）
                wchar_t szName[kRegValNameWchars] = { 0 };
                DWORD nNameLen = kRegValNameWchars;
                constexpr int kRegValWchars = 1024;   // 1024 缓冲尺寸（语义命名，数值不变）
                wchar_t szData[kRegValWchars] = { 0 };
                DWORD nDataLen = sizeof(szData);
                DWORD dwType = 0;
                LONG rc = ::RegEnumValue(hRun, dwEnum++, szName, &nNameLen, nullptr,
                                         &dwType, reinterpret_cast<BYTE*>(szData), &nDataLen);
                if (rc == ERROR_NO_MORE_ITEMS) break;
                if (rc != ERROR_SUCCESS) continue;
                if (dwType != REG_SZ && dwType != REG_EXPAND_SZ) continue;
                bool bEnabled = true;
                if (hApp)
                {
                    BYTE abBin[12] = { 0 };
                    DWORD nBin = sizeof(abBin);
                    if (::RegQueryValueEx(hApp, szName, nullptr, nullptr,
                                          abBin, &nBin) == ERROR_SUCCESS)
                        bEnabled = abBin[0] != 2;
                }
                StartupEntry en;
                en.nScope = def.hRoot == HKEY_CURRENT_USER ? 0
                         : def.hRoot == HKEY_LOCAL_MACHINE ? 1 : 2;
                en.nSrcType = def.nSrcType;
                en.sName = szName;
                en.sCommand = szData;
                en.sLocation = def.pszRunKey;
                en.bEnabled = bEnabled;
                en.nArch = ResolveStartupArch(szData);
                vOut.push_back(en);
            }
        }
    }
}

// 原 BuildServicesPage 的 SCM 枚举段（逐字搬迁；m_services -> vOut）
// [R117-1] 重构：按服务类型多次 EnumServicesStatusEx，
//          补齐驱动服务（SERVICE_KERNEL_DRIVER / SERVICE_FILE_SYSTEM_DRIVER /
//          SERVICE_RECOGNIZER_DRIVER），D1 枚举范围从 Win32 服务扩展到 Win32+驱动。
int StartupService::EnumerateServices(std::vector<ServiceEntry>& vOut)
{
    // [R117-1] 枚举类型表：Win32 服务 + 三类驱动服务。
    static const DWORD s_aTypes[] = {
        SERVICE_WIN32,                    // 用户态服务（含 SERVICE_WIN32_OWN_PROCESS/SHARE_PROCESS）
        SERVICE_KERNEL_DRIVER,            // 内核驱动
        SERVICE_FILE_SYSTEM_DRIVER,       // 文件系统驱动
        SERVICE_RECOGNIZER_DRIVER,        // 识别器驱动
    };
    // [R117-1] 服务类型值（EnumServicesStatusEx 的 dwServiceType 即本值）。
    // 用于回填 ServiceEntry::nSvcType，与 s_aTypes 一一对应。
    static const int s_aSvcTypes[] = {
        SERVICE_WIN32,
        SERVICE_KERNEL_DRIVER,
        SERVICE_FILE_SYSTEM_DRIVER,
        SERVICE_RECOGNIZER_DRIVER,
    };

    SC_HANDLE hScm = ::OpenSCManager(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE | SC_MANAGER_CONNECT);
    if (!hScm)
        return 1;

    bool bAnyFail = false;
    DWORD nWin32Count = 0;

    for (size_t t = 0; t < _countof(s_aTypes); ++t)
    {
        DWORD nBytes = 0, nCount = 0, nResume = 0;
        ::EnumServicesStatusEx(hScm, SC_ENUM_PROCESS_INFO, s_aTypes[t], SERVICE_STATE_ALL,
                               nullptr, 0, &nBytes, &nCount, &nResume, nullptr);
        if (nBytes == 0)
        {
            // 该类型无命中（Win32 类型无命中视为致命，其余类型允许为空）
            if (s_aTypes[t] == SERVICE_WIN32)
            {
                ::CloseServiceHandle(hScm);
                return 2;
            }
            continue;
        }
        std::vector<BYTE> vBuf(nBytes);
        if (!::EnumServicesStatusEx(hScm, SC_ENUM_PROCESS_INFO, s_aTypes[t], SERVICE_STATE_ALL,
                                    vBuf.data(), nBytes, &nBytes, &nCount, &nResume, nullptr))
        {
            bAnyFail = true;
            continue;
        }
        ENUM_SERVICE_STATUS_PROCESS* pSvc =
            reinterpret_cast<ENUM_SERVICE_STATUS_PROCESS*>(vBuf.data());
        for (DWORD i = 0; i < nCount; ++i)
        {
            ServiceEntry se;
            se.sName = pSvc[i].lpServiceName;
            se.sDisplayName = pSvc[i].lpDisplayName;
            se.bRunning = pSvc[i].ServiceStatusProcess.dwCurrentState == SERVICE_RUNNING;
            // [R131-2] D3：保留 SCM 原始状态（ACTIVE/INACTIVE 全量细分）
            se.nCurrentState = pSvc[i].ServiceStatusProcess.dwCurrentState;
            se.nSvcType = s_aSvcTypes[t];
            se.nStartType = -1;   // [C4] 查询失败时如实显示"未知"，不再伪装成"手动"
            SC_HANDLE hSvc = ::OpenService(hScm, pSvc[i].lpServiceName, SERVICE_QUERY_CONFIG);
            if (hSvc)
            {
                DWORD nCfg = 0;
                ::QueryServiceConfig(hSvc, nullptr, 0, &nCfg);
                if (nCfg)
                {
                    std::vector<BYTE> vCfg(nCfg);
                    LPQUERY_SERVICE_CONFIG pCfg =
                        reinterpret_cast<LPQUERY_SERVICE_CONFIG>(vCfg.data());
                    if (::QueryServiceConfig(hSvc, pCfg, nCfg, &nCfg))
                        se.nStartType = pCfg->dwStartType;
                }
                // [R131-2] D3：QueryServiceConfig2 读服务恢复策略
                // （SERVICE_CONFIG_FAILURE_ACTIONS），取首条 SC_ACTION_RESTART 的 Delay。
                // 需 SERVICE_QUERY_CONFIG + SERVICE_QUERY_STATUS 权限，失败不阻断。
                // [N09] 恢复策略解析抽为 StartupQueryRestartDelay 压平 for>if>if 嵌套
                StartupQueryRestartDelay(hSvc, se.nRestartDelay);
                ::CloseServiceHandle(hSvc);   // 保持原句柄配对，关闭服务句柄
            }
            if (s_aTypes[t] == SERVICE_WIN32) ++nWin32Count;
            vOut.push_back(se);
        }
    }

    ::CloseServiceHandle(hScm);

    // Win32 主枚举空 → 致命；否则只要 Win32 非空即成功，
    // 驱动枚举失败不阻断（降级为仅 Win32）。
    if (nWin32Count == 0)
        return 3;
    if (bAnyFail)
        return 0;   // 部分类型失败仍算成功，调用方日志记录
    return 0;
}

// ============================================================
// [R93] EnumDependentServices：删除/停止服务前的依赖提醒
// ============================================================
int StartupService::EnumerateDependentServices(const wchar_t* pszName,
                                               std::vector<DepServiceEntry>& vOut,
                                               DWORD& dwErrOut)
{
    dwErrOut = 0;
    vOut.clear();

    SC_HANDLE hScm = ::OpenSCManager(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE | SC_MANAGER_CONNECT);
    if (!hScm) { dwErrOut = ::GetLastError(); return 1; }

    SC_HANDLE hSvc = ::OpenService(hScm, pszName, SERVICE_ENUMERATE_DEPENDENTS);
    if (!hSvc)
    {
        dwErrOut = ::GetLastError();
        ::CloseServiceHandle(hScm);
        return 2;
    }

    // 第一遍取字节数
    DWORD nBytes = 0, nCount = 0;
    BOOL bOk = ::EnumDependentServices(hSvc, SERVICE_STATE_ALL, nullptr, 0, &nBytes, &nCount);
    if (!bOk && nBytes == 0)
    {
        // 无依赖：nCount 应为 0，属正常，不算失败
        dwErrOut = ::GetLastError();
        ::CloseServiceHandle(hSvc);
        ::CloseServiceHandle(hScm);
        if (dwErrOut == ERROR_MORE_DATA || dwErrOut == ERROR_INSUFFICIENT_BUFFER)
            dwErrOut = 0;
        return 3;
    }

    std::vector<BYTE> vBuf(nBytes);
    ENUM_SERVICE_STATUS* pDep = reinterpret_cast<ENUM_SERVICE_STATUS*>(vBuf.data());
    bOk = ::EnumDependentServices(hSvc, SERVICE_STATE_ALL, pDep, nBytes, &nBytes, &nCount);
    if (!bOk)
    {
        dwErrOut = ::GetLastError();
        ::CloseServiceHandle(hSvc);
        ::CloseServiceHandle(hScm);
        return 4;
    }

    for (DWORD i = 0; i < nCount; ++i)
    {
        DepServiceEntry e;
        e.sName = pDep[i].lpServiceName;
        e.sDisplayName = pDep[i].lpDisplayName;
        e.nDependType = static_cast<int>(pDep[i].ServiceStatus.dwServiceType);
        e.bRunning = (pDep[i].ServiceStatus.dwCurrentState == SERVICE_RUNNING)
                  || (pDep[i].ServiceStatus.dwCurrentState == SERVICE_START_PENDING);
        vOut.push_back(e);
    }

    ::CloseServiceHandle(hSvc);
    ::CloseServiceHandle(hScm);
    return 0;
}

// 原 OnStartupToggle 的 StartupApproved 写入段（switch 逐字搬迁）
int StartupService::SetStartupApproved(const StartupEntry& e, bool bEnabled)
{
    // 按来源确定 StartupApproved 的 root / 子键 / 注册表视图
    HKEY hRoot = HKEY_CURRENT_USER;
    const wchar_t* pszApproved =
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
    REGSAM samExtra = 0;
    switch (e.nSrcType)
    {
    case StartupSrcType::SRC_HKCU_RUN:
        hRoot = HKEY_CURRENT_USER;
        pszApproved = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
        samExtra = 0;
        break;
    case StartupSrcType::SRC_HKLM_RUN:
        hRoot = HKEY_LOCAL_MACHINE;
        pszApproved = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
        samExtra = KEY_WOW64_64KEY;
        break;
    case StartupSrcType::SRC_HKCU_RUNONCE:
        hRoot = HKEY_CURRENT_USER;
        pszApproved = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\RunOnce";
        samExtra = 0;
        break;
    case StartupSrcType::SRC_HKLM_RUNONCE:
        hRoot = HKEY_LOCAL_MACHINE;
        pszApproved = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\RunOnce";
        samExtra = KEY_WOW64_64KEY;
        break;
    case StartupSrcType::SRC_HKLM_WOW_RUN:
        // 32位 Run 的启停状态仍写 64位视图的 StartupApproved\Run
        hRoot = HKEY_LOCAL_MACHINE;
        pszApproved = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
        samExtra = KEY_WOW64_64KEY;
        break;
    case StartupSrcType::SRC_HKLM_WOW_RUNONCE:
        hRoot = HKEY_LOCAL_MACHINE;
        pszApproved = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\RunOnce";
        samExtra = KEY_WOW64_64KEY;
        break;
    case StartupSrcType::SRC_HKU_DEFAULT_RUN:
        hRoot = HKEY_USERS;
        pszApproved = L".DEFAULT\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
        samExtra = 0;
        break;
    case StartupSrcType::SRC_GPOLICY_RUN:
        // [R126-2] 组策略 Run 项：启停位与用户 Run 共用 HKCU StartupApproved\Run
        //（与任务管理器对组策略启动项的开关行为一致）。
        hRoot = HKEY_CURRENT_USER;
        pszApproved = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
        samExtra = 0;
        break;
    case StartupSrcType::SRC_STARTUP_FOLDER_USER_EXE:
        // [R126-2] 用户启动文件夹非 .lnk 可执行文件：启停位同启动文件夹。
        hRoot = HKEY_CURRENT_USER;
        pszApproved = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\StartupFolder";
        samExtra = 0;
        break;
    case StartupSrcType::SRC_HKU_DEFAULT_RUNONCE:
        hRoot = HKEY_USERS;
        pszApproved = L".DEFAULT\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\RunOnce";
        samExtra = 0;
        break;
    case StartupSrcType::SRC_STARTUP_FOLDER_USER:
        // 用户启动文件夹：root=HKCU，值名为文件名（如 app.lnk）
        hRoot = HKEY_CURRENT_USER;
        pszApproved = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\StartupFolder";
        samExtra = 0;
        break;
    case StartupSrcType::SRC_STARTUP_FOLDER_COMMON:
        // 公共启动文件夹：root=HKLM
        hRoot = HKEY_LOCAL_MACHINE;
        pszApproved = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\StartupFolder";
        samExtra = KEY_WOW64_64KEY;
        break;
    }
    SURegGuard hKey;
    DWORD dwDisp = 0;
    REGSAM sam = KEY_READ | KEY_WRITE | samExtra;
    LONG rc = ::RegCreateKeyEx(hRoot, pszApproved,
        0, nullptr, REG_OPTION_NON_VOLATILE, sam, nullptr, hKey.out(), &dwDisp);
    if (rc != ERROR_SUCCESS)
        return 1;
    // 12 字节二进制：首字节 2=禁用 / 3=启用（沿用现有格式）
    BYTE abBin[12] = { 0 };
    abBin[0] = bEnabled ? 3 : 2;
    rc = ::RegSetValueEx(hKey, e.sName.GetData(), 0, REG_BINARY, abBin, sizeof(abBin));
    return rc == ERROR_SUCCESS ? 0 : 2;
}

// 原 DeleteStartupEntry 的系统操作段（启动文件夹 / BootExecute / Run 值）
// [REFACTOR] 前置声明（定义在文件末尾）
static bool RebuildBootExecuteWithout(HKEY hRoot, std::vector<BYTE>& vBuf,
                                      const DuiLib::CDuiString& sCmd);

int StartupService::DeleteStartupEntry(const StartupEntry& e)
{
    int nResult = 1;   // 通用失败（调用方保留原错误文案）
    if (e.nSrcType == StartupSrcType::SRC_STARTUP_FOLDER_USER || e.nSrcType == StartupSrcType::SRC_STARTUP_FOLDER_COMMON
        || e.nSrcType == StartupSrcType::SRC_STARTUP_FOLDER_USER_EXE)
    {
        // 启动文件夹：删除 .lnk / .exe 等文件本身（若在列表里）
        // [R126-2] StartupSrcType::SRC_STARTUP_FOLDER_USER_EXE 同属用户启动文件夹，走文件删除。
        if (!e.sCommand.IsEmpty() && ::DeleteFile(e.sCommand.GetData()))
            nResult = 0;
        else
            nResult = 2;
    }
    else if (e.nSrcType == StartupSrcType::SRC_BOOT_EXECUTE)
    {
        // BootExecute：REG_MULTI_SZ，删掉整行
        SURegGuard hRoot;
        const wchar_t* pszSub = L"SYSTEM\\CurrentControlSet\\Control\\Session Manager";
        if (::RegOpenKeyEx(HKEY_LOCAL_MACHINE, pszSub, 0,
                           KEY_READ | KEY_WRITE | KEY_WOW64_64KEY, hRoot.out()) == ERROR_SUCCESS)
        {
            DWORD nBytes = 0;
            ::RegQueryValueEx(hRoot, L"BootExecute", nullptr, nullptr, nullptr, &nBytes);
            if (nBytes > 0)
            {
                std::vector<BYTE> vBuf(nBytes + 2, 0);
                if (::RegQueryValueEx(hRoot, L"BootExecute", nullptr, nullptr,
                                      vBuf.data(), &nBytes) == ERROR_SUCCESS)
                {
                    if (!RebuildBootExecuteWithout(hRoot, vBuf, e.sCommand))
                        nResult = 4;
                    else nResult = 0;
                }
            }
        }
        else nResult = 3;
    }
    else
    {
        // Run / RunOnce：删除值
        HKEY hRoot = nullptr;
        DuiLib::CDuiString sPath;
        REGSAM samExtra = 0;
        if (FindRunKey(e.nSrcType, hRoot, sPath, samExtra))
        {
            SURegGuard hKey;
            LONG rc = ::RegOpenKeyEx(hRoot, sPath.GetData(), 0,
                KEY_READ | KEY_WRITE | samExtra, hKey.out());
            if (rc == ERROR_SUCCESS)
            {
                if (::RegDeleteValue(hKey, e.sName.GetData()) == ERROR_SUCCESS)
                    nResult = 0;
                else nResult = 6;
            }
            else nResult = 5;
        }
    }
    return nResult;
}

// 原 AddStartupItem 的注册表写入段（值名生成逐字搬迁）
int StartupService::AddCurrentUserRunItem(const wchar_t* pszFile, DuiLib::CDuiString& sNameOut)
{
    // 获取当前用户启动项注册表路径
    DuiLib::CDuiString sRegPath = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run";
    SURegGuard hKey;
    LONG lRet = ::RegOpenKeyExW(HKEY_CURRENT_USER, sRegPath.GetData(), 0, KEY_SET_VALUE, hKey.out());
    if (lRet != ERROR_SUCCESS)
        return 1;
    // 生成任务名称（取文件名不带扩展名）
    DuiLib::CDuiString sName = pszFile;
    const wchar_t* pSlash = wcsrchr(pszFile, L'\\');
    if (pSlash) sName = pSlash + 1;
    const wchar_t* pDot = wcsrchr(sName.GetData(), L'.');
    if (pDot) sName = DuiLib::CDuiString(sName.GetData(), static_cast<int>(pDot - sName.GetData()));
    sNameOut = sName;
    // 写入注册表
    DuiLib::CDuiString sFile = pszFile;
    int nDataLen = (sFile.GetLength() + 1) * sizeof(wchar_t);
    LONG lRet2 = ::RegSetValueExW(hKey, sName.GetData(), 0, REG_SZ,
                                  (const BYTE*)pszFile, nDataLen);
    return lRet2 == ERROR_SUCCESS ? 0 : 2;
}

// 原 OnServiceStartType 的 SCM 写入段
int StartupService::SetServiceStartType(const wchar_t* pszName, DWORD dwStart, DWORD& dwErrOut)
{
    SC_HANDLE hScm = ::OpenSCManager(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!hScm)
        return 1;
    SC_HANDLE hSvc = ::OpenService(hScm, pszName, SERVICE_CHANGE_CONFIG);
    BOOL bOk = FALSE;
    if (hSvc)
        bOk = ::ChangeServiceConfig(hSvc, SERVICE_NO_CHANGE, dwStart, SERVICE_NO_CHANGE,
                                    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    else
        dwErrOut = ::GetLastError();
    if (hSvc) ::CloseServiceHandle(hSvc);
    ::CloseServiceHandle(hScm);
    if (!bOk)
    {
        if (hSvc) dwErrOut = ::GetLastError();
        return 2;
    }
    return 0;
}

// [REFACTOR] 从 ControlServiceItem 提取：启动服务并映射错误码（2 = 启动失败）。
// 返回 0 表示成功（服务已在运行也视同成功）；*pdwErr 仅在返回非 0 时被写入。
// 错误码取值与原内联分支逐字一致，调用方仍按原样写入 code/dwErr。
static int SvcTryStart(SC_HANDLE hSvc, DWORD* pdwErr)
{
    if (!::StartService(hSvc, 0, nullptr))
    {
        const DWORD dwLE = ::GetLastError();
        if (dwLE != ERROR_SERVICE_ALREADY_RUNNING)
        {
            if (pdwErr) *pdwErr = dwLE;
            return 2;
        }
    }
    return 0;
}

// [REFACTOR] 从 ControlServiceItem 提取：停止服务并等待进入 STOPPED。
// 返回 0 成功；3 = ControlService 失败（*pdwErr = GetLastError）；
// 4 = 等待 STOPPED 超时（不写 *pdwErr，与原实现一致）。
static int SvcTryStop(SC_HANDLE hSvc, DWORD* pdwErr)
{
    SERVICE_STATUS ss = { 0 };
    if (!::ControlService(hSvc, SERVICE_CONTROL_STOP, &ss))
    {
        const DWORD dwLE = ::GetLastError();
        if (dwLE != ERROR_SERVICE_NOT_ACTIVE)
        {
            if (pdwErr) *pdwErr = dwLE;
            return 3;
        }
    }
    if (!WaitSvcState(hSvc, SERVICE_STOPPED, 20000))
        return 4;
    return 0;
}

// [REFACTOR] 从 ControlServiceItem 提取：暂停/继续服务并等待目标态。
// bPause=true 走 SERVICE_CONTROL_PAUSE（8/10），false 走 CONTINUE（9/11）。
// 返回值与 *pdwErr 的写法与原内联分支逐字一致。
static int SvcTryPauseResume(SC_HANDLE hSvc, bool bPause, DWORD* pdwErr)
{
    SERVICE_STATUS ss = { 0 };
    const DWORD dwCtrl = bPause ? SERVICE_CONTROL_PAUSE : SERVICE_CONTROL_CONTINUE;
    if (!::ControlService(hSvc, dwCtrl, &ss))
    {
        const DWORD dwLE = ::GetLastError();
        if (dwLE != ERROR_SERVICE_NOT_ACTIVE)
        {
            if (pdwErr) *pdwErr = dwLE;
            return bPause ? 8 : 9;
        }
    }
    if (!WaitSvcState(hSvc, bPause ? SERVICE_PAUSED : SERVICE_RUNNING, 20000))
        return bPause ? 10 : 11;
    return 0;
}

// 原 ShowServiceContextMenu 的 SCM 操作段（含 WaitSvcState 20 秒轮询）
StartupService::SvcCtlResult StartupService::ControlServiceItem(const wchar_t* pszName, int iCmd)
{
    SvcCtlResult r;
    SC_HANDLE hScm = ::OpenSCManager(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!hScm)
    {
        r.code = 1;
        return r;
    }
    SC_HANDLE hSvc = ::OpenService(hScm, pszName,
        SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS);
    int   code = 0;
    DWORD dwErr = 0;
    DWORD dwWant = 0;
    bool  bWantRun = false;
    if (hSvc && iCmd == 4401)
    {
        code = SvcTryStart(hSvc, &dwErr);
        if (code == 0) { dwWant = SERVICE_RUNNING; bWantRun = true; }
    }
    else if (hSvc && (iCmd == 4402 || iCmd == 4403))
    {
        code = SvcTryStop(hSvc, &dwErr);
        // 重启（4403）：停止成功后再启动；失败码沿用原 5
        if (code == 0 && iCmd == 4403)
            code = SvcTryStart(hSvc, &dwErr);
        if (code == 0)
        {
            dwWant = (iCmd == 4403) ? SERVICE_RUNNING : SERVICE_STOPPED;
            bWantRun = (iCmd == 4403);
        }
    }
    else if (hSvc && (iCmd == 4404 || iCmd == 4405))
    {
        // [R80] 暂停 / 继续。仅服务支持暂停（接受 SERVICE_CONTROL_PAUSE）时有效。
        const bool bPause = (iCmd == 4404);
        code = SvcTryPauseResume(hSvc, bPause, &dwErr);
        if (code == 0)
        {
            dwWant = bPause ? SERVICE_PAUSED : SERVICE_RUNNING;
            bWantRun = (iCmd == 4405);
        }
    }
    else if (!hSvc)
    {
        code = 7;
        dwErr = ::GetLastError();
    }
    if (hSvc && code == 0)
    {
        bool bOk = WaitSvcState(hSvc, dwWant, 20000);
        if (!bOk)
            code = 6;
    }
    if (hSvc) ::CloseServiceHandle(hSvc);
    ::CloseServiceHandle(hScm);
    r.code = code;
    r.dwErr = dwErr;
    r.bWantRun = (code == 0) && bWantRun;
    return r;
}

// [R80] 删除服务。先由调用方停止服务（ControlServiceItem 4402），再执行删除。
int StartupService::DeleteServiceItem(const wchar_t* pszName, DWORD& dwErrOut)
{
    dwErrOut = 0;
    SC_HANDLE hScm = ::OpenSCManager(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!hScm) { dwErrOut = ::GetLastError(); return 1; }

    SC_HANDLE hSvc = ::OpenService(hScm, pszName, DELETE);
    if (!hSvc)
    {
        dwErrOut = ::GetLastError();
        ::CloseServiceHandle(hScm);
        return 2;
    }
    BOOL bOk = ::DeleteService(hSvc);
    if (!bOk) dwErrOut = ::GetLastError();
    ::CloseServiceHandle(hSvc);
    ::CloseServiceHandle(hScm);
    return bOk ? 0 : 3;
}
// [C++17 R3] 本文件内联常量（C++17 inline variables，多 TU ODR 一致）：

// [REFACTOR] 从 DeleteStartupEntry 提取：过滤指定命令行后重建 BootExecute MULTI_SZ。
// 逐字搬迁零逻辑改动，仅把深层嵌套块提出为独立函数。
// 返回 true 表示写回成功。
static bool RebuildBootExecuteWithout(HKEY hRoot, std::vector<BYTE>& vBuf,
                                      const DuiLib::CDuiString& sCmd)
{
    // 重建 MULTI_SZ 输出缓冲（每行命令通常远小于 4KB）
    constexpr size_t kBootExecuteOutBufWchars = 4096;
    std::vector<wchar_t> vOut(kBootExecuteOutBufWchars, 0);
    wchar_t* p = reinterpret_cast<wchar_t*>(vBuf.data());
    wchar_t* d = vOut.data();
    while (*p)
    {
        DuiLib::CDuiString sLine = p;
        if (sLine.Compare(sCmd) != 0)
        {
            wmemcpy_s(d, vOut.data() + vOut.size() - d, p, wcslen(p) + 1);
            d += wcslen(p) + 1;
        }
        p += wcslen(p) + 1;
    }
    vOut[0] = 0; vOut[1] = 0;   // 双 null 结尾
    return ::RegSetValueEx(hRoot, L"BootExecute", 0, REG_MULTI_SZ,
                           reinterpret_cast<BYTE*>(vOut.data()),
                           static_cast<DWORD>((d - vOut.data()) * sizeof(wchar_t))) == ERROR_SUCCESS;
}
