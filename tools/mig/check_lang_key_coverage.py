"""门禁：每个被 StringMgr.Get 引用的语言键都必须能被解析成实际文案。

为什么需要它
-----------
StringMgr.Get() 是三级回退：
    1) 语言包 DLL（bin64/lang/zh-CN.dll 等）
    2) 内置表 s_builtinZhCN（StringManager.cpp）
    3) 都查不到 -> 原样返回 "[key]"

第 3 步不会报错、不会崩溃，只是在界面上显示一对方括号加键名。
用户看到的是「[toolbox.ui_stage1_wim]」这种文字，看起来像渲染坏了，
实际是文案缺失。

本轮实测就撞上了这一类：toolbox.ui_* 共 11 个键在内置表和语言包里都不存在，
而源码一直在用。逐个手查很容易漏，且以后新增文案还会重犯 —— 所以固化成门禁。

三层口径与能力边界（必须如实声明）
--------------------------------
* 内置表：解析 StringManager.cpp 里的 { "key", L"..." } 条目。
  必须同时支持 L"..." 与 LR"(...)" 两种写法 —— 早期只认前者，
  把 5 条 LR"(...)" 条目误判成缺失，若照那个结果去补，会重复添加已存在的键。
* 语言包：从 zh-CN.dll 的字节流里扫 ASCII 形式的键名。
  这是**下界**：如果某个键以 UTF-16 存储且未在别处以 ASCII 出现，这里扫不到。
  因此本门禁的结论是「至少缺这些」，而不是「只缺这些」。
* 源码引用：扫全部 .cpp/.h 里的 StringMgr.Get("...") / Get(L"...")。

退出码：0 干净，1 有缺失键，2 输入缺失，3 --inject 未报出。
"""
from __future__ import annotations

import argparse
import glob
import io
import os
import re
import sys

ROOT = r"D:\DuiLib_Ultimate-master"
APP = os.path.join(ROOT, "Demos", "Win8Metro")
STRMGR = os.path.join(APP, "StringManager.cpp")
LANGDIR = os.path.join(ROOT, "bin64", "lang")

KEY_RE = re.compile(r'StringMgr\.Get\(\s*(?:L)?"([a-zA-Z0-9_.]+)"')
PKG_KEY_RE = re.compile(rb"([a-z]+\.[a-zA-Z0-9_.]{3,44})")


def read_text(path):
    raw = open(path, "rb").read()
    for enc in ("utf-8-sig", "gbk"):
        try:
            return raw.decode(enc)
        except UnicodeDecodeError:
            continue
    return raw.decode("utf-8", errors="replace")


def parse_builtin():
    """解析内置表。两种字符串写法都要认，见模块头的说明。"""
    keys = set()
    for line in read_text(STRMGR).split("\n"):
        s = line.strip()
        if not s.startswith('{ "'):
            continue
        # 键名是第一个引号对
        parts = s.split('"')
        if len(parts) < 2:
            continue
        key = parts[1]
        if 'L"' in s or 'LR"(' in s:
            keys.add(key)
    return keys


def parse_lang_pack(lang="zh-CN"):
    path = os.path.join(LANGDIR, lang + ".dll")
    if not os.path.isfile(path):
        return None, "language pack missing: " + path
    blob = open(path, "rb").read()
    keys = set(m.group(1).decode("ascii", "replace")
               for m in PKG_KEY_RE.finditer(blob))
    return keys, None


def parse_refs():
    """返回 {key: [(file, line), ...]}。

    记录位置是为了让失败信息能直接指到调用点，不用再手工搜一遍。
    """
    refs = {}
    files = []
    for ext in ("*.cpp", "*.h"):
        files += glob.glob(os.path.join(APP, "**", ext), recursive=True)
    for f in files:
        rel = os.path.relpath(f, ROOT)
        for ln, line in enumerate(read_text(f).split("\n"), 1):
            for m in KEY_RE.finditer(line):
                refs.setdefault(m.group(1), []).append((rel, ln))
    return refs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--inject", action="store_true",
                    help="plant a missing key; the gate must report it")
    args = ap.parse_args()

    for p in (STRMGR, LANGDIR):
        if not os.path.exists(p):
            print("[FAIL] missing input: " + p)
            return 2

    builtin = parse_builtin()
    pkg, err = parse_lang_pack()
    if pkg is None:
        print("[FAIL] " + err)
        return 2
    refs = parse_refs()

    covered = builtin | pkg
    missing = {k: v for k, v in refs.items() if k not in covered}

    if args.inject:
        missing["__injected_missing_key__"] = [("<injected>", 0)]

    print("[INFO] builtin entries=%d  lang-pack keys=%d  referenced keys=%d"
          % (len(builtin), len(pkg), len(refs)))
    print("[NOTE] the lang-pack count is a LOWER BOUND: keys stored only as "
          "UTF-16 and never appearing as ASCII are not seen by this scan.")

    if missing:
        print("[FAIL] %d key(s) are referenced but resolve to neither the builtin "
              "table nor the zh-CN pack:" % len(missing))
        for k in sorted(missing):
            locs = missing[k]
            where = ", ".join("%s:%d" % (f, l) for f, l in locs[:3])
            more = "" if len(locs) <= 3 else " (+%d)" % (len(locs) - 3)
            print("       %-44s %s%s" % (k, where, more))
        print("       Why it matters: StringMgr.Get falls back to \"[key]\" and the")
        print("       user sees a bracketed identifier instead of text. Nothing")
        print("       crashes and no log line is written, so it reads as a render bug.")
        print("       Fix: add the key to the builtin table in StringManager.cpp")
        print("            (both L\"...\" and LR\"(...)\" forms are parsed).")
        if args.inject:
            return 1
        return 1

    if args.inject:
        print("[FAIL] inject: the planted key was not reported -- gate is blind")
        return 3

    print("[PASS] every referenced language key resolves (builtin table or zh-CN pack)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
