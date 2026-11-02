#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""gen_attr_whitelist.py -- regenerate the legal skin-attribute whitelist.

Two sources must be merged. Using only the first one produces false positives:

  1) DuiLib/S3AttrRegistry.h -- the s3::PropId enum (~320 names). This is the
     fast-path hash registry used by CControlUI/CContainerUI SetAttribute.
  2) The legacy `_tcsicmp(pstrName, _T("..."))` / PropId legacy fallback chains
     in DuiLib/**/*.cpp (~245 names). Control-specific attributes such as
     railbkcolor / thumbbkcolor live ONLY here -- CScrollBarUI::SetAttribute
     parses them, but they never appear in the PropId enum.

Measured: PropId alone missed railbkcolor/thumbbkcolor/thumbpushedbkcolor, so
the audit reported the VScrollBar default block as broken. It was not. That is
exactly the false-positive class this generator exists to prevent.

Usage:
  python gen_attr_whitelist.py --root D:/DuiLib_Ultimate-master -o _propids.txt

ASCII-only output.
"""

import argparse
import glob
import os
import re
import sys

# Attributes registered by project code (grep: lstrcmpiW(pstrName, L"...")).
PROJECT_ATTRS = ['navicon', 'capkind']


def props_from_registry(root):
    path = os.path.join(root, 'DuiLib', 'S3AttrRegistry.h')
    if not os.path.isfile(path):
        return set()
    src = open(path, encoding='utf-8', errors='replace').read()
    m = re.search(r'enum class PropId[^{]*\{(.*?)\};', src, re.S)
    if not m:
        return set()
    body = m.group(1).replace('Unknown = -1', '')
    return {x.strip() for x in body.split(',') if x.strip()}


def props_from_legacy(root):
    """Every _tcsicmp(pstrName, _T("x")) and lstrcmpiW(pstrName, L"x") in DuiLib."""
    out = set()
    dirs = ['DuiLib/Core', 'DuiLib/Control', 'DuiLib/Layout', 'DuiLib/Utils']
    # Three comparison spellings appear in this codebase. Matching only the
    # case-insensitive one missed childvalign, which the architecture build
    # parses with _tcscmp -- that produced 56 false "unknown attribute" hits
    # against a perfectly valid skin.
    pats = [
        re.compile(r'_tcsicmp\s*\(\s*pstrName\s*,\s*_T\("([a-z0-9_]+)"\)'),
        re.compile(r'_tcscmp\s*\(\s*pstrName\s*,\s*_T\("([a-z0-9_]+)"\)'),
        re.compile(r'lstrcmpi?W\s*\(\s*pstrName\s*,\s*L"([a-z0-9_]+)"\)'),
    ]
    for d in dirs:
        for f in glob.glob(os.path.join(root, d, '*.cpp')):
            src = open(f, encoding='utf-8', errors='replace').read()
            for p in pats:
                out.update(p.findall(src))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', required=True)
    ap.add_argument('-o', '--out', required=True)
    args = ap.parse_args()

    reg = props_from_registry(args.root)
    leg = props_from_legacy(args.root)

    # Older trees (e.g. the architecture build) predate S3AttrRegistry.h and
    # parse every attribute through the legacy _tcsicmp chains only. Demanding
    # the registry would make the gate unusable there, so degrade instead --
    # but only when the legacy source actually produced something, otherwise a
    # mis-pointed root would yield an empty whitelist that flags every attribute.
    if not reg:
        if not leg:
            print('[FATAL] neither S3AttrRegistry.h nor legacy attribute chains found under '
                  + args.root)
            return 2
        print('[NOTE] no S3AttrRegistry.h in this tree; legacy chains only')

    merged = sorted(reg | leg | set(PROJECT_ATTRS))
    with open(args.out, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(merged) + '\n')

    print('registry: %d' % len(reg))
    print('legacy:   %d' % len(leg))
    print('merged:   %d  -> %s' % (len(merged), args.out))
    return 0


if __name__ == '__main__':
    sys.exit(main())
