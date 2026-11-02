// ============================================================================
//  VssNativeService.cpp
//
//  R-B31 —— VSS 原生 API 实现（清单维度 3『后端引擎』缺失 2）
//
//  语义对照（来自 Windows SDK vss.h / vsbackup.h / vsmgmt.h 官方接口）：
//    1. CreateVssBackupComponents()   —— vsbackup.h L834 inline（导出自 vssapi.dll）
//    2. IVssBackupComponents::InitializeForBackup()
//    3. IVssBackupComponents::SetContext(VSS_CTX_ALL)
//    4. IVssBackupComponents::DeleteSnapshots(SnapshotId, VSS_OBJECT_SNAPSHOT,
//       TRUE /*force*/, &nDeleted, nullptr)
//    5. IVssBackupComponents::AbortBackup()
//    6. CLSID_VssSnapshotMgmt → IVssSnapshotMgmt::QueryVolumesSupportedForSnapshots
//       → IVssEnumMgmtObject::Next → IVssSnapshotMgmt::QuerySnapshotsByVolume
//       → IVssEnumObject::Next → VSS_OBJECT_SNAPSHOT 汇总
//
//  回退：本模块纯原生；外部命令回退由调用方 Page_Cleanup.cpp 负责。
// ============================================================================

#include "stdafx.h"

#include "VssNativeService.h"

#include <objbase.h>
#include <utility>   // [C++17 R-P11] std::move
#include <objidl.h>

// VSS 头文件顺序敏感：vsmgmt.h 依赖 vss.h；vsbackup.h 使用 vss.h 的 VSS_ID 等，
// 且 vsbackup.h 使用 vswriter.h 的 VSS_COMPONENT_TYPE/VSS_USAGE_TYPE/
// VSS_RESTOREMETHOD_ENUM。标准包含顺序：vss.h → vswriter.h → vsbackup.h → vsmgmt.h。
#include <vss.h>
#include <vswriter.h>
#include <vsbackup.h>
#include <vsmgmt.h>

// 链接 vssapi.lib（x86 / x64 构建目标均已加入 vcxproj AdditionalDependencies）。
#pragma comment(lib, "vssapi.lib")
#pragma comment(lib, "ole32.lib")

namespace VssNative
{

// ---------------- 工具函数 ----------------

std::wstring FormatGuid(const GUID& g)
{
    std::wstring s;
    s.resize(64);
    wfmt::FormatInto(s.data(), s.size() + 1,
        L"{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
        (unsigned)(g.Data1), (unsigned)(g.Data2), (unsigned)(g.Data3),
        (unsigned)(g.Data4[0]), (unsigned)(g.Data4[1]),
        (unsigned)(g.Data4[2]), (unsigned)(g.Data4[3]),
        (unsigned)(g.Data4[4]), (unsigned)(g.Data4[5]),
        (unsigned)(g.Data4[6]), (unsigned)(g.Data4[7]));
    s.resize(wcslen(s.c_str()));
    return s;
}

bool IsNativeEnabled()
{
    return true;
}

// 把 VSS 的 FILETIME 时间戳格式化为本地时间字符串（YYYY-MM-DD HH:MM:SS）。
static std::wstring FormatTimestamp(VSS_TIMESTAMP ts)
{
    FILETIME ft;
    ft.dwLowDateTime  = static_cast<DWORD>(ts & 0xFFFFFFFFui64);
    ft.dwHighDateTime = static_cast<DWORD>((ts >> 32) & 0xFFFFFFFFui64);

    FILETIME ftLocal = {0, 0};
    if (!::FileTimeToLocalFileTime(&ft, &ftLocal))
        return L"";

    SYSTEMTIME st = {0};
    if (!::FileTimeToSystemTime(&ftLocal, &st))
        return L"";

    std::wstring s;
    s.resize(64);
    wfmt::FormatInto(s.data(), s.size() + 1,
        L"%04u-%02u-%02u %02u:%02u:%02u",
        (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay,
        (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond);
    s.resize(wcslen(s.c_str()));
    return s;
}

// HRESULT 失败码 → 可读文本（用于回退决策与日志）。
static std::wstring DescribeHr(HRESULT hr)
{
    std::wstring s;
    s.resize(128);
    wfmt::FormatInto(s.data(), s.size() + 1, L"0x%08X", (unsigned)(unsigned long)hr);
    s.resize(wcslen(s.c_str()));
    return s;
}

// [N17] 单卷快照枚举抽为独立函数（原 for(;;)>if>if>if>for>if>if 7 层压平）。
//       逐字搬迁原逻辑：对 pwszVol 枚举全部快照，VSS_OBJECT_SNAPSHOT 才计入。
//       返回 S_OK 表示枚举完成；失败/无快照枚举器时返回相应 HRESULT。
static HRESULT VssEnumerateSnapshotsForVolume(IVssSnapshotMgmt* pMgmt,
                                              VSS_PWSZ pwszVol,
                                              std::vector<VSS_ID>* pIds,
                                              std::vector<SnapshotInfo>* pLog)
{
    if (!pMgmt || !pwszVol || !pIds || !pLog)
        return E_POINTER;
    IVssEnumObject* pSnapEnum = nullptr;
    HRESULT hrSnap = pMgmt->QuerySnapshotsByVolume(pwszVol, GUID_NULL, &pSnapEnum);
    if (FAILED(hrSnap) || !pSnapEnum)
        return FAILED(hrSnap) ? hrSnap : E_FAIL;
    for (;;)
    {
        VSS_OBJECT_PROP snapProp;
        snapProp = {};
        unsigned long cSnap = 0;
        HRESULT hrSnapNext = pSnapEnum->Next(1, &snapProp, &cSnap);
        if (hrSnapNext == S_FALSE || cSnap == 0)
            break;
        if (FAILED(hrSnapNext))
            break;

        if (snapProp.Type == VSS_OBJECT_SNAPSHOT)
        {
            VSS_SNAPSHOT_PROP& sp = snapProp.Obj.Snap;
            pIds->push_back(sp.m_SnapshotId);

            SnapshotInfo info;
            info.sSnapshotId = FormatGuid(sp.m_SnapshotId);
            if (sp.m_pwszOriginalVolumeName)
                info.sOriginalVolume = sp.m_pwszOriginalVolumeName;
            if (sp.m_pwszSnapshotDeviceObject)
                info.sDeviceObject = sp.m_pwszSnapshotDeviceObject;
            info.sCreationTime = FormatTimestamp(sp.m_tsCreationTimestamp);
            info.nDeleted = -1;
            // [C++17 R-P11] info 已完整构造且此后不再引用，显式 move 转移 4 个 std::wstring
            pLog->push_back(std::move(info));

            ::VssFreeSnapshotProperties(&sp);
        }
    }
    pSnapEnum->Release();
    return S_OK;
}

// ---------------- 枚举快照 ----------------

// 枚举所有 VSS 快照 ID。只依赖 IVssSnapshotMgmt，不创建 IVssBackupComponents。
// 返回 S_OK 时 *pIds 已填入所有快照 ID；*pLog 附每条的来源卷与创建时间。
static HRESULT EnumerateSnapshotIds(
    std::vector<VSS_ID>* pIds,
    std::vector<SnapshotInfo>* pLog,
    std::wstring* pErr)
{
    if (!pIds || !pLog)
        return E_POINTER;

    HRESULT hrMgmt = S_OK;

    // 1) 创建 VSS 快照管理 COM 对象
    IVssSnapshotMgmt* pMgmt = nullptr;
    hrMgmt = ::CoCreateInstance(
        CLSID_VssSnapshotMgmt, nullptr, CLSCTX_ALL,
        IID_IVssSnapshotMgmt, reinterpret_cast<void**>(&pMgmt));
    if (FAILED(hrMgmt) || !pMgmt)
    {
        if (pErr) *pErr = L"VssNative: CoCreateInstance(CLSID_VssSnapshotMgmt) failed " + DescribeHr(hrMgmt);
        return FAILED(hrMgmt) ? hrMgmt : E_FAIL;
    }

    // 2) 枚举支持创建快照的卷
    IVssEnumMgmtObject* pVolEnum = nullptr;
    hrMgmt = pMgmt->QueryVolumesSupportedForSnapshots(GUID_NULL, VSS_CTX_ALL, &pVolEnum);
    if (FAILED(hrMgmt) || !pVolEnum)
    {
        if (pErr) *pErr = L"VssNative: QueryVolumesSupportedForSnapshots failed " + DescribeHr(hrMgmt);
        pMgmt->Release();
        return FAILED(hrMgmt) ? hrMgmt : E_FAIL;
    }

    // 3) 逐卷枚举快照
    unsigned long nVolumes = 0;
    for (;;)
    {
        VSS_MGMT_OBJECT_PROP volProp;
        volProp = {};
        unsigned long cFetched = 0;
        HRESULT hrNext = pVolEnum->Next(1, &volProp, &cFetched);
        if (hrNext == S_FALSE || cFetched == 0)
            break;
        if (FAILED(hrNext))
        {
            if (pErr) *pErr = L"VssNative: QueryVolumesSupportedForSnapshots enum failed " + DescribeHr(hrNext);
            pVolEnum->Release();
            pMgmt->Release();
            return hrNext;
        }
        ++nVolumes;

        if (volProp.Type == VSS_MGMT_OBJECT_VOLUME || volProp.Type == VSS_MGMT_OBJECT_DIFF_VOLUME)
        {
            VSS_PWSZ pwszVol = nullptr;
            if (volProp.Type == VSS_MGMT_OBJECT_VOLUME)
                pwszVol = volProp.Obj.Vol.m_pwszVolumeName;
            else
                pwszVol = volProp.Obj.DiffVol.m_pwszVolumeName;

            if (pwszVol)
            {
                // [N17] 逐卷快照枚举抽为 VssEnumerateSnapshotsForVolume，
                //       原 for(;;)>if>if>if>for>if>if 7 层嵌套压平。
                VssEnumerateSnapshotsForVolume(pMgmt, pwszVol, pIds, pLog);
            }
        }
    }

    pVolEnum->Release();
    pMgmt->Release();
    return S_OK;
}// ---------------- 删除入口 ----------------

HRESULT DeleteAllShadows(std::wstring* pErrDetail)
{
    // 先枚举所有快照 ID（使用 IVssSnapshotMgmt 路径）
    std::vector<VSS_ID> vIds;
    std::vector<SnapshotInfo> vLog;
    HRESULT hrEnum = EnumerateSnapshotIds(&vIds, &vLog, pErrDetail);
    if (FAILED(hrEnum))
        return hrEnum;

    if (vIds.empty())
    {
        // 无快照可删，仍返回 S_OK（清理成功，语义等价 vssadmin 无输出）
        return S_OK;
    }

    // 创建 IVssBackupComponents 会话
    IVssBackupComponents* pBc = nullptr;
    HRESULT hr = ::CreateVssBackupComponents(&pBc);
    if (FAILED(hr) || !pBc)
    {
        if (pErrDetail) *pErrDetail = L"VssNative: CreateVssBackupComponents failed " + DescribeHr(hr);
        return FAILED(hr) ? hr : E_FAIL;
    }

    HRESULT hrInit = pBc->InitializeForBackup(nullptr);
    if (FAILED(hrInit))
    {
        if (pErrDetail) *pErrDetail = L"VssNative: InitializeForBackup failed " + DescribeHr(hrInit);
        pBc->Release();
        return hrInit;
    }

    HRESULT hrCtx = pBc->SetContext(VSS_CTX_ALL);
    if (FAILED(hrCtx))
    {
        if (pErrDetail) *pErrDetail = L"VssNative: SetContext failed " + DescribeHr(hrCtx);
        pBc->AbortBackup();
        pBc->Release();
        return hrCtx;
    }

    // GatherWriterMetadata 使 VSS 服务进入可操作状态（官方建议）。
    // 需要 IVssAsync 指针承载异步操作；立即完成并释放。
    IVssAsync* pAsync = nullptr;
    HRESULT hrGather = pBc->GatherWriterMetadata(&pAsync);
    if (pAsync)
    {
        pAsync->Release();
        pAsync = nullptr;
    }
    if (FAILED(hrGather))
    {
        // 写收集失败不是致命——仍可删快照；记录但不阻断
        if (pErrDetail) *pErrDetail = L"VssNative: GatherWriterMetadata warning " + DescribeHr(hrGather);
    }

    // 逐个强制删除
    int nDeleted = 0;
    for (size_t i = 0; i < vIds.size(); ++i)
    {
        LONG lDeleted = 0;
        VSS_ID idNonDeleted = GUID_NULL;
        HRESULT hrDel = pBc->DeleteSnapshots(
            vIds[i], VSS_OBJECT_SNAPSHOT, TRUE /*force*/,
            &lDeleted, &idNonDeleted);
        if (SUCCEEDED(hrDel))
        {
            nDeleted += static_cast<int>(lDeleted);
            if (i < vLog.size())
            {
                vLog[i].nDeleted = (lDeleted > 0) ? 1 : 0;
            }
        }
        else
        {
            // 单个快照删除失败不阻断整体（如快照已被系统自动回收）
            if (i < vLog.size())
                vLog[i].nDeleted = -2; // -2 表示删除失败
        }
    }

    // 结束会话
    pBc->AbortBackup();
    pBc->Release();

    // 输出到 DeleteResult（此处以 vLog 回填调用方，若未来扩展）
    return S_OK;
}

HRESULT EnumShadows(std::vector<SnapshotInfo>* pOut, std::wstring* pErrDetail)
{
    if (!pOut)
        return E_POINTER;

    std::vector<VSS_ID> vIds;
    std::vector<SnapshotInfo> vLog;
    HRESULT hr = EnumerateSnapshotIds(&vIds, &vLog, pErrDetail);
    if (FAILED(hr))
        return hr;

    *pOut = vLog;
    return S_OK;
}

} // namespace VssNative
// [C++17 R3] 本文件内联常量（C++17 inline variables，多 TU ODR 一致）：
