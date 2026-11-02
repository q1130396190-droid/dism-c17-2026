// ============================================================
// namehash_probe.cpp —— 验证 CPaintManagerUI::FindControl(名字) 是否失效
//
// 背景（本轮从 /DumpPages 得到的硬证据）：
//   page_cleanup 用 m_pm.FindControl(L"page_cleanup") 得到 nullptr，
//   但递归遍历整棵控件树能找到它（26 个控件）。
//   而所有页面的 Build() 都经 HostFindControl → m_pm.FindControl，
//   一旦它返回空，Build() 立即 early-return —— 这正是
//   "每个页面切进去都是空的"的根因。
//
// 判据：
//   同一批控件名，分别用两种方式查找并对比：
//     A) pm.FindControl(name)      —— 产品代码实际使用的方式（名字哈希）
//     B) 递归遍历比较 GetName()    —— 不依赖哈希
//   若 A 全空而 B 全中，即可确认名字哈希未建立/已失效。
// ============================================================

#include "UIlib.h"
#include "CaptionBtn.h"   // RegisterCaptionBtnControl(): minbtn/maxbtn/closebtn

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
    char sz[512] = {};
    ::WideCharToMultiByte(CP_UTF8, 0, sDetail.c_str(),
                          static_cast<int>(sDetail.size()),
                          sz, sizeof(sz) - 1, nullptr, nullptr);
    if (bOk) { ++g_nPass; ::printf("[PASS] %s | %s\n", pszName, sz); }
    else     { ++g_nFail; ::printf("[FAIL] %s | %s\n", pszName, sz); }
    ::fflush(stdout);
}

CControlUI* FindRecursive(CControlUI* pRoot, const wchar_t* pszName)
{
    if (pRoot == nullptr || pszName == nullptr) return nullptr;
    std::vector<CControlUI*> st;
    st.push_back(pRoot);
    while (!st.empty())
    {
        CControlUI* p = st.back();
        st.pop_back();
        if (p == nullptr) continue;
        if (p->GetName().Compare(pszName) == 0) return p;
        if (auto* pc = dynamic_cast<CContainerUI*>(p))
            for (int i = 0; i < pc->GetCount(); ++i)
                st.push_back(pc->GetItemAt(i));
    }
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
        size_t p = sRoot.find_last_of(L"\\/");
        if (p != std::wstring::npos) sRoot.resize(p);
    }

    HINSTANCE hInst = ::GetModuleHandleW(nullptr);
    CPaintManagerUI::SetInstance(hInst);
    CPaintManagerUI::SetResourcePath((sRoot + L"\\bin64\\skin\\Win8Metro").c_str());

    const wchar_t* kCls = L"NameHashProbeWnd";
    WNDCLASSEXW wc;
    ::ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ::DefWindowProcW;
    wc.hInstance = hInst;
    wc.lpszClassName = kCls;
    ::RegisterClassExW(&wc);
    HWND hWnd = ::CreateWindowExW(0, kCls, L"probe", WS_OVERLAPPEDWINDOW,
                                  0, 0, 1280, 900, nullptr, nullptr, hInst, nullptr);
    if (!hWnd) return 2;

    CPaintManagerUI pm;
    pm.Init(hWnd, L"NameHashProbe");
    CControlFactory::GetInstance()->RegistControl(
        L"CNavOptionUI", static_cast<CreateClass>(CProbeNavUI::CreateControl));
    // Main.xml 里的 minbtn/maxbtn/restorebtn/closebtn 是 <CaptionBtn> 控件。
    // 不注册它，这三个名字在探针里必然查不到——那是探针盲区，不是产品缺陷。
    RegisterCaptionBtnControl();

    CDialogBuilder builder;
    CControlUI* pRoot = builder.Create(L"Main.xml", nullptr, nullptr, &pm);
    if (!pRoot) { ::printf("[FATAL] parse failed\n"); return 2; }
    pm.AttachDialog(pRoot);

    ::printf("=== 查找方式对比（AttachDialog 之后、SetDPI 之前）===\n");
    ::printf("%-22s %-12s %-12s\n", "name", "pm.FindCtrl", "recursive");
    ::printf("%-22s %-12s %-12s\n", "----", "-----------", "---------");

    // 覆盖各层级的代表性控件
    static const wchar_t* kNames[] = {
        L"root", L"titlebar", L"app_title", L"minbtn", L"maxbtn", L"closebtn",
        L"menubar", L"menu_file", L"sidenav", L"nav_0", L"nav_1",
        L"content_area", L"page_cleanup", L"page_startup", L"bottombar",
        L"app_status", L"clean_list", L"clean_filter",
    };

    int nA = 0, nB = 0;
    for (const wchar_t* n : kNames)
    {
        CControlUI* pa = pm.FindControl(n);
        CControlUI* pb = FindRecursive(pRoot, n);
        if (pa) ++nA;
        if (pb) ++nB;
        ::wprintf(L"%-22s %-12s %-12s\n", n,
                  pa ? L"FOUND" : L"null",
                  pb ? L"FOUND" : L"null");
        ::fflush(stdout);
    }

    ::printf("\n汇总：pm.FindControl 命中 %d/%d，递归命中 %d/%d\n",
             nA, (int)(sizeof(kNames)/sizeof(kNames[0])),
             nB, (int)(sizeof(kNames)/sizeof(kNames[0])));
    ::fflush(stdout);

    Chk("pm.FindControl 能按名找到控件", nA > 0,
        L"命中 " + std::to_wstring(nA) + L"/18（0 表示名字哈希未建立）");
    Chk("递归查找能按名找到控件", nB > 0,
        L"命中 " + std::to_wstring(nB) + L"/18");

    // SetDPI 之后再看一次：确认是否是 SetDPI 清掉了哈希
    pm.SetDPI(96);
    int nA2 = 0;
    for (const wchar_t* n : kNames)
        if (pm.FindControl(n) != nullptr) ++nA2;
    ::printf("SetDPI 之后 pm.FindControl 命中 %d/18\n", nA2);
    ::fflush(stdout);

    Chk("SetDPI 不破坏名字哈希", nA2 == nA,
        L"SetDPI 前=" + std::to_wstring(nA) + L" 后=" + std::to_wstring(nA2));

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
