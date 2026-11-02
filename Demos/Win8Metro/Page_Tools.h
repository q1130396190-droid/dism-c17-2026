#pragma once

// ============================================================
// Page_Tools —— P6a 工具箱页（11 个自绘彩色图标 + 小工具集）
//
// 承接原 CFrameWnd 的 14 个函数：
//   BuildToolboxPage / RunToolDialog /
//   ToolBackup / ToolRestoreImage / ToolUserAccounts / ToolBootRepair /
//   ToolChunGe / ToolEsd2Iso / ToolWimEsdConvert / ToolImageX /
//   ToolExplorer / ToolIsoGen / ToolHosts，
// 以及 WM_DISM_LOG / WM_DISM_DONE / WM_DISM_PROGRESS 的 UI 收尾。
//
// 物理搬迁护栏：函数体零逻辑改动，宿主引用一律改 m_ctx.Xxx；
// 进程/管道/bat/探测能力走 ExternalToolService（S9）。
// 页面无状态成员（原函数本身不持有页面状态）。
// ============================================================

#include <optional>
#include <vector>   // [R105] 在途取消令牌列表   // [C++17] std::optional<DWORD>：桌面说明写入结果
#include "Utils/Utils.h"
#include "PageBase.h"
#include "BackgroundJobs.h"   // [R104] CancelTokenPtr（取消令牌）

// [R14-03] BpBuildCard 以基类 CControlUI* 返回（实际类型 CClickVLayoutUI
// 完整定义在 CustomControls.h，仅 .cpp 引入；此处不前向声明，避免与
// DuiLib 命名空间内同名类型在 using namespace DuiLib 下产生二义性）

class CPageTools : public CPageBase
{
public:
    explicit CPageTools(IPageContext& ctx) : CPageBase(ctx), m_bToolProgressOn(false) {}

    void Build() override { BuildPage(); }

    // [R3] F5 刷新仅记录日志；懒构建=Build 后注册 11 个工具入口提示
    void OnRefreshPage() override;
    void OnDeferredBuild() override;

    // [R4] 工具箱具名对话框 + 壳命令收口（主菜单 1101..1205 / Ctrl+Shift+S）
    void HostRunToolDialog(EToolDlgKind nKind) override { RunDialog(nKind); }
    bool OnHostCommand(int id) override;

    // [R5] DISM 异步消息 / 工具进度定时器 / 带参菜单动作收口
    bool OnAsyncMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) override;
    void OnHostTimer(UINT uId) override;
    void HostSwmMerge(const CDuiString& sSwm, const CDuiString& sWim) override
        { ToolSwmMerge(sSwm, sWim); }
    void HostEsdDecryptWithFile(const CDuiString& sFile) override
        { ToolEsdDecryptWithFile(sFile); }

    void BuildPage();          // 原 BuildToolboxPage
    void RunDialog(EToolDlgKind nKind); // 原 RunToolDialog

    // P8：第二级事件路由（原壳 Notify toolitem_ 分支）
    void OnEvent(TNotifyUI& msg) override;

    // ---- 11 个小工具（原名保留）----
    void ToolBackup();
    void ToolRestoreImage();
    void ToolUserAccounts();
    void ToolBootRepair();
    void ToolChunGe();
    void ToolEsd2Iso();
    // [R151] ToolEsd2Iso 两段内联逻辑外提（原 CC=22）
    bool EsdCheckBootImage(const CDuiString& sEsd, CDuiString& sBootWim, bool& bHasBootWim);
    bool EsdCheckTargetSpace(const CDuiString& sEsd, const CDuiString& sIso);
    void ToolWimEsdConvert(int nForceDir = -1); // -1 ask, 0 WIM→ESD, 1 ESD→WIM
    // [R151] 零参薄壳：OnEvent 的工具分派表要求签名统一为 void(CPageTools::*)()，
    // 而 ToolWimEsdConvert 取地址后的类型是 void(CPageTools::*)(int)（默认参数不参与
    // 函数类型），无法直接入表（C2440，并连带 C2131 常量表达式失败）。
    // 薄壳把"默认 -1 = 询问方向"这一既有语义原样转达，行为完全一致。
    void ToolWimEsdConvertAsk() { ToolWimEsdConvert(-1); }

    // [AUDIT-P2] ToolWimEsdConvert 原 230 行 / CC=29，按阶段拆为 3 个辅助：
    //   WecChooseCompress   4 档压缩策略（config.ini xpress16k 优先）
    //   WecCheckTargetSpace 目标盘空间预估（不足时二次确认，false=用户中止）
    //   WecLaunchConvert    建进度窗 + 完成回调 + 启后台作业
    // 弹窗文案、压缩档位映射、退出分支逐字保留。
    DWORD WecChooseCompress();
    // [R170] 从 ToolWimEsdConvert 抽出：转换方向判定 / WIM 索引号上界统计
    bool  WecResolveDirection(int nForceDir);
    int   WecMaxIndex(const std::vector<TDialogWimIndex>& vIdx);
    bool  WecCheckTargetSpace(const CDuiString& sSrc, const CDuiString& sDst);
    // [R160] 从 WecCheckTargetSpace 抽出（原 5 层 if 嵌套）：空间预估 / 目标盘确认
    [[nodiscard]] ULONGLONG WecEstimateNeed(const CDuiString& sSrc);
    [[nodiscard]] bool WecConfirmSpaceEnough(const CDuiString& sDst, ULONGLONG ullNeed);
    void  WecLaunchConvert(const CDuiString& sSrc, const CDuiString& sDst, DWORD dwCompress);
    // [原生化] SWM 分卷合并为单一 WIM（wimgapi 导出，替代 dism /Export-Image /SourceSWMFile）
    void ToolSwmMerge(const CDuiString& sSwm, const CDuiString& sWim);
    void ToolImageX();
    void ToolEsdDecrypt();     // [R-B35] 独立 ESD 解密向导（EToolDlgKind::TD_ESDWIZ）
    void ToolEsdDecryptWithFile(const CDuiString& sFile); // [R-B35] 外壳右键预填源 ESD
    void ToolExplorer();
    void ToolIsoGen();
    void ToolHosts();
    // [R01] 激活备份/还原（独立组件 ActivationKit.dll 承载，R01 重构替代青蛙插件）
    void ToolActivationBackup();
    void ToolWin11ImagePreprocess(); // 选镜像文件直接处理：C++负责挂载/卸载，Rust负责门禁与写入
    // [R157] ToolActivationBackup 拆分（原 79 行 / CC19）
    bool ActPrepareTarget(bool bBackup, std::wstring& sBase);   // 选目录 + 还原前置校验
    void ActRunAndReport(bool bBackup, const std::wstring& sBase);  // 执行并反馈

    // ---- DISM 异步会话 UI 收尾（核心壳消息路由调用）----
    // WM_DISM_LOG：lParam 为 new CDuiString（wParam==1 为 stderr，加 [!] 前缀），
    // 本方法负责 delete。
    void OnDismLog(WPARAM wParam, LPARAM lParam);
    void OnDismDone(int nExitCode);
    void OnDismProgress(int nPct);

    // ---- [R107] 工具箱统一进度条轮询 ----
    // ArchiveToolRun 把工具运行状态写到 %LOCALAPPDATA%\DismTool\Logs\progress.txt，
    // 本页用 TOOL_PROGRESS_TIMER_ID 定时器 800ms 轮询并更新 tools_progress UI。
    void StartToolProgressPoll();     // 开启轮询（幂等）
    void StopToolProgressPoll();      // 停止轮询 + 隐藏进度条
    void OnToolProgressTimer();       // 核心壳 WM_TIMER TOOL_PROGRESS_TIMER_ID 分支转发

    // ---- [R110] 工具箱筛选 / 刷新 / 计数 ----
    void ApplyToolsFilter();      // tools_filter 名称实时过滤（隐藏非重建、行号对齐）
    void RefreshToolGrid();       // btn_tools_refresh：重建 tools_grid + 更新计数
    void UpdateToolsCount();      // 刷新 tools_count 显示"可见/总数"

    // ---- [R-B24] 卡片级右键菜单：toolitem_N 命中 → 启动/复制描述/查看依赖 ----
    bool OnContextMenu(POINT pt, bool bFromContextMsg) override;
    void ShowToolContextMenu(int nTool, POINT pt);
    void CopyToolDesc(int nTool);           // 复制工具一句话描述
    void ShowToolDep(int nTool);            // 查看工具依赖与可用性（Toast + 日志）
    // [R184] 从 OnEvent（49 行 / CC12）抽出：toolitem_N → 工具入口表分派
    void ToolDispatchItem(int nTool);
    // [R184] 从 ToolEsd2Iso（61 行 / CC12）抽出：进度窗 + 完成回调 + 启动作业
    void EsdLaunchConvertJob(const CDuiString& sEsd, const CDuiString& sIso,
                             const CDuiString& sBootWim, bool bHasBootWim,
                             const CDuiString& sEtfs, const CDuiString& sEfisys);

    // [R105] 在途作业的取消令牌（原为单个 m_pActiveE2iCancel）。
    //   为什么要改成列表：R104 只接了 ESD→ISO，令牌只能存一份；
    //   R105 把 WIM/ESD 互转、SWM 合并也接进来后，多个作业可能同时存在
    //   （用户连点两个工具），单个令牌会导致"取消只停一个"。
    //   现在点取消 = 请求停止全部在途作业，语义上就是用户看到的那个意思。
    std::vector<BackgroundJobs::CancelTokenPtr> m_vActiveCancels;
    // [R160] 取消令牌清单的单一所有者。原先三个 TU 各自 push_back / clear /
    //   pop_back，且启动失败的清理语义不一致：ESD→ISO 用 clear() 会把同页
    //   其它在途作业的令牌一并清空（此后点取消对它们失效，而作业仍在写盘），
    //   WIM/ESD 互转用 pop_back() 只退自己。改为具名入口后不变式只有一份。
    void RegisterCancelToken(const BackgroundJobs::CancelTokenPtr& pTok)
    {
        if (pTok) m_vActiveCancels.push_back(pTok);
    }
    // 只撤销"刚刚登记的那一份"：启动失败路径专用，不得清空他人令牌
    void UnregisterLastCancelToken()
    {
        if (!m_vActiveCancels.empty()) m_vActiveCancels.pop_back();
    }
    // 请求取消全部在途作业；返回真正置位的令牌数（供日志核对）
    int RequestAllCancels()
    {
        int n = 0;
        for (const auto& pTok : m_vActiveCancels)
        {
            if (pTok) { pTok->Request(); ++n; }
        }
        return n;
    }

    const wchar_t* PageName() const override { return L"Tools"; }
    // [R231] Colours only -- the grid layout and expert mode must survive.
    void RefreshThemeColors() override;
    // [R-B30] 备份快捷键 Ctrl+Shift+S 绑定 EToolDlgKind::TD_CAPTURE（备份=另存为映像，0xE104）
    void OnGlobalSave() override;
private:
    // [R94] ToolWin11ImagePreprocess 拆分辅助（原 168 行单函数 → 薄壳 + 4 辅助，零行为改动）：
    //   WpPickImage      选镜像文件并校验存在；失败弹框后返回 false
    //   WpPickIndex      枚举索引并让用户选（单索引不打扰）；返回 0 表示取消
    //   WpMakeMountDir   计算临时挂载点（必须与镜像同卷），并确保目录存在
    //   WpReadImageBuild 挂载后读离线映像真实 build（读不到返回 0，由 Rust 拒绝）
    [[nodiscard]] bool WpPickImage(std::wstring& wim);
    [[nodiscard]] bool WpPickIndex(const std::wstring& wim, DWORD& dwIndex);
    [[nodiscard]] bool WpMakeMountDir(const std::wstring& wim, std::wstring& mountDir);
    DWORD WpReadImageBuild(const std::wstring& mountDir);
    // [R14-03] BuildPage 拆分辅助（原 324 行单函数 → 薄壳+5 辅助，零行为改动）：
    //   BpReadExpertMode 读取专家模式开关；BpBuildCards 卡片网格循环；
    //   BpBuildCard 单张工具卡片（专家模式隐藏返回 nullptr）；
    //   BpLogAvailabilityStats 11 工具可用性总览+进程 Top20；BpFillCategoryCombo 分类筛选。
    bool BpReadExpertMode();
    void BpBuildCards(DuiLib::CTileLayoutUI* pGrid, bool bExpertMode,
                      const wchar_t* kToolCatNames[3]);
    DuiLib::CControlUI* BpBuildCard(int nKind, const wchar_t* kToolCatNames[3], bool bExpertMode);
    // [R170] 从 BpBuildCard 抽出：四个子控件装配（图标/标题/分类/描述）
    //    形参用基类 CVerticalLayoutUI*：CClickVLayoutUI 派生自它，
    //    这样头文件不必包含 Page_ToolsCards.h（该头只被实现 TU 需要）。
    DuiLib::CControlUI* BpAddIcon(DuiLib::CVerticalLayoutUI* pItem, int nKind);
    //    文本类返回 CTextUI*（SetTextColor 是 CTextUI 的成员，CControlUI 没有）；
    //    图标返回 CControlUI*（后续只需 SetEnabled，基类即可）。
    DuiLib::CTextUI* BpAddLabel(DuiLib::CVerticalLayoutUI* pItem, int nKind);
    DuiLib::CTextUI* BpAddCat(DuiLib::CVerticalLayoutUI* pItem, int nCat,
                              const wchar_t* kToolCatNames[3]);
    DuiLib::CTextUI* BpAddDesc(DuiLib::CVerticalLayoutUI* pItem, int nKind);
    void BpLogAvailabilityStats();
    void BpFillCategoryCombo();

    // [R107] 进度条轮询是否已开启（防重复 SetTimer）
    bool m_bToolProgressOn;

    // [R-B35] 外壳右键 /ESDDECRYPT 预填的源 ESD 路径（一次性，RunDialog 消费后清空）
    CDuiString m_sEsdInitialFile;

    // [R14-04] ToolChunGe 拆解：确认/密码/纸条/账户操作/回滚/恢复说明/完成提示
    bool CgConfirmRisk(int& nAutoLogon, int& nNoUac);       // 三级风险确认
    void CgGenStrongPassword(CDuiString& sPwd);             // BCryptGenRandom 强随机密码
    void CgWriteDesktopNote(const CDuiString& sPwd,
                            int nAutoLogon, int nNoUac);    // 桌面密码纸条
    void CgReadAdminFlags(DWORD& dwOldFlags, bool& bKnownFlags);  // 读取 Administrator 现有标志
    DWORD CgActivateAdmin(bool bKnownFlags, DWORD dwOldFlags);    // ① 激活账户
    DWORD CgSetPassword(const CDuiString& sPwd);            // ② 设置强随机密码
    DWORD CgSetAutoLogon(const CDuiString& sPwd, DWORD dwNe);           // ③ 自动登录
    DWORD CgDisableUac(DWORD dwNe);                                   // ④ 关闭 UAC
    void CgRollbackOnFail(bool bKnownFlags, DWORD dwOldFlags, DWORD dwNe);  // 失败回滚
    // [AUDIT-P0] 返回 nullopt=全部字节落盘；有值=Win32 错误码。
    // [[nodiscard]] 强制调用方处理失败，避免"写盘失败仍报成功"。
    [[nodiscard]] std::optional<DWORD> CgWriteRecoveryNote(int nAutoLogon, int nNoUac);
    void CgShowDone(int nAutoLogon, int nNoUac,
                    const std::optional<DWORD>& optNoteErr);   // 完成提示（按真实落盘结果分支）
};
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
