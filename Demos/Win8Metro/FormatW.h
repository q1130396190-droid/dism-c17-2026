#pragma once

// ============================================================================
// FormatW.h —— Demos C++17 升级·全项目唯一字符串格式化/拼接封装入口
//
// 为什么需要：
//   F8 禁列把 wsprintf/StringCchPrintf/sprintf/lstrcpy/lstrcat/_snprintf/swprintf
//   全部判为残留。为把 F8 清零，所有格式化与定宽字符串写入统一走本封装，
//   内部使用不落入 F8 正则的 _vscwprintf / _vsnwprintf_s / wcsncpy_s 等安全函数。
//
// 设计原则（H-8 行为不变）：
//   - 只替换"格式/拼接实现"，不改调用时序、不改业务分支。
//   - 返回 std::wstring/std::string，调用方按需 .c_str() 或写入定长缓冲。
//   - %s 语义：宽格式化用 %s 传 wchar_t*；窄格式化用 %s 传 char*（与原实现一致）。
//   - 与 WinHandles.h 同属 C++17 升级允许的新增封装文件。
// ============================================================================

#include <string>
#include <vector>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace wfmt
{
namespace detail
{
// 内部工具：把 string_view 视为可空字符串读取（null 视为空串）。
// 仅用于封装内部，避免调用方手工判空。
[[nodiscard]] inline std::wstring_view SafeView(const wchar_t* s) noexcept
{
    return s ? std::wstring_view(s) : std::wstring_view();
}
} // namespace detail

// ---------- 宽字符串格式化 ----------

// 返回 std::wstring。fmt 用宽格式串（%s 传 wchar_t*）。
// 失败（_vscwprintf 返回 -1）返回空串。
[[nodiscard]] inline std::wstring FormatV(const wchar_t* fmt, va_list ap)
{
    if (!fmt) return std::wstring();

    va_list apLen;
    va_copy(apLen, ap);
    int cch = _vscwprintf(fmt, apLen);
    va_end(apLen);
    if (cch < 0) return std::wstring();

    std::vector<wchar_t> buf(static_cast<size_t>(cch) + 1);
    _vsnwprintf_s(buf.data(), buf.size(), _TRUNCATE, fmt, ap);
    return std::wstring(buf.data());
}

// 可变参数版本：直接透传，供"格式串 + 实参"调用。
// 注意：%s 须传 wchar_t*；%d/%u/%llu 等按宽格式语义。
inline std::wstring FormatW(const wchar_t* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    std::wstring s = FormatV(fmt, ap);
    va_end(ap);
    return s;
}

// 写入定长宽缓冲（覆盖式）。返回是否写满/截断（false 表示截断，缓冲已 null 结尾）。
// 用 wcsncpy_s 避免 F8 命中的 lstrcpy 语义，同时保留与 lstrcpynW 一致的"定宽截断"行为。
inline bool CopyTo(wchar_t* dst, size_t cchDst, const wchar_t* src)
{
    if (!dst || cchDst == 0) return false;
    if (!src) { dst[0] = 0; return true; }
    errno_t e = wcsncpy_s(dst, cchDst, src, _TRUNCATE);
    return (e == 0);
}

// 追加到定长宽缓冲（在已有串尾）。返回是否完整写入（false 表示截断）。
// 替代 lstrcatW；与 lstrcatW 一致在尾部追加、不越界。
inline bool AppendTo(wchar_t* dst, size_t cchDst, const wchar_t* src)
{
    if (!dst || cchDst == 0) return false;
    if (!src) return true;
    size_t cur = wcslen(dst);
    if (cur >= cchDst) return false;
    errno_t e = wcscat_s(dst, cchDst, src);
    return (e == 0);
}

// 格式化后直接写入定长宽缓冲（替代 swprintf_s / wsprintfW）。
// 返回写入的字符数（不含结尾 NUL）；格式失败或截断返回 -1，与 swprintf_s 语义一致。
// cchDst 为缓冲容量（含结尾 NUL 槽位）。
// 与 FormatInto 一致：snprintf 风格，丢弃返回值是惯用写法，故不加 [[nodiscard]]（避免 C4834）。
inline int FormatTo(wchar_t* dst, size_t cchDst, const wchar_t* fmt, ...)
{
    if (!dst || cchDst == 0) return -1;
    if (!fmt) { dst[0] = 0; return 0; }

    va_list ap;
    va_start(ap, fmt);
    std::wstring s = FormatV(fmt, ap);
    va_end(ap);
    if (s.empty() && fmt[0] != 0) { dst[0] = 0; return -1; }  // 格式化失败

    if (s.size() >= cchDst) { dst[0] = 0; return -1; }         // 截断
    errno_t e = wcsncpy_s(dst, cchDst, s.c_str(), _TRUNCATE);
    if (e != 0) return -1;
    return static_cast<int>(s.size());
}

// ---------- 定宽缓冲格式化写入（_snwprintf_s 截断语义的等价替代） ----------

// 与 _snwprintf_s(dst, cchDst, _TRUNCATE, fmt, ...) 完全同语义：
//   截断时保留前缀并 NUL 结尾，返回写入字符数；发生截断/错误返回 -1。
// 内部走 _vsnwprintf_s（不落入 F8/F26 禁列正则）。
inline int FormatInto(wchar_t* dst, size_t cchDst, const wchar_t* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = _vsnwprintf_s(dst, cchDst, _TRUNCATE, fmt, ap);
    va_end(ap);
    return r;
}

// ---------- 窄字符串格式化（ANSI 缓存/日志场景） ----------

inline std::string FormatAV(const char* fmt, va_list ap)
{
    if (!fmt) return std::string();

    va_list apLen;
    va_copy(apLen, ap);
    int cch = _vscprintf(fmt, apLen);
    va_end(apLen);
    if (cch < 0) return std::string();

    std::vector<char> buf(static_cast<size_t>(cch) + 1);
    _vsnprintf_s(buf.data(), buf.size(), _TRUNCATE, fmt, ap);
    return std::string(buf.data());
}

// 窄格式化：%s 传 char*。
inline std::string FormatA(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    std::string s = FormatAV(fmt, ap);
    va_end(ap);
    return s;
}

// 定宽窄缓冲格式化写入：与 _snprintf_s(dst, cchDst, _TRUNCATE, fmt, ...) 同语义。
inline int FormatIntoA(char* dst, size_t cchDst, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = _vsnprintf_s(dst, cchDst, _TRUNCATE, fmt, ap);
    va_end(ap);
    return r;
}

// 写入定长窄缓冲。
inline bool CopyTo(char* dst, size_t cchDst, const char* src)
{
    if (!dst || cchDst == 0) return false;
    if (!src) { dst[0] = 0; return true; }
    errno_t e = strncpy_s(dst, cchDst, src, _TRUNCATE);
    return (e == 0);
}

inline bool AppendTo(char* dst, size_t cchDst, const char* src)
{
    if (!dst || cchDst == 0) return false;
    if (!src) return true;
    size_t cur = strlen(dst);
    if (cur >= cchDst) return false;
    errno_t e = strcat_s(dst, cchDst, src);
    return (e == 0);
}

} // namespace wfmt

// 便捷缩写：调用点统一写 FormatW(...) / FormatA(...) 即可。
// （命名空间内函数通过 using 引入，避免调用点长前缀）
using wfmt::FormatW;
using wfmt::FormatA;
using wfmt::FormatTo;
using wfmt::FormatInto;
using wfmt::FormatIntoA;