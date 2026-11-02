"""门禁：皮肤里带 {tok:*} 底色的可见容器必须有 name。

为什么需要它
-----------
切换主题时，皮肤层的颜色靠 SkinTokenReplayTable **按控件名**重设。
没有 name 的容器无法被定位，它画的底就永远停在建树时那一套主题的颜色上。

这不是理论风险，是实测缺陷：
    page_cleanup.xml 右侧说明面板
        <VerticalLayout width="330" bkcolor="{tok:Surface}" ...>   <- 没有 name
    浅色主题下这块区域 bbox x 1665..2238 y 336..1404 全是 RGB(32,32,32)
    （深色 Surface），占整页 7.9%，压在上面的深色文字直接看不见 ——
    用户报的"浅色下字体显示不全"就是这个。
    补上 name="cln_desc_panel" 并重新生成重放表后降到 0.0%。

为什么允许一部分匿名元素
-----------------------
纯装饰性的 1px 分隔条（<Control width="1" bkcolor="{tok:BorderSoft}"/>）
漏刷新的视觉后果是"分隔线颜色略深"，不造成文字不可读，而且数量多、
逐个起名会污染命名空间。它们进 allow-list，但**必须显式登记**，
并在输出里报数，避免 allow-list 悄悄覆盖掉真正的缺陷。

判据
----
1) 带 {tok:*} 的 bkcolor / bordercolor 元素，若控件宽高都 > 1 且没有 name
   -> 违规。
2) 1px 分隔条 -> 计入 allow-list 计数（不违规，但报数）。
3) 例外：显式写了 <!-- COLREPLAY-OK: 理由 --> 的行跳过。

退出码：0 干净，1 发现违规，2 输入缺失，3 --inject 未报出。
"""
from __future__ import annotations

import argparse
import glob
import io
import os
import re
import sys

ROOT = r"D:\DuiLib_Ultimate-master"
SKIN = os.path.join(ROOT, "bin64", "skin", "Win8Metro")

# 会决定"一大块底"的属性。文字色不影响整块残留，不在此列。
BG_TOK_ATTRS = ("bkcolor", "bordercolor")

TOK = re.compile(r"^\{tok:([A-Za-z]+)\}$")
TAG_RE = re.compile(r"<(\w+)((?:\s+\w+=\"[^\"]*\")*)\s*/?>")


def scan(inject=False):
    files = sorted(glob.glob(os.path.join(SKIN, "**", "*.xml"), recursive=True))
    if not files:
        return None, "skin dir has no xml: " + SKIN

    violations = []
    hover_as_surface = []
    separators = 0
    named = 0

    for f in files:
        rel = os.path.relpath(f, SKIN)
        text = io.open(f, encoding="utf-8", errors="replace").read()
        for ln, line in enumerate(text.splitlines(), 1):
            if "COLREPLAY-OK" in line:
                continue
            for m in TAG_RE.finditer(line):
                tag, body = m.group(1), m.group(2)
                attrs = dict(re.findall(r'(\w+)="([^"]*)"', body))

                tok_bg = None
                for a in BG_TOK_ATTRS:
                    tm = TOK.match(attrs.get(a, "").strip())
                    if tm:
                        tok_bg = (a, tm.group(1))
                        break
                if not tok_bg:
                    continue

                # [HOVER-AS-SURFACE] 悬停色不得当作常驻底色。
                #   实测 5 处工具条容器写 bkcolor="{tok:SurfaceHover}"：
                #   inc_task_panel / page_assoc / page_driver / page_startup / page_update。
                #   语义上 SurfaceHover 是"鼠标移上去"的瞬时态，用作常驻底会让
                #   整条工具条在浅色下仍是深色 (51,51,51) —— 实测浅色页里有 32 个
                #   40x40 块残留就是这么来的，压在上面的文字读不出来。
                #   修法：改用 SurfaceAlt（卡片/工具条语义）。
                # [STATE-AS-SURFACE] 只有 SurfaceHover 属于"纯瞬时态"，不得当常驻底。
                #   SurfaceSunk 不在此列：它是输入框/凹陷区语义，搜索栏用它是对的
                #   （实测 6 处 page_control/page_intl/page_regclean/page_startup/
                #    page_tools/inc_task_panel 的 30px 工具条，语义正确）。
                #   Accent 系列同理可用于强调容器，不拦。
                if tag in ("HorizontalLayout", "VerticalLayout") and                         attrs.get("bkcolor", "").strip() == "{tok:SurfaceHover}":
                    hover_as_surface.append(
                        (rel, ln, tag, attrs.get("name", "(none)"),
                         line.strip()[:100]))
                    continue

                if "name" in attrs:
                    named += 1
                    continue

                # 匿名：区分"整块底"与"1px 分隔条"
                def dim(k):
                    v = attrs.get(k)
                    return int(v) if v and v.isdigit() else None

                w, h = dim("width"), dim("height")
                is_sep = (tag == "Control" and
                          ((w is not None and w <= 1) or
                           (h is not None and h <= 1)))
                if is_sep:
                    separators += 1
                    continue

                violations.append((rel, ln, tag, tok_bg[0], tok_bg[1],
                                   w, h, line.strip()[:100]))

    if inject:
        violations.append(("<injected>", 0, "VerticalLayout", "bkcolor",
                           "Surface", 400, 300, "planted anonymous container"))

    stats = {"files": len(files), "named": named,
             "separators": separators, "violations": len(violations)}
    return violations, hover_as_surface, stats


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--inject", action="store_true",
                    help="plant an anonymous container; the gate must report it")
    args = ap.parse_args()

    res, hover_bad, stats = scan(inject=args.inject)
    if res is None:
        print("[FAIL] " + stats)
        return 2

    print("[INFO] skin files=%d  named {tok:*} bg=%d  "
          "1px separators (allow-listed)=%d  violations=%d"
          % (stats["files"], stats["named"], stats["separators"],
             stats["violations"]))
    print("[NOTE] 1px separators are skipped on purpose: an unrefreshed divider "
          "only shifts a hairline colour and cannot hide text.")

    if hover_bad:
        print("[FAIL] %d container(s) use SurfaceHover as a resting background:"
              % len(hover_bad))
        for (rel, ln, tag, nm, src) in hover_bad:
            print("       %-26s:%-4d <%s name=\"%s\">" % (rel, ln, tag, nm))
            print("              %s" % src)
        print("       Why: SurfaceHover is the mouse-over state. Using it as a")
        print("       standing background leaves the strip dark in the light")
        print("       theme -- measured 32 of 40x40 blocks across 5 pages, with")
        print("       the label text on it unreadable.")
        print("       Fix: use SurfaceAlt (card / toolbar semantics).")

    if hover_bad or res:
        pass
    if res:
        print("[FAIL] %d visible container(s) use {tok:*} but have no name:"
              % len(res))
        for (rel, ln, tag, attr, tok, w, h, src) in res:
            print("       %-26s:%-4d <%s %s={tok:%s} w=%s h=%s>"
                  % (rel, ln, tag, attr, tok, w, h))
            print("              %s" % src)
        print("       Why it matters: SkinTokenReplayTable addresses colours by")
        print("       control name, so an unnamed container keeps the previous")
        print("       theme's fill. Measured: the cleanup page's description panel")
        print("       stayed RGB(32,32,32) in the light theme over 7.9% of the")
        print("       page, hiding every label drawn on it.")
        print("       Fix: add name=\"...\" and re-run")
        print("            python tools/mig/gen_skin_token_table.py")
        if args.inject:
            return 1
        return 1

    if args.inject:
        print("[FAIL] inject: planted container was not reported -- gate is blind")
        return 3

    if hover_bad:
        return 1

    print("[PASS] every visible {tok:*} container is nameable by the replay table, "
          "and no hover colour is used as a resting background")
    return 0


if __name__ == "__main__":
    sys.exit(main())
