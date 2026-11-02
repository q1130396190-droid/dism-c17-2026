#pragma once

// ============================================================
// Page_ToolsCommon.h —— 工具箱页跨文件共享的 RAII 守卫与 UI helper。
//
// 背景：[R127-1] Page_Tools.cpp 曾达 4155 行/118 函数，规模维度评分 1.2。
// 按业务域拆为 Page_Tools_E2I / Wim / Hosts / 主文件后，守卫与 helper
// 必须共享同一份定义（不可复制多份，否则 ODR 冲突且修正发散）。
//
// 本文件只放「无状态、可内联、不依赖 CPageTools 成员」的内容。
// ============================================================

#include "stdafx.h"
#include "NotifyBox.h"   // [R143] ToolSql::HostNotify / HostConfirm
#include "ExternalToolService.h"   // [R163] BpToolAvailable → IsToolAvailable

#include <optional>
#include <string>
#include <type_traits>

// [C++17 阶段2] 局部 RAII 守卫：文件/查找/令牌句柄、注册表键（见 WinHandles.h 使用规范）
struct PTFileGuard
{
    HANDLE h = nullptr;
    PTFileGuard() = default;
    explicit PTFileGuard(HANDLE x) : h(x) {}
    ~PTFileGuard() { if (h && h != INVALID_HANDLE_VALUE) ::CloseHandle(h); }
    PTFileGuard(const PTFileGuard&) = delete;
    PTFileGuard& operator=(const PTFileGuard&) = delete;
    HANDLE* out() { return &h; }
    operator HANDLE() const { return h; }
};

struct PTFindGuard
{
    HANDLE h;
    explicit PTFindGuard(HANDLE x) : h(x) {}
    ~PTFindGuard() { if (h && h != INVALID_HANDLE_VALUE) ::FindClose(h); }
    PTFindGuard(const PTFindGuard&) = delete;
    PTFindGuard& operator=(const PTFindGuard&) = delete;
    operator HANDLE() const { return h; }
};

struct PTRegGuard
{
    HKEY k = nullptr;
    ~PTRegGuard() { if (k) ::RegCloseKey(k); }
    HKEY* out() { return &k; }
    operator HKEY() const { return k; }
};

namespace ToolSql
{

// [R122-2] 动态 API 解析统一入口：把 FARPROC -> 函数指针这一次 reinterpret_cast
// 收敛到单点，调用方只写目标签名，页面正文不再出现裸转换。
template <typename PFN>
[[nodiscard]] PFN ResolveProc(const wchar_t* pszDll, const char* pszName) noexcept
{
    static_assert(std::is_pointer_v<PFN>, "PFN must be a function pointer type");
    HMODULE hMod = ::GetModuleHandleW(pszDll);
    if (hMod == nullptr) hMod = ::LoadLibraryW(pszDll);
    if (hMod == nullptr) return nullptr;
    const auto pfn = ::GetProcAddress(hMod, pszName);
    return pfn ? reinterpret_cast<PFN>(pfn) : nullptr;
}

// [R143] HostNotify / HostConfirm 已抽到中立头 NotifyBox.h ——
// 该头只含这两个无状态 inline 函数，其它页面（Optim / Appx）可直接包含，
// 不必反向依赖本工具箱页内部头。此处保留原命名空间与函数名，调用点零改动。
} // namespace ToolSql

// ============================================================
// [R170] 常用目录菜单（Page_ToolsDirs.cpp 实现，Page_Tools.cpp 调用）
//
// 原为 Page_Tools.cpp 匿名命名空间内的一整组 static（TCommonDir / Fill*Dirs /
// ShowCommonDirMenu，共 114 行）。整组不依赖 CPageTools 成员，故外迁为独立 TU，
// 只保留这一个入口声明。整组实现细节（三段目录来源、菜单 ID 基址 1000）
// 仍封装在实现文件的具名命名空间内，不对外暴露。
// ============================================================
namespace tool_dirs
{
// 弹出常用目录菜单并把用户选择交给 Explorer。hWnd 为宿主窗口。
void ShowCommonDirMenu(HWND hWnd);
} // namespace tool_dirs

// ============================================================
// [R163] 工具依赖可用性探测（卡片构建与总览统计共用同一口径）。
//
// 原为主文件内 static，拆出 Page_ToolsBuild.cpp（BpBuildCard）后两个 TU
// 都要用它 → 提升为本头 inline，避免各留一份导致口径漂移。
// ============================================================
[[nodiscard]] inline bool BpToolAvailable(const wchar_t* pszDep)
{
    if (pszDep == nullptr || pszDep[0] == 0)
        return true;   // 内置 DLL 链路，无外部依赖
    if (lstrcmpiW(pszDep, L"explorer.exe") == 0
        || lstrcmpiW(pszDep, L"notepad.exe") == 0)
        return true;   // 永远可用
    return ExternalToolService::IsToolAvailable(pszDep);
}

// ============================================================
// [R163] 工具分类 / 风险 / 依赖表（卡片构建与筛选共用的唯一真源）
//
// 原为 Page_Tools.cpp 匿名命名空间内的 static 表。拆出 Page_ToolsBuild.cpp
// （BpBuildCard）后两个 TU 都要读 kToolCat / kToolDepends，故提升为头内
// inline 变量（C++17 inline variables，ODR 安全，不产生多份副本）。
// ============================================================
struct ToolCatEntry
{
    int         nCat;
    int         nRisk;
    const wchar_t*     pszKey;
    const wchar_t*     pszEn;
};
inline const ToolCatEntry kToolCat[] = {
    { 0, 1, L"toolbox.cat_sys",       L"System"   },  // 0 系统备份
    // [修复] 系统还原对齐原版：工具箱默认就同时展示"系统备份/系统还原"两个
    // 入口。旧代码自创"非专家模式隐藏 nRisk==2 卡片"，导致默认安装下用户
    // 根本看不到还原功能（被误认为功能缺失）；危险拦截由入口内多步确认承担。
    { 0, 1, L"toolbox.cat_sys",       L"System"   },  // 1 系统还原
    { 0, 1, L"toolbox.cat_sys",       L"System"   },  // 2 账户管理
    { 0, 1, L"toolbox.cat_sys",       L"System"   },  // 3 引导修复 [R-B85] 高(隐藏)→中：bcdboot 重建引导是常用修复入口，默认须可见；点击仍有二次确认
    { 0, 1, L"toolbox.cat_sys",       L"System"   },  // 4 春哥附体
    { 1, 0, L"toolbox.cat_image",     L"Image"    },  // 5 ESD转ISO
    { 1, 0, L"toolbox.cat_image",     L"Image"    },  // 6 WIM/ESD互转
    { 1, 0, L"toolbox.cat_image",     L"Image"    },  // 7 ImageX
    { 2, 0, L"toolbox.cat_util",      L"Utility"  },  // 8 文件浏览器
    { 1, 0, L"toolbox.cat_image",     L"Image"    },  // 9 ISO生成器
    { 2, 0, L"toolbox.cat_util",      L"Utility"  },  // 10 hosts 编辑器
    // [R01] 激活备份/还原（独立组件 ActivationKit.dll；还原写注册表，定中风险）
    { 2, 1, L"toolbox.cat_util",      L"Utility"  },  // 11 激活备份
    { 1, 1, L"toolbox.cat_image",     L"Image"    },  // 12 Windows 11 镜像处理
};
inline constexpr int kToolCatCount = static_cast<int>(sizeof(kToolCat) / sizeof(kToolCat[0]));
// [R119-1] 风险等级中文名 + 徽章色（0=低 1=中 2=高）
inline const wchar_t* const kToolRiskKeys[3] = {
    L"toolbox.risk_low", L"toolbox.risk_mid", L"toolbox.risk_high",
};
inline const DWORD kToolRiskColors[3] = {
    0xFF2E7D32, 0xFFF57C00, 0xFFD32F2F,
};
    // [R-B24]/[2026-10-08] 11 个工具箱工具的依赖组件
    //   （知识库 40 §2.1/§2.3：原版几乎不走外部命令，备份/还原/互转走
    //    wimgapi.dll、春哥附体走 netapi32，均为内置 DLL；空串=无外部
    //    依赖，永远可用。oscdimg.exe 是 ADK 组件，没装时 kind 5/9 置灰）
inline const wchar_t* const kToolDepends[] = {
        L"",              // 0 系统备份（wimgapi CaptureImage）
        L"",              // 1 系统还原（wimgapi ApplyImage）
        L"",              // 2 账户管理（进程内账户对话框）
        L"bcdboot.exe",   // 3 引导修复（随包 amd64\bcdboot.exe + bootsect，ProcessRunner 负责定位）
        L"",              // 4 春哥附体（netapi32 NetUser*，非 net.exe）
        L"",              // 5 ESD转ISO（内置 wimgapi 导出 + IsoWriter 组装）
        L"",              // 6 WIM/ESD互转（wimgapi WIMExportImage 压缩切换）
        L"",              // 7 ImageX（进程内映像索引管理）
        L"",              // 8 文件浏览器（explorer 为系统外壳，永远可用）
        L"",              // 9 ISO生成器（内置 IsoWriter，非 oscdimg）
        L"",              // 10 hosts 编辑器（进程内 hosts 编辑对话框）
        // [R01] 11 激活备份：依赖探测不用 exe 表，BpBuildCard 特判
        // ActivationKitClient::IsReady()（DLL 缺失时卡片置灰）。
        L"",              // 11 激活备份（ActivationKit.dll）
        L"",              // 12 Windows 11 镜像处理（Rust dismcore）
    };
    // [R-B24] kToolDepends 长度校验：与 kToolCat 一致
    static_assert(_countof(kToolDepends) == kToolCatCount, "kToolDepends size mismatch");
    // [R103] 与 ToolDependencyTable.h 的 kToolKindCount 钉在一起。
    //   历史上这两个常量一个 13 一个 12，导致 kind 12（Win11 镜像处理）
    //   在 DependencyGraphService 里查依赖拿到 nullptr，被判为"不可用"。
    //   任一侧增删工具而忘了同步，这里会直接编译失败。
    static_assert(kToolCatCount == 13, "kToolCat 增删工具后，务必同步 ToolDependencyTable.h 的 kToolKindCount 与 ToolDependsForKind 的 case");
