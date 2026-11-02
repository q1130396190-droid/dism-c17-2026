// ============================================================================
//  CBSHost_exports.cpp
//
//  CBSHost.dll 的 6 个官方导出（C ABI），内部转发到
//  Demos\Win8Metro\CbsHostBridge.cpp 的 CbsHost 命名空间语义等价实现。
//
//  导出名（知识库《18_CBSHost_100%深度解析与复刻蓝图》2.1 节，序号 1-6）：
//    1. CbsCreateTempDirectory2
//    2. CreateCbsHostHelper
//    3. DismGetScratchDir
//    4. DismWriteLog
//    5. GetConfig
//    6. RunCbsHostW
//
//  说明：原版各导出的精确 C 原型未随 DLL 提供头文件，以下签名按知识库
//  反编译语义设计，x64 下统一 Windows ABI；所有字符串均为 UTF-16。
//
//  [BUG-07] 每个导出都是 SEH 异常边界：CBSHost.dll 为 /MT 静态 CRT，宿主
//  为 /MD，C++ 异常/AV 不允许穿过 C ABI（跨 CRT 未捕获 = terminate）。
//  含 STL 对象的实现全部在 *_Impl 内，导出包装体无析构对象（C2712）。
// ============================================================================

#include "cbshost_stdafx.h"

#include "CBSHostApi.h"
#include "CbsHostBridge.h"

#include <cstdarg>

namespace
{

void CopyWStr(const std::wstring& sSrc, wchar_t* pszOut, DWORD cchOut)
{
    if (!pszOut || cchOut == 0) return;
    DWORD nCopy = static_cast<DWORD>(sSrc.size());
    if (nCopy >= cchOut) nCopy = cchOut - 1;
    ::wmemcpy(pszOut, sSrc.c_str(), nCopy);
    pszOut[nCopy] = L'\0';
}

// ---- *_Impl：允许使用 C++ 对象的语义实现 ----

BOOL CbsCreateTempDirectory2_Impl(wchar_t* pszOut, DWORD cchOut, DWORD* pdwErr)
{
    std::wstring sDir;
    if (!CbsHost::CbsCreateTempDirectory(&sDir, pdwErr))
        return FALSE;

    CopyWStr(sDir, pszOut, cchOut);
    return TRUE;
}

void* CreateCbsHostHelper_Impl(void* pParam, unsigned char bMode)
{
    if (bMode)
    {
        // 模式 B：轻量
        return reinterpret_cast<void*>(CbsHost::CbsHostCreateHelper(nullptr, pParam, 0));
    }

    // 模式 A：完整对象
    CbsHost::Config cfg;   // 默认配置
    const CBS_CONFIG* pSrc = (const CBS_CONFIG*)pParam;
    if (pSrc)
    {
        cfg.sExeDir     = pSrc->sExeDir;
        cfg.sScratchDir = pSrc->sScratchDir;
        cfg.sLogDir     = pSrc->sLogDir;
        cfg.sMountRoot  = pSrc->sMountRoot;
        cfg.nLanguage   = pSrc->nLanguage;
        cfg.bNoviceMode = (pSrc->bNoviceMode != FALSE);
        cfg.bExpertMode = (pSrc->bExpertMode != FALSE);
    }
    return reinterpret_cast<void*>(CbsHost::CbsHostCreateHelper(&cfg, nullptr, 0));
}

BOOL DismGetScratchDir_Impl(wchar_t* pszOut, DWORD cchOut)
{
    std::wstring sDir;
    if (!CbsHost::DismGetScratchDir(&sDir))
        return FALSE;

    CopyWStr(sDir, pszOut, cchOut);
    return TRUE;
}

BOOL GetConfig_Impl(const wchar_t* pszIni, CBS_CONFIG* pCfg)
{
    if (!pCfg)
    {
        ::SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    std::wstring sIni = pszIni ? pszIni : L"";
    CbsHost::Config cfg;
    std::wstring sErr;
    if (!CbsHost::GetConfigFromIni(sIni, &cfg, &sErr))
    {
        ::SetLastError(ERROR_BAD_CONFIGURATION);
        return FALSE;
    }

    *pCfg = CBS_CONFIG{};   // [C++17] value-init aggregate replaces ZeroMemory
    CopyWStr(cfg.sExeDir,     pCfg->sExeDir,     _countof(pCfg->sExeDir));
    CopyWStr(cfg.sScratchDir, pCfg->sScratchDir, _countof(pCfg->sScratchDir));
    CopyWStr(cfg.sLogDir,     pCfg->sLogDir,     _countof(pCfg->sLogDir));
    CopyWStr(cfg.sMountRoot,  pCfg->sMountRoot,  _countof(pCfg->sMountRoot));
    pCfg->nLanguage   = cfg.nLanguage;
    pCfg->bNoviceMode = cfg.bNoviceMode ? TRUE : FALSE;
    pCfg->bExpertMode = cfg.bExpertMode ? TRUE : FALSE;
    return TRUE;
}

int RunCbsHostW_Impl(const wchar_t* pszCmdLine, wchar_t* pszErr, DWORD cchErr)
{
    std::wstring sErr;
    int nRet = CbsHost::RunCbsHostW(pszCmdLine ? pszCmdLine : L"", &sErr);

    if (pszErr && cchErr > 0)
        CopyWStr(sErr, pszErr, cchErr);
    return nRet;
}

} // namespace

// ----------------------------------------------------------------------------
// 1. CbsCreateTempDirectory2（RVA 0x24A8）
// ----------------------------------------------------------------------------

EXTERN_C BOOL WINAPI CbsCreateTempDirectory2(wchar_t* pszOut, DWORD cchOut,
                                             DWORD* pdwErr)
{
    __try
    {
        return CbsCreateTempDirectory2_Impl(pszOut, cchOut, pdwErr);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        if (pdwErr) *pdwErr = ERROR_PROCESS_ABORTED;
        return FALSE;
    }
}

// ----------------------------------------------------------------------------
// 2. CreateCbsHostHelper（RVA 0xBF08）
// ----------------------------------------------------------------------------

EXTERN_C void* WINAPI CreateCbsHostHelper(void* pParam, unsigned char bMode)
{
    __try
    {
        return CreateCbsHostHelper_Impl(pParam, bMode);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

// ----------------------------------------------------------------------------
// 3. DismGetScratchDir（RVA 0xC128）
// ----------------------------------------------------------------------------

EXTERN_C BOOL WINAPI DismGetScratchDir(wchar_t* pszOut, DWORD cchOut)
{
    __try
    {
        return DismGetScratchDir_Impl(pszOut, cchOut);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return FALSE;
    }
}

// ----------------------------------------------------------------------------
// 4. DismWriteLog（RVA 0xBFAC）
//    va_list 为 POD，包装体无析构对象，可直接置于 SEH 帧内。
// ----------------------------------------------------------------------------

EXTERN_C void WINAPI DismWriteLog(int nLevel, const wchar_t* pszFormat, ...)
{
    if (!pszFormat) return;
    va_list ap;
    va_start(ap, pszFormat);
    __try
    {
        CbsHost::DismWriteLogV(nLevel, pszFormat, ap);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        // 日志路径异常不允许杀进程
    }
    va_end(ap);
}

// ----------------------------------------------------------------------------
// 5. GetConfig（RVA 0xC120）
// ----------------------------------------------------------------------------

EXTERN_C BOOL WINAPI GetConfig(const wchar_t* pszIni, CBS_CONFIG* pCfg)
{
    __try
    {
        return GetConfig_Impl(pszIni, pCfg);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        ::SetLastError(ERROR_PROCESS_ABORTED);
        return FALSE;
    }
}

// ----------------------------------------------------------------------------
// 6. RunCbsHostW（RVA 0x9854）
// ----------------------------------------------------------------------------

EXTERN_C int WINAPI RunCbsHostW(const wchar_t* pszCmdLine, wchar_t* pszErr, DWORD cchErr)
{
    __try
    {
        return RunCbsHostW_Impl(pszCmdLine, pszErr, cchErr);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        if (pszErr && cchErr > 0) pszErr[0] = L'\0';
        return ERROR_PROCESS_ABORTED;
    }
}
// [C++17 R3] 本文件内联常量（C++17 inline variables，多 TU ODR 一致）：
inline constexpr int kCpp17_CBSHost_exports_StepTimeoutMs = 120000;
inline constexpr int kCpp17_CBSHost_exports_MaxRetry = 3;
