// ============================================================
// IsoImageService.cpp —— [R6-04] wimgapi 映像编辑作业工厂实现。
//
// 8 个作业闭包逐字搬迁自 ToolDialog.cpp ImageXAction(4789-5334)/
// ImageXSplit(5343-5407)（2026-10-13 基线），仅把 CDuiString 捕获
// 换成 std::wstring、进度适配器改为本 TU 匿名命名空间内的同名类。
// 线程/进度盾/完成弹窗仍在 Page（RunImageXJob/OnJobDone）。
// ============================================================

#include <windows.h>

#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "WIMHelperBridge.h"
#include "EsdDecryptor.h"      // [R7] ESD 自主解密（CryptoAPI，非 UI 原语）
#include "IsoWriter.h"         // [R7] ISO 全内置组装（非 UI 原语）

#include "IsoMount.h"          // [R-B80] ISO 直连挂载/卸载/内部映像定位
#include "IIsoImageService.h"
// [C++17 阶段2] 局部 RAII 守卫：wimgapi 句柄（见 WinHandles.h 使用规范）
struct ISWimGuard
{
    WIMHandle h = INVALID_HANDLE_VALUE;
    ISWimGuard() = default;
    explicit ISWimGuard(WIMHandle x) : h(x) {}
    ~ISWimGuard() { if (h && h != INVALID_HANDLE_VALUE) CWIMHelper::CloseHandle(h); }
    ISWimGuard(const ISWimGuard&) = delete;
    ISWimGuard& operator=(const ISWimGuard&) = delete;
    operator WIMHandle() const { return h; }
    [[nodiscard]] bool valid() const { return h && h != INVALID_HANDLE_VALUE; }
};
struct ISImgGuard
{
    WIMImageHandle h = INVALID_HANDLE_VALUE;
    ISImgGuard() = default;
    explicit ISImgGuard(WIMImageHandle x) : h(x) {}
    ~ISImgGuard() { if (h && h != INVALID_HANDLE_VALUE) WIMCloseHandle(h); }
    ISImgGuard(const ISImgGuard&) = delete;
    ISImgGuard& operator=(const ISImgGuard&) = delete;
    operator WIMImageHandle() const { return h; }
    [[nodiscard]] bool valid() const { return h && h != INVALID_HANDLE_VALUE; }
};


namespace toolsvc {


namespace {

// [R7] wimgapi 0-100% 进度 → IToolJobSink
class CSinkWimProgress : public WIMBridge::IWimProgress
{
public:
    IToolJobSink* pSink = nullptr;
    virtual void OnWimProgress(DWORD dwPercent, DWORD /*dwMsgId*/) override
    {
        if (pSink) pSink->OnProgress(static_cast<int>(dwPercent), std::wstring());
    }
};

// 多索引循环任务的进度适配器：把单次 wimgapi 调用的 0-100 映射到总进度
// 的 [nLo, nLo+nSpan] 区间，避免批量导出/删除时进度条在每个索引间回弹。
// （逐字搬自 ToolDialog.cpp 1267-1282 的 CScaledWimProgress。）
class CScaledWimProgress : public WIMBridge::IWimProgress
{
public:
    WIMBridge::IWimProgress* pInner;
    int nLo;
    int nSpan;
    CScaledWimProgress() : pInner(nullptr), nLo(0), nSpan(100) {}
    virtual void OnWimProgress(DWORD dwPercent, DWORD dwMsgId) override
    {
        if (!pInner) return;
        int v = nLo + static_cast<int>((long long)nSpan * (long long)dwPercent / 100);
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        pInner->OnWimProgress(static_cast<DWORD>(v), dwMsgId);
    }
};

namespace {

// [FIX] 受检的原子替换：MoveFileExW 覆盖原镜像并写穿；失败即返回错误，
// 不留下“临时文件已生成、原文件未替换却报成功”的假状态。
bool ReplaceWithVerifiedTemp(const std::wstring& wTmp, const std::wstring& wFile,
                             const wchar_t* pszTag, std::wstring& sErr)
{
    if (::GetFileAttributesW(wTmp.c_str()) == INVALID_FILE_ATTRIBUTES)
    { sErr = std::wstring(pszTag) + L": temp image missing"; return false; }
    if (!::MoveFileExW(wTmp.c_str(), wFile.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    { sErr = std::wstring(pszTag) + L": replace failed, original preserved"; return false; }
    return true;
}

// [FIX] 把源容器中除 vDel 之外的索引重建到临时容器，再由调用方原子替换。
// 这样中途失败时原镜像保持不变，不会出现“删了一半”的部分成功状态。
bool RebuildWimExcluding(const std::wstring& wFile, const std::wstring& wTmp,
                         const std::vector<DWORD>& vDel, DWORD dwCompress,
                         WIMBridge::IWimProgress* pProg, std::wstring& sErr)
{
    std::vector<WIMBridge::WimInfo> vInfo;
    if (!WIMBridge::QueryWimInfo(wFile, vInfo, &sErr) || vInfo.empty())
    { if (sErr.empty()) sErr = L"query wim info failed"; return false; }

    std::set<DWORD> del(vDel.begin(), vDel.end());
    std::vector<DWORD> keep;
    for (const auto& it : vInfo)
        if (del.find(it.index) == del.end()) keep.push_back(it.index);
    if (keep.empty())
    { sErr = L"refusing to delete every image in the container"; return false; }

    ::DeleteFileW(wTmp.c_str());
    CScaledWimProgress sp; sp.pInner = pProg;
    const size_t n = keep.size();
    for (size_t i = 0; i < n; ++i)
    {
        sp.nLo = n ? static_cast<int>(i * 100 / n) : 0;
        sp.nSpan = n ? static_cast<int>(100 / n) + 1 : 100;
        if (!WIMBridge::ExportImageEx(wFile, keep[i], wTmp, dwCompress,
                L"", L"", false, &sErr, &sp))
        { sErr = L"[wimgapi] rebuild export failed: " + sErr; ::DeleteFileW(wTmp.c_str()); return false; }
    }
    if (pProg) pProg->OnWimProgress(100, 0);
    return true;
}

} // namespace

class CIsoImageService : public IIsoImageService
{
public:
    // [R-B80] 两个槽位当前透明挂载的 ISO 原路径（空=未持有）
    std::wstring m_sSlotIso[2];

    virtual ~CIsoImageService() { CloseIso(); }

    // ---- [R16][T4] 弹窗入参持有（ESD 向导密钥/预填、PE 设置挂载目录预填）----
    virtual void Open(const ToolOpenArgs& args) override
    {
        m_vEsdKeys        = args.vEsdKeys;
        m_sInitialEsdFile = args.sInitialEsdFile;
        m_bHasInitialEsd  = args.bHasInitialEsd;
        m_sInitialPeDir   = args.sInitialPeDir;
    }
    virtual const std::vector<std::wstring>& EsdKeys() const override
    { return m_vEsdKeys; }
    virtual bool HasInitialEsd() const override { return m_bHasInitialEsd; }
    virtual const std::wstring& InitialEsdFile() const override
    { return m_sInitialEsdFile; }
    virtual const std::wstring& InitialPeDir() const override
    { return m_sInitialPeDir; }

    std::vector<std::wstring> m_vEsdKeys;
    std::wstring              m_sInitialEsdFile;
    bool                      m_bHasInitialEsd = false;
    std::wstring              m_sInitialPeDir;

    // ---- case 0 删除 ----
    WimJobFn MakeDeleteEsdRebuildJob(
        const std::wstring& sFile,
        const std::wstring& sTmp,
        const std::vector<DWORD>& vKeep,
        bool bSingle)
    {
        std::wstring wFile = sFile, wTmp = sTmp;
        std::vector<DWORD> keep = vKeep;
        const wchar_t* szTag = bSingle ? L"[wimgapi] single delete failed: "
                                       : L"[wimgapi] batch delete failed: ";
        return [wFile, wTmp, keep, szTag](
            IToolJobSink* pSink, std::wstring& sErr) -> bool
        { CSinkWimProgress _sinkProg; _sinkProg.pSink = pSink; WIMBridge::IWimProgress* pProg = &_sinkProg;
            ::DeleteFileW(wTmp.c_str());
            CScaledWimProgress sp; sp.pInner = pProg;
            const size_t n = keep.size();
            for (size_t i = 0; i < n; ++i)
            {
                sp.nLo = n ? static_cast<int>(i * 100 / n) : 0;
                sp.nSpan = n ? static_cast<int>(100 / n) + 1 : 100;
                if (!WIMBridge::ExportImageEx(wFile, keep[i], wTmp,
                        WIM_COMPRESS_LZMS, L"", L"", false, &sErr, &sp))
                { sErr = szTag + sErr; return false; }
            }
            if (pProg) pProg->OnWimProgress(100, 0);
            if (!::MoveFileExW(wTmp.c_str(), wFile.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            { sErr = L"[wimgapi] bootable replace failed"; return false; }
            return true;
        };
    }

    virtual WimJobFn MakeDeleteEsdRebuildJob(
        const std::wstring& sFile,
        const std::wstring& sTmp,
        const std::vector<DWORD>& vKeep) override
    {
        return MakeDeleteEsdRebuildJob(sFile, sTmp, vKeep, false);
    }

    // 单删 ESD 与批删同体，仅错误标签不同（保持基线文案）。
    virtual WimJobFn MakeDeleteEsdRebuildSingleJob(
        const std::wstring& sFile,
        const std::wstring& sTmp,
        const std::vector<DWORD>& vKeep) override
    {
        return MakeDeleteEsdRebuildJob(sFile, sTmp, vKeep, true);
    }

    virtual WimJobFn MakeDeleteWimBatchJob(
        const std::wstring& sFile,
        const std::vector<DWORD>& vDel) override
    {
        std::wstring wFile = sFile;
        std::vector<DWORD> vd = vDel;
        return [wFile, vd](
            IToolJobSink* pSink, std::wstring& sErr) -> bool
        { CSinkWimProgress _sinkProg; _sinkProg.pSink = pSink; WIMBridge::IWimProgress* pProg = &_sinkProg;
            // [FIX] 事务化：先重建到临时容器，成功后原子替换，避免逐索引原地删除
            // 中途失败留下“删了一半”的部分状态。
            const std::wstring wTmp = wFile + L".rebuild.tmp";
            if (!RebuildWimExcluding(wFile, wTmp, vd, WIM_COMPRESS_LZX, pProg, sErr))
            { sErr = L"[wimgapi] batch delete (WIM) failed: " + sErr; return false; }
            return ReplaceWithVerifiedTemp(wTmp, wFile, L"[wimgapi] batch delete (WIM)", sErr);
        };
    }

    virtual WimJobFn MakeDeleteWimSingleJob(
        const std::wstring& sFile, DWORD nIdx) override
    {
        std::wstring wFile = sFile;
        return [wFile, nIdx](
            IToolJobSink* pSink, std::wstring& sErr) -> bool
        { CSinkWimProgress _sinkProg; _sinkProg.pSink = pSink; WIMBridge::IWimProgress* pProg = &_sinkProg;
            // [FIX] 同上：重建后原子替换，原容器在失败时保持不变。
            const std::wstring wTmp = wFile + L".rebuild.tmp";
            std::vector<DWORD> vDel(1, nIdx);
            if (!RebuildWimExcluding(wFile, wTmp, vDel, WIM_COMPRESS_LZX, pProg, sErr))
            { sErr = L"[wimgapi] single delete (WIM) failed: " + sErr; return false; }
            return ReplaceWithVerifiedTemp(wTmp, wFile, L"[wimgapi] single delete (WIM)", sErr);
        };
    }

    // ---- case 1 可启动 ----
    virtual WimJobFn MakeBootableRebuildJob(
        const std::wstring& sFile,
        const std::wstring& sTmp,
        const std::vector<TBootItem>& vItems,
        DWORD dwComp) override
    {
        std::wstring wFile = sFile, wTmp = sTmp;
        std::vector<TBootItem> vi = vItems;
        return [wFile, wTmp, vi, dwComp](
            IToolJobSink* pSink, std::wstring& sErr) -> bool
        { CSinkWimProgress _sinkProg; _sinkProg.pSink = pSink; WIMBridge::IWimProgress* pProg = &_sinkProg;
            ::DeleteFileW(wTmp.c_str());
            CScaledWimProgress sp; sp.pInner = pProg;
            const size_t n = vi.size();
            for (size_t i = 0; i < n; ++i)
            {
                sp.nLo = n ? static_cast<int>(i * 100 / n) : 0;
                sp.nSpan = n ? static_cast<int>(100 / n) + 1 : 100;
                if (!WIMBridge::ExportImageEx(wFile, vi[i].nIdx, wTmp,
                        dwComp, L"", L"", vi[i].bBoot, &sErr, &sp))
                { sErr = L"[wimgapi] bootable failed: " + sErr; return false; }
            }
            if (pProg) pProg->OnWimProgress(100, 0);
            if (!::MoveFileExW(wTmp.c_str(), wFile.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            { sErr = L"[wimgapi] bootable replace failed"; return false; }
            return true;
        };
    }

    // ---- case 2 导出 ----
    virtual WimJobFn MakeExportBatchJob(
        const std::wstring& sFile,
        const std::vector<TExportItem>& vItems,
        DWORD dwCompress) override
    {
        std::wstring wFile = sFile;
        std::vector<TExportItem> vi = vItems;
        return [wFile, vi, dwCompress](
            IToolJobSink* pSink, std::wstring& sErr) -> bool
        { CSinkWimProgress _sinkProg; _sinkProg.pSink = pSink; WIMBridge::IWimProgress* pProg = &_sinkProg;
            CScaledWimProgress sp; sp.pInner = pProg;
            const size_t n = vi.size();
            for (size_t i = 0; i < n; ++i)
            {
                sp.nLo = n ? static_cast<int>(i * 100 / n) : 0;
                sp.nSpan = n ? static_cast<int>(100 / n) + 1 : 100;
                if (!WIMBridge::ExportImageEx(wFile, vi[i].nIdx, vi[i].sOut,
                        dwCompress, L"", L"", false, &sErr, &sp))
                { sErr = L"[wimgapi] batch export failed: " + sErr; return false; }
            }
            if (pProg) pProg->OnWimProgress(100, 0);
            return true;
        };
    }

    virtual WimJobFn MakeExportSingleJob(
        const std::wstring& sFile,
        const std::wstring& sOut,
        DWORD nIdx,
        DWORD dwCompress) override
    {
        std::wstring wFile = sFile, wOut = sOut;
        return [wFile, wOut, nIdx, dwCompress](
            IToolJobSink* pSink, std::wstring& sErr) -> bool
        { CSinkWimProgress _sinkProg; _sinkProg.pSink = pSink; WIMBridge::IWimProgress* pProg = &_sinkProg;
            if (!WIMBridge::ExportImageEx(wFile, nIdx, wOut,
                    dwCompress, L"", L"", false, &sErr, pProg))
            { sErr = L"[wimgapi] export failed: " + sErr; return false; }
            return true;
        };
    }

    // ---- case 3 追加 ----
    virtual WimJobFn MakeAppendJob(
        const std::wstring& sFile,
        const std::wstring& sTmp,
        const std::wstring& sName,
        const std::wstring& sSrcDir,
        bool bEsd) override
    {
        std::wstring wFile = sFile, wTmp = sTmp, wName = sName, wSrc = sSrcDir;
        return [wFile, wTmp, wName, wSrc, bEsd](
            IToolJobSink* pSink, std::wstring& sErr) -> bool
        { CSinkWimProgress _sinkProg; _sinkProg.pSink = pSink; WIMBridge::IWimProgress* pProg = &_sinkProg;
            bool bOk = false;
            if (bEsd)
            {
                // ESD：先捕获为临时 WIM(LZX)，再以 LZMS 导出追加进目标 ESD
                ::DeleteFileW(wTmp.c_str());
                CScaledWimProgress sp1; sp1.pInner = pProg; sp1.nLo = 0; sp1.nSpan = 50;
                bOk = WIMBridge::CaptureImageEx(wTmp, wSrc, wName, wName,
                        WIM_COMPRESS_LZX, false, &sErr, &sp1);
                if (bOk)
                {
                    CScaledWimProgress sp2; sp2.pInner = pProg; sp2.nLo = 50; sp2.nSpan = 50;
                    bOk = WIMBridge::ExportImageEx(wTmp, 1, wFile,
                            WIM_COMPRESS_LZMS, wName, L"", false, &sErr, &sp2);
                }
                ::DeleteFileW(wTmp.c_str());
                if (!bOk) sErr = L"[wimgapi] append failed: " + sErr;
            }
            else
            {
                // [FIX] 事务化追加：先复制原容器到临时文件，在副本上追加，
                // 成功后再原子替换。原容器在失败时保持不变，重试不会重复追加。
                const std::wstring wCopy = wFile + L".append.tmp";
                ::DeleteFileW(wCopy.c_str());
                if (!::CopyFileW(wFile.c_str(), wCopy.c_str(), FALSE))
                { sErr = L"[wimgapi] append failed: cannot create working copy"; return false; }
                bOk = WIMBridge::AppendImageEx(wCopy, wSrc, wName, &sErr, pProg);
                if (!bOk)
                {
                    ::DeleteFileW(wCopy.c_str());
                    sErr = L"[wimgapi] append failed: " + sErr;
                    return false;
                }
                if (pProg) pProg->OnWimProgress(100, 0);
                return ReplaceWithVerifiedTemp(wCopy, wFile, L"[wimgapi] append", sErr);
            }
            if (bOk && pProg) pProg->OnWimProgress(100, 0);
            return bOk;
        };
    }

    // ---- SWM 拆分 ----
    virtual WimJobFn MakeSplitJob(
        const std::wstring& sFile,
        const std::wstring& sOut,
        DWORD dwPartMB) override
    {
        std::wstring wFile = sFile, wOut = sOut;
        return [wFile, wOut, dwPartMB](
            IToolJobSink* pSink, std::wstring& sErr) -> bool
        { CSinkWimProgress _sinkProg; _sinkProg.pSink = pSink; WIMBridge::IWimProgress* pProg = &_sinkProg;
            if (!WIMBridge::SplitImageEx(wFile, wOut, dwPartMB, &sErr, pProg))
            { sErr = L"[wimgapi] split failed: " + sErr; return false; }
            return true;
        };
    }

    // ---- [R-B80] ISO 透明挂载（逐字搬迁自 Page ApplyNormalizeImage/
    // ImgxBrowseWim 的挂载序列，仅 CDuiString→wstring）----
    virtual bool MountIsoSlot(int nSlot, const std::wstring& sIso,
                              std::wstring& sInner, std::wstring& sErr) override
    {
        if (nSlot != 0 && nSlot != 1) return false;

        // 先前已透明挂载过同一槽位：先卸载，避免重复挂载占用盘符
        if (!m_sSlotIso[nSlot].empty())
        {
            IsoDismount(m_sSlotIso[nSlot]);
            m_sSlotIso[nSlot].clear();
        }

        std::wstring sMount = IsoMount(sIso);
        if (sMount.empty())
        {
            sErr = L"ISO 挂载失败，无法读取内部安装映像。\r\n"
                   L"请确认系统为 Win8 及以上且未精简虚拟磁盘服务。";
            return false;
        }
        sInner = IsoResolveInner(sMount);
        if (sInner.empty())
        {
            IsoDismount(sIso);
            sErr = L"已挂载 ISO，但未找到内部安装映像"
                   L"（sources\\install.wim / install.esd）。";
            return false;
        }
        m_sSlotIso[nSlot] = sIso;
        return true;
    }

    virtual void CloseIso() override
    {
        for (int i = 0; i < 2; ++i)
        {
            if (!m_sSlotIso[i].empty())
            {
                IsoDismount(m_sSlotIso[i]);
                m_sSlotIso[i].clear();
            }
        }
    }

    // ---- Capture（系统备份向导）----
    virtual WimJobFn MakeCaptureJob(
        const std::wstring& sOut,
        const std::wstring& sSrcRoot,
        const std::wstring& sName,
        const std::wstring& sDesc,
        DWORD dwCompress,
        bool bBoot) override
    {
        std::wstring wOut = sOut, wSrc = sSrcRoot, wName = sName, wDesc = sDesc;
        return [wOut, wSrc, wName, wDesc, dwCompress, bBoot](
            IToolJobSink* pSink, std::wstring& sErr) -> bool
        { CSinkWimProgress _sinkProg; _sinkProg.pSink = pSink; WIMBridge::IWimProgress* pProg = &_sinkProg;
            return WIMBridge::CaptureImageEx(wOut, wSrc, wName, wDesc,
                                             dwCompress, bBoot, &sErr, pProg);
        };
    }

    virtual std::wstring SystemDriveRoot() override
    {
        // [R-B90] wimgapi 不展开环境变量：先展开为真实根，取不到回退 C 盘根
        wchar_t sz[16] = { 0 };
        ::GetEnvironmentVariableW(L"SystemDrive", sz, static_cast<DWORD>(_countof(sz)));
        return sz[0] ? (std::wstring(sz) + L"\\") : std::wstring(L"C:\\");
    }

    // ---- [R6-05c] 挂载/卸载（逐字转调 WIMBridge 同名封装）----
    virtual bool MountImage(const std::wstring& sWim, DWORD nIdx,
                            const std::wstring& sDir, bool bReadOnly) override
    {
        return WIMBridge::Mount(sWim, nIdx, sDir,
                                bReadOnly ? WIM_FLAG_MOUNT_READONLY : 0)
               ? true : false;
    }

    virtual bool UnmountImage(const std::wstring& sWim, DWORD nIdx,
                              const std::wstring& sDir, bool bCommit) override
    {
        return WIMBridge::Unmount(sWim, nIdx, sDir, bCommit ? TRUE : FALSE)
               ? true : false;
    }

    // ---- [R7] 能力/元信息修改 ----
    virtual bool WimReady() override { return WIMBridge::IsReady(); }
    virtual void WimInit() override { WIMBridge::Init(); }

    virtual void DetectContainer(const std::wstring& sFile,
                                 TContainerInfo& out) override
    {
        WIM_CONTAINER_INFO ci = {};
        CWIMHelper::DetectContainer(sFile, ci);
        // WIM_CONTAINER_KIND 与 EImgContainerKind 数值一一对应
        out.nKind       = static_cast<EImgContainerKind>(ci.kind);
        out.nImageCount = ci.dwImageCount;
        out.nPartNumber = ci.dwPartNumber;
        out.nTotalParts = ci.dwTotalParts;
    }

    virtual bool ProbeWimArchitectures(const std::wstring& sWim,
                                       std::wstring& sSummary,
                                       uint32_t& nCount,
                                       uint32_t& nArchFound) override
    {
        nCount = 0;
        nArchFound = 0;
        sSummary.clear();
        if (!WIMBridge::IsReady()) WIMBridge::Init();

        // 架构值 → 名称（WIM_XML <ARCH>：0=x86 9=amd64 12=arm64 14=arm64ec）
        auto ArchName = [](DWORD arch) -> std::wstring
        {
            switch (arch)
            {
            case 0:  return L"x86";
            case 9:  return L"x64 (amd64)";
            case 12: return L"ARM64";
            case 14: return L"ARM64EC";
            default:
            {
                return L"0x" + std::to_wstring(static_cast<unsigned long>(arch)) + L" (unknown)";
            }
            }
        };

        // OpenForRead 内部已设 %TEMP%\wimgapi 临时目录（只读 ISO 上
        // WIMLoadImage 否则报 1632）。
        ISWimGuard hWim(CWIMHelper::OpenForRead(sWim));
        if (hWim == INVALID_HANDLE_VALUE) return false;

        DWORD nCnt = CWIMHelper::GetImageCount(hWim);
        nCount = nCnt;
        if (nCnt == 0) return false;   // ISWimGuard 析构自动关闭

        std::set<std::wstring> archSet;   // 需 <set>（经 WIMHelper.h 间接不可靠→显式包含）
        DWORD nArch = 0;
        for (DWORD i = 1; i <= nCnt; ++i)
        {
            ISImgGuard hImg(WIMLoadImage(hWim, i));
            if (hImg == INVALID_HANDLE_VALUE) continue;
            IMAGE_INFO info = {};
            if (CWIMHelper::GetImageInformation(hImg, &info))
            {
                ++nArch;
                archSet.insert(ArchName(info.dwArchitecture));
            }
            // ISImgGuard 析构自动关闭
        }
        // ISWimGuard 析构自动关闭
        nArchFound = nArch;
        if (archSet.empty()) return false;

        bool bFirst = true;
        for (std::set<std::wstring>::const_iterator it = archSet.begin();
             it != archSet.end(); ++it)
        {
            if (!bFirst) sSummary += L" + ";
            bFirst = false;
            sSummary += *it;
        }
        return true;
    }

    virtual WimJobFn MakeMetaRenameJob(
        const std::wstring& sFile,
        const std::wstring& sTmp,
        const std::wstring& sName,
        const std::wstring& sDesc,
        DWORD dwCompress,
        const std::vector<TMetaItem>& vItemsIn) override
    {
        std::vector<TMetaItem> vItems = vItemsIn;
        std::wstring wFile = sFile, wTmp = sTmp, wName = sName, wDesc = sDesc;
        return [wFile, wTmp, wName, wDesc, dwCompress, vItems]
        (IToolJobSink* pSink, std::wstring& sErr) -> bool
        { CSinkWimProgress _sinkProg; _sinkProg.pSink = pSink; WIMBridge::IWimProgress* pProg = &_sinkProg;
            ::DeleteFileW(wTmp.c_str());
            CScaledWimProgress sp; sp.pInner = pProg;
            const size_t n = vItems.size();
            for (size_t i = 0; i < n; ++i)
            {
                sp.nLo = n ? static_cast<int>(i * 100 / n) : 0;
                sp.nSpan = n ? static_cast<int>(100 / n) + 1 : 100;
                if (!WIMBridge::ExportImageEx(wFile, static_cast<DWORD>(vItems[i].nIdx), wTmp,
                        dwCompress,
                        vItems[i].bSel ? wName : L"",
                        vItems[i].bSel ? wDesc : L"",
                        false, &sErr, &sp))
                { sErr = L"[wimgapi] rename failed: " + sErr; return false; }
            }
            if (pProg) pProg->OnWimProgress(100, 0);
            if (!::MoveFileExW(wTmp.c_str(), wFile.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            { sErr = L"[wimgapi] metadata replace failed"; return false; }
            return true;
        };
    }

    // ---- [R7] ESD 自主解密 ----
    virtual bool DecryptEsd(const std::wstring& sSrc,
                            const std::wstring& sDst,
                            const std::vector<std::wstring>& vKeys,
                            std::wstring& sErr) override
    {
        return EsdDecryptor::DecryptToFile(sSrc, sDst, vKeys, &sErr);
    }

    // ---- [R7] ISO 生成 ----
    virtual bool CreateIsoImage(const TIsoCreateArgs& a,
                                IToolJobSink* pSink,
                                std::wstring& sErr) override
    {
        IsoWriter::Options io;
        io.sVolumeLabel  = a.sVolumeLabel;
        io.sBiosBootFile = a.sBiosBootFile;
        io.sUefiBootFile = a.sUefiBootFile;

        // 整数百分比去重后回调（逐字搬自 Page IsoCreateThreadProc）
        DWORD dwLastPct = 0xFFFFFFFF;
        IsoWriter::ProgressFn fnProg =
            [&](ULONGLONG ullDone, ULONGLONG ullTotal)
        {
            if (ullTotal == 0 || !pSink) return;
            DWORD v = static_cast<DWORD>((ullDone * 100ULL) / ullTotal);
            if (v > 100) v = 100;
            if (v == dwLastPct) return;
            dwLastPct = v;
            pSink->OnProgress(static_cast<int>(v), std::wstring());
        };

        bool bOk = false;
        try
        {
            bOk = IsoWriter::CreateImage(a.sSrc, a.sOut, io, &sErr, fnProg);
        }
        catch (...)
        {
            bOk = false;
            if (sErr.empty()) sErr = L"ISO 生成任务发生异常";
        }
        return bOk;
    }
};

} // anonymous namespace

std::unique_ptr<IIsoImageService> CreateIsoImageService()
{
    // [C++17 阶段3] 工厂所有权移交调用方（unique_ptr 管理，无裸 delete）
    return std::make_unique<CIsoImageService>();
}

} // namespace toolsvc
