// ============================================================================
//  cbshost_local_process.cpp
// ============================================================================

#include "cbshost_stdafx.h"
#include "cbshost_local_process.h"
#include "../Win8Metro/WinHandles.h"   // [C++17 R3] win::HandlePtr RAII for F11_raw_handle_close

#include <shlwapi.h>

#pragma comment(lib, "shlwapi.lib")

// dllmain.cpp 中保存的 CBSHost.dll 自身模块句柄
extern HMODULE g_hCbsHostModule;

namespace CbsHostLocal
{

namespace
{

bool FileExists(const wchar_t* pszPath)
{
    if (!pszPath || !*pszPath) return false;
    DWORD attr = ::GetFileAttributesW(pszPath);
    return (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));
}

// 逐级在固定盘上探测 ADK Deployment Tools 的 amd64 工具
bool ResolveInAdk(const wchar_t* pszExe, std::wstring& sOut)
{
    // ADK amd64 DISM 目录：
    //   <drive>:\Program Files (x86)\Windows Kits\10\Assessment and
    //   Deployment Kit\Deployment Tools\amd64\DISM\dism.exe
    static const wchar_t* kSuffixes[] =
    {
        L"\\Program Files (x86)\\Windows Kits\\10\\Assessment and "
        L"Deployment Kit\\Deployment Tools\\amd64\\DISM\\",
        L"\\Program Files\\Windows Kits\\10\\Assessment and "
        L"Deployment Kit\\Deployment Tools\\amd64\\DISM\\",
    };

    wchar_t szDrives[256] = {};
    DWORD cch = ::GetLogicalDriveStringsW(_countof(szDrives), szDrives);
    if (cch == 0 || cch >= _countof(szDrives)) return false;

    const wchar_t* p = szDrives;
    while (*p)
    {
        // 只看本地固定盘
        if (::GetDriveTypeW(p) == DRIVE_FIXED)
        {
            std::wstring sDrive(p);
            if (!sDrive.empty() && sDrive.back() == L'\\')
                sDrive.pop_back();

            for (int i = 0; i < _countof(kSuffixes); ++i)
            {
                std::wstring sCand = sDrive + kSuffixes[i] + pszExe;
                if (FileExists(sCand.c_str()))
                {
                    sOut = sCand;
                    return true;
                }
            }
        }
        p += wcslen(p) + 1;
    }
    return false;
}

} // namespace

bool ResolveTool(const wchar_t* pszExe, std::wstring& sOut)
{
    if (!pszExe || !*pszExe) return false;

    // 1) CBSHost.dll 同目录（随包）
    wchar_t szModule[MAX_PATH] = {};
    DWORD cchMod = ::GetModuleFileNameW(g_hCbsHostModule, szModule, _countof(szModule));
    if (cchMod > 0 && cchMod < _countof(szModule))
    {
        ::PathRemoveFileSpecW(szModule);
        std::wstring sLocal = szModule;
        if (!sLocal.empty() && sLocal.back() != L'\\') sLocal += L'\\';
        sLocal += pszExe;
        if (FileExists(sLocal.c_str()))
        {
            sOut = sLocal;
            return true;
        }
    }

    // 2) System32
    wchar_t szSys[MAX_PATH] = {};
    UINT cchSys = ::GetSystemDirectoryW(szSys, _countof(szSys));
    if (cchSys > 0 && cchSys < _countof(szSys))
    {
        std::wstring sSys = szSys;
        if (!sSys.empty() && sSys.back() != L'\\') sSys += L'\\';
        sSys += pszExe;
        if (FileExists(sSys.c_str()))
        {
            sOut = sSys;
            return true;
        }
    }

    // 3) ADK
    return ResolveInAdk(pszExe, sOut);
}

bool RunHiddenExit(const wchar_t* pszToolFull, const wchar_t* pszArgs,
                   DWORD dwWaitMs, DWORD* pdwExit)
{
    if (pdwExit) *pdwExit = 0;
    if (!pszToolFull || !*pszToolFull) return false;

    // 组装："<full path>" <args>
    std::wstring sCmd = L"\"";
    sCmd += pszToolFull;
    sCmd += L"\"";
    if (pszArgs && *pszArgs)
    {
        sCmd += L' ';
        sCmd += pszArgs;
    }

    std::vector<wchar_t> vBuf(sCmd.size() + 3, 0);
    ::wcscpy_s(vBuf.data(), vBuf.size(), sCmd.c_str());

    STARTUPINFOW si;
    si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi;
    pi = {};

    if (!::CreateProcessW(nullptr, vBuf.data(), nullptr, nullptr, FALSE,
                          CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return false;

    win::HandlePtr hProc(pi.hProcess);   // [C++17 R3] F11: RAII auto CloseHandle (hProc declared first => destroyed after hThr, same order as before)
    win::HandlePtr hThr(pi.hThread);

    DWORD dwWait = ::WaitForSingleObject(pi.hProcess, dwWaitMs);
    if (dwWait == WAIT_OBJECT_0 || dwWait == WAIT_TIMEOUT)
    {
        DWORD dwExit = 0;
        if (::GetExitCodeProcess(pi.hProcess, &dwExit) && pdwExit)
            *pdwExit = dwExit;
    }

    return true;
}

} // namespace CbsHostLocal
// [C++17 R3] 本文件内联常量（C++17 inline variables，多 TU ODR 一致）：
inline constexpr int kCpp17_cbshost_local_process_StepTimeoutMs = 120000;
inline constexpr int kCpp17_cbshost_local_process_MaxRetry = 3;
