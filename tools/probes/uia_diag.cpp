// [A11Y-3 diag] Read the exported navigation counters from DuiLib.dll.
//
// Purpose: "elements=1" from the UIA client can mean either "the walker never
// asked the provider" or "the provider said no children". These counters
// distinguish the two without guessing.
#include <windows.h>
#include <cstdio>

typedef long (*PFN_COUNT)(void);

int main(int argc, char** argv)
{
    const wchar_t* pszDll = (argc >= 2) ? L"DuiLib_diag.dll" : L"DuiLib.dll";
    HMODULE h = ::LoadLibraryW(pszDll);
    if (h == nullptr)
    {
        ::printf("[FAIL] LoadLibrary(%ls) err=%lu", pszDll, ::GetLastError());
        ::putchar(10);
        return 1;
    }
    auto pNav = reinterpret_cast<PFN_COUNT>(::GetProcAddress(h, "DuiLibUiaNav"));
    auto pChild = reinterpret_cast<PFN_COUNT>(::GetProcAddress(h, "DuiLibUiaChild0"));
    ::printf("nav=%ld child0=%ld", pNav ? pNav() : -1, pChild ? pChild() : -1);
    ::putchar(10);
    ::FreeLibrary(h);
    return 0;
}
