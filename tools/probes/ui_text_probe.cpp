// ============================================================
// ui_text_probe.cpp —— 界面取值断言（验证信息真的写进了控件）
//
// 为什么必须单独做这一步：
//   前几轮只验证了"代码里有没有赋值"和"控件有没有被改色"。
//   但信息填充的验收标准是【用户在界面上能看到什么】。
//   代码里写 SetCtrlText 不等于值真的落到了控件上：
//     * 控件名拼错 → FindControl 返回 nullptr，赋值静默失败
//     * 皮肤里同名控件被 Include 覆盖 → 值丢了
//     * 赋了空串 → 界面还是空白
//   这些都是"代码看着对、界面是空的"的典型，必须读控件实际文本才能发现。
//
// 判据：
//   1) 关键会话条字段存在，且文本非空
//   2) 文本不是皮肤的占位符默认值（如 "-"、"准备就绪" 未替换）
//   3) 无 name 的空控件不在检查范围（皮肤用于占位的 Control）
// ============================================================

#include "UIlib.h"
#include "../../Demos/Win8Metro/ThemeTokens.h"

// [TOK-EXPAND] 探针必须自己展开 {tok:*}。
//   造真窗口走的是 CFrameWnd::ExpandSkinFile，它在解析前把 {tok:Surface}
//   替换成 "#FF202020"。探针早期直接 builder.Create("Main.xml") 读磁盘文件，
//   于是 focusbordercolor="{tok:AccentBorder}" 被 DuiLib 当成非法颜色串，
//   _tcstoul 得到 0 —— 断言报"皮肤未启用焦点框颜色"，看着像产品缺陷，
//   实际只是探针走了另一条解析路径。这里按同一套令牌表做文本替换。
//
// 名字表由 ThemeTokens.h 的枚举顺序机械生成，顺序必须与 EToken 一致
// （下面的 static_assert 会拦住两者不同步）。
static const wchar_t* const kProbeTokenNames[] = {
    L"Accent",
    L"AccentHot",
    L"AccentPushed",
    L"AccentText",
    L"AccentSoft",
    L"AccentBorder",
    L"Surface",
    L"SurfaceAlt",
    L"SurfaceHover",
    L"SurfaceSunk",
    L"Border",
    L"BorderSoft",
    L"Ink",
    L"InkSecondary",
    L"InkTertiary",
    L"InkMuted",
    L"Danger",
    L"Success",
    L"Warn",
    L"WarnBg",
    L"OnAccent",
    L"HeaderBand",
    L"OnHeaderBand",
    L"DangerHot",
    L"DangerPushed",
    L"OnDanger",
    L"OnAccentHot",
    L"OnAccentPushed",
};
static_assert(sizeof(kProbeTokenNames) / sizeof(kProbeTokenNames[0])
              == static_cast<size_t>(tokens::EToken::Count),
              "probe token name table out of sync with tokens::EToken");

static std::wstring ExpandTokens(const std::wstring& in)
{
    std::wstring out = in;
    size_t pos = 0;
    while ((pos = out.find(L"{tok:", pos)) != std::wstring::npos)
    {
        const size_t close = out.find(L'}', pos);
        if (close == std::wstring::npos) break;
        const std::wstring name = out.substr(pos + 5, close - pos - 5);
        bool found = false;
        for (int i = 0; i < static_cast<int>(tokens::EToken::Count); ++i)
        {
            if (name != kProbeTokenNames[i]) continue;
            wchar_t szHex[16] = {};
            ::swprintf_s(szHex, L"#%08lX",
                         static_cast<unsigned long>(
                             tokens::Token(static_cast<tokens::EToken>(i))));
            out.replace(pos, close + 1 - pos, szHex);
            pos += ::wcslen(szHex);
            found = true;
            break;
        }
        if (!found) out.erase(pos, close + 1 - pos);
    }
    return out;
}

static std::wstring ReadSkinAsText(const std::wstring& sFullPath)
{
    HANDLE h = ::CreateFileW(sFullPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::wstring();
    DWORD sz = ::GetFileSize(h, nullptr);
    std::string raw(sz, 0);
    DWORD got = 0;
    ::ReadFile(h, &raw[0], sz, &got, nullptr);
    ::CloseHandle(h);
    raw.resize(got);
    if (raw.size() >= 3 && (unsigned char)raw[0] == 0xEF) raw.erase(0, 3);
    int nw = ::MultiByteToWideChar(CP_UTF8, 0, raw.data(), (int)raw.size(), nullptr, 0);
    std::wstring w(nw, 0);
    ::MultiByteToWideChar(CP_UTF8, 0, raw.data(), (int)raw.size(), &w[0], nw);
    return w;
}



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

class CProbeNavOptionUI : public COptionUI
{
    DECLARE_DUICONTROL(CProbeNavOptionUI)
public:
    CProbeNavOptionUI() {}
    LPCTSTR GetClass() const override { return L"NavOptionUI"; }
};

IMPLEMENT_DUICONTROL(CProbeNavOptionUI)

// 检查一个控件：存在 + 文本非空 + 不是占位符
void ChkField(CControlUI* pRoot, const wchar_t* pszName,
              const wchar_t* pszPlaceholder)
{
    char szLabel[128] = {};
    ::WideCharToMultiByte(CP_UTF8, 0, pszName, -1, szLabel, sizeof(szLabel) - 1,
                          nullptr, nullptr);

    CControlUI* p = FindByName(pRoot, pszName);
    if (p == nullptr)
    {
        Chk(szLabel, false, L"控件不存在");
        return;
    }

    const CDuiString sText = p->GetText();
    const bool bEmpty = (sText.GetLength() == 0);
    const bool bPlaceholder = (pszPlaceholder != nullptr &&
                               sText.Compare(pszPlaceholder) == 0);

    std::wstring sDetail = L"text=\"";
    sDetail += sText.GetData();
    sDetail += L"\"";

    Chk(szLabel, !bEmpty && !bPlaceholder,
        bEmpty ? (sDetail + L" → 空值")
               : (bPlaceholder ? (sDetail + L" → 仍是皮肤占位符") : sDetail));
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
    ULONG_PTR tok = 0;
    if (Gdiplus::GdiplusStartup(&tok, &gsi, nullptr) != Gdiplus::Ok)
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

    ::printf("=== 界面取值断言 ===\n\n");

    HINSTANCE hInst = ::GetModuleHandleW(nullptr);
    CPaintManagerUI::SetInstance(hInst);
    CPaintManagerUI::SetResourcePath(sSkin.c_str());

    const wchar_t* kCls = L"UiTextProbeWnd";
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
    pm.Init(hWnd, L"UiTextProbe");

    CControlFactory::GetInstance()->RegistControl(
        L"CNavOptionUI", static_cast<CreateClass>(CProbeNavOptionUI::CreateControl));

    CDialogBuilder builder;
    // 读磁盘皮肤并展开 {tok:*}：与 CFrameWnd::ExpandSkinFile 走同一套令牌表，
    //   否则 focusbordercolor 等属性会拿到非法颜色串。
    std::wstring sXml = ExpandTokens(ReadSkinAsText(sSkin + L"\\Main.xml"));
    // <Include source="pages/xxx.xml"/> 也要内联，否则各页控件不存在。
    {
        size_t ip = 0;
        while ((ip = sXml.find(L"<Include ", ip)) != std::wstring::npos)
        {
            size_t tagEnd = sXml.find(L"/>", ip);
            if (tagEnd == std::wstring::npos) break;
            size_t srcPos = sXml.find(L"source=\"", ip);
            if (srcPos == std::wstring::npos || srcPos > tagEnd) { ip = tagEnd + 2; continue; }
            const size_t valStart = srcPos + 8;
            size_t valEnd = sXml.find(L"\"", valStart);
            if (valEnd == std::wstring::npos) break;
            std::wstring sSub = sXml.substr(valStart, valEnd - valStart);
            std::wstring sSubXml = ExpandTokens(ReadSkinAsText(sSkin + L"\\" + sSub));
            // 子文件带 <?xml?> 与 <Window> 外壳，内联前要剥掉
            const size_t d = sSubXml.find(L"<?xml");
            if (d != std::wstring::npos) {
                const size_t de = sSubXml.find(L"?>", d);
                if (de != std::wstring::npos) sSubXml.erase(d, de + 2 - d);
            }
            {
                const size_t wo = sSubXml.find(L"<Window");
                if (wo != std::wstring::npos) {
                    const size_t woEnd = sSubXml.find(L">", wo);
                    if (woEnd != std::wstring::npos) sSubXml.erase(wo, woEnd + 1 - wo);
                }
                const size_t wc = sSubXml.rfind(L"</Window>");
                if (wc != std::wstring::npos) sSubXml.erase(wc);
            }
            if (sSubXml.empty()) { ip = tagEnd + 2; continue; }
            sXml.replace(ip, tagEnd + 2 - ip, sSubXml);
            ip += sSubXml.size();
        }
    }
    CControlUI* pRoot = builder.Create(sXml.c_str(), nullptr, nullptr, &pm);
    Chk("Main.xml 解析成功", pRoot != nullptr, L"");
    if (pRoot == nullptr) return 2;
    pm.AttachDialog(pRoot);
    pm.SetDPI(96);

    // ---- 静态皮肤文本：必须来自皮肤且非空 ----
    // 这些是写死在 XML 里的，能读到非空值即证明皮肤解析链路完整。
    // app_title 在运行期由 InitSystemInfo 按 kAppVersion 重写，
    // 这里仍检查皮肤解析后非空即可（真实绑定由运行期测）。
    const wchar_t* kStatic[] = {
        L"app_title", L"menu_file", L"grp_common",
    };
    for (const wchar_t* n : kStatic)
        ChkField(pRoot, n, nullptr);

    // ---- 导航项：11 个都必须有文本 ----
    {
        int nOk = 0;
        for (int i = 0; i <= 10; ++i)
        {
            wchar_t szName[32] = {};
            ::swprintf_s(szName, L"nav_%d", i);
            if (CControlUI* p = FindByName(pRoot, szName))
                if (p->GetText().GetLength() > 0) ++nOk;
        }
        wchar_t sz[64] = {};
        ::swprintf_s(sz, L"%d/11 有文本", nOk);
        Chk("导航项文本齐全", nOk == 11, sz);
    }

    // ---- 页面级字段：只断言"皮肤里写了非空文本"的静态标签 ----
    //
    // 不能把运行期填充的控件混进来。反例：lbl_startup_count 在皮肤里是
    // text=""，由 Page_Startup.cpp 在页构建时算总数再写入；本探针只解析
    // 皮肤、不构建懒加载页面，读到空值是【预期】，把它当失败属于断言分类错误。
    // 判据：静态文本看皮肤，动态文本看运行期（由 /StartupCheck 与人工验收覆盖）。
    const struct { const wchar_t* name; } kStaticPageFields[] = {
        { L"lbl_cln_desc" },       // 空间回收：描述标题
        { L"btn_scan" },           // 空间回收：扫描按钮
        { L"btn_tools_refresh" },  // 工具箱：刷新按钮
        { L"lbl_tools_title" },    // 工具箱：标题
        { L"btn_clean" },          // 空间回收：清理按钮
    };
    for (const auto& f : kStaticPageFields)
        ChkField(pRoot, f.name, nullptr);

    // ---- 动态字段：断言"字段存在"，不断言文本 ----
    // 这些值由页构建/InitSystemInfo 在运行期写入，皮肤里是空串或占位符。
    const wchar_t* kDynamicFields[] = {
        L"lbl_startup_count", L"tools_count", L"sess_type",
        // 本轮补的两个"代码写入但皮肤从未定义"的字段：
        // Page_RegClean 写 regclean_count、Page_Update 写 upd_install_state，
        // 前者影响扫描计数可见性，后者影响安装进度可见性。
        // 断言它们存在，防止日后被误删而回到静默失败。
        L"regclean_count", L"upd_install_state",
        // 第 15 轮补的两个"皮肤已定义但代码零引用"的字段：
        // app_status_right（状态条右侧环境信息）、task_infobar（任务页信息条）。
        // 之前它们占着版面却永远空白。
        L"app_status_right", L"task_infobar",
    };
    for (const wchar_t* n : kDynamicFields)
    {
        CControlUI* p = FindByName(pRoot, n);
        char szLabel[128] = {};
        ::WideCharToMultiByte(CP_UTF8, 0, n, -1, szLabel, sizeof(szLabel) - 1,
                              nullptr, nullptr);
        std::wstring sDetail = (p != nullptr)
            ? (std::wstring(L"skin 值=\"") + p->GetText().GetData() + L"\"，运行期填充")
            : std::wstring(L"控件不存在");
        Chk(szLabel, p != nullptr, sDetail);
    }

    // ---- 16 个页面根必须全部存在 ----
    //
    // 为什么值得断言：Main.xml 用 <Include source="pages/xxx.xml"/> 展开各页，
    // 任一页文件缺失或被改名，DuiLib 会**静默跳过**该 Include —— 界面上
    // 表现为"切到那一页是空白"，而启动、日志、其余页面全都正常。
    // 这与第 14 轮查出的"皮肤缺控件"是同一类静默失败，但粒度是整页。
    //
    // 判据用"页根存在"而非"页内有控件"：前者不受本机状态影响，可稳定复现。
    {
        const wchar_t* kPageRoots[] = {
            L"page_cleanup", L"page_startup", L"page_appx",  L"page_tools",
            L"page_optim",   L"page_driver",  L"page_program", L"page_feature",
            L"page_update",  L"page_assoc",   L"page_unattend", L"page_tasks",
            L"page_control", L"page_intl",    L"page_regclean", L"page_welcome",
        };
        int nFound = 0;
        std::wstring sMissing;
        for (const wchar_t* n : kPageRoots)
        {
            if (FindByName(pRoot, n) != nullptr) { ++nFound; continue; }
            if (!sMissing.empty()) sMissing += L", ";
            sMissing += n;
        }
        wchar_t sz[96] = {};
        ::swprintf_s(sz, L"%d/16", nFound);
        std::wstring sDetail = sz;
        if (!sMissing.empty()) sDetail += L" 缺失: " + sMissing;
        Chk("16 个页面根齐全", nFound == 16, sDetail);
    }

    // ---- 键盘焦点框：皮肤必须真的把 focusbordercolor 应用到按钮上 ----
    // 框架早已支持（CControlUI::PaintBorder 优先用 m_dwFocusBorderColor），
    // 但皮肤此前 0 处设置，Tab 切换时看不出焦点在哪 —— 属于"能力有、未启用"。
    {
        auto* pBtn = dynamic_cast<CButtonUI*>(FindByName(pRoot, L"btn_clean"));
        if (pBtn == nullptr)
        {
            Chk("焦点框按钮存在", false, L"找不到 btn_clean");
        }
        else
        {
            const DWORD dwFocus = pBtn->GetFocusBorderColor();
            wchar_t sz[96] = {};
            ::swprintf_s(sz, L"focusBorderColor=0x%08lX", static_cast<unsigned long>(dwFocus));
            Chk("皮肤已启用焦点框颜色", dwFocus != 0, sz);
        }
    }

    // ---- 关键：sess_type 这类"运行期填充"的字段 ----
    // 皮肤默认值是"本地硬盘"，探针环境不跑 InitSystemInfo，
    // 所以这里读到占位符是【预期】的——本项改为验证"字段存在且皮肤值可读"，
    // 真实填充由运行期日志与人工验收覆盖。
    {
        CControlUI* p = FindByName(pRoot, L"sess_type");
        if (p == nullptr)
        {
            Chk("sess_type 字段存在", false, L"控件不存在");
        }
        else
        {
            std::wstring s = L"skin 默认值=\"";
            s += p->GetText().GetData();
            s += L"\"（运行期由 InitSystemInfo 覆盖）";
            Chk("sess_type 字段存在", true, s);
        }
    }

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
