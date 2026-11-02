// ============================================================
// roundrect_filled.cpp —— DrawRoundRectFilled 像素级实测探针
//
// 为什么需要像素读回：
//   "圆角填充"不能靠看代码或看截图判断。判据必须可量化——
//   在 32bpp DIB 上绘制一个圆角实心矩形，然后读回像素：
//     * 四个角点  → 必须仍为背景色（未被填充覆盖）
//     * 中心点    → 必须为填充色
//     * 边中点    → 必须为填充色（证明不是画了个空框）
//   同时对比"无圆角"路径（cxyRound=0 走 DrawColor），
//   角点必须被填满 —— 这样能证明差异确实来自圆角，而非别的因素。
//
// 输出：每项 PASS/FAIL，最后汇总；有任何 FAIL 退出码 1。
// ============================================================

#include <windows.h>
#include <gdiplus.h>
#include <cstdio>

#pragma comment(lib, "gdiplus.lib")

// 直接消费框架公开导出（链接 bin64/amd64/DuiLib.lib），
// 保证探针验证的就是 Demos 实际运行的那份实现，而不是另一份副本。
#include "UIlib.h"


namespace
{
constexpr int kW = 120;
constexpr int kH = 60;

struct Bitmap
{
    HDC      hDC = nullptr;
    HBITMAP  hBM = nullptr;
    HBITMAP  hOld = nullptr;
    DWORD*   pBits = nullptr;

    bool Create()
    {
        hDC = ::CreateCompatibleDC(nullptr);
        if (hDC == nullptr) return false;

        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = kW;
        bi.bmiHeader.biHeight = -kH;      // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        void* pv = nullptr;
        hBM = ::CreateDIBSection(hDC, &bi, DIB_RGB_COLORS, &pv, nullptr, 0);
        if (hBM == nullptr) return false;
        pBits = static_cast<DWORD*>(pv);
        hOld = static_cast<HBITMAP>(::SelectObject(hDC, hBM));

        // 铺背景：纯绿，便于和填充色区分
        for (int i = 0; i < kW * kH; ++i) pBits[i] = kBg;
        return true;
    }

    void Destroy()
    {
        if (hDC != nullptr && hOld != nullptr) ::SelectObject(hDC, hOld);
        if (hBM != nullptr) ::DeleteObject(hBM);
        if (hDC != nullptr) ::DeleteDC(hDC);
        hDC = nullptr; hBM = nullptr; hOld = nullptr; pBits = nullptr;
    }

    DWORD At(int x, int y) const
    {
        if (pBits == nullptr) return 0;
        return pBits[y * kW + x];
    }

    static constexpr DWORD kBg = 0xFF00FF00;      // 绿
    static constexpr DWORD kFill = 0xFF3366CC;    // 蓝
};

int g_nPass = 0;
int g_nFail = 0;

void Chk(const char* pszName, bool bOk, const char* pszDetail)
{
    if (bOk) { ++g_nPass; ::printf("[PASS] %s  %s\n", pszName, pszDetail); }
    else     { ++g_nFail; ::printf("[FAIL] %s  %s\n", pszName, pszDetail); }
}

// 允许 ±8 的通道容差：GDI+ 抗锯齿会在角点留下部分混合像素
bool NearColor(DWORD a, DWORD b, int nTol = 8)
{
    const int dr = abs(static_cast<int>(GetRValue(a)) - static_cast<int>(GetRValue(b)));
    const int dg = abs(static_cast<int>(GetGValue(a)) - static_cast<int>(GetGValue(b)));
    const int db = abs(static_cast<int>(GetBValue(a)) - static_cast<int>(GetBValue(b)));
    return dr <= nTol && dg <= nTol && db <= nTol;
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
    ULONG_PTR hToken = 0;
    if (Gdiplus::GdiplusStartup(&hToken, &gsi, nullptr) != Gdiplus::Ok)
    {
        ::printf("[FATAL] GdiplusStartup failed\n");
        return 2;
    }

    ::printf("=== DrawRoundRectFilled 像素级验证 ===\n");
    ::printf("画布 %dx%d，背景 0x%08lX（绿），填充 0x%08lX（蓝）\n\n",
             kW, kH, (unsigned long)Bitmap::kBg, (unsigned long)Bitmap::kFill);

    RECT rc = { 0, 0, kW, kH };

    // ---- 用例 1：圆角 = 12px ----
    {
        Bitmap bm;
        if (!bm.Create()) { ::printf("[FATAL] CreateDIBSection failed\n"); return 2; }

        const int nRound = 12;
        DuiLib::CRenderEngine::DrawRoundRectFilled(bm.hDC, rc, nRound, Bitmap::kFill);

        // 角点 (1,1)：圆角半径 12 时，该点必须在圆外 → 仍是背景绿
        const DWORD cCornerTL = bm.At(1, 1);
        const DWORD cCornerTR = bm.At(kW - 2, 1);
        const DWORD cCornerBL = bm.At(1, kH - 2);
        const DWORD cCornerBR = bm.At(kW - 2, kH - 2);

        // 中心与边中点：必须被填充
        const DWORD cCenter = bm.At(kW / 2, kH / 2);
        const DWORD cMidTop = bm.At(kW / 2, 2);
        const DWORD cMidLeft = bm.At(2, kH / 2);

        char buf[256];
        ::sprintf_s(buf, "cornerTL=0x%08lX (期望背景 0x%08lX)",
                    (unsigned long)cCornerTL, (unsigned long)Bitmap::kBg);
        Chk("圆角12 左上角未被填充", NearColor(cCornerTL, Bitmap::kBg), buf);

        ::sprintf_s(buf, "cornerBR=0x%08lX", (unsigned long)cCornerBR);
        Chk("圆角12 右下角未被填充", NearColor(cCornerBR, Bitmap::kBg), buf);

        Chk("圆角12 右上角未被填充", NearColor(cCornerTR, Bitmap::kBg), "cornerTR");
        Chk("圆角12 左下角未被填充", NearColor(cCornerBL, Bitmap::kBg), "cornerBL");

        ::sprintf_s(buf, "center=0x%08lX (期望填充 0x%08lX)",
                    (unsigned long)cCenter, (unsigned long)Bitmap::kFill);
        Chk("圆角12 中心已填充", NearColor(cCenter, Bitmap::kFill), buf);

        ::sprintf_s(buf, "midTop=0x%08lX", (unsigned long)cMidTop);
        Chk("圆角12 上边中点已填充", NearColor(cMidTop, Bitmap::kFill), buf);

        ::sprintf_s(buf, "midLeft=0x%08lX", (unsigned long)cMidLeft);
        Chk("圆角12 左边中点已填充", NearColor(cMidLeft, Bitmap::kFill), buf);

        bm.Destroy();
    }

    // ---- 用例 2：对照 —— cxyRound=0 必须退化为直角填满 ----
    {
        Bitmap bm;
        if (!bm.Create()) { ::printf("[FATAL] CreateDIBSection failed\n"); return 2; }

        DuiLib::CRenderEngine::DrawRoundRectFilled(bm.hDC, rc, 0, Bitmap::kFill);

        const DWORD cCorner = bm.At(1, 1);
        char buf[256];
        ::sprintf_s(buf, "corner=0x%08lX (期望填充 0x%08lX)",
                    (unsigned long)cCorner, (unsigned long)Bitmap::kFill);
        Chk("圆角0 退化直角、角点被填满", NearColor(cCorner, Bitmap::kFill), buf);

        bm.Destroy();
    }

    // ---- 用例 3：半径超过短边一半时必须被钳制，不能画崩 ----
    {
        Bitmap bm;
        if (!bm.Create()) { ::printf("[FATAL] CreateDIBSection failed\n"); return 2; }

        DuiLib::CRenderEngine::DrawRoundRectFilled(bm.hDC, rc, 500, Bitmap::kFill);

        const DWORD cCenter = bm.At(kW / 2, kH / 2);
        char buf[256];
        ::sprintf_s(buf, "center=0x%08lX", (unsigned long)cCenter);
        Chk("半径超大被钳制、中心仍填充", NearColor(cCenter, Bitmap::kFill), buf);

        bm.Destroy();
    }

    // ---- 用例 4：全透明色必须直接跳过（与 DrawColor 语义一致）----
    {
        Bitmap bm;
        if (!bm.Create()) { ::printf("[FATAL] CreateDIBSection failed\n"); return 2; }

        DuiLib::CRenderEngine::DrawRoundRectFilled(bm.hDC, rc, 12, 0x00123456);

        const DWORD cCenter = bm.At(kW / 2, kH / 2);
        char buf[256];
        ::sprintf_s(buf, "center=0x%08lX (期望仍是背景)", (unsigned long)cCenter);
        Chk("全透明色不绘制", NearColor(cCenter, Bitmap::kBg), buf);

        bm.Destroy();
    }

    Gdiplus::GdiplusShutdown(hToken);

    ::printf("\n结果：%d 通过 / %d 失败\n", g_nPass, g_nFail);
    if (bInject)
    {
        Chk("injected-must-fail", false, "self-test: forced failure");
    }
    return (g_nFail == 0) ? 0 : 1;
}
