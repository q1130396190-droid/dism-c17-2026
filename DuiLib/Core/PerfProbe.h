// [PERF-21] Sub-millisecond clock for startup instrumentation.
//
// Why this exists: every probe mark used ::GetTickCount64(), which is
// quantised to the 15.6ms timer tick. Sub-tick stages therefore all read
// "0ms" or "15/16ms", and the sum of the sub-stages could exceed the parent
// stage it belongs to (62ms of initwindow reported 110ms of children).
// Numbers that contradict each other are worse than no numbers, because they
// invite wrong optimisations.
//
// QPC is monotonic and free of that quantisation. Only used when the
// DUILIB_STARTUP_PROBE environment variable is set, so release behaviour and
// per-paint cost are untouched.
//
// Header-only and ASCII-only: it is included from GBK and UTF-8 translation
// units alike, so it must not carry non-ASCII bytes or link dependencies.
#ifndef DUILIB_PERF_PROBE_H
#define DUILIB_PERF_PROBE_H

#include <windows.h>
#include <cstdio>
#include <cstdlib>

namespace DuiLib {
namespace perfprobe {

struct CClock
{
    LARGE_INTEGER freq;
    LARGE_INTEGER start;

    CClock()
    {
        ::QueryPerformanceFrequency(&freq);
        ::QueryPerformanceCounter(&start);
        if (freq.QuadPart <= 0) freq.QuadPart = 1;
    }

    LARGE_INTEGER Now() const
    {
        LARGE_INTEGER n;
        ::QueryPerformanceCounter(&n);
        return n;
    }

    double MsSince(LARGE_INTEGER t0) const
    {
        LARGE_INTEGER n;
        ::QueryPerformanceCounter(&n);
        return static_cast<double>(n.QuadPart - t0.QuadPart) * 1000.0
             / static_cast<double>(freq.QuadPart);
    }

    // Exact duration between two captured stamps. MsSince(a) - MsSince(b) is NOT
    // the duration: each call samples its own "now", so the subtraction carries
    // that sampling skew and can even come out negative. This does the subtraction
    // on the stamps themselves.
    double Between(LARGE_INTEGER t0, LARGE_INTEGER t1) const
    {
        return static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0
             / static_cast<double>(freq.QuadPart);
    }

    // Process-lifetime singleton; construct-on-first-use is thread-safe in C++11.
    static CClock& Instance()
    {
        static CClock s;
        return s;
    }
};

// True when the caller asked for startup instrumentation.
inline bool Enabled()
{
    static const bool s_bOn =
        (::GetEnvironmentVariableW(L"DUILIB_STARTUP_PROBE", nullptr, 0) != 0);
    return s_bOn;
}

// One "tag <elapsed>ms" line on stdout. Deliberately narrow ASCII so logs stay
// readable in any console code page.
inline void Mark(const char* pszTag, LARGE_INTEGER t0)
{
    char sz[160] = {};
    ::sprintf_s(sz, "[perf] %-22s %.2fms", pszTag ? pszTag : "?",
                CClock::Instance().MsSince(t0));
    ::fputs(sz, stdout);
    ::fputc(10, stdout);
    ::fflush(stdout);
}

} // namespace perfprobe
} // namespace DuiLib

#endif // DUILIB_PERF_PROBE_H
