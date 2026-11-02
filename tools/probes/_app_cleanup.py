"""Shared pre-launch cleanup for probes that start Win8Metro.

Why this exists
---------------
Win8Metro is single-instance: it takes a named mutex on startup. A leftover
process keeps that mutex, so the next launch silently does nothing -- or keeps
``bin64/amd64/*.dll`` mapped, and the following MSBuild fails with LNK1104.

``check_pages_theme.py`` and ``check_theme_switch.py`` each grew their own copy
of the kill-and-verify loop. That is exactly how the batch failure was born: the
driver cleaned only ``Win8Metro.exe``, the two probes cleaned ``*Win8Metro*``, and
a probe copy left behind by a third script blocked both. One implementation, used
by every launcher, makes "did this probe clean up?" a question with one answer.

Matching is on ``*Win8Metro*``, not on the shipping name. Probes copy the exe to
names such as ``Win8Metro_svcgate.exe`` / ``Win8Metro_keyscan.exe`` and launch
those; a cleanup that only knows the main name leaves the copy holding the mutex.
"""

from __future__ import annotations

import subprocess
import time
from typing import Optional

WORKSPACE = r"D:\DuiLib_Ultimate-master"
BIN_DIR = WORKSPACE + r"\bin64"
APP_EXE = "Win8Metro.exe"

# The shipping exe is an input, and Win8Metro_asi.exe is the asInvoker build the
# a11y gate launches. Everything else matching Win8Metro_*.exe is a probe copy.
KEEP_EXES = ("Win8Metro.exe", "Win8Metro_asi.exe")

_PS = ["powershell", "-NoProfile", "-Command"]


def _count() -> Optional[int]:
    r = subprocess.run(
        _PS + ["(Get-Process | Where-Object { $_.ProcessName -like '*Win8Metro*' } "
               "| Measure-Object).Count"],
        capture_output=True)
    try:
        return int(r.stdout.decode("utf-8", "replace").strip() or "0")
    except ValueError:
        return None


def kill_app(timeout_s: float = 10.0) -> bool:
    """Stop every *Win8Metro* process and wait for the mutex to be released.

    Returns True only when the process count actually reached zero. A caller that
    launches immediately after a fire-and-forget kill races the handle teardown and
    gets a silent no-op, so the wait is part of the contract rather than an
    optional convenience.
    """
    subprocess.run(
        _PS + ["Get-Process | Where-Object { $_.ProcessName -like '*Win8Metro*' } "
               "| ForEach-Object { Stop-Process -Id $_.Id -Force "
               "-ErrorAction SilentlyContinue }"],
        capture_output=True)
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if _count() == 0:
            return True
        time.sleep(0.5)
    return _count() == 0


def remove_probe_copies(bin_dir: str = BIN_DIR) -> list:
    """Delete leftover ``Win8Metro_*.exe`` copies; keep the shipping and asInvoker exes.

    Killing alone is not enough: a copy left on disk is resurrected by the next
    probe that launches it by name. Deleting it makes the stale-process problem
    unrepeatable instead of merely fixing the current run.
    """
    removed = []
    try:
        import os
        for name in os.listdir(bin_dir):
            if not name.lower().startswith("win8metro_") or not name.lower().endswith(".exe"):
                continue
            if name in KEEP_EXES:
                continue
            try:
                os.remove(os.path.join(bin_dir, name))
                removed.append(name)
            except OSError:
                pass
    except OSError:
        pass
    return removed


def prepare_launch(bin_dir: str = BIN_DIR, quiet: bool = False) -> bool:
    """Clean processes and probe copies. Returns False if the mutex is still held."""
    ok = kill_app()
    if not ok and not quiet:
        print("[CLEAN] a *Win8Metro* process survived the kill; launch will probably no-op")
    gone = remove_probe_copies(bin_dir)
    if gone and not quiet:
        print("[CLEAN] removed probe copies: " + ", ".join(gone))
    return ok
