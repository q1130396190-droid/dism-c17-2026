"""Every converted control class must still export its factory entry point.

Why: converting a class from `class UILIB_API X` to member-level export drops
`?CreateControl@X@DuiLib@@...` unless that member is explicitly marked UILIB_API.
DECLARE_DUICONTROL emits it unmarked, so the loss is silent:

  * the build succeeds  -- ControlFactory calls CreateControl from inside the DLL,
                           where the un-exported symbol resolves fine;
  * the app runs fine   -- nothing outside the DLL calls CreateControl;
  * only the ABI diff shows it, and only if someone reads it.

Round 67 hit exactly this on CPageControlUI and caught it by eye. An eye is not a
mechanism. This check asserts: for every control class that is NOT declared
`class UILIB_API`, a `?CreateControl@<Class>@DuiLib@@` export must exist.

Usage:
    python tools/check_control_factory_exports.py --dll bin64/amd64/DuiLib.dll
    python tools/check_control_factory_exports.py --dll ... --inject
"""
import argparse
import os
import pathlib
import re
import subprocess
import sys
import sys as _sys, pathlib as _pl
_sys.path.insert(0, str(_pl.Path(__file__).resolve().parent))
import find_msvc_tool  # [R87] single source of truth for dumpbin

HEADER_DIRS = ['DuiLib/Control', 'DuiLib/Core', 'DuiLib/Layout', 'DuiLib/Utils']

def find_dumpbin():
    # [R87] Single source of truth (see tools/find_msvc_tool.py).
    return find_msvc_tool.find_dumpbin()


def exported_names(dumpbin, dll):
    out = subprocess.run([dumpbin, '/nologo', '/exports', dll],
                         capture_output=True, text=True, errors='replace')
    names = set()
    for line in out.stdout.splitlines():
        m = re.match(r'\s*\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)', line)
        if m:
            names.add(m.group(1))
    return names


def converted_classes():
    """Control classes NOT declared `class UILIB_API` -- the ones at risk.

    A class declared `class UILIB_API X` exports every member implicitly, so its
    CreateControl is exported regardless. Converted classes lost that blanket
    export, which is why only they need an explicit declaration.

    Two false positives had to be excluded (round 87):
      * `enum class StylePresets : int` matches a naive class pattern -- require the
        line to start with `class`;
      * non-control helpers such as ReceiverImplBase live in these directories too.
        A control is identified by deriving from CControlUI (directly or through
        another control), which is the property that makes CreateControl meaningful.
    """
    decl = re.compile(r'^\s*class\s+(UILIB_API\s+)?(\w+)\s*(?::\s*(?:public\s+)?([\w:]+))?')
    blanket, risky, bases = set(), {}, {}
    blanket_base = {}
    for d in HEADER_DIRS:
        root = pathlib.Path(d)
        if not root.is_dir():
            continue
        for f in root.glob('*.h'):
            for line in f.read_text(encoding='utf-8', errors='replace').splitlines():
                m = decl.match(line)
                if not m:
                    continue
                name, base = m.group(2), (m.group(3) or '')
                if m.group(1):
                    blanket.add(name)
                    # Keep the entry WITH a base: headers contain forward
                    # declarations ('class CEditUI;') whose base is empty, and
                    # setdefault() kept whichever came first -- which stopped the
                    # derivation walk at CEditUI (measured round 87: CIPAddressExUI
                    # dropped out of the candidate set, leaving 2 instead of 3).
                    if base and name not in blanket_base:
                        blanket_base[name] = base
                else:
                    # Forward declarations ('class CEditUI;') have no base and land
                    # here. Storing them in `risky` polluted the base lookup: the
                    # walk asked risky first, found '', and stopped -- so
                    # CIPAddressExUI never reached CControlUI (measured round 87).
                    # A real definition always carries a base, so require one.
                    if base and name not in risky:
                        risky[name] = base

    # Walk the derivation chain upward; keep only classes that reach CControlUI.
    def reaches_control(base, seen=None):
        if not base:
            return False
        base = base.split('::')[-1].strip()
        if base == 'CControlUI':
            return True
        if base in blanket:
            # A blanket-exported base is NOT automatically a control: CDelegateBase
            # is declared with UILIB_API and has nothing to do with CControlUI, and
            # treating it as one pulled CDelegate/CDelegateStatic into the candidate
            # set as "controls without a factory" (measured round 87). Recurse into
            # its own base instead of assuming.
            return reaches_control(risky.get(base, blanket_base.get(base, '')), seen)
        seen = seen or set()
        if base in seen:
            return False
        seen.add(base)
        # Look the base up in BOTH maps. Checking only `risky` stopped the walk at
        # any blanket-exported ancestor, so CIPAddressExUI -> CEditUI -> CLabelUI ->
        # CControlUI resolved to False and the class silently dropped out of the
        # candidate set (measured round 87: candidates fell to 2).
        return reaches_control(risky.get(base, blanket_base.get(base, '')), seen)

    return {n for n, b in risky.items() if reaches_control(b)} - blanket


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dll', required=True)
    ap.add_argument('--inject', action='store_true',
                    help='self-test: require a missing factory export to be reported')
    args = ap.parse_args()

    dumpbin = find_dumpbin()
    if dumpbin is None:
        print('[FAIL] dumpbin.exe not found -- cannot inspect exports')
        return 2
    if not os.path.isfile(args.dll):
        print('[FAIL] dll not found: %s' % args.dll)
        return 2

    exported = exported_names(dumpbin, args.dll)
    if not exported:
        print('[FAIL] export table parsed as empty -- a scan that reads nothing cannot pass')
        return 2

    classes = sorted(converted_classes())
    if not classes:
        print('[FAIL] no converted control classes found -- the header scan read nothing')
        return 2
    print('[INFO] converted_classes=%d exports=%d' % (len(classes), len(exported)))

    if args.inject:
        # Round 87: the previous self-test looked for classes already missing an
        # export, so on a healthy tree it reported "the check is blind" -- the
        # injection depended on a pre-existing defect. Fabricate the condition
        # instead: pretend one real class has no export and require the same
        # detection code to notice.
        probe_class = classes[0]
        fabricated = {n for n in exported
                      if not n.startswith('?CreateControl@' + probe_class + '@DuiLib@@')}
        absent = [c for c in classes
                  if not any(n.startswith('?CreateControl@' + c + '@DuiLib@@')
                             for n in fabricated)]
        if absent:
            print('[PASS] inject: %d class(es) without a factory export are detectable '
                  '(fabricated: %s)' % (len(absent), absent[0]))
            return 1
        print('[FAIL] inject: fabricating a missing export was not detected')
        return 3

    missing = [c for c in classes
               if not any(n.startswith('?CreateControl@' + c + '@DuiLib@@') for n in exported)]
    if missing:
        print('[FAIL] %d converted class(es) export no factory entry point:' % len(missing))
        for c in missing:
            print('       ' + c)
        print('       add UILIB_API to its CreateControl declaration; otherwise anything')
        print('       outside the DLL cannot create the control, and nothing fails loudly')
        return 1

    print('[PASS] every converted class still exports its factory entry point (%d classes)'
          % len(classes))
    return 0


if __name__ == '__main__':
    sys.exit(main())
