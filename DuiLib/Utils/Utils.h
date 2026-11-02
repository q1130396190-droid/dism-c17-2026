#ifndef __UTILS_H__
#define __UTILS_H__

#pragma once
#include "OAIdl.h"
#include <vector>

#include <string>   // S2/T2.4: CDuiString 内部改用 std::basic_string<TCHAR>

#include <utility> // T4.3: std::move

namespace DuiLib
{
	/////////////////////////////////////////////////////////////////////////////////////
	//

	class UILIB_API STRINGorID
	{
	public:
		STRINGorID(LPCTSTR lpString) : m_lpstr(lpString)
		{ }
		STRINGorID(UINT nID) : m_lpstr(MAKEINTRESOURCE(nID))
		{ }
		// F2 fix (C2665): a string literal is equally convertible to LPCTSTR and
		// to UINT, so the two overloads above are ambiguous. This array-reference
		// overload is a better match for literals (no conversion needed), which
		// resolves the ambiguity without touching any call site.
		template <std::size_t N>
		STRINGorID(const TCHAR (&szLiteral)[N]) : m_lpstr(szLiteral)
		{ }
		LPCTSTR m_lpstr;
	};

	/////////////////////////////////////////////////////////////////////////////////////
	//

	class UILIB_API CDuiPoint : public tagPOINT
	{
	public:
		CDuiPoint();
		CDuiPoint(const POINT& src);
		CDuiPoint(int x, int y);
		CDuiPoint(LPARAM lParam);
	};


	/////////////////////////////////////////////////////////////////////////////////////
	//

	class UILIB_API CDuiSize : public tagSIZE
	{
	public:
		CDuiSize();
		CDuiSize(const SIZE& src);
		CDuiSize(const RECT rc);
		CDuiSize(int cx, int cy);
	};


	/////////////////////////////////////////////////////////////////////////////////////
	//

	class UILIB_API CDuiRect : public tagRECT
	{
	public:
		CDuiRect();
		CDuiRect(const RECT& src);
		CDuiRect(int iLeft, int iTop, int iRight, int iBottom);

		int GetWidth() const;
		int GetHeight() const;
		void Empty();
		bool IsNull() const;
		void Join(const RECT& rc);
		void ResetOffset();
		void Normalize();
		void Offset(int cx, int cy);
		void Inflate(int cx, int cy);
		void Deflate(int cx, int cy);
		void Union(CDuiRect& rc);
	};

	/////////////////////////////////////////////////////////////////////////////////////
	//

	class UILIB_API CStdPtrArray
	{
	public:
		CStdPtrArray(int iPreallocSize = 0);
		CStdPtrArray(const CStdPtrArray& src);
		~CStdPtrArray();

		void Empty();
		void Resize(int iSize);
		bool IsEmpty() const;
		int Find(LPVOID iIndex) const;
		bool Add(LPVOID pData);
		bool SetAt(int iIndex, LPVOID pData);
		bool InsertAt(int iIndex, LPVOID pData);
		bool Remove(int iIndex);
		int GetSize() const;
		LPVOID* GetData();

		LPVOID GetAt(int iIndex) const;
		LPVOID operator[] (int nIndex) const;
		// S4/T4.1: C++17 迭代器接口（头文件内联、非导出、不改 ABI 布局）
		LPVOID* begin() { return m_items.data(); }
		LPVOID* end() { return m_items.data() + m_items.size(); }
		const LPVOID* begin() const { return m_items.data(); }
		const LPVOID* end() const { return m_items.data() + m_items.size(); }


	protected:
		// S2/T2.1: 内部改用 std::vector 承接存储与生命周期管理，
		// 公开 API 与方法语义逐字不变。
		// S4/T4.1: m_nCount / m_nAllocated 保留 —— 本类为 UILIB_API 导出类，
		// 依据 v2 C17.3「禁止在导出类中增删数据成员（会改 sizeof）」，
		// 收口只补 begin()/end() 内联迭代器，不删成员。
		std::vector<LPVOID> m_items;
		int m_nCount;
		int m_nAllocated;
	};


	/////////////////////////////////////////////////////////////////////////////////////
	//

	// S2/T2.2: 遗留类型，实测为死代码 —— 全仓库仅出现在本文件（声明）与
	// Utils.cpp（实现），无任何外部使用点。因属 UILIB_API 导出类，
	// 第三方代码可能引用，故保留定义不做移除，仅作标注。
	// 参见 docs/s2-container-usage.json 的 is_dead_code 结论。
	class UILIB_API CStdValArray
	{
	public:
		CStdValArray(int iElementSize, int iPreallocSize = 0);
		~CStdValArray();

		void Empty();
		bool IsEmpty() const;
		bool Add(LPCVOID pData);
		bool Remove(int iIndex);
		int GetSize() const;
		LPVOID GetData();

		LPVOID GetAt(int iIndex) const;
		LPVOID operator[] (int nIndex) const;

	protected:
		LPBYTE m_pVoid;
		int m_iElementSize;
		int m_nCount;
		int m_nAllocated;
	};


	/////////////////////////////////////////////////////////////////////////////////////
	//

	class UILIB_API CDuiString
	{
	public:
		enum { MAX_LOCAL_STRING_LEN = 63 };

		CDuiString();
		CDuiString(const TCHAR ch);
		CDuiString(const CDuiString& src);
		// T4.3: 移动构造（头文件 inline，不产生导出符号；类无虚函数，ABI 冻结面无变更）
		CDuiString(CDuiString&& src) noexcept
			: m_str(std::move(src.m_str))
		{ }
		CDuiString(LPCTSTR lpsz, int nLen = -1);
		~CDuiString();

		void Empty();
		int GetLength() const;
		bool IsEmpty() const;
		TCHAR GetAt(int nIndex) const;
		void Append(LPCTSTR pstr);
		void Assign(LPCTSTR pstr, int nLength = -1);
		LPCTSTR GetData() const;

		void SetAt(int nIndex, TCHAR ch);
		operator LPCTSTR() const;

		TCHAR operator[] (int nIndex) const;
		const CDuiString& operator=(const CDuiString& src);
		// T4.3: 移动赋值（头文件 inline，不产生导出符号；ABI 冻结面无变更）
		CDuiString& operator=(CDuiString&& src) noexcept
		{
			if (this != &src) m_str = std::move(src.m_str);
			return *this;
		}
		const CDuiString& operator=(const TCHAR ch);
		const CDuiString& operator=(LPCTSTR pstr);
#ifdef _UNICODE
		const CDuiString& operator=(LPCSTR lpStr);
		const CDuiString& operator+=(LPCSTR lpStr);
#else
		const CDuiString& operator=(LPCWSTR lpwStr);
		const CDuiString& operator+=(LPCWSTR lpwStr);
#endif
		CDuiString operator+(const CDuiString& src) const;
		CDuiString operator+(LPCTSTR pstr) const;
		const CDuiString& operator+=(const CDuiString& src);
		const CDuiString& operator+=(LPCTSTR pstr);
		const CDuiString& operator+=(const TCHAR ch);

		bool operator == (LPCTSTR str) const;
		bool operator != (LPCTSTR str) const;
		bool operator <= (LPCTSTR str) const;
		bool operator <  (LPCTSTR str) const;
		bool operator >= (LPCTSTR str) const;
		bool operator >  (LPCTSTR str) const;

		int Compare(LPCTSTR pstr) const;
		int CompareNoCase(LPCTSTR pstr) const;

		void MakeUpper();
		void MakeLower();

		CDuiString Left(int nLength) const;
		CDuiString Mid(int iPos, int nLength = -1) const;
		CDuiString Right(int nLength) const;
		CDuiString& TrimLeft();
		CDuiString& TrimRight();
		CDuiString& Trim();

		int Find(TCHAR ch, int iPos = 0) const;
		int Find(LPCTSTR pstr, int iPos = 0) const;
		int ReverseFind(TCHAR ch) const;
		int Replace(LPCTSTR pstrFrom, LPCTSTR pstrTo);

		int __cdecl Format(LPCTSTR pstrFormat, ...);
		int __cdecl SmallFormat(LPCTSTR pstrFormat, ...);

	protected:
		int __cdecl InnerFormat(LPCTSTR pstrFormat, va_list Args);

	protected:
		// S2/T2.4: 内部改用 std::basic_string<TCHAR> 承接存储与生命周期管理。
		// 选 basic_string<TCHAR> 而非 std::wstring，是为了一套代码同时覆盖
		// Unicode(wchar_t) 与 MBCS(char) 两种构型。
		//
		// 公开 API（~50 个方法）逐字保留；关键不变量：
		//   operator LPCTSTR() / GetData() 仍返回 NUL 结尾的连续指针
		//   （c_str() 满足，DLL 边界大量依赖此性质）。
		//
		// 注：sizeof(CDuiString) 已变化，DUILIB_ABI_TAG 已相应递增。
		std::basic_string<TCHAR> m_str;
	};

	static std::vector<CDuiString> StrSplit(CDuiString text, CDuiString sp)
	{
		std::vector<CDuiString> vResults;
		int pos = text.Find(sp, 0);
		while (pos >= 0)
		{
			CDuiString t = text.Left(pos);
			vResults.push_back(t);
			text = text.Right(text.GetLength() - pos - sp.GetLength());
			pos = text.Find(sp);
		}
		vResults.push_back(text);
		return vResults;
}
	/////////////////////////////////////////////////////////////////////////////////////
	//

	struct TITEM
	{
		CDuiString Key;
		LPVOID Data;
		// S2/T2.3: 由指针改为**下标**（-1 表示无，NODE_DEAD 表示已失效）。
		// 全仓库搜索确认 TITEM 仅在 Utils.h/Utils.cpp 内使用，无外部引用。
		int pPrev;
		int pNext;
	};

	class CStdStringPtrMap
	{
	public:
		// [EXPORT-2] Member-level export: the six protected helpers below were in
		// the export table purely because dllexport applies to the whole class. The
		// class is not inherited anywhere (verified), so they are implementation
		// detail and are now private and unexported.
		UILIB_API CStdStringPtrMap(int nSize = 83);
		UILIB_API ~CStdStringPtrMap();

		UILIB_API void Resize(int nSize = 83);
		UILIB_API LPVOID Find(LPCTSTR key, bool optimize = true) const;
		UILIB_API bool Insert(LPCTSTR key, LPVOID pData);
		UILIB_API LPVOID Set(LPCTSTR key, LPVOID pData);
		UILIB_API bool Remove(LPCTSTR key);
		UILIB_API void RemoveAll();
		UILIB_API int GetSize() const;
		UILIB_API LPCTSTR GetAt(int iIndex) const;
		UILIB_API LPCTSTR operator[] (int nIndex) const;

		// Copy/assign stay available. I tried deleting them ("nothing copies this"),
		// but CStdStringPtrMap is a MEMBER of CControlUI/CPaintManagerUI, so deleting
		// its copy ctor made every containing class non-copyable too -- the build
		// still linked, but 173 public symbols (control copy ctors) silently vanished
		// from the export table. That is exactly the kind of ABI loss this ratchet
		// exists to catch, so the deletion was reverted.
		UILIB_API CStdStringPtrMap(const CStdStringPtrMap&);
		UILIB_API CStdStringPtrMap& operator=(const CStdStringPtrMap&);


	private:
		// S2/T2.3: 内部改用 STL 容器承接生命周期，但**完整保留原始哈希桶结构**
		// （UINT HashKey / 桶数与索引 / 头插 / 桶降序扫描 / Find 的桶内重排），
		// 以确保 GetAt(i) 的返回序列与改造前逐字相同（87 个调用点的行为契约）。
		//
		// m_nodes  : 节点池。以**下标**而非指针互链，因此池扩容搬迁地址无害。
		// m_buckets: 桶表。存 m_nodes 下标，-1 表示空桶。
		// mutable：Find() 声明为 const，但其 optimize 分支会重排桶内链
		// （旧实现同样在 const 方法中修改链表，属既有权为）。用 mutable
		// 显式承认这一点，而非在调用处 const_cast。
		mutable std::vector<TITEM> m_nodes;
		mutable std::vector<int>   m_buckets;
		int m_nBuckets;
		int m_nCount;

		int  NodeCount() const { return static_cast<int>(m_nodes.size()); }
		int  AllocNode(const TITEM& item) const { m_nodes.push_back(item); return static_cast<int>(m_nodes.size()) - 1; }
		bool ValidNode(int i) const { return i >= 0 && i < static_cast<int>(m_nodes.size()); }

		// 节点存活标记：以 pPrev == -2 表示已失效（删除后不再归还下标，避免
		// 复用下标导致仍持有旧下标的桶链误指向新节点）
		static const int NODE_DEAD = -2;
		bool IsNodeAlive(int i) const { return ValidNode(i) && m_nodes[i].pPrev != NODE_DEAD; }
		void InvalidateNode(int i) const { if (ValidNode(i)) m_nodes[i].pPrev = NODE_DEAD; }

		// 把 iItem 挂到 slot 桶头（Find 的 optimize 重排用）
		void MoveNodeToBucketHead(int slot, int iItem) const
		{
			m_nodes[iItem].pPrev = -1;
			m_nodes[iItem].pNext = m_buckets[slot];
			if (m_buckets[slot] != -1)
				m_nodes[m_buckets[slot]].pPrev = iItem;
			m_buckets[slot] = iItem;
		}
	};

	/////////////////////////////////////////////////////////////////////////////////////
	//

	class UILIB_API CWaitCursor
	{
	public:
		CWaitCursor();
		~CWaitCursor();

	protected:
		HCURSOR m_hOrigCursor;
	};

	/////////////////////////////////////////////////////////////////////////////////////
	//

	class CDuiVariant : public VARIANT
	{
	public:
		CDuiVariant() 
		{ 
			VariantInit(this); 
		}
		CDuiVariant(int i)
		{
			VariantInit(this);
			this->vt = VT_I4;
			this->intVal = i;
		}
		CDuiVariant(float f)
		{
			VariantInit(this);
			this->vt = VT_R4;
			this->fltVal = f;
		}
		CDuiVariant(LPOLESTR s)
		{
			VariantInit(this);
			this->vt = VT_BSTR;
			this->bstrVal = s;
		}
		CDuiVariant(IDispatch *disp)
		{
			VariantInit(this);
			this->vt = VT_DISPATCH;
			this->pdispVal = disp;
		}

		~CDuiVariant() 
		{ 
			VariantClear(this); 
		}
	};

	//////////////////////////////////////////////////////////////////////////////////////
	//
	static char* w2a(wchar_t* lpszSrc, UINT   CodePage = CP_ACP)
	{
		if (lpszSrc != nullptr)
		{
			int  nANSILen = WideCharToMultiByte(CodePage, 0, lpszSrc, -1, nullptr, 0, nullptr, nullptr);
			char* pANSI = new char[nANSILen + 1];
			if (pANSI != nullptr)
			{
				ZeroMemory(pANSI, nANSILen + 1);
				WideCharToMultiByte(CodePage, 0, lpszSrc, -1, pANSI, nANSILen, nullptr, nullptr);
				return pANSI;
			}
		}	
		return nullptr;
	}

	static wchar_t* a2w(char* lpszSrc, UINT   CodePage = CP_ACP)
	{
		if (lpszSrc != nullptr)
		{
			int nUnicodeLen = MultiByteToWideChar(CodePage, 0, lpszSrc, -1, nullptr, 0);
			LPWSTR pUnicode = new WCHAR[nUnicodeLen + 1];
			if (pUnicode != nullptr)
			{
				ZeroMemory(static_cast<void*>(pUnicode), (nUnicodeLen + 1) * sizeof(WCHAR));
				MultiByteToWideChar(CodePage, 0, lpszSrc,-1, pUnicode, nUnicodeLen);
				return pUnicode;
			}
		}
		return nullptr;
	}

	///////////////////////////////////////////////////////////////////////////////////////
	////
	//struct TImageInfo;
	//class CPaintManagerUI;
	//class UILIB_API CImageString
	//{
	//public:
	//	CImageString();
	//	CImageString(const CImageString&);
	//	const CImageString& operator=(const CImageString&);
	//	virtual ~CImageString();

	//	const CDuiString& GetAttributeString() const;
	//	void SetAttributeString(LPCTSTR pStrImageAttri);
	//	void ModifyAttribute(LPCTSTR pStrModify);
	//	bool LoadImage(CPaintManagerUI* pManager);
	//	bool IsLoadSuccess();

	//	RECT GetDest() const;
	//	void SetDest(const RECT &rcDest);
	//	const TImageInfo* GetImageInfo() const;

	//private:
	//	void Clone(const CImageString&);
	//	void Clear();
	//	void ParseAttribute(LPCTSTR pStrImageAttri);

	//protected:
	//	friend class CRenderEngine;
	//	CDuiString	m_sImageAttribute;

	//	CDuiString	m_sImage;
	//	CDuiString	m_sResType;
	//	TImageInfo	*m_imageInfo;
	//	bool		m_bLoadSuccess;

	//	RECT	m_rcDest;
	//	RECT	m_rcSource;
	//	RECT	m_rcCorner;
	//	BYTE	m_bFade;
	//	DWORD	m_dwMask;
	//	bool	m_bHole;
	//	bool	m_bTiledX;
	//	bool	m_bTiledY;
	//};
}// namespace DuiLib

#endif // __UTILS_H__