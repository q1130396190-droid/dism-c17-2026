"""Feature-surface to gate matrix: which UI pages actually have assertions?

Counting "gates" is not the same as knowing what is verified. A page name appearing in a
probe list or a comment is documentation; only a probe that (a) names the page and
(b) emits an assertion mechanism is verification.

This tool answers one question per page: is there anything that would FAIL if the page
broke? It is deliberately strict, because the failure mode it guards against is a suite
that looks complete while no assertion ever mentions a given screen.

Exit: 0 every page has an asserting probe, 1 uncovered pages listed, 2 no pages found.
"""

from __future__ import annotations

import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PAGES_DIR = os.path.join(ROOT, "bin64", "skin", "Win8Metro", "pages")
PROBE_DIRS = [os.path.join(ROOT, "tools", "probes"), os.path.join(ROOT, "tools", "mig")]

# An assertion mechanism, in any of the dialects this repo uses.
ASSERT_PAT = re.compile(r"\[PASS\]|\[FAIL\]|\bCHK\s*\(|\bMark\s*\(|\bAssert\w*\s*\(")


def discover_pages():
    pages = []
    for p in sorted(glob.glob(os.path.join(PAGES_DIR, "page_*.xml"))):
        name = os.path.basename(p)[len("page_"):-len(".xml")]
        pages.append(name)
    return pages


def probe_files():
    out = []
    for d in PROBE_DIRS:
        for ext in ("*.cpp", "*.ps1", "*.py"):
            out += glob.glob(os.path.join(d, ext))
    return out


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    text = re.sub(r"^\s*#[^\n]*", " ", text, flags=re.M)
    return text


def main() -> int:
    pages = discover_pages()
    if not pages:
        print("[FAIL] no page_*.xml under " + PAGES_DIR)
        return 2

    probes = probe_files()
    print("[INFO] pages=%d probes=%d" % (len(pages), len(probes)))

    cover = {pg: [] for pg in pages}
    for p in probes:
        try:
            raw = open(p, "rb").read().decode("utf-8", errors="replace")
        except OSError:
            continue
        if not ASSERT_PAT.search(raw):
            continue                      # no assertion mechanism at all
        body = strip_comments(raw)        # mentions in comments are not coverage
        for pg in pages:
            if (re.search(r"\bpage_%s\b" % re.escape(pg), body)
                    or re.search(r"\bPage_%s\b" % pg.capitalize(), body)):
                cover[pg].append(os.path.basename(p))

    uncovered = [pg for pg in pages if not cover[pg]]
    for pg in pages:
        tag = ", ".join(sorted(set(cover[pg]))[:3]) if cover[pg] else "-- NONE --"
        print("       %-12s %s" % (pg, tag))

    print()
    if uncovered:
        print("[FAIL] %d/%d page(s) have no asserting probe: %s"
              % (len(uncovered), len(pages), ", ".join(uncovered)))
        return 1
    print("[PASS] all %d page(s) are named by at least one probe that asserts"
          % len(pages))
    print("[NOTE] 'named by an asserting probe' is a lower bound: it says some check")
    print("       mentions the page, not that every behaviour on it is checked.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
