"""Directories no repository scan should descend into, in one place.

Why this module exists (round 87): the same set was written out by hand in seven
tools, and the copies had already diverged:

    5 files: {'.box-agent-scratch', '.git', 'bin', 'lib', 'obj', 'temp'}
    2 files: {'.box-agent-scratch',        'bin', 'lib', 'obj', 'temp'}

The two without '.git' walked the git object store on every scan. That is a cost,
but the dangerous part is the divergence pattern itself: this project has repeatedly
fixed one copy and left the others (round 85 fixed a prefix match and missed its
duplicate inside --inject; round 87 found three hard-coded MSVC paths). A shared
constant removes the class, not just the instance.

Note on '.git': it is included because a scan has no business inside a git object
store, and because .git can contain large binary blobs that a text scanner would
report as garbage hits.
"""
import pathlib

# Lowercased directory names to skip. Lowercased because callers compare against
# `x.lower() for x in f.parts` -- keeping the case-insensitivity here means every
# caller gets it right by construction.
SKIP_DIRS = frozenset({
    'bin',
    'lib',
    'obj',
    'temp',
    '.git',
    '.box-agent-scratch',
})


def should_skip(path):
    """True when any path component is a directory that scans must skip."""
    p = path if isinstance(path, pathlib.PurePath) else pathlib.PurePath(str(path))
    return bool({x.lower() for x in p.parts} & SKIP_DIRS)


def iter_repo_files(root, suffixes):
    """Yield files under `root` with one of `suffixes`, skipping SKIP_DIRS.

    Callers in this repository all wanted the same three things: recurse, filter by
    suffix, skip the same directories. Providing it once removes the chance of a
    fourth copy with a fourth variation.
    """
    root = pathlib.Path(root)
    if not root.is_dir():
        return
    wanted = {s.lower() for s in suffixes}
    for f in root.rglob('*'):
        if not f.is_file():
            continue
        if f.suffix.lower() not in wanted:
            continue
        if should_skip(f):
            continue
        yield f


if __name__ == '__main__':
    print('SKIP_DIRS:', ' '.join(sorted(SKIP_DIRS)))
