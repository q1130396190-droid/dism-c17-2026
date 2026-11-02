#ifndef __UIMARKUP_H__
#define __UIMARKUP_H__

#pragma once

namespace DuiLib {

	enum
	{
		XMLFILE_ENCODING_UTF8 = 0,
		XMLFILE_ENCODING_UNICODE = 1,
		XMLFILE_ENCODING_ASNI = 2,
	};

	class CMarkup;
	class CMarkupNode;


	// [EXPORT-1] Member-level export, same reason as CMarkupNode above: the class had
	// 11 private helpers (_Parsexxx family) exported because dllexport on a class
	// covers every member. Only the documented public surface is exported now.
	class CMarkup
	{
		friend class CMarkupNode;
	public:
		UILIB_API CMarkup(LPCTSTR pstrXML = nullptr);
		UILIB_API ~CMarkup();

		UILIB_API bool Load(LPCTSTR pstrXML);
		UILIB_API bool LoadFromMem(BYTE* pByte, DWORD dwSize, int encoding = XMLFILE_ENCODING_UTF8);
		UILIB_API bool LoadFromFile(LPCTSTR pstrFilename, int encoding = XMLFILE_ENCODING_UTF8);
		UILIB_API void Release();
		UILIB_API bool IsValid() const;

		UILIB_API void SetPreserveWhitespace(bool bPreserve = true);
		UILIB_API void GetLastErrorMessage(LPTSTR pstrMessage, SIZE_T cchMax) const;
		UILIB_API void GetLastErrorLocation(LPTSTR pstrSource, SIZE_T cchMax) const;

		UILIB_API CMarkupNode GetRoot();

		// Exported to preserve the existing ABI surface: dropping these removed public
		// symbols that downstream binaries may bind to. Declared here, defined in
		// UIMarkup.cpp.
		UILIB_API CMarkup(const CMarkup&);
		UILIB_API CMarkup& operator=(const CMarkup&);

	private:
		struct XMLELEMENT
		{
			ULONG iStart;
			ULONG iChild;
			ULONG iNext;
			ULONG iParent;
			ULONG iData;
		};

		LPTSTR m_pstrXML;
		XMLELEMENT* m_pElements;
		ULONG m_nElements;
		ULONG m_nReservedElements;
		TCHAR m_szErrorMsg[100];
		TCHAR m_szErrorXML[50];
		bool m_bPreserveWhitespace;

	private:
		bool _Parse();
		bool _Parse(LPTSTR& pstrText, ULONG iParent);
		XMLELEMENT* _ReserveElement();
		inline void _SkipWhitespace(LPTSTR& pstr) const;
		inline void _SkipWhitespace(LPCTSTR& pstr) const;
		inline void _SkipIdentifier(LPTSTR& pstr) const;
		inline void _SkipIdentifier(LPCTSTR& pstr) const;
		bool _ParseData(LPTSTR& pstrText, LPTSTR& pstrData, char cEnd);
		void _ParseMetaChar(LPTSTR& pstrText, LPTSTR& pstrDest);
		bool _ParseAttributes(LPTSTR& pstrText);
		bool _Failed(LPCTSTR pstrError, LPCTSTR pstrLocation = nullptr);
	};


	// [EXPORT-1] Member-level export instead of `class UILIB_API`.
	// __declspec(dllexport) on a class exports EVERY member, including the two
	// private constructors and the two private helpers below -- which put four
	// symbols in the export table that no caller could ever use. Measured: the DLL
	// carried 406 non-public exports (round 53). Exporting only the public surface
	// keeps every legitimate caller working while dropping the rest.
	class CMarkupNode
	{
		friend class CMarkup;
	private:
		CMarkupNode();
		CMarkupNode(CMarkup* pOwner, int iPos);

	public:
		UILIB_API bool IsValid() const;

		UILIB_API CMarkupNode GetParent();
		UILIB_API CMarkupNode GetSibling();
		UILIB_API CMarkupNode GetChild();
		UILIB_API CMarkupNode GetChild(LPCTSTR pstrName);

		UILIB_API bool HasSiblings() const;
		UILIB_API bool HasChildren() const;
		UILIB_API LPCTSTR GetName() const;
		UILIB_API LPCTSTR GetValue() const;

		UILIB_API bool HasAttributes();
		UILIB_API bool HasAttribute(LPCTSTR pstrName);
		UILIB_API int GetAttributeCount();
		UILIB_API LPCTSTR GetAttributeName(int iIndex);
		UILIB_API LPCTSTR GetAttributeValue(int iIndex);
		UILIB_API LPCTSTR GetAttributeValue(LPCTSTR pstrName);
		UILIB_API bool GetAttributeValue(int iIndex, LPTSTR pstrValue, SIZE_T cchMax);
		UILIB_API bool GetAttributeValue(LPCTSTR pstrName, LPTSTR pstrValue, SIZE_T cchMax);

		// A node is a (owner, index) handle and the framework DOES reassign it
		// (UIResourceManager.cpp:63, UIDlgBuilder.cpp:159/450 write `node = x->GetChild()`),
		// so assignment must stay. It also has to stay EXPORTED: downstream code that
		// holds a CMarkupNode by value binds to these, and dropping them removed 7
		// public symbols when I tried. Declared here, defined in UIMarkup.cpp.
		UILIB_API CMarkupNode(const CMarkupNode&);
		UILIB_API CMarkupNode& operator=(const CMarkupNode&);
		UILIB_API CMarkupNode& operator=(CMarkupNode&&);

	private:
		void _MapAttributes();
		bool _HasValidPos() const; // [SEC-08] bounds check for m_iPos against owner's element table

		enum { MAX_XML_ATTRIBUTES = 64 };

		struct XMLATTRIBUTE
		{
			ULONG iName;
			ULONG iValue;
		};

		int m_iPos;
		int m_nAttributes;
		XMLATTRIBUTE m_aAttributes[MAX_XML_ATTRIBUTES];
		CMarkup* m_pOwner;
	};

} // namespace DuiLib

#endif // __UIMARKUP_H__
