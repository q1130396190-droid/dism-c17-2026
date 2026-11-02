// ============================================================
// markup_namechar_probe.cpp —— XML 名称字符集回归探针
//
// 为什么必须有这一条：
//   CMarkup::_SkipIdentifier 原实现只接受 '_'、':' 和字母数字，
//   把 '-' 和 '.' 排除在外。而 XML 规范里两者都是合法 NameChar。
//
//   实测后果（本轮在运行时日志里发现）：
//     DismData/Data.xml 用 <zh-Hans>/<zh-Hant> 做多语言标签，
//     解析器把 "zh-Hans" 拆成"元素 zh + 属性 Hans"，随后期望 '=' 却读到 '>',
//     报 "Error while parsing attributes"，
//     → 整个清理规则文件加载失败 → 空间回收页一条规则都没有。
//     日志表现为 "[engine] load failed"。
//
//   这个缺陷不会让程序崩溃，只会让核心功能静默失效 ——
//   所以必须有探针守着，否则修好之后仍可能被"清理"掉。
//
// 判据：
//   1) 含 '-' 的元素名可被正确解析，且标签名完整保留
//   2) 含 '.' 的元素名同样可用
//   3) 常规名称（不含特殊字符）行为不变
//   4) 真实规则文件 Data.xml / Data_Win11.xml 能完整加载
// ============================================================

#include "UIlib.h"

#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>

using namespace DuiLib;

namespace
{
int g_nPass = 0;
int g_nFail = 0;

void Mark(const char* s) { ::printf("[STEP] %s\n", s); ::fflush(stdout); }

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

// 用 CMarkup 解析一段 XML，返回是否成功
bool Parse(const wchar_t* pszXml, std::wstring* psErr = nullptr)
{
    CMarkup xml;
    std::wstring s(pszXml);
    // 必须显式指定 UNICODE 编码。默认值是 XMLFILE_ENCODING_UTF8，
    // 传宽字符缓冲进去会被当成 UTF-8 字节流，标签名解析直接失败
    // （探针首版就踩了这个，3 条 FAIL 全是探针自身问题，不是产品缺陷）。
    const bool bOk = xml.LoadFromMem(reinterpret_cast<BYTE*>(&s[0]),
                                    static_cast<DWORD>(s.size() * sizeof(wchar_t)),
                                    XMLFILE_ENCODING_UNICODE);
    if (!bOk && psErr != nullptr)
    {
        wchar_t szErr[256] = {};
        xml.GetLastErrorMessage(szErr, 255);
        *psErr = szErr;
    }
    return bOk;
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

    Mark("1 begin");

    // ---- 1) 连字符标签：正是 Data.xml 里 <zh-Hans> 的形态 ----
    {
        const wchar_t* psz =
            L"<Root>\r\n"
            L"  <Warning ID=\"KB1\">\r\n"
            L"    <en>Some games will not run properly.</en>\r\n"
            L"    <zh-Hans>将导致某些游戏无法正常运行。</zh-Hans>\r\n"
            L"    <zh-Hant>將導致某些遊戲無法正常執行。</zh-Hant>\r\n"
            L"  </Warning>\r\n"
            L"</Root>";
        std::wstring sErr;
        const bool bOk = Parse(psz, &sErr);
        Chk("连字符标签可解析", bOk,
            bOk ? L"<zh-Hans>/<zh-Hant> 通过" : (L"失败: " + sErr));
    }

    // ---- 2) 点号标签（XML 同样合法）----
    {
        const wchar_t* psz = L"<Root><item.Name>x</item.Name></Root>";
        std::wstring sErr;
        const bool bOk = Parse(psz, &sErr);
        Chk("点号标签可解析", bOk, bOk ? L"<item.Name> 通过" : (L"失败: " + sErr));
    }

    // ---- 3) 常规名称行为不变（回归保护）----
    {
        const wchar_t* psz = L"<Root><item attr=\"1\">x</item></Root>";
        std::wstring sErr;
        const bool bOk = Parse(psz, &sErr);
        Chk("常规标签不受影响", bOk, bOk ? L"<item attr> 通过" : (L"失败: " + sErr));
    }

    // ---- 4) 畸形 XML 仍应被拒绝（放宽字符集不能放宽校验）----
    {
        const wchar_t* psz = L"<Root><item attr>broken</item></Root>";
        std::wstring sErr;
        const bool bOk = Parse(psz, &sErr);
        Chk("缺等号的属性仍被拒绝", !bOk, bOk ? L"错误地接受了畸形 XML" : L"已拒绝");
    }

    // ---- 5) 真实规则文件加载 ----
    {
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

        const wchar_t* kFiles[] = { L"Data.xml", L"Data_Win11.xml" };
        for (const wchar_t* f : kFiles)
        {
            const std::wstring sPath = sRoot + L"\\bin64\\DismData\\" + f;

            FILE* fp = nullptr;
            if (::_wfopen_s(&fp, sPath.c_str(), L"rb") != 0 || fp == nullptr)
            {
                Chk("规则文件存在", false, sPath);
                continue;
            }
            ::fseek(fp, 0, SEEK_END);
            const long cb = ::ftell(fp);
            ::fseek(fp, 0, SEEK_SET);
            std::string buf(static_cast<size_t>(cb > 0 ? cb : 0), '\0');
            if (cb > 0) (void)::fread(&buf[0], 1, buf.size(), fp);
            ::fclose(fp);

            CMarkup xml;
            const bool bOk = xml.LoadFromMem(reinterpret_cast<BYTE*>(&buf[0]),
                                            static_cast<DWORD>(buf.size()));
            std::wstring sErr;
            if (!bOk)
            {
                wchar_t szErr[256] = {};
                xml.GetLastErrorMessage(szErr, 255);
                sErr = szErr;
            }
            Chk("真实规则文件可加载", bOk,
                std::wstring(f) + L" (" + std::to_wstring(cb) + L" bytes)" +
                (bOk ? L"" : (L" 失败: " + sErr)));
        }
    }

    ::printf("\n结果：%d 通过 / %d 失败\n", g_nPass, g_nFail);
    ::fflush(stdout);
    if (bInject)
    {
        Chk("injected-must-fail", false, L"self-test: forced failure");
    }
    return (g_nFail == 0) ? 0 : 1;
}
