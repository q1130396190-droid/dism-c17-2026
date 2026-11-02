#pragma once

// ============================================================
// AppxDeployService.h —— Appx 部署动作 Service 层 [P0-A1 全原生化]
//
// 旧 PowerShell / dism.exe 外部进程链已全部删除：
//   用户包卸载/备份/恢复 → AppxRuntimeBridge（WinRT PackageManager ABI）
//   预配包移除           → DismApiBridge（DismRemoveProvisionedAppxPackage）
//
// 纯系统层：不依赖 UI，不 include UIlib.h / StringManager.h。
// CDuiString 使用 DuiLib::CDuiString 全限定。
// ============================================================

#include <windows.h>
#include <vector>

#ifndef UILIB_API
#define UILIB_API __declspec(dllimport)
#endif

#include "Utils/Utils.h"

namespace DuiLib { class CDuiString; }

// 返回码枚举：0 成功，负数 = 分级失败
enum class EAppxDeployResult : int
{
    APPX_DEPLOY_OK = 0,
    APPX_DEPLOY_ERR_EMPTY_SELECTION = -1,
    APPX_DEPLOY_ERR_NOT_ELEVATED = -2,
    APPX_DEPLOY_ERR_ARG = -3,
    APPX_DEPLOY_ERR_RUN_FAIL = -4,
    APPX_DEPLOY_ERR_EXIT_NONZERO = -5
};

// [R121-1] Appx 部署 HRESULT 映射分级（PowerShell/Appx 部署常见错误码）
// 返回 0=非 HRESULT（无法细分）；非 0 = 错误码等级序号（供 UI 展示分级提示）。
enum class EAppxHResultKind : int
{
    APPX_HR_NONE = 0,
    APPX_HR_ACCESS_DENIED = 1,     // 0x80070005 ACCESS_DENIED
    APPX_HR_ALREADY_INSTALLED = 2, // 0x80073CFB 已安装
    APPX_HR_DEPLOY_BLOCKED = 3,    // 0x80073D06 部署阻止（策略/签名）
    APPX_HR_DEP_LOST = 4,          // 0x80073CF3 依赖包缺失
    APPX_HR_PKG_UPDATE_NOT_ALLOWED = 5, // 0x80073D0B 已存在的包更新失败
    APPX_HR_LICENSE_NOT_FOUND = 6, // 0x80073CFA 许可缺失
    APPX_HR_INSUFF_MEMORY = 7,     // 0x8007000E 内存不足
    APPX_HR_INVALID_ARG = 8,       // 0x80070057 参数无效
    APPX_HR_OTHER = 9              // 其它 HRESULT
};

// [R121-1] HRESULT → 等级映射（供 ClassifyDeployError 调用）
int AppxClassifyDeployHResult(DWORD dwHr);

class AppxDeployService
{
public:
    // 卸载当前用户 Appx 包（按 PackageFullName 批量）
    // [P0-A1] 原生 WinRT PackageManager.RemovePackageAsync，
    // 无 powershell.exe 子进程；dwTimeoutMs 保留兼容签名（桥内等待完成）。
    static EAppxDeployResult AppxRemovePackage(const std::vector<DuiLib::CDuiString>& vFullNames,
                                 DuiLib::CDuiString& sErrOut,
                                 DWORD dwTimeoutMs = 300000);

    // 卸载 Provisioned（预装）Appx 包（按 FamilyName 批量）
    // [P0-A1] 原生 dismapi DismRemoveProvisionedAppxPackage，无 dism.exe 子进程。
    static EAppxDeployResult AppxRemoveProvisioned(const std::vector<DuiLib::CDuiString>& vFamilyNames,
                                     DuiLib::CDuiString& sErrOut,
                                     DWORD dwTimeoutMs = 300000);

    // 备份（导出）选中 Appx 包到指定目录
    // [P0-A1] 原生读取 InstalledLocation 后递归复制（等价 Export-AppxPackage
    // 的松散包目录产物），无 powershell.exe 子进程。
    static EAppxDeployResult AppxBackup(const std::vector<DuiLib::CDuiString>& vFullNames,
                          const DuiLib::CDuiString& sTargetDir,
                          DuiLib::CDuiString& sErrOut,
                          DWORD dwTimeoutMs = 600000);

    // 恢复（安装）.appx / .msix / .appxbundle / .msixbundle
    // [P0-A1] 原生 WinRT PackageManager.AddPackageAsync +
    // ForceApplicationShutdown，sDepDir 非空时自动装载目录内依赖。
    static EAppxDeployResult AppxRestore(const DuiLib::CDuiString& sPkgFile,
                           const DuiLib::CDuiString& sDepDir,
                           DuiLib::CDuiString& sErrOut,
                           DWORD dwTimeoutMs = 600000);

    // [R92] 判断进程是否提权（Elevated）
    static bool IsElevated();

    // [R121-1] 从 PowerShell / Appx 部署错误文本中识别 HRESULT 并分级
    // 返回 EAppxHResultKind；非 HRESULT 或无匹配返回 EAppxHResultKind::APPX_HR_NONE。
    // 供 UI 层针对不同错误码显示差异化 Toast（D4 失败分级）。
    static int ClassifyDeployError(const DuiLib::CDuiString& sErrText);
};
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
