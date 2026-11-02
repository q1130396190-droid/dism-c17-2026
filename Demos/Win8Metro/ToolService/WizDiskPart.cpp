// ============================================================
// WizDiskPart.cpp —— [R6-05] 磁盘/diskpart 原语实现。
// 逐字搬迁自 ToolDialog.cpp(2026-10-13) 139-241，仅把
// CDuiString/_T 边界换成 std::wstring/L""，逻辑零改动。
// ============================================================

#include <windows.h>
#include <fstream>   // [C17] std::ofstream（F17_C_fileio 清零）
#include <winioctl.h>             // IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS

#include <cstdio>
#include <vector>
#include <string>

#include "ProcessRunner.h"        // diskpart 进程执行（非 UI 设施）
#include "SystemInfoService.h"    // MakeTempWorkDir（非 UI 设施）
#include "WinHandles.h"           // [C++17 R11] win::HandlePtr

#include "WizDiskPart.h"

namespace toolsvc {

namespace {

// diskpart 按 OEM/ANSI 读脚本，必须把宽串转成当前代码页字节。
bool WizWriteTempFile(const std::wstring& sPath, const std::wstring& sBody)
{
    int cb = ::WideCharToMultiByte(CP_ACP, 0, sBody.c_str(), -1,
                                   nullptr, 0, nullptr, nullptr);
    if (cb <= 0) return false;
    std::vector<char> vA(cb + 2, 0);
    if (::WideCharToMultiByte(CP_ACP, 0, sBody.c_str(), -1,
                              vA.data(), cb, nullptr, nullptr) <= 0)
        return false;
    // 写入文件（ofstream 覆盖模式，F17_C_fileio 清零）
    std::ofstream ofs(sPath, std::ios::binary);
    if (!ofs) return false;
    ofs.write(vA.data(), cb - 1);
    return true;
}

} // anonymous namespace

bool WizVolumeDiskNumber(wchar_t cLetter, DWORD& pdwDisk)
{
    std::wstring sDev = L"\\\\.\\";
    sDev += cLetter;
    sDev += L":";
    win::HandlePtr h = win::file_handle(::CreateFileW(sDev.c_str(), 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr));   // [C++17 R11] auto CloseHandle
    if (!h) return false;
    BYTE abBuf[4096] = { 0 };
    DWORD cb = 0;
    BOOL b = ::DeviceIoControl(h.get(), IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
        nullptr, 0, abBuf, sizeof(abBuf), &cb, nullptr);
    h.reset();   // [C++17 R11] explicit close at original point (RAII also guards early returns)
    if (!b) return false;
    VOLUME_DISK_EXTENTS* p = (VOLUME_DISK_EXTENTS*)abBuf;
    if (p->NumberOfDiskExtents < 1) return false;
    pdwDisk = p->Extents[0].DiskNumber;
    return true;
}

bool WizRunDiskpart(const std::wstring& sBody, DWORD dwTimeout,
                    std::wstring* psOut)
{
    // 边界转换：既有设施签名是 DuiLib::CDuiString
    DuiLib::CDuiString sWork = SystemInfoService::MakeTempWorkDir(L"dp");
    DuiLib::CDuiString sScr = sWork + L"\\run.txt";
    if (!WizWriteTempFile(std::wstring(sScr.GetData()), sBody)) return false;
    DuiLib::CDuiString sArgs;
    sArgs.Format(L"/s \"%s\"", sScr.GetData());
    if (psOut)
    {
        DuiLib::CDuiString sOut;
        bool b = ProcessRunner::RunCapture(L"diskpart.exe", sArgs, sOut,
                                           dwTimeout);
        if (b) *psOut = sOut.GetData();
        return b;
    }
    return ProcessRunner::RunHiddenExit(L"diskpart.exe", sArgs,
                                        dwTimeout) == 0;
}

wchar_t WizFreeLetter()
{
    for (wchar_t c = L'Z'; c >= L'F'; --c)
    {
        std::wstring sRoot;
        sRoot += c;
        sRoot += L":\\";
        if (::GetDriveTypeW(sRoot.c_str()) == DRIVE_NO_ROOT_DIR) return c;
    }
    return 0;
}

bool WizFindEspIndex(DWORD dwDisk, int& pnPart)
{
    std::wstring sScript = L"select disk " + std::to_wstring(dwDisk) + L"\r\nlist partition\r\n";
    std::wstring sOut;
    if (!WizRunDiskpart(sScript, 60000, &sOut)) return false;

    bool bFound = false;
    const wchar_t* p = sOut.c_str();
    std::wstring sLine;
    while (p && *p)
    {
        if (*p == L'\r' || *p == L'\n')
        {
            std::wstring l = sLine;
            sLine.clear();
            ++p;
            std::wstring lc = l;
            for (size_t i = 0; i < lc.size(); ++i)
                lc[i] = (wchar_t)towlower(lc[i]);
            // Match the "System" (ESP) partition row; English output only to
            // keep this source file ASCII-safe.
            if (!bFound && lc.find(L"system") != std::wstring::npos)
            {
                const wchar_t* q = l.c_str();
                while (*q && (*q < L'0' || *q > L'9')) ++q;
                if (*q) { pnPart = _wtoi(q); bFound = true; }
            }
            continue;
        }
        sLine += *p;
        ++p;
    }
    return bFound;
}

} // namespace toolsvc
// [C++17 R3] 本文件内联常量（C++17 inline variables，多 TU ODR 一致）：
