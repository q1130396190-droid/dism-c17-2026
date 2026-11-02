// ============================================================
// attr_validation_probe.cpp -- Release 构建下属性诊断是否真的存在
//
// 为什么必须单独测这件事
// ----------------------
// R35-R37 反复遇到同一类问题：属性写错或写在不支持的控件上，DuiLib 完全
// 不报错、不打日志、界面上也看不出来。查源码发现根因在框架层：
//
//   DuiLib/Core/UIManager.cpp
//   #ifdef _DEBUG
//       ... 一整套 ReportAttributeValidationUnknown / 去重 / 计数 ...
//   #else
//       // Release build: no N1 code at all (constraint C4).
//       void ReportAttributeValidationUnknown(LPCTSTR) { return false; }
//   #endif
//
// 而 DuiLib/Core/UIControl.cpp 里调用它的地方也整段在 #ifdef _DEBUG 内。
// 也就是说：**Debug 能看到诊断，用户实际运行的 Release 什么都看不到。**
//
// 本轮把诊断代码移出 _DEBUG（默认仍是关闭，按需开启），本探针负责证明：
//   1) Release 下 SetAttributeValidationEnabled(true) 之后，
//      未知属性会被计数；
//   2) 同名属性只计一次（去重有效），否则好奇的日志会刷屏；
//   3) 关闭开关后不再增加（默认零开销的承诺成立）。
//
// 判据用 CPaintManagerUI::GetAttributeValidationUnknownCount() 的真实返回值，
// 不靠日志文本——日志可能被重定向或丢弃，计数器不会。
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

} // namespace

int main()
{
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

    const wchar_t* kCls = L"AttrValidationProbeWnd";
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
    pm.Init(hWnd, L"AttrValidationProbe");
    CControlFactory::GetInstance()->RegistControl(
        L"CNavOptionUI", static_cast<CreateClass>(CProbeNavUI::CreateControl));

    CDialogBuilder builder;
    CControlUI* pRoot = builder.Create(L"Main.xml", nullptr, nullptr, &pm);
    if (pRoot == nullptr) { ::printf("[FATAL] Main.xml 解析失败\n"); return 2; }
    pm.AttachDialog(pRoot);

    // ---- 基线：开关默认关闭 ----
    CPaintManagerUI::ResetAttributeValidationWarnedCount();
    const int nBase = CPaintManagerUI::GetAttributeValidationUnknownCount();
    Chk("默认关闭时计数器为 0（默认零开销）", nBase == 0,
        std::wstring(L"count=") + std::to_wstring(nBase));

    CControlUI* pBtn = pm.FindControl(L"menu_file");
    if (pBtn == nullptr) { ::printf("[FATAL] 找不到 menu_file\n"); return 1; }

    // 开关关闭时写入一个不存在的属性，计数器不应变化
    pBtn->SetAttribute(L"bkc0lor_probe", L"#FF000000");
    const int nWhileOff = CPaintManagerUI::GetAttributeValidationUnknownCount();
    Chk("开关关闭时未知属性不被计数", nWhileOff == nBase,
        std::wstring(L"before=") + std::to_wstring(nBase) +
        L" after=" + std::to_wstring(nWhileOff));

    // ---- 打开开关 ----
    pm.SetAttributeValidationEnabled(true);
    Chk("开关可被打开", pm.IsAttributeValidationEnabled(),
        L"IsAttributeValidationEnabled()==true");

    CPaintManagerUI::ResetAttributeValidationWarnedCount();

    // 1) 一个拼错的属性
    pBtn->SetAttribute(L"bkc0lor", L"#FF000000");
    const int nAfterFirst = CPaintManagerUI::GetAttributeValidationUnknownCount();
    Chk("未知属性被计数（Release 下诊断真的存在）", nAfterFirst >= 1,
        std::wstring(L"count=") + std::to_wstring(nAfterFirst));

    // 2) 同名再来两次，必须只计一次
    pBtn->SetAttribute(L"bkc0lor", L"#FF111111");
    pBtn->SetAttribute(L"bkc0lor", L"#FF222222");
    const int nAfterDup = CPaintManagerUI::GetAttributeValidationUnknownCount();
    Chk("同一属性名只告警一次（去重有效，不刷屏）", nAfterDup == nAfterFirst,
        std::wstring(L"after 3 writes=") + std::to_wstring(nAfterDup) +
        L" after first=" + std::to_wstring(nAfterFirst));

    // 3) 另一个未知属性应再计一次
    pBtn->SetAttribute(L"another_bogus_attr", L"1");
    const int nAfterSecond = CPaintManagerUI::GetAttributeValidationUnknownCount();
    Chk("不同属性名各自计数一次", nAfterSecond == nAfterDup + 1,
        std::wstring(L"count=") + std::to_wstring(nAfterSecond));

    // 4) 合法属性不应被计数
    const int nBeforeLegal = CPaintManagerUI::GetAttributeValidationUnknownCount();
    pBtn->SetAttribute(L"textcolor", L"#FF123456");
    pBtn->SetAttribute(L"text", L"probe-legal");
    const int nAfterLegal = CPaintManagerUI::GetAttributeValidationUnknownCount();
    Chk("合法属性不被计数（无误报）", nAfterLegal == nBeforeLegal,
        std::wstring(L"before=") + std::to_wstring(nBeforeLegal) +
        L" after=" + std::to_wstring(nAfterLegal));

    // ---- 关掉开关，确认恢复静默 ----
    pm.SetAttributeValidationEnabled(false);
    const int nBeforeOff = CPaintManagerUI::GetAttributeValidationUnknownCount();
    pBtn->SetAttribute(L"third_bogus_attr", L"2");
    const int nAfterOff = CPaintManagerUI::GetAttributeValidationUnknownCount();
    Chk("重新关闭后不再计数（可开关）", nAfterOff == nBeforeOff,
        std::wstring(L"before=") + std::to_wstring(nBeforeOff) +
        L" after=" + std::to_wstring(nAfterOff));

    ::DestroyWindow(hWnd);
    ::UnregisterClassW(kCls, hInst);
    Gdiplus::GdiplusShutdown(gtok);

    ::printf("\n结果：%d 通过 / %d 失败\n", g_nPass, g_nFail);
    ::fflush(stdout);
    return (g_nFail == 0) ? 0 : 1;
}
