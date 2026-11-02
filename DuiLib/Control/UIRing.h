#ifndef __UIROTATE_H__
#define __UIROTATE_H__

#pragma once

namespace DuiLib
{
	// [R87] Converted to member-level export. The class is registered with the
	// factory (ControlFactory.cpp: INNER_REGISTER_DUICONTROL(CRingUI)) but carried
	// no UILIB_API at all, so it exported ZERO symbols -- verified with dumpbin:
	// "CRingUI exports: 0" while the DLL has 2640 others. Anything outside the DLL
	// could not create a RingUI even though CreateControl("RingUI") resolves.
	// This is the same failure shape as round 67 CPageControlUI, but total rather
	// than partial: it was invisible because every caller was inside the DLL.
	class CRingUI : public CLabelUI
	{
		enum
		{ 
			RING_TIMERID = 100,
		};
		// Not using DECLARE_DUICONTROL: the macro emits an unmarked
		// `static CControlUI* CreateControl();`, and this class is no longer
		// `class UILIB_API`, so the factory entry point would lose its export.
		// Declared explicitly (same pattern as CPageControlUI, round 67/78).
	public:
		UILIB_API static CControlUI* CreateControl();
		UILIB_API CRingUI();
		UILIB_API ~CRingUI();

		UILIB_API LPCTSTR GetClass() const;
		UILIB_API LPVOID GetInterface(LPCTSTR pstrName);
		UILIB_API void SetAttribute(LPCTSTR pstrName, LPCTSTR pstrValue);
		UILIB_API void SetBkImage(LPCTSTR pStrImage);	
		UILIB_API virtual void DoEvent(TEventUI& event);
		UILIB_API virtual void PaintBkImage(HDC hDC);	

	private:
		void InitImage();
		void DeleteImage();

	public:
		float m_fCurAngle;
		Gdiplus::Image* m_pBkimage;
	};
}

#endif // __UIROTATE_H__