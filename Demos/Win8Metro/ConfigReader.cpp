// ============================================================
// ConfigReader.cpp
// ============================================================

#include "stdafx.h"
#include <optional>   // [C++17 R-P10]
#include "ConfigReader.h"
#include "WinHandles.h"   // [C++17 R3] win::file_handle RAII for F11_raw_handle_close

namespace i18n {

std::wstring CConfigReader::s_configPath;

bool CConfigReader::Load(const wchar_t* pszAppDir)
{
    if (!pszAppDir || !pszAppDir[0]) return false;

    std::wstring dir = pszAppDir;
    if (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/'))
        dir.pop_back();
    s_configPath = dir + L"\\config.ini";

    // 文件不存在则创建默认内容
    DWORD attr = ::GetFileAttributesW(s_configPath.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES)
    {
        static const wchar_t* kDefaultIni =
            L"; Win8Metro config.ini\r\n"
            L"; Edit this file to customize behavior\r\n"
            L"\r\n"
            L"[Language]\r\n"
            L"; zh-CN | en-US | ja-JP | ko-KR\r\n"
            L"Current=zh-CN\r\n"
            L"\r\n"
            L"[General]\r\n"
            L"StartMaximized=0\r\n"
            L"RememberLastLang=1\r\n";

        win::HandlePtr hFile(win::file_handle(::CreateFileW(s_configPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                      CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)));   // [C++17 R3] F11: RAII auto CloseHandle
        if (hFile)
        {
            // [R119] 原实现丢弃 WriteFile 返回值：配置文件建出来了但内容没写进去时，
            //   下次启动读到空文件，用户改的设置全部落到默认值，而界面毫无提示。
            //   这里检查写入量与失败状态；失败就删掉这个空壳，让下次启动重新尝试创建，
            //   避免留下一个"存在但为空"的 config.ini 长期生效。
            DWORD dw = 0;
            const DWORD cbWant = static_cast<DWORD>(wcslen(kDefaultIni) * sizeof(wchar_t));
            if (!::WriteFile(hFile.get(), kDefaultIni, cbWant, &dw, nullptr) || dw != cbWant)
            {
                ::OutputDebugStringW(L"[Config] 默认 config.ini 写入失败，已删除残留空文件\r\n");
                hFile.reset();
                ::DeleteFileW(s_configPath.c_str());
            }
        }
    }
    return true;
}

std::wstring CConfigReader::GetString(const wchar_t* pszSection, const wchar_t* pszKey,
                                      const wchar_t* pszDefault)
{
    if (s_configPath.empty()) return pszDefault ? pszDefault : L"";

    constexpr int kIniValWchars = 512;   // 512 缓冲尺寸（语义命名，数值不变）
    wchar_t buf[kIniValWchars] = {};
    ::GetPrivateProfileStringW(pszSection, pszKey,
                               pszDefault ? pszDefault : L"",
                               buf, _countof(buf),
                               s_configPath.c_str());
    return buf;
}

bool CConfigReader::SetString(const wchar_t* pszSection, const wchar_t* pszKey, const wchar_t* pszValue)
{
    if (s_configPath.empty()) return false;
    return ::WritePrivateProfileStringW(pszSection, pszKey,
                                        pszValue ? pszValue : L"",
                                        s_configPath.c_str()) != 0;
}

bool CConfigReader::HasKey(const wchar_t* pszSection, const wchar_t* pszKey)
{
    if (s_configPath.empty()) return false;

    constexpr int kIniValWchars = 512;   // 512 缓冲尺寸（语义命名，数值不变）
    wchar_t buf[kIniValWchars] = { '?', 0 }; // 默认值用单字符占位以区分"未设置"
    ::GetPrivateProfileStringW(pszSection, pszKey, L"?",
                               buf, _countof(buf),
                               s_configPath.c_str());
    // 若返回 "?" 说明键不存在或值为空
    return !(buf[0] == L'?' && buf[1] == 0);
}

const wchar_t* CConfigReader::GetLanguageCurrent()
{
    if (s_configPath.empty()) return nullptr;

    wchar_t buf[64] = {};
    ::GetPrivateProfileStringW(L"Language", L"Current", L"",
                               buf, _countof(buf),
                               s_configPath.c_str());
    if (buf[0] == 0) return nullptr;

    // 转成 const wchar_t* 稳定指针（静态 buffer）
    static thread_local wchar_t s_cached[64];
    wcscpy_s(s_cached, buf);
    return s_cached;
}

// [C++17 R-P10] 可选语义版：无配置/空值时返回 std::nullopt，消除调用方的 nullptr 判定。
// 与 const wchar_t* 版行为等价：未加载/无此键/值为空均视为 absent。
std::optional<std::wstring> CConfigReader::GetLanguageCurrentOpt()
{
    if (s_configPath.empty()) return std::nullopt;

    wchar_t buf[64] = {};
    ::GetPrivateProfileStringW(L"Language", L"Current", L"",
                               buf, _countof(buf),
                               s_configPath.c_str());
    if (buf[0] == 0) return std::nullopt;

    return std::wstring(buf);
}

bool CConfigReader::SetLanguageCurrent(const wchar_t* pszLangCode)
{
    if (!pszLangCode || !pszLangCode[0]) return false;
    return SetString(L"Language", L"Current", pszLangCode);
}

bool CConfigReader::IsFileLoaded()
{
    return !s_configPath.empty();
}

const wchar_t* CConfigReader::GetConfigPath()
{
    static thread_local wchar_t s_empty[1] = { 0 };
    return s_configPath.empty() ? s_empty : s_configPath.c_str();
}

} // namespace i18n
