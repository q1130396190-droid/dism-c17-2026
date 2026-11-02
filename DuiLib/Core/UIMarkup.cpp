#include "StdAfx.h"


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


#ifndef TRACE
#define TRACE
#endif

namespace DuiLib {
///////////////////////////////////////////////////////////////////////////////////////
//
//
//
// [SEC-01] XML parse-recursion guard.
//
// CMarkup::_Parse() recurses once per nesting level with no depth bound.  A
// crafted skin file (~1.2 MB of "<a>" repeated) exhausts the parser stack and
// crashes the host process: AddressSanitizer reports stack-overflow.  Because
// skin/XML resources may be attacker-supplied (theme packs, downloaded skins,
// resource DLLs), this is a remote denial-of-service.
//
// The guard is a thread_local counter instead of a new member or parameter so
// that the CMarkup class layout and every private symbol stay unchanged
// (ABI-freeze surface, docs/abi-freeze-surface.json).
namespace {
    constexpr int kMaxXmlParseDepth = 64;
    thread_local int t_nXmlParseDepth = 0;

    struct CXmlDepthGuard
    {
        bool bExceeded;
        CXmlDepthGuard() : bExceeded(++t_nXmlParseDepth > kMaxXmlParseDepth) {}
        ~CXmlDepthGuard() { --t_nXmlParseDepth; }
    };
} // namespace

CMarkupNode::CMarkupNode() : m_iPos(0), m_nAttributes(0), m_pOwner(nullptr)
{
}

CMarkupNode::CMarkupNode(CMarkup* pOwner, int iPos) : m_pOwner(pOwner), m_iPos(iPos), m_nAttributes(0)
{
}

CMarkupNode CMarkupNode::GetSibling()
{
    if( !_HasValidPos() ) return CMarkupNode(); // [SEC-08]
    ULONG iPos = m_pOwner->m_pElements[m_iPos].iNext;
    if( iPos == 0 || iPos >= m_pOwner->m_nElements ) return CMarkupNode(); // [SEC-08]
    return CMarkupNode(m_pOwner, iPos);
}

bool CMarkupNode::HasSiblings() const
{
    if( !_HasValidPos() ) return false; // [SEC-08]
    ULONG iPos = m_pOwner->m_pElements[m_iPos].iNext;
    return iPos > 0;
}

CMarkupNode CMarkupNode::GetChild()
{
    if( !_HasValidPos() ) return CMarkupNode(); // [SEC-08]
    ULONG iPos = m_pOwner->m_pElements[m_iPos].iChild;
    if( iPos == 0 || iPos >= m_pOwner->m_nElements ) return CMarkupNode(); // [SEC-08]
    return CMarkupNode(m_pOwner, iPos);
}

CMarkupNode CMarkupNode::GetChild(LPCTSTR pstrName)
{
    if( !_HasValidPos() ) return CMarkupNode(); // [SEC-08]
    ULONG iPos = m_pOwner->m_pElements[m_iPos].iChild;
    while( iPos != 0 ) {
        if( iPos >= m_pOwner->m_nElements ) return CMarkupNode(); // [SEC-08] broken list
        if( _tcsicmp(m_pOwner->m_pstrXML + m_pOwner->m_pElements[iPos].iStart, pstrName) == 0 ) {
            return CMarkupNode(m_pOwner, iPos);
        }
        iPos = m_pOwner->m_pElements[iPos].iNext;
    }
    return CMarkupNode();
}

bool CMarkupNode::HasChildren() const
{
    if( !_HasValidPos() ) return false; // [SEC-08]
    return m_pOwner->m_pElements[m_iPos].iChild != 0;
}

CMarkupNode CMarkupNode::GetParent()
{
    if( !_HasValidPos() ) return CMarkupNode(); // [SEC-08]
    ULONG iPos = m_pOwner->m_pElements[m_iPos].iParent;
    if( iPos == 0 || iPos >= m_pOwner->m_nElements ) return CMarkupNode(); // [SEC-08]
    return CMarkupNode(m_pOwner, iPos);
}

bool CMarkupNode::IsValid() const
{
    return _HasValidPos(); // [SEC-08] owner alone is not enough: the index must be in range
}

bool CMarkupNode::_HasValidPos() const
{
    return m_pOwner != nullptr && m_iPos > 0
        && static_cast<ULONG>(m_iPos) < m_pOwner->m_nElements;
}

LPCTSTR CMarkupNode::GetName() const
{
    if( !_HasValidPos() ) return nullptr; // [SEC-08]
    return m_pOwner->m_pstrXML + m_pOwner->m_pElements[m_iPos].iStart;
}

LPCTSTR CMarkupNode::GetValue() const
{
    if( !_HasValidPos() ) return nullptr; // [SEC-08]
    return m_pOwner->m_pstrXML + m_pOwner->m_pElements[m_iPos].iData;
}

LPCTSTR CMarkupNode::GetAttributeName(int iIndex)
{
    if( !_HasValidPos() ) return nullptr; // [SEC-08]
    if( m_nAttributes == 0 ) _MapAttributes();
    if( iIndex < 0 || iIndex >= m_nAttributes ) return _T("");
    return m_pOwner->m_pstrXML + m_aAttributes[iIndex].iName;
}

LPCTSTR CMarkupNode::GetAttributeValue(int iIndex)
{
    if( !_HasValidPos() ) return nullptr; // [SEC-08]
    if( m_nAttributes == 0 ) _MapAttributes();
    if( iIndex < 0 || iIndex >= m_nAttributes ) return _T("");
    return m_pOwner->m_pstrXML + m_aAttributes[iIndex].iValue;
}

LPCTSTR CMarkupNode::GetAttributeValue(LPCTSTR pstrName)
{
    if( !_HasValidPos() ) return nullptr; // [SEC-08]
    if( m_nAttributes == 0 ) _MapAttributes();
    for( int i = 0; i < m_nAttributes; i++ ) {
        if( _tcsicmp(m_pOwner->m_pstrXML + m_aAttributes[i].iName, pstrName) == 0 ) return m_pOwner->m_pstrXML + m_aAttributes[i].iValue;
    }
    return _T("");
}

bool CMarkupNode::GetAttributeValue(int iIndex, LPTSTR pstrValue, SIZE_T cchMax)
{
    if( !_HasValidPos() ) return false; // [SEC-08]
    if( m_nAttributes == 0 ) _MapAttributes();
    if( iIndex < 0 || iIndex >= m_nAttributes ) return false;
    _tcsncpy(pstrValue, m_pOwner->m_pstrXML + m_aAttributes[iIndex].iValue, cchMax);
    return true;
}

bool CMarkupNode::GetAttributeValue(LPCTSTR pstrName, LPTSTR pstrValue, SIZE_T cchMax)
{
    if( !_HasValidPos() ) return false; // [SEC-08]
    if( m_nAttributes == 0 ) _MapAttributes();
    for( int i = 0; i < m_nAttributes; i++ ) {
        if( _tcsicmp(m_pOwner->m_pstrXML + m_aAttributes[i].iName, pstrName) == 0 ) {
            _tcsncpy(pstrValue, m_pOwner->m_pstrXML + m_aAttributes[i].iValue, cchMax);
            return true;
        }
    }
    return false;
}

int CMarkupNode::GetAttributeCount()
{
    if( !_HasValidPos() ) return 0; // [SEC-08]
    if( m_nAttributes == 0 ) _MapAttributes();
    return m_nAttributes;
}

bool CMarkupNode::HasAttributes()
{
    if( !_HasValidPos() ) return false; // [SEC-08]
    if( m_nAttributes == 0 ) _MapAttributes();
    return m_nAttributes > 0;
}

bool CMarkupNode::HasAttribute(LPCTSTR pstrName)
{
    if( !_HasValidPos() ) return false; // [SEC-08]
    if( m_nAttributes == 0 ) _MapAttributes();
    for( int i = 0; i < m_nAttributes; i++ ) {
        if( _tcsicmp(m_pOwner->m_pstrXML + m_aAttributes[i].iName, pstrName) == 0 ) return true;
    }
    return false;
}

void CMarkupNode::_MapAttributes()
{
    m_nAttributes = 0;
    LPCTSTR pstr = m_pOwner->m_pstrXML + m_pOwner->m_pElements[m_iPos].iStart;
    LPCTSTR pstrEnd = m_pOwner->m_pstrXML + m_pOwner->m_pElements[m_iPos].iData;
    pstr += _tcslen(pstr) + 1;
    while( pstr < pstrEnd ) {
        m_pOwner->_SkipWhitespace(pstr);
        if( pstr >= pstrEnd ) return; // [SEC-08] do not start an attribute past the element
        m_aAttributes[m_nAttributes].iName = pstr - m_pOwner->m_pstrXML;
        pstr += _tcslen(pstr) + 1;
        if( pstr >= pstrEnd ) return; // [SEC-08]
        m_pOwner->_SkipWhitespace(pstr);
        if( *pstr++ != _T('\"') ) return; // if( *pstr != _T('\"') ) { pstr = ::CharNext(pstr); return; }

        m_aAttributes[m_nAttributes++].iValue = pstr - m_pOwner->m_pstrXML;
        if( m_nAttributes >= MAX_XML_ATTRIBUTES ) return;
        pstr += _tcslen(pstr) + 1;
    }
}


///////////////////////////////////////////////////////////////////////////////////////
//
//
//

// [EXPORT-3] CMarkup owns two malloc'd buffers (m_pstrXML, m_pElements) and its
// destructor frees both. A defaulted copy constructor shares those pointers, so
// destroying both copies frees the same memory twice. That was latent for years
// because the class-level dllexport only guaranteed the SYMBOL existed -- nothing
// called it. The vtable probe (round 56) called it and the process died with
// 0xC0000374 (heap corruption) *after* every assertion had passed.
//
// The symbols must stay exported for ABI, so the operations are implemented for
// real: deep copy of the element table and the XML text.
CMarkup::CMarkup(const CMarkup& other)
{
    m_pstrXML = nullptr;
    m_pElements = nullptr;
    m_nElements = 0;
    m_nReservedElements = 0;
    m_bPreserveWhitespace = other.m_bPreserveWhitespace;
    ::memcpy(m_szErrorMsg, other.m_szErrorMsg, sizeof(m_szErrorMsg));
    ::memcpy(m_szErrorXML, other.m_szErrorXML, sizeof(m_szErrorXML));

    if (other.m_pstrXML != nullptr) {
        const size_t cch = _tcslen(other.m_pstrXML) + 1;
        m_pstrXML = static_cast<LPTSTR>(malloc(cch * sizeof(TCHAR)));
        if (m_pstrXML) ::memcpy(m_pstrXML, other.m_pstrXML, cch * sizeof(TCHAR));
    }
    if (other.m_pElements != nullptr && other.m_nElements > 0) {
        const size_t cb = other.m_nElements * sizeof(XMLELEMENT);
        m_pElements = static_cast<XMLELEMENT*>(malloc(cb));
        if (m_pElements) {
            ::memcpy(m_pElements, other.m_pElements, cb);
            m_nElements = other.m_nElements;
            m_nReservedElements = other.m_nElements;
        }
    }
}

CMarkup& CMarkup::operator=(const CMarkup& other)
{
    if (this == &other) return *this;
    Release();
    m_bPreserveWhitespace = other.m_bPreserveWhitespace;
    ::memcpy(m_szErrorMsg, other.m_szErrorMsg, sizeof(m_szErrorMsg));
    ::memcpy(m_szErrorXML, other.m_szErrorXML, sizeof(m_szErrorXML));

    if (other.m_pstrXML != nullptr) {
        const size_t cch = _tcslen(other.m_pstrXML) + 1;
        m_pstrXML = static_cast<LPTSTR>(malloc(cch * sizeof(TCHAR)));
        if (m_pstrXML) ::memcpy(m_pstrXML, other.m_pstrXML, cch * sizeof(TCHAR));
    }
    if (other.m_pElements != nullptr && other.m_nElements > 0) {
        const size_t cb = other.m_nElements * sizeof(XMLELEMENT);
        m_pElements = static_cast<XMLELEMENT*>(malloc(cb));
        if (m_pElements) {
            ::memcpy(m_pElements, other.m_pElements, cb);
            m_nElements = other.m_nElements;
            m_nReservedElements = other.m_nElements;
        }
    }
    return *this;
}

// CMarkupNode is a (owner, index) handle with no owned resources -- the default
// copy/assign is correct for it and must exist because the framework reassigns
// nodes (UIResourceManager.cpp:63, UIDlgBuilder.cpp:159/450).
CMarkupNode::CMarkupNode(const CMarkupNode&) = default;
CMarkupNode& CMarkupNode::operator=(const CMarkupNode&) = default;
CMarkupNode& CMarkupNode::operator=(CMarkupNode&&) = default;

CMarkup::CMarkup(LPCTSTR pstrXML)
{
    m_pstrXML = nullptr;
    m_pElements = nullptr;
    m_nElements = 0;
    m_bPreserveWhitespace = true;
    if( pstrXML != nullptr ) Load(pstrXML);
}

CMarkup::~CMarkup()
{
    Release();
}

bool CMarkup::IsValid() const
{
    return m_pElements != nullptr;
}

void CMarkup::SetPreserveWhitespace(bool bPreserve)
{
    m_bPreserveWhitespace = bPreserve;
}

bool CMarkup::Load(LPCTSTR pstrXML)
{
    Release();
    if( pstrXML == nullptr ) return _Failed(_T("XML source is null"));
    const SIZE_T cchLen = _tcslen(pstrXML) + 1;
    // [SEC-11] Allocation failure used to be followed by an unconditional
    // CopyMemory into a null pointer, so a low-memory condition crashed inside
    // the parser instead of reporting a load error.
    m_pstrXML = static_cast<LPTSTR>(malloc(cchLen * sizeof(TCHAR)));
    if( m_pstrXML == nullptr ) return _Failed(_T("Out of memory"));
    ::CopyMemory(m_pstrXML, pstrXML, cchLen * sizeof(TCHAR));
    const bool bRes = _Parse();
    if( !bRes ) Release();
    return bRes;
}

bool CMarkup::LoadFromMem(BYTE* pByte, DWORD dwSize, int encoding)
{
    // [SEC-02] Reject null / empty input before any codec or parser access.
    if( pByte == nullptr || dwSize == 0 ) return _Failed(_T("Empty XML buffer"));

    const BYTE* pData = pByte;
    DWORD cbData = dwSize;
    LPTSTR pszOut = nullptr;

#ifdef _UNICODE
    if (encoding == XMLFILE_ENCODING_UTF8)
    {
        if( cbData >= 3 && pData[0] == 0xEF && pData[1] == 0xBB && pData[2] == 0xBF )
        {
            pData += 3; cbData -= 3;
        }
        if( cbData == 0 ) return _Failed(_T("Empty XML buffer"));
        const int nWide = ::MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<LPCSTR>(pData), static_cast<int>(cbData), nullptr, 0);
        if( nWide <= 0 ) return _Failed(_T("XML encoding conversion failed"));
        pszOut = static_cast<LPTSTR>(malloc((static_cast<SIZE_T>(nWide) + 1) * sizeof(TCHAR)));
        if( pszOut == nullptr ) return _Failed(_T("Out of memory"));
        if( ::MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<LPCSTR>(pData), static_cast<int>(cbData), pszOut, nWide) != nWide )
        {
            free(pszOut);
            return _Failed(_T("XML encoding conversion failed"));
        }
        pszOut[nWide] = 0;
    }
    else if (encoding == XMLFILE_ENCODING_ASNI)
    {
        const int nWide = ::MultiByteToWideChar(CP_ACP, 0, reinterpret_cast<LPCSTR>(pData), static_cast<int>(cbData), nullptr, 0);
        if( nWide <= 0 ) return _Failed(_T("XML encoding conversion failed"));
        pszOut = static_cast<LPTSTR>(malloc((static_cast<SIZE_T>(nWide) + 1) * sizeof(TCHAR)));
        if( pszOut == nullptr ) return _Failed(_T("Out of memory"));
        if( ::MultiByteToWideChar(CP_ACP, 0, reinterpret_cast<LPCSTR>(pData), static_cast<int>(cbData), pszOut, nWide) != nWide )
        {
            free(pszOut);
            return _Failed(_T("XML encoding conversion failed"));
        }
        pszOut[nWide] = 0;
    }
    else
    {
        // [SEC-12] UTF-16 input is copied first and byte-swapped in the copy.
        // The previous code swapped the caller's buffer in place, which faults
        // when the XML lives in a read-only mapped section, and permanently
        // corrupted any caller that reused its buffer.
        bool bSwap = false;
        if( cbData >= 2 && pData[0] == 0xFE && pData[1] == 0xFF ) { bSwap = true; pData += 2; cbData -= 2; }
        else if( cbData >= 2 && pData[0] == 0xFF && pData[1] == 0xFE ) { pData += 2; cbData -= 2; }

        cbData -= cbData % sizeof(TCHAR);
        const SIZE_T cch = static_cast<SIZE_T>(cbData) / sizeof(TCHAR);
        pszOut = static_cast<LPTSTR>(malloc((cch + 1) * sizeof(TCHAR)));
        if( pszOut == nullptr ) return _Failed(_T("Out of memory"));
        if( cch != 0 ) ::CopyMemory(pszOut, pData, cch * sizeof(TCHAR));
        pszOut[cch] = 0;
        if( bSwap )
        {
            for( SIZE_T i = 0; i < cch; ++i )
            {
                const TCHAR ch = pszOut[i];
                pszOut[i] = static_cast<TCHAR>((ch >> 8) | (ch << 8));
            }
        }
    }
#else // !_UNICODE
    if (encoding == XMLFILE_ENCODING_UTF8)
    {
        if( cbData >= 3 && pData[0] == 0xEF && pData[1] == 0xBB && pData[2] == 0xBF )
        {
            pData += 3; cbData -= 3;
        }
        if( cbData == 0 ) return _Failed(_T("Empty XML buffer"));
        const int nWide = ::MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<LPCSTR>(pData), static_cast<int>(cbData), nullptr, 0);
        if( nWide <= 0 ) return _Failed(_T("XML encoding conversion failed"));

        LPWSTR w_str = static_cast<LPWSTR>(malloc((static_cast<SIZE_T>(nWide) + 1) * sizeof(WCHAR)));
        if( w_str == nullptr ) return _Failed(_T("Out of memory"));
        if( ::MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<LPCSTR>(pData), static_cast<int>(cbData), w_str, nWide) != nWide )
        {
            free(w_str);
            return _Failed(_T("XML encoding conversion failed"));
        }
        w_str[nWide] = 0;

        const int nNarrow = ::WideCharToMultiByte(CP_ACP, 0, w_str, nWide, nullptr, 0, nullptr, nullptr);
        if( nNarrow <= 0 ) { free(w_str); return _Failed(_T("XML encoding conversion failed")); }
        pszOut = static_cast<LPTSTR>(malloc((static_cast<SIZE_T>(nNarrow) + 1) * sizeof(TCHAR)));
        if( pszOut == nullptr ) { free(w_str); return _Failed(_T("Out of memory")); }
        if( ::WideCharToMultiByte(CP_ACP, 0, w_str, nWide, pszOut, nNarrow, nullptr, nullptr) != nNarrow )
        {
            free(w_str);
            free(pszOut);
            return _Failed(_T("XML encoding conversion failed"));
        }
        pszOut[nNarrow] = 0;
        free(w_str);
    }
    else if (encoding == XMLFILE_ENCODING_UNICODE)
    {
        bool bSwap = false;
        if( cbData >= 2 && pData[0] == 0xFE && pData[1] == 0xFF ) { bSwap = true; pData += 2; cbData -= 2; }
        else if( cbData >= 2 && pData[0] == 0xFF && pData[1] == 0xFE ) { pData += 2; cbData -= 2; }

        cbData -= cbData % sizeof(WCHAR);
        const SIZE_T cchWide = static_cast<SIZE_T>(cbData) / sizeof(WCHAR);
        LPWSTR w_str = static_cast<LPWSTR>(malloc((cchWide + 1) * sizeof(WCHAR)));
        if( w_str == nullptr ) return _Failed(_T("Out of memory"));
        if( cchWide != 0 ) ::CopyMemory(w_str, pData, cchWide * sizeof(WCHAR));
        w_str[cchWide] = 0;
        if( bSwap )
        {
            for( SIZE_T i = 0; i < cchWide; ++i )
            {
                const WCHAR ch = w_str[i];
                w_str[i] = static_cast<WCHAR>((ch >> 8) | (ch << 8));
            }
        }

        const int nNarrow = ::WideCharToMultiByte(CP_ACP, 0, w_str, static_cast<int>(cchWide), nullptr, 0, nullptr, nullptr);
        if( nNarrow < 0 ) { free(w_str); return _Failed(_T("XML encoding conversion failed")); }
        pszOut = static_cast<LPTSTR>(malloc((static_cast<SIZE_T>(nNarrow) + 1) * sizeof(TCHAR)));
        if( pszOut == nullptr ) { free(w_str); return _Failed(_T("Out of memory")); }
        if( nNarrow > 0 && ::WideCharToMultiByte(CP_ACP, 0, w_str, static_cast<int>(cchWide), pszOut, nNarrow, nullptr, nullptr) != nNarrow )
        {
            free(w_str);
            free(pszOut);
            return _Failed(_T("XML encoding conversion failed"));
        }
        pszOut[nNarrow] = 0;
        free(w_str);
    }
    else
    {
        pszOut = static_cast<LPTSTR>(malloc((static_cast<SIZE_T>(cbData) + 1) * sizeof(TCHAR)));
        if( pszOut == nullptr ) return _Failed(_T("Out of memory"));
        ::CopyMemory(pszOut, pData, cbData * sizeof(TCHAR));
        pszOut[cbData] = 0;
    }
#endif // _UNICODE

    m_pstrXML = pszOut;
    const bool bRes = _Parse();
    if( !bRes ) Release();
    return bRes;
}

bool CMarkup::LoadFromFile(LPCTSTR pstrFilename, int encoding)
{
    Release();
    CDuiString sFile = CPaintManagerUI::GetResourcePath();
    if( CPaintManagerUI::GetResourceZip().IsEmpty() ) {
        sFile += pstrFilename;
        HANDLE hFile = ::CreateFile(sFile, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if( hFile == INVALID_HANDLE_VALUE ) return _Failed(_T("Error opening file"));
        DWORD dwSize = ::GetFileSize(hFile, nullptr);
        if( dwSize == 0 ) return _Failed(_T("File is empty"));
        if ( dwSize > 4096*1024 ) return _Failed(_T("File too large"));

        DWORD dwRead = 0;
        BYTE* pByte = new BYTE[ dwSize ];
        ::ReadFile( hFile, pByte, dwSize, &dwRead, nullptr );
        ::CloseHandle( hFile );
        if( dwRead != dwSize ) {
            delete[] pByte;
			pByte = nullptr;
            Release();
            return _Failed(_T("Could not read file"));
        }

        bool ret = LoadFromMem(pByte, dwSize, encoding);
        delete[] pByte;
		pByte = nullptr;

        return ret;
    }
    else {
		sFile += CPaintManagerUI::GetResourceZip();
		HZIP hz = nullptr;
        if( CPaintManagerUI::IsCachedResourceZip() ) hz = reinterpret_cast<HZIP>(CPaintManagerUI::GetResourceZipHandle());
        else {
			CDuiString sFilePwd = CPaintManagerUI::GetResourceZipPwd();
#ifdef UNICODE
			char* pwd = w2a(const_cast<wchar_t*>(sFilePwd.GetData()));
			hz = OpenZip(sFile.GetData(), pwd);
			if(pwd) delete[] pwd;
#else
            hz = OpenZip(sFile.GetData(), sFilePwd.GetData());
#endif
		}
        if( hz == nullptr ) return _Failed(_T("Error opening zip file"));
        ZIPENTRY ze; 
        int i = 0; 
		CDuiString key = pstrFilename;
		key.Replace(_T("\\"), _T("/"));
        if( FindZipItem(hz, key, true, &i, &ze) != 0 ) return _Failed(_T("Could not find ziped file"));
        DWORD dwSize = ze.unc_size;
        if( dwSize == 0 ) return _Failed(_T("File is empty"));
        if ( dwSize > 4096*1024 ) return _Failed(_T("File too large"));
        BYTE* pByte = new BYTE[ dwSize ];
        int res = UnzipItem(hz, i, pByte, dwSize);
        if( res != 0x00000000 && res != 0x00000600) {
            delete[] pByte;
            if( !CPaintManagerUI::IsCachedResourceZip() ) CloseZip(hz);
            return _Failed(_T("Could not unzip file"));
        }
        if( !CPaintManagerUI::IsCachedResourceZip() ) CloseZip(hz);
        bool ret = LoadFromMem(pByte, dwSize, encoding);
        delete[] pByte;
		pByte = nullptr;
        return ret;
    }
}

void CMarkup::Release()
{
    if( m_pstrXML != nullptr ) free(m_pstrXML);
    if( m_pElements != nullptr ) free(m_pElements);
    m_pstrXML = nullptr;
    m_pElements = nullptr;
    m_nElements = 0;
}

void CMarkup::GetLastErrorMessage(LPTSTR pstrMessage, SIZE_T cchMax) const
{
    _tcsncpy(pstrMessage, m_szErrorMsg, cchMax);
}

void CMarkup::GetLastErrorLocation(LPTSTR pstrSource, SIZE_T cchMax) const
{
    _tcsncpy(pstrSource, m_szErrorXML, cchMax);
}

CMarkupNode CMarkup::GetRoot()
{
    if( m_nElements <= 1 ) return CMarkupNode(); // [SEC-07] index 0 is the error slot
    return CMarkupNode(this, 1);
}

bool CMarkup::_Parse()
{
    if( _ReserveElement() == nullptr ) return _Failed(_T("Out of memory")); // Reserve index 0 for errors
    ::ZeroMemory(m_szErrorMsg, sizeof(m_szErrorMsg));
    ::ZeroMemory(m_szErrorXML, sizeof(m_szErrorXML));
    LPTSTR pstrXML = m_pstrXML;
    bool bRes = _Parse(pstrXML, 0);
    // [SEC-07] Index 0 is the error slot, so an empty document (empty string,
    // whitespace-only, or comment-only) leaves m_nElements == 1.  Reporting
    // success made GetRoot() hand out node index 1, which is past the array:
    // the caller then read uninitialised memory and crashed (ASan
    // access-violation through an uninitialised iChild/iNext).
    if( bRes && m_nElements <= 1 ) return _Failed(_T("XML document has no root element"));
    return bRes;
}

bool CMarkup::_Parse(LPTSTR& pstrText, ULONG iParent)
{
    CXmlDepthGuard depthGuard;
    if( depthGuard.bExceeded ) return _Failed(_T("XML nesting too deep"));
    _SkipWhitespace(pstrText);
    ULONG iPrevious = 0;
    for( ; ; ) 
    {
        if( *pstrText == _T('\0') && iParent <= 1 ) return true;
        _SkipWhitespace(pstrText);
        if( *pstrText != _T('<') ) return _Failed(_T("Expected start tag"), pstrText);
        if( pstrText[1] == _T('/') ) return true;
        *pstrText++ = _T('\0');
        _SkipWhitespace(pstrText);
        // Skip comment or processing directive
        if( *pstrText == _T('!') || *pstrText == _T('?') ) {
            TCHAR ch = *pstrText;
            if( *pstrText == _T('!') ) ch = _T('-');
            while( *pstrText != _T('\0') && !(*pstrText == ch && *(pstrText + 1) == _T('>')) ) pstrText = ::CharNext(pstrText);
            if( *pstrText != _T('\0') ) pstrText += 2;
            _SkipWhitespace(pstrText);
            continue;
        }
        _SkipWhitespace(pstrText);
        // Fill out element structure
        XMLELEMENT* pEl = _ReserveElement();
        if( pEl == nullptr ) return _Failed(_T("Out of memory"));
        ULONG iPos = pEl - m_pElements;
        pEl->iStart = pstrText - m_pstrXML;
        pEl->iParent = iParent;
        pEl->iNext = pEl->iChild = 0;
        if( iPrevious != 0 ) m_pElements[iPrevious].iNext = iPos;
        else if( iParent > 0 ) m_pElements[iParent].iChild = iPos;
        iPrevious = iPos;
        // Parse name
        LPCTSTR pstrName = pstrText;
        _SkipIdentifier(pstrText);
        LPTSTR pstrNameEnd = pstrText;
        if( *pstrText == _T('\0') ) return _Failed(_T("Error parsing element name"), pstrText);
        // Parse attributes
        if( !_ParseAttributes(pstrText) ) return false;
        _SkipWhitespace(pstrText);
        if( pstrText[0] == _T('/') && pstrText[1] == _T('>') )
        {
            pEl->iData = pstrText - m_pstrXML;
            *pstrText = _T('\0');
            pstrText += 2;
        }
        else
        {
            if( *pstrText != _T('>') ) return _Failed(_T("Expected start-tag closing"), pstrText);
            // Parse node data
            pEl->iData = ++pstrText - m_pstrXML;
            LPTSTR pstrDest = pstrText;
            if( !_ParseData(pstrText, pstrDest, _T('<')) ) return false;
            // Determine type of next element
            if( *pstrText == _T('\0') && iParent <= 1 ) return true;
            if( *pstrText != _T('<') ) return _Failed(_T("Expected end-tag start"), pstrText);
            if( pstrText[0] == _T('<') && pstrText[1] != _T('/') ) 
            {
                if( !_Parse(pstrText, iPos) ) return false;
            }
            if( pstrText[0] == _T('<') && pstrText[1] == _T('/') ) 
            {
                *pstrDest = _T('\0');
                *pstrText = _T('\0');
                pstrText += 2;
                _SkipWhitespace(pstrText);
                SIZE_T cchName = pstrNameEnd - pstrName;
                if( _tcsncmp(pstrText, pstrName, cchName) != 0 ) return _Failed(_T("Unmatched closing tag"), pstrText);
                pstrText += cchName;
                _SkipWhitespace(pstrText);
                if( *pstrText++ != _T('>') ) return _Failed(_T("Unmatched closing tag"), pstrText);
            }
        }
        *pstrNameEnd = _T('\0');
        _SkipWhitespace(pstrText);
    }
}

CMarkup::XMLELEMENT* CMarkup::_ReserveElement()
{
    if( m_nElements == 0 ) m_nReservedElements = 0;
    if( m_nElements >= m_nReservedElements ) {
        ULONG nNewReserved = m_nReservedElements + (m_nReservedElements / 2) + 500;
        // [SEC-03] realloc failure must not be dereferenced.  Growing a 5-field
        // struct by 1.5x+500 cannot plausibly allocate 0 bytes, so a null return
        // means genuine OOM; surface it instead of writing through nullptr.
        XMLELEMENT* pNew = static_cast<XMLELEMENT*>(
            realloc(m_pElements, static_cast<size_t>(nNewReserved) * sizeof(XMLELEMENT)));
        if( pNew == nullptr ) return nullptr;
        m_pElements = pNew;
        m_nReservedElements = nNewReserved;
    }
    return &m_pElements[m_nElements++];
}

void CMarkup::_SkipWhitespace(LPCTSTR& pstr) const
{
    while( *pstr > _T('\0') && *pstr <= _T(' ') ) pstr = ::CharNext(pstr);
}

void CMarkup::_SkipWhitespace(LPTSTR& pstr) const
{
    while( *pstr > _T('\0') && *pstr <= _T(' ') ) pstr = ::CharNext(pstr);
}

void CMarkup::_SkipIdentifier(LPCTSTR& pstr) const
{
    // 属性只能用英文，所以这样处理没有问题
    // [WIN11-ADAPT] NameChar must include '-' and '.' (XML spec).
    // Excluding them made <zh-Hans>/<zh-Hant> in DismData/Data.xml parse as
    // "element zh + attribute Hans", then _ParseAttributes expected '=' and
    // failed -> the WHOLE cleanup rule file failed to load (empty pages).
    // Both overloads must stay in sync.
    while( *pstr != _T('\0') && (*pstr == _T('_') || *pstr == _T(':') || *pstr == _T('-') || *pstr == _T('.') || _istalnum(*pstr)) ) pstr = ::CharNext(pstr);
}

void CMarkup::_SkipIdentifier(LPTSTR& pstr) const
{
    // 属性只能用英文，所以这样处理没有问题
    // [WIN11-ADAPT] NameChar must include '-' and '.' (XML spec).
    // Excluding them made <zh-Hans>/<zh-Hant> in DismData/Data.xml parse as
    // "element zh + attribute Hans", then _ParseAttributes expected '=' and
    // failed -> the WHOLE cleanup rule file failed to load (empty pages).
    // Both overloads must stay in sync.
    while( *pstr != _T('\0') && (*pstr == _T('_') || *pstr == _T(':') || *pstr == _T('-') || *pstr == _T('.') || _istalnum(*pstr)) ) pstr = ::CharNext(pstr);
}

bool CMarkup::_ParseAttributes(LPTSTR& pstrText)
{   
	// 无属性
	LPTSTR pstrIdentifier = pstrText;
	if( *pstrIdentifier == _T('/') && *++pstrIdentifier == _T('>') ) return true;
    if( *pstrText == _T('>') ) return true;
    *pstrText++ = _T('\0');
    _SkipWhitespace(pstrText);
    while( *pstrText != _T('\0') && *pstrText != _T('>') && *pstrText != _T('/') ) {
        _SkipIdentifier(pstrText);
        LPTSTR pstrIdentifierEnd = pstrText;
        _SkipWhitespace(pstrText);
        if( *pstrText != _T('=') ) return _Failed(_T("Error while parsing attributes"), pstrText);
        *pstrText++ = _T(' ');
        *pstrIdentifierEnd = _T('\0');
        _SkipWhitespace(pstrText);
        if( *pstrText++ != _T('\"') ) return _Failed(_T("Expected attribute value"), pstrText);
        LPTSTR pstrDest = pstrText;
        if( !_ParseData(pstrText, pstrDest, _T('\"')) ) return false;
        if( *pstrText == _T('\0') ) return _Failed(_T("Error while parsing attribute string"), pstrText);
        *pstrDest = _T('\0');
        if( pstrText != pstrDest ) *pstrText = _T(' ');
        pstrText++;
        _SkipWhitespace(pstrText);
    }
    return true;
}

bool CMarkup::_ParseData(LPTSTR& pstrText, LPTSTR& pstrDest, char cEnd)
{
    while( *pstrText != _T('\0') && *pstrText != cEnd ) {
		if( *pstrText == _T('&') ) {
			while( *pstrText == _T('&') ) {
				_ParseMetaChar(++pstrText, pstrDest);
			}
			if (*pstrText == cEnd)
				break;
		}

        if( *pstrText == _T(' ') ) {
            *pstrDest++ = *pstrText++;
            if( !m_bPreserveWhitespace ) _SkipWhitespace(pstrText);
        }
        else {
            LPTSTR pstrTemp = ::CharNext(pstrText);
            while( pstrText < pstrTemp) {
                *pstrDest++ = *pstrText++;
            }
        }
    }
    // Make sure that MapAttributes() works correctly when it parses
    // over a value that has been transformed.
    LPTSTR pstrFill = pstrDest + 1;
    while( pstrFill < pstrText ) *pstrFill++ = _T(' ');
    return true;
}

void CMarkup::_ParseMetaChar(LPTSTR& pstrText, LPTSTR& pstrDest)
{
    // [SEC-13] Compare with bounded string helpers instead of indexing fixed
    // offsets. A truncated trailing entity ("&amp" at end of document) used to
    // read past the terminator, which is an out-of-bounds read when the input
    // sits at the end of its allocation. _tcsncmp stops at the NUL by itself.
    if( _tcsncmp(pstrText, _T("amp;"), 4) == 0 ) {
        *pstrDest++ = _T('&');
        pstrText += 4;
    }
    else if( _tcsncmp(pstrText, _T("lt;"), 3) == 0 ) {
        *pstrDest++ = _T('<');
        pstrText += 3;
    }
    else if( _tcsncmp(pstrText, _T("gt;"), 3) == 0 ) {
        *pstrDest++ = _T('>');
        pstrText += 3;
    }
    else if( _tcsncmp(pstrText, _T("quot;"), 5) == 0 ) {
        *pstrDest++ = _T('\"');
        pstrText += 5;
    }
    else if( _tcsncmp(pstrText, _T("apos;"), 5) == 0 ) {
        *pstrDest++ = _T('\'');
        pstrText += 5;
    }
    else {
        *pstrDest++ = _T('&');
    }
}

bool CMarkup::_Failed(LPCTSTR pstrError, LPCTSTR pstrLocation)
{
    // Register last error
    TRACE(_T("XML Error: %s"), pstrError);
    if( pstrLocation != nullptr ) TRACE(pstrLocation);
    _tcsncpy(m_szErrorMsg, pstrError, (sizeof(m_szErrorMsg) / sizeof(m_szErrorMsg[0])) - 1);
    _tcsncpy(m_szErrorXML, pstrLocation != nullptr ? pstrLocation : _T(""), lengthof(m_szErrorXML) - 1);
    return false; // Always return 'false'
}

} // namespace DuiLib
