#pragma once

// ============================================================
// IIsoImageService.h —— [R6-04] 映像编辑（wimgapi）作业服务接口。
//
// 职责边界：
//   - 服务只产出"后台作业闭包" WimJobFn（纯 wimgapi/文件操作），
//     不建线程、不碰控件、不弹 MessageBox、不调 StringMgr；
//   - Page 保留交互确认/文件选择、RunImageXJob 的进度盾+工作线程+
//     OnJobDone 弹窗（绞杀式：UI 编排留页，作业体下沉）。
//
// 依赖：WIMHelperBridge.h 是 wstring/wimgapi 非 UI 原语头，
//       不含 UIlib.h / C*UI（满足 R7 服务层契约）。
// ============================================================

#include <windows.h>          // [R16][T5] DWORD（作业签名/DTO；服务层允许 windows 系统类型）
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ToolTypes.h"          // IToolJobSink（零 UI 作业回调）

namespace toolsvc {

// [R7] 后台映像作业：进度统一经 IToolJobSink 回传，服务内部再适配
// WIMBridge::IWimProgress；Page 不再直接依赖 WIMHelperBridge.h。
using WimJobFn = std::function<bool(IToolJobSink* pSink,
                                    std::wstring& sErr)>;

// 元信息修改作业：逐项导出重建（bSel=true 时替换名称/说明）。
struct TMetaItem
{
    uint32_t nIdx = 0;
    bool     bSel = false;
};

// case1 可启动重建：全索引导出，选中项打可启动标记。
struct TBootItem
{
    DWORD       nIdx;
    bool        bBoot;
};

// case2 批量导出：索引 -> 独立输出文件。
struct TExportItem
{
    DWORD           nIdx;
    std::wstring    sOut;
};

class IIsoImageService
{
public:
    virtual ~IIsoImageService() {}

    // ---- ImageXAction case 0：删除映像 ----
    // ESD 不支持物理删除：保留索引逐个 LZMS 导出重建再覆盖原文件
    // （单删/批删共用同一作业体，仅 vKeep 构成不同）。
    virtual WimJobFn MakeDeleteEsdRebuildJob(
        const std::wstring& sFile,
        const std::wstring& sTmp,
        const std::vector<DWORD>& vKeep) = 0;
    // 单删 ESD：作业体同上，仅错误标签保持基线 "single delete failed"。
    virtual WimJobFn MakeDeleteEsdRebuildSingleJob(
        const std::wstring& sFile,
        const std::wstring& sTmp,
        const std::vector<DWORD>& vKeep) = 0;
    // WIM 批量删除：大索引先删避免索引号漂移（vDel 已降序）。
    virtual WimJobFn MakeDeleteWimBatchJob(
        const std::wstring& sFile,
        const std::vector<DWORD>& vDel) = 0;
    // WIM 单索引物理删除。
    virtual WimJobFn MakeDeleteWimSingleJob(
        const std::wstring& sFile, DWORD nIdx) = 0;

    // ---- case 1：可启动标记（全量导出重建覆盖原文件）----
    virtual WimJobFn MakeBootableRebuildJob(
        const std::wstring& sFile,
        const std::wstring& sTmp,
        const std::vector<TBootItem>& vItems,
        DWORD dwCompress) = 0;

    // ---- case 2：导出 ----
    virtual WimJobFn MakeExportBatchJob(
        const std::wstring& sFile,
        const std::vector<TExportItem>& vItems,
        DWORD dwCompress) = 0;
    virtual WimJobFn MakeExportSingleJob(
        const std::wstring& sFile,
        const std::wstring& sOut,
        DWORD nIdx,
        DWORD dwCompress) = 0;

    // ---- Capture（系统备份向导）----
    // 整盘捕获为 WIM/ESD（bBoot=可启动标记；dwCompress=LZX/LZMS）。
    virtual WimJobFn MakeCaptureJob(
        const std::wstring& sOut,
        const std::wstring& sSrcRoot,
        const std::wstring& sName,
        const std::wstring& sDesc,
        DWORD dwCompress,
        bool bBoot) = 0;
    // [R-B90] 展开 %SystemDrive% 为真实根（wimgapi 不展开环境变量），
    // 取不到时回退 C:\。
    virtual std::wstring SystemDriveRoot() = 0;

    // ---- case 3：追加映像（ESD 先捕获临时 WIM 再 LZMS 导出；WIM 直接追加）----
    virtual WimJobFn MakeAppendJob(
        const std::wstring& sFile,
        const std::wstring& sTmp,
        const std::wstring& sName,
        const std::wstring& sSrcDir,
        bool bEsd) = 0;

    // ---- ImageXSplit：SWM 分卷（WIMSplitFile 直连）----
    virtual WimJobFn MakeSplitJob(
        const std::wstring& sFile,
        const std::wstring& sOut,
        DWORD dwPartMB) = 0;

    // ---- [R-B80] ISO 透明挂载所有权（2026-10-13 自 Page 迁入）----
    // 挂载 sIso 并解析内部 install.wim/esd：成功填 sInner，且服务持有该
    // 槽位的 ISO 路径（同槽重复调用先卸载旧挂载）；失败回填 sErr、不持有。
    // 槽位：0=Apply 释放映像向导，1=ImageX 编辑面板。
    virtual bool MountIsoSlot(int nSlot, const std::wstring& sIso,
                              std::wstring& sInner, std::wstring& sErr) = 0;
    // 卸载服务仍持有的全部 ISO 挂载（弹窗析构时调用，保证 R-B80 不回退）。
    virtual void CloseIso() = 0;

    // ---- [R6-05c] 映像挂载/卸载（wimgapi 直连，替代 dism /Mount-Wim）----
    // 只读内部传 WIM_FLAG_MOUNT_READONLY。
    virtual bool MountImage(const std::wstring& sWim, DWORD nIdx,
                            const std::wstring& sDir, bool bReadOnly) = 0;
    // bCommit=TRUE 提交修改后卸载，FALSE 放弃修改。
    virtual bool UnmountImage(const std::wstring& sWim, DWORD nIdx,
                              const std::wstring& sDir, bool bCommit) = 0;

    // ---- [R7] 能力/元信息修改 ----
    // wimgapi 是否可用（Page 各入口的 IsReady 预检统一走这里）。
    virtual bool WimReady() = 0;
    // 显式初始化 wimgapi（对应旧 WIMBridge::Init）。
    virtual void WimInit() = 0;
    // 魔数级容器探测（不依赖扩展名，对应 CWIMHelper::DetectContainer）。
    virtual void DetectContainer(const std::wstring& sFile,
                                 TContainerInfo& out) = 0;
    // WIMGetImageCount + 逐索引 WIMGetImageInformation 判架构集合；
    // sSummary 形如 "x86 + x64 (amd64)"。false=无法读取或无架构信息。
    virtual bool ProbeWimArchitectures(const std::wstring& sWim,
                                       std::wstring& sSummary,
                                       uint32_t& nCount,
                                       uint32_t& nArchFound) = 0;
    // 逐索引导出重建修改元信息（bSel 项替换名称/说明），完成后覆盖原文件。
    virtual WimJobFn MakeMetaRenameJob(
        const std::wstring& sFile,
        const std::wstring& sTmp,
        const std::wstring& sName,
        const std::wstring& sDesc,
        DWORD dwCompress,
        const std::vector<TMetaItem>& vItems) = 0;

    // ---- [R7] ESD 自主解密（EsdDecryptor 原语，工作线程内调用）----
    virtual bool DecryptEsd(const std::wstring& sSrc,
                            const std::wstring& sDst,
                            const std::vector<std::wstring>& vBase64Keys,
                            std::wstring& sErr) = 0;

    // ---- [R7] ISO 生成（IsoWriter 原语，工作线程内调用）----
    struct TIsoCreateArgs
    {
        std::wstring sSrc;         // 源目录树
        std::wstring sOut;         // 输出 .iso
        std::wstring sVolumeLabel;
        std::wstring sBiosBootFile; // etfsboot.com（空=无）
        std::wstring sUefiBootFile; // efisys.bin（空=无）
    };
    // 字节进度经 pSink->OnProgress 去重回传；失败填 sErr。
    virtual bool CreateIsoImage(const TIsoCreateArgs& a,
                                IToolJobSink* pSink,
                                std::wstring& sErr) = 0;

    // ---- [R16][T4] 弹窗入参持有（ESD 解密向导 / PE 映像设置）----
    // EToolKind::TK_ESDWIZ/EToolKind::TK_PECFG 无独立 IToolService 子类，其注入数据（解密密钥、
    // 外壳预填 ESD 路径、挂载目录预填）由映像服务持有，Page 经访问器读取。
    virtual void Open(const ToolOpenArgs& args) = 0;
    virtual const std::vector<std::wstring>& EsdKeys() const = 0;
    virtual bool HasInitialEsd() const = 0;
    virtual const std::wstring& InitialEsdFile() const = 0;
    virtual const std::wstring& InitialPeDir() const = 0;
};

// 工厂：服务持有 ISO 挂载槽位状态，Page 构造时持有一个；
// 析构（或显式 CloseIso）卸载仍挂着的 ISO（R-B80）。
std::unique_ptr<IIsoImageService> CreateIsoImageService();

} // namespace toolsvc
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
