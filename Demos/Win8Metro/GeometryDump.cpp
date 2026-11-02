// Dump the laid-out control tree to a text file.
//
// Why this exists
// ---------------
// Eleven rounds of trying to read geometry out of screenshots produced two wrong answers
// and one abandoned tool (nav_w measured 575 where the truth is 233). The control tree
// already knows every rectangle exactly -- GetPos() after layout is the authority. Reading
// it is both simpler and correct, which is the whole argument for this file.
//
// The dump is written after the pages have been laid out (see the call site), because
// GetPos() before layout returns default rectangles and would silently report zeros.
//
// Format, one line per control, depth-first:
//
//   <depth>\t<name>\t<class>\t<L>\t<T>\t<R>\t<B>\t<visible>
//
// Coordinates are client pixels at the current DPI, so a caller comparing against a design
// must divide by the same scale it divides the screenshots by.
#include "GeometryDump.h"

#include "ThemeTokens.h"
#include "UIlib.h"

namespace geomdump
{

namespace
{

void Walk(DuiLib::CControlUI* p, int depth, std::string& out, int maxDepth, int maxNodes)
{
    if (p == nullptr || depth > maxDepth || static_cast<int>(out.size()) > maxNodes) return;

    const RECT rc = p->GetPos();
    const DuiLib::CDuiString sName = p->GetName();
    const DuiLib::CDuiString sClass = p->GetClass();

    // Narrow to UTF-8 once, here, so the consumer never has to guess the encoding.
    auto narrow = [](const DuiLib::CDuiString& s) -> std::string {
        const TCHAR* psz = s.GetData();
        if (psz == nullptr || psz[0] == 0) return std::string();
#ifdef _UNICODE
        const int cb = ::WideCharToMultiByte(CP_UTF8, 0, psz, -1, nullptr, 0, nullptr, nullptr);
        if (cb <= 1) return std::string();
        std::string r(static_cast<size_t>(cb - 1), static_cast<char>(0));
        ::WideCharToMultiByte(CP_UTF8, 0, psz, -1, &r[0], cb, nullptr, nullptr);
        return r;
#else
        return std::string(psz);
#endif
    };

    // Hold the narrowed strings in named locals. Passing narrow(...).c_str() straight
    // into sprintf_s destroys the temporary at the end of the full expression; reading
    // it afterwards is undefined behaviour and produced a 0-byte file.
    const std::string sNameU8 = narrow(sName);
    const std::string sClassU8 = narrow(sClass);

    // Sized buffer with _TRUNCATE instead of a 64-byte local: a long name would have
    // silently overflowed the old buffer and corrupted the columns.
    char szLine[512] = {};
    ::_snprintf_s(szLine, sizeof(szLine), _TRUNCATE,
                  "%d	%s	%s	%d	%d	%d	%d	%d\n",
                  depth, sNameU8.c_str(), sClassU8.c_str(),
                  static_cast<int>(rc.left), static_cast<int>(rc.top),
                  static_cast<int>(rc.right), static_cast<int>(rc.bottom),
                  p->IsVisible() ? 1 : 0);
    out += szLine;

    // Only containers have children; CControlUI::GetItemAt is not part of the base, so the
    // cast must be to the container type rather than assumed.
    if (DuiLib::CContainerUI* pBox = dynamic_cast<DuiLib::CContainerUI*>(p))
    {
        const int n = pBox->GetCount();
        for (int i = 0; i < n; ++i)
            Walk(pBox->GetItemAt(i), depth + 1, out, maxDepth, maxNodes);
    }
}

}   // namespace

bool DumpTree(DuiLib::CControlUI* pRoot, const wchar_t* pszPath, std::string* pOut)
{
    if (pRoot == nullptr || pszPath == nullptr) return false;

    std::string out;
    out.reserve(64 * 1024);
    out += "# depth\tname\tclass\tleft\ttop\tright\tbottom\tvisible\n";
    const HANDLE h = ::CreateFileW(pszPath, GENERIC_WRITE, 0, nullptr,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    // The file is opened BEFORE the walk on purpose. An earlier version walked first, so
    // a fault inside Walk produced no file at all and was indistinguishable from the
    // function never being called.
    Walk(pRoot, 0, out, 40, 2 * 1024 * 1024);

    DWORD dwW = 0;
    const bool ok = (::WriteFile(h, out.data(), static_cast<DWORD>(out.size()),
                                 &dwW, nullptr) != FALSE);
    ::CloseHandle(h);
    if (ok && pOut != nullptr) *pOut = out;
    return ok;
}

int CountNodes(DuiLib::CControlUI* pRoot)
{
    if (pRoot == nullptr) return 0;
    int n = 1;
    if (DuiLib::CContainerUI* pBox = dynamic_cast<DuiLib::CContainerUI*>(pRoot))
        for (int i = 0; i < pBox->GetCount(); ++i)
            n += CountNodes(pBox->GetItemAt(i));
    return n;
}

}   // namespace geomdump
