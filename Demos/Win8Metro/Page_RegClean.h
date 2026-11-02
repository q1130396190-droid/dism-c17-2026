#pragma once

// ============================================================
// Page_RegClean —— 注册表清理页（[R79] 四件套接线版）
//
// 承接原 CFrameWnd::BuildRegCleanPage：
//   Data.xml 中 Reg 类动作规则枚举（原版独立页还原）
//
// [R79] 增加：
//   * BuildPage() 从 RegCleanEngine 拉取真实规则，渲染为复选框行
//   * OnEvent() 响应 btn_regclean_scan / clean / backup / restore
//   * 行点击 regcleanrow_%d 选中 + 勾选跟踪
//   * OnScan/OnClean/OnRestore/OnBackup 四件套 UI 段
//
// 物理搬迁护栏：宿主引用一律改 m_ctx.Xxx。
// ============================================================

#include <vector>
#include "PageBase.h"
#include "RegCleanRule.h"
#include "RegCleanEngine.h"
#include "RegCleanJobService.h"   // [R111] RegCleanDoneMsg / RegCleanJobService
#include "FrameWnd_Messages.h"    // [R111] WM_REGCLEN_* 常量（FrameWnd 已包含，但显式声明更清晰）

class CPageRegClean : public CPageBase
{
public:
    explicit CPageRegClean(IPageContext& ctx) : CPageBase(ctx), m_nSelected(-1) {}

    void Build() override { BuildPage(); }

    // P8：第二级事件路由（原壳 Notify 本页分支）
    void OnEvent(TNotifyUI& msg) override;

    void BuildPage();   // 原 BuildRegCleanPage

    // [R103] 行级右键菜单（regcleanrow_%d 命中）
    bool OnContextMenu(POINT pt, bool bFromContextMsg) override;

    // [R79] 四件套 UI 段
    void OnScan();
    void OnClean();
    void OnBackup();
    void OnRestore();
    void SelectAll(bool bCheck);
    // [R118-1] 创建系统还原点（btn_regclean_restorepoint）
    void OnRestorePoint();

    // ---- [R-B1] 全局快捷键 override ----
    const wchar_t* PageName() const override { return L"RegClean"; }
    // [R232] Colours only -- rebuilding would re-run the scan-driven build.
    void RefreshThemeColors() override;
    void    OnGlobalSelectAll() override { SelectAll(true); }
    void    OnGlobalCopy(bool bCut) override;
    void    OnGlobalSave() override { OnBackup(); }

// [R111] 后台清理完成回调（WM_REGCLEN_DONE）
    void OnCleanDone(RegCleanDoneMsg* pMsg);
    // [R111] 后台清理进度回调（WM_REGCLEN_PROGRESS）
    void OnCleanProgress(int nDone, int nTotal);
    // [R-EXTASYNC] 后台扫描完成回调（WM_REGCLEN_SCANDONE）
    void OnScanDone(RegCleanScanDoneMsg* pMsg);

    // [R5] 后台消息统一入口（三条 WM_REGCLEN_* 均为 kMsgOwnShell：
    // 本页只读取，堆参数由壳 ReleaseMsgParam 统一释放，禁止本页 delete）。
    [[nodiscard]] bool OnAsyncMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) override;

private:
    std::vector<int> m_vRegCleanItems;  // Reg 类规则索引（原）
    std::vector<RegCleanScanResult> m_vScanResults; // [R79] 最近一次扫描结果
    RegCleanScanSummary m_scanSummary;  // [R-EXTASYNC] 后台扫描结果汇总（worker 写）

    // [REFACTOR] 从 OnContextMenu 提取：查找 TEMP 下最新 RegCleanBackup_*.reg。
    // 无备份时返回 TEMP 目录本身。逐字搬迁零逻辑改动。
    CDuiString FindLatestRegCleanBackup();

    // [REFACTOR] 从 OnContextMenu 提取：按规则动作组装菜单所需的目标键与
    // 清理命令行（多条动作以 " && " 串联）。逐字搬迁零逻辑改动。
    void ResolveRuleCommand(const RegCleanRule& r,
                            CDuiString& sFirstKey, CDuiString& sCleanCmd);

    // [R162] 从 OnContextMenu 提取：从命中控件向上找 regcleanrow_N，并校验
    // 行号/规则号有效。抽出后 OnContextMenu 嵌套由 5 降到 3。
    [[nodiscard]] bool ResolveRowFromHit(CControlUI* pHit, int& nRow, int& iRule);

    // [R167] 从 FindLatestRegCleanBackup 抽出：按 mtime 取最新备份文件（嵌套 5→3）
    [[nodiscard]] bool RcNewestFileByTime(const wchar_t* pszPattern,
                                          const CDuiString& sDir, CDuiString& sOut);

    // [R167] 从 OnEvent 抽出：勾选框状态写回规则（三道校验，嵌套 5→4）
    void RcToggleRuleAt(int nRow, CControlUI* pSender);

    // [R167] 从 OnContextMenu 抽出：规则详情 / 打开备份目录（嵌套 5→3）
    void RcShowRuleDetail(int nRow, int iRule, const CDuiString& sRuleName,
                          const CDuiString& sFirstKey);
    // [R175] 从 ResolveRuleCommand 抽出：动作 → reg.exe 命令行（嵌套 4→2）
    [[nodiscard]] CDuiString RcBuildCleanCommand(const RegCleanRule& r);

    // [R175] 从 OnEvent 抽出：行点击记录选中（嵌套 4→3）
    void RcSelectRowAt(int nRow);

    [[nodiscard]] bool RcOpenBackupFolder(int nRow, const CDuiString& sLastBackup,
                                          const CDuiString& sRuleName);

    // [R162] 从 OnCleanDone 提取：追加首条失败规则的证据（全失败/部分失败共用）
    void AppendFirstFailHint(const RegCleanDoneMsg* pMsg, CDuiString& sErr, int nLogLevel);

    // [R162] 从 OnContextMenu 提取：复制类菜单动作统一实现（空则提示，非空则复制+提示）
    void CopyOrWarn(const CDuiString& sValue, const CDuiString& sName,
                    const CDuiString& sOkName,
                    const char* pszOkKey, const char* pszEmptyKey, const char* pszLogTag);

    // [R-EXTASYNC] 扫描/清理进行中禁用页内动作按钮，防重入
    void SetRegCleanButtonsBusy(bool bBusy);

    // [R79] 当前选中行索引（-1 表示无选中），行点击设置
    int m_nSelected;

    // [R79] 取选中索引；越界或无选中返回 -1
    int CurrentSelectedIndex() const;
};