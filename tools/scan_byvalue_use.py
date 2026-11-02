"""Prove (or disprove) that a class is never copied by value.

Why this exists: round 57 found CMarkup double-freed on copy. Rounds 57-58 classified
the remaining findings into "fixed" and "copy path unreachable", but "unreachable"
was a human reading of a few grep hits -- not evidence that survives a code change.
Three classes stayed in the baseline on that judgement alone.

This tool turns that judgement into a checkable statement. For each named class it
looks for by-value uses, i.e. any appearance of the type that is NOT behind a
pointer or reference:

    CActiveXCtrl*      pointer          -> fine (no copy of the object)
    CActiveXCtrl&      reference        -> fine
    CActiveXCtrl ctrl; declaration      -> BY VALUE, would copy
    void f(CActiveXCtrl x)  parameter   -> BY VALUE, would copy
    CActiveXCtrl Make()     return type -> BY VALUE, would copy

It is a text scan, so it cannot see through `auto`/typedef aliases. That limit is
reported explicitly rather than left implicit: a quote-shaped answer is worse than a
narrow one that says what it covers.

Usage:
    python tools/scan_byvalue_use.py --class CActiveXCtrl --class MenuObserverImpl
    python tools/scan_byvalue_use.py --class Foo --inject   # self-test
"""
import argparse
import pathlib
import re
import sys

# [R87] shared skip set (was a 7th copy of the same literal)
from scan_scope import SKIP_DIRS
import sys as _sys, pathlib as _pl
_sys.path.insert(0, str(_pl.Path(__file__).resolve().parent))


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


def collect_files(dirs):
    files = []
    for d in dirs:
        root = pathlib.Path(d)
        if not root.is_dir():
            continue
        for f in root.rglob('*'):
            if not f.is_file() or f.suffix.lower() not in ('.h', '.hpp', '.cpp', '.cxx'):
                continue
            if {x.lower() for x in f.parts} & SKIP_DIRS:
                continue
            files.append(f)
    return files


def find_byvalue(cls, files):
    """Return [(file, line, text, kind)] for every by-value appearance of cls."""
    hits = []
    # An appearance is fine if immediately followed by * or & (pointer/reference).
    pat = re.compile(r'(?<![A-Za-z0-9_])' + re.escape(cls) + r'(?![A-Za-z0-9_])')
    for f in files:
        body = strip_comments(read_text(f))
        lines = body.splitlines()
        for i, line in enumerate(lines, 1):
            s = line.strip()
            for m in pat.finditer(s):
                after = s[m.end():m.end() + 3].lstrip()
                before = s[:m.start()].rstrip()
                # Pointer / reference: safe.
                if after.startswith('*') or after.startswith('&'):
                    continue
                # Qualified member / static call: `Class::Method(` or `Class::` scope.
                if after.startswith('::'):
                    continue
                # A declaration `Class name;` / `Class name = ...` / `Class name(`
                # -- anchored anywhere on the line, because the same shape appears as a
                # function parameter (`void f(Class x)`) and `re.match` from the line
                # start misses that entirely. That is precisely why the injected
                # violation `void TakeIt(CActiveXCtrl ctrl);` produced no hit.
                decl = re.search(r'(?<![A-Za-z0-9_])' + re.escape(cls) +
                                 r'\s+[A-Za-z_]\w*\s*[;=({,)]', s)
                # `static T name;` at function scope is a single construction, not a
                # copy -- MenuObserverImpl is a function-local static returned by
                # reference. Skip statics, but only that exact shape.
                is_static_local = bool(re.match(r'^(?:static|const\s+static)\s', s))
                if decl and is_static_local:
                    continue
                if decl:
                    # Constructing a temporary for a call that takes a reference looks
                    # identical here; record it and let the caller judge.
                    hits.append((str(f), i, s[:100], 'declaration'))
                    continue
                # Return type: `Class f(` / `Class ClassName(`.
                if re.search(r'(?<![A-Za-z0-9_])' + re.escape(cls) + r'\s+\w+\s*\(', s):
                    hits.append((str(f), i, s[:100], 'parameter-or-return'))
                    continue
                # Constructor/destructor definitions are not copies.
                if re.search(re.escape(cls) + r'\s*::\s*[~]?' + re.escape(cls), s):
                    continue
                if re.match(r'^(?:class|struct)\s', s):
                    continue
                if re.search(r'~\s*' + re.escape(cls) + r'\s*\(', s):
                    continue
    return hits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--class', dest='classes', action='append', default=None)
    ap.add_argument('--dir', action='append', default=None)
    ap.add_argument('--inject', action='store_true',
                    help='self-test: require at least one by-value hit')
    args = ap.parse_args()

    classes = args.classes or []
    if not classes:
        print('[FAIL] no --class given')
        return 2

    dirs = args.dir or ['DuiLib', 'Demos']
    files = collect_files(dirs)
    if not files:
        print('[FAIL] zero source files found -- a scan that reads nothing cannot pass')
        return 2

    total_hits = 0
    print('[INFO] files_scanned=%d classes=%d' % (len(files), len(classes)))
    for cls in classes:
        hits = find_byvalue(cls, files)
        total_hits += len(hits)
        if hits:
            print('[FAIL] %s: %d by-value appearance(s)' % (cls, len(hits)))
            for path, line, text, kind in hits[:10]:
                print('       %-34s L%-5d %-22s %s'
                      % (pathlib.Path(path).name, line, kind, text[:70]))
        else:
            print('[PASS] %s: no by-value use found (pointer/reference only)' % cls)

    print('[INFO] note: text scan; aliases via auto/typedef are not resolved')

    if args.inject:
        if total_hits:
            print('[PASS] inject: %d hit(s) reported, so the check can fail' % total_hits)
            return 1
        print('[FAIL] inject: expected at least one by-value hit, got none')
        return 3

    return 1 if total_hits else 0


if __name__ == '__main__':
    sys.exit(main())
