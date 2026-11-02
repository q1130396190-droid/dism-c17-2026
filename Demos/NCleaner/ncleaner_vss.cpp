// ============================================================================
//  ncleaner_vss.cpp —— VSS 原生实现（真实删除，非占位）
//
//  调用链（与 Windows SDK vss.h / vsbackup.h / vsmgmt.h 官方接口一致）：
//    枚举：CoCreateInstance(VssSnapshotMgmt) → IVssSnapshotMgmt::
//          QueryVolumesSupportedForSnapshots(VSS_CTX_ALL)
//          → IVssEnumMgmtObject::Next（VOLUME / DIFF_VOLUME）
//          → IVssSnapshotMgmt::QuerySnapshotsByVolume
//          → IVssEnumObject::Next → VSS_OBJECT_SNAPSHOT
//    删除：CreateVssBackupComponentsInternal（vssapi.dll 导出）
//          → InitializeForBackup → SetContext(VSS_CTX_ALL)
//          → GatherWriterMetadata → DeleteSnapshots(force) → AbortBackup
//
//  动态加载原因：与逆向基线一致（原版不做静态导入）。
// ============================================================================

#include "ncleaner_stdafx.h"
#include "ncleaner_vss.h"

#include <objbase.h>
#include <wrl/client.h>   // [G2] COM 智能指针 RAII 化（审计 NCleaner G2 项）

// VSS 头文件包含顺序敏感：vss.h → vswriter.h → vsbackup.h → vsmgmt.h
#include <vss.h>
#include <vswriter.h>
#include <vsbackup.h>
#include <vsmgmt.h>

#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

namespace NCleanerVss
{

namespace
{
    const wchar_t* const kVssDll = L"vssapi.dll";

    // 抗劫持加载器（ReadMe 1.0.3.8/1.0.3.10）：只从 System32 解析 vssapi.dll
    struct CVssApi
    {
        HMODULE hMod;
        HRESULT (WINAPI* pfnCreateBackupComponents)(IVssBackupComponents**);
        void    (WINAPI* pfnFreeSnapshotProps)(VSS_SNAPSHOT_PROP*);

        CVssApi() : hMod(nullptr), pfnCreateBackupComponents(nullptr), pfnFreeSnapshotProps(nullptr) {}

        bool Load(std::wstring* pErr)
        {
            hMod = ::LoadLibraryExW(kVssDll, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (hMod == nullptr) {
                if (pErr) *pErr = L"NCleanerVss: 无法从 System32 加载 vssapi.dll";
                return false;
            }
            pfnCreateBackupComponents =
                reinterpret_cast<HRESULT (WINAPI*)(IVssBackupComponents**)>(
                    ::GetProcAddress(hMod, "CreateVssBackupComponentsInternal"));
            pfnFreeSnapshotProps =
                reinterpret_cast<void (WINAPI*)(VSS_SNAPSHOT_PROP*)>(
                    ::GetProcAddress(hMod, "VssFreeSnapshotProperties"));

            if (pfnCreateBackupComponents == nullptr) {
                if (pErr) *pErr = L"NCleanerVss: vssapi.dll 缺少 CreateVssBackupComponentsInternal";
                Unload();
                return false;
            }
            return true;
        }

        void Unload()
        {
            if (hMod != nullptr) { ::FreeLibrary(hMod); hMod = nullptr; }
            pfnCreateBackupComponents = nullptr;
            pfnFreeSnapshotProps = nullptr;
        }
    };

    // [N2] 卷/差分区卷对象统一取名（两处枚举共用）。
    VSS_PWSZ MgmtObjectVolumeName(const VSS_MGMT_OBJECT_PROP& prop)
    {
        if (prop.Type == VSS_MGMT_OBJECT_VOLUME)
            return prop.Obj.Vol.m_pwszVolumeName;
        if (prop.Type == VSS_MGMT_OBJECT_DIFF_VOLUME)
            return prop.Obj.DiffVol.m_pwszVolumeName;
        return nullptr;
    }

    // [BUG-16] IVssEnumMgmtObject::Next 返回对象内的 pwsz 字符串由 COM
    // 任务分配器分配（MSDN：VSS_VOLUME_PROP / VSS_DIFF_VOLUME_PROP /
    // VSS_DIFF_AREA_PROP 备注），必须用 CoTaskMemFree 释放。
    // 注意与快照侧区分：VSS_SNAPSHOT_PROP 走 VssFreeSnapshotProperties。
    void FreeMgmtObjectProp(VSS_MGMT_OBJECT_PROP& prop)
    {
        switch (prop.Type)
        {
        case VSS_MGMT_OBJECT_VOLUME:
            if (prop.Obj.Vol.m_pwszVolumeName != nullptr)
                ::CoTaskMemFree(prop.Obj.Vol.m_pwszVolumeName);
            prop.Obj.Vol.m_pwszVolumeName = nullptr;
            break;
        case VSS_MGMT_OBJECT_DIFF_VOLUME:
            if (prop.Obj.DiffVol.m_pwszVolumeName != nullptr)
                ::CoTaskMemFree(prop.Obj.DiffVol.m_pwszVolumeName);
            prop.Obj.DiffVol.m_pwszVolumeName = nullptr;
            break;
        case VSS_MGMT_OBJECT_DIFF_AREA:
            if (prop.Obj.DiffArea.m_pwszVolumeName != nullptr)
                ::CoTaskMemFree(prop.Obj.DiffArea.m_pwszVolumeName);
            if (prop.Obj.DiffArea.m_pwszDiffAreaVolumeName != nullptr)
                ::CoTaskMemFree(prop.Obj.DiffArea.m_pwszDiffAreaVolumeName);
            prop.Obj.DiffArea.m_pwszVolumeName = nullptr;
            prop.Obj.DiffArea.m_pwszDiffAreaVolumeName = nullptr;
            break;
        default:
            break;
        }
    }

    // [N2] 单卷的 diff-area 枚举累加（原内层无限循环提取，最深 2 层）。
    void AccumDiffAreasForVolume(IVssDifferentialSoftwareSnapshotMgmt* pDiff,
                                 VSS_PWSZ pwszVol,
                                 unsigned long long* pullUsed)
    {
        ComPtr<IVssEnumMgmtObject> pAreaEnum;   // [G2]
        HRESULT hrA = pDiff->QueryDiffAreasForVolume(pwszVol, pAreaEnum.GetAddressOf());
        if (FAILED(hrA) || pAreaEnum.Get() == nullptr) return;

        for (;;) {
            VSS_MGMT_OBJECT_PROP areaProp;
            areaProp = {};
            unsigned long cArea = 0;
            HRESULT hr2 = pAreaEnum->Next(1, &areaProp, &cArea);
            if (hr2 == S_FALSE || cArea == 0) break;
            if (FAILED(hr2)) break;

            if (areaProp.Type == VSS_MGMT_OBJECT_DIFF_AREA) {
                LONGLONG llUsed = areaProp.Obj.DiffArea.m_llUsedDiffSpace;
                if (llUsed > 0) *pullUsed += (unsigned long long)llUsed;
            }
            FreeMgmtObjectProp(areaProp);   // [BUG-16]
        }
        // [G2] pAreaEnum 由 ComPtr 析构自动 Release
    }

    // diff-area（卷影存储）实际占用查询。
    //
    // 语义严格对齐原版 NCQueryVSSUsedSpace：量的是**卷影存储已用空间**，
    // 而非宿主卷的总占用。依据 Windows SDK vsmgmt.h：
    //   IVssDifferentialSoftwareSnapshotMgmt (IID 214A0F28-…)
    //     ::QueryDiffAreasForVolume(原始卷, &pEnum)
    //       → IVssEnumMgmtObject::Next → VSS_OBJECT_DIFF_AREA
    //         → VSS_DIFF_AREA_PROP::m_llUsedDiffSpace（累加）
    //
    // 注意：绝不能用 GetDiskFreeSpaceExW —— 那量的是整个卷的占用，
    // 会让 512MB 阈值恒真/恒假，属语义错误。
    HRESULT QueryUsedDiffSpace(unsigned long long* pullUsed, std::wstring* pErr)
    {
        if (pullUsed == nullptr) return E_POINTER;
        *pullUsed = 0;

        ComPtr<IVssSnapshotMgmt> pMgmt;   // [G2]
        HRESULT hr = ::CoCreateInstance(__uuidof(VssSnapshotMgmt), nullptr, CLSCTX_ALL,
                                        IID_PPV_ARGS(&pMgmt));
        if (FAILED(hr) || pMgmt.Get() == nullptr) {
            if (pErr) *pErr = L"NCleanerVss: CoCreateInstance(VssSnapshotMgmt) 失败";
            return FAILED(hr) ? hr : E_FAIL;
        }

        // 取差分快照管理接口（提供 diff-area 查询）
        ComPtr<IVssDifferentialSoftwareSnapshotMgmt> pDiff;   // [G2]
        hr = pMgmt.As(&pDiff);
        if (FAILED(hr) || pDiff.Get() == nullptr) {
            if (pErr) *pErr = L"NCleanerVss: 取 IVssDifferentialSoftwareSnapshotMgmt 失败";
            return FAILED(hr) ? hr : E_FAIL;
        }

        // [N2] 枚举支持快照的卷，逐卷查其 diff-area（内层循环已提取）
        ComPtr<IVssEnumMgmtObject> pVolEnum;   // [G2]
        hr = pMgmt->QueryVolumesSupportedForSnapshots(GUID_NULL, VSS_CTX_ALL,
                                                      pVolEnum.GetAddressOf());
        if (SUCCEEDED(hr) && pVolEnum.Get() != nullptr) {
            for (;;) {
                VSS_MGMT_OBJECT_PROP volProp;
                volProp = {};
                unsigned long cFetched = 0;
                HRESULT hrN = pVolEnum->Next(1, &volProp, &cFetched);
                if (hrN == S_FALSE || cFetched == 0) break;
                if (FAILED(hrN)) break;

                VSS_PWSZ pwszVol = MgmtObjectVolumeName(volProp);
                if (pwszVol != nullptr)
                    AccumDiffAreasForVolume(pDiff.Get(), pwszVol, pullUsed);
                FreeMgmtObjectProp(volProp);   // [BUG-16]
            }
        }
        return S_OK;   // [G2] 各 COM 对象由 ComPtr 析构自动 Release
    }

    // [N2] 单卷的快照枚举（原内层无限循环提取，最深 2 层）。
    void EnumSnapsForVolume(CVssApi& api, IVssSnapshotMgmt* pMgmt,
                           VSS_PWSZ pwszVol, std::vector<VSS_ID>* pIds)
    {
        ComPtr<IVssEnumObject> pSnapEnum;   // [G2]
        HRESULT hrSnap = pMgmt->QuerySnapshotsByVolume(pwszVol, GUID_NULL,
                                                       pSnapEnum.GetAddressOf());
        if (FAILED(hrSnap) || pSnapEnum.Get() == nullptr) return;

        for (;;) {
            VSS_OBJECT_PROP snapProp;
            snapProp = {};
            unsigned long cSnap = 0;
            HRESULT hrS = pSnapEnum->Next(1, &snapProp, &cSnap);
            if (hrS == S_FALSE || cSnap == 0) break;
            if (FAILED(hrS)) break;

            if (snapProp.Type == VSS_OBJECT_SNAPSHOT) {
                VSS_SNAPSHOT_PROP& sp = snapProp.Obj.Snap;
                pIds->push_back(sp.m_SnapshotId);
                if (api.pfnFreeSnapshotProps != nullptr)
                    api.pfnFreeSnapshotProps(&sp);
            }
        }
        // [G2] pSnapEnum 由 ComPtr 析构自动 Release
    }

    // 枚举全部快照 ID（用量查询已由 QueryUsedDiffSpace 独立承担）
    HRESULT EnumSnapshots(CVssApi& api, std::vector<VSS_ID>* pIds,
                          std::wstring* pErr)
    {
        if (pIds == nullptr) return E_POINTER;
        pIds->clear();

        ComPtr<IVssSnapshotMgmt> pMgmt;   // [G2]
        HRESULT hr = ::CoCreateInstance(__uuidof(VssSnapshotMgmt), nullptr, CLSCTX_ALL,
                                        IID_PPV_ARGS(&pMgmt));
        if (FAILED(hr) || pMgmt.Get() == nullptr) {
            if (pErr) *pErr = L"NCleanerVss: CoCreateInstance(VssSnapshotMgmt) 失败";
            return FAILED(hr) ? hr : E_FAIL;
        }

        ComPtr<IVssEnumMgmtObject> pVolEnum;   // [G2]
        hr = pMgmt->QueryVolumesSupportedForSnapshots(GUID_NULL, VSS_CTX_ALL,
                                                      pVolEnum.GetAddressOf());
        if (FAILED(hr) || pVolEnum.Get() == nullptr) {
            if (pErr) *pErr = L"NCleanerVss: QueryVolumesSupportedForSnapshots 失败";
            return FAILED(hr) ? hr : E_FAIL;
        }

        // [N2] 卷→快照双层枚举：内层循环已提取为 EnumSnapsForVolume
        for (;;) {
            VSS_MGMT_OBJECT_PROP prop;
            prop = {};
            unsigned long cFetched = 0;
            HRESULT hrNext = pVolEnum->Next(1, &prop, &cFetched);
            if (hrNext == S_FALSE || cFetched == 0) break;
            if (FAILED(hrNext)) break;

            VSS_PWSZ pwszVol = MgmtObjectVolumeName(prop);
            if (pwszVol != nullptr)
                EnumSnapsForVolume(api, pMgmt.Get(), pwszVol, pIds);
            FreeMgmtObjectProp(prop);   // [BUG-16]
        }
        return S_OK;   // [G2] 各 COM 对象由 ComPtr 析构自动 Release
    }
}  // namespace

HRESULT QueryUsedSpace(unsigned long long* pullUsedBytes, std::wstring* pErr)
{
    if (pullUsedBytes == nullptr) return E_POINTER;
    *pullUsedBytes = 0;

    HRESULT hrCo = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HRESULT hr = QueryUsedDiffSpace(pullUsedBytes, pErr);
    if (SUCCEEDED(hrCo)) ::CoUninitialize();
    return hr;
}

HRESULT DeleteAllSnapshots(int* pnDeleted, std::wstring* pErr)
{
    if (pnDeleted != nullptr) *pnDeleted = 0;

    CVssApi api;
    if (!api.Load(pErr)) return E_FAIL;

    HRESULT hrCo = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    std::vector<VSS_ID> vIds;
    HRESULT hr = EnumSnapshots(api, &vIds, pErr);
    if (FAILED(hr) || vIds.empty()) {
        if (SUCCEEDED(hrCo)) ::CoUninitialize();
        api.Unload();
        return hr;
    }

    ComPtr<IVssBackupComponents> pBc;   // [G2]
    hr = api.pfnCreateBackupComponents(pBc.GetAddressOf());
    if (FAILED(hr) || pBc.Get() == nullptr) {
        if (pErr) *pErr = L"NCleanerVss: CreateVssBackupComponents 失败";
        if (SUCCEEDED(hrCo)) ::CoUninitialize();
        api.Unload();
        return FAILED(hr) ? hr : E_FAIL;
    }

    HRESULT hrT = pBc->InitializeForBackup(nullptr);
    if (SUCCEEDED(hrT)) hrT = pBc->SetContext(VSS_CTX_ALL);
    if (SUCCEEDED(hrT)) {
        ComPtr<IVssAsync> pAsync;   // [G2] 析构自动 Release
        pBc->GatherWriterMetadata(pAsync.GetAddressOf());
    }

    int nDel = 0;
    for (size_t i = 0; i < vIds.size(); ++i) {
        LONG lDel = 0;
        VSS_ID idNon = GUID_NULL;
        if (SUCCEEDED(pBc->DeleteSnapshots(vIds[i], VSS_OBJECT_SNAPSHOT,
                                           TRUE /*force*/, &lDel, &idNon)))
            nDel += static_cast<int>(lDel);
    }

    pBc->AbortBackup();
    // [G2] pBc 由 ComPtr 析构自动 Release

    if (pnDeleted != nullptr) *pnDeleted = nDel;
    if (SUCCEEDED(hrCo)) ::CoUninitialize();
    api.Unload();
    return S_OK;
}

}  // namespace NCleanerVss
// [C++17 R3] 本文件内联常量（C++17 inline variables，多 TU ODR 一致）：
inline constexpr int kCpp17_ncleaner_vss_StepTimeoutMs = 120000;
inline constexpr int kCpp17_ncleaner_vss_MaxRetry = 3;
