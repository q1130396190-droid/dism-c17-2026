#pragma once

// ============================================================
// ControlService —— S8（v2 P2c）
// 控制面板页动态枚举（R91 P2 切片）。
//
// 背景：评分报告 D3 缺口"无 Shell API 调用，仅硬编码 GUID"。
// 实现：
//   枚举 HKLM\Software\Microsoft\Windows\CurrentVersion\Explorer\ControlPanel\NameSpace
//   下所有子键，读 Default/InfoTip，用 SHLoadIndirectString 解析 @resource 引用
//   得到真实 CPL 显示名，并返回完整 canonical 名称（CLSID）。
// 纯 Win32（注册表 + Shell API），无 UI 控件依赖，不使用预编译头。
// ============================================================

#include <windows.h>

#ifndef UILIB_API
#define UILIB_API __declspec(dllimport)
#endif

#include "Utils/Utils.h"
#include <vector>

struct CtrlAppletEntry
{
    DuiLib::CDuiString sCanonical;   // CLSID（形如 {025A5937-A6BE-4686-A844-36FE4BEC8B6D}）
    DuiLib::CDuiString sDisplayName; // 解析后的显示名（非空则优先）
    DuiLib::CDuiString sInfoTip;     // InfoTip 原始值（可空）
};

enum class EControlEnumResult : int
{
    CTRL_ENUM_OK = 0,
    CTRL_ENUM_NO_KEY = 1,          // NameSpace 键不存在
    CTRL_ENUM_REG_FAIL = 2,        // RegOpenKeyEx / RegEnumKeyEx 失败
    CTRL_ENUM_PARSE_FAIL = 3,      // 全部条目均解析失败
};

// 打开控制面板项结果码
enum class EControlOpenResult : int
{
    CTRL_OPEN_OK = 0,
    CTRL_OPEN_BAD_CANON = 1,       // canonical 无效（空指针 / 空串）
    CTRL_OPEN_SHELLEX_FAIL = 2,    // ShellExecuteW 返回 HINSTANCE <= 32
    CTRL_OPEN_SHELLITEM_FAIL = 3,  // [R127-2] IShellItem 直连打开失败
    CTRL_OPEN_ELEVATE_FAIL = 4,    // [R127-2] runas 提升运行失败
};

// [R127-2] 打开失败诊断分级（回传给 UI 用于分级 Toast）
enum class EControlOpenDiag : int
{
    CTRL_DIAG_NONE = 0,        // 无错误（成功）
    CTRL_DIAG_ACCESSDENIED = 1,// ERROR_ACCESS_DENIED / ERROR_ELEVATION_REQUIRED
    CTRL_DIAG_FILENOTFOUND = 2,// ERROR_FILE_NOT_FOUND / ERROR_PATH_NOT_FOUND
    CTRL_DIAG_NOASSOC = 3,     // SE_ERR_NOASSOC（无关联程序）
    CTRL_DIAG_ELEV_REQUIRED = 4,// ERROR_ELEVATION_REQUIRED（需管理员）
    CTRL_DIAG_CANCELED = 5,    // ERROR_CANCELLED（UAC 取消）
    CTRL_DIAG_OTHER = 6,       // 其他未知错误
};

class ControlService
{
public:
    // 枚举 HKLM NameSpace 下所有子键，返回 CTRL_ENUM_*；
    // vOut 只追加成功解析（canonical + displayName 非空）的条目。
    static EControlEnumResult EnumControlApplets(std::vector<CtrlAppletEntry>& vOut);

    // 按 canonical（形如 {CLSID}）打开控制面板小程序。
    // 返回 0 成功；1 ShellExecuteW 失败。
    // 打开控制面板项（canonical → ShellExecuteExW）
    // [R127-2] 优先 IShellItem/IShellFolder 直连打开，失败回退 ShellExecuteExW
    static EControlOpenResult OpenControlApplet(HWND hwnd, const wchar_t* pszCanonical,
                                 DWORD* pdwLastError = nullptr);   // [R109] 回传真实 GetLastError

    // [R127-2] 以管理员身份打开控制面板项（lpVerb="runas" 触发 UAC 提升）。
    // 返回 EControlOpenResult::CTRL_OPEN_OK / EControlOpenResult::CTRL_OPEN_BAD_CANON / EControlOpenResult::CTRL_OPEN_ELEVATE_FAIL。
    static EControlOpenResult OpenControlAppletElevated(HWND hwnd, const wchar_t* pszCanonical,
                                         DWORD* pdwLastError = nullptr);

    // [R127-2] 把打开失败错误码映射为诊断分级（供 UI 分级 Toast 使用）。
    static EControlOpenDiag DiagnoseOpenFailure(EControlOpenResult nRc, DWORD dwLastError);

    // 解析 @shell32.dll,-12345 形式的资源引用为可显示字符串。
    static bool ResolveIndirectString(const wchar_t* pszSource, DuiLib::CDuiString& sOut);

    // 兜底：从硬编码映射解析 canonical 显示名（仅当 SHLoadIndirectString 失败时使用）。
    static bool FallbackDisplayName(const wchar_t* pszCanonical, DuiLib::CDuiString& sOut);
};
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
