// [GATE-COV] LogService + MountLedgerService read paths.
//
// Why: both were on the round-30 uncovered list. Neither needs COM or elevation,
// so they can be asserted for real instead of skipped.
//
// What is NOT tested:
//   * LogService::AppendToFile / AppendToFileBatch -- they write
//     action_log.jsonl, and a gate must not append to the developer's log.
//   * MountLedger add/remove/set -- they write HKCU. Only the Get* side runs here.
//
// Every assertion checks a value. The log-level parser is a pure function, so it
// can be tested exhaustively; the ledger reads are asserted for self-consistency.
#include "stdafx.h"
#include "LogService.h"
#include "MountLedgerService.h"

#include <cstdio>
#include <string>

static int g_nPass = 0;
static int g_nFail = 0;

static void Chk(const char* pszName, bool bOk, const std::string& sDetail)
{
    ::printf("[%s] %s | %s\n", bOk ? "PASS" : "FAIL", pszName, sDetail.c_str());
    ::fflush(stdout);
    if (bOk) ++g_nPass; else ++g_nFail;
}

int main(int argc, char** argv)
{
    bool bInject = false;
    for (int i = 1; i < argc; ++i)
        if (::lstrcmpiA(argv[i], "/Inject") == 0) bInject = true;

    // ============ LogService: level parsing is a pure function ============
    {
        struct Case { const wchar_t* line; int expect; };
        const Case cases[] = {
            { L"[ERROR] disk full",            LOGSRV_ERROR },
            { L"[error] lowercase tag",        LOGSRV_ERROR },
            { L"[WARN] retrying",              LOGSRV_WARN  },
            { L"[warn] lowercase tag",         LOGSRV_WARN  },
            { L"[INFO] started",               LOGSRV_INFO  },
            { L"plain text, no tag",           LOGSRV_INFO  },
            { L"",                             LOGSRV_INFO  },
            // The implementation uses Find() (any position), not a prefix match.
            // Every real caller passes lines it generated itself in
            // "[LEVEL] message" form, so the two behave identically in practice.
            // Asserting prefix-only semantics here would be testing a contract the
            // code never promised -- measured, not assumed.
            { L"see [ERROR] later",            LOGSRV_ERROR },
        };
        int nBad = 0;
        for (size_t i = 0; i < _countof(cases); ++i)
        {
            const int got = LogService::DetectLevel(cases[i].line);
            if (got != cases[i].expect) ++nBad;
        }
        char sz[160] = {};
        ::sprintf_s(sz, "cases=%d mismatched=%d", (int)_countof(cases), nBad);
        ::printf("[INFO] measured log levels: %s\n", sz);
        ::fflush(stdout);
        Chk("level parsing matches every documented case", nBad == 0, sz);
    }

    // ============ LogService: offline buffer is bounded ============
    {
        LogService svc;
        // The buffer drops the oldest beyond 300 entries; feeding 320 must keep
        // exactly 300 and keep the NEWEST ones.
        for (int i = 0; i < 320; ++i)
        {
            wchar_t line[32] = {};
            ::swprintf_s(line, L"line-%d", i);
            svc.BufferOffline(line);
        }
        const size_t nKept = svc.BufferedCount();
        const bool bNewest = (nKept > 0 && svc.BufferedAt(nKept - 1).Find(L"line-319") == 0);
        char sz[160] = {};
        ::sprintf_s(sz, "fed=320 kept=%d newest=%d", (int)nKept, bNewest ? 1 : 0);
        ::printf("[INFO] measured buffer: %s\n", sz);
        ::fflush(stdout);
        // [ZERO-INPUT] A buffer that keeps 0 entries would silently discard every
        // offscreen log line while still "working".
        // The implementation erases the oldest while size() > 300, so feeding 320
        // must leave EXACTLY 300. "kept > 0" would pass a buffer that dropped 319
        // of 320 lines -- which is the failure mode this buffer exists to prevent
        // (offscreen log lines silently discarded). Asserted as a value, not a range.
        Chk("offline buffer caps at exactly 300 entries", nKept == 300, sz);
        Chk("offline buffer keeps the newest entries", bNewest, sz);
    }

    // ============ MountLedgerService: HKCU reads ============
    {
        std::vector<std::wstring> vImages;
        const bool bImg = DismLedger::GetImageFiles(vImages);
        std::vector<std::wstring> vMounts, vWims;
        std::vector<DWORD> vIdx;
        const bool bMnt = DismLedger::GetMounted(vMounts, vWims, vIdx);
        const DWORD dwFlags = DismLedger::GetLastMountFlags();

        char sz[256] = {};
        ::sprintf_s(sz, "imgOk=%d images=%d mntOk=%d mounts=%d flags=0x%08lX",
                    bImg ? 1 : 0, (int)vImages.size(),
                    bMnt ? 1 : 0, (int)vMounts.size(),
                    (unsigned long)dwFlags);
        ::printf("[INFO] measured ledger: %s\n", sz);
        ::fflush(stdout);

        // ReadMulti returns false when the key does not exist, which is the
        // correct meaning of "no data yet" (a fresh profile has no MRU). What must
        // hold either way is that the call completes and leaves a usable container:
        // asserting "must return true" would fail on every clean machine and say
        // nothing about correctness.
        const bool bImgSane = (bImg == !vImages.empty());
        const bool bMntSane = (bMnt == !vMounts.empty());
        ::sprintf_s(sz, "imgOk=%d images=%d imgSane=%d mntOk=%d mounts=%d mntSane=%d",
                    bImg ? 1 : 0, (int)vImages.size(), bImgSane ? 1 : 0,
                    bMnt ? 1 : 0, (int)vMounts.size(), bMntSane ? 1 : 0);
        Chk("image MRU result is self-consistent", bImgSane, sz);
        Chk("mount list result is self-consistent", bMntSane, sz);

        // The three parallel arrays must agree, or the UI would pair a mount
        // directory with the wrong WIM.
        ::sprintf_s(sz, "mounts=%d wims=%d indices=%d",
                    (int)vMounts.size(), (int)vWims.size(), (int)vIdx.size());
        Chk("mount/wim/index lists agree in length",
            vMounts.size() == vWims.size() && vMounts.size() == vIdx.size(), sz);
    }

    if (bInject)
    {
        Chk("injected-must-fail", false, "self-test: forced failure");
    }

    ::printf("\nresult: %d pass / %d fail\n", g_nPass, g_nFail);
    return (g_nFail == 0) ? 0 : 1;
}
