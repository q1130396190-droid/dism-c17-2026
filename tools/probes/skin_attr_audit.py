#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""skin_attr_audit.py -- audit DuiLib skin XML for attributes that silently do nothing.

Why this exists
---------------
R34/R35 found a class of defect that produces NO error, NO warning, and NO log:
an attribute valid only on one control family is written on another one, and
DuiLib simply ignores it. Two real cases:

  * inset on a CTextUI  -> ignored (only CContainerUI parses it, UIContainer.cpp:739)
  * inset on upd_install_state -> 14px of padding that never existed

The same silent-failure shape applies to misspelled or non-existent attribute
names. Neither shows up in a build, a log, or a screenshot review.

Two checks
----------
  CONTAINER_ON_NONCONTAINER : container-only attribute used on a non-container tag
  UNKNOWN_ATTR              : attribute name not in the legal set at all
  DEFAULT_BLOCK_ATTR        : same two checks, applied to the attribute string
                              inside <Default name="X" value="a=1 b=2"/>

The Default block matters more than a single control: UIDlgBuilder passes the
value string straight to CPaintManagerUI::AddDefaultAttributeList, and every
control of that class runs ApplyAttributeList(value) at creation
(UIDlgBuilder.cpp:529). One bad attribute there affects every control of that
class, and it still produces no error.

The legal set is generated from DuiLib/S3AttrRegistry.h (the s3::PropId enum,
320 entries) plus project-registered custom attributes. Being data-driven means
this script does not drift when the framework adds properties.

Exit code
---------
  0 = no findings
  1 = findings
  2 = setup problem (skin dir or prop table missing)

ASCII-only output on purpose: this project's consoles are CP936 and non-ASCII
has broken parsing here before.
"""

import argparse
import os
import re
import sys

# Container-only attributes: parsed in CContainerUI::SetAttribute.
CONTAINER_ATTRS = {
    'inset', 'childpadding', 'childalign', 'childvalign', 'childspacing',
    'mousechild', 'vscrollbar', 'hscrollbar', 'vscrollbarstyle',
    'hscrollbarstyle', 'scrollstepsize', 'fixedscrollbar', 'showscrollbar',
}

# Tags whose class derives from CContainerUI (verified against the headers).
# CContainerUI subclasses accept CONTAINER_ATTRS, so they are NOT findings.
CONTAINER_TAGS = {
    'VerticalLayout', 'HorizontalLayout', 'TabLayout', 'TileLayout',
    'ChildLayout', 'AnimationTabLayout',
    'RichEdit',   # CRichEditUI : public CContainerUI
    'Combo',      # CComboUI    : public CContainerUI
    'List',       # CListUI     : public CContainerUI
}

# Non-control elements: they have no SetAttribute of their own.
NON_CONTROL_TAGS = {'Window', 'Include', 'Font', 'Default', 'Image', 'Style'}

# Project-registered custom attributes (grep: lstrcmpiW(pstrName, L"...")).
CUSTOM_ATTRS = {'navicon', 'capkind'}


def load_props(path):
    if not os.path.isfile(path):
        return None
    with open(path, encoding='utf-8', errors='replace') as f:
        return {ln.strip() for ln in f if ln.strip()}


MAX_DEPTH = 6

# Directories that hold backups, not live skin. Auditing them is misleading:
# a finding there describes a file that is not loaded by the application, and
# "fixing" it changes nothing the user can see. Confirmed present:
#   .orig_backup/pages/*.xml  (18 files) -- the pre-edit copies kept by R28.
SKIP_DIRS = {'.orig_backup', '.git', 'backup', 'bak', '.svn'}


def iter_skin_files(root):
    """Every .xml under the skin root, recursively, cycle-safe.

    The old version listed root/*.xml plus root/pages/*.xml by name. That misses
    any second-level include directory a skin may grow later, and it silently
    under-reports instead of failing. Depth is capped so a self-referencing
    directory cannot hang the gate.
    """
    seen = set()
    out = []
    root = os.path.abspath(root)
    for dirpath, dirnames, filenames in os.walk(root):
        rel = os.path.relpath(dirpath, root)
        depth = 0 if rel == '.' else rel.count(os.sep) + 1
        if depth > MAX_DEPTH:
            dirnames[:] = []
            continue
        dirnames[:] = [d for d in dirnames if d.lower() not in SKIP_DIRS]
        real = os.path.realpath(dirpath)
        if real in seen:
            dirnames[:] = []
            continue
        seen.add(real)
        for fn in sorted(filenames):
            if fn.lower().endswith('.xml'):
                out.append(os.path.join(dirpath, fn))
    return sorted(out)


def parse_default_value(value_str, out, path, ctrl_name):
    """Audit the attribute string inside a <Default value="..."> element.

    The value is HTML-entity encoded (value=&quot;a=1 b=2&quot;), so after XML
    unescaping it is exactly the text handed to ApplyAttributeList: a
    whitespace-separated list of name=value pairs.
    """
    pairs = re.findall(r'([A-Za-z_][\w-]*)\s*=\s*"([^"]*)"', value_str)
    if not pairs:
        pairs = [(m.group(1), '') for m in re.finditer(r'([A-Za-z_][\w-]*)\s*=', value_str)]
    return [name for name, _ in pairs]


def audit_file(path, legal, out):
    txt = open(path, encoding='utf-8', errors='replace').read()
    # Strip comments so <Text inset=...> inside a comment is not a finding.
    txt = re.sub(r'<!--.*?-->', '', txt, flags=re.S)

    # <Default name="X" value="a=1 b=2"/> -- the value is applied to every
    # control of class X at creation time, so it needs the same audit as a
    # per-control attribute. Checked separately because the tag itself is a
    # config element, not a control.
    for dm in re.finditer(r'<Default(?=[\s/>])((?:\s+[A-Za-z_][\w-]*="[^"]*")*)\s*/?>', txt):
        dattrs = dm.group(1)
        nm = re.search(r'name="([^"]*)"', dattrs)
        cls = nm.group(1) if nm else '-'
        vm = re.search(r'value="([^"]*)"', dattrs)
        if not vm:
            continue
        # XML entities are already decoded by the regex-captured text only after
        # an explicit unescape; the parser here is regex-based, so do it by hand.
        val = (vm.group(1).replace('&quot;', '"').replace('&apos;', "'")
                          .replace('&lt;', '<').replace('&gt;', '>')
                          .replace('&amp;', '&'))
        for a in parse_default_value(val, out, path, cls):
            if a not in legal:
                out.append(('DEFAULT_BLOCK_ATTR', path, 'Default', cls, a,
                            'not in PropId table (applies to ALL <%s>)' % cls))
            elif a in CONTAINER_ATTRS and cls not in CONTAINER_TAGS:
                out.append(('DEFAULT_BLOCK_ATTR', path, 'Default', cls, a,
                            'only CContainerUI parses this (applies to ALL <%s>)' % cls))

    for m in re.finditer(r'<([A-Za-z][A-Za-z0-9_]*)((?:\s+[A-Za-z_][\w-]*="[^"]*")*)\s*/?>', txt):
        tag, attrs = m.group(1), m.group(2)
        if tag in NON_CONTROL_TAGS:
            continue

        names = re.findall(r'([A-Za-z_][\w-]*)="', attrs)
        nm = re.search(r'name="([^"]*)"', attrs)
        ctrl = nm.group(1) if nm else '-'

        if tag not in CONTAINER_TAGS:
            for a in names:
                if a in CONTAINER_ATTRS:
                    out.append(('CONTAINER_ON_NONCONTAINER', path, tag, ctrl, a,
                                'only CContainerUI parses this'))

        for a in names:
            if a not in legal:
                out.append(('UNKNOWN_ATTR', path, tag, ctrl, a, 'not in PropId table'))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--skin', action='append', required=True,
                    help='skin root directory (repeatable)')
    ap.add_argument('--props', required=True, help='_propids.txt generated from S3AttrRegistry.h')
    ap.add_argument('--max', type=int, default=40, help='max findings to print')
    args = ap.parse_args()

    props = load_props(args.props)
    if props is None:
        print('[FATAL] prop table not found: ' + args.props)
        return 2

    legal = props | CUSTOM_ATTRS | {'bkimage', 'sizebox', 'mininfo',
                                    'roundcorner', 'caption', 'maxinfo', 'inset',
                                    'padding', 'textpadding', 'pos', 'style'}

    findings = []
    scanned = 0
    for root in args.skin:
        if not os.path.isdir(root):
            print('[FATAL] skin dir not found: ' + root)
            return 2
        for f in iter_skin_files(root):
            audit_file(f, legal, findings)
            scanned += 1

    print('=== skin attribute audit ===')
    print('legal attrs: %d   files scanned: %d' % (len(legal), scanned))
    print('')

    if not findings:
        print('no findings')
        return 0

    for kind, path, tag, ctrl, attr, why in findings[:args.max]:
        print('%-26s %s  <%s name=%s>  %s="..."  (%s)'
              % (kind, os.path.basename(path), tag, ctrl, attr, why))
    if len(findings) > args.max:
        print('... and %d more' % (len(findings) - args.max))

    print('')
    print('FINDINGS: %d' % len(findings))
    return 1


if __name__ == '__main__':
    sys.exit(main())
