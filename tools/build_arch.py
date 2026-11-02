#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
架构版构建验证：用本机真实工具链构建 DuiLib 框架 + 业务 Demo

背景
----
D:\\DuiLib_Ultimate-master架构版\\tools\\build_vs2026.bat 硬编码了：
    MSBuild : C:\\Program Files\\Microsoft Visual Studio\\18\\Enterprise\\...
    SDK     : 10.0.28000.0
本机实际是：
    MSBuild : ...\\18\\Community\\MSBuild\\Current\\Bin\\MSBuild.exe
    SDK     : 10.0.26100.0（且 DuiLib.vcxproj 自身声明的就是它）
直接跑原脚本必然失败，属于脚本环境假设过期，不是代码缺陷。

本工具用本机真实值构建，并把每次调用的完整参数与输出落盘，便于复核。

用法
----
    python tools/build_arch.py --lib                 # 只构建 DuiLib
    python tools/build_arch.py --demo Win8Metro      # 构建某个 Demo
    python tools/build_arch.py --all                 # DuiLib + 全部 Demo
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOCS = os.path.join(ROOT, "docs")
OUT = os.path.join(DOCS, "arch-build.json")

VCROOT = r"C:\Program Files\Microsoft Visual Studio\18\Community"
MSBUILD = os.path.join(VCROOT, "MSBuild", "Current", "Bin", "MSBuild.exe")


def sdk_version():
    """取本机实际存在的 Windows SDK 版本"""
    cands = sorted(glob.glob(r"C:\Program Files (x86)\Windows Kits\10\Include\*"))
    return os.path.basename(cands[-1]) if cands else None


def run_msbuild(proj, outdir=None, extra=None, tag=""):
    if not os.path.exists(MSBUILD):
        return {"tag": tag, "ok": False, "error": "MSBuild not found: " + MSBUILD}

    sdk = sdk_version()
    args = [MSBUILD, proj, "/t:Rebuild",
            "/p:Configuration=Release", "/p:Platform=x64",
            "/p:PlatformToolset=v145",
            "/p:SolutionDir=" + ROOT + os.sep,
            "/m", "/nologo", "/v:minimal"]
    if sdk:
        args.append("/p:WindowsTargetPlatformVersion=" + sdk)
    if outdir:
        args.append("/p:OutDir=" + outdir + os.sep)
    if extra:
        args.extend(extra)

    t0 = time.time()
    try:
        r = subprocess.run(args, cwd=ROOT, capture_output=True, text=True,
                           encoding="utf-8", errors="replace", timeout=1800)
    except subprocess.TimeoutExpired:
        return {"tag": tag, "ok": False, "error": "TIMEOUT", "cmd": args}

    out = (r.stdout or "") + (r.stderr or "")
    errs = [l.strip()[:240] for l in out.splitlines()
            if " error " in l.lower() or "error MSB" in l or "error C" in l or "LNK" in l and "error" in l.lower()]
    warn = len([l for l in out.splitlines() if " warning " in l.lower()])

    return {
        "tag": tag,
        "ok": r.returncode == 0,
        "exit": r.returncode,
        "elapsed_s": round(time.time() - t0, 1),
        "errors": errs[:20],
        "error_count": len(errs),
        "warning_count": warn,
        "cmd": args,
        "tail": [l.rstrip() for l in out.splitlines() if l.strip()][-12:],
    }


def find_artifact(name):
    pats = [
        os.path.join(ROOT, "bin64", "**", name),
        os.path.join(ROOT, "DuiLib", "Bin", "**", name),
        os.path.join(ROOT, "DuiLib", "bin64", "**", name),
        os.path.join(ROOT, "Lib", name),
        os.path.join(ROOT, "DuiLib", "Lib", name),
    ]
    for p in pats:
        hits = glob.glob(p, recursive=True)
        if hits:
            hits.sort(key=os.path.getmtime, reverse=True)
            return hits[0]
    return None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--lib", action="store_true")
    ap.add_argument("--demo", default="")
    ap.add_argument("--all", action="store_true")
    args = ap.parse_args()

    os.makedirs(DOCS, exist_ok=True)
    results = []

    print("=" * 76)
    print("架构版构建验证")
    print("  MSBuild :", MSBUILD)
    print("  SDK     :", sdk_version())
    print("  ROOT    :", ROOT)
    print("=" * 76)

    if args.lib or args.all:
        print("\n[1] DuiLib 框架 (Release|x64, v145)")
        r = run_msbuild(os.path.join(ROOT, "DuiLib", "DuiLib.vcxproj"), tag="DuiLib")
        results.append(r)
        print("    exit=%s  %.1fs  errors=%d" % (r.get("exit"), r.get("elapsed_s", 0),
                                                 r.get("error_count", 0)))
        for e in (r.get("errors") or [])[:5]:
            print("      " + e)
        for name in ("DuiLib.dll", "DuiLib.lib"):
            p = find_artifact(name)
            print("    %-12s %s" % (name, p if p else "NOT FOUND"))

    demos = []
    if args.all:
        demos = [os.path.basename(p) for p in glob.glob(os.path.join(ROOT, "Demos", "*"))
                 if os.path.isdir(p)]
    elif args.demo:
        demos = [args.demo]

    for d in demos:
        vcx = glob.glob(os.path.join(ROOT, "Demos", d, "*.vcxproj"))
        if not vcx:
            print("\n[SKIP] %s: no .vcxproj" % d)
            continue
        print("\n[Demo] %s" % d)
        r = run_msbuild(vcx[0], outdir=os.path.join(ROOT, "bin64"), tag=d)
        results.append(r)
        print("    exit=%s  %.1fs  errors=%d  warnings=%d"
              % (r.get("exit"), r.get("elapsed_s", 0), r.get("error_count", 0),
                 r.get("warning_count", 0)))
        for e in (r.get("errors") or [])[:6]:
            print("      " + e)
        # 工程类型决定产物：DynamicLibrary 产出 .dll，Application 产出 .exe。
        # 早先只找 .exe，把 3 个 DLL 项目误报成 "NOT FOUND"（构建其实成功）。
        cfg = open(vcx[0], "rb").read().decode("utf-8", errors="replace")
        m = re.search(r"<ConfigurationType>([^<]+)<", cfg)
        ctype = m.group(1) if m else "Application"
        want = ".dll" if ctype == "DynamicLibrary" else ".exe"
        art = find_artifact(d + want)
        r["config_type"] = ctype
        r["artifact"] = art
        print("    %-14s %s" % (d + want, art if art else "NOT FOUND"))

    payload = {
        "purpose": "架构版：用本机真实工具链构建 DuiLib + Demos",
        "msbuild": MSBUILD,
        "sdk": sdk_version(),
        "root": ROOT,
        "results": results,
        "verdict": "PASS" if results and all(r.get("ok") for r in results) else "FAIL",
    }
    json.dump(payload, open(OUT, "w", encoding="utf-8", newline="\n"),
              ensure_ascii=False, indent=1)
    print()
    print("  判定: %s" % payload["verdict"])
    print("  已写出: %s" % os.path.relpath(OUT, ROOT))
    return 0 if payload["verdict"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
