#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""Height rhythm analyzer for bin64/skin/Win8Metro (READ-ONLY).

Scans every .xml under bin64/skin/Win8Metro, extracts control tags with a
height attribute, filters out stretch/placeholder nodes, classifies each tag
into a UI role, and prints four tables (A/B/C/D).

This script never writes to the skin tree; it only prints to stdout.
"""
import os
import re
import sys
import collections

try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
SKIN = os.path.join(ROOT, "bin64", "skin", "Win8Metro")

CHROME = {"minbtn", "maxbtn", "restorebtn", "closebtn"}
TARGETS = {"button": 32, "edit": 32, "combo": 32, "tab": 40,
           "checkbox": 32, "header": 28}
ROLES = ["button", "edit", "combo", "tab", "checkbox", "header",
         "container", "misc", "chrome"]
MIN_RHYTHM = 4  # height <= 3 => separator / decoration, not rhythm

TAG_RE = re.compile(r"<\s*([A-Za-z][A-Za-z0-9_]*)([^>]*)>", re.S)
H_RE = re.compile(r'\bheight\s*=\s*"(\d+)"')
N_RE = re.compile(r'\bname\s*=\s*"([^"]*)"')
V_RE = re.compile(r'\bvisible\s*=\s*"false"')


def classify(tag, name):
    low = name.lower()
    if low in CHROME:
        return "chrome"
    if tag in ("Button", "OptionButton"):
        return "button"
    if tag in ("Edit", "RichEdit"):
        return "edit"
    if tag in ("Combo", "ComboBox"):
        return "combo"
    if tag in ("Option", "NavOption"):
        return "tab"
    if tag == "CheckBox":
        return "checkbox"
    if tag.endswith("Layout"):
        return "header" if "hdr" in low else "container"
    if tag in ("Text", "Progress", "Control"):
        return "misc"
    return None


def scan():
    recs, skipped, files = [], collections.Counter(), []
    for dp, dn, fn in os.walk(SKIN):
        dn.sort()
        for f in sorted(fn):
            if not f.lower().endswith(".xml"):
                continue
            path = os.path.join(dp, f)
            rel = os.path.relpath(path, ROOT).replace("\\", "/")
            files.append(rel)
            with open(path, "rb") as fh:
                src = fh.read().decode("utf-8", "replace")
            for m in TAG_RE.finditer(src):
                tag, attrs = m.group(1), m.group(2)
                hm = H_RE.search(attrs)
                nm = N_RE.search(attrs)
                name = nm.group(1) if nm else ""
                role = classify(tag, name)
                if role is None:
                    continue
                line = src.count("\n", 0, m.start()) + 1
                if V_RE.search(attrs):
                    skipped["visible=false"] += 1
                    continue
                if not hm:
                    skipped["height missing"] += 1
                    continue
                h = int(hm.group(1))
                if h == 0:
                    skipped["height=0"] += 1
                    continue
                recs.append({"file": rel, "line": line, "tag": tag,
                             "name": name, "h": h, "role": role})
    return recs, skipped, files


def bar(n, total):
    return "%5.1f%%" % (100.0 * n / total) if total else "  0.0%"


def main():
    recs, skipped, files = scan()
    total = len(recs)
    print("=" * 78)
    print("DuiLib Win8Metro height rhythm analysis (READ-ONLY)")
    print("=" * 78)
    print("scan root : %s" % SKIN)
    print("xml files : %d" % len(files))
    for f in files:
        print("            - %s" % f)
    print("kept tags : %d" % total)
    for k in ("visible=false", "height=0", "height missing"):
        print("excluded  : %-16s %d" % (k, skipped[k]))
    print()

    allh = collections.Counter(r["h"] for r in recs)

    print("-" * 78)
    print("A. RAW HEIGHT DISTRIBUTION (after filters)")
    print("-" * 78)
    print("%6s %6s %8s  %s" % ("height", "count", "pct", "note"))
    for h, c in sorted(allh.items(), key=lambda kv: (-kv[1], kv[0])):
        note = ""
        if h in (TARGETS["button"],):
            note = "Fluent 32 (button/edit/combo/checkbox)"
        elif h == TARGETS["tab"]:
            note = "Fluent 40 (row/large)"
        elif h == 24:
            note = "Fluent 24 (compact menu)"
        elif h == 28:
            note = "Fluent 28 (dense input)"
        elif h == 37:
            note = "titlebar chrome - excluded from rhythm"
        elif h <= 3:
            note = "separator/decoration"
        print("%6d %6d %8s  %s" % (h, c, bar(c, total), note))
    print()

    print("-" * 78)
    print("B. ROLE x HEIGHT x COUNT")
    print("-" * 78)
    byrole = collections.defaultdict(list)
    for r in recs:
        byrole[r["role"]].append(r)
    for role in ROLES:
        rows = byrole.get(role, [])
        if not rows:
            print("%-10s  (none)" % role)
            continue
        cnt = collections.Counter(x["h"] for x in rows)
        modes = [h for h, c in cnt.items() if c == max(cnt.values())]
        dist = ", ".join("%d:%d" % (h, c) for h, c in
                         sorted(cnt.items(), key=lambda kv: (-kv[1], kv[0])))
        print("%-10s total=%-3d  main=%-6s  %s"
              % (role, len(rows), "/".join(str(m) for m in sorted(modes)), dist))
    print()

    print("-" * 78)
    print("C. OUTLIERS (global height count <= 2)")
    print("-" * 78)
    out = [(h, c) for h, c in allh.items() if c <= 2]
    if not out:
        print("(none)")
    for h, c in sorted(out, key=lambda kv: (kv[0], kv[1])):
        print("height=%-4d count=%d" % (h, c))
        for r in recs:
            if r["h"] == h:
                print("    %-42s L%-5d %-14s %-12s %s"
                      % (r["file"], r["line"], r["tag"], r["role"],
                         r["name"] or "(anon)"))
    print()

    print("-" * 78)
    print("D. CONVERGENCE PROPOSAL (target per role)")
    print("-" * 78)
    print("%-10s %7s %9s %9s  %s"
          % ("role", "target", "to-change", "kept", "current distribution"))
    grand = 0
    for role in ["button", "edit", "combo", "tab", "checkbox", "header"]:
        rows = [r for r in byrole.get(role, []) if r["h"] >= MIN_RHYTHM]
        t = TARGETS[role]
        change = [r for r in rows if r["h"] != t]
        kept = [r for r in rows if r["h"] == t]
        cnt = collections.Counter(x["h"] for x in rows)
        dist = ", ".join("%d:%d" % (h, c) for h, c in
                         sorted(cnt.items(), key=lambda kv: (-kv[1], kv[0])))
        grand += len(change)
        print("%-10s %7d %9d %9d  %s"
              % (role, t, len(change), len(kept), dist or "(none)"))
    for role in ("container", "misc", "chrome"):
        rows = byrole.get(role, [])
        cnt = collections.Counter(x["h"] for x in rows)
        dist = ", ".join("%d:%d" % (h, c) for h, c in
                         sorted(cnt.items(), key=lambda kv: (-kv[1], kv[0])))
        print("%-10s %7s %9s %9s  %s"
              % (role, "n/a", "-", "-", dist or "(none)"))
    print()
    deco = [r for r in recs if r["h"] < MIN_RHYTHM]
    print("total height attributes to change : %d" % grand)
    print("separator/decoration (h<=3) skipped in D : %d" % len(deco))
    print("chrome (37px titlebar) records : %d" % len(byrole.get("chrome", [])))
    print()
    print("NOTE: list-row heights may be set in C++ (SetItemHeight/ItemHeight);")
    print("      XML-side statistics cannot observe those values.")


if __name__ == "__main__":
    main()
