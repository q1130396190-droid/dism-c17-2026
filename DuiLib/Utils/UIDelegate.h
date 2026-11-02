#ifndef __UIDELEGATE_H__
#define __UIDELEGATE_H__

#pragma once

namespace DuiLib {

class UILIB_API CDelegateBase	 
{
public:
    // S2/T2.5: 函数地址指纹改为 uintptr_t，消除 void* 与成员函数指针之间的双关。
    // 指纹只用于 Equals() 相等比较；调用始终由派生类用类型正确的成员指针完成。
    // 原 void* 构造保留为兼容重载（既有代码可继续编译）。
    CDelegateBase(void* pObject, uintptr_t uFn);
    CDelegateBase(void* pObject, void* pFn);
    CDelegateBase(const CDelegateBase& rhs);
    virtual ~CDelegateBase();
    bool Equals(const CDelegateBase& rhs) const;
    bool operator() (void* param);
    virtual CDelegateBase* Copy() const = 0; // add const for gcc

protected:
    virtual uintptr_t GetFn() const;
    void* GetObject();
    virtual bool Invoke(void* param) = 0;

private:
    void* m_pObject;
    uintptr_t m_uFn;
};

class CDelegateStatic: public CDelegateBase
{
    using Fn = bool (*)(void*);
public:
    CDelegateStatic(Fn pFn) : CDelegateBase(nullptr, reinterpret_cast<uintptr_t>(pFn)), m_pFn(pFn) { } 
    CDelegateStatic(const CDelegateStatic& rhs) : CDelegateBase(rhs), m_pFn(rhs.m_pFn) { } 
    virtual CDelegateBase* Copy() const { return new CDelegateStatic(*this); }

protected:
    // S2/T2.5: 调用直接使用类型正确的 m_pFn，不再从 GetFn() 反解函数指针
    virtual bool Invoke(void* param)
    {
        return (*m_pFn)(param); 
    }

private:
    Fn m_pFn;
};

template <class O, class T>
class CDelegate : public CDelegateBase
{
    using Fn = bool (T::*)(void*);
public:
    CDelegate(O* pObj, Fn pFn) : CDelegateBase(pObj, FnFingerprint(pFn)), m_pFn(pFn) { }
    CDelegate(const CDelegate& rhs) : CDelegateBase(rhs) { m_pFn = rhs.m_pFn; } 
    virtual CDelegateBase* Copy() const { return new CDelegate(*this); }
    // S2/T2.5: 不再用 *(void**)&m_pFn 做指针双关；改为在模板内计算可比较的指纹
    virtual uintptr_t GetFn() const { return FnFingerprint(m_pFn); }

protected:
    virtual bool Invoke(void* param)
    {
        O* pObject = (O*) GetObject();
        return (pObject->*m_pFn)(param); 
    }

private:
    // 计算成员函数指针的可比较指纹。成员指针不能安全转 uintptr_t，
    // 因此按字节折叠 —— 仅用于相等比较，不参与调用。
    static uintptr_t FnFingerprint(Fn pFn)
    {
        uintptr_t h = 1469598103934665603ull;   // FNV-1a 64 偏移基数
        const unsigned char* p = reinterpret_cast<const unsigned char*>(&pFn);
        for (size_t i = 0; i < sizeof(Fn); ++i) {
            h ^= static_cast<uintptr_t>(p[i]);
            h *= 1099511628211ull;              // FNV-1a 素数
        }
        return h;
    }

    // 说明：原实现把 GetFn() 放在 private 之上、此处同样保持可覆盖


private:
    Fn m_pFn;
};


template <class O, class T>
CDelegate<O, T> MakeDelegate(O* pObject, bool (T::* pFn)(void*))
{
    return CDelegate<O, T>(pObject, pFn);
}

inline CDelegateStatic MakeDelegate(bool (*pFn)(void*))
{
    return CDelegateStatic(pFn); 
}

class UILIB_API CEventSource
{
    using FnType = bool (*)(void*);
public:
    ~CEventSource();
    explicit operator bool() const noexcept;
    void operator+= (const CDelegateBase& d); // add const for gcc
    void operator+= (FnType pFn);
    void operator-= (const CDelegateBase& d);
    void operator-= (FnType pFn);
    bool operator() (void* param);
	void Clear();

protected:
    CStdPtrArray m_aDelegates;
};

} // namespace DuiLib

#endif // __UIDELEGATE_H__