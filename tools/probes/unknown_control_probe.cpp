// ============================================================
// unknown_control_probe.cpp -- 未知控件类在 Release 下是否安全跳过
//
// 缺陷背景（R49 扫描 #ifdef _DEBUG 时发现）
// ----------------------------------------
// DuiLib/Core/UIDlgBuilder.cpp 里原本是：
//
//     if( pControl == nullptr ) {
//     #ifdef _DEBUG
//         DUITRACE("unknown control: %s", pstrClass);
//     #else
//         continue;
//     #endif
//     }
//
// 两个问题，第二个更严重：
//
//   1) Release 下静默跳过，一个控件类名写错就整棵子树消失，没有任何日志。
//      这与 R43 修的「属性诊断被编译掉」是同一类缺陷。
//
//   2) Debug 下只有 trace、**没有 continue**，于是 pControl(==nullptr) 继续
//      被当作父控件传给 _Parse(children) 和 attach 步骤。也就是说两个构建
//      的行为不一致，而 Debug 那条是危险的。
//
// 修复后：两个构建都记日志、都 continue。
//
// 本探针验证
// ----------
//   1) 含未知控件类的皮肤不会让解析崩溃，其余控件照常创建
//   2) 未知类所在位置的兄弟控件仍然存在（证明 continue 生效而不是中断整棵树）
//   3) 未知类本身没有被创建
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
    char szd[512] = {};
    ::WideCharToMultiByte(CP_UTF8, 0, sDetail.c_str(), static_cast<int>(sDetail.size()),
                          szd, sizeof(szd) - 1, nullptr, nullptr);
    if (bOk) { ++g_nPass; ::printf("[PASS] %s | %s\n", pszName, szd); }
    else     { ++g_nFail; ::printf("[FAIL] %s | %s\n", pszName, szd); }
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

// 含一个不存在的控件类 <NoSuchControlUI>
//
// NOTE: must be wide. CDialogBuilder::Create takes STRINGorID, which does not
// accept a narrow const char* -- that was a C2665 in the first version.
const wchar_t* kSkinWithUnknown =
    L"<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    L"<Window>"
    L"<VerticalLayout name=\"probe_root\">"
    L"  <Text name=\"before_unknown\" text=\"BEFORE\"/>"
    L"  <NoSuchControlUI name=\"ghost_control\"/>"
    L"  <Text name=\"after_unknown\" text=\"AFTER\"/>"
    L"</VerticalLayout>"
    L"</Window>";

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

    HINSTANCE hInst = ::GetModuleHandleW(nullptr);
    CPaintManagerUI::SetInstance(hInst);

    const wchar_t* kCls = L"UnknownControlProbeWnd";
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ::DefWindowProcW;
    wc.hInstance = hInst;
    wc.lpszClassName = kCls;
    ::RegisterClassExW(&wc);

    HWND hWnd = ::CreateWindowExW(0, kCls, L"probe", WS_OVERLAPPEDWINDOW,
                                  0, 0, 800, 600, nullptr, nullptr, hInst, nullptr);
    if (hWnd == nullptr) { ::printf("[FATAL] CreateWindowEx\n"); return 2; }

    CPaintManagerUI pm;
    pm.Init(hWnd, L"UnknownControlProbe");

    // 从内存解析，避免依赖皮肤文件
    CDialogBuilder builder;
    CControlUI* pRoot = builder.Create(STRINGorID(kSkinWithUnknown), L"xml", nullptr, &pm);

    Chk("含未知控件类的皮肤仍能解析出根节点（不崩溃）", pRoot != nullptr,
        pRoot ? L"根节点已创建" : L"解析返回空");

    if (pRoot != nullptr)
    {
        CControlUI* pBefore = FindByName(pRoot, L"before_unknown");
        CControlUI* pAfter  = FindByName(pRoot, L"after_unknown");
        CControlUI* pGhost  = FindByName(pRoot, L"ghost_control");

        Chk("未知类之前的兄弟控件正常创建", pBefore != nullptr,
            pBefore ? L"before_unknown 存在" : L"缺失");
        Chk("未知类之后的兄弟控件也正常创建（continue 生效，未中断整棵树）",
            pAfter != nullptr,
            pAfter ? L"after_unknown 存在" : L"缺失");
        Chk("未知控件本身没有被创建", pGhost == nullptr,
            pGhost ? L"意外创建了" : L"未创建（正确）");
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
