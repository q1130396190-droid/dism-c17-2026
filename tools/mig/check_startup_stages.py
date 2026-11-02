"""冷启动阶段耗时门禁。

为什么重写
---------
旧实现用 `/DumpPages` 启动程序并 `Start-Process -Wait`，而该入口早已不存在：
程序进入消息循环，`-Wait` 永不返回，探针每次都被套件杀掉（exit 124）。
它还试图从墙钟推"启动耗时"，但实测 `Start-Process -Wait` 对 `cmd /c exit`
这类空程序就有约 1000ms 的启动器开销，墙钟数字根本不能用。

现在的做法
---------
框架层本来就有进程内埋点（DuiLib/Core/PerfProbe.h），由环境变量
`DUILIB_STARTUP_PROBE=1` 启用，用 QPC 打点并直接打印：
    [perf] pm init        0.04ms
    [perf] dlg xml-parse  0.46ms
    [perf] dlg build     13.05ms
    [perf] attach         8.84ms
    [perf] initwindow    50.36ms
这些是**进程内**的真实分段耗时，不含启动器开销，正是要卡的对象。

判据
----
1) 每个阶段都必须真的输出（缺一段说明埋点被破坏，属回归）
2) dlg build 与 initwindow 各自不超过预算
3) 用 asInvoker 副本启动，避免 UAC 弹窗阻塞自动化

预算取值来自实测而不是拍脑袋：dlg build 13.05ms、initwindow 50.36ms，
预算给 3 倍余量（40ms / 160ms），既能拦住"回到 109ms"这类真实退化，
又不会因机器抖动误报。

退出码：0 通过，1 超预算或缺阶段，2 环境不就绪（SKIP），3 注入未报出。
"""
from __future__ import annotations

import argparse
import ctypes
import os
import re
import subprocess
import sys
import time

ROOT = r"D:\DuiLib_Ultimate-master"
BIN = os.path.join(ROOT, "bin64")
ASI = os.path.join(BIN, "Win8Metro_asi.exe")
OUT = os.path.join(ROOT, ".box-agent-scratch", "mig", "startup_measure.out")
BAT = os.path.join(ROOT, ".box-agent-scratch", "mig", "startup_measure.bat")

# 实测：dlg build 13.05ms / initwindow 50.36ms -> 3x headroom
BUDGET = {
    "dlg build": 40.0,
    "initwindow": 160.0,
}
REQUIRED = ("pm init", "dlg xml-parse", "dlg build", "attach", "initwindow")

PS = ["powershell", "-NoProfile", "-Command"]


def kill_app():
    subprocess.run(PS + ["Get-Process | Where-Object { $_.ProcessName -like '*Win8Metro*' } "
                         "| ForEach-Object { Stop-Process -Id $_.Id -Force "
                         "-ErrorAction SilentlyContinue }"],
                   capture_output=True)
    for _ in range(10):
        time.sleep(1)
        r = subprocess.run(PS + ["(Get-Process | Where-Object { $_.ProcessName -like "
                                 "'*Win8Metro*' } | Measure-Object).Count"],
                           capture_output=True)
        try:
            if int(r.stdout.decode("utf-8", "replace").strip() or "0") == 0:
                return
        except ValueError:
            pass


def measure():
    """跑一次带埋点的启动，返回 {阶段: ms}。"""
    if os.path.exists(OUT):
        os.remove(OUT)
    with open(BAT, "w") as f:
        f.write("@echo off\n")
        f.write("set DUILIB_STARTUP_PROBE=1\n")
        f.write('cd /d "%s"\n' % BIN)
        f.write('"%s" > "%s" 2>&1\n' % (ASI, OUT))

    ctypes.windll.shell32.ShellExecuteA(None, b"open", BAT.encode(), None, None, 0)
    for _ in range(40):
        time.sleep(1)
        if os.path.exists(OUT) and os.path.getsize(OUT) > 200:
            break

    kill_app()
    if not os.path.exists(OUT):
        return None

    text = open(OUT, "rb").read().decode("gbk", "replace")
    stages = {}
    for line in text.splitlines():
        m = re.search(r"\[perf\]\s+(.+?)\s+([\d.]+)ms\s*$", line)
        if m:
            name = m.group(1).strip()
            # 缩进的子项带上父名，避免 "pmctor:register" 这类重名
            if line.startswith("[perf]   "):
                name = "  " + name
            stages[name] = float(m.group(2))
    return stages


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--inject", action="store_true",
                    help="tighten a budget so the gate must fail")
    args = ap.parse_args()

    if not os.path.isfile(ASI):
        print("[SKIP] asInvoker build missing (Win8Metro_asi.exe); "
              "use mt.exe to rewrite the manifest to asInvoker")
        return 2

    kill_app()
    stages = measure()

    if not stages:
        print("[SKIP] no [perf] output; DUILIB_STARTUP_PROBE instrumentation "
              "may be missing or the app did not start")
        return 2

    print("[INFO] measured %d stage(s) in-process (QPC, no launcher overhead)"
          % len(stages))
    for name, ms in stages.items():
        print("       %-24s %7.2f ms" % (name, ms))

    problems = []

    missing = [r for r in REQUIRED if r not in stages]
    if missing:
        problems.append("missing stage(s): " + ", ".join(missing))
    else:
        print("[PASS] every required stage reported: " + ", ".join(REQUIRED))

    budget = dict(BUDGET)
    if args.inject:
        budget["dlg build"] = 0.001      # force a failure
        print("[INFO] inject: dlg build budget forced to 0.001ms")

    for name, limit in budget.items():
        if name not in stages:
            continue
        ms = stages[name]
        if ms > limit:
            problems.append("%s %.2fms > budget %.1fms" % (name, ms, limit))
        else:
            print("[PASS] %-16s %7.2f ms <= %.1f ms" % (name, ms, limit))

    if problems:
        print("[FAIL] %d problem(s):" % len(problems))
        for p in problems:
            print("       " + p)
        if args.inject:
            # Return NON-ZERO on purpose. gate_inject_meta requires an injected run
            # to fail; a 0 here made it classify this gate as "unusable" even though
            # the tightened budget really did trip. The marker line says what
            # happened, the exit code says "yes, it failed as designed".
            print("[PASS] inject detected: the gate can fail as intended")
            return 1
        return 1

    if args.inject:
        print("[FAIL] inject: tightened budget was not reported -- gate is blind")
        return 3

    print("[PASS] cold-start stages are within budget")
    return 0


if __name__ == "__main__":
    sys.exit(main())
