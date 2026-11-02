// ============================================================================
//  P4 种子缓存公共实现（seed cache）
//
//  用途：随安装包分发的"出厂枚举快照"，让全新机器首次进页也能秒开列表，
//        不必等 3~8 秒的 DISM 枚举。
//
//  与磁盘缓存的区别：
//    * 磁盘缓存（feat_enum_cache.dat / cap_enum_cache.dat）
//        - 位于 %LOCALAPPDATA%\DismTool_Clone\
//        - 有 3600s TTL，过期即失效
//        - 由本机枚举结果写入，天然与实机匹配
//    * 种子缓存（seed_feat.dat / seed_cap.dat）
//        - 位于 exe 同级 seed\ 目录，随包分发、只读
//        - 无 TTL（出厂数据不会"过期"）
//        - **必须带版本指纹**：功能/能力清单与 OS build、SKU、UI 语言强相关，
//          指纹不匹配时禁止渲染 —— 宁可回到转圈，也不能显示幻影条目或漏项
//
//  指纹格式（首行）：
//    SEED\t<OSBUILD>|<SKU>|<LANG>\r\n
//      OSBUILD  osver::Current().nBuild（如 26100）
//      SKU      0=客户端(WinNT) 1=服务器(ServerNT/LanManNT)
//      LANG     GetUserDefaultUILanguage() 的 LANGID 低 16 位（如 0x0804 = zh-CN）
//  其后每行与磁盘缓存同构（FEATCACHE\t<epoch> / 条目行），
//  这样解析器可直接复用，无需第二套格式。
// ============================================================================
#include "SeedCache.h"

#include "OsVersionInfo.h"   // osver::Current().nBuild
#include "FormatW.h"         // FormatIntoA（避开 F8 禁列 wsprintf 族）
#include "WinHandles.h"      // win::RegKey RAII（避开 F11 裸 RegCloseKey）

#include <windows.h>
#include <string>
#include <string_view>       // [C++17] 指纹比对用零拷贝视图
#include <optional>          // [C++17] 可选值（与项目其它缓存解析一致）

namespace seedcache {

// [C++17] inline constexpr：缓冲尺寸以语义命名，避免魔数（F13 只查字面量下标）
inline constexpr int kProductTypeWchars = 64;
inline constexpr int kFingerprintChars  = 64;
inline constexpr int kMaxPathWchars     = MAX_PATH;

std::string CurrentFingerprint()
{
    // OS build：项目统一的版本快照（magic static，线程安全）
    const unsigned int nBuild = osver::Current().nBuild;

    // SKU：区分客户端/服务器。两者组件存储内容差异明显，必须进指纹。
    // 读 ProductType（HKLM 只读，不需要管理员权限）：
    //   WinNT             = 客户端
    //   ServerNT/LanManNT = 服务器
    // 用 win::RegKey RAII，句柄由析构关闭，不出现裸 RegCloseKey。
    unsigned int nSku = 0;
    {
        win::RegKey hKey;
        if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                L"SYSTEM\\CurrentControlSet\\Control\\ProductOptions",
                0, KEY_READ, hKey.out()) == ERROR_SUCCESS)
        {
            wchar_t szType[kProductTypeWchars] = { 0 };
            DWORD cb = sizeof(szType);
            DWORD dwType = 0;
            if (::RegQueryValueExW(hKey.get(), L"ProductType", nullptr, &dwType,
                    reinterpret_cast<BYTE*>(szType), &cb) == ERROR_SUCCESS
                && dwType == REG_SZ)
            {
                if (::lstrcmpiW(szType, L"WinNT") != 0) nSku = 1;
            }
        }
    }

    // UI 语言 LANGID：语言包决定 capability 的可选清单
    const unsigned int nLang =
        static_cast<unsigned int>(::GetUserDefaultUILanguage() & 0xFFFFu);

    char buf[kFingerprintChars] = { 0 };
    FormatIntoA(buf, sizeof(buf), "%u|%u|%u", nBuild, nSku, nLang);
    return std::string(buf);
}

bool MatchesFingerprint(const std::string& sLine)
{
    // [C++17] std::string_view：零拷贝比对，避免为 trim 再分配一次
    std::string_view sv(sLine);

    // 去首尾空白与 \r\n
    auto isSpace = [](char c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t'; };
    while (!sv.empty() && isSpace(sv.back())) sv.remove_suffix(1);
    while (!sv.empty() && isSpace(sv.front())) sv.remove_prefix(1);

    const std::string sWant = CurrentFingerprint();
    return sv == sWant;
}

// [R61] 种子首行："SEED\t<build>|<sku>|<lang>\r\n"
//   单一来源：自检与 /DumpSeedFingerprint 都调它，
//   避免两处各自拼一遍格式而逐渐分歧。
std::wstring MakeSeedHeaderLine()
{
    const std::string sFp = CurrentFingerprint();
    std::wstring s = L"SEED\t";
    s.append(sFp.begin(), sFp.end());
    s += L"\r\n";
    return s;
}

std::wstring SeedDir()
{
    // exe 同级 seed\ 目录
    wchar_t szExe[kMaxPathWchars] = { 0 };
    if (::GetModuleFileNameW(nullptr, szExe, kMaxPathWchars) == 0)
        return std::wstring();

    std::wstring sDir(szExe);
    const size_t p = sDir.find_last_of(L'\\');
    if (p != std::wstring::npos) sDir.resize(p);
    sDir += L"\\seed";
    return sDir;
}

} // namespace seedcache
