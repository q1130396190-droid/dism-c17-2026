#include "stdafx.h"

#include "CommandLineService.h"

#include <algorithm>
#include <cstdio>

// 静态成员定义
std::vector<std::pair<std::wstring, std::wstring>> CommandLineService::s_map;
std::vector<std::wstring> CommandLineService::s_positional;
bool CommandLineService::s_bParsed = false;

// ============================================================
// 通用工具
// ============================================================
namespace {

// 小写化
std::wstring ToLower(const std::wstring& s)
{
    std::wstring r = s;
    std::transform(r.begin(), r.end(), r.begin(),
        [](wchar_t c) { return (wchar_t)towlower(c); });
    return r;
}

// 去除首尾空白与引号
std::wstring TrimArg(const std::wstring& s)
{
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == L' ' || s[b] == L'\t' || s[b] == L'"' || s[b] == L'\'')) ++b;
    while (e > b && (s[e-1] == L' ' || s[e-1] == L'\t' || s[e-1] == L'"' || s[e-1] == L'\'')) --e;
    return s.substr(b, e - b);
}

} // namespace

// ============================================================
// Parse：扫描命令行，按空格切分 token，逐 token 提取 /key=value
// ============================================================
void CommandLineService::Parse(const wchar_t* pszCmdLine)
{
    Clear();
    if (!pszCmdLine || !pszCmdLine[0])
    {
        s_bParsed = true;
        return;
    }

    // 手动分词（避免依赖 CommandLineToArgvW 的引号规则）
    std::wstring cmd(pszCmdLine);
    size_t i = 0;
    const size_t n = cmd.size();
    while (i < n)
    {
        // 跳过空白
        while (i < n && (cmd[i] == L' ' || cmd[i] == L'\t'))
            ++i;
        if (i >= n) break;

        // 读一个 token（支持 "..." 引号内空格）
        std::wstring token;
        bool bInQuote = false;
        while (i < n)
        {
            wchar_t c = cmd[i];
            if (c == L'"')
            {
                bInQuote = !bInQuote;
                ++i;
                continue;
            }
            if (!bInQuote && (c == L' ' || c == L'\t'))
                break;
            token.push_back(c);
            ++i;
        }

        token = TrimArg(token);
        if (token.empty()) continue;

        // 识别开关：以 / 或 - 开头
        if (token[0] == L'/' || token[0] == L'-')
        {
            // 剥掉前缀
            size_t keyStart = 1;
            // 找 = 分割键值
            size_t eq = token.find(L'=', keyStart);
            std::wstring key, val;
            if (eq == std::wstring::npos)
            {
                key = token.substr(keyStart);
            }
            else
            {
                key = token.substr(keyStart, eq - keyStart);
                val = token.substr(eq + 1);
            }
            key = ToLower(key);
            val = TrimArg(val);

            // 合并同类键：保留最后一次值
            auto it = std::find_if(s_map.begin(), s_map.end(),
                [&](const std::pair<std::wstring, std::wstring>& p) {
                    return p.first == key;
                });
            if (it != s_map.end())
                it->second = val;
            else
                s_map.push_back(std::make_pair(key, val));
        }
        else
        {
            // [P0] KB57 基线：无开关位置参数（外壳动词的 %1 目标路径）。
            // lpCmdLine 不含 argv[0]，首个位置参数即用户路径。
            s_positional.push_back(token);
        }
    }

    s_bParsed = true;
}

bool CommandLineService::Has(const wchar_t* pszKey)
{
    if (!pszKey) return false;
    std::wstring k = ToLower(pszKey);
    return std::any_of(s_map.begin(), s_map.end(),
        [&](const std::pair<std::wstring, std::wstring>& p) {
            return p.first == k;
        });
}

std::wstring CommandLineService::Get(const wchar_t* pszKey)
{
    if (!pszKey) return std::wstring();
    std::wstring k = ToLower(pszKey);
    auto it = std::find_if(s_map.begin(), s_map.end(),
        [&](const std::pair<std::wstring, std::wstring>& p) {
            return p.first == k;
        });
    if (it == s_map.end()) return std::wstring();
    return it->second;
}

int CommandLineService::GetInt(const wchar_t* pszKey, int nDef)
{
    std::wstring v = Get(pszKey);
    if (v.empty()) return nDef;
    wchar_t* pEnd = nullptr;
    long r = wcstol(v.c_str(), &pEnd, 10);
    if (pEnd == v.c_str()) return nDef;
    return static_cast<int>(r);
}

// ============================================================
// 动作汇总
// ============================================================
EClrAction CommandLineService::GetPrimaryAction()
{
    if (Has(L"?") || Has(L"help") || Has(L"h"))
        return EClrAction::ECLR_HELP;
    if (Has(L"image"))
        return EClrAction::ECLR_IMAGE;
    if (Has(L"compactos"))
        return EClrAction::ECLR_COMPACTOS;
    // [R-B55] 资源管理器 compact 右键：/CompactEx=<path>（带目标目录）
    if (Has(L"compactex"))
        return EClrAction::ECLR_COMPACTEX;
    if (Has(L"restorehealth"))
        return EClrAction::ECLR_RESTOREHEALTH;
    if (Has(L"esd2iso"))
        return EClrAction::ECLR_ESD2ISO;      // [R-B28] 外壳右键 ESD→ISO
    if (Has(L"esd2wim"))
        return EClrAction::ECLR_ESD2WIM;      // [R-B28] 外壳右键 ESD→WIM
    if (Has(L"esddecrypt"))
        return EClrAction::ECLR_ESDDECRYPT;   // [R-B28] 外壳右键 ESD 解密
    // [R-B51] 三阶段自调用：/PreDownload|/Install|/Finalize 升级链阶段
    if (Has(L"predownload"))
        return EClrAction::ECLR_PREDOWNLOAD;
    if (Has(L"install"))
        return EClrAction::ECLR_INSTALL;
    if (Has(L"finalize"))
        return EClrAction::ECLR_FINALIZE;
    if (Has(L"c"))
        return EClrAction::ECLR_CLEAN;
    return EClrAction::ECLR_NONE;
}

// [R-B28] 外壳右键：取得 ESD 动作目标文件（%1 由注册表 command 传入）
bool CommandLineService::GetEsdTarget(DuiLib::CDuiString& sOut)
{
    std::wstring v = Get(L"esd2iso");
    if (v.empty()) v = Get(L"esd2wim");
    if (v.empty()) v = Get(L"esddecrypt");
    if (v.empty()) return false;
    // 外壳传入的是带双引号的 %1，这里统一剥掉
    if (v.size() >= 2 && v.front() == L'"' && v.back() == L'"')
        v = v.substr(1, v.size() - 2);
    sOut = v.c_str();
    return !sOut.IsEmpty();
}

// [R-B55] 资源管理器 compact 右键：取得目标目录（%1 由注册表 command 传入）
bool CommandLineService::GetCompactTarget(DuiLib::CDuiString& sOut)
{
    std::wstring v = Get(L"compactex");
    if (v.empty()) return false;
    // 外壳传入的可能是带双引号的 %1，剥掉
    if (v.size() >= 2 && v.front() == L'"' && v.back() == L'"')
        v = v.substr(1, v.size() - 2);
    sOut = v.c_str();
    return !sOut.IsEmpty();
}

// [P0] KB57 基线：/C /exe xpress4k "%1"
// 兼容两种写法：/exe=xpress4k "<path>" 与 /exe xpress4k "<path>"。
// 算法名若以独立位置参数出现则被消费，其余首个位置参数为目标路径。
bool CommandLineService::GetCompactExeTarget(DuiLib::CDuiString& sOut, EClrCompress& eAlg)
{
    sOut.Empty();
    eAlg = EClrCompress::ECLR_COMP_NONE;
    if (!Has(L"c") || !Has(L"exe") || s_positional.empty())
        return false;

    size_t iPath = 0;
    eAlg = GetCompress();
    if (eAlg == EClrCompress::ECLR_COMP_NONE && !s_positional.empty())
    {
        // /exe 未带 = 值时，算法名是紧随其后的位置参数
        std::wstring v = ToLower(s_positional[0]);
        if (v == L"xpress4k" || v == L"xpress8k" || v == L"xpress16k" ||
            v == L"lzx" || v == L"max")
        {
            if (v == L"xpress4k")  eAlg = EClrCompress::ECLR_COMP_XPRESS4K;
            if (v == L"xpress8k")  eAlg = EClrCompress::ECLR_COMP_XPRESS8K;
            if (v == L"xpress16k") eAlg = EClrCompress::ECLR_COMP_XPRESS16K;
            if (v == L"lzx")       eAlg = EClrCompress::ECLR_COMP_LZX;
            if (v == L"max")       eAlg = EClrCompress::ECLR_COMP_MAX;
            iPath = 1;
        }
    }
    if (eAlg == EClrCompress::ECLR_COMP_NONE || iPath >= s_positional.size())
        return false;

    std::wstring v = s_positional[iPath];
    if (v.size() >= 2 && v.front() == L'"' && v.back() == L'"')
        v = v.substr(1, v.size() - 2);
    sOut = v.c_str();
    return !sOut.IsEmpty();
}

// [P0] KB57 基线：/U "%1" —— /U 带位置参数时语义为"解压目标路径"，
// 不再解释为无人值守（FrameWnd 分发时先消费本动作）。
bool CommandLineService::GetUncompactTarget(DuiLib::CDuiString& sOut)
{
    sOut.Empty();
    if (!Has(L"u") || s_positional.empty())
        return false;
    std::wstring v = s_positional[0];
    if (v.size() >= 2 && v.front() == L'"' && v.back() == L'"')
        v = v.substr(1, v.size() - 2);
    sOut = v.c_str();
    return !sOut.IsEmpty();
}

size_t CommandLineService::GetPositionalCount()
{
    return s_positional.size();
}

// [R-B51] /path=：升级阶段状态文件目标目录/路径（主进程传入）
bool CommandLineService::GetUpdPhasePath(DuiLib::CDuiString& sOut)
{
    std::wstring v = Get(L"path");
    if (v.empty()) return false;
    // 外壳/命令串可能带双引号
    if (v.size() >= 2 && v.front() == L'"' && v.back() == L'"')
        v = v.substr(1, v.size() - 2);
    sOut = v.c_str();
    return !sOut.IsEmpty();
}

// [R-B51] /noreboot：安装阶段禁止自动重启（缺省 false）
bool CommandLineService::IsUpdNoReboot()
{
    return Has(L"noreboot");
}

EClrCompress CommandLineService::GetCompress()
{
    std::wstring v = ToLower(Get(L"exe"));
    if (v == L"xpress4k")  return EClrCompress::ECLR_COMP_XPRESS4K;
    if (v == L"xpress8k")  return EClrCompress::ECLR_COMP_XPRESS8K;
    if (v == L"xpress16k") return EClrCompress::ECLR_COMP_XPRESS16K;   // [R-B48]
    if (v == L"lzx")       return EClrCompress::ECLR_COMP_LZX;
    if (v == L"max")       return EClrCompress::ECLR_COMP_MAX;
    return EClrCompress::ECLR_COMP_NONE;
}

bool CommandLineService::IsUnattended()
{
    return Has(L"u") || Has(L"unattend") || Has(L"silent");
}

bool CommandLineService::IsSelectAll()
{
    return Has(L"all");
}

bool CommandLineService::HasImagePath(DuiLib::CDuiString& sOut)
{
    std::wstring v = Get(L"image");
    if (v.empty()) return false;
    sOut = v.c_str();
    return true;
}

bool CommandLineService::HasScratchDir(DuiLib::CDuiString& sOut)
{
    std::wstring v = Get(L"scratchdir");
    if (v.empty()) return false;
    sOut = v.c_str();
    return true;
}

bool CommandLineService::HasPluginDir(DuiLib::CDuiString& sOut)
{
    std::wstring v = Get(L"plugin");
    if (v.empty()) return false;
    sOut = v.c_str();
    return true;
}

bool CommandLineService::IsHelpRequested()
{
    return GetPrimaryAction() == EClrAction::ECLR_HELP;
}

// [R-B39] /FunSelfTest：?Get* 内嵌函数对拍单测开关
bool CommandLineService::IsFunSelfTest()
{
    return Has(L"FunSelfTest");
}

// [R31] /DismCoreSelfTest：Rust dismcore FFI 自检开关。
// 该自检此前从未被接线（文件也未注册进 vcxproj），是纯死代码；
// 接线后才真正可运行，也才能在 CI 里作为冒烟项使用。
bool CommandLineService::IsDismCoreSelfTest()
{
    return Has(L"DismCoreSelfTest");
}

// [R53] /DumpSeedFingerprint：只输出指纹，不拉 GUI。
//   供制作种子包的人直接复制首行，避免“猜指纹→加载被拒→无报错”的静默失败。
bool CommandLineService::IsDumpSeedFingerprint()
{
    return Has(L"DumpSeedFingerprint");
}

// [R81] 无界面基准入口。为什么需要：R78/R80 的优化结论全部来自代码结构推算，
//   "加载到底几秒"从未实测过（唯一产 ttfr 日志的路径要人工进页）。
//   这两个开关让枚举与渲染的耗时能在 CI/无人值守环境跑出来。
bool CommandLineService::IsBenchEnum()
{
    return Has(L"BenchEnum");
}

bool CommandLineService::IsBenchRender()
{
    return Has(L"BenchRender");
}

// [R84] 种子包生成入口。生成后立即用加载端回读，避免"生成了但读不了"。
bool CommandLineService::IsMakeSeed()
{
    return Has(L"MakeSeed");
}

// [R85] 种子失效信号探测。
bool CommandLineService::IsSeedProbe()
{
    return Has(L"SeedProbe");
}

const wchar_t* CommandLineService::GetHelpText()
{
    return
        L"Win8Metro / Dism++ 命令行用法\n"
        L"=============================\n"
        L"/C               启动后自动执行清理\n"
        L"/exe=<alg>       导出压缩算法: xpress4k | xpress8k | xpress16k | lzx | max\n"
        L"/C /exe <alg> \"<path>\"  按 KB57 基线对指定路径执行 EXE 压缩\n"
        L"                   alg = xpress4k | xpress8k | xpress16k | lzx | max\n"
        L"/U \"<path>\"      按 KB57 基线对指定路径递归解压\n"
        L"/U               无人值守（不带位置路径时；等价 /Unattend）\n"
        L"/ScratchDir=     设置临时工作目录\n"
        L"/Image=<path>    打开指定 WIM/ESD 映像（切到 ImageX）\n"
        L"/All             清理时全选所有规则\n"
        L"/Plugin=<dir>    插件目录\n"
        L"/CompactOS       执行 CompactOS 优化\n"
        L"/RestoreHealth   打开\"修复 Windows 映像\"\n"
        L"/lang=<code>     语言代码（zh-CN/en-US/ja-JP/ko-KR 等）\n"
        L"/DumpSeedFingerprint  输出本机种子指纹（SEED 首行用）\n"
        L"/? | /Help | /H  显示本帮助\n";
}

void CommandLineService::Clear()
{
    s_map.clear();
    s_positional.clear();
    s_bParsed = false;
}
// [C++17] 数字解析基数（GetInt 使用，原 L150 魔数常量化）
inline constexpr int kDecimalRadix = 10;
