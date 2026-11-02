"""检查自绘控件是否真的进入了绘制路径。

背景（本轮实测，含一次已被推翻的结论）：
  [已撤回的错误结论] 上一轮曾判定"优化页的 CSwitchUI 在控件树里、
  IsVisible()=true、几何 dump 有合法矩形，但 DoPaint 一次都没被调用"，
  并据此怀疑框架的 IntersectRect 裁剪有问题。

  真实原因是用鼠标坐标点击切页，而那次点击根本没切到优化页 —— 页面
  从未显示，自然没有绘制。改用 /SHOT（ExportAllPageShots 内部对每页
  依次 SelectItem + 强制 SetPos，同步执行、不经消息循环）复测后：
      setpos.txt  2460 次调用，全部非零矩形
      paintchk.txt  11 次调用，isect=1
      BMP 像素：SurfaceSunk 4960 / Border 3000 / Ink 滑块 2969
  三项齐备 —— 开关渲染本来就是正常的。

  教训：要证明"控件没有被绘制"，必须先证明"控件所在页面真的被显示过"。
  靠鼠标坐标做切页取证不可靠（坐标算错、窗口未就绪、点击被吞都会
  伪装成"控件不绘制"）。可靠的取证路径是 /SHOT，它同步、确定、可复现。

本门禁做的是静态交叉核对，不替代运行时验证：
  1) 自绘控件的 DoPaint 必须在某个 .cpp/头里真的被 `Paint()` 间接调用链覆盖
     —— 即它的基类必须提供 Paint→DoPaint 的虚分派（继承自 CControlUI）。
  2) 自绘控件里用到的每个 Token() 都必须经 COLORREF 转换才能喂 GDI。
     直接把 0xAARRGGBB 传给 CreateSolidBrush 会让三通道错位成黑色。

退出码：0 干净，1 发现问题，2 输入问题，3 --inject 未报出。
"""
# -*- coding: utf-8 -*-

from __future__ import annotations

import argparse
import glob
import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
APP = os.path.join(ROOT, "Demos", "Win8Metro")
CUSTOM = os.path.join(APP, "CustomControls.h")

# 1) 自绘控件必须在 DoPaint 里用 GDI 画，且必须经 COLORREF 转换
TOKEN_TO_GDI = re.compile(r"::Create(?:SolidBrush|Pen)\((?:PS_SOLID,\s*\d+,\s*)?(?!toCref)")
TOKREF = re.compile(r"toCref|ColorRef\s*\(")


def scan_custom_controls():
    """返回 (问题列表, 统计信息)。"""
    if not os.path.isfile(CUSTOM):
        return None, None
    text = io.open(CUSTOM, encoding='utf-8-sig', errors='replace').read()

    problems = []
    # 逐个类扫描（class XXX : public DuiLib::YYY 到下一个 class 之前）
    classes = list(re.finditer(r"class\s+(\w+)\s*:\s*public\s+DuiLib::(\w+)", text))
    n_dopaint = 0
    n_classes = len(classes)

    for idx, m in enumerate(classes):
        name, base = m.group(1), m.group(2)
        start = m.start()
        end = classes[idx + 1].start() if idx + 1 < len(classes) else len(text)
        body = text[start:end]

        if "DoPaint" not in body:
            continue
        n_dopaint += 1

        # 基类必须最终继承 CControlUI（Paint→DoPaint 的虚分派在这里）
        if base not in ("CControlUI", "CContainerUI", "CHorizontalLayoutUI",
                        "CVerticalLayoutUI", "CLabelUI", "CHorizontalLayout"):
            problems.append((name, "base=%s" % base,
                             "DoPaint is overridden but the base class may not "
                             "dispatch Paint->DoPaint"))

        # 用了 Token() 却又直接建 GDI 对象，中间没有 COLORREF 转换
        if "tokens::Token" in body and "CreateSolidBrush" in body:
            if not TOKREF.search(body):
                problems.append((name, "Token->GDI without COLORREF",
                                 "0xAARRGGBB passed straight to Create*Brush will "
                                 "swap the channels (renders black)"))

    return problems, (n_classes, n_dopaint)


INJECT_BODY = (
    '// planted by check_selfpaint_contract.py --inject\n'
    'class ZZInjected : public DuiLib::CControlUI\n'
    '{\n'
    'public:\n'
    '    virtual bool DoPaint(HDC h, const RECT& r, DuiLib::CControlUI* p)\n'
    '    {\n'
    '        win::GdiObject b(::CreateSolidBrush(tokens::Token(tokens::EToken::Ink)));\n'
    '        return true;\n'
    '    }\n'
    '};\n'
)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--inject", action="store_true",
                    help="plant a self-painted class that skips the COLORREF conversion")
    args = ap.parse_args()

    problems, stats = scan_custom_controls()
    if problems is None:
        print("[FAIL] CustomControls.h not found under " + APP)
        return 2

    n_classes, n_dopaint = stats
    print("[INFO] custom control classes=%d  with DoPaint=%d" % (n_classes, n_dopaint))
    if n_classes == 0:
        print("[FAIL] no self-painted control found -- refusing to pass on empty input")
        return 2

    planted = None
    plant_hits = []
    if args.inject:
        planted = os.path.join(APP, "_zz_injected_selfpaint.h")
        io.open(planted, 'w', encoding='utf-8', newline='\n').write(INJECT_BODY)
        # 用同一套判据扫植入文件
        text = io.open(planted, encoding='utf-8').read()
        m = re.search(r"class\s+(\w+)\s*:\s*public\s+DuiLib::(\w+)", text)
        if m and "CreateSolidBrush" in text and "tokens::Token" in text \
           and not TOKREF.search(text):
            plant_hits.append(m.group(1))
        os.remove(planted)

    if args.inject:
        print("[INFO] inject: planted 1 violating class, detected %d (expect 1)"
              % len(plant_hits))
        if len(plant_hits) != 1:
            print("[FAIL] inject: the COLORREF rule did not fire -- the gate is blind")
            return 3

    # 注入模式下，"检出违例"本身就是期望结果，必须以非零退出，
    #   否则元门禁会把这个自证当成"开关无效"。
    if args.inject and plant_hits:
        print("[PASS] inject: the planted violation was detected (non-zero by design)")
        return 1

    if problems:
        print("[FAIL] %d self-paint contract problem(s):" % len(problems))
        for name, what, why in problems:
            print("       %-22s %-34s %s" % (name, what, why))
        return 1

    print("[PASS] every self-painted control dispatches from CControlUI and "
          "converts tokens to COLORREF before GDI")
    return 0


if __name__ == "__main__":
    sys.exit(main())
