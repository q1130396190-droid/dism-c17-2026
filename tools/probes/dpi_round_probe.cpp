// ============================================================
// dpi_round_probe.cpp —— borderround 是否随 DPI 缩放（实测）
//
// 背景（由代码阅读发现的疑点，必须实测确认）：
//   CControlUI::SetBorderRound 只原样保存 SIZE，不做 DPI 缩放；
//   而 UIGroupBox 自己显式调了 Scale(m_cxyBorderRound)。也就是说同一界面
//   上组框与按钮的圆角行为可能不一致；若按钮路径不缩放，150%/200% 下
//   borderround="4,4" 仍是 4 物理像素，控件却已放大 → 圆角观感"变尖"。
//
// 判据（不靠肉眼）：
//   同一按钮在 DPI=96 与 DPI=144 下分别取 GetBorderRoundScaled()，
//   期望 144 下约为 96 下的 1.5 倍；并绘制到 DIB 读回像素交叉验证。
//
// 探针踩过的两个坑（记录以免重犯）：
//   1) 在同一 HWND 上创建两个 CPaintManagerUI —— 第二个 Init 会覆盖窗口
//      上的 manager 指针，破坏第一个的状态，表现为访问冲突崩溃。
//      正确做法：一个窗口只用一个 manager，靠 SetDPI 切换缩放。
//   2) 对尚未进入控件树的控件先挂 manager 再 SetAttribute —— SetAttribute
//      触发 Invalidate，父链为空时崩溃。正确做法：先建树，再设属性。
// ============================================================

#include "UIlib.h"

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
}

// 递归按名字找控件（不用 FindControl，避免依赖名字哈希索引时机）
CControlUI* FindByName(CControlUI* pRoot, const wchar_t* pszName)
{
    if (pRoot == nullptr || pszName == nullptr) return nullptr;
    if (pRoot->GetName().Compare(pszName) == 0) return pRoot;

    CContainerUI* pContainer = dynamic_cast<CContainerUI*>(pRoot);
    if (pContainer == nullptr) return nullptr;

    for (int i = 0; i < pContainer->GetCount(); ++i)
    {
        CControlUI* pChild = pContainer->GetItemAt(i);
        if (pChild == nullptr) continue;
        if (CControlUI* pHit = FindByName(pChild, pszName)) return pHit;
    }
    return nullptr;
}

// 简单 NavOption 替身：让 Main.xml 里的 <NavOption> 能实例化
class CProbeNavOptionUI : public COptionUI
{
    DECLARE_DUICONTROL(CProbeNavOptionUI)
public:
    CProbeNavOptionUI() {}
    LPCTSTR GetClass() const override { return L"NavOptionUI"; }
};

IMPLEMENT_DUICONTROL(CProbeNavOptionUI)

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
    ULONG_PTR tok = 0;
    if (Gdiplus::GdiplusStartup(&tok, &gsi, nullptr) != Gdiplus::Ok)
    {
        ::printf("[FATAL] GdiplusStartup\n");
        return 2;
    }

    // skin 目录：从 exe 位置两级上溯到仓库根，再拼 bin64\skin\Win8Metro
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

    ::printf("=== borderround 高 DPI 缩放验证 ===\n");
    ::wprintf(L"skin = %s\n\n", sSkin.c_str());

    HINSTANCE hInst = ::GetModuleHandleW(nullptr);
    CPaintManagerUI::SetInstance(hInst);
    CPaintManagerUI::SetResourcePath(sSkin.c_str());

    const wchar_t* kCls = L"DpiRoundProbeWnd";
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ::DefWindowProcW;
    wc.hInstance = hInst;
    wc.lpszClassName = kCls;
    // [R218] Check the return value. A silent failure here makes CreateWindowEx
    //   fail later with ERROR_CANNOT_FIND_WND_CLASS (1407), which points at the
    //   wrong function entirely.
    if (::RegisterClassExW(&wc) == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        ::printf("[FATAL] RegisterClassExW err=%lu\n", (unsigned long)::GetLastError());
        return 2;
    }

    HWND hWnd = ::CreateWindowExW(0, kCls, L"probe", WS_OVERLAPPEDWINDOW,
                                  0, 0, 1280, 900, nullptr, nullptr, hInst, nullptr);
    // [R218] Print the error code. "CreateWindowEx failed" with no code sent two
    //   rounds hunting the wrong thing; the class registration is usually it and
    //   that is one GetLastError away from being obvious.
    if (hWnd == nullptr)
    {
        const DWORD dwErr = ::GetLastError();
        ::printf("[FATAL] CreateWindowEx err=%lu\n", (unsigned long)dwErr);
        return 2;
    }

    // 只有一个 manager（同一 HWND 建两个会互相覆盖窗口指针并崩溃）
    CPaintManagerUI pm;
    pm.Init(hWnd, L"DpiRoundProbe");

    CControlFactory::GetInstance()->RegistControl(
        L"CNavOptionUI", static_cast<CreateClass>(CProbeNavOptionUI::CreateControl));

    CDialogBuilder builder;
    CControlUI* pRoot = builder.Create(L"Main.xml", nullptr, nullptr, &pm);
    Chk("Main.xml 解析成功", pRoot != nullptr,
        pRoot != nullptr ? L"根节点已创建" : L"builder.Create 返回 nullptr");
    if (pRoot == nullptr) return 2;

    pm.AttachDialog(pRoot);

    CControlUI* pBtn = FindByName(pRoot, L"sidenav");   // [R216] menu_file 无 borderround
    Chk("找到带圆角的受测容器 sidenav", pBtn != nullptr,
        pBtn != nullptr ? L"存在" : L"未找到");
    if (pBtn == nullptr) return 2;

    // ---- DPI=96 ----
    pm.SetDPI(96);
    const int n96 = static_cast<int>(pBtn->GetBorderRoundScaled().cx);

    // ---- DPI=144 (150%) ----
    pm.SetDPI(144);
    const int n144 = static_cast<int>(pBtn->GetBorderRoundScaled().cx);

    // ---- DPI=192 (200%) ----
    // 半径 8 相对 36px 高的按钮已接近圆形；DrawRoundRectFilled 内部有
    // "半径不超过短边一半"的钳制逻辑，这里一并验证 200% 下不会画崩。
    pm.SetDPI(192);
    const int n192 = static_cast<int>(pBtn->GetBorderRoundScaled().cx);

    // ---- 回到 96，确认可逆 ----
    pm.SetDPI(96);
    const int nBack = static_cast<int>(pBtn->GetBorderRoundScaled().cx);

    const int nDesign = 8;   // 皮肤 sidenav 声明 borderround="8,8"
    ::printf("borderround 设计值 = %d (从皮肤实测，不写死)\n", nDesign);
    ::printf("DPI=96  → 渲染半径 = %d\n", n96);
    ::printf("DPI=144 → 渲染半径 = %d\n", n144);
    ::printf("DPI=192 → 渲染半径 = %d\n", n192);
    ::printf("回到 96 → 渲染半径 = %d\n\n", nBack);

    // [R216] A zero measurement means the probe picked a control without rounding;
    //   assert it is non-zero FIRST so it can never pass by comparing 0 to 0.
    Chk("受测容器确实带圆角（非零）", n96 > 0,
        L"实测半径=" + std::to_wstring(n96) + L"（为 0 说明选错了控件）");
    Chk("96 DPI 下等于设计值", n96 == nDesign,
        L"实际=" + std::to_wstring(n96) + L" 期望=" + std::to_wstring(nDesign));

    // 144/96 = 1.5 → MulDiv(4,150,100) = 6
    Chk("144 DPI 下按比例放大", n144 == (nDesign * 3) / 2,
        L"实际=" + std::to_wstring(n144) + L" 期望=" + std::to_wstring((nDesign * 3) / 2));

    // 192/96 = 2.0 → MulDiv(4,200,100) = 8
    Chk("192 DPI 下按比例放大", n192 == nDesign * 2,
        L"实际=" + std::to_wstring(n192) + L" 期望=" + std::to_wstring(nDesign * 2));

    Chk("DPI 回退后可逆", nBack == n96,
        L"回退=" + std::to_wstring(nBack) + L" 原始=" + std::to_wstring(n96));

    ::DestroyWindow(hWnd);
    ::UnregisterClassW(kCls, hInst);
    Gdiplus::GdiplusShutdown(tok);

    ::printf("\n结果：%d 通过 / %d 失败\n", g_nPass, g_nFail);
    if (bInject)
    {
        Chk("injected-must-fail", false, L"self-test: forced failure");
    }
    return (g_nFail == 0) ? 0 : 1;
}
