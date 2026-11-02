// ============================================================================
//  AppxRuntimeBridge.h
//
//  [P0-A1 2026-10-11] Appx 运行时原生桥：WinRT PackageManager ABI 直连
//
//  替代 AppxDeployService 旧降级链：
//    卸载用户包 : powershell.exe Get-AppxPackage | Remove-AppxPackage
//    卸载预装包 : dism.exe /Online /Remove-ProvisionedAppxPackage
//                 （预配包走 DismApiBridge::RemoveProvisionedAppx，不在本桥）
//    备份/导出 : powershell.exe Export-AppxPackage
//    恢复/安装 : powershell.exe Add-AppxPackage -DependencyPath
//
//  全部使用系统自带 combase/winrt ABI（RoActivateInstance + IPackageManager），
//  不 fork 任何外部进程；函数内部自建 MTA 工作线程并同步等待，
//  调用方所在 UI(STA) 线程的套间模型不受影响。
//
//  纯系统层：不依赖 UI / DuiLib。
// ============================================================================

#ifndef APPX_RUNTIME_BRIDGE_H
#define APPX_RUNTIME_BRIDGE_H

#include <string>
#include <vector>

namespace AppxRuntimeBridge
{
    // 已安装包信息（在线当前用户）
    struct PkgInfo
    {
        std::wstring sFullName;     // PackageFullName
        std::wstring sFamilyName;   // PackageFamilyName
        std::wstring sInstallPath;  // InstalledLocation 物理路径
        std::wstring sVersion;      // Major.Minor.Build.Revision
        bool         bFramework;    // Package.IsFramework
        std::vector<std::wstring> vDeps;  // 依赖包 PackageFullName 列表
    };

    // 当前系统 WinRT 激活能力是否可用（RoActivateInstance 可创建 PackageManager）
    [[nodiscard]] bool IsAvailable();

    // 枚举当前用户已安装包（IPackageManager::FindPackages）
    [[nodiscard]] bool EnumeratePackages(std::vector<PkgInfo>& vOut, std::wstring* pErr);

    // 卸载当前用户包（RemovePackageAsync，等待 DeploymentResult）
    [[nodiscard]] bool RemovePackage(const std::wstring& sFullName, std::wstring* pErr);

    // 备份包：取 InstalledLocation 后原生递归复制到 sTargetDir\FullName
    // （等价 Export-AppxPackage -Path 产出的松散包目录）
    [[nodiscard]] bool BackupPackage(const std::wstring& sFullName,
                       const std::wstring& sTargetDir,
                       std::wstring* pErr);

    // 安装 .appx/.msix/.appxbundle/.msixbundle（AddPackageAsync +
    // ForceApplicationShutdown）；sDepDir 非空时自动加入目录内全部
    // appx/msix 依赖（等价 Add-AppxPackage -DependencyPath）
    [[nodiscard]] bool AddPackage(const std::wstring& sPkgFile,
                    const std::wstring& sDepDir,
                    std::wstring* pErr);

    // 重注册当前用户全部已安装包（等价
    // Get-AppxPackage | Add-AppxPackage -Register <manifest>）：
    // 对每个包 InstalledLocation\AppxManifest.xml 调 RegisterPackageAsync。
    // pnOk/pnTotal 可空。
    [[nodiscard]] bool ReRegisterAllPackages(int* pnOk, int* pnTotal, std::wstring* pErr);

    // 清理状态异常包（等价 Get-AppxPackage | ? Status -ne 'OK' |
    // Remove-AppxPackage）：IPackageStatus::VerifyIsOK==VARIANT_FALSE 即移除。
    // pnRemoved/pnTotal 可空。
    [[nodiscard]] bool RemoveBadPackages(int* pnRemoved, int* pnTotal, std::wstring* pErr);
}

#endif // APPX_RUNTIME_BRIDGE_H
