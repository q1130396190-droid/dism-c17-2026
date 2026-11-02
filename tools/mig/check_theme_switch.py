"""手动切换主题的残留门禁。

用户报的症状
-----------
"默认启动深浅色都正常，手动切换才出 bug"。这条链路静态检查完全覆盖不到：
ExpandSkinFile 在建树时把 {tok:*} 换成字面值，建树只发生一次，所以启动
永远是对的；只有手动切换才会让控件停留在旧主题的字面颜色上。

实测演进（主窗口客户区暗像素占比）
----------------------------------
    默认启动浅色                      dark =  1.0%
    手动切到浅色（初版）              dark = 33.7%   Session 信息条整条残留
    补匿名容器名后                    dark =  9.4%
    补 clngrp 分组头重刷后            dark =  0.8%   优于默认启动

三个根因（都已修）
----------------
1) session 条里两个 VerticalLayout 用了 {tok:SurfaceAlt} 却没有 name，
   SkinTokenReplayTable 按名定位不到；
2) CPageCleanup::RefreshThemeColors() 只遍历规则行 clnrow_*，漏了分组头
   clngrp_*；
3) 皮肤里还有若干匿名容器，由 kLegacy 按值兜底（见生成器输出的 unnamed 计数）。

工具约束（都是踩过的坑）
----------------------
* PrintWindow 跨完整性级别返回 0，位图全黑，看着像"页面没渲染"。本进程
  必须先提权，再用 PrintWindow。
* 主程序以 runas 启动，单实例互斥量会挡掉新实例；启动前必须清残留。
* 主程序不能用 PowerShell 的 Start-Process -Verb RunAs 启动（实测每次都在
  启动阶段以 0xC0000005 退出），必须用 ShellExecuteW。

退出码：0 通过，1 残留超标，2 环境不就绪（SKIP），3 --inject 未报出。
"""
from __future__ import annotations

import ctypes
import os
import subprocess
import sys
import time
from ctypes import wintypes

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                '..', 'probes'))
import _app_cleanup  # noqa: E402  (shared pre-launch cleanup)

ROOT = "D:/DuiLib_Ultimate-master"
BIN = ROOT + "/bin64"
EXE = BIN + "/Win8Metro.exe"
CFG = BIN + "/config.ini"

# 阈值演进（收缩式棘轮，只允许越来越严）：
#   修复前 33.7% -> 首版 20% -> 补匿名容器 9.4% -> 补 clngrp 0.8%
# 5% 留 4 个百分点余量给文字/图标，同时能拦住"整条带残留"（那样会跳到 30%+）。
MAX_DARK_AFTER_SWITCH = 5.0

u = ctypes.windll.user32
gdi = ctypes.windll.gdi32
u.PostMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_size_t,
                           ctypes.c_ssize_t]
u.FindWindowW.restype = ctypes.c_void_p

INJECT = "--inject" in sys.argv
LAUNCH = "--launch" in sys.argv


def ensure_elevated():
    """PrintWindow 对跨完整性级别的窗口返回 0（位图全黑）。

    实测：非提权进程调 PrintWindow 一律 False，截出来 100% 黑，看起来像
    "页面根本没渲染"，实际只是拿不到像素。所以先自检，不足就重启自己。
    """
    if "--elevated" in sys.argv:
        return
    if ctypes.windll.shell32.IsUserAnAdmin():
        return

    out = os.path.join(os.environ.get("TEMP", "."), "theme_switch_elev.txt")
    if os.path.exists(out):
        try:
            os.remove(out)
        except OSError:
            pass
    # 把调用方的 -- 参数一并转给子进程：漏掉 --inject 会让注入自测在提权后
    #   静默退化成"正常跑一遍"，看着通过但什么都没验证。
    passthru = " ".join(a for a in sys.argv[1:] if a.startswith("--"))
    args = '%s %s --elevated %s > "%s" 2>&1' % (
        sys.executable, os.path.abspath(__file__), passthru, out)
    ctypes.windll.shell32.ShellExecuteW(
        None, "runas", os.environ.get("COMSPEC", "cmd.exe"),
        "/c " + args, None, 1)

    for _ in range(240):
        time.sleep(1)
        if not os.path.exists(out):
            continue
        try:
            txt = open(out, "r", encoding="utf-8", errors="replace").read()
        except OSError:
            continue
        if any(k in txt for k in ("[PASS]", "[FAIL]", "[SKIP]")):
            print(txt)
            if "[PASS]" in txt:
                sys.exit(0)
            if "[SKIP]" in txt:
                sys.exit(2)
            sys.exit(1)
    print("[SKIP] elevated child produced no result")
    sys.exit(2)


def kill_app():
    """Stop every *Win8Metro* variant and wait for the mutex to be released.

    Delegates to tools/probes/_app_cleanup.py. Two copies of this loop had already
    drifted apart -- one cleaned only the shipping exe name, the other used a
    wildcard -- and the difference is exactly what let a leftover probe copy hold
    the single-instance mutex while a batch run reported "window not found".
    """
    return _app_cleanup.kill_app()


def find_main():
    return u.FindWindowW(ctypes.c_wchar_p("DismppConsole"), None)


def grab():
    """截主窗口客户区，返回 (raw, w, h, stride)。"""
    h = find_main()
    if not h:
        return None
    rc = wintypes.RECT()
    u.GetClientRect(ctypes.c_void_p(h), ctypes.byref(rc))
    w, hh = rc.right, rc.bottom
    hdc = u.GetDC(ctypes.c_void_p(h))
    mem = gdi.CreateCompatibleDC(hdc)
    bmp = gdi.CreateCompatibleBitmap(hdc, w, hh)
    old = gdi.SelectObject(mem, bmp)
    u.PrintWindow(ctypes.c_void_p(h), mem, 0)
    gdi.SelectObject(mem, old)

    class BIH(ctypes.Structure):
        _fields_ = [("biSize", wintypes.DWORD), ("biWidth", wintypes.LONG),
                    ("biHeight", wintypes.LONG), ("biPlanes", wintypes.WORD),
                    ("biBitCount", wintypes.WORD),
                    ("biCompression", wintypes.DWORD),
                    ("biSizeImage", wintypes.DWORD),
                    ("biXPels", wintypes.LONG), ("biYPels", wintypes.LONG),
                    ("biClrUsed", wintypes.DWORD),
                    ("biClrImp", wintypes.DWORD)]

    bi = BIH()
    bi.biSize = ctypes.sizeof(BIH)
    bi.biWidth = w
    bi.biHeight = -hh
    bi.biPlanes = 1
    bi.biBitCount = 32
    stride = w * 4
    buf = ctypes.create_string_buffer(stride * hh)
    gdi.GetDIBits(mem, bmp, 0, hh, buf, ctypes.byref(bi), 0)
    gdi.DeleteObject(bmp)
    gdi.DeleteDC(mem)
    u.ReleaseDC(ctypes.c_void_p(h), hdc)
    return buf.raw, w, hh, stride


def dark_share(raw, w, h, stride):
    dark = tot = 0
    for y in range(0, h, 6):
        base = y * stride
        for x in range(0, w, 6):
            i = base + x * 4
            lum = (raw[i + 2] * 299 + raw[i + 1] * 587 + raw[i] * 114) // 1000
            tot += 1
            if lum < 80:
                dark += 1
    return dark * 100.0 / max(tot, 1)


def post_click(hwnd, x, y):
    lp = (int(y) << 16) | (int(x) & 0xFFFF)
    h = ctypes.c_void_p(hwnd)
    u.PostMessageW(h, 0x0200, 0, lp)
    time.sleep(0.10)
    u.PostMessageW(h, 0x0201, 1, lp)
    time.sleep(0.08)
    u.PostMessageW(h, 0x0202, 0, lp)


def find_picker():
    out = []

    def cb(hwnd, lp):
        cls = ctypes.create_unicode_buffer(128)
        u.GetClassNameW(hwnd, cls, 128)
        if cls.value == "DismThemePickerWnd":
            r = wintypes.RECT()
            u.GetWindowRect(ctypes.c_void_p(hwnd), ctypes.byref(r))
            out.append((hwnd, r.left, r.top, r.right, r.bottom))
        return True

    u.EnumWindows(ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p,
                                     ctypes.c_void_p)(cb), 0)
    return out[0] if out else None


def switch_to(idx, n_items=3):
    """点开主题浮层并选第 idx 项。

    项顺序取自 ThemePickerWnd.cpp:96-98：
        0 = 跟随系统   1 = 浅色   2 = 深色
    早期按 "0.20 = 浅色" 估算，实际点了 Auto（系统本身浅色），于是"切换
    成功"其实是跟随系统，量出来仍 33.7%，把自己带偏过一轮。
    """
    h = find_main()
    if not h:
        return False
    post_click(h, 1094, 20)         # btn_theme，实测 x 1072..1117
    time.sleep(1.6)
    pk = find_picker()
    if not pk:
        return False
    ph, pl, pt, pr, pb = pk
    iy = int((pb - pt) * (idx + 0.5) / n_items)
    post_click(ph, (pr - pl) // 2, iy)
    time.sleep(2.0)
    return True


def set_theme(mode):
    """config.ini 是 UTF-8 BOM，只换 Mode= 的值，其余字节不动。"""
    raw = open(CFG, "rb").read()
    for old in (b"Mode=light", b"Mode=dark", b"Mode=auto"):
        if old in raw:
            open(CFG, "wb").write(raw.replace(old, b"Mode=" + mode.encode(), 1))
            return True
    return False


def launch_and_wait():
    """清残留 -> 启动 -> 等首帧画稳。"""
    kill_app()
    ctypes.windll.shell32.ShellExecuteW(None, "runas", EXE, None, BIN, 1)
    for _ in range(40):
        time.sleep(1)
        if not find_main():
            continue
        # 窗口刚出现时首帧还没画完，截图会得到整屏纯色（实测 100% dark）。
        #   连续两次采样差异很小才认为可以测量。
        prev = None
        for _ in range(15):
            time.sleep(1)
            g = grab()
            if not g:
                break
            cur = dark_share(*g)
            if prev is not None and abs(cur - prev) < 3.0 and 0 < cur < 100:
                return True
            prev = cur
        return True
    return False


def main():
    ensure_elevated()

    if not os.path.isfile(EXE):
        print("[SKIP] Win8Metro.exe not built")
        return 3

    if LAUNCH or INJECT:
        set_theme("dark")            # 用户报的是"深色启动后切浅色"
        if not launch_and_wait():
            print("[SKIP] app did not come up -- nothing asserted")
            return 3

    h = find_main()
    if not h:
        print("[SKIP] main window not found -- nothing asserted")
        return 3

    g = grab()
    if not g:
        print("[SKIP] cannot capture client area")
        return 3
    d0 = dark_share(*g)
    print("[INFO] start dark=%.1f%%" % d0)

    # 先确保处于深色，再切浅色 —— 这才是用户报的场景。
    if d0 < 50:
        if not switch_to(2):
            print("[SKIP] picker did not open")
            return 3
        g = grab()
        d0 = dark_share(*g)
        print("[INFO] forced dark, dark=%.1f%%" % d0)

    if not switch_to(1):
        print("[SKIP] picker did not open on the second attempt")
        return 3

    g = grab()
    if not g:
        print("[SKIP] cannot capture after switch")
        return 3
    d1 = dark_share(*g)
    print("[INFO] after switch-to-light dark=%.1f%% (threshold %.1f%%)"
          % (d1, MAX_DARK_AFTER_SWITCH))

    if INJECT:
        print("[INFO] inject: threshold forced to 0.0%")
        if d1 <= 0.0:
            print("[FAIL] inject: even 0.0%% passed -- the gate is blind")
            return 3
        print("[PASS] inject detected: tightened threshold fails as expected")
        return 0

    if d1 > MAX_DARK_AFTER_SWITCH:
        print("[FAIL] residual dark after manual switch: %.1f%% > %.1f%%"
              % (d1, MAX_DARK_AFTER_SWITCH))
        print("       likely: a skin container uses {tok:*} but has no name, or a")
        print("       page's RefreshThemeColors() misses some controls.")
        return 1

    print("[PASS] manual light/dark switch leaves no large residual band")
    return 0


if __name__ == "__main__":
    sys.exit(main())
