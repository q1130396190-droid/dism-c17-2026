#pragma once

// ============================================================
// Page_Assoc —— v2 P2c 纵切页面（content_area 页索引 9：文件关联）
//
// 承接原 CFrameWnd 的 6 个关联页函数（BuildAssocCommon /
// QueryAssociation / ApplyAssocFilter / UpdateAssocCount / AssocButton /
// ShowPickAssocProgramDialog）及视图数据 m_vAssocExt（与
// assocrow_%d 行序号严格一一对应）。
// UserChoice 注册表写操作归 AssocService（S8），dism 关联导入导出同；
// 注册表只读助手 RegGetString 在 Registry 基础层。
//
// 物理搬迁护栏：函数体零逻辑改动，仅把宿主引用从 CFrameWnd
// 隐式成员改为 m_ctx.Xxx（FindControl/Hwnd/日志/确认/弹框）。
// ============================================================

#include <vector>
#include "Utils/Utils.h"
#include "PageBase.h"
#include "CustomControls.h"   // CClickRowUI (R14-03 行构建)

class CPageAssoc : public CPageBase
{
public:
    explicit CPageAssoc(IPageContext& ctx) : CPageBase(ctx) {}

    void Build() override;   // 原 BuildAssocCommon

    // P8：第二级事件路由（原壳 Notify / WM_RBUTTONUP 关联页分支）
    void OnEvent(TNotifyUI& msg) override;
    bool OnContextMenu(POINT pt, bool bFromContextMsg) override;

    // [R128-2] 文件关联行级专属右键菜单（assocrow_%d 命中）
    void ShowAssocContextMenu(int nRow, POINT pt);

    // [R165] 从 ShowAssocContextMenu 抽出：复制类菜单项 / 处理程序枚举并复制
    void AssocCopyMenuField(const CDuiString& sValue, const CDuiString& sTipName,
                            const char* pszOkKey, const char* pszEmptyKey,
                            const char* pszLogTag, int nRow);
    void AssocCopyHandler(const CDuiString& sExt, int nRow);

    // [R128-2] AssocService 写路径失败码 → 分级 Toast 文案（避免 Page 头 include AssocService.h，用 int 传码）
    void ShowAssocWriteError(int nResult, const wchar_t* pszExt);

    void QueryAssociation(const wchar_t* pszExt);
    void ApplyAssocFilter();

    // [AUDIT-P2] ApplyAssocFilter 原 212 行 / CC=25，拆为 3 段，并合并两处重复清理：
    //   AsfMatchRow      单行关键字匹配（扩展名 / ProgId 任一命中）
    //   AsfRemoveByName  按控件名从父容器摘除并释放（原 assoc_empty /
    //                    assoc_empty_row 两段逐字重复，合并为一处）
    //   AsfShowEmptyHint 无匹配时的空态提示行
    // 过滤语义、assocrow_ 前缀、计数口径逐字保留。
    bool AsfMatchRow(DuiLib::CControlUI* pRow, const CDuiString& sKwLower);
    void AsfRemoveByName(const wchar_t* pszName);
    void AsfShowEmptyHint(DuiLib::CVerticalLayoutUI* pLive, const CDuiString& sKw);
    void UpdateAssocCount(int nShown, int nTotal = -1);
    void AssocButton(const wchar_t* pszBtn);

    // [AUDIT-P2] AssocButton 原 192 行 / CC=29，按职责拆为 3 段：
    //   AswResolveAction 按钮 → (作用域/动作/路径)，false=用户取消或按钮未知
    //   AswRunAction     执行原生关联维护，返回退出码（0=成功）
    //   AswReportResult  拼结果文案 + 失败复制剪贴板 + 告警 + 成功后刷新
    // 弹窗文案、ErrorMap 映射、日志文本逐字保留。
    bool  AswResolveAction(const wchar_t* pszBtn, bool& bImage, int& nAction,
                           CDuiString& sXml, CDuiString& sMount);
    DWORD AswRunAction(int nAction, bool bImage, const CDuiString& sMount,
                       const CDuiString& sXml, CDuiString& sA9Err);
    void  AswReportResult(bool bOk, int nAction, DWORD dwExit,
                          const CDuiString& sA9Err);
    int  ShowPickAssocProgramDialog(const DuiLib::CDuiString& sExt);

    // [R97] "打开方式"按钮：用当前过滤扩展名（或首个扩展名）触发系统 OpenWith 对话框
    void OnOpenWithButton();

    // [R108] 批量应用：把 assoc_progid 当前 ProgId 写到联机列表所有可见 assocrow_ 行
    // 走 AssocService::ApplyAssocBatch（内部逐项 SetUserFileAssociationEx + SHCNE_ASSOCCHANGED）。
    // 返回成功条数；失败条数经 HostLog + Toast 反馈。
    int OnBatchApply();

    // [R115-1] HKCR 层写路径：注册扩展名 / 删除扩展名 / 恢复系统默认。
    // 三个按钮分别走 AssocService::RegisterExtKey / DeleteExtKey / RestoreExtDefault，
    // 失败分级 Toast + HostLog，成功后 QueryAssociation 刷新详情。
    void OnRegisterExtKey();
    void OnDeleteExtKey();
    void OnRestoreExtDefault();

    // 供核心壳 assocrow_%d 点击路由做边界检查与取扩展名
    size_t AssocCount() const { return m_vAssocExt.size(); }
    const DuiLib::CDuiString& AssocAt(int nIdx) const { return m_vAssocExt[nIdx]; }

    const wchar_t* PageName() const override { return L"Assoc"; }
    void    OnGlobalCopy(bool bCut) override;

    // [R230] 浅色/深色切换后重取令牌色。契约见 PageBase.h：只改颜色属性，
    // 禁止 Build()/Add/Remove —— 那会清空两个列表、丢失滚动位置，
    // 并让 m_pFocusCtrl 悬空。
    void    RefreshThemeColors() override;
private:
    std::vector<DuiLib::CDuiString> m_vAssocExt;   // 联机扩展名（与 assocrow_ 序号对应）

    // [R14-03] Build 拆解：离线提示 / 枚举 / 预计算 / 渲染 / 统计
    void AssocBuildOfflineTip();
    void AssocBuildEnumHkcu();        // HKCU\Explorer\FileExts
    void AssocBuildEnumHklm();        // HKLM\SOFTWARE\Classes
    void AssocBuildEnumHkcr();        // HKCR（. 前缀子键，上限 1000）
    void AssocBuildDedupExts();       // 去重排序 m_vAssocExt
    void AssocBuildPrecomputeGroup(
        std::vector<DuiLib::CDuiString>& vGroupKey,
        std::vector<DuiLib::CDuiString>& vProgId,
        std::vector<DuiLib::CDuiString>& vAssocExe);   // 预计算 + 排序（输出三并行数组）
    void AssocBuildRenderRows(
        const std::vector<DuiLib::CDuiString>& vGroupKey,
        const std::vector<DuiLib::CDuiString>& vProgId,
        const std::vector<DuiLib::CDuiString>& vAssocExe,
        CVerticalLayoutUI* pLive,
        int& nGroupCount, int& nExeResolved);   // 输出统计（供 AssocBuildLogStats）
    void AssocBuildLogStats(
        const std::vector<DuiLib::CDuiString>& vAssocExe,
        int nGroupCount, int nExeResolved);   // 分组/图标/状态条统计
};
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
