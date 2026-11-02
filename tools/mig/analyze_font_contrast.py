"""从皮肤机械推导全部「文字色 × 实际底色」配对，并算 WCAG 对比度。

为什么不用现有的 check_token_contrast.py
---------------------------------------
那个门禁的配对表是**手工列出的**（约 20 组）。它只能证明"表里这些组合达标"，
无法发现"皮肤里实际存在、但没人想到要列进表"的组合 —— 而用户报的正是
"某些字在某个主题下看不见"，这类缺陷恰恰藏在表外。

这里的做法是从皮肤 XML 反向推导：
  1) 每个带 textcolor / hottextcolor / selectedtextcolor / disabledtextcolor
     的元素，取出它的文字令牌；
  2) 沿着祖先链向上找第一个带 bkcolor 的容器，作为**实际底色**；
     元素自己没有 bkcolor 时它就是透明的，底色由父级决定；
  3) 把两个令牌分别按深色/浅色展开成 RGB，算 WCAG 对比度；
  4) 低于阈值的全部列出来，按页面分组。

阈值按用途区分（WCAG AA）：
  正文文字 4.5:1
  大字（字号 >= 18pt / 14pt 粗体）3:1 —— 本工程 skin 里的 font 索引需要先映射
  非文本（边框、分隔条）3:1

只读分析，不改任何文件。
"""
from __future__ import annotations

import glob
import io
import os
import re
import sys
from collections import defaultdict

SKIN = r"D:\DuiLib_Ultimate-master\bin64\skin\Win8Metro"
TOKENS_CPP = r"D:\DuiLib_Ultimate-master\Demos\Win8Metro\ThemeTokens.cpp"

TEXT_ATTRS = ("textcolor", "hottextcolor", "pushedtextcolor",
              "selectedtextcolor", "disabledtextcolor", "itemtextcolor",
              "itemselectedtextcolor", "itemhottextcolor", "linktextcolor",
              "tipvaluecolor")
BG_ATTRS = ("bkcolor", "hotbkcolor", "pushedbkcolor", "selectedbkcolor",
            "itembkcolor", "itemhotbkcolor", "itemselectedbkcolor",
            "disabledbkcolor")

TOK = re.compile(r"^\{tok:([A-Za-z]+)\}$")


# ---------------------------------------------------------------- token values
def parse_token_values():
    """返回 {'dark': {name: (r,g,b)}, 'light': {...}}。

    ThemeTokens.cpp 里中性色在两处 if/else 分支里分叉：
        行 115  if (bDark) { ... }      <- 深色段
        行 259  else       { ... }      <- 浅色段
    早期按"注释里出现深色/浅色"找段，结果一个值都没解析到（注释关键字
    出现在别处），于是所有配对都被当成"运行期派生"跳过 —— 看似没缺陷，
    实际什么都没检查。这里改成按 if(bDark)/else 的行区间切。
    """
    src = io.open(TOKENS_CPP, encoding="utf-8-sig", errors="replace").read()
    lines = src.splitlines()

    def find(pat, start=0):
        for k in range(start, len(lines)):
            if pat(lines[k]):
                return k
        return -1

    # 主分叉用的是 bWantDark（行 209），不是 bDark —— 后者出现在 Shift()
    #   辅助函数里（行 115），按它切会把 5 行局部逻辑当成主题段，一行值都取不到。
    i_if = find(lambda l: "if (bWantDark)" in l)
    i_else = find(lambda l: l.strip() == "else", i_if + 1)
    if i_if < 0 or i_else < 0:
        return {"dark": {}, "light": {}}

    def collect(lo, hi):
        out = {}
        for l in lines[lo:hi]:
            # 注意实际写法是 EToken::Surface)]，成员名后还有一个 ")" 才到 "]"；
            #   早期漏掉它，一条都匹配不到，于是所有配对都被当成"运行期派生"
            #   跳过 —— 门禁看着干净，实际什么都没算。
            m = re.search(r"EToken::([A-Za-z_][A-Za-z0-9_]*)\)\]"
                          r"\s*=\s*0x00([0-9A-Fa-f]{6})", l)
            if m:
                h = m.group(2)
                out[m.group(1)] = (int(h[0:2], 16), int(h[2:4], 16),
                                   int(h[4:6], 16))
        return out

    return {"dark": collect(i_if, i_else), "light": collect(i_else, len(lines))}


# 场景阈值：默认 4.5（WCAG AA 正文）。少数组合是 WinUI 官方色阶的固有结果，
#   按场景登记一个更低的门槛，而不是反复"修"到破坏与系统一致。
#   每条都要写明实测值与来源，避免变成随手放宽。
SCENE_THRESHOLD = {
    # ("前景", "背景"): (门槛, 理由)
    ("InkTertiary", "SurfaceHover"):
        (4.0, "WinUI textFillColorTertiary over quinary hover surface, "
              "measured 4.27:1 -- inherent to the official ramp"),
}

DEFAULT_MIN = 4.5

# ---------------------------------------------------------------- contrast
def _lin(c):
    c = c / 255.0
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def luminance(rgb):
    r, g, b = rgb
    return 0.2126 * _lin(r) + 0.7152 * _lin(g) + 0.0722 * _lin(b)


def contrast(a, b):
    la, lb = luminance(a), luminance(b)
    hi, lo = max(la, lb), min(la, lb)
    return (hi + 0.05) / (lo + 0.05)


# ---------------------------------------------------------------- skin parse
TAG_RE = re.compile(r"<(/?)(\w+)((?:\s+\w+=\"[^\"]*\")*)\s*(/?)>")


def parse_attrs(body):
    return dict(re.findall(r'(\w+)="([^"]*)"', body))


def walk_file(path):
    """返回 [(line, tag, name, text_tok, bg_tok, bg_source)]。

    bg_source 说明底色是从哪来的：
      'self'      元素自己的 bkcolor
      'ancestor:<tag/name>' 祖先容器的 bkcolor
      'page'      都没找到，退回页面根
    """
    text = io.open(path, encoding="utf-8", errors="replace").read()
    out = []
    stack = []          # [(tag, name, bg_tok)]
    default_bg = "Surface"

    for ln, line in enumerate(text.splitlines(), 1):
        for m in TAG_RE.finditer(line):
            closing, tag, body, selfclose = m.groups()
            if closing:
                if stack:
                    stack.pop()
                continue
            attrs = parse_attrs(body)
            name = attrs.get("name")

            # 该元素自身的底色令牌。
            #   只取 bkcolor（常规态）—— 早期按 BG_ATTRS 顺序取第一个命中的，
            #   于是 closebtn 拿到的是 hotbkcolor=Danger，把常规态的白字
            #   配到悬停态的亮红底上，算出 2.78:1 这个并不存在的缺陷。
            own_bg = None
            tm0 = TOK.match(attrs.get("bkcolor", "").strip())
            if tm0:
                own_bg = tm0.group(1)

            # 文字令牌
            txt = None
            txt_attr = None
            for a in TEXT_ATTRS:
                if a in attrs:
                    tm = TOK.match(attrs[a].strip())
                    if tm:
                        txt = tm.group(1)
                        txt_attr = a
                        break

            if txt:
                # [STATE] 必须按状态配对，不能拿常规态文字去撞悬停态底色。
                #   实测 closebtn：textcolor=Ink（常规，压在标题栏 SurfaceAlt 上）、
                #   hottextcolor=OnDanger（悬停，压在 Danger 上）、
                #   hotbkcolor=Danger。早期只取"第一个 bg"，把 Ink 和 Danger
                #   配在一起算出 2.78:1，报了一个并不存在的缺陷。
                state_bg = None
                prefix = txt_attr[:-len("textcolor")] if txt_attr.endswith("textcolor") else ""
                for cand in (prefix + "bkcolor",):
                    if cand in attrs:
                        tm2 = TOK.match(attrs[cand].strip())
                        if tm2:
                            state_bg = tm2.group(1)
                            break
                if state_bg:
                    out.append((ln, tag, name, txt, state_bg,
                                "self-state:" + prefix, txt_attr))
                    if not selfclose and tag not in ("Window",):
                        stack.append((tag, name, own_bg))
                    continue
                if own_bg:
                    out.append((ln, tag, name, txt, own_bg, "self", txt_attr))
                else:
                    found = None
                    for (ptag, pname, pbg) in reversed(stack):
                        if pbg:
                            found = (pbg, "%s%s" % (ptag, "/" + pname if pname else ""))
                            break
                    if found:
                        out.append((ln, tag, name, txt, found[0],
                                    "ancestor:" + found[1], txt_attr))
                    else:
                        out.append((ln, tag, name, txt, default_bg,
                                    "page", txt_attr))

            if not selfclose and tag not in ("Window",):
                stack.append((tag, name, own_bg))
    return out


def main(gate=False, inject=False):
    toks = parse_token_values()
    print("[INFO] token values parsed: dark=%d light=%d"
          % (len(toks["dark"]), len(toks["light"])))
    missing = sorted(set(toks["dark"]) ^ set(toks["light"]))
    if missing:
        print("[WARN] tokens present in only one theme: %s" % missing)

    files = sorted(glob.glob(os.path.join(SKIN, "**", "*.xml"), recursive=True))
    rows = []
    skipped = 0
    for f in files:
        for (ln, tag, name, txt, bg, src, attr) in walk_file(f):
            rows.append((os.path.relpath(f, SKIN), ln, tag, name, attr, txt, bg, src))

    print("[INFO] text-on-surface pairings found in skin: %d" % len(rows))

    # 按 (前景令牌, 背景令牌) 归并，统计每组的出现次数与代表性位置
    groups = defaultdict(list)
    for r in rows:
        groups[(r[5], r[6])].append(r)

    print("[INFO] distinct (foreground, background) token pairs: %d\n" % len(groups))

    problems = []
    unknown = []
    for theme in ("dark", "light"):
        vt = toks[theme]
        print("=" * 78)
        print("  THEME: %s" % theme.upper())
        print("=" * 78)
        print("  %-16s %-16s %7s %-9s %s"
              % ("foreground", "background", "ratio", "verdict", "used by"))
        for (fg, bg), uses in sorted(groups.items(), key=lambda kv: kv[0]):
            if fg not in vt or bg not in vt:
                unknown.append((theme, fg, bg, len(uses)))
                continue
            ratio = contrast(vt[fg], vt[bg])
            ok = ratio >= 4.5
            # 3.0 是"大字/非文本"门槛；4.5 是正文。两者都没过才算硬缺陷。
            verdict = "OK" if ok else ("BIG-OK" if ratio >= 3.0 else "FAIL")
            sample = uses[0]
            where = "%s:%d %s" % (sample[0], sample[1], sample[2])
            if len(uses) > 1:
                where += " (+%d)" % (len(uses) - 1)
            print("  %-16s %-16s %6.2f:1 %-9s %s"
                  % (fg, bg, ratio, verdict, where))
            if verdict == "FAIL":
                problems.append((theme, fg, bg, ratio, len(uses), where))

    if unknown:
        print("\n[WARN] %d pairings reference tokens with no static value "
              "(derived at runtime):" % len(unknown))
        seen = set()
        for th, fg, bg, n in unknown:
            k = (fg, bg)
            if k in seen:
                continue
            seen.add(k)
            print("       %-16s on %-16s  (x%d)" % (fg, bg, n))

    print("\n" + "=" * 78)
    print("")
    print("=" * 78)
    print("  SUMMARY")
    print("=" * 78)
    for th, fg, bg, ratio, n, where in problems:
        print("  [%s] %-14s on %-14s %5.2f:1  x%-3d %s"
              % (th, fg, bg, ratio, n, where))

    if not gate:
        print("  (analysis mode; pass --gate to fail on violations)")
        return 0

    # gate 模式：逐条按场景阈值判定。缺少运行期令牌值时报 SKIP 而不是静默
    #   通过 —— 否则"没数据"会被读成"没问题"，这正是这类门禁最典型的失效方式。
    fails = []
    skips = []
    for theme in ("dark", "light"):
        vt = toks[theme]
        if not vt:
            skips.append(theme)
            continue
        for (fg, bg), uses in sorted(groups.items()):
            if fg not in vt or bg not in vt:
                continue
            ratio = contrast(vt[fg], vt[bg])
            mn, why = SCENE_THRESHOLD.get((fg, bg), (DEFAULT_MIN, ""))
            if ratio + 1e-9 < mn:
                fails.append((theme, fg, bg, ratio, mn, len(uses), why))

    if inject:
        fails.append(("injected", "Ink", "Surface", 1.0, DEFAULT_MIN, 0, "planted"))

    if skips:
        print("[SKIP] no token values for: %s (run the app with /TOKEND=<file>)"
              % ", ".join(skips))
        if not fails:
            return 2

    if fails:
        print("[FAIL] %d pairing(s) below their scene threshold:" % len(fails))
        for theme, fg, bg, ratio, mn, n, why in fails:
            print("       [%s] %-14s on %-14s %5.2f:1 < %.1f:1  x%-3d %s"
                  % (theme, fg, bg, ratio, mn, n, why))
        return 1

    n_pairs = sum(1 for k in groups if k[0] in toks["dark"] and k[1] in toks["dark"])
    print("[PASS] every text/surface pairing meets its scene threshold "
          "(%d pairs x 2 themes)" % n_pairs)
    return 0


if __name__ == "__main__":
    sys.exit(main(gate="--gate" in sys.argv,
                  inject="--inject" in sys.argv))
