#pragma once

// ============================================================
// Page_Unattend —— v2 P2 第一个纵切页面（content_area 页索引 10）
//
// 承接原 CFrameWnd 的 12 个预应答函数（BuildUnattendPage /
// ToggleUnattendGroup / RebuildUcBody / OnUcRowSelect /
// ShowUcContextMenu / UcEdit / UcAdd / UcDelete / UcUp /
// UpdateUcButtons / UcExport / UcImport）及 6 个视图/数据成员。
// 纯本地 vector 数据，无 Service 依赖。
//
// 物理搬迁护栏：函数体零逻辑改动，仅把宿主引用从 CFrameWnd
// 隐式成员改为 m_ctx.Xxx（FindControl/SetFocus/Hwnd/字形/日志/弹框）。
// ============================================================

#include <vector>
#include "Utils/Utils.h"
#include "PageBase.h"

struct UcSchemaStats;

// [R175] 从 Page_UnattendIbs_Report.cpp 上移：IBS 报告的单设备信息。
//   上移原因：UcAppendDevInfo 是成员函数，签名需要该类型在头内可见。
struct UcDevInfo
{
    CDuiString sClass;
    CDuiString sName;
    CDuiString sInf;
};

class CPageUnattend : public CPageBase
{
public:
    explicit CPageUnattend(IPageContext& ctx) : CPageBase(ctx)
    {
        m_nUcSelGroup = -1;
        m_nUcSelItem = -1;
        m_nUcEditGroup = -1;
        m_nUcEditItem = -1;
        m_bUcBuilt = false;   // [R-fix] 预应答器页状态保护：首轮 Build() 才清空数据
    }

    // [R162] 选中项双分量单一入口。原在 2 个 TU 里成对直写 11 处，
    //   "必须成对"只靠人工保证；有一处只写 item、依赖前一步已复位。
    //   集中后不会出现"组已换、项还是旧组的下标"这种越界选中。
    void RememberUcSelection(int nGroup, int nItem)
    { m_nUcSelGroup = nGroup; m_nUcSelItem = nItem; }

    // [R162] 从 UcNormalizeXmlViaDom 抽出：DOM 规范化（true = 已替换 sXml）
    [[nodiscard]] bool UcTryExportViaDom(CDuiString& sXml, int nTotal);

    // [R175] 从 UcTryExportViaDom 抽出：DOM 成功后取 xml 串（嵌套 4→3）
    [[nodiscard]] bool UcTakeDomXml(IDispatch* pDoc, CDuiString& sXml, int nTotal);

    // [R175] 从 BuildIBSReport 抽出：读取单设备信息并入表（嵌套 4→2）
    // [R175] 用 void* 收 HDEVINFO：避免在头里 include setupapi.h
    //   （会把 devguid.h 等一并拖进来，且 Page_Unattend.h 已被多处包含）
    void UcAppendDevInfo(void* hDevRaw, void* pDidRaw, std::vector<UcDevInfo>& vDevs);

    // [R162] 从 InjectIbsStage 抽出：读 Panther\unattend.xml / 拼 RunSynchronous 片段
    void UcReadPantherXml(const CDuiString& sPath, std::string& sExisting);
    void UcBuildRunSyncFragment(int nStage, CDuiString& sInject);

    // [R168] 从 InjectIbsStage 抽出：组装目标 XML / 带 BOM 写回
    bool UcComposeInjectedXml(const std::string& sExisting, const CDuiString& sInject,
                              int nStage, std::string& sXml);
    [[nodiscard]] bool UcWriteXmlUtf8Bom(const CDuiString& sPath, const std::string& sXml);

    void Build() override;   // 原 BuildUnattendPage
    // [R-fix] 状态保持：F5 刷新与切页重入不再清空用户数据
    //   * Build() 首次执行完整 UI 构建 + 数据清零；后续重入保留命令/展开/选中态
    //   * 其他页（Cleanup/Appx/Capability/Tools/Update）均已 override
    //     OnRefreshPage/OnDeferredBuild 保护，唯本页遗漏，导致定位丢失
    void OnRefreshPage() override { Build(); }
    void OnDeferredBuild() override { Build(); }

    // P8：第二级事件路由（原壳 Notify / WM_RBUTTONUP / VK_DELETE 本页分支）
    void OnEvent(TNotifyUI& msg) override;
    bool OnContextMenu(POINT pt, bool bFromContextMsg) override;
    bool OnDeleteKey() override;

    void ToggleGroup(int n);                 // 原 ToggleUnattendGroup
    void RebuildBody(int n);                 // 原 RebuildUcBody
    void OnRowSelect(int g, int k);          // 原 OnUcRowSelect
    void ShowContextMenu(int g, int k);      // 原 ShowUcContextMenu
    void Edit(int g, int k);                 // 原 UcEdit
    void Add();                              // 原 UcAdd
    void Delete();                           // 原 UcDelete
    void Up();                               // 原 UcUp
    void UpdateButtons();
    // [R164] UpdateButtons 拆分（原 46 行 / CC15）
    void UcRefreshGroupBadges();                 // 组计数徽章刷新
    void UcRefreshStatusLine(bool bSel, int nTotal);  // 底部状态条文案
    // [R164] OnEvent 拆分（原 35 行 / CC15）
    bool UcDispatchClickButton(const CDuiString& sName);                    // 原 UpdateUcButtons
    void Export();                           // 原 UcExport
    // [R150] Export 拆分（原 163 行 / CC26）
    void UcBuildExportXml(CDuiString& sXml);                              // 拼装 unattend.xml 正文
    // [R150] ApplyUnattend 拆分（原 175 行 / CC19）
    void UcBuildApplyXml(CDuiString& sXml);                               // 写盘用 XML（Path 节点版）
    // [R65] 返回 false 表示转换/打开/写入任一环节失败，调用方不得继续跑 sysprep。
    bool UcWriteApplyXml(const CDuiString& sXmlPath, const CDuiString& sXml);  // UTF-8 BOM 临时副本
    bool UcLaunchSysprep(const CDuiString& sXmlPath);                     // 复制 Panther + 拉起 sysprep
    void UcNormalizeXmlViaDom(CDuiString& sXml, int nTotal);              // DOM 解析/校验/规范化序列化
    bool UcWriteExportFile(const CDuiString& sPath, const CDuiString& sXml, int nTotal);  // 写盘+回读自校验
    void Import();                           // 原 UcImport
    void RunSysprep();                       // [R71] 执行 sysprep
    void BuildIBSReport();                   // [R71] 生成 IBS 报告
    void ApplyUnattend();                    // [R71] 应用预应答配置
    void InjectIbsStage(int nStage);         // [R-B60] IBS 四注入点（0=PreSysPrep 1=PostSysPrep 2=PreOobe 3=PostOobe），向 %SystemRoot%\Panther\unattend.xml 注入 RunSynchronous 命令节点
    // [R173] 从 InjectIbsStage（原 117 行 / CC14）抽出的四段：
    //   读现有文件 / 组装片段 / 合并进 XML / BOM 写回。
    // 拆分不改变任何字节级行为：UTF-8 转换仍带结尾 NUL、BOM 仍前置三字节、
    // 结构不可识别仍记 ERROR 并中止（不写盘）。
    void UcReadExistingXml(const CDuiString& sPath, std::string& sExisting);
    [[nodiscard]] CDuiString UcBuildInjectFragment(const std::vector<CDuiString>& vCmds);
    [[nodiscard]] bool UcMergeInjectIntoXml(const std::string& sExisting,
                                            const CDuiString& sInject,
                                            const wchar_t* pszPass,
                                            std::string& sXml);
    void UcWriteXmlWithBom(const CDuiString& sPath, const std::string& sXml);
    // [R173] 从 ImportByScan（43 行 / CC14）抽出：pass 范围收集 CommandLine
    void UcScanCommandLinesInPass(const wchar_t*& p, const wchar_t* pLimit, int nGroup);
    // [R173] 从 Build（101 行）抽出：单个组的组头 / 组体装配
    [[nodiscard]] DuiLib::CControlUI* UcBuildGroupHeader(int n, const wchar_t* pszName,
                                                         const wchar_t* pszPassLabel);
    [[nodiscard]] DuiLib::CControlUI* UcBuildGroupBody(int n);
    // [R177] 从 UcWriteExportFile（61 行 / CC13）抽出：写 BOM 文件 / 回读校验
    // [R177] 从 ShowContextMenu（CC13）抽出：行级右键命令分派
    void UcDispatchRowCmd(int iCmd, int g, int k);
    // [R177] 从 Add（47 行 / CC13）抽出：空输入拒绝 / 编辑模式替换
    void UcRejectEmptyInput();
    [[nodiscard]] bool UcTryReplaceEditing(int g, const CDuiString& sCmd);
    bool UcWriteUtf8BomFile(const CDuiString& sPath, const CDuiString& sXml);
    [[nodiscard]] CDuiString UcVerifyExportBom(const CDuiString& sPath, int nTotal);
    // [R185] 从 SchemaCheck（55 行 / CC12）抽出：标签扫描 / 结果判定与输出
    void UcScanSchemaTags(const CDuiString& sContent, UcSchemaStats& st);
    void UcReportSchemaStats(const UcSchemaStats& st);
    // [R185] 从 ImportByDom（76 行 / CC12）抽出：DOM Schema 校验 / XPath 收集
    void UcDomReportSchema(const CDuiString& sContent, CDuiString& sDomErr);
    void UcDomCollectAllSettings(IDispatch* pDoc);
    // [R122] D2 行级右键菜单增强：复制命令 / 复制 Pass 名称 / 打开导出位置
    void CopyRowCommand(int g, int k);       // 复制选中行 CommandLine 到剪贴板
    void CopyRowPass(int g);                 // 复制当前组 Pass 名称到剪贴板
    void OpenExportLocation();               // 打开最近一次导出文件的所在目录

    bool HasSelection() const { return m_nUcSelGroup >= 0 && m_nUcSelItem >= 0; }

    const wchar_t* PageName() const override { return L"Unattend"; }
    // [R232] Colours only -- rebuilding would lose the expansion state.
    void RefreshThemeColors() override;
    void    OnGlobalCopy(bool bCut) override;
    void    OnGlobalSave() override;

    // [R11-01] Import() 深度拆解（d13→≤4）：上下文 struct + 循环体提取 + 卫语句
    // 私有辅助，纯结构性拆分，逐字搬迁 Import 原逻辑，零行为改动。
    // 原 Import 500 行/d≈16 → 主函数仅留卫语句 + 顺序调用。
private:
    // 读取文件字节 + 跳过 BOM + UTF-8 解码；失败已弹错误框
    bool ReadFileContent(const CDuiString& sPath, CDuiString& outContent);
    // 导入前自动备份当前内存命令（[R31-B]）；nBeforeTotal 返回备份条数
    void BackupBeforeImport(const CDuiString& sPath, int nBeforeTotal);
    // [R40/R50] Schema 结构校验 + OOBE Pass 阶段统计（纯日志，不阻断）
    void SchemaCheck(const CDuiString& sContent);
    // [REFACTOR] 从 SchemaCheck 提取：从 '<' 之后提取标签名（跳过前导空白，
    // 收集到空白/引号/'/'/'>' 为止）并转大写；outNameEnd 回写标签名结束位置。
    [[nodiscard]] bool FetchTagName(const wchar_t* pszSrc, int nContentLen, int pOpen,
                      CDuiString& outTag, int& outNameEnd);
    // 重置 m_vUcCmds/m_vUcExpanded/选择态；返回原命令总数
    int ResetImportState();
    // [R130] MSXML DOM 直连导入；成功返回 true 且已填充 m_vUcCmds
    bool ImportByDom(const CDuiString& sPath, const CDuiString& sContent);
    // 字符串扫描回退路径（DOM 失败时）
    void ImportByScan(const CDuiString& sContent);
    // 导入后重建 UI + 更新状态标签（DOM/扫描共用）
    void FinishImport(const CDuiString& sPath, int nBefore, int nTotal);
private:
    // [R-fix] 预应答器页状态保护：首次 Build() 置 true；
    // 此后 OnRefreshPage/OnDeferredBuild 重入不再清空用户已添加的命令。
    // BeginUcBuild 返回 true 表示首轮（已清零数据），false 表示重入（须保留并恢复）。
    [[nodiscard]] bool BeginUcBuild();
    void RestoreUcBuild();          // 重入时重建 body 内容 + 恢复展开态/箭头
    bool                            m_bUcBuilt;
    std::vector<bool>               m_vUcExpanded;
    std::vector<DuiLib::CDuiString> m_vUcCmds[4];
    int                             m_nUcSelGroup;
    int                             m_nUcSelItem;
    int                             m_nUcEditGroup;
    int                             m_nUcEditItem;
    DuiLib::CDuiString              m_sLastExportPath;  // [R122] 最近一次导出文件路径（右键"打开导出位置"用）
};
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
