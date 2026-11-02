"""Patch verify_matrix.py: replace condition recognition with real evaluation.

The patcher rewrites two regions by locating them structurally (function boundaries),
not by matching long text, so it cannot silently half-apply.
"""
import re

P = r"tools\mig\verify_matrix.py"

NEW_COND = '''COND_TOKEN = re.compile(r"'(\\$?\\(?[^'\\n]*?)'")
EXISTS_RE = re.compile(r"(?<!\\w)Ex?ists\\s*\\(\\s*'([^']*)'\\s*\\)", re.I)


def _split_top(expr, op):
    """把 expr 按顶层 op（'==' / '!=' / 'And' / 'Or'）切开，忽略引号内内容。"""
    parts, depth, quote, buf = [], 0, False, []
    i = 0
    n = len(expr)
    lowered = expr.lower()
    opl = op.lower()
    while i < n:
        ch = expr[i]
        if ch == "'":
            quote = not quote
            buf.append(ch)
            i += 1
            continue
        if not quote:
            if ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
            if depth == 0 and lowered.startswith(opl, i):
                # 词运算符需要边界，符号运算符不需要
                if opl.isalpha():
                    before_ok = i == 0 or not (expr[i - 1].isalnum() or expr[i - 1] == "_")
                    after = i + len(op)
                    after_ok = after >= n or not (expr[after].isalnum() or expr[after] == "_")
                    if not (before_ok and after_ok):
                        buf.append(ch)
                        i += 1
                        continue
                parts.append("".join(buf))
                buf = []
                i += len(op)
                continue
        buf.append(ch)
        i += 1
    parts.append("".join(buf))
    return [p.strip() for p in parts]


def _strip_outer_parens(expr):
    expr = expr.strip()
    while expr.startswith("(") and expr.endswith(")"):
        depth = 0
        balanced = True
        for idx, ch in enumerate(expr):
            if ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
                if depth == 0 and idx != len(expr) - 1:
                    balanced = False
                    break
        if not balanced:
            break
        expr = expr[1:-1].strip()
    return expr


def eval_condition(condition, conf, plat, solution_dir, project_dir, project_name):
    """对具体 (Configuration, Platform) 求值一个 MSBuild Condition。

    返回 True/False。无法识别的形态抛 ConditionError，调用方必须把它报出来——
    静默当作 False 正是旧实现漏判的根源。
    """
    if condition is None:
        return True
    expr = _strip_outer_parens(condition)
    if not expr:
        return True

    # Or 优先级低于 And
    ors = _split_top(expr, " Or ")
    if len(ors) > 1:
        return any(eval_condition(o, conf, plat, solution_dir, project_dir, project_name)
                   for o in ors)
    ands = _split_top(expr, " And ")
    if len(ands) > 1:
        return all(eval_condition(a, conf, plat, solution_dir, project_dir, project_name)
                   for a in ands)

    expr = _strip_outer_parens(expr)

    m = EXISTS_RE.fullmatch(expr)
    if m:
        raw = m.group(1)
        path = expand_vars(raw, conf, plat, solution_dir, project_dir, project_name, None)
        return os.path.exists(path)

    if expr.startswith("!") and not expr.startswith("!="):
        inner = expr[1:].strip()
        if not inner:
            raise ConditionError("empty negation in: " + condition)
        return not eval_condition(inner, conf, plat, solution_dir, project_dir, project_name)

    for op in ("==", "!="):
        halves = _split_top(expr, op)
        if len(halves) == 2:
            lhs = expand_vars(_unquote(halves[0]), conf, plat,
                              solution_dir, project_dir, project_name, None)
            rhs = expand_vars(_unquote(halves[1]), conf, plat,
                              solution_dir, project_dir, project_name, None)
            return (lhs == rhs) if op == "==" else (lhs != rhs)

    raise ConditionError("unrecognised condition: " + condition)


class ConditionError(Exception):
    pass


def _unquote(token):
    token = token.strip()
    if len(token) >= 2 and token[0] == "'" and token[-1] == "'":
        return token[1:-1]
    return token
'''

NEW_RESOLVE = '''    unevaluable = []

    def resolve(pairs, declared):
        """pairs: [(group_cond, elem_cond, value)] 按文档顺序 -> {cfg: value}

        每个配置独立求值，而不是先从条件里"猜"出候选配置再取交集。
        """
        out = {}
        for group_cond, elem_cond, value in pairs:
            for cfg in declared:
                cconf, cplat = split_cfg(cfg)
                try:
                    if not eval_condition(group_cond, cconf, cplat,
                                          solution_dir, project_dir, project_name):
                        continue
                    if not eval_condition(elem_cond, cconf, cplat,
                                          solution_dir, project_dir, project_name):
                        continue
                except ConditionError as exc:
                    unevaluable.append((cfg, str(exc)))
                    continue
                out[cfg] = value
        return out
'''


def main():
    src = open(P, encoding="utf-8").read()
    lines = src.splitlines(keepends=True)

    def find_func(name):
        start = None
        for i, line in enumerate(lines):
            if line.startswith("def %s(" % name):
                start = i
                break
        if start is None:
            raise SystemExit("function not found: " + name)
        end = start + 1
        while end < len(lines) and (lines[end].startswith((" ", "\t")) or lines[end].strip() == ""):
            end += 1
        return start, end

    # 1) replace condition_targets with the evaluator + helpers
    s, e = find_func("condition_targets")
    lines[s:e] = [NEW_COND]
    src = "".join(lines)

    # 2) replace the inner resolve() in parse_vcxproj
    old_start = src.index("    def resolve(pairs, declared):")
    old_end = src.index("        return out\n", old_start) + len("        return out\n")
    src = src[:old_start] + NEW_RESOLVE + src[old_end:]

    open(P, "w", encoding="utf-8", newline="\n").write(src)

    # post-conditions: must compile, and the old recogniser must be gone
    import ast
    ast.parse(src)
    assert "condition_targets" not in src, "old recogniser still referenced"
    assert "def eval_condition" in src and "class ConditionError" in src
    print("patched; ast OK; lines", len(src.splitlines()))


if __name__ == "__main__":
    main()
