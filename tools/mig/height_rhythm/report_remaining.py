#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""逐页高度节奏统计（height rhythm rollout 用）。

与 analyze.py 的区别：analyze.py 做全皮肤横向汇总，本脚本**按页面**输出
「标签 x 角色 x height 值」分布、梯级内占比与不同值个数，可直接用于改动前后的
before/after 对比。

统计口径（与 docs/height_rhythm_pilot.md 一致）：
  * 只统计 skin 页面 XML 中带 height="<纯数字>" 的标签
  * height="0"（可伸缩容器）不计入
  * height<=3（分隔线）不计入梯级占比，单独计数
  * 带 visible="false" 的隐藏控件不计入
  * 梯级阶梯 = {24, 28, 32, 40}（48 仅大触控，当前不用）

用法：
  python tools/mig/height_rhythm/report_remaining.py            # 人类可读
  python tools/mig/height_rhythm/report_remaining.py --json     # 机读
  python tools/mig/height_rhythm/report_remaining.py --page page_appx
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SKIN = ROOT / "bin64" / "skin" / "Win8Metro"
PAGES_DIR = SKIN / "pages"

# 本轮推广范围的 14 个页面（不含已完成的 program / optim）
PAGES = [
    "page_appx",
    "page_assoc",
    "page_cleanup",
    "page_control",
    "page_driver",
    "page_feature",
    "page_intl",
    "page_regclean",
    "page_startup",
    "page_tasks",
    "page_tools",
    "page_unattend",
    "page_update",
    "page_welcome",
]

LADDER = (24, 28, 32, 40)
CHROME = ("minbtn", "maxbtn", "restorebtn", "closebtn")

TAG_RE = re.compile(r"<\s*([A-Za-z][A-Za-z0-9_]*)([^>]*)>", re.S)
H_RE = re.compile(r'\bheight\s*=\s*"(\d+)"')
N_RE = re.compile(r'\bname\s*=\s*"([^"]*)"')
V_RE = re.compile(r'\bvisible\s*=\s*"false"')


def classify(tag: str, name: str) -> str:
    """标签 + name 推断角色。归类不清时返回 misc，调用方不得据此改动。"""
    low = name.lower()
    if low in CHROME:
        return "chrome"
    if tag in ("Button", "OptionButton"):
        return "button"
    if tag in ("Edit", "RichEdit"):
        return "edit"
    if tag == "Combo":
        return "combo"
    if tag in ("Option", "NavOption"):
        return "tab"
    if tag == "CheckBox":
        return "checkbox"
    if tag.endswith("Layout"):
        return "header" if "hdr" in low else "container"
    if tag in ("Text", "Progress", "Control", "List", "TreeView", "TabLayout"):
        return "misc"
    return "misc"


def scan_page(path: Path):
    text = path.read_text(encoding="utf-8", errors="replace")
    rows = []
    for m in TAG_RE.finditer(text):
        tag, attrs = m.group(1), m.group(2)
        if attrs.rstrip().endswith("/") or tag.startswith("/"):
            pass
        hm = H_RE.search(attrs)
        if not hm:
            continue
        if V_RE.search(attrs):
            continue
        h = int(hm.group(1))
        if h == 0:
            continue
        nm = N_RE.search(attrs)
        name = nm.group(1) if nm else ""
        line = text.count("\n", 0, m.start()) + 1
        rows.append(
            {
                "line": line,
                "tag": tag,
                "name": name,
                "role": classify(tag, name),
                "h": h,
            }
        )
    return rows


def summarize(rows):
    counted = [r for r in rows if r["h"] > 3]
    seps = len(rows) - len(counted)
    inl = [r for r in counted if r["h"] in LADDER]
    dist = {}
    for r in counted:
        dist[r["h"]] = dist.get(r["h"], 0) + 1
    by_role = {}
    for r in counted:
        d = by_role.setdefault(r["role"], {})
        d[r["h"]] = d.get(r["h"], 0) + 1
    ratio = (100.0 * len(inl) / len(counted)) if counted else 100.0
    return {
        "counted": len(counted),
        "seps": seps,
        "inladder": len(inl),
        "ratio": round(ratio, 1),
        "distinct": len(dist),
        "dist": dict(sorted(dist.items())),
        "by_role": {k: dict(sorted(v.items())) for k, v in sorted(by_role.items())},
    }


def bar(pct: float, width: int = 20) -> str:
    n = int(round(pct / 100.0 * width))
    return "#" * n + "." * (width - n)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--page", default=None, help="只统计单个页面（不带 .xml）")
    ap.add_argument("--detail", action="store_true", help="打印每页非梯级明细行")
    args = ap.parse_args(argv)

    pages = PAGES
    if args.page:
        pages = [args.page]

    result = {}
    for p in pages:
        f = PAGES_DIR / (p + ".xml")
        if not f.exists():
            result[p] = {"error": "missing"}
            continue
        rows = scan_page(f)
        s = summarize(rows)
        s["off_ladder"] = [
            {"line": r["line"], "tag": r["tag"], "name": r["name"], "role": r["role"], "h": r["h"]}
            for r in rows
            if r["h"] > 3 and r["h"] not in LADDER
        ]
        result[p] = s

    if args.json:
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return 0

    print("=" * 78)
    print("height rhythm — 逐页分布（梯级 24/28/32/40；已排除 height=0 与 visible=false）")
    print("=" * 78)
    tot_counted = tot_in = 0
    for p in pages:
        s = result[p]
        if "error" in s:
            print("%-16s  !! %s" % (p, s["error"]))
            continue
        tot_counted += s["counted"]
        tot_in += s["inladder"]
        print(
            "%-16s n=%-3d in-ladder=%-3d %5.1f%% %s distinct=%d sep=%d"
            % (p, s["counted"], s["inladder"], s["ratio"], bar(s["ratio"]), s["distinct"], s["seps"])
        )
        print("    dist: " + ", ".join("%d=x%d" % (k, v) for k, v in s["dist"].items()))
        print("    role: " + "; ".join(
            "%s{%s}" % (r, ",".join("%d:%d" % (k, v) for k, v in d.items()))
            for r, d in s["by_role"].items()
        ))
        if args.detail and s["off_ladder"]:
            for o in s["off_ladder"]:
                print("      L%-4d %-18s %-22s %-9s %d" % (o["line"], o["tag"], o["name"] or "-", o["role"], o["h"]))
    if tot_counted:
        print("-" * 78)
        print(
            "TOTAL           n=%d  in-ladder=%d  %.1f%%"
            % (tot_counted, tot_in, 100.0 * tot_in / tot_counted)
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
