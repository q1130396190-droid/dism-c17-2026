#ifndef __UIGROUPBOX_H__
#define __UIGROUPBOX_H__

#pragma once

namespace DuiLib
{

	// [EXPORT-5] Member-level export: PaintText/PaintBorder are base-class virtual
	// overrides reached through the vtable, and CalcrectSize is a private helper.
	// Neither needs a symbol in the export table. SetAttribute stays exported: it is
	// part of the public API surface that derived code may call by name.
	class CGroupBoxUI : public CVerticalLayoutUI
	{
	public:
		UILIB_API static CControlUI* CreateControl();
	public:
		UILIB_API CGroupBoxUI();
		UILIB_API ~CGroupBoxUI();
		UILIB_API LPCTSTR GetClass() const;
		UILIB_API LPVOID GetInterface(LPCTSTR pstrName);
		UILIB_API void SetTextColor(DWORD dwTextColor);
		UILIB_API DWORD GetTextColor() const;
		UILIB_API void SetDisabledTextColor(DWORD dwTextColor);
		UILIB_API DWORD GetDisabledTextColor() const;
		UILIB_API void SetFont(int index);
		UILIB_API int GetFont() const;

	protected:	
		//Paint
		virtual void PaintText(HDC hDC);
		virtual void PaintBorder(HDC hDC);
		UILIB_API virtual void SetAttribute(LPCTSTR pstrName, LPCTSTR pstrValue);

	private:
		SIZE CalcrectSize(SIZE szAvailable);

	protected:
		DWORD m_dwTextColor;
		DWORD m_dwDisabledTextColor;
		int m_iFont;
		UINT m_uTextStyle;
	};
}
#endif // __UIGROUPBOX_H__