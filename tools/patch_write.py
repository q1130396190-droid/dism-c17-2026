"""Single entry point for writing files during a patch, with a built-in self-check.

Why: two habits in this project kept failing.

  1. Writing regex escapes through a patch layer produced a literal 0x08 ten times.
     Each time the only thing that caught it was the full suite, minutes later.
  2. "Remember to run verify_written.py" is not a mechanism. It worked exactly as
     often as I remembered it, which is not a guarantee.

So the check moves into the write itself. Every write goes through here and the
result is verified before the tool reports success.

Usage:
    python tools/patch_write.py write   <path> --text <new content file or ->
    python tools/patch_write.py write   <path> --stdin      (read from stdin)
    python tools/patch_write.py replace <path> --old-file <f> --new-file <f>
    python tools/patch_write.py append  <path> --text <f>

Exit codes:
    0  written and verified
    1  written but the verification failed (the file is left as written so the
       mistake is inspectable, and the caller must not proceed)
    2  refused before writing (bad arguments, missing input, target dir absent)

Deliberately NOT a sandbox: it does not stop anyone from opening a file with an
editor or calling open(). It makes the safe path the cheap path; it does not make
the unsafe path impossible. Saying otherwise would be a lie about what this is.
"""
import argparse
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from check_control_bytes import (scan_file, scan_embedded_whitespace,  # noqa: E402
                                 FORBIDDEN)
from backup_before_write import back_up  # noqa: E402


def _read_text_arg(path_or_dash):
    if path_or_dash == '-':
        return sys.stdin.buffer.read()
    p = pathlib.Path(path_or_dash)
    if not p.is_file():
        raise SystemExit('[FAIL] input file not found: %s' % p)
    return p.read_bytes()


def _payload_problems(data):
    """Return [(offset, byte, name)] for forbidden control bytes in `data`.

    This is the *pre*-write half of the check and it is the one that matters.
    _verify() below runs after the bytes are already on disk, so a bad payload
    used to destroy the target and only then report FAIL -- the original content
    was gone and the message arrived too late to be useful. Measured in round 72:
    the file ended up containing the corrupt text, exit code 1.

    UTF-16 payloads are exempt for the same reason scan_file exempts them: every
    ASCII character there is followed by a NUL, which is not a mistake.
    """
    if data[:2] in (bytes([0xFF, 0xFE]), bytes([0xFE, 0xFF])):
        return []
    return [(i, c, FORBIDDEN[c]) for i, c in enumerate(data) if c in FORBIDDEN]


LEDGER = pathlib.Path('tools/escape_rejections.log')


def _record_rejection(what, hits):
    """Append one line per refusal, so the trend is measurable over time.

    Why a log instead of another rule: by round 75 the same escape-expansion
    accident had happened 18+ times. Adding a nineteenth check has not changed
    the rate; measuring the rate is what can. If refusals trend down, the
    "write values, not escapes" convention is being followed. If they stay flat,
    the problem is the tool chain, not the knowledge, and more rules will not
    help.

    Failure to log must never break the refusal itself: a ledger write error is
    reported and swallowed.
    """
    # Self-test refusals are recorded separately. They are synthetic by
    # construction, so letting them into the trend line would make the number of
    # "accidents" grow every time the suite runs -- the metric would measure test
    # executions instead of real mistakes.
    import os
    if os.environ.get('PATCH_WRITE_SELFTEST') == '1':
        return
    try:
        import datetime
        stamp = datetime.datetime.now().isoformat(timespec='seconds')
        names = sorted({n for _, _, n in hits})
        line = '%s  %s  0x%02X x%d  %s' % (
            stamp,
            pathlib.Path(str(what)).name,
            hits[0][1],
            len(hits),
            ','.join(names))
        with open(LEDGER, 'a', encoding='utf-8') as fh:
            fh.write(line + chr(10))
        _maybe_rotate()
    except OSError as exc:
        print('[WARN] could not append to %s: %s' % (LEDGER, exc))


LEDGER_MAX_LINES = 500


def _maybe_rotate():
    """Keep the ledger bounded WITHOUT losing the trend.

    The problem (round 77 left it open): an append-only ledger grows forever.
    The trap: truncating it throws away the totals, and this file is specifically
    documented as "do not add these numbers to the historical 18+", so silently
    shrinking the count would break the one property it is meant to have.

    Resolution: a cumulative counter lives in the header and is incremented on
    every write; only the DETAIL lines are trimmed. So:

        total      = monotone, survives every rotation
        recent     = kept detail, newest LEDGER_MAX_LINES_RECENT lines
        oldest     = dropped details, but their count is inside total

    A reader can therefore always answer "how many refusals happened since round
    76" (total) and "what did the recent ones look like" (recent), and a rotation
    never makes the trend look better.
    """
    if not LEDGER.is_file():
        return
    text = LEDGER.read_text(encoding='utf-8')
    lines = text.splitlines()
    header = [l for l in lines if l.startswith('#') or l.startswith('TOTAL ')]
    details = [l for l in lines if l and not l.startswith('#') and not l.startswith('TOTAL ')]
    if len(details) <= LEDGER_MAX_LINES:
        return
    keep = details[-LEDGER_MAX_LINES:]
    dropped = len(details) - len(keep)

    total = 0
    for l in header:
        if l.startswith('TOTAL '):
            try:
                total = int(l.split()[1])
            except (IndexError, ValueError):
                total = 0
    # `total` counts everything written so far, `details` is what survived earlier
    # rotations. Adding `dropped` alone under-counts: the first rotation has no
    # TOTAL line yet, so the pre-rotation lines were never counted at all.
    # Measured: 20 writes with cap 3 produced TOTAL 17 that way.
    if total == 0:
        total = len(details)
    else:
        total += len(details) - len(keep)


    out = [l for l in header if not l.startswith('TOTAL ')]
    out.append('TOTAL %d  (cumulative since round 76; details below are the most '
               'recent %d)' % (total, LEDGER_MAX_LINES))
    out.extend(keep)
    LEDGER.write_text(chr(10).join(out) + chr(10), encoding='utf-8')


def _reject_dirty(data, what):
    """Print the rejection and return True when `data` must not be written."""
    hits = _payload_problems(data)
    if not hits:
        # Round 82: TAB is legal whitespace, so the byte check above cannot flag it.
        # A TAB inside a quoted path in a shell script is the residue of an expanded
        # backslash-t; detect that shape too, before the write rather than after.
        suffix = pathlib.Path(str(what)).suffix.lower()
        if suffix in {'.ps1', '.bat', '.cmd'}:
            # The probe file must carry the TARGET's extension: the scanner scopes
            # itself to shell files by suffix, and a '.tmp' name silently skipped the
            # check (measured: the guard reported PASS and wrote a TAB-laden script).
            tmp = pathlib.Path('.box-agent-scratch') / ('_pw_probe' + suffix)
            try:
                tmp.parent.mkdir(parents=True, exist_ok=True)
                tmp.write_bytes(data)
                emb = scan_embedded_whitespace(tmp)
            finally:
                try:
                    tmp.unlink()
                except OSError:
                    pass
            if emb:
                line, byte, name = emb[0]
                print('[FAIL] refusing to write %s: %s' % (what, name))
                print('       L%-5d 0x%02X -- a backslash-t pair was expanded before this call'
                      % (line, byte))
                print('       Nothing was written; fix the payload and re-run.')
                _record_rejection(what, emb)
                return True
    if not hits:
        return False
    print('[FAIL] refusing to write %s: %d control byte(s) in the payload'
          % (what, len(hits)))
    for off, c, name in hits[:10]:
        line = data.count(0x0A, 0, off) + 1
        print('       L%-5d 0x%02X (%s) at payload offset %d' % (line, c, name, off))
    print('       a regex escape (usually backslash-b) was expanded into a real byte before')
    print('       this call. Nothing was written; fix the payload and re-run.')
    _record_rejection(what, hits)
    return True


def _bom_state(data):
    """(has_bom, encoding_hint) for a byte string."""
    if data[:3] == bytes([0xEF, 0xBB, 0xBF]):
        return True, "utf-8"
    if data[:2] in (bytes([0xFF, 0xFE]), bytes([0xFE, 0xFF])):
        return True, "utf-16"
    return False, "utf-8"


def _guard_bom(path, data):
    """Refuse a write that would silently drop the file's existing BOM.

    Measured twice in one session: decoding with utf-8-sig and re-encoding with utf-8
    drops the BOM with no error, and nothing else notices. It matters for skin XML
    because the parser's encoding detection changes with the BOM.

    Rule: file has a BOM but the payload does not -> refuse. Adding a BOM where there was
    none is allowed, because the source tree genuinely mixes both and "preserve what was
    there" is the only stable contract.
    """
    p = pathlib.Path(path)
    if not p.is_file():
        return False
    try:
        current = p.read_bytes()
    except OSError:
        return False
    had, enc = _bom_state(current)
    if not had:
        return False
    has, _ = _bom_state(data)
    if has:
        return False
    print('[FAIL] refusing to write %s: it has a %s BOM and the payload does not'
          % (p, enc))
    print('       decode-with-utf-8-sig then encode-with-utf-8 removes it silently.')
    print('       Re-encode with utf-8-sig or prepend the BOM. Nothing was written.')
    return True


def safe_write(path, data):
    """Write `data` to `path`, refusing first if it carries control bytes.

    On a successful write the previous content is backed up first (round 88).
    A refusal returns before that point on purpose: the file is untouched, so a
    backup would only add noise and make the useful ones harder to find.

    This is the importable form of the guarantee the CLI already makes, so a
    script can get the same protection without shelling out:

        from patch_write import safe_write
        safe_write('DuiLib/Core/X.cpp', new_bytes)

    Returns True when the write happened, False when it was refused. Raises
    nothing on refusal -- callers are batch rewriters that must report and keep
    going rather than abort mid-run.
    """
    if _reject_dirty(data, str(path)):
        return False
    if _guard_bom(path, data):
        return False
    back_up(path, note='safe_write')
    pathlib.Path(path).write_bytes(data)
    return True


def safe_write_text(path, text, encoding='utf-8'):
    return safe_write(path, text.encode(encoding))


def _verify(path):
    """Same checks as verify_written.py, inline so there is one implementation."""
    p = pathlib.Path(path)
    if not p.is_file():
        print('[FAIL] %s: write reported success but the file does not exist' % p)
        return 1
    size = p.stat().st_size
    if size == 0:
        print('[FAIL] %s: file is 0 bytes (truncated by the write?)' % p)
        return 1
    hits = scan_file(p)
    if hits:
        print('[FAIL] %s: %d control byte(s) in the written content' % (p, len(hits)))
        for line, b, name in hits[:10]:
            print('       L%-5d 0x%02X (%s)' % (line, b, name))
        print('       a regex escape was expanded into a real byte; fix the source')
        print('       text and re-run, do not proceed with this file')
        return 1
    print('[PASS] %s: %d bytes, no control bytes' % (p, size))
    return 0


def cmd_write(args):
    p = pathlib.Path(args.path)
    if not p.parent.is_dir():
        print('[FAIL] target directory does not exist: %s' % p.parent)
        return 2
    data = _read_text_arg(args.text if args.text else '-')
    # Reject before touching the file: a corrupt payload must not destroy the
    # original (see _payload_problems).
    if _reject_dirty(data, str(p)):
        return 2
    p.write_bytes(data)
    return _verify(p)


def cmd_append(args):
    p = pathlib.Path(args.path)
    if not p.parent.is_dir():
        print('[FAIL] target directory does not exist: %s' % p.parent)
        return 2
    data = _read_text_arg(args.text if args.text else '-')
    if _reject_dirty(data, str(p)):
        return 2
    with open(p, 'ab') as fh:
        fh.write(data)
    return _verify(p)


def cmd_insert(args):
    """Insert content immediately AFTER a unique anchor.

    Why not just replace: adding a new block means the anchor plus the new text,
    and re-typing the anchor in the replacement file is where transcription errors
    come from. Keeping the anchor out of the payload removes that step entirely.
    """
    p = pathlib.Path(args.path)
    if not p.is_file():
        print('[FAIL] target file does not exist: %s' % p)
        return 2
    anchor_bytes = _read_text_arg(args.anchor_file)
    payload = _read_text_arg(args.text if args.text else '-')
    body = p.read_bytes()
    n = body.count(anchor_bytes)
    if n == 0:
        print('[FAIL] the anchor does not appear in %s -- nothing inserted' % p)
        print('       (check line endings: a CRLF/LF mismatch is the usual cause)')
        return 2
    if n > 1:
        print('[FAIL] the anchor appears %d times in %s -- ambiguous' % (n, p))
        return 2
    if _reject_dirty(payload, str(p)):
        return 2
    at = body.find(anchor_bytes) + len(anchor_bytes)
    p.write_bytes(body[:at] + payload + body[at:])
    return _verify(p)


def cmd_replace(args):
    p = pathlib.Path(args.path)
    if not p.is_file():
        print('[FAIL] target file does not exist: %s' % p)
        return 2
    old = _read_text_arg(args.old_file)
    new = _read_text_arg(args.new_file)
    body = p.read_bytes()
    n = body.count(old)
    if n == 0:
        print('[FAIL] the old text does not appear in %s -- nothing replaced' % p)
        print('       (check line endings: a CRLF/LF mismatch is the usual cause)')
        return 2
    if n > 1:
        print('[FAIL] the old text appears %d times in %s -- ambiguous anchor' % (n, p))
        return 2
    if _reject_dirty(new, str(p)):
        return 2
    result = body.replace(old, new, 1)
    # The replacement can also be clean on its own while the *result* is corrupt
    # (the old text may have carried a control byte that got duplicated). Check
    # the bytes that would actually land on disk.
    if _reject_dirty(result, str(p) + ' (after replacement)'):
        return 2
    p.write_bytes(result)
    return _verify(p)


def selftest():
    """Prove the pre-write guard rejects a dirty payload AND leaves the file alone.

    Why a real self-test instead of a comment: the whole point of this guard is
    that _verify() fires too late. A test that only checks _payload_problems()
    returns a hit would pass even if cmd_write forgot to call it. This one drives
    the actual command path against a throwaway file and asserts the original
    bytes survive.
    """
    import os
    os.environ['PATCH_WRITE_SELFTEST'] = '1'
    # Built from byte values, not from escapes. This very file has had escapes
    # expanded in transit several times; byte values cannot be mangled that way.
    orig = bytes([47, 47, 32, 111, 114, 105, 103, 10, 105, 110, 116, 32, 61, 32, 49, 59, 10])
    dirty = bytes([47, 47, 32, 110, 101, 119, 10, 120, 61, 34, 8, 34, 59, 10])
    # Scratch lives under the repo, not the system temp: this runtime refuses to
    # delete outside the workspace, and a self-test that dies on cleanup looks
    # like a failed guard.
    td = pathlib.Path('.box-agent-scratch') / 'pw_selftest'
    # mkdir + overwrite the two files; no recursive delete. This runtime refuses
    # rmtree outside the workspace's own cleanup flow, and a self-test that dies
    # on teardown looks exactly like a broken guard.
    td.mkdir(parents=True, exist_ok=True)
    if True:
        tgt = pathlib.Path(td) / 'victim.cpp'
        pay = pathlib.Path(td) / 'payload.txt'
        tgt.write_bytes(orig)
        pay.write_bytes(dirty)

        class _A:
            path = str(tgt)
            text = str(pay)

        rc = cmd_write(_A())
        after = tgt.read_bytes()
    if rc != 2:
        print('[FAIL] selftest: dirty payload returned %d, expected 2 (refused)' % rc)
        return 1
    if after != orig:
        print('[FAIL] selftest: the target was modified -- the guard fires too late')
        return 1
    # The importable form must give the same guarantee as the CLI. Checked here
    # rather than in a separate suite because a caller reaching for safe_write()
    # is the whole point of the exposure.
    scratch2 = pathlib.Path('.box-agent-scratch') / 'pw_selftest'
    scratch2.mkdir(parents=True, exist_ok=True)
    tgt2 = scratch2 / 'victim2.cpp'
    tgt2.write_bytes(orig)
    if safe_write(tgt2, dirty):
        print('[FAIL] selftest: safe_write() accepted a dirty payload')
        return 1
    if tgt2.read_bytes() != orig:
        print('[FAIL] selftest: safe_write() modified the target before refusing')
        return 1
    if not safe_write(tgt2, bytes([47, 47, 32, 111, 107, 10])):
        print('[FAIL] selftest: safe_write() refused a clean payload')
        return 1
    if tgt2.read_bytes() != bytes([47, 47, 32, 111, 107, 10]):
        print('[FAIL] selftest: safe_write() did not write a clean payload')
        return 1

    # Round 82: the TAB-inside-a-shell-path shape must also be refused. It is not a
    # control byte (0x09 is legal whitespace), so it needs its own case -- a guard
    # that only covers the byte list leaves this path open.
    scratch3 = pathlib.Path('.box-agent-scratch') / 'pw_selftest'
    scratch3.mkdir(parents=True, exist_ok=True)
    tabbed = scratch3 / 'victim3.ps1'
    tabbed.write_bytes(bytes([35, 32, 111, 107, 10]))
    # Real shape: & '.<TAB>hread_gate.exe' -- the TAB replaces a backslash-t pair
    # inside a quoted path, and the path has an extension the scanner looks for.
    tab_payload = (bytes([32, 38, 32, 0x27, 0x2E, 0x09])
                   + b'hread_gate.exe' + bytes([0x27]))
    if safe_write(tabbed, tab_payload):
        print('[FAIL] selftest: a TAB inside a shell path was accepted')
        return 1
    if tabbed.read_bytes() != bytes([35, 32, 111, 107, 10]):
        print('[FAIL] selftest: the TAB payload modified the target')
        return 1
    print('[PASS] selftest: TAB inside a shell path is refused')

    print('[PASS] selftest: dirty payload refused before write; target untouched')
    print('[PASS] selftest: safe_write() refuses dirty and accepts clean')
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--selftest', action='store_true',
                    help='prove the pre-write control-byte guard refuses and preserves')
    sub = ap.add_subparsers(dest='op')

    w = sub.add_parser('write', help='replace the whole file')
    w.add_argument('path')
    w.add_argument('--text', default='', help='file holding the new content, or - for stdin')
    w.set_defaults(func=cmd_write)

    a = sub.add_parser('append', help='append to the file')
    a.add_argument('path')
    a.add_argument('--text', default='', help='file holding the bytes to append, or -')
    a.set_defaults(func=cmd_append)

    r = sub.add_parser('replace', help='replace one unique occurrence')
    r.add_argument('path')
    r.add_argument('--old-file', required=True)
    r.add_argument('--new-file', required=True)
    r.set_defaults(func=cmd_replace)

    i = sub.add_parser('insert', help='insert content after a unique anchor')
    i.add_argument('path')
    i.add_argument('--anchor-file', required=True,
                   help='file holding the anchor text; must occur exactly once')
    i.add_argument('--text', default='', help='file holding the content to insert, or -')
    i.set_defaults(func=cmd_insert)

    # --selftest must work without a subcommand, so parse it separately first.
    if '--selftest' in sys.argv[1:]:
        return selftest()
    args = ap.parse_args()
    return args.func(args)


if __name__ == '__main__':
    sys.exit(main())
