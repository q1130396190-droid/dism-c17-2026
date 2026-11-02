#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
DuiLib_Ultimate C++23 升级 —— 构建矩阵探针 (S0 / 轮次 2, T0.2)

背景
----
环境无 cmake/msbuild，但有 MSVC 19.51（VS18 Community）。
因此本脚本用 `cl.exe /c` 直接编译「包含 DuiLib/UIlib.h 的探针编译单元」，
在不做完整链接的前提下验证 8 种构型能否通过**预处理 + 语法 + 类型检查**。

覆盖构型（T0.2 要求的 8 构型）
------------------------------
  {Win32, x64} x {Unicode, MBCS} x {静态库(UILIB_STATIC), 动态库(UILIB_EXPORTS)}

用法
----
    python tools/build_matrix.py                    # 默认 /std:c++latest
    python tools/build_matrix.py --std c++20        # 指定标准
    python tools/build_matrix.py --std c++17
    python tools/build_matrix.py --json             # 输出机器可读结果

产出
----
docs/build-matrix-<std>.json

注意
----
- 只读：不写入 DuiLib/**，探针临时文件放在 .box-agent-scratch/。
- 本探针验证「头文件可编译性」，不等价于完整构建（完整构建需 msbuild/cmake）。
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOCS = os.path.join(ROOT, "docs")
SCRATCH = os.path.join(ROOT, ".box-agent-scratch", "build_matrix")

PROBE_SRC = r'''// 自动生成：构建矩阵探针。验证 DuiLib/UIlib.h 在指定构型下可编译。
// 头文件路径由 build_matrix.py 通过 /I 注入，此处用绝对路径占位符避免相对路径歧义。
#include "@@DUILIB_ROOT@@/DuiLib/UIlib.h"

#include <memory>
#include <string_view>
#include <vector>
#include <span>
#include <optional>

#ifdef __cpp_lib_expected
#include <expected>
#endif
#ifdef __cpp_lib_flat_map
#include <flat_map>
#endif

using namespace DuiLib;

static_assert(sizeof(TNotifyUI) > 0, "TNotifyUI must be complete");
static_assert(sizeof(CDuiString) > 0, "CDuiString must be complete");

// 导出面抽样：确认关键类声明可见
static_assert(sizeof(CControlUI) > 0, "CControlUI must be complete");
static_assert(sizeof(CPaintManagerUI) > 0, "CPaintManagerUI must be complete");

int probe(std::string_view sv)
{
    std::vector<int> v;
    auto copy(v);
    return (int)sv.size() + (int)copy.size();
}

int probe_expected()
{
#ifdef __cpp_lib_expected
    std::expected<int, int> e = 42;
    return *e;
#else
    return 0;
#endif
}
'''


def find_toolchain():
    """定位 MSVC / Windows SDK。返回 dict 或 None。"""
    pats = [
        r"C:\Program Files\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*",
        r"C:\Program Files (x86)\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*",
    ]
    vc_dirs = []
    for p in pats:
        vc_dirs += glob.glob(p)
    if not vc_dirs:
        return None
    vc = sorted(vc_dirs)[-1]

    sdk_inc = sorted(glob.glob(r"C:\Program Files (x86)\Windows Kits\10\Include\*"))
    sdk_lib_root = r"C:\Program Files (x86)\Windows Kits\10\Lib"
    sdk_lib_ver = sorted(glob.glob(os.path.join(sdk_lib_root, "*")))
    if not sdk_inc or not sdk_lib_ver:
        return None

    return {
        "vc": vc,
        "msvc_version": os.path.basename(vc),
        "sdk_include": sdk_inc[-1],
        "sdk_lib": sdk_lib_ver[-1],
        "webview2_include": os.path.join(ROOT, "3rd", "WebView2", "include"),
    }


def build_clinvoke(tc, arch, std):
    """构造 cl.exe 调用参数。arch: x86 | x64"""
    host = "Hostx64" if arch == "x64" else "Hostx86"
    cl = os.path.join(tc["vc"], "bin", host, arch, "cl.exe")
    sdk_inc = tc["sdk_include"]
    inc = [
        os.path.join(tc["vc"], "include"),
        os.path.join(sdk_inc, "ucrt"),
        os.path.join(sdk_inc, "shared"),
        os.path.join(sdk_inc, "um"),
        os.path.join(sdk_inc, "winrt"),
    ]
    if os.path.isdir(tc["webview2_include"]):
        inc.append(tc["webview2_include"])
    inc.append(ROOT)  # 保证 DuiLib/UIlib.h 可被解析
    return cl, ["/I" + p for p in inc]


def run_case(tc, arch, charset, linkage, std, verbose=False):
    cl, inc_flags = build_clinvoke(tc, arch, std)
    defines = ["/DUNICODE", "/D_UNICODE"] if charset == "Unicode" else ["/DMBCS"]
    defines.append("/DUILIB_STATIC" if linkage == "static" else "/DUILIB_EXPORTS")

    tag = f"{arch}_{charset}_{linkage}_{std.replace('+','p')}"
    outdir = os.path.join(SCRATCH, tag)
    os.makedirs(outdir, exist_ok=True)
    src = os.path.join(SCRATCH, "probe.cpp")
    with open(src, "w", encoding="utf-8") as f:
        f.write(PROBE_SRC.replace("@@DUILIB_ROOT@@", ROOT.replace("\\", "/")))

    args = [cl, "/nologo", "/c", f"/std:{std}", "/EHsc", "/W3", "/WX-",
            "/Fo" + os.path.join(outdir, "probe.obj")] + defines + inc_flags + [src]

    try:
        r = subprocess.run(args, cwd=SCRATCH, capture_output=True, text=True,
                           encoding="utf-8", errors="replace", timeout=600)
    except subprocess.TimeoutExpired:
        return {"tag": tag, "arch": arch, "charset": charset, "linkage": linkage,
                "std": std, "ok": False, "errors": 1, "warnings": 0,
                "error_detail": ["TIMEOUT"]}

    out = (r.stdout or "") + (r.stderr or "")
    errors = [l for l in out.splitlines() if " error " in l or "fatal error" in l]
    warns = [l for l in out.splitlines() if " warning " in l]
    return {
        "tag": tag, "arch": arch, "charset": charset, "linkage": linkage, "std": std,
        "ok": r.returncode == 0 and not errors,
        "exit_code": r.returncode,
        "errors": len(errors),
        "warnings": len(warns),
        "error_detail": [e.strip()[:220] for e in errors[:5]],
        "warning_detail": [w.strip()[:200] for w in warns[:6]],
    }



def run_dual(tc, args) -> int:
    """双轨验证：兼容线(c++17) + 现代线(c++latest)，两条线各 8 构型都必须通过。"""
    tracks = [("compat_cpp17", "c++17"), ("modern_cpp23", "c++latest")]
    report = {"purpose": "S1 双轨验证：兼容线(C++17) 与 现代线(C++23) 并行必须全绿",
              "method": "cl.exe /c 编译包含 DuiLib/UIlib.h 的探针编译单元", "tracks": {}}
    overall_ok = True

    for tname, std in tracks:
        cases = []
        for arch in ("x64", "x86"):
            for charset in ("Unicode", "MBCS"):
                for linkage in ("static", "dynamic"):
                    cases.append(run_case(tc, arch, charset, linkage, std))
        passed = sum(1 for c in cases if c["ok"])
        ok = passed == len(cases)
        overall_ok = overall_ok and ok
        report["tracks"][tname] = {
            "std": std, "cases": len(cases), "passed": passed,
            "ok": ok, "detail": cases,
        }

    report["summary"] = {
        "tracks": len(tracks),
        "all_passed": overall_ok,
        "verdict": ("双轨全部通过：兼容线与现代线可并行维护"
                    if overall_ok else "双轨存在失败：某条线无法编译，需修复后再推进"),
    }
    out_path = os.path.join(DOCS, "build-matrix-dual.json")
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(report, f, ensure_ascii=False, indent=2)

    if not args.json:
        print("=" * 74)
        print(f"双轨构建矩阵验证  MSVC={tc['msvc_version']}")
        print("=" * 74)
        for tname, info in report["tracks"].items():
            mark = "PASS" if info["ok"] else "FAIL"
            print(f"  {tname:16s} ({info['std']:11s})  {mark}  {info['passed']}/{info['cases']}")
            for c in info["detail"]:
                if not c["ok"]:
                    print(f"      ! {c['tag']}")
                    for e in c["error_detail"]:
                        print(f"        {e}")
        print()
        print(f"  >>> {report['summary']['verdict']}")
        print(f"已写出: {os.path.relpath(out_path, ROOT)}")

    return 0 if overall_ok else 1


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--std", default="c++latest", help="c++17 | c++20 | c++latest")
    ap.add_argument("--dual", action="store_true",
                    help="双轨模式：一次跑完 c++17(兼容线) 与 c++latest(现代线) 两档")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    tc = find_toolchain()
    if tc is None:
        print("[FATAL] 未找到 MSVC / Windows SDK，无法执行构建矩阵。")
        return 2

    os.makedirs(SCRATCH, exist_ok=True)
    os.makedirs(DOCS, exist_ok=True)

    # 双轨模式：兼容线 c++17 + 现代线 c++latest，两条线都必须全绿
    if args.dual:
        return run_dual(tc, args)

    cases = []
    for arch in ("x64", "x86"):
        for charset in ("Unicode", "MBCS"):
            for linkage in ("static", "dynamic"):
                cases.append(run_case(tc, arch, charset, linkage, args.std))

    passed = sum(1 for c in cases if c["ok"])
    payload = {
        "purpose": "S0 T0.2：C++23 升级的 8 构型编译矩阵探针（头文件可编译性）",
        "method": "cl.exe /c 编译包含 DuiLib/UIlib.h 的探针编译单元；不做链接",
        "toolchain": {
            "msvc": tc["msvc_version"],
            "sdk_include": tc["sdk_include"],
            "webview2_include_present": os.path.isdir(tc["webview2_include"]),
        },
        "std": args.std,
        "summary": {
            "cases": len(cases),
            "passed": passed,
            "failed": len(cases) - passed,
        },
        "cases": cases,
        "caveat": "探针验证头文件可编译性，不等价于完整构建；完整构建需 msbuild/cmake。",
    }
    out_path = os.path.join(DOCS, f"build-matrix-{args.std.replace('+','p')}.json")
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(payload, f, ensure_ascii=False, indent=2)

    if args.json:
        print(json.dumps(payload["summary"], ensure_ascii=False))
    else:
        print("=" * 74)
        print(f"构建矩阵探针  std={args.std}  MSVC={tc['msvc_version']}")
        print("=" * 74)
        print(f"  {'构型':<28} {'结果':<8} {'error':<7} {'warning'}")
        print("  " + "-" * 66)
        for c in cases:
            mark = "PASS" if c["ok"] else "FAIL"
            print(f"  {c['tag']:<28} {mark:<8} {c['errors']:<7} {c['warnings']}")
            for e in c["error_detail"]:
                print(f"        ! {e}")
        print()
        print(f"  通过 {passed}/{len(cases)}")
        print(f"已写出: {os.path.relpath(out_path, ROOT)}")

    return 0 if passed == len(cases) else 1


if __name__ == "__main__":
    sys.exit(main())
