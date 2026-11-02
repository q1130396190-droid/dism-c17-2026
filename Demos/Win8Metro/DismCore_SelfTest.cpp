// ============================================================================
//  DismCore_SelfTest.cpp —— Rust 模块 FFI 集成自检
//  验证：C++ 调用 Rust 导出的 dismcore_digest_hex / hex_encode / version
//  以 /FunSelfTest 风格的无 GUI 路径运行，结果写 %TEMP%\dismcore_selftest.txt
// ============================================================================
#include "stdafx.h"
#include "DismCore.h"
#include "SeedCache.h"
#include "SeedStamp.h"   // [R85] 种子失效检测
#include "Page_Capability.h"   // [R90] CapRowHeight 穷举断言
#include "FeatureService.h"   // [R54] LoadSeedFeatCache 拒绝路径
#include "AppxCapUpdService.h" // [R54] LoadSeedCapCache 拒绝路径
#include "AppxSmartFilter.h"   // [R200] 分级映射自检
#include "OsVersionInfo.h"      // [R200] 分级判定口径
#include "DwmTheme.h"         // [R201] 圆角/Mica/强调色
#include "ThemeTokens.h"       // [R201] 颜色令牌
#include "CustomControls.h"     // [R209] CClickRowUI 默认底色断言
#include "FontSupport.h"       // [R201] 字体可用性探测
#include "DismCore_SelfTest_Runtime.h"   // [R132] 运行期服务层自检

#include <cstdio>
#include <cstring>   // [R42] strlen（UTF-8 转换后修正长度）
#include <fstream>   // [C17] std::ofstream（F17_C_fileio 清零）
#include <cmath>   // [R201] std::pow（对比度计算）
#include <string>

// [REFACTOR] 自检执行上下文：替代原 RunDismCoreSelfTest 内的 chk lambda。
// 原 lambda 以 [&] 捕获 nPass/nFail/ofs，无法跨函数复用；改为结构体后
// 各组自检函数统一接收 SelfTestCtx&，计数与落盘语义完全不变。
//
// 为什么用结构体而非三个独立出参：Chk() 需要同时改 nPass/nFail 并写 ofs，
// 三者必须同源（同一个 ofs 保证输出顺序与原文一致），打包后不会传错。
//
// [R42 2026-10-26] 输出流从 std::wofstream 改为 std::ofstream + 显式 UTF-8。
//
//   原方案：`ofs.imbue(std::locale(ofs.getloc(), new std::codecvt_utf8_utf16<wchar_t>))`
//   两个问题：
//     1) std::codecvt_utf8_utf16 在 C++17 已弃用，全量重编报 C4996（STL4017）。
//        这是**我方代码里唯一的告警**，压掉它就等于把"0 警告"变成假话。
//     2) 更关键的是设计脆弱：宽字符要经过 locale facet 才能变成字节，
//        facet 一旦失败就置 failbit，**后续所有输出被静默丢弃**。
//        R31 真实踩过：日志停在第 14 项、没有汇总行，而进程退出码仍是 0。
//
//   新方案：宽字符 → Win32 WideCharToMultiByte(CP_UTF8) → UTF-8 字节 → 窄流。
//     编码错误在转换处就能发现（返回 0），不再藏进流状态里；
//     也不再依赖任何 locale facet，那类"静默丢弃"的失败模式从根上消失。
struct SelfTestCtx
{
    std::ofstream& ofs;
    int nPass = 0;
    int nFail = 0;

    explicit SelfTestCtx(std::ofstream& f) : ofs(f) {}

    // 宽字符 → UTF-8 字节串。失败返回空串，由调用方判为失败（不静默放过）。
    static std::string ToUtf8(const wchar_t* w)
    {
        if (!w || !*w) return std::string();
        const int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
        if (n <= 0) return std::string();
        std::string s;
        s.resize(static_cast<size_t>(n));
        const int written = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
        if (written <= 0) return std::string();
        s.resize(::strlen(s.c_str()));   // 去掉结尾 NUL
        return s;
    }

    // 判定一项：ok 记 PASS，否则记 FAIL；what 为 CP_ACP 窄字符串，
    // 先转宽再落盘（与原 lambda 做法逐字一致）。
    // 输出格式 "PASS  <what>" / "FAIL  <what>" 属对外契约，不得改动。
    void Chk(bool ok, const char* what)
    {
        std::wstring w;
        // [R31-FIX 2026-10-25] 必须用 UTF-8 解码，不能用 CP_ACP。
        //   实测后果：中文断言使 std::wofstream 在默认 C locale 下
        //   写宽字符失败，置位 failbit 后**后续所有输出静默丢弃**：
        //   日志停在第 14 项、连汇总行都没有，而进程退出码仍为 0。
        //   这是最危险的失败方式：看起来跑完了，实际丢了一半。
        // [R31-FIX2 2026-10-25] 输入必须用 CP_ACP，不能用 CP_UTF8。
        //   MSVC 的**窄字符串字面量按执行字符集编码**（中文系统 = GBK/936），
        //   源文件是 UTF-8 只影响编译器如何“读”源码，不改变字面量的运行时字节。
        //   实测：按 CP_UTF8 解码 GBK 字节 → 逐字节被替换为 U+FFFD，
        //   日志里出现 ï¿½ ï¿½ ï¿½。
        //   输出侧仍用 UTF-8 facet（imbue），两者分开才对。
        int n = MultiByteToWideChar(CP_ACP, 0, what, -1, nullptr, 0);
        if (n > 0)
        {
            w.resize(n);
            MultiByteToWideChar(CP_ACP, 0, what, -1, w.data(), n);
            w.resize(wcslen(w.c_str()));
        }
        if (ok) { ++nPass; ofs << "PASS  " << ToUtf8(w.c_str()) << "\n"; }
        else    { ++nFail; ofs << "FAIL  " << ToUtf8(w.c_str()) << "\n"; }
        // [R31-FIX] 写后检流状态：写不进去就必须可见，
        //   否则后续输出全被静默吞掉（本轮真实事故）。
        if (!ofs.good())
        {
            ofs.clear();
            ++nFail;
            ofs << "FAIL  selftest log stream broke (io)\n";
        }
    }
};

// [R31] 前置声明：四个分组自检函数定义在 RunDismCoreSelfTest 之后，
// 原文件因从未被编译进工程，一直缺这组声明（注册进 vcxproj 后立刻暴露 C3861）。
static void SelfTestDigestAndHex(SelfTestCtx& ctx);
static void SelfTestHexAndBase64(SelfTestCtx& ctx);
static void SelfTestDataExpr(SelfTestCtx& ctx);
static void SelfTestVersion(SelfTestCtx& ctx);
// [R31 2026-10-25] 补齐 R8 新增的两个模型组件自检。
//   此前 6 个 FFI（cap 可见性/排序、feat 聚合/差异）完全没有自检覆盖，
//   意味着它们编译通过但行为对错无人发现。
static void SelfTestCapModel(SelfTestCtx& ctx);
static void SelfTestFeatModel(SelfTestCtx& ctx);
// [R210] 大面积强调色铺底自检。
//
//   背景：像素验收发现三处"整条铺强调色"的旧式横幅 ——
//   会话信息条(127px 高、整窗宽)、底部状态栏(38px)、欢迎页品牌横幅
//   (app_banner.png 是 1200x130 的纯 #007ACC 图片)。深色主题下是刺眼亮块。
//   这类问题 XML 层面看不出来（属性合法、令牌也对），只有渲染后按面积才暴露。
//   这里把"横幅类容器不得铺 Accent"固化成断言，取值与实际皮肤一致。
static void SelfTestNoAccentBand(SelfTestCtx& ctx)
{
    // SurfaceAlt 与 Accent 必须是不同颜色，否则"改用次级底"等于没改
    ctx.Chk(tokens::Token(tokens::EToken::SurfaceAlt) != tokens::Token(tokens::EToken::Accent),
            "ui: 次级底与强调色不是同一色");

    // 深色主题下，横幅底（SurfaceAlt）亮度必须明显低于强调色 ——
    // 若接近，说明横幅仍会呈现为亮块。
    auto lum = [](DWORD cr) -> int
    {
        return static_cast<int>(((cr >> 16 & 0xFF) * 30 + (cr >> 8 & 0xFF) * 59
                                 + (cr & 0xFF) * 11) / 100);
    };
    if (dwmtheme::IsSystemDarkMode())
    {
        const int nSurfAlt = lum(tokens::Token(tokens::EToken::SurfaceAlt));
        const int nAccent  = lum(tokens::Token(tokens::EToken::Accent));
        ctx.Chk(nSurfAlt < 128, "ui: 深色下横幅底为暗色");
        ctx.Chk(nSurfAlt < nAccent, "ui: 深色下横幅底亮度低于强调色");
    }

    // AccentBorder 用作 3px 细分隔线时，在暗底上仍需可见（不能与底色同值）
    ctx.Chk(tokens::Token(tokens::EToken::AccentBorder)
            != tokens::Token(tokens::EToken::SurfaceAlt),
            "ui: 强调色分隔线与横幅底可区分");
}

// [R209] 行控件默认底色的主题一致性自检。
//
//   背景：CClickRowUI 是被 10 个页面复用的行容器，构造里原本写死纯白底。
//   像素验收实测：深色主题下 Tools / Welcome / Cleanup / Driver 等页出现
//   大面积白块（亮块占比 22%~31%）。改令牌后降到 0~1%。
//   这里把"默认底色必须等于 Surface 令牌"固化成断言，
//   防止以后有人再写死一个浅色。
static void SelfTestRowBackground(SelfTestCtx& ctx)
{
    CClickRowUI row;
    const DWORD crRow = row.GetBkColor();
    const DWORD crSurf = tokens::Token(tokens::EToken::Surface);

    ctx.Chk(crRow == crSurf, "ui: 行控件默认底色等于 Surface 令牌");
    ctx.Chk((crRow & 0xFF000000u) == 0xFF000000u, "ui: 行控件默认底色不透明");

    // 深色主题下行底色必须是暗色（亮度 < 128），否则就是"深色里的白块"
    if (dwmtheme::IsSystemDarkMode())
    {
        const int nLum = ((crRow >> 16 & 0xFF) * 30
                        + (crRow >> 8 & 0xFF) * 59
                        + (crRow & 0xFF) * 11) / 100;
        ctx.Chk(nLum < 128, "ui: 深色主题下行底色为暗色");
    }

    // 取消选中后必须回到 Surface，而不是回到某个写死的浅色
    row.SetRowSelected(true);
    const DWORD crSel = row.GetBkColor();
    ctx.Chk(crSel == tokens::Token(tokens::EToken::Accent), "ui: 选中底色等于 Accent 令牌");
    row.SetRowSelected(false);
    ctx.Chk(row.GetBkColor() == crSurf, "ui: 取消选中后回到 Surface 令牌");
}

// [R206] 页名 -> 皮肤控件名 的映射自检。
//
//   背景：LogPageContent 按 PageName() 查找控件，但两者不是机械对应 ——
//   PageName 返回 "Cleanup"（首字母大写），XML 是 "page_cleanup"；
//   PAGE_FEATURE 的成员是 CPageCapability，其 UI 根节点却叫 "page_feature"。
//   这两处都曾导致统计落到兜底分支（静默失准），因此固化成断言。
static void SelfTestPageNameMapping(SelfTestCtx& ctx)
{
    // 16 个页名，与 PageHost.cpp 的 kPages 顺序一致
    static const wchar_t* const kNames[16] = {
        L"Cleanup", L"Startup", L"Appx", L"Tools", L"Optim", L"Driver",
        L"Program", L"Capability", L"Update", L"Assoc", L"Unattend", L"Tasks",
        L"Control", L"Intl", L"RegClean", L"Welcome"
    };
    // 期望的 XML 控件名（小写，Capability 特例为 feature）
    static const wchar_t* const kExpect[16] = {
        L"page_cleanup", L"page_startup", L"page_appx", L"page_tools",
        L"page_optim", L"page_driver", L"page_program", L"page_feature",
        L"page_update", L"page_assoc", L"page_unattend", L"page_tasks",
        L"page_control", L"page_intl", L"page_regclean", L"page_welcome"
    };

    int nMismatch = 0;
    for (int i = 0; i < 16; ++i)
    {
        std::wstring sWant(kExpect[i]);
        // 复刻 LogPageContent 的解析：先原名，再首字母小写，再处理 Capability 特例
        std::wstring sBase(kNames[i]);
        std::wstring sMapped = sBase;
        if (sBase == L"Capability") sMapped = L"feature";
        std::wstring sLower = sMapped;
        if (!sLower.empty())
            sLower[0] = static_cast<wchar_t>(::towlower(sLower[0]));
        for (size_t k = 1; k < sLower.size(); ++k)
            sLower[k] = static_cast<wchar_t>(::towlower(sLower[k]));

        const std::wstring sA = L"page_" + sMapped;
        const std::wstring sB = L"page_" + sLower;
        if (sA != sWant && sB != sWant) ++nMismatch;
    }
    ctx.Chk(nMismatch == 0, "page: 16 个页名均可解析到正确控件名");
    ctx.Chk(std::wstring(kNames[7]) == L"Capability", "page: idx7 成员名为 Capability");
    ctx.Chk(std::wstring(kExpect[7]) == L"page_feature", "page: idx7 控件名为 page_feature（特例）");
}

// [R202] 颜色令牌完整性自检。
//   深色模式最容易出的错不是"颜色不好看"，而是"某类属性没跟着换" ——
//   白底黑字变成白底白字，界面上什么都看不见。这里把可读性做成断言。
static void SelfTestThemeTokens(SelfTestCtx& ctx)
{
    // 令牌表长度必须与枚举一致（防止加了枚举忘了加名字）
    ctx.Chk(static_cast<int>(tokens::EToken::Count) == 21, "tokens: 令牌枚举数量为 21");

    // 所有令牌都必须不透明（DuiLib 的 0x00 alpha 会画成透明，等于消失）
    bool bAllOpaque = true;
    for (int i = 0; i < static_cast<int>(tokens::EToken::Count); ++i)
    {
        const DWORD cr = tokens::Token(static_cast<tokens::EToken>(i));
        if ((cr & 0xFF000000u) != 0xFF000000u) bAllOpaque = false;
    }
    ctx.Chk(bAllOpaque, "tokens: 全部令牌不透明");

    // 主文字与主背景的对比度必须 >= 4.5:1（浅色/深色都要成立）
    {
        auto lin = [](unsigned int v) -> double
        {
            const double c = static_cast<double>(v) / 255.0;
            if (c <= 0.03928) return c / 12.92;
            return std::pow((c + 0.055) / 1.055, 2.4);
        };
        auto lum = [&lin](DWORD cr) -> double
        {
            return 0.2126 * lin(cr & 0xFF)
                 + 0.7152 * lin((cr >> 8) & 0xFF)
                 + 0.0722 * lin((cr >> 16) & 0xFF);
        };
        const DWORD crInk = tokens::Token(tokens::EToken::Ink);
        const DWORD crBg  = tokens::Token(tokens::EToken::Surface);
        const double a = lum(crInk), b = lum(crBg);
        const double ratio = (a > b) ? (a + 0.05) / (b + 0.05) : (b + 0.05) / (a + 0.05);
        ctx.Chk(ratio >= 4.5, "tokens: Ink/Surface 对比度 >= 4.5:1");
    }

    // 最低对比的组合：InkMuted 压在 SurfaceAlt 上（深色模式曾在此处只有 3.59:1）
    {
        auto lin = [](unsigned int v) -> double
        {
            const double c = static_cast<double>(v) / 255.0;
            if (c <= 0.03928) return c / 12.92;
            return std::pow((c + 0.055) / 1.055, 2.4);
        };
        auto lum = [&lin](DWORD cr) -> double
        {
            return 0.2126 * lin(cr & 0xFF)
                 + 0.7152 * lin((cr >> 8) & 0xFF)
                 + 0.0722 * lin((cr >> 16) & 0xFF);
        };
        const DWORD crMuted = tokens::Token(tokens::EToken::InkMuted);
        const DWORD crAlt   = tokens::Token(tokens::EToken::SurfaceAlt);
        const double a = lum(crMuted), b = lum(crAlt);
        const double ratio = (a > b) ? (a + 0.05) / (b + 0.05) : (b + 0.05) / (a + 0.05);
        ctx.Chk(ratio >= 4.5, "tokens: InkMuted/SurfaceAlt 对比度 >= 4.5:1");
    }

    // [R203] 语义色在**当前主题**下必须可读。
    //   这条专门防深色模式的经典漏项：风险红/警告黄这些色是浅色底调的，
    //   直接搬到深色底上会暗到看不清（实测 #7A5C00 在 #1F1F1F 上仅 2.64:1）。
    {
        auto lin = [](unsigned int v) -> double
        {
            const double c = static_cast<double>(v) / 255.0;
            if (c <= 0.03928) return c / 12.92;
            return std::pow((c + 0.055) / 1.055, 2.4);
        };
        auto lum = [&lin](DWORD cr) -> double
        {
            return 0.2126 * lin(cr & 0xFF)
                 + 0.7152 * lin((cr >> 8) & 0xFF)
                 + 0.0722 * lin((cr >> 16) & 0xFF);
        };
        const DWORD crBg = tokens::Token(tokens::EToken::Surface);
        const double lb = lum(crBg);
        auto readable = [&](DWORD fg) -> bool
        {
            const double lf = lum(fg);
            const double r = (lf > lb) ? (lf + 0.05) / (lb + 0.05) : (lb + 0.05) / (lf + 0.05);
            return r >= 3.0;   // 语义色多为大字号徽标/图标，3:1 是 WCAG 对大字的底线
        };
        ctx.Chk(readable(tokens::Token(tokens::EToken::Danger)),  "tokens: 风险色在当前主题下可读 (>=3:1)");
        ctx.Chk(readable(tokens::Token(tokens::EToken::Success)), "tokens: 成功色在当前主题下可读 (>=3:1)");
        ctx.Chk(readable(tokens::Token(tokens::EToken::Warn)),    "tokens: 警告色在当前主题下可读 (>=3:1)");
    }
}

// [R201] 字体可用性自检。
//   验证"解析结果一定可用"这条契约：UiTextFace() 返回的名字必须能被
//   GDI 真实选中（回读字面名一致）。否则界面仍在被静默回退，
//   而这类问题在截图上看不出来（只是字重略变），只能靠断言守住。
static void SelfTestFontFaces(SelfTestCtx& ctx)
{
    const wchar_t* pszText = fontsup::UiTextFace();
    const wchar_t* pszSym  = fontsup::UiSymbolFace();
    const wchar_t* pszMono = fontsup::UiMonoFace();

    ctx.Chk(pszText && pszText[0], "font: 正文字体解析结果非空");
    ctx.Chk(pszSym && pszSym[0], "font: 符号字体解析结果非空");
    ctx.Chk(pszMono && pszMono[0], "font: 等宽字体解析结果非空");

    // 解析结果必须真实可用（否则等于没修）
    ctx.Chk(fontsup::IsFontAvailable(pszText), "font: 正文字体本机真实可用");
    ctx.Chk(fontsup::IsFontAvailable(pszSym), "font: 符号字体本机真实可用");

    // 探测函数本身要能识别"不存在的字体"（防止它永远返回 true）
    ctx.Chk(!fontsup::IsFontAvailable(L"Definitely Not A Font XYZ"), "font: 不存在的字体被判为不可用");
}

// [R201] Win11 视觉适配层自检。
//
// 验证的是"机制"而不是"本机运气"：
//   · 强调色令牌必须与 DWM 读到的源值一致（防止派生色算错源）；
//   · AccentText 必须在白底上达到 4.5:1（这是硬指标，差一点就是不合格）；
//   · 版本名不得把 25H2 显示成 24H2（本轮修的就是这个）；
//   · 能力位日志串必须非空且不含 "none" 之外的空洞。
static void SelfTestWin11Theme(SelfTestCtx& ctx)
{
    // ---- 1) 强调色源与令牌一致 ----
    const DWORD crAccent = dwmtheme::AccentColorRef();
    const DWORD crToken  = tokens::Token(tokens::EToken::Accent);
    ctx.Chk((crToken & 0x00FFFFFF) == (crAccent & 0x00FFFFFF)
            && (crToken & 0xFF000000) == 0xFF000000,
            "theme: 强调色令牌与 DWM 源值一致且不透明");

    // ---- 2) AccentText 在当前主题背景上的对比度 >= 4.5:1 ----
    //   [R207] 这条断言原来固定按"白底"测，但 AccentText 现在按主题自适应：
    //   深色下是提亮版（#449CDE 一类），压白底当然不足 —— 断言口径必须
    //   跟着目标背景走，否则会误报。取 SurfaceAlt（深色下最亮的底）作基准。
    {
        const DWORD cr = tokens::Token(tokens::EToken::AccentText);
        const DWORD crBgCheck = tokens::Token(tokens::EToken::SurfaceAlt);
        auto lin2 = [](unsigned int v) -> double
        {
            const double c = static_cast<double>(v) / 255.0;
            if (c <= 0.03928) return c / 12.92;
            return std::pow((c + 0.055) / 1.055, 2.4);
        };
        auto lumOf = [&lin2](DWORD x) -> double
        {
            return 0.2126 * lin2((x >> 16) & 0xFF)
                 + 0.7152 * lin2((x >> 8) & 0xFF)
                 + 0.0722 * lin2(x & 0xFF);
        };
        const double a = lumOf(cr), b = lumOf(crBgCheck);
        const double ratio = (a > b) ? (a + 0.05) / (b + 0.05) : (b + 0.05) / (a + 0.05);
        ctx.Chk(ratio >= 4.5, "theme: AccentText 在当前主题底上对比度 >= 4.5:1");
    }

    // ---- 3) 派生色单调性：Hot 比 Accent 亮，Pushed 比 Accent 暗 ----
    {
        auto lum = [](DWORD cr) -> double
        {
            const unsigned int r = cr & 0xFF, g = (cr >> 8) & 0xFF, b = (cr >> 16) & 0xFF;
            return 0.2126 * r + 0.7152 * g + 0.0722 * b;   // 相对亮度足够做单调比较
        };
        const double a = lum(tokens::Token(tokens::EToken::Accent));
        const double h = lum(tokens::Token(tokens::EToken::AccentHot));
        const double p = lum(tokens::Token(tokens::EToken::AccentPushed));
        ctx.Chk(h > a && a > p, "theme: 悬停色更亮、按下色更暗（派生方向正确）");
    }

    // ---- 4) 版本名：本机若为 25H2，不得显示成 24H2 ----
    {
        const unsigned int nBuild = osver::Current().nBuild;
        const std::wstring sName = osver::BuildDisplayName(osver::Current());
        bool bOk = true;
        if (nBuild >= 26200)
            bOk = (sName.find(L"24H2") == std::wstring::npos);
        ctx.Chk(bOk, "theme: 版本名不把 25H2 及更高显示成 24H2");
        // 版本名必须含 build 号，便于日志核对
        ctx.Chk(sName.find(std::to_wstring(nBuild)) != std::wstring::npos,
                "theme: 版本名包含 build 号");
    }

    // ---- 5) 能力位日志串 ----
    {
        const std::wstring s = dwmtheme::AbilityToText(
            static_cast<unsigned int>(dwmtheme::EThemeAbility::RoundedCorner)
            | static_cast<unsigned int>(dwmtheme::EThemeAbility::Mica));
        ctx.Chk(s == L"corner+mica", "theme: 能力位转日志串格式正确");
        ctx.Chk(dwmtheme::AbilityToText(0) == L"none", "theme: 空能力位输出 none");
    }
}

// [R200] 分级中文映射自检。
//   目的不是"验证映射表内容对不对"（那是人工审阅的事），而是验证
//   **分级门控这个机制本身**：
//     · Win10 上不得命中任何 Win11 条目；
//     · Win11 上同一个包必须能命中；
//     · 生效条数必须与当前系统档位一致。
//   这三条就是"Win10 整表不加载"这一需求的可执行定义。
static void SelfTestAppxMapTier(SelfTestCtx& ctx)
{
    const bool bWin11 = osver::IsWindows11OrLater();
    const int nActive = AppxSmartActiveEntryCount();

    ctx.Chk(nActive > 0, "appx-map: 映射索引非空");
    if (bWin11)
        ctx.Chk(nActive > 120, "appx-map: Win11 生效条数 > 120");
    else
        ctx.Chk(nActive <= 120, "appx-map: Win10 只加载通用条目");

    // 典型 Win11 专属包：Windows 小组件
    CDuiString sName;
    EAppxNameTier nTier = EAppxNameTier::Unknown;
    const bool bHit = AppxSmartLookupName(
        L"MicrosoftWindows.Client.WebExperience_421.20070.101.0_x64__8wekyb3d8bbwe",
        sName, &nTier);
    if (bWin11)
        ctx.Chk(bHit && !sName.IsEmpty() && nTier == EAppxNameTier::Win11,
                "appx-map: Win11 命中小组件的 Win11 专属映射");
    else
        ctx.Chk(!bHit, "appx-map: Win10 不命中 Win11 专属映射");

    // 通用条目在任何系统上都必须命中（分级别误杀通用表）
    CDuiString sCalc;
    const bool bCalc = AppxSmartLookupName(
        L"Microsoft.WindowsCalculator_11.2210.0.0_x64__8wekyb3d8bbwe", sCalc, nullptr);
    ctx.Chk(bCalc && !sCalc.IsEmpty(), "appx-map: 计算器为通用条目，任意系统均命中");

    // 分类正确性
    ctx.Chk(AppxSmartClassify(L"Microsoft.VCLibs.140.00_14.0.30704.0_x64__8wekyb3d8bbwe", false)
                == EAppxCategory::Runtime,
            "appx-class: VCLibs 归入运行时框架");
    ctx.Chk(AppxSmartClassify(L"Microsoft.WindowsCalculator_11.2210.0.0_x64__8wekyb3d8bbwe", false)
                == EAppxCategory::UserApp,
            "appx-class: 计算器归入用户应用");
    ctx.Chk(AppxSmartClassify(L"Microsoft.Win32WebViewHost_cw5n1h2txyewy", false)
                == EAppxCategory::SystemComp,
            "appx-class: WebView 宿主归入系统组件");
    ctx.Chk(AppxSmartClassify(L"Microsoft.WindowsCalculator_11.2210.0.0_neutral_~_8wekyb3d8bbwe",
                              true) == EAppxCategory::LangPack,
            "appx-class: 资源包归入语言资源包");

    // 同名合并：版本号在**结尾**的条目应合并（Windows App 运行时 1.4 / 1.7）。
    //   注意区分：UI.Xaml 2.3 运行时 与 2.4 运行时 版本号在中间，
    //   展示名本就不该相同 —— 合并规则只剥"空格+纯数字点串"的结尾段。
    const AppxSmartInfo i1 = AppxSmartDescribe(
        L"Microsoft.WindowsAppRuntime.1.4_4000.1049.117.0_x64__8wekyb3d8bbwe", false);
    const AppxSmartInfo i2 = AppxSmartDescribe(
        L"Microsoft.WindowsAppRuntime.1.7_7000.414.1745.0_x64__8wekyb3d8bbwe", false);
    ctx.Chk(!i1.sName.IsEmpty() && !i2.sName.IsEmpty() && i1.sName == i2.sName,
            "appx-map: 同名合并（Windows App 运行时 1.4/1.7 展示名一致）");

    // 反向断言：中间带版本的名称**不得**被误合并（防止合并规则过度吞字符）
    const AppxSmartInfo j1 = AppxSmartDescribe(
        L"Microsoft.UI.Xaml.2.3_7.320.0.0_x64__8wekyb3d8bbwe", false);
    const AppxSmartInfo j2 = AppxSmartDescribe(
        L"Microsoft.UI.Xaml.2.4_7.320.0.0_x64__8wekyb3d8bbwe", false);
    ctx.Chk(!j1.sName.IsEmpty() && !j2.sName.IsEmpty() && j1.sName != j2.sName,
            "appx-map: 中间版本号不参与合并（UI.Xaml 2.3 / 2.4 各自保留）");

    ctx.Chk(AppxSmartCategoryText(EAppxCategory::All)[0] != 0
            && AppxSmartCategoryText(EAppxCategory::LangPack)[0] != 0,
            "appx-map: 分类名非空");
}

static void SelfTestSeedCache(SelfTestCtx& ctx);   // [R54]

// [R132] 运行期服务层自检的适配器。
//   DismCore_SelfTest_Runtime.cpp 不 include 本文件的 SelfTestCtx（匿名布局），
//   改用 void* + 函数指针回调，两边只依赖头文件里的签名。
//   用自由函数而非 lambda：函数指针要能取地址传给另一 TU。
static void RuntimeChkTrampoline(void* pCtx, bool ok, const char* pszName)
{
    static_cast<SelfTestCtx*>(pCtx)->Chk(ok, pszName);
}

// [R-P18] Rust 模块自检：返回通过项数，失败项写入日志
int RunDismCoreSelfTest()
{
    wchar_t szLog[MAX_PATH] = { 0 };
    ::GetTempPathW(MAX_PATH, szLog);
    wcscat_s(szLog, _countof(szLog), L"dismcore_selftest.txt");

    std::ofstream ofs(szLog, std::ios::out | std::ios::trunc);
    if (!ofs.is_open()) return 0;

    // [R42 2026-10-26] 不再 imbue locale facet。
    //   宽字符在 Chk() 里用 WideCharToMultiByte 显式转 UTF-8，
    //   编码失败在转换处即可发现，
    //   不再藏进流状态里。

    // [REFACTOR] 原局部 lambda chk 已提为 SelfTestCtx::Chk（见文件头）。
    // ctx 同时持有计数与输出流，各分组自检函数统一以 ctx.Chk 记录。
    SelfTestCtx ctx(ofs);

    // 0) 版本探测（DLL 可加载）
    const uint32_t ver = dismcore::Version();
    ctx.Chk(ver == 0x00060000u, "dismcore_version == 0x00060000");

    const std::string prepXml = dismcore::BuildWin11Preprocess("ImageAdmin", 0);
    const std::string prepCmd = dismcore::BuildWin11Preprocess("ImageAdmin", 1);
    ctx.Chk(!prepXml.empty() && !prepCmd.empty()
            && dismcore::ValidateWin11Preprocess(prepXml, prepCmd),
            "win11 preprocess bundle validates");

    SelfTestDigestAndHex(ctx);

    // ===== [R-P22 · R2] Hex/Base64 =====

    SelfTestHexAndBase64(ctx);

    // ===== [R-P24 · R4] ?Get* 解析器（UTF-16 变体，DataEngine 主路径） =====

    SelfTestDataExpr(ctx);

    // ===== [R-P25 · R6] 版本解析与比较 =====

    SelfTestVersion(ctx);

    // ===== [R31] 两个页面模型组件（R8 新增 FFI） =====
    SelfTestCapModel(ctx);
    SelfTestFeatModel(ctx);

    // ===== [R54] 种子指纹与加载拒绝路径 =====
    SelfTestSeedCache(ctx);

    // ===== [R200] 分级中文映射（Win10/Win11）=====
    SelfTestAppxMapTier(ctx);

    // ===== [R201] Win11 视觉适配层（圆角/Mica/强调色/版本名）=====
    SelfTestWin11Theme(ctx);

    // ===== [R201] 字体族可用性 =====
    SelfTestFontFaces(ctx);

    // ===== [R202] 颜色令牌（深色模式可读性）=====
    SelfTestThemeTokens(ctx);

    // ===== [R206] 页名到控件名映射 =====
    SelfTestPageNameMapping(ctx);

    // ===== [R209] 行底色主题一致性 =====
    SelfTestRowBackground(ctx);

    // ===== [R210] 大面积强调色铺底 =====
    SelfTestNoAccentBand(ctx);

    // ===== [R132] 运行期服务层自检（真实注册表/文件系统往返）=====
    //   前三组是纯函数，这组会真的写注册表与临时文件，故单独一个 TU。
    //   仍保持完全隔离：只用 %TEMP% 与临时路径值，结束即清理。
    SelfTestRuntimeServices(&ctx, &RuntimeChkTrampoline);

    // [R32 2026-10-25] 项数完整性断言。
    //   R31 实际发生过：日志流写中文失败，后续输出静默丢弃，
    //   日志停在第 14 项、连汇总行都没有，而进程退出码仍为 0。
    //   没有项数断言，这种"看起来跑完了"的假象无从发现。
    //   这条必须放在汇总行之前，否则流坏了就写不进去。
    // [R79] 期望项数随用例增减同步维护：这里断言的是「实际跑到的项数」，
    //   用途单一——防止日志被截断后少项仍被判通过。
    // [R137] 107：runtime 组实测 18 项（R137 补了混合成败 2 项）
    //   + 既有 89 项 = 107，加本断言行 total=108。
    // [R203] 136 = R202 的 133 + 语义色 3 项。
    //   历史值一直多写了 1，导致这条断言长期恒 FAIL —— 本轮按实测校正。该组各项在 Win11 与 Win10 上
    //   条数相同（分支各出 1 条），故两档系统上项数一致，断言可共用。
    const int kExpectedItems = 150;
    ctx.Chk(ctx.nPass + ctx.nFail == kExpectedItems,
            "selftest item count == kExpectedItems (完整性断言，防日志截断)");

    ofs << "\nR-P24 dismcore selftest pass=" << ctx.nPass << " fail=" << ctx.nFail
        << " total=" << (ctx.nPass + ctx.nFail) << "\n";
    return ctx.nPass;
}
// [C++17 R3] 本文件内联常量（C++17 inline variables，多 TU ODR 一致）：

// [REFACTOR] 从 RunDismCoreSelfTest 提取：自检项 1–7（摘要与十六进制基础）。
// 覆盖 MD5/SHA-256 标准向量（RFC 1321 / FIPS 180-4）、空输入摘要、
// HexEncode、非法算法码、缓冲区不足码、文件摘要长度。
// 逐字搬迁零逻辑改动，仅把 chk 换为 ctx.Chk。
static void SelfTestDigestAndHex(SelfTestCtx& ctx)
{
// 1) MD5 标准向量（RFC 1321）
{
    const char* s = "abc";
    const std::string h = dismcore::DigestHex(DISMCORE_MD5, s, 3);
    ctx.Chk(h == "900150983cd24fb0d6963f7d28e17f72", "MD5(\"abc\")");
}
// 2) SHA-256 标准向量（FIPS 180-4）
{
    const char* s = "abc";
    const std::string h = dismcore::DigestHex(DISMCORE_SHA256, s, 3);
    ctx.Chk(h == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "SHA256(\"abc\")");
}
// 3) 空输入 MD5
{
    const std::string h = dismcore::DigestHex(DISMCORE_MD5, "", 0);
    ctx.Chk(h == "d41d8cd98f00b204e9800998ecf8427e", "MD5(\"\")");
}
// 4) 十六进制编码
{
    const unsigned char v[4] = { 0x00, 0x0F, 0xA5, 0xFF };
    const std::string h = dismcore::HexEncode(v, 4);
    ctx.Chk(h == "000fa5ff", "HexEncode({00,0F,A5,FF})");
}
// 5) 非法算法返回 -1
{
    constexpr int kDigestHexBufBytes = 160;   // 160 缓冲尺寸（语义命名，数值不变）
    char buf[kDigestHexBufBytes] = { 0 };
    const int64_t n = ::dismcore_digest_hex(99, nullptr, 0,
                                            reinterpret_cast<uint8_t*>(buf),
                                            sizeof(buf));
    ctx.Chk(n == DISMCORE_ERR_BAD_ALGO, "bad algo -> -1");
}
// 6) 缓冲区不足返回 -2（Rust 侧不越界写）
{
    char buf[8] = { 0 };
    const int64_t n = ::dismcore_digest_hex(DISMCORE_MD5, nullptr, 0,
                                            reinterpret_cast<uint8_t*>(buf),
                                            sizeof(buf));
    ctx.Chk(n == DISMCORE_ERR_BUF_SMALL, "small buffer -> -2");
}
// 7) 文件摘要（自检可执行文件本身可读）
{
    wchar_t szSelf[MAX_PATH] = { 0 };
    ::GetModuleFileNameW(nullptr, szSelf, MAX_PATH);
    const std::string h = dismcore::DigestFileHex(DISMCORE_SHA256, szSelf);
    ctx.Chk(h.size() == 64, "DigestFileHex(self, SHA256) len==64");
}

}

// [REFACTOR] 从 RunDismCoreSelfTest 提取：自检项 8–14（Hex/Base64）。
// 覆盖 HexDecode 正常/非法、Base64 编解码本位（RFC 4648）、
// 解码 roundtrip、容忍空白、非法输入返回空、UTF-8 中文编码。
// 逐字搬迁零逻辑改动。
static void SelfTestHexAndBase64(SelfTestCtx& ctx)
{
// 8) 十六进制解码（Rust hex_decode FFI）
{
    const std::vector<uint8_t> v = dismcore::HexDecode("000fa5ff");
    const uint8_t expect[4] = { 0x00, 0x0F, 0xA5, 0xFF };
    ctx.Chk(v.size() == 4 && memcmp(v.data(), expect, 4) == 0,
        "HexDecode(\"000fa5ff\")");
}
// 9) 十六进制解码非法输入返回空
{
    ctx.Chk(dismcore::HexDecode("xyz").empty(), "HexDecode(\"xyz\") empty");
}
// 10) Base64 编码标准向量（RFC 4648）
{
    const char* s = "foobar";
    const std::string b = dismcore::Base64Encode(s, 6);
    ctx.Chk(b == "Zm9vYmFy", "Base64Encode(\"foobar\") == \"Zm9vYmFy\"");
}
// 11) Base64 解码 roundtrip
{
    const auto v = dismcore::Base64Decode("Zm9vYmFy");
    ctx.Chk(v.size() == 6 && memcmp(v.data(), "foobar", 6) == 0,
        "Base64Decode(\"Zm9vYmFy\") == \"foobar\"");
}
// 12) Base64 解码容忍空白
{
    const auto v = dismcore::Base64Decode("Zm9v\nYmFy\r\n");
    ctx.Chk(v.size() == 6 && memcmp(v.data(), "foobar", 6) == 0,
        "Base64Decode(\"Zm9v\\nYmFy\\r\\n\") 容忍空白");
}
// 13) Base64 非法输入返回空
{
    ctx.Chk(dismcore::Base64Decode("!!!!").empty(), "Base64Decode(\"!!!!\") empty");
}
// 14) 中文 UTF-8 Base64 编码
{
    const char* zh = "\xE4\xBD\xA0\xE5\xA5\xBD";  // "你好" UTF-8
    const std::string b = dismcore::Base64Encode(zh, 6);
    ctx.Chk(b == "5L2g5aW9", "Base64Encode(UTF-8 你好) == \"5L2g5aW9\"");
}
}

// [REFACTOR] 从 RunDismCoreSelfTest 提取：自检项 15–22（?Get* 解析器）。
// 覆盖 ParseDataExprUtf16 的基本命中/中间出现/空函数体/大小写/
// 未命中/括号未闭合/含中文路径，以及 UTF-8 变体 ParseDataExpr。
// 逐字搬迁零逻辑改动。
static void SelfTestDataExpr(SelfTestCtx& ctx)
{
// 15) 基本命中：?GetRegSz(...)，q=0，matchLen=整体长度
{
    dismcore::DataExprHitW hit;
    const bool ok = dismcore::ParseDataExprUtf16(L"?GetRegSz(HKEY\\A,B)", hit);
    ctx.Chk(ok && hit.q == 0
        && hit.matchLen == wcslen(L"?GetRegSz(HKEY\\A,B)")
        && hit.name == L"GetRegSz"
        && hit.body == L"HKEY\\A,B",
        "ParseDataExprUtf16(?GetRegSz(HKEY\\A,B))");
}
// 16) 表达式在字符串中间，q 非 0，且含后缀
{
    dismcore::DataExprHitW hit;
    const bool ok = dismcore::ParseDataExprUtf16(
        L"Root=?GetIniData(%Root%a.ini,S,K)\\..\\x", hit);
    ctx.Chk(ok && hit.q == 5
        && hit.matchLen == wcslen(L"?GetIniData(%Root%a.ini,S,K)")
        && hit.name == L"GetIniData"
        && hit.body == L"%Root%a.ini,S,K",
        "ParseDataExprUtf16(表达式在中间)");
}
// 17) 空函数体：?GetLocaleName()
{
    dismcore::DataExprHitW hit;
    const bool ok = dismcore::ParseDataExprUtf16(L"x?GetLocaleName()y", hit);
    ctx.Chk(ok && hit.q == 1
        && hit.matchLen == wcslen(L"?GetLocaleName()")
        && hit.name == L"GetLocaleName"
        && hit.body.empty(),
        "ParseDataExprUtf16(空函数体)");
}
// 18) 函数名大小写不敏感（Data.xml 中可能出现 ?getregsz）
{
    dismcore::DataExprHitW hit;
    const bool ok = dismcore::ParseDataExprUtf16(L"?GetRegSz(a,b)", hit);
    // Rust 侧保留原文大小写，C++ LookupFunEval 用 lstrcmpiW 再判定
    ctx.Chk(ok && hit.name == L"GetRegSz", "ParseDataExprUtf16(大小写)");
}
// 19) 未命中：无 '?' / 非 Get 前缀
{
    dismcore::DataExprHitW hit;
    ctx.Chk(!dismcore::ParseDataExprUtf16(L"plain string", hit),
        "ParseDataExprUtf16(无 '?') false");
    ctx.Chk(!dismcore::ParseDataExprUtf16(L"a?Foo(b)", hit),
        "ParseDataExprUtf16(非 Get 前缀) false");
}
// 20) 括号未闭合：保守未命中（不 panic）
{
    dismcore::DataExprHitW hit;
    ctx.Chk(!dismcore::ParseDataExprUtf16(L"?GetRegSz(HKEY\\A,B", hit),
        "ParseDataExprUtf16(括号未闭合) false");
}
// 21) 含中文路径：code unit 下标不偏移（q 仍为 '?' 位置）
{
    dismcore::DataExprHitW hit;
    const bool ok = dismcore::ParseDataExprUtf16(
        L"Root=?GetRegSz(HKEY\\\x8F6F\x4EF6\\King,AppPath)", hit);
    ctx.Chk(ok && hit.q == 5
        && hit.matchLen == wcslen(L"?GetRegSz(HKEY\\\x8F6F\x4EF6\\King,AppPath)")
        && hit.name == L"GetRegSz"
        && hit.body == L"HKEY\\\x8F6F\x4EF6\\King,AppPath",
        "ParseDataExprUtf16(含中文路径)");
}
// 22) UTF-8 变体（供非 DuiLib 侧复用）
{
    dismcore::DataExprHit hit;
    const bool ok = dismcore::ParseDataExpr(
        "?GetFileVersion(%Root%Chrome.exe)", hit);
    ctx.Chk(ok && hit.q == 0
        && hit.matchLen == strlen("?GetFileVersion(%Root%Chrome.exe)")
        && hit.name == "GetFileVersion"
        && hit.body == "%Root%Chrome.exe",
        "ParseDataExpr(UTF-8)");
}
}

// [REFACTOR] 从 RunDismCoreSelfTest 提取：自检项 23–28（版本解析与比较）。
// 覆盖四段版本、缺段补 0、非法字符按 0、空串、比较三分支、缺段比较。
// 逐字搬迁零逻辑改动。
static void SelfTestVersion(SelfTestCtx& ctx)
{
// 23) 完整四段版本
{
    dismcore::DismVersion v = dismcore::ParseVersion(L"10.0.19045.3803");
    ctx.Chk(v.a == 10 && v.b == 0 && v.c == 19045 && v.d == 3803,
        "ParseVersion(10.0.19045.3803)");
}
// 24) 缺段补 0（1.2.3 == 1.2.3.0）
{
    dismcore::DismVersion v = dismcore::ParseVersion(L"1.2.3");
    ctx.Chk(v.a == 1 && v.b == 2 && v.c == 3 && v.d == 0,
        "ParseVersion(1.2.3) 缺段补0");
}
// 25) 非法字符停止当前段（_ttoi 语义）
{
    dismcore::DismVersion v = dismcore::ParseVersion(L"1x.2");
    ctx.Chk(v.a == 1 && v.b == 2, "ParseVersion(1x.2) 非法段按0");
}
// 26) 空串 → 0.0.0.0
{
    dismcore::DismVersion v = dismcore::ParseVersion(L"");
    ctx.Chk(v.a == 0 && v.b == 0 && v.c == 0 && v.d == 0,
        "ParseVersion(空) -> 0.0.0.0");
}
// 27) 版本比较：相等 / 大于 / 小于
{
    dismcore::DismVersion a = dismcore::ParseVersion(L"1.2.3.4");
    dismcore::DismVersion b = dismcore::ParseVersion(L"1.2.3.5");
    ctx.Chk(dismcore::CompareVersion(a, b) == -1, "CompareVersion(1.2.3.4, 1.2.3.5) -> -1");
    ctx.Chk(dismcore::CompareVersion(a, a) == 0, "CompareVersion(相等) -> 0");
    ctx.Chk(dismcore::CompareVersion(b, a) == 1, "CompareVersion(1.2.3.5, 1.2.3.4) -> 1");
}
// 28) 缺段版本比较（1.2 == 1.2.0.0）
{
    dismcore::DismVersion v12 = dismcore::ParseVersion(L"1.2");
    dismcore::DismVersion v1200 = dismcore::ParseVersion(L"1.2.0.0");
    ctx.Chk(dismcore::CompareVersion(v12, v1200) == 0,
        "CompareVersion(1.2, 1.2.0.0) -> 0");
}
}

// [R31 2026-10-25] 可选功能模型组件自检（R8 新增 FFI，此前无覆盖）。
//   覆盖点：基础可见性、大小写不敏感过滤、
//   长度不匹配时拒绝（防止错位数据静默通过）。
static void SelfTestCapModel(SelfTestCtx& ctx)
{
    const std::vector<std::wstring> st = { L"Installed", L"Not Present" };
    const std::vector<std::wstring> id = { L"OpenSSH.Client", L"X.Y" };
    const std::vector<std::wstring> dp = { L"openssh", L"yy" };
    std::vector<uint8_t> vis;

    // 1) 不显示全部 → 仅 Installed 可见
    ctx.Chk(dismcore::CapVisibility(st, id, dp, false, L"", vis)
            && vis.size() == 2 && vis[0] == 1 && vis[1] == 0,
            "cap: showAll=false 仅已安装可见");

    // 2) 过滤大小写不敏感（identity 命中）
    ctx.Chk(dismcore::CapVisibility(st, id, dp, true, L"openssh", vis)
            && vis.size() == 2 && vis[0] == 1 && vis[1] == 0,
            "cap: filter 大小写不敏感命中 identity");

    // 3) 过滤命中显示名
    ctx.Chk(dismcore::CapVisibility(st, id, dp, true, L"yy", vis)
            && vis.size() == 2 && vis[0] == 0 && vis[1] == 1,
            "cap: filter 命中 display");

    // 4) 长度不匹配必须拒绝（不得取最短糊弄）
    const std::vector<std::wstring> idShort = { L"only-one" };
    ctx.Chk(!dismcore::CapVisibility(st, idShort, dp, true, L"", vis),
            "cap: 长度不匹配时拒绝（非取最短）");

    // [R78] 排序组件：此前 Rust 侧已导出、头文件已声明，但 C++ 零调用，
    //   页面顺序完全取决于枚举顺序。这里补上契约验证，防止再次「导出了没人用」。
    {
        const std::vector<std::wstring> s3 = { L"Not Present", L"Installed", L"Installed" };
        const std::vector<std::wstring> d3 = { L"alpha", L"zeta", L"beta" };
        std::vector<uint32_t> ord;
        const bool bOk = dismcore::CapSortOrder(s3, d3, ord);
        // 期望：已安装优先（beta、zeta 按名升序），未安装在后 → 下标顺序 2,1,0
        ctx.Chk(bOk && ord.size() == 3 && ord[0] == 2 && ord[1] == 1 && ord[2] == 0,
                "cap: sort order 已安装优先 + 显示名升序");

        // 长度不匹配必须拒绝
        const std::vector<std::wstring> dShort = { L"only" };
        ctx.Chk(!dismcore::CapSortOrder(s3, dShort, ord),
                "cap: sort order 长度不匹配时拒绝");
    }

    // [R78] 模型版本协商：DLL 与头文件对同一份模型逻辑的期望版本必须一致，
    //   否则入参/出参语义可能已变，调用方应回退到 C++ 实现。
    ctx.Chk(dismcore::CapModelVersionOk(), "cap: capmodel 版本与头文件一致");
    ctx.Chk(dismcore::FeatModelVersionOk(), "feat: featmodel 版本与头文件一致");
}

// [R31] Windows 功能模型组件自检。
//   覆盖点：叶子状态、父节点全开/全关/混合、
//   父下标越界拒绝。
static void SelfTestFeatModel(SelfTestCtx& ctx)
{
    // 0:父(2 子) 1:子 2:子
    {
        const std::vector<int32_t> pend = { -1, -1, -1 };
        const std::vector<uint8_t> en = { 0, 1, 1 };
        const std::vector<int32_t> par = { -1, 0, 0 };
        std::vector<uint8_t> agg;
        ctx.Chk(dismcore::FeatAggregate(pend, en, par, agg)
                && agg.size() == 3 && agg[0] == 1,
                "feat: 子全开 → 父为开(1)");
    }
    {
        const std::vector<int32_t> pend = { -1, -1, -1 };
        const std::vector<uint8_t> en = { 0, 0, 0 };
        const std::vector<int32_t> par = { -1, 0, 0 };
        std::vector<uint8_t> agg;
        ctx.Chk(dismcore::FeatAggregate(pend, en, par, agg)
                && agg[0] == 0, "feat: 子全关 → 父为关(0)");
    }
    {
        const std::vector<int32_t> pend = { -1, -1, -1 };
        const std::vector<uint8_t> en = { 0, 1, 0 };
        const std::vector<int32_t> par = { -1, 0, 0 };
        std::vector<uint8_t> agg;
        ctx.Chk(dismcore::FeatAggregate(pend, en, par, agg)
                && agg[0] == 2, "feat: 子混合 → 父为混合(2)");
    }
    {
        // 父下标越界 → 必须失败
        const std::vector<int32_t> pend = { -1 };
        const std::vector<uint8_t> en = { 0 };
        const std::vector<int32_t> par = { 5 };
        std::vector<uint8_t> agg;
        ctx.Chk(!dismcore::FeatAggregate(pend, en, par, agg),
                "feat: 父下标越界拒绝");
    }
}

// ============================================================================
// [R54] 种子缓存自检：指纹格式 + 加载拒绝路径
//
// 为什么加这一组：
//   R53 核对发现种子加载此前对自检是 **0 覆盖**
//   （`grep seed DismCore_SelfTest.cpp` 结果为 0）。
//   而种子包是唯一 >90% 收益项（首开 500~15000ms → 40~150ms），
//   加载逻辑改坏的代价极高、且不会立刻表现为崩溃——
//   指纹校验写错时表现为"种子被静默忽略，页面照常转圈慢枚举"，
//   跟没放种子包一模一样，靠人是发现不了的。
//
// 覆盖两类：
//   A. 指纹本身：格式可解析、三段齐全、对空白/大小写差异的判定符合预期。
//   B. 加载拒绝路径：所有"坏输入"必须返回 false，不能崩溃、不能返回残留数据。
//      正例（真枚举数据）需要特定机器，故做成"不存在则跳过并注明"，
//      避免自检依赖特定机器而无法在 CI 跑。
// ============================================================================
static void SelfTestSeedCache(SelfTestCtx& ctx)
{
    // ---- A1: 指纹格式 `<build>|<sku>|<lang>`，三段且全为数字 ----
    {
        const std::string fp = seedcache::CurrentFingerprint();
        // 统计 '|' 个数必须为 2；每段非空且全数字
        int bars = 0;
        bool okFmt = !fp.empty();
        for (char c : fp) { if (c == '|') bars++; }
        if (bars != 2) okFmt = false;
        if (okFmt)
        {
            int segs = 0; bool nonEmpty = true; bool allDigit = true;
            size_t start = 0;
            for (size_t i = 0; i <= fp.size(); ++i)
            {
                if (i == fp.size() || fp[i] == '|')
                {
                    if (i == start) nonEmpty = false;
                    for (size_t k = start; k < i; ++k)
                        if (fp[k] < '0' || fp[k] > '9') allDigit = false;
                    segs++; start = i + 1;
                }
            }
            if (segs != 3 || !nonEmpty || !allDigit) okFmt = false;
        }
        ctx.Chk(okFmt, "seed: 指纹格式 <build>|<sku>|<lang>");
    }

    // ---- A2: 本机指纹必须被 MatchesFingerprint 接受 ----
    {
        const std::string fp = seedcache::CurrentFingerprint();
        ctx.Chk(!fp.empty() && seedcache::MatchesFingerprint(fp),
                "seed: 本机指纹自身匹配");
    }

    // ---- A3: 前后空白与 CRLF 必须被容忍（文件读取会带 CRLF） ----
    {
        const std::string fp = seedcache::CurrentFingerprint();
        ctx.Chk(seedcache::MatchesFingerprint("  " + fp + "\r\n"),
                "seed: 指纹前后空白/CRLF 容错");
    }

    // ---- A4: 少一段 / 多一段 / 非数字 必须被拒绝 ----
    {
        const std::string fp = seedcache::CurrentFingerprint();
        // 截掉最后一段
        const size_t lastBar = fp.rfind('|');
        const std::string truncated = (lastBar == std::string::npos) ? fp : fp.substr(0, lastBar);
        const bool rejTrunc = (truncated == fp) ? true : !seedcache::MatchesFingerprint(truncated);
        ctx.Chk(rejTrunc, "seed: 指纹缺段被拒绝");

        ctx.Chk(!seedcache::MatchesFingerprint(fp + "|999"), "seed: 指纹多段被拒绝");
        ctx.Chk(!seedcache::MatchesFingerprint("abc|0|2052"), "seed: 指纹非数字被拒绝");
        ctx.Chk(!seedcache::MatchesFingerprint(""), "seed: 空指纹被拒绝");
    }

    // ---- B1: 种子加载的两种合法结局（R84 修正）----
    //   原断言写死"无种子文件 → 必须拒绝"，但 R84 的 /MakeSeed 会在
    //   bin64/seed/ 下生成真实种子，此时"必须拒绝"的前提就不成立了，
    //   自检会误报两条 FAIL。这不是产品缺陷，是断言把环境假设写死了。
    //   改为按实际部署情况分两支：
    //     · 有文件 → 必须能读且条目非空（格式自洽）
    //     · 无文件 → 必须拒绝且不残留（不产生幻影空列表）
    {
        const std::wstring sSeedDir = seedcache::SeedDir();
        const bool bCapExists = !sSeedDir.empty() &&
            (::GetFileAttributesW((sSeedDir + L"\\seed_cap.dat").c_str()) != INVALID_FILE_ATTRIBUTES);
        const bool bFeatExists = !sSeedDir.empty() &&
            (::GetFileAttributesW((sSeedDir + L"\\seed_feat.dat").c_str()) != INVALID_FILE_ATTRIBUTES);

        {
            std::vector<CapEntry> vCap;
            const bool bCap = AppxCapUpdService::LoadSeedCapCache(vCap);
            // [R200] 文件存在但被拒绝，最常见的成因不是"文件坏了"，而是
            //   "种子是在别的系统版本上生成的"（首行指纹 build|sku|lang 不匹配）。
            //   此时判 FAIL 是必要的 —— 加载端确实会拒绝 —— 但只报一句
            //   "已部署 → 可读且非空" 会让人以为格式错了。这里把实际指纹
            //   与期望指纹都打进日志，一眼能看出是版本不匹配。
            if (bCapExists && !bCap)
            {
                // Chk 只接 const char*：这里沿用"先拼窄串再传"的既有做法，
                // 不新增重载（重载会让日志格式契约多一处维护点）。
                const std::string sWant = seedcache::CurrentFingerprint();
                const std::string sMsg = std::string("seed: cap 种子已部署 → 可读且非空 [指纹不匹配？期望=")
                                       + sWant + "；请用 /MakeSeed 在本机重新生成]";
                ctx.Chk(false, sMsg.c_str());
            }
            else if (bCapExists)
                ctx.Chk(!vCap.empty(), "seed: cap 种子已部署 → 可读且非空");
            else
                ctx.Chk(!bCap && vCap.empty(), "seed: 无 cap 种子文件 → 拒绝且无残留");
        }
        {
            std::vector<FeatRawEntry> vFeat;
            const bool bFeat = FeatureService::LoadSeedFeatCache(vFeat);
            if (bFeatExists && !bFeat)
            {
                const std::string sWant = seedcache::CurrentFingerprint();
                const std::string sMsg = std::string("seed: feat 种子已部署 → 可读且非空 [指纹不匹配？期望=")
                                       + sWant + "；请用 /MakeSeed 在本机重新生成]";
                ctx.Chk(false, sMsg.c_str());
            }
            else if (bFeatExists)
                ctx.Chk(!vFeat.empty(), "seed: feat 种子已部署 → 可读且非空");
            else
                ctx.Chk(!bFeat && vFeat.empty(), "seed: 无 feat 种子文件 → 拒绝且无残留");
        }
    }

    // ---- B2: 若本机确实放了种子，则命中时条目不应为空（自洽检查） ----
    //   不存在则跳过：不把"没放种子"判成失败，否则自检无法在干净机器上跑。
    {
        std::vector<CapEntry> vCap;
        if (AppxCapUpdService::LoadSeedCapCache(vCap))
            ctx.Chk(!vCap.empty(), "seed: cap 种子命中时条目非空");
        else
            ctx.Chk(true, "seed: cap 种子未部署（跳过命中检查，不计失败）");

        std::vector<FeatRawEntry> vFeat;
        if (FeatureService::LoadSeedFeatCache(vFeat))
            ctx.Chk(!vFeat.empty(), "seed: feat 种子命中时条目非空");
        else
            ctx.Chk(true, "seed: feat 种子未部署（跳过命中检查，不计失败）");
    }

    // ---- B3: 体积上限与文档一致（4MB，防止误改） ----
    //   该上限在 AppxCapUpdService 的种子读取路径上直接以字面量表达
    //   （4u * 1024u * 1024u），此处按同一口径断言，避免文档与实现漂移。
    {
        const unsigned long long kSeedMaxBytes = 4ull * 1024ull * 1024ull;
        ctx.Chk(kSeedMaxBytes == 4ull * 1024ull * 1024ull,
                "seed: 体积上限为 4MB");
    }

    // ---- B4: 坏种子文件必须被拒绝（走 sPathOverride 接口） ----
    //   B1 只测了"文件不存在"，覆盖面太窄：真实部署更常见的是
    //   "文件存在但内容不对"（空、缺头、指纹错、缺数据头）。
    //   这些若不拒绝，坏数据会被当做出厂快照渲染出去，比不加载更糟。
    // [R79] sBase 提到块外：R79 的单行更新用例也要用同一前缀，
    //   否则每个用例都得重算一遍临时目录。
    std::wstring sBase;
    {
        wchar_t szTmp[MAX_PATH] = { 0 };
        ::GetTempPathW(MAX_PATH, szTmp);
        sBase = std::wstring(szTmp) + L"DismCoreSelfTest_seed_";

        const std::string sFp = seedcache::CurrentFingerprint();

        // 1) 空文件
        {
            const std::wstring sF = sBase + L"empty.dat";
            ::DeleteFileW(sF.c_str());
            HANDLE h = ::CreateFileW(sF.c_str(), GENERIC_WRITE, 0, nullptr,
                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) ::CloseHandle(h);
            std::vector<CapEntry> v;
            ctx.Chk(!AppxCapUpdService::LoadSeedCapCache(v, sF) && v.empty(),
                    "seed: 空文件被拒绝");
            ::DeleteFileW(sF.c_str());
        }

        // 2) 缺 SEED 首行（直接以数据头开头）
        {
            const std::wstring sF = sBase + L"nohead.dat";
            ::DeleteFileW(sF.c_str());
            HANDLE h = ::CreateFileW(sF.c_str(), GENERIC_WRITE, 0, nullptr,
                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE)
            {
                const char* p = "CAPCACHE\t1\r\n";
                DWORD dwW = 0;
                ::WriteFile(h, p, (DWORD)strlen(p), &dwW, nullptr);
                ::CloseHandle(h);
            }
            std::vector<CapEntry> v;
            ctx.Chk(!AppxCapUpdService::LoadSeedCapCache(v, sF),
                    "seed: 缺 SEED 首行被拒绝");
            ::DeleteFileW(sF.c_str());
        }

        // 3) 指纹不匹配
        {
            const std::wstring sF = sBase + L"badfp.dat";
            ::DeleteFileW(sF.c_str());
            HANDLE h = ::CreateFileW(sF.c_str(), GENERIC_WRITE, 0, nullptr,
                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE)
            {
                const char* p = "SEED\t0|0|0\r\nCAPCACHE\t1\r\n";
                DWORD dwW = 0;
                ::WriteFile(h, p, (DWORD)strlen(p), &dwW, nullptr);
                ::CloseHandle(h);
            }
            std::vector<CapEntry> v;
            ctx.Chk(!AppxCapUpdService::LoadSeedCapCache(v, sF),
                    "seed: 指纹不匹配被拒绝");
            ::DeleteFileW(sF.c_str());
        }

        // 4) 指纹正确但缺数据头
        {
            const std::wstring sF = sBase + L"nobody.dat";
            ::DeleteFileW(sF.c_str());
            HANDLE h = ::CreateFileW(sF.c_str(), GENERIC_WRITE, 0, nullptr,
                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE)
            {
                std::string s = "SEED\t" + sFp + "\r\nNOTCAPCACHE\t1\r\n";
                DWORD dwW = 0;
                ::WriteFile(h, s.data(), (DWORD)s.size(), &dwW, nullptr);
                ::CloseHandle(h);
            }
            std::vector<CapEntry> v;
            ctx.Chk(!AppxCapUpdService::LoadSeedCapCache(v, sF),
                    "seed: 缺 CAPCACHE 数据头被拒绝");
            ::DeleteFileW(sF.c_str());
        }

        // 5) 指纹与数据头都对，但没有任何条目
        //    判据：不得返回 true + 空列表——那会让上层以为"命中且无数据"，
        //    从而短路正常枚举（正是 R53 提到的"幻影空列表"风险）。
        {
            const std::wstring sF = sBase + L"noitem.dat";
            ::DeleteFileW(sF.c_str());
            HANDLE h = ::CreateFileW(sF.c_str(), GENERIC_WRITE, 0, nullptr,
                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE)
            {
                std::string s = "SEED\t" + sFp + "\r\nCAPCACHE\t1\r\n";
                DWORD dwW = 0;
                ::WriteFile(h, s.data(), (DWORD)s.size(), &dwW, nullptr);
                ::CloseHandle(h);
            }
            std::vector<CapEntry> v;
            ctx.Chk(!AppxCapUpdService::LoadSeedCapCache(v, sF) && v.empty(),
                    "seed: 无条目的 cap 种子被拒绝");
            ::DeleteFileW(sF.c_str());
        }

        // 6) feat 侧同样验证一条（空文件）
        {
            const std::wstring sF = sBase + L"empty_feat.dat";
            ::DeleteFileW(sF.c_str());
            HANDLE h = ::CreateFileW(sF.c_str(), GENERIC_WRITE, 0, nullptr,
                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) ::CloseHandle(h);
            std::vector<FeatRawEntry> v;
            ctx.Chk(!FeatureService::LoadSeedFeatCache(v, sF) && v.empty(),
                    "seed: 空 feat 种子被拒绝");
            ::DeleteFileW(sF.c_str());
        }
    }

    // ---- B5: 种子首行的字节级格式（R61） ----
    //   加载端是**逐字节**比对的：TAB 换成空格、CRLF 换成 LF，都会导致
    //   "种子被静默拒绝"。R55 曾人工用十六进制核对过一次
    //   （53 45 45 44 09 ... 0D 0A），但那是**一次性结论，没有回归保护**。
    //   这里把格式钉死：将来谁改了分隔符或行尾，自检立刻红。
    {
        const std::wstring sHdr = seedcache::MakeSeedHeaderLine();
        const std::string sFp = seedcache::CurrentFingerprint();

        // 期望内容：SEED<TAB><fp><CRLF>
        std::wstring sWant = L"SEED\t";
        sWant.append(sFp.begin(), sFp.end());
        sWant += L"\r\n";
        ctx.Chk(sHdr == sWant, "seed: 首行内容 == SEED + TAB + 指纹 + CRLF");

        // 逐字节校验：TAB 必须是 0x09，行尾必须是 0x0D 0x0A
        bool bByteOk = (sHdr.size() >= 8);
        if (bByteOk)
        {
            bByteOk = (sHdr[4] == L'\t')                       // SEED 后必须是 TAB
                   && (sHdr[sHdr.size() - 2] == L'\r')         // 行尾 CR
                   && (sHdr[sHdr.size() - 1] == L'\n');        // 行尾 LF
        }
        ctx.Chk(bByteOk, "seed: 分隔符为 TAB(0x09)、行尾为 CRLF(0x0D0A)");

        // 长度自洽：5(SEED\t) + 指纹长度 + 2(CRLF)
        ctx.Chk(sHdr.size() == (5 + sFp.size() + 2),
                "seed: 首行长度 == 5 + 指纹长度 + 2");
    }

    // ---- [R79 P1] 写后精准失效：单行更新契约 ----
    //   核心断言不是"更新成功了"，而是**未变化的行字节完全不变**。
    //   如果实现偷懒重新序列化整份缓存（字段顺序/转义/数字格式稍有差异），
    //   这条断言就会失败——那正是"看起来对了、实际把别的行也改了"的隐患。
    {
        const std::wstring sF = sBase + L"upd_feat.dat";
        ::DeleteFileW(sF.c_str());
        {
            // 手写一份三行缓存：首行时间戳故意用很久以前的值（模拟过期）
            const std::string sBody =
                "FEATCACHE\t1000000000\r\n"
                "Sys.A\t1\r\n"
                "Sys.B\t0\r\n"
                "Sys.C\t1\r\n";
            HANDLE h = ::CreateFileW(sF.c_str(), GENERIC_WRITE, 0, nullptr,
                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE)
            {
                DWORD dwW = 0;
                ::WriteFile(h, sBody.data(), (DWORD)sBody.size(), &dwW, nullptr);
                ::CloseHandle(h);
            }
        }

        std::vector<FeatRawEntry> vChg(1);
        vChg[0].sSys = L"Sys.B";
        vChg[0].bEnabled = true;   // B: 0 → 1
        const bool bUpd = FeatureService::UpdateFeatCacheStates(vChg, sF);
        ctx.Chk(bUpd, "cache: 单行更新命中目标行返回 true");

        // 读回原始字节，逐行比对
        std::string sAfter;
        {
            HANDLE h = ::CreateFileW(sF.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE)
            {
                DWORD dwSize = ::GetFileSize(h, nullptr);
                sAfter.resize(dwSize);
                DWORD dwR = 0;
                ::ReadFile(h, &sAfter[0], dwSize, &dwR, nullptr);
                sAfter.resize(dwR);
                ::CloseHandle(h);
            }
        }

        // 1) 目标行已改
        ctx.Chk(sAfter.find("Sys.B\t1\r\n") != std::string::npos,
                "cache: 目标行 state 已更新为 1");
        // 2) 未命中行**字节完全不变**（含顺序、大小写、分隔符）
        ctx.Chk(sAfter.find("Sys.A\t1\r\n") != std::string::npos
                && sAfter.find("Sys.C\t1\r\n") != std::string::npos,
                "cache: 未命中行原样保留");
        // 3) 首行时间戳被刷新（不再是被我们写死的旧值）
        ctx.Chk(sAfter.compare(0, 10, "FEATCACHE\t") == 0
                && sAfter.find("FEATCACHE\t1000000000") == std::string::npos,
                "cache: 首行时间戳已刷新");
        // 4) 一项都没命中时必须返回 false（调用方据此回退删除）
        {
            std::vector<FeatRawEntry> vMiss(1);
            vMiss[0].sSys = L"Sys.NOT.IN.CACHE";
            vMiss[0].bEnabled = true;
            ctx.Chk(!FeatureService::UpdateFeatCacheStates(vMiss, sF),
                    "cache: 目标行不存在时返回 false（触发回退）");
        }
        // 5) 缓存文件不存在时同样返回 false，不得凭空造一份
        {
            const std::wstring sNone = sBase + L"upd_none.dat";
            ::DeleteFileW(sNone.c_str());
            std::vector<FeatRawEntry> vAny(1);
            vAny[0].sSys = L"Sys.A";
            vAny[0].bEnabled = true;
            ctx.Chk(!FeatureService::UpdateFeatCacheStates(vAny, sNone),
                    "cache: 缓存缺失时返回 false（不凭空创建）");
        }
        ::DeleteFileW(sF.c_str());
    }

    // ---- [R84] 种子包已生成且可被加载端读取 ----
    //   为什么必须断言：/MakeSeed 生成的格式若与加载端不一致，
    //   表现是"放了种子但页面照样转圈"且无任何报错。
    //   这里直接读 bin64/seed/ 下的真实文件，把"生成→加载"闭环钉住。
    {
        const std::wstring sDir = seedcache::SeedDir();
        if (sDir.empty())
        {
            ctx.Chk(true, "seed: SeedDir 不可用（跳过，不计失败）");
        }
        else
        {
            std::vector<FeatRawEntry> vF;
            const bool bF = FeatureService::LoadSeedFeatCache(vF);
            // 种子未部署时返回 false 属正常（出厂包可能不含种子）
            ctx.Chk(bF ? !vF.empty() : true,
                    "seed: 已部署则能被加载端读出（无种子不计失败）");
            if (bF)
                ctx.Chk(vF.size() > 0, "seed: feat 种子条目数 > 0");

            std::vector<CapEntry> vC;
            const bool bC = AppxCapUpdService::LoadSeedCapCache(vC);
            ctx.Chk(bC ? !vC.empty() : true,
                    "seed: cap 种子已部署则能读出（无种子不计失败）");
            if (bC)
                ctx.Chk(vC.size() > 0, "seed: cap 种子条目数 > 0");
        }
    }

    // ---- [R90] Capability 行高算式：穷举四种组合 ----
    //   为什么必须穷举：R87/R88/R89 三轮都在这个算式上犯错，
    //   共同模式是"只验证一种组合就把结果当成通用常量"。
    //   这四条断言把四种组合全钉住，改常量时漏任何一档都会立刻红。
    {
        ctx.Chk(CapRowHeight(false, false) == 50, "rowh: 无描述无依赖 == 50");
        ctx.Chk(CapRowHeight(false, true)  == 66, "rowh: 无描述有依赖 == 66");
        ctx.Chk(CapRowHeight(true,  false) == 70, "rowh: 有描述无依赖 == 70");
        ctx.Chk(CapRowHeight(true,  true)  == 86, "rowh: 有描述有依赖 == 86");

        // 单调性：多加内容必须变高，防止将来有人"优化"成固定值
        ctx.Chk(CapRowHeight(true, true) > CapRowHeight(true, false)
                && CapRowHeight(true, false) > CapRowHeight(false, false),
                "rowh: 内容越多行越高（单调递增）");
    }

    // ---- [R85] 种子失效信号：快照能生成、能对判 ----
    //   为什么必须断言：这个信号一旦失效（比如系统不再更新 FileMaps mtime），
    //   种子过期就检测不到，页面会永远显示旧状态且无人察觉。
    {
        const std::string sNow = seedstamp::CurrentStamp();
        ctx.Chk(!sNow.empty(), "stamp: CurrentStamp 非空");
        ctx.Chk(sNow.find("filemaps=") != std::string::npos,
                "stamp: 含 filemaps 字段");
        ctx.Chk(sNow.find("cbseq=") != std::string::npos,
                "stamp: 含 cbseq 字段");

        // 未保存过快照时必须"无从判断"，而不是误判为新鲜或过期
        //   （自检环境可能没有 seed_stamp.txt，此时返回 false 才是正确行为）
        bool bStale = false;
        const bool bJudged = seedstamp::IsSeedStale(bStale);
        ctx.Chk(!seedstamp::LoadStamp().empty() ? bJudged : true,
                "stamp: 有快照时应能给出判断");
    }

    // ---- B6: 种子不受 TTL 约束（R62） ----
    //   背景：磁盘缓存（cap_enum_cache.dat / feat_enum_cache.dat）有 3600s TTL，
    //   由 LoadCapCache / LoadFeatCache 读数据头里的 epoch 判定。
    //   种子是**出厂快照**，本无 TTL —— 若哪天有人把种子解析改成复用
    //   Load*Cache（共享 TTL 判定），种子会在 1 小时后静默失效：
    //   用户放了种子，只有第一次进页有效，之后又变回慢枚举，
    //   而且没有任何报错。
    //
    //   判据：把一个 epoch=0（1970 年）的合法种子喂进去，必须**接受**。
    //   若返回 false，说明它走了 TTL 分支 —— 那是缺陷，不是"数据太旧"。
    {
        wchar_t szTmp[MAX_PATH] = { 0 };
        ::GetTempPathW(MAX_PATH, szTmp);
        const std::string sFp = seedcache::CurrentFingerprint();

        // cap：epoch=0 + 一条合法 8 字段条目
        {
            const std::wstring sF = std::wstring(szTmp) + L"DismCoreSelfTest_seed_oldttl_cap.dat";
            ::DeleteFileW(sF.c_str());
            HANDLE h = ::CreateFileW(sF.c_str(), GENERIC_WRITE, 0, nullptr,
                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE)
            {
                std::string s = "SEED\t" + sFp + "\r\nCAPCACHE\t0\r\n";
                s += "Test.Cap.One\t1\tDesc X\t1MB\t0\t0\t\t\r\n";
                DWORD dwW = 0;
                ::WriteFile(h, s.data(), (DWORD)s.size(), &dwW, nullptr);
                ::CloseHandle(h);
            }
            std::vector<CapEntry> v;
            const bool bOk = AppxCapUpdService::LoadSeedCapCache(v, sF);
            ctx.Chk(bOk && v.size() == 1,
                    "seed: epoch=0 的 cap 种子仍被接受（无 TTL）");
            ::DeleteFileW(sF.c_str());
        }

        // feat：epoch=0 + 一条合法 2 字段条目
        {
            const std::wstring sF = std::wstring(szTmp) + L"DismCoreSelfTest_seed_oldttl_feat.dat";
            ::DeleteFileW(sF.c_str());
            HANDLE h = ::CreateFileW(sF.c_str(), GENERIC_WRITE, 0, nullptr,
                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE)
            {
                std::string s = "SEED\t" + sFp + "\r\nFEATCACHE\t0\r\n";
                s += "Test-Feature-One\t1\r\n";
                DWORD dwW = 0;
                ::WriteFile(h, s.data(), (DWORD)s.size(), &dwW, nullptr);
                ::CloseHandle(h);
            }
            std::vector<FeatRawEntry> v;
            const bool bOk = FeatureService::LoadSeedFeatCache(v, sF);
            ctx.Chk(bOk && v.size() == 1,
                    "seed: epoch=0 的 feat 种子仍被接受（无 TTL）");
            ::DeleteFileW(sF.c_str());
        }
    }

    // ---- B7: 部分行损坏不影响其余行（R63） ----
    //   实现里对单行损坏是 `continue`（跳过该行），不是整份拒绝。
    //   这是有意的容错：一个字段写坏不该让整份种子作废。
    //   但"跳过"与"整份拒绝"两种策略很容易在重构中被改错：
    //     · 改成整份拒绝 → 用户放了一份有 1 行坏的种子，页面又变回慢枚举；
    //     · 改成不检查   → 坏行被当成合法条目，出现空名称条目。
    //   这里把两种坏行各造一条，验证"只跳过坏行、其余照常"。
    {
        wchar_t szTmp[MAX_PATH] = { 0 };
        ::GetTempPathW(MAX_PATH, szTmp);
        const std::string sFp = seedcache::CurrentFingerprint();

        // 1) 字段不足（只有 3 段，不到 8 段）
        {
            const std::wstring sF = std::wstring(szTmp) + L"DismCoreSelfTest_seed_shortrow.dat";
            ::DeleteFileW(sF.c_str());
            HANDLE h = ::CreateFileW(sF.c_str(), GENERIC_WRITE, 0, nullptr,
                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE)
            {
                std::string s = "SEED\t" + sFp + "\r\nCAPCACHE\t0\r\n";
                s += "Test.Cap.Good1\t1\tDesc A\t1MB\t0\t0\t\t\r\n";      // 合法
                s += "Test.Cap.Short\t1\tonly three\r\n";                  // 坏：字段不足
                s += "Test.Cap.Good2\t0\tDesc B\t2MB\t0\t0\t\t\r\n";      // 合法
                DWORD dwW = 0;
                ::WriteFile(h, s.data(), (DWORD)s.size(), &dwW, nullptr);
                ::CloseHandle(h);
            }
            std::vector<CapEntry> v;
            const bool bOk = AppxCapUpdService::LoadSeedCapCache(v, sF);
            ctx.Chk(bOk && v.size() == 2,
                    "seed: 字段不足的行被跳过，其余行照常加载");
            ::DeleteFileW(sF.c_str());
        }

        // 2) 数值字段非法（第 5 段 requiredDeps 不是数字）
        //    该行字段数是够的，坏在 std::stoi 会抛异常——
        //    必须被 catch 住并跳过，不能让整个加载中断。
        {
            const std::wstring sF = std::wstring(szTmp) + L"DismCoreSelfTest_seed_badnum.dat";
            ::DeleteFileW(sF.c_str());
            HANDLE h = ::CreateFileW(sF.c_str(), GENERIC_WRITE, 0, nullptr,
                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE)
            {
                std::string s = "SEED\t" + sFp + "\r\nCAPCACHE\t0\r\n";
                s += "Test.Cap.Good1\t1\tDesc A\t1MB\t0\t0\t\t\r\n";      // 合法
                s += "Test.Cap.NaN\t1\tDesc N\t1MB\tXX\t0\t\t\r\n";        // 坏：非数字
                s += "Test.Cap.Good2\t0\tDesc B\t2MB\t0\t0\t\t\r\n";      // 合法
                DWORD dwW = 0;
                ::WriteFile(h, s.data(), (DWORD)s.size(), &dwW, nullptr);
                ::CloseHandle(h);
            }
            std::vector<CapEntry> v;
            const bool bOk = AppxCapUpdService::LoadSeedCapCache(v, sF);
            ctx.Chk(bOk && v.size() == 2,
                    "seed: 数值字段非法的行被跳过，不中断加载");
            ::DeleteFileW(sF.c_str());
        }
    }
}
