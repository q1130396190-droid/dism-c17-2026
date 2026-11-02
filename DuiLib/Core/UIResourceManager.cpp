#include "StdAfx.h"
#include "UIResourceManager.h"

namespace DuiLib {
	
	CResourceManager::CResourceManager(void)
	{
		m_pQuerypInterface = nullptr;
		
	}

	CResourceManager::~CResourceManager(void)
	{
		ResetResourceMap();
	}

	BOOL CResourceManager::LoadResource(STRINGorID xml, LPCTSTR type)
	{
		if( HIWORD(xml.m_lpstr) != 0 ) 
		{
			if( *(xml.m_lpstr) == _T('<') ) 
			{
				if( !m_xml.Load(xml.m_lpstr) ) return FALSE;
			}
			else 
			{
				if( !m_xml.LoadFromFile(xml.m_lpstr) ) return FALSE;
			}
		}
		else
		{
			HRSRC hResource = ::FindResource(CPaintManagerUI::GetResourceDll(), xml.m_lpstr, type);
			if( hResource == nullptr ) return FALSE;
			HGLOBAL hGlobal = ::LoadResource(CPaintManagerUI::GetResourceDll(), hResource);
			if( hGlobal == nullptr ) 
			{
				FreeResource(hResource);
				return FALSE;
			}

			if( !m_xml.LoadFromMem(reinterpret_cast<BYTE*>(::LockResource(hGlobal)), ::SizeofResource(CPaintManagerUI::GetResourceDll(), hResource) )) {
				return FALSE;
			}
			::FreeResource(hGlobal);
		}

		return LoadResource(m_xml.GetRoot());
	}

	BOOL CResourceManager::LoadResource(CMarkupNode Root)
	{
		if( !Root.IsValid() ) return FALSE;

		LPCTSTR pstrClass = nullptr;
		int nAttributes = 0;
		LPCTSTR pstrName = nullptr;
		LPCTSTR pstrValue = nullptr;
		LPTSTR pstr = nullptr;

		//加载图片资源
		LPCTSTR pstrId = nullptr;
		LPCTSTR pstrPath = nullptr;
		for( CMarkupNode node = Root.GetChild() ; node.IsValid(); node = node.GetSibling() ) 
		{
			pstrClass = node.GetName();
			CMarkupNode ChildNode = node.GetChild();
			if(ChildNode.IsValid()) LoadResource(node);
			else if ((_tcsicmp(pstrClass,_T("Image")) == 0) && node.HasAttributes())
			{
				//加载图片资源
				nAttributes = node.GetAttributeCount();
				for( int i = 0; i < nAttributes; i++ ) 
				{
					pstrName = node.GetAttributeName(i);
					pstrValue = node.GetAttributeValue(i);

					if( _tcsicmp(pstrName, _T("id")) == 0 ) 
					{
						pstrId = pstrValue;
					}
					else if( _tcsicmp(pstrName, _T("path")) == 0 ) 
					{
						pstrPath = pstrValue;
					}
				}
				if( pstrId == nullptr ||  pstrPath == nullptr) continue;
				CDuiString * pstrFind = static_cast<CDuiString *>(m_mImageHashMap.Find(pstrId));
				if(pstrFind != nullptr) continue;
				m_mImageHashMap.Insert(pstrId, static_cast<LPVOID>(new CDuiString(pstrPath)));
			}
			else if( _tcsicmp(pstrClass,_T("Xml")) == 0 && node.HasAttributes()) {
				//加载XML配置文件
				nAttributes = node.GetAttributeCount();
				for( int i = 0; i < nAttributes; i++ ) 
				{
					pstrName = node.GetAttributeName(i);
					pstrValue = node.GetAttributeValue(i);

					if( _tcsicmp(pstrName, _T("id")) == 0 ) 
					{
						pstrId = pstrValue;
					}
					else if( _tcsicmp(pstrName, _T("path")) == 0 ) 
					{
						pstrPath = pstrValue;
					}
				}
				if( pstrId == nullptr ||  pstrPath == nullptr) continue;
				CDuiString * pstrFind = static_cast<CDuiString *>(m_mXmlHashMap.Find(pstrId));
				if(pstrFind != nullptr) continue;
				m_mXmlHashMap.Insert(pstrId, static_cast<LPVOID>(new CDuiString(pstrPath)));
			}
			else continue;
		}
		return TRUE;
	}

	LPCTSTR CResourceManager::GetImagePath(LPCTSTR lpstrId)
	{
		CDuiString * lpStr = static_cast<CDuiString *>(m_mImageHashMap.Find(lpstrId));
		return lpStr == nullptr? nullptr:lpStr->GetData();
	}

	LPCTSTR CResourceManager::GetXmlPath(LPCTSTR lpstrId)
	{
		CDuiString * lpStr = static_cast<CDuiString *>(m_mXmlHashMap.Find(lpstrId));
		return lpStr == nullptr? nullptr:lpStr->GetData();
	}

	void CResourceManager::ResetResourceMap()
	{
		CDuiString* lpStr;
		for( int i = 0; i< m_mImageHashMap.GetSize(); i++ )
		{
			if(LPCTSTR key = m_mImageHashMap.GetAt(i))
			{
				lpStr = static_cast<CDuiString *>(m_mImageHashMap.Find(key));
				delete lpStr;
				lpStr = nullptr;
			}
		}
		for( int i = 0; i< m_mXmlHashMap.GetSize(); i++ )
		{
			if(LPCTSTR key = m_mXmlHashMap.GetAt(i))
			{
				lpStr = static_cast<CDuiString *>(m_mXmlHashMap.Find(key));
				delete lpStr;
				lpStr = nullptr;
			}
		}
		for( int i = 0; i< m_mTextResourceHashMap.GetSize(); i++ )
		{
			if(LPCTSTR key = m_mTextResourceHashMap.GetAt(i))
			{
				lpStr = static_cast<CDuiString *>(m_mTextResourceHashMap.Find(key));
				delete lpStr;
				lpStr = nullptr;
			}
		}
	}

	BOOL CResourceManager::LoadLanguage(LPCTSTR pstrXml)
	{
		CMarkup xml;
		if( *(pstrXml) == _T('<') ) {
			if( !xml.Load(pstrXml) ) return FALSE;
		}
		else {
			if( !xml.LoadFromFile(pstrXml) ) return FALSE;
		}
		CMarkupNode Root = xml.GetRoot();
		if( !Root.IsValid() ) return FALSE;

		LPCTSTR pstrClass = nullptr;
		int nAttributes = 0;
		LPCTSTR pstrName = nullptr;
		LPCTSTR pstrValue = nullptr;
		LPTSTR pstr = nullptr;

		//加载图片资源
		LPCTSTR pstrId = nullptr;
		LPCTSTR pstrText = nullptr;
		for( CMarkupNode node = Root.GetChild() ; node.IsValid(); node = node.GetSibling() ) 
		{
			pstrClass = node.GetName();
			if ((_tcsicmp(pstrClass,_T("Text")) == 0) && node.HasAttributes())
			{
				//加载图片资源
				nAttributes = node.GetAttributeCount();
				for( int i = 0; i < nAttributes; i++ ) 
				{
					pstrName = node.GetAttributeName(i);
					pstrValue = node.GetAttributeValue(i);

					if( _tcsicmp(pstrName, _T("id")) == 0 ) 
					{
						pstrId = pstrValue;
					}
					else if( _tcsicmp(pstrName, _T("value")) == 0 ) 
					{
						pstrText = pstrValue;
					}
				}
				if( pstrId == nullptr ||  pstrText == nullptr) continue;

				CDuiString *lpstrFind = static_cast<CDuiString *>(m_mTextResourceHashMap.Find(pstrId));
				if(lpstrFind != nullptr) {
					lpstrFind->Assign(pstrText);
				}
				else {
					m_mTextResourceHashMap.Insert(pstrId, static_cast<LPVOID>(new CDuiString(pstrText)));
				}
			}
			else continue;
		}

		return TRUE;
	}

	CDuiString CResourceManager::GetText(LPCTSTR lpstrId, LPCTSTR lpstrType)
	{
		if(lpstrId == nullptr) return _T("");

		CDuiString *lpstrFind = static_cast<CDuiString *>(m_mTextResourceHashMap.Find(lpstrId));
		if (lpstrFind == nullptr && m_pQuerypInterface)
		{
			LPCTSTR lpText = m_pQuerypInterface->QueryControlText(lpstrId, lpstrType);
			if(lpText != nullptr) {
				lpstrFind = new CDuiString(lpText);
				m_mTextResourceHashMap.Insert(lpstrId, static_cast<LPVOID>(lpstrFind));
			}
		}
		// C2445 fix: unify both branches to CDuiString (the declared return type)
		return (lpstrFind == nullptr) ? CDuiString(lpstrId) : *lpstrFind;
	}

	void CResourceManager::ReloadText()
	{
		if(m_pQuerypInterface == nullptr) return;
		//重载文字描述
		LPCTSTR lpstrId = nullptr;
		LPCTSTR lpstrText;
		for( int i = 0; i < m_mTextResourceHashMap.GetSize(); i++ )
		{
			lpstrId = m_mTextResourceHashMap.GetAt(i);
			if (lpstrId == nullptr) continue;
			lpstrText = m_pQuerypInterface->QueryControlText(lpstrId, nullptr);
			if(lpstrText != nullptr) {
				CDuiString * lpStr = static_cast<CDuiString *>(m_mTextResourceHashMap.Find(lpstrId));
				lpStr->Assign(lpstrText);
			}
		}
	}
	void CResourceManager::ResetTextMap()
	{
		CDuiString * lpStr;
		for( int i = 0; i< m_mTextResourceHashMap.GetSize(); i++ )
		{
			if(LPCTSTR key = m_mTextResourceHashMap.GetAt(i))
			{
				lpStr = static_cast<CDuiString *>(m_mTextResourceHashMap.Find(key));
				delete lpStr;
			}
		}
	}

	

	// ---- T5.24 Scope registry (D2 方案) ----
	// 实现细节：
	//   - std::map<std::string, std::unique_ptr<CResourceManager>> 持每 scope 一个实例，
	//     unique_ptr 保证进程退出时静态存储期自动析构（无泄漏）
	//   - std::mutex 保护注册表与查询（多线程安全）
	//   - Release / ReleaseScope 后再次 GetInstance 会重建实例（不再是悬空指针）
	//   - 默认参数 "default" 保持既有 GetInstance() 无参调用完全兼容

	namespace
	{
		std::mutex& GetRegistryMutex()
		{
			static std::mutex m;
			return m;
		}

		std::map<std::string, std::unique_ptr<CResourceManager>>& GetRegistry()
		{
			static std::map<std::string, std::unique_ptr<CResourceManager>> registry;
			return registry;
		}

		std::string NormalizeScope(LPCTSTR scope)
		{
			if (scope == nullptr) return std::string("default");
			if (scope[0] == _T('\0')) return std::string("default");
			std::string s;
			#ifdef _UNICODE
				int n = WideCharToMultiByte(CP_UTF8, 0, scope, -1, nullptr, 0, nullptr, nullptr);
				if (n <= 0) return std::string("default");
				s.resize(n);
				int got = WideCharToMultiByte(CP_UTF8, 0, scope, -1, &s[0], n, nullptr, nullptr);
				if (got <= 1) return std::string("default");
				s.resize(got - 1);  // 去掉末尾 \0
			#else
				s.assign(scope);
			#endif
			return s.empty() ? std::string("default") : s;
		}

		// [EXPORT-RATCHET] File-local: a private static member of an exported class
		// still lands in the export table (+1 private symbol, ratchet 147 -> 148).
		void EraseScopeKey(const std::string& key)
		{
			std::lock_guard<std::mutex> lk(GetRegistryMutex());
			auto& reg = GetRegistry();
			auto it = reg.find(key);
			if (it != reg.end())
			{
				reg.erase(it);  // unique_ptr deletes exactly once
			}
		}
	}

	CResourceManager* CResourceManager::GetInstance(LPCTSTR scope)
	{
		std::string key = NormalizeScope(scope);
		std::lock_guard<std::mutex> lk(GetRegistryMutex());
		auto& reg = GetRegistry();
		auto it = reg.find(key);
		if (it == reg.end())
		{
			// unique_ptr 自动管理生命周期；无参构造函数在头文件 private
			// 但 C++17 起 static_cast<std::unique_ptr<T>>(new T) 合法
			std::unique_ptr<CResourceManager> inst(new CResourceManager);
			CResourceManager* raw = inst.get();
			// [RESMGR-OWNERSHIP] Record the owning scope so the legacy instance
			// Release() can ask the registry to erase that exact entry. Without it the
			// instance had no way to remove itself, and `delete this` was the only
			// option left -- which is what corrupted the heap.
			raw->m_scopeKey = key;
			reg.emplace(key, std::move(inst));
			return raw;
		}
		return it->second.get();
	}

	void CResourceManager::Release(void)
	{
		// [RESMGR-OWNERSHIP] Erase this instance's registry entry; do NOT delete this.
		//
		// The instance is owned by a std::unique_ptr inside the registry. The previous
		// implementation ran `delete this` here, destroying an object the registry
		// still pointed at:
		//   GetInstance("x") -> Release()      -> registry keeps a dangling pointer
		//   GetInstance("x")                   -> hands the freed pointer back (UAF)
		//   ReleaseScope("x")                  -> ~unique_ptr on it (double free)
		// Measured before the fix: scope count stayed 1 after Release(), GetInstance
		// returned the same address, and the process died with 0xC0000374.
		//
		// The key is copied first because erasing the entry destroys *this.
		std::string key;
		{
			std::lock_guard<std::mutex> lk(GetRegistryMutex());
			key = m_scopeKey;
		}
		EraseScopeKey(key);
	}

	// [EXPORT-RATCHET] File-local helper, not a member: a private static member of an
	// exported class would still be exported and would trip the surface ratchet.

	void CResourceManager::ReleaseScope(LPCTSTR scope)
	{
		EraseScopeKey(NormalizeScope(scope));
	}

	void CResourceManager::ReleaseAll()
	{
		std::lock_guard<std::mutex> lk(GetRegistryMutex());
		GetRegistry().clear();
	}

	int CResourceManager::GetScopeCount()
	{
		std::lock_guard<std::mutex> lk(GetRegistryMutex());
		return static_cast<int>(GetRegistry().size());
	}

} // namespace DuiLib