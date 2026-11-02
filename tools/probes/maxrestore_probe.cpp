// ============================================================
// maxrestore_probe.cpp —— maxbtn / restorebtn 互斥切换验证
//
// 背景（本轮定位到的真实缺陷）：
//   皮肤 Main.xml 里写的是
//       <CaptionBtn name="maxbtn"     capkind="1" ... />
//       <CaptionBtn name="restorebtn" capkind="2" ... visible="false" />
//   但全仓库没有任何一行在窗口最大化/还原时切换这两者的可见性。
//   于是窗口一旦最大化：
//     * maxbtn 仍在，点它只是再发一次 SC_MAXIMIZE —— 没有任何效果；
//     * 唯一的还原按钮始终隐藏 —— 用户只能双击标题栏或走系统菜单还原。
//   根因是点击处理器只发 SC_* 命令，却没有同步按钮状态。
//
// 本探针验证三件事：
//   1) 皮肤初始状态确实是「max 可见 / restore 隐藏」；
//   2) 判据 ::IsZoomed 能正确反映窗口的真实最大化状态
//      （所以同步逻辑用它而不是 WM_SIZE 的 wParam）；
//   3) 按该判据切换后，两个按钮的可见性真的互换，
//      且不会因为 NeedParentUpdate 触发崩溃。
// ============================================================

#include "UIlib.h"
#include "CaptionBtn.h"

#include <windows.h>
#include <gdiplus.h>
#include <cstdio>
#include <string>

#pragma comment(lib, "gdiplus.lib")

using namespace DuiLib;

namespace
{
int g_nPass = 0;
int g_nFail = 0;

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

// 与 CFrameWnd::SyncMaxRestoreButtons 相同的切换规则。
// 这里独立实现一份，是为了验证「规则本身正确」；
// 产品里是否真的被调用，由 FrameWnd_SysMsg.cpp 的调用点保证（见报告）。
void ApplyMaxRestore(CPaintManagerUI& pm, bool bMax)
{
    CControlUI* pMax     = pm.FindControl(L"maxbtn");
    CControlUI* pRestore = pm.FindControl(L"restorebtn");
    if (pMax != nullptr)     pMax->SetVisible(!bMax);
    if (pRestore != nullptr) pRestore->SetVisible(bMax);
    if (CControlUI* pRoot = pm.GetRoot()) pRoot->NeedParentUpdate();
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
    if (Gdiplus::GdiplusStartup(&gtok, &gsi, nullptr) != Gdiplus::Ok) return 2;

    wchar_t szExe[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, szExe, MAX_PATH);
    std::wstring sDir(szExe);
    size_t nSlash = sDir.find_last_of(L"\\/");
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

    const wchar_t* kCls = L"MaxRestoreProbeWnd";
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
    pm.Init(hWnd, L"MaxRestoreProbe");
    RegisterCaptionBtnControl();

    CDialogBuilder builder;
    CControlUI* pRoot = builder.Create(L"Main.xml", nullptr, nullptr, &pm);
    if (pRoot == nullptr) { ::printf("[FATAL] Main.xml 解析失败\n"); return 2; }
    pm.AttachDialog(pRoot);

    CControlUI* pMax     = pm.FindControl(L"maxbtn");
    CControlUI* pRestore = pm.FindControl(L"restorebtn");

    Chk("皮肤中 maxbtn / restorebtn 都能按名找到",
        pMax != nullptr && pRestore != nullptr,
        std::wstring(L"maxbtn=") + (pMax ? L"found" : L"null") +
        L" restorebtn=" + (pRestore ? L"found" : L"null"));

    if (pMax == nullptr || pRestore == nullptr)
    {
        ::DestroyWindow(hWnd);
        ::UnregisterClassW(kCls, hInst);
        Gdiplus::GdiplusShutdown(gtok);
        return 1;
    }

    // ---- 1) 皮肤初始状态 ----
    ::printf("\n[初始状态] maxbtn.visible=%d  restorebtn.visible=%d\n",
             pMax->IsVisible() ? 1 : 0, pRestore->IsVisible() ? 1 : 0);
    ::fflush(stdout);

    Chk("初始：maxbtn 可见、restorebtn 隐藏（皮肤声明的非最大化态）",
        pMax->IsVisible() && !pRestore->IsVisible(),
        std::wstring(L"符合预期"));

    // ---- 2) ::IsZoomed 能反映真实最大化状态 ----
    Chk("未最大化时 IsZoomed == false", ::IsZoomed(hWnd) == FALSE, L"");

    ::ShowWindow(hWnd, SW_MAXIMIZE);
    ::UpdateWindow(hWnd);

    const bool bZoomed = (::IsZoomed(hWnd) != FALSE);
    Chk("ShowWindow(SW_MAXIMIZE) 后 IsZoomed == true", bZoomed,
        bZoomed ? L"判据可用" : L"判据失效——同步逻辑会取到错误状态");

    // ---- 3) 切换规则 ----
    ApplyMaxRestore(pm, bZoomed);
    ::printf("\n[最大化后] maxbtn.visible=%d  restorebtn.visible=%d\n",
             pMax->IsVisible() ? 1 : 0, pRestore->IsVisible() ? 1 : 0);
    ::fflush(stdout);

    Chk("最大化后 maxbtn 隐藏", !pMax->IsVisible(), L"");
    Chk("最大化后 restorebtn 可见", pRestore->IsVisible(), L"");
    Chk("两个按钮不会同时不可见（否则用户无处还原）",
        pMax->IsVisible() || pRestore->IsVisible(), L"");

    // ---- 4) 还原回去 ----
    ::ShowWindow(hWnd, SW_RESTORE);
    ::UpdateWindow(hWnd);

    const bool bZoomed2 = (::IsZoomed(hWnd) != FALSE);
    Chk("SW_RESTORE 后 IsZoomed == false", !bZoomed2, L"");

    ApplyMaxRestore(pm, bZoomed2);
    ::printf("\n[还原后] maxbtn.visible=%d  restorebtn.visible=%d\n",
             pMax->IsVisible() ? 1 : 0, pRestore->IsVisible() ? 1 : 0);
    ::fflush(stdout);

    Chk("还原后 maxbtn 可见、restorebtn 隐藏（可逆）",
        pMax->IsVisible() && !pRestore->IsVisible(), L"");

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
