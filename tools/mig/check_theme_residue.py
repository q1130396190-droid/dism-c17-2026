"""检查两件事：

1) 代码里出现字面量 "{tok:...}" —— 它不会被展开。

   {tok:} 只在皮肤 XML 展开阶段由 FrameWnd::ExpandSkinFile 替换。
   在 C++ 里 SetAttribute(L"bordercolor", L"{tok:Border}") 传入的是一个
   非法颜色串，控件取默认值，深色下边框与背景同色直接消失。
   这类缺陷在浅色模式下往往"看起来还行"，所以肉眼很难发现。

2) Build 里设了颜色、RefreshThemeColors() 里没刷同一个属性 —— 切换后残留旧色。

   刷新清单是手写枚举，漏项只能靠逐属性对照发现。已实测的漏项案例：
     · Page_OptimBuild: itemtextcolor / itemselectedbkcolor
     · Page_Unattend:   子控件 bkcolor ×2、组头 hotbkcolor

退出码：0 干净，1 发现问题，2 输入问题，3 --inject 没报出来。
"""
from __future__ import annotations

import argparse
import glob
import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
APP = os.path.join(ROOT, "Demos", "Win8Metro")

# 字面 {tok:...} 出现在 C++ 里：只有 XML 展开器会替换它
TOK_LITERAL = re.compile(r'L"\s*\{tok:[^}]*\}"')

# 颜色属性/设置器。attr 形式与类型化 setter 形式都要覆盖。
ATTR_COLOUR = re.compile(r'L"((?:item|hot|pushed|selected|disabled|link)?'
                         r'(?:text|bk|border)color)"')
SETTER_COLOUR = re.compile(r'Set(?:Text|Bk|HotBk|PushedBk|SelectedBk|Border)Color\s*\(')


def func_body(text: str, marker: str):
    """取 marker 所在函数的函数体（花括号配平）。找不到返回 None。"""
    i = text.find(marker)
    if i < 0:
        return None
    start = text.find('{', i)
    if start < 0:
        return None
    depth = 0
    for j in range(start, len(text)):
        if text[j] == '{':
            depth += 1
        elif text[j] == '}':
            depth -= 1
            if depth == 0:
                return text[start:j + 1]
    return None


def colour_names(seg: str):
    """从一段代码里抽出所有被设置的颜色属性名。"""
    return set(ATTR_COLOUR.findall(seg))


def has_setter(seg: str) -> bool:
    return bool(SETTER_COLOUR.search(seg))


def scan_file(path: str):
    """返回 (tok_literals, residue)。

    tok_literals: [(行号, 文本)]
    residue:      [(行号, 属性名)]  —— Build 里设了、Refresh 没刷
    """
    text = io.open(path, encoding='utf-8-sig', errors='replace').read()
    lines = text.splitlines()

    toks = []
    for n, line in enumerate(lines, 1):
        if TOK_LITERAL.search(line):
            toks.append((n, line.strip()))

    body = func_body(text, '::RefreshThemeColors')
    if body is None:
        # 没有刷新钩子：由 check_theme_refresh 负责，这里不重复报
        return toks, []

    # 刷新段覆盖的属性：函数体 + 它调用的同文件辅助函数（BFS 一层）
    refreshed = colour_names(body)
    for helper in set(re.findall(r'\b([A-Z]\w+)\s*\(', body)):
        hb = func_body(text, '::' + helper)
        if hb:
            refreshed |= colour_names(hb)

    # [R244] 类型化 setter 与字符串属性名是同一个"已刷新"语义。
    #   Page_Welcome 的刷新用的是 SetBorderColor(dwBorder)，而 Build 里写的是
    #   SetAttribute(L"bordercolor", ...)。只按属性名比对会把它误报成漏刷。
    #   这里把 setter 也映射成属性名，两边才能对齐比较。
    def setter_names(seg):
        out = set()
        if re.search(r'SetTextColor\s*\(', seg):        out.add('textcolor')
        if re.search(r'SetBkColor\s*\(', seg):          out.add('bkcolor')
        if re.search(r'SetHotBkColor\s*\(', seg):       out.add('hotbkcolor')
        if re.search(r'SetPushedBkColor\s*\(', seg):    out.add('pushedbkcolor')
        if re.search(r'SetSelectedBkColor\s*\(', seg):  out.add('selectedbkcolor')
        if re.search(r'SetBorderColor\s*\(', seg):      out.add('bordercolor')
        return out

    refreshed |= setter_names(body)

    # 刷新段之外的所有设置点
    outside = text.replace(body, ' ' * len(body))
    out_lines = outside.splitlines()
    residue = []
    for n, line in enumerate(out_lines, 1):
        names = colour_names(line) | setter_names(line)
        for name in names:
            if name in refreshed:
                continue
            residue.append((n, name))
    return toks, residue


# ---------------------------------------------------------------------------
# kLegacy 双向覆盖检查
#
# ApplyAccentToControls() 里的 kLegacy 表是"按控件当前值反查令牌"：
#   if (DWORD v = mapLegacy(p->GetBkColor())) p->SetBkColor(v);
# 因此表里必须有**两个主题各自的令牌值**，否则某一方向的切换会全部落空：
#   只有浅色值 -> 深->浅 时当前值是深色，一个都不命中 -> 深色整片残留
#
# 实测症状：用户报"切到浅色模式显示不正常，深色残留比较严重"，
# 根因就是原表 13 项全是浅色值，深色的 10 个核心值一个都没有。
#
# 本检查读取 ThemeTokens.cpp 的深浅两个分支，取出核心中性色，
# 再确认它们都出现在 FrameWnd.cpp 的 kLegacy 表里。
# ---------------------------------------------------------------------------

# 需要双向覆盖的令牌（强调色/语义色跟随系统主题色，不在此列）
NEUTRAL_TOKENS = ('Surface', 'SurfaceAlt', 'SurfaceSunk',
                  'Border', 'BorderSoft',
                  'Ink', 'InkSecondary', 'InkTertiary', 'InkMuted')

# 明确豁免：一个色值对应两种语义时无法入表，必须显式登记理由。
# 0xFF333333 = 浅色 InkSecondary == 深色 SurfaceHover
LEGACY_EXEMPT = {
    0xFF333333: 'light InkSecondary equals dark SurfaceHover; hover is refreshed per page',
}


def _token_values(src_path):
    """从 ThemeTokens.cpp 抽出每个令牌在深/浅分支里的字面值。

    真实写法是：
        s.arr[static_cast<int>(EToken::Border)]       = 0x00757575;
    所以正则必须容许 static_cast 与任意空格，不能只匹配 EToken::X]。
    """
    text = io.open(src_path, encoding='utf-8-sig', errors='replace').read()

    # 深分支以 "if (bWantDark)" 开头（旧版是 IsSystemDarkMode()），浅分支是它后面的 else。
    needles = ('if (bWantDark)', 'if (dwmtheme::IsSystemDarkMode())')
    i = -1
    for n in needles:
        i = text.find(n)
        if i >= 0:
            break
    if i < 0:
        return {}, {}
    brace = text.find('{', i)
    els = text.find('else', brace)
    if brace < 0 or els < 0:
        return {}, {}
    # 浅分支到 OnAccent 计算块之前结束（那之后的 OnAccent/OnDanger 是两分支共用）
    light_end = text.find('OnAccent', els)
    if light_end < 0 or light_end < els:
        light_end = min(len(text), els + 8000)

    pat = re.compile(r'EToken::(\w+)\)\]\s*=\s*0x00([0-9A-Fa-f]{6})')

    def grab(seg):
        return {m.group(1): int(m.group(2), 16) for m in pat.finditer(seg)}

    return grab(text[brace:els]), grab(text[els:light_end])


def _legacy_keys(src_path):
    text = io.open(src_path, encoding='utf-8-sig', errors='replace').read()
    i = text.find('static const LegacyPair kLegacy[]')
    if i < 0:
        return None
    j = text.find('};', i)
    return {int(m.group(1), 16) for m in
            re.finditer(r'\{ (0x[0-9A-Fa-f]{8}), tokens::EToken::', text[i:j])}


def check_legacy_coverage():
    """返回 [(说明, 是否通过)]，或 None 表示输入不可用。"""
    tok_src = os.path.join(ROOT, 'Demos', 'Win8Metro', 'ThemeTokens.cpp')
    fw_src = os.path.join(ROOT, 'Demos', 'Win8Metro', 'FrameWnd.cpp')
    if not (os.path.isfile(tok_src) and os.path.isfile(fw_src)):
        return None

    dark, light = _token_values(tok_src)
    keys = _legacy_keys(fw_src)
    if keys is None or not dark or not light:
        return None

    # 口径对齐：kLegacy 的键是 0xAARRGGBB（带 FF alpha），
    #   而 _token_values 取的是源码里的 0x00RRGGBB 字面量。
    #   直接比较会全部落空（实测 18 个全报缺失，而它们其实都在表里）。
    keys_rgb = {k & 0x00FFFFFF for k in keys}

    out = []
    for name in NEUTRAL_TOKENS:
        for label, vals in (('dark', dark), ('light', light)):
            v = vals.get(name)
            if v is None or v in LEGACY_EXEMPT:
                continue
            out.append(('%s.%-12s #%06X' % (label, name, v), v in keys_rgb))
    return out


INJECT_TOK = '_zz_injected_tok_literal.cpp'
INJECT_TOK_BODY = (
    '// planted by check_theme_residue.py --inject\n'
    'void zz_tok_probe(void* p)\n'
    '{\n'
    '    ((struct S*)p)->SetAttribute(L"bordercolor", L"{tok:Border}");\n'
    '}\n'
)
INJECT_TOK_EXPECT = 1


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--inject', action='store_true',
                    help='plant a {tok:} literal; the gate must report it')
    args = ap.parse_args()

    files = sorted(glob.glob(os.path.join(APP, 'Page_*.cpp'))
                   + glob.glob(os.path.join(APP, 'FrameWnd*.cpp')))
    if not files:
        print('[FAIL] no sources under ' + APP)
        return 2

    print('[INFO] files_scanned=%d' % len(files))

    planted = None
    plant_toks = []
    if args.inject:
        planted = os.path.join(APP, INJECT_TOK)
        with io.open(planted, 'w', encoding='utf-8', newline='\n') as fh:
            fh.write(INJECT_TOK_BODY)
        plant_toks, _ = scan_file(planted)
        files = files + [planted]

    try:
        all_toks = []
        all_res = []
        for f in files:
            toks, res = scan_file(f)
            rel = os.path.relpath(f, ROOT)
            for n, t in toks:
                all_toks.append((rel, n, t))
            for n, name in res:
                all_res.append((rel, n, name))
    finally:
        if planted and os.path.exists(planted):
            os.remove(planted)

    print('[INFO] tok_literals=%d  residue=%d' % (len(all_toks), len(all_res)))

    if args.inject:
        # 自证：必须精确报出植入的那一条，而不是"有东西被报出"就算过。
        n_plant = len([x for x in all_toks if INJECT_TOK in x[0]])
        print('[INFO] inject: planted=%d reported=%d (expect %d)'
              % (INJECT_TOK_EXPECT, n_plant, INJECT_TOK_EXPECT))
        if n_plant != INJECT_TOK_EXPECT:
            print('[FAIL] inject: {tok:} literal not reported -- the gate is blind')
            return 3

    if all_toks or all_res:
        if all_toks:
            print('[FAIL] %d literal {tok:} placeholder(s) in C++ -- never expanded:'
                  % len(all_toks))
            for rel, n, t in all_toks[:20]:
                print('       %-52s L%-5d %s' % (rel, n, t[:80]))
            print('       {tok:} is replaced only while expanding skin XML; '
                  'use tokens::TokenHex(EToken::X) here')
        if all_res:
            print('[FAIL] %d colour attribute(s) set in Build but not refreshed:'
                  % len(all_res))
            for rel, n, name in all_res[:30]:
                print('       %-52s L%-5d %s' % (rel, n, name))
            print('       add them to RefreshThemeColors() or they keep the old '
                  'theme colour after a light<->dark switch')
        return 1

    # kLegacy 双向覆盖：表里必须同时有浅色与深色令牌值，否则单向切换整片残留。
    cov = check_legacy_coverage()
    if cov is None:
        print('[FAIL] cannot read ThemeTokens.cpp kLegacy table -- refusing to pass')
        return 2
    missing = [d for d, ok in cov if not ok]
    print('[INFO] kLegacy coverage: %d checked, %d present'
          % (len(cov), len(cov) - len(missing)))
    if missing:
        print('[FAIL] %d theme token value(s) missing from kLegacy:' % len(missing))
        for d in missing:
            print('       %s' % d)
        print('       a value absent from kLegacy is treated as "unknown, leave it", '
              'so that side of the light<->dark switch keeps the old colours')
        return 1

    print('[PASS] no literal {tok:} placeholders; every Build-time colour is '
          'refreshed on theme switch; kLegacy covers both themes')
    return 0


if __name__ == '__main__':
    sys.exit(main())
