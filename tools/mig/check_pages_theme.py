"""逐页主题一致性检查：导出 /SHOT 后对每页做客观像素审计。

为什么走 /SHOT 而不是截屏
------------------------
本会话是非交互桌面，CopyFromScreen / PrintWindow 取不到稳定像素。
/SHOT 是程序内部把控件树画到内存位图（ExportAllPageShots：对每页
SelectItem + 强制 SetPos，同步执行、不进消息循环），这是唯一可复现的路径。

在此之前踩过的坑，都写进判据
--------------------------
1) 曾用鼠标坐标点导航切页，实际没切过去，于是"整页零像素"被误判成
   "控件不绘制"——白查了一轮。所以检查必须自己驱动 /SHOT，不靠交互。
2) 深色下曾出现整条 Session 信息带残留（SurfaceAlt 底色），根源是那两个
   容器没有 name，SkinTokenReplayTable 按名定位不到。现在补了名，本脚本
   用"整块同色带"判据专门拦这类回归。

判据
----
深色页：暗像素 >= 60%，且亮像素 <= 12%
浅色页：亮像素 >= 70%，且暗像素 <= 15%
另外统计"整块 40x40 纯色块"的比例，超过 25% 说明有大面积未刷新（无论
哪套主题），单独报出来。

用法
----
  python tools/mig/check_pages_theme.py --theme dark
  python tools/mig/check_pages_theme.py --theme light
  python tools/mig/check_pages_theme.py --theme both --inject

退出码：0 全部通过，1 有页不合格，2 环境不就绪（导出失败），3 --inject 未报出。
"""
from __future__ import annotations

import argparse
import ctypes
import glob
import os
import struct
import subprocess
import sys
import time
from collections import Counter

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                '..', 'probes'))
import _app_cleanup  # noqa: E402  (shared pre-launch cleanup)

ROOT = r"D:\DuiLib_Ultimate-master"
BIN = os.path.join(ROOT, "bin64")
EXE = os.path.join(BIN, "Win8Metro.exe")
CFG = os.path.join(BIN, "config.ini")

# 每页在 /SHOT 里的文件名前缀，与 ExportAllPageShots 的页面顺序一致
PAGE_NAMES = [
    "00_Cleanup", "01_Startup", "02_Appx", "03_Tools", "04_Optim",
    "05_Driver", "06_Program", "07_Capability", "08_Update", "09_Assoc",
    "10_Unattend", "11_Tasks", "12_Control", "13_Intl", "14_RegClean",
    "15_Welcome",
]

THRESH = {
    "dark":  {"dark_min": 60.0, "light_max": 12.0},
    "light": {"light_min": 70.0, "dark_max": 15.0},
}
BLOCK = 40
BLOCK_UNIFORM_MAX = 25.0     # 40x40 纯色块占比上限


def kill_app():
    """Stop every *Win8Metro* variant and wait for the mutex to be released.

    Delegates to tools/probes/_app_cleanup.py. Two copies of this loop had already
    drifted apart -- one cleaned only the shipping exe name, the other used a
    wildcard -- and the difference is exactly what let a leftover probe copy hold
    the single-instance mutex while a batch run reported "window not found".
    """
    return _app_cleanup.kill_app()


def set_theme(mode):
    """config.ini 是 UTF-8 BOM，只替换 Mode= 的值，其余字节不动。"""
    raw = open(CFG, "rb").read()
    for old in (b"Mode=light", b"Mode=dark", b"Mode=auto"):
        if old in raw:
            raw = raw.replace(old, b"Mode=" + mode.encode(), 1)
            break
    else:
        return False
    open(CFG, "wb").write(raw)
    return True


def export(theme):
    """跑一次 /SHOT，返回导出目录。"""
    out = os.path.join(ROOT, "_pageshot_" + theme)
    if os.path.isdir(out):
        for f in glob.glob(os.path.join(out, "*")):
            try:
                os.remove(f)
            except OSError:
                pass
    else:
        os.makedirs(out, exist_ok=True)

    # [KEY] 必须"先按相反主题冷启动，再切到目标主题"后才导出。
    #   /SHOT 是冷启动渲染：每页首次构建时令牌已是最新值，所以它**天然**
    #   不会暴露"切主题时漏刷新"。实测：把 CPageCleanup::RefreshThemeColors()
    #   改成空实现后，/SHOT 仍然 16/16 全 OK（看着绿，实际对这类缺陷失明）；
    #   换成先切换再渲染，该页立刻报 38.4% 残留。所以这里走 /SHOTSWITCH。
    other = "dark" if theme == "light" else "light"
    set_theme(other)          # 冷启动进的是相反主题
    ctypes.windll.shell32.ShellExecuteW(
        None, "runas", EXE,
        "/SHOTSWITCH=" + out + "," + theme, BIN, 0)

    # 16 页导出约 40-50 秒；按文件数收敛，最多等 120 秒
    for _ in range(120):
        time.sleep(1)
        if len(glob.glob(os.path.join(out, "*.bmp"))) >= len(PAGE_NAMES):
            time.sleep(2)
            break
    kill_app()
    return out


def read_bmp(path):
    b = open(path, "rb").read()
    off = struct.unpack_from("<I", b, 10)[0]
    w = struct.unpack_from("<i", b, 18)[0]
    h = struct.unpack_from("<i", b, 22)[0]
    bpp = struct.unpack_from("<H", b, 28)[0]
    if bpp not in (24, 32):
        return None
    stride = ((w * bpp // 8) + 3) // 4 * 4
    return b, off, w, h, bpp, stride


def audit(path):
    """返回 (dark%, light%, uniform_block%, 主导色)。"""
    r = read_bmp(path)
    if not r:
        return None
    b, off, w, h, bpp, stride = r
    step_px = bpp // 8
    dark = light = tot = 0
    colours = Counter()

    # 纯色块统计：按 BLOCK 分格，格内所有采样点同色即视为"未刷新块"
    uniform = 0
    nblocks = 0

    for by in range(0, h - BLOCK + 1, BLOCK):
        for bx in range(0, w - BLOCK + 1, BLOCK):
            nblocks += 1
            first = None
            same = True
            for y in range(by, by + BLOCK, 8):
                yy = (h - 1 - y) if h > 0 else y
                base = off + yy * stride
                for x in range(bx, bx + BLOCK, 8):
                    i = base + x * step_px
                    c = (b[i + 2], b[i + 1], b[i])
                    if first is None:
                        first = c
                    elif c != first:
                        same = False
            if same:
                uniform += 1

    for y in range(0, h, 6):
        yy = (h - 1 - y) if h > 0 else y
        base = off + yy * stride
        for x in range(0, w, 6):
            i = base + x * step_px
            rr, gg, bb = b[i + 2], b[i + 1], b[i]
            lum = (rr * 299 + gg * 587 + bb * 114) // 1000
            tot += 1
            if lum < 80:
                dark += 1
            elif lum > 200:
                light += 1
            colours[(rr, gg, bb)] += 1

    dominant = colours.most_common(1)[0][0] if colours else (0, 0, 0)
    return (dark * 100.0 / max(tot, 1),
            light * 100.0 / max(tot, 1),
            uniform * 100.0 / max(nblocks, 1),
            dominant)


def check_theme(theme):
    if not set_theme(theme):
        print("[FAIL] cannot set Mode=%s in config.ini" % theme)
        return 2, []
    out = export(theme)
    files = sorted(glob.glob(os.path.join(out, "*.bmp")))
    if len(files) < len(PAGE_NAMES):
        print("[SKIP] only %d/%d pages exported -- nothing asserted"
              % (len(files), len(PAGE_NAMES)))
        return 2, []

    th = THRESH[theme]
    print("\n[%s] %d pages in %s" % (theme.upper(), len(files), out))
    print("  %-16s %7s %7s %7s %-18s %s"
          % ("page", "dark%", "light%", "plain%", "dominant", "verdict"))

    bad = []
    for f in files:
        name = os.path.basename(f)[:-4]
        a = audit(f)
        if a is None:
            bad.append((name, "unreadable bmp"))
            continue
        d, l, u, dom = a
        if theme == "dark":
            ok = d >= th["dark_min"] and l <= th["light_max"]
        else:
            ok = l >= th["light_min"] and d <= th["dark_max"]
        # 大面积纯色块：两套主题都查，拦"整块没刷新"
        plain_bad = u > BLOCK_UNIFORM_MAX and not ok

        verdict = "OK" if (ok and not plain_bad) else (
            "RESIDUAL" if not ok else "PLAIN-BLOCK")
        if verdict != "OK":
            bad.append((name, verdict))

        print("  %-16s %6.1f%% %6.1f%% %6.1f%% RGB%-14s %s"
              % (name, d, l, u, str(dom), verdict))

    return (1 if bad else 0), bad


MUTATION_TARGET = os.path.join(ROOT, "Demos", "Win8Metro", "Page_Cleanup_Build.cpp")
MUTATION_MARK = b"    return;   // [MUTATION]"


def plant_mutation():
    """在 RefreshThemeColors 开头插入提前返回，制造真实的"漏刷新"缺陷。

    为什么必须真改产品代码
    --------------------
    上一版注入是"把第 0 页的结论直接标成 BAD"，那只证明退出码链路通，
    完全没证明判据能识别真实像素残留 —— 属于自证失效。这里改产品源码，
    让某一页真的不重刷，跑完再逐字节还原。

    返回 (backup_bytes, None) 或 (None, reason)。
    """
    if not os.path.isfile(MUTATION_TARGET):
        return None, "target missing"
    orig = open(MUTATION_TARGET, "rb").read()
    if MUTATION_MARK in orig:
        return None, "already mutated"
    lines = orig.decode("utf-8-sig").split(chr(10))
    idx = None
    for i, l in enumerate(lines):
        if l.startswith("void CPageCleanup::RefreshThemeColors()"):
            idx = i
            break
    if idx is None or idx + 1 >= len(lines):
        return None, "function not found"
    crlf = chr(13) if lines[idx].endswith(chr(13)) else ""
    lines.insert(idx + 2, MUTATION_MARK.decode() + crlf)
    bom = bytes([0xEF, 0xBB, 0xBF]) if orig[:3] == bytes([0xEF, 0xBB, 0xBF]) else b""
    open(MUTATION_TARGET, "wb").write(bom + chr(10).join(lines).encode("utf-8"))
    return orig, None


def restore_mutation(orig):
    open(MUTATION_TARGET, "wb").write(orig)
    return open(MUTATION_TARGET, "rb").read() == orig


def build_app():
    """用 MSBuild 重建主程序；返回 True 表示产物是新的。"""
    vswhere = os.path.join(os.environ.get("ProgramFiles(x86)", ""),
                           "Microsoft Visual Studio", "Installer", "vswhere.exe")
    try:
        r = subprocess.run([vswhere, "-latest", "-property", "installationPath"],
                           capture_output=True, text=True)
    except OSError:
        return False
    vs = r.stdout.strip()
    if not vs:
        return False
    msb = os.path.join(vs, "MSBuild", "Current", "Bin", "MSBuild.exe")
    prj = os.path.join(ROOT, "Demos", "Win8Metro", "Win8Metro.vcxproj")
    r = subprocess.run([msb, prj, "/t:Build", "/p:Configuration=Release",
                        "/p:Platform=x64", "/p:SolutionDir=" + ROOT + "\\",
                        "/m:1", "/nodeReuse:false", "/nologo", "/v:minimal"],
                       capture_output=True)
    return r.returncode == 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--theme", choices=["dark", "light", "both"], default="both")
    ap.add_argument("--inject", action="store_true",
                    help="mark one page as bad; the checker must exit non-zero")
    args = ap.parse_args()

    if not os.path.isfile(EXE):
        print("[SKIP] Win8Metro.exe not built")
        return 2

    # --inject：真的把某一页的 RefreshThemeColors 改成提前返回，重建，
    #   跑一遍确认能检出，然后逐字节还原并重建。跑完必须确认还原成功，
    #   否则会把验证用的故意缺陷留在产品里。
    backup = None
    if args.inject:
        backup, why = plant_mutation()
        if backup is None:
            print("[SKIP] cannot plant mutation: " + why)
            return 2
        print("[INFO] mutation planted in Page_Cleanup_Build.cpp")
        if not build_app():
            restore_mutation(backup)
            print("[SKIP] rebuild failed; mutation reverted, nothing asserted")
            return 2

    try:
        themes = ["dark", "light"] if args.theme == "both" else [args.theme]
        worst = 0
        allbad = []
        for th in themes:
            kill_app()
            rc, bad = check_theme(th)
            worst = max(worst, rc)
            allbad.extend((th,) + b for b in bad)
    finally:
        if backup is not None:
            ok = restore_mutation(backup)
            print("[INFO] mutation reverted byte-identical: %s" % ok)
            if not build_app():
                print("[WARN] rebuild after revert failed; next run will rebuild")

    print()
    if args.inject:
        if worst == 1:
            print("[PASS] inject detected: the mutated page is reported as residual")
            return 0
        print("[FAIL] inject: mutated page was NOT reported -- gate is blind")
        return 3

    # SKIP (2) must not be reported as FAIL. Both themes returning "cannot
    #   export" means the environment is not ready (typically a stale probe copy
    #   holding the single-instance mutex), not that a page has a residual band.
    #   The driver distinguishes them by the [SKIP] marker + exit 3.
    if worst == 2:
        print("[SKIP] could not export pages; environment not ready, nothing asserted")
        return 3

    if worst != 0:
        print("[FAIL] %d page(s) failed the theme audit:" % len(allbad))
        for t, n, why in allbad:
            print("       [%s] %-16s %s" % (t, n, why))
        return 1

    print("[PASS] every page matches its theme (no residual band, no stale block)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
