#ifndef __UIIPADDRESSEX_H__
#define __UIIPADDRESSEX_H__

#pragma once

//给该控件添加一个属性dtstyle

namespace DuiLib
{
	
    /// IP输入控件
    // [EXPORT-5] Member-level export: CharToInt/GetNumInput/UpdateText/IncNum/DecNum
// are private helpers with no external caller. Only the documented public API keeps
// UILIB_API. The vtable still works -- round 67 proved a vtable slot does not need
// its symbol exported.
class CIPAddressExUI : public CEditUI
    {
	public:
		UILIB_API static CControlUI* CreateControl();
    public:
        UILIB_API CIPAddressExUI();
        UILIB_API ~CIPAddressExUI();
        UILIB_API LPCTSTR GetClass() const;
        UILIB_API LPVOID GetInterface(LPCTSTR pstrName);
        UILIB_API UINT GetControlFlags() const;
        UILIB_API void DoEvent(TEventUI& event);
        UILIB_API void PaintText(HDC hDC);

        UILIB_API void SetIP(LPCTSTR lpIP);
        UILIB_API CDuiString GetIP();

    private:
        void CharToInt();
        void GetNumInput(TCHAR chKey);
        void UpdateText();
        void IncNum();
        void DecNum();

    protected:
        int m_nFirst;
        int m_nSecond;
        int m_nThird;
        int m_nFourth;
        int m_nActiveSection;

        TCHAR m_chNum;
        CDuiString m_strNum;
    };
}
#endif // __UIIPADDRESSEX_H__