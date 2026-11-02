// ============================================================
// ConfigReader.h
// ------------------------------------------------------------
// 便携版 config.ini 读写（GetPrivateProfileStringW 包装）
//
// 便携部署：exe 与 config.ini 同目录。
// 优先级：命令行 /lang=XXX > config.ini > 系统 UI locale > 默认 zh-CN
// ============================================================

#pragma once

#include <windows.h>
#include <string>
#include <optional>   // [C++17 R-P10] 语言代码可选语义
#include "StdAfx.h"

namespace i18n {

class CConfigReader
{
public:
    // ------------------------------------------------------------
    // 加载 exe 同目录下的 config.ini。若文件不存在则创建一个
    // 带默认值的 config.ini，方便用户后续编辑。
    // ------------------------------------------------------------
    static bool Load(const wchar_t* pszAppDir);

    // ------------------------------------------------------------
    // 从 [Section] 读一个键值。空值返回 defaultVal。
    // ------------------------------------------------------------
    static std::wstring GetString(const wchar_t* pszSection, const wchar_t* pszKey,
                                  const wchar_t* pszDefault = nullptr);

    // ------------------------------------------------------------
    // 写一个键值到 config.ini（会同步到磁盘）。
    // ------------------------------------------------------------
    static bool SetString(const wchar_t* pszSection, const wchar_t* pszKey, const wchar_t* pszValue);

    // ------------------------------------------------------------
    // 检查键是否存在（值可为空）
    // ------------------------------------------------------------
    static bool HasKey(const wchar_t* pszSection, const wchar_t* pszKey);

    // ------------------------------------------------------------
    // 读取当前语言代码；若无配置返回 nullptr
    // ------------------------------------------------------------
    static const wchar_t* GetLanguageCurrent();

    // ------------------------------------------------------------
    // [C++17 R-P10] 可选语义版本：无配置时返回 std::nullopt
    //（原 const wchar_t* 版本保留，供未切换的调用点使用）。
    // ------------------------------------------------------------
    static std::optional<std::wstring> GetLanguageCurrentOpt();

    // ------------------------------------------------------------
    // 设置当前语言代码（写入 config.ini）
    // ------------------------------------------------------------
    static bool SetLanguageCurrent(const wchar_t* pszLangCode);

    // ------------------------------------------------------------
    // 检查 config.ini 文件是否存在
    // ------------------------------------------------------------
    static bool IsFileLoaded();

    // ------------------------------------------------------------
    // 当前 config.ini 完整路径（供调试）
    // ------------------------------------------------------------
    static const wchar_t* GetConfigPath();

private:
    static std::wstring s_configPath;
};

} // namespace i18n
