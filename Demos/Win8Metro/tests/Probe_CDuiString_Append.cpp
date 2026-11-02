// Behaviour probe for CDuiString::operator+=(LPCSTR / LPCWSTR).
//
// Why a runtime probe and not just dumpbin: the previous round verified that the two
// mangled names appear in the export table. Existence of a symbol proves it links; it
// says nothing about what it does. These overloads were added *by hand* this migration,
// so the conversion, the append semantics and the null case have never been executed.
//
// What is asserted:
//   1. appending a narrow string grows the wide storage with the right characters
//   2. appending a wide string works in the MBCS build (this file compiles both ways)
//   3. appending null and appending empty are NO-OPS, unlike operator= which clears
//   4. appending preserves the existing prefix (this is "+=", not "=")
//
// Exit code is the verdict: 0 only when every assertion passes.
// UIlib.h is the framework's public entry header; it pulls windows.h, tchar.h and the
// Utils/Utils.h declaration the probe needs. Including Utils.h alone fails because it
// assumes TCHAR and the Windows typedefs are already visible.
#include "UIlib.h"
#include <cstdio>

using DuiLib::CDuiString;

static int g_failed = 0;
static int g_total = 0;

static void check(bool ok, const char* what)
{
    ++g_total;
    if (!ok)
    {
        ++g_failed;
        ::printf("[FAIL] %s\n", what);
    }
    else
    {
        ::printf("[PASS] %s\n", what);
    }
}

int main()
{
    // --- 1. narrow append into the TCHAR storage -----------------------------
    // In the Unicode build LPCSTR != LPCTSTR, so this exercises operator+=(LPCSTR).
    {
        CDuiString s;
        s += "abc";
        check(s.GetLength() == 3, "narrow append: length is 3");
        // GetData() is LPCTSTR, so the comparison has to follow the build's character
        // width; comparing against the wrong one would silently be a tautology.
        check(_tcscmp(s.GetData(), _T("abc")) == 0,
              "narrow append: content is 'abc'");
    }

    // --- 2. append preserves the existing prefix -----------------------------
    {
        CDuiString s;
        s = "XY";
        s += "Z";
        bool ok = (s.GetLength() == 3);
        if (ok)
        {
            const TCHAR* p = s.GetData();
            ok = (p[0] == (TCHAR)'X') && (p[1] == (TCHAR)'Y') && (p[2] == (TCHAR)'Z');
        }
        check(ok, "append keeps the prefix (XY + Z == XYZ)");
    }

    // --- 3. null is a no-op, NOT a clear ------------------------------------
    // This is the semantic that differs from operator=, and the one a future reader is
    // most likely to "fix" into a clear by mistake.
    {
        CDuiString s;
        s = "keep";
        const LPCSTR pszNull = nullptr;
        s += pszNull;
        check(s.GetLength() == 4, "appending null does not change the string");
        check(_tcscmp(s.GetData(), _T("keep")) == 0,
              "appending null keeps the original content");
    }

    // --- 4. empty string is a no-op -----------------------------------------
    {
        CDuiString s;
        s = "keep";
        s += "";
        check(s.GetLength() == 4, "appending empty does not change the string");
    }

    // --- 5. wide append -----------------------------------------------------
    // In the MBCS build LPCWSTR != LPCTSTR, so this exercises operator+=(LPCWSTR).
    {
        CDuiString s;
        s = "pre";
        s += L"WIDE";
        check(s.GetLength() == 7, "wide append: length is 7");
    }

    // --- 6. operator=(LPCSTR) clears on null, unlike operator+=(LPCSTR) ------
    // This is the pair that actually differs, and the one a future refactor is most
    // likely to unify by mistake: assignment REPLACES (so null clears), append ADDS
    // (so null is a no-op). Both are exercised above and asserted here together.
    //
    // Note on operator=(LPCWSTR): Utils.h declares it only under #else (MBCS), so in the
    // Unicode build "s = (LPCWSTR)nullptr" binds operator=(LPCTSTR), whose contract is
    // "keep the existing value when the pointer is null". An earlier version of this
    // probe asserted "clears" and failed -- the probe was wrong, not the framework.
#ifdef _UNICODE
    {
        // Unicode only: LPCSTR is a distinct type here, so this binds the dedicated
        // operator=(LPCSTR) overload. In the MBCS build LPCSTR IS LPCTSTR and the call
        // would bind operator=(LPCTSTR) instead, whose contract is "keep on null" --
        // asserting "clears" there fails, which is what happened when this case was
        // written unconditionally.
        CDuiString s;
        s = "something";
        const LPCSTR pszNull = nullptr;
        s = pszNull;
        check(s.GetLength() == 0, "operator=(null narrow) clears, unlike operator+= which keeps");
    }
#else
    {
        // MBCS counterpart: the dedicated overload is operator=(LPCWSTR), same contract.
        CDuiString s;
        s = "something";
        const LPCWSTR pwszNull = nullptr;
        s = pwszNull;
        check(s.GetLength() == 0, "operator=(null wide) clears, unlike operator+= which keeps");
    }
#endif

    // --- 7. operator=(LPCSTR) converts and replaces --------------------------
    {
        CDuiString s;
        s = "old";
        s = "new";
        check(s.GetLength() == 3 && _tcscmp(s.GetData(), _T("new")) == 0,
              "operator=(narrow) replaces the whole value");
    }

    ::printf("\n[RESULT] %d/%d assertions passed\n", g_total - g_failed, g_total);
    return g_failed == 0 ? 0 : 1;
}
