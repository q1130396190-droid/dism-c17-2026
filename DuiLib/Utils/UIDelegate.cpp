#include "StdAfx.h"

namespace DuiLib {

CDelegateBase::CDelegateBase(void* pObject, uintptr_t uFn) 
{
    m_pObject = pObject;
    m_uFn = uFn; 
}

CDelegateBase::CDelegateBase(void* pObject, void* pFn) 
{
    m_pObject = pObject;
    m_uFn = reinterpret_cast<uintptr_t>(pFn); 
}

CDelegateBase::CDelegateBase(const CDelegateBase& rhs) 
{
    m_pObject = rhs.m_pObject;
    m_uFn = rhs.m_uFn; 
}

CDelegateBase::~CDelegateBase()
{

}

bool CDelegateBase::Equals(const CDelegateBase& rhs) const 
{    
    return m_pObject == rhs.m_pObject && GetFn() == rhs.GetFn();
}

bool CDelegateBase::operator() (void* param) 
{
    return Invoke(param); 
}

uintptr_t CDelegateBase::GetFn() const
{
    return m_uFn; 
}

void* CDelegateBase::GetObject() 
{
    return m_pObject; 
}

CEventSource::~CEventSource()
{
    for( LPVOID pRaw : m_aDelegates ) {
        CDelegateBase* pObject = static_cast<CDelegateBase*>(pRaw);
        if( pObject) delete pObject;
    }
}

CEventSource::operator bool() const noexcept
{
    return m_aDelegates.GetSize() > 0;
}

void CEventSource::operator+= (const CDelegateBase& d)
{ 
    for( LPVOID pRaw : m_aDelegates ) {
        CDelegateBase* pObject = static_cast<CDelegateBase*>(pRaw);
        if( pObject && pObject->Equals(d) ) return;
    }

    m_aDelegates.Add(d.Copy());
}

void CEventSource::operator+= (FnType pFn)
{ 
    (*this) += MakeDelegate(pFn);
}

void CEventSource::operator-= (const CDelegateBase& d) 
{
    for( int i = 0; i < m_aDelegates.GetSize(); i++ ) {
        CDelegateBase* pObject = static_cast<CDelegateBase*>(m_aDelegates[i]);
        if( pObject && pObject->Equals(d) ) {
            delete pObject;
            m_aDelegates.Remove(i);
            return;
        }
    }
}
void CEventSource::operator-= (FnType pFn)
{ 
    (*this) -= MakeDelegate(pFn);
}

bool CEventSource::operator() (void* param) 
{
    for( LPVOID pRaw : m_aDelegates ) {
        CDelegateBase* pObject = static_cast<CDelegateBase*>(pRaw);
        if( pObject && !(*pObject)(param) ) return false;
    }
    return true;
}
void CEventSource::Clear()
{
	for( LPVOID pRaw : m_aDelegates ) {
		CDelegateBase* pObject = static_cast<CDelegateBase*>(pRaw);
		if( pObject) delete pObject;
	}
	m_aDelegates.Empty();
}
} // namespace DuiLib
