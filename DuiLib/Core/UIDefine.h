#pragma once

	// [S4/T4.2] DuiMessageMapFunctions 需要 memcpy/memset（用字节缓冲承载成员指针）
#include <string.h>
// T5.5: std::array<std::byte,N> 承载字节缓冲（编译期常量 N，ABI 布局不变）
#include <array>
#include <cstddef>

namespace DuiLib
{
inline constexpr int MAX_FONT_ID = 30000;
inline constexpr UINT_PTR CARET_TIMERID = 0x1999;
	// 列表类型
	enum ListType
	{
		LT_LIST = 0,
		LT_COMBO,
		LT_TREE,
		LT_MENU,
	};

	// 鼠标光标定义
#define DUI_ARROW           32512
#define DUI_IBEAM           32513
#define DUI_WAIT            32514
#define DUI_CROSS           32515
#define DUI_UPARROW         32516
#define DUI_SIZE            32640
#define DUI_ICON            32641
#define DUI_SIZENWSE        32642
#define DUI_SIZENESW        32643
#define DUI_SIZEWE          32644
#define DUI_SIZENS          32645
#define DUI_SIZEALL         32646
#define DUI_NO              32648
#define DUI_HAND            32649

	// 消息类型
	enum class DuiSig : unsigned int
	{
		DuiSig_end = 0, // [marks end of message map]
		DuiSig_lwl,     // LRESULT (WPARAM, LPARAM)
		DuiSig_vn,      // void (TNotifyUI)
	};

	// 核心控件
	class CControlUI;

	// Structure for notifications to the outside world
	struct TNotifyUI
	{
		CDuiString sType;
		CDuiString sVirtualWnd;
		CControlUI* pSender;
		DWORD dwTimestamp;
		POINT ptMouse;
		WPARAM wParam;
		LPARAM lParam;
	}; 

	class CNotifyPump;
	using DUI_PMSG = void (CNotifyPump::*)(TNotifyUI&);      //指针类型

	// [S4/T4.2] 消息映射成员指针的类型安全承载。
	// 原先用 union 在三种成员函数指针之间做类型双关（按 nSig 写入一种签名、读出另一种），
	// 属于依赖实现细节的 UB。现改为「定长字节缓冲 + 按签名显式 memcpy 还原」。
	// 语义与原 union 等价：每次读写的类型都显式匹配。
	//
	// 缓冲取 4*sizeof(void*) 而非精确值：实测 MSVC 在模板实例化内对成员函数
	// 指针的 sizeof 报值不可靠（见 docs/s4-message-design.json）。本实现故意不用模板，
	// 改用具体类型重载，避开模板报值问题。用户决策 D1：放弃 Windows XP 支持。
	static const size_t DUIMSG_FN_BUF_SIZE = 4 * sizeof(void*);

	struct DuiMessageMapFunctions
	{
		// T5.5: std::array<std::byte,N> 替代裸 unsigned char[N]；
		// N 仍为编译期常量 DUIMSG_FN_BUF_SIZE，sizeof 不变，ABI 布局不变。
		std::array<std::byte, DUIMSG_FN_BUF_SIZE> m_fnBuf;

		DuiMessageMapFunctions()
		{
			m_fnBuf.fill(std::byte{ 0 });
		}

		// 存入通用成员指针（对应原 mmf.pfn = lpEntry->pfn）
		void store(DUI_PMSG fn)
		{
			m_fnBuf.fill(std::byte{ 0 });
			memcpy(m_fnBuf.data(), &fn, sizeof(fn));
		}

		// 按 DuiSig_lwl 签名还原
		void load_lwl(LRESULT(CNotifyPump::*&out)(WPARAM, LPARAM)) const
		{
			memcpy(&out, m_fnBuf.data(), sizeof(out));
		}

		// 按 DuiSig_vn 签名还原
		void load_vn(void (CNotifyPump::*&out)(TNotifyUI&)) const
		{
			memcpy(&out, m_fnBuf.data(), sizeof(out));
		}
	};

	//定义所有消息类型
	//////////////////////////////////////////////////////////////////////////

#define DUI_MSGTYPE_MENU                   (_T("menu"))
#define DUI_MSGTYPE_LINK                   (_T("link"))

#define DUI_MSGTYPE_TIMER                  (_T("timer"))
#define DUI_MSGTYPE_CLICK                  (_T("click"))
#define DUI_MSGTYPE_DBCLICK                (_T("dbclick"))

#define DUI_MSGTYPE_RETURN                 (_T("return"))
#define DUI_MSGTYPE_SCROLL                 (_T("scroll"))

#define DUI_MSGTYPE_PREDROPDOWN            (_T("predropdown"))
#define DUI_MSGTYPE_DROPDOWN               (_T("dropdown"))
#define DUI_MSGTYPE_SETFOCUS               (_T("setfocus"))

#define DUI_MSGTYPE_KILLFOCUS              (_T("killfocus"))
#define DUI_MSGTYPE_ITEMCLICK 		   	   (_T("itemclick"))
#define DUI_MSGTYPE_ITEMRCLICK 			   (_T("itemrclick"))
#define DUI_MSGTYPE_TABSELECT              (_T("tabselect"))

#define DUI_MSGTYPE_ITEMSELECT 		   	   (_T("itemselect"))
#define DUI_MSGTYPE_ITEMEXPAND             (_T("itemexpand"))
#define DUI_MSGTYPE_WINDOWINIT             (_T("windowinit"))
#define DUI_MSGTYPE_WINDOWSIZE             (_T("windowsize"))
#define DUI_MSGTYPE_BUTTONDOWN 		   	   (_T("buttondown"))
#define DUI_MSGTYPE_MOUSEENTER			   (_T("mouseenter"))
#define DUI_MSGTYPE_MOUSELEAVE			   (_T("mouseleave"))

#define DUI_MSGTYPE_TEXTCHANGED            (_T("textchanged"))
#define DUI_MSGTYPE_HEADERCLICK            (_T("headerclick"))
#define DUI_MSGTYPE_ITEMDBCLICK            (_T("itemdbclick"))
#define DUI_MSGTYPE_SHOWACTIVEX            (_T("showactivex"))

#define DUI_MSGTYPE_ITEMCOLLAPSE           (_T("itemcollapse"))
#define DUI_MSGTYPE_ITEMACTIVATE           (_T("itemactivate"))
#define DUI_MSGTYPE_VALUECHANGED           (_T("valuechanged"))
#define DUI_MSGTYPE_VALUECHANGED_MOVE      (_T("movevaluechanged"))

#define DUI_MSGTYPE_SELECTCHANGED 		   (_T("selectchanged"))
#define DUI_MSGTYPE_UNSELECTED	 		   (_T("unselected"))

#define DUI_MSGTYPE_TREEITEMDBCLICK 		(_T("treeitemdbclick"))
#define DUI_MSGTYPE_CHECKCLICK				(_T("checkclick"))
#define DUI_MSGTYPE_TEXTROLLEND 			(_T("textrollend"))
#define DUI_MSGTYPE_COLORCHANGED		    (_T("colorchanged"))

#define DUI_MSGTYPE_LISTITEMSELECT 		   	(_T("listitemselect"))
#define DUI_MSGTYPE_LISTITEMCHECKED 		(_T("listitemchecked"))
#define DUI_MSGTYPE_COMBOITEMSELECT 		(_T("comboitemselect"))
#define DUI_MSGTYPE_LISTHEADERCLICK			(_T("listheaderclick"))
#define DUI_MSGTYPE_LISTHEADITEMCHECKED		(_T("listheaditemchecked"))
#define DUI_MSGTYPE_LISTPAGECHANGED			(_T("listpagechanged"))

#define DUI_MSGTYPE_PAGECHANED				(_T("page_selected_changed"))


	//////////////////////////////////////////////////////////////////////////

	struct DUI_MSGMAP_ENTRY;
	struct DUI_MSGMAP
	{
#ifndef UILIB_STATIC
		const DUI_MSGMAP* (PASCAL* pfnGetBaseMap)();
#else
		const DUI_MSGMAP* pBaseMap;
#endif
		const DUI_MSGMAP_ENTRY* lpEntries;
	};

	//结构定义
	struct DUI_MSGMAP_ENTRY //定义一个结构体，来存放消息信息
	{
		CDuiString sMsgType;          // DUI消息类型
		CDuiString sCtrlName;         // 控件名称
		UINT       nSig;              // 标记函数指针类型
		DUI_PMSG   pfn;               // 指向函数的指针
	};

	//定义
#ifndef UILIB_STATIC
#define DUI_DECLARE_MESSAGE_MAP()                                         \
private:                                                                  \
	static const DUI_MSGMAP_ENTRY _messageEntries[];                      \
protected:                                                                \
	static const DUI_MSGMAP messageMap;                                   \
	static const DUI_MSGMAP* PASCAL _GetBaseMessageMap();                 \
	virtual const DUI_MSGMAP* GetMessageMap() const;                      \

#else
#define DUI_DECLARE_MESSAGE_MAP()                                         \
private:                                                                  \
	static const DUI_MSGMAP_ENTRY _messageEntries[];                      \
protected:                                                                \
	static  const DUI_MSGMAP messageMap;				                  \
	virtual const DUI_MSGMAP* GetMessageMap() const;                      \

#endif


	//基类声明开始
#ifndef UILIB_STATIC
#define DUI_BASE_BEGIN_MESSAGE_MAP(theClass)                              \
	const DUI_MSGMAP* PASCAL theClass::_GetBaseMessageMap()               \
	{ return nullptr; }                                                  \
	const DUI_MSGMAP* theClass::GetMessageMap() const                     \
	{ return &theClass::messageMap; }                                 \
	UILIB_COMDAT const DUI_MSGMAP theClass::messageMap =                  \
	{  &theClass::_GetBaseMessageMap, &theClass::_messageEntries[0] };\
	UILIB_COMDAT const DUI_MSGMAP_ENTRY theClass::_messageEntries[] =     \
	{                                                                     \

#else
#define DUI_BASE_BEGIN_MESSAGE_MAP(theClass)                              \
	const DUI_MSGMAP* theClass::GetMessageMap() const                     \
	{ return &theClass::messageMap; }                                 \
	UILIB_COMDAT const DUI_MSGMAP theClass::messageMap =                  \
	{  nullptr, &theClass::_messageEntries[0] };                         \
	UILIB_COMDAT const DUI_MSGMAP_ENTRY theClass::_messageEntries[] =     \
	{                                                                     \

#endif


	//子类声明开始
#ifndef UILIB_STATIC
#define DUI_BEGIN_MESSAGE_MAP(theClass, baseClass)                        \
	const DUI_MSGMAP* PASCAL theClass::_GetBaseMessageMap()               \
	{ return &baseClass::messageMap; }                                \
	const DUI_MSGMAP* theClass::GetMessageMap() const                     \
	{ return &theClass::messageMap; }                                 \
	UILIB_COMDAT const DUI_MSGMAP theClass::messageMap =                  \
	{ &theClass::_GetBaseMessageMap, &theClass::_messageEntries[0] }; \
	UILIB_COMDAT const DUI_MSGMAP_ENTRY theClass::_messageEntries[] =     \
	{                                                                     \

#else
#define DUI_BEGIN_MESSAGE_MAP(theClass, baseClass)                        \
	const DUI_MSGMAP* theClass::GetMessageMap() const                     \
	{ return &theClass::messageMap; }                                 \
	UILIB_COMDAT const DUI_MSGMAP theClass::messageMap =                  \
	{ &baseClass::messageMap, &theClass::_messageEntries[0] };        \
	UILIB_COMDAT const DUI_MSGMAP_ENTRY theClass::_messageEntries[] =     \
	{                                                                     \

#endif


	//声明结束
#define DUI_END_MESSAGE_MAP()                                             \
	{ _T(""), _T(""), static_cast<UINT>(DuiSig::DuiSig_end), (DUI_PMSG)0 }                           \
	};                                                                        \


	//定义消息类型--执行函数宏
#define DUI_ON_MSGTYPE(msgtype, memberFxn)                                \
	{ msgtype, _T(""), static_cast<UINT>(DuiSig::DuiSig_vn), (DUI_PMSG)&memberFxn},                  \


	//定义消息类型--控件名称--执行函数宏
#define DUI_ON_MSGTYPE_CTRNAME(msgtype,ctrname,memberFxn)                 \
	{ msgtype, ctrname, static_cast<UINT>(DuiSig::DuiSig_vn), (DUI_PMSG)&memberFxn },                \


	//定义click消息的控件名称--执行函数宏
#define DUI_ON_CLICK_CTRNAME(ctrname,memberFxn)                           \
	{ DUI_MSGTYPE_CLICK, ctrname, static_cast<UINT>(DuiSig::DuiSig_vn), (DUI_PMSG)&memberFxn },      \


	//定义selectchanged消息的控件名称--执行函数宏
#define DUI_ON_SELECTCHANGED_CTRNAME(ctrname,memberFxn)                   \
	{ DUI_MSGTYPE_SELECTCHANGED,ctrname,static_cast<UINT>(DuiSig::DuiSig_vn),(DUI_PMSG)&memberFxn }, \


	//定义killfocus消息的控件名称--执行函数宏
#define DUI_ON_KILLFOCUS_CTRNAME(ctrname,memberFxn)                       \
	{ DUI_MSGTYPE_KILLFOCUS,ctrname,static_cast<UINT>(DuiSig::DuiSig_vn),(DUI_PMSG)&memberFxn },     \


	//定义menu消息的控件名称--执行函数宏
#define DUI_ON_MENU_CTRNAME(ctrname,memberFxn)                            \
	{ DUI_MSGTYPE_MENU,ctrname,static_cast<UINT>(DuiSig::DuiSig_vn),(DUI_PMSG)&memberFxn },          \


	//定义与控件名称无关的消息宏

	//定义timer消息--执行函数宏
#define DUI_ON_TIMER()                                                    \
	{ DUI_MSGTYPE_TIMER, _T(""), static_cast<UINT>(DuiSig::DuiSig_vn),(DUI_PMSG)&OnTimer },          \


	///
	//////////////END消息映射宏定义////////////////////////////////////////////////////


//////////////BEGIN控件名称宏定义//////////////////////////////////////////////////
///
#define  DUI_CTR_BOX							 (_T("Box")) //

#define  DUI_CTR_EDIT                            (_T("Edit"))
#define  DUI_CTR_LIST                            (_T("List"))
#define  DUI_CTR_TEXT                            (_T("Text"))
#define  DUI_CTR_HBOX                            (_T("HBox"))
#define  DUI_CTR_VBOX                            (_T("VBox"))
#define  DUI_CTR_RING							 (_T("Ring"))

#define  DUI_CTR_COMBO                           (_T("Combo"))
#define  DUI_CTR_LABEL                           (_T("Label"))
#define  DUI_CTR_FLASH							 (_T("Flash"))

#define  DUI_CTR_BUTTON                          (_T("Button"))
#define  DUI_CTR_OPTION                          (_T("Option"))
#define  DUI_CTR_SLIDER                          (_T("Slider"))
#define  DUI_CTR_TAB_BOX					     (_T("TabBox")) //

#define  DUI_CTR_CONTROL                         (_T("Control"))
#define  DUI_CTR_ACTIVEX                         (_T("ActiveX"))
#define  DUI_CTR_GIFANIM                         (_T("GifAnim"))
#define	 DUI_CTR_TILE_BOX						 (_T("TileBox")) //
#define  DUI_CTR_LOADINGCIRCLE					 (_T("Loading")) //

#define  DUI_CTR_LISTITEM                        (_T("ListItem"))
#define  DUI_CTR_PROGRESS                        (_T("Progress"))
#define  DUI_CTR_RICHEDIT                        (_T("RichEdit"))
#define  DUI_CTR_CHECKBOX                        (_T("CheckBox"))
#define  DUI_CTR_COMBOBOX                        (_T("ComboBox"))
#define  DUI_CTR_DATETIME                        (_T("DateTime"))
#define  DUI_CTR_TREEVIEW                        (_T("TreeView"))
#define  DUI_CTR_TREENODE                        (_T("TreeNode"))
#define  DUI_CTR_CHILD_BOX					     (_T("ChildBox")) //

#define  DUI_CTR_CONTAINER                       (_T("Container"))
#define  DUI_CTR_TABLAYOUT                       (_T("TabLayout"))
#define  DUI_CTR_SCROLLBAR                       (_T("ScrollBar"))
#define  DUI_CTR_IPADDRESS                       (_T("IPAddress"))

#define  DUI_CTR_LISTHEADER                      (_T("ListHeader"))
#define  DUI_CTR_LISTFOOTER                      (_T("ListFooter"))
#define  DUI_CTR_TILELAYOUT                      (_T("TileLayout"))
#define  DUI_CTR_WEBBROWSER                      (_T("WebBrowser"))

#define  DUI_CTR_CHILDLAYOUT                     (_T("ChildLayout"))
#define  DUI_CTR_LISTELEMENT                     (_T("ListElement"))

#define  DUI_CTR_VERTICALLAYOUT                  (_T("VerticalLayout"))
#define  DUI_CTR_LISTHEADERITEM                  (_T("ListHeaderItem"))

#define  DUI_CTR_LISTTEXTELEMENT                 (_T("ListTextElement"))

#define  DUI_CTR_HORIZONTALLAYOUT                (_T("HorizontalLayout"))
#define  DUI_CTR_LISTLABELELEMENT                (_T("ListLabelElement"))

#define  DUI_CTR_ANIMATIONTABLAYOUT				 (_T("AnimationTabLayout"))

#define  DUI_CTR_LISTCONTAINERELEMENT            (_T("ListContainerElement"))

#define  DUI_CTR_TEXTSCROLL						 (_T("TextScroll"))

#define DUI_CTR_COLORPALETTE					  (_T("ColorPalette"))
	///
	//////////////END控件名称宏定义//////////////////////////////////////////////////

	}// namespace DuiLib

