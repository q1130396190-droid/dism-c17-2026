// ============================================================
// UiSnapshot.cpp —— 界面离屏渲染实现
// 见 UiSnapshot.h 顶部说明（为什么需要、与真实窗口的差异）。
// ============================================================

#include "stdafx.h"

#include "UiSnapshot.h"
#include "ThemeTokens.h"   // 铺底用 Surface 令牌，与皮肤根节点一致

#include <vector>
#include <map>
#include <cstdio>

namespace uisnap
{

namespace
{
// [R209] 最近一次渲染的统计文本（供日志输出）
std::wstring g_sLastStats;
}

namespace
{

}   // namespace

bool RenderRootToPng(DuiLib::CControlUI* pRoot, const wchar_t* pszPath,
                     int width, int height)
{
    if (!pRoot || !pszPath || width <= 0 || height <= 0) return false;

    // ---- 1) 32 位 DIB：可写像素，便于事后统计；同时兼容 GDI 绘制 ----
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = width;
    bi.bmiHeader.biHeight      = -height;   // 负数 = 自上而下，行序与图片一致
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* pBits = nullptr;
    HDC hdcScreen = ::GetDC(nullptr);
    if (!hdcScreen) return false;

    HDC hdcMem = ::CreateCompatibleDC(hdcScreen);
    HBITMAP hBmp = ::CreateDIBSection(hdcScreen, &bi, DIB_RGB_COLORS, &pBits, nullptr, 0);
    ::ReleaseDC(nullptr, hdcScreen);
    if (!hdcMem || !hBmp || !pBits)
    {
        if (hBmp) ::DeleteObject(hBmp);
        if (hdcMem) ::DeleteDC(hdcMem);
        return false;
    }
    HGDIOBJ hOldBmp = ::SelectObject(hdcMem, hBmp);

    // ---- 2) 先铺底色：DoPaint 不保证覆盖每个像素，未铺底会留下随机内存 ----
    //   用 Surface 令牌（与皮肤根节点一致），保证"空白处"也是正确底色。
    {
        const DWORD cr = tokens::Token(tokens::EToken::Surface);
        const COLORREF clr = RGB((cr >> 16) & 0xFF, (cr >> 8) & 0xFF, cr & 0xFF);
        HBRUSH hbr = ::CreateSolidBrush(clr);
        RECT rcFill = { 0, 0, width, height };
        ::FillRect(hdcMem, &rcFill, hbr);
        ::DeleteObject(hbr);
    }

    // ---- 3) 布局 + 绘制 ----
    //   SetPos 传客户区矩形：root 是 Window 的子节点，直接给全客户区即可。
    RECT rcRoot = { 0, 0, width, height };
    pRoot->SetPos(rcRoot);
    RECT rcPaint = rcRoot;

    // [R239] 必须走 Paint()，不能直接 DoPaint()。
    //
    // 这是「导出/启动后只剩底色、一个控件都看不见」的根因：
    //   m_rcPaint 只在 CControlUI::Paint() 里通过
    //       ::IntersectRect(&m_rcPaint, &rcPaint, &m_rcItem)
    //   赋值；构造函数里它是 ZeroMemory 过的 {0,0,0,0}。
    //   旧代码绕过 Paint() 直接调 DoPaint()，于是 m_rcPaint 一直是空矩形：
    //     · DoPaint 用 m_rcPaint 生成子控件裁剪区（GenerateClip），
    //       空矩形 ⇒ 裁剪区面积为 0 ⇒ 每个子控件 Paint 时 IntersectRect 失败并 return；
    //     · 根节点没有 borderround 时走 PaintBkColor(hDC)，用的是 m_rcPaint，
    //       画出来也是空 —— 最终整幅图只剩本函数第 2 步预铺的 Surface 底色。
    //   实测对照：修复前 16 页里 15 页非底色占比只有 24~33%（等于"只有底色"），
    //   只有 Cleanup 因控件树更早就位而侥幸不同。
    //
    // 走 Paint() 后 m_rcPaint 被正确设为 rcPaint ∩ m_rcItem = 全客户区，
    // 子控件随之各自算出自己的 m_rcPaint，整棵树才真正画得出来。
    pRoot->Paint(hdcMem, rcPaint, nullptr);

    // 让 GDI 把绘制命令立即刷到位图，否则 CreateDIBSection 的 pBits 可能还是旧内容
    ::GdiFlush();

    // ---- 4) 写 BMP（无编码依赖）----
    //   原实现用 GDI+ 存 PNG，但 GDI+ 的编码走 COM，在本会话里由 dllhost
    //   代理执行，导致输出文件被另一进程长期占用、后续读不出来。
    //   改写成 24 位 BMP：纯 Win32 文件写入，句柄在本进程内即时关闭。
    bool bOk = false;
    {
        const int nRowBytes = ((width * 3 + 3) / 4) * 4;   // BMP 行 4 字节对齐
        const DWORD dwImgSize = static_cast<DWORD>(nRowBytes) * height;
        BITMAPFILEHEADER fh = {};
        BITMAPINFOHEADER ih = {};
        fh.bfType = 0x4D42;                     // 'BM'
        fh.bfOffBits = sizeof(fh) + sizeof(ih);
        fh.bfSize = fh.bfOffBits + dwImgSize;
        ih.biSize = sizeof(ih);
        ih.biWidth = width;
        ih.biHeight = height;                   // 正数 = 自下而上
        ih.biPlanes = 1;
        ih.biBitCount = 24;
        ih.biCompression = BI_RGB;
        ih.biSizeImage = dwImgSize;

        HANDLE hFile = ::CreateFileW(pszPath, GENERIC_WRITE, 0, nullptr,
                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile != INVALID_HANDLE_VALUE)
        {
            DWORD dwW = 0;
            bool bAllOk = true;
            bAllOk &= (::WriteFile(hFile, &fh, sizeof(fh), &dwW, nullptr) != FALSE);
            bAllOk &= (::WriteFile(hFile, &ih, sizeof(ih), &dwW, nullptr) != FALSE);

            // 逐行转换 BGRA -> BGR（自下而上）
            std::vector<BYTE> row(static_cast<size_t>(nRowBytes), 0);
            const BYTE* pSrc = static_cast<const BYTE*>(pBits);
            for (int y = height - 1; y >= 0 && bAllOk; --y)
            {
                const BYTE* p = pSrc + static_cast<size_t>(y) * width * 4;
                for (int x = 0; x < width; ++x)
                {
                    row[x * 3 + 0] = p[x * 4 + 0];
                    row[x * 3 + 1] = p[x * 4 + 1];
                    row[x * 3 + 2] = p[x * 4 + 2];
                }
                bAllOk &= (::WriteFile(hFile, row.data(), static_cast<DWORD>(nRowBytes),
                                       &dwW, nullptr) != FALSE);
            }
            ::CloseHandle(hFile);
            bOk = bAllOk;

            // [R209] 同进程内直接算像素统计，避免"外部工具读不到文件"的隔离问题。
            //   统计口径与分析脚本一致：主导色占比、唯一色数、侧栏/内容区底色。
            {
                std::wstring sStat(pszPath);
                sStat += L".txt";
                HANDLE hTxt = ::CreateFileW(sStat.c_str(), GENERIC_WRITE,
                                            FILE_SHARE_READ, nullptr,
                                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (hTxt != INVALID_HANDLE_VALUE)
                {
                    std::map<DWORD, int> hist;
                    int nSamples = 0;
                    for (int y = 0; y < height; y += 3)
                    {
                        for (int x = 0; x < width; x += 3)
                        {
                            const BYTE* pp = static_cast<const BYTE*>(pBits)
                                           + (static_cast<size_t>(y) * width + x) * 4;
                            const DWORD c = pp[0] | (pp[1] << 8) | (pp[2] << 16);
                            ++hist[c];
                            ++nSamples;
                        }
                    }
                    DWORD dwTop = 0; int nTop = 0;
                    for (const auto& kv : hist)
                        if (kv.second > nTop) { nTop = kv.second; dwTop = kv.first; }

                    auto at = [&](int x, int y) -> DWORD
                    {
                        const BYTE* pp = static_cast<const BYTE*>(pBits)
                                       + (static_cast<size_t>(y) * width + x) * 4;
                        return pp[0] | (pp[1] << 8) | (pp[2] << 16);
                    };
                    const DWORD cSide = at(120, height / 2);
                    const DWORD cCont = at(width * 3 / 4, height / 2);

                    // [R241] 内容区口径：整幅图占比无法区分"空白"与"正常留白"。
                    //   界面左侧是导航、上下是工具条，整图底色占比 60~75% 本来就很正常；
                    //   用它判"页面是不是空的"会一直误报。这里单独统计主内容区
                    //   （右侧、避开导航与上下条）的唯一色数与非底色占比，
                    //   空白页会得到 unique 个位数，有内容则是数十到数百。
                    int nContUnique = 0;
                    int nContNonBg = 0;
                    {
                        const int x0 = width / 5;
                        const int y0 = height / 5;
                        const int x1 = width - 8;
                        const int y1 = height - 12;
                        std::map<DWORD, int> histC;
                        int nTotC = 0;
                        for (int y = y0; y < y1; y += 4)
                        {
                            for (int x = x0; x < x1; x += 4)
                            {
                                const DWORD c = at(x, y);
                                ++histC[c];
                                ++nTotC;
                            }
                        }
                        nContUnique = static_cast<int>(histC.size());
                        int nTopC = 0;
                        for (const auto& kv : histC)
                            if (kv.second > nTopC) nTopC = kv.second;
                        nContNonBg = nTotC ? (100 - nTopC * 100 / nTotC) : 0;
                    }

                    // 逐段拼接：避免 snprintf 的 %zu 在某些 CRT 下不兼容导致空输出
                    std::string sRep;
                    {
                        char b1[64] = {};
                        std::snprintf(b1, sizeof(b1), "size=%dx%d", width, height);
                        sRep += b1;
                        std::snprintf(b1, sizeof(b1), " unique=%d", static_cast<int>(hist.size()));
                        sRep += b1;
                        std::snprintf(b1, sizeof(b1), " top=#%06X", static_cast<unsigned>(dwTop));
                        sRep += b1;
                        std::snprintf(b1, sizeof(b1), " toppct=%d", nTop * 100 / (nSamples ? nSamples : 1));
                        sRep += b1;
                        std::snprintf(b1, sizeof(b1), " nonbg=%d",
                                      100 - nTop * 100 / (nSamples ? nSamples : 1));
                        sRep += b1;
                        std::snprintf(b1, sizeof(b1), " side=#%06X", static_cast<unsigned>(cSide));
                        sRep += b1;
                        std::snprintf(b1, sizeof(b1), " content=#%06X", static_cast<unsigned>(cCont));
                        sRep += b1;
                        std::snprintf(b1, sizeof(b1), " cuniq=%d", nContUnique);
                        sRep += b1;
                        std::snprintf(b1, sizeof(b1), " cnonbg=%d", nContNonBg);
                        sRep += b1;

                        // [R209] bright ratio: on a dark theme near-white
                        //   pixels must stay rare. A high value means some
                        //   control never picked up the theme token.
                        int nBright = 0;
                        for (int y = 0; y < height; y += 3)
                        {
                            for (int x = 0; x < width; x += 3)
                            {
                                const BYTE* pp = static_cast<const BYTE*>(pBits)
                                               + (static_cast<size_t>(y) * width + x) * 4;
                                if ((pp[2] * 30 + pp[1] * 59 + pp[0] * 11) / 100 >= 200)
                                    ++nBright;
                            }
                        }
                        std::snprintf(b1, sizeof(b1), " bright=%d",
                                      nBright * 100 / (nSamples ? nSamples : 1));
                        sRep += b1;
                    }
                    const int nLen = static_cast<int>(sRep.size());
                    // 同步一份宽字符版给日志用
                    {
                        std::wstring w(sRep.begin(), sRep.end());
                        g_sLastStats.swap(w);
                    }
                    DWORD dwW2 = 0;
                    if (nLen > 0) ::WriteFile(hTxt, sRep.data(), static_cast<DWORD>(nLen), &dwW2, nullptr);
                    ::CloseHandle(hTxt);
                }
            }
        }
    }

    ::SelectObject(hdcMem, hOldBmp);
    ::DeleteObject(hBmp);
    ::DeleteDC(hdcMem);
    return bOk;
}

const wchar_t* LastStatsText()
{
    return g_sLastStats.c_str();
}

}   // namespace uisnap
