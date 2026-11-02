#include "StdAfx.h"
#include "Utils.h"


// [T5.40] W4 batch-6: suppress data-truncation warnings (C4244/C4302).
// DuiLib sits at the GDI+/Win32 API boundary and relies on narrow types
// (int/BYTE/WORD) at call sites where value domains are provably bounded:
//   * LONG/LONG_PTR/UINT_PTR/WPARAM -> int  (UI coords/handles, int32 safe)
//   * LONG -> Gdiplus::REAL             (GDI+ API requires float coords)
//   * int  -> BYTE / float               (opacity / percentage within range)
//   * LPVOID -> int                     (CStdPtrArray int-as-pointer borrow,
//                                       see docs/t532-w4-b3-pointer-cast-audit.json)
//   * LPWSTR -> WORD/TCHAR              (16-bit unicode char truncation by design)
// Modernization path: migrate CStdPtrArray<LPVOID> to std::vector<int> /
// std::vector<COLORREF>, and use explicit width/height structs instead of
// mixing LONG with Gdiplus::REAL.  See docs/t538 and docs/t539 scans.
#pragma warning(disable: 4244 4302)
// [T5.40] end


namespace DuiLib
{

	/////////////////////////////////////////////////////////////////////////////////////
	//
	//

	CDuiPoint::CDuiPoint()
	{
		x = y = 0;
	}

	CDuiPoint::CDuiPoint(const POINT& src)
	{
		x = src.x;
		y = src.y;
	}

	CDuiPoint::CDuiPoint(int _x, int _y)
	{
		x = _x;
		y = _y;
	}

	CDuiPoint::CDuiPoint(LPARAM lParam)
	{
		x = GET_X_LPARAM(lParam);
		y = GET_Y_LPARAM(lParam);
	}


	/////////////////////////////////////////////////////////////////////////////////////
	//
	//

	CDuiSize::CDuiSize()
	{
		cx = cy = 0;
	}

	CDuiSize::CDuiSize(const SIZE& src)
	{
		cx = src.cx;
		cy = src.cy;
	}

	CDuiSize::CDuiSize(const RECT rc)
	{
		cx = rc.right - rc.left;
		cy = rc.bottom - rc.top;
	}

	CDuiSize::CDuiSize(int _cx, int _cy)
	{
		cx = _cx;
		cy = _cy;
	}


	/////////////////////////////////////////////////////////////////////////////////////
	//
	//

	CDuiRect::CDuiRect()
	{
		left = top = right = bottom = 0;
	}

	CDuiRect::CDuiRect(const RECT& src)
	{
		left = src.left;
		top = src.top;
		right = src.right;
		bottom = src.bottom;
	}

	CDuiRect::CDuiRect(int iLeft, int iTop, int iRight, int iBottom)
	{
		left = iLeft;
		top = iTop;
		right = iRight;
		bottom = iBottom;
	}

	int CDuiRect::GetWidth() const
	{
		return right - left;
	}

	int CDuiRect::GetHeight() const
	{
		return bottom - top;
	}

	void CDuiRect::Empty()
	{
		left = top = right = bottom = 0;
	}

	bool CDuiRect::IsNull() const
	{
		return (left == 0 && right == 0 && top == 0 && bottom == 0); 
	}

	void CDuiRect::Join(const RECT& rc)
	{
		if( rc.left < left ) left = rc.left;
		if( rc.top < top ) top = rc.top;
		if( rc.right > right ) right = rc.right;
		if( rc.bottom > bottom ) bottom = rc.bottom;
	}

	void CDuiRect::ResetOffset()
	{
		::OffsetRect(this, -left, -top);
	}

	void CDuiRect::Normalize()
	{
		if( left > right ) { int iTemp = left; left = right; right = iTemp; }
		if( top > bottom ) { int iTemp = top; top = bottom; bottom = iTemp; }
	}

	void CDuiRect::Offset(int cx, int cy)
	{
		::OffsetRect(this, cx, cy);
	}

	void CDuiRect::Inflate(int cx, int cy)
	{
		::InflateRect(this, cx, cy);
	}

	void CDuiRect::Deflate(int cx, int cy)
	{
		::InflateRect(this, -cx, -cy);
	}

	void CDuiRect::Union(CDuiRect& rc)
	{
		::UnionRect(this, this, &rc);
	}


	/////////////////////////////////////////////////////////////////////////////////////
	//
	//

		CStdPtrArray::CStdPtrArray(int iPreallocSize) : m_nCount(0), m_nAllocated(iPreallocSize)
	{
		ASSERT(iPreallocSize>=0);
		// S2/T2.1: 预分配容量（count 保持 0，与旧行为一致）
		if( iPreallocSize > 0 ) m_items.reserve(static_cast<size_t>(iPreallocSize));
	}

	CStdPtrArray::CStdPtrArray(const CStdPtrArray& src) : m_nCount(0), m_nAllocated(0)
	{
		for(int i=0; i<src.GetSize(); i++)
			Add(src.GetAt(i));
	}

	CStdPtrArray::~CStdPtrArray()
	{
		// S2/T2.1: 由 std::vector 自动释放，无需 free
	}

	void CStdPtrArray::Empty()
	{
		m_items.clear();
		m_nCount = m_nAllocated = 0;
	}

	void CStdPtrArray::Resize(int iSize)
	{
		// S2/T2.1: 复刻旧语义 —— 清空、分配 iSize 个 nullptr 槽位，
		// 且 count = allocated = iSize（"分配即计数"）
		ASSERT(iSize >= 0);
		m_items.assign(static_cast<size_t>(iSize), nullptr);
		m_nAllocated = iSize;
		m_nCount = iSize;
	}

	bool CStdPtrArray::IsEmpty() const
	{
		return m_nCount == 0;
	}

	bool CStdPtrArray::Add(LPVOID pData)
	{
		// S2/T2.1: 保留「先自增再比较 >=」的旧式边界语义。
		// 不变量：m_items.size() 必须始终等于 m_nCount（承载"已用槽位"）。
		if( ++m_nCount >= m_nAllocated ) {
			int nAllocated = m_nAllocated * 2;
			if( nAllocated == 0 ) nAllocated = 11;
			try {
				m_items.reserve(static_cast<size_t>(nAllocated));
			}
			catch( const std::bad_alloc& ) {
				--m_nCount;
				return false;
			}
			m_nAllocated = nAllocated;
		}
		// 同步 size 后再写入，避免越界（旧实现对裸内存无此约束）
		m_items.resize(static_cast<size_t>(m_nCount));
		m_items[static_cast<size_t>(m_nCount) - 1] = pData;
		return true;
	}

	bool CStdPtrArray::InsertAt(int iIndex, LPVOID pData)
	{
		if( iIndex == m_nCount ) return Add(pData);
		if( iIndex < 0 || iIndex > m_nCount ) return false;
		if( ++m_nCount >= m_nAllocated ) {
			int nAllocated = m_nAllocated * 2;
			if( nAllocated == 0 ) nAllocated = 11;
			try {
				m_items.reserve(static_cast<size_t>(nAllocated));
			}
			catch( const std::bad_alloc& ) {
				--m_nCount;
				return false;
			}
			m_nAllocated = nAllocated;
		}
		// 同步 size 后再右移（旧实现对裸内存无此约束）
		m_items.resize(static_cast<size_t>(m_nCount));
		// 复刻 memmove 右移语义
		for( int i = m_nCount - 1; i > iIndex; --i )
			m_items[static_cast<size_t>(i)] = m_items[static_cast<size_t>(i) - 1];
		m_items[static_cast<size_t>(iIndex)] = pData;
		return true;
	}

	bool CStdPtrArray::SetAt(int iIndex, LPVOID pData)
	{
		if( iIndex < 0 || iIndex >= m_nCount ) return false;
		m_items[static_cast<size_t>(iIndex)] = pData;
		return true;
	}

	bool CStdPtrArray::Remove(int iIndex)
	{
		if( iIndex < 0 || iIndex >= m_nCount ) return false;
		// 复刻「先 --count 再左移」的旧语义。
		// 注意：复位必须在左移**之后** —— 若先 resize(size-1) 会把待搬移的末元素丢弃。
		if( iIndex < --m_nCount )
			for( int i = iIndex; i < m_nCount; ++i )
				m_items[static_cast<size_t>(i)] = m_items[static_cast<size_t>(i) + 1];
		m_items.resize(static_cast<size_t>(m_nCount));
		return true;
	}

	int CStdPtrArray::Find(LPVOID pData) const
	{
		for( int i = 0; i < m_nCount; i++ ) if( m_items[static_cast<size_t>(i)] == pData ) return i;
		return -1;
	}

	int CStdPtrArray::GetSize() const
	{
		return m_nCount;
	}

	LPVOID* CStdPtrArray::GetData()
	{
		// S2/T2.1: 必须返回**可原地修改**的连续指针 ——
		// 实测 UICombo.cpp:1263/1302 与 UIList.cpp:1256 用它做 qsort_s 原地排序。
		// 空时返回 nullptr 以贴近旧行为（旧实现在空时 m_ppVoid 为 nullptr）。
		if( m_nCount == 0 ) return nullptr;
		return m_items.data();
	}

	LPVOID CStdPtrArray::GetAt(int iIndex) const
	{
		if( iIndex < 0 || iIndex >= m_nCount ) return nullptr;
		return m_items[static_cast<size_t>(iIndex)];
	}

	LPVOID CStdPtrArray::operator[] (int iIndex) const
	{
		// [SEC-15] ASSERT maps to _ASSERTE, which raises a *modal* dialog in debug
		// builds; an unattended process then blocks forever instead of failing.
		// Diagnostic assertion only when a debugger is attached; the bound is
		// enforced in every configuration.
		if( iIndex < 0 || iIndex >= m_nCount )
		{
			if( ::IsDebuggerPresent() ) ASSERT(FALSE);
			return nullptr;
		}
		return m_items[static_cast<size_t>(iIndex)];
	}
CStdValArray::CStdValArray(int iElementSize, int iPreallocSize /*= 0*/) : 
	m_pVoid(nullptr), 
		m_nCount(0), 
		m_iElementSize(iElementSize), 
		m_nAllocated(iPreallocSize)
	{
		ASSERT(iElementSize>0);
		ASSERT(iPreallocSize>=0);
		if( iPreallocSize > 0 ) m_pVoid = static_cast<LPBYTE>(malloc(iPreallocSize * m_iElementSize));
	}

	CStdValArray::~CStdValArray()
	{
		if( m_pVoid != nullptr ) free(m_pVoid);
	}

	void CStdValArray::Empty()
	{   
		m_nCount = 0;  // NOTE: We keep the memory in place
	}

	bool CStdValArray::IsEmpty() const
	{
		return m_nCount == 0;
	}

	bool CStdValArray::Add(LPCVOID pData)
	{
		if( ++m_nCount >= m_nAllocated) {
			int nAllocated = m_nAllocated * 2;
			if( nAllocated == 0 ) nAllocated = 11;
			LPBYTE pVoid = static_cast<LPBYTE>(realloc(m_pVoid, nAllocated * m_iElementSize));
			if( pVoid != nullptr ) {
				m_nAllocated = nAllocated;
				m_pVoid = pVoid;
			}
			else {
				--m_nCount;
				return false;
			}
		}
		::CopyMemory(m_pVoid + ((m_nCount - 1) * m_iElementSize), pData, m_iElementSize);
		return true;
	}

	bool CStdValArray::Remove(int iIndex)
	{
		if( iIndex < 0 || iIndex >= m_nCount ) return false;
		if( iIndex < --m_nCount ) ::CopyMemory(m_pVoid + (iIndex * m_iElementSize), m_pVoid + ((iIndex + 1) * m_iElementSize), (m_nCount - iIndex) * m_iElementSize);
		return true;
	}

	int CStdValArray::GetSize() const
	{
		return m_nCount;
	}

	LPVOID CStdValArray::GetData()
	{
		return static_cast<LPVOID>(m_pVoid);
	}

	LPVOID CStdValArray::GetAt(int iIndex) const
	{
		if( iIndex < 0 || iIndex >= m_nCount ) return nullptr;
		return m_pVoid + (iIndex * m_iElementSize);
	}

	LPVOID CStdValArray::operator[] (int iIndex) const
	{
		ASSERT(iIndex>=0 && iIndex<m_nCount);
		return m_pVoid + (iIndex * m_iElementSize);
	}


	/////////////////////////////////////////////////////////////////////////////////////
	//
	//

	CDuiString::CDuiString()
	{
		// S2/T2.4: 由 std::basic_string<TCHAR> 托管，默认即空串
	}

	CDuiString::CDuiString(const TCHAR ch)
	{
		m_str.assign(1, ch);
	}

	CDuiString::CDuiString(LPCTSTR lpsz, int nLen)
	{
		ASSERT(!::IsBadStringPtr(lpsz,-1) || lpsz==nullptr);
		Assign(lpsz, nLen);
	}

	CDuiString::CDuiString(const CDuiString& src)
	{
		m_str = src.m_str;
	}

	CDuiString::~CDuiString()
	{
		// S2/T2.4: 由 std::basic_string 自动释放
	}

	int CDuiString::GetLength() const
	{ 
		return static_cast<int>(m_str.length()); 
	}

	CDuiString::operator LPCTSTR() const 
	{ 
		return m_str.c_str(); 
	}

	void CDuiString::Append(LPCTSTR pstr)
	{
		if( pstr == nullptr ) return;
		m_str.append(pstr);
	}

	void CDuiString::Assign(LPCTSTR pstr, int cchMax)
	{
		if( pstr == nullptr ) pstr = _T("");
		cchMax = (cchMax < 0 ? static_cast<int>(_tcslen(pstr)) : cchMax);
		// 复刻「截断到 cchMax」语义
		m_str.assign(pstr, static_cast<size_t>(cchMax));
	}

	bool CDuiString::IsEmpty() const 
	{ 
		return m_str.empty(); 
	}

	void CDuiString::Empty() 
	{ 
		m_str.clear();
	}

	LPCTSTR CDuiString::GetData() const
	{
		return m_str.c_str();
	}

	TCHAR CDuiString::GetAt(int nIndex) const
	{
		return m_str[static_cast<size_t>(nIndex)];
	}

	TCHAR CDuiString::operator[] (int nIndex) const
	{ 
		return m_str[static_cast<size_t>(nIndex)];
	}   

	const CDuiString& CDuiString::operator=(const CDuiString& src)
	{      
		m_str = src.m_str;
		return *this;
	}

	const CDuiString& CDuiString::operator=(LPCTSTR lpStr)
	{      
		if ( lpStr )
		{
			ASSERT(!::IsBadStringPtr(lpStr,-1));
			m_str = lpStr;
		}
		return *this;
	}

#ifdef _UNICODE
	// Unicode build only: Utils.h declares operator+=(LPCSTR) in the #ifdef _UNICODE
	// branch, where LPCSTR is a different type from the internal TCHAR (wchar_t)
	// storage, so this overload needs its own definition.  In the MultiByte build
	// LPCSTR is the same type as LPCTSTR and the declaration merges with
	// operator+=(LPCTSTR).  The narrow argument is widened through CP_ACP and
	// appended to the internal std::basic_string<wchar_t>.
	const CDuiString& CDuiString::operator+=(LPCSTR lpStr)
	{
		// Append-nothing must be a no-op: a null or empty source leaves *this
		// untouched.  This is intentionally different from operator=(LPCSTR),
		// where a null source clears the string.
		if ( lpStr == nullptr ) return *this;
		if ( lpStr[0] == '\0' ) return *this;

		// Size query: -1 means "up to and including the terminating NUL".
		const int nLen = ::MultiByteToWideChar(CP_ACP, 0, lpStr, -1, nullptr, 0);
		if ( nLen <= 1 )
		{
			// 0 == conversion failure, 1 == only the NUL: nothing to append.
			return *this;
		}

		// Zero-initialised scratch buffer; ANSI->wide never exceeds nLen wchar_t.
		std::vector<wchar_t> buf(static_cast<size_t>(nLen));
		const int nWritten = ::MultiByteToWideChar(CP_ACP, 0, lpStr, -1, buf.data(), nLen);
		if ( nWritten != nLen )
		{
			// Conversion failed mid-way: leave *this unchanged, do not append half.
			return *this;
		}

		// Drop the trailing NUL that MultiByteToWideChar wrote, then append.
		m_str.append(buf.data(), static_cast<size_t>(nWritten - 1));
		return *this;
	}
	// Unicode build only: Utils.h declares operator=(LPCSTR) on the same line as the
	// append overload above, but only the append one had a definition. A consumer
	// writing `s = "literal";` therefore failed to link with LNK2019 on
	// ??4CDuiString@DuiLib@@QEAAAEBV01@PEBD@Z (measured by the append probe).
	// Semantics follow operator=(LPCWSTR) in the MBCS branch: assigning nothing clears.
	const CDuiString& CDuiString::operator=(LPCSTR lpStr)
	{
		// Null clears rather than dereferences. Note this differs from operator+=(LPCSTR)
		// just above, which leaves the string untouched -- assignment replaces, append adds.
		if ( lpStr == nullptr )
		{
			m_str.clear();
			return *this;
		}

		const int nLen = ::MultiByteToWideChar(CP_ACP, 0, lpStr, -1, nullptr, 0);
		if ( nLen <= 1 )
		{
			// 0 == conversion failure, 1 == only the NUL: both yield an empty string.
			m_str.clear();
			return *this;
		}

		std::vector<wchar_t> buf(static_cast<size_t>(nLen));
		const int nWritten = ::MultiByteToWideChar(CP_ACP, 0, lpStr, -1, buf.data(), nLen);
		if ( nWritten != nLen )
		{
			// Do not leave a half-converted value behind.
			m_str.clear();
			return *this;
		}

		m_str.assign(buf.data(), static_cast<size_t>(nWritten - 1));
		return *this;
	}
#endif // _UNICODE

#ifndef _UNICODE
	// MultiByte build only: Utils.h declares operator=(LPCWSTR) and operator+=(LPCWSTR)
	// in the #else branch of the _UNICODE switch.  Under _UNICODE, LPCWSTR is the
	// same type as LPCTSTR, so the declaration merges with operator=(LPCTSTR) and
	// no separate definition is needed.  Under MultiByte they are distinct types,
	// so the overload needs its own definition or every call site (for example
	// CRichEditUI::GetLine, which assigns an EM_GETLINE result) fails with
	// LNK2019.  This definition converts via CP_ACP into the internal
	// std::basic_string<char> storage.
	const CDuiString& CDuiString::operator=(LPCWSTR lpwStr)
	{
		// Null source clears the string instead of dereferencing nullptr.
		if ( lpwStr == nullptr )
		{
			m_str.clear();
			return *this;
		}

		// Size query: -1 means "up to and including the terminating NUL".
		const int nLen = ::WideCharToMultiByte(CP_ACP, 0, lpwStr, -1, nullptr, 0, nullptr, nullptr);
		if ( nLen <= 1 )
		{
			// 0 == conversion failure, 1 == only the NUL: both yield an empty string.
			m_str.clear();
			return *this;
		}

		// Zero-initialised scratch buffer; wide->ANSI never exceeds nLen bytes.
		std::vector<char> buf(static_cast<size_t>(nLen));
		const int nWritten = ::WideCharToMultiByte(CP_ACP, 0, lpwStr, -1, buf.data(), nLen, nullptr, nullptr);
		if ( nWritten != nLen )
		{
			// Conversion failed mid-way: do not leave a half-converted value behind.
			m_str.clear();
			return *this;
		}

		// Drop the trailing NUL that WideCharToMultiByte wrote.
		m_str.assign(buf.data(), static_cast<size_t>(nWritten - 1));
		return *this;
	}

	// MultiByte build only: Utils.h declares operator+=(LPCWSTR) in the #else branch
	// of the _UNICODE switch, where LPCWSTR is a different type from the internal
	// TCHAR (char) storage.  The wide argument is narrowed through CP_ACP and
	// appended to the internal std::basic_string<char>.
	const CDuiString& CDuiString::operator+=(LPCWSTR lpwStr)
	{
		// Append-nothing must be a no-op: a null or empty source leaves *this
		// untouched.  This is intentionally different from operator=(LPCWSTR),
		// where a null source clears the string.
		if ( lpwStr == nullptr ) return *this;
		if ( lpwStr[0] == L'\0' ) return *this;

		// Size query: -1 means "up to and including the terminating NUL".
		const int nLen = ::WideCharToMultiByte(CP_ACP, 0, lpwStr, -1, nullptr, 0, nullptr, nullptr);
		if ( nLen <= 1 )
		{
			// 0 == conversion failure, 1 == only the NUL: nothing to append.
			return *this;
		}

		// Zero-initialised scratch buffer; wide->ANSI never exceeds nLen bytes.
		std::vector<char> buf(static_cast<size_t>(nLen));
		const int nWritten = ::WideCharToMultiByte(CP_ACP, 0, lpwStr, -1, buf.data(), nLen, nullptr, nullptr);
		if ( nWritten != nLen )
		{
			// Conversion failed mid-way: leave *this unchanged, do not append half.
			return *this;
		}

		// Drop the trailing NUL that WideCharToMultiByte wrote, then append.
		m_str.append(buf.data(), static_cast<size_t>(nWritten - 1));
		return *this;
	}
#endif // !_UNICODE

	CDuiString CDuiString::operator+(const CDuiString& src) const
	{
		CDuiString sNew = *this;
		sNew.m_str += src.m_str;
		return sNew;
	}

	CDuiString CDuiString::operator+(LPCTSTR pstr) const
	{
		CDuiString sNew = *this;
		if ( pstr ) sNew.m_str += pstr;
		return sNew;
	}

	const CDuiString& CDuiString::operator+=(const CDuiString& src)
	{
		m_str += src.m_str;
		return *this;
	}

	const CDuiString& CDuiString::operator+=(LPCTSTR pstr)
	{
		if ( pstr ) m_str += pstr;
		return *this;
	}

	const CDuiString& CDuiString::operator+=(const TCHAR ch)
	{
		m_str += ch;
		return *this;
	}

	int CDuiString::Compare(LPCTSTR pstr) const
	{
		// [SEC-13] _tcscmp(lhs, nullptr) dereferences the null pointer and the
		// process dies with an access violation. _tcsicmp with a null rhs is
		// worse: UCRT spins inside it and the call never returns (observed as a
		// silent hang, not a crash). Treat a null argument as the empty string,
		// which matches the existing CDuiString(nullptr) -> length 0 semantics.
		if( pstr == nullptr ) return m_str.empty() ? 0 : 1;
		return _tcscmp(m_str.c_str(), pstr);
	}

	int CDuiString::CompareNoCase(LPCTSTR pstr) const
	{
		// [SEC-13] see Compare(): null rhs must not reach _tcsicmp.
		if( pstr == nullptr ) return m_str.empty() ? 0 : 1;
		return _tcsicmp(m_str.c_str(), pstr);
	}

	void CDuiString::MakeUpper()
	{
		for( size_t i = 0; i < m_str.length(); ++i )
			m_str[i] = reinterpret_cast<TCHAR>(::CharUpper(reinterpret_cast<LPTSTR>(static_cast<ULONG_PTR>(m_str[i]))));
	}

	void CDuiString::MakeLower()
	{
		for( size_t i = 0; i < m_str.length(); ++i )
			m_str[i] = reinterpret_cast<TCHAR>(::CharLower(reinterpret_cast<LPTSTR>(static_cast<ULONG_PTR>(m_str[i]))));
	}

	CDuiString CDuiString::Left(int iLength) const
	{
		if( iLength > static_cast<int>(m_str.length()) ) iLength = static_cast<int>(m_str.length());
		if( iLength < 0 ) iLength = 0;
		CDuiString sNew;
		sNew.m_str.assign(m_str, 0, static_cast<size_t>(iLength));
		return sNew;
	}

	CDuiString CDuiString::Mid(int iPos, int iLength) const
	{
		if( iPos > static_cast<int>(m_str.length()) ) iPos = static_cast<int>(m_str.length());
		if( iPos < 0 ) iPos = 0;
		int nRemain = static_cast<int>(m_str.length()) - iPos;
		if( iLength < 0 || iLength > nRemain ) iLength = nRemain;
		CDuiString sNew;
		sNew.m_str.assign(m_str, static_cast<size_t>(iPos), static_cast<size_t>(iLength));
		return sNew;
	}

	CDuiString CDuiString::Right(int iLength) const
	{
		if( iLength > static_cast<int>(m_str.length()) ) iLength = static_cast<int>(m_str.length());
		if( iLength < 0 ) iLength = 0;
		int iPos = static_cast<int>(m_str.length()) - iLength;
		CDuiString sNew;
		sNew.m_str.assign(m_str, static_cast<size_t>(iPos), static_cast<size_t>(iLength));
		return sNew;
	}

	CDuiString& CDuiString::TrimLeft()
	{
		size_t i = 0;
		while( i < m_str.length() && _istspace(m_str[i]) ) ++i;
		m_str.erase(0, i);
		return *this;
	}

	CDuiString& CDuiString::TrimRight()
	{
		size_t n = m_str.length();
		while( n > 0 && _istspace(m_str[n - 1]) ) --n;
		m_str.erase(n);
		return *this;
	}

	CDuiString& CDuiString::Trim()
	{
		TrimRight();
		return TrimLeft();
	}

	int CDuiString::Find(TCHAR ch, int iPos) const
	{
		size_t r = m_str.find(ch, static_cast<size_t>(iPos < 0 ? 0 : iPos));
		return (r == std::basic_string<TCHAR>::npos) ? -1 : static_cast<int>(r);
	}

	int CDuiString::Find(LPCTSTR pstr, int iPos) const
	{
		if( pstr == nullptr ) return -1;
		size_t r = m_str.find(pstr, static_cast<size_t>(iPos < 0 ? 0 : iPos));
		return (r == std::basic_string<TCHAR>::npos) ? -1 : static_cast<int>(r);
	}

	int CDuiString::ReverseFind(TCHAR ch) const
	{
		size_t r = m_str.rfind(ch);
		return (r == std::basic_string<TCHAR>::npos) ? -1 : static_cast<int>(r);
	}

	int CDuiString::Replace(LPCTSTR pstrFrom, LPCTSTR pstrTo)
	{
		if( pstrFrom == nullptr || pstrTo == nullptr ) return 0;
		const size_t nFromLen = _tcslen(pstrFrom);
		if( nFromLen == 0 ) return 0;
		int nReplaced = 0;
		size_t iPos = 0;
		while( (iPos = m_str.find(pstrFrom, iPos)) != std::basic_string<TCHAR>::npos ) {
			m_str.replace(iPos, nFromLen, pstrTo);
			iPos += _tcslen(pstrTo);
			++nReplaced;
		}
		return nReplaced;
	}

	int __cdecl CDuiString::Format(LPCTSTR pstrFormat, ...)
	{
		va_list Args;
		va_start(Args, pstrFormat);
		int nRet = InnerFormat(pstrFormat, Args);
		va_end(Args);
		return nRet;
	}

	int __cdecl CDuiString::SmallFormat(LPCTSTR pstrFormat, ...)
	{
		// 复刻旧语义：SmallFormat 与 Format 行为一致（旧实现同样转发）
		va_list Args;
		va_start(Args, pstrFormat);
		int nRet = InnerFormat(pstrFormat, Args);
		va_end(Args);
		return nRet;
	}

	int __cdecl CDuiString::InnerFormat(LPCTSTR pstrFormat, va_list Args)
	{
		// 复刻旧实现：先按较小缓冲试探，不足则扩容重试
		TCHAR szBuffer[256] = { 0 };
		int nRet = _vsntprintf(szBuffer, 255, pstrFormat, Args);
		if( nRet >= 0 && nRet < 255 ) {
			m_str = szBuffer;
			return nRet;
		}
		// 需要更大缓冲
		int nNeed = (nRet < 0) ? 1024 : (nRet + 1);
		std::vector<TCHAR> vBuf(static_cast<size_t>(nNeed) + 1, 0);
		va_list ArgsCopy;
		va_copy(ArgsCopy, Args);
		nRet = _vsntprintf(vBuf.data(), static_cast<size_t>(nNeed), pstrFormat, ArgsCopy);
		va_end(ArgsCopy);
		vBuf[static_cast<size_t>(nNeed)] = 0;
		m_str = vBuf.data();
		return nRet;
	}

	void CDuiString::SetAt(int nIndex, TCHAR ch)
	{
		ASSERT(nIndex>=0 && nIndex<GetLength());
		m_str[static_cast<size_t>(nIndex)] = ch;
	}

	bool CDuiString::operator == (LPCTSTR str) const { return (Compare(str) == 0); };
	bool CDuiString::operator != (LPCTSTR str) const { return (Compare(str) != 0); };
	bool CDuiString::operator <= (LPCTSTR str) const { return (Compare(str) <= 0); };
	bool CDuiString::operator <  (LPCTSTR str) const { return (Compare(str) <  0); };
	bool CDuiString::operator >= (LPCTSTR str) const { return (Compare(str) >= 0); };
	bool CDuiString::operator >  (LPCTSTR str) const { return (Compare(str) >  0); };
	static UINT HashKey(LPCTSTR Key)
	{
		UINT i = 0;
		SIZE_T len = _tcslen(Key);
		while( len-- > 0 ) i = (i << 5) + i + Key[len];
		return i;
	}

	static UINT HashKey(const CDuiString& Key)
	{
		return HashKey(static_cast<LPCTSTR>(Key));
	};

	CStdStringPtrMap::CStdStringPtrMap(int nSize) : m_nCount(0)
	{
		// S2/T2.3: 保留 nSize<16 时提升为 16 的旧语义
		if( nSize < 16 ) nSize = 16;
		m_nBuckets = nSize;
		m_buckets.assign(static_cast<size_t>(nSize), -1);
	}

	// [EXPORT-2] Defined out of line and exported: these were always part of the
	// class's implicit interface (CStdStringPtrMap is a member of CControlUI and
	// CPaintManagerUI, so their copy semantics depend on it). Keeping them exported
	// preserves the 173 public symbols that mistake once removed.
	CStdStringPtrMap::CStdStringPtrMap(const CStdStringPtrMap&) = default;
	CStdStringPtrMap& CStdStringPtrMap::operator=(const CStdStringPtrMap&) = default;

	CStdStringPtrMap::~CStdStringPtrMap()
	{
		// S2/T2.3: 由容器自动释放，无需手工 delete
	}

	void CStdStringPtrMap::RemoveAll()
	{
		// S2/T2.3: 复刻旧语义 —— 只清节点与计数，桶表保持已分配状态
		m_nodes.clear();
		std::fill(m_buckets.begin(), m_buckets.end(), -1);
		m_nCount = 0;
	}

	void CStdStringPtrMap::Resize(int nSize)
	{
		// S2/T2.3: 完全复刻旧语义 —— 释放全部节点、按 nSize 重建桶表并清零计数
		m_nodes.clear();
		m_buckets.clear();

		// [FIX-NAMEMAP-ZEROBUCKETS] Keep the ctor invariant: at least 16 buckets.
		// Resize(0) used to leave m_nBuckets == 0, and Find/Insert/Set all bail
		// out early when m_nBuckets == 0, so the map stayed permanently dead.
		// CPaintManagerUI::Init() calls m_mNameHash.Resize(0); afterwards every
		// Set() from InitControls/__FindControlFromNameHash silently failed, so
		// CPaintManagerUI::FindControl(LPCTSTR) always returned nullptr and every
		// page-container lookup in the deferred-build path early-returned, which
		// is exactly the reported "every page opens blank" symptom. Clamping here
		// degrades "clear" to RemoveAll semantics instead of destroying the map.
		if( nSize < 16 ) nSize = 16;
		m_buckets.assign(static_cast<size_t>(nSize), -1);
		m_nBuckets = nSize;
		m_nCount = 0;
	}

	LPVOID CStdStringPtrMap::Find(LPCTSTR key, bool optimize) const
	{
		// [SEC-14] HashKey() calls _tcslen(key); a null key crashed the process.
		if( key == nullptr || m_nBuckets == 0 || GetSize() == 0 ) return nullptr;

		UINT slot = HashKey(key) % m_nBuckets;
		int iPrev = -1;
		for( int iItem = m_buckets[slot]; iItem != -1; iItem = m_nodes[iItem].pNext ) {
			if( m_nodes[iItem].Key == key ) {
				// 复刻旧语义：命中且不在桶头时，把该节点移动到桶头（自修改优化）
				if (optimize && iPrev != -1) {
					// 从原位置摘除
					if (m_nodes[iItem].pNext != -1)
						m_nodes[m_nodes[iItem].pNext].pPrev = m_nodes[iItem].pPrev;
					m_nodes[m_nodes[iItem].pPrev].pNext = m_nodes[iItem].pNext;
					// 挂到桶头
					MoveNodeToBucketHead(slot, iItem);
				}
				return m_nodes[iItem].Data;
			}
			iPrev = iItem;
		}

		return nullptr;
	}

	bool CStdStringPtrMap::Insert(LPCTSTR key, LPVOID pData)
	{
		if( key == nullptr || m_nBuckets == 0 ) return false; // [SEC-14]
		if( Find(key) ) return false;

		// Add first in bucket（复刻：头插）
		UINT slot = HashKey(key) % m_nBuckets;
		TITEM t;
		t.Key = key;
		t.Data = pData;
		t.pPrev = -1;
		t.pNext = m_buckets[slot];
		int iNew = AllocNode(t);
		if (m_buckets[slot] != -1)
			m_nodes[m_buckets[slot]].pPrev = iNew;
		m_buckets[slot] = iNew;
		m_nCount++;
		return true;
	}

	LPVOID CStdStringPtrMap::Set(LPCTSTR key, LPVOID pData)
	{
		if( key == nullptr || m_nBuckets == 0 ) return pData; // [SEC-14]

		if (GetSize()>0) {
			UINT slot = HashKey(key) % m_nBuckets;
			// Modify existing item
			for( int iItem = m_buckets[slot]; iItem != -1; iItem = m_nodes[iItem].pNext ) {
				if( m_nodes[iItem].Key == key ) {
					LPVOID pOldData = m_nodes[iItem].Data;
					m_nodes[iItem].Data = pData;
					return pOldData;
				}
			}
		}

		Insert(key, pData);
		return nullptr;
	}

	bool CStdStringPtrMap::Remove(LPCTSTR key)
	{
		if( key == nullptr || m_nBuckets == 0 || GetSize() == 0 ) return false; // [SEC-14]

		UINT slot = HashKey(key) % m_nBuckets;
		int iPrev = -1;
		int iItem = m_buckets[slot];
		while( iItem != -1 ) {
			if( m_nodes[iItem].Key == key ) {
				int iNext = m_nodes[iItem].pNext;
				if( iPrev == -1 ) m_buckets[slot] = iNext;
				else m_nodes[iPrev].pNext = iNext;
				if( iNext != -1 ) m_nodes[iNext].pPrev = iPrev;
				// 复刻旧语义：把该节点从链上摘除，并标记失效（不归还下标，
				// 避免仍持有旧下标的桶链误指向复用后的新节点）。
				InvalidateNode(iItem);
				m_nCount--;
				return true;
			}
			iPrev = iItem;
			iItem = m_nodes[iItem].pNext;
		}

		return false;
	}

	int CStdStringPtrMap::GetSize() const
	{
		return m_nCount;
	}

	LPCTSTR CStdStringPtrMap::GetAt(int iIndex) const
	{
		// [SEC-16] negative index never matches `pos++ == iIndex`, so the scan
		// walked every bucket before returning null; reject it up front.
		if( iIndex < 0 || m_nBuckets == 0 || GetSize() == 0 ) return nullptr;

		int pos = 0;
		int len = m_nBuckets;
		while( len-- ) {
			for( int iItem = m_buckets[len]; iItem != -1; iItem = m_nodes[iItem].pNext ) {
				if( IsNodeAlive(iItem) ) {
					if( pos++ == iIndex ) {
						return m_nodes[iItem].Key.GetData();
					}
				}
			}
		}

		return nullptr;
	}

	LPCTSTR CStdStringPtrMap::operator[] (int nIndex) const
	{
		return GetAt(nIndex);
	}



	/////////////////////////////////////////////////////////////////////////////////////
	//
	//

	CWaitCursor::CWaitCursor()
	{
		m_hOrigCursor = ::SetCursor(::LoadCursor(nullptr, IDC_WAIT));
	}

	CWaitCursor::~CWaitCursor()
	{
		::SetCursor(m_hOrigCursor);
	}

} // namespace DuiLib