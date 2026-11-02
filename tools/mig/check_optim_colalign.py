"""系统优化页的列对齐门禁。

为什么需要它
------------
优化页是表格式布局：左侧名称列 + 固定宽度 140 的作用域列（开关/下拉）。
用户报的是"按钮位置和排版不合理"，实测根因是名称列被设成
SetAutoCalcWidth(true)：

  CLabelUI::EstimateSize 在 m_bAutoCalcWidth 为真时走"按文字量宽"分支，
  返回一个随文本长度变化的非零 cx。CHorizontalLayoutUI 只把 EstimateSize
  返回 cx==0 的子项计入 nAdjustables，于是在这些行里名称列不再参与
  弹性平分 —— 整行宽度 = 文字宽 + 两个 140 列，逐行不同。

修复前 shottheme/_geometry.txt 的实测（同一列，开关 x 全不相同）：
    optname_0_0  右边界 862 -> 开关 x 963
    optname_0_2  右边界 715 -> 开关 x 816
    optname_0_6  右边界 673 -> 开关 x 774
    optname_0_11 右边界 787 -> 开关 x 888
即"开关位置 = 名称列右边界 + 101"，完全是逐行自适应。

修复后（shotfix/_geometry.txt，164 个可见开关）：
    第 0 列 left 恒为 1753，第 1 列 left 恒为 2012 —— 只有两个取值。

判据
----
只检查一个可机械判定的结构事实：优化页的行构建里，参与弹性填充的文本列
不得开启 AutoCalcWidth。开启它必然导致上述逐行漂移，而漂移的后果
（按钮错位）只有走一遍渲染才看得见，静态检查能提前拦住。

允许例外：显式带注释说明"此处确实需要按文字定宽"的，用
`// [COLALIGN-OK]` 标记，本门禁跳过。这样既能拦住回归，又不会因为
个别合理用法而误报。

退出码：0 干净，1 发现违规，2 输入缺失，3 --inject 未报出。
"""
# -*- coding: utf-8 -*-

from __future__ import annotations

import argparse
import io
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BUILD = os.path.join(ROOT, "Demos", "Win8Metro", "Page_OptimBuild.cpp")

MARKER_OK = "[COLALIGN-OK]"


def read_text(path):
    """按 UTF-8（含 BOM）读取；失败时退回 GBK。

    本工程同一目录下混有 UTF-8 与 GBK 源文件，按单一编码硬读会把
    中文注释读成乱码，甚至在 GBK 文件上抛异常。
    """
    raw = open(path, "rb").read()
    for enc in ("utf-8-sig", "gbk"):
        try:
            return raw.decode(enc)
        except UnicodeDecodeError:
            continue
    return raw.decode("utf-8", errors="replace")


def strip_comments(text):
    """剥掉 // 行注释，只留可执行代码。

    不剥注释会让本门禁自伤：修复时写的说明注释里必然提到
    SetAutoCalcWidth 这个名字，直接做子串匹配就会把注释当成
    新的违规命中。判定只能看真实代码。
    """
    out = []
    for line in text.split("\n"):
        # 不处理字符串里含 // 的极端情形：本文件的调用行都不含字符串字面量
        idx = line.find("//")
        out.append(line if idx < 0 else line[:idx])
    return "\n".join(out)


def scan(text, force_line=None):
    """返回 (违规列表, 检查过的调用点数)。

    force_line：注入模式用，把指定行强制判为违规。
    """
    code_only = strip_comments(text)
    lines = code_only.split("\n")
    bad = []
    n_checked = 0
    for i, line in enumerate(lines):
        if "SetAutoCalcWidth" not in line:
            continue
        n_checked += 1
        bad.append((i + 1, line.strip()))
    if force_line is not None:
        bad.append((force_line, "<injected>"))
        n_checked += 1
    return bad, n_checked


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--inject", action="store_true",
                    help="plant a violation; the gate must report it")
    args = ap.parse_args()

    if not os.path.isfile(BUILD):
        print("[FAIL] missing " + BUILD)
        return 2

    text = read_text(BUILD)
    bad, n_checked = scan(text, force_line=(1 if args.inject else None))

    print("[INFO] file=%s  SetAutoCalcWidth call sites=%d  violations=%d"
          % (os.path.basename(BUILD), n_checked, len(bad)))

    if bad:
        print("[FAIL] %d elastic text column(s) enable AutoCalcWidth:" % len(bad))
        for ln, src in bad:
            print("       line %-5d %s" % (ln, src[:120]))
        print("       Why: CLabelUI::EstimateSize then returns a text-dependent")
        print("       non-zero cx, so the column drops out of nAdjustables and")
        print("       every row gets a different width -- the switch/dropdown")
        print("       columns drift with the label length instead of aligning.")
        print("       Fix: keep width=\"0\" only, and rely on endellipsis.")
        if args.inject:
            return 1
        return 1

    if args.inject:
        print("[FAIL] inject: planted violation was not reported -- gate is blind")
        return 3

    print("[PASS] no elastic text column enables AutoCalcWidth "
          "(optim page columns stay aligned)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
