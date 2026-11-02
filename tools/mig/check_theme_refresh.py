"""Assert every page that builds controls in code also implements RefreshThemeColors.

Why this gate exists
--------------------
`new CButtonUI()` followed by `SetTextColor(tokens::Token(Ink))` stores the RESOLVED
ARGB, not a token reference. After a light<->dark switch the control keeps painting the
old colour until something re-derives it. The framework provides a per-page hook
(`PageBase.h` `virtual void RefreshThemeColors()`), and `FrameWnd::ApplySystemTheme()`
calls it for every built page.

Nothing checked that the hook was actually implemented. Round 30 finished with 14 pages
covered -- and missed `CPageFeature` entirely, because the coverage was tracked by hand.
A page that builds rows and colours them but does NOT implement the hook silently keeps
stale colours forever: no compile error, no runtime error, just wrong colours.

How "needs the hook" is decided (both must hold)
------------------------------------------------
1. the file builds a control in code  (`make_unique<C...UI>` / `new C...UI(`)
2. the file also sets a colour on a control  (`SetTextColor` / `SetBkColor` /
   `SetBorderColor` / `SetAttribute(L"*color*", ...)`)

A page that only uses skin XML does not need the hook -- the skin re-expands `{tok:...}`
on every `GetSkinFile()`, so its colours follow the theme by construction.

Exit: 0 every such page is covered, 1 gaps found, 2 input problem, 3 --inject blind.
"""

from __future__ import annotations

import argparse
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
APP = os.path.join(ROOT, "Demos", "Win8Metro")

BUILDS = re.compile(r"make_unique<C\w+UI>|new C\w+UI\(")
COLOURS = re.compile(
    r"SetTextColor|SetBkColor|SetBorderColor"
    r"|SetAttribute\(\s*L\"[\w]*(?:color|bkcolor|textcolor)\w*\""
)
CLASS_USE = re.compile(r"\b(CPage\w+)::")
IMPL = re.compile(r"void\s+(CPage\w+)::RefreshThemeColors\s*\(")


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def page_files():
    return sorted(glob.glob(os.path.join(APP, "Page_*.cpp")))


def scan(inject: bool = False):
    """Return (total_classes_needing_hook, covered, missing)."""
    files = page_files()
    if not files:
        return 0, 0, []

    needs = {}          # class -> [files]
    impl = set()

    for p in files:
        raw = open(p, "rb").read().decode("utf-8", errors="replace")
        body = strip_comments(raw)
        for m in IMPL.finditer(body):
            impl.add(m.group(1))
        if BUILDS.search(body) and COLOURS.search(body):
            cm = CLASS_USE.search(body)
            if cm:
                needs.setdefault(cm.group(1), []).append(os.path.basename(p))

    if inject:
        # Pretend one real implementation is missing, so the gate must report it.
        if impl:
            impl.discard(sorted(impl)[0])

    missing = [(c, needs[c]) for c in sorted(needs) if c not in impl]
    return len(needs), len(needs) - len(missing), missing


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--inject", action="store_true",
                    help="drop one real implementation; the gate must report it")
    args = ap.parse_args()

    total, covered, missing = scan(inject=args.inject)

    print("[INFO] page files=%d  classes needing the hook=%d  covered=%d"
          % (len(page_files()), total, covered))

    # Zero-input guard: scanning nothing must never read as success.
    if total == 0:
        print("[FAIL] no page class scanned -- refusing to pass")
        return 2

    if missing:
        print("[FAIL] %d page class(es) build controls and colour them but do NOT "
              "implement RefreshThemeColors():" % len(missing))
        for cls, where in missing:
            print("       %-22s (%s)" % (cls, ", ".join(where)))
        print("       these pages keep STALE colours after a light<->dark switch.")
        print("       add `void RefreshThemeColors() override;` and re-set the colours")
        print("       of the controls you created (no Build/Add/Remove -- see PageBase.h).")
        return 1

    if args.inject:
        print("[FAIL] inject NOT reported -- the gate is blind")
        return 3

    print("[PASS] every page that builds and colours controls implements "
          "RefreshThemeColors() (%d/%d)" % (covered, total))
    return 0


if __name__ == "__main__":
    sys.exit(main())
