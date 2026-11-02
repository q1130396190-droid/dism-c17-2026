"""Add DUI_ASSERT_UI_THREAD to control setters that override without calling base.

Why: round 36 guarded CControlUI::SetText/SetPos/SetVisible/SetEnabled, assuming
subclasses would chain to the base implementation. They often do not -- 17
overrides reimplement the setter entirely, so the Debug assertion never runs for
those controls. A guard that only covers the base class is a guard that misses
most of the real calls.

Scope of the edit: insert the macro as the FIRST statement of each listed
overriding body. That is safe because
  * it compiles to nothing in Release;
  * it is skipped when thread affinity is still unclaimed (controls built before
    AttachDialog), so skin-time SetText/SetPos does not spuriously fire.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path('DuiLib')

# (file, class, setter) triples confirmed by the earlier scan to bypass the base.
TARGETS = [
    ('Control/UICombo.cpp',        'CComboUI',                 'SetEnabled'),
    ('Core/UIContainer.cpp',       'CContainerUI',             'SetEnabled'),
    ('Control/UIEdit.cpp',         'CEditUI',                  'SetText'),
    ('Control/UIGifAnimEx.cpp',    'CGifAnimExUI',             'SetVisible'),
    ('Control/UIHotKey.cpp',       'CHotKeyUI',                'SetText'),
    ('Control/UIList.cpp',         'CListContainerElementUI',  'SetPos'),
    ('Control/UIList.cpp',         'CListContainerElementUI',  'SetVisible'),
    ('Control/UIListEx.cpp',       'CListContainerHeaderItemUI', 'SetEnabled'),
    ('Control/UIList.cpp',         'CListHeaderItemUI',        'SetEnabled'),
    ('Control/UIList.cpp',         'CListTextElementUI',       'SetText'),
    ('Control/UIListEx.cpp',       'CListTextExtElementUI',    'SetText'),
    ('Control/UIList.cpp',         'CListUI',                  'SetPos'),
    ('Control/UIRichEdit.cpp',     'CRichEditUI',              'SetEnabled'),
    ('Control/UIRichEdit.cpp',     'CRichEditUI',              'SetText'),
    ('Control/UIRollText.cpp',     'CRollTextUI',              'SetPos'),
    ('Control/UIRollText.cpp',     'CRollTextUI',              'SetText'),
    ('Control/UIScrollBar.cpp',    'CScrollBarUI',             'SetVisible'),
]

MARK = 'DUI_ASSERT_UI_THREAD();'
done, skipped, failed = [], [], []

for rel, cls, name in TARGETS:
    p = ROOT / rel
    if not p.exists():
        failed.append((rel, 'missing file'))
        continue
    txt = p.read_text(encoding='utf-8', errors='replace')

    m = re.search(re.escape(f'void {cls}::{name}') + r'\s*\(', txt)
    if not m:
        failed.append((f'{cls}::{name}', 'signature not found'))
        continue

    open_brace = txt.find('{', m.end())
    if open_brace < 0:
        failed.append((f'{cls}::{name}', 'no body'))
        continue

    head = txt[open_brace: open_brace + 200]
    if MARK in head:
        skipped.append(f'{cls}::{name} (already guarded)')
        continue

    # Insert right after the opening brace, preserving line ending style.
    nl = '\r\n' if '\r\n' in txt[max(0, open_brace - 200): open_brace + 200] else '\n'
    insert = nl + '\t\t' + MARK
    txt = txt[: open_brace + 1] + insert + txt[open_brace + 1:]
    p.write_text(txt, encoding='utf-8')
    done.append(f'{cls}::{name}')

print('guarded %d, skipped %d, failed %d' % (len(done), len(skipped), len(failed)))
for d in done:
    print('  + ' + d)
for s in skipped:
    print('  = ' + s)
for f, why in failed:
    print('  ! %-40s %s' % (f, why))
sys.exit(1 if failed else 0)
