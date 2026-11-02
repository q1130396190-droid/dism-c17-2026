"""Apply the Win11 Fluent corner-radius system to the skin XML.

Specification used (Win11 Fluent / WinUI 3 defaults):

    content buttons, inputs, combos     4 px
    cards / panels / list containers    8 px
    nav pills, tabs, chips             full round (handled by the custom control already)
    window caption buttons, menu items 0 px  -- Win11 draws these as square hit areas

Why the radius differs by element rather than being one number everywhere: Fluent uses a
small radius on controls that sit inside a surface and a larger one on the surfaces
themselves. Using 8 px on a 24 px-tall button looks like a pill; using 4 px on a card
looks barely rounded. The two-tier rule is what makes it read as the same design language.

Deliberately excluded, and why:
  * caption buttons (minbtn/maxbtn/closebtn) -- the window manager draws their corners
  * menu_* items -- Win11 menu rows are square; rounding them looks like a web menu
  * <Text>, <Control> spacers, <VerticalLayout> wrappers -- no fill, radius invisible

The insert is written to be safe by construction: the attribute goes immediately before
the tag's closing "/>" or ">", and the result is parsed before it is written. An earlier
version replaced the first ">" blindly and turned `x="1"/>` into `x="1"/ r="4,4">`, which
is malformed XML; the app then crashed on skin load. Never again.
"""

from __future__ import annotations

import glob
import os
import re
import xml.etree.ElementTree as ET

PAGES = os.path.join(r"D:\DuiLib_Ultimate-master", "bin64", "skin", "Win8Metro")
MAIN = os.path.join(PAGES, "Main.xml")

SMALL = 4
LARGE = 8

# element -> radius
RULES = {
    "Button": SMALL,
    "Edit": SMALL,
    "Combo": SMALL,
    "CheckBox": SMALL,
    "Progress": SMALL,
    "HorizontalLayout": None,   # decided per element by whether it has a fill
    "VerticalLayout": None,
}

EXCLUDE_NAME_PREFIX = ("minbtn", "maxbtn", "closebtn", "restorebtn", "menu_")


def add_attr(text: str, tag: str, radius: int) -> tuple[str, int]:
    """Insert borderround before each <tag>'s closing bracket, safely."""
    out, pos, n = [], 0, 0
    for m in re.finditer(r"<%s\b[^>]*?(/?)>" % tag, text):
        seg = m.group(0)
        if "borderround=" in seg:
            out.append(text[pos:m.end()])
            pos = m.end()
            continue
        name_m = re.search(r'name="([^"]*)"', seg)
        name = name_m.group(1) if name_m else ""
        if name.startswith(EXCLUDE_NAME_PREFIX):
            out.append(text[pos:m.end()])
            pos = m.end()
            continue
        selfclose = seg.endswith("/>")
        body = (seg[:-2] if selfclose else seg[:-1]).rstrip()
        out.append(text[pos:m.start()])
        out.append(body + ' borderround="%d,%d"' % (radius, radius) + ("/>" if selfclose else ">"))
        pos = m.end()
        n += 1
    out.append(text[pos:])
    return "".join(out), n


def has_fill(seg: str) -> bool:
    """A layout only shows a radius if it actually paints a background."""
    m = re.search(r'bkcolor="([^"]*)"', seg)
    if not m:
        return False
    v = m.group(1)
    return v not in ("#00000000", "0", "")


def add_attr_if_filled(text: str, tag: str, radius: int) -> tuple[str, int]:
    out, pos, n = [], 0, 0
    for m in re.finditer(r"<%s\b[^>]*?(/?)>" % tag, text):
        seg = m.group(0)
        if "borderround=" in seg or not has_fill(seg):
            out.append(text[pos:m.end()])
            pos = m.end()
            continue
        name_m = re.search(r'name="([^"]*)"', seg)
        name = name_m.group(1) if name_m else ""
        if name.startswith(EXCLUDE_NAME_PREFIX):
            out.append(text[pos:m.end()])
            pos = m.end()
            continue
        selfclose = seg.endswith("/>")
        body = (seg[:-2] if selfclose else seg[:-1]).rstrip()
        out.append(text[pos:m.start()])
        out.append(body + ' borderround="%d,%d"' % (radius, radius) + ("/>" if selfclose else ">"))
        pos = m.end()
        n += 1
    out.append(text[pos:])
    return "".join(out), n


def main() -> int:
    files = [MAIN] + sorted(glob.glob(os.path.join(PAGES, "pages", "*.xml")))
    print("[INFO] files=%d" % len(files))
    total = 0
    for p in files:
        b = open(p, "rb").read()
        bom = b[:3] == b"\xef\xbb\xbf"
        t = b.decode("utf-8-sig")
        n = 0
        for tag, r in RULES.items():
            if r is None:
                t, k = add_attr_if_filled(t, tag, LARGE)
            else:
                t, k = add_attr(t, tag, r)
            n += k
        # Parse before writing: a malformed skin crashes the application at load time.
        ET.fromstring(t)
        if n:
            data = t.encode("utf-8")
            if bom:
                data = b"\xef\xbb\xbf" + data
            open(p, "wb").write(data)
            total += n
            print("  %-24s +%d" % (os.path.basename(p), n))
    print("[DONE] %d attribute(s) added" % total)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
