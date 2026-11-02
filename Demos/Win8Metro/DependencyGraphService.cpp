#include "stdafx.h"

#include "DependencyGraphService.h"
#include "StartupService.h"
#include "ExternalToolService.h"
#include "ToolDependencyTable.h"

#include <cstdio>

// ============================================================
// DependencyGraphService.cpp —— [R-B25] 依赖图数据采集与树构建
//
// 只做数据侧；UI 渲染在 CToolDialogPage::BuildDepGraph（EToolDlgKind::TD_DEPGRAPH）。
// ============================================================

// ------------------------------------------------------------
// Compose：把"根标签 + 子标签列表"压成 DepGraphData（两层的依赖树）
// ------------------------------------------------------------
void DependencyGraphService::Compose(const CDuiString& sRoot,
                                     const std::vector<CDuiString>& vChildren,
                                     DepGraphData& out)
{
    out.vNodes.clear();
    out.nErr = 0;

    if (sRoot.IsEmpty()) return;

    DepGraphNode root;
    root.sLabel  = sRoot;
    root.sKind   = L"root";
    root.nParent = -1;
    root.nLevel  = 0;
    out.vNodes.push_back(root);

    for (size_t i = 0; i < vChildren.size(); ++i)
    {
        if (vChildren[i].IsEmpty()) continue;
        DepGraphNode n;
        n.sLabel  = vChildren[i];
        n.sKind   = L"dep";
        n.nParent = 0;
        n.nLevel  = 1;
        out.vNodes.push_back(n);
    }
}

// ------------------------------------------------------------
// BuildAppxGraph：输入包全名 + 各自依赖列表，构建"包 → 依赖包"两层图
// ------------------------------------------------------------
void DependencyGraphService::BuildAppxGraph(
        const std::vector<CDuiString>& vPkgs,
        const std::vector<CDuiString>& vDepLists,
        DepGraphData& out)
{
    out.vNodes.clear();
    out.nErr = 0;

    size_t nCount = vPkgs.size();
    if (vDepLists.size() < nCount) nCount = vDepLists.size();

    for (size_t i = 0; i < nCount; ++i)
    {
        const CDuiString& sPkg = vPkgs[i];
        const CDuiString& sDep = vDepLists[i];
        if (sPkg.IsEmpty()) continue;

        // 根：包全名
        DepGraphNode root;
        root.sLabel  = sPkg;
        root.sKind   = L"appx";
        root.nParent = -1;
        root.nLevel  = 0;
        int nRootIdx = static_cast<int>(out.vNodes.size());
        out.vNodes.push_back(root);

        if (sDep.IsEmpty()) continue;

        // 依赖列表（逗号分隔）
        CDuiString sWork = sDep;
        int nPos = 0;
        while (true)
        {
            int nComma = sWork.Find(L",", nPos);
            CDuiString sOne;
            if (nComma < 0)
            {
                sOne = sWork.Mid(nPos);
                nPos = static_cast<int>(sWork.GetLength());
            }
            else
            {
                sOne = sWork.Mid(nPos, nComma - nPos);
                nPos = nComma + 1;
            }
            sOne.TrimLeft();
            sOne.TrimRight();
            if (sOne.IsEmpty())
            {
                if (nPos >= static_cast<int>(sWork.GetLength())) break;
                continue;
            }

            DepGraphNode n;
            n.sLabel  = sOne;
            n.sKind   = L"appxdep";
            n.nParent = nRootIdx;
            n.nLevel  = 1;
            out.vNodes.push_back(n);

            if (nPos >= static_cast<int>(sWork.GetLength())) break;
        }
    }
}

// ------------------------------------------------------------
// QueryServiceDependencies：SCM 正向依赖（该服务依赖哪些服务）
// 使用 QueryServiceConfig 的 lpDependencies（双 \0 分隔的服务名列表）。
// 注：SERVICE_CONFIG_DEPENDENCIES 不存在于 Windows SDK，服务前向依赖
//     只通过 lpDependencies 字段暴露。
// ------------------------------------------------------------
bool DependencyGraphService::QueryServiceDependencies(
        const wchar_t* pszSvcName,
        std::vector<CDuiString>& vDepServices,
        DWORD& dwErrOut)
{
    vDepServices.clear();
    dwErrOut = 0;

    SC_HANDLE hScm = ::OpenSCManager(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!hScm) { dwErrOut = ::GetLastError(); return false; }

    SC_HANDLE hSvc = ::OpenService(hScm, pszSvcName, SERVICE_QUERY_CONFIG);
    if (!hSvc)
    {
        dwErrOut = ::GetLastError();
        ::CloseServiceHandle(hScm);
        return false;
    }

    // 第一次取长度
    DWORD dwNeed = 0;
    ::QueryServiceConfig(hSvc, nullptr, 0, &dwNeed);
    if (dwNeed == 0)
    {
        dwErrOut = ::GetLastError();
        ::CloseServiceHandle(hSvc);
        ::CloseServiceHandle(hScm);
        return true;  // 无配置属正常
    }

    std::vector<BYTE> vBuf(dwNeed);
    if (!::QueryServiceConfig(hSvc,
                              reinterpret_cast<LPQUERY_SERVICE_CONFIG>(vBuf.data()),
                              dwNeed, &dwNeed))
    {
        dwErrOut = ::GetLastError();
        ::CloseServiceHandle(hSvc);
        ::CloseServiceHandle(hScm);
        return false;
    }

    LPQUERY_SERVICE_CONFIG pCfg =
        reinterpret_cast<LPQUERY_SERVICE_CONFIG>(vBuf.data());
    if (!pCfg || !pCfg->lpDependencies) return true;   // 无依赖

    // 格式：以 \0\0 结尾的双 \0 分隔字符串数组
    const wchar_t* psz = pCfg->lpDependencies;
    while (psz && *psz)
    {
        CDuiString s = psz;
        if (!s.IsEmpty()) vDepServices.push_back(s);
        psz += s.GetLength() + 1;
    }

    ::CloseServiceHandle(hSvc);
    ::CloseServiceHandle(hScm);
    return true;
}

// ------------------------------------------------------------
// EnumerateStartupDependents：反向依赖（哪些服务依赖该启动项）
// 直接复用 StartupService::EnumerateDependentServices。
// ------------------------------------------------------------
bool DependencyGraphService::EnumerateStartupDependents(
        const wchar_t* pszSvcName,
        std::vector<CDuiString>& vDepNames,
        std::vector<CDuiString>& vDepDisplay,
        DWORD& dwErrOut)
{
    vDepNames.clear();
    vDepDisplay.clear();

    std::vector<DepServiceEntry> vDep;
    int nRet = StartupService::EnumerateDependentServices(
                    pszSvcName, vDep, dwErrOut);
    if (nRet != 0 && nRet != 3)  // 3 = 无依赖，也算成功
        return false;

    for (size_t i = 0; i < vDep.size(); ++i)
    {
        vDepNames.push_back(vDep[i].sName);
        vDepDisplay.push_back(vDep[i].sDisplayName);
    }
    return true;
}

// ------------------------------------------------------------
// ToolDependencyAvailable：工具 nKind 依赖的 exe 是否存在
// ------------------------------------------------------------
bool DependencyGraphService::ToolDependencyAvailable(
        int nKind, CDuiString& sDepExeOut)
{
    const wchar_t* pszDep = ToolDependsForKind(nKind);
    if (!pszDep)
    {
        sDepExeOut.Empty();
        return false;
    }
    // [R01] 空串 = 无外部 exe 依赖（kind 11 激活备份走卫星 DLL 软依赖，
    // 可用性由 ActivationKitClient::IsReady 判定），此处按可用处理。
    if (pszDep[0] == L'\0')
    {
        sDepExeOut.Empty();
        return true;
    }
    sDepExeOut = pszDep;
    return ExternalToolService::IsToolAvailable(pszDep);
}
// [C++17 R3] 本文件内联常量（C++17 inline variables，多 TU ODR 一致）：
