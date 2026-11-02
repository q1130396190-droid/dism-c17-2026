// ============================================================================
//  HostExports.cpp —— 宿主 6 回调导出实现
//
//  等价原版 Dism++x64.exe 导出表中 NCleaner 反向导入的 6 个符号
//  （ord 27/28/40/46/47/59）。
//
//  实现策略（尽量原生，减少外部进程）：
//    DismRegOpenKeyEx            → RegOpenKeyExW 直调（原生）
//    DismGetSystemInfoBySession  → RtlGetVersion + GetNativeSystemInfo（原生）
//    DismWriteLog                → 追加 action_log.jsonl + Debug 输出（原生）
//    DismFreeMemory              → HeapFree/CoTaskMemFree 分派（原生）
//    DismGetAllUsersAppx         → AppxAllUserStore 注册表枚举（原生，覆盖全用户）
//    DismRemoveAppx              → 复用工程既有 Appx 移除通道（PowerShell
//                                  Remove-AppxPackage，与 AppxDeployService /
//                                  Page_Appx / Page_Cleanup 同一路线，已入
//                                  external_cmd_ledger 审计）
//
//  本层不 include UIlib.h，与 DuiLib 完全隔离（ABI 面）。
// ============================================================================

#include "HostExports.h"
#include "FormatW.h"
#include "AppxRuntimeBridge.h"   // [P0-A1] 原生 WinRT 枚举/卸载
#include "LogService.h"          // 统一日志入口（显式 UTF-8 + 互斥 + 统一目录）
#include <algorithm>             // [C++17] std::fill_n replaces ZeroMemory




#include <shlwapi.h>
#include <fstream>   // [C17] std::wofstream
#include <string>
#include <vector>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cwchar>
// [C++17 阶段2] 局部 RAII 守卫：注册表键（见 WinHandles.h 使用规范）
struct HERegGuard
{
    HKEY k = nullptr;
    ~HERegGuard() { if (k) ::RegCloseKey(k); }
    HKEY* out() { return &k; }
    operator HKEY() const { return k; }
};


#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")

namespace
{

// [L-22][P-死代码] 原 GetLogPath / MakeTimestamp 在本层已无调用者
//   （DismWriteLog 改为直接调 LogService::Write），按门禁"无消费者的死代码
//   应清除"删除，避免与 LogService 形成第二份路径/时间戳逻辑漂移。

// ---- 自包含隐藏执行 + 捕获 stdout（UTF-8/ANSI 字节） ------------------------
// 不依赖 ProcessRunner（避免引入 DuiLib 依赖）。
bool RunCaptureBytes(const wchar_t* pszCmdLine, std::string& sOut, DWORD dwWaitMs)
{
    sOut.clear();
    if (pszCmdLine == nullptr || *pszCmdLine == L'\0') return false;

    SECURITY_ATTRIBUTES sa = { 0 };
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    // [阶段2 有意保留] 管道句柄对 + 子进程句柄：与 ProcessRunner.cpp R-B83/R-B84 同构——
    // CreateProcess 后立即关 hWr、PeekNamedPipe 异步读防写满死锁、超时 TerminateProcess；
    // 时序敏感，保持显式管理不引入守卫。
    HANDLE hRd = nullptr, hWr = nullptr;
    if (!::CreatePipe(&hRd, &hWr, &sa, 0)) return false;
    ::SetHandleInformation(hRd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = { 0 };
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hWr;
    si.hStdError  = hWr;
    si.hStdInput  = nullptr;

    PROCESS_INFORMATION pi = { 0 };

    std::wstring sCmd(pszCmdLine);
    std::vector<wchar_t> vCmd(sCmd.begin(), sCmd.end());
    vCmd.push_back(L'\0');

    BOOL bOk = ::CreateProcessW(nullptr, vCmd.data(), nullptr, nullptr, TRUE,
                                CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    ::CloseHandle(hWr);

    if (!bOk) {
        ::CloseHandle(hRd);
        return false;
    }

    // 异步读避免管道写满死锁
    constexpr size_t kHostPipeBufSize = 4096;
    char szBuf[kHostPipeBufSize];
    DWORD dwRead = 0;
    DWORD dwTickStart = ::GetTickCount();

    for (;;) {
        DWORD dwAvail = 0;
        if (::PeekNamedPipe(hRd, nullptr, 0, nullptr, &dwAvail, nullptr) && dwAvail > 0) {
            if (::ReadFile(hRd, szBuf, sizeof(szBuf), &dwRead, nullptr) && dwRead > 0) {
                sOut.append(szBuf, dwRead);
                continue;
            }
            break;
        }
        DWORD dwWait = ::WaitForSingleObject(pi.hProcess, 50);
        if (dwWait == WAIT_OBJECT_0) {
            // 进程结束：把残留读完
            while (::ReadFile(hRd, szBuf, sizeof(szBuf), &dwRead, nullptr) && dwRead > 0) {
                sOut.append(szBuf, dwRead);
            }
            break;
        }
        if (dwWaitMs != INFINITE && (::GetTickCount() - dwTickStart) > dwWaitMs) {
            ::TerminateProcess(pi.hProcess, 1);
            break;
        }
    }

    ::CloseHandle(hRd);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return true;
}

// ---- 字节流 → 宽字符（自动判定 UTF-16LE / UTF-8 / ANSI） -------------------
std::wstring BytesToWide(const std::string& sBytes)
{
    if (sBytes.empty()) return std::wstring();

    // UTF-16LE BOM
    if (sBytes.size() >= 2 &&
        (unsigned char)sBytes[0] == 0xFF && (unsigned char)sBytes[1] == 0xFE) {
        const wchar_t* p = reinterpret_cast<const wchar_t*>(sBytes.data() + 2);
        size_t n = (sBytes.size() - 2) / sizeof(wchar_t);
        return std::wstring(p, n);
    }

    // 无 BOM 的 UTF-16LE 启发式：ASCII 区字符的高字节多为 0
    if (sBytes.size() >= 4) {
        size_t nZero = 0, nOdd = 0;
        size_t nCheck = sBytes.size() / 2 * 2;
        for (size_t i = 1; i < nCheck; i += 2) {
            ++nOdd;
            if (sBytes[i] == 0) ++nZero;
        }
        if (nOdd > 0 && nZero * 100 / nOdd > 60) {
            const wchar_t* p = reinterpret_cast<const wchar_t*>(sBytes.data());
            return std::wstring(p, sBytes.size() / sizeof(wchar_t));
        }
    }

    // 按 UTF-8 解码，失败回退 ANSI
    int nWide = ::MultiByteToWideChar(CP_UTF8, 0, sBytes.data(),
                                      static_cast<int>(sBytes.size()), nullptr, 0);
    if (nWide > 0) {
        std::wstring sOut(static_cast<size_t>(nWide), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, sBytes.data(), static_cast<int>(sBytes.size()),
                              &sOut[0], nWide);
        return sOut;
    }

    nWide = ::MultiByteToWideChar(CP_ACP, 0, sBytes.data(),
                                  static_cast<int>(sBytes.size()), nullptr, 0);
    if (nWide > 0) {
        std::wstring sOut(static_cast<size_t>(nWide), L'\0');
        ::MultiByteToWideChar(CP_ACP, 0, sBytes.data(), static_cast<int>(sBytes.size()),
                              &sOut[0], nWide);
        return sOut;
    }

    return std::wstring();
}

// ---- [BUG-01 修复] 损坏 Appx 登记探测 --------------------------------------
// NCCorruptedAppXOnlineCleanup 的语义是清理「损坏包」，绝不能对全量包发起
// 移除。登记表里的条目只有在物理安装位置缺失（或清单文件缺失）时才判定为
// 损坏/孤立登记：
//   * 机器安装的包物理位置默认为 %ProgramFiles%\WindowsApps\<PackageFullName>；
//   * 调用方（WinRT 回退路径）已知 InstalledLocation 时优先使用该路径。
// 目录或 AppxManifest.xml 任一缺失即视为损坏；探测失败按「健康」处理，
// 遵循「拿不准就不删」的保守策略。
bool PathExists(const std::wstring& sPath)
{
    if (sPath.empty()) return false;
    return ::GetFileAttributesW(sPath.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool IsCorruptedAppxEntry(const wchar_t* pFullName,
                          const wchar_t* pszInstallPath)
{
    if (pFullName == nullptr || *pFullName == L'\0') return false;

    std::wstring sLoc;
    if (pszInstallPath != nullptr && *pszInstallPath != L'\0')
    {
        sLoc = pszInstallPath;
    }
    else
    {
        wchar_t szPF[MAX_PATH] = { 0 };
        if (::GetEnvironmentVariableW(L"ProgramFiles", szPF,
                                      _countof(szPF)) == 0)
            return false;   // 无法定位默认安装目录 → 保守视为健康
        sLoc = szPF;
        sLoc += L"\\WindowsApps\\";
        sLoc += pFullName;
    }

    if (!PathExists(sLoc)) return true;                 // 安装目录整体缺失
    std::wstring sManifest = sLoc + L"\\AppxManifest.xml";
    if (!PathExists(sManifest)) return true;            // 清单缺失 = 残缺登记
    return false;
}

}  // namespace

// ============================================================================
//  1) DismGetAllUsersAppx（原版 ord=28）—— 枚举所有用户的 Appx 包全名
//
//  原生实现：HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Appx\
//            AppxAllUserStore\Applications 的每个子键即一个包全名
//            （系统维护的"全部用户已安装包"登记表，覆盖 provisioned +
//              per-user 安装，等价 PowerShell Get-AppxPackage -AllUsers）。
//  失败/为空时回退 PowerShell 枚举（与工程既有 Appx 通道一致）。
// ============================================================================

HOST_API int __cdecl DismGetAllUsersAppx(wchar_t* pNames, int nCap)
{
    std::vector<std::wstring> vPackages;

    // ---- 主路径：AppxAllUserStore 注册表枚举（原生） ----
    // [BUG-01 修复] 只收集「损坏/孤立登记」：物理安装目录或清单缺失的条目。
    // 健康包（目录与清单齐备）一律不得出现在结果中，否则调用方会误删整机应用。
    {
        const wchar_t* kStore =
            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Appx\\"
            L"AppxAllUserStore\\Applications";

        HERegGuard hKey;
        if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, kStore, 0,
                            KEY_READ | KEY_WOW64_64KEY, hKey.out()) == ERROR_SUCCESS) {
            for (DWORD i = 0; ; ++i) {
                wchar_t szName[MAX_PATH * 2] = { 0 };
                DWORD cchName = _countof(szName);
                LONG lRc = ::RegEnumKeyExW(hKey, i, szName, &cchName,
                                           nullptr, nullptr, nullptr, nullptr);
                if (lRc != ERROR_SUCCESS) break;
                if (szName[0] != L'\0' && IsCorruptedAppxEntry(szName, nullptr))
                    vPackages.push_back(szName);
            }
        }
    }

    // ---- 回退路径：[P0-A1] 原生 WinRT PackageManager.FindPackages ----
    // 同样按 InstalledLocation 存在性过滤，只保留损坏条目。
    if (vPackages.empty()) {
        std::vector<AppxRuntimeBridge::PkgInfo> vRt;
        std::wstring sErr;
        if (AppxRuntimeBridge::EnumeratePackages(vRt, &sErr)) {
            for (size_t i = 0; i < vRt.size(); ++i) {
                if (IsCorruptedAppxEntry(vRt[i].sFullName.c_str(),
                                         vRt[i].sInstallPath.c_str()))
                    vPackages.push_back(vRt[i].sFullName);
            }
        }
    }

    int nCount = static_cast<int>(vPackages.size());

    // 仅计数模式
    if (pNames == nullptr || nCap <= 0) return nCount;

    // 填充调用方缓冲区（每项 MAX_PATH 宽）
    int nFill = (nCount < nCap) ? nCount : nCap;
    for (int i = 0; i < nFill; ++i) {
        wchar_t* pDst = pNames + static_cast<size_t>(i) * MAX_PATH;
        std::fill_n(pDst, static_cast<size_t>(MAX_PATH), wchar_t{0});   // [C++17] replaces ZeroMemory
        wcsncpy_s(pDst, MAX_PATH, vPackages[i].c_str(), _TRUNCATE);
    }
    return nFill;
}

// ============================================================================
//  2) DismRegOpenKeyEx（原版 ord=46）—— 打开注册表键
//  原生实现：RegOpenKeyExW 直调。成功返回 ERROR_SUCCESS(0)。
// ============================================================================

HOST_API int __cdecl DismRegOpenKeyEx(HKEY hRoot, const wchar_t* pSubKey,
                                      DWORD dwSam, HKEY* phKey)
{
    if (pSubKey == nullptr || phKey == nullptr) return ERROR_INVALID_PARAMETER;
    *phKey = nullptr;
    return (int)::RegOpenKeyExW(hRoot, pSubKey, 0, dwSam, phKey);
}

// ============================================================================
//  3) DismWriteLog（原版 ord=59）—— 写日志
//  落到 EXE 同目录 action_log.jsonl（与 LogService 同格式），并 Debug 输出。
//  level 0=info / 1=warn / 2=error（与原版 DismWriteLog 分级一致）。
// ============================================================================

HOST_API void __cdecl DismWriteLog(int nLevel, const wchar_t* pText)
{
    if (pText == nullptr || *pText == L'\0') return;

    ::OutputDebugStringW(pText);
    ::OutputDebugStringW(L"\n");

    // [L-19][L-02][L-03] 不再自己开文件、自己转义：
    //   旧实现用 std::wofstream（C locale）写，中文会截断；转义只处理 \\ 与 "
    //   漏了 \n\r\t，多行文本会把 JSONL 写成多行破坏逐行解析；且此函数可由插件
    //   任意线程调用，与 UI 线程的批量写入并发 append、轮转竞态。
    //   现在统一交给 LogService::Write（显式 UTF-8 + 互斥 + 完整转义）。
    //
    //   nLevel 沿用插件 ABI 的旧语义 0=info / 1=warn / 2=error（≥3 视为更严重），
    //   必须显式映射，不能 static_cast —— 新枚举 ELogSeverity 的 kInfo 是 2，
    //   直接强转会把 info 变成 "info"、把 warn 变成 "warn" 却把 error 降级。
    ELogSeverity lv = ELogSeverity::kInfo;
    if (nLevel >= 5)      lv = ELogSeverity::kFatal;
    else if (nLevel == 4) lv = ELogSeverity::kError;
    else if (nLevel == 3) lv = ELogSeverity::kError;
    else if (nLevel == 2) lv = ELogSeverity::kError;   // 旧 ABI 的 error
    else if (nLevel == 1) lv = ELogSeverity::kWarn;    // 旧 ABI 的 warn
    else                  lv = ELogSeverity::kInfo;    // 0 及负数按 info
    LogService::Write(lv, L"plugin", pText);
}

// ============================================================================
//  4) DismGetSystemInfoBySession（原版 ord=40）—— 按会话取系统信息
//  原生实现：RtlGetVersion + GetNativeSystemInfo，返回格式化描述串。
//  返回 1 成功 / 0 失败。
// ============================================================================

HOST_API int __cdecl DismGetSystemInfoBySession(DWORD dwSession,
                                                wchar_t* pBuf, DWORD cch)
{
    if (pBuf == nullptr || cch == 0) return 0;

    // RtlGetVersion 是唯一能拿到真实 build 的公开途径
    using PFN_RtlGetVersion = LONG ( WINAPI*)(PRTL_OSVERSIONINFOW);
    RTL_OSVERSIONINFOW osvi = { 0 };
    osvi.dwOSVersionInfoSize = sizeof(osvi);

    HMODULE hNtdll = ::GetModuleHandleW(L"ntdll.dll");
    if (hNtdll != nullptr) {
        PFN_RtlGetVersion pRtl = reinterpret_cast<PFN_RtlGetVersion>(
            ::GetProcAddress(hNtdll, "RtlGetVersion"));
        if (pRtl != nullptr) pRtl(&osvi);
    }

    SYSTEM_INFO si = { 0 };
    ::GetNativeSystemInfo(&si);

    const wchar_t* pszArch = L"x86";
    if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64) pszArch = L"x64";
    else if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64) pszArch = L"ARM64";

    wchar_t szComp[MAX_COMPUTERNAME_LENGTH + 1] = { 0 };
    DWORD cchComp = _countof(szComp);
    ::GetComputerNameW(szComp, &cchComp);

    wfmt::FormatInto(pBuf, cch,
                 L"%s %u.%u.%u %s session=%lu",
                 szComp,
                 osvi.dwMajorVersion, osvi.dwMinorVersion, osvi.dwBuildNumber,
                 pszArch, (unsigned long)dwSession);
    return 1;
}

// ============================================================================
//  5) DismRemoveAppx（原版 ord=47）—— 移除 Appx 包
//  [P0-A1] 原生 WinRT PackageManager.RemovePackageAsync（同步等待结果），
//  旧 PowerShell Remove-AppxPackage -AllUsers 子进程已删除。
//  返回 HRESULT（0 = S_OK）。
// ============================================================================

HOST_API long __cdecl DismRemoveAppx(const wchar_t* pFullName)
{
    if (pFullName == nullptr || *pFullName == L'\0') return E_INVALIDARG;

    std::wstring sErr;
    if (AppxRuntimeBridge::RemovePackage(pFullName, &sErr))
        return S_OK;

    // 错误串以 "0xXXXXXXXX" 起始时透传部署 HRESULT
    if (sErr.size() > 2 && sErr[0] == L'0' &&
        (sErr[1] == L'x' || sErr[1] == L'X'))
    {
        wchar_t* pEnd = nullptr;
        unsigned long ul = wcstoul(sErr.c_str() + 2, &pEnd, 16);
        if (pEnd && pEnd != sErr.c_str() + 2) return (long)ul;
    }
    return E_FAIL;
}

// ============================================================================
//  6) DismFreeMemory（原版 ord=27）—— 释放内存
//  释放本模块分配的内存。跨模块边界严禁混用分配器，
//  故仅接受本模块 HeapAlloc/CoTaskMemAlloc 产物；其余交由调用方自理。
// ============================================================================

HOST_API void __cdecl DismFreeMemory(void* p)
{
    if (p == nullptr) return;
    ::HeapFree(::GetProcessHeap(), 0, p);
}

// [R01] 原 #7-#12 插件协议导出（DismExpandEnvironmentStrings /
//  DismFormatMessage / DismMultiLanguage / DismRegOpenKey / DismApplyDPI /
//  DismCreateInterface）随 FrogPlugins 框架整体剥离删除。
//  StringManager.cpp 中的 DismStringLookupW 桥同步删除（唯一消费者已不存在）。
// [C++17 R3] 本文件内联常量（C++17 inline variables，多 TU ODR 一致）：
