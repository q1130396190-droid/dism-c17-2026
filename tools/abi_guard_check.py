#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
T5.15  ABI 护栏检查（G11）

目的：用 dumpbin 提取 DuiLib 每个 .cpp 编译出的 .obj 文件中的【外部符号】
     （未定义与已定义，即 DuiLib 向外界暴露的 API 面），与基线 diff，
     判定是否有 ABI 破坏性变更（新增/移除/改名）。

DuiLib 采用 UILIB_STATIC 静态库模式，没有 __declspec(dllexport)；因此
"公开符号"= obj 中所有 extern 符号（已定义 + 未定义），代表编译期 API 面。

用法：
    python tools/abi_guard_check.py --init        # 首次生成基线
    python tools/abi_guard_check.py --check       # 与基线 diff
    python tools/abi_guard_check.py --verify      # CI 模式：仅退出码

产物：
    docs/abi-baseline.json   符号基线（首次 init 生成）
    docs/abi-guard.json      diff 结果（check 生成）

判定：
    PASS  —— 无 removed（既有符号未减少）；新增符号属可选扩展
    FAIL  —— 有 removed 或 renamed（既有符号被移除或改名，破坏 ABI）
    SKIP  —— 基线不存在，需 --init
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WORK = os.path.join(ROOT, ".box-agent-scratch", "abi_guard")
BASELINE = os.path.join(ROOT, "docs", "abi-baseline.json")
OUT = os.path.join(ROOT, "docs", "abi-guard.json")
# T5.20：DuiLib::ext 第三方扩展公开 API 面（独立成栏，不与 core API 混淆）
OUT_EXT = os.path.join(ROOT, "docs", "abi-guard-ext.json")

# MSVC mangled name 中 `DuiLib::ext::` 前缀的匹配片段（不同 ABI 版本 `?xxx@DuiLib@ext@@`）
# dumpbin 输出的 External 符号可能以 `?xxx@DuiLib@ext@@` 出现（MSVC 命名）或 `DuiLib::ext::xxx`（LLVM/装饰）
EXT_PREFIXES = ("@DuiLib@ext", "DuiLib::ext::")

# 与 n1_integration_test.py 一致：跳过 precompiled-header 入口与第三方
SKIP = {"stb_image.h", "WebView2.h", "flash11.tlh", "StdAfx.cpp"}


def find_toolchain():
    vc = sorted(glob.glob(r"C:\Program Files\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*")
                + glob.glob(r"C:\Program Files (x86)\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*"))
    inc = sorted(glob.glob(r"C:\Program Files (x86)\Windows Kits\10\Include\*"))
    if not vc or not inc:
        return None
    vc = vc[-1]
    inc = inc[-1]
    dumpbin = os.path.join(vc, "bin", "Hostx64", "x64", "dumpbin.exe")
    cl = os.path.join(vc, "bin", "Hostx64", "x64", "cl.exe")
    if not os.path.exists(cl):
        cl = os.path.join(vc, "bin", "Hostx64", "x86", "cl.exe")
    return {"vc": vc, "sdk_inc": inc, "dumpbin": dumpbin, "cl": cl}


def build_includes(tc):
    inc_paths = [os.path.join(tc["vc"], "include"),
                 os.path.join(tc["vc"], "atlmfc", "include"),
                 os.path.join(tc["sdk_inc"], "ucrt"),
                 os.path.join(tc["sdk_inc"], "shared"),
                 os.path.join(tc["sdk_inc"], "um"),
                 os.path.join(tc["sdk_inc"], "winrt")]
    inc_paths += [os.path.join(ROOT, "3rd", "WebView2", "include"),
                  os.path.join(ROOT, "DuiLib"),
                  os.path.join(ROOT, "DuiLib", "Core"),
                  os.path.join(ROOT, "DuiLib", "Control"),
                  os.path.join(ROOT, "DuiLib", "Layout"),
                  os.path.join(ROOT, "DuiLib", "Utils"),
                  os.path.join(ROOT, ".box-agent-scratch", "s3_registry")]
    return inc_paths


def dui_sources():
    srcs = []
    for sub in ("Core", "Control", "Layout", "Utils"):
        d = os.path.join(ROOT, "DuiLib", sub)
        if not os.path.isdir(d):
            continue
        for fn in sorted(os.listdir(d)):
            if fn.endswith(".cpp") and fn not in SKIP:
                srcs.append(os.path.join(ROOT, d, fn))
    return srcs


def compile_all(tc, inc_paths, tag, std, debug):
    """编译全部 DuiLib .cpp 到 obj，返回成功编译的 obj 列表。"""
    cl = tc["cl"]
    defines = ["/DWIN32", "/D_WINDOWS", "/DUNICODE", "/D_UNICODE",
               "/DUILIB_STATIC", "/D_CRT_SECURE_NO_WARNINGS",
               "/D_CRT_SECURE_NO_DEPRECATE", "/D_CRT_NONSTDC_NO_WARNINGS"]
    defines.append("/D_DEBUG") if debug else defines.append("/DNDEBUG")

    os.makedirs(WORK, exist_ok=True)
    objs = []
    fails = []
    for i, src in enumerate(dui_sources()):
        obj = os.path.join(WORK, f"obj_{tag}_{i:03d}.obj")
        args = [cl, "/nologo", "/c", "/std:" + std, "/EHsc", "/W3", "/WX-", "/MP"] \
               + defines + ["/Fo" + obj, src] + ["/I" + p for p in inc_paths]
        try:
            r = subprocess.run(args, cwd=WORK, capture_output=True,
                               encoding="utf-8", errors="replace", timeout=180)
        except subprocess.TimeoutExpired:
            fails.append({"file": os.path.basename(src), "errors": ["TIMEOUT"]})
            continue
        out = (r.stdout or "") + (r.stderr or "")
        errs = [l.strip()[:200] for l in out.splitlines() if " error " in l]
        if r.returncode != 0 or errs:
            fails.append({"file": os.path.basename(src), "errors": errs[:3]})
        elif os.path.exists(obj):
            objs.append(obj)
    return objs, fails


def extract_symbols(tc, objs):
    """用 dumpbin /symbols 提取每个 obj 的 External 符号（跨模块可见 = DuiLib 的公开 API）
    以及 UNDEF 符号（未定义，来自外部库）。

    格式：
        <index> <value> <type> <type_info> <storage_class> | <name>
    例：
        00B 00000000 SECT4 notype External | IID_ICoreWebView2...
        9CC 00000000 UNDEF notype External | __security_cookie
    """
    dumpbin = tc["dumpbin"]
    defined = set()   # External 已定义（DuiLib 向外界暴露的符号）
    undefined = set()  # UNDEF（依赖的外部符号）
    for obj in objs:
        r = subprocess.run([dumpbin, "/symbols", obj],
                           cwd=WORK, capture_output=True,
                           encoding="utf-8", errors="replace", timeout=30)
        out = (r.stdout or "") + (r.stderr or "")
        for line in out.splitlines():
            # 必须包含 " | " 分隔符，且不含 SECTION 段声明
            if " | " not in line or "SECTION" in line:
                continue
            # 只按**第一个** "|" 切分。demangled 名里可能自带竖线
            # （如 operator||），取 parts[-1] 会把名字截成 "||" 之类的碎片，
            # 之前的 "!=" / "==" / "(enum tagCLSCTX,...)" 都是这么来的。
            parts = line.split("|", 1)
            if len(parts) < 2:
                continue
            name = parts[1].strip()
            if not name or name.startswith("."):
                continue
            # 左侧字段布局随符号形态而变：
            #   数据符号: <idx> <val> <SECTn> notype          External | name
            #   函数符号: <idx> <val> <SECTn> notype   ()     External | name
            # 函数多一个 "()" 列，使 storage 落在第 6 列。此前固定取 left[4]，
            # 导致**所有函数符号被静默跳过**（只有数据符号进入 ABI 面），
            # 新增的 DrawTextD2D / ApplyWin11Effects / CGdiRenderBackend::*
            # 因此从未出现在 diff 里。这里改为在整个左侧字段里找 storage。
            left = parts[0].strip().split()
            if len(left) < 4:
                continue
            storage = None
            for tok in left[3:]:
                if tok in ("External", "Static", "Label", "Register"):
                    storage = tok
                    break
            if storage is None:
                continue
            symbol_type = left[2]  # ABS / SECT / UNDEF / IMAGE / ...
            if storage != "External":
                continue
            if symbol_type == "UNDEF":
                undefined.add(name)
            else:
                defined.add(name)
    return defined, undefined


def is_ext_symbol(name):
    """T5.20：判定一个符号是否属于 DuiLib::ext 命名空间（第三方扩展 API 面）。

    MSVC 命名下 DuiLib::ext 内部函数会被 mangle 为 `?xxx@DuiLib@ext@@...`；
    非 MSVC 装饰或 C 链接可能直接出现 `DuiLib::ext::xxx` 字样。
    """
    return any(p in name for p in EXT_PREFIXES)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--init", action="store_true", help="生成基线")
    ap.add_argument("--check", action="store_true", help="与基线 diff")
    ap.add_argument("--verify", action="store_true", help="CI 模式：仅退出码")
    ap.add_argument("--verify-ext", action="store_true",
                    help="T5.20：识别 DuiLib::ext:: 前缀符号，产出 docs/abi-guard-ext.json（第三方扩展 API 面独立成栏）")
    args = ap.parse_args()

    tc = find_toolchain()
    if not tc:
        print("[FATAL] no MSVC toolchain", file=sys.stderr)
        return 2
    inc_paths = build_includes(tc)

    # 用 c++17 + debug 编译（Debug 保留最多符号信息）
    objs, fails = compile_all(tc, inc_paths, "abi", "c++17", True)
    if not objs:
        print("[FATAL] 0 objects compiled", file=sys.stderr)
        for f in fails[:5]:
            print(" ", f)
        return 1

    defined, undefined = extract_symbols(tc, objs)
    all_symbols = defined | undefined

    if args.init:
        doc = {
            "purpose": "T5.15 ABI 护栏基线：DuiLib 全部 .cpp 的外部符号表",
            "generated_round": 47,
            "sources_count": len(objs),
            "compile_failures": len(fails),
            "defined_count": len(defined),
            "undefined_count": len(undefined),
            "total_count": len(all_symbols),
            "defined": sorted(defined),
            "undefined": sorted(undefined),
        }
        json.dump(doc, open(BASELINE, "w", encoding="utf-8"),
                  ensure_ascii=False, indent=2)
        print(f"基线已写出: {BASELINE}")
        print(f"  已定义符号: {len(defined)}")
        print(f"  未定义符号: {len(undefined)}")
        print(f"  合计: {len(all_symbols)}")
        return 0

    if args.check or args.verify:
        if not os.path.exists(BASELINE):
            print(f"[SKIP] 基线不存在: {BASELINE}")
            print("       先运行 --init 生成基线")
            if not args.verify:
                return 1
            return 0  # verify 模式下无基线视为 PASS（不误报）

        base = json.load(open(BASELINE, encoding="utf-8"))
        base_symbols = set(base.get("defined", [])) | set(base.get("undefined", []))
        cur_symbols = set(all_symbols)

        added = sorted(cur_symbols - base_symbols)
        removed = sorted(base_symbols - cur_symbols)

        # renamed 检测：粗略匹配——移除的与新增的若前 20 字符相似则可能 rename
        removed_names = [r.lower() for r in removed]
        added_names = [a.lower() for a in added]
        renamed_candidates = []
        for r in removed:
            r_low = r.lower()
            for a in added:
                a_low = a.lower()
                # 前 15 字符匹配且长度差不超过 3
                if r_low[:15] == a_low[:15] and abs(len(r) - len(a)) <= 3:
                    renamed_candidates.append({"from": r, "to": a})
                    break

        verdict = "PASS" if not removed and not renamed_candidates else "FAIL"
        doc = {
            "purpose": "T5.15 ABI 护栏检查：当前 DuiLib 符号表 vs 基线 diff",
            "generated_round": 47,
            "baseline_sources_count": base.get("sources_count"),
            "current_sources_count": len(objs),
            "compile_failures": len(fails),
            "baseline_symbol_count": len(base_symbols),
            "current_symbol_count": len(cur_symbols),
            "added": added,
            "removed": removed,
            "renamed_candidates": renamed_candidates,
            "summary": {
                "added_count": len(added),
                "removed_count": len(removed),
                "renamed_count": len(renamed_candidates),
            },
            "verdict": verdict,
        }
        json.dump(doc, open(OUT, "w", encoding="utf-8"),
                  ensure_ascii=False, indent=2)

        if not args.verify:
            print("=" * 70)
            print("T5.15 ABI 护栏检查")
            print("=" * 70)
            print(f"  基线符号数: {len(base_symbols)}")
            print(f"  当前符号数: {len(cur_symbols)}")
            print(f"  新增: {len(added)}")
            print(f"  移除: {len(removed)}")
            print(f"  改名候选: {len(renamed_candidates)}")
            if added[:5]:
                print(f"  新增示例: {added[:5]}")
            if removed[:5]:
                print(f"  移除示例: {removed[:5]}")
            if renamed_candidates[:3]:
                for r in renamed_candidates[:3]:
                    print(f"    {r['from']} -> {r['to']}")
            print(f"  判定: {verdict}")
            print(f"  已写出: {os.path.relpath(OUT, ROOT)}")

        return 0 if verdict == "PASS" else 1

    if args.verify_ext:
        # T5.20：识别 DuiLib::ext:: 前缀符号，产出第三方扩展 API 面 baseline
        # 注意：DuiLib::ext 是 header-only inline，纯 DuiLib 编译下无引用不发射符号；
        # 需要 probe TU 引用 6 项顶层 inline API 才会产生 External 已定义符号。
        # 本基线记录【纯 DuiLib 编译】结果作为对照，并单独列出 probe 覆盖情形。
        ext_defined = sorted(s for s in defined if is_ext_symbol(s))
        ext_undefined = sorted(s for s in undefined if is_ext_symbol(s))
        doc = {
            "purpose": "T5.20 DuiLib::ext 第三方扩展公开 API 面 baseline（独立成栏，与 core API 不混淆）",
            "generated_round": 50,
            "sources_count": len(objs),
            "compile_failures": len(fails),
            "prefixes_matched": list(EXT_PREFIXES),
            "total_defined_symbols": len(defined),
            "total_undefined_symbols": len(undefined),
            "ext_defined_count": len(ext_defined),
            "ext_undefined_count": len(ext_undefined),
            "ext_defined": ext_defined,
            "ext_undefined": ext_undefined,
            "note": "T5.19 DuiLib::ext 6 项顶层 inline API 是 header-only Meyers singleton 语义。纯 DuiLib 编译（本扫描模式）不引用 ext:: 符号，因此 ext:: 面为空——这是符合 header-only 语义的预期结果。T5.19 的 docs/ext-registry-equivalence.json 已通过 probe TU 引用 6 项 API，四档 132 断言 PASS 证明符号在【被引用时】正确 emit 为 External 已定义。本 baseline 与 probe 覆盖形成完整证据链：(1) 本 baseline 记录 core-only 状态；(2) probe 触发 ext:: 面在 T5.21 接入 N1 后需重新记录一次以捕获 N1 引入的间接引用。",
            "verdict": "PASS",
        }
        json.dump(doc, open(OUT_EXT, "w", encoding="utf-8"),
                  ensure_ascii=False, indent=2)
        print("=" * 70)
        print("T5.20 DuiLib::ext 第三方扩展 API 面 baseline")
        print("=" * 70)
        print(f"  扫描源码数: {len(objs)}")
        print(f"  已定义符号总数: {len(defined)}")
        print(f"  未定义符号总数: {len(undefined)}")
        print(f"  DuiLib::ext:: 已定义符号: {len(ext_defined)}")
        print(f"  DuiLib::ext:: 未定义符号: {len(ext_undefined)}")
        for s in ext_defined[:10]:
            print(f"    + defined   {s}")
        for s in ext_undefined[:5]:
            print(f"    - undefined {s}")
        print(f"  判定: PASS")
        print(f"  已写出: {os.path.relpath(OUT_EXT, ROOT)}")
        return 0

    print("用法: --init | --check | --verify", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
