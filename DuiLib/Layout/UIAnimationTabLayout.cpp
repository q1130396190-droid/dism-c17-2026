#include "StdAfx.h"

#include "S3AttrRegistry.h"

#include "UIAnimationTabLayout.h"


// [T5.40] W4 batch-6: suppress data-truncation warnings (C4244/C4302).
// DuiLib sits at the GDI+/Win32 API boundary and relies on narrow types
// (int/BYTE/WORD) at call sites where value domains are provably bounded:
//   * LONG/LONG_PTR/UINT_PTR/WPARAM -> int  (UI coords/handles, int32 safe)
//   * LONG -> Gdiplus::REAL             (GDI+ API requires float coords)
//   * int  -> BYTE / float               (opacity / percentage within range)
//   * LPVOID -> int                     (CStdPtrArray int-as-pointer borrow,
//                                       see docs/t532-w4-b3-pointer-cast-audit.json)
//   * LPWSTR -> WORD/TCHAR              (16-bit unicode char truncation by design)
// Modernization path: migrate CStdPtrArray<LPVOID> to std::vector<int> /
// std::vector<COLORREF>, and use explicit width/height structs instead of
// mixing LONG with Gdiplus::REAL.  See docs/t538 and docs/t539 scans.
#pragma warning(disable: 4244 4302)
// [T5.40] end


namespace DuiLib {
	IMPLEMENT_DUICONTROL(CAnimationTabLayoutUI)

	CAnimationTabLayoutUI::CAnimationTabLayoutUI() : 
		m_bIsVerticalDirection( false ), 
		m_nPositiveDirection( 1 ),
		m_pCurrentControl( nullptr ),
		m_bControlVisibleFlag( false )
	{
		Attach(this);
	}

	LPCTSTR CAnimationTabLayoutUI::GetClass() const
	{
		return _T("AnimationTabLayoutUI");
	}

	LPVOID CAnimationTabLayoutUI::GetInterface(LPCTSTR pstrName)
	{
		if( _tcsicmp(pstrName, _T("AnimationTabLayout")) == 0 ) 
			return static_cast<CAnimationTabLayoutUI*>(this);
		return CTabLayoutUI::GetInterface(pstrName);
	}

	bool CAnimationTabLayoutUI::SelectItem( int iIndex )
	{
		if( iIndex < 0 || iIndex >= m_items.GetSize() ) return false;
		if( iIndex == m_iCurSel ) return true;
		if( iIndex > m_iCurSel ) m_nPositiveDirection = -1;
		if( iIndex < m_iCurSel ) m_nPositiveDirection = 1;

		int iOldSel = m_iCurSel;
		m_iCurSel = iIndex;
		for( int it = 0; it < m_items.GetSize(); it++ ) {
			CControlUI* pItem = GetItemAt(it);
			if( pItem == nullptr ) continue;
			if( it == iIndex ) {
				pItem->SetVisible(true);
				pItem->SetFocus();
				m_bControlVisibleFlag = false;
				m_pCurrentControl = static_cast<CControlUI*>(m_items[it]);
				
			}
			else pItem->SetVisible(false);
		}

		NeedParentUpdate();
		if( nullptr != m_pCurrentControl ) m_pCurrentControl->SetVisible( false );
		AnimationSwitch();

		if( m_pManager != nullptr ) {
			m_pManager->SetNextTabControl();
			m_pManager->SendNotify(this, _T("tabselect"), m_iCurSel, iOldSel);
		}
		return true;
	}

	void CAnimationTabLayoutUI::AnimationSwitch()
	{
		m_rcItemOld = m_rcItem;
		if( !m_bIsVerticalDirection )
		{
			m_rcCurPos.top = m_rcItem.top;
			m_rcCurPos.bottom = m_rcItem.bottom;
			m_rcCurPos.left = m_rcItem.left - ( m_rcItem.right - m_rcItem.left ) * m_nPositiveDirection + 52 * m_nPositiveDirection;
			m_rcCurPos.right = m_rcItem.right - ( m_rcItem.right - m_rcItem.left ) * m_nPositiveDirection+ 52 * m_nPositiveDirection;		
		}
		else
		{
			m_rcCurPos.left = m_rcItem.left;
			m_rcCurPos.right = m_rcItem.right;
			m_rcCurPos.top = m_rcItem.top - ( m_rcItem.bottom - m_rcItem.top ) * m_nPositiveDirection;
			m_rcCurPos.bottom = m_rcItem.bottom - ( m_rcItem.bottom - m_rcItem.top ) * m_nPositiveDirection;		
		}

		StopAnimation( TAB_ANIMATION_ID );
		StartAnimation( TAB_ANIMATION_ELLAPSE, TAB_ANIMATION_FRAME_COUNT, TAB_ANIMATION_ID );
	}

	void CAnimationTabLayoutUI::DoEvent(TEventUI& event)
	{
		if( event.Type == UIEVENT_TIMER ) 
		{
			OnTimer(  event.wParam );
		}
		__super::DoEvent( event );
	}

	void CAnimationTabLayoutUI::OnTimer( int nTimerID )
	{
		OnAnimationElapse( nTimerID );
	}

	void CAnimationTabLayoutUI::OnAnimationStep(INT nTotalFrame, INT nCurFrame, INT nAnimationID)
	{
		if( !m_bControlVisibleFlag ) {
			m_bControlVisibleFlag = true;
			m_pCurrentControl->SetVisible( true );
		}

		int iStepLen = 0;
		if( !m_bIsVerticalDirection )
		{
			iStepLen = ( m_rcItemOld.right - m_rcItemOld.left ) * m_nPositiveDirection / nTotalFrame;
			if( nCurFrame != nTotalFrame )
			{
				m_rcCurPos.left = m_rcCurPos.left + iStepLen;
				m_rcCurPos.right = m_rcCurPos.right +iStepLen;			
			}
			else
			{
				m_rcItem = m_rcCurPos = m_rcItemOld;
			}
		}
		else
		{
			iStepLen = ( m_rcItemOld.bottom - m_rcItemOld.top ) * m_nPositiveDirection / nTotalFrame;
			if( nCurFrame != nTotalFrame )
			{
				m_rcCurPos.top = m_rcCurPos.top + iStepLen;
				m_rcCurPos.bottom = m_rcCurPos.bottom +iStepLen;			
			}
			else
			{
				m_rcItem = m_rcCurPos = m_rcItemOld;	
			}	
		}
		SetPos(m_rcCurPos);
	}

	void CAnimationTabLayoutUI::OnAnimationStop(INT nAnimationID) 
	{
		SetPos(m_rcItemOld);
		NeedParentUpdate();
	}

	void CAnimationTabLayoutUI::SetAttribute(LPCTSTR pstrName, LPCTSTR pstrValue)
	{
		// ---- S3/T3.8 batch 2: fast path via compile-time hash registry ----
		// O(log n) lookup instead of O(n) string compares.
		// Falls through to the legacy chain below when unmatched, so
		// third-party custom attributes keep working unchanged (D3 open).
		switch( DuiLib::s3::lookup_prop_t(pstrName) ) {
		case DuiLib::s3::PropId::animation_direction: { if( _tcsicmp(pstrValue, _T("vertical")) == 0 ) m_bIsVerticalDirection = true; return; }
		default: break;
		}

		// ---- legacy fallback chain (kept verbatim for compatibility) ----

		if( _tcsicmp(pstrName, _T("animation_direction")) == 0 && _tcsicmp( pstrValue, _T("vertical")) == 0 ) m_bIsVerticalDirection = true; // pstrValue = "vertical" or "horizontal"
		return CTabLayoutUI::SetAttribute(pstrName, pstrValue);
	}
} // namespace DuiLib