#ifndef __UIFLASH_H__
#define __UIFLASH_H__
#pragma once

// \Utils\Flash11.tlb 为Flash11接口文件，部分方法在低版本不存在，使用需注意
// #import "PROGID:ShockwaveFlash.ShockwaveFlash"  \
//      raw_interfaces_only,       /* Don't add raw_ to method names */ \
//  	named_guids,           /* Named guids and declspecs */    \
//  	rename("IDispatchEx","IMyDispatchEx")	/* fix conflicting with IDispatchEx ant dispex.h */  
// using namespace ShockwaveFlashObjects;
#include "Utils/FlashEventHandler.h"
#include "Utils/flash11.tlh"

class CActiveXCtrl;

namespace DuiLib
{
	// [EXPORT-4] Member-level export. `class UILIB_API` exported all 16 private COM /
	// ActiveX members below (QueryInterface/AddRef/Release/GetTypeInfo/FSCommand).
	// Those are vtable slots the compiler fills in inside this DLL -- a vtable does
	// not need the symbol exported to point at it, so they were never public surface.
	class CFlashUI
		: public CActiveXUI
	//	, public IOleInPlaceSiteWindowless // 透明模式绘图，需要实现这个接口
		, public _IShockwaveFlashEvents
		, public ITranslateAccelerator
	{
		// Declared explicitly instead of DECLARE_DUICONTROL: the macro emits an unmarked
		// CreateControl(), which would lose its export now that the class is no longer
		// `class UILIB_API` -- and ControlFactory calls it across the DLL boundary.
	public:
		UILIB_API static CControlUI* CreateControl();
	public:
		UILIB_API CFlashUI(void);
		UILIB_API ~CFlashUI(void);

		UILIB_API void SetFlashEventHandler(CFlashEventHandler* pHandler);
		UILIB_API virtual bool DoCreateControl();
		IShockwaveFlash* m_pFlash;

	private:
		virtual LPCTSTR GetClass() const;
		virtual LPVOID GetInterface( LPCTSTR pstrName );

		virtual HRESULT STDMETHODCALLTYPE GetTypeInfoCount( __RPC__out UINT *pctinfo );
		virtual HRESULT STDMETHODCALLTYPE GetTypeInfo( UINT iTInfo, LCID lcid, __RPC__deref_out_opt ITypeInfo **ppTInfo );
		virtual HRESULT STDMETHODCALLTYPE GetIDsOfNames( __RPC__in REFIID riid, __RPC__in_ecount_full(cNames ) LPOLESTR *rgszNames, UINT cNames, LCID lcid, __RPC__out_ecount_full(cNames) DISPID *rgDispId);
		virtual HRESULT STDMETHODCALLTYPE Invoke( DISPID dispIdMember, REFIID riid, LCID lcid, WORD wFlags, DISPPARAMS *pDispParams, VARIANT *pVarResult, EXCEPINFO *pExcepInfo, UINT *puArgErr );

		virtual HRESULT STDMETHODCALLTYPE QueryInterface( REFIID riid, void **ppvObject );
		virtual ULONG STDMETHODCALLTYPE AddRef( void );
		virtual ULONG STDMETHODCALLTYPE Release( void );

		HRESULT OnReadyStateChange (long newState);
		HRESULT OnProgress(long percentDone );
		HRESULT FSCommand (_bstr_t command, _bstr_t args);
		HRESULT FlashCall (_bstr_t request );

		virtual void ReleaseControl();
		HRESULT RegisterEventHandler(BOOL inAdvise);

		// ITranslateAccelerator
		// Duilib消息分发给WebBrowser
		virtual LRESULT TranslateAccelerator( MSG *pMsg );

	private:
		LONG m_dwRef;
		DWORD m_dwCookie;
		CFlashEventHandler* m_pFlashEventHandler;
	};
}

#endif // __UIFLASH_H__
