#pragma once

#include "UIlib.h"
#include "NotifyBox.h"   // [R-AUTOTEST] ToolSql::UnattendedSilent（HostAlert 自动应答）
#include <vector>
#include <string>   // [R-B53] m_mount.vMountedDirs（std::wstring）
#include <memory>   // [R6] std::unique_ptr 持有 IToolService

using namespace DuiLib;

#include "ToolService/IIsoImageService.h"  // [R7] toolsvc::WimJobFn（零 UI 头）
#include "DependencyGraphService.h"   // [R-B25] DepGraphData（宿主 Page_Appx/Page_Startup 等经本头共享，保留传递）

// ============================================================
// 工具箱各工具的自定义模态弹窗（参照原版 Dism++ 运作机制，
// UI 风格保持 DuiLib/Win8Metro 原样；所有机制真实运行）。
// ============================================================

// 弹窗种类（与工具箱 toolitem_N 序号一致）
enum class EToolDlgKind : int
{
    TD_CAPTURE = 0,  // 另存为映像
    TD_APPLY   = 1,  // 释放映像
    TD_ACCOUNT = 2,  // 账户管理
    TD_IMAGEX  = 7,  // 编辑映像文件
    TD_ISO     = 9,  // ISO生成器
    TD_DETAILS = 13,  // 详细设置（选项 → 详细设置；原版 DLGTEMPLATEEX ID133）
    TD_LOGVIEW = 14, // 查看日志（原版 DLGTEMPLATEEX ID134）
    TD_DEPGRAPH = 15, // [R-B25] 依赖图可视化（Appx/Service/Startup/Tools/Capability 共用）
    TD_ESDWIZ   = 16, // [R-B35] 独立 ESD 解密向导（选 esd→显示索引→选密钥→导出）
    TD_LINKLIST = 17, // [R-B44] 超链接列表（原版 DLGTEMPLATEEX ID135，列表 + 5 个 SysLink）
    TD_DUALPATH = 18, // [R-B44] 双路径选择器（原版 DLGTEMPLATEEX ID137，Edit×2 + 浏览×2）
    TD_DELCONFIRM = 19, // [R-B44] 删除确认列表（原版 DLGTEMPLATEEX ID140，危险项清单）
    TD_MOUNT      = 20, // [R-B53] 挂载 12 步向导（原版 PEImageManager，清单维度 4 缺失 2）
    TD_PECFG      = 21, // [R-B79] PE 映像设置（ScratchSpace/TargetPath，PEImageManager.xml 两配置项）
    TD_BOOTFIX    = 22, // 引导修复（原版"引导修复-UEFI/BIOS"：分区表选中目标分区，bcdboot 真修复）
};

// WIM/ESD 中的一个映像索引（宿主通过 dism /Get-WimInfo 真实解析）
struct TDialogWimIndex
{
    TDialogWimIndex() : nIndex(0) {}

    int        nIndex;
    CDuiString sName;     // 映像名称（原始，通常英文）
    CDuiString sDesc;     // 映像说明（原始，通常英文）
    CDuiString sEdition;  // 映像标志（EDITIONID，如 EnterpriseS / Professional）
    CDuiString sVersion;  // 系统版本（<VERSION>，如 10.0.26100.3775）
    CDuiString sArch;     // 系统体系（x86/x64/arm64/arm64ec）
    CDuiString sProduct;  // 产品分析（ResolveProduct 一句话汇总）
    CDuiString sCreated;  // 创建日期
    CDuiString sSize;     // 展开空间（字节数字串，Win7 映像无 Version 字段）
};

// 引导修复弹窗中的一个分区行（物理磁盘 IOCTL 真实枚举）
struct TBfPart
{
    CDuiString sPos;        // 位置：硬盘X 分区Y（D:）
    CDuiString sName;       // 名称：EFI / MSR / 卷标-NTFS
    CDuiString sSize;       // 大小：300 MB / 238 GB
    CDuiString sType;       // 类型：GPT / MBR
    CDuiString sVolumeGuid; // 卷 GUID 路径 \\?\Volume{GUID}\（无卷为空）
    CDuiString sFs;         // 文件系统：FAT32 / NTFS ...
    DWORD      dwDisk;
    DWORD      dwPart;
    bool       bEfi;
    bool       bMsr;
    bool       bPrimary;    // MBR：主分区
    bool       bActive;     // MBR：活动分区
};

// 弹窗依赖的宿主能力，由 CFrameWnd 真实实现（禁止模拟）
class IToolHost
{
public:
    virtual ~IToolHost() {}
    virtual HWND       HostHwnd() = 0;
    virtual int        HostDPI() = 0;
    virtual bool       HostOpenFile(CDuiString& sResult, const wchar_t* pszFilter, const wchar_t* pszTitle) = 0;
    virtual bool       HostSaveFile(CDuiString& sResult, const wchar_t* pszFilter, const wchar_t* pszTitle,
                                    const wchar_t* pszDefName = nullptr) = 0;
    virtual bool       HostBrowseFolder(CDuiString& sResult, const wchar_t* pszTitle) = 0;
    virtual bool       HostQueryWim(const CDuiString& sWim,
                                    std::vector<TDialogWimIndex>& vOut,
                                    CDuiString* psErr = nullptr) = 0;
    // 新建临时工作目录、写入批处理并（经 UAC）真实执行；
    // phProcess 非空时接收 cmd 进程句柄（调用方负责 CloseHandle），
    // 弹窗据此在外部命令行窗口关闭后自动解除操作锁定。
    virtual bool       HostRunBat(const wchar_t* pszBatName, const CDuiString& sBody,
                                  HANDLE* phProcess = nullptr) = 0;
    virtual void       HostLog(const CDuiString& sLine, int nLevel = 0) = 0;

    // 模态信息提示（仅“知道了”），供 Page/Service 替代裸 MessageBox；
    // 危险操作的“是/否”仍走 HostConfirm（默认按钮固定在“否”）。
    // [R-AUTOTEST] 无人值守下告知框直接返回（不弹窗、不阻塞自动化流）。
    virtual void       HostAlert(const wchar_t* pszTitle, const wchar_t* pszText,
                                 UINT nIcon = MB_ICONINFORMATION)
    {
        if (ToolSql::UnattendedSilent()) return;
        ::MessageBoxW(HostHwnd(), pszText, pszTitle, MB_OK | nIcon);
    }

    // [R19] 宿主统一交互能力（默认空实现，弹窗按需调用，不强制接入）：
    //   气泡通知 / 模态确认 / 进度条 (nMax<=0 隐藏)
    virtual void       HostNotify(const wchar_t* /*pszTitle*/, const wchar_t* /*pszText*/,
                                  int /*nIcon*/ = 0) {}
    virtual bool       HostConfirm(const wchar_t* pszTitle, const wchar_t* pszText)
    {
        return ::MessageBoxW(HostHwnd(), pszText, pszTitle,
                             MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2) == IDOK;
    }
    virtual void       HostProgress(int /*nVal*/, int /*nMax*/) {}
};

class CTdlgCheck;

// [R6] ToolService 层前向声明（具体头只在 .cpp include，UI 头不扩散服务依赖）
// [L-10] TLogLine 是查看器的成员容器元素，必须完整定义（仅前置声明无法实例化 vector）
#include "ToolService/LogDepGraphService.h"

namespace toolsvc {
    class IToolService;
    class IToolServiceHost;
    class CLogDepGraphService;
    class CListService;
    class CConfirmService;
    class IIsoImageService;   // [R6-04] wimgapi 映像编辑作业工厂
    class IWizardService;     // [R6-05] 六大向导业务
    struct TMountedRow;       // [R6-05c] 挂载枚举行（定义在 ToolTypes.h）
}
// [R16][T6] UI 侧宿主适配器桥已取消：CFrameWnd 直接实现 toolsvc::IToolServiceHost。

class CToolDialogPage : public WindowImplBase
{
public:
    // [B4] marquee 往复扫描动画定时器（'TP'）
    static constexpr UINT TASK_ANIM_TIMER_ID = 0x5450;
    // 查看日志：窗口显示后延时一次性加载正文（避免模态创建期同步读盘+排版卡死）
    static constexpr UINT LOGVIEW_LOAD_TIMER_ID = 0x5451;
    // [L-10] 自动跟随：查看日志期间定时增量刷新（仅 logview 页生效）
    static constexpr UINT LOGVIEW_FOLLOW_TIMER_ID = 0x5452;

    CToolDialogPage(IToolHost* pHost, EToolDlgKind nKind);
    ~CToolDialogPage();   // [R-B80] 关闭时卸载 ImageX 透明挂载的 ISO

    const wchar_t*    GetWindowClassName() const;
    CDuiString GetSkinFile();
    CDuiString GetSkinFolder();
    void       InitWindow();
    void       Notify(TNotifyUI& msg);
    LRESULT    HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam);

    // 创建前由宿主调用：弹窗跟随父窗口 DPI 缩放
    void SetDialogDPI(int nDpi) { m_nDPI = nDpi; }

    // [R16][T4] 统一入参注入通路（替代旧 SetDepGraphData/SetEsdKeys/
    // SetInitialEsdFile/SetInitialPeDir/SetLinkItems/SetDualPath/
    // SetDelConfirmItems）：宿主在 Create 前组装 ToolOpenArgs 调用本方法，
    // 内部转发 m_pSvc->Open；ESDWIZ/PECFG 等无业务服务的 kind 由映像服务持有。
    void Open(const toolsvc::ToolOpenArgs& args);

private:
    // 一列的定义；nWidth < 0 表示弹性填充
    struct TGridCol
    {
        TGridCol(const wchar_t* pszText, int nW)
            : pszTitle(pszText), nWidth(nW) {}
        const wchar_t* pszTitle;
        int     nWidth;
    };

    void BuildChrome();      // 标题栏 + 关闭按钮
    void BuildBodyShell();   // 内容容器（统一边距）
    void BuildContent();     // 按种类分发

    // ---- 通用控件工厂 ----
    CTextUI*   MkText(const wchar_t* pszText, int nFont, DWORD dwColor);
    CButtonUI* MkBtn(const wchar_t* pszText, const wchar_t* pszName, bool bPrimary);
    CEditUI*   MkEdit(const wchar_t* pszName);
    CComboUI*  MkCombo(const wchar_t* pszName, int nDropH);
    CVerticalLayoutUI* MkGrid(const std::vector<TGridCol>& cols,
                              int nTotalRows, int nRowH, bool bSelectable);
    void AddBottomRow(CButtonUI* pOk, CButtonUI* pCancel);

    // 可选表格行（gridrow_0..）
    void SetGridSelection(int nRow);
    void OnGridRow(const CDuiString& sName);

    // ---- 各工具页面 ----
    void BuildCapture();
    void BuildApply();
    // [R16][T10] BuildCapture/BuildApply 超长函数机械拆分（零行为改动）
    void CapFillSystemInfo();               // 系统名称/说明/标志推断
    void CapBuildInfoGrid();                // 信息表格与行标签
    CVerticalLayoutUI* ApplyBuildInputBox(); // Apply 输入区边框盒（含索引/路径/复选）
    CHorizontalLayoutUI* AoMkBrowseRow(const wchar_t* pszEdit, const wchar_t* pszBtn, const wchar_t* pszText);
    CHorizontalLayoutUI* ApplyBuildChkRow();
    void BuildAccount();
    void BuildImageX();
    void BuildImageXMeta();   // ImageX → 编辑映像元信息
    // [R16][T10] BuildImageX 超长函数机械拆分（零行为改动）
    void ImxBuildBatchBox();  // 批量模式面板（含 SWM 分卷行）
    void ImxBuildLinkRow();   // 底部 6 个操作链接行
    void BuildIso();
    void BuildDetails();      // 详细设置（原版 DLGTEMPLATEEX ID133，含 5 选项）
    void DetailsOk();         // 详细设置 → 写回 config.ini [Advanced] 段
    void BuildLogView();      // 查看日志（原版 DLGTEMPLATEEX ID134）
    void LogViewRefresh();    // 重新读取 action_log.jsonl 填充正文
    void LogViewOpenFolder(); // 打开日志所在目录
    void LogViewClear();      // 清空日志文件 + 正文
    void LogViewCopySelection();          // 复制日志选中段落
    void LogViewSelectAll();              // 全选日志正文
    void LogViewKeepTail();               // 聚焦时兜底把插入点钉在末尾
    // [L-10] 查看器交互：级别过滤 / 关键字搜索 / 自动跟随 / 导出
    void LogViewExport();                 // 导出当前（已过滤）内容到文件
    void LogViewToggleFollow();           // 切换自动跟随
    void LogViewFillView();               // 按当前过滤/搜索条件重填正文
    void LogViewContextMenu(POINT ptScreen); // 日志正文右键菜单（屏幕坐标）
    CDuiString LogViewPath(); // 返回 action_log.jsonl 完整路径
    // [R-B35] 独立 ESD 解密向导（EToolDlgKind::TD_ESDWIZ）
    void BuildEsdWiz();       // 构建解密向导页
    void EsdWizBrowse();      // 浏览源 ESD
    void EsdWizReload(bool bAlert);  // 读取索引并刷新表格
    void EsdWizBuildKeyRow();        // [R16][T10] 自 BuildEsdWiz 机械抽取
    void EsdWizFillRows();    // 用 m_esd.vIdx 填充表格
    void EsdWizExport();      // 生成 dism /Export-Image 导出

    // [R-B25] 依赖图（EToolDlgKind::TD_DEPGRAPH）
    void BuildDepGraph();     // 构建缩进依赖树
    void DepGraphRefresh();   // 按 m_depData 刷新列表
    void DepGraphCopyAll();   // 复制全部依赖树到剪贴板

    // [R-B44] 原版 DLGTEMPLATEEX 三模态（ID135/ID137/ID140）
    CTextUI* MkSysLink(const wchar_t* pszText, const wchar_t* pszName);  // SysLink 语义链接
    void BuildLinkList();     // ID135 列表 + 5 个 SysLink
    void LinkListOpen(const CDuiString& sName);  // 点击链接打开目标
    void BuildDualPath();     // ID137 双路径选择器
    void DualBrowse(int nIdx);
    void DualOk();            // 校验并收集双路径，Close(1)
    void BuildDelConfirm();   // ID140 删除确认列表
    void DelConfirmOk();      // 标记确认并 Close(1)

    // [R-B53] 挂载 12 步向导（原版 PEImageManager，清单维度 4 缺失 2）
    void BuildMount();        // 构建挂载向导（挂载点 + 只读 + 已挂载列表）
    void MountBrowse();       // 浏览 WIM/ESD 源文件
    void MountReload(bool bAlert); // 读取索引刷新表格
    void MountFillRows();     // 用 m_mount.vIdx 填充表格
    void MountOk();           // 校验并挂载；挂载失败跳过异常挂载点
    void MountUnmount();      // 卸载选中的已挂载目录
    void RefreshMountedList();// 刷新已挂载下拉（MountLedger 枚举）
    void MountOpenPeCfg();    // [R-B79] 打开 PE 映像设置对话框（预填当前挂载点）

    // [R-B79] PE 映像设置（EToolDlgKind::TD_PECFG）：dism /Get|Set-ScratchSpace、/Get|Set-TargetPath
    void BuildPeCfg();
    void PeCfgBrowse();       // 浏览挂载目录
    void PeCfgRefresh();      // 刷新当前 ScratchSpace/TargetPath
    void PeCfgApply();        // 应用两项设置

    // 引导修复（EToolDlgKind::TD_BOOTFIX，原版"引导修复-UEFI/BIOS"）
    bool EnumerateBfParts();      // 物理磁盘/分区/卷 真实枚举
    void BuildBootFix();          // 构建弹窗（提示 + 分区表 + 三按钮）
    void BfRefreshTarget();       // 按当前选中刷新首行目标描述
    void BootFixToggleBasic();    // 基本/更多 模式切换
    void BootFixOk();             // 校验 → bcdboot 真修复
    // [R6-05h] 旧 BfCleanupAfterTask（bat 时代临时盘符回收/结果文件）已随
    // bcdboot 直连下沉删除；盘符回收在 IWizardService::BfFixBoot 内同步完成。

    // ---- 事件 ----
    void OnClickName(const CDuiString& sName);
    void OnSelectChangedName(const CDuiString& sName);
    // [R16][T10] 回车触发当前页"确定"按钮（自 HandleMessage 机械抽取）
    bool TryEnterOk();

    // [GC-02] 按 kind 拆分的点击处理器（原 OnClickName 内 15 路 case 体，
    // 经 kKindClickTab 分发表调用；新增弹窗只加一个处理器 + 表中一行）
    void ClickCapture(const CDuiString& sName);
    void ClickApply(const CDuiString& sName);
    void ClickAccount(const CDuiString& sName);
    void ClickImageX(const CDuiString& sName);
    void ClickIso(const CDuiString& sName);
    void ClickDetails(const CDuiString& sName);
    void ClickLogView(const CDuiString& sName);
    void ClickDepGraph(const CDuiString& sName);
    void ClickEsdWiz(const CDuiString& sName);
    void ClickLinkList(const CDuiString& sName);
    void ClickDualPath(const CDuiString& sName);
    void ClickDelConfirm(const CDuiString& sName);
    void ClickMount(const CDuiString& sName);
    void ClickPeCfg(const CDuiString& sName);
    void ClickBootFix(const CDuiString& sName);

    // [GC-02] kind 分发表（静态成员可在表中取私有 Build/Click 成员指针；
    // 表体定义在 ToolDialog.cpp）
    struct TKindSizeEnt  { EToolDlgKind nKind; int cx; int cy; };
    struct TKindEditEnt  { EToolDlgKind nKind; const wchar_t* psz; };
    struct TKindTitleEnt { EToolDlgKind nKind; const wchar_t* psz; bool bLiteral; };
    using PfnKindBuild = void (CToolDialogPage::*)();
    using PfnKindClick = void (CToolDialogPage::*)(const CDuiString&);
    struct TKindBuildEnt { EToolDlgKind nKind; PfnKindBuild pfnBuild; };
    struct TKindClickEnt { EToolDlgKind nKind; PfnKindClick pfnClick; };
    static const TKindSizeEnt  kKindSizeTab[15];
    static const TKindEditEnt  kKindFirstEditTab[7];
    static const TKindEditEnt  kKindOkBtnTab[5];
    static const TKindTitleEnt kKindTitleTab[11];
    static const TKindBuildEnt kKindBuildTab[15];
    static const TKindClickEnt kKindClickTab[15];

    // 另存为映像
    void CaptureBrowse();
    void CaptureOk();

    // 释放映像
    void ApplyBrowseWim();
    void ApplyBrowseDir();
    void ApplyIndexChanged();
    void ApplyOk();

    // 账户管理
    bool LoadAccounts();
    void AccountEnable();
    void AccountClearPwd();

    // 编辑映像文件
    void ImageXBrowse();
    bool ImageXReload(bool bAlert);   // 重新读取 m_imgx.sFile 并刷新表格
    void ImageXFillRows();            // 用 m_imgx.vIdx 填充表格
    void ImageXAction(int nAction);
    void ImageXSplit();               // [P0] 拆分 SWM（dism /Split-Image）
    void ImageXMetaOk();

    // [R14-04] ImageXAction 拆解：上下文结构 + 批量解析 + case 0/1/2/3/4
    struct SImgxCtx
    {
        int         nAction;
        bool        bBatch;
        std::vector<int> vBatchIdx;
        CDuiString  sFile;
        CDuiString  sDir;
        CDuiString  sExt;
        bool        bEsd;
        CDuiString  sComp;
        int         nCount;
        int         nIndex;
        std::vector<DWORD> vAllIdx;
    };
    bool ImxPrepare(int nAction, SImgxCtx& c);          // 前置校验 + 批量解析 + 快照
    void ImxParseBatch(SImgxCtx& c);                    // [R16][T10] 自 ImxPrepare 机械抽取
    bool ImxCaseDelete(SImgxCtx& c);                    // case 0
    bool ImxDeleteBatch(SImgxCtx& c, const CDuiString& sTitleImgx); // [R16][T10] 自 ImxCaseDelete 机械抽取
    bool ImxCaseBootable(SImgxCtx& c);                  // case 1
    bool ImxCaseExport(SImgxCtx& c);                    // case 2
    bool ImxExportBatch(SImgxCtx& c, const CDuiString& sTitleImgx, CDuiString& sOut); // [R16][T10] 自 ImxCaseExport 机械抽取
    bool ImxMetaBatchPhase(const CDuiString& sName, int nCount,
                           std::vector<int>& vBatchIdx, bool& bBatchMeta); // [R16][T10] 自 ImageXMetaOk 机械抽取
    bool ImxCaseAppend(SImgxCtx& c);                    // case 3
    bool ImxCaseMeta(SImgxCtx& c);                      // case 4

    // ISO生成器
    void IsoBrowseSrc();
    void IsoBrowseIso();
    void IsoOk();
    bool IsoPrecheck9660(const CDuiString& sSrc); // [R16][T10] 自 IsoOk 机械抽取
    bool BfValidateFirmwareMatch(const TBfPart& pt, bool bUefi); // [R16][T10] 自 BootFixOk 机械抽取

    // [B4] 长耗时 wimgapi 原生作业期间的进度展示：原版 Dism++ 路线（PE 资源
    // 11 个对话框无任何 progress 控件，进度在 DuiLib 界面内呈现）——内嵌
    // 任务进度面板（覆盖内容区，字体/缩放全走 DuiLib，杜绝原生窗高 DPI 乱码）。
    // pszText: 阶段提示；bMarquee: 无百分比来源时跑往复扫描动画。
    void BuildTaskPanel();
    void ShowTaskPanel(const wchar_t* pszText, bool bMarquee);
    void HideTaskPanel();
    void SetTaskStage(const wchar_t* pszText);
    void SetTaskProgress(int nPercent);
    void BeginTaskShield(const wchar_t* pszText = nullptr, bool bMarquee = false);
    void UpdateTaskShield(int nPercent, const wchar_t* pszText = nullptr);
    void EndTaskShield();

    // [2026-10-12] 后台映像任务统一框架（std::thread + detach + PostMessage 回 UI）：
    // 工作线程内执行 fn(pProg,sErr)，wimgapi 0-100% 实时进进度窗；
    // 完成后回 UI 线程弹成功/失败 MessageBox、按 bDirtyOnOk 重读 ImageX、
    // 按 bCloseOnOk 关闭弹窗。fn 内禁止直接操作 DuiLib 控件。
    // [R7] 作业闭包类型直接复用服务层 toolsvc::WimJobFn（IToolJobSink 回调）
    using TImgxJobFn = toolsvc::WimJobFn;
    void RunImageXJob(CDuiString sText, CDuiString sTitle, CDuiString sOkMsg,
                      bool bDirtyOnOk, bool bCloseOnOk, TImgxJobFn fn,
                      int nKind = 2);
    void OnJobDone(LPARAM lParam);

    static CDuiString GetRegSZ(HKEY hRoot, const wchar_t* pszSub, const wchar_t* pszValue);

private:
    // ============================================================
    // [R16][T10] 成员瘦身：各 kind 散落成员归并为 8 个 kind 状态结构体；
    // 服务别名成员（m_pSvcHost/SvcLogDep()/SvcList()/SvcConfirm()）删除，
    // 改按需向下转换助手（定义在公共壳 ToolDialogPage.cpp）。
    // 业务数据（TDialogWimIndex 等）基于 CDuiString（UI 类型），按 R7 零 UI
    // 契约不可下沉服务层，由 Page 持有（控件指针/选择/进度态）。
    // ============================================================

    // ---- 跨 kind 共享（公共壳） ----
    IToolHost*          m_pHost;
    EToolDlgKind        m_nKind;
    int                 m_nDPI;
    int                 m_cx;
    int                 m_cy;
    CVerticalLayoutUI*  m_pRoot;   // dlg_root
    CVerticalLayoutUI*  m_pBody;   // 内容容器

    CVerticalLayoutUI*  m_pGrid;
    std::vector<CHorizontalLayoutUI*> m_vRows;
    int                 m_nSel;

    // 外部命令防重入 / [B4] 内嵌任务进度面板状态
    bool            m_bTaskRunning;
    bool            m_bTaskMarquee;
    // [FIX-P2M] 详细设置（DetailsOk）后台应用在途标志：整合 RE 三条
    // reagentc 最坏 ~7 分钟，后台执行期间挡重入，完成后 WM_TDLG_TASKDONE
    // (wParam=2) 收口关窗。
    bool            m_bAdvancedApplying = false;
    // [FIX-T2-5] WIM 挂载/卸载后台在途标志：wimgapi 同步操作秒~十秒级，
    // 后台执行期间遮罩挡重入，WM_TDLG_TASKDONE(wParam=3 成功/4 失败挂载、
    // 5 卸载完成) 收口刷新。
    bool            m_bMountBusy = false;
    bool            m_bLogViewLoaded;  // 查看日志正文是否已完成首次加载
    // [L-10] 查看器状态（跨刷新保留：用户设的过滤/搜索不会因点"刷新"而丢）
    bool            m_bLogFollow;      // 自动跟随（定时增量刷新）
    int             m_nLogFilter;      // 0=全部 1=警告及以上 2=仅错误
    CDuiString      m_sLogSearch;      // 关键字（大小写不敏感）
    std::vector<toolsvc::TLogLine> m_vLogAll;   // 最近一次读到的全量行（过滤的源）
    int             m_nMarqueePos;     // 往复扫描当前值（0..100）
    bool            m_bMarqueeDir;     // true=递增
    CControlUI*     m_pTaskMask;       // 覆盖内容区的浮层（吞鼠标点击）
    CTextUI*        m_pTaskStage;      // 阶段文字（可两行）
    CProgressUI*    m_pTaskBar;        // 0-100% 进度条
    CTextUI*        m_pTaskPct;        // 百分比文字

    // ---- 服务层（工厂按 kind 装配，拥有；宿主非拥有见构造函数局部变量）----
    std::unique_ptr<toolsvc::IToolService>     m_pSvc;
    // [R6-04] 映像编辑作业服务（wimgapi 闭包工厂；线程/进度盾仍在本页）
    std::shared_ptr<toolsvc::IIsoImageService> m_pImgSvc;  // [R16][T2] 改 shared_ptr：ESD 作业线程持拥有型副本
    // [R6-05] 向导业务服务（构造注入 m_pImgSvc）
    std::unique_ptr<toolsvc::IWizardService>   m_pWizSvc;

public:
    // [FIX-T2-5] 后台作业（WIM 挂载/卸载）访问桥：
    //   · ImgSvcForJob/WizSvcForJob 返回拥有型/裸借用副本——对话框可能在线程
    //     期间被关闭，shared_ptr 副本保证服务存活；IWizardService 裸借用仅限
    //     遮罩存续期（m_bMountBusy 挡关窗）使用。
    std::shared_ptr<toolsvc::IIsoImageService> ImgSvcForJob() const { return m_pImgSvc; }
    toolsvc::IWizardService* WizSvcForJob() const { return m_pWizSvc.get(); }
    // HostLog 经 AppendLogEx（已互斥）线程安全
    void JobHostLog(const CDuiString& s) const { m_pHost->HostLog(s); }
    HWND JobHwnd() const { return m_hWnd; }

    // [R16][T10] 旧别名成员删除：按需向下转换（非对应 kind 得 nullptr，与原行为一致）
    toolsvc::CLogDepGraphService* SvcLogDep() const;   // 日志/依赖图
    toolsvc::CListService*        SvcList() const;     // 超链接列表
    toolsvc::CConfirmService*     SvcConfirm() const;  // 详细设置/双路径/删除确认

    // ---- kind 状态：另存为映像（EToolDlgKind::TD_CAPTURE） ----
    struct TCapState
    {
        TCapState() : pChkShadow(nullptr), pChkBoot(nullptr) {}
        CDuiString  sName;      // 映像名称
        CDuiString  sDesc;      // 映像说明
        CDuiString  sFlag;      // 映像标志（EDITIONID）
        CTdlgCheck* pChkShadow; // VSS 卷影副本
        CTdlgCheck* pChkBoot;   // 生成可启动
    };
    TCapState m_cap;

    // ---- kind 状态：释放映像（EToolDlgKind::TD_APPLY） ----
    struct TApplyState
    {
        TApplyState() : pCombo(nullptr) { for (int i = 0; i < 5; ++i) apChk[i] = nullptr; }
        std::vector<TDialogWimIndex> vIdx;      // 索引列表
        std::vector<CTextUI*>        vInfoVal;  // 信息表各值单元格
        CComboUI*                    pCombo;    // 索引下拉
        CTdlgCheck*                  apChk[5];  // WIMBoot/WTG/Compact 等 5 选项
    };
    TApplyState m_apply;

    // [R6-04] 透明挂载 ISO 的原路径改由 IIsoImageService 槽位 0 持有。
    // .iso → 挂载+定位内部 install.wim/esd；非 ISO 原样返回。
    // 失败时 sOut 不变并返回 false（sErr 给中文原因）。
    bool ApplyNormalizeImage(const CDuiString& sIn, CDuiString& sOut,
                             CDuiString* psErr);

    // [R14-03] ApplyOk 拆分辅助（原 317 行单函数 → 薄壳+4 辅助，零行为改动）：
    //   AoNormalizeIso 输入 ISO 透明挂载并刷新索引下拉（失败已弹窗，返回 false）；
    //   AoWarnIfTargetMounted 目标盘系统目录挂载检测（用户选否返回 false）；
    //   AoAskDriverPath WIMBoot/WTG/Compact 时询问额外驱动目录；
    //   AoRecordLastApply 最近释放参数写 HKCU\Software\DismTool\LastApply。
    bool AoNormalizeIso(DuiLib::CEditUI* pWim, CDuiString& sWim);
    bool AoWarnIfTargetMounted(const CDuiString& sTgtDr);
    void AoAskDriverPath(bool bWIMBoot, bool bWTG, bool bCompact,
                         CDuiString& sDrvPath);
    void AoRecordLastApply(const CDuiString& sWim, const CDuiString& sDir,
                           int nIndex, DWORD dwFlags);

    // ---- kind 状态：账户管理（EToolDlgKind::TD_ACCOUNT） ----
    struct TAcctEntry
    {
        CDuiString sName;
        DWORD      dwFlags;
        CDuiString sDesc;
    };
    struct TAcctState
    {
        TAcctState() : pEnable(nullptr), pClear(nullptr) {}
        std::vector<TAcctEntry> vList;
        CButtonUI* pEnable;
        CButtonUI* pClear;
    };
    TAcctState m_acct;

    // ---- kind 状态：编辑映像文件（EToolDlgKind::TD_IMAGEX） ----
    struct TImgxState
    {
        TImgxState() : bMeta(false), bDirty(false), pBatchMode(nullptr), pBatchEdit(nullptr) {}
        std::vector<TDialogWimIndex> vIdx;  // 索引列表
        CDuiString sFile;                   // 当前映像路径
        // [R6-04] ImageX 透明挂载 ISO 原路径改由 IIsoImageService 槽位 1 持有（R-B80）。
        bool       bMeta;                   // 元信息编辑态
        bool       bDirty;                  // 外部命令完成后需自动重读刷新
        // P1-5: 批量模式
        CTdlgCheck* pBatchMode;             // 批量模式开关
        CEditUI*    pBatchEdit;             // 索引列表输入框
        CDuiString  sBatchList;             // 缓存最后一次解析的批量索引列表（元信息编辑时透传）
    };
    TImgxState m_imgx;

    // ---- kind 状态：ESD 解密向导（EToolDlgKind::TD_ESDWIZ） ----
    // [R16][T4] 密钥/外壳预填路径改由 IIsoImageService 持有（经 Open 注入）；
    // sFile 保留为运行时状态（浏览/预填后的当前源路径）。
    struct TEsdState
    {
        TEsdState() : pKey(nullptr), pSrcEdit(nullptr) {}
        std::vector<TDialogWimIndex> vIdx;  // 源 ESD 的索引列表
        CDuiString sFile;                   // 源 ESD 路径
        CComboUI*  pKey;                    // 解密密钥下拉
        CEditUI*   pSrcEdit;                // 源路径输入框
    };
    TEsdState m_esd;

    // [ESD 自主解密 2026-10-08] 后台解密任务（知识库 41 §26）
    static DWORD WINAPI EsdDecryptThread(void* pv);
    void OnEsdTaskDone(void* pvResult);

    // ---- kind 状态：挂载 12 步向导（EToolDlgKind::TD_MOUNT，原版 PEImageManager） ----
    struct TMountState
    {
        TMountState() : pDirCombo(nullptr) {}
        std::vector<TDialogWimIndex> vIdx;      // 源 WIM/ESD 的索引列表
        CDuiString sFile;                       // 源 WIM/ESD 路径
        CDuiString sDir;                        // 挂载点路径
        CComboUI*  pDirCombo;                   // 已挂载点下拉（供卸载）
        std::vector<std::wstring> vMountedDirs; // 已挂载目录（MountLedger）
        // [R6-05c] 已挂载映像行（IWizardService::MountEnumRows 产出）
        std::vector<toolsvc::TMountedRow> vRows;
    };
    TMountState m_mount;

    // ---- kind 状态：PE 映像设置（EToolDlgKind::TD_PECFG，ScratchSpace/TargetPath） ----
    // [R16][T4] 挂载目录预填改由 IIsoImageService 持有（经 Open 注入）。
    struct TPeState
    {
        TPeState() : pDir(nullptr), pScratch(nullptr), pTarget(nullptr) {}
        CEditUI* pDir;      // 挂载目录输入框
        CEditUI* pScratch;  // 暂存空间(MB)
        CEditUI* pTarget;   // 目标路径
    };
    TPeState m_pe;

    // ---- kind 状态：引导修复（EToolDlgKind::TD_BOOTFIX） ----
    // [R6-05h] 临时盘符分配/回收改由 IWizardService::BfFixBoot 内部完成。
    struct TBfState
    {
        TBfState() : nSel(-1), bBasic(false), pTarget(nullptr), pBasicBtn(nullptr), pGrid(nullptr) {}
        std::vector<TBfPart> vParts;  // 全部分区行
        int        nSel;              // 当前选中（与 grid 选择一致）
        bool       bBasic;            // 基本模式（隐藏分区表）
        CDuiString sWork;             // 自有临时目录（结果文件）
        CTextUI*   pTarget;           // 首行目标描述
        CButtonUI* pBasicBtn;         // 基本/更多 按钮
        CVerticalLayoutUI* pGrid;     // 分区表容器
    };
    TBfState m_bf;

    // [R-B44] 超链接列表/双路径/删除确认：条目/标题/路径已由
    // CListService/CConfirmService 持有（[R16][T4] 经 Open 注入）。
    CDuiString DualPath1() const;
    CDuiString DualPath2() const;
    bool DelConfirmed() const;
};
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
