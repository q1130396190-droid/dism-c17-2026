// ============================================================
// render_page_probe.cpp —— 用 DuiLib 自己的绘制路径出图（绕开 RDP 截图全黑）
//
// 为什么必须这样做：
//   本机是断开的 RDP 会话，PrintWindow / CopyFromScreen 一律返回全黑位图
//   （已实测多轮）。但"界面到底画成什么样"正是用户反馈的问题（页面内容区
//   空白、标题栏按钮显示异常），必须有可见证据。
//
//   办法：不走屏幕，直接调 CControlUI::Paint 把控件树画到自建的 32bpp DIB，
//   再落成 PNG。绘制路径与真实窗口完全一致（同一份 DoPaint/Paint 实现），
//   因此能代表用户看到的画面。
//
// 判据：
//   1) 每个页根在真实布局尺寸下绘制后，非背景像素数 > 0（不是空白）
//   2) 标题栏四个按钮的绘制区域必须有像素（不是空白/错位）
//   3) 导出的 PNG 供人工查看
// ============================================================

#include "UIlib.h"
// [R39] Shared layout numbers, same header the product includes.

#include <windows.h>
#include <gdiplus.h>
#include <cstdio>
#include <string>
#include <memory>
#include <vector>

#pragma comment(lib, "gdiplus.lib")

using namespace DuiLib;

namespace
{
int g_nPass = 0;
int g_nFail = 0;

void Mark(const char* s) { ::printf("[STEP] %s\n", s); ::fflush(stdout); }

void Chk(const char* pszName, bool bOk, const std::wstring& sDetail)
{
    char sz[512] = {};
    ::WideCharToMultiByte(CP_UTF8, 0, sDetail.c_str(),
                          static_cast<int>(sDetail.size()),
                          sz, sizeof(sz) - 1, nullptr, nullptr);
    if (bOk) { ++g_nPass; ::printf("[PASS] %s | %s\n", pszName, sz); }
    else     { ++g_nFail; ::printf("[FAIL] %s | %s\n", pszName, sz); }
    ::fflush(stdout);
}

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

class CProbeNavUI : public COptionUI
{
    DECLARE_DUICONTROL(CProbeNavUI)
public:
    CProbeNavUI() {}
    LPCTSTR GetClass() const override { return L"NavOptionUI"; }
};

IMPLEMENT_DUICONTROL(CProbeNavUI)

struct DIB
{
    HDC     hDC;
    HBITMAP hBM;
    HBITMAP hOld;
    DWORD*  p;
    int     w;
    int     h;

    DIB() : hDC(nullptr), hBM(nullptr), hOld(nullptr), p(nullptr), w(0), h(0) {}

    bool Create(int cx, int cy, DWORD bg)
    {
        w = cx; h = cy;
        hDC = ::CreateCompatibleDC(nullptr);
        if (hDC == nullptr) return false;

        BITMAPINFO bi;
        ::ZeroMemory(&bi, sizeof(bi));
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = cx;
        bi.bmiHeader.biHeight = -cy;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        void* pv = nullptr;
        hBM = ::CreateDIBSection(hDC, &bi, DIB_RGB_COLORS, &pv, nullptr, 0);
        if (hBM == nullptr) return false;
        p = static_cast<DWORD*>(pv);
        hOld = static_cast<HBITMAP>(::SelectObject(hDC, hBM));
        for (int i = 0; i < cx * cy; ++i) p[i] = bg;
        return true;
    }

    // 统计不同颜色个数（采样，避免大画布全量 set）
    // 判据意义：只有底色时颜色数≈1；有文字/边框/图标时颜色数会显著增多，
    // 因为抗锯齿与 ClearType 会产生大量中间色。
    int CountDistinctColors() const
    {
        if (p == nullptr) return 0;
        // 用 4096 桶的布尔表统计高 12 位，足够区分"单色 vs 有内容"
        unsigned char seen[4096] = {};
        int n = 0;
        const int nStep = (w * h > 200000) ? 3 : 1;
        for (int i = 0; i < w * h; i += nStep)
        {
            const DWORD c = p[i] & 0x00FFFFFF;
            const int key = static_cast<int>((c * 2654435761u) >> 20) & 0xFFF;
            if (!seen[key]) { seen[key] = 1; ++n; }
        }
        return n;
    }

    // 统计与背景不同的像素数
    int CountNonBg(DWORD bg) const
    {
        if (p == nullptr) return -1;
        int n = 0;
        for (int i = 0; i < w * h; ++i)
            if ((p[i] & 0x00FFFFFF) != (bg & 0x00FFFFFF)) ++n;
        return n;
    }

    // 统计指定矩形内与背景不同的像素数
    // [R36] Count pixels inside rc that differ from bg.
    //
    // Two latent bugs fixed here. Neither was live -- the function has no
    // callers today -- but both would bite the first person to use it:
    //   1) negative rc.left/top indexed p[y*w+x] out of bounds (a caller
    //      clipping to the widget rect easily produces negative coords);
    //   2) the p[...] != 0 guard was an alpha test on a 32bpp buffer whose
    //      top byte is 0 for 0x00FFFFFF entries, so fully opaque black
    //      pixels could be skipped while the comparison below already
    //      masks off alpha -- the two checks contradicted each other.
    int CountNonBgInRect(const RECT& rc, DWORD bg) const
    {
        if (p == nullptr) return -1;
        const int x0 = (rc.left   > 0) ? rc.left   : 0;
        const int y0 = (rc.top    > 0) ? rc.top    : 0;
        const int x1 = (rc.right  < w) ? rc.right  : w;
        const int y1 = (rc.bottom < h) ? rc.bottom : h;
        if (x1 <= x0 || y1 <= y0) return 0;
        int n = 0;
        const DWORD bgc = bg & 0x00FFFFFF;
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x)
                if ((p[y * w + x] & 0x00FFFFFF) != bgc) ++n;
        return n;
    }

    // [R34] Count neutral dark pixels (text). Sentinel bg 0x00FF00 sums to 255,
    // so a plain r+g+b<threshold test counts every unpainted pixel as text --
    // measured: all 620 rows reported 710 "dark" pixels where the PNG had no text.
    // Real text is neutral dark: all channels low AND close together.
    int CountDark() const
    {
        if (p == nullptr) return -1;
        int n = 0;
        for (int i = 0; i < w * h; ++i)
        {
            const DWORD c = p[i] & 0x00FFFFFF;
            const int r = (c >> 16) & 0xFF;
            const int g = (c >> 8) & 0xFF;
            const int b = c & 0xFF;
            const int mx = (r > g) ? ((r > b) ? r : b) : ((g > b) ? g : b);
            const int mn = (r < g) ? ((r < b) ? r : b) : ((g < b) ? g : b);
            if (mx < 140 && (mx - mn) < 40) ++n;
        }
        return n;
    }

    bool SavePng(const std::wstring& sPath)
    {
        if (p == nullptr) return false;
        // 用 GDI+ 从 HBITMAP 编码为 PNG
        Gdiplus::Bitmap bmp(hBM, nullptr);
        CLSID clsid;
        if (::CLSIDFromString(L"{557CF406-1A04-11D3-9A73-0000F81EF32E}", &clsid) != S_OK)
            return false;
        return bmp.Save(sPath.c_str(), &clsid, nullptr) == Gdiplus::Ok;
    }

    void Destroy()
    {
        if (hDC != nullptr && hOld != nullptr) ::SelectObject(hDC, hOld);
        if (hBM != nullptr) ::DeleteObject(hBM);
        if (hDC != nullptr) ::DeleteDC(hDC);
        hDC = nullptr; hBM = nullptr; hOld = nullptr; p = nullptr;
    }
};

const DWORD kBg = 0xFF00FF00u;   // 哨兵绿：便于统计"画了多少像素"

} // namespace

int main(int argc, char** argv)
{
    // [GATE-SELFTEST] /Inject adds one deliberately failing assertion so this
    // probe can prove its verdict path still works; otherwise a check that
    // silently stopped asserting would look exactly like a passing run.
    bool bInject = false;
    for (int i = 1; i < argc; ++i)
        if (::lstrcmpiA(argv[i], "/Inject") == 0) bInject = true;

    Mark("1 begin");

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
    const std::wstring sSkin = sRoot + L"\\bin64\\skin\\Win8Metro";
    const std::wstring sOutDir = sRoot + L"\\.box-agent-scratch\\render";

    ::CreateDirectoryW(sOutDir.c_str(), nullptr);

    HINSTANCE hInst = ::GetModuleHandleW(nullptr);
    CPaintManagerUI::SetInstance(hInst);
    CPaintManagerUI::SetResourcePath(sSkin.c_str());
    // [R65] Print the resolved resource path. The probe reported
    // "Main.xml parse failed" while the same skin loads fine in the real exe,
    // so the first thing to rule out is that it is looking somewhere else.
    ::wprintf(L"[INFO] skin path = %ls\r\n", sSkin.c_str());
    ::fflush(stdout);
    Mark("2 env ok");


    const wchar_t* kCls = L"RenderPageProbeWnd";
    WNDCLASSEXW wc;
    ::ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ::DefWindowProcW;
    wc.hInstance = hInst;
    wc.lpszClassName = kCls;
    ::RegisterClassExW(&wc);

    HWND hWnd = ::CreateWindowExW(0, kCls, L"probe", WS_OVERLAPPEDWINDOW,
                                  0, 0, 1280, 900, nullptr, nullptr, hInst, nullptr);
    if (hWnd == nullptr) { ::printf("[FATAL] CreateWindowEx\n"); return 2; }

    CPaintManagerUI pm;
    pm.Init(hWnd, L"RenderPageProbe");
    Mark("3 init ok");

    CControlFactory::GetInstance()->RegistControl(
        L"CNavOptionUI", static_cast<CreateClass>(CProbeNavUI::CreateControl));

    // [STALE-FIX] 原来这里还调用 RegisterCaptionBtnControl()，对应 CaptionBtn.cpp
    //   与皮肤中的 <CaptionBtn> 标签。两者都已移除（标题栏按钮改由皮肤内普通
    //   按钮配合 FrameWnd 的消息处理完成），该调用会因缺符号而编译失败，已删除。
    //   上方 CNavOptionUI 的注册仍与 CFrameWnd::RegisterWin8MetroControls() 一致。

    CDialogBuilder builder;
    CControlUI* pRoot = builder.Create(L"Main.xml", nullptr, nullptr, &pm);
    if (pRoot == nullptr) { ::printf("[FATAL] Main.xml parse failed\n"); return 2; }
    pm.AttachDialog(pRoot);
    pm.SetDPI(96);
    Mark("4 attached");

    // 真实窗口尺寸下布局（与 1280x900 主窗口一致）
    const int W = 1280, H = 900;
    RECT rcAll = { 0, 0, W, H };
    pRoot->SetPos(rcAll);
    Mark("5 laid out");

    // ---- 1) 整窗绘制并出 PNG ----
    {
        DIB d;
        if (d.Create(W, H, kBg))
        {
            pRoot->Paint(d.hDC, rcAll, nullptr);
            const int nPix = d.CountNonBg(kBg);
            const std::wstring sPng = sOutDir + L"\\main_window.png";
            const bool bSaved = d.SavePng(sPng);

            wchar_t sz[220] = {};
            ::swprintf_s(sz, L"pixels=%d png=%s", nPix, bSaved ? sPng.c_str() : L"(save failed)");
            Chk("整窗绘制非空", nPix > 1000, sz);
            d.Destroy();
        }
        else
        {
            Chk("整窗绘制非空", false, L"DIB 创建失败");
        }
    }

    // ---- 2) 标题栏四个按钮必须有像素 ----
    {
        // restorebtn 在 Main.xml 里是 visible="false"（仅在最大化时出现），
        // 未最大化时尺寸 0x0 属设计如此，不能当失败。
        struct { const wchar_t* n; bool needVisible; } kBtns[] = {
            { L"minbtn", true }, { L"maxbtn", true },
            { L"restorebtn", false }, { L"closebtn", true },
        };
        for (const auto& b : kBtns)
        {
            CControlUI* p = FindByName(pRoot, b.n);
            if (p == nullptr)
            {
                Chk("标题栏按钮存在", false, std::wstring(L"找不到 ") + b.n);
                continue;
            }
            if (!b.needVisible && !p->IsVisible())
            {
                ++g_nPass;
                ::printf("[PASS] 标题栏按钮可绘制 | %ls 未激活时隐藏（设计如此）\n", b.n);
                ::fflush(stdout);
                continue;
            }

            const RECT rcItem = p->GetPos();
            const int cw = rcItem.right - rcItem.left;
            const int ch = rcItem.bottom - rcItem.top;

            // 单独在按钮自身尺寸的画布上绘制，避免被父级裁切干扰
            DIB d;
            if (!d.Create(cw > 0 ? cw : 1, ch > 0 ? ch : 1, kBg))
            {
                Chk("标题栏按钮可绘制", false, L"DIB 创建失败");
                continue;
            }
            RECT rcLocal = { 0, 0, cw, ch };
            p->SetPos(rcLocal);
            p->Paint(d.hDC, rcLocal, nullptr);
            const int nPix = d.CountNonBg(kBg);

            wchar_t sz[200] = {};
            ::swprintf_s(sz, L"size=%dx%d 绘制像素=%d", cw, ch, nPix);
            Chk("标题栏按钮可绘制", nPix > 0, std::wstring(b.n) + L" " + sz);
            d.Destroy();
        }
    }

    // ---- 3) 逐个页根绘制（只画该页自身，尺寸取页面区域）----
    {
        const wchar_t* kPages[] = {
            L"page_cleanup", L"page_startup", L"page_appx",   L"page_tools",
            L"page_optim",   L"page_driver",  L"page_program", L"page_feature",
            L"page_update",  L"page_assoc",   L"page_unattend", L"page_tasks",
            L"page_control", L"page_intl",    L"page_regclean", L"page_welcome",
        };

        auto* pTabs = dynamic_cast<CTabLayoutUI*>(FindByName(pRoot, L"content_area"));
        if (pTabs == nullptr)
        {
            Chk("content_area TabLayout 存在", false, L"未找到");
        }
        else
        {
            ::printf("[INFO] content_area 子页数 = %d\n", pTabs->GetCount());
            ::fflush(stdout);
        }

        int nEmpty = 0;
        for (const wchar_t* pn : kPages)
        {
            CControlUI* p = FindByName(pRoot, pn);
            if (p == nullptr) continue;

            // 先激活所在 Tab 页，否则该页 visible=false、m_rcItem 为 0，
            // Paint 里 IntersectRect 直接失败 → 读到 0 像素（探针偏差）。
            if (pTabs != nullptr)
            {
                for (int t = 0; t < pTabs->GetCount(); ++t)
                {
                    if (pTabs->GetItemAt(t) == p) { pTabs->SelectItem(t); break; }
                }
            }

            const int pw = 1040, ph = 620;   // 内容区大致尺寸
            RECT rc = { 0, 0, pw, ph };
            pRoot->SetPos(rcAll, false);     // 重新布局整树，页才拿到矩形
            p->SetPos(rc, false);

            DIB d;
            if (!d.Create(pw, ph, kBg)) continue;
            p->Paint(d.hDC, rc, nullptr);
            const int nColors = d.CountDistinctColors();
            d.Destroy();

            char szName[128] = {};
            ::WideCharToMultiByte(CP_UTF8, 0, pn, -1, szName, sizeof(szName) - 1, nullptr, nullptr);
            char szDetail[160] = {};
            ::sprintf_s(szDetail, "distinctColors=%d", nColors);

            // 只有底色时颜色数很少（<=4）；有文字/控件时会明显更多
            const bool bOk = nColors > 4;
            if (!bOk) ++nEmpty;
            if (bOk) { ++g_nPass; ::printf("[PASS] %s | %s\n", szName, szDetail); }
            else     { ++g_nFail; ::printf("[FAIL] %s | %s (页面空白)\n", szName, szDetail); }
            ::fflush(stdout);
        }
        ::printf("[INFO] 空白页数量 = %d / 16\n", nEmpty);
        ::fflush(stdout);
    }

            // ---- [R34] 数据填充模式：列表有内容时会不会错位 ----
            //
            // 静态皮肤画得对，不代表填入真实行之后还对。空列表不会暴露行高累加溢出、
            // 文字裁切、滚动条挤占列宽这一类问题。按 Page_Cleanup_Build.cpp 的真实结构
            // （组头 34px + 行 28px）填入模拟数据后重排重绘。
            //
            // inset 只在 CContainerUI 上生效（UIContainer.cpp:739），所以行容器用
            // CVerticalLayoutUI而不是 CTextUI——否则缩进被静默忽略，
            // 探针会凭空报出一个不存在的排版缺陷。
            {
                auto* pTabs4 = dynamic_cast<CTabLayoutUI*>(FindByName(pRoot, L"content_area"));
                if (pTabs4 != nullptr)
                {
                    CControlUI* pFirstTab = FindByName(pRoot, L"page_cleanup");
                    for (int t = 0; t < pTabs4->GetCount(); ++t)
                    {
                        if (pTabs4->GetItemAt(t) == pFirstTab) { pTabs4->SelectItem(t); break; }
                    }
                }

                CControlUI* pClean = FindByName(pRoot, L"page_cleanup");
                auto* pList = dynamic_cast<CVerticalLayoutUI*>(FindByName(pRoot, L"clean_list"));

                Chk("clean_list 存在且是 VerticalLayout",
                    pClean != nullptr && pList != nullptr,
                    pList ? L"找到" : L"未找到");

                if (pList != nullptr)
                {
                    const int pw = 1040, ph = 620;
                    RECT rcPage = { 0, 0, pw, ph };
                    pRoot->SetPos(rcAll, false);
                    if (pClean) pClean->SetPos(rcPage, false);

                    int nTextEmpty = 0;
                    {
                        DIB d;
                        if (d.Create(pw, ph, kBg))
                        {
                            pClean->Paint(d.hDC, rcPage, nullptr);
                            nTextEmpty = d.CountDark();
                            d.Destroy();
                        }
                    }

                    pList->RemoveAll();
                    int nRowsAdded = 0;
                    for (int g = 0; g < 3; ++g)
                    {
                        auto pHdr = std::make_unique<CVerticalLayoutUI>();
                        CDuiString sH; sH.Format(L"grp_%d", g);
                        pHdr->SetName(sH.GetData());
                        pHdr->SetFixedHeight(cm::kGroupHeaderHeight);
                        pHdr->SetInset(RECT{ cm::kGroupHeaderInsetLeft, 0, cm::kGroupHeaderInsetRight, 0 });
                        pHdr->SetPadding(RECT{ 0, cm::GroupGapFor(g), 0, 0 });
                        {
                            auto pT = std::make_unique<CTextUI>();
                            pT->SetText(L"■ 示例分组");
                            pT->SetFont(6);
                            pT->SetTextColor(0xFF1A1A1A);
                            pT->SetAttribute(L"align", L"left");
                            pT->SetAttribute(L"valign", L"vcenter");
                            pHdr->Add(pT.release());
                        }
                        pList->Add(pHdr.release());

                        for (int r = 0; r < 4; ++r)
                        {
                            auto pRow = std::make_unique<CVerticalLayoutUI>();
                            CDuiString sR; sR.Format(L"row_%d_%d", g, r);
                            pRow->SetName(sR.GetData());
                            pRow->SetFixedHeight(cm::kRuleRowHeight);
                            pRow->SetInset(RECT{ cm::kRuleRowInsetLeft, 0, cm::kRuleRowInsetRight, 0 });
                            {
                                auto pT = std::make_unique<CTextUI>();
                                pT->SetText(L"示例清理项：临时文件与缓存目录");
                                pT->SetFont(1);
                                pT->SetTextColor(0xFF1A1A1A);
                                pT->SetAttribute(L"align", L"left");
                                pT->SetAttribute(L"valign", L"vcenter");
                                pRow->Add(pT.release());
                            }
                            pList->Add(pRow.release());
                            ++nRowsAdded;
                        }
                    }

                    pRoot->SetPos(rcAll, false);
                    if (pClean) pClean->SetPos(rcPage, false);

                    const RECT rcList = pList->GetPos();
                    int nOverflow = 0, nZeroRect = 0;
                    for (int i = 0; i < pList->GetCount(); ++i)
                    {
                        CControlUI* pc = pList->GetItemAt(i);
                        if (pc == nullptr) continue;
                        const RECT rr = pc->GetPos();
                        if (rr.right <= rr.left || rr.bottom <= rr.top) { ++nZeroRect; continue; }
                        if (rr.bottom > rcList.bottom) ++nOverflow;
                    }

                    int nTextFilled = 0;
                    {
                        DIB d;
                        if (d.Create(pw, ph, kBg))
                        {
                            pClean->Paint(d.hDC, rcPage, nullptr);
                            nTextFilled = d.CountDark();
                            wchar_t szPng[260] = {};
                            ::swprintf_s(szPng, L"%s\\page_cleanup_filled.png", sOutDir.c_str());
                            if (!d.SavePng(szPng))
                                ::printf("[WARN] filled png save failed\n");
                            d.Destroy();
                        }
                    }

                    ::printf("[INFO] 数据填充 | added=%d zeroRect=%d overflow=%d text %d -> %d\n",
                             nRowsAdded, nZeroRect, nOverflow, nTextEmpty, nTextFilled);

                    // [R39] Dump child rects so layout metrics can be
                    // measured. Without this the mutation test below could not
                    // tell 8px from 16px: both passed every existing assertion.
                    for (int di = 0; di < pList->GetCount() && di < 6; ++di)
                    {
                        CControlUI* pd = pList->GetItemAt(di);
                        if (pd == nullptr) continue;
                        const RECT rr = pd->GetPos();
                        wchar_t szN[160] = {};
                        ::swprintf_s(szN, L"  item[%d] %s top=%ld h=%ld",
                                     di, pd->GetName().GetData(),
                                     (long)rr.top, (long)(rr.bottom - rr.top));
                        ::wprintf(L"%ls\n", szN);
                    }
                    ::fflush(stdout);
                    ::fflush(stdout);

                    Chk("填充后行控件都拿到真实矩形（无 0 尺寸）",
                        nZeroRect == 0,
                        std::wstring(L"zeroRect=") + std::to_wstring(nZeroRect) +
                        L" / " + std::to_wstring(nRowsAdded + 3) + L" 个");

                    Chk("填充后文字像素明显增加（数据真的画出来了）",
                        nTextFilled > nTextEmpty,
                        std::wstring(L"空=") + std::to_wstring(nTextEmpty) +
                        L" 填充=" + std::to_wstring(nTextFilled));

                    if (nOverflow > 0)
                        ::printf("[INFO] %d 行超出列表可视区（预期，列表可滚动）\n", nOverflow);

                    pList->RemoveAll();
                }
            }


    // ---- 5) [R51] Optim page real rows ----------------------------------
    //
    // The optim page has had ZERO mutation cases since mutation testing
    // started. The reason was not laziness: the probe only ever drew the
    // page's static skin, and Page_Optim.cpp builds its rows at runtime,
    // so there was no observable line to watch. R50 hoisted the numbers
    // into OptimLayoutMetrics.h; this block finally builds rows from them
    // and prints their rects, which is what a mutation case needs.
    //
    // Sizes come from om::* -- the same header the product includes -- so
    // a drift between product and probe is a compile-time impossibility
    // rather than something a human has to notice.
    {
        auto* pTabs5 = dynamic_cast<CTabLayoutUI*>(FindByName(pRoot, L"content_area"));
        if (pTabs5 != nullptr)
        {
            CControlUI* pOptimTab = FindByName(pRoot, L"page_optim");
            for (int t = 0; t < pTabs5->GetCount(); ++t)
            {
                if (pTabs5->GetItemAt(t) == pOptimTab) { pTabs5->SelectItem(t); break; }
            }
        }

        CControlUI* pOptim = FindByName(pRoot, L"page_optim");
        auto* pTree = dynamic_cast<CVerticalLayoutUI*>(FindByName(pRoot, L"optim_tree"));

        Chk("optim_tree \u5b58\u5728\u4e14\u662f VerticalLayout",
            pOptim != nullptr && pTree != nullptr,
            pTree ? L"\u627e\u5230" : L"\u672a\u627e\u5230");

        if (pTree != nullptr)
        {
            const int pw = 1040, ph = 620;
            RECT rcPage = { 0, 0, pw, ph };
            pRoot->SetPos(rcAll, false);
            if (pOptim) pOptim->SetPos(rcPage, false);

            pTree->RemoveAll();
            int nAdded = 0;
            for (int g = 0; g < 2; ++g)
            {
                auto pHdr = std::make_unique<CVerticalLayoutUI>();
                CDuiString sH; sH.Format(L"og_%d", g);
                pHdr->SetName(sH.GetData());
                pHdr->SetFixedHeight(om::kGroupRowHeight);
                pHdr->SetInset(RECT{ om::kGroupRowInsetLeft, 0, om::kGroupRowInsetRight, 0 });
                pHdr->SetChildPadding(om::kGroupRowChildPadding);
                {
                    auto pT = std::make_unique<CTextUI>();
                    pT->SetText(L"\u793a\u4f8b\u4f18\u5316\u7ec4");
                    pT->SetFont(6);
                    pT->SetTextColor(0xFF1A1A1A);
                    pT->SetAttribute(L"align", L"left");
                    pT->SetAttribute(L"valign", L"vcenter");
                    pHdr->Add(pT.release());
                }
                pTree->Add(pHdr.release());

                for (int r = 0; r < 3; ++r)
                {
                    auto pRow = std::make_unique<CVerticalLayoutUI>();
                    CDuiString sR; sR.Format(L"or_%d_%d", g, r);
                    pRow->SetName(sR.GetData());
                    pRow->SetFixedHeight(om::kItemRowHeight);
                    pRow->SetInset(RECT{ om::kItemRowInsetLeft, 0, om::kItemRowInsetRight, 0 });
                    pRow->SetChildPadding(om::kItemRowChildPadding);
                    {
                        auto pT = std::make_unique<CTextUI>();
                        pT->SetText(L"\u793a\u4f8b\u4f18\u5316\u9879");
                        pT->SetFont(1);
                        pT->SetTextColor(0xFF1A1A1A);
                        pRow->Add(pT.release());
                    }
                    pTree->Add(pRow.release());
                    ++nAdded;
                }
            }

            pRoot->SetPos(rcAll, false);
            if (pOptim) pOptim->SetPos(rcPage, false);

            int nZero = 0;
            for (int i = 0; i < pTree->GetCount(); ++i)
            {
                CControlUI* pc = pTree->GetItemAt(i);
                if (pc == nullptr) continue;
                const RECT rr = pc->GetPos();
                if (rr.right <= rr.left || rr.bottom <= rr.top) ++nZero;
            }

            ::printf("[INFO] optim rows | added=%d zeroRect=%d\n", nAdded, nZero);

            // Dump the first rows: this is the observation point the optim
            // mutation cases watch. Without it, changing kItemRowHeight would
            // leave every assertion green -- exactly the R39 blind-suite trap.
            for (int i = 0; i < pTree->GetCount() && i < 5; ++i)
            {
                CControlUI* pc = pTree->GetItemAt(i);
                if (pc == nullptr) continue;
                const RECT rr = pc->GetPos();
                wchar_t szN[160] = {};
                ::swprintf_s(szN, L"  oitem[%d] %s top=%ld h=%ld",
                             i, pc->GetName().GetData(),
                             (long)rr.top, (long)(rr.bottom - rr.top));
                ::wprintf(L"%ls\n", szN);
            }
            ::fflush(stdout);

            Chk("\u4f18\u5316\u9875\u586b\u5145\u540e\u884c\u63a7\u4ef6\u90fd\u62ff\u5230\u771f\u5b9e\u77e9\u5f62",
                nZero == 0,
                std::wstring(L"zeroRect=") + std::to_wstring(nZero) +
                L" / " + std::to_wstring(nAdded + 2) + L" \u4e2a");

            pTree->RemoveAll();
        }
    }


    // ---- [R58] welcome_box must resolve, or the page builds nothing --------
    //
    // CPageWelcome::BuildPage() starts with `if (!pBox) return;`. Neither skin
    // defined welcome_box (the name lived only inside a comment), so the
    // refresh button, the brand banner and all four system overview cards
    // were never created. The static text still rendered, which is why the
    // page never looked broken.
    //
    // Checking the lookup is the cheapest faithful test: it is the exact
    // condition the function gates on. It does not prove the cards lay out
    // correctly, only that the build is no longer skipped.
    {
        CControlUI* pWb = FindByName(pRoot, L"welcome_box");
        Chk("welcome_box \u5b58\u5728\uff08\u5426\u5219 BuildPage() \u63d0\u524d\u8fd4\u56de\uff09",
            pWb != nullptr,
            pWb ? L"\u627e\u5230" : L"\u7f3a\u5931 -> \u6b22\u8fce\u9875\u52a8\u6001\u5185\u5bb9\u4e0d\u4f1a\u6784\u5efa");

        // [R64] Presence alone is not enough. BuildPage() fills this container
        // at run time and bails out silently on several paths (system API
        // failure, empty overview), so "container exists" and "container has
        // content" are different facts.
        //
        // The probe loads the skin but does not call BuildPage(), so it cannot
        // assert "has cards". It can pin the invariant that the static skin must
        // leave it empty: content is the runtime builder's job. Recording the
        // count turns a future "someone put static children here" into a
        // visible diff instead of an invisible one.
        int nKids = 0;
        if (auto* pC = dynamic_cast<CContainerUI*>(pWb))
            nKids = pC->GetCount();

        wchar_t szKids[160] = {};
        ::swprintf_s(szKids,
                     L"children=%d \uff08\u8fd0\u884c\u671f\u586b\u5145\uff0c\u9759\u6001\u671f\u5e94\u4e3a 0\uff09",
                     nKids);
        Chk("welcome_box \u9759\u6001\u671f\u4e0d\u542b\u5b50\u63a7\u4ef6",
            nKids == 0, szKids);
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
