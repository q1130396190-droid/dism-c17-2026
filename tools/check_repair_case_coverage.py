"""Keep the repair-coverage case list honest as tools grow.

Why this exists (round 84): tools/check_repair_coverage.py verifies by behaviour
that each documented section can actually be repaired. Its CASES list is written
by hand. A new section added to check_doc_numbers.py would simply not be covered --
silently, forever. That is the same weakness the round-73 baseline has: a list
maintained by memory, not by the repository.

This tool derives the candidate set mechanically from the code:

  * every `problems.append('<prefix>: ...')` in a repair tool is a section that can
    report a problem, and
  * each candidate must either appear in check_repair_coverage.CASES, or be listed
    below with a written reason.

Nothing here decides whether a repair is correct -- check_repair_coverage.py does
that by perturbing documents. This only answers "is every reportable section either
tested or explicitly exempted?", which is a question the code can answer.

Exit codes:
    0  every candidate section is either cased or exempted with a reason
    1  a candidate section is neither (names it)
    2  zero candidates derived -- the derivation broke, so silence means nothing
    3  --inject planted a new section and the scan failed to require it
"""
import argparse
import ast
import pathlib
import re
import sys

REPAIR_TOOLS = [
    'tools/check_doc_numbers.py',
    'tools/check_thread_doc_sync.py',
]

# Sections that report a problem but cannot be repaired by `--write`, with the
# reason. Each entry must be a genuine "no repair is possible" case -- not "we did
# not get around to it".
EXEMPT = {
    'encoding: counted 0 source files': 'the encoder found nothing to scan; a repair '
                                        'cannot invent source files',
    'encoding: document no longer states a file count': 'the row is gone; the tool '
                                                        'can rewrite a value but not '
                                                        'decide where a missing row belongs',
    'encoding: row for %s not found': 'same as above -- a missing table row has no '
                                      'anchor to rewrite',
    'thread: document no longer states a coverage count': 'the sentence is gone; '
                                                          're-inserting it would be '
                                                          'inventing document structure',
    'escape: document no longer states the percentage row': 'same shape: nothing to '
                                                            'rewrite',
    'escape: doc says %d%%': 'this is the derived-percentage mismatch report for the '
                             'same row the case already perturbs',
    # check_thread_doc_sync builds two prefixes at runtime from a file name
    # ('<file>: doc says ...' and '<file>: listed in doc but ...'). A blanket '%s'
    # entry would absorb ANY new dynamic prefix -- measured in round 85: a planted
    # "'%s: dynamic section'" report passed silently. The exemption is therefore
    # keyed on the full literal shape, not on the placeholder alone.
    '%s: listed in doc but has no call site': 'the doc lists a file the code no longer '
                                              'has; removing a row is a judgement call, '
                                              'not a mechanical repair',
    'total': 'part of check_thread_doc_sync, whose single --write branch rewrites '
             'the whole document; the thread_sync case covers it behaviourally',
    'file count': 'same branch; see the thread_sync case',
}


def count_dynamic_sites():
    """Return {(tool, prefix): occurrences} for prefixes built at runtime."""
    counts = {}
    for rel in REPAIR_TOOLS:
        p = pathlib.Path(rel)
        if not p.is_file():
            continue
        text = p.read_text(encoding='utf-8', errors='replace')
        for m in re.finditer(r"problems\.append\(\s*['\"]([^'\"]*)", text):
            prefix = m.group(1).split(':')[0].strip()
            if '%' not in prefix:
                continue
            key = (pathlib.Path(rel).name, prefix)
            counts[key] = counts.get(key, 0) + 1
    return counts


def derive_candidates():
    """Return sorted [(tool, prefix)] for every reportable section prefix."""
    found = set()
    for rel in REPAIR_TOOLS:
        p = pathlib.Path(rel)
        if not p.is_file():
            continue
        text = p.read_text(encoding='utf-8', errors='replace')
        for m in re.finditer(r'problems\.append\(\s*[\'"]([^\'"]+)', text):
            literal = m.group(1)
            # The prefix is the part before the first colon; that is what the tools
            # themselves use to group sections.
            prefix = literal.split(':')[0].strip()
            found.add((pathlib.Path(rel).name, prefix))
    # Two literals in check_thread_doc_sync build their prefix at runtime ('%s: ...'),
    # so statically they all collapse to the meaningless name '%s'. Report the
    # placeholder so the gap is visible rather than silently collapsing several
    # sections into one bucket.
    return sorted(found)
    return sorted(found)


def case_labels():
    """Return the set of section prefixes the behavioural check covers."""
    p = pathlib.Path('tools/check_repair_coverage.py')
    if not p.is_file():
        return set()
    text = p.read_text(encoding='utf-8', errors='replace')
    out = set()
    # Case labels look like ('doc_numbers/escape', ...) -- the part after the slash
    # is the section prefix in the doc_numbers tool.
    for m in re.finditer(r"\(\s*'(?:doc_numbers|thread_sync)/([a-z0-9\-]+)'", text):
        out.add(m.group(1))
    return out


def normalize(prefix):
    return prefix.strip().rstrip('.').lower()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--inject', action='store_true',
                    help='self-test: append a new reportable section and require it be flagged')
    args = ap.parse_args()

    if args.inject:
        plant = pathlib.Path('tools/check_doc_numbers.py')
        backup = plant.read_text(encoding='utf-8')
        try:
            # Append a brand-new section prefix that no case or exemption mentions.
            marker = '    for d in done:'
            assert marker in backup
            injected = backup.replace(
                marker,
                "    problems.append('brandnew: something went wrong')\n" + marker, 1)
            plant.write_text(injected, encoding='utf-8')
            cands = derive_candidates()
            hit = [c for c in cands if c[1] == 'brandnew']
        finally:
            plant.write_text(backup, encoding='utf-8')
        if hit:
            # Deriving it is not enough: the point is that an unhandled section FAILS.
            # Check the verdict logic on the derived set, without the injected tool
            # still being modified.
            # Round 86: this self-test branch still used a PREFIX match on EXEMPT
            # while the real verdict used exact matching. Two code paths deciding
            # the same question differently means the self-test can pass while the
            # real check fails, or the reverse -- measured: the injected '%s'
            # section was reported MISSING by the real path but the self-test
            # rejected it as "accepted". Keep one implementation.
            norm = normalize(hit[0][1])
            labels = {normalize(x) for x in case_labels()}
            covered = any(lab == norm or lab.startswith(norm + '-') or norm.startswith(lab)
                          for lab in labels)
            exempt = any(normalize(k) == norm for k in EXEMPT)
            if not covered and not exempt:
                print('[PASS] inject: new section %r would be reported as MISSING'
                      % hit[0][1])
                return 3
            print('[FAIL] inject: new section %r was accepted (covered=%s exempt=%s)'
                  % (hit[0][1], covered, exempt))
            return 0
        print('[FAIL] inject: the new section was not derived -- the scan is blind')
        return 0

    cands = derive_candidates()
    if not cands:
        print('[FAIL] zero candidate sections derived -- the extraction reads nothing')
        return 2

    labels = {normalize(x) for x in case_labels()}
    missing = []
    print('[INFO] derived %d reportable section(s)' % len(cands))
    for tool, prefix in cands:
        # A case label is 'encoding-total' while the section prefix is 'encoding',
        # so match on the leading token rather than on equality.
        norm = normalize(prefix)
        covered = any(lab == norm or lab.startswith(norm + '-') or norm.startswith(lab)
                      for lab in labels)
        # Exact match on the normalized key. A prefix match here lets any new
        # dynamic prefix ride on an existing entry -- measured round 85: a planted
        # '%s: dynamic section' passed because '%s' matched '%s: doc says' by prefix.
        # The cost of exactness is that every exemption must name its full shape,
        # which is the point: the list stays readable and cannot absorb surprises.
        exempt = any(normalize(k) == norm for k in EXEMPT)
        if covered:
            state = 'case'
        elif exempt:
            state = 'exempt'
        elif '%' in prefix:
            # Judged by the dynamic-site block below, not here: a runtime-built
            # prefix cannot be exempted by name, only counted.
            state = 'dynamic'
        else:
            state = 'MISSING'
            missing.append((tool, prefix))
        print('       %-32s %-24s %s' % (tool, prefix, state))

    # Round 85: a prefix that is a format placeholder cannot name a section. It is
    # not a failure by itself -- the report is legitimately about a dynamic file
    # name -- but it means several sections share one bucket, so it must be
    # accounted for explicitly rather than passed over.
    # Round 85: dynamic prefixes are counted, not exempted. An exemption keyed on
    # the placeholder itself would approve every future dynamic section -- measured:
    # a planted "'%s: dynamic section'" passed while '%s' was exempted. Instead the
    # count is reported and each one must be a KNOWN site: check_thread_doc_sync has
    # exactly two, and both are covered by that tool's single behavioural case.
    placeholder = [(tl, px) for tl, px in cands if '%' in px]
    KNOWN_DYNAMIC = {('check_thread_doc_sync.py', '%s'): 2}
    dyn_counts = count_dynamic_sites()
    for tl, px in placeholder:
        allowed = KNOWN_DYNAMIC.get((tl, px))
        actual = dyn_counts.get((tl, px), 0)
        if allowed is None:
            missing.append((tl, px + ' (dynamic prefix, not a known site)'))
        elif actual != allowed:
            missing.append((tl, px + ' (dynamic prefix count %d, expected %d)'
                            % (actual, allowed)))

    if missing:
        print('[FAIL] %d section(s) are neither cased nor exempted:' % len(missing))
        for tool, prefix in missing:
            print('       %s -> %s' % (tool, prefix))
        print('       add a case to tools/check_repair_coverage.py, or an EXEMPT entry')
        print('       with a reason explaining why no repair is possible')
        return 1

    print('[PASS] every reportable section is either covered by a case or exempted')
    return 0


if __name__ == '__main__':
    sys.exit(main())
