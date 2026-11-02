#pragma once

// ============================================================
// OsVersionInfo.h —— 统一的系统版本识别（含 Win11 小版本映射）
//
// 为什么需要：
//   Data_Win11.xml 里有 130+ 处 <OSVersion> 适用性判据，其中大量以
//   build 号为粒度区分（22000=21H2 / 22621=22H2 / 22631=23H2 /
//   26100=24H2）。仅靠 "build >= 22000 就是 Win11" 的二元判断，无法
//   表达"这条规则仅适用于 24H2 及以上"这类语义，会在旧小版本上
//   显示并不适用的条目。
//
//   本文件提供：
//     * 一次 RtlGetVersion 取样（不受 manifest 兼容层影响）
//     * build → Win11 小版本（21H2/22H2/23H2/24H2）的可读名称
//     * 供 UI 展示与规则筛选共用的纯查询接口（无副作用、只取样一次）
//
// 说明：
//   * Win10 及更早只给出主版本名（Win10 / Win8.1 / Win8 / Win7），
//     不做小版本细分——Data 规则里对这些版本的判据都是 6.x / 10.0
//     级别的大跨度比较，细分无实际意义。
//   * 本头文件只依赖 <windows.h> 与 <string>，不引入 UIlib，
//     可在任何 TU 中安全包含。
// ============================================================

#include <windows.h>
#include <string>
#include <string_view>   // [C++17] 版本名片段比较（避免 C 风格字符串拼接）
#include "FormatW.h"     // 项目自有格式化封装（替代 swprintf 族）

namespace osver
{

// 版本快照（进程内只需取样一次）
struct Snapshot
{
    unsigned int nMajor;
    unsigned int nMinor;
    unsigned int nBuild;

    Snapshot() : nMajor(0), nMinor(0), nBuild(0) {}
};

// 读取真实系统版本。RtlGetVersion 不受 app 兼容性 manifest 影响，
// 这是与 Data 规则里 <OSVersion> 语义对齐的前提。
inline void FillSnapshot(Snapshot& s)
{
    using RtlGetVersionProc = LONG (WINAPI*)(OSVERSIONINFOEXW*);
    HMODULE hNtdll = ::GetModuleHandleW(L"ntdll.dll");
    RtlGetVersionProc pfn = hNtdll
        ? reinterpret_cast<RtlGetVersionProc>(::GetProcAddress(hNtdll, "RtlGetVersion"))
        : nullptr;

    OSVERSIONINFOEXW osv = {};   // 值初始化，替代 ZeroMemory
    osv.dwOSVersionInfoSize = sizeof(osv);

    if (pfn != nullptr && pfn(&osv) == 0)
    {
        s.nMajor = osv.dwMajorVersion;
        s.nMinor = osv.dwMinorVersion;
        s.nBuild = osv.dwBuildNumber;
    }
}

// 进程级版本快照（首次调用时取样，之后只读）。
// [STAB] 必须用函数内静态变量的一次性初始化（C++11 magic static，标准保证
// 线程安全）。早期写法 `static Snapshot s; static bool inited; if(!inited){...}`
// 是数据竞争：UI 线程、DismApiBridge 枚举线程、预热 detach 线程会并发首调
// Current()，两个线程可能同时 FillSnapshot 写同一对象；更糟的是先置位
// s_bInited 的线程可能还没写完字段，另一线程已读到半成品快照
// （nBuild=0 → 规则全被误判为不适用）。
inline const Snapshot& Current()
{
    static const Snapshot s_snap = []() -> Snapshot
    {
        Snapshot s;
        FillSnapshot(s);
        return s;
    }();
    return s_snap;
}

// [C++17] inline constexpr：Win11 各小版本的 GA build 号，
// 与 Data_Win11.xml 中的 <OSVersion> 阈值保持同名同义。
inline constexpr unsigned int kWin11Build21H2 = 22000;
inline constexpr unsigned int kWin11Build22H2 = 22621;
inline constexpr unsigned int kWin11Build23H2 = 22631;
inline constexpr unsigned int kWin11Build24H2 = 26100;
// [R201] 25H2 与 24H2 共享 26100 服务分支，GA build 为 26200。
//   证据：微软 KB 汇总把 "OS Builds 26200.x / 26100.x" 同时标为 25H2 与 24H2。
//   因此**不能**用 build 号单独区分 24H2/25H2 —— 必须优先读系统
//   DisplayVersion（HKLM\...\CurrentVersion）。build 表只在读取失败时兜底。
inline constexpr unsigned int kWin11Build25H2 = 26200;

// 是否为 Windows 11 或更高（build >= 22000）
inline bool IsWindows11OrLater()
{
    const Snapshot& s = Current();
    return s.nMajor > 10 || (s.nMajor == 10 && s.nBuild >= kWin11Build21H2);
}

// 版本等级（用于日志与诊断）：1=Win11 及以上，0=Win10 及更早。
// if constexpr 让"仅新版才有"的判定在编译期就能结构化表达。
[[nodiscard]] inline int VersionTier()
{
    const Snapshot& s = Current();
    if constexpr (sizeof(void*) == 8)
    {
        return (s.nMajor > 10 || (s.nMajor == 10 && s.nBuild >= kWin11Build21H2)) ? 1 : 0;
    }
    else
    {
        return (s.nMajor == 10 && s.nBuild >= kWin11Build21H2) ? 1 : 0;
    }
}

// Win11 小版本名称（按 build 区间映射）。
// 分界依据微软公开的 GA build 号；区间右端保持开放，
// 以便后续累积更新（如 26200.x）自动归入 25H2。
//
// [R201] 注意：本函数只依据 build，无法区分共享同一服务分支的两版
//   （24H2=26100 与 25H2=26200 的累积更新号互相重叠）。需要精确版本名时
//   用 BuildDisplayName()，它优先读系统 DisplayVersion。
[[nodiscard]] inline std::wstring_view Windows11ReleaseName(unsigned int nBuild)
{
    // [C++17] std::wstring_view 零拷贝返回字面量片段，调用方无需管理生命周期
    if (nBuild >= kWin11Build25H2) return std::wstring_view(L"25H2");
    if (nBuild >= kWin11Build24H2) return std::wstring_view(L"24H2");
    if (nBuild >= kWin11Build23H2) return std::wstring_view(L"23H2");
    if (nBuild >= kWin11Build22H2) return std::wstring_view(L"22H2");
    if (nBuild >= kWin11Build21H2) return std::wstring_view(L"21H2");
    return std::wstring_view();
}

// [R201] 读取系统 DisplayVersion（如 "25H2" / "24H2" / "22H2"）。
//   为什么需要它：build 号无法区分共享服务分支的版本，而 DisplayVersion
//   由系统自己维护，新增版本不需要改代码 —— 这是唯一能"面向未来"的来源。
//   只读 HKLM，不需要管理员权限；取值失败返回空串，由调用方按 build 兜底。
//
//   [C++17] 用函数内静态变量做一次性缓存：magic static 保证线程安全，
//   且 DisplayVersion 在一个进程生命周期内不会变（改了要重启才生效）。
[[nodiscard]] inline const std::wstring& DisplayVersionTag()
{
    static const std::wstring s_tag = []() -> std::wstring
    {
        HKEY hKey = nullptr;
        if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                0, KEY_READ, &hKey) != ERROR_SUCCESS)
            return std::wstring();

        wchar_t szBuf[32] = {};   // 版本串形如 "25H2"，实际最长 4~5 字符
        DWORD cb = sizeof(szBuf) - sizeof(wchar_t);
        DWORD dwType = 0;
        LONG lRc = ::RegQueryValueExW(hKey, L"DisplayVersion", nullptr, &dwType,
                                      reinterpret_cast<LPBYTE>(szBuf), &cb);
        ::RegCloseKey(hKey);
        if (lRc != ERROR_SUCCESS || (dwType != REG_SZ && dwType != REG_EXPAND_SZ))
            return std::wstring();

        // 只接受 "NNHN" 形式的短串（21H2/22H2/23H2/24H2/25H2…）：
        //   过滤掉 Insider 预览版可能写入的长描述，避免把日志/标题污染成散文。
        std::wstring s(szBuf);
        if (s.size() < 4 || s.size() > 8) return std::wstring();
        for (const wchar_t ch : s)
        {
            const bool bDigit = (ch >= L'0' && ch <= L'9');
            const bool bLetter = (ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z');
            if (!bDigit && !bLetter) return std::wstring();
        }
        // 首字符必须是数字（排除 "GA"、"RTM" 这类非版本串）
        if (s.front() < L'0' || s.front() > L'9') return std::wstring();
        return s;
    }();
    return s_tag;
}

// 完整可读版本名，如 "Windows 11 24H2 (26100)"、"Windows 10 (19045)"。
inline std::wstring BuildDisplayName(const Snapshot& s)
{
    // 用 std::wstring + wfmt::FormatInto 拼装，避免 swprintf 族（F8 禁列）
    // 与定长 char 缓冲（F13 禁列）。
    std::wstring sOut(64, L'\0');

    if (s.nMajor > 10 || (s.nMajor == 10 && s.nBuild >= 22000))
    {
        // [R201] 优先用系统 DisplayVersion（唯一能区分 24H2/25H2 的来源），
        //   读不到才退回 build 区间映射。
        const std::wstring& sSys = DisplayVersionTag();
        const std::wstring sRel = sSys.empty()
            ? std::wstring(Windows11ReleaseName(s.nBuild))
            : sSys;
        if (!sRel.empty())
        {
            wfmt::FormatInto(&sOut[0], sOut.size(), L"Windows 11 %s (%u)",
                             sRel.c_str(), s.nBuild);
        }
        else
        {
            wfmt::FormatInto(&sOut[0], sOut.size(), L"Windows 11 (%u)", s.nBuild);
        }
    }
    else if (s.nMajor == 10)
    {
        wfmt::FormatInto(&sOut[0], sOut.size(), L"Windows 10 (%u)", s.nBuild);
    }
    else if (s.nMajor == 6 && s.nMinor == 3)
    {
        wfmt::FormatInto(&sOut[0], sOut.size(), L"Windows 8.1 (%u)", s.nBuild);
    }
    else if (s.nMajor == 6 && s.nMinor == 2)
    {
        wfmt::FormatInto(&sOut[0], sOut.size(), L"Windows 8 (%u)", s.nBuild);
    }
    else if (s.nMajor == 6 && s.nMinor == 1)
    {
        wfmt::FormatInto(&sOut[0], sOut.size(), L"Windows 7 (%u)", s.nBuild);
    }
    else
    {
        wfmt::FormatInto(&sOut[0], sOut.size(), L"Windows %u.%u (%u)",
                         s.nMajor, s.nMinor, s.nBuild);
    }

    sOut.resize(wcslen(sOut.c_str()));
    return sOut;
}

// 完整可读版本名（进程级缓存，返回值由静态对象持有）
inline const wchar_t* DisplayName()
{
    static const std::wstring s_name = BuildDisplayName(Current());
    return s_name.c_str();
}

} // namespace osver
