#pragma once

// ============================================================
// DependencyGraphService.h —— [R-B25] 依赖图数据采集与树构建
//
// 评分标准 §7.3「无依赖图可视化 -1.5 分」要求 Driver/Capability/Appx/
// Feature/Tools/Service/Startup/Scheduled 八个页面至少具备依赖图可视化。
// 本服务承担"数据侧"：
//   * Appx     —— 解析 PowerShell Get-AppxPackage Depends（TSV 回读）
//   * Service  —— SCM QueryServiceConfig2(SERVICE_CONFIG_DEPENDENCIES)
//                 枚举服务的 DependOnService / ServiceLoadOrderGroup
//   * Startup  —— EnumDependentServices 反向枚举依赖某启动项的服务
//   * Tools    —— ToolDependsForKind（本工程已就绪）
//   * Capability—— DISM /Get-Capabilities 的 Dependencies 字段
//
// 树节点是纯数据（parent index + level），UI 层按 level 缩进渲染，
// 不依赖 DuiLib CTreeViewUI（DuiLib.lib 未编译该控件，避免改库）。
//
// 规则：线程仅 PostMessage 回传结果，UI 线程负责 new/delete。
// ============================================================

#include <windows.h>
#include <vector>
#include "Utils/Utils.h"

using DuiLib::CDuiString;

// [R-B25] 依赖图节点（parent = 父节点在 vNodes 中的下标，-1 表示根）
struct DepGraphNode
{
    CDuiString sLabel;   // 节点显示文本
    CDuiString sKind;    // 节点类型（exe / service / appx / capability / package）
    int        nParent = -1;  // 父节点下标，-1 表示根
    int        nLevel  = 0;   // 缩进层级（0 根）
};

// [R-B25] 一张依赖图 = 若干根节点 + 若干子节点（同一 vNodes 扁平数组）
struct DepGraphData
{
    std::vector<DepGraphNode> vNodes;
    int  nErr = 0;        // 0 成功；非 0 采集失败
    int  nSource = 0;     // 采集源：0=Appx 1=Service 2=Startup 3=Tools 4=Capability
};

class DependencyGraphService
{
public:
    // ---- Appx：解析 appx_deps.tsv（每行 FullName \t Dep1,Dep2,...）----
    // 由 EnrichAppxDependencies 的 PowerShell 后台进程写盘，本函数读回。
    // bUser: 用户应用（true）/ 预装（false），用于区分 Tab。
    static void BuildAppxGraph(const std::vector<CDuiString>& vPkgs,
                               const std::vector<CDuiString>& vDepLists,
                               DepGraphData& out);

    // ---- Service：SCM QueryServiceConfig2(SERVICE_CONFIG_DEPENDENCIES) ----
    // 查询某服务依赖哪些服务；失败填 nErr。
    static bool QueryServiceDependencies(const wchar_t* pszSvcName,
                                         std::vector<CDuiString>& vDepServices,
                                         DWORD& dwErrOut);

    // ---- Startup：EnumDependentServices 反向依赖 ----
    // 已经存在于 StartupService::EnumerateDependentServices，本函数直接调用；
    // 返回依赖该启动项的服务列表。
    static bool EnumerateStartupDependents(const wchar_t* pszSvcName,
                                           std::vector<CDuiString>& vDepNames,
                                           std::vector<CDuiString>& vDepDisplay,
                                           DWORD& dwErrOut);

    // ---- Tools：工具依赖 ----
    // 工具 nKind 依赖的 exe 是否存在（存在则节点可见）。
    static bool ToolDependencyAvailable(int nKind, CDuiString& sDepExeOut);

    // ---- 通用：把 vParents（父标签）+ vChildren（子标签列表）压成 DepGraphData ----
    static void Compose(const CDuiString& sRoot,
                        const std::vector<CDuiString>& vChildren,
                        DepGraphData& out);
};
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
