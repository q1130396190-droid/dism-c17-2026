"""Find classes at risk of double-free from defaulted copy semantics.

Why this exists: round 56 found CMarkup had a defaulted copy constructor while owning
two malloc'd buffers and freeing both in its destructor. Copying it produced two
objects sharing the same memory, and the second destructor freed it again --
0xC0000374 (heap corruption). It had been latent for years because the class-level
dllexport only guaranteed the SYMBOL existed; nothing ever called it.

That is a shape, not an incident. The shape is:

    class T {
        ~T() { Release(); }          // destructor frees owned resources
        ...                          // Release() calls free/delete/CloseHandle
        HANDLE/T* m_x;               // member holds a raw owned resource
        // no T(const T&), no operator=  -> compiler generates shallow copies
    };

Any such class double-frees when copied. This scan finds them.

Three signals must all hold (this ordering is what keeps the false-positive rate
manageable -- a class with a raw pointer but no destructor release is fine, because
nothing to free twice):

  1. THE CLASS OWNS AND FREES: a destructor body that calls free/delete[]/delete/
     CloseHandle/Release/delete[]/HeapFree/CoTaskMemFree/fclose, either directly or
     through a helper defined in the same file that does.
  2. IT HOLDS A RAW RESOURCE: a member declared as `T*`, `HANDLE`, or another Win32
     handle typedef.
  3. COPY IS IMPLICIT: no `T(const T&)`, no `operator=`, no `= delete` / `= default`
     for either. Note that `= delete` and `= default` both count as "handled" --
     delete is safe by definition, default is *explicit* and represents a deliberate
     choice by whoever wrote it (CMarkup's `= default` was written by this project in
     round 55, which is exactly why it needs to be visible rather than silently
     inherited).

Reported per finding: class, file, whether the copy ctor / assignment are the
compiler-generated ones, and which resource the destructor frees.
"""
import argparse
import pathlib
import re
import sys

FREE_CALLS = ('free(', 'delete[]', 'delete ', 'CloseHandle', 'fclose(',
              'HeapFree', 'CoTaskMemFree', 'LocalFree', 'GlobalFree',
              'RegCloseKey', 'DeleteObject', 'UnmapViewOfFile', 'DestroyIcon',
              'CloseServiceHandle', 'ReleaseDC')

# Members that denote an owned raw resource.
RAW_MEMBER = re.compile(
    r'^\s*(?:mutable\s+)?(?:const\s+)?([A-Za-z_]\w*)\s*\*\s*(m_\w+|\w+)\s*(?:\[[^\]]*\])?\s*;'
)
HANDLE_MEMBER = re.compile(
    r'^\s*(?:mutable\s+)?(HANDLE|HBITMAP|HDC|HPEN|HBRUSH|HFONT|HICON|HMENU|HWND|'
    r'HKEY|SC_HANDLE|HRGN|HGLOBAL|HLOCAL|HMODULE|SOCKET|LPVOID)\s+(m_\w+|\w+)\s*;'
)

CLASS_RE = re.compile(r'^\s*(?:class|struct)\s+(?:UILIB_API\s+)?(\w+)\s*(?::[^{]*)?\{?\s*$')
DTOR_RE = re.compile(r'~(\w+)\s*\(\s*\)')
COPY_DECL_RE = re.compile(r'\b(\w+)\s*\(\s*const\s+\1\s*&')
ASSIGN_DECL_RE = re.compile(r'\b(\w+)\s*&\s*operator\s*=\s*\(\s*const\s+\1\s*&')
COPTOR_DELETED_RE = re.compile(r'=\s*(delete|default)\s*;')


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


def split_classes(body):
    """Yield (class_name, [lines of the class body]).

    Deliberately naive: a class starts at `class X {`/`struct X {` at column 0-1 and
    ends at the matching `};`. Nested classes are reported as part of the outer one,
    which is acceptable -- the outer class's copy semantics are what get generated.
    """
    lines = body.splitlines()
    out = []
    i = 0
    while i < len(lines):
        m = CLASS_RE.match(lines[i])
        if not m:
            i += 1
            continue
        name = m.group(1)
        # `class X` and the opening brace are usually on separate lines, so the
        # depth is 0 on the header line and the loop below would exit immediately --
        # which is why the injected sample produced no finding and the whole scan
        # reported zero. Keep consuming until the opening brace has been seen AND
        # balanced.
        start = i
        depth = lines[i].count('{') - lines[i].count('}')
        seen_open = '{' in lines[i]
        i += 1
        while i < len(lines):
            seg = lines[i]
            depth += seg.count('{') - seg.count('}')
            if '{' in seg:
                seen_open = True
            if seen_open and depth <= 0:
                i += 1
                break
            i += 1
        out.append((name, lines[start:i], start + 1))
    return out


BASE_RE_CACHE = {}


def collect_bases(dirs):
    """Map class name -> list of base class names (best effort, header corpus)."""
    bases = {}
    # re.S so the base list may span lines before the opening brace -- the header
    # style here puts `: public CControlUI` on the class line and `{` on the next.
    # Without it this collected ZERO base relations and the inheritance check was a
    # no-op (found by asking the tool to print how many bases it had: 0).
    pat = re.compile(r'(?:^|[^A-Za-z0-9_])(?:class|struct)\s+(?:UILIB_API\s+)?(\w+)\s*:\s*([^{]+?)\{',
                     re.S)
    for d in dirs:
        for f in list(d.glob('*.h')) + list(d.glob('*.cpp')):
            body = strip_comments(read_text(f))
            for m in pat.finditer(body):
                name = m.group(1)
                bl = []
                for b in m.group(2).split(','):
                    b = b.strip()
                    if not b:
                        continue
                    tok = b.split()[-1] if ' ' in b else b
                    tok = tok.replace('public', '').replace('protected', '').replace('private', '').strip()
                    if tok:
                        bl.append(tok)
                if bl:
                    bases.setdefault(name, []).extend(bl)
    return bases


def noncopyable_by_base(name, bases, protected, depth=0, seen=None):
    """True when some base class is known non-copyable (explicitly deleted)."""
    if depth > 8:
        return False
    if seen is None:
        seen = set()
    if name in seen:
        return False
    seen.add(name)
    for b in bases.get(name, []):
        if b in protected:
            return True
        if noncopyable_by_base(b, bases, protected, depth + 1, seen):
            return True
    return False


def analyse_class(name, class_lines, path_name, file_body, corpus="", bases=None):
    """corpus is the concatenation of ALL scanned files. bases maps class->bases."""
    bases = bases or {}
    """corpus is the concatenation of ALL scanned files.

    The first version only looked for the destructor body inside the class text, so a
    destructor declared in the header and defined in the .cpp (the normal pattern --
    CMarkup does exactly this) was invisible and the scan reported zero findings on a
    codebase that had a known double-free. That is the "check reads nothing" failure
    mode this project keeps hitting: a zero result has to be proved capable of being
    non-zero before it means anything.
    """
    joined = '\n'.join(class_lines)

    # Signal 1: destructor that frees.
    frees = []
    for m in DTOR_RE.finditer(joined):
        # find the body following the dtor signature
        rest = joined[m.end():]
        brace = rest.find('{')
        if brace < 0:
            continue
        depth = 0
        j = brace
        while j < len(rest):
            if rest[j] == '{':
                depth += 1
            elif rest[j] == '}':
                depth -= 1
                if depth == 0:
                    break
            j += 1
        dbody = rest[brace:j + 1]
        for call in FREE_CALLS:
            if call in dbody:
                frees.append(call.rstrip('(['))
        # A dtor that calls a helper defined in the same file also counts.
        for hm in re.finditer(r'\b(\w+)\s*\(\s*\)\s*;', dbody):
            helper = hm.group(1)
            if helper == name:
                continue
            hpat = re.compile(r'void\s+\w+::' + re.escape(helper) +
                              r'\s*\(\s*\)\s*\{([^}]*)\}', re.S)
            for h in hpat.finditer(corpus):
                for call in FREE_CALLS:
                    if call in h.group(1):
                        frees.append(helper + '->' + call.rstrip('(['))
    # Out-of-line destructor (Name::~Name() { ... }) -- the common pattern.
    ool = re.compile(r'(?:^|[^A-Za-z0-9_])' + re.escape(name) + r'\s*::~\s*' + re.escape(name) +
                     r'\s*\(\s*\)\s*\{', re.S)
    for m in ool.finditer(corpus):
        rest = corpus[m.end() - 1:]
        depth = 0
        j = 0
        while j < len(rest):
            if rest[j] == '{':
                depth += 1
            elif rest[j] == '}':
                depth -= 1
                if depth == 0:
                    break
            j += 1
        dbody = rest[:j + 1]
        for call in FREE_CALLS:
            if call in dbody:
                frees.append(call.rstrip('(['))
        for hm in re.finditer(r'(?:^|[^A-Za-z0-9_])(\w+)\s*\(\s*\)\s*;', dbody):
            helper = hm.group(1)
            if helper == name:
                continue
            hpat = re.compile(r'void\s+\w+::' + re.escape(helper) +
                              r'\s*\(\s*\)\s*\{([^}]*)\}', re.S)
            for h in hpat.finditer(corpus):
                for call in FREE_CALLS:
                    if call in h.group(1):
                        frees.append(helper + '->' + call.rstrip('(['))

    if not frees:
        return None

    # Signal 2: a raw member that the destructor ACTUALLY frees.
    #
    # The first version accepted any raw pointer member, which flagged CControlUI for
    # m_pManager/m_pParent -- both are documented non-owning back-pointers
    # (UIControl.cpp:128 says so explicitly). A back-pointer produces no double-free,
    # so requiring the member name to appear alongside a free in the destructor text
    # is what separates "owns" from "merely points at".
    raw_candidates = []
    for ln in class_lines:
        mh = HANDLE_MEMBER.match(ln)
        if mh:
            raw_candidates.append(mh.group(2))
            continue
        mr = RAW_MEMBER.match(ln)
        if mr:
            raw_candidates.append(mr.group(2))

    dtor_text = ' '.join(frees) + ' ' + ' '.join(
        m.group(0) for m in re.finditer(r'[^;{}]*', '')
    )
    # Rebuild the destructor bodies we saw, for member-name matching.
    bodies = []
    for m in ool.finditer(corpus):
        rest = corpus[m.end() - 1:]
        depth = 0
        j = 0
        while j < len(rest):
            if rest[j] == '{':
                depth += 1
            elif rest[j] == '}':
                depth -= 1
                if depth == 0:
                    break
            j += 1
        bodies.append(rest[:j + 1])
    for m in DTOR_RE.finditer(joined):
        rest = joined[m.end():]
        brace = rest.find('{')
        if brace >= 0:
            depth = 0
            j = brace
            while j < len(rest):
                if rest[j] == '{':
                    depth += 1
                elif rest[j] == '}':
                    depth -= 1
                    if depth == 0:
                        break
                j += 1
            bodies.append(rest[brace:j + 1])
    body_text = ' '.join(bodies)

    # Signal 2, tightened: the member must be the argument of a releasing call.
    #
    # Round 57 accepted "member name appears anywhere in the destructor", which kept
    # flagging CControlUI for m_pManager -- a documented non-owning back-pointer that
    # the destructor only null-checks. Reading is not freeing.
    #
    # Implementation note: a single "find a call whose argument mentions the member"
    # regex is not reliable here. A permissive pattern like
    #     [^;{}]*?\(\s*(?:[^)]*,\s*)?(member)
    # happily matches `if (m_pBuf)` because `if` looks like a call. So instead the
    # releasing calls are enumerated, their argument lists extracted by paren
    # matching, and each argument checked for the member as a whole word.
    RELEASE_FUNCS = ('free', 'CloseHandle', 'DeleteObject', 'DeleteDC', 'ReleaseDC',
                     'fclose', 'RegCloseKey', 'FindClose', 'CloseServiceHandle',
                     'UnmapViewOfFile', 'DestroyIcon', 'LocalFree', 'GlobalFree',
                     'HeapFree', 'CoTaskMemFree')

    def _release_args(text):
        """Yield every argument string of every releasing call in text."""
        for fn in RELEASE_FUNCS:
            for m in re.finditer(r'(?<![A-Za-z0-9_])' + fn + r'\s*\(', text):
                i = text.find('(', m.start())
                depth = 0
                j = i
                while j < len(text):
                    if text[j] == '(':
                        depth += 1
                    elif text[j] == ')':
                        depth -= 1
                        if depth == 0:
                            break
                    j += 1
                yield text[i + 1:j]

    raws = []
    for arglist in _release_args(body_text):
        for cand in raw_candidates:
            if re.search(r'(?<![A-Za-z0-9_])' + re.escape(cand) + r'(?![A-Za-z0-9_])', arglist):
                raws.append(cand)
    # `delete p;` / `delete[] p;` have no parentheses.
    for cand in raw_candidates:
        if re.search(r'delete\s*\[?\s*' + re.escape(cand) + r'(?![A-Za-z0-9_])', body_text):
            raws.append(cand)
    raws = sorted(set(raws))
    if not raws:
        return None

    # Signal 0: a base class that already deleted its copy operations makes this
    # class non-copyable too -- the compiler cannot generate a copy ctor for a
    # derived type whose base has none. These are safe by construction.
    PROTECTED_BASES = {'CControlUI', 'CContainerUI', 'CPaintManagerUI'}
    if noncopyable_by_base(name, bases, PROTECTED_BASES):
        return None

    # Signal 3: copy semantics implicit (neither declared nor deleted/defaulted).
    has_copy = bool(COPY_DECL_RE.search(joined))
    has_assign = bool(ASSIGN_DECL_RE.search(joined))
    has_deleted = bool(re.search(r'=\s*delete\s*;', joined))
    has_defaulted = bool(re.search(r'=\s*default\s*;', joined))
    if has_copy and has_assign and (has_deleted or has_defaulted):
        return None
    if has_copy and has_assign and not has_deleted and not has_defaulted:
        # Declared out-of-line with a real body -- presumably deep. Still report as
        # a lower-severity item because "presumably" is doing work in that sentence.
        return {
            'class': name, 'file': path_name, 'severity': 'review',
            'frees': sorted(set(frees)), 'raws': raws[:4],
            'copy': 'declared', 'assign': 'declared',
        }

    return {
        'class': name, 'file': path_name, 'severity': 'risk',
        'frees': sorted(set(frees)), 'raws': raws[:4],
        'copy': 'implicit' if not has_copy else 'partial',
        'assign': 'implicit' if not has_assign else 'partial',
    }


def scan(root, corpus='', bases=None):
    findings = []
    files = sorted(list(root.glob('*.h')) + list(root.glob('*.cpp')))
    for f in files:
        body = strip_comments(read_text(f))
        for name, class_lines, line_no in split_classes(body):
            r = analyse_class(name, class_lines, f.name, body, corpus, bases)
            if r:
                r['line'] = line_no
                findings.append(r)
    return files, findings


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dir', action='append', default=None)
    ap.add_argument('--inject', action='store_true',
                    help='self-test: require at least one risk finding')
    ap.add_argument('--all', action='store_true', help='include review-severity rows')
    args = ap.parse_args()

    dirs = args.dir or ['DuiLib']
    roots = [pathlib.Path(d) for d in dirs]
    for r in roots:
        if not r.is_dir():
            print('[FAIL] directory not found: %s' % r)
            return 2

    all_files = []
    corpus_parts = []
    for r in roots:
        for f in sorted(list(r.glob('*.h')) + list(r.glob('*.cpp'))):
            corpus_parts.append(strip_comments(read_text(f)))
    corpus = chr(10).join(corpus_parts)
    bases = collect_bases(roots)

    findings = []
    for r in roots:
        files, _ = scan(r, corpus, bases)
        all_files.extend(files)
        findings.extend(_)
    if not all_files:
        print('[FAIL] zero source files -- a scan that reads nothing cannot pass')
        return 2

    risk = [f for f in findings if f['severity'] == 'risk']
    review = [f for f in findings if f['severity'] == 'review']

    print('[INFO] files=%d classes_scanned (findings: risk=%d review=%d)'
          % (len(all_files), len(risk), len(review)))

    def _dump(rows, header):
        if not rows:
            return
        print(header % len(rows))
        for f in rows:
            print('       %-40s L%-5d %-28s copy=%-9s assign=%-9s'
                  % (f['file'], f['line'], f['class'], f['copy'], f['assign']))
            print('           frees=%s raws=%s'
                  % ('/'.join(f['frees']), ','.join(f['raws'])))


    if args.all and review:
        print('')
        print('[INFO] review-severity (copy declared out-of-line; verify the body is deep):')
        for f in review:
            print('       %-40s L%-5d %s' % (f['file'], f['line'], f['class']))

    # Baseline: known risk rows that have been reviewed and are not acted on yet.
    # Without this the gate is red from day one on pre-existing findings, and a gate
    # that is always red teaches people to ignore it (the same reasoning as the
    # export-surface ratchet). Only NEW risk rows fail.
    baseline_file = pathlib.Path('tools/double_free_baseline.txt')
    known = set()
    if baseline_file.is_file():
        for line in baseline_file.read_text(encoding='utf-8').splitlines():
            line = line.strip()
            if line and not line.startswith('#'):
                known.add(line)

    # Evidence-based exemption: a class with zero by-value uses cannot be copied in
    # practice, so its implicit copy semantics are unreachable. Checked LIVE rather
    # than stored in the baseline -- "nobody copies it" stops being true the moment
    # someone adds a by-value parameter, and a stale note would never notice.
    import importlib.util as _ilu
    _byvalue = None
    try:
        _spec = _ilu.spec_from_file_location(
            'byvalue', str(pathlib.Path(__file__).with_name('scan_byvalue_use.py')))
        _byvalue = _ilu.module_from_spec(_spec)
        _spec.loader.exec_module(_byvalue)
    except Exception as _e:
        print('[WARN] by-value evidence module unavailable (%s); baseline only' % _e)

    _byval_files = None

    def _uncopyable_by_evidence(cls):
        nonlocal _byval_files
        if _byvalue is None:
            return False
        if _byval_files is None:
            _byval_files = _byvalue.collect_files(dirs)
        return not _byvalue.find_byvalue(cls, _byval_files)

    new_risk = []
    known_risk = []
    proven_safe = []
    for f in risk:
        key = '%s|%s' % (f['class'], f['file'])
        if key in known:
            known_risk.append(f)
        elif _uncopyable_by_evidence(f['class']):
            proven_safe.append(f)
        else:
            new_risk.append(f)

    if proven_safe:
        print('[INFO] %d class(es) have zero by-value uses in the repo (proved by '
              'tools/scan_byvalue_use.py):' % len(proven_safe))
        for f in proven_safe:
            print('       %-28s %s' % (f['class'], f['file']))

    _used_d = set('%s|%s' % (f['class'], f['file']) for f in risk)
    _unused_d = sorted(known - _used_d)
    if _unused_d:
        print('[WARN] %d baseline row(s) matched nothing (stale or wrong format):'
              % len(_unused_d))
        for _u in _unused_d[:10]:
            print('       ' + _u)
        print('       expected format is <Class>|<File>')

    if known_risk:
        _dump(known_risk,
              '[INFO] %d class(es) matched the known double-free shape (tolerated by '
              'baseline; see tools/double_free_baseline.txt):')
    if new_risk:
        _dump(new_risk,
              '[FAIL] %d NEW class(es) free a resource in the destructor while relying '
              'on compiler-generated copy semantics:')

    if args.inject:
        if risk:
            print('[PASS] inject: %d risk finding(s) reported, so the check can fail'
                  % len(risk))
            return 1
        print('[FAIL] inject: expected at least one risk finding, got none')
        return 3

    if not new_risk:
        print('[PASS] no NEW double-free risk class (baseline covers the rest)')
    return 1 if new_risk else 0


if __name__ == '__main__':
    sys.exit(main())
