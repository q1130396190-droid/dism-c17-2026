#include "StdAfx.h"
#include "S3AttrRegistry.h"
#include "AttrParse.h"

namespace DuiLib
{

	/////////////////////////////////////////////////////////////////////////////////////
	//
	//
	IMPLEMENT_DUICONTROL(CContainerUI)

		CContainerUI::CContainerUI()
		: m_iChildPadding(0),
		m_iChildAlign(DT_LEFT),
		m_iChildVAlign(DT_TOP),
		m_bAutoDestroy(true),
		// [WIN10-FIX] 禁用延迟销毁：Win10 上 vector 重分配期间 AddDelayedCleanup
		// 可能把已失效的指针放进延迟列表，WM_APP+1 消息到来时 delete 垃圾指针导致崩溃。
		// 改为直接 delete（与 CPaintManagerUI::~CPaintManagerUI 的销毁顺序也更安全）。
		m_bDelayedDestroy(false),
		m_bMouseChildEnabled(true),
		m_pVerticalScrollBar(nullptr),
		m_pHorizontalScrollBar(nullptr),
		m_nScrollStepSize(0),
		m_bFixedScrollbar(false),
		m_bShowScrollbar(true)
	{
		::ZeroMemory(&m_rcInset, sizeof(m_rcInset));
	}

	CContainerUI::~CContainerUI()
	{
		m_bDelayedDestroy = false;
		RemoveAll();
		if( m_pVerticalScrollBar ) {
			delete m_pVerticalScrollBar;
			m_pVerticalScrollBar = nullptr;
		}
		if( m_pHorizontalScrollBar ) {
			delete m_pHorizontalScrollBar;
			m_pHorizontalScrollBar = nullptr;
		}
	}

	LPCTSTR CContainerUI::GetClass() const
	{
		return _T("ContainerUI");
	}

	LPVOID CContainerUI::GetInterface(LPCTSTR pstrName)
	{
		if( _tcsicmp(pstrName, _T("IContainer")) == 0 ) return static_cast<IContainerUI*>(this);
		else if( _tcsicmp(pstrName, DUI_CTR_CONTAINER) == 0 ) return static_cast<CContainerUI*>(this);
		return CControlUI::GetInterface(pstrName);
	}

	CControlUI* CContainerUI::GetItemAt(int iIndex) const
	{
		if( iIndex < 0 || iIndex >= m_items.GetSize() ) return nullptr;
		return static_cast<CControlUI*>(m_items[iIndex]);
	}

	int CContainerUI::GetItemIndex(CControlUI* pControl) const
	{
		for( int it = 0; it < m_items.GetSize(); it++ ) {
			if( static_cast<CControlUI*>(m_items[it]) == pControl ) {
				return it;
			}
		}

		return -1;
	}

	bool CContainerUI::SetItemIndex(CControlUI* pControl, int iIndex)
	{
		for( int it = 0; it < m_items.GetSize(); it++ ) {
			if( static_cast<CControlUI*>(m_items[it]) == pControl ) {
				NeedUpdate();            
				m_items.Remove(it);
				return m_items.InsertAt(iIndex, pControl);
			}
		}

		return false;
	}

	int CContainerUI::GetCount() const
	{
		return m_items.GetSize();
	}

	bool CContainerUI::Add(CControlUI* pControl)
	{
		// [R79] Child-list mutation is UI-thread-only. Safe during markup parse:
		// CThreadAffinity is still unclaimed then (ClaimUiThread runs inside
		// AttachDialog, after the controls are built), and the macro skips while
		// unclaimed -- so this cannot fire on a legitimate skin load.
		DUI_ASSERT_UI_THREAD_FOR(m_pManager);
		if( pControl == nullptr) return false;

		if( m_pManager != nullptr ) m_pManager->InitControls(pControl, this);
		if( IsVisible() ) NeedUpdate();
		else pControl->SetInternVisible(false);
		return m_items.Add(pControl);   
	}

	bool CContainerUI::AddAt(CControlUI* pControl, int iIndex)
	{
		DUI_ASSERT_UI_THREAD_FOR(m_pManager);
		if( pControl == nullptr) return false;

		if( m_pManager != nullptr ) m_pManager->InitControls(pControl, this);
		if( IsVisible() ) NeedUpdate();
		else pControl->SetInternVisible(false);
		return m_items.InsertAt(iIndex, pControl);
	}

	bool CContainerUI::Remove(CControlUI* pControl)
	{
		DUI_ASSERT_UI_THREAD_FOR(m_pManager);
		if( pControl == nullptr) return false;

		// [WIN10-CRASH] 指针有效性校验
		uintptr_t paddr = reinterpret_cast<uintptr_t>(pControl);
		if( paddr < 0x10000 || paddr > 0x00007FFFFFFFFFFF ) return false;

		for( int it = 0; it < m_items.GetSize(); it++ ) {
			if( static_cast<CControlUI*>(m_items[it]) == pControl ) {
				NeedUpdate();
				if( m_bAutoDestroy ) {
					if( m_bDelayedDestroy && m_pManager ) m_pManager->AddDelayedCleanup(pControl);             
					else delete pControl;
				}
				return m_items.Remove(it);
			}
		}
		return false;
	}

	bool CContainerUI::RemoveAt(int iIndex)
	{
		CControlUI* pControl = GetItemAt(iIndex);
		if (pControl != nullptr) {
			return CContainerUI::Remove(pControl);
		}

		return false;
	}

	void CContainerUI::RemoveAll()
	{
		DUI_ASSERT_UI_THREAD_FOR(m_pManager);
 		for( int it = 0; m_bAutoDestroy && it < m_items.GetSize(); it++ ) {
			CControlUI* pItem = static_cast<CControlUI*>(m_items[it]);
			// [WIN10-CRASH] 指针有效性校验
			if( pItem != nullptr ) {
				uintptr_t paddr = reinterpret_cast<uintptr_t>(pItem);
				if( paddr < 0x10000 || paddr > 0x00007FFFFFFFFFFF ) continue;
			}
			if( m_bDelayedDestroy && m_pManager ) {
				m_pManager->AddDelayedCleanup(pItem);             
			}
			else {
				delete pItem;
				pItem = nullptr;
			}
		}
		m_items.Empty();
		NeedUpdate();
	}

	bool CContainerUI::IsAutoDestroy() const
	{
		return m_bAutoDestroy;
	}

	void CContainerUI::SetAutoDestroy(bool bAuto)
	{
		m_bAutoDestroy = bAuto;
	}

	bool CContainerUI::IsDelayedDestroy() const
	{
		return m_bDelayedDestroy;
	}

	void CContainerUI::SetDelayedDestroy(bool bDelayed)
	{
		m_bDelayedDestroy = bDelayed;
	}

	RECT CContainerUI::GetInset() const
	{
		if(m_pManager) return m_pManager->GetDPIObj()->Scale(m_rcInset);
		return m_rcInset;
	}

	void CContainerUI::SetInset(RECT rcInset)
	{
		m_rcInset = rcInset;
		NeedUpdate();
	}

	int CContainerUI::GetChildPadding() const
	{
		if (m_pManager) return m_pManager->GetDPIObj()->Scale(m_iChildPadding);
		return m_iChildPadding;
	}


	void CContainerUI::SetChildPadding(int iPadding)
	{
		m_iChildPadding = iPadding;
		NeedUpdate();
	}

	UINT CContainerUI::GetChildAlign() const
	{
		return m_iChildAlign;
	}

	void CContainerUI::SetChildAlign(UINT iAlign)
	{
		m_iChildAlign = iAlign;
		NeedUpdate();
	}

	UINT CContainerUI::GetChildVAlign() const
	{
		return m_iChildVAlign;
	}

	void CContainerUI::SetChildVAlign(UINT iVAlign)
	{
		m_iChildVAlign = iVAlign;
		NeedUpdate();
	}

	bool CContainerUI::IsMouseChildEnabled() const
	{
		return m_bMouseChildEnabled;
	}

	void CContainerUI::SetMouseChildEnabled(bool bEnable)
	{
		m_bMouseChildEnabled = bEnable;
	}
	
	bool CContainerUI::IsFixedScrollbar()
	{
		return m_bFixedScrollbar;
	}

	void CContainerUI::SetFixedScrollbar(bool bFixed)
	{
		m_bFixedScrollbar = bFixed;
		Invalidate();
	}

	bool CContainerUI::IsShowScrollbar()
	{
		return m_bShowScrollbar;
	}

	void CContainerUI::SetShowScrollbar(bool bShow)
	{
		m_bShowScrollbar = bShow;
		
		if( m_pVerticalScrollBar != nullptr ) m_pVerticalScrollBar->SetShow(bShow);
		if( m_pHorizontalScrollBar != nullptr ) m_pHorizontalScrollBar->SetShow(bShow);
	}

	void CContainerUI::SetVisible(bool bVisible)
	{
		if( m_bVisible == bVisible ) return;
		CControlUI::SetVisible(bVisible);
		for( int it = 0; it < m_items.GetSize(); it++ ) {
			static_cast<CControlUI*>(m_items[it])->SetInternVisible(IsVisible());
		}
	}

	// �߼��ϣ�����Container�ؼ��������˷���
	// ���ô˷����Ľ���ǣ��ڲ��ӿؼ����أ��ؼ�������Ȼ��ʾ��������Ч������
	void CContainerUI::SetInternVisible(bool bVisible)
	{
		CControlUI::SetInternVisible(bVisible);
		if( m_items.IsEmpty() ) return;
		for( int it = 0; it < m_items.GetSize(); it++ ) {
			// �����ӿؼ���ʾ״̬
			// InternVisible״̬Ӧ���ӿؼ��Լ�����
			static_cast<CControlUI*>(m_items[it])->SetInternVisible(IsVisible());
		}
	}

	void CContainerUI::SetEnabled(bool bEnabled)
	{
		DUI_ASSERT_UI_THREAD_FOR(m_pManager);
		if( m_bEnabled == bEnabled ) return;

		m_bEnabled = bEnabled;

		for( int it = 0; it < m_items.GetSize(); it++ ) {
			static_cast<CControlUI*>(m_items[it])->SetEnabled(m_bEnabled);
		}

		Invalidate();
	}

	void CContainerUI::SetMouseEnabled(bool bEnabled)
	{
		if( m_pVerticalScrollBar != nullptr ) m_pVerticalScrollBar->SetMouseEnabled(bEnabled);
		if( m_pHorizontalScrollBar != nullptr ) m_pHorizontalScrollBar->SetMouseEnabled(bEnabled);
		CControlUI::SetMouseEnabled(bEnabled);
	}

	void CContainerUI::DoEvent(TEventUI& event)
	{
		if( !IsMouseEnabled() && event.Type > UIEVENT__MOUSEBEGIN && event.Type < UIEVENT__MOUSEEND ) {
			if( m_pParent != nullptr ) m_pParent->DoEvent(event);
			else CControlUI::DoEvent(event);
			return;
		}

		if( event.Type == UIEVENT_SETFOCUS ) 
		{
			m_bFocused = true;
			return;
		}
		if( event.Type == UIEVENT_KILLFOCUS ) 
		{
			m_bFocused = false;
			return;
		}
		if( m_pVerticalScrollBar != nullptr && m_pVerticalScrollBar->IsVisible() && m_pVerticalScrollBar->IsEnabled() )
		{
			if( event.Type == UIEVENT_KEYDOWN ) 
			{
				switch( event.chKey ) {
				case VK_DOWN:
					LineDown();
					return;
				case VK_UP:
					LineUp();
					return;
				case VK_NEXT:
					PageDown();
					return;
				case VK_PRIOR:
					PageUp();
					return;
				case VK_HOME:
					HomeUp();
					return;
				case VK_END:
					EndDown();
					return;
				}
			}
			else if( event.Type == UIEVENT_SCROLLWHEEL )
			{
				switch( LOWORD(event.wParam) ) {
				case SB_LINEUP:
					LineUp();
					return;
				case SB_LINEDOWN:
					LineDown();
					return;
				}
			}
		}
		if( m_pHorizontalScrollBar != nullptr && m_pHorizontalScrollBar->IsVisible() && m_pHorizontalScrollBar->IsEnabled() ) {
			if( event.Type == UIEVENT_KEYDOWN ) 
			{
				switch( event.chKey ) {
				case VK_RIGHT:
					LineRight();
					return;
				case VK_LEFT:
					LineLeft();
					return;
				case VK_NEXT:
					PageRight();
					return;
				case VK_PRIOR:
					PageLeft();
					return;
				case VK_HOME:
					HomeLeft();
					return;
				case VK_END:
					EndRight();
					return;
				}
			}
			else if( event.Type == UIEVENT_SCROLLWHEEL )
			{
				switch( LOWORD(event.wParam) ) {
				case SB_LINEUP:
					LineLeft();
					return;
				case SB_LINEDOWN:
					LineRight();
					return;
				}
			}
		}
		CControlUI::DoEvent(event);
	}

	SIZE CContainerUI::GetScrollPos() const
	{
		SIZE sz = {0, 0};
		if( m_pVerticalScrollBar && m_pVerticalScrollBar->IsVisible() ) sz.cy = m_pVerticalScrollBar->GetScrollPos();
		if( m_pHorizontalScrollBar && m_pHorizontalScrollBar->IsVisible() ) sz.cx = m_pHorizontalScrollBar->GetScrollPos();
		return sz;
	}

	SIZE CContainerUI::GetScrollRange() const
	{
		SIZE sz = {0, 0};
		if( m_pVerticalScrollBar && m_pVerticalScrollBar->IsVisible() ) sz.cy = m_pVerticalScrollBar->GetScrollRange();
		if( m_pHorizontalScrollBar && m_pHorizontalScrollBar->IsVisible() ) sz.cx = m_pHorizontalScrollBar->GetScrollRange();
		return sz;
	}

	void CContainerUI::SetScrollPos(SIZE szPos, bool bMsg)
	{
		int cx = 0;
		int cy = 0;
		if( m_pVerticalScrollBar && m_pVerticalScrollBar->IsVisible() ) {
			int iLastScrollPos = m_pVerticalScrollBar->GetScrollPos();
			m_pVerticalScrollBar->SetScrollPos(szPos.cy);
			cy = m_pVerticalScrollBar->GetScrollPos() - iLastScrollPos;
		}

		if( m_pHorizontalScrollBar && m_pHorizontalScrollBar->IsVisible() ) {
			int iLastScrollPos = m_pHorizontalScrollBar->GetScrollPos();
			m_pHorizontalScrollBar->SetScrollPos(szPos.cx);
			cx = m_pHorizontalScrollBar->GetScrollPos() - iLastScrollPos;
		}

		if( cx == 0 && cy == 0 ) return;

		RECT rcPos;
		for( int it2 = 0; it2 < m_items.GetSize(); it2++ ) {
			CControlUI* pControl = static_cast<CControlUI*>(m_items[it2]);
			if( !pControl->IsVisible() ) continue;
			if( pControl->IsFloat() ) continue;

			rcPos = pControl->GetPos();
			rcPos.left -= cx;
			rcPos.right -= cx;
			rcPos.top -= cy;
			rcPos.bottom -= cy;
			pControl->SetPos(rcPos, false);
		}

		// Batch child movement into one viewport invalidation.
		Invalidate();

		if(m_pVerticalScrollBar)
		{
			// ���͹�����Ϣ
			if( m_pManager != nullptr && bMsg ) {
				const int nLineSize = m_pVerticalScrollBar->GetLineSize();
				const int nPage = nLineSize > 0 ? (m_pVerticalScrollBar->GetScrollPos() + nLineSize) / nLineSize : 0;
				m_pManager->SendNotify(this, DUI_MSGTYPE_SCROLL, static_cast<WPARAM>(nPage));
			}
		}
	}

	void CContainerUI::SetScrollStepSize(int nSize)
	{
		if (nSize >0)
			m_nScrollStepSize = nSize;
	}

	int CContainerUI::GetScrollStepSize() const
	{
		if(m_pManager )return m_pManager->GetDPIObj()->Scale(m_nScrollStepSize);

		return m_nScrollStepSize;
	}

	void CContainerUI::LineUp()
	{
		int cyLine = GetScrollStepSize();
		if (cyLine == 0) {
			cyLine = 8;
			if( m_pManager ) cyLine = m_pManager->GetDefaultFontInfo()->tm.tmHeight + 8;
		}

		SIZE sz = GetScrollPos();
		sz.cy -= cyLine;
		SetScrollPos(sz);
	}

	void CContainerUI::LineDown()
	{
		int cyLine = GetScrollStepSize();
		if (cyLine == 0) {
			cyLine = 8;
			if( m_pManager ) cyLine = m_pManager->GetDefaultFontInfo()->tm.tmHeight + 8;
		}

		SIZE sz = GetScrollPos();
		sz.cy += cyLine;
		SetScrollPos(sz);
	}

	void CContainerUI::PageUp()
	{
		SIZE sz = GetScrollPos();
		int iOffset = m_rcItem.bottom - m_rcItem.top - m_rcInset.top - m_rcInset.bottom;
		if( m_pHorizontalScrollBar && m_pHorizontalScrollBar->IsVisible() ) iOffset -= m_pHorizontalScrollBar->GetFixedHeight();
		sz.cy -= iOffset;
		SetScrollPos(sz);
	}

	void CContainerUI::PageDown()
	{
		SIZE sz = GetScrollPos();
		int iOffset = m_rcItem.bottom - m_rcItem.top - m_rcInset.top - m_rcInset.bottom;
		if( m_pHorizontalScrollBar && m_pHorizontalScrollBar->IsVisible() ) iOffset -= m_pHorizontalScrollBar->GetFixedHeight();
		sz.cy += iOffset;
		SetScrollPos(sz);
	}

	void CContainerUI::HomeUp()
	{
		SIZE sz = GetScrollPos();
		sz.cy = 0;
		SetScrollPos(sz);
	}

	void CContainerUI::EndDown()
	{
		SIZE sz = GetScrollPos();
		sz.cy = GetScrollRange().cy;
		SetScrollPos(sz);
	}

	void CContainerUI::LineLeft()
	{
		int nScrollStepSize = GetScrollStepSize();
		int cxLine = nScrollStepSize == 0 ? 8 : nScrollStepSize;

		SIZE sz = GetScrollPos();
		sz.cx -= cxLine;
		SetScrollPos(sz);
	}

	void CContainerUI::LineRight()
	{
		int nScrollStepSize = GetScrollStepSize();
		int cxLine = nScrollStepSize == 0 ? 8 : nScrollStepSize;

		SIZE sz = GetScrollPos();
		sz.cx += cxLine;
		SetScrollPos(sz);
	}

	void CContainerUI::PageLeft()
	{
		SIZE sz = GetScrollPos();

		RECT rcInset = GetInset();
		int iOffset = m_rcItem.right - m_rcItem.left - rcInset.left - rcInset.right;
		if( m_pVerticalScrollBar && m_pVerticalScrollBar->IsVisible() ) iOffset -= m_pVerticalScrollBar->GetFixedWidth();
		sz.cx -= iOffset;
		SetScrollPos(sz);
	}

	void CContainerUI::PageRight()
	{
		SIZE sz = GetScrollPos();

		RECT rcInset = GetInset();
		int iOffset = m_rcItem.right - m_rcItem.left - rcInset.left - rcInset.right;
		if( m_pVerticalScrollBar && m_pVerticalScrollBar->IsVisible() ) iOffset -= m_pVerticalScrollBar->GetFixedWidth();
		sz.cx += iOffset;
		SetScrollPos(sz);
	}

	void CContainerUI::HomeLeft()
	{
		SIZE sz = GetScrollPos();
		sz.cx = 0;
		SetScrollPos(sz);
	}

	void CContainerUI::EndRight()
	{
		SIZE sz = GetScrollPos();
		sz.cx = GetScrollRange().cx;
		SetScrollPos(sz);
	}

	void CContainerUI::EnableScrollBar(bool bEnableVertical, bool bEnableHorizontal)
	{
		if( bEnableVertical && !m_pVerticalScrollBar ) {
			m_pVerticalScrollBar = new CScrollBarUI;
			m_pVerticalScrollBar->SetOwner(this);
			m_pVerticalScrollBar->SetManager(m_pManager, nullptr, false);
			if ( m_pManager ) {
				LPCTSTR pDefaultAttributes = m_pManager->GetDefaultAttributeList(_T("VScrollBar"));
				if( pDefaultAttributes ) {
					m_pVerticalScrollBar->ApplyAttributeList(pDefaultAttributes);
				}

				m_pVerticalScrollBar->SetShow(m_bShowScrollbar);
			}
		}
		else if( !bEnableVertical && m_pVerticalScrollBar ) {
			delete m_pVerticalScrollBar;
			m_pVerticalScrollBar = nullptr;
		}

		if( bEnableHorizontal && !m_pHorizontalScrollBar ) {
			m_pHorizontalScrollBar = new CScrollBarUI;
			m_pHorizontalScrollBar->SetHorizontal(true);
			m_pHorizontalScrollBar->SetOwner(this);
			m_pHorizontalScrollBar->SetManager(m_pManager, nullptr, false);

			if ( m_pManager ) {
				LPCTSTR pDefaultAttributes = m_pManager->GetDefaultAttributeList(_T("HScrollBar"));
				if( pDefaultAttributes ) {
					m_pHorizontalScrollBar->ApplyAttributeList(pDefaultAttributes);
				}

				m_pHorizontalScrollBar->SetShow(m_bShowScrollbar);
			}
		}
		else if( !bEnableHorizontal && m_pHorizontalScrollBar ) {
			delete m_pHorizontalScrollBar;
			m_pHorizontalScrollBar = nullptr;
		}

		NeedUpdate();
	}

	CScrollBarUI* CContainerUI::GetVerticalScrollBar() const
	{
		return m_pVerticalScrollBar;
	}

	CScrollBarUI* CContainerUI::GetHorizontalScrollBar() const
	{
		return m_pHorizontalScrollBar;
	}

	int CContainerUI::FindSelectable(int iIndex, bool bForward /*= true*/) const
	{
		// NOTE: This is actually a helper-function for the list/combo/ect controls
		//       that allow them to find the next enabled/available selectable item
		const int nCount = GetCount();
		if( nCount == 0 ) return -1;
		// [WIN10-FIX] CLAMP 安全边界：GetCount()-1 若为 -1（空容器），上限用 0 兜底。
		const int nMaxIndex = (nCount > 0) ? (nCount - 1) : 0;
		iIndex = CLAMP(iIndex, 0, nMaxIndex);
		if( bForward ) {
			for( int i = iIndex; i < nCount; i++ ) {
				CControlUI* pItem = GetItemAt(i);
				// [WIN10-FIX] GetItemAt 可能返回 nullptr（越界/空容器），先检查再解引用。
				if( pItem == nullptr ) continue;
				if( pItem->GetInterface(_T("ListItem")) != nullptr 
					&& pItem->IsVisible()
					&& pItem->IsEnabled() ) return i;
			}
			return -1;
		}
		else {
			for( int i = iIndex; i >= 0; --i ) {
				CControlUI* pItem = GetItemAt(i);
				if( pItem == nullptr ) continue;
				if( pItem->GetInterface(_T("ListItem")) != nullptr 
					&& pItem->IsVisible()
					&& pItem->IsEnabled() ) return i;
			}
			return FindSelectable(0, true);
		}
	}

	RECT CContainerUI::GetClientPos() const
	{
		RECT rc = m_rcItem;

		RECT rcInset = GetInset();
		rc.left += rcInset.left;
		rc.top += rcInset.top;
		rc.right -= rcInset.right;
		rc.bottom -= rcInset.bottom;

		if( m_pVerticalScrollBar && m_pVerticalScrollBar->IsVisible() ) {
			rc.top -= m_pVerticalScrollBar->GetScrollPos();
			rc.bottom -= m_pVerticalScrollBar->GetScrollPos();
			rc.bottom += m_pVerticalScrollBar->GetScrollRange();
			rc.right -= m_pVerticalScrollBar->GetFixedWidth();
		}
		if( m_pHorizontalScrollBar && m_pHorizontalScrollBar->IsVisible() ) {
			rc.left -= m_pHorizontalScrollBar->GetScrollPos();
			rc.right -= m_pHorizontalScrollBar->GetScrollPos();
			rc.right += m_pHorizontalScrollBar->GetScrollRange();
			rc.bottom -= m_pHorizontalScrollBar->GetFixedHeight();
		}
		return rc;
	}

	void CContainerUI::Move(SIZE szOffset, bool bNeedInvalidate)
	{
		CControlUI::Move(szOffset, bNeedInvalidate);
		if( m_pVerticalScrollBar && m_pVerticalScrollBar->IsVisible() ) m_pVerticalScrollBar->Move(szOffset, false);
		if( m_pHorizontalScrollBar && m_pHorizontalScrollBar->IsVisible() ) m_pHorizontalScrollBar->Move(szOffset, false);
		for( int it = 0; it < m_items.GetSize(); it++ ) {
			CControlUI* pControl = static_cast<CControlUI*>(m_items[it]);
			if( pControl != nullptr && pControl->IsVisible() ) pControl->Move(szOffset, false);
		}
	}

	void CContainerUI::SetPos(RECT rc, bool bNeedInvalidate)
	{
		CControlUI::SetPos(rc, bNeedInvalidate);
		if( m_items.IsEmpty() ) return;

		rc = m_rcItem;
		RECT rcInset = GetInset();
		rc.left += rcInset.left;
		rc.top += rcInset.top;
		rc.right -= rcInset.right;
		rc.bottom -= rcInset.bottom;

		if( m_pVerticalScrollBar && m_pVerticalScrollBar->IsVisible() ) {
			rc.top -= m_pVerticalScrollBar->GetScrollPos();
			rc.bottom -= m_pVerticalScrollBar->GetScrollPos();
			rc.bottom += m_pVerticalScrollBar->GetScrollRange();
			rc.right -= m_pVerticalScrollBar->GetFixedWidth();
		}
		if( m_pHorizontalScrollBar && m_pHorizontalScrollBar->IsVisible() ) {
			rc.left -= m_pHorizontalScrollBar->GetScrollPos();
			rc.right -= m_pHorizontalScrollBar->GetScrollPos();
			rc.right += m_pHorizontalScrollBar->GetScrollRange();
			rc.bottom -= m_pHorizontalScrollBar->GetFixedHeight();
		}

		for( int it = 0; it < m_items.GetSize(); it++ ) {
			CControlUI* pControl = static_cast<CControlUI*>(m_items[it]);
			if( !pControl->IsVisible() ) continue;
			if( pControl->IsFloat() ) {
				SetFloatPos(it);
			}
			else { 
				SIZE sz = { rc.right - rc.left, rc.bottom - rc.top };
				if( sz.cx < pControl->GetMinWidth() ) sz.cx = pControl->GetMinWidth();
				if( sz.cx > pControl->GetMaxWidth() ) sz.cx = pControl->GetMaxWidth();
				if( sz.cy < pControl->GetMinHeight() ) sz.cy = pControl->GetMinHeight();
				if( sz.cy > pControl->GetMaxHeight() ) sz.cy = pControl->GetMaxHeight();
				RECT rcCtrl = { rc.left, rc.top, rc.left + sz.cx, rc.top + sz.cy };
				pControl->SetPos(rcCtrl, false);
			}
		}
	}

	void CContainerUI::SetAttribute(LPCTSTR pstrName, LPCTSTR pstrValue)
	{		// ---- S3/T3.3a: fast path via compile-time hash registry ----
		// O(log n) lookup instead of O(n) string compares.
		// Falls through to the legacy chain below when unmatched, so
		// third-party custom attributes keep working unchanged.
		switch( DuiLib::s3::lookup_prop_t(pstrName) ) {
		case DuiLib::s3::PropId::inset: { RECT rcInset = { 0 }; if( !DuiLib::attr::ParseRect4(pstrValue, rcInset) ) { DUI_LOGW("attr 'inset' bad value='%s'", pstrValue); return; } SetInset(rcInset); return; }
		case DuiLib::s3::PropId::mousechild: { SetMouseChildEnabled(_tcsicmp(pstrValue, _T("true")) == 0); return; }
		case DuiLib::s3::PropId::vscrollbar: { EnableScrollBar(_tcsicmp(pstrValue, _T("true")) == 0, GetHorizontalScrollBar() != nullptr); return; }
		case DuiLib::s3::PropId::vscrollbarstyle: { m_sVerticalScrollBarStyle = pstrValue; EnableScrollBar(TRUE, GetHorizontalScrollBar() != nullptr); if( GetVerticalScrollBar() ) { 				LPCTSTR pStyle = m_pManager->GetStyle(m_sVerticalScrollBarStyle); if( pStyle ) { 					GetVerticalScrollBar()->ApplyAttributeList(pStyle); } 				else { 					GetVerticalScrollBar()->ApplyAttributeList(pstrValue); } 			}; return; }
		case DuiLib::s3::PropId::hscrollbar: { EnableScrollBar(GetVerticalScrollBar() != nullptr, _tcsicmp(pstrValue, _T("true")) == 0); return; }
		case DuiLib::s3::PropId::hscrollbarstyle: { m_sHorizontalScrollBarStyle = pstrValue; EnableScrollBar(TRUE, GetHorizontalScrollBar() != nullptr); if( GetHorizontalScrollBar() ) { 				LPCTSTR pStyle = m_pManager->GetStyle(m_sHorizontalScrollBarStyle); if( pStyle ) { 					GetHorizontalScrollBar()->ApplyAttributeList(pStyle); } 				else { 					GetHorizontalScrollBar()->ApplyAttributeList(pstrValue); } 			}; return; }
		case DuiLib::s3::PropId::childpadding: { SetChildPadding(_ttoi(pstrValue)); return; }
		case DuiLib::s3::PropId::scrollstepsize: { SetScrollStepSize(_ttoi(pstrValue)); return; }
		case DuiLib::s3::PropId::fixedscrollbar: { SetFixedScrollbar(_tcsicmp(pstrValue, _T("true")) == 0); return; }
		case DuiLib::s3::PropId::showscrollbar: { SetShowScrollbar(_tcsicmp(pstrValue, _T("true")) == 0); return; }
		default: break;
		}

		// ---- legacy fallback chain (kept verbatim for compatibility) ----

		if( _tcsicmp(pstrName, _T("inset")) == 0 ) {
			RECT rcInset = { 0 };
			if( !DuiLib::attr::ParseRect4(pstrValue, rcInset) ) { DUI_LOGW("attr 'inset' bad value='%s'", pstrValue); }
			else { SetInset(rcInset); }
		}
		else if( _tcsicmp(pstrName, _T("mousechild")) == 0 ) SetMouseChildEnabled(_tcsicmp(pstrValue, _T("true")) == 0);
		else if( _tcsicmp(pstrName, _T("vscrollbar")) == 0 ) {
			EnableScrollBar(_tcsicmp(pstrValue, _T("true")) == 0, GetHorizontalScrollBar() != nullptr);
		}
		else if( _tcsicmp(pstrName, _T("vscrollbarstyle")) == 0 ) {
			m_sVerticalScrollBarStyle = pstrValue;
			EnableScrollBar(TRUE, GetHorizontalScrollBar() != nullptr);
			if( GetVerticalScrollBar() ) {
				LPCTSTR pStyle = m_pManager->GetStyle(m_sVerticalScrollBarStyle);
				if( pStyle ) {
					GetVerticalScrollBar()->ApplyAttributeList(pStyle);
				}
				else {
					GetVerticalScrollBar()->ApplyAttributeList(pstrValue);
				}
			}
		}
		else if( _tcsicmp(pstrName, _T("hscrollbar")) == 0 ) {
			EnableScrollBar(GetVerticalScrollBar() != nullptr, _tcsicmp(pstrValue, _T("true")) == 0);
		}
		else if( _tcsicmp(pstrName, _T("hscrollbarstyle")) == 0 ) {
			m_sHorizontalScrollBarStyle = pstrValue;
			EnableScrollBar(TRUE, GetHorizontalScrollBar() != nullptr);
			if( GetHorizontalScrollBar() ) {
				LPCTSTR pStyle = m_pManager->GetStyle(m_sHorizontalScrollBarStyle);
				if( pStyle ) {
					GetHorizontalScrollBar()->ApplyAttributeList(pStyle);
				}
				else {
					GetHorizontalScrollBar()->ApplyAttributeList(pstrValue);
				}
			}
		}
		else if( _tcsicmp(pstrName, _T("childpadding")) == 0 ) SetChildPadding(_ttoi(pstrValue));
		else if( _tcscmp(pstrName, _T("childalign")) == 0 ) {
			if( _tcscmp(pstrValue, _T("left")) == 0 ) m_iChildAlign = DT_LEFT;
			else if( _tcscmp(pstrValue, _T("center")) == 0 ) m_iChildAlign = DT_CENTER;
			else if( _tcscmp(pstrValue, _T("right")) == 0 ) m_iChildAlign = DT_RIGHT;
		}
		else if( _tcscmp(pstrName, _T("childvalign")) == 0 ) {
			if( _tcscmp(pstrValue, _T("top")) == 0 ) m_iChildVAlign = DT_TOP;
			else if( _tcscmp(pstrValue, _T("vcenter")) == 0 ) m_iChildVAlign = DT_VCENTER;
			else if( _tcscmp(pstrValue, _T("bottom")) == 0 ) m_iChildVAlign = DT_BOTTOM;
		}
		else if( _tcsicmp(pstrName, _T("scrollstepsize")) == 0 ) SetScrollStepSize(_ttoi(pstrValue));
		else if (_tcsicmp(pstrName, _T("fixedscrollbar")) == 0) SetFixedScrollbar(_tcsicmp(pstrValue, _T("true")) == 0);
		else if (_tcsicmp(pstrName, _T("showscrollbar")) == 0) SetShowScrollbar(_tcsicmp(pstrValue, _T("true")) == 0);
		else CControlUI::SetAttribute(pstrName, pstrValue);
	}

	void CContainerUI::SetManager(CPaintManagerUI* pManager, CControlUI* pParent, bool bInit)
	{
		for( int it = 0; it < m_items.GetSize(); it++ ) {
			static_cast<CControlUI*>(m_items[it])->SetManager(pManager, this, bInit);
		}

		if( m_pVerticalScrollBar != nullptr ) m_pVerticalScrollBar->SetManager(pManager, this, bInit);
		if( m_pHorizontalScrollBar != nullptr ) m_pHorizontalScrollBar->SetManager(pManager, this, bInit);
		CControlUI::SetManager(pManager, pParent, bInit);
	}

	CControlUI* CContainerUI::FindControl(FINDCONTROLPROC Proc, LPVOID pData, UINT uFlags)
	{
		if( (uFlags & UIFIND_VISIBLE) != 0 && !IsVisible() ) return nullptr;
		if( (uFlags & UIFIND_ENABLED) != 0 && !IsEnabled() ) return nullptr;
		if( (uFlags & UIFIND_HITTEST) != 0 && !::PtInRect(&m_rcItem, *(static_cast<LPPOINT>(pData))) ) return nullptr;
		if( (uFlags & UIFIND_UPDATETEST) != 0 && Proc(this, pData) != nullptr ) return nullptr;

		CControlUI* pResult = nullptr;
		if( (uFlags & UIFIND_ME_FIRST) != 0 ) {
			if( (uFlags & UIFIND_HITTEST) == 0 || IsMouseEnabled() ) pResult = Proc(this, pData);
		}
		if( pResult == nullptr && m_pVerticalScrollBar != nullptr ) {
			if( (uFlags & UIFIND_HITTEST) == 0 || IsMouseEnabled() ) pResult = m_pVerticalScrollBar->FindControl(Proc, pData, uFlags);
		}
		if( pResult == nullptr && m_pHorizontalScrollBar != nullptr ) {
			if( (uFlags & UIFIND_HITTEST) == 0 || IsMouseEnabled() ) pResult = m_pHorizontalScrollBar->FindControl(Proc, pData, uFlags);
		}
		if( pResult != nullptr ) return pResult;

		if( (uFlags & UIFIND_HITTEST) == 0 || IsMouseChildEnabled() ) {
			RECT rc = m_rcItem;
			
			RECT rcInset = GetInset();
			rc.left += rcInset.left;
			rc.top += rcInset.top;
			rc.right -= rcInset.right;
			rc.bottom -= rcInset.bottom;

			if( m_pVerticalScrollBar && m_pVerticalScrollBar->IsVisible() ) rc.right -= m_pVerticalScrollBar->GetFixedWidth();
			if( m_pHorizontalScrollBar && m_pHorizontalScrollBar->IsVisible() ) rc.bottom -= m_pHorizontalScrollBar->GetFixedHeight();
			if( (uFlags & UIFIND_TOP_FIRST) != 0 ) {
				for( int it = m_items.GetSize() - 1; it >= 0; it-- ) {
					pResult = static_cast<CControlUI*>(m_items[it])->FindControl(Proc, pData, uFlags);
					if( pResult != nullptr ) {
						if( (uFlags & UIFIND_HITTEST) != 0 && !pResult->IsFloat() && !::PtInRect(&rc, *(static_cast<LPPOINT>(pData))) )
							continue;
						else 
							return pResult;
					}          
				}
			}
			else {
				for( int it = 0; it < m_items.GetSize(); it++ ) {
					pResult = static_cast<CControlUI*>(m_items[it])->FindControl(Proc, pData, uFlags);
					if( pResult != nullptr ) {
						if( (uFlags & UIFIND_HITTEST) != 0 && !pResult->IsFloat() && !::PtInRect(&rc, *(static_cast<LPPOINT>(pData))) )
							continue;
						else 
							return pResult;
					} 
				}
			}
		}

		pResult = nullptr;
		if( pResult == nullptr && (uFlags & UIFIND_ME_FIRST) == 0 ) {
			if( (uFlags & UIFIND_HITTEST) == 0 || IsMouseEnabled() ) pResult = Proc(this, pData);
		}
		return pResult;
	}

	bool CContainerUI::DoPaint(HDC hDC, const RECT& rcPaint, CControlUI* pStopControl)
	{
		RECT rcTemp = { 0 };
		if( !::IntersectRect(&rcTemp, &rcPaint, &m_rcItem) ) return true;

		CRenderClip clip;
		CRenderClip::GenerateClip(hDC, rcTemp, clip);
		CControlUI::DoPaint(hDC, rcPaint, pStopControl);

		if( m_items.GetSize() > 0 ) {
			RECT rcInset = GetInset();
			RECT rc = m_rcItem;
			rc.left += rcInset.left;
			rc.top += rcInset.top;
			rc.right -= rcInset.right;
			rc.bottom -= rcInset.bottom;
			if( m_pVerticalScrollBar && m_pVerticalScrollBar->IsVisible() ) rc.right -= m_pVerticalScrollBar->GetFixedWidth();
			if( m_pHorizontalScrollBar && m_pHorizontalScrollBar->IsVisible() ) rc.bottom -= m_pHorizontalScrollBar->GetFixedHeight();

			if( !::IntersectRect(&rcTemp, &rcPaint, &rc) ) {
				for( int it = 0; it < m_items.GetSize(); it++ ) {
					CControlUI* pControl = static_cast<CControlUI*>(m_items[it]);
					if( pControl == pStopControl ) return false;
					if( !pControl->IsVisible() ) continue;
					if( !::IntersectRect(&rcTemp, &rcPaint, &pControl->GetPos()) ) continue;
					if( pControl ->IsFloat() ) {
						if( !::IntersectRect(&rcTemp, &m_rcItem, &pControl->GetPos()) ) continue;
						if( !pControl->Paint(hDC, rcPaint, pStopControl) ) return false;
					}
				}
			}
			else {
				CRenderClip childClip;
				// [R45] Honour borderround for children, not just for our own fill.
				//
				// Before this, borderround drew a rounded background and a rounded
				// stroke but every child control still painted into the square rect,
				// so any child that reached a corner stuck out past the curve. That
				// is visible whenever a container has both a round background and
				// content near its edges.
				//
				// The clip is the intersection of the paint rect and a rounded-rect
				// region, so it is strictly tighter than before: nothing new can be
				// revealed, only the corners get trimmed. Zero radius keeps the old
				// rectangular path exactly (GenerateRoundClip with 0,0 degenerates
				// to a rect), so square-cornered containers are unaffected.
				const SIZE szRound = GetBorderRoundScaled();
				if( szRound.cx > 0 || szRound.cy > 0 ) {
					CRenderClip::GenerateRoundClip(hDC, rcTemp, m_rcItem,
					                               szRound.cx, szRound.cy, childClip);
				}
				else {
					CRenderClip::GenerateClip(hDC, rcTemp, childClip);
				}
				for( int it = 0; it < m_items.GetSize(); it++ ) {
					CControlUI* pControl = static_cast<CControlUI*>(m_items[it]);
					if( pControl == pStopControl ) return false;
					if( !pControl->IsVisible() ) continue;
					if( !::IntersectRect(&rcTemp, &rcPaint, &pControl->GetPos()) ) continue;
					if( pControl->IsFloat() ) {
						if( !::IntersectRect(&rcTemp, &m_rcItem, &pControl->GetPos()) ) continue;
						CRenderClip::UseOldClipBegin(hDC, childClip);
                        if( !pControl->Paint(hDC, rcPaint, pStopControl) ) return false;
						CRenderClip::UseOldClipEnd(hDC, childClip);
					}
					else {
						if( !::IntersectRect(&rcTemp, &rc, &pControl->GetPos()) ) continue;
                        if( !pControl->Paint(hDC, rcPaint, pStopControl) ) return false;
					}
				}
			}
		}

		if( m_pVerticalScrollBar != nullptr && m_pVerticalScrollBar->IsVisible() ) {
			if( m_pVerticalScrollBar == pStopControl ) return false;
			if( ::IntersectRect(&rcTemp, &rcPaint, &m_pVerticalScrollBar->GetPos()) ) {
				if( !m_pVerticalScrollBar->Paint(hDC, rcPaint, pStopControl) ) return false;
			}
		}

		if( m_pHorizontalScrollBar != nullptr && m_pHorizontalScrollBar->IsVisible() ) {
			if( m_pHorizontalScrollBar == pStopControl ) return false;
			if( ::IntersectRect(&rcTemp, &rcPaint, &m_pHorizontalScrollBar->GetPos()) ) {
				if( !m_pHorizontalScrollBar->Paint(hDC, rcPaint, pStopControl) ) return false;
			}
		}
		return true;
	}

	void CContainerUI::SetFloatPos(int iIndex)
	{
		// ��ΪCControlUI::SetPos��float�Ĳ���Ӱ�죬���ﲻ�ܶ�float������ӹ�������Ӱ��
		if( iIndex < 0 || iIndex >= m_items.GetSize() ) return;

		CControlUI* pControl = static_cast<CControlUI*>(m_items[iIndex]);

		if( !pControl->IsVisible() ) return;
		if( !pControl->IsFloat() ) return;

		SIZE szXY = pControl->GetFixedXY();
		SIZE sz = {pControl->GetFixedWidth(), pControl->GetFixedHeight()};

		int nParentWidth = m_rcItem.right - m_rcItem.left;
		int nParentHeight = m_rcItem.bottom - m_rcItem.top;

		UINT uAlign = pControl->GetFloatAlign();
		if(uAlign != 0) {
			RECT rcCtrl = {0, 0, sz.cx, sz.cy};
			if((uAlign & DT_CENTER) != 0) {
				::OffsetRect(&rcCtrl, (nParentWidth - sz.cx) / 2, 0);
			}
			else if((uAlign & DT_RIGHT) != 0) {
				::OffsetRect(&rcCtrl, nParentWidth - sz.cx, 0);
			}
			else {
				::OffsetRect(&rcCtrl, szXY.cx, 0);
			}

			if((uAlign & DT_VCENTER) != 0) {
				::OffsetRect(&rcCtrl, 0, (nParentHeight - sz.cy) / 2);
			}
			else if((uAlign & DT_BOTTOM) != 0) {
				::OffsetRect(&rcCtrl, 0, nParentHeight - sz.cy);
			}
			else {
				::OffsetRect(&rcCtrl, 0, szXY.cy);
			}

			::OffsetRect(&rcCtrl, m_rcItem.left, m_rcItem.top);
			pControl->SetPos(rcCtrl, false);
		}
		else {
			TPercentInfo rcPercent = pControl->GetFloatPercent();
			LONG width = m_rcItem.right - m_rcItem.left;
			LONG height = m_rcItem.bottom - m_rcItem.top;
			LONG left = szXY.cx < 0 ? m_rcItem.right : m_rcItem.left;
			LONG top = szXY.cy < 0 ? m_rcItem.bottom : m_rcItem.top;
			RECT rcCtrl = { 0 };
			rcCtrl.left = static_cast<LONG>(width*rcPercent.left) + szXY.cx + left;
			rcCtrl.top = static_cast<LONG>(height*rcPercent.top) + szXY.cy + top;
			rcCtrl.right = static_cast<LONG>(width*rcPercent.right) + szXY.cx + sz.cx + left;
			rcCtrl.bottom = static_cast<LONG>(height*rcPercent.bottom) + szXY.cy + sz.cy + top;
			pControl->SetPos(rcCtrl, false);
		}
	}

	void CContainerUI::ProcessScrollBar(RECT rc, int cxRequired, int cyRequired)
	{
		while (m_pHorizontalScrollBar)
		{
			// Scroll needed
			if (cxRequired > rc.right - rc.left && !m_pHorizontalScrollBar->IsVisible())
			{
				m_pHorizontalScrollBar->SetVisible(true);
				m_pHorizontalScrollBar->SetScrollRange(cxRequired - (rc.right - rc.left));
				m_pHorizontalScrollBar->SetScrollPos(0);
				SetPos(m_rcItem);
				break;
			}

			// No scrollbar required
			if (!m_pHorizontalScrollBar->IsVisible()) break;

			// Scroll not needed anymore?
			int cxScroll = cxRequired - (rc.right - rc.left);
			if (cxScroll <= 0)
			{
				m_pHorizontalScrollBar->SetVisible(false);
				m_pHorizontalScrollBar->SetScrollPos(0);
				m_pHorizontalScrollBar->SetScrollRange(0);
				SetPos(m_rcItem);
			}
			else
			{
				RECT rcScrollBarPos = { rc.left, rc.bottom, rc.right, rc.bottom + m_pHorizontalScrollBar->GetFixedHeight() };
				m_pHorizontalScrollBar->SetPos(rcScrollBarPos);

				if (m_pHorizontalScrollBar->GetScrollRange() != cxScroll) 
				{
					int iScrollPos = m_pHorizontalScrollBar->GetScrollPos();
					m_pHorizontalScrollBar->SetScrollRange(::abs(cxScroll)); // if scrollpos>range then scrollpos=range
					if(iScrollPos > m_pHorizontalScrollBar->GetScrollPos()) 
					{
						SetPos(m_rcItem);
					}
				}
			}
			break;
		}

		while (m_pVerticalScrollBar)
		{
			// Scroll needed
			if (cyRequired > rc.bottom - rc.top && !m_pVerticalScrollBar->IsVisible()) 
			{
				m_pVerticalScrollBar->SetVisible(true);
				m_pVerticalScrollBar->SetScrollRange(cyRequired - (rc.bottom - rc.top));
				m_pVerticalScrollBar->SetScrollPos(0);
				SetPos(m_rcItem);
				break;
			}

			// No scrollbar required
			if (!m_pVerticalScrollBar->IsVisible()) break;

			// Scroll not needed anymore?
			int cyScroll = cyRequired - (rc.bottom - rc.top);
			if (cyScroll <= 0) 
			{
				m_pVerticalScrollBar->SetVisible(false);
				m_pVerticalScrollBar->SetScrollPos(0);
				m_pVerticalScrollBar->SetScrollRange(0);
				SetPos(m_rcItem);
				break;
			}

			RECT rcScrollBarPos = { rc.right, rc.top, rc.right + m_pVerticalScrollBar->GetFixedWidth(), rc.bottom };
			m_pVerticalScrollBar->SetPos(rcScrollBarPos);

			if (m_pVerticalScrollBar->GetScrollRange() != cyScroll)
			{
				int iScrollPos = m_pVerticalScrollBar->GetScrollPos();
				m_pVerticalScrollBar->SetScrollRange(::abs(cyScroll)); // if scrollpos>range then scrollpos=range
				if(iScrollPos > m_pVerticalScrollBar->GetScrollPos()) 
				{
					SetPos(m_rcItem);
				}
			}
			break;
		}
	}

	bool CContainerUI::SetSubControlText( LPCTSTR pstrSubControlName,LPCTSTR pstrText )
	{
		CControlUI* pSubControl=nullptr;
		pSubControl=this->FindSubControl(pstrSubControlName);
		if (pSubControl!=nullptr) {
			pSubControl->SetText(pstrText);
			return TRUE;
		}
		else return FALSE;
	}

	bool CContainerUI::SetSubControlFixedHeight( LPCTSTR pstrSubControlName,int cy )
	{
		CControlUI* pSubControl=nullptr;
		pSubControl=this->FindSubControl(pstrSubControlName);
		if (pSubControl!=nullptr) {
			pSubControl->SetFixedHeight(cy);
			return TRUE;
		}
		else return FALSE;
	}

	bool CContainerUI::SetSubControlFixedWdith( LPCTSTR pstrSubControlName,int cx )
	{
		CControlUI* pSubControl=nullptr;
		pSubControl=this->FindSubControl(pstrSubControlName);
		if (pSubControl!=nullptr) {
			pSubControl->SetFixedWidth(cx);
			return TRUE;
		}
		else return FALSE;
	}

	bool CContainerUI::SetSubControlUserData( LPCTSTR pstrSubControlName,LPCTSTR pstrText )
	{
		CControlUI* pSubControl=nullptr;
		pSubControl=this->FindSubControl(pstrSubControlName);
		if (pSubControl!=nullptr) {
			pSubControl->SetUserData(pstrText);
			return TRUE;
		}
		else return FALSE;
	}

	DuiLib::CDuiString CContainerUI::GetSubControlText( LPCTSTR pstrSubControlName )
	{
		CControlUI* pSubControl=nullptr;
		pSubControl=this->FindSubControl(pstrSubControlName);
		if (pSubControl==nullptr)
			return _T("");
		else
			return pSubControl->GetText();
	}

	int CContainerUI::GetSubControlFixedHeight( LPCTSTR pstrSubControlName )
	{
		CControlUI* pSubControl=nullptr;
		pSubControl=this->FindSubControl(pstrSubControlName);
		if (pSubControl==nullptr) return -1;
		else return pSubControl->GetFixedHeight();
	}

	int CContainerUI::GetSubControlFixedWdith( LPCTSTR pstrSubControlName )
	{
		CControlUI* pSubControl=nullptr;
		pSubControl=this->FindSubControl(pstrSubControlName);
		if (pSubControl==nullptr) return -1;
		else return pSubControl->GetFixedWidth();
	}

	const CDuiString CContainerUI::GetSubControlUserData( LPCTSTR pstrSubControlName )
	{
		CControlUI* pSubControl=nullptr;
		pSubControl=this->FindSubControl(pstrSubControlName);
		if (pSubControl==nullptr) return _T("");
		else return pSubControl->GetUserData();
	}

	CControlUI* CContainerUI::FindSubControl( LPCTSTR pstrSubControlName )
	{
		CPaintManagerUI* pManager = GetManager();
		if (pManager == nullptr || pstrSubControlName == nullptr) return nullptr;
		return static_cast<CControlUI*>(pManager->FindSubControlByName(this, pstrSubControlName));
	}

} // namespace DuiLib
