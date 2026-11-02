#pragma once
// ============================================================================
//  AttrParse.h  --  DuiLib internal header (NOT exported, NOT ABI-relevant).
//
//  Purpose: host the C++17 comma/space-separated attribute parsers shared by
//  CControlUI::SetAttribute (S3 fast path + legacy chain) and every derived
//  control's SetAttribute.  This is the 阶段 2b (N2) delivery of 计划书 §2.2:
//  one helper set, no duplicated ad-hoc `_tcstol(pstr + 1, ...)` loops, and no
//  read past the terminating NUL when the value string is exhausted (the H5
//  class of malformed-XML out-of-bounds reads).
//
//  Design notes:
//    * Header-only, `inline` -> no exported symbols, no vtable/class layout
//      change, DUILIB_ABI_TAG untouched.
//    * Template on IntT so both `LONG` and `int` targets work (Windows LONG is
//      `long` on MSVC, distinct from `int`).
//    * Returns false as soon as the string is exhausted or no digit was
//      consumed; callers keep the previous default value and log via
//      DUI_LOGW (provided by DuiLib/Utils/UICrashGuard.h, included by UIlib.h).
// ============================================================================

#include <tchar.h>
#include <cwchar>

namespace DuiLib {
namespace attr {

// Consume one base-10 integer starting at *p.  Advances *p past the value and
// one separator (comma or space) so repeated calls walk a 4-/2-value string.
// Returns false when *p is null, empty, or holds no digits; the caller must
// then stop parsing and keep its default.  Never reads past the terminating NUL.
template <typename IntT>
inline bool NextInt(LPCTSTR& p, IntT& out)
{
    if (p == nullptr || *p == _T('\0')) {
        return false;
    }
    LPTSTR end = nullptr;
    long v = _tcstol(p, &end, 10);
    if (end == p) {
        return false;                     // no digits consumed
    }
    out = static_cast<IntT>(v);
    if (*end == _T('\0')) {
        p = end;                          // value consumed, string done
        return true;
    }
    p = ::CharNext(end);                  // skip one separator
    return true;
}

// Same contract as NextInt but for double values (attr_float percentages).
inline bool NextDouble(LPCTSTR& p, double& out)
{
    if (p == nullptr || *p == _T('\0')) {
        return false;
    }
    LPTSTR end = nullptr;
    out = _tcstod(p, &end);
    if (end == p) {
        return false;
    }
    if (*end == _T('\0')) {
        p = end;
        return true;
    }
    p = ::CharNext(end);
    return true;
}

// Parse "left,top,right,bottom" (or space-separated) into a RECT.
// Returns true only when all four integers parsed.
inline bool ParseRect4(LPCTSTR s, RECT& rc)
{
    LPCTSTR p = s;
    return NextInt(p, rc.left) && NextInt(p, rc.top) &&
           NextInt(p, rc.right) && NextInt(p, rc.bottom);
}

// Parse "cx,cy" (or space-separated) into a SIZE.
inline bool ParseSize2(LPCTSTR s, SIZE& sz)
{
    LPCTSTR p = s;
    return NextInt(p, sz.cx) && NextInt(p, sz.cy);
}

} // namespace attr
} // namespace DuiLib