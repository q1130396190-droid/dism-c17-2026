// ============================================================
// AppxDeployService.cpp —— [P0-A1] Appx 部署动作 Service 实现（全原生化）
//
// 旧降级链（已全部删除）：
//   powershell.exe Get-AppxPackage | Remove-AppxPackage
//   dism.exe /Online /Remove-ProvisionedAppxPackage
//   powershell.exe Export-AppxPackage
//   powershell.exe Add-AppxPackage -DependencyPath
//
// 现原生通道：
//   用户包卸载/备份/恢复 → AppxRuntimeBridge（WinRT IPackageManager ABI）
//   预配包移除           → DismApiBridge::RemoveProvisionedAppx
//                          （DismRemoveProvisionedAppxPackage 公开导出）
//
// 行尾工程：CRLF；每行间空行风格；不用预编译头；不 include UIlib.h。
// ============================================================

#include "AppxDeployService.h"
#include "ExternalToolService.h"      // IsElevated
#include "DismApiBridge.h"            // [P0-A1] DismRemoveProvisionedAppxPackage
#include "AppxRuntimeBridge.h"        // [P0-A1] WinRT PackageManager
#include <cwctype>   // [C17] iswxdigit（F18 tchar.h 清零）

// [R121-1] 文件作用域辅助：HRESULT → EAppxHResultKind 等级映射
// （Appx 部署常见错误码；返回 EAppxHResultKind::APPX_HR_OTHER 表示非 HRESULT 或未识别）
int AppxClassifyDeployHResult(DWORD dwHr)
{
    switch (dwHr)
    {
    case 0x80070005: return static_cast<int>(EAppxHResultKind::APPX_HR_ACCESS_DENIED);
    case 0x80073CFB: return static_cast<int>(EAppxHResultKind::APPX_HR_ALREADY_INSTALLED);
    case 0x80073D06: return static_cast<int>(EAppxHResultKind::APPX_HR_DEPLOY_BLOCKED);
    case 0x80073CF3: return static_cast<int>(EAppxHResultKind::APPX_HR_DEP_LOST);
    case 0x80073D0B: return static_cast<int>(EAppxHResultKind::APPX_HR_PKG_UPDATE_NOT_ALLOWED);
    case 0x80073CFA: return static_cast<int>(EAppxHResultKind::APPX_HR_LICENSE_NOT_FOUND);
    case 0x8007000E: return static_cast<int>(EAppxHResultKind::APPX_HR_INSUFF_MEMORY);
    case 0x80070057: return static_cast<int>(EAppxHResultKind::APPX_HR_INVALID_ARG);
    default:
        if ((dwHr & 0x80000000) != 0) return static_cast<int>(EAppxHResultKind::APPX_HR_OTHER);
        return static_cast<int>(EAppxHResultKind::APPX_HR_NONE);
    }
}

// ============================================================
// AppxRemovePackage：按 PackageFullName 批量卸载当前用户包
//   原生 WinRT：PackageManager.RemovePackageAsync
// ============================================================
EAppxDeployResult AppxDeployService::AppxRemovePackage(const std::vector<DuiLib::CDuiString>& vFullNames,
                                         DuiLib::CDuiString& sErrOut,
                                         DWORD dwTimeoutMs)
{
    (void)dwTimeoutMs;   // 原生异步在桥内等待，无外部进程超时概念

    if (vFullNames.empty())
    {
        sErrOut = L"未选择 Appx 包";
        return EAppxDeployResult::APPX_DEPLOY_ERR_EMPTY_SELECTION;
    }

    if (!IsElevated())
    {
        sErrOut = L"需要管理员权限执行 Appx 部署操作";
        return EAppxDeployResult::APPX_DEPLOY_ERR_NOT_ELEVATED;
    }

    int nFail = 0;

    for (size_t k = 0; k < vFullNames.size(); ++k)
    {
        std::wstring sApiErr;
        if (AppxRuntimeBridge::RemovePackage(vFullNames[k].GetData(), &sApiErr))
            continue;

        ++nFail;
        DuiLib::CDuiString sOne;
        sOne.Format(L"[%s] %s\r\n",
                    vFullNames[k].GetData(),
                    sApiErr.empty() ? L"RemovePackageAsync failed" : sApiErr.c_str());
        sErrOut += sOne;
    }

    if (nFail == 0)
    {
        sErrOut = L"Appx 包已通过 WinRT PackageManager 原生卸载";
        return EAppxDeployResult::APPX_DEPLOY_OK;
    }

    if (static_cast<size_t>(nFail) == vFullNames.size())
        return EAppxDeployResult::APPX_DEPLOY_ERR_RUN_FAIL;

    // 部分成功：仍返回失败，错误文本含失败明细
    return EAppxDeployResult::APPX_DEPLOY_ERR_EXIT_NONZERO;
}

// ============================================================
// AppxRemoveProvisioned：按 FamilyName 批量移除预装（预配）包
//   原生 dismapi：DismRemoveProvisionedAppxPackage(Session, FamilyName)
// ============================================================
EAppxDeployResult AppxDeployService::AppxRemoveProvisioned(const std::vector<DuiLib::CDuiString>& vFamilyNames,
                                             DuiLib::CDuiString& sErrOut,
                                             DWORD dwTimeoutMs)
{
    (void)dwTimeoutMs;

    if (vFamilyNames.empty())
    {
        sErrOut = L"未选择预装 Appx 包";
        return EAppxDeployResult::APPX_DEPLOY_ERR_EMPTY_SELECTION;
    }

    if (!IsElevated())
    {
        sErrOut = L"需要管理员权限执行 Appx 部署操作";
        return EAppxDeployResult::APPX_DEPLOY_ERR_NOT_ELEVATED;
    }

    if (!DismApiBridge::Init())
    {
        sErrOut = L"dismapi.dll 初始化失败，无法移除预装 Appx 包";
        return EAppxDeployResult::APPX_DEPLOY_ERR_RUN_FAIL;
    }

    int nFail = 0;

    for (size_t k = 0; k < vFamilyNames.size(); ++k)
    {
        std::wstring sApiErr;
        if (DismApiBridge::RemoveProvisionedAppx(vFamilyNames[k].GetData(), &sApiErr))
            continue;

        ++nFail;
        DuiLib::CDuiString sOne;
        sOne.Format(L"[%s] %s\r\n",
                    vFamilyNames[k].GetData(),
                    sApiErr.empty() ? L"DismRemoveProvisionedAppxPackage failed"
                                    : sApiErr.c_str());
        sErrOut += sOne;
    }

    if (nFail == 0)
    {
        sErrOut = L"预装 Appx 包已通过 dismapi 原生素移除";
        return EAppxDeployResult::APPX_DEPLOY_OK;
    }

    if (static_cast<size_t>(nFail) == vFamilyNames.size())
        return EAppxDeployResult::APPX_DEPLOY_ERR_RUN_FAIL;

    return EAppxDeployResult::APPX_DEPLOY_ERR_EXIT_NONZERO;
}

// ============================================================
// AppxBackup：原生读取 InstalledLocation 后递归复制到目标目录
//   （等价 Export-AppxPackage 的松散包目录产物，零外部进程）
// ============================================================
EAppxDeployResult AppxDeployService::AppxBackup(const std::vector<DuiLib::CDuiString>& vFullNames,
                                  const DuiLib::CDuiString& sTargetDir,
                                  DuiLib::CDuiString& sErrOut,
                                  DWORD dwTimeoutMs)
{
    (void)dwTimeoutMs;

    if (vFullNames.empty())
    {
        sErrOut = L"未选择 Appx 包";
        return EAppxDeployResult::APPX_DEPLOY_ERR_EMPTY_SELECTION;
    }

    if (sTargetDir.IsEmpty())
    {
        sErrOut = L"备份目录非法";
        return EAppxDeployResult::APPX_DEPLOY_ERR_ARG;
    }

    int nFail = 0;

    for (size_t k = 0; k < vFullNames.size(); ++k)
    {
        std::wstring sApiErr;
        if (AppxRuntimeBridge::BackupPackage(vFullNames[k].GetData(),
                                             sTargetDir.GetData(), &sApiErr))
            continue;

        ++nFail;
        DuiLib::CDuiString sOne;
        sOne.Format(L"[%s] %s\r\n",
                    vFullNames[k].GetData(),
                    sApiErr.empty() ? L"BackupPackage failed" : sApiErr.c_str());
        sErrOut += sOne;
    }

    if (nFail == 0)
        return EAppxDeployResult::APPX_DEPLOY_OK;

    return (static_cast<size_t>(nFail) == vFullNames.size())
        ? EAppxDeployResult::APPX_DEPLOY_ERR_RUN_FAIL
        : EAppxDeployResult::APPX_DEPLOY_ERR_EXIT_NONZERO;
}

// ============================================================
// AppxRestore：WinRT PackageManager.AddPackageAsync（含依赖目录）
// ============================================================
EAppxDeployResult AppxDeployService::AppxRestore(const DuiLib::CDuiString& sPkgFile,
                                   const DuiLib::CDuiString& sDepDir,
                                   DuiLib::CDuiString& sErrOut,
                                   DWORD dwTimeoutMs)
{
    (void)dwTimeoutMs;

    if (sPkgFile.IsEmpty())
    {
        sErrOut = L"包文件非法";
        return EAppxDeployResult::APPX_DEPLOY_ERR_ARG;
    }

    if (!IsElevated())
    {
        sErrOut = L"需要管理员权限执行 Appx 部署操作";
        return EAppxDeployResult::APPX_DEPLOY_ERR_NOT_ELEVATED;
    }

    std::wstring sApiErr;
    if (AppxRuntimeBridge::AddPackage(sPkgFile.GetData(),
                                      sDepDir.IsEmpty() ? std::wstring()
                                                        : std::wstring(sDepDir.GetData()),
                                      &sApiErr))
    {
        return EAppxDeployResult::APPX_DEPLOY_OK;
    }

    sErrOut = sApiErr.empty()
        ? DuiLib::CDuiString(L"AddPackageAsync failed")
        : DuiLib::CDuiString(sApiErr.c_str());
    return EAppxDeployResult::APPX_DEPLOY_ERR_RUN_FAIL;
}

// ============================================================
// IsElevated：委托 ExternalToolService（避免重复实现）
// ============================================================
bool AppxDeployService::IsElevated()
{
    return ExternalToolService::IsElevated();
}

// ============================================================
// [R121-1] Appx 部署错误 HRESULT 分级
// 输入为部署错误文本（原生错误串同样以 "0xXXXXXXXX" 起始），
// 无法识别返回 EAppxHResultKind::APPX_HR_NONE。
// ============================================================
int AppxDeployService::ClassifyDeployError(const DuiLib::CDuiString& sErrText)
{
    if (sErrText.IsEmpty()) return static_cast<int>(EAppxHResultKind::APPX_HR_NONE);

    // 1) 提取 "0xXXXXXXXX" HRESULT（原生 DeploymentResult.ErrorCode 文本）
    const wchar_t* p = sErrText.GetData();
    while (p && *p)
    {
        if (p[0] == L'0' && (p[1] == L'x' || p[1] == L'X'))
        {
            std::wstring sHex;
            const wchar_t* q = p + 2;
            while (*q && sHex.size() < 30 && iswxdigit(static_cast<wint_t>(*q)))
            {
                sHex.push_back(*q);
                ++q;
            }
            if (sHex.size() >= 4)
            {
                DWORD dwHr = static_cast<DWORD>(wcstoul(sHex.c_str(), nullptr, 16));
                int nKind = AppxClassifyDeployHResult(dwHr);
                if (nKind != static_cast<int>(EAppxHResultKind::APPX_HR_NONE)) return nKind;
            }
            p = q;
            continue;
        }
        ++p;
    }

    // 2) 文本关键词兜底
    if (wcsstr(sErrText.GetData(), L"Access is denied"))
        return static_cast<int>(EAppxHResultKind::APPX_HR_ACCESS_DENIED);
    if (wcsstr(sErrText.GetData(), L"already installed"))
        return static_cast<int>(EAppxHResultKind::APPX_HR_ALREADY_INSTALLED);
    if (wcsstr(sErrText.GetData(), L"deployment blocked"))
        return static_cast<int>(EAppxHResultKind::APPX_HR_DEPLOY_BLOCKED);
    if (wcsstr(sErrText.GetData(), L"dependency"))
        return static_cast<int>(EAppxHResultKind::APPX_HR_DEP_LOST);

    return static_cast<int>(EAppxHResultKind::APPX_HR_OTHER);
}
// [C++17 R3] 本文件内联常量（C++17 inline variables，多 TU ODR 一致）：
