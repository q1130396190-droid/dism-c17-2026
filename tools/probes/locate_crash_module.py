"""Resolve which module a crash address belongs to.

The crash log reports an absolute address. Different runs show the same low 3
hex digits with a different base (7FFB74F534C1 vs 7FFB5EB434C1), which is the
signature of ASLR moving one and the same module. This tool maps the address
back to a module so the next step is source rather than guesswork.
"""

import ctypes
import os
import subprocess
import sys
import time
from ctypes import wintypes

TH32CS_SNAPMODULE = 0x8
TH32CS_SNAPMODULE32 = 0x10
INVALID_HANDLE = ctypes.c_void_p(-1).value


class MODULEENTRY32W(ctypes.Structure):
    _fields_ = [
        ("dwSize", wintypes.DWORD),
        ("th32ModuleID", wintypes.DWORD),
        ("th32ProcessID", wintypes.DWORD),
        ("GlblcntUsage", wintypes.DWORD),
        ("ProccntUsage", wintypes.DWORD),
        ("modBaseAddr", ctypes.c_void_p),
        ("modBaseSize", wintypes.DWORD),
        ("hModule", ctypes.c_void_p),
        ("szModule", wintypes.WCHAR * 256),
        ("szExePath", wintypes.WCHAR * 260),
    ]


k32 = ctypes.windll.kernel32


def modules(pid):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid)
    if snap in (0, INVALID_HANDLE):
        return []
    out = []
    try:
        e = MODULEENTRY32W()
        e.dwSize = ctypes.sizeof(MODULEENTRY32W)
        ok = k32.Module32FirstW(snap, ctypes.byref(e))
        while ok:
            base = e.modBaseAddr or 0
            out.append((e.szModule, base, e.modBaseSize, e.szExePath))
            ok = k32.Module32NextW(snap, ctypes.byref(e))
    finally:
        k32.CloseHandle(snap)
    return out


def launch_and_snapshot(bin_dir, exe, tries=60, step=0.02):
    """Start the exe and poll its module list until it dies.

    The crash happens ~100 ms in, so a single snapshot after the fact is empty.
    Polling keeps the union of everything that got mapped before the fault.
    """
    p = subprocess.Popen([os.path.join(bin_dir, exe)], cwd=bin_dir)
    seen = {}
    for _ in range(tries):
        if p.poll() is not None:
            break
        for name, base, size, path in modules(p.pid):
            seen.setdefault(name.lower(), (name, base, size, path))
        time.sleep(step)
    code = p.wait()
    return seen, code


def locate(seen, addr):
    for name, base, size, path in seen.values():
        if base <= addr < base + size:
            return name, base, addr - base, path
    return None


def main():
    bin_dir = r"D:\DuiLib_Ultimate-master\bin64"
    exe = "Win8Metro.exe"
    addr = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0x00007FFB5EB434C1

    seen, code = launch_and_snapshot(bin_dir, exe)
    print("exit code = 0x%08X (%d)" % (code & 0xFFFFFFFF, code))
    print("modules mapped before exit: %d" % len(seen))
    for key in ("win8metro.exe", "duilib.dll", "skimthemedll.dll", "skinthemedll.dll",
                "comctl32.dll", "user32.dll"):
        if key in seen:
            print("   %-22s base=0x%016X size=0x%X" % (seen[key][0], seen[key][1], seen[key][2]))

    hit = locate(seen, addr)
    if hit:
        print("CRASH ADDRESS 0x%X -> %s + 0x%X" % (addr, hit[0], hit[2]))
        print("   file: %s" % hit[3])
    else:
        print("CRASH ADDRESS 0x%X not inside any module mapped before exit" % addr)


if __name__ == "__main__":
    main()
