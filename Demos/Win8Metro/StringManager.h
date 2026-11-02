// ============================================================
// StringManager.h
// ------------------------------------------------------------
// 多语言字符串管理器（Phase 1 落地）
//
// 设计原则：
//   1) 独立语言 DLL 方案：主程序不内嵌任何语言文本，
//      运行时通过 LoadLibraryA("lang/<code>.dll") 加载语言包。
//   2) 8 个固定导出函数（与用户方案一致）：
//         GetLangName / GetLangCode / GetVersion /
//         GetAuthor   / GetDate     / GetCopyright /
//         GetString   / GetKeyCount
//   3) 内置 zh-CN 回退表：DLL 加载失败或 key 未命中时，
//      从进程内存里的静态表返回，保证永远不返回 nullptr。
//   4) 单例，线程安全（初始化一次后只读）。
//   5) DuiLib SetText 需要 const wchar_t* 非空值 —— 本类保证 Get() 永不返回 nullptr。
// ============================================================

#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>   // [S-01-FIX] LookupBuiltin 索引化
#include "StdAfx.h"     // DuiLib 的 CDuiString 依赖

namespace i18n {

// ----------------------------------------------------------
// 语言包导出函数指针类型（DLL 内导出函数签名）
// ----------------------------------------------------------
using PFN_GetString = const wchar_t* (*)(const char* pszKey);
using PFN_GetKeyCount = int (*)();
using PFN_GetTextFn = const wchar_t* (*)();

class CStringManager
{
public:
    // ------------------------------------------------------------
    // 单例
    // ------------------------------------------------------------
    static CStringManager& GetInstance()
    {
        static CStringManager s_instance;
        return s_instance;
    }

    // ------------------------------------------------------------
    // 初始化：加载 lang/<code>.dll。code 为 nullptr 时使用当前
    // 系统 UI locale（例如 zh-CN）。返回 false 表示回退到内置表。
    // ------------------------------------------------------------
    bool Init(const wchar_t* pszAppDir, const wchar_t* pszLangCode = nullptr);

    // ------------------------------------------------------------
    // 取字符串（UTF-8 key 版本，主用）：key 用 UTF-8 传入。
    // 未命中时返回 "[%s]" 包裹的 key，便于调试。
    // ------------------------------------------------------------
    const wchar_t* Get(const char* pszKeyUtf8);

    // ------------------------------------------------------------
    // 取字符串（宽字符 key 版本，兼容已有调用）
    // ------------------------------------------------------------
    const wchar_t* Get(const wchar_t* pszKeyW);

    // ------------------------------------------------------------
    // 探测式取字符串：仅当语言 DLL 或内置表真实命中时填充 out
    // 并返回 true；未命中返回 false（不产生 "[key]" 占位符）。
    // 用于"有译文则用译文，否则保留皮肤 XML 原文"的场景。
    // ------------------------------------------------------------
    bool TryGet(const wchar_t* pszKeyW, CDuiString& out);

    // ------------------------------------------------------------
    // 格式化：先取 key，再走 _vsnwprintf_s 输出 %d/%s 等。
    // ------------------------------------------------------------
    CDuiString Format(const wchar_t* pszKey, ...) const;

    // ------------------------------------------------------------
    // 运行时切换语言（本阶段：只切下一次启动生效，
    // 实际生效发生在 Init 阶段；此处仅记录 + 更新 config.ini 由调用者负责）
    // ------------------------------------------------------------
    bool SwitchLang(const wchar_t* pszLangCode);

    // ------------------------------------------------------------
    // [R50] 检查指定语言是否可用（DLL 存在或内置支持）
    // 用于语言切换 UI：禁用不可用的语言项
    // ------------------------------------------------------------
    bool IsLangAvailable(const wchar_t* pszLangCode) const;

    // ------------------------------------------------------------
    // 元数据
    // ------------------------------------------------------------
    const wchar_t*  GetCurrentLang()     const { return m_currentLang.c_str(); }
    const wchar_t*  GetLangVersion()     const { return m_langVersion.c_str();  }
    const wchar_t*  GetLangName()        const { return m_langName.c_str();     }
    const wchar_t*  GetLangAuthor()      const { return m_langAuthor.c_str();   }
    const wchar_t*  GetLangDate()        const { return m_langDate.c_str();     }
    const wchar_t*  GetLangCopyright()   const { return m_langCopyright.c_str(); }
    int      GetLoadedKeyCount()  const { return m_keyCount;             }
    bool     IsUsingFallback()    const { return m_usingFallback;        }

    // ------------------------------------------------------------
    // 枚举可用语言：扫描 appDir/lang/ 目录下的 *.dll
    // ------------------------------------------------------------
    std::vector<CDuiString> ListAvailableLangs() const;

    // ------------------------------------------------------------
    // [R-B73] 可选外部语言加载（松散目录 / ZIP）
    // 用户可自行投放 <appDir>/lang-extra/<code>/ 松散 JSON，
    // 或 <appDir>/lang-extra/<code>.zip（内含 strings.json）。
    // 优先级：卫星 DLL > 外部 JSON > 内置 zh-CN 表 > [key]。
    // 用于恢复原版 Languages\*.zip + StringMaps 的可扩展性。
    // ------------------------------------------------------------
    bool  LoadExternalLang(const wchar_t* pszLangCode);
    void  UnloadExternalLang();
    const wchar_t* LookupExternal(const char* pszKeyUtf8);
    bool  IsExternalLangAvailable(const wchar_t* pszLangCode) const;

    // ------------------------------------------------------------
    // 卸载语言 DLL（进程退出时可选）
    // ------------------------------------------------------------
    void Shutdown();

private:
    CStringManager()
        : m_hModule(nullptr)
        , m_pfnGetString(nullptr)
        , m_pfnGetKeyCount(nullptr)
        , m_currentLang(L"zh-CN")
        , m_langVersion(L"builtin")
        , m_langName(L"\u7b80\u4f53\u4e2d\u6587")  // 简体中文
        , m_langAuthor(L"Dism++ Team")
        , m_langDate(L"2026-09-28")
        , m_langCopyright(L"Dism++")
        , m_keyCount(0)
        , m_usingFallback(true)
        , m_initialized(false)
    {}
    ~CStringManager();

    CStringManager(const CStringManager&) = delete;
    CStringManager& operator=(const CStringManager&) = delete;

    bool  LoadLanguageDll(const wchar_t* pszPath, const wchar_t* pszLangCode);
    void  UnloadLanguageDll();
    const wchar_t* LookupBuiltin(const char* pszKeyUtf8);

    // 已加载 DLL
    HMODULE m_hModule;
    PFN_GetString   m_pfnGetString;
    PFN_GetKeyCount m_pfnGetKeyCount;

    // 元数据缓存
    std::wstring m_currentLang;
    std::wstring m_langVersion;
    std::wstring m_langName;
    std::wstring m_langAuthor;
    std::wstring m_langDate;
    std::wstring m_langCopyright;
    int          m_keyCount;

    // 状态
    bool m_usingFallback;
    bool m_initialized;

    // 缓存 appDir/lang 目录
    std::wstring m_langDir;

    // [R-B73] 外部语言（松散 JSON / ZIP）翻译表：UTF-8 key -> 宽字符串
    std::map<std::string, std::wstring> m_externalMap;
    std::wstring m_externalDir;   // lang-extra 根目录
};

} // namespace i18n

// ------------------------------------------------------------
// 便捷全局访问宏（放在命名空间外，全局可见）
// ------------------------------------------------------------
#define StringMgr   i18n::CStringManager::GetInstance()
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
