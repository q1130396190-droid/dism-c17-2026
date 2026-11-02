#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""间距 / 内边距 / 对齐 属性的机械统计器（只读，不改皮肤）。

扫描 bin64/skin/**/*.xml，按「属性 x 页面 x 取值」统计：
  inset / childpadding / textpadding / align / valign / childvalign
  以及 Button / Text 这类固定尺寸控件的 height / width（排除 width=0 可伸缩项）

提取方式：先按行保留地清空 <!-- ... --> 注释（多行注释不影响行号），
再用带引号感知的 `<` ... `>` 扫描器切标签，最后用正则抽属性。
因此可以正确处理：含逗号的取值（inset="14,6,14,0"）、自闭合标签、
跨行书写的标签与跨行注释。

输出：stdout + tools/mig/layout_stats/spacing_report.txt
"""
from __future__ import annotations

import collections
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parents[3]
SKIN = ROOT / "bin64" / "skin"
OUT = pathlib.Path(__file__).resolve().parent / "spacing_report.txt"

SPACING_ATTRS = ("inset", "childpadding", "textpadding")
ALIGN_ATTRS = ("align", "valign", "childvalign")
FIXED_TAGS = ("Button", "Text")          # 只统计这类固定尺寸控件的 height/width
FIXED_SIZE_ATTRS = ("height", "width")

COMMENT_RE = re.compile(r"<!--.*?-->", re.S)
TAGNAME_RE = re.compile(r"<([A-Za-z_][\w.:-]*)")
ATTR_RE = re.compile(r"([A-Za-z_][\w.:-]*)\s*=\s*\"([^\"]*)\"")
ATTR_RE_SQ = re.compile(r"([A-Za-z_][\w.:-]*)\s*=\s*'([^']*)'")


def strip_comments(text: str) -> str:
    """把注释替换成等长空白（保留换行），使行号与原文一致。"""
    def blank(m: re.Match) -> str:
        return re.sub(r"[^\n]", " ", m.group(0))
    return COMMENT_RE.sub(blank, text)


def iter_tags(text: str):
    """产出 (line, raw_tag)。raw_tag 从 '<' 到匹配的 '>'（引号内的 '>' 不算结束）。"""
    i, n = 0, len(text)
    while True:
        lt = text.find("<", i)
        if lt < 0:
            return
        j = lt + 1
        quote = None
        while j < n:
            ch = text[j]
            if quote:
                if ch == quote:
                    quote = None
            elif ch in "\"'":
                quote = ch
            elif ch == ">":
                break
            j += 1
        yield text.count("\n", 0, lt) + 1, text[lt:j + 1]
        i = j + 1


class Node:
    __slots__ = ("tag", "attrs", "line", "depth", "parent", "children", "page")

    def __init__(self, tag, attrs, line, depth, parent, page):
        self.tag = tag
        self.attrs = attrs
        self.line = line
        self.depth = depth
        self.parent = parent
        self.children = []
        self.page = page

    @property
    def name(self):
        return self.attrs.get("name", "")


def parse_file(path: pathlib.Path):
    raw = path.read_text(encoding="utf-8-sig")
    text = strip_comments(raw)
    page = path.stem
    roots, stack = [], []
    for line, raw_tag in iter_tags(text):
        if raw_tag.startswith("<?") or raw_tag.startswith("<!"):
            continue
        if raw_tag.startswith("</"):
            if stack:
                stack.pop()
            continue
        m = TAGNAME_RE.match(raw_tag)
        if not m:
            continue
        body = raw_tag[1:-1]
        self_closing = body.rstrip().endswith("/")
        if self_closing:
            body = body.rstrip()[:-1]
        attrs = {}
        for key, val in ATTR_RE.findall(body):
            attrs[key] = val
        for key, val in ATTR_RE_SQ.findall(body):
            attrs.setdefault(key, val)
        node = Node(m.group(1), attrs, line, len(stack), stack[-1] if stack else None, page)
        (stack[-1].children if stack else roots).append(node)
        if not self_closing:
            stack.append(node)
    return page, roots, len(raw)


def walk(nodes):
    for n in nodes:
        yield n
        yield from walk(n.children)


def main() -> int:
    files = sorted(SKIN.rglob("*.xml"))
    pages = {}
    for f in files:
        page, roots, nbytes = parse_file(f)
        pages[page] = (f, roots, nbytes)

    lines = []

    def emit(s=""):
        lines.append(s)

    # ---------- 0. 概览 ----------
    all_nodes = []
    for page, (_f, roots, _b) in pages.items():
        all_nodes.extend(walk(roots))

    total_elems = len(all_nodes)
    counts = collections.Counter()
    for n in all_nodes:
        for a in SPACING_ATTRS + ALIGN_ATTRS:
            if a in n.attrs:
                counts[a] += 1

    fixed_nodes = [n for n in all_nodes
                   if n.tag in FIXED_TAGS and n.attrs.get("width", "") != "0"]
    for a in FIXED_SIZE_ATTRS:
        counts[a] = sum(1 for n in fixed_nodes if a in n.attrs)

    emit("=" * 78)
    emit("间距 / 内边距 / 对齐 机械统计  (bin64/skin)")
    emit("=" * 78)
    emit(f"XML 文件数          : {len(files)}")
    emit(f"元素标签总数        : {total_elems}")
    emit(f"Button/Text 固定尺寸项(排除 width=0): {len(fixed_nodes)}")
    emit("")
    emit("属性出现次数（该属性在多少标签上显式出现）:")
    for a in SPACING_ATTRS + ALIGN_ATTRS + FIXED_SIZE_ATTRS:
        distinct = len({n.attrs[a] for n in (fixed_nodes if a in FIXED_SIZE_ATTRS else all_nodes)
                        if a in n.attrs})
        emit(f"  {a:<14} {counts[a]:>5} 次   {distinct:>4} 种取值")
    emit("")
    emit("每文件元素数:")
    for page in sorted(pages):
        emit(f"  {page:<22} {sum(1 for _ in walk(pages[page][1])):>4} 元素")

    # ---------- 1. 取值分布 ----------
    def dist(nodes, attr):
        per = collections.defaultdict(collections.Counter)
        for n in nodes:
            if attr in n.attrs:
                per[n.attrs[attr]][n.page] += 1
        return per

    def emit_dist(title, nodes, attr):
        per = dist(nodes, attr)
        emit("")
        emit(f"--- {title}  (取值 {len(per)} 种, 出现 {sum(sum(c.values()) for c in per.values())} 次) ---")
        rows = sorted(per.items(), key=lambda kv: (-sum(kv[1].values()), kv[0]))
        for value, pages_c in rows:
            total = sum(pages_c.values())
            pages_s = ", ".join(f"{p}({c})" for p, c in sorted(pages_c.items()))
            emit(f"  {value!s:<24} x{total:<3} [{len(pages_c)} 页] {pages_s}")

    emit("")
    emit("=" * 78)
    emit("1. 取值分布（值 -> 次数 -> 涉及页面，括号内为该页出现次数）")
    emit("=" * 78)
    for a in SPACING_ATTRS:
        emit_dist(f"{a}", all_nodes, a)
    for a in ALIGN_ATTRS:
        emit_dist(f"{a}", all_nodes, a)
    for a in FIXED_SIZE_ATTRS:
        emit_dist(f"{a}（仅 Button/Text 且 width!=0）", fixed_nodes, a)

    # ---------- 2. 每页取值 ----------
    emit("")
    emit("=" * 78)
    emit("2. 每页取值分布（同一属性在一页内部的多样性）")
    emit("=" * 78)
    for page in sorted(pages):
        roots = pages[page][1]
        nodes = list(walk(roots))
        emit("")
        emit(f"# {page}")
        for a in SPACING_ATTRS:
            c = collections.Counter(n.attrs[a] for n in nodes if a in n.attrs)
            if c:
                emit(f"  {a:<13} " + "  ".join(f"{v}x{n}" for v, n in sorted(c.items())))
        for a in ALIGN_ATTRS:
            c = collections.Counter(n.attrs[a] for n in nodes if a in n.attrs)
            if c:
                emit(f"  {a:<13} " + "  ".join(f"{v}x{n}" for v, n in sorted(c.items())))

    # ---------- 3. 语义角色（启发式） ----------
    emit("")
    emit("=" * 78)
    emit("3. 语义角色推断（启发式，规则见各条注释）")
    emit("=" * 78)

    def role_of(n):
        roles = []
        if n.depth == 1:
            roles.append("page_root")          # <Window> 的直接子节点 = 页面根容器
        bk = n.attrs.get("bkcolor", "")
        has_hdr_child = any(c.name.startswith("hdr_") for c in n.children)
        if "HeaderBand" in bk or has_hdr_child:
            roles.append("header_band")        # 表头带
        if n.tag == "HorizontalLayout" and \
                sum(1 for c in n.children if c.tag in ("Button", "Edit", "ComboBox", "Option")) >= 2:
            roles.append("action_bar")         # 一行里 >=2 个可点控件
        if n.attrs.get("vscrollbar") == "true" or "list" in n.name or "tree" in n.name:
            roles.append("list_body")          # 可滚动列表/树
        if "inset" in n.attrs and n.depth >= 2 and ("bordercolor" in n.attrs or "borderround" in n.attrs):
            roles.append("card")               # 带边框/圆角的卡片容器
        return roles

    role_data = collections.defaultdict(lambda: collections.defaultdict(collections.Counter))
    role_pages = collections.defaultdict(lambda: collections.defaultdict(collections.Counter))
    for n in all_nodes:
        for r in role_of(n):
            sig = (n.tag,
                   n.attrs.get("inset", "-"),
                   n.attrs.get("childpadding", "-"),
                   n.attrs.get("height", "-"),
                   n.attrs.get("align", "-"),
                   n.attrs.get("valign", "-"))
            role_data[r][sig][n.page] += 1

    for role in ("page_root", "header_band", "action_bar", "list_body", "card"):
        sigs = role_data.get(role, {})
        emit("")
        emit(f"## {role}  ({len(sigs)} 种签名, 共 {sum(sum(c.values()) for c in sigs.values())} 个节点)")
        emit("   tag | inset | childpadding | height | align | valign")
        for sig, pc in sorted(sigs.items(), key=lambda kv: (-sum(kv[1].values()), kv[0])):
            total = sum(pc.values())
            emit(f"   {' | '.join(sig)}   x{total}  [{', '.join(sorted(pc))}]")

    # ---------- 4. 结构骨架 ----------
    emit("")
    emit("=" * 78)
    emit("4. 结构骨架（每页 depth 0..2，列出影响间距的属性）")
    emit("=" * 78)
    for page in sorted(pages):
        emit("")
        emit(f"# {page}")
        for n in walk(pages[page][1]):
            if n.depth > 2:
                continue
            bits = [n.tag]
            if n.name:
                bits.append(f'name="{n.name}"')
            for a in SPACING_ATTRS + ALIGN_ATTRS + ("height", "width", "borderround"):
                if a in n.attrs:
                    bits.append(f'{a}="{n.attrs[a]}"')
            if n.attrs.get("vscrollbar") == "true":
                bits.append("vscrollbar")
            emit("  " * n.depth + "<" + " ".join(bits) + f">   ::L{n.line}")

    # ---------- 5. 离群值 ----------
    emit("")
    emit("=" * 78)
    emit("5. 离群值（取值出现次数 <=2 且与同属性主流值不同）")
    emit("=" * 78)

    def num_key(v):
        try:
            parts = [int(p) for p in v.split(",") if p.strip() != ""]
        except ValueError:
            return None
        return parts

    for a in ("inset", "childpadding", "textpadding", "height", "width", "align", "valign", "childvalign"):
        src = fixed_nodes if a in FIXED_SIZE_ATTRS else all_nodes
        per = dist(src, a)
        if len(per) < 2:
            continue
        total = sum(sum(c.values()) for c in per.values())
        mode, mode_c = max(per.items(), key=lambda kv: sum(kv[1].values()))
        emit("")
        emit(f"--- {a}: 主流值 {mode!r} ({sum(mode_c.values())}/{total} = "
             f"{100.0 * sum(mode_c.values()) / total:.0f}%) ---")
        for value, pc in sorted(per.items(), key=lambda kv: (sum(kv[1].values()), kv[0])):
            cnt = sum(pc.values())
            if cnt > 2 or value == mode:
                continue
            emit(f"    离群 {value!r} x{cnt}  页面: {', '.join(sorted(pc))}")

    txt = "\n".join(lines) + "\n"
    OUT.write_text(txt, encoding="utf-8")
    print(txt)
    print(f"[written] {OUT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
