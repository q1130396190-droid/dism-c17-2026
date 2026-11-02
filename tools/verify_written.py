"""Check a file immediately after writing it.

Why a dedicated entry point: writing regex escapes through a patch layer has produced
a literal 0x08 (backspace) ten times in this project. Every occurrence was caught only
by the full suite minutes later, and each time the symptom was "the check silently
stopped matching" -- indistinguishable from correct code. Ten is enough to stop
treating it as an accident.

This should be called right after every write to a .py/.ps1/.cpp/.h/.xml/.json:

    python tools/verify_written.py <file> [<file> ...]

Exit code is non-zero when the file contains a control byte, so a caller can chain it
with && and fail immediately instead of at the end of the round.

Design notes:
  * It reuses check_control_bytes.scan_file so there is exactly one definition of
    "forbidden byte" in the repo.
  * UTF-16 files are accepted (their NULs are the encoding), same as the suite check.
  * It also reports the file's byte length and whether it is empty, because a patch
    that truncates a file to zero bytes is another failure mode seen here and it does
    not involve any control byte at all.
"""
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from check_control_bytes import scan_file  # noqa: E402


def main(argv):
    if len(argv) < 2:
        print('usage: verify_written.py <file> [<file> ...]')
        return 2
    bad = 0
    for name in argv[1:]:
        p = pathlib.Path(name)
        if not p.is_file():
            print('[FAIL] %s: does not exist (a write that reported success but left '
                  'no file is a failure too)' % name)
            bad += 1
            continue
        size = p.stat().st_size
        if size == 0:
            print('[FAIL] %s: file is 0 bytes (truncated by a failed write?)' % name)
            bad += 1
            continue
        hits = scan_file(p)
        if hits:
            bad += 1
            print('[FAIL] %s: %d control byte(s)' % (name, len(hits)))
            for line, b, byte_name in hits[:10]:
                print('       L%-5d 0x%02X (%s)' % (line, b, byte_name))
        else:
            print('[PASS] %s: %d bytes, no control bytes' % (name, size))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
