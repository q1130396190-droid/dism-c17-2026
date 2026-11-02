"""Every CPaintManagerUI::Invalidate overload must carry DUI_ASSERT_UI_THREAD.

Why this exists (round 72): the thread-affinity guard already existed and the
no-argument Invalidate() already used it, so "the invalidate path is guarded"
looked true. It was true for one of the two overloads. Invalidate(RECT&) reached
the same m_rcLayeredUpdate members and the same InvalidateRect with nothing
watching. Half-guarded reads as fully guarded in every prose summary, which is
exactly why this needs to be mechanical.

Round 71 decided not to put an assertion on all 37 CRenderEngine draw entry
points (127 call sites, all on the paint hot path). The alternative recorded then
was "guard where invalidation is requested, one place instead of thirty-seven".
This checks that decision actually holds for every overload of that one place.

Exit codes:
    0  every overload guarded
    1  at least one overload is unguarded (message names the line)
    2  no overloads found -- a scan that finds nothing cannot pass
    3  --inject was requested and the check did not flag the synthetic gap
"""
import argparse
import pathlib
import re
import sys

SOURCE = pathlib.Path('DuiLib/Core/UIManager.cpp')
GUARD = 'DUI_ASSERT_UI_THREAD'

# Matches the definition line, e.g.  void CPaintManagerUI::Invalidate(RECT& rcItem)
SIG = re.compile(r'^\s*void\s+CPaintManagerUI::Invalidate\s*\(([^)]*)\)')


def find_bodies():
    """Return [(signature, first_line_no, body_text)] for each definition."""
    raw = SOURCE.read_bytes()
    # UIManager.cpp is GBK. Decoding as UTF-8 would raise and the scan would die,
    # which in a gate reads as "nothing found" if the caller is careless.
    try:
        text = raw.decode('utf-8-sig')
    except UnicodeDecodeError:
        text = raw.decode('gbk', errors='replace')
    lines = text.splitlines()
    out = []
    for i, ln in enumerate(lines):
        m = SIG.match(ln)
        if not m:
            continue
        # Body = following lines until a line whose brace depth returns to zero.
        #
        # The opening brace can be on the signature line itself (this file puts it
        # there) or on the next line. Starting the depth at zero and looping
        # "while depth > 0" therefore captured ONLY the signature for the
        # same-line style, and the first version of this scan reported both
        # overloads as unguarded -- including the one that has had the guard for
        # rounds. Start the brace hunt from the signature and let the loop advance
        # until the depth returns to zero after having gone positive.
        depth = ln.count('{') - ln.count('}')
        body = [ln]
        j = i
        opened = depth > 0
        while j + 1 < len(lines) and (not opened or depth > 0):
            j += 1
            body.append(lines[j])
            depth += lines[j].count('{') - lines[j].count('}')
            if depth > 0:
                opened = True
        out.append((m.group(0).strip(), i + 1, '\n'.join(body)))
    return out


def report(bodies, label=''):
    problems = []
    for sig, line, body in bodies:
        if GUARD not in body:
            problems.append((sig, line))
    if problems:
        print('[FAIL] %d Invalidate overload(s) reach the paint state unguarded%s:'
              % (len(problems), label))
        for sig, line in problems:
            print('       %-56s L%d' % (sig, line))
        print('       add DUI_ASSERT_UI_THREAD() at the top of each body')
        return 1
    print('[INFO] overloads_checked=%d' % len(bodies))
    print('[PASS] every Invalidate overload carries DUI_ASSERT_UI_THREAD')
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--inject', action='store_true',
                    help='self-test: prove an unguarded overload is reported')
    args = ap.parse_args()

    if not SOURCE.is_file():
        print('[FAIL] source not found: %s' % SOURCE)
        return 2
    bodies = find_bodies()
    if not bodies:
        print('[FAIL] no CPaintManagerUI::Invalidate definition found in %s' % SOURCE)
        print('       the scan read nothing, so it cannot pass')
        return 2

    if args.inject:
        # Strip the guard from a copy of the first body and require a report. Using
        # a mutated copy rather than editing the real file keeps this runnable on
        # any working tree, including one that is currently broken.
        sig, line, body = bodies[0]
        mutated = body.replace(GUARD, '/* injected-out */')
        rc = report([(sig, line, mutated)], ' (injected)')
        if rc == 1:
            print('[PASS] inject: an unguarded overload is reported')
            return 1
        print('[FAIL] inject: removing the guard was not detected')
        return 3

    return report(bodies)


if __name__ == '__main__':
    sys.exit(main())
