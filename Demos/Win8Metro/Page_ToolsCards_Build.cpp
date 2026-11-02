#include "stdafx.h"

// ============================================================
// Page_ToolsCards_Build.cpp —— [R97] 工具箱"卡片网格构建组"
//
// 从 Page_Tools.cpp 切出的 **4 个**成员函数（函数体零逻辑改动）：
//   BpReadExpertMode        读详细设置 [Advanced] ExpertMode（R-B32 消费点）
//   BpBuildCards            TileLayout 逐 kind 建卡；专家模式隐藏高风险项
//   （BpBuildCard 单卡构建本就在 Page_ToolsBuild.cpp，未动）
//   BpLogAvailabilityStats  11 工具可用性总览 + 进程 Top20
//   BpFillCategoryCombo     分类筛选下拉填充
//
// [R97 拆分前提] 按 R96 确立的流程先做三项预检，全部通过后才动手：
//   · 文件换行符实测为 CRLF（不是假设）；
//   · 切片 211..292 位于 Page_Tools.cpp 两个匿名 namespace
//     （52..76 / 330..385）之间的普通作用域内，不跨边界；
//   · 段内无 namespace 关键字，括号平衡为 0。
//   —— R92 就是漏了第二步，切出无配对的命名空间闭括号，13 个编译错误。
//
// 为什么这组可以独立：
//   五者共同构成"把数据变成卡片控件"的构建层，与命令分发、
//   消息处理、工具执行三层均无共享 static；
//   切片外调用点：BuildPage（建网格）、RefreshToolGrid（重建）、
//   ApplyToolsFilter（按分类过滤），全部经类声明访问。
//
// 拆分动因：Page_Tools.cpp R96 后仍是 619 行，为工具箱最大文件。
// ============================================================

#include "Page_Tools.h"
#include "Page_ToolsCommon.h"   // 跨文件共享的 RAII 守卫与 UI helper
#include "Page_ToolsCards.h"    // 卡片/图标自绘层（CClickVLayoutUI / CToolIconUI）
#include "StringManager.h"
#include "SystemInfoService.h"
#include "ConfigReader.h"       // [R-B32] ExpertMode 消费点
#include "ActivationKitClient.h" // ActivationKit::IsReady（卡片可用性）
#include "DiagnosticsService.h"    // Top20ProcessesToLog

using namespace DuiLib;
using i18n::CStringManager;

namespace {
using ToolCards::CClickVLayoutUI;
using ToolCards::CToolIconUI;
} // namespace
// [R-B32] ExpertMode 消费点：详细设置『专家模式』开关此前只存不读。
// 非专家模式（默认）下隐藏高风险（nRisk==2）工具卡片（当前仅系统还原；
// [R-B85] 引导修复已降为中风险默认显示）。读取失败/键缺失按默认 0 处理。
bool CPageTools::BpReadExpertMode()
{
    bool bExpertMode = false;
    {
        i18n::CConfigReader::Load(nullptr);
        std::wstring wsExpert = i18n::CConfigReader::GetString(
            L"Advanced", L"ExpertMode", L"0");
        bExpertMode = (wsExpert == L"1");
        m_ctx.HostLog(bExpertMode
                      ? L"[tools] ExpertMode=1 (高级项可见)"
                      : L"[tools] ExpertMode=0 (隐藏高风险卡片)");
    }
    return bExpertMode;
}

// [R-B19] TileLayout 卡片网格：逐 kind 建卡；专家模式隐藏项不入网格，
// 但 nKind 照常自增，保持 toolitem_N → nKind 映射不断裂。
void CPageTools::BpBuildCards(CTileLayoutUI* pGrid, bool bExpertMode,
                              const wchar_t* kToolCatNames[3])
{
    int nKind = 0;
    for (int c = 0; c < kToolCatCount; ++c)
    {
        CControlUI* pItem = BpBuildCard(nKind, kToolCatNames, bExpertMode);
        if (pItem) pGrid->Add(pItem);
        ++nKind;
    }
}

// [R14-03] 单张工具卡片（名称/分类/描述/依赖置灰；[U-3] 风险徽章已移除）。
// 非专家模式且 nRisk==2 时返回 nullptr（卡片已构造但不入网格，与原 continue 等价）。

// [R58-B] 11 个工具可用性统计横幅；[R59-B] 同机记录进程 Top-20 快照。
void CPageTools::BpLogAvailabilityStats()
{
    int nAvail = 0, nDisabled = 0;
    for (int k = 0; k < kToolCatCount; ++k)
    {
        // [R01] 激活备份卡（kind=11）可用性以 ActivationKit.dll 装载为准
        bool bOk = (k == 11) ? ActivationKitClient::IsReady()
                             : BpToolAvailable(kToolDepends[k]);
        if (bOk) ++nAvail;
        else ++nDisabled;
    }
    CDuiString sStat;
    sStat.Format(StringMgr.Get("misc.hd772"),
                 nAvail, nDisabled);
    m_ctx.HostLog(sStat.GetData());
    // [R59-B] 后台/进程 Top-20 排序：每次进入工具箱页自动记录一次进程快照
    // 简化版：按进程名字典序取 Top-20；完整日志落 %LOCALAPPDATA%\DismTool_Clone\logs\proctop20.log
    // [FIX-P2P] 快照+落盘挪后台线程：CreateToolhelp32Snapshot 全系统枚举
    // 原在 PAGE_BUILD（UI 线程延迟构建）内执行，切 Tools 页一次卡顿数十毫秒。
    // HostLog 经 AppendLogEx（已互斥）线程安全；页面常驻，WM_CLOSE 的
    // WaitAll 保证后台任务先于页面析构结束。
    {
        struct Top20Ctx { IPageContext* pSelf; };
        Top20Ctx* pTopCtx = new Top20Ctx{ &m_ctx };
        HANDLE hTop = BackgroundJobs::Start([](void* pv) -> DWORD {
            std::unique_ptr<Top20Ctx> guard(static_cast<Top20Ctx*>(pv));
            CDuiString sProc = DiagnosticsService::Top20ProcessesToLog();
            if (!sProc.IsEmpty()) guard->pSelf->HostLog(sProc.GetData());
            return 0;
        }, pTopCtx, "proc-top20");
        if (!hTop) delete pTopCtx;
    }
}

// [R119-1] 填充 tools_cat_filter 分类筛选 Combo（幂等：已有 4 项即跳过）
void CPageTools::BpFillCategoryCombo()
{
    CComboUI* pCatCombo = static_cast<CComboUI*>(
        m_ctx.HostFindControl(L"tools_cat_filter"));
    if (pCatCombo && pCatCombo->GetCount() == 0)
    {
        const wchar_t* pszCats[] = {
            StringMgr.Get("toolbox.cat_all"),
            StringMgr.Get("toolbox.cat_sys"),
            StringMgr.Get("toolbox.cat_image"),
            StringMgr.Get("toolbox.cat_util"),
        };
        for (int ci = 0; ci < 4; ++ci)
        {
            auto pItem = std::make_unique<CListLabelElementUI>(); // [C++17 R3]
            pItem->SetText(pszCats[ci]);
            pItem->SetFixedHeight(24);
            pCatCombo->Add(pItem.release());  // [C++17 R3] ownership to DuiLib
        }
        pCatCombo->SelectItem(0);
    }
}
