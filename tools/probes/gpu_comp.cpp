// R259: GPU 合成可行性量测
//
// 要回答的问题
// ------------
// 当前架构每帧成本集中在 AlphaBlend 整屏合成（实测 2.52 ms）。
// 报告建议的 DirectComposition 能否真正突破这个上限？还是只是换了个地方开销？
//
// 本探针不做产品级实现，只做可行性量测：
//   1) D3D11 设备能否创建（Win11 上应当可以）
//   2) 创建 1920x1080 BGRA 纹理 + 清屏 + 提交的耗时
//   3) 与 GDI AlphaBlend 整屏成本对比
//
// 判定标准：
//   * 若 GPU 单帧合成显著低于 GDI（例如 < 1/2），则值得投入
//   * 若接近或更慢，则说明瓶颈不在合成 API，而在别处（布局/绘制调用数），
//     此时改动架构收益有限 —— 这个结论同样有价值
#include "StdAfx.h"
#include "UIlib.h"
#include <cstdio>
#include <chrono>

#include <d3d11.h>
#include <dxgi.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using clk = std::chrono::high_resolution_clock;
static double us_since(clk::time_point a) {
    return std::chrono::duration<double, std::micro>(clk::now() - a).count();
}

int main()
{
    const UINT W = 1920, H = 1080;
    int bad = 0;

    // ---------- 1) 创建 D3D11 设备 ----------
    ID3D11Device* pDevice = nullptr;
    ID3D11DeviceContext* pCtx = nullptr;
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_9_1;
    const D3D_FEATURE_LEVEL want[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1
    };

    auto t0 = clk::now();
    HRESULT hr = ::D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,      // BGRA is required for D2D/DComp interop
        want, _countof(want), D3D11_SDK_VERSION,
        &pDevice, &level, &pCtx);
    double tCreate = us_since(t0);

    if (FAILED(hr) || pDevice == nullptr) {
        printf("[probe] D3D11 hardware device creation failed hr=0x%08lX\n",
               (unsigned long)hr);
        printf("[R259] SKIP: no GPU path on this machine\n");
        return 0;
    }
    printf("[probe] D3D11 device created in %.0f us, feature level 0x%X\n",
           tCreate, (unsigned)level);

    // ---------- 2) 创建与屏幕同尺寸的纹理 ----------
    D3D11_TEXTURE2D_DESC td = { 0 };
    td.Width = W;
    td.Height = H;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    ID3D11Texture2D* pTex = nullptr;
    ID3D11RenderTargetView* pRTV = nullptr;
    t0 = clk::now();
    hr = pDevice->CreateTexture2D(&td, nullptr, &pTex);
    double tTex = us_since(t0);
    if (FAILED(hr) || pTex == nullptr) {
        printf("[probe] CreateTexture2D failed hr=0x%08lX\n", (unsigned long)hr);
        pCtx->Release(); pDevice->Release();
        return 2;
    }
    hr = pDevice->CreateRenderTargetView(pTex, nullptr, &pRTV);
    if (FAILED(hr) || pRTV == nullptr) {
        printf("[probe] CreateRenderTargetView failed\n");
        pTex->Release(); pCtx->Release(); pDevice->Release();
        return 2;
    }
    printf("[probe] %ux%u BGRA texture created in %.0f us\n", W, H, tTex);

    // ---------- 3) GPU 帧成本：清屏 + 提交 ----------
    const int N = 60;
    const float clear[4] = { 0.1f, 0.1f, 0.1f, 1.0f };

    // 预热
    for (int i = 0; i < 5; ++i) {
        pCtx->ClearRenderTargetView(pRTV, clear);
        pCtx->Flush();
    }

    t0 = clk::now();
    for (int i = 0; i < N; ++i) {
        pCtx->ClearRenderTargetView(pRTV, clear);
        pCtx->Flush();     // 强制提交，模拟真实呈现节奏
    }
    double tGpuClear = us_since(t0) / N;
    printf("[probe] GPU clear+flush  %.1f us/frame\n", tGpuClear);

    // ---------- 4) GDI 对照：整屏 AlphaBlend ----------
    HDC hScreen = ::GetDC(nullptr);
    HDC hDst = ::CreateCompatibleDC(hScreen);
    BYTE* pDst = nullptr;
    HBITMAP hDstBmp = DuiLib::CRenderEngine::CreateARGB32Bitmap(hDst, W, H, &pDst);
    ::SelectObject(hDst, hDstBmp);

    HDC hSrc = ::CreateCompatibleDC(hScreen);
    BYTE* pSrc = nullptr;
    HBITMAP hSrcBmp = DuiLib::CRenderEngine::CreateARGB32Bitmap(hSrc, W, H, &pSrc);
    ::SelectObject(hSrc, hSrcBmp);

    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    for (int i = 0; i < 3; ++i)
        ::AlphaBlend(hDst, 0, 0, W, H, hSrc, 0, 0, W, H, bf);

    t0 = clk::now();
    for (int i = 0; i < N; ++i)
        ::AlphaBlend(hDst, 0, 0, W, H, hSrc, 0, 0, W, H, bf);
    double tGdiBlend = us_since(t0) / N;
    printf("[probe] GDI AlphaBlend  %.1f us/frame\n", tGdiBlend);

    ::DeleteObject(hDstBmp);
    ::DeleteObject(hSrcBmp);
    ::DeleteDC(hDst);
    ::DeleteDC(hSrc);
    ::ReleaseDC(nullptr, hScreen);

    // ---------- 5) 结论 ----------
    printf("\n[probe] ratio GPU/GDI = %.2f\n", tGpuClear / (tGdiBlend > 0 ? tGdiBlend : 1));
    printf("[probe] 说明：GPU 侧只做了 clear+flush，不含实际内容绘制；\n");
    printf("[probe]      因此这是「合成路径下限」，不是完整替换后的帧成本。\n");

    pRTV->Release();
    pTex->Release();
    pCtx->Release();
    pDevice->Release();

    printf("\n[R259] %s (device ok, gpu_clear=%.1fus, gdi_blend=%.1fus)\n",
           bad == 0 ? "DONE" : "FAIL", tGpuClear, tGdiBlend);
    return bad;
}
