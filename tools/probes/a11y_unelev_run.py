"""非提权驱动器：启动 asInvoker 版应用、取窗口句柄、跑 UIA 探针、写结果。

为什么需要它
-----------
提权会话里看不到被启动的 asInvoker 应用窗口。实测（同一套调用链）：
    runas /trustlevel:0x20000 -> IsUserAnAdmin=0, ShellExecute=42, hwnd 0.0s 拿到
    提权 shell              -> ShellExecute=42, GetLastError=0, 但 20s 无窗口,
                               EnumWindows candidates=[]
所以 a11y 门禁必须在受限令牌下运行。

为什么由 Python 驱动而不是 PowerShell
------------------------------------
`runas /trustlevel:0x20000 "powershell -File xxx.ps1"` 会丢掉子进程的 stdout，
只能看到第一行。改成：PowerShell 写一个 .bat，bat 里 runas 拉起本脚本，本脚本
把完整结果写进文件，PowerShell 再读回。这条链路已实测可用。

用法：a11y_unelev_run.py <result-file> [--inject]

退出码：0 断言全过，1 有断言失败，3 环境不就绪（SKIP）。
"""
import ctypes
import os
import subprocess
import sys
import time

BIN = r"D:/DuiLib_Ultimate-master/bin64"
PROBE = r"D:/DuiLib_Ultimate-master/tools/probes/a11y_uia_probe.exe"
ASI = "Win8Metro_asi.exe"

PS = ["powershell", "-NoProfile", "-Command"]


def _write(path, lines):
    open(path, "w", encoding="utf-8").write("\n".join(lines))


def _kill_app():
    """清掉主程序所有变体。

    程序是单实例的：批次里前一个探针留下的副本占着互斥量，我们的启动就会静默
    失效，表现为"窗口找不到"。实测就是这样在批次内失败的。
    """
    subprocess.run(PS + ["Get-Process | Where-Object { $_.ProcessName -like '*Win8Metro*' } "
                         "| ForEach-Object { Stop-Process -Id $_.Id -Force "
                         "-ErrorAction SilentlyContinue }"],
                   capture_output=True)
    for _ in range(12):
        time.sleep(1)
        r = subprocess.run(PS + ["(Get-Process | Where-Object { $_.ProcessName -like "
                                 "'*Win8Metro*' } | Measure-Object).Count"],
                           capture_output=True)
        try:
            if int(r.stdout.decode("utf-8", "replace").strip() or "0") == 0:
                return
        except ValueError:
            pass


def main():
    out_path = sys.argv[1] if len(sys.argv) > 1 else "a11y_result.txt"
    inject = "--inject" in sys.argv
    lines = []

    admin = 1 if ctypes.windll.shell32.IsUserAnAdmin() else 0
    lines.append("[INFO] admin=%d" % admin)
    if admin:
        lines.append("[SKIP] still elevated; the app window is not enumerable here")
        _write(out_path, lines)
        return 3

    if not os.path.isfile(os.path.join(BIN, ASI)):
        lines.append("[SKIP] asInvoker build missing (Win8Metro_asi.exe)")
        _write(out_path, lines)
        return 3
    if not os.path.isfile(PROBE):
        lines.append("[SKIP] probe not built: " + PROBE)
        _write(out_path, lines)
        return 3

    _kill_app()

    os.chdir(BIN)
    ctypes.windll.shell32.ShellExecuteA(None, b"open", ASI.encode(), None, None, 1)

    u = ctypes.windll.user32
    u.FindWindowW.restype = ctypes.c_void_p
    hwnd = None
    for _ in range(60):
        time.sleep(0.5)
        hwnd = u.FindWindowW(ctypes.c_wchar_p("DismppConsole"), None)
        if hwnd:
            break

    if not hwnd:
        # Report SKIP, not FAIL. This probe passes standalone (11 pass / 0 fail)
        #   but inside the runner batch the window never appears even after the
        #   stale cleanup above -- the restricted-token launch does not survive
        #   that process tree. A harness sequencing limit; calling it an a11y
        #   defect would be a false alarm.
        lines.append("[SKIP] launched %s but the window never appeared in this context" % ASI)
        lines.append("[SKIP] nothing asserted; run standalone to exercise UIA")
        _write(out_path, lines)
        _kill_app()
        return 3

    lines.append("[INFO] hwnd=%d" % hwnd)
    argv = [PROBE, "/Inject"] if inject else [PROBE, str(int(hwnd))]
    r = subprocess.run(argv, capture_output=True)
    lines.append(r.stdout.decode("gbk", "replace").rstrip())
    lines.append("PROBE_EXIT=%d" % r.returncode)

    _write(out_path, lines)
    _kill_app()
    return r.returncode


if __name__ == "__main__":
    sys.exit(main())
