#include "Registry.h"
#include "WinHandles.h"   // [H-51] win::RegKey RAII for F11_raw_handle_close


using namespace DuiLib;

CDuiString RegGetString(HKEY hRoot, const wchar_t* pszPath, const wchar_t* pszValue,
                        REGSAM samDesired)
{
    CDuiString strOut;
    win::RegKey hKey;
    if (::RegOpenKeyEx(hRoot, pszPath, 0, samDesired, hKey.out()) == ERROR_SUCCESS)
    {
        DWORD dwType = 0;
        constexpr int kRegValWchars = 1024;   // 1024 缓冲尺寸（语义命名，数值不变）
        wchar_t szBuf[kRegValWchars] = { 0 };
        DWORD dwSize = sizeof(szBuf);
        if (::RegQueryValueEx(hKey.get(), pszValue, nullptr, &dwType,
                              reinterpret_cast<BYTE*>(szBuf), &dwSize) == ERROR_SUCCESS
            && (dwType == REG_SZ || dwType == REG_EXPAND_SZ))
        {
            strOut = szBuf;
        }
        
    }
    return strOut;
}
// [C++17 R3] 本文件内联常量（C++17 inline variables，多 TU ODR 一致）：
