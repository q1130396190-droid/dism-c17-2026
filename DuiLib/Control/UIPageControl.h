#ifndef __UIPageControl_H
#define __UIPageControl_H

#pragma once
#include <algorithm> // std::min/std::max
namespace DuiLib
{
    // [EXPORT-4] Member-level export. `class UILIB_API` exported every member,
    // including the 8 private/protected helpers below (ResetAllPages, UpdatePosition,
    // SelectPage, ...) which no caller outside this translation unit can reach.
    // Only the documented public surface keeps UILIB_API.
    class CPageControlUI : public CHorizontalLayoutUI
    {
        // Not using DECLARE_DUICONTROL here: the macro emits an unmarked
        // `static CControlUI* CreateControl();`, and this class is no longer
        // `class UILIB_API`, so the factory entry point would lose its export and
        // ControlFactory::CreateControl would fail to construct the control.
        // Declared explicitly instead; the macro cannot carry UILIB_API on the member
        // because classes still declared `class UILIB_API X` would then mark it twice
        // (MSVC C2487).
    public:
        UILIB_API static CControlUI* CreateControl();
    public:
        UILIB_API CPageControlUI();
        UILIB_API ~CPageControlUI();
        //maxpage="20,6"
        UILIB_API void SetMaxPages(int nPages, int nShowNum = 6);
        UILIB_API void SetMaxChar(int nNum = 2);
        //设置字体[font="1"]
        UILIB_API void SetFont(int nFont);

        //设置页码选中的颜色[pageselectedtextcolor]
        UILIB_API void SetPageSelectedTextColor(DWORD cr = 0xFF4EB857);
        //设置选中的页按钮背景色[pageselectedbkcolor]
        UILIB_API void SetPageSelectedBkColor(DWORD cr = 0x00000000);

        //设置页码文字颜色[pagetextcolor]
        UILIB_API void SetPageNormalTextColor(DWORD cr = 0xff000000);
        //设置按钮背景颜色[pagebkcolor]
        UILIB_API void SetPageBkColor(DWORD cr = 0x00000000);

        //设置hot按钮背景色[pagehotbkcolor]
        UILIB_API void SetPageHotBkColor(DWORD cr = 0x00000000);
        //设置hot字体颜色[pagehottextcolor]
        UILIB_API void SetPageHotTextColor(DWORD cr = 0x00000000);


        //设置页码按钮的大小[pagesize="30,30"]
        UILIB_API void SetPageNoSize(int w = 30, int h = 30);
        //设置goto按钮的大小[gotoeditsize="1,1"]
        UILIB_API void SetGotoEditSize(int w = 30, int h = 26);
        //设置GotoEdit边框颜色[gotoeditbordercolor="0x00000000"]
        UILIB_API void SetGotoEditBorderColor(DWORD cr = 0x00000000);
        //设置GotoEdit边框的大小[gotoeditbordersize="1"]
        UILIB_API void SetGotoEditBorderSize(int size = 1);

    public:
        UILIB_API void GotoPage(int nPageNo, bool bAutoGotoOneWhenError = true);
    public:
        UILIB_API virtual void SetAttribute(LPCTSTR pstrName, LPCTSTR pstrValue);

    protected:
        void ResetAllPages();
        void UpdatePosition();
        void SelectPage(int nPageIdx);
        int GetCurSel();

        void UpdateMoreBtnVisiable();
        bool IsExistNextMore();

        bool OnOptionSelChanged(void* p);
        bool OnBtnClick(void* p);

    private:
        int GetPageNoByControl(CControlUI* pcon);
        void UpdatePageNo(int iDelta, bool bReset = false);
        void UpdateItemIndex();
        int GetIndexByPageNo(int nPageNo);
        int GetShowPageNum() { return (m_nShowPage < m_nMaxPage) ? m_nShowPage : m_nMaxPage; };
    private:
        int m_nFont;

        int m_nShowPage;
        int m_nMaxPage;
        SIZE m_szPage;
        SIZE m_szGotoEdit;
        int m_nSelPageNo;

        DWORD m_dwSelTextColor;
        DWORD m_dwSelectedBkColor;
        DWORD m_dwNormalTextColor;
        DWORD m_dwNormalBkColor;
        DWORD m_dwHotBkColor;
        DWORD m_dwHotTextColor;

        CButtonUI* m_BtnPrevious;
        CButtonUI* m_BtnNext;
        CButtonUI* m_BtnNextMore;
        CStdPtrArray m_OptPageNoArr;    //COptionUI*
        CButtonUI* m_BtnGoto;
        CEditUI* m_EdtPageNo;

        COptionUI* m_ConCurSel;    //当前是第几页
    };
}
#endif //__UIPageControl_H
