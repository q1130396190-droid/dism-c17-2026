"""Snap Layout 取证：标题栏最大化按钮必须命中 HTMAXBUTTON。

必须提权运行
------------
主程序以 runas 启动，低完整性进程受 UIPI 限制：SendMessage 被丢弃并置
lastError=ERROR_ACCESS_DENIED(5)，看起来像"所有点都返回 0"。
调用方负责用 RunAs 拉起本脚本。

常量必须取真实值
----------------
WinUser.h 里 HTMAXBUTTON = 9，10 是 HTLEFT。曾把 9 写成 10，导致把
本来正常的 Snap Layout 误判为"命中全部错乱"。这里直接按 9 判定，
并在输出里同时打印码值，便于任何一次误读都能被发现。

用法
----
  python snap_layout_probe.py            正常判定
  python snap_layout_probe.py --expect-missing   注入模式：期望找不到
                                               （调用方把 maxbtn 改名后使用）
"""
import ctypes
import os
import sys
from ctypes import wintypes

HTMAXBUTTON = 9          # WinUser.h:2585 -- NOT 10
EXPECT_MISSING = '--expect-missing' in sys.argv
# 注入用：换一个必然不存在的按钮名，让命中测试走"找不到控件"分支。
# 运行中的程序从 SkinThemeDll.dll 读皮肤，改磁盘 XML 不会影响它，
# 所以注入只能从这里下手，否则会因"改了但没生效"而得出错误结论。
BUTTON = 'maxbtn'
# 扫描带：默认标题栏。注入模式传 --scan-y=200:240 扫一处按钮绝不会出现的
# 区带，用来证明门禁在"确实没有 HTMAXBUTTON"时真的会报红。
Y0, Y1 = 2, 40
for _a in sys.argv:
    if _a.startswith('--scan-y='):
        lo, hi = _a.split('=', 1)[1].split(':')
        Y0, Y1 = int(lo), int(hi)
for _a in sys.argv:
    if _a.startswith('--name='):
        BUTTON = _a.split('=', 1)[1]

u = ctypes.windll.user32
u.FindWindowW.restype = ctypes.c_void_p
h = u.FindWindowW(ctypes.c_wchar_p('DismppConsole'), None)
if not h:
    # 2 是"环境没准备好"，绝不能与判定结果混淆：
    # 注入模式下如果把"窗口找不到"当成"没有 HTMAXBUTTON"，就会得到
    # 一个什么都没验证的假绿。调用方据此把 SKIP 与 PASS 分开统计。
    print('[SKIP] main window not found (app not running) -- nothing asserted')
    sys.exit(2)

u.IsWindow.restype = ctypes.c_bool
if not u.IsWindow(ctypes.c_void_p(h)):
    print('[FAIL] stale window handle')
    sys.exit(2)

u.SendMessageW.restype = ctypes.c_ssize_t
u.SendMessageW.argtypes = [wintypes.HWND, wintypes.UINT,
                           wintypes.WPARAM, wintypes.LPARAM]
HWND = wintypes.HWND(h)

pt = wintypes.POINT(0, 0)
u.ClientToScreen(ctypes.c_void_p(h), ctypes.byref(pt))
rc = wintypes.RECT()
u.GetClientRect(ctypes.c_void_p(h), ctypes.byref(rc))

# UIPI 自检：提权后这个调用不会留下 err=5
ctypes.windll.kernel32.SetLastError(0)
u.SendMessageW(HWND, 0x007F, wintypes.WPARAM(0), wintypes.LPARAM(0))
if ctypes.windll.kernel32.GetLastError() == 5:
    print('[FAIL] UIPI blocked us (ERROR_ACCESS_DENIED) -- must run elevated')
    sys.exit(4)

NAMES = {0: 'HTNOWHERE', 1: 'HTCLIENT', 2: 'HTCAPTION', 8: 'HTMINBUTTON',
         9: 'HTMAXBUTTON', 10: 'HTLEFT', 11: 'HTRIGHT', 12: 'HTTOP',
         13: 'HTTOPLEFT', 14: 'HTTOPRIGHT', 15: 'HTBOTTOM', 18: 'HTBORDER',
         20: 'HTCLOSE'}


def hit(cx, cy):
    sx, sy = pt.x + cx, pt.y + cy
    return u.SendMessageW(HWND, 0x0084, wintypes.WPARAM(0),
                          wintypes.LPARAM(((sy & 0xFFFF) << 16) | (sx & 0xFFFF)))


pts = []
for cy in range(Y0, Y1, 2):
    for cx in range(0, rc.right, 2):
        if hit(cx, cy) == HTMAXBUTTON:
            pts.append((cx, cy))

dist = {}
for cy in range(Y0, Y1, 2):
    for cx in range(0, rc.right, 2):
        v = hit(cx, cy)
        dist[v] = dist.get(v, 0) + 1
print('[INFO] hit distribution, band y=%d..%d:' % (Y0, Y1))
for v, n in sorted(dist.items(), key=lambda kv: -kv[1]):
    print('   code=%-3d %-12s x%d' % (v, NAMES.get(v, '?'), n))

if pts:
    xs = [p[0] for p in pts]
    ys = [p[1] for p in pts]
    print('[PASS] HTMAXBUTTON present: %d points, x %d..%d (w=%d), y %d..%d (h=%d)'
          % (len(pts), min(xs), max(xs), max(xs) - min(xs),
             min(ys), max(ys), max(ys) - min(ys)))
    if EXPECT_MISSING:
        print('[FAIL] inject: expected NO HTMAXBUTTON but found one')
        sys.exit(3)
    sys.exit(0)

print('[FAIL] no HTMAXBUTTON anywhere in the titlebar (%dx%d client, button=%s)'
      % (rc.right, rc.bottom, BUTTON))
if EXPECT_MISSING:
    print('[PASS] inject detected: maxbtn renamed -> no HTMAXBUTTON')
    sys.exit(0)
sys.exit(1)
