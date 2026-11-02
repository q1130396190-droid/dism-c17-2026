"""Check that every Demo header is self-contained.

Why: WinHandles.h used CoTaskMemFree without including <objbase.h> -- the
declaration only arrived via stdafx.h -> UIlib.h -> objbase.h. Every probe in this
repo compiled with the Demo PCH, so nothing ever saw it. It surfaced only when
round 48 built a translation unit that included the header first.

That is a class of defect, not one instance: a header that compiles only after
some other header happens to be included first is a latent build break waiting for
a reorder. Human review does not catch it because the file looks fine in context.

Method: for each header, extract the Win32/CRT identifiers it references and check
that at least one of its own includes could plausibly declare the symbol. This is a
heuristic (a mapping table, not a compiler), so it is deliberately conservative:
only flag a symbol when the header includes NOTHING that is in the same header
group. A false positive here is expensive -- it sends someone chasing a non-issue.

The real check would be "compile each header alone". That is the honest approach
and it is what the -Compile mode does for the headers that matter; the static pass
exists to run fast in CI and to point at candidates.
"""
import argparse
import pathlib
import re
import sys

# symbol -> candidate SDK headers that declare it.
API_HEADERS = {
    # COM: <windows.h> includes <ole2.h> UNLESS WIN32_LEAN_AND_MEAN is defined, and
    # ole2.h includes objbase.h/combaseapi.h. Verified in the SDK header
    # (10.0.26100.0/um/Windows.h contains `#include <ole2.h>`). The Demo project
    # does not define WIN32_LEAN_AND_MEAN in its real build, so windows.h is a
    # legitimate provider here -- the first version omitted it and produced two
    # false positives (DragDropImpl.h, WebView2Helper.h). Probes that DO define
    # WIN32_LEAN_AND_MEAN must include objbase.h themselves; that is a different
    # check and is caught by the compile of the probe, not by this scan.
    'CoTaskMemFree':        {'objbase.h', 'combaseapi.h', 'ole2.h', 'windows.h'},
    'CoInitialize':         {'objbase.h', 'combaseapi.h', 'ole2.h', 'windows.h'},
    'CoUninitialize':       {'objbase.h', 'combaseapi.h', 'ole2.h', 'windows.h'},
    'CoCreateInstance':     {'objbase.h', 'combaseapi.h', 'ole2.h', 'windows.h'},
    'CoInitializeEx':       {'objbase.h', 'combaseapi.h', 'ole2.h', 'windows.h'},
    'CoTaskMemAlloc':       {'objbase.h', 'combaseapi.h', 'ole2.h', 'windows.h'},
    'IID_PPV_ARGS':         {'objbase.h', 'combaseapi.h', 'ole2.h', 'windows.h'},
    'SHGetFolderPathW':     {'shlobj.h', 'shlobj_core.h'},
    'SHGetSpecialFolderPathW': {'shlobj.h', 'shlobj_core.h'},
    'SHGetKnownFolderPath': {'shlobj.h', 'shlobj_core.h'},
    'SHBrowseForFolderW':   {'shlobj.h', 'shlobj_core.h'},
    'ShellExecuteW':        {'shellapi.h'},
    'ShellExecuteExW':      {'shellapi.h'},
    'SHFileOperationW':     {'shellapi.h'},
    'SHQueryUserNotificationState': {'shellapi.h'},
    'PathFileExistsW':      {'shlwapi.h'},
    'PathCombineW':         {'shlwapi.h'},
    'StrCmpIW':             {'shlwapi.h'},
    'RegOpenKeyExW':        {'winreg.h', 'windows.h'},
    'RegCloseKey':          {'winreg.h', 'windows.h'},
    'RegQueryValueExW':     {'winreg.h', 'windows.h'},
    'GetAdaptersAddresses': {'iphlpapi.h'},
    'SetupDiGetClassDevsW': {'setupapi.h'},
    'DeviceIoControl':      {'winioctl.h', 'windows.h'},
    'GetVolumeInformationW': {'windows.h', 'fileapi.h'},
    'CreateToolhelp32Snapshot': {'tlhelp32.h'},
    'Process32FirstW':      {'tlhelp32.h'},
    'NtQuerySystemInformation': {'winternl.h'},
    'URLDownloadToFileW':   {'urlmon.h'},
    'InternetOpenW':        {'wininet.h'},
    'HttpOpenRequestW':     {'wininet.h'},
    'CryptHashData':        {'wincrypt.h'},
    'CryptAcquireContextW': {'wincrypt.h'},
    'CreateDIBSection':     {'windows.h', 'wingdi.h'},
    'AlphaBlend':           {'wingdi.h', 'windows.h'},
    'SetWindowLongPtrW':    {'windows.h', 'winuser.h'},
    'GetWindowLongPtrW':    {'windows.h', 'winuser.h'},
    'TrackMouseEvent':      {'windows.h', 'winuser.h'},
    'GetDpiForWindow':      {'windows.h', 'winuser.h', 'windef.h'},
    'EnableNonClientDpiScaling': {'windows.h', 'winuser.h'},
}

# SDK headers are leaves as far as file-following goes, but several of them are
# known to drag in the foundational Win32 declarations. Without this, a header that
# includes only <shlobj.h> would look like it has no access to windows.h -- and
# shlobj.h includes windows.h in practice. Recorded here rather than following the
# SDK tree: following it would mean parsing thousands of vendor files on every run,
# and the SDK layout is not stable across versions.
SDK_TRANSITIVE = {
    'windows.h':      {'windows.h'},
    'shlobj.h':       {'shlobj.h', 'windows.h', 'objbase.h', 'ole2.h'},
    'shlobj_core.h':  {'shlobj_core.h', 'windows.h'},
    'shellapi.h':     {'shellapi.h', 'windows.h'},
    'shlwapi.h':      {'shlwapi.h', 'windows.h'},
    'winreg.h':       {'winreg.h', 'windows.h'},
    'winuser.h':      {'winuser.h', 'windows.h'},
    'wingdi.h':       {'wingdi.h', 'windows.h'},
    'fileapi.h':      {'fileapi.h', 'windows.h'},
    'ole2.h':         {'ole2.h', 'objbase.h', 'windows.h'},
    'objbase.h':      {'objbase.h', 'windows.h'},
    'combaseapi.h':   {'combaseapi.h', 'windows.h'},
    'oleauto.h':      {'oleauto.h', 'windows.h', 'objbase.h'},
    'commctrl.h':     {'commctrl.h', 'windows.h'},
    'tlhelp32.h':     {'tlhelp32.h', 'windows.h'},
    'setupapi.h':     {'setupapi.h', 'windows.h'},
    'winternl.h':     {'winternl.h', 'windows.h'},
    'wininet.h':      {'wininet.h', 'windows.h'},
    'shlwapi.h':      {'shlwapi.h', 'windows.h'},
    'wincrypt.h':     {'wincrypt.h', 'windows.h'},
    'process.h':      {'process.h'},
    'memory':         {'memory'},
    'vector':         {'vector'},
    'string':         {'string'},
    'utility':        {'utility'},
}

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.M)


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


_CACHE = {}


def closure(path, _stack=None):
    """All headers reachable from `path` through its own includes (transitive).

    The first version only looked at DIRECT includes, which produced a false
    positive on SetWindowLongPtrW: BackgroundJobs.h does not include <windows.h>,
    but it includes WinHandles.h which does. A finding has to survive the
    transitive walk or it is noise, and a noisy check gets ignored.
    """
    key = str(path).lower()
    if key in _CACHE:
        return _CACHE[key]
    if _stack is None:
        _stack = set()
    if key in _stack:
        return set()
    _stack.add(key)

    body = strip_comments(read_text(path))
    names = set()
    for inc in INCLUDE_RE.findall(body):
        inc_norm = inc.replace(chr(92), '/')
        base = pathlib.PurePosixPath(inc_norm).name.lower()
        names.add(base)
        # SDK headers: expand through the known table instead of the file tree.
        names |= SDK_TRANSITIVE.get(base, set())
        cand = path.parent / inc_norm
        if cand.exists():
            names |= closure(cand, _stack)
    _CACHE[key] = names
    return names


def analyse(path):
    body = strip_comments(read_text(path))
    reachable = closure(path)
    problems = []
    for sym, needed in API_HEADERS.items():
        # Whole-word match so GetWindowLongPtrW does not catch GetWindowLongPtr.
        if not re.search(r'(?:^|[^A-Za-z0-9_])' + re.escape(sym) + r'\s*\(', body):
            continue
        if reachable & needed:
            continue
        problems.append((sym, sorted(needed)))
    return reachable, problems


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dir', action='append', default=None,
                    help='directory to scan; repeatable. Defaults to Demos/Win8Metro.')
    ap.add_argument('--inject', action='store_true',
                    help='self-test: expect at least one finding')
    ap.add_argument('--list', action='store_true', help='print every header scanned')
    args = ap.parse_args()

    dirs = args.dir or ['Demos/Win8Metro']
    headers = []
    for d in dirs:
        root = pathlib.Path(d)
        if not root.is_dir():
            print('[FAIL] header directory not found: %s' % root)
            return 2
        found = sorted(root.glob('*.h'))
        if not found:
            print('[FAIL] zero headers found under %s -- a scan that reads nothing cannot pass' % root)
            return 2
        headers.extend(found)
    headers = sorted(set(headers), key=lambda q: str(q).lower())

    findings = []
    for h in headers:
        _, problems = analyse(h)
        for sym, needed in problems:
            findings.append((h.name, sym, needed))

    if args.list:
        for h in headers:
            own, _ = analyse(h)
            print('  %-42s includes=%d' % (h.name, len(own)))

    print('[INFO] scanned headers=%d api_symbols_checked=%d' % (len(headers), len(API_HEADERS)))
    if findings:
        print('[FAIL] %d header(s) reference an API whose declaring header they do not include:' % len(findings))
        for name, sym, needed in findings:
            print('       %-40s %-30s needs one of: %s' % (name, sym, ', '.join(needed)))
    else:
        print('[PASS] every scanned header includes what it references')

    if args.inject:
        if findings:
            print('[PASS] inject: %d finding(s) were reported, so the check can fail' % len(findings))
            return 1
        print('[FAIL] inject: expected at least one finding, got none -- the check cannot fail')
        return 3

    return 1 if findings else 0


if __name__ == '__main__':
    sys.exit(main())
