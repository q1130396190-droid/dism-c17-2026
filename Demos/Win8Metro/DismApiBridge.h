// ============================================================================
//  DismApiBridge.h
//
//  R30 v6.5 —— CBS 组件栈操作直连 dismapi.dll（dism.exe 内部实际调用的官方公开 API 层）
//
//  背景：
//    早前计划通过 cbsapi.dll 直连，但本机实测：
//      * C:\Windows\System32\cbsapi.dll 不存在
//      * Windows SDK / ADK 无 cbsapi.h / cbs.h / cbsapi.lib
//    故 cbsapi.dll 直连不可行（无对象可逆，属逆向范畴）。
//
//    而 dismapi.dll 是微软公开导出的 DISM API 层：
//      * C:\Windows\System32\dismapi.dll 本机存在
//      * ADK SDK 自带官方 dismapi.h + dismapi.lib（amd64/x86/arm64）
//      * 导出函数完整覆盖 CBS 组件栈操作（Cleanup-Image / Capabilities / Features / Add-Driver）
//    因此本 Bridge 以 dismapi.dll 直连替代外部 dism.exe 进程 fork，
//    消除 FrameWnd 中 CBS 相关 5 处外部进程依赖中的 4 处（AppAssociations 无公开 API，保留兜底）。
//
//  加载策略：
//    * 通过 LoadLibraryW + GetProcAddress 动态解析，避免强依赖 dismapi.lib
//      （ADK SDK 的 lib 非所有开发机都有，动态加载更稳健）
//    * 未解析到任一必需导出时 IsReady() 返回 false，调用方回退到原有 dism.exe 路径
//
//  线程模型：
//    * Init/Shutdown 须与进程生命周期同步（在 FrameWnd 构造/析构调用）
//    * 具体操作函数在调用方自行决定线程（FrameWnd 使用工作线程 + PostMessage 回 UI）
//    * DISM API 要求每次操作前 DismOpenSession、操作后 DismCloseSession
//      （在线映像传 DISM_ONLINE_IMAGE）
//
// ============================================================================

#ifndef DISM_API_BRIDGE_H
#define DISM_API_BRIDGE_H

#include <windows.h>
#include <string>
#include <vector>
#include <functional>

namespace DismApiBridge
{
    // ---- 生命周期 ----
    // 必须在进程初始化时调用一次；失败返回 false（此时 IsReady() 恒 false）。
    bool Init(const wchar_t* pszLogFile = nullptr);
    // 进程退出前调用（析构路径），释放所有动态解析的模块句柄。
    void Shutdown();
    // 核心依赖是否就绪（DismInitialize + 至少 CleanImage/OpenSession/CloseSession 已解析）。
    bool IsReady();

    // ---- 会话助手（内部使用，也暴露给调用方做扩展）----
    // 打开在线会话（DISM_ONLINE_IMAGE）；失败返回 0 且 pErr 记录 DismGetLastErrorMessage 文本。
    UINT OpenOnlineSession(std::wstring* pErr);
    // [P0-A3 2026-10-11] 打开离线映像会话（pszImageRoot 为挂载/目标根，如 "D:\"）。
    // pszImageRoot 为空/ nullptr 时等价 OpenOnlineSession。
    UINT OpenImageSession(const wchar_t* pszImageRoot, std::wstring* pErr);
    void CloseSession(UINT sess);

    // ---- 组件存储清理：替代 dism /Online /Cleanup-Image ----
    // bResetBase = true 时等价 /ResetBase（永久移除被取代组件，需谨慎）。
    // [P1-收尾] pszImageRoot 非空时定向离线映像（等价 /Image:"<root>" /Cleanup-Image）。
    // 返回 true 表示 HRESULT 成功（含 ERROR_SUCCESS_REBOOT_REQUIRED=3010L）。
    [[nodiscard]] bool CleanComponentStore(bool bResetBase, std::wstring* pErr,
                             const wchar_t* pszImageRoot = nullptr);
    // [R7] 带真实进度的组件清理：pfnProgress 为 DISM 原生回调（当前/总量）。
    // 与原重载共存，调用方按需选择；取消语义仍由 DISM 会话结束承担。
    using CleanProgressFn = void (*)(unsigned int nCurrent, unsigned int nTotal, void* pUser);
    [[nodiscard]] bool CleanComponentStoreEx(bool bResetBase, std::wstring* pErr,
                             CleanProgressFn pfnProgress, void* pUser,
                             const wchar_t* pszImageRoot = nullptr);

    // ---- 可选功能：替代 dism /Online /Enable-Feature ----
    // pszImageRoot 非空时定向到离线映像（等价 /Image:"<root>"）。
    [[nodiscard]] bool EnableFeature(const std::wstring& sName, const std::wstring& sSource,
                       bool bLimitAccess, std::wstring* pErr,
                       const wchar_t* pszImageRoot = nullptr);
    // ---- 可选功能：替代 dism /Online /Disable-Feature /Remove ----
    // bRemovePayload = true 时等价 /Remove（移除功能载荷）。
    bool DisableFeature(const std::wstring& sName, bool bRemovePayload, std::wstring* pErr,
                        const wchar_t* pszImageRoot = nullptr);

    // ---- 能力：替代 dism /Online /Add-Capability ----
    // sSource 非空时作为本地源（等价 /Source），bLimitAccess=true 时加 /LimitAccess。
    [[nodiscard]] bool AddCapability(const std::wstring& sName, const std::wstring& sSource,
                       bool bLimitAccess, std::wstring* pErr,
                       const wchar_t* pszImageRoot = nullptr);
    // ---- 能力：替代 dism /Online /Remove-Capability ----
    [[nodiscard]] bool RemoveCapability(const std::wstring& sName, std::wstring* pErr,
                          const wchar_t* pszImageRoot = nullptr);

    // ---- 驱动：替代 dism /Online /Add-Driver ----
    // [P1-A6 2026-10-10] pszImageRoot 非空时定向离线挂载映像；
    // bForceUnsigned=true 等价 /ForceUnsigned（离线注入未签名驱动）。
    [[nodiscard]] bool AddDriver(const std::wstring& sPath, std::wstring* pErr,
                   const wchar_t* pszImageRoot = nullptr,
                   bool bForceUnsigned = false);

    // ---- 驱动：替代 dism /Online /Remove-Driver /Driver:oemN.inf ----
    // [P1-A6 2026-10-10] DismRemoveDriver 直连；离线会话同样支持。
    [[nodiscard]] bool RemoveDriver(const std::wstring& sInf, std::wstring* pErr,
                      const wchar_t* pszImageRoot = nullptr);

    // ---- 驱动：替代 dism /Online /Get-Drivers /Format:Table ----
    // [P1-A6 2026-10-10] DismGetDrivers 结构化枚举，消除 pnputil
    // /enum-drivers 文本解析。AllDrivers=TRUE 同时返回内置驱动。
    struct DriverInfo
    {
        std::wstring sPublished;   // oemN.inf
        std::wstring sOriginal;    // 原始 INF 文件名
        std::wstring sClass;       // ClassName：Display/Net/MEDIA ...
        std::wstring sProvider;    // ProviderName
        std::wstring sDate;        // DriverVer 日期
        std::wstring sVersion;     // DriverVer 版本
        bool         bInBox;       // 是否系统内置
    };
    bool GetDrivers(std::vector<DriverInfo>& vOut, std::wstring* pErr,
                    const wchar_t* pszImageRoot = nullptr,
                    bool bAllDrivers = true);

    // ---- Appx 包：替代 dism /online /remove-package ----
    // 等价 dism.exe "/Online /Remove-Package /PackageName:<name> /PackageType:Name"。
    // 内部 OpenOnlineSession + DismRemovePackage（DismPackageName）+ CloseSession。
    // 返回 true 表示 HRESULT 成功（含 ERROR_SUCCESS_REBOOT_REQUIRED=3010L）。
    [[nodiscard]] bool RemovePackage(const std::wstring& sName, std::wstring* pErr);

    // [P1-收尾] 添加包：替代 dism /Online /Add-Package /PackagePath /NoRestart。
    // sPath 为 .cab/.msu 路径；pszImageRoot 非空时定向离线映像。
    [[nodiscard]] bool AddPackage(const std::wstring& sPath, std::wstring* pErr,
                    const wchar_t* pszImageRoot = nullptr);

    // ---- 预配 Appx：替代 dism /Online /Remove-ProvisionedAppxPackage ----
    // [P0-A1 2026-10-11] DismRemoveProvisionedAppxPackage(Session, PackageFamilyName)
    // 公开 SDK 导出，pszImageRoot 非空时定向离线映像（与 /Image 一致）。
    [[nodiscard]] bool RemoveProvisionedAppx(const std::wstring& sFamilyName, std::wstring* pErr,
                               const wchar_t* pszImageRoot = nullptr);

    // ---- 映像健康检查：替代 dism /Online /Cleanup-Image /CheckHealth|/ScanHealth ----
    // bScan=false 等价 /CheckHealth（快速标志位检测）；
    // bScan=true  等价 /ScanHealth（整库扫描，耗时较长）。
    // *pdwStatus 接收映像健康状态码（DismImageHealthState：0 Healthy /
    // 1 Repairable / 2 NonRepairable），可空。
    // [BUG-09] pszImageRoot 非空时定向离线映像（等价 /Image），与其他操作
    // 保持一致；为空时在线检查。
    bool CheckImageHealth(bool bScan, DWORD* pdwStatus, std::wstring* pErr,
                          const wchar_t* pszImageRoot = nullptr);

    // ---- 映像修复：替代 dism /Online /Cleanup-Image /RestoreHealth ----
    // sSource 非空时作为本地修复源（分号分隔，等价 /Source），
    // bLimitAccess=true 等价 /LimitAccess（禁止 Windows Update）；
    // pszImageRoot 非空时定向离线映像（等价 /Image）。
    [[nodiscard]] bool RestoreImageHealth(const std::wstring& sSource, bool bLimitAccess,
                            std::wstring* pErr,
                            const wchar_t* pszImageRoot = nullptr);

    // ---- 包状态查询：替代 dism /Online /Get-Packages ----
    struct PkgInfo
    {
        std::wstring sName;          // PackageName
        int          nState;         // DismPackageFeatureState
        int          nReleaseType;   // DismReleaseType
    };
    // 返回当前映像全部包（名称+状态+发行类型），失败返回 false。
    // pszImageRoot 非空时定向离线映像（等价 dism /Image）。
    [[nodiscard]] bool GetPackages(std::vector<PkgInfo>& vOut, std::wstring* pErr,
                     const wchar_t* pszImageRoot = nullptr);

    // ---- 功能枚举：替代 dism /Online /Get-Features /format:list ----
    // [P0-A3 2026-10-11] DismGetFeatures 结构化枚举，消除 bat→runas→
    // featraw.sig 轮询与 ANSI 文本解析。
    // [rb23] 状态码经系统 dismapi.dll 实测（dism.exe 交叉验证）：
    //   0=Disabled/未安装，2=Staged（Disabled），4=Enabled/已安装；
    //   5/6（文档值/Permanent）一并按启用兼容。bEnabled 即据此派生。
    struct FeatureInfo
    {
        std::wstring sName;       // FeatureName
        int          nState;      // DismPackageFeatureState 原值（4=已启用）
        bool         bEnabled;    // 状态为 4 Installed/Enabled（兼容 5/6）
    };
    // pszImageRoot 非空时枚举离线映像；返回 false 时 pErr 有文本。
    bool GetFeatures(std::vector<FeatureInfo>& vOut, std::wstring* pErr,
                     const wchar_t* pszImageRoot = nullptr);

    // ---- 能力枚举：替代 dism /Online /Get-Capabilities ----
    // [P0-A3 2026-10-11] DismGetCapabilities 结构化枚举。
    struct CapabilityInfo
    {
        std::wstring sName;       // Capability identity，如 OpenSSH.Client~~~~0.0.1.0
        int          nState;      // DismPackageFeatureState 原值（0 未安装 / 4 已安装）
        bool         bInstalled;  // 状态为 4 Installed（兼容 5/6）
    };
    [[nodiscard]] bool GetCapabilities(std::vector<CapabilityInfo>& vOut, std::wstring* pErr,
                         const wchar_t* pszImageRoot = nullptr);

    // ---- 错误文本 ----
    // 取回最近一次 API 失败对应的 DismGetLastErrorMessage 文本（自动 DismDelete 释放）。
    std::wstring GetLastErrorText();

    // ---- [A1] 性能埋点（供页面侧计时复用，避免各写一套）----
    //   开关：环境变量 DISMTOOL_PERF_TRACE 存在即启用；未设置时零开销。
    //   口径：只输出到 OutputDebugString，不写文件、不影响 UI。
    [[nodiscard]] bool PerfTraceEnabled();
    class PerfScope
    {
    public:
        explicit PerfScope(const wchar_t* pszNode);
        ~PerfScope();
        PerfScope(const PerfScope&) = delete;
        PerfScope& operator=(const PerfScope&) = delete;
    private:
        const wchar_t* m_pszNode;
        long long      m_llBeg;
    };

    // ---- 原始 HRESULT 工具（供调用方判断 ERROR_SUCCESS_REBOOT_REQUIRED 等）----
    [[nodiscard]] HRESULT LastHResult();
    void SetLastHResult(HRESULT hr);
}

#endif // DISM_API_BRIDGE_H