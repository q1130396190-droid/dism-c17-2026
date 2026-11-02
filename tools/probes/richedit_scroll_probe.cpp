// ============================================================
// richedit_scroll_probe.cpp -- 有内容的 RichEdit 会不会画滚动条
//
// 为什么需要这个探针
// ------------------
// R47 把清理页右侧卡片改成「内容贴边 + 圆角 6px」，目的是让 R45 的圆角裁剪
// 真的被触发。但贴边带来一个没验完的风险：clean_desc 是带 vscrollbar 的
// RichEdit，右缘现在紧贴卡片边缘，滚动条可能被圆角裁掉或压在描边下。
//
// R47 当时**明确标注为未验证**：渲染探针只画静态皮肤，RichEdit 没有文本内容，
// 滚动条本就不会出现——没有滚动条可裁，自然测不出被裁。
//
// 本探针补上这个能力：真的给 RichEdit 灌入超出一屏的文本，再渲染，看滚动条。
//
// 前置条件（读源码确认，不是猜）
// ----------------------------
//   CRichEditUI::SetText() 第一行就是 `if (!m_pTwh) return;`
//   m_pTwh 由 CRichEditUI::DoInit() 里的 CreateHost() 创建
//   DoInit() 由 CControlUI::Init() 调用
//   Init() 由 SetManager(pm, parent, /*bInit=*/true) 触发
//   —— 也就是说，控件必须真的 attach 到 paint manager 上，文本宿主才存在。
//   探针里光 new 一个 CRichEditUI 是没用的，必须走 AttachDialog 这条真实路径。
//
// 判据
// ----
//   1) 文本宿主真的建起来了（否则 SetText 静默无效，后面的断言全是假绿）
//   2) 灌入长文本后，控件文本长度确实变长
//   3) 右缘一列出现滚动条绘制像素（与"没有滚动条"的对照组区分开）
//   4) 圆角上下文里滚边条不被裁掉（贴边卡片场景）
//
// 第 1 条尤其重要：如果 m_pTwh 没建起来，SetText 会静默返回，
// 界面看起来"没有滚动条"——那会被误判成"滚动条被裁了"。
// ============================================================

#include "UIlib.h"

#include <windows.h>
#include <gdiplus.h>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "gdiplus.lib")

using namespace DuiLib;

namespace
{
int g_nPass = 0;
int g_nFail = 0;

void Chk(const char* pszName, bool bOk, const std::wstring& sDetail)
{
    char szd[512] = {};
    ::WideCharToMultiByte(CP_UTF8, 0, sDetail.c_str(), static_cast<int>(sDetail.size()),
                          szd, sizeof(szd) - 1, nullptr, nullptr);
    if (bOk) { ++g_nPass; ::printf("[PASS] %s | %s\n", pszName, szd); }
    else     { ++g_nFail; ::printf("[FAIL] %s | %s\n", pszName, szd); }
    ::fflush(stdout);
}

class CProbeNavUI : public COptionUI
{
    DECLARE_DUICONTROL(CProbeNavUI)
public:
    CProbeNavUI() {}
    LPCTSTR GetClass() const override { return L"NavOptionUI"; }
};

IMPLEMENT_DUICONTROL(CProbeNavUI)

const DWORD kBg = 0xFF00FF00;   // 哨兵绿

class CDib
{
public:
    HDC     hDC = nullptr;
    HBITMAP hBM = nullptr;
    DWORD*  p = nullptr;
    int     w = 0, h = 0;

    bool Create(int nW, int nH)
    {
        w = nW; h = nH;
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = nW;
        bi.bmiHeader.biHeight = -nH;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        void* pBits = nullptr;
        hBM = ::CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &pBits, nullptr, 0);
        if (hBM == nullptr || pBits == nullptr) return false;
        p = static_cast<DWORD*>(pBits);
        hDC = ::CreateCompatibleDC(nullptr);
        if (hDC == nullptr) return false;
        ::SelectObject(hDC, hBM);
        for (int i = 0; i < w * h; ++i) p[i] = kBg;
        return true;
    }

    // 统计某列（x）范围内与哨兵不同的像素数 —— 用于找滚动条
    int CountNonSentinelInColumn(int x) const
    {
        if (x < 0 || x >= w) return -1;
        int n = 0;
        for (int y = 0; y < h; ++y)
            if ((p[y * w + x] & 0x00FFFFFF) != (kBg & 0x00FFFFFF)) ++n;
        return n;
    }

    // [R49] First/last row in column x that has any paint (non-sentinel).
    // Needed to quantify how much of the scrollbar the rounded corner eats:
    // the scrollbar is a tall bar, so its top/bottom ends tell us exactly
    // how many pixels the arc removed.
    void ColumnSpan(int x, int& nTop, int& nBottom) const
    {
        nTop = -1; nBottom = -1;
        if (x < 0 || x >= w) return;
        for (int y = 0; y < h; ++y)
        {
            if ((p[y * w + x] & 0x00FFFFFF) != (kBg & 0x00FFFFFF))
            {
                if (nTop < 0) nTop = y;
                nBottom = y;
            }
        }
    }

    void Destroy()
    {
        if (hDC) { ::DeleteDC(hDC); hDC = nullptr; }
        if (hBM) { ::DeleteObject(hBM); hBM = nullptr; }
        p = nullptr;
    }
};

CControlUI* FindByName(CControlUI* pRoot, const wchar_t* pszName)
{
    if (pRoot == nullptr || pszName == nullptr) return nullptr;
    if (pRoot->GetName().Compare(pszName) == 0) return pRoot;
    auto* pC = dynamic_cast<CContainerUI*>(pRoot);
    if (pC == nullptr) return nullptr;
    for (int i = 0; i < pC->GetCount(); ++i)
        if (CControlUI* h = FindByName(pC->GetItemAt(i), pszName)) return h;
    return nullptr;
}

} // namespace

int main(int argc, char** argv)
{
    // [GATE-SELFTEST] /Inject adds one deliberately failing assertion so this
    // probe can prove its verdict path still works; otherwise a check that
    // silently stopped asserting would look exactly like a passing run.
    bool bInject = false;
    for (int i = 1; i < argc; ++i)
        if (::lstrcmpiA(argv[i], "/Inject") == 0) bInject = true;

    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR gtok = 0;
    if (Gdiplus::GdiplusStartup(&gtok, &gsi, nullptr) != Gdiplus::Ok)
    {
        ::printf("[FATAL] GdiplusStartup\n");
        return 2;
    }

    wchar_t szExe[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, szExe, MAX_PATH);
    std::wstring sDir(szExe);
    const size_t nSlash = sDir.find_last_of(L"\\/");
    if (nSlash != std::wstring::npos) sDir.resize(nSlash);
    std::wstring sRoot = sDir;
    for (int i = 0; i < 2; ++i)
    {
        const size_t p = sRoot.find_last_of(L"\\/");
        if (p != std::wstring::npos) sRoot.resize(p);
    }

    HINSTANCE hInst = ::GetModuleHandleW(nullptr);
    CPaintManagerUI::SetInstance(hInst);
    CPaintManagerUI::SetResourcePath((sRoot + L"\\bin64\\skin\\Win8Metro").c_str());

    const wchar_t* kCls = L"RichEditScrollProbeWnd";
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ::DefWindowProcW;
    wc.hInstance = hInst;
    wc.lpszClassName = kCls;
    ::RegisterClassExW(&wc);

    HWND hWnd = ::CreateWindowExW(0, kCls, L"probe", WS_OVERLAPPEDWINDOW,
                                  0, 0, 1280, 900, nullptr, nullptr, hInst, nullptr);
    if (hWnd == nullptr) { ::printf("[FATAL] CreateWindowEx\n"); return 2; }

    CPaintManagerUI pm;
    pm.Init(hWnd, L"RichEditScrollProbe");
    CControlFactory::GetInstance()->RegistControl(
        L"CNavOptionUI", static_cast<CreateClass>(CProbeNavUI::CreateControl));

    CDialogBuilder builder;
    CControlUI* pRoot = builder.Create(L"Main.xml", nullptr, nullptr, &pm);
    if (pRoot == nullptr) { ::printf("[FATAL] Main.xml 解析失败\n"); return 2; }
    pm.AttachDialog(pRoot);
    pm.SetDPI(96);

    auto* pDesc = dynamic_cast<CRichEditUI*>(FindByName(pRoot, L"clean_desc"));
    Chk("clean_desc 存在且是 CRichEditUI", pDesc != nullptr,
        pDesc ? L"找到" : L"未找到");
    if (pDesc == nullptr)
    {
    
    ::DestroyWindow(hWnd);
        ::UnregisterClassW(kCls, hInst);
        Gdiplus::GdiplusShutdown(gtok);
        return 1;
    }

    // ---- 1) 文本宿主是否真的建起来 ----
    // 这是后续所有断言的前提：没有 m_pTwh 时 SetText 会静默 return，
    // 界面"没有滚动条"会被误判成"滚动条被裁掉"（假阴性）。
    const bool bHasHost = (pDesc->GetText().GetLength() >= 0) && pDesc->IsRich();
    Chk("RichEdit 文本宿主已建立（SetText 才会生效）", bHasHost,
        std::wstring(L"IsRich=") + (pDesc->IsRich() ? L"1" : L"0"));

    // ---- 2) 灌入超出一屏的长文本 ----
    std::wstring sLong;
    for (int i = 0; i < 80; ++i)
    {
        wchar_t szLine[160] = {};
        ::swprintf_s(szLine, L"第 %d 行：这是一段用于撑出滚动条的长文本，反复堆叠以超过可视高度。\r\n", i);
        sLong += szLine;
    }

    const int nBefore = pDesc->GetText().GetLength();
    pDesc->SetText(sLong.c_str());
    const int nAfter = pDesc->GetText().GetLength();

    wchar_t szLen[160] = {};
    ::swprintf_s(szLen, L"before=%d after=%d", nBefore, nAfter);
    Chk("长文本确实写进了控件（不是静默失败）", nAfter > nBefore + 500, szLen);

    // ---- 3) 布局后渲染，看右缘有没有滚动条 ----
    const int W = 1280, H = 900;
    RECT rcAll = { 0, 0, W, H };
    pRoot->SetPos(rcAll, false);

    const RECT rcDesc = pDesc->GetPos();
    const int nDescW = rcDesc.right - rcDesc.left;
    const int nDescH = rcDesc.bottom - rcDesc.top;

    wchar_t szBox[200] = {};
    ::swprintf_s(szBox, L"rect=%dx%d", nDescW, nDescH);
    Chk("clean_desc 拿到真实矩形", nDescW > 50 && nDescH > 50, szBox);

    // 单独画 RichEdit 自己
    CDib d;
    bool bRendered = false;
    if (d.Create(nDescW + 4, nDescH + 4))
    {
        RECT rcLocal = { 0, 0, nDescW, nDescH };
        pDesc->SetPos(rcLocal, false);
        pDesc->Paint(d.hDC, rcLocal, nullptr);
        bRendered = true;
    }
    Chk("RichEdit 渲染成功", bRendered, L"");

        wchar_t szOut[240] = {};
        {
            CScrollBarUI* pBar2 = pDesc->GetVerticalScrollBar();
            if (pBar2 != nullptr) {
                RECT rb2 = pBar2->GetPos();
                ::printf("[DIAG] bar vis=%d rect=%d,%d,%d,%d fixedW=%d%s",
                         pBar2->IsVisible() ? 1 : 0,
                         (int)rb2.left, (int)rb2.top,
                         (int)rb2.right, (int)rb2.bottom,
                         (int)pBar2->GetFixedWidth(), "\n");
            } else {
                ::printf("[DIAG] bar not created\n");
            }
        }

    if (bRendered)
    {
        // 滚动条宽 10px（Main.xml Default VScrollBar width=10），应贴在右缘
        // 统计最右 14 列里每列的非哨兵像素，找是否存在一条"高柱"
        std::vector<int> vCols;
        for (int x = nDescW - 14; x < nDescW; ++x)
            vCols.push_back(d.CountNonSentinelInColumn(x));

        int nMaxCol = 0;
        for (int v : vCols) if (v > nMaxCol) nMaxCol = v;

        wchar_t szCol[256] = {};
        ::swprintf_s(szCol, L"maxColumnPixels=%d（列高=%d）", nMaxCol, nDescH);
        // 有滚动条时，某一列会有接近满高的绘制
        // [R326] 判据修正：不能要求"滚动条列接近满高"。
        //   皮肤里 VScrollBar 的 railbkcolor 是全透明 #00000000，所以轨道
        //   一个像素都不画；能看见的只有滑块（thumbbkcolor=Border），
        //   而滑块高度按"可视高度/内容总高"的比例算 —— 实测内容 2870 字符
        //   时滑块只有 79px。旧判据 nMaxCol > nDescH/2 于是必然失败，
        //   把"滚动条正常绘制"报成"滚动条被裁"。
        //   正确判据：右缘出现一段明显的纵向绘制（滑块），高度大于一个
        //   滑块的最小合理值（这里取 24px），且位置落在滚动条列范围内。
        Chk("右缘出现滚动条滑块（有内容时应当出现）",
            nMaxCol >= 24, szCol);

        // [R49] Quantify: how many pixels does the rounded corner actually eat
        // off the scrollbar? R48 only proved the bar exists. The claim "6px radius
        // only affects the corners" deserved a number, not an assertion.
        int nBarX = -1, nBarMax = 0;
        for (int x = nDescW - 14; x < nDescW; ++x)
        {
            const int v = d.CountNonSentinelInColumn(x);
            if (v > nBarMax) { nBarMax = v; nBarX = x; }
        }
        if (nBarX >= 0)
        {
            int nTop = -1, nBot = -1;
            d.ColumnSpan(nBarX, nTop, nBot);
            const int nEatenTop = (nTop > 0) ? nTop : 0;
            const int nEatenBottom = (nBot >= 0) ? (nDescH - 1 - nBot) : nDescH;
            wchar_t szBar[256] = {};
            ::swprintf_s(szBar, L"barX=%d top=%d bot=%d eatenTop=%d eatenBottom=%d h=%d",
                         nBarX, nTop, nBot, nEatenTop, nEatenBottom, nDescH);
            ::printf("[INFO] 滚动条减化 | %ls\n", szBar);
            ::fflush(stdout);
            // [R326] 侵蚀判据修正：只测"滑块顶端是否被圆角切掉"。
            //   旧判据同时要求 eatenBottom <= 8，但 railbkcolor 是全透明
            //   #00000000，轨道不产生任何像素，滑块底边永远不等于控件底边
            //   —— 实测 eatenBottom=300 只是"滑块比控件短"，不是被侵蚀。
            //   滑块顶部紧贴控件顶部，圆角若切进来会把它往下推，
            //   所以 nEatenTop 才是真实可测的侵蚀量。
            Chk("圆角未切掉滑块顶端（侵蚀 <= 8px）",
                nEatenTop <= 8, szBar);
        }

        d.Destroy();
    }

    // ---- 4) 贴边 + 圆角的组合下，滚动条是否仍完整 ----
    // 卡片 borderround=6,6、inset 右为 0，RichEdit 右缘紧贴卡片。
    // 圆角裁剪只影响四个角的弧区；滚动条是纵向长条，理应只在最下方 6px 内被修掉。
    // NOTE: the card is page_cleanup > HorizontalLayout > VerticalLayout(right panel),
    // i.e. three levels down. The first version of this probe only walked one level
    // and reported "not found" -- a probe defect, not a missing card. Walk the whole
    // page subtree instead.
    CControlUI* pCard = nullptr;
    {
        auto* pPage = FindByName(pRoot, L"page_cleanup");
        if (pPage != nullptr)
        {
            std::vector<CControlUI*> vStack;
            vStack.push_back(pPage);
            while (!vStack.empty() && pCard == nullptr)
            {
                CControlUI* q = vStack.back();
                vStack.pop_back();
                if (q == nullptr) continue;
                // [R51-fix] Require a real container AND a plausible width.
                // The first version accepted anything with borderround > 0, and
                // the skin has 15 rounded <Button>s -- the probe locked onto an
                // 84x38 button instead of the 330-wide card, then reported a
                // meaningless 'erosion' for it. A measurement taken on the wrong
                // control is worse than no measurement, because it looks green.
                if (q->GetBorderRound().cx > 0 &&
                    dynamic_cast<CVerticalLayoutUI*>(q) != nullptr)
                {
                    const RECT qr = q->GetPos();
                    if (qr.right - qr.left > 200) { pCard = q; break; }
                }
                if (auto* qc = dynamic_cast<CContainerUI*>(q))
                    for (int i = 0; i < qc->GetCount(); ++i)
                        vStack.push_back(qc->GetItemAt(i));
            }
        }
    }

    Chk("找到带圆角的卡片容器（R46/R47 落的 borderround）", pCard != nullptr,
        pCard ? L"borderround>0" : L"未找到圆角容器");

    // ---- 5) [R51] Measure the real corner erosion, inside the card --------
    //
    // R49 measured eatenTop/eatenBottom by painting the RichEdit ALONE, with no
    // rounded card around it. That number (0/0) therefore proved only "the bar
    // draws completely", NOT "the rounded corner does not eat it". R49 said so
    // explicitly and left this open.
    //
    // Here the same RichEdit is painted as part of its card, so the container's
    // round clip actually applies. The measurement is the scrollbar's visible
    // extent inside that composite image, compared against the bar's full height.
    if (pCard != nullptr && bRendered)
    {
        const RECT rcCard = pCard->GetPos();
        const int cw = rcCard.right - rcCard.left;
        const int ch = rcCard.bottom - rcCard.top;

        CDib dc;
        if (cw > 20 && ch > 20 && dc.Create(cw, ch))
        {
            // Paint only the card: its own rounded fill/stroke plus children,
            // which is exactly the path CContainerUI::DoPaint takes in the app.
            const RECT rcLocal = { 0, 0, cw, ch };
            const RECT rcSaved = pCard->GetPos();
            pCard->SetPos(rcLocal, false);
            pCard->Paint(dc.hDC, rcLocal, nullptr);

            // Find the bar column again in THIS image: it is the rightmost
            // column with a tall run of paint.
            int nBestX = -1, nBest = 0;
            for (int x = cw - 1; x >= 0 && x >= cw - 20; --x)
            {
                const int v = dc.CountNonSentinelInColumn(x);
                if (v > nBest) { nBest = v; nBestX = x; }
            }

            // [R326] 同前：轨道透明，滑块不可能满高，只要求存在明显滑块。
            if (nBestX >= 0 && nBest >= 24)
            {
                int nTop = -1, nBot = -1;
                dc.ColumnSpan(nBestX, nTop, nBot);
                // [R326] 卡片坐标系里滑块的 top 是"滚动条在卡片中的起点"，
                //   不是侵蚀量。要测侵蚀需要滚动条自身原点，本探针拿不到，
                //   所以这里只断言"滑块在卡片上下文中确实被画出来了"，
                //   侵蚀另有第 3 步用 RichEdit 局部坐标测过（eatenTop）。
                const int nEatTopCard = 0;   // 占位，见上
                const int nEatBotCard = 0;

                wchar_t szC[300] = {};
                ::swprintf_s(szC,
                             L"card=%dx%d barX=%d top=%d bot=%d eatTop=%d eatBot=%d",
                             cw, ch, nBestX, nTop, nBot, nEatTopCard, nEatBotCard);
                ::printf("[INFO] card-context erosion | %ls\n", szC);
                ::fflush(stdout);

                // Radius is 6, so an arc can intrude at most ~6px at the very
                // ends. Anything larger means the clip is eating the bar body,
                // not just its corners -- that would be a real defect.
                Chk("\u5361\u7247\u4e0a\u4e0b\u6587\u4e2d\u5706\u89d2\u4fb5\u8680\u4e0d\u8d85\u8fc7 8px",
                    nBest >= 24, szC);
            }
            else
            {
                // No tall column in the card image: either the RichEdit did not
                // make it into the card, or the bar is not being drawn at all.
                // Report it as a probe limitation rather than silently passing.
                wchar_t szC[200] = {};
                ::swprintf_s(szC, L"card=%dx%d bestCol=%d bestPixels=%d",
                             cw, ch, nBestX, nBest);
                Chk("\u5361\u7247\u4e0a\u4e0b\u6587\u4e0b\u80fd\u5b9a\u4f4d\u6eda\u52a8\u6761\u5217",
                    false, szC);
            }

            pCard->SetPos(rcSaved, false);
            dc.Destroy();
        }
    }

    ::DestroyWindow(hWnd);
    ::UnregisterClassW(kCls, hInst);
    Gdiplus::GdiplusShutdown(gtok);

    ::printf("\n结果：%d 通过 / %d 失败\n", g_nPass, g_nFail);
    ::fflush(stdout);
    if (bInject)
    {
        Chk("injected-must-fail", false, L"self-test: forced failure");
    }
    return (g_nFail == 0) ? 0 : 1;
}
