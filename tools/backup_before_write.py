"""Back up a file before rewriting it, so a bad edit is always recoverable.

Why this exists (round 87): rewriting abi_baseline.py, check_lib_dll_match.py and
check_control_factory_exports.py with an over-broad regex truncated all three to
their headers, deleting every function including main(). There is no version
control in this repository, and the bytecode cache had already been regenerated, so
the original implementations were simply gone. Two of the three had to be rewritten
from their docstrings, and one of them (abi_baseline's --surface classification)
could not be reproduced at all -- the historical count of 186 became an
unverifiable claim.

The cost of preventing that is a file copy. This module makes the safe path the
default path:

    from backup_before_write import back_up
    back_up('tools/abi_baseline.py')      # before touching it

Design decisions, each because of a failure mode above:

  * The copy keeps a timestamp and the ORIGINAL's sha256 in its name, so several
    backups of the same file never overwrite each other and an operator can tell
    which one is which without diffing.
  * Backups live under .box-agent-scratch/, which scan_scope.SKIP_DIRS already
    excludes -- a backup must never appear as repository content, or it would show
    up in file counts, encoding censuses and ABI scans.
  * back_up() is a no-op for a missing file (nothing to lose) and returns the path
    it wrote, so callers can print it.
  * Restoring is deliberately manual: an automatic restore would hide the fact that
    a bad edit happened.

Retention: keep the newest N per file, delete older ones. Without a cap this
directory grows forever, and a backup system that fills the disk gets disabled.
"""
import datetime
import hashlib
import pathlib
import shutil
import sys

BACKUP_ROOT = pathlib.Path('.box-agent-scratch/backups')
KEEP_PER_FILE = 8


def _digest(path):
    h = hashlib.sha256()
    with open(path, 'rb') as fh:
        for chunk in iter(lambda: fh.read(65536), b''):
            h.update(chunk)
    return h.hexdigest()[:12]


def back_up(path, note=''):
    """Copy `path` into the backup root. Returns the backup path, or None.

    `note` is written into the filename (sanitised) so a human can tell why the
    backup was taken without opening it.
    """
    src = pathlib.Path(path)
    if not src.is_file():
        return None
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    digest = _digest(src)
    safe_note = ''.join(c if (c.isalnum() or c in '-_') else '-' for c in note)[:40]
    parts = [src.name, stamp, digest]
    if safe_note:
        parts.append(safe_note)
    dest = BACKUP_ROOT / ('__'.join(parts) + '.bak')
    BACKUP_ROOT.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(src, dest)
    _prune(src.name)
    return dest


def _prune(basename):
    """Keep only the newest KEEP_PER_FILE backups for one file."""
    existing = sorted(BACKUP_ROOT.glob(basename + '__*.bak'))
    for old in existing[:-KEEP_PER_FILE]:
        try:
            old.unlink()
        except OSError:
            pass


def latest_for(path):
    """Return the newest backup for `path`, or None."""
    basename = pathlib.Path(path).name
    existing = sorted(BACKUP_ROOT.glob(basename + '__*.bak'))
    return existing[-1] if existing else None


def restore(path, which=None):
    """Copy a backup back over `path`. Returns the backup used, or None.

    Manual on purpose: see the module docstring.
    """
    target = pathlib.Path(path)
    src = pathlib.Path(which) if which else latest_for(target)
    if src is None or not src.is_file():
        return None
    shutil.copyfile(src, target)
    return src


def selftest():
    """Prove a backup can actually restore a truncated file.

    Why a behavioural test: the failure this module prevents (round 87) was a
    truncation that silently deleted every function. Asserting that back_up()
    returns a path would pass even if restore() were broken. This damages a real
    file, restores it, and compares hashes.
    """
    probe = pathlib.Path('.box-agent-scratch/backup_selftest.txt')
    probe.parent.mkdir(parents=True, exist_ok=True)
    payload = bytes(range(32, 32 + 96))
    probe.write_bytes(payload)
    made = back_up(probe, note='selftest')
    if made is None:
        print('[FAIL] selftest: back_up returned None for an existing file')
        return 1
    probe.write_bytes(payload[:10])          # the round-87 shape: truncated
    used = restore(probe)
    if used is None:
        print('[FAIL] selftest: restore found no backup')
        return 1
    if probe.read_bytes() != payload:
        print('[FAIL] selftest: restored content differs from the original')
        return 1
    print('[PASS] selftest: truncated file restored byte-for-byte')
    return 0


def main():
    if len(sys.argv) < 2:
        print('usage: backup_before_write.py <file> [<file> ...]')
        print('       backup_before_write.py --list <file>')
        print('       backup_before_write.py --restore <file> [<backup>]')
        return 2
    args = sys.argv[1:]
    if args[0] == '--selftest':
        return selftest()
    if args[0] == '--list':
        for f in args[1:]:
            b = latest_for(f)
            print('%s -> %s' % (f, b if b else '(no backup)'))
        return 0
    if args[0] == '--restore':
        which = args[2] if len(args) > 2 else None
        used = restore(args[1], which)
        if used is None:
            print('[FAIL] no backup found for %s' % args[1])
            return 1
        print('[PASS] restored %s from %s' % (args[1], used))
        return 0
    for f in args:
        dest = back_up(f, note='manual')
        print(('%s -> %s' % (f, dest)) if dest else ('%s: not found, skipped' % f))
    return 0


if __name__ == '__main__':
    sys.exit(main())
