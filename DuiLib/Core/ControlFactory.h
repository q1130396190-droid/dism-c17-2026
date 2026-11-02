#pragma once
#include <map>
namespace DuiLib 
{
	using CreateClass = CControlUI*  (*)();
	using MAP_DUI_CTRATECLASS = std::map<CDuiString, CreateClass>;

	class UILIB_API CControlFactory
	{
	public:
		[[nodiscard]] CControlUI* CreateControl(CDuiString strClassName);
		void RegistControl(CDuiString strClassName, CreateClass pFunc);

		static CControlFactory* GetInstance();
		void Release();

	private:	
		CControlFactory();
		virtual ~CControlFactory();

	private:
		MAP_DUI_CTRATECLASS m_mapControl;
	};

// NOTE: CreateControl must stay exported for member-level-export controls, but it
// cannot carry UILIB_API here: for classes still declared `class UILIB_API X` the
// member would be marked twice and MSVC rejects it with C2487. Controls converted
// to member-level export therefore declare their own exported CreateControl (see
// CPageControlUI in UIPageControl.h).
#define DECLARE_DUICONTROL(class_name)\
public:\
	static CControlUI* CreateControl();

#define IMPLEMENT_DUICONTROL(class_name)\
	CControlUI* class_name::CreateControl()\
	{\
		return new class_name;\
	}

#define REGIST_DUICONTROL(class_name)\
	CControlFactory::GetInstance()->RegistControl(_T(#class_name), static_cast<CreateClass>(class_name::CreateControl));

#define INNER_REGISTER_DUICONTROL(class_name)\
	RegistControl(_T(#class_name), static_cast<CreateClass>(class_name::CreateControl));


#define INNER_REGISTER_DUICONTROL_EX(str_class_name,class_type)\
	{\
		CDuiString sRegClassName;\
		sRegClassName.Format(_T("C%sUI"), str_class_name); \
		RegistControl(sRegClassName, static_cast<CreateClass>(class_type::CreateControl));\
    }
}