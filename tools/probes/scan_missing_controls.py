#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""scan_missing_controls.py -- find controls referenced by name in C++ but never
defined in the skin.

Why this exists
---------------
R57 ran a throwaway version of this scan and reported 23 missing controls.
21 of them were false positives, because the regex only matched
`SetName(L"...")` literals and missed controls named through factory helpers:

    MkEdit(pszName)                      name is arg 0
    MkCombo(pszName, nDropH)             name is arg 0
    MkBtn(pszText, pszName, bPrimary)    name is arg 1   <-- not 0
    MkSysLink(pszText, pszName)          name is arg 1
    AoMkBrowseRow(pszEdit, pszBtn, ...)  two named controls
    MkText / MkGrid                      no name argument at all

Assuming "the name is always the first argument" would silently drop all 47
MkBtn call sites. The index is therefore declared per helper.

What it found when fixed
------------------------
`welcome_box`: CPageWelcome::BuildPage() starts with `if (!pBox) return;`, so
the refresh button, brand banner and four overview cards were never created.
The page looked fine because its static text is in the skin. A missing control
that is guarded by an if-check fails silently by design, which is why this needs
a mechanical scan rather than eyeballing.

Exit code
---------
  0 = no NEW findings (baseline-known names are tolerated)
  1 = at least one finding that is not in the baseline
  2 = setup problem (skin or source directory missing)

--baseline <file>
-----------------
A JSON file holding names already triaged as harmless, e.g.:

    {"cap_ring": "guarded lookup; miss only stops an animation refresh"}

Why a baseline instead of zero tolerance: five findings remain and all five were
reviewed in R57. A gate that is red from day one gets ignored -- this project
already recorded that failure mode for a probe that always returned 0. What
matters is that NEW ones fail the build.

Known limitations (printed in the output, not hidden)
-----------------------------------------------------
  * Controls created by any helper not listed in NAME_ARG_INDEX will still show
    up as false positives. The list must be extended when new helpers appear.
  * Guarded lookups are still reported; a guard makes the miss harmless but does
    not make the control exist. Triage them rather than silencing the scanner.

ASCII-only output: this project's consoles are CP936.
"""

import argparse
import glob
import io
import json
import os
import re
import sys

# Explicit overrides for helpers whose name argument cannot be inferred from a
# direct SetName(<param>) call in the same file (e.g. the helper forwards the
# name to another helper). Everything else is derived from source by
# derive_name_arg_index(); this table only needs entries the inference misses.
NAME_ARG_INDEX_OVERRIDE = {
}


def derive_name_arg_index(cppdir):
    """Map helper name -> [arg indices] by reading which parameter feeds SetName.

    Why not a hand-written table: R57 shipped one and it was wrong (MkBtn puts the
    name in argument 1, not 0). A hand table also rots the moment a new helper
    appears, and the rot shows up as unexplained "missing control" findings.

    Inference: for each member function with a body, split its parameter list,
    then look for SetName(<ident>) where <ident> matches a parameter name. The
    position of that parameter is the answer.
    """
    derived = {}
    fn_index = {}
    for f in glob.glob(os.path.join(cppdir, "*.cpp")):
        src = open(f, encoding="utf-8", errors="replace").read()
        for m in re.finditer(r'\b(\w+)::(~?\w+)\s*\(([^)]*)\)\s*(?:const\s*)?\{', src):
            fn = m.group(2)
            params = split_args(m.group(3))
            # parameter name is the last identifier in each declaration
            names = []
            for prm in params:
                ids = re.findall(r'[A-Za-z_]\w*', prm)
                names.append(ids[-1] if ids else "")
            if not names:
                continue

            # bounded body read
            start = m.end()
            depth, end = 1, -1
            for j in range(start, min(len(src), start + 6000)):
                if src[j] == "{":
                    depth += 1
                elif src[j] == "}":
                    depth -= 1
                    if depth == 0:
                        end = j
                        break
            if end < 0:
                continue
            body = src[start:end]

            for sm in re.finditer(r'SetName\(\s*([A-Za-z_]\w*)\s*\)', body):
                ident = sm.group(1)
                for idx, nm in enumerate(names):
                    if nm and nm == ident:
                        derived.setdefault(fn, set()).add(idx)

            # Remember enough about this helper to resolve forwarding later.
            fn_index[fn] = (names, body)

    # ---- propagate through forwarding helpers -----------------------------
    #
    # A helper that does not call SetName directly can still name controls by
    # passing its parameters to another helper. AoMkBrowseRow is exactly that:
    # it calls MkEdit(pszEdit) and MkBtn(..., pszBtn, ...).
    #
    # Iterate to a fixed point: each pass may teach one more helper, and chains
    # of arbitrary depth settle because the set only grows.
    changed = True
    while changed:
        changed = False
        for fn, (names, body) in fn_index.items():
            for target, target_idxs in list(derived.items()):
                for call in re.finditer(re.escape(target) + r'\s*\(', body):
                    i = call.end() - 1
                    depth, end = 0, -1
                    for j in range(i, min(len(body), i + 800)):
                        if body[j] == "(":
                            depth += 1
                        elif body[j] == ")":
                            depth -= 1
                            if depth == 0:
                                end = j
                                break
                    if end < 0:
                        continue
                    args = split_args(body[i + 1:end])
                    for t_idx in target_idxs:
                        if t_idx >= len(args):
                            continue
                        ident = args[t_idx].strip()
                        if not re.match(r'^[A-Za-z_]\w*$', ident):
                            continue
                        for idx, nm in enumerate(names):
                            if nm and nm == ident:
                                if idx not in derived.get(fn, set()):
                                    derived.setdefault(fn, set()).add(idx)
                                    changed = True
    return {k: sorted(v) for k, v in derived.items()}




def split_args(s):
    """Split a C++ argument list on top-level commas."""
    out, depth, cur = [], 0, []
    for ch in s:
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        if ch == "," and depth == 0:
            out.append("".join(cur).strip())
            cur = []
        else:
            cur.append(ch)
    if cur:
        out.append("".join(cur).strip())
    return out


def dyn_names(cppdir):
    """Names created at runtime: literal SetName, or via factory helpers."""
    out = set()
    index = derive_name_arg_index(cppdir)
    for fn, idxs in NAME_ARG_INDEX_OVERRIDE.items():
        index[fn] = idxs

    for f in glob.glob(os.path.join(cppdir, "*.cpp")):
        src = open(f, encoding="utf-8", errors="replace").read()
        out.update(re.findall(r'SetName\(\s*L"([\w]+)"', src))

        for fn, idxs in index.items():
            for m in re.finditer(re.escape(fn) + r"\s*\(", src):
                i = m.end() - 1
                depth, end = 0, -1
                for j in range(i, min(len(src), i + 800)):
                    if src[j] == "(":
                        depth += 1
                    elif src[j] == ")":
                        depth -= 1
                        if depth == 0:
                            end = j
                            break
                if end < 0:
                    continue
                args = split_args(src[i + 1:end])
                for k in idxs:
                    if k < len(args):
                        lit = re.match(r'L"([\w]+)"\s*$', args[k])
                        if lit:
                            out.add(lit.group(1))
    return out


def array_literals(src):
    """Collect string literals declared inside array initialisers.

    Returns {array_identifier: set(literals)}.

    Why this exists
    ---------------
    A large share of name lookups in this codebase do not carry the name at the
    call site at all. They pass an element of a file-local table:

        static const wchar_t* s_ids[] = { L"btn_scan", L"btn_clean" };
        for (...) { m_ctx.HostFindControl(s_ids[i]); }

    The pattern regexes only match `FindControl(L"literal")`, so every name that
    travels through a table was invisible. Measured: 48 distinct non-literal
    arguments across Demos/Win8Metro, most of them tables. That is the same
    silent-no-op class the gate exists to catch -- a control renamed in the skin
    but still listed in a table would never be reported.

    Scope is deliberately narrow: only `name[] = { ... }` declarations in the
    same file are parsed. Cross-file tables and tables built at run time stay
    unresolved (and therefore stay out of `need`), so this can only add true
    positives for names that ARE spelled as literals here -- it cannot invent
    findings for computed names.
    """
    out = {}
    # `... ident[] = {` then a balanced brace body.
    for m in re.finditer(r'([A-Za-z_]\w*)\s*\[\s*\]\s*=\s*\{', src):
        ident = m.group(1)
        i = m.end() - 1
        depth = 0
        end = -1
        for j in range(i, min(len(src), i + 20000)):
            if src[j] == "{":
                depth += 1
            elif src[j] == "}":
                depth -= 1
                if depth == 0:
                    end = j
                    break
        if end < 0:
            continue
        body = src[i:end + 1]
        lits = set(re.findall(r'L"([\w]+)"', body))
        if lits:
            out.setdefault(ident, set()).update(lits)
    return out


def _call_args_at(src, fn_names):
    """Yield the argument lists of every call to any of fn_names.

    Uses balanced-paren scanning rather than a regex so nested calls such as
    HostFindControl(StringMgr.Get("x")) do not truncate the argument text.
    """
    for fn in fn_names:
        for m in re.finditer(re.escape(fn) + r"\s*\(", src):
            i = m.end() - 1
            depth = 0
            end = -1
            for j in range(i, min(len(src), i + 2000)):
                if src[j] == "(":
                    depth += 1
                elif src[j] == ")":
                    depth -= 1
                    if depth == 0:
                        end = j
                        break
            if end < 0:
                continue
            yield split_args(src[i + 1:end])


# The lookup entry points. Kept next to _call_args_at so the two cannot drift.
LOOKUP_FNS = ("HostFindControl", "FindControl", "HostSetCtrlText",
              "HostSkinText", "GetSkinText", "SetCtrlText")

# [R236] Names that are looked up on purpose WITHOUT ever existing as a control.
#
# GetSkinText(pm, name) resolves to "the text of the control with this name". The
# loading hints are built in code and only take their *words* from the skin, so
# there is no such control by design: FrameWnd.cpp GetLoadingHintText() supplies a
# built-in default ("正在加载…") when neither a translation nor a control exists.
#
# Exact names only. A prefix or regex would blanket-exempt future real omissions --
# the failure mode this gate exists to catch. Each entry must be justified by an
# actual fallback in the lookup's implementation, verified in source.
NAMES_WITHOUT_CONTROL = {
    "lbl_loading":      "FrameWnd.cpp GetLoadingHintText(): built-in default text",
    "lbl_appx_loading": "FrameWnd.cpp GetLoadingHintText(): built-in default text",
}

# Call-site patterns for names passed as the FIRST argument to a lookup.
#
# Single source of truth: names_used() and ownership_findings() both read this.
# They used to carry their own copies, which is the kind of duplication that
# lets a newly-added entry take effect in one check and not the other.
#
# [R72] Added the wrapper helpers below after measuring them: 15
# AddTooltipForControl, 4 FillFormatCombo, 11 ReadEditValue/ReadCheckValue
# calls pass a control name into a real lookup. None of them was missing a
# control, so this closed a blind spot rather than a live defect -- but the
# next rename through one of these paths would have gone unreported.
LOOKUP_NAME_PATS = [
    r'FindControl\(\s*L"([\w]+)"',
    r'HostFindControl\(\s*L"([\w]+)"',
    r'HostSetCtrlText\(\s*L"([\w]+)"',
    r'HostSkinText\(\s*L"([\w]+)"',
    r'GetSkinText\(\s*[^,]+,\s*L"([\w]+)"',
    r'SetCtrlText\(\s*[^,]+,\s*L"([\w]+)"',
    # FrameWnd/PageIntl wrappers that resolve the name themselves.
    r'AddTooltipForControl\(\s*L"([\w]+)"',
    r'FillFormatCombo\(\s*L"([\w]+)"',
    r'ReadEditValue\(\s*L"([\w]+)"',
    r'ReadCheckValue\(\s*L"([\w]+)"',
]


def tables_used(src):
    """Names reached through `table[index]` in the NAME slot of a lookup call.

    Why this is not "any indexed array": an earlier attempt collected the
    literals of every array indexed anywhere in the file and produced 355
    findings, almost all wrong -- enum labels, PowerShell service states
    (STOPPED/RUNNING), error-code tables, package names. Those never name a
    control. The constraint that makes this sound is positional: only an
    expression in the argument slot that a lookup function treats as a control
    name can be a control name.
    """
    out = set()
    arrays = array_literals(src)
    if not arrays:
        return out

    for args in _call_args_at(src, LOOKUP_FNS):
        for arg in args:
            # Two shapes carry a control name out of a file-local table:
            #   s_ids[i]            -- plain array of names
            #   kBtns[i].name       -- array of structs, name in a member
            # Both appear in this codebase (Page_Cleanup.cpp / Page_Update.cpp).
            m = re.match(r'\s*([A-Za-z_]\w*)\s*\[', arg)
            if m and m.group(1) in arrays:
                out.update(arrays[m.group(1)])
    return out


def controls_in_skin(root):
    names = set()
    for f in glob.glob(os.path.join(root, "*.xml")) + glob.glob(os.path.join(root, "pages", "*.xml")):
        txt = open(f, encoding="utf-8", errors="replace").read()
        names.update(re.findall(r'name="([A-Za-z_][\w]*)"', txt))
    return names


def names_used(cppdir):
    used = {}
    # Every runtime entry point that resolves a control BY NAME must be listed
    # here, otherwise references made through it are invisible and a genuinely
    # missing control passes the gate.
    #
    # HostSkinText was such a gap: it is implemented as
    #     GetSkinText(pm, pszName) -> ... -> pm.FindControl(pszName)
    # so it is a real name lookup, but the scanner did not know about it.
    # Callers use it as HostSkinText(L"lbl_cap_loading") to fetch placeholder
    # text, which is exactly the pattern that would go unnoticed if the control
    # were renamed or dropped from the skin.
    pats = LOOKUP_NAME_PATS
    for f in glob.glob(os.path.join(cppdir, "*.cpp")):
        src = open(f, encoding="utf-8", errors="replace").read()
        base = os.path.basename(f)
        for pat in pats:
            for n in re.findall(pat, src):
                used.setdefault(n, set()).add(base)
        # Names that travel through a file-local table (see array_literals).
        for n in tables_used(src):
            used.setdefault(n, set()).add(base)
    return used


def enclosing_function(src, pos):
    """Best-effort name of the function containing `pos`.

    A MISSING line that says only "page_cleanup.cpp" leaves the reader to open
    the file and hunt. Naming the enclosing function usually makes the purpose
    obvious ("SetScanButtonsBusy" vs "OnExport" tells you what breaks).
    """
    head = src[:pos]
    # nearest preceding definition line: <type> Class::Name(...)  or  Name(...)
    best = None
    for m in re.finditer(r'^(?:[\w:<>,\*&\s]+?\s)?([A-Za-z_]\w*(?:::[A-Za-z_]\w*)*)\s*\([^;{)]*\)[^;{]*\{',
                         head, re.M):
        best = m.group(1)
    return best


def references_with_context(cppdir, missing):
    """For each missing name, first reference site: file, line, enclosing function."""
    out = {}
    want = set(missing)
    if not want:
        return out
    for f in glob.glob(os.path.join(cppdir, "*.cpp")):
        src = open(f, encoding="utf-8", errors="replace").read()
        for pat in LOOKUP_NAME_PATS:
            for m in re.finditer(pat, src):
                n = m.group(1)
                if n not in want or n in out:
                    continue
                line = src.count("\n", 0, m.start()) + 1
                out[n] = (os.path.basename(f), line, enclosing_function(src, m.start()))
        # Names reached through a file-local table have no call site of
        # their own; point at the table entry instead.
        arrays = array_literals(src)
        for ident, lits in arrays.items():
            for n in lits:
                if n not in want or n in out:
                    continue
                m = re.search('L"' + re.escape(n) + '"', src)
                if not m:
                    continue
                line = src.count("\n", 0, m.start()) + 1
                out[n] = (os.path.basename(f), line,
                          enclosing_function(src, m.start()) or ("table " + ident))
    return out


def scan(skin, code):
    have = controls_in_skin(skin)
    dyn = dyn_names(code)
    need = names_used(code)
    # [R236] NAMES_WITHOUT_CONTROL is an exact-name exemption, not a wildcard:
    #   those lookups have a built-in default in the lookup's own implementation,
    #   so the control being absent is the designed state, not a broken feature.
    miss = sorted(n for n in need
                  if n not in have and n not in dyn and n not in NAMES_WITHOUT_CONTROL)
    return have, dyn, need, miss


# ===========================================================================
# Page-ownership check
# ===========================================================================
# The missing-name check above answers "does this control exist ANYWHERE?".
# That is weaker than it looks: a control owned by one page but referenced from
# another page's code exists, so the check passes -- yet at run time the lookup
# happens on the *current* page's control tree and returns null.
#
# R71 hit exactly this: Page_Cleanup_Build.cpp disabled L"btn_refresh", which
# only exists in page_optim.xml. Scan/clean never actually disabled the cleanup
# page's refresh button, and no gate noticed because the name did exist.

PAGE_XML = re.compile(r'^page_(.+)\.xml$')


def duplicate_names(skin):
    """Find a name declared more than once inside the same skin file.

    R71 hit this: page_intl.xml declared btn_intl_apply_uilang twice in two
    identical rows, so the page showed two buttons that did the same thing.
    Both resolve by name, so nothing errored -- it just looked wrong.
    Returns [(file, name, count)].
    """
    out = []
    files = []
    main = os.path.join(skin, "Main.xml")
    if os.path.isfile(main):
        files.append(main)
    files.extend(sorted(glob.glob(os.path.join(skin, "pages", "*.xml"))))
    for f in files:
        t = io.open(f, encoding="utf-8-sig", errors="replace").read()
        counts = {}
        for n in re.findall(r'name="([A-Za-z_]\w*)"', t):
            counts[n] = counts.get(n, 0) + 1
        for n, c in sorted(counts.items()):
            if c > 1:
                out.append((os.path.basename(f), n, c))
    return out


def names_by_page(skin):
    """Map control name -> set of owning page tokens (or 'Main' for chrome)."""
    owner = {}
    main = os.path.join(skin, "Main.xml")
    if os.path.isfile(main):
        t = io.open(main, encoding="utf-8-sig", errors="replace").read()
        for n in re.findall(r'name="([A-Za-z_]\w*)"', t):
            owner.setdefault(n, set()).add("Main")
    for f in glob.glob(os.path.join(skin, "pages", "*.xml")):
        m = PAGE_XML.match(os.path.basename(f))
        if not m:
            continue
        page = m.group(1)
        t = io.open(f, encoding="utf-8-sig", errors="replace").read()
        for n in re.findall(r'name="([A-Za-z_]\w*)"', t):
            owner.setdefault(n, set()).add(page)
    return owner


def page_of_cpp(base):
    """Infer the page a .cpp belongs to, or None when it is not page-scoped.

    FrameWnd* and other shell files legitimately touch every page: they are the
    host. Page_Cleanup_Build.cpp -> 'cleanup'. Anything we cannot attribute is
    treated as shell and skipped, so this check stays silent rather than
    guessing.
    """
    m = re.match(r'^Page_([A-Za-z0-9]+)(?:_|\.cpp$)', base)
    if not m:
        return None
    return m.group(1).lower()


# Shared sub-views: a component class that renders INTO another page's skin.
# CPageFeature is the Windows-features view shared by the program page's tab,
# and CPageCapability borrows the capability controls that live in
# page_feature.xml. Both are real cross-references, not defects, so they are
# named here rather than silenced per-finding.
SHARED_VIEWS = {
    "capability": {"feature"},
    "feature": {"program", "feature"},
}

# Wrappers used for a deliberate "read mine, else fall back to theirs" lookup.
# CPageCapability reads cap_source_edit first and only then feat_src_edit, so
# the second read is by design, not a typo. Ownership violations through these
# wrappers are therefore not reported -- but the same name reached through
# HostFindControl still is, which keeps the check useful for real mistakes.
FALLBACK_READERS = ("HostReadEditValue",)


def _fallback_spans(src):
    """Character ranges of `X(L"name")` calls made through a fallback reader."""
    spans = []
    for fn in FALLBACK_READERS:
        for m in re.finditer(re.escape(fn) + r'\s*\(\s*L"([\w]+)"', src):
            spans.append((m.start(), m.end(), m.group(1)))
    return spans


def ownership_findings(skin, code):
    """List (cpp, name, owners) where a page's code uses another page's control."""
    owner = names_by_page(skin)
    out = []
    for f in glob.glob(os.path.join(code, "*.cpp")):
        base = os.path.basename(f)
        page = page_of_cpp(base)
        if page is None:
            continue                      # shell/host file: may touch any page
        allowed = SHARED_VIEWS.get(page, set()) | {page}

        src = io.open(f, encoding="utf-8", errors="replace").read()
        used = set()
        for pat in LOOKUP_NAME_PATS:
            used.update(re.findall(pat, src))
        used |= tables_used(src)

        # Drop a name only when EVERY reference to it in this file goes through
        # a fallback reader ("read mine, else read theirs"). If the same name is
        # also reached by a plain lookup, it stays reported -- that keeps the
        # check meaningful for genuine mistakes.
        for n in list(used):
            esc = re.escape(n)
            total = len(re.findall(r'L"' + esc + r'"', src))
            via_fallback = len(re.findall(
                r'(?:' + "|".join(FALLBACK_READERS) + r')\s*\(\s*L"' + esc + r'"', src))
            if total > 0 and via_fallback == total:
                used.discard(n)

        for n in sorted(used):
            owners = owner.get(n)
            if not owners:
                continue                  # unknown/dynamic: the other check's job
            if "Main" in owners:
                continue                  # chrome is addressable from any page
            if owners & allowed:
                continue
            out.append((base, n, sorted(owners)))
    return out


def run_selftest():
    """Verify the scanner against a fixture, without touching product source."""
    import tempfile
    import shutil

    root = tempfile.mkdtemp(prefix="scanfix_")
    skin = os.path.join(root, "skin")
    pages = os.path.join(skin, "pages")
    code = os.path.join(root, "code")
    os.makedirs(pages)
    os.makedirs(code)

    try:
        with io.open(os.path.join(pages, "p.xml"), "w", encoding="utf-8") as f:
            f.write('<Window><VerticalLayout name="real_control"/>'
                    '<VerticalLayout name="tbl_defined_d"/></Window>')

        with io.open(os.path.join(code, "c.cpp"), "w", encoding="utf-8") as f:
            f.write(r"""
void f(void* pm) {
    // 1) undefined name referenced by a literal lookup -> must be reported
    FindControl(L"r61_injected_missing");
    // 2) defined name -> must NOT be reported
    FindControl(L"real_control");
    // 3) factory helper with the name in argument 1 -> must NOT be reported
    MkBtn(L"btn_text", L"factory_named_btn", true);
    // 4) factory helper with the name in argument 0 -> must NOT be reported
    MkEdit(L"factory_named_edit");
    // 4b) wrapper helpers that resolve the name internally [R72]. The names are
    //     undefined here, so a scanner unaware of these wrappers reports nothing
    //     for them -- that was the blind spot.
    AddTooltipForControl(L"wrap_missing_tip", 0);
    FillFormatCombo(L"wrap_missing_combo");
    ReadEditValue(L"wrap_missing_edit");
    ReadCheckValue(L"wrap_missing_check");
}

// 5) the table-driven shape that the call-site regexes cannot see.
//    Both entries are undefined in the fixture skin, so the scanner must
//    report them; a literal-only scanner reports neither.
void table_driven(void* pm) {
    static const wchar_t* s_ids[] = { L"tbl_missing_a", L"tbl_missing_b" };
    for (size_t i = 0; i < 2; ++i) {
        HostFindControl(s_ids[i]);
    }
}

// 6) a table of structs, the shape Page_Update.cpp uses for its button lists.
//    The name is a member, reached as `tbl[i].name` on the lookup call.
void struct_table(void* pm) {
    struct { const wchar_t* name; int flag; } kBtns[] = {
        { L"tbl_missing_c", 1 },
        { L"tbl_defined_d", 0 },
    };
    for (int i = 0; i < 2; ++i) {
        HostFindControl(kBtns[i].name);
    }
}

// 5) forwarding helper: names controls only via its parameters.
//    This is the shape that regressed in R62 (AoMkBrowseRow).
CHorizontalLayoutUI* CToolDialogPage::AoMkBrowseRow(
    const wchar_t* pszEdit, const wchar_t* pszBtn, const wchar_t* pszText)
{
    CEditUI* pEdit = MkEdit(pszEdit);
    CButtonUI* pB  = MkBtn(StringMgr.Get("general.browse"), pszBtn, false);
    return nullptr;
}

// 6) one more indirection: forwards into AoMkBrowseRow instead of the leaf helpers
void CToolDialogPage::AoMkBrowseRowWrapped(const wchar_t* pszEdit2, const wchar_t* pszBtn2)
{
    AoMkBrowseRow(pszEdit2, pszBtn2, nullptr);
}

// 7) third level: the propagation must keep iterating until it settles.
//    Level 1 = MkEdit/MkBtn (direct SetName), 2 = AoMkBrowseRow, 3 = this,
//    4 = the call site below. A single pass would stop at level 2.
void CToolDialogPage::AoMkBrowseRowDeep(const wchar_t* pszEdit3, const wchar_t* pszBtn3)
{
    AoMkBrowseRowWrapped(pszEdit3, pszBtn3);
}

void g(void* pm) {
    (void)0;
    AoMkBrowseRow(L"fwd_edit_name", L"fwd_btn_name", L"label");
    AoMkBrowseRowWrapped(L"fwd2_edit_name", L"fwd2_btn_name");
    AoMkBrowseRowDeep(L"fwd3_edit_name", L"fwd3_btn_name");
}
""")

        # --- page-ownership fixture ---------------------------------------
        # Two pages with one control each, plus one chrome control in Main.
        # Page_Cleanup.cpp must be flagged for touching the optim-only name,
        # must NOT be flagged for its own name or for the chrome one, and a
        # declared shared view must stay silent.
        with io.open(os.path.join(pages, "page_cleanup.xml"), "w",
                     encoding="utf-8") as f:
            f.write('<Window><VerticalLayout name="cln_own"/>'
                    '<VerticalLayout name="own_cap"/></Window>')
        with io.open(os.path.join(pages, "page_optim.xml"), "w",
                     encoding="utf-8") as f:
            f.write('<Window><VerticalLayout name="opt_only"/></Window>')
        with io.open(os.path.join(pages, "page_dup.xml"), "w",
                     encoding="utf-8") as f:
            f.write('<Window><VerticalLayout name="dup_name"/>'
                    '<Button name="dup_name"/></Window>')
        with io.open(os.path.join(skin, "Main.xml"), "w", encoding="utf-8") as f:
            f.write('<Window><Text name="chrome_shared"/></Window>')

        with io.open(os.path.join(code, "Page_Cleanup.cpp"), "w",
                     encoding="utf-8") as f:
            f.write(r"""
void h(void* pm) {
    FindControl(L"cln_own");        // own page   -> must NOT be flagged
    FindControl(L"chrome_shared");  // chrome     -> must NOT be flagged
    FindControl(L"opt_only");       // other page -> must BE flagged
}
""")
        with io.open(os.path.join(code, "Page_Capability.cpp"), "w",
                     encoding="utf-8") as f:
            f.write(r"""
void i(void* pm) {
    FindControl(L"own_cap");        // own page, fine
}
void i2(void* pm) {
    HostReadEditValue(L"opt_only"); // fallback read only -> NOT reported
}
""")
        with io.open(os.path.join(code, "Page_Feature.cpp"), "w",
                     encoding="utf-8") as f:
            f.write(r"""
void j(void* pm) {
    FindControl(L"opt_only");       // feature may borrow program/feature only,
                                    // so this stays flagged
}
""")
        with io.open(os.path.join(code, "FrameWnd.cpp"), "w",
                     encoding="utf-8") as f:
            f.write(r"""
void k(void* pm) {
    FindControl(L"opt_only");       // shell file -> exempt by design
}
""")

        have, dyn, need, miss = scan(skin, code)
        own_bad = ownership_findings(skin, code)
        bad_pairs = {(base, n) for base, n, _o in own_bad}

        cases = [
            ("undefined literal lookup is reported",
             "r61_injected_missing" in miss),
            ("defined name is not reported",
             "real_control" not in miss),
            ("MkBtn arg-1 name is treated as dynamic (R57 false-positive class)",
             "factory_named_btn" not in miss),
            ("MkEdit arg-0 name is treated as dynamic",
             "factory_named_edit" not in miss),
            # R63: the class that R62 regressed on and the old fixture could not see
            ("forwarding helper: arg 0 reaching MkEdit is dynamic (R62 regression)",
             "fwd_edit_name" not in miss),
            ("forwarding helper: arg 1 reaching MkBtn is dynamic (R62 regression)",
             "fwd_btn_name" not in miss),
            ("indirect forwarding: chain through another helper is resolved",
             "fwd2_edit_name" not in miss and "fwd2_btn_name" not in miss),
            # R64: 3 levels of forwarding. A single-pass implementation stops at
            # level 2 and would report these as missing.
            ("deep forwarding: 3-level chain settles (fixed-point iteration)",
             "fwd3_edit_name" not in miss and "fwd3_btn_name" not in miss),
            # R69: names that only appear inside a file-local table. A
            # literal-only scanner reports all of these as MISSING-in-scanner,
            # i.e. it silently under-reports (the gate would stay green while a
            # renamed control kept breaking a page at run time).
            ("table-driven lookup: array literals are resolved",
             "tbl_missing_a" in miss and "tbl_missing_b" in miss),
            ("table-driven lookup: struct-member tables are resolved",
             "tbl_missing_c" in miss),
            ("table-driven lookup: defined table entry is not reported",
             "tbl_defined_d" not in miss),
            # R71: cross-page ownership. These four pin the check in both
            # directions -- it must catch a real miss and must not fire on the
            # legitimate cases (own page, chrome, shell file).
            ("ownership: another page's control is reported",
             ("Page_Cleanup.cpp", "opt_only") in bad_pairs),
            ("ownership: own page's control is not reported",
             ("Page_Cleanup.cpp", "cln_own") not in bad_pairs),
            ("ownership: chrome (Main.xml) control is not reported",
             ("Page_Cleanup.cpp", "chrome_shared") not in bad_pairs),
            ("ownership: shell file (FrameWnd) is exempt",
             not any(b == "FrameWnd.cpp" for b, _n in bad_pairs)),
            ("ownership: shared-view exemption is limited to declared pairs",
             ("Page_Feature.cpp", "opt_only") in bad_pairs),
            ("ownership: fallback read of another page's control is not reported",
             ("Page_Capability.cpp", "opt_only") not in bad_pairs),
            # R71: a name declared twice in one file shows a duplicate widget.
            ("duplicate name in one skin file is reported",
             any(f == "page_dup.xml" and n == "dup_name"
                 for f, n, _c in duplicate_names(skin))),
            # R72: wrapper helpers resolve the name themselves; a scanner that
            # only knows FindControl/HostFindControl misses them entirely.
            ("wrapper helper AddTooltipForControl name is seen",
             "wrap_missing_tip" in miss),
            ("wrapper helper FillFormatCombo name is seen",
             "wrap_missing_combo" in miss),
            ("wrapper helpers ReadEditValue/ReadCheckValue names are seen",
             "wrap_missing_edit" in miss and "wrap_missing_check" in miss),
        ]

        bad = 0
        for name, ok in cases:
            if ok:
                print("[PASS] %s" % name)
            else:
                print("[FAIL] %s" % name)
                bad += 1

        print("")
        print("findings in fixture: %s" % (sorted(miss) if miss else "(none)"))
        if bad == 0:
            print("SELFTEST PASS (%d checks)" % len(cases))
            return 0
        print("SELFTEST FAILED: %d of %d" % (bad, len(cases)))
        return 1
    finally:
        # Cleanup must never decide the verdict: the sandbox retention guard
        # can refuse the delete (CHAT_REFERENCE_PROTECTED), and raising from a
        # finally block used to turn a fully passing self-test into a failure.
        try:
            shutil.rmtree(root, ignore_errors=True)
        except Exception as e:
            print("NOTE: fixture cleanup skipped (%s)" % e)


def main():
    ap = argparse.ArgumentParser()
    # Not required=True: --selftest runs its own fixture and needs neither.
    # argparse would otherwise reject the invocation before main() can
    # dispatch on the flag.
    ap.add_argument("--skin", action="append", default=[],
                    help="skin root (repeatable)")
    ap.add_argument("--code", action="append", default=[],
                    help="Demos/Win8Metro source dir (repeatable, same order as --skin)")
    ap.add_argument("--baseline", default="",
                    help="JSON file of triaged names to tolerate")
    ap.add_argument("--selftest", action="store_true",
                    help="run the built-in fixture instead of scanning")
    args = ap.parse_args()

    if args.selftest:
        return run_selftest()

    if not args.skin or not args.code:
        print("[FATAL] --skin and --code are required unless --selftest is used")
        return 2

    tolerated = {}
    if args.baseline:
        if not os.path.isfile(args.baseline):
            print("[FATAL] baseline not found: " + args.baseline)
            return 2
        with io.open(args.baseline, "r", encoding="utf-8") as bf:
            tolerated = json.load(bf)
        print("baseline: %d tolerated name(s)" % len(tolerated))

    if len(args.skin) != len(args.code):
        print("[FATAL] --skin and --code must be given the same number of times")
        return 2

    total = 0
    all_missing = set()
    all_owner_bad = []
    all_dups = []
    for skin, code in zip(args.skin, args.code):
        if not os.path.isdir(skin) or not os.path.isdir(code):
            print("[FATAL] missing directory: %s or %s" % (skin, code))
            return 2

        label = os.path.basename(os.path.dirname(os.path.dirname(skin))) or skin
        have, dyn, need, miss = scan(skin, code)
        print("--- %s ---" % skin)
        print("    skin controls=%d  dynamic=%d  referenced=%d  missing=%d"
              % (len(have), len(dyn), len(need), len(miss)))
        ctx = references_with_context(code, miss)
        for n in miss:
            print("    MISSING %-24s <- %s" % (n, ",".join(sorted(need[n]))))
            if n in ctx:
                f, ln, fn = ctx[n]
                where = "%s:%d" % (f, ln)
                if fn:
                    where += "  in " + fn + "()"
                print("            first use: %s" % where)
                print("            the feature behind this lookup is unreachable"
                      " while the control is absent")
        total += len(miss)
        all_missing.update(miss)

        # Cross-page ownership: the name exists, but on a different page.
        bad = ownership_findings(skin, code)
        print("    cross-page refs=%d" % len(bad))
        for base, n, owners in bad:
            print("    FOREIGN %-24s %s -> owned by %s"
                  % (n, base, ",".join(owners)))
        all_owner_bad.extend(bad)

        # Duplicate declarations inside one skin file.
        dups = duplicate_names(skin)
        print("    duplicate names=%d" % len(dups))
        for f, n, c in dups:
            print("    DUPLICATE %-22s %s x%d" % (n, f, c))
        all_dups.extend((label, f, n, c) for f, n, c in dups)

    print("")
    print("note: guarded lookups are still reported. A guard makes the miss")
    print("      harmless, it does not make the control exist. Triage, do not")
    print("      silence the scanner.")

    # Split findings into triaged (baseline) and new.
    new_names = sorted(n for n in all_missing if n not in tolerated)
    known_names = sorted(n for n in all_missing if n in tolerated)

    if known_names:
        print("")
        print("tolerated (in baseline, reviewed earlier):")
        for n in known_names:
            print("    %-24s %s" % (n, tolerated[n]))

    # [R236] Cross-page refs get the same baseline treatment as missing names.
    #   Before this, `all_owner_bad` was checked unconditionally, so the 56 pairs
    #   that exist by design (a page's build/render split touching its own tree from
    #   a sibling .cpp) made the gate return 1 on every run. A gate that is always
    #   red is a gate nobody reads -- the same failure this file's own header warns
    #   about. Baseline entries are keyed "cpp::name" so a NEW page/name pair still
    #   fails; only reviewed ones are tolerated.
    def _own_key(base, n):
        return "owner:%s::%s" % (base, n)

    new_owner = [b for b in all_owner_bad if _own_key(b[0], b[1]) not in tolerated]
    known_owner = [b for b in all_owner_bad if _own_key(b[0], b[1]) in tolerated]

    if known_owner:
        print("")
        print("tolerated cross-page refs (in baseline, reviewed earlier): %d"
              % len(known_owner))

    if not new_names and not new_owner and not all_dups:
        print("")
        print("no NEW findings (%d tolerated names, %d tolerated cross-page refs)"
              % (len(known_names), len(known_owner)))
        return 0

    if all_dups:
        print("")
        print("DUPLICATE NAMES: %d" % len(all_dups))
        for label, f, n, c in all_dups:
            print("    %-22s %s x%d" % (n, f, c))
        print("A control name declared twice in one skin file produces two sibling")
        print("controls that answer to the same lookup: the page shows a duplicate")
        print("widget and only the first is addressable by name.")

    if new_owner:
        print("")
        print("CROSS-PAGE FINDINGS: %d (new)" % len(new_owner))
        for base, n, owners in new_owner:
            print("    %-24s %s -> owned by %s" % (n, base, ",".join(owners)))
        print("A page's code resolved this name against its own control tree, but")
        print("the control is defined in another page's skin. At run time the")
        print("lookup returns null and the intended effect silently does nothing.")
        print("Point the call at this page's control, or add the name to")
        print("SHARED_VIEWS if the two pages really share one sub-view.")

    if new_names:
        print("")
        print("NEW FINDINGS: %d" % len(new_names))
        for n in new_names:
            print("    %s" % n)
        print("A name referenced by FindControl/HostSetCtrlText but neither defined in")
        print("the skin nor created at runtime does nothing at run time, silently.")
        print("Either add the control, stop referencing it, or record it in the")
        print("baseline with a reason.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
