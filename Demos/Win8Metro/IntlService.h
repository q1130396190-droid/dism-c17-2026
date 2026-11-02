#pragma once

// ============================================================
// IntlService —— S8（v2 P2c）
// 区域/国际化页写路径（R90 P2 切片）。
//
// 背景：评分报告 D3 缺口 SetUserLocale / SetSystemLocale 0 命中——
//       实际不存在 SetUserDefaultLocaleName/SetSystemDefaultUILanguage
//       公开导出（SDK 头与 DLL 均无），真实写路径为：
//         * 时区：SetDynamicTimeZoneInformation（kernel32），TZI 从
//           HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Time Zones\<id>\TZI
//           读取（二进制 DYNAMIC_TIME_ZONE_INFORMATION）
//         * 区域/语言：HKCU\Control Panel\International（LocaleName /
//           Locale / User Profile\Languages），Windows 内部会同步给 NLS
// 实现遵循 AssocService 模式：纯 Win32，无 UI 控件依赖，不使用预编译头。
// ============================================================

#include <windows.h>
#include <vector>

#ifndef UILIB_API
#define UILIB_API __declspec(dllimport)
#endif

#include "Utils/Utils.h"

// 系统区域格式类型（R120-1）
enum class EIntlFormatKind : int
{
    INTL_FMT_SHORTDATE = 0,   // 短日期格式（如 yyyy/M/d）
    INTL_FMT_SHORTTIME = 1,   // 短时间格式（如 HH:mm）
    INTL_FMT_DECIMAL   = 2,   // 十进制小数点符号
    INTL_FMT_CURRENCY  = 3,   // 货币符号
};

// 写路径结果码
enum class EIntlWriteResult : int
{
    INTL_WRITE_OK = 0,
    INTL_WRITE_NO_PERMISSION = -1,
    INTL_WRITE_INVALID_VALUE = -2,
    INTL_WRITE_REG_FAIL = -3,
    INTL_WRITE_TZI_MISSING = -4,
    INTL_WRITE_TZI_INVALID = -5,
};

// 语言/区域条目（供 UI 填充 Combo）
struct IntlLangEntry
{
    DuiLib::CDuiString sName;     // LocaleName（如 "zh-CN"）
    DuiLib::CDuiString sDisplay;  // 本地化显示名（如 "中文(简体，中国)"）
    bool       bInstalled;        // 是否已安装
    bool       bCurrent;          // 是否为当前用户默认
};

class IntlService
{
public:
    // ---- 时区 ----
    // 读取当前时区 ID（TimeZoneKeyName），失败返回 false
    static bool GetCurrentTimeZone(DuiLib::CDuiString& sTzId);

    // 按 Windows 时区 ID（如 "China Standard Time"）设置系统时区
    static EIntlWriteResult SetTimeZone(const wchar_t* pszTzId, DuiLib::CDuiString& sErr);

    // ---- 区域 / 语言 ----
    // 设置当前用户默认区域（LocaleName，如 "zh-CN"），
    // 写 HKCU\Control Panel\International\LocaleName + Locale(LCID)
    static EIntlWriteResult SetUserLocaleName(const wchar_t* pszLocaleName, DuiLib::CDuiString& sErr);

    // 读取当前用户默认区域名（GetUserDefaultLocaleName）
    static bool GetUserLocaleName(DuiLib::CDuiString& sLocaleName);

    // 设置当前线程 UI 语言（进程内即时生效，不改用户默认）
    static bool SetThreadUILanguageName(const wchar_t* pszLangName);

    // ---- [R96] 系统区域 / UI 语言 / 已安装语言 ----
    // 读取系统（非当前用户）默认区域（GetSystemDefaultLocaleName），失败 false
    static bool GetSystemLocaleName(DuiLib::CDuiString& sLocaleName);

    // 设置系统默认区域（写 HKLM\SYSTEM\CurrentControlSet\Control\Nls\Language
    //   + HKLM\Control Panel\International\Locale + HKU\.DEFAULT\Control
    //   Panel\International\Locale），需管理员权限；失败返回错误码
    static EIntlWriteResult SetSystemLocaleName(const wchar_t* pszLocaleName, DuiLib::CDuiString& sErr);

    // 读取当前用户 UI 语言（GetUserDefaultUILanguage -> LCIDToLocaleName）
    static bool GetUserUILanguageName(DuiLib::CDuiString& sLangName);

    // 设置系统 UI 语言（写 HKLM\...\MUI\Settings\PreferredUILanguages，
    //   REG_MULTI_SZ + "0" 启用语言列表），需管理员权限；失败返回错误码
    static EIntlWriteResult SetSystemUILanguageName(const wchar_t* pszLangName, DuiLib::CDuiString& sErr);

    // 枚举当前用户已安装语言（HKCU\Control Panel\International\User Profile\Languages）
    static int GetInstalledLanguages(std::vector<IntlLangEntry>& outEntries);

    // 枚举 Windows 内置候选区域（按 LocaleName 升序，仅常见区域）
    static int GetCandidateLocales(std::vector<IntlLangEntry>& outEntries);

    // ---- [R101] 已安装语言 安装/删除 ----
    // 把指定 LocaleName 追加到用户语言列表（HKCU\...\User Profile\Languages REG_MULTI_SZ）
    // 已存在时不重复追加，成功返回 0；失败返回错误码并回传 sErr
    static EIntlWriteResult AddInstalledLanguage(const wchar_t* pszLangName, DuiLib::CDuiString& sErr);

    // 从用户语言列表移除指定 LocaleName；成功返回 0，失败返回错误码
    static EIntlWriteResult RemoveInstalledLanguage(const wchar_t* pszLangName, DuiLib::CDuiString& sErr);

    // ---- [R120-1] 系统区域格式（HKCU\Control Panel\International）----
    // 读取指定格式的当前值（短日期/短时间/十进制/货币）。
    // 返回 true 表示读到（可能为默认值）；false 表示注册表项缺失或类型异常
    static bool GetLocaleFormat(EIntlFormatKind nKind, DuiLib::CDuiString& sValue);

    // 写入指定格式值（REG_SZ，幂等）。返回 EIntlWriteResult::INTL_WRITE_OK 或错误码
    static EIntlWriteResult SetLocaleFormat(EIntlFormatKind nKind, const wchar_t* pszValue,
                               DuiLib::CDuiString& sErr);
};
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
