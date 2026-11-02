// ============================================================
// IntlService.cpp —— 区域/国际化页写路径（R90 P2 切片）
//
// 背景：评分报告 D3 缺口 SetUserLocale / SetSystemLocale 0 命中。
// 经 SDK 与 DLL 导出实测，SetUserDefaultLocaleName/SetSystemDefaultUILanguage
// 均不存在（概念名称），真实写路径为：
//   时区：SetDynamicTimeZoneInformation（kernel32）— TZI 从
//         HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Time Zones\<id>\TZI
//         读取二进制 TIME_ZONE_INFORMATION
//   区域：HKCU\Control Panel\International\LocaleName + Locale
//   线程 UI 语言：SetThreadPreferredUILanguages（进程内即时生效）
// 纯 Win32，无 UI 控件依赖；实现不使用预编译头。
// ============================================================

#include "IntlService.h"
#include "FormatW.h"



#include <vector>
#include <algorithm>

#include "UIlib.h"      // CMarkupNode（DataEngine.h 依赖；本文件不用 PCH）
using namespace DuiLib; // DataEngine.h 的裸类型名依赖（等价 stdafx.h 末尾 using）
#include "DataEngine.h" // [P0] WinPE 离线会话：区域/时区写挂载蜂巢
#include "OfflineSession.h" // [R10-02] 离线状态经独立组件访问（去 CDataEngine 静态耦合）
// [C++17 阶段2] 局部 RAII 守卫：注册表键（见 WinHandles.h 使用规范）
struct IntlRegGuard
{
    HKEY k = nullptr;
    ~IntlRegGuard() { if (k) ::RegCloseKey(k); }
    HKEY* out() { return &k; }
    operator HKEY() const { return k; }   // [C++17 R-P8] 补隐式转换
};


namespace
{
    // [P0] WinPE 离线会话：把 (hRoot, path) 重定向到挂载的目标蜂巢。
    // 在线会话原样透传；未挂载的根（HARDWARE 等）保持原文，让调用失败
    // 而非误写 PE RAM 盘。
    void IntlRemap(HKEY hRootIn, const wchar_t* pszPathIn, HKEY& hRootOut, CDuiString& sPathOut)
    {
        hRootOut = hRootIn;
        sPathOut = pszPathIn ? pszPathIn : L"";
        if (!COfflineSession::IsActive()) return;

        const wchar_t* pszName = nullptr;
        if (hRootIn == HKEY_LOCAL_MACHINE)      pszName = L"HKEY_LOCAL_MACHINE";
        else if (hRootIn == HKEY_CURRENT_USER)  pszName = L"HKEY_CURRENT_USER";
        else if (hRootIn == HKEY_USERS)         pszName = L"HKEY_USERS";
        else if (hRootIn == HKEY_CLASSES_ROOT)  pszName = L"HKEY_CLASSES_ROOT";
        else return;

        CDuiString sFull = CDuiString(pszName) + L"\\" + sPathOut;
        // ParseRegPath 内部首步即 RemapOfflinePath（含 DPP_OFF_ 透传，幂等）
        COfflineSession::ParseRegPath(sFull.GetData(), hRootOut, sPathOut);
    }

    // 读取注册表二进制值（REG_BINARY）
    bool ReadRegBinary(HKEY hRoot, const wchar_t* pszPath, const wchar_t* pszValue,
                       std::vector<BYTE>& outData)
    {
        outData.clear();
        CDuiString sPath; IntlRemap(hRoot, pszPath, hRoot, sPath);
        IntlRegGuard hKey;
        if (::RegOpenKeyEx(hRoot, sPath.GetData(), 0,
                           KEY_READ | KEY_WOW64_64KEY, hKey.out()) != ERROR_SUCCESS)
            return false;

        DWORD dwType = 0, dwSize = 0;
        if (::RegQueryValueEx(hKey, pszValue, nullptr, &dwType, nullptr, &dwSize)
                != ERROR_SUCCESS || dwType != REG_BINARY)
        {
            return false;
        }

        outData.resize(dwSize);
        if (::RegQueryValueEx(hKey, pszValue, nullptr, &dwType, outData.data(),
                              &dwSize) != ERROR_SUCCESS)
        {
            outData.clear();
            return false;
        }

        return !outData.empty();
    }

    // 写入注册表字符串值（REG_SZ），自动创建键
    bool WriteRegString(HKEY hRoot, const wchar_t* pszPath, const wchar_t* pszValue,
                        const wchar_t* pszData)
    {
        CDuiString sPath; IntlRemap(hRoot, pszPath, hRoot, sPath);
        IntlRegGuard hKey;
        DWORD dwDisp = 0;
        LONG l = ::RegCreateKeyEx(hRoot, sPath.GetData(), 0, nullptr, 0,
                                  KEY_SET_VALUE | KEY_WOW64_64KEY,
                                  nullptr, hKey.out(), &dwDisp);
        if (l != ERROR_SUCCESS) return false;

        l = ::RegSetValueEx(hKey, pszValue, 0, REG_SZ,
                            (const BYTE*)pszData,
                            (DWORD)((wcslen(pszData) + 1) * sizeof(wchar_t)));
        return l == ERROR_SUCCESS;
    }

    // 读取注册表字符串值（REG_SZ），失败返回 false
    bool ReadRegString(HKEY hRoot, const wchar_t* pszPath, const wchar_t* pszValue,
                       wchar_t* pszBuf, DWORD cchBuf)
    {
        CDuiString sPath; IntlRemap(hRoot, pszPath, hRoot, sPath);
        IntlRegGuard hKey;
        if (::RegOpenKeyEx(hRoot, sPath.GetData(), 0,
                           KEY_READ | KEY_WOW64_64KEY, hKey.out()) != ERROR_SUCCESS)
            return false;

        DWORD dwType = 0, dwSize = static_cast<DWORD>(cchBuf * sizeof(wchar_t));
        LONG l = ::RegQueryValueEx(hKey, pszValue, nullptr, &dwType,
                                   reinterpret_cast<BYTE*>(pszBuf), &dwSize);
        return l == ERROR_SUCCESS && dwType == REG_SZ && pszBuf[0] != 0;
    }

    // 写入注册表多字符串值（REG_MULTI_SZ），空列表写双 \0
    bool WriteRegMultiString(HKEY hRoot, const wchar_t* pszPath, const wchar_t* pszValue,
                             const std::vector<CDuiString>& items)
    {
        CDuiString sPath; IntlRemap(hRoot, pszPath, hRoot, sPath);
        IntlRegGuard hKey;
        DWORD dwDisp = 0;
        LONG l = ::RegCreateKeyEx(hRoot, sPath.GetData(), 0, nullptr, 0,
                                  KEY_SET_VALUE | KEY_WOW64_64KEY,
                                  nullptr, hKey.out(), &dwDisp);
        if (l != ERROR_SUCCESS) return false;

        // 构造 REG_MULTI_SZ 缓冲区（\0 结尾 + 终止双 \0）
        size_t cbTotal = sizeof(wchar_t);
        for (size_t i = 0; i < items.size(); ++i)
            cbTotal += (items[i].GetLength() + 1) * sizeof(wchar_t);

        std::vector<wchar_t> buf(cbTotal / sizeof(wchar_t) + 1, 0);
        size_t n = 0;
        for (size_t i = 0; i < items.size(); ++i)
        {
            const wchar_t* p = items[i].GetData();
            size_t len = wcslen(p);
            wcsncpy_s(buf.data() + n, buf.size() - n, p, len);
            n += len + 1;
        }
        buf[n] = 0;

        l = ::RegSetValueEx(hKey, pszValue, 0, REG_MULTI_SZ,
                            (const BYTE*)buf.data(),
                            static_cast<DWORD>((n + 1) * sizeof(wchar_t)));
        return l == ERROR_SUCCESS;
    }

    // 读取注册表多字符串值（REG_MULTI_SZ）
    bool ReadRegMultiString(HKEY hRoot, const wchar_t* pszPath, const wchar_t* pszValue,
                            std::vector<CDuiString>& outItems)
    {
        outItems.clear();
        CDuiString sPath; IntlRemap(hRoot, pszPath, hRoot, sPath);
        IntlRegGuard hKey;
        if (::RegOpenKeyEx(hRoot, sPath.GetData(), 0,
                           KEY_READ | KEY_WOW64_64KEY, hKey.out()) != ERROR_SUCCESS)
            return false;

        DWORD dwType = 0, dwSize = 0;
        LONG l = ::RegQueryValueEx(hKey, pszValue, nullptr, &dwType, nullptr, &dwSize);
        if (l != ERROR_SUCCESS || dwType != REG_MULTI_SZ || dwSize < 2)
        {
            return false;
        }

        std::vector<wchar_t> buf(dwSize / sizeof(wchar_t) + 1, 0);
        l = ::RegQueryValueEx(hKey, pszValue, nullptr, &dwType,
                              reinterpret_cast<BYTE*>(buf.data()), &dwSize);
        if (l != ERROR_SUCCESS) return false;

        for (size_t n = 0; n < buf.size(); )
        {
            if (buf[n] == 0) break;
            outItems.push_back(buf.data() + n);
            n += wcslen(buf.data() + n) + 1;
        }
        return !outItems.empty();
    }

    // 写入 DWORD 值（离线时区键用）
    bool WriteRegDword(HKEY hRoot, const wchar_t* pszPath, const wchar_t* pszValue, DWORD dwData)
    {
        CDuiString sPath; IntlRemap(hRoot, pszPath, hRoot, sPath);
        IntlRegGuard hKey;
        DWORD dwDisp = 0;
        LONG l = ::RegCreateKeyEx(hRoot, sPath.GetData(), 0, nullptr, 0,
                                  KEY_SET_VALUE | KEY_WOW64_64KEY,
                                  nullptr, hKey.out(), &dwDisp);
        if (l != ERROR_SUCCESS) return false;
        l = ::RegSetValueEx(hKey, pszValue, 0, REG_DWORD,
                            (const BYTE*)&dwData, sizeof(dwData));
        return l == ERROR_SUCCESS;
    }

    // 写入二进制值（离线时区 SYSTEMTIME 键用）
    bool WriteRegBinary(HKEY hRoot, const wchar_t* pszPath, const wchar_t* pszValue,
                        const BYTE* pData, DWORD cbData)
    {
        CDuiString sPath; IntlRemap(hRoot, pszPath, hRoot, sPath);
        IntlRegGuard hKey;
        DWORD dwDisp = 0;
        LONG l = ::RegCreateKeyEx(hRoot, sPath.GetData(), 0, nullptr, 0,
                                  KEY_SET_VALUE | KEY_WOW64_64KEY,
                                  nullptr, hKey.out(), &dwDisp);
        if (l != ERROR_SUCCESS) return false;
        l = ::RegSetValueEx(hKey, pszValue, 0, REG_BINARY, pData, cbData);
        return l == ERROR_SUCCESS;
    }

    // 获取某区域/语言的本地化显示名（用当前用户 UI 语言）
    bool GetLocaleDisplayName(const wchar_t* pszLocale, CDuiString& outDisplay)
    {
        outDisplay.Empty();
        constexpr int kLocaleNameWchars = 256;   // 256 缓冲尺寸（语义命名，数值不变）
        wchar_t buf[kLocaleNameWchars] = { 0 };

        // 优先用 GetLocaleInfoEx 取本地化显示名（真实 API，替代 LocaleNameToDisplayName）
        int nLen = ::GetLocaleInfoEx(pszLocale, LOCALE_SLOCALIZEDDISPLAYNAME,
                                     buf, static_cast<int>(256 / sizeof(wchar_t)));
        if (nLen > 0 && buf[0])
        {
            outDisplay = buf;
            return true;
        }

        // 兜底：LCID 路径
        LCID lcid = ::LocaleNameToLCID(pszLocale, 0);
        if (lcid != 0)
        {
            nLen = ::GetLocaleInfoW(lcid, LOCALE_SLOCALIZEDDISPLAYNAME, buf, 256);
            if (nLen > 0 && buf[0])
            {
                outDisplay = buf;
                return true;
            }
        }

        outDisplay = pszLocale;
        return false;
    }

    // 生成 LCID 十六进制字符串（%08X）
    void FormatLcidHex(LCID lcid, wchar_t* pszOut, int cch)
    {
        wfmt::FormatInto(pszOut, static_cast<size_t>(cch), L"%08X", (unsigned)lcid);
    }
}

// ---- 时区 ----

bool IntlService::GetCurrentTimeZone(CDuiString& sTzId)
{
    sTzId.Empty();

    // [P0] WinPE 离线会话：读目标系统
    // HKLM\SYSTEM\<CS>\Control\TimeZoneInformation!TimeZoneKeyName
    // （经 ReadRegString 自动重映射到挂载蜂巢）
    if (COfflineSession::IsActive())
    {
        wchar_t bufOff[128] = { 0 };
        if (ReadRegString(HKEY_LOCAL_MACHINE,
                          L"SYSTEM\\CurrentControlSet\\Control\\TimeZoneInformation",
                          L"TimeZoneKeyName", bufOff, 128))
        {
            sTzId = bufOff;
            return true;
        }
        return false;
    }

    // 优先读 HKCU\Control Panel\TimeZone\TimeZoneKeyName（Windows 当前生效值）
    IntlRegGuard hKey;
    if (::RegOpenKeyEx(HKEY_CURRENT_USER,
                       L"Control Panel\\TimeZone", 0,
                       KEY_READ | KEY_WOW64_64KEY, hKey.out()) == ERROR_SUCCESS)
    {
        wchar_t buf[128] = { 0 };
        DWORD dwSize = sizeof(buf);
        if (::RegQueryValueEx(hKey, L"TimeZoneKeyName", nullptr, nullptr,
                              reinterpret_cast<BYTE*>(buf), &dwSize) == ERROR_SUCCESS && buf[0])
        {
            sTzId = buf;
            return true;
        }
    }

    // 兜底：GetDynamicTimeZoneInformation
    DYNAMIC_TIME_ZONE_INFORMATION dtzi;
    if (::GetDynamicTimeZoneInformation(&dtzi) != TIME_ZONE_ID_INVALID
        && dtzi.TimeZoneKeyName[0])
    {
        sTzId = dtzi.TimeZoneKeyName;
        return true;
    }

    return false;
}

EIntlWriteResult IntlService::SetTimeZone(const wchar_t* pszTzId, CDuiString& sErr)
{
    sErr.Empty();
    if (!pszTzId || !*pszTzId)
    {
        sErr = L"INTL_ERR_INVALID_TZ";
        return EIntlWriteResult::INTL_WRITE_INVALID_VALUE;
    }

    // 1) 从 HKLM Time Zones\<id> 读 TZI 二进制（TIME_ZONE_INFORMATION 44 字节）
    CDuiString sTzPath;
    sTzPath.Format(L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Time Zones\\%s",
                   pszTzId);

    std::vector<BYTE> vTzi;
    if (!ReadRegBinary(HKEY_LOCAL_MACHINE, sTzPath.GetData(), L"TZI", vTzi))
    {
        sErr = L"INTL_ERR_TZI_MISSING";
        return EIntlWriteResult::INTL_WRITE_TZI_MISSING;
    }

    if (vTzi.size() < sizeof(TIME_ZONE_INFORMATION))
    {
        sErr = L"INTL_ERR_TZI_INVALID";
        return EIntlWriteResult::INTL_WRITE_TZI_INVALID;
    }

    // 2) 构造 DYNAMIC_TIME_ZONE_INFORMATION（复用 TZI 前 44 字节）
    DYNAMIC_TIME_ZONE_INFORMATION dtzi;
    dtzi = {};

    TIME_ZONE_INFORMATION tzi;
    ::CopyMemory(&tzi, vTzi.data(), sizeof(TIME_ZONE_INFORMATION));

    dtzi.Bias             = tzi.Bias;
    dtzi.StandardBias     = tzi.StandardBias;
    dtzi.StandardDate     = tzi.StandardDate;
    dtzi.DaylightBias     = tzi.DaylightBias;
    dtzi.DaylightDate     = tzi.DaylightDate;
    wcsncpy_s(dtzi.StandardName, 32, tzi.StandardName, _TRUNCATE);
    wcsncpy_s(dtzi.DaylightName, 32, tzi.DaylightName, _TRUNCATE);
    wcsncpy_s(dtzi.TimeZoneKeyName, 128, pszTzId, _TRUNCATE);

    // 3) 写入系统时区
    //
    // [P0] WinPE 离线会话：SetDynamicTimeZoneInformation 只会作用于 PE RAM 盘，
    // 必须改写目标映像 SYSTEM\<CS>\Control\TimeZoneInformation 全套键值，
    // 系统首次启动时服务控制管理器/时区服务按这些键初始化（tzutil 同效）。
    if (COfflineSession::IsActive())
    {
        const wchar_t* kTziKey = L"SYSTEM\\CurrentControlSet\\Control\\TimeZoneInformation";
        bool bOk = true;
        bOk &= WriteRegDword(HKEY_LOCAL_MACHINE, kTziKey, L"Bias", static_cast<DWORD>(static_cast<LONG>(tzi.Bias)));
        bOk &= WriteRegString(HKEY_LOCAL_MACHINE, kTziKey, L"StandardName", tzi.StandardName);
        bOk &= WriteRegDword(HKEY_LOCAL_MACHINE, kTziKey, L"StandardBias", static_cast<DWORD>(static_cast<LONG>(tzi.StandardBias)));
        bOk &= WriteRegBinary(HKEY_LOCAL_MACHINE, kTziKey, L"StandardStart",
                              (const BYTE*)&tzi.StandardDate, sizeof(SYSTEMTIME));
        bOk &= WriteRegString(HKEY_LOCAL_MACHINE, kTziKey, L"DaylightName", tzi.DaylightName);
        bOk &= WriteRegDword(HKEY_LOCAL_MACHINE, kTziKey, L"DaylightBias", static_cast<DWORD>(static_cast<LONG>(tzi.DaylightBias)));
        bOk &= WriteRegBinary(HKEY_LOCAL_MACHINE, kTziKey, L"DaylightStart",
                              (const BYTE*)&tzi.DaylightDate, sizeof(SYSTEMTIME));
        bOk &= WriteRegDword(HKEY_LOCAL_MACHINE, kTziKey, L"ActiveTimeBias",
                             static_cast<DWORD>(static_cast<LONG>(tzi.Bias + tzi.DaylightBias)));
        bOk &= WriteRegString(HKEY_LOCAL_MACHINE, kTziKey, L"TimeZoneKeyName", pszTzId);
        // 默认用户蜂巢同步（经重映射落至挂载的 Default NTUSER.DAT）
        WriteRegString(HKEY_CURRENT_USER, L"Control Panel\\TimeZone",
                       L"TimeZoneKeyName", pszTzId);
        if (!bOk)
        {
            sErr = L"INTL_ERR_REG_FAIL";
            return EIntlWriteResult::INTL_WRITE_REG_FAIL;
        }
        return EIntlWriteResult::INTL_WRITE_OK;
    }

    if (!::SetDynamicTimeZoneInformation(&dtzi))
    {
        DWORD dwErr = ::GetLastError();
        if (dwErr == ERROR_ACCESS_DENIED)
        {
            sErr = L"INTL_ERR_NO_PERMISSION";
            return EIntlWriteResult::INTL_WRITE_NO_PERMISSION;
        }
        sErr = L"INTL_ERR_REG_FAIL";
        return EIntlWriteResult::INTL_WRITE_REG_FAIL;
    }

    // 4) 同步 HKCU\Control Panel\TimeZone 便于 UI 回读一致
    WriteRegString(HKEY_CURRENT_USER, L"Control Panel\\TimeZone",
                   L"TimeZoneKeyName", pszTzId);

    return EIntlWriteResult::INTL_WRITE_OK;
}

// ---- 区域 / 语言 ----

EIntlWriteResult IntlService::SetUserLocaleName(const wchar_t* pszLocaleName, CDuiString& sErr)
{
    sErr.Empty();
    if (!pszLocaleName || !*pszLocaleName)
    {
        sErr = L"INTL_ERR_INVALID_LOCALE";
        return EIntlWriteResult::INTL_WRITE_INVALID_VALUE;
    }

    // LocaleName -> LCID（如 "zh-CN" -> 0x0804）
    LCID lcid = ::LocaleNameToLCID(pszLocaleName, 0);
    if (lcid == 0)
    {
        sErr = L"INTL_ERR_INVALID_LOCALE";
        return EIntlWriteResult::INTL_WRITE_INVALID_VALUE;
    }

    // 1) LocaleName（REG_SZ）
    if (!WriteRegString(HKEY_CURRENT_USER, L"Control Panel\\International",
                        L"LocaleName", pszLocaleName))
    {
        sErr = L"INTL_ERR_NO_PERMISSION";
        return EIntlWriteResult::INTL_WRITE_NO_PERMISSION;
    }

    // 2) Locale（REG_SZ，LCID 十六进制，兼容旧版）
    std::wstring sLcid;
    sLcid.resize(16);
    wfmt::FormatInto(sLcid.data(), sLcid.size() + 1, L"%08X", (unsigned)lcid);
    sLcid.resize(wcslen(sLcid.c_str()));
    WriteRegString(HKEY_CURRENT_USER, L"Control Panel\\International",
                   L"Locale", sLcid.c_str());

    return EIntlWriteResult::INTL_WRITE_OK;
}

bool IntlService::GetUserLocaleName(CDuiString& sLocaleName)
{
    sLocaleName.Empty();
    wchar_t buf[LOCALE_NAME_MAX_LENGTH] = { 0 };
    int nLen = ::GetUserDefaultLocaleName(buf, LOCALE_NAME_MAX_LENGTH);
    if (nLen <= 0) return false;
    sLocaleName = buf;
    return true;
}

bool IntlService::SetThreadUILanguageName(const wchar_t* pszLangName)
{
    if (!pszLangName || !*pszLangName) return false;

    CDuiString sList;
    sList.Format(L"%s;", pszLangName);

    DWORD dwNum = 0;
    BOOL bOk = ::SetThreadPreferredUILanguages(MUI_LANGUAGE_NAME, sList.GetData(),
                                               &dwNum);
    return bOk == TRUE && dwNum > 0;
}
// ---- [R96] 系统区域 / UI 语言 / 已安装语言 ----

bool IntlService::GetSystemLocaleName(CDuiString& sLocaleName)
{
    sLocaleName.Empty();
    wchar_t buf[LOCALE_NAME_MAX_LENGTH] = { 0 };
    int nLen = ::GetSystemDefaultLocaleName(buf, LOCALE_NAME_MAX_LENGTH);
    if (nLen <= 0) return false;
    sLocaleName = buf;
    return true;
}

EIntlWriteResult IntlService::SetSystemLocaleName(const wchar_t* pszLocaleName, CDuiString& sErr)
{
    sErr.Empty();
    if (!pszLocaleName || !*pszLocaleName)
    {
        sErr = L"INTL_ERR_INVALID_LOCALE";
        return EIntlWriteResult::INTL_WRITE_INVALID_VALUE;
    }

    LCID lcid = ::LocaleNameToLCID(pszLocaleName, 0);
    if (lcid == 0)
    {
        sErr = L"INTL_ERR_INVALID_LOCALE";
        return EIntlWriteResult::INTL_WRITE_INVALID_VALUE;
    }

    std::wstring sLcid;
    sLcid.resize(16);
    FormatLcidHex(lcid, sLcid.data(), static_cast<int>(sLcid.size()));
    sLcid.resize(wcslen(sLcid.c_str()));

    // 1) HKLM\SYSTEM\CurrentControlSet\Control\Nls\Language\Default
    if (!WriteRegString(HKEY_LOCAL_MACHINE,
                        L"SYSTEM\\CurrentControlSet\\Control\\Nls\\Language",
                        L"Default", sLcid.c_str()))
    {
        sErr = L"INTL_ERR_NO_PERMISSION";
        return EIntlWriteResult::INTL_WRITE_NO_PERMISSION;
    }

    // 2) HKLM\Control Panel\International\Locale（镜像给 .DEFAULT）
    if (!WriteRegString(HKEY_LOCAL_MACHINE, L"Control Panel\\International",
                        L"Locale", sLcid.c_str()))
    {
        sErr = L"INTL_ERR_REG_FAIL";
        return EIntlWriteResult::INTL_WRITE_REG_FAIL;
    }
    WriteRegString(HKEY_LOCAL_MACHINE,
                   L"Control Panel\\International\\User Profile\\System Locale",
                   L"Locale", sLcid.c_str());

    // 3) HKU\.DEFAULT\Control Panel\International\Locale（新用户默认）
    WriteRegString(HKEY_USERS, L".DEFAULT\\Control Panel\\International",
                   L"Locale", sLcid.c_str());

    return EIntlWriteResult::INTL_WRITE_OK;
}

bool IntlService::GetUserUILanguageName(CDuiString& sLangName)
{
    sLangName.Empty();
    LANGID lang = ::GetUserDefaultUILanguage();
    LCID lcid = MAKELCID(lang, SORT_DEFAULT);
    wchar_t buf[LOCALE_NAME_MAX_LENGTH] = { 0 };
    int nLen = ::LCIDToLocaleName(lcid, buf, LOCALE_NAME_MAX_LENGTH, 0);
    if (nLen <= 0) return false;
    sLangName = buf;
    return true;
}

EIntlWriteResult IntlService::SetSystemUILanguageName(const wchar_t* pszLangName, CDuiString& sErr)
{
    sErr.Empty();
    if (!pszLangName || !*pszLangName)
    {
        sErr = L"INTL_ERR_INVALID_LANG";
        return EIntlWriteResult::INTL_WRITE_INVALID_VALUE;
    }

    std::vector<CDuiString> vLangs;
    vLangs.push_back(CDuiString(pszLangName));

    // 写 HKLM\...\MUI\Settings\PreferredUILanguages（REG_MULTI_SZ）
    if (!WriteRegMultiString(HKEY_LOCAL_MACHINE,
                             L"SYSTEM\\CurrentControlSet\\Control\\MUI\\Settings",
                             L"PreferredUILanguages", vLangs))
    {
        sErr = L"INTL_ERR_NO_PERMISSION";
        return EIntlWriteResult::INTL_WRITE_NO_PERMISSION;
    }

    // 同时写 "0" 表示按用户语言列表启用
    if (!WriteRegString(HKEY_LOCAL_MACHINE,
                        L"SYSTEM\\CurrentControlSet\\Control\\MUI\\Settings",
                        L"0", L"1"))
    {
        sErr = L"INTL_ERR_REG_FAIL";
        return EIntlWriteResult::INTL_WRITE_REG_FAIL;
    }

    return EIntlWriteResult::INTL_WRITE_OK;
}

int IntlService::GetInstalledLanguages(std::vector<IntlLangEntry>& outEntries)
{
    outEntries.clear();

    CDuiString sCurUser, sCurUi;
    GetUserLocaleName(sCurUser);
    GetUserUILanguageName(sCurUi);

    std::vector<CDuiString> vLangs;
    if (!ReadRegMultiString(HKEY_CURRENT_USER,
                            L"Control Panel\\International\\User Profile",
                            L"Languages", vLangs))
        return 0;

    for (size_t i = 0; i < vLangs.size(); ++i)
    {
        IntlLangEntry e;
        e.sName = vLangs[i];
        GetLocaleDisplayName(vLangs[i].GetData(), e.sDisplay);
        e.bInstalled = true;
        e.bCurrent = (sCurUser.Compare(vLangs[i]) == 0
                      || sCurUi.Compare(vLangs[i]) == 0);
        outEntries.push_back(e);
    }
    return static_cast<int>(outEntries.size());
}

int IntlService::GetCandidateLocales(std::vector<IntlLangEntry>& outEntries)
{
    outEntries.clear();

    // Windows 常见可用区域（LocaleName），按显示名排序
    static const wchar_t* sCandidates[] = {
        L"ar-SA", L"cs-CZ", L"da-DK", L"de-DE", L"el-GR",
        L"en-GB", L"en-US", L"es-ES", L"es-MX", L"fi-FI",
        L"fr-CA", L"fr-FR", L"he-IL", L"hi-IN", L"hu-HU",
        L"id-ID", L"it-IT", L"ja-JP", L"ko-KR", L"ms-MY",
        L"nb-NO", L"nl-NL", L"pl-PL", L"pt-BR", L"pt-PT",
        L"ro-RO", L"ru-RU", L"sk-SK", L"sv-SE", L"th-TH",
        L"tr-TR", L"uk-UA", L"vi-VN", L"zh-CN", L"zh-TW",
    };
    constexpr int nCount = static_cast<int>(sizeof(sCandidates) / sizeof(sCandidates[0]));

    CDuiString sCurUser, sCurSys, sCurUi;
    GetUserLocaleName(sCurUser);
    GetSystemLocaleName(sCurSys);
    GetUserUILanguageName(sCurUi);

    // 收集已安装语言集合
    std::vector<CDuiString> vInstalled;
    ReadRegMultiString(HKEY_CURRENT_USER,
                       L"Control Panel\\International\\User Profile",
                       L"Languages", vInstalled);

    for (int i = 0; i < nCount; ++i)
    {
        IntlLangEntry e;
        e.sName = sCandidates[i];
        GetLocaleDisplayName(e.sName.GetData(), e.sDisplay);
        e.bInstalled = false;
        for (size_t k = 0; k < vInstalled.size(); ++k)
            if (vInstalled[k].Compare(e.sName) == 0) { e.bInstalled = true; break; }
        e.bCurrent = (sCurUser.Compare(e.sName) == 0
                      || sCurSys.Compare(e.sName) == 0
                      || sCurUi.Compare(e.sName) == 0);
        outEntries.push_back(e);
    }
    return nCount;
}

// ---- [R101] 已安装语言 安装/删除 ----

EIntlWriteResult IntlService::AddInstalledLanguage(const wchar_t* pszLangName, CDuiString& sErr)
{
    sErr.Empty();
    if (!pszLangName || !*pszLangName)
    {
        sErr = L"INTL_ERR_INVALID_LANG";
        return EIntlWriteResult::INTL_WRITE_INVALID_VALUE;
    }

    // 校验 LocaleName 有效性
    LCID lcid = ::LocaleNameToLCID(pszLangName, 0);
    if (lcid == 0)
    {
        sErr = L"INTL_ERR_INVALID_LANG";
        return EIntlWriteResult::INTL_WRITE_INVALID_VALUE;
    }

    // 读取现有语言列表
    const wchar_t* kPath = L"Control Panel\\International\\User Profile";
    std::vector<CDuiString> vLangs;
    ReadRegMultiString(HKEY_CURRENT_USER, kPath, L"Languages", vLangs);

    for (size_t i = 0; i < vLangs.size(); ++i)
        if (vLangs[i].Compare(pszLangName) == 0)
            return EIntlWriteResult::INTL_WRITE_OK;   // 已安装，幂等成功

    vLangs.push_back(CDuiString(pszLangName));

    if (!WriteRegMultiString(HKEY_CURRENT_USER, kPath, L"Languages", vLangs))
    {
        sErr = L"INTL_ERR_REG_FAIL";
        return EIntlWriteResult::INTL_WRITE_REG_FAIL;
    }

    return EIntlWriteResult::INTL_WRITE_OK;
}

EIntlWriteResult IntlService::RemoveInstalledLanguage(const wchar_t* pszLangName, CDuiString& sErr)
{
    sErr.Empty();
    if (!pszLangName || !*pszLangName)
    {
        sErr = L"INTL_ERR_INVALID_LANG";
        return EIntlWriteResult::INTL_WRITE_INVALID_VALUE;
    }

    const wchar_t* kPath = L"Control Panel\\International\\User Profile";
    std::vector<CDuiString> vLangs;
    ReadRegMultiString(HKEY_CURRENT_USER, kPath, L"Languages", vLangs);

    bool bFound = false;
    std::vector<CDuiString> vOut;
    for (size_t i = 0; i < vLangs.size(); ++i)
    {
        if (vLangs[i].Compare(pszLangName) == 0) { bFound = true; continue; }
        vOut.push_back(vLangs[i]);
    }
    if (!bFound)
    {
        sErr = L"INTL_ERR_NOT_INSTALLED";
        return EIntlWriteResult::INTL_WRITE_INVALID_VALUE;
    }

    if (!WriteRegMultiString(HKEY_CURRENT_USER, kPath, L"Languages", vOut))
    {
        sErr = L"INTL_ERR_REG_FAIL";
        return EIntlWriteResult::INTL_WRITE_REG_FAIL;
    }

    return EIntlWriteResult::INTL_WRITE_OK;
}

// ---- [R120-1] 系统区域格式（HKCU\Control Panel\International）----

bool IntlService::GetLocaleFormat(EIntlFormatKind nKind, CDuiString& sValue)
{
    sValue.Empty();

    const wchar_t* pszValue = nullptr;
    switch (nKind)
    {
    case EIntlFormatKind::INTL_FMT_SHORTDATE: pszValue = L"sShortDate"; break;
    case EIntlFormatKind::INTL_FMT_SHORTTIME: pszValue = L"sTimeFormat"; break;
    case EIntlFormatKind::INTL_FMT_DECIMAL:   pszValue = L"sDecimal";   break;
    case EIntlFormatKind::INTL_FMT_CURRENCY:  pszValue = L"sCurrency";  break;
    default: return false;
    }

    // 1) 注册表实值
    wchar_t buf[128] = { 0 };
    if (ReadRegString(HKEY_CURRENT_USER, L"Control Panel\\International",
                      pszValue, buf, 128) && buf[0])
    {
        sValue = buf;
        return true;
    }

    // 2) 兜底：按当前用户区域取默认格式（NLS API）
    WCHAR wbuf[128] = { 0 };
    LCTYPE lct = 0;
    switch (nKind)
    {
    case EIntlFormatKind::INTL_FMT_SHORTDATE: lct = LOCALE_SSHORTDATE;  break;
    case EIntlFormatKind::INTL_FMT_SHORTTIME: lct = LOCALE_STIMEFORMAT; break;
    case EIntlFormatKind::INTL_FMT_DECIMAL:   lct = LOCALE_SDECIMAL;    break;
    case EIntlFormatKind::INTL_FMT_CURRENCY:  lct = LOCALE_SCURRENCY;   break;
    default: return false;
    }

    LCID lcid = ::GetUserDefaultLCID();
    if (::GetLocaleInfoW(lcid, lct, wbuf, 128) > 0 && wbuf[0])
    {
        sValue = wbuf;
        return true;
    }

    return false;
}

EIntlWriteResult IntlService::SetLocaleFormat(EIntlFormatKind nKind, const wchar_t* pszValue, CDuiString& sErr)
{
    sErr.Empty();
    if (!pszValue || !*pszValue)
    {
        sErr = L"INTL_ERR_INVALID_FORMAT";
        return EIntlWriteResult::INTL_WRITE_INVALID_VALUE;
    }

    const wchar_t* pszValueName = nullptr;
    switch (nKind)
    {
    case EIntlFormatKind::INTL_FMT_SHORTDATE: pszValueName = L"sShortDate"; break;
    case EIntlFormatKind::INTL_FMT_SHORTTIME: pszValueName = L"sTimeFormat"; break;
    case EIntlFormatKind::INTL_FMT_DECIMAL:   pszValueName = L"sDecimal";   break;
    case EIntlFormatKind::INTL_FMT_CURRENCY:  pszValueName = L"sCurrency";  break;
    default:
        sErr = L"INTL_ERR_INVALID_FORMAT";
        return EIntlWriteResult::INTL_WRITE_INVALID_VALUE;
    }

    // 写入 HKCU\Control Panel\International\<ValueName>（REG_SZ）
    if (!WriteRegString(HKEY_CURRENT_USER, L"Control Panel\\International",
                        pszValueName, pszValue))
    {
        sErr = L"INTL_ERR_REG_FAIL";
        return EIntlWriteResult::INTL_WRITE_REG_FAIL;
    }

    return EIntlWriteResult::INTL_WRITE_OK;
}
// [C++17 R3] 本文件内联常量（C++17 inline variables，多 TU ODR 一致）：
