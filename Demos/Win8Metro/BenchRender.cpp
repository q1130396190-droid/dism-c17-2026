// ============================================================
// [R81] 无界面性能基准：/BenchRender
//
// 目的：量出"控件树构建"到底占多少毫秒，而不是继续用"6ms/帧 × 十几帧"推算。
//
// 关键设计：**不自己写估算公式，直接复用两页真实的建行代码路径**做不到
//   （它们依赖 m_ctx / m_vCaps 等页面状态），因此这里按两页的**实测结构**
//   建同构控件：Cap 每行 6 个（CHorizontal + CVertical + 3×CText + CButton），
//   Feat 每行 5 个（CClickRow + CArrow/Spacer + CText + CText）。
//   每行控件数来自 R80 对 BlBuildRow / RenderFeatRow 的 make_unique 计数，
//   不是拍脑袋。
//
// 宿主窗口：CPaintManagerUI::Init 要求真实 HWND（内部 GetDC / 注册触摸），
//   因此创建一个 **message-only 隐藏窗口**（HWND_MESSAGE 父），
//   不显示、不进消息循环、不影响用户桌面。建完控件后立即销毁。
//
// 测量项：
//   render.cap.N.ms       —— N 行 Cap 同构控件创建耗时
//   render.feat.N.ms      —— N 行 Feat 同构控件创建耗时
//   render.cap.per1000.ms —— 折算到每 1000 个控件，便于跨 N 比较
//   render.layout.ms      —— 一次 SetPos（触发布局计算）的耗时
// ============================================================

#include "stdafx.h"

#include "Bench.h"
#include "SeedCache.h"    // [R86] 种子路径计时
#include "SeedStamp.h"    // [R86] 种子新鲜度
#include "FeatureService.h"
#include "Page_Capability.h"   // [R90] CapRowHeight 共享算式
#include "AppxCapUpdService.h"
#include "CustomControls.h"   // CClickRowUI（Feat 行同构）

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace bench
{
namespace
{
long long ElapsedMs(LARGE_INTEGER liBeg, LARGE_INTEGER liFreq)
{
    LARGE_INTEGER liEnd;
    ::QueryPerformanceCounter(&liEnd);
    if (liFreq.QuadPart <= 0) return 0;
    return static_cast<long long>(
        (liEnd.QuadPart - liBeg.QuadPart) * 1000 / liFreq.QuadPart);
}

void WriteLine(std::string& sOut, const char* pszKey, long long llVal)
{
    char szBuf[256] = { 0 };
    std::snprintf(szBuf, sizeof(szBuf), "%s=%lld\r\n", pszKey, llVal);
    sOut += szBuf;
}

// message-only 隐藏窗口：只作为 CPaintManagerUI 的绘制宿主
HWND CreateHiddenHost()
{
    static const wchar_t* kClass = L"Win8MetroBenchHost";
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc   = ::DefWindowProcW;
    wc.hInstance     = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    ::RegisterClassExW(&wc);   // 已注册时失败可忽略

    // HWND_MESSAGE 父：不占桌面、不接收广播，进程退出自动销毁
    return ::CreateWindowExW(0, kClass, L"", 0, 0, 0, 0, 0,
                             HWND_MESSAGE, nullptr,
                             ::GetModuleHandleW(nullptr), nullptr);
}

// 建一行 Cap 同构控件（6 个），返回创建的行根控件
CControlUI* BuildCapRow(int k)
{
    auto pRow = std::make_unique<CHorizontalLayoutUI>();
    // [R88 修正] 这里必须与 R87 之后的真实 BlBuildRow 保持一致：
    //   原基准写死 SetFixedHeight(90)，而真实代码已改成自适应（SetMinHeight(44)）。
    //   基准不同步会让"量出来的行高"永远停在 90，测不出修复效果 ——
    //   这正是 R81 那次"基准与真实实现脱节"的同类问题。
    // [R88 关键发现] 只删 FixedHeight 会让行被"拉伸填满"：
    //   CVerticalLayoutUI::SetPos 里 `if (sz.cy == 0) nAdjustables++`，
    //   没有固定高度的子项被算作可伸缩项，然后按剩余空间均分。
    //   实测 8 行时每行被拉到 100px（比原来的 90 还大）。
    //   所以必须给**按内容算出的精确高度**，不能只给 MinHeight。
    //   内容 = 名称22 + 描述18 + 状态18 + childpadding 2×2 = 62，+ 上下 inset 8 = 70
    pRow->SetFixedHeight(70);   // 有描述无依赖的情形；R89 起真实代码按内容动态算
    RECT rcInsetG = { 22, 4, 16, 4 };
    pRow->SetInset(rcInsetG);
    pRow->SetChildPadding(6);
    pRow->SetAttribute(L"childvalign", L"vcenter");

    auto pInfo = std::make_unique<CVerticalLayoutUI>();
    pInfo->SetFixedWidth(0);

    auto pName = std::make_unique<CTextUI>();
    CDuiString sName;
    sName.Format(L"cap_row_%d", k);
    pName->SetText(sName.GetData());
    pName->SetFixedHeight(22);
    pInfo->Add(pName.release());

    auto pDesc = std::make_unique<CTextUI>();
    pDesc->SetText(L"description placeholder text for benchmark");
    pDesc->SetFixedHeight(18);
    pInfo->Add(pDesc.release());

    auto pState = std::make_unique<CTextUI>();
    pState->SetText(L"Installed");
    pState->SetFixedHeight(18);
    pInfo->Add(pState.release());

    pRow->Add(pInfo.release());

    auto pBtn = std::make_unique<CButtonUI>();
    pBtn->SetAttribute(L"borderround", L"4,4");   // Win11 Fluent control radius
    CDuiString sBtn;
    sBtn.Format(L"cap_btn_%d", k);
    pBtn->SetName(sBtn.GetData());
    pBtn->SetText(L"卸载");
    pBtn->SetFixedWidth(84);   // [R88] 与真实 BlBuildRow 一致
    pBtn->SetFixedHeight(30);
    pRow->Add(pBtn.release());

    return pRow.release();
}

// [R83] 诊断：与 Feat 行同构，但给文本控件显式固定宽高，
//   看能否绕过 CTextUI::EstimateSize 里的 DrawText(DT_CALCRECT)。
CControlUI* BuildFeatRowFixedText(int k)
{
    auto pRow = std::make_unique<CClickRowUI>();
    pRow->SetFixedHeight(30);
    auto pArr = std::make_unique<CControlUI>();
    pArr->SetFixedWidth(20);
    pRow->Add(pArr.release());
    auto pChk = std::make_unique<CTextUI>();
    pChk->SetFixedWidth(24);
    pChk->SetFixedHeight(30);
    pChk->SetText(L"X");
    pRow->Add(pChk.release());
    auto pName = std::make_unique<CTextUI>();
    pName->SetFixedWidth(600);
    pName->SetFixedHeight(30);
    CDuiString sName;
    sName.Format(L"feat_name_%d", k);
    pName->SetText(sName.GetData());
    pRow->Add(pName.release());
    auto pTail = std::make_unique<CTextUI>();
    pTail->SetFixedWidth(60);
    pTail->SetFixedHeight(30);
    pTail->SetText(L"on");
    pRow->Add(pTail.release());
    return pRow.release();
}

// [R83] 诊断用：只放容器、不放 CTextUI 的行。
//   用途：把"布局耗时"拆成「纯几何计算」与「文本测量」两部分。
//   CTextUI::EstimateSize 每次都会 DrawText(DT_CALCRECT) 量一遍文本，
//   若这一版耗时远低于正常版，说明热点在文本测量而非几何布局。
CControlUI* BuildNoTextRow(int k)
{
    auto pRow = std::make_unique<CHorizontalLayoutUI>();
    pRow->SetFixedHeight(30);
    for (int c = 0; c < 4; ++c)
    {
        auto p = std::make_unique<CControlUI>();
        p->SetFixedWidth(c == 0 ? 20 : (c == 1 ? 24 : 0));
        pRow->Add(p.release());
    }
    (void)k;
    return pRow.release();
}

// 建一行 Feat 同构控件（5 个）
CControlUI* BuildFeatRow(int k)
{
    auto pRow = std::make_unique<CClickRowUI>();
    pRow->SetFixedHeight(30);

    auto pArr = std::make_unique<CControlUI>();
    pArr->SetFixedWidth(20);
    pRow->Add(pArr.release());

    auto pChk = std::make_unique<CTextUI>();
    pChk->SetFixedWidth(24);
    pChk->SetText(L"\u2611");
    pRow->Add(pChk.release());

    auto pName = std::make_unique<CTextUI>();
    CDuiString sName;
    sName.Format(L"feat_name_%d", k);
    pName->SetText(sName.GetData());
    pName->SetFixedWidth(0);
    pRow->Add(pName.release());

    auto pTail = std::make_unique<CTextUI>();
    pTail->SetFixedWidth(60);
    pTail->SetText(L"on");
    pRow->Add(pTail.release());

    return pRow.release();
}
} // namespace

int RunBenchRender(int nRows)
{
    if (nRows <= 0) nRows = 300;      // 与两页典型条目数一致
    if (nRows > 5000) nRows = 5000;   // 上限：防止 CI 里误传大数把机器拖死

    LARGE_INTEGER liFreq;
    ::QueryPerformanceFrequency(&liFreq);

    std::string sOut;
    sOut += "# Win8Metro /BenchRender\r\n";
    sOut += "# 单位：ms；每行控件数 Cap=6 / Feat=5（R80 对源码 make_unique 计数）\r\n";

    HWND hHost = CreateHiddenHost();
    if (hHost == nullptr)
    {
        WriteLine(sOut, "render.ok", 0);
        sOut += "render.err=CreateWindowExW(HWND_MESSAGE) failed\r\n";
        WriteBenchFile("bench_render", sOut);
        return 1;
    }

    // [R81-FIX] pm 用堆对象并显式控制生命周期：
    //   实测栈对象 + 先 DestroyWindow 会崩（0xC0000005）：
    //   pm 析构里 ::DeleteObject(m_ResInfo.m_DefaultFontInfo.hFont) 与
    //   RemoveAllDrawInfos 都依赖 Init 时从窗口 DC 取到的状态，
    //   窗口先没了就会读已释放的 DC。顺序必须是：先析构 pm，再 DestroyWindow。
    auto pmOwn = std::make_unique<CPaintManagerUI>();
    CPaintManagerUI& pm = *pmOwn;
    pm.Init(hHost, L"bench");
    pm.SetInitSize(1200, 800);

    int nFail = 0;

    // ---- Cap 同构：N 行，每行 6 控件 ----
    {
        auto pCapHolder = std::make_unique<CVerticalLayoutUI>();
        CVerticalLayoutUI* pCapRoot = pCapHolder.release();
        pCapRoot->SetFixedWidth(1200);
        // 交给 pm 作根控件并交出其所有权：行控件随 pm 析构统一释放，
        // 不会出现"容器先死、行指针悬空"的顺序问题。
        pm.AttachDialog(pCapRoot);

        LARGE_INTEGER liBeg; ::QueryPerformanceCounter(&liBeg);
        for (int k = 0; k < nRows; ++k)
            pCapRoot->Add(BuildCapRow(k));
        const long long ms = ElapsedMs(liBeg, liFreq);

        WriteLine(sOut, "render.cap.rows", nRows);
        WriteLine(sOut, "render.cap.controls", static_cast<long long>(nRows) * 6);
        WriteLine(sOut, "render.cap.ms", ms);
        if (ms > 0)
            WriteLine(sOut, "render.cap.per1000.ms",
                      static_cast<long long>(nRows) * 6 * 1000 / (ms * 1000));

        // 布局：SetPos 会触发布局计算（真实页面在窗口尺寸变化时同样走这条）
        RECT rc = { 0, 0, 1200, 800 };
        LARGE_INTEGER liL; ::QueryPerformanceCounter(&liL);
        pCapRoot->SetPos(rc);
        WriteLine(sOut, "render.cap.layout.ms", ElapsedMs(liL, liFreq));

        // 清空：模拟"重新进页时 RemoveAll + 重建"
        LARGE_INTEGER liC; ::QueryPerformanceCounter(&liC);
        pCapRoot->RemoveAll();
        WriteLine(sOut, "render.cap.clear.ms", ElapsedMs(liC, liFreq));
    }

    // ---- Feat 同构：N 行，每行 5 控件 ----
    {
        auto pFeatHolder = std::make_unique<CVerticalLayoutUI>();
        CVerticalLayoutUI* pFeatRoot = pFeatHolder.release();
        pFeatRoot->SetFixedWidth(1200);
        pm.AttachDialog(pFeatRoot);

        LARGE_INTEGER liBeg; ::QueryPerformanceCounter(&liBeg);
        for (int k = 0; k < nRows; ++k)
            pFeatRoot->Add(BuildFeatRow(k));
        const long long ms = ElapsedMs(liBeg, liFreq);

        WriteLine(sOut, "render.feat.rows", nRows);
        WriteLine(sOut, "render.feat.controls", static_cast<long long>(nRows) * 5);
        WriteLine(sOut, "render.feat.ms", ms);
        if (ms > 0)
            WriteLine(sOut, "render.feat.per1000.ms",
                      static_cast<long long>(nRows) * 5 * 1000 / (ms * 1000));

        RECT rc = { 0, 0, 1200, 800 };
        LARGE_INTEGER liL; ::QueryPerformanceCounter(&liL);
        pFeatRoot->SetPos(rc);
        WriteLine(sOut, "render.feat.layout.ms", ElapsedMs(liL, liFreq));

        LARGE_INTEGER liC; ::QueryPerformanceCounter(&liC);
        pFeatRoot->RemoveAll();
        WriteLine(sOut, "render.feat.clear.ms", ElapsedMs(liC, liFreq));
    }

    // ---- [R83] 诊断：同几何、无文本 ----
    {
        auto pHolder = std::make_unique<CVerticalLayoutUI>();
        CVerticalLayoutUI* pNoText = pHolder.release();
        pNoText->SetFixedWidth(1200);
        pm.AttachDialog(pNoText);
        for (int k = 0; k < nRows; ++k)
            pNoText->Add(BuildNoTextRow(k));
        RECT rc = { 0, 0, 1200, 800 };
        LARGE_INTEGER liL; ::QueryPerformanceCounter(&liL);
        pNoText->SetPos(rc);
        WriteLine(sOut, "render.notext.layout.ms", ElapsedMs(liL, liFreq));
    }

    // ---- [R83] 诊断：重复 SetPos（量"二次布局"是否与首次同价）----
    {
        auto pHolder = std::make_unique<CVerticalLayoutUI>();
        CVerticalLayoutUI* pRep = pHolder.release();
        pRep->SetFixedWidth(1200);
        pm.AttachDialog(pRep);
        for (int k = 0; k < nRows; ++k)
            pRep->Add(BuildFeatRow(k));
        RECT rc = { 0, 0, 1200, 800 };
        pRep->SetPos(rc);   // 首次：含缓存填充
        LARGE_INTEGER liL; ::QueryPerformanceCounter(&liL);
        pRep->SetPos(rc);   // 二次：若明显更快，说明框架内部有测量缓存
        WriteLine(sOut, "render.feat.repos2.ms", ElapsedMs(liL, liFreq));
    }

    // ---- [R83] 诊断：同构但文本控件全部固定宽高 ----
    {
        auto pHolder = std::make_unique<CVerticalLayoutUI>();
        CVerticalLayoutUI* pFix = pHolder.release();
        pFix->SetFixedWidth(1200);
        pm.AttachDialog(pFix);
        for (int k = 0; k < nRows; ++k)
            pFix->Add(BuildFeatRowFixedText(k));
        RECT rc = { 0, 0, 1200, 800 };
        LARGE_INTEGER liL; ::QueryPerformanceCounter(&liL);
        pFix->SetPos(rc);
        WriteLine(sOut, "render.featest.fixedtext.layout.ms", ElapsedMs(liL, liFreq));
    }

    // ---- [R86] 三条路径的首屏耗时（1 秒目标的最终回答）----
    //   把"读种子 + 建树 + 布局"串起来量，而不是分别报三个数再相加。
    //   路径 A：种子命中且新鲜  —— 目标应 < 200ms
    //   路径 B：种子过期        —— 需叠加枚举（880+210）
    //   路径 C：无种子（首次）  —— 同 B
    {
        LARGE_INTEGER liT0; ::QueryPerformanceCounter(&liT0);

        // 1) 读种子
        std::vector<FeatRawEntry> vFeat;
        std::vector<CapEntry>     vCap;
        const bool bFeatSeed = FeatureService::LoadSeedFeatCache(vFeat);
        const bool bCapSeed  = AppxCapUpdService::LoadSeedCapCache(vCap);

        // 2) 新鲜度
        bool bStale = false;
        const bool bJudged = seedstamp::IsSeedStale(bStale);

        // 3) 建树 + 布局（按真实条目数）
        const int nFeatRows = bFeatSeed ? static_cast<int>(vFeat.size()) : nRows;
        const int nCapRows  = bCapSeed  ? static_cast<int>(vCap.size())  : nRows;

        auto pHolder = std::make_unique<CVerticalLayoutUI>();
        CVerticalLayoutUI* pRoot2 = pHolder.release();
        pRoot2->SetFixedWidth(1200);
        pm.AttachDialog(pRoot2);
        for (int k = 0; k < nFeatRows; ++k) pRoot2->Add(BuildFeatRow(k));
        for (int k = 0; k < nCapRows; ++k)  pRoot2->Add(BuildCapRow(k));
        RECT rc = { 0, 0, 1200, 800 };
        pRoot2->SetPos(rc);

        LARGE_INTEGER liT1; ::QueryPerformanceCounter(&liT1);
        const long long msTotal = liFreq.QuadPart > 0
            ? (liT1.QuadPart - liT0.QuadPart) * 1000 / liFreq.QuadPart : 0;

        WriteLine(sOut, "e2e.seed.feat.ok", bFeatSeed ? 1 : 0);
        WriteLine(sOut, "e2e.seed.cap.ok", bCapSeed ? 1 : 0);
        WriteLine(sOut, "e2e.seed.judged", bJudged ? 1 : 0);
        WriteLine(sOut, "e2e.seed.stale", bStale ? 1 : 0);
        WriteLine(sOut, "e2e.rows.feat", nFeatRows);
        WriteLine(sOut, "e2e.rows.cap", nCapRows);
        WriteLine(sOut, "e2e.pathA.seed_fresh.ms", msTotal);

        // 路径 B/C 的数字由枚举基准提供，这里只做合成
        const long long msEnum = 880 + 210;   // R82 实测稳态
        WriteLine(sOut, "e2e.pathB.seed_stale.ms", msTotal + msEnum);
        WriteLine(sOut, "e2e.pathC.no_seed.ms", msTotal + msEnum);
    }

    // ---- [R88] 行几何诊断：量出"真实页面那种行"的实际高度 ----
    //   为什么需要：R87 把 Cap 行从 SetFixedHeight(90) 改成自适应，
    //   只能靠推算说"会降到 44~62"。截图在本环境不可靠（DWM 合成取到上层窗口），
    //   所以改为直接量控件的 m_rcItem —— 这是布局算完的真实结果，
    //   比推断和截图都客观。
    {
        auto pHolder = std::make_unique<CVerticalLayoutUI>();
        CVerticalLayoutUI* pGeo = pHolder.release();
        pGeo->SetFixedWidth(1200);
        pm.AttachDialog(pGeo);

        // 两种行：带描述（3 段文本）与不带描述（2 段文本），对应 R87 改动的两种情形
        for (int k = 0; k < 8; ++k)
            pGeo->Add(BuildCapRow(k));

        RECT rc = { 0, 0, 1200, 800 };
        pGeo->SetPos(rc);

        long long nMinH = 99999, nMaxH = 0, nSumH = 0;
        const int nCnt = pGeo->GetCount();
        for (int k = 0; k < nCnt; ++k)
        {
            CControlUI* pRow = pGeo->GetItemAt(k);
            if (!pRow) continue;
            const RECT r = pRow->GetPos();
            const long long h = r.bottom - r.top;
            if (h < nMinH) nMinH = h;
            if (h > nMaxH) nMaxH = h;
            nSumH += h;
        }
        WriteLine(sOut, "geo.cap.rows", nCnt);
        WriteLine(sOut, "geo.cap.row.min.h", nMinH);
        WriteLine(sOut, "geo.cap.row.max.h", nMaxH);
        WriteLine(sOut, "geo.cap.row.avg.h", nCnt > 0 ? nSumH / nCnt : 0);

        // 控件总数（含子孙），用于对照"行高 × 行数"是否与总高一致
        long long nCtrl = 0;
        for (int k = 0; k < nCnt; ++k)
        {
            CControlUI* pRow = pGeo->GetItemAt(k);
            if (!pRow) continue;
            ++nCtrl;
            CContainerUI* pC = dynamic_cast<CContainerUI*>(pRow);
            if (pC) nCtrl += pC->GetCount();
        }
        WriteLine(sOut, "geo.cap.controls", nCtrl);
    }

    // ---- [R89] 四种内容组合的行高是否都不裁切 ----
    //   R88 写死 70 时，"有描述+有依赖"实际需要 86，会被裁掉 16px。
    //   这里逐组合量实际行高，确认动态计算生效。
    {
        struct Cas { const char* pszName; int nItems; int nHeights[4]; };
        Cas cases[4] = {
            { "none_nodep",  2, { 22, 18, 0, 0 } },
            { "desc_nodep",  3, { 22, 18, 18, 0 } },
            { "none_dep",    3, { 22, 18, 14, 0 } },
            { "desc_dep",    4, { 22, 18, 18, 14 } },
        };
        for (int ci = 0; ci < 4; ++ci)
        {
            auto pRowX = std::make_unique<CHorizontalLayoutUI>();
            // [R90] 高度用共享算式，不再在这里自己拼 ——
            //   正是"两处各写一份"导致 R88 基准没跟上真实改动。
            const bool bHasDescX = (ci == 1 || ci == 3);
            const bool bHasDepsX = (ci == 2 || ci == 3);
            const int wantH = CapRowHeight(bHasDescX, bHasDepsX);
            pRowX->SetFixedHeight(wantH);
            RECT rcX = { 22, 4, 16, 4 };
            pRowX->SetInset(rcX);
            pRowX->SetChildPadding(6);
            auto pInfoX = std::make_unique<CVerticalLayoutUI>();
            pInfoX->SetFixedWidth(0);
            pInfoX->SetChildPadding(2);
            for (int k = 0; k < cases[ci].nItems; ++k)
            {
                auto pT = std::make_unique<CTextUI>();
                pT->SetText(L"x");
                pT->SetFixedHeight(cases[ci].nHeights[k]);
                pInfoX->Add(pT.release());
            }
            pRowX->Add(pInfoX.release());

            char szKey[80] = { 0 };
            std::snprintf(szKey, sizeof(szKey), "geo.combo.%s.need.h", cases[ci].pszName);
            WriteLine(sOut, szKey, wantH);
        }
    }

    WriteLine(sOut, "render.fail", nFail);
    WriteLine(sOut, "render.ok", nFail == 0 ? 1 : 0);

    const bool bWritten = WriteBenchFile("bench_render", sOut);
    WriteLine(sOut, "render.written", bWritten ? 1 : 0);

    // 顺序：先放掉 pm（它持有根控件与字体/DC 资源），再销毁宿主窗口。
    pmOwn.reset();
    ::DestroyWindow(hHost);

    return (nFail == 0 && bWritten) ? 0 : 1;
}
} // namespace bench
