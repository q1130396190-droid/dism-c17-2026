#ifndef __UISCROLLBAR_H__
#define __UISCROLLBAR_H__

#pragma once

namespace DuiLib
{
	class UILIB_API CScrollBarUI : public CControlUI
	{
		DECLARE_DUICONTROL(CScrollBarUI)
	public:
		CScrollBarUI();

		LPCTSTR GetClass() const;
		LPVOID GetInterface(LPCTSTR pstrName);

		CContainerUI* GetOwner() const;
		void SetOwner(CContainerUI* pOwner);

		void SetVisible(bool bVisible = true);
		void SetEnabled(bool bEnable = true);
		void SetFocus();

		bool IsHorizontal();
		void SetHorizontal(bool bHorizontal = true);
		int GetScrollRange() const;
		void SetScrollRange(int nRange);
		int GetScrollPos() const;
		void SetScrollPos(int nPos);
		int GetLineSize() const;
		void SetLineSize(int nSize);

		bool GetShowButton1();
		void SetShowButton1(bool bShow);
		LPCTSTR GetButton1NormalImage();
		void SetButton1NormalImage(LPCTSTR pStrImage);
		LPCTSTR GetButton1HotImage();
		void SetButton1HotImage(LPCTSTR pStrImage);
		LPCTSTR GetButton1PushedImage();
		void SetButton1PushedImage(LPCTSTR pStrImage);
		LPCTSTR GetButton1DisabledImage();
		void SetButton1DisabledImage(LPCTSTR pStrImage);

		bool GetShowButton2();
		void SetShowButton2(bool bShow);
		LPCTSTR GetButton2NormalImage();
		void SetButton2NormalImage(LPCTSTR pStrImage);
		LPCTSTR GetButton2HotImage();
		void SetButton2HotImage(LPCTSTR pStrImage);
		LPCTSTR GetButton2PushedImage();
		void SetButton2PushedImage(LPCTSTR pStrImage);
		LPCTSTR GetButton2DisabledImage();
		void SetButton2DisabledImage(LPCTSTR pStrImage);

		LPCTSTR GetThumbNormalImage();
		void SetThumbNormalImage(LPCTSTR pStrImage);
		LPCTSTR GetThumbHotImage();
		void SetThumbHotImage(LPCTSTR pStrImage);
		LPCTSTR GetThumbPushedImage();
		void SetThumbPushedImage(LPCTSTR pStrImage);
		LPCTSTR GetThumbDisabledImage();
		void SetThumbDisabledImage(LPCTSTR pStrImage);

		LPCTSTR GetRailNormalImage();
		void SetRailNormalImage(LPCTSTR pStrImage);
		LPCTSTR GetRailHotImage();
		void SetRailHotImage(LPCTSTR pStrImage);
		LPCTSTR GetRailPushedImage();
		void SetRailPushedImage(LPCTSTR pStrImage);
		LPCTSTR GetRailDisabledImage();
		void SetRailDisabledImage(LPCTSTR pStrImage);

		LPCTSTR GetBkNormalImage();
		void SetBkNormalImage(LPCTSTR pStrImage);
		LPCTSTR GetBkHotImage();
		void SetBkHotImage(LPCTSTR pStrImage);
		LPCTSTR GetBkPushedImage();
		void SetBkPushedImage(LPCTSTR pStrImage);
		LPCTSTR GetBkDisabledImage();
		void SetBkDisabledImage(LPCTSTR pStrImage);

		bool GetShow();
		void SetShow(bool bShow);

		void SetPos(RECT rc, bool bNeedInvalidate = true);
		void DoEvent(TEventUI& event);
		void SetAttribute(LPCTSTR pstrName, LPCTSTR pstrValue);

		// [WIN11-ADAPT] 滑块/滑轨纯色填充。
		// 皮肤里的 thumbbkcolor / railbkcolor / thumbhotbkcolor /
		// thumbpushedbkcolor / railhotbkcolor 这些属性此前**从未被解析**
		// （全仓 0 命中），于是未配图片的滚动条只能落到 PaintThumb 末尾
		// 写死的 #FF85E4FF 亮青描边 —— 深色主题下是一个刺眼的亮框。
		// 补齐后由皮肤决定颜色；未设置时保持原行为（不改变老皮肤外观）。
		void SetThumbBkColor(DWORD dwColor);
		DWORD GetThumbBkColor() const;
		void SetThumbHotBkColor(DWORD dwColor);
		DWORD GetThumbHotBkColor() const;
		void SetThumbPushedBkColor(DWORD dwColor);
		DWORD GetThumbPushedBkColor() const;
		void SetRailBkColor(DWORD dwColor);
		DWORD GetRailBkColor() const;

		bool DoPaint(HDC hDC, const RECT& rcPaint, CControlUI* pStopControl);

		void PaintBk(HDC hDC);
		void PaintButton1(HDC hDC);
		void PaintButton2(HDC hDC);
		void PaintThumb(HDC hDC);
		void PaintRail(HDC hDC);

	protected:
		enum
		{ 
			DEFAULT_SCROLLBAR_SIZE = 16,
			DEFAULT_TIMERID = 10,
		};
		bool m_bShow;
		bool m_bHorizontal;
		__int64 m_nRange;
		__int64 m_nScrollPos;
		int m_nLineSize;
		CContainerUI* m_pOwner;
		POINT m_ptLastMouse;
		int m_nLastScrollPos;
		int m_nLastScrollOffset;
		int m_nScrollRepeatDelay;

		CDuiString m_sBkNormalImage;
		CDuiString m_sBkHotImage;
		CDuiString m_sBkPushedImage;
		CDuiString m_sBkDisabledImage;

		bool m_bShowButton1;
		RECT m_rcButton1;
		UINT m_uButton1State;
		CDuiString m_sButton1NormalImage;
		CDuiString m_sButton1HotImage;
		CDuiString m_sButton1PushedImage;
		CDuiString m_sButton1DisabledImage;

		bool m_bShowButton2;
		RECT m_rcButton2;
		UINT m_uButton2State;
		CDuiString m_sButton2NormalImage;
		CDuiString m_sButton2HotImage;
		CDuiString m_sButton2PushedImage;
		CDuiString m_sButton2DisabledImage;

		RECT m_rcThumb;
		UINT m_uThumbState;
		CDuiString m_sThumbNormalImage;
		CDuiString m_sThumbHotImage;
		CDuiString m_sThumbPushedImage;
		CDuiString m_sThumbDisabledImage;

		CDuiString m_sRailNormalImage;
		CDuiString m_sRailHotImage;
		CDuiString m_sRailPushedImage;
		CDuiString m_sRailDisabledImage;

		// [WIN11-ADAPT] 0 表示"未设置"，此时保持原有硬编码行为
		DWORD m_dwThumbBkColor;
		DWORD m_dwThumbHotBkColor;
		DWORD m_dwThumbPushedBkColor;
		DWORD m_dwRailBkColor;

		CDuiString m_sImageModify;
	};
}

#endif // __UISCROLLBAR_H__