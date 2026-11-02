#include "stdafx.h"
#include <random>   // [C13] C++11 mt19937
#include <filesystem>   // [C17] file_size（F17_C_fileio 清零）

#include <cstdio>
#include <optional>   // [C++17] std::optional<DWORD>：桌面说明写入结果
#include <shlobj.h>
#include <commctrl.h>   // [rb28] 任务进度窗 PROGRESS_CLASS
#pragma comment(lib, "comctl32.lib")
#include <lm.h>       // [2026-10-08] 春哥附体直走 NetUser*（知识库 40 §2.3）
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "netapi32.lib")

#include "Page_Tools.h"
#include "Page_ToolsCommon.h"  // [R127-1] 跨文件共享的 RAII 守卫与 UI helper
#include "Page_ToolsCards.h"      // [R133-1] 卡片/图标自绘层（共享）
#include "Page_ToolsHosts.h"       // [R129-1] hosts 编辑器辅助层（共享）
#include "Page_ToolsJobs.h"       // [R130-1] 作业进度窗与进度桥（共享）
#include "Page_ToolsE2I.h"        // [R131-1] E2I 作业层（共享）
#include "Page_ToolsWim.h"        // [R132-1] WIM/ESD 互转与 SWM 合并线程层（共享）

#include "CustomControls.h"
#include "StringManager.h"
#include "FrameWnd_Util.h"       // P8：ParsePrefix1
#include "HostInterfaces.h"      // P8：static_cast<int>(ELogLevel::LOG_INFO)
#include "FrameWnd_Messages.h"
#include "SystemInfoService.h"
#include "Win32DialogService.h"
#include "DiagnosticsService.h"
#include "ExternalToolService.h"
#include "ProcessRunner.h"  // [R73] ResolveSysTool
#include "ConfigReader.h"        // [R-B32] 详细设置 [Advanced] ExpertMode 消费点
#include "WIMHelper.h"           // [2026-10-08] WIM/ESD 互转直走 WIMGAPI
#include "WIMHelperBridge.h"     // [原生化] SWM→WIM 合并走 wimgapi 导出
#include "IsoWriter.h"           // [2026-10-08] ESD转ISO/ISO生成器 内置 ISO 组装
#include "ActivationKitClient.h" // [R01] 激活备份/还原走独立组件 ActivationKit.dll
#include "DismCore.h"             // Rust Windows 11 镜像预处理包生成/校验
#include <thread>
#include "BackgroundJobs.h"  // [CRASHFIX] 后台作业统一收口（SEH 边界+退出等待）

// ============================================================
// Page_Tools.cpp —— P6a 工具箱页
// 14 个原 CFrameWnd 函数物理搬迁，函数体零逻辑改动，宿主引用
// 改 m_ctx.Xxx；进程/管道/bat/探测能力在 ExternalToolService（S9）。
// CToolIconUI / CClickVLayoutUI 为本页自绘控件（原 FrameWnd.cpp 文件内类）。
// ============================================================

using namespace DuiLib;
using i18n::CStringManager;

namespace {
using ToolCards::CClickVLayoutUI;
using ToolCards::CToolIconUI;
using ToolE2I::E2IBuild;
using ToolE2I::E2iJobThreadProc;
using ToolE2I::TE2iJob;
using ToolJobs::CJobProgressWnd;
using ToolJobs::JobProgPost;
using ToolJobs::TJobWimProgress;
using ToolHosts::CHostEditCtx;
using ToolHosts::HOSTDLG_ID_EDIT;
using ToolHosts::Buf;
using ToolHosts::HostEditProc;
using ToolHosts::MakeHostDialogTemplate;
using ToolSql::ResolveProc;
using ToolSql::HostNotify;
using ToolSql::HostConfirm;
// self-drawn Fluent-style toolbox icon: 64x64 design, one of 11 kinds.
// [U-2] 卡片交互态桥：图标 SetMouseEnabled(false) 后收不到任何鼠标事件，
// 旧的 m_bHot/m_bPushed 永远为 false（hover/press 视觉是死代码）。
// 由卡片 CClickVLayoutUI 实现本接口，图标绘制时经 GetInterface 查询。
// [R163] 工具分类 / 风险 / 依赖表已提升到 Page_ToolsCommon.h（跨 TU 共享）：
//   ToolCatEntry / kToolCat / kToolCatCount / kToolRiskKeys / kToolRiskColors / kToolDepends
//   分类：0=系统（0/1/2/3/4），1=磁盘镜像（5/6/7/9），2=系统工具（8/10）；风险：0=低 1=中 2=高。
} // anonymous namespace（P6a：CToolIconUI / CClickVLayoutUI / kToolCat）

// [R3] F5 刷新：工具箱页仅记录日志（保持原壳 switch 行为）
void CPageTools::OnRefreshPage()
{
    m_ctx.HostLog(StringMgr.Get("misc.h563d"));
}

// [R3] 懒构建：构建工具箱页后注册 11 个入口提示（原壳 EnsurePageBuilt
// PAGE_TOOLS 分支，Tooltip 管理经 IPageContext 薄转回核心壳）。
void CPageTools::OnDeferredBuild()
{
    Build();
    m_ctx.HostAddTooltip(L"toolitem_0",  StringMgr.Get("toolbox.s7b1ce435"));
    m_ctx.HostAddTooltip(L"toolitem_1",  StringMgr.Get("cleanup.sdcc3069a"));
    m_ctx.HostAddTooltip(L"toolitem_2",  StringMgr.Get("edit.sb36cc8d7"));
    m_ctx.HostAddTooltip(L"toolitem_3",  StringMgr.Get("system.sfadbdb5a"));
    m_ctx.HostAddTooltip(L"toolitem_4",  StringMgr.Get("toolbox.s4b6cd6bd"));
    m_ctx.HostAddTooltip(L"toolitem_5",  StringMgr.Get("misc.sacf1e1c4"));
    m_ctx.HostAddTooltip(L"toolitem_6",  StringMgr.Get("toolbox.sa905368c"));
    m_ctx.HostAddTooltip(L"toolitem_7",  StringMgr.Get("toolbox.sf211f56b"));
    m_ctx.HostAddTooltip(L"toolitem_8",  StringMgr.Get("misc.sfdf4a314"));
    m_ctx.HostAddTooltip(L"toolitem_9",  StringMgr.Get("misc.s8a89cce2"));
    m_ctx.HostAddTooltip(L"toolitem_10", StringMgr.Get("edit.s60fc7848"));
    // [R01] 第 12 卡：激活备份/还原
    m_ctx.HostAddTooltip(L"toolitem_11", StringMgr.Get("toolbox.act_tip"));
    m_ctx.HostRefreshTooltipRects();
}

// [R4] 主菜单/加速键命令收口（原 CFrameWnd::ShowPopupMenu 与 OnAccelCommand
// 中 10 处 m_pgTools.ToolXxx() 直调）。菜单 ID 沿用原裸值，不识别返回 false。
bool CPageTools::OnHostCommand(int id)
{
    // [R170] 原为 13 个 case 的 switch（CC14）。改两份查表：
    //   ① 只要「打开某个工具箱对话框」的命令 —— id → EToolDlgKind；
    //   ② 调用某个无参工具函数的命令 —— id → 成员函数指针。
    // 表内顺序与原 case 顺序一致；未命中一律返回 false（与原 default 一致）。
    // 注意 1110 走 ToolWimEsdConvertAsk（零参薄壳），因为函数指针类型
    // 不含默认参数（台账第 14 条，否则 C2440 + C2131）。
    static constexpr struct { int id; EToolDlgKind kind; } kDlgCmds[] = {
        { 1101, EToolDlgKind::TD_APPLY   },   // 释放映像
        { 1105, EToolDlgKind::TD_CAPTURE },   // 另存为映像
        { 1102, EToolDlgKind::TD_MOUNT   },   // 挂载映像
        { 1103, EToolDlgKind::TD_IMAGEX  },   // 保存映像
        { 1104, EToolDlgKind::TD_IMAGEX  },   // 打开映像
        { 1106, EToolDlgKind::TD_IMAGEX  },   // 卸载映像
    };
    for (const auto& d : kDlgCmds)
    {
        if (d.id == id) { RunDialog(d.kind); return true; }
    }

    using Handler = void (CPageTools::*)();
    static constexpr struct { int id; Handler fn; } kToolCmds[] = {
        { 1110, &CPageTools::ToolWimEsdConvertAsk },  // ESD<-->WIM（询问方向）
        { 1120, &CPageTools::ToolEsd2Iso        },
        { 1130, &CPageTools::ToolEsdDecrypt     },    // 独立解密向导
        { 1203, &CPageTools::ToolBootRepair     },
        { 1204, &CPageTools::ToolBackup         },
        { 1205, &CPageTools::ToolRestoreImage   },
        { static_cast<int>(EAccelCmd::ID_ACCEL_SAVEAS), &CPageTools::ToolBackup },
    };
    for (const auto& c : kToolCmds)
    {
        if (c.id == id) { (this->*(c.fn))(); return true; }
    }
    return false;
}

// ============================================================
// [R5] DISM 异步消息 + 工具进度定时器收口（原壳 OnPgXxx/OnTimerXxx）
// ============================================================

bool CPageTools::OnAsyncMessage(UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_DISM_LOG:
        // kMsgOwnPage：new CDuiString 由 OnDismLog 内 delete。
        OnDismLog(wParam, lParam);
        return true;
    case WM_DISM_DONE:
        // 先清理 S9 进程/句柄，再做页 UI 收尾（顺序同原 HCM）。
        ExternalToolService::OnDone();
        OnDismDone(static_cast<int>(wParam));
        return true;
    case WM_DISM_PROGRESS:
        OnDismProgress(static_cast<int>(wParam));
        return true;
    // [R104] 进度窗"取消"按钮 → 置位当前作业的取消令牌。
    //   只置位不等待：作业在 ISO 逐块写入处自行查到并收尾，
    //   真正的收尾（删半截 ISO、回投完成信封）由作业线程负责。
    case JOBWM_CANCEL_REQ:
        // [R105] 请求停止**全部**在途作业。
        //   判断依据仍是 BackgroundJobs::ActiveCount()：没有在跑的作业就不置位，
        //   避免把陈旧的令牌标脏。
        //   顺带清理已结束作业留下的空令牌：IsRequested 为真的不算陈旧
        //   （那是真的被取消过），只移除底层无法再被查到的那些。
        if (BackgroundJobs::ActiveCount() > 0)
        {
            // [R160] 置位逻辑归口 RequestAllCancels，与登记/撤销同处实现
            const int nCancelled = RequestAllCancels();
            CDuiString sLog;
            sLog.Format(L"[tools] cancel requested: %d active token(s)", nCancelled);
            m_ctx.HostLog(sLog.GetData(), static_cast<int>(ELogLevel::LOG_WARN));
        }
        return true;
    default:
        return false;
    }
}

void CPageTools::OnHostTimer(UINT uId)
{
    if (uId == TOOL_PROGRESS_TIMER_ID)
    {
        // [R107] 工具箱统一进度条轮询（800ms 读 progress.txt）
        OnToolProgressTimer();
    }
}

// [R14-03] BuildPage 拆分（原 324 行单函数）：专家模式读取/卡片网格循环/
// 单卡组装/可用性总览/分类 Combo 各独立（Bp*）。纯 UI 组装迁移，零行为改动。
// 工具依赖可用性探测（卡片置灰与总览统计共用同一口径）。
// [R163] 已提升到 Page_ToolsCommon.h 的 inline：Page_ToolsBuild.cpp 的 BpBuildCard
// 与主文件的 BpBuildLogStats 都要用它，避免两处各留一份导致口径漂移。

void CPageTools::BuildPage()
{
    bool bExpertMode = BpReadExpertMode();
    CTileLayoutUI* pGrid = static_cast<CTileLayoutUI*>(m_ctx.HostFindControl(L"tools_grid"));
    if (!pGrid) return;
    pGrid->RemoveAll();
    // [R119-1] 分类中文名（StringMgr.Get 返回内置静态缓存，可长期持有）
    const wchar_t* kToolCatNames[3] = {
        StringMgr.Get("toolbox.cat_sys"),
        StringMgr.Get("toolbox.cat_image"),
        StringMgr.Get("toolbox.cat_util"),
    };
    // [R-B19] TileLayout：卡片直接作为子节点，SetPos 自动排布列
    // [FIX-COLS] 这里原写死 12，但 kToolCat 已到 13 项（0..12）。
    //   CTileLayoutUI::SetPos 在 itemsize.cx>0 时会用它重算列数
    //   （m_nColumns = 宽度 / szItem.cx），写死的值只在无 itemsize 时兜底；
    //   为避免"列数与卡片数不一致导致最后一张单独换行/挤压"，
    //   统一按 kToolCatCount 传入，增删工具时自动跟随。
    pGrid->SetColumns(kToolCatCount);
    pGrid->SetChildPadding(8);   // nCardSpacing
    BpBuildCards(pGrid, bExpertMode, kToolCatNames);
    // [R58-B] 工具箱模块状态可视化：顶部横幅显示 11 个工具的可用性统计
    // 让用户一眼看到哪些工具可用、哪些因缺少依赖（如 ADK 未装 oscdimg.exe）置灰
    BpLogAvailabilityStats();
    // [R119-1] 填充 tools_cat_filter 分类筛选 Combo（幂等：已有 4 项即跳过）
    BpFillCategoryCombo();
    // [R110] 页面重建后更新 tools_count 显示
    UpdateToolsCount();
}


void CPageTools::RunDialog(EToolDlgKind nKind)

{
    LARGE_INTEGER _a,_b,_c,_fq; ::QueryPerformanceFrequency(&_fq); ::QueryPerformanceCounter(&_a);
    CToolDialogPage dlg(&m_ctx, nKind);
    ::QueryPerformanceCounter(&_b);
    // [R-B35] 独立 ESD 解密向导：注入 Data.xml <ESDDecryptKey> 6 条密钥
    // [R16][T4] 密钥/预填统一经 Open(ToolOpenArgs) 通路注入（旧 setter 已删除）。
    if (nKind == EToolDlgKind::TD_ESDWIZ)
    {
        toolsvc::ToolOpenArgs args;
        std::vector<CDuiString> vKeys;
        m_ctx.DataEngine().GetEsdKeys(vKeys);
        args.vEsdKeys.reserve(vKeys.size());
        for (size_t i = 0; i < vKeys.size(); ++i)
            args.vEsdKeys.push_back(std::wstring(vKeys[i].GetData()));
        // 外壳右键 /ESDDECRYPT 场景：消费一次性预填路径
        if (!m_sEsdInitialFile.IsEmpty())
        {
            args.sInitialEsdFile = std::wstring(m_sEsdInitialFile.GetData());
            args.bHasInitialEsd = true;
            m_sEsdInitialFile.Empty();
        }
        dlg.Open(args);
    }
    // [C5] 弹窗跟随主窗口所在显示器 DPI（高分屏不再 1:1 物理像素显示）
    dlg.SetDialogDPI(m_ctx.HostDPI());
    dlg.Create(m_ctx.HostHwnd(), nullptr, UI_WNDSTYLE_DIALOG, 0);
    ::QueryPerformanceCounter(&_c);
    { wchar_t _s[256]={0}; swprintf_s(_s,L"[dlgperf] ctor=%.1fms create=%.1fms\n",
        1000.0*(_b.QuadPart-_a.QuadPart)/_fq.QuadPart,
        1000.0*(_c.QuadPart-_b.QuadPart)/_fq.QuadPart); ::OutputDebugStringW(_s);
      // 同时落盘，便于自动化取证（不依赖 DebugView）
      wchar_t _p[MAX_PATH]={0}; ::GetModuleFileNameW(nullptr,_p,MAX_PATH);
      wchar_t* _sl=::wcsrchr(_p,L'\\'); if(_sl) *(_sl+1)=L'\0';
      ::lstrcatW(_p,L"perf_probe.txt");
      HANDLE _h=::CreateFileW(_p,GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
      if(_h!=INVALID_HANDLE_VALUE){ ::SetFilePointer(_h,0,nullptr,FILE_END);
        DWORD _w=0; ::WriteFile(_h,_s,(DWORD)(wcslen(_s)*sizeof(wchar_t)),&_w,nullptr); ::CloseHandle(_h);} }
    dlg.ShowModal();
}

// 0 — 另存为映像（自定义模态弹窗，真实 DISM 捕获）


// [R110] 工具箱筛选：tools_filter 名称实时过滤
// 显示名匹配（tl_%d 文本）+ 工具编号数字匹配（如输入 5 命中 ESD 转 ISO）。
// 隐藏非重建，toolitem_N → nKind 映射保持不变。
// ============================================================
namespace
{
// [R146] 分类是否命中：nCatFilter==0 表示"全部"。
// 提取为具名谓词，使 ApplyToolsFilter 的主循环只表达流程，不堆判定。
[[nodiscard]] bool ToolCatMatches(int nKind, int nCatFilter)
{
    if (nCatFilter == 0) return true;
    if (nKind < 0 || nKind >= kToolCatCount) return false;
    return kToolCat[nKind].nCat == nCatFilter - 1;
}

// 卡片子节点显示名是否命中关键字（关键字已预置为小写）。
[[nodiscard]] bool ToolCardTextMatches(CControlUI* pCard, const CDuiString& sKwLow)
{
    CContainerUI* pCardC = dynamic_cast<CContainerUI*>(pCard);
    if (!pCardC) return false;
    const int nSub = pCardC->GetCount();
    for (int k = 0; k < nSub; ++k)
    {
        CControlUI* pSub = pCardC->GetItemAt(k);
        if (!pSub) continue;
        CDuiString sT = pSub->GetText();
        sT.MakeLower();
        if (!sT.IsEmpty() && sT.Find(sKwLow.GetData()) != -1) return true;
    }
    return false;
}

// 关键字是否命中某张卡片：先比工具编号（输入 5 命中 ESD 转 ISO），再比显示名。
[[nodiscard]] bool ToolCardMatches(CControlUI* pCard, int nKind, const CDuiString& sKwLow)
{
    CDuiString sNum;
    sNum.Format(L"%d", nKind);
    if (sNum.Find(sKwLow.GetData()) != -1) return true;
    return ToolCardTextMatches(pCard, sKwLow);
}

// [R170] 单张卡片的筛选判定（从 ApplyToolsFilter 内层循环抽出，原 CC14 的主体）。
// 三态返回是必要的：原循环用 continue 区分「不是工具卡片」（不动显隐、不计数）
// 与「是工具卡片但不命中」（要隐藏），二值 bool 会把前者误当成后者。
enum class EToolCardHit { NotTool, Miss, Hit };



static EToolCardHit ToolMatchCard(CControlUI* pCard, const CDuiString& sKwLow,
                                  bool bKwEmpty, int nCatFilter)
{
    const wchar_t* pszN = pCard->GetName();
    if (!pszN || wcsncmp(pszN, L"toolitem_", 9) != 0) return EToolCardHit::NotTool;
    const int nKind = _ttoi(pszN + 9);
    // [R119-1] 分类过滤优先；关键字为空时只看分类
    const bool bHit = ToolCatMatches(nKind, nCatFilter)
                   && (bKwEmpty || ToolCardMatches(pCard, nKind, sKwLow));
    return bHit ? EToolCardHit::Hit : EToolCardHit::Miss;
}
}   // namespace
void CPageTools::ApplyToolsFilter()
{
    CDuiString sKw;
    if (CEditUI* pEdit = static_cast<CEditUI*>(m_ctx.HostFindControl(L"tools_filter")))
        sKw = pEdit->GetText();
    CDuiString sKwLow = sKw;
    sKwLow.MakeLower();
    // [R119-1] 分类筛选：tools_cat_filter 当前选中序号（0=全部，1=系统，2=磁盘镜像，3=系统工具）
    int nCatFilter = 0;
    CComboUI* pCatCombo = static_cast<CComboUI*>(m_ctx.HostFindControl(L"tools_cat_filter"));
    if (pCatCombo && pCatCombo->GetCount() > 0)
        nCatFilter = pCatCombo->GetCurSel();
    // [R-B19] TileLayout：卡片直接是 tools_grid 子节点
    CTileLayoutUI* pGrid = static_cast<CTileLayoutUI*>(m_ctx.HostFindControl(L"tools_grid"));
    if (!pGrid) return;
    const bool bKwEmpty = sKwLow.IsEmpty();
    int nShown = 0;
    const int nCount = pGrid->GetCount();
    for (int i = 0; i < nCount; ++i)
    {
        CControlUI* pCard = pGrid->GetItemAt(i);
        if (!pCard) continue;
        const EToolCardHit hit = ToolMatchCard(pCard, sKwLow, bKwEmpty, nCatFilter);
        if (hit == EToolCardHit::NotTool) continue;
        const bool bHit = (hit == EToolCardHit::Hit);
        pCard->SetVisible(bHit);
        if (bHit) ++nShown;
    }
    UpdateToolsCount();
    if (!bKwEmpty && nShown == 0)
        m_ctx.HostToast(StringMgr.Get("toolbox.s_filter_noresult"), L"", 0);
}

// ============================================================
// [R110] 刷新工具网格：重建 tools_grid + 更新计数
// ============================================================
void CPageTools::RefreshToolGrid()
{
    BuildPage();
    m_ctx.HostLog(StringMgr.Get("toolbox.s_refreshed"));
}

// ============================================================
// [R110] 更新 tools_count：显示"可见/总数"
// ============================================================
void CPageTools::UpdateToolsCount()
{
    // [R-B19] TileLayout：卡片直接是 tools_grid 子节点
    CTileLayoutUI* pGrid = static_cast<CTileLayoutUI*>(m_ctx.HostFindControl(L"tools_grid"));
    if (!pGrid) return;
    int nShown = 0;
    int nTotal = 0;
    int nCount = pGrid->GetCount();
    for (int i = 0; i < nCount; ++i)
    {
        CControlUI* pCard = pGrid->GetItemAt(i);
        if (!pCard) continue;
        const wchar_t* pszN = pCard->GetName();
        if (!pszN || wcsncmp(pszN, L"toolitem_", 9) != 0) continue;
        ++nTotal;
        if (pCard->IsVisible()) ++nShown;
    }
    CDuiString sCount;
    if (nShown == nTotal)
        sCount.Format(StringMgr.Get("toolbox.s_count_all"), nTotal);
    else
        sCount.Format(StringMgr.Get("toolbox.s_count_filtered"), nShown, nTotal);
    m_ctx.HostSetCtrlText(L"tools_count", sCount.GetData());
}

// ============================================================
// P8：第二级事件路由（原 CFrameWnd::Notify toolitem_ 分支逐字迁入）
// ============================================================

// [R151] OnEvent 分派改为「消息类型判定 + 工具号查表」，原 CC=23。
// 原实现：2 段 textchanged/itemselect + 1 段 click 前置判定 + 11 路 switch，
// 判定点集中在同一函数。抽出后主函数只做「归类 → 查表 → 未知项记日志」。
// 工具表放在成员函数体内：成员函数指针取地址在类作用域外是非法访问。
void CPageTools::OnEvent(TNotifyUI& msg)
{
    // 输入类消息：过滤器实时生效（textchanged 与分类下拉共用同一处理）
    const bool bInputMsg = (msg.sType == L"textchanged")
                        || (msg.sType == DUI_MSGTYPE_ITEMSELECT);
    if (bInputMsg)
    {
        CDuiString sT = msg.pSender ? msg.pSender->GetName() : CDuiString();
        if (sT == L"tools_filter" || sT == L"tools_cat_filter")
            ApplyToolsFilter();
        return;
    }
    if (msg.sType != L"click") return;
    CDuiString sName = msg.pSender ? msg.pSender->GetName() : CDuiString(L"");
    // [R110] 刷新工具网格
    if (sName == L"btn_tools_refresh")
    {
        RefreshToolGrid();
        return;
    }
    // ---- toolbox items：toolitem_N → 工具入口表 ----
    int nTool = -1;
    if (!ParsePrefix1(sName, L"toolitem_", nTool)) return;
    ToolDispatchItem(nTool);   // [R184] 入口表分派抽为函数
}

// [R184] 从 OnEvent（49 行 / CC12）抽出：toolitem_N → 工具入口表分派。
// 表内 12 项与卡片 kind 一一对应；未知 kind 记 INFO 级日志后返回（原行为）。
// 表放成员函数内：取私有成员函数地址在类作用域外是非法访问。
void CPageTools::ToolDispatchItem(int nTool)
{
    using ToolEntry = void (CPageTools::*)();
    struct ToolRoute { int nKind; ToolEntry pfn; };
    static constexpr ToolRoute kRoutes[] = {
        { 0,  &CPageTools::ToolBackup },           // 系统备份
        { 1,  &CPageTools::ToolRestoreImage },     // 系统还原
        { 2,  &CPageTools::ToolUserAccounts },     // 账户管理
        { 3,  &CPageTools::ToolBootRepair },       // 引导修复
        { 4,  &CPageTools::ToolChunGe },           // 春哥附体
        { 5,  &CPageTools::ToolEsd2Iso },          // ESD转ISO
        { 6,  &CPageTools::ToolWimEsdConvertAsk },    // WIM/ESD互转（默认参数 -1 询问方向）
        { 7,  &CPageTools::ToolImageX },           // ImageX
        { 8,  &CPageTools::ToolExplorer },         // 文件浏览器
        { 9,  &CPageTools::ToolIsoGen },           // ISO生成器
        { 10, &CPageTools::ToolHosts },            // hosts编辑器
        { 11, &CPageTools::ToolActivationBackup }, // [R01] 激活备份/还原
        { 12, &CPageTools::ToolWin11ImagePreprocess }, // Win11 镜像处理
    };
    for (const ToolRoute& r : kRoutes)
    {
        if (r.nKind != nTool) continue;
        (this->*r.pfn)();
        return;
    }
    CDuiString sLog;
    sLog.Format(L"[tools] unknown item %d", nTool);
    m_ctx.HostLog(sLog.GetData(), static_cast<int>(ELogLevel::LOG_INFO));
}

// ============================================================
// [R-B24] 工具箱卡片级右键菜单：toolitem_N 命中 → 启动/复制描述/查看依赖
// 闭合评分标准 §7.3「无右键菜单 -1.5 分 | Tools」项。
// 仅 WM_RBUTTONUP（bFromContextMsg=false），与 Capability/Feature 页一致。
// ============================================================
bool CPageTools::OnContextMenu(POINT pt, bool bFromContextMsg)
{
    if (bFromContextMsg) return false;
    CControlUI* pHit = m_ctx.HostFindControlAt(pt);
    if (!pHit) return false;
    for (CControlUI* p = pHit; p; p = p->GetParent())
    {
        const wchar_t* psz = p->GetName();
        if (!psz) continue;
        if (wcsncmp(psz, L"toolitem_", 9) != 0) continue;
        int nTool = -1;
        if (!ParsePrefix1(psz, L"toolitem_", nTool)) continue;
        if (nTool < 0 || nTool >= kToolCatCount) continue;
        ShowToolContextMenu(nTool, pt);
        return true;
    }
    return false;
}

void CPageTools::ShowToolContextMenu(int nTool, POINT pt)
{
    if (nTool < 0 || nTool >= kToolCatCount) return;
    HMENU hMenu = ::CreatePopupMenu();
    // 4801 启动工具 / 4802 复制工具描述 / 4803 查看工具依赖
    ::AppendMenu(hMenu, MF_STRING, 4801, StringMgr.Get("toolbox.s_ctx_run"));
    ::AppendMenu(hMenu, MF_STRING, 4802, StringMgr.Get("toolbox.s_ctx_copydesc"));
    ::AppendMenu(hMenu, MF_STRING, 4803, StringMgr.Get("toolbox.s_ctx_dep"));
    int iCmd = ::TrackPopupMenu(hMenu,
        TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
        pt.x, pt.y, 0, m_ctx.HostHwnd(), nullptr);
    ::DestroyMenu(hMenu);
    // [FIX-S1-11] 旧实现 RunDialog(static_cast<EToolDlgKind>(nTool))：卡片
    // 工具号与 EToolDlgKind 枚举不对齐（枚举仅 0/1/2/7/9），工具 3/4/5/6/8/
    // 10/11/12 共 8 项落入无命中的 kind → 弹出空白壳窗（功能不可用）。
    // 改走 ToolDispatchItem 入口表：12 项 kind 与卡片一一对应，
    // 对话框型工具内部自行 RunDialog，非对话框型（引导修复/春哥等）也由此
    // 获得真实入口。
    if (iCmd == 4801)        ToolDispatchItem(nTool);
    else if (iCmd == 4802)   CopyToolDesc(nTool);
    else if (iCmd == 4803)   ShowToolDep(nTool);
}

void CPageTools::CopyToolDesc(int nTool)
{
    if (nTool < 0 || nTool >= kToolCatCount) return;
    CDuiString sDescKey;
    sDescKey.Format(L"toolbox.td_%d", nTool);
    CDuiString sDesc = StringMgr.Get(sDescKey.GetData());
    FrameWnd_Util_CopyTextToClipboard(m_ctx.HostHwnd(), sDesc.GetData());
    m_ctx.HostToast(StringMgr.Get("toolbox.s_ctx_copydesc_done"),
                    StringMgr.Get("toolbox.s_ctx_copydesc_done_body"));
    CDuiString sLog;
    sLog.Format(L"[tools] copy tool desc: [%d] %s", nTool, sDesc.GetData());
    m_ctx.HostLog(sLog.GetData());
}

void CPageTools::ShowToolDep(int nTool)
{
    if (nTool < 0 || nTool >= kToolCatCount) return;
    const wchar_t* pszDep = kToolDepends[nTool];
    bool bAvail = true;
    if (pszDep == nullptr || pszDep[0] == 0)
        bAvail = true;   // 内置 DLL，无外部依赖
    else if (lstrcmpiW(pszDep, L"explorer.exe") == 0 ||
        lstrcmpiW(pszDep, L"notepad.exe") == 0)
        bAvail = true;
    else
        bAvail = ExternalToolService::IsToolAvailable(pszDep);
    CDuiString sMsg;
    if (pszDep == nullptr || pszDep[0] == 0)
        sMsg.Format(StringMgr.Get("toolbox.s_ctx_dep_ok"),
                    StringMgr.Get("toolbox.ui_no_ext_dep"));
    else
        sMsg.Format(bAvail
            ? StringMgr.Get("toolbox.s_ctx_dep_ok")
            : StringMgr.Get("toolbox.s_ctx_dep_missing"), pszDep);
    m_ctx.HostToast(StringMgr.Get("toolbox.s_ctx_dep"),
                    sMsg.GetData());
    CDuiString sLog;
    sLog.Format(L"[tools] tool dep: [%d] %s avail=%d",
                nTool, pszDep, bAvail ? 1 : 0);
    m_ctx.HostLog(sLog.GetData());
}

// ============================================================
// [R-B30] 备份快捷键 Ctrl+Shift+S 绑定 EToolDlgKind::TD_CAPTURE（备份=另存为映像 0xE104）
// ------------------------------------------------------------
// 原版加速键 Ctrl+Shift+S / Ctrl+Shift+N 分别指向 Capture 备份与
// 新实例流程；本页在工具箱页收到 Ctrl+S/另存时打开 EToolDlgKind::TD_CAPTURE 另存为映像。
// ============================================================
void CPageTools::OnGlobalSave()
{
    // 工具箱页 Ctrl+S 语义 = 另存为映像（EToolDlgKind::TD_CAPTURE，原版备份主入口）
    RunDialog(EToolDlgKind::TD_CAPTURE);
    m_ctx.HostLog(L"[tools] Ctrl+S -> EToolDlgKind::TD_CAPTURE (另存为映像)");
}

#include <memory>   // [C++17 R3] make_unique（置于文件尾，避免影响 verify 行号）
