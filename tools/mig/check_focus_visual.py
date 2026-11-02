"""Assert every tab-stop control can obtain a keyboard focus ring.

Why this gate exists
--------------------
The framework has always supported `focusbordercolor` (s3 fast path UIControl.cpp:1212,
legacy chain :1389-1394, painted in PaintBorder :1644-1690), yet the skin used it ZERO
times. So keyboard users got the system highlight fallback (:1664-1668) -- not themed,
not 2px, and invisible against an accent-coloured button.

The subtle part is how <Default> resolves
-----------------------------------------
<Default> is keyed by XML TAG NAME, not by the C++ class hierarchy.
UIDlgBuilder.cpp:583-585 does GetDefaultAttributeList(pstrClass) + ApplyAttributeList(),
so writing only `name="Button"` leaves COptionUI and CCheckBoxUI uncovered even though
COptionUI derives from CButtonUI. That is 16 controls (Option 12 + CheckBox 4) silently
left without a focus ring. This gate therefore requires a per-tag rule and merges the
<Default> list before judging, so "declared once in Default" counts as covered.

Exit: 0 every tab-stop control covered, 1 gaps found, 2 input problem, 3 --inject blind.
"""

from __future__ import annotations

import argparse
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SKIN = os.path.join(ROOT, "bin64", "skin", "Win8Metro")

# Classes the framework gives UIFLAG_TABSTOP to, so they can receive keyboard focus.
#   CComboUI/CComboUI : unconditional (UICombo.cpp:325)
#   CEditUI           : unconditional (UIEdit.cpp:285)
#   CButtonUI         : when keyboard="true" (UIButton.cpp:68) -- covered by default below
#   COptionUI/CCheckBoxUI inherit CButtonUI's tab handling
TABSTOP_TAGS = ("Button", "Edit", "Combo", "Option", "CheckBox")

# Controls that are decoration or chrome: no focus ring expected.
EXEMPT_NAMES = ("minbtn", "maxbtn", "closebtn", "restorebtn", "menu_")

TAG_RE = re.compile(r"<([A-Za-z][\w]*)\b([^>]*?)/?>", re.S)
DEF_RE = re.compile(r"<Default\b([^>]*?)/?>", re.S)
ATTR_RE = re.compile(r'([\w]+)\s*=\s*"([^"]*)"')


def skin_files():
    return [os.path.join(SKIN, "Main.xml")] + sorted(
        glob.glob(os.path.join(SKIN, "pages", "*.xml")))


def strip_comments(text: str) -> str:
    return re.sub(r"<!--.*?-->", lambda m: " " * len(m.group(0)), text, flags=re.S)


def parse_defaults(text: str) -> dict:
    """tag -> set of attribute names its <Default> rule supplies."""
    out = {}
    for m in DEF_RE.finditer(text):
        attrs = dict(ATTR_RE.findall(m.group(1)))
        tag = attrs.get("name")
        value = attrs.get("value", "")
        if not tag:
            continue
        supplied = set(re.findall(r"([\w]+)\s*=", value))
        out.setdefault(tag, set()).update(supplied)
    return out


def scan(extra_default_focus: bool = False):
    """Return (total, covered, missing, defaults)."""
    files = skin_files()
    if not files:
        return 0, 0, [], {}

    defaults = {}
    bodies = {}
    for p in files:
        raw = open(p, "rb").read().decode("utf-8-sig", errors="replace")
        body = strip_comments(raw)
        bodies[p] = body
        for tag, attrs in parse_defaults(body).items():
            defaults.setdefault(tag, set()).update(attrs)

    # The injected fault: pretend no <Default> supplies focusbordercolor.
    if extra_default_focus:
        defaults = {k: {a for a in v if a != "focusbordercolor"}
                    for k, v in defaults.items()}

    total = 0
    covered = 0
    missing = []
    for p, body in bodies.items():
        for m in TAG_RE.finditer(body):
            tag, attrs_raw = m.group(1), m.group(2)
            if tag not in TABSTOP_TAGS:
                continue
            attrs = dict(ATTR_RE.findall(attrs_raw))
            name = attrs.get("name", "")
            if name.startswith(EXEMPT_NAMES) and tag == "Button":
                continue
            total += 1
            inherited = "focusbordercolor" in defaults.get(tag, set())
            own = "focusbordercolor" in attrs
            if own or inherited:
                covered += 1
            else:
                where = name or ("line %d" % (body.count("\n", 0, m.start()) + 1))
                missing.append((os.path.basename(p), tag, where, own, inherited))
    return total, covered, missing, defaults


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--inject", action="store_true",
                    help="drop the Default-supplied focus colour; the gate must report it")
    args = ap.parse_args()

    files = skin_files()
    if not files:
        print("[FAIL] no skin XML under " + SKIN)
        return 2

    total, covered, missing, defaults = scan(extra_default_focus=args.inject)

    print("[INFO] skin files=%d  tab-stop controls=%d  covered=%d  default-rules=%d"
          % (len(files), total, covered, len(defaults)))
    for tag in TABSTOP_TAGS:
        if "focusbordercolor" in defaults.get(tag, set()):
            print("       <Default name=\"%s\"> supplies focusbordercolor" % tag)

    # Zero-input guard: a run that examined nothing must never read as success.
    if total == 0:
        print("[FAIL] no tab-stop controls scanned -- refusing to pass")
        return 2

    if missing:
        print("[FAIL] %d tab-stop control(s) cannot obtain a focus ring:" % len(missing))
        for f, tag, where, own, inh in missing[:20]:
            print("       %-22s <%s %s>  own=%s inherited=%s"
                  % (f, tag, where, own, inh))
        if len(missing) > 20:
            print("       ... and %d more" % (len(missing) - 20))
        print("       add a <Default name=\"<Tag>\" value=\"focusbordercolor=...\" /> rule")
        print("       NOTE: <Default> is keyed by TAG NAME, so Option/CheckBox need")
        print("             their own rule even though they derive from CButtonUI.")
        return 1

    if args.inject:
        print("[FAIL] inject NOT reported -- the gate is blind")
        return 3

    print("[PASS] every tab-stop control can obtain a focus ring (%d/%d)" % (covered, total))
    return 0


if __name__ == "__main__":
    sys.exit(main())
