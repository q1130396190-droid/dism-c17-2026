// ============================================================
// Page_UpdateCbs.cpp —— 更新页「CBS 合并 + 安装结果上报」（从 Page_Update.cpp 拆出）。
//
// [R180] Page_Update.cpp 拆前 1,223 行、规模分 9.12（六页最低）。本文件承接
// 五个成员，合计 137 行，边界按「CBS 组件包并入已安装列表 + 结果回传」划分：
//   · UpdBuildCbsDedup       WUA/CBS 双方 KB 号集合去重（2 层嵌套循环）
//   · UpdAppendCbsToggleRow  默认折叠时的"显示系统组件包(N)"开关行
//   · UpdMergeCbs            CBS 结果并入（去重 → 折叠行 / 直接分批追加）
//   · CollectKbNumbers       从单个字符串收集 KBxxxxxx 形式的 KB 号
//   · PostInstallResultReport 安装结果回传（尽力而为，失败排队 outbox）
//
// 闭包说明：五个成员声明见 Page_Update.h。跨 TU 依赖仅五项且都是头文件级：
//   · m_ctx（HostFindControl / HostLog / HostToast）
//   · m_vCbsPending / m_vCbsDedup / m_bCbsReady / m_bCbsExpanded（成员）
//   · m_vUpdList（按 Tab 的更新项缓存）
//   · UpdateCount / UpdStartBatchRender（成员）
//   · UpdateMetaService::PostReportEvent（UpdateMetaService.h）
//
// 逐字搬运，零逻辑改动。
// ============================================================

#include "stdafx.h"
#include "ThemeTokens.h"   // [R203] 颜色令牌（深色/浅色跟随）

#include "Page_Update.h"

#include "StringManager.h"
#include "UpdateMetaService.h"   // PostReportEvent
#include "CustomControls.h"      // CClickRowUI / CTextUI / CVerticalLayoutUI

#include <memory>

using namespace DuiLib;
using i18n::CStringManager;

// [R180] 上报事件的等待超时（毫秒）。原为 PostReportEvent 调用处的裸字面量 6000，
// 具名后与发起方约定的语义（6 秒内没送达就排队 outbox）显式可见。
inline constexpr int kUpdReportTimeoutMs = 6000;

// ============================================================

// [性能 A2] CBS 结果按 KB 与 WUA 去重后并入"已安装"列表：默认折叠为
// 一行"显示系统组件包(N)"开关，点击后再分批追加，不再默认膨胀数百行。
// [R168] CBS 去重集合构建（从 UpdMergeCbs 抽出，原含 2 层嵌套去重循环）。
// WUA 侧扫描 Title 与 KB 两个字段；CBS 侧只扫 Title（identity 里的 KB 号即 Title 字段）。
void CPageUpdate::UpdBuildCbsDedup()
{
    m_vCbsDedup.clear();
    std::vector<CDuiString> vWuaKb;
    for (size_t i = 0; i < m_vUpdList[1].size(); ++i)
    {
        CollectKbNumbers(m_vUpdList[1][i].sTitle.GetData(), vWuaKb);
        CollectKbNumbers(m_vUpdList[1][i].sKB.GetData(), vWuaKb);
    }
    for (size_t k = 0; k < m_vCbsPending.size(); ++k)
    {
        const UpdEntry& e = m_vCbsPending[k];
        std::vector<CDuiString> vCbsKb;
        CollectKbNumbers(e.sTitle.GetData(), vCbsKb);
        bool bDup = false;
        for (size_t c = 0; c < vCbsKb.size() && !bDup; ++c)
            for (size_t w = 0; w < vWuaKb.size(); ++w)
                if (vWuaKb[w] == vCbsKb[c]) { bDup = true; break; }
        if (!bDup) m_vCbsDedup.push_back(e);
    }
}

// [R168] CBS 折叠开关行（从 UpdMergeCbs 抽出）。
// 默认折叠：仅追加一行"▸ 显示系统组件包（N 个，与已安装更新去重后）"。
void CPageUpdate::UpdAppendCbsToggleRow(CVerticalLayoutUI* pList)
{
    auto pToggle = std::make_unique<CClickRowUI>(); // [C++17 R3]
    pToggle->SetName(L"cbstoggle_row");
    pToggle->SetFixedHeight(34);
    pToggle->SetBkColor(tokens::Token(tokens::EToken::SurfaceHover));
    RECT rcInset = { 10, 0, 0, 0 };
    pToggle->SetInset(rcInset);
    auto pToggleText = std::make_unique<CTextUI>(); // [C++17 R3]
    CDuiString sToggle;
    sToggle.Format(L"▸ 显示系统组件包（%d 个，与已安装更新去重后）",
                   static_cast<int>(m_vCbsDedup.size()));
    pToggleText->SetText(sToggle);
    pToggleText->SetFixedWidth(0);
    pToggleText->SetTextColor(tokens::Token(tokens::EToken::Success));
    pToggleText->SetTextStyle(DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    pToggleText->SetMouseEnabled(false);
    pToggle->Add(pToggleText.release());  // [C++17 R3] ownership to DuiLib
    pList->Add(pToggle.release());  // [C++17 R3] ownership to DuiLib
}

void CPageUpdate::UpdMergeCbs()
{
    if (!m_bCbsReady) return;
    CVerticalLayoutUI* pList = static_cast<CVerticalLayoutUI*>(
        m_ctx.HostFindControl(L"upd_list"));
    if (!pList) return;
    // 去重：收集 WUA 标题/KB 中的 KB 号集合，CBS identity 含同名 KB 则跳过
    UpdBuildCbsDedup();
    if (m_vCbsDedup.empty())
    {
        CDuiString sLog;
        sLog.Format(L"[update] CBS /Get-Packages 并入：原始 %d 个均与 WUA 重复，无新增",
                    static_cast<int>(m_vCbsPending.size()));
        m_ctx.HostLog(sLog.GetData());
        return;
    }
    // WUA 列表为空时容器里只有一个占位 CTextUI：并入前移除占位
    if (pList->GetCount() == 1)
    {
        CControlUI* pOnly = pList->GetItemAt(0);
        if (pOnly && dynamic_cast<CClickRowUI*>(pOnly) == nullptr)
            pList->RemoveAt(0);
    }
    if (m_bCbsExpanded)
    {
        UpdStartBatchRender(pList, 1, true);   // 已展开（如刷新后）：直接分批追加
        return;
    }
    // 默认折叠：仅追加一行开关
    UpdAppendCbsToggleRow(pList);
    CDuiString sLog;
    sLog.Format(L"[update] CBS /Get-Packages 并入完成：原始 %d 个，去重后 %d 个（默认折叠）",
                static_cast<int>(m_vCbsPending.size()), static_cast<int>(m_vCbsDedup.size()));
    m_ctx.HostLog(sLog.GetData());
    UpdateCount(static_cast<int>(m_vUpdList[1].size() + 1), static_cast<int>(m_vUpdList[1].size() + 1));
}

// [REFACTOR] 从 UpdMergeCbs 提取：从单个字符串收集 "KBxxxxxx" 形式的 KB 号。
// 扫描规则与原内联循环逐字一致：发现 K/k 紧跟 B/b 再紧跟数字，则收集后续
// 连续数字；非空则追加到 outKb，随后 q2 跳过 "KB"+数字部分继续查找，
// 避免同一个 KB 号被重复计入。
// 两处调用方的差异由调用侧承担：WUA 侧扫描 Title 与 sKB 两个字段，
// CBS 侧只扫描 Title——本函数只负责"扫一个字符串"。
// [R187] KB 前缀判定。原内联 5 个 && 条件（K/k + B/b + 首位数字），CC 记账 3 点。
// 大小写都收，是为了兼容 WUA 标题里 "kb1234567" 与 "KB1234567" 两种写法。
[[nodiscard]] static bool UpdIsKbPrefix(const wchar_t* q)
{
    if (q[0] != L'K' && q[0] != L'k') return false;
    if (q[1] != L'B' && q[1] != L'b') return false;
    return q[2] >= L'0' && q[2] <= L'9';
}

void CPageUpdate::CollectKbNumbers(const wchar_t* p, std::vector<CDuiString>& outKb)
{
    for (const wchar_t* q2 = p; q2 && *q2; ++q2)
    {
        if (!UpdIsKbPrefix(q2)) continue;      // [R187] 前缀判定抽为函数
        CDuiString sNum;
        for (const wchar_t* d = q2 + 2; *d >= L'0' && *d <= L'9'; ++d)
            sNum += *d;
        if (!sNum.IsEmpty()) outKb.push_back(sNum);
        q2 += sNum.GetLength() + 1;
    }
}

void CPageUpdate::PostInstallResultReport(int nKind, int nErr,
                                          int nResultCode, int nHr)
{
    wchar_t szExe[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, szExe, MAX_PATH);
    wchar_t* pSlash = wcsrchr(szExe, L'\\');
    CDuiString sOutbox;
    if (pSlash)
    {
        *pSlash = 0;
        sOutbox = CDuiString(szExe) + L"\\Report\\outbox";
    }
    SYSTEMTIME st;
    ::GetLocalTime(&st);
    CDuiString sBody;
    sBody.Format(L"<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                    "<Report Kind=\"UpdateReport\">"
                    "<Time>%04u-%02u-%02u %02u:%02u:%02u</Time>"
                    "<Result>%s</Result><Err>%d</Err>"
                    "<ResultCode>%d</ResultCode><HResult>0x%08X</HResult>"
                    "<Items>%d</Items></Report>",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                nKind == 2 ? L"Success" : L"Failed",
                nErr, nResultCode, (unsigned)nHr, static_cast<int>(m_vUpdList[1].size()));
    UpdateMetaService::PostReportEvent(m_sReportEndpoint, L"UpdateReport",
                                       sBody.GetData(), sOutbox.GetData(), kUpdReportTimeoutMs);
}
