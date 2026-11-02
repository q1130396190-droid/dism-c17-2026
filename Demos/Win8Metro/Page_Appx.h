#pragma once

// ============================================================
// Page_Appx —— v2 P4 纵切页面（用户/预装 Appx 两个子页）
//
// 承接原 CFrameWnd 的 8 个函数：
//   StartAppxEnumeration / OnAppxData / BuildAppxList /
//   UpdateAppxCount / ApplyAppxFilter / ShowAppxDetail /
//   AppxSelectAll / AppxDelete
// 及视图数据 m_vAppxCache[2] / m_nAppxGroupMode / m_bAppxLoading。
// 后台注册表枚举归 AppxCapUpdService（S6），结果经 WM_APPX_DATA
// 由核心壳转发 OnData。
//
// 物理搬迁护栏：函数体零逻辑改动，宿主引用一律改 m_ctx.Xxx。
// ============================================================

#include <vector>
#include "Utils/Utils.h"
#include "PageBase.h"
#include "AppxCapUpdService.h"   // AppxPkg / AppxDataMsg
#include "AppxDeployService.h"   // EAppxDeployResult（ShowDeployFailToast 参数）
#include "AppxSmartFilter.h"     // [R200] 分级中文映射 + 智能筛选分类

class CPageAppx : public CPageBase
{
public:
    explicit CPageAppx(IPageContext& ctx)
        : CPageBase(ctx), m_nGroupMode(0), m_bLoading(false) {}

    void Build() override { StartEnumeration(); }

    // [R3] F5 刷新=重新触发异步枚举并附刷新日志（懒构建默认即 Build）
    void OnRefreshPage() override;

    // P8：第二级事件路由（原壳 Notify / WM_RBUTTONUP 本页分支）
    void OnEvent(TNotifyUI& msg) override;
    bool OnContextMenu(POINT pt, bool bFromContextMsg) override;

    // [R5] 后台枚举结果收口（WM_APPX_DATA / WM_APPX_DEPDATA，消息体本页释放）
    bool OnAsyncMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) override;

    void StartEnumeration();                       // 原 StartAppxEnumeration
    void OnData(AppxDataMsg* pData);               // 原 OnAppxData
    // [R-B25] WM_APPX_DEPDATA：依赖 TSV 回读完成 → 按 FullName 填 sDependencies
    void OnDepData(AppxDepsMsg* pMsg);

    // [R164] 从 OnDepData 抽出：按 sFull 回填依赖到两个 tab 的缓存
    void FillDepsIntoCache(const std::vector<AppxDepEntry>& vDeps);
    // [R-B25] Appx 依赖后台线程（EnrichAppxDependencies 启动）
    static DWORD WINAPI AppxDepThreadProc(void* pv);
    void BuildList(int nTab, const std::vector<AppxPkg>& v);  // 原 BuildAppxList

    // [R200] 当前分类下拉框选中项（0=全部；Win11 独有项在 Win10 上不会出现）。
    [[nodiscard]] EAppxCategory AxCurrentCategory() const;
    // [R200] 单行"是否通过智能筛选"：分类 + 关键词。渲染与过滤共用同一判据，
    //   避免出现"列表里看得见、计数里算不到"的两套口径。
    [[nodiscard]] bool AxRowPasses(const AppxPkg& pkg, const CDuiString& sKwLower) const;
    // [R200] 分级映射生效情况写一行日志（Win10 应显示 win10 / 已关闭）。
    void AxLogMapTier();
    // [R200] 填充分类下拉框并把选中项复位到「全部」。
    void AxInitSmartCombo();

    // [R14-04] BuildList 拆解：空态 / 单行 / 统计 / 发布者分组
    void AxBuildEmptyList(int nTab);
    void AxBuildRow(int nTab, int k, const AppxPkg& pkg, CVerticalLayoutUI* pList);
    // [R171] 从 AxBuildRow 抽出（原 100 行）：四个装配函数 + 两个文本拼接函数
    //    返回 CTextUI* 会漏掉 CVerticalLayoutUI/CCheckBoxUI 的成员差异，
    //    故装配函数一律返回 void，内部自建自 Add（R70 C2039 教训）。
    [[nodiscard]] CDuiString AxFormatRowSub(const AppxPkg& pkg);
    [[nodiscard]] CDuiString AxFormatRowOwner(const AppxPkg& pkg);
    //    形参用基类 CHorizontalLayoutUI*（CClickRowUI 派生自它），
    //    这样头文件不必包含 CustomControls.h（R70 同类教训）。
    void AxAddRowCheck(DuiLib::CHorizontalLayoutUI* pRow, int nTab, int k);
    void AxAddRowInfo(DuiLib::CHorizontalLayoutUI* pRow, const AppxPkg& pkg);
    void AxAddRowSize(DuiLib::CHorizontalLayoutUI* pRow, const AppxPkg& pkg);
    // [R166] ApplyFilter 内联行构建抽取（原 124 行 / CC14 → 分段）
    // 注意：与 AxBuildRow 不等价（Owner 行无系统星标/Publisher），不可互换。
    void AxBuildFilteredRow(int nTab, size_t k, const AppxPkg& pkg, CVerticalLayoutUI* pList);
    void AxLogStats(int nTab, const std::vector<AppxPkg>& v);
    void AxLogPublisherTop(int nTab, const std::vector<AppxPkg>& v,
                           const std::vector<CDuiString>& vSeenPub);
    // [R175] 从 AxLogPublisherTop（CC13）抽出：发布者计数项 / 统计 / 降序排序
    struct AxPubCount { CDuiString sName; int n; };
    void AxCountPublishers(const std::vector<AppxPkg>& v,
                           const std::vector<CDuiString>& vSeenPub,
                           std::vector<AxPubCount>& vPubs);
    static void AxSortPublishersByCount(std::vector<AxPubCount>& vPubs);
    // [R175] 从 OnContextMenu（CC13）抽出：行级 / 列表级右键菜单命中判定
    bool AxTryRowContextMenu(DuiLib::CControlUI* pHit, POINT pt);
    [[nodiscard]] bool AxTryListContextMenu(DuiLib::CControlUI* pHit, POINT pt);
    void UpdateCount(int nTab);                    // 原 UpdateAppxCount

    // [R164] 从 UpdateCount 抽出：统计列表勾选数并带出总行数
    [[nodiscard]] int CntCheckedInList(const wchar_t* pszListName, int& nTotal);
    void ApplyFilter();                            // 原 ApplyAppxFilter
    void ShowDetail(int nTab, int k);
    // [R155] ShowDetail 拆分（原 48 行 / CC17）
    [[nodiscard]] CDuiString AxFormatVersionLine(const AppxPkg& pkg);
    // [R155] OnEvent 拆分（原 69 行 / CC21）
    bool AxDispatchClickButton(const CDuiString& sName);
    void AxHandleSelectChanged(TNotifyUI& msg, const CDuiString& sName, const wchar_t* pszName);        
    // [R155] Backup 拆分（原 64 行 / CC16）
    void AxCollectCheckedFullNames(DuiLib::CVerticalLayoutUI* pList, std::vector<CDuiString>& vSel);
    // [R155] ShowDeployFailToast 拆分（原 72 行 / CC15）
    void AxClassifyHResultText(const CDuiString& sErr,
                               const wchar_t*& pszTitle, const wchar_t*& pszBody);      // 原 ShowAppxDetail
    void SelectAll();                             // 原 AppxSelectAll
    void Delete();

    // [AUDIT-P2] Delete 原 202 行 / CC=30，拆为 4 段（采集/确认/部署/上报）。
    // 确认文案、退出码语义、Toast 分级逐字保留。
    void AxCollectChecked(DuiLib::CVerticalLayoutUI* pList, int nTab,
                          std::vector<CDuiString>& vSel, ULONGLONG& ullTotalSize);
    // [R166] AxCollectChecked 拆分（原 55 行 / CC15）
    [[nodiscard]] ULONGLONG AxRowCacheSize(const CControlUI* pRow, int nTab) const;
    bool AxConfirmDelete(const std::vector<CDuiString>& vSel, ULONGLONG ullTotalSize);
    EAppxDeployResult AxDeployRemove(int nTab, const std::vector<CDuiString>& vSel,
                                     CDuiString& sErr);
    void AxReportDeleteResult(EAppxDeployResult nDeploy,
                              const std::vector<CDuiString>& vSel,
                              const CDuiString& sErr);                                // 原 AppxDelete
    void EnrichAppxDependencies(std::vector<AppxPkg>& vUser, std::vector<AppxPkg>& vPre);  // [R66] 依赖关系富化
    void AppxInstall();                           // [R67] App Installer 安装通道
    void Backup();                                // [R81] 导出选中 Appx 包（Export-AppxPackage）
    void Restore();                               // [R81] 从 .msixbundle/.appx 恢复安装
    // [R121-1] Appx 部署失败分级 Toast：按 HRESULT 等级展示差异化提示
    void ShowDeployFailToast(const wchar_t* pszAction, EAppxDeployResult nDeployCode, const CDuiString& sErr);

    // [R123-1] 行级右键菜单增强：打开安装位置 / 复制包名 / 复制 FamilyName
    void ShowRowContextMenu(int nTab, int k, POINT pt);
    void OpenInstallLocation(int nTab, int k);
    void CopyFullName(int nTab, int k);
    void CopyFamilyName(int nTab, int k);
    // [R-B25] 依赖图可视化（4405）
    void ShowDepGraph(int nTab, int k);

    const wchar_t* PageName() const override { return L"Appx"; }
    void    RefreshThemeColors() override;   // [R222] rebuild code-drawn row colours
    void    OnGlobalSelectAll() override;
    void    OnGlobalCopy(bool bCut) override;
private:
    std::vector<AppxPkg> m_vCache[2];   // 原 m_vAppxCache，缓存原始数据用于过滤
    int                  m_nGroupMode;  // 原 m_nAppxGroupMode：0=按标签 1=按发布者
    bool                 m_bLoading;    // 原 m_bAppxLoading
};
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
