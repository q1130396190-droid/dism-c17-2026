#pragma once

// ============================================================
// Page_UnattendInternal.h —— 应答文件页跨翻译单元共享的 RAII 守卫。
//
// [R139-2] Page_Unattend.cpp 原 2393 行，拆出 Page_UnattendIbs.cpp 后，
// 两个 .cpp 都需要 FileGuard / HandleGuard（原为主文件内定义）。
// 提为本头的共享定义，避免各留一份。
//
// 说明：FileGuard 由 Page_Unattend.cpp 多处使用；HandleGuard 目前仅
// Ibs 子系统的提权/注册表路径使用。二者均为 Win32 句柄所有权契约，
// 不是页面业务逻辑，故适合放在内部头中。
// ============================================================

#include "stdafx.h"

#include <windows.h>

// 句柄有效性判定（INVALID_HANDLE_VALUE 在部分 SDK 配置下非编译期常量，
// 故以 inline 函数而非 constexpr 常量表达）。
[[nodiscard]] inline bool IsGuardHandleValid(HANDLE h) noexcept
{
    return h != nullptr && h != INVALID_HANDLE_VALUE;
}

// 文件/设备句柄 RAII：析构即关闭，禁拷贝。
struct FileGuard
{
    FileGuard() = default;
    FileGuard(HANDLE h) : m_h(h) {}   // 非 explicit：调用点用拷贝初始化 FileGuard h = CreateFileW(...)
    ~FileGuard() { if (m_h != nullptr && m_h != INVALID_HANDLE_VALUE) ::CloseHandle(m_h); }
    FileGuard(const FileGuard&) = delete;
    FileGuard& operator=(const FileGuard&) = delete;
    operator HANDLE() const { return m_h; }
private:
    HANDLE m_h = nullptr;
};

// 通用句柄 RAII：额外暴露 out() 供 WinAPI 输出参数直接写入。
struct HandleGuard
{
    HandleGuard() = default;
    ~HandleGuard() { if (m_h != nullptr && m_h != INVALID_HANDLE_VALUE) ::CloseHandle(m_h); }
    HandleGuard(const HandleGuard&) = delete;
    HandleGuard& operator=(const HandleGuard&) = delete;
    operator HANDLE() const { return m_h; }
    HANDLE* out() { return &m_h; }
private:
    HANDLE m_h = nullptr;
};


// ============================================================
// [R149] MSXML DOM 基础层 + unattend.xml pass 映射（跨 TU 共享）
//
// 拆出 Page_UnattendXml.cpp（DOM 层 + 导入导出族）后，下列符号被
// 两个 TU 同时使用，故逐字提升为 inline 单一真源：
//   · UcAutoWrap          被 ApplyUnattend 直接调用
//   · UcDomCreate         同上
//   · UcDomParseErrorCode 被 UcDomLoadXmlString 调用（随其共享）
//   · UcDomLoadXmlString  被 ApplyUnattend 直接调用
//   · kUcMap              ApplyUnattend 与 CopyRowPass 都要取 pass 名
//
// 其余 DOM 辅助（LoadFile / SelectNodes / ListCount / ListItem /
// GetNodeText / GetNodeAttr / SchemaCheck）段外零引用，
// 随段迁入新 TU 并以 static 保留，不进本头。
//
// 搬迁为逐字搬运，仅 static → inline（头内需避免每 TU 一份副本）。
// ============================================================

#include <objbase.h>
#include <oleauto.h>
#include <comdef.h>

#include <string>
#include <vector>

// 简化版 IDispatch::Invoke 封装
inline HRESULT UcAutoWrap(DWORD dwFlags, VARIANT* pvResult, IDispatch* pDisp,
                          LPOLESTR ptName, int cArgs, VARIANT* pArgs)
{
    if (!pDisp) return E_POINTER;
    DISPID dispid = 0;
    OLECHAR* pNames[1] = { ptName };
    HRESULT hr = pDisp->GetIDsOfNames(IID_NULL, pNames, 1,
                                      LOCALE_USER_DEFAULT, &dispid);
    if (FAILED(hr)) return hr;
    DISPPARAMS dp = { nullptr, nullptr, 0, 0 };
    VARIANT vResult;
    ::VariantInit(&vResult);
    // [C++17] 纯局部配对：参数数组由 vector 作用域自动释放（与原 delete[] 等价）
    std::vector<VARIANT> vArgs;
    if (cArgs > 0)
    {
        dp.cArgs = cArgs;
        vArgs.resize(static_cast<size_t>(cArgs));
        dp.rgvarg = vArgs.data();
        for (int i = 0; i < cArgs; ++i)
            dp.rgvarg[i] = pArgs[cArgs - 1 - i];
    }
    hr = pDisp->Invoke(dispid, IID_NULL, static_cast<WORD>(LOCALE_USER_DEFAULT),
                       static_cast<WORD>(dwFlags), &dp, &vResult, nullptr, nullptr);
    if (SUCCEEDED(hr) && pvResult)
        ::VariantCopy(pvResult, &vResult);
    ::VariantClear(&vResult);
    return hr;
}

// 创建 Msxml2.DOMDocument.6.0 对象
inline int UcDomCreate(IDispatch** ppDisp)
{
    if (!ppDisp) return 3;
    *ppDisp = nullptr;
    CLSID clsid = { 0 };
    if (FAILED(::CLSIDFromProgID(L"Msxml2.DOMDocument.6.0", &clsid)))
        return 2;
    HRESULT hr = ::CoCreateInstance(clsid, nullptr, CLSCTX_ALL, IID_IDispatch,
                                    (void**)ppDisp);
    if (FAILED(hr) || !*ppDisp) return 3;
    // 关闭异步解析 / 不解析外部实体
    VARIANT vFalse;
    ::VariantInit(&vFalse);
    vFalse.vt = VT_BOOL;
    vFalse.boolVal = VARIANT_FALSE;
    ::UcAutoWrap(DISPATCH_PROPERTYPUT, nullptr, *ppDisp, L"async", 1, &vFalse);
    ::UcAutoWrap(DISPATCH_PROPERTYPUT, nullptr, *ppDisp,
                 L"resolveExternals", 1, &vFalse);
    ::UcAutoWrap(DISPATCH_PROPERTYPUT, nullptr, *ppDisp,
                 L"validateOnParse", 1, &vFalse);
    ::VariantClear(&vFalse);
    return 0;
}

// 读取 DOM 节点的 parseError.code（0 表示无错误）
inline int UcDomParseErrorCode(IDispatch* pDoc, CDuiString& sMsg)
{
    if (!pDoc) return -1;
    VARIANT vErr;
    ::VariantInit(&vErr);
    HRESULT hr = ::UcAutoWrap(DISPATCH_PROPERTYGET, &vErr, pDoc,
                              L"parseError", 0, nullptr);
    if (FAILED(hr) || vErr.vt != VT_DISPATCH || !vErr.pdispVal)
    {
        ::VariantClear(&vErr);
        return -1;
    }
    IDispatch* pPE = vErr.pdispVal;
    VARIANT vCode;
    ::VariantInit(&vCode);
    hr = ::UcAutoWrap(DISPATCH_PROPERTYGET, &vCode, pPE,
                      L"errorCode", 0, nullptr);
    long lCode = -1;
    if (SUCCEEDED(hr) && (vCode.vt == VT_I4 || vCode.vt == VT_I2))
        lCode = (vCode.vt == VT_I4) ? vCode.lVal : vCode.iVal;
    ::VariantClear(&vCode);
    VARIANT vReason;
    ::VariantInit(&vReason);
    if (SUCCEEDED(::UcAutoWrap(DISPATCH_PROPERTYGET, &vReason, pPE,
                               L"reason", 0, nullptr)) && vReason.vt == VT_BSTR)
    {
        sMsg = (const wchar_t*)(_bstr_t(vReason.bstrVal, false));
    }
    ::VariantClear(&vReason);
    ::VariantClear(&vErr);
    return static_cast<int>(lCode);
}

// 用字符串解析 XML（返回 0 成功；4=loadXML 失败，5=parseError 非零）
inline int UcDomLoadXmlString(IDispatch* pDoc, const wchar_t* pszXml,
                              CDuiString& sErrMsg)
{
    VARIANT vArg;
    ::VariantInit(&vArg);
    vArg.vt = VT_BSTR;
    vArg.bstrVal = ::SysAllocString(pszXml ? pszXml : L"");
    VARIANT vResult;
    ::VariantInit(&vResult);
    HRESULT hr = ::UcAutoWrap(DISPATCH_METHOD, &vResult, pDoc,
                              L"loadXML", 1, &vArg);
    ::VariantClear(&vResult);
    ::VariantClear(&vArg);
    if (FAILED(hr)) return 4;
    CDuiString sTmp;
    int nErr = ::UcDomParseErrorCode(pDoc, sTmp);
    if (nErr != 0 && nErr != -1)
    {
        sErrMsg = sTmp;
        return 5;
    }
    return 0;
}


// 4 个分组 → unattend.xml 的 pass + component 映射
// [R18 P0 修正] pszName 为真实 pass 名（仅用于导出 XML 的 Description）
// [R31 修正 P0-1] 第 4 组从 generalize → auditSystem（MSDN 定义，generalize pass 不存在）
// 原为文件内匿名 struct 数组，提升后命名为 UcPassMapEntry 以便跨 TU 引用。
struct UcPassMapEntry
{
    const wchar_t* pszPass;
    const wchar_t* pszComp;
    const wchar_t* pszName;  // pass 名, 用于 Description
};

inline constexpr UcPassMapEntry kUcMap[4] = {
    { L"windowsPE",   L"Microsoft-Windows-Setup",     L"windowsPE" },
    { L"specialize",  L"Microsoft-Windows-Deployment",L"specialize" },
    { L"oobeSystem",  L"Microsoft-Windows-Deployment",L"oobeSystem" },
    { L"auditSystem", L"Microsoft-Windows-Deployment",L"auditSystem" },
};

inline constexpr int kUcGroupCount = 4;   // kUcMap 长度（原为字面量 4 散落各处）

// ============================================================
// [R99-RECOVER] 管理员权限断言（跨 TU 共享）
//
// 原为 Page_Unattend.cpp 匿名命名空间内的页内私有函数，R99 把
// RunSysprep / BuildIBSReport 拆到 Page_UnattendIbs_Report.cpp 后，
// 匿名命名空间不可跨 TU 引用（C3861），故提升为 inline 单一真源。
// 函数体逐字保留，未改判定顺序与提示文案。
// ============================================================

#include "ExternalToolService.h"   // IsElevated
#include "StringManager.h"

// 管理员权限断言（HostInterfaces 无此能力，页内等价实现）
inline bool RequireUnattendAdmin(IPageContext& ctx, const wchar_t* pszAction)
{
    if (ExternalToolService::IsElevated()) return true;
    CDuiString sMsg;
    sMsg.Format(StringMgr.Get("misc.sdb6ef517"), pszAction);
    ctx.HostNotify(StringMgr.Get("misc.sd6e4b309"), sMsg.GetData(), 2);
    ctx.HostLog(StringMgr.Get("misc.sdb6ef517"), static_cast<int>(ELogLevel::LOG_WARN));
    return false;
}
