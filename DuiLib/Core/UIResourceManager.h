#ifndef __UIRESOURCEMANAGER_H__
#define __UIRESOURCEMANAGER_H__
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>

// Self-contained: UILIB_API fallback when UIlib.h not yet included.
// Mirrors UIlib.h contract: empty under UILIB_STATIC, dllexport/dllimport otherwise.
#ifndef UILIB_API
    #ifdef UILIB_STATIC
        #define UILIB_API
    #elif defined(UILIB_EXPORTS)
        #define UILIB_API __declspec(dllexport)
    #elif defined(_MSC_VER)
        #define UILIB_API __declspec(dllimport)
    #else
        #define UILIB_API
    #endif
#endif

namespace DuiLib {
	// 控件文字查询接口
	class UILIB_API IQueryControlText
	{
	public:
		virtual LPCTSTR QueryControlText(LPCTSTR lpstrId, LPCTSTR lpstrType) = 0;
	};

	class UILIB_API CResourceManager
	{
	private:
		// 私有构造：强制通过 GetInstance(scope) 获取，不允许外部 new。
		CResourceManager(void);

		// [RESMGR-OWNERSHIP] Scope this instance was registered under. Set by
		// GetInstance, used by the legacy Release() to erase the right registry entry
		// instead of running `delete this` on an object the registry still owns.
		std::string m_scopeKey;

		// [EXPORT-RATCHET] The registry-erase helper is NOT a member. CResourceManager is
		// an exported class, so even a private static member becomes an exported symbol
		// (measured: +1 private export, ratchet 147 -> 148). It lives as a file-local
		// function in UIResourceManager.cpp instead.

	public:
		// 公开析构：供 Registry 内部 std::unique_ptr<CResourceManager> 使用
		// （unique_ptr 需要调用 complete-object 析构以正确 delete）。
		// 外部仍不应主动 delete，请使用 Release / ReleaseScope / ReleaseAll。
		~CResourceManager(void);

	public:
		// ---- T5.24 Scope registry（D2 方案）----
		//
		// 默认参数 "default" 保持既有代码 `GetInstance()` 无参调用完全兼容。
		// 传入非空 scope 字符串可创建/获取独立资源集，用于多实例隔离
		// （例如多租户 / 多主题 / 多皮肤同时加载而不互相污染）。
		//
		// 实现语义：
		//   - 每 scope 一个 CResourceManager 实例，由 std::unique_ptr 持有，
		//     析构自动；不会因进程退出时的静态存储期析构顺序问题而泄漏。
		//   - Release() 显式释放当前 scope；ReleaseScope(name) 释放指定 scope。
		//   - Release 后再次 GetInstance(name) 会新建实例（不再是悬空指针）。
		//   - 注册表与查询均持 mutex，多线程安全。
		static CResourceManager* GetInstance(LPCTSTR scope = nullptr);
		// 【Deprecated】实例方法 Release()：保留向后兼容外部代码；DuiLib 内部与所有调用点
		// 已在 Round 53 T5.27 迁移到 ReleaseScope(nullptr)（释放 default scope 语义等价，
		// 且避免 D2 Scope registry 中 unique_ptr 悬空）。
		// 新代码请直接使用 CResourceManager::ReleaseScope(nullptr)（释放 default）或
		// CResourceManager::ReleaseScope(scope) / ReleaseAll()（释放指定或全部 scope）。
		void Release(void);
		static void ReleaseScope(LPCTSTR scope = nullptr);
		static void ReleaseAll();
		static int GetScopeCount();

	public:
		BOOL LoadResource(STRINGorID xml, LPCTSTR type = nullptr);
		BOOL LoadResource(CMarkupNode Root);
		void ResetResourceMap();
		LPCTSTR GetImagePath(LPCTSTR lpstrId);
		LPCTSTR GetXmlPath(LPCTSTR lpstrId);

	public:
		void SetLanguage(LPCTSTR pstrLanguage) { m_sLauguage = pstrLanguage; }
		LPCTSTR GetLanguage() { return m_sLauguage; }
		BOOL LoadLanguage(LPCTSTR pstrXml);
		
	public:
		void SetTextQueryInterface(IQueryControlText* pInterface) { m_pQuerypInterface = pInterface; }
		CDuiString GetText(LPCTSTR lpstrId, LPCTSTR lpstrType = nullptr);
		void ReloadText();
		void ResetTextMap();

	private:
		CStdStringPtrMap m_mTextResourceHashMap;
		IQueryControlText*	m_pQuerypInterface;
		CStdStringPtrMap m_mImageHashMap;
		CStdStringPtrMap m_mXmlHashMap;
		CMarkup m_xml;
		CDuiString m_sLauguage;
		CStdStringPtrMap m_mTextHashMap;
	};

} // namespace DuiLib

#endif // __UIRESOURCEMANAGER_H__
