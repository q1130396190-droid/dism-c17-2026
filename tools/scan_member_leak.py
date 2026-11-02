"""Find member resources that are allocated but never released by their owner.

Why this exists: rounds 56-59 covered the DOUBLE-FREE direction (allocated + released
twice). The leak direction -- allocated and never released -- was explicitly recorded
as "not covered" at the end of round 59, because the existing scan only looks at
destructors that free things. A class that allocates into a member and never frees it
is invisible to that check by construction.

The shape:

    class T {
        T() { m_pBuf = new char[64]; }   // allocates into a member
        ~T() { /* nothing about m_pBuf */ }   // never releases it
        X* m_pBuf;
    };

Method: for each class, find member assignments from an allocating call
(`new`, `malloc`, `calloc`, `realloc`, `_tcsdup`, `StrDup`, `strdup`, `HeapAlloc`,
`LocalAlloc`, `GlobalAlloc`, `CoTaskMemAlloc`). Then search the SAME class body plus
any `T::~T` / `T::SomeMethod` definitions in the corpus for a release of that member
(`delete`, `free(`, `HeapFree`, `CloseHandle`, `Release`, `LocalFree`, `RegCloseKey`).

Reported in three buckets, because a two-way split would be dishonest:

  LEAK        allocated into a member, no release found anywhere in the class
  EXTERNAL    allocated here, and the member is handed to someone else (GetX returns
              it, or it is assigned to another object) -- ownership may move
  OK          a release exists

Only LEAK fails. EXTERNAL rows are listed so the boundary is visible instead of being
silently counted as safe.
"""
import argparse
import pathlib
import re
import sys

ALLOC_CALLS = ('new ', 'malloc(', 'calloc(', 'realloc(', '_tcsdup(', 'StrDup(',
               'strdup(', 'HeapAlloc(', 'LocalAlloc(', 'GlobalAlloc(',
               'CoTaskMemAlloc(')

RELEASE_CALLS = ('delete ', 'delete[]', 'free(', 'HeapFree', 'LocalFree',
                 'GlobalFree', 'CloseHandle', 'Release()', 'RegCloseKey',
                 'CoTaskMemFree', 'DeleteObject', 'UnmapViewOfFile')

# [R87] shared skip set (was a 7th copy of the same literal)
from scan_scope import SKIP_DIRS
import sys as _sys, pathlib as _pl
_sys.path.insert(0, str(_pl.Path(__file__).resolve().parent))

ALLOC_RE = re.compile(
    r'(m_\w+|p[A-Z]\w*)\s*=\s*(?:new\b|\([^)]*\)\s*(?:malloc|calloc|HeapAlloc)'
    r'|(?:malloc|calloc|realloc|_tcsdup|StrDup|strdup|HeapAlloc|LocalAlloc'
    r'|GlobalAlloc|CoTaskMemAlloc)\s*\()')


def read_text(p):
    for enc in ('utf-8-sig', 'utf-8', 'gbk'):
        try:
            return p.read_text(encoding=enc)
        except UnicodeDecodeError:
            continue
    return p.read_text(encoding='utf-8', errors='replace')


def strip_comments(txt):
    txt = re.sub(r'/\*.*?\*/', ' ', txt, flags=re.S)
    return '\n'.join(re.sub(r'//.*$', '', ln) for ln in txt.splitlines())


def collect(dirs):
    cor = {}
    for d in dirs:
        root = pathlib.Path(d)
        if not root.is_dir():
            continue
        for f in root.rglob('*'):
            if not f.is_file() or f.suffix.lower() not in ('.h', '.hpp', '.cpp', '.cxx'):
                continue
            if {x.lower() for x in f.parts} & SKIP_DIRS:
                continue
            cor[f] = strip_comments(read_text(f))
    return cor


def class_spans(text):
    """Yield (class_name, body_text)."""
    out = []
    pat = re.compile(r'\b(?:class|struct)\s+(?:UILIB_API\s+)?(\w+)\s*(?::[^{]*)?\{')
    for m in pat.finditer(text):
        i = text.find('{', m.start())
        depth = 0
        j = i
        while j < len(text):
            if text[j] == '{':
                depth += 1
            elif text[j] == '}':
                depth -= 1
                if depth == 0:
                    break
            j += 1
        out.append((m.group(1), text[i:j + 1]))
    return out


def method_bodies(cls, corpus_text):
    """All out-of-line DEFINITIONS belonging to cls.

    The first version matched `ClassName::` anywhere, which also matched CALLS such as
    `CControlUI::Activate()` inside unrelated methods -- and then took the next
    {...} block as "this class's body". That swept CComboUI/CEditUI/CHotKeyUI bodies
    into CControlUI and produced bogus rows (CControlUI "owning" m_pWindow, a member
    that does not exist in that class at all).

    A definition looks like `<ret> Class::Name(args) ... {`. Requiring a return-type
    token before `Class::`, an identifier and '(' after it, and line-anchoring the
    match excludes qualified calls.
    """
    bodies = []
    pat = re.compile(
        r'^[ 	]*[\w:<>,*&\[\] ]*?(?:^|[^A-Za-z0-9_])' + re.escape(cls) +
        r'\s*::~?\s*\w+\s*\([^;{}]*\)[^;{}]*\{',
        re.M)
    for m in pat.finditer(corpus_text):
        i = corpus_text.find('{', m.start())
        if i < 0:
            continue
        depth = 0
        j = i
        while j < len(corpus_text):
            if corpus_text[j] == '{':
                depth += 1
            elif corpus_text[j] == '}':
                depth -= 1
                if depth == 0:
                    break
            j += 1
        bodies.append(corpus_text[m.start():j + 1])
    return bodies
def releases_member(member, text):
    for call in RELEASE_CALLS:
        if call not in text:
            continue
        # release must mention the member somewhere in the same statement
        for stmt in re.split(r'[;{}]', text):
            if call in stmt and re.search(r'(?<![A-Za-z0-9_])' + re.escape(member) + r'(?![A-Za-z0-9_])', stmt):
                return True
    return False


# Windows that own themselves: DuiLib's native-host windows delete themselves in
# OnFinalMessage (CEditWnd etc.), and at that moment they clear the owning control's
# back-pointer (`m_pOwner->m_pWindow = nullptr`). So the control never frees it and
# never should -- ownership moved to the window. Verified mechanically: every class
# below has `delete this` in OnFinalMessage AND clears the owner's pointer.
SELF_DELETING_WINDOWS = {
    'CComboWnd', 'CDateTimeWnd', 'CEditWnd', 'CHotKeyWnd',
    'CIPAddressWnd', 'CMenuWnd', 'CRichEditWnd',
}


def owned_by_self_deleting_window(member, corpus_text):
    """True when the member is a self-deleting window assigned from `new`.

    The assignment shape is `m_pWindow = new CEditWnd();` -- so if the type being
    constructed is a known self-deleting window, no leak.
    """
    for cls_w in SELF_DELETING_WINDOWS:
        if re.search(re.escape(member) + r'\s*=\s*new\s+' + re.escape(cls_w) + r'(?:[^A-Za-z0-9_]|$)',
                     corpus_text):
            return True
    return False


def attached_as_dialog_root(member, corpus_text):
    """True when the member is handed to CPaintManagerUI::AttachDialog.

    AttachDialog stores the pointer in m_pRoot (UIManager.cpp:2257) and
    CPaintManagerUI's destructor deletes m_pRoot (UIManager.cpp:422). So passing a
    member to AttachDialog transfers ownership to the manager, which does free it.

    Round 60 reported CComboWnd::m_pLayout and CMenuWnd::m_pLayout as leaks by
    missing exactly this: both do `m_pm.AttachDialog(m_pLayout)`. Both are owned by
    their window's embedded paint manager and freed in its destructor. The
    "leak" was my reading error, not a product defect.
    """
    return bool(re.search(r'AttachDialog\s*\(\s*' + re.escape(member) + r'(?:[^A-Za-z0-9_]|$)',
                          corpus_text))


def handed_off(member, cls, class_body, corpus_text):
    """Member returned to a caller, or assigned into another object's member."""
    if re.search(r'return\s+' + re.escape(member) + r'\s*;', corpus_text):
        return True
    if re.search(r'\.\s*\w+\s*=\s*' + re.escape(member) + r'\b', corpus_text):
        return True
    if re.search(r'->\s*\w+\s*=\s*' + re.escape(member) + r'\b', corpus_text):
        return True
    return False


def verify_attach_ownership(corpus):
    """Assert the ownership chain that makes AttachDialog an exempt shape.

    The exemption above is only valid while two lines exist:
        AttachDialog stores the control:   m_pRoot = pControl;
        ~CPaintManagerUI frees it:         if (m_pRoot != nullptr) delete m_pRoot;
    If either disappears, every `m_p<Layout>` passed to AttachDialog becomes a real
    leak again -- so the exemption has to be conditional on the mechanism, not on the
    convention. Round 60 got this wrong by grepping with a tool that could not match
    the pattern and concluding "never assigned"; this check is the guard against
    repeating that.
    """
    mgr = corpus.get('UIManager.cpp')
    if mgr is None:
        for f, body in corpus.items():
            if f.name == 'UIManager.cpp':
                mgr = body
                break
    if mgr is None:
        return None, 'UIManager.cpp not in corpus'
    store = re.search(r'm_pRoot\s*=\s*pControl\s*;', mgr)
    free = re.search(r'if\s*\(\s*m_pRoot\s*!=\s*nullptr\s*\)\s*delete\s+m_pRoot\s*;', mgr)
    if not store:
        return False, 'AttachDialog no longer stores pControl into m_pRoot'
    if not free:
        return False, '~CPaintManagerUI no longer deletes m_pRoot'
    return True, 'store@%d free@%d' % (
        mgr.count(chr(10), 0, store.start()) + 1,
        mgr.count(chr(10), 0, free.start()) + 1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dir', action='append', default=None)
    ap.add_argument('--inject', action='store_true',
                    help='self-test: require at least one LEAK finding')
    ap.add_argument('--all', action='store_true', help='also print EXTERNAL rows')
    args = ap.parse_args()

    dirs = args.dir or ['DuiLib', 'DuiLib/Core', 'DuiLib/Utils', 'DuiLib/Control', 'DuiLib/Layout']
    corpus = collect(dirs)
    if not corpus:
        print('[FAIL] zero source files -- a scan that reads nothing cannot pass')
        return 2
    corpus_text = '\n'.join(corpus.values())

    leaks = []
    external = []
    checked = 0

    for fpath, body in corpus.items():
        for cls, cbody in class_spans(body):
            # Allocations may sit in an out-of-line constructor (CLeaky::CLeaky()),
            # not only in the class body. The first version scanned only the body and
            # therefore reported "0 classes with member allocations" on a file whose
            # whole point was to leak -- a silent no-op caught by the injection test.
            mdefs = method_bodies(cls, corpus_text)
            alloc_scope = cbody + chr(10) + chr(10).join(mdefs)

            # Only names DECLARED AS MEMBERS of this class count. ALLOC_RE also
            # matches locals like `CAnimationData* pAnimation = new ...` -- a local is
            # not a leak when the function returns, and counting it produced rows
            # such as "CUIAnimation leaks pAnimation" twice (once per file).
            member_decl = set()
            for line in cbody.splitlines():
                s = line.strip()
                if not s or s.startswith(('//', '#')):
                    continue
                mdecl = re.match(
                    r'(?:mutable\s+)?(?:const\s+)?[\w:<>,*&\[\] ]+?[\s*&]'
                    r'((?:m_|p[A-Z])\w*)\s*(?:\[[^\]]*\])?\s*[;=]', s)
                if mdecl:
                    member_decl.add(mdecl.group(1))
            if not member_decl:
                continue

            allocs = set(ALLOC_RE.findall(alloc_scope)) & member_decl
            if not allocs:
                continue
            checked += 1
            scope = cbody + '\n' + '\n'.join(mdefs)
            for member in sorted(allocs):
                if releases_member(member, scope):
                    continue
                row = (cls, fpath.name, member)
                if owned_by_self_deleting_window(member, scope):
                    external.append(row)
                elif attached_as_dialog_root(member, scope):
                    external.append(row)
                elif handed_off(member, cls, cbody, scope):
                    external.append(row)
                else:
                    leaks.append(row)

    ok, why = verify_attach_ownership(corpus)
    if ok is None:
        print('[WARN] AttachDialog ownership not verifiable (%s)' % why)
    elif ok:
        print('[INFO] AttachDialog ownership chain intact (%s)' % why)
    else:
        print('[FAIL] AttachDialog ownership chain broken: %s' % why)
        print('       The AttachDialog exemption is no longer valid -- members passed to')
        print('       it must now be treated as leaks.')
        leaks.append(('<ownership-chain>', 'UIManager.cpp', 'm_pRoot'))

    print('[INFO] files=%d classes_with_member_alloc=%d' % (len(corpus), checked))
    if not leaks:
        print('[PASS] every allocated member has a release path in its owning class')

    if args.all and external:
        print('')
        print('[INFO] %d member(s) allocated here but handed to another owner '
              '(not counted as leaks):' % len(external))
        for cls, fname, member in external:
            print('       %-28s %-26s %s' % (cls, fname, member))

    # Baseline: known leaks that were read and are queued, not dismissed. Same
    # reasoning as the export ratchet -- a gate that is red from day one gets ignored.
    baseline_file = pathlib.Path('tools/member_leak_baseline.txt')
    known = set()
    if baseline_file.is_file():
        for line in baseline_file.read_text(encoding='utf-8').splitlines():
            line = line.strip()
            if line and not line.startswith('#'):
                known.add(line)

    # A baseline row that matches nothing is worse than no row: it looks like the
    # finding is tolerated while the scanner keeps failing. Round 62 hit this by
    # writing `Class|File` where the format is `Class|Member` -- the tool then reported
    # the same item as NEW. So unused rows are reported explicitly.
    used = set()
    for r in leaks:
        used.add('%s|%s' % (r[0], r[2]))
    unused = sorted(known - used)
    if unused:
        print('[WARN] %d baseline row(s) matched nothing (stale or wrong format):'
              % len(unused))
        for u in unused[:10]:
            print('       ' + u)
        print('       expected format is <Class>|<Member>')

    known_rows = [r for r in leaks if '%s|%s' % (r[0], r[2]) in known]
    new_rows = [r for r in leaks if '%s|%s' % (r[0], r[2]) not in known]
    if known_rows:
        print('[INFO] %d known leak(s) tolerated by baseline (see %s):'
              % (len(known_rows), baseline_file))
        for cls, fname, member in known_rows:
            print('       %-28s %-26s %s' % (cls, fname, member))
    if new_rows:
        print('[FAIL] %d NEW member leak(s):' % len(new_rows))
        for cls, fname, member in new_rows:
            print('       %-28s %-26s %s' % (cls, fname, member))
    elif leaks:
        print('[PASS] no NEW member leak (baseline covers the known ones)')
    leaks = new_rows

    if args.inject:
        if leaks:
            print('[PASS] inject: %d leak(s) reported, so the check can fail' % len(leaks))
            return 1
        print('[FAIL] inject: expected at least one leak, got none')
        return 3

    return 1 if leaks else 0


if __name__ == '__main__':
    sys.exit(main())
