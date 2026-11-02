#include "StdAfx.h"
#include "ControlFactory.h"

namespace DuiLib 
{
	CControlFactory::CControlFactory()
	{
		INNER_REGISTER_DUICONTROL(CControlUI);
		INNER_REGISTER_DUICONTROL(CContainerUI);
		INNER_REGISTER_DUICONTROL(CButtonUI);
		INNER_REGISTER_DUICONTROL(CComboUI);
		INNER_REGISTER_DUICONTROL(CComboBoxUI);
		INNER_REGISTER_DUICONTROL(CDateTimeUI);
		INNER_REGISTER_DUICONTROL(CEditUI);
		INNER_REGISTER_DUICONTROL(CActiveXUI);
		INNER_REGISTER_DUICONTROL(CFlashUI);
		INNER_REGISTER_DUICONTROL(CGifAnimUI);
#ifdef USE_XIMAGE_EFFECT
		INNER_REGISTER_DUICONTROL(CGifAnimExUI);
#endif
		INNER_REGISTER_DUICONTROL(CGroupBoxUI);
		INNER_REGISTER_DUICONTROL(CIPAddressUI);
		INNER_REGISTER_DUICONTROL(CIPAddressExUI);
		INNER_REGISTER_DUICONTROL(CLabelUI);
		INNER_REGISTER_DUICONTROL(CListUI);
		INNER_REGISTER_DUICONTROL(CListHeaderUI);
		INNER_REGISTER_DUICONTROL(CListHeaderItemUI);
		INNER_REGISTER_DUICONTROL(CListLabelElementUI);
		INNER_REGISTER_DUICONTROL(CListTextElementUI);
		INNER_REGISTER_DUICONTROL(CListContainerElementUI);
		INNER_REGISTER_DUICONTROL(CMenuUI);
		INNER_REGISTER_DUICONTROL(CMenuElementUI);
		INNER_REGISTER_DUICONTROL(COptionUI);
		INNER_REGISTER_DUICONTROL(CCheckBoxUI);
		INNER_REGISTER_DUICONTROL(CProgressUI);
		INNER_REGISTER_DUICONTROL(CRichEditUI);
		INNER_REGISTER_DUICONTROL(CScrollBarUI);
		INNER_REGISTER_DUICONTROL(CSliderUI);
		INNER_REGISTER_DUICONTROL(CTextUI);
		INNER_REGISTER_DUICONTROL(CTreeNodeUI);
		INNER_REGISTER_DUICONTROL(CTreeViewUI);
		INNER_REGISTER_DUICONTROL(CWebBrowserUI);
		INNER_REGISTER_DUICONTROL(CAnimationTabLayoutUI);
		INNER_REGISTER_DUICONTROL(CChildLayoutUI);
		INNER_REGISTER_DUICONTROL(CHorizontalLayoutUI);
		INNER_REGISTER_DUICONTROL(CTabLayoutUI);
		INNER_REGISTER_DUICONTROL(CTileLayoutUI);
		INNER_REGISTER_DUICONTROL(CVerticalLayoutUI);
		INNER_REGISTER_DUICONTROL(CRollTextUI);
		INNER_REGISTER_DUICONTROL(CColorPaletteUI);
		INNER_REGISTER_DUICONTROL(CListExUI);
		INNER_REGISTER_DUICONTROL(CListContainerHeaderItemUI);
		INNER_REGISTER_DUICONTROL(CListTextExtElementUI);
		INNER_REGISTER_DUICONTROL(CHotKeyUI);
		INNER_REGISTER_DUICONTROL(CFadeButtonUI);
		INNER_REGISTER_DUICONTROL(CRingUI);
		INNER_REGISTER_DUICONTROL(CLoadingUI);
		INNER_REGISTER_DUICONTROL(CPageControlUI);

        INNER_REGISTER_DUICONTROL_EX(DUI_CTR_BOX, CContainerUI);
        INNER_REGISTER_DUICONTROL_EX(DUI_CTR_HBOX, CHorizontalLayoutUI);
        INNER_REGISTER_DUICONTROL_EX(DUI_CTR_VBOX, CVerticalLayoutUI);
        INNER_REGISTER_DUICONTROL_EX(DUI_CTR_TAB_BOX, CTabLayoutUI);
        INNER_REGISTER_DUICONTROL_EX(DUI_CTR_TILE_BOX, CTileLayoutUI);
        INNER_REGISTER_DUICONTROL_EX(DUI_CTR_CHILD_BOX, CChildLayoutUI);
	}

	CControlFactory::~CControlFactory()
	{
	}

	CControlUI* CControlFactory::CreateControl(CDuiString strClassName)
	{
		strClassName.MakeLower();
		MAP_DUI_CTRATECLASS::iterator iter = m_mapControl.find(strClassName);
		if ( iter == m_mapControl.end()) {
			return nullptr;
		}
		else {
			return static_cast<CControlUI*>(iter->second());
		}
	}

	void CControlFactory::RegistControl(CDuiString strClassName, CreateClass pFunc)
	{
		strClassName.MakeLower();
		m_mapControl.insert(MAP_DUI_CTRATECLASS::value_type(strClassName, pFunc));
	}

	CControlFactory* CControlFactory::GetInstance()
	{
		// T5.4: Meyers 单例 —— 函数局部 static 对象，C++11 起线程安全初始化，
		// 由编译器保证唯一构造与析构，替代原裸 new + 手工 delete this。
		static CControlFactory instance;
		return &instance;
	}

	void CControlFactory::Release()
	{
		// T5.4: 语义化清空 —— 释放所有注册的控件类型，但不销毁单例对象。
		// 原实现 delete this（self-delete）存在 use-after-free 风险：
		//   若 Release() 后再调用 GetInstance()/CreateControl()，将解引用悬垂指针。
		// 新实现保持导出符号 Release() 不变（ABI 冻结面零变更），仅将副作用从
		// 「销毁对象」改为「清空注册表」，此后 GetInstance() 返回合法空表单例，
		// CreateControl() 安全返回 nullptr，不再触发 UB。
		m_mapControl.clear();
	}
}