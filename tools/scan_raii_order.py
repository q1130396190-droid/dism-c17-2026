"""Find RAII-destructor-order bugs: a `return` expression that uses a resource whose
release happens in the destructor of an object that is still alive at that point.

Why this exists: AppxCapUpdService::SaveCapCache ended with

    FileGuard hFile = ::CreateFileW(sTmp...);
    ...
    return ::MoveFileExW(sTmp, sPath, MOVEFILE_REPLACE_EXISTING) != FALSE;

The `return` expression is evaluated BEFORE FileGuard's destructor runs, so the write
handle was still open, the rename failed with ERROR_SHARING_VIOLATION (32), and the
function returned false on every single call. The cache was never persisted and
nothing crashed, logged, or looked wrong. 48 rounds of gates missed it because no
gate had ever called that function.

That is a class of defect, not one instance: any `return <call needing the resource
released>` where the guard is still in scope has the same behaviour.

Method (deliberately conservative -- a false positive here sends someone reading
correct code):
  1. Find local objects whose type has a destructor that releases something
     (name ends in Guard/Lock/Holder/Handle/Cleaner, or the file defines a struct
     with a ~T that calls CloseHandle/DeleteObject/fclose/Unlock...).
  2. Inside the same scope, find `return <expr>;` lines that mention a Win32 call
     known to require the handle to be closed (rename/delete/move).
  3. Report only when both are in the same function and the guard is declared
     BEFORE the return and not inside an inner block that has already closed.

Step 3 is what keeps it quiet: a guard confined to `{ ... }` before the return is
fine, and that is exactly how FeatureService gets it right.
"""
import argparse
import pathlib
import re
import sys

# Calls that fail if the same file is still open for writing by this process.
NEEDS_CLOSED = {
    'MoveFileExW', 'MoveFileW', 'MoveFileExA', 'MoveFileA',
    'DeleteFileW', 'DeleteFileA',
    'RemoveDirectoryW', 'RemoveDirectoryA',
    'ReplaceFileW', 'CopyFileW',
}

# Destructor bodies that release a resource -> the type is a guard.
RELEASE_CALLS = ('CloseHandle', 'DeleteObject', 'DeleteDC', 'fclose', 'ReleaseDC',
                 'UnlockFile', 'UnmapViewOfFile', 'RegCloseKey', 'FindClose',
                 'DestroyWindow', 'LocalFree', 'GlobalFree', 'ReleaseCapture')

GUARD_NAME_RE = re.compile(r'\b(\w*(?:Guard|Lock|Holder|Cleaner|HandlePtr|Handle)\w*)\b')
DTOR_RE = re.compile(r'~(\w+)\s*\(\s*\)[^;{]*\{([^}]*)\}')
DECL_RE = re.compile(r'^\s*(\w[\w:<>,\s\*&]*?)\s+(\w+)\s*(?:=\s*[^;]+|\{[^}]*\}|\([^)]*\))\s*;')


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


def guard_types(text):
    """Types whose destructor releases a resource.

    Two sources: an explicit destructor body in this file that calls a release API,
    and a name that matches the conventional guard naming.
    """
    found = set()
    for m in DTOR_RE.finditer(text):
        body = m.group(2)
        if any(c in body for c in RELEASE_CALLS):
            found.add(m.group(1))
    for m in GUARD_NAME_RE.finditer(text):
        found.add(m.group(1))
    return found


def find_bugs(path):
    """Report `return <call needing the resource released>` where a guard is alive.

    The first version tracked braces but never RESET the guard table at a function
    boundary, so a guard declared in one function was still "alive" when a later
    function's return line was examined -- both findings it produced were bogus.
    Functions are now split explicitly and each one is analysed on its own.
    """
    raw = read_text(path)
    body = strip_comments(raw)
    if not body.strip():
        return []
    guards = guard_types(body)
    if not guards:
        return []

    lines = body.splitlines()
    hits = []

    # Split into function spans by brace depth: a signature line (no leading indent
    # that ends in `)`) followed by a line of only `{` opens a new function span.
    # The previous version accepted any `... )` line, which also matched `if (...)`,
    # so functions were sliced into fragments and the real defect was missed.
    starts = [0]
    for i in range(len(lines) - 1):
        head = lines[i].rstrip()
        brace = lines[i + 1].strip()
        if brace == '{' and head.endswith(')') and not head.lstrip().startswith(('if', 'for', 'while', 'switch', 'else')):
            starts.append(i)
    starts = sorted(set(starts))
    starts.append(len(lines))

    for si in range(len(starts) - 1):
        lo, hi = starts[si], starts[si + 1]
        span = lines[lo:hi]
        # Guards declared in THIS function, with the brace depth of their scope.
        guard_depth = {}
        depth = 0
        for off, line in enumerate(span):
            stripped = line.strip()

            # A return that needs the resource released, while a guard is still in
            # scope (its declaration depth is at or below the current depth, and the
            # block that declared it has not closed).
            if 'return' in stripped:
                for call in NEEDS_CLOSED:
                    if re.search(r'(?:^|[^A-Za-z0-9_])' + call + r'\s*\(', stripped):
                        if guard_depth:
                            for g in guard_depth:
                                hits.append((lo + off + 1, g, call, stripped[:100]))
                        break

            m = DECL_RE.match(line)
            if m:
                base = m.group(1).split('::')[-1].strip()
                if base in guards:
                    guard_depth[m.group(2)] = depth

            # Update depth and drop guards whose scope just closed.
            closes = stripped.count('}')
            opens = stripped.count('{')
            depth += opens - closes
            for g in [g for g, d in guard_depth.items() if d > depth]:
                del guard_depth[g]
    return hits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dir', action='append', default=None)
    ap.add_argument('--inject', action='store_true',
                    help='self-test: expect at least one finding')
    args = ap.parse_args()

    dirs = args.dir or ['Demos/Win8Metro']
    files = []
    for d in dirs:
        root = pathlib.Path(d)
        if not root.is_dir():
            print('[FAIL] source directory not found: %s' % root)
            return 2
        files.extend(sorted(root.glob('*.cpp')))
    if not files:
        print('[FAIL] zero .cpp files found -- a scan that reads nothing cannot pass')
        return 2

    findings = []
    for f in files:
        for line_no, gname, call, snippet in find_bugs(f):
            findings.append((f.name, line_no, gname, call, snippet))

    print('[INFO] scanned cpp=%d guard_types_detected_in_each_file' % len(files))
    if findings:
        print('[FAIL] %d return() using a resource while its RAII guard is still alive:' % len(findings))
        for name, ln, g, c, s in findings:
            print('       %-34s L%-5d guard=%-22s call=%s' % (name, ln, g, c))
            print('           %s' % s)
    else:
        print('[PASS] no return expression uses a resource still held by a live guard')

    if args.inject:
        if findings:
            print('[PASS] inject: %d finding(s) reported, so the check can fail' % len(findings))
            return 1
        print('[FAIL] inject: expected at least one finding, got none -- the check cannot fail')
        return 3

    return 1 if findings else 0


if __name__ == '__main__':
    sys.exit(main())
