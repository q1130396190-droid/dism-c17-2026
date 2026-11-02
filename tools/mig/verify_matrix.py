#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""构建配置矩阵校验门禁（静态 + 语义，不执行构建）。

检查项（每条失败都打印 `[FAIL] <工程> <配置> <判据>: <实际值>`）:

  1. solution-config-mapping
     解析 DuiLib.sln 的 SolutionConfigurationPlatforms 与每个工程的
     ProjectConfigurationPlatforms；每个解决方案配置对每个工程都必须有
     ActiveCfg 与 Build.0，且 ActiveCfg 指向的工程配置必须在该工程
     .vcxproj 的 <ProjectConfiguration Include="..."> 中真实存在。
     判据是"映射目标必须真实存在"，而不是"必须有 16 个配置"：
     DuiLib 项目有全部 16 个，Demos 只映射到 Debug/Release，两者都合法。
     => 捕获缺陷 1（缺映射导致 MSB8013）

  2. static-dynamic-isolation
     解析 DuiLib.vcxproj 的每个 ItemDefinitionGroup（按 Condition 取配置名），
     用 ConfigurationType 区分 StaticLibrary / DynamicLibrary。
     静态配置的 ImportLibrary、OutDir 不得与任何动态配置的对应值相同；
     动态配置的 ImportLibrary 不得与任何静态配置的 OutDir+TargetName+.lib 相同。
     => 捕获缺陷 2（静态库覆盖 Lib\\x64 下的导入库 DuiLib.lib -> LNK2001 CDuiString）

  3. intdir-uniqueness
     同一工程内任意两个配置的 IntDir 规范化后不得相同。
     => 捕获缺陷 3（SDebug 与 Debug 共用 Temp\\Debug）

  4. artifact-uniqueness
     同一工程内 OutDir+TargetName 规范化后不得重复。

  import-expansion（U3，覆盖度而非判据）
    递归展开 <Import Project="...">：被导入文件（.props/.targets）的
     PropertyGroup / ItemDefinitionGroup 按其在宿主文件中的位置插入求值序列，
     从而"后出现的覆盖先出现的"（等同 MSBuild 文档顺序语义）。
     无法展开的 Import（$(VCTargetsPath)、$(UserRootDir) 等 SDK/用户变量）
     必须显式记为"未展开"，计入 [SCAN] 统计并输出 [WARN]/[COVERAGE]，
     不允许静默跳过——否则"零输入 PASS"会被误读成"检查通过"。

退出码:
  0 = 全部检查通过
  1 = 有检查失败
  2 = 输入缺失/无法解析（sln 或 vcxproj 不存在），或 --inject 自测未报出

用法:
  python tools/mig/verify_matrix.py
  python tools/mig/verify_matrix.py --inject      # 追加一次内存注入的自测
"""

from __future__ import annotations

import argparse
import os
import re
import sys
import xml.etree.ElementTree as ET

NS = "{http://schemas.microsoft.com/developer/msbuild/2003}"

# 条件形如: '$(Configuration)|$(Platform)'=='Debug|x64'
# 只用 r"" 前缀的简单正则，避免 \S \D 之类被当转义。
COND_RE = re.compile(r"==\s*'([^']+)'")
PROJ_RE = re.compile(
    r'^Project\("\{[0-9A-Fa-f\-]+\}"\)\s*=\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*"(\{[0-9A-Fa-f\-]+\})"'
)
MAP_RE = re.compile(r"^(\{[0-9A-Fa-f\-]+\})\.(.+?)\.(ActiveCfg|Build\.0)\s*=\s*(.+)$")

CFG_PROP_NAMES = ("OutDir", "IntDir", "TargetName")

BSLASH = chr(92)  # 反斜杠，避免任何字面量转义问题


# ---------------------------------------------------------------- 基础工具


def read_text(path):
    """按 utf-8-sig 读取（DuiLib.sln / DuiLib.vcxproj 都是 UTF-8 with BOM）。"""
    with open(path, "r", encoding="utf-8-sig", errors="replace") as fh:
        return fh.read()


def norm_path(value):
    """规范化路径：统一分隔符、消掉 . 与 ..、忽略大小写。"""
    if value is None:
        return ""
    p = str(value).strip().replace(BSLASH, "/")
    parts = []
    for seg in p.split("/"):
        if seg == "" or seg == ".":
            continue
        if seg == "..":
            if parts:
                parts.pop()
            continue
        parts.append(seg)
    return "/".join(parts).lower()


def split_cfg(included):
    """'Debug|x64' -> ('Debug', 'x64')"""
    if "|" in included:
        conf, plat = included.split("|", 1)
        return conf.strip(), plat.strip()
    return included.strip(), ""


def expand_vars(value, conf, plat, solution_dir, project_dir, project_name, target_name):
    if value is None:
        return None
    text = str(value)
    reps = [
        ("$(SolutionDir)", solution_dir.rstrip(BSLASH + "/") + "/"),
        ("$(ProjectDir)", project_dir.rstrip(BSLASH + "/") + "/"),
        ("$(ProjectName)", project_name),
        ("$(TargetName)", target_name if target_name else project_name),
        ("$(Configuration)", conf),
        ("$(Platform)", plat),
    ]
    for key, val in reps:
        text = text.replace(key, val)
    return text


COND_TOKEN = re.compile(r"'(\$?\(?[^'\n]*?)'")
EXISTS_RE = re.compile(r"(?<!\w)Ex?ists\s*\(\s*'([^']*)'\s*\)", re.I)


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
# ---------------------------------------------------------------- 解析 sln


def parse_sln(path):
    text = read_text(path)
    sln = {
        "projects": [],          # [{"name","path","guid"}]
        "configs": [],           # ["Debug|x64", ...]（SolutionConfigurationPlatforms 左值）
        "mapping": {},           # guid -> {solcfg: {"ActiveCfg":..., "Build.0":...}}
        "cfg_profiles": {},      # 左侧配置 -> 右侧配置
    }
    section = None
    for raw in text.splitlines():
        line = raw.strip()
        if not line:
            continue
        if "GlobalSection(SolutionConfigurationPlatforms)" in line:
            section = "solcfg"
            continue
        if "GlobalSection(ProjectConfigurationPlatforms)" in line:
            section = "projmap"
            continue
        if line.startswith("EndGlobalSection"):
            section = None
            continue

        if section == "solcfg":
            if "=" in line:
                left, right = line.split("=", 1)
                left, right = left.strip(), right.strip()
                sln["configs"].append(left)
                sln["cfg_profiles"][left] = right
            continue

        if section == "projmap":
            m = MAP_RE.match(line)
            if not m:
                continue
            guid = m.group(1).upper()
            sol_cfg = m.group(2).strip()
            kind = m.group(3)
            value = m.group(4).strip()
            sln["mapping"].setdefault(guid, {}).setdefault(sol_cfg, {})[kind] = value
            continue

        m = PROJ_RE.match(line)
        if m:
            sln["projects"].append(
                {"name": m.group(1).strip(), "path": m.group(2).strip(), "guid": m.group(3).upper()}
            )
    return sln


# ---------------------------------------------------------------- 解析 vcxproj
#
# Import 展开（U3）:
#   MSBuild 的求值单位是"文档顺序"：<Import> 出现的位置就是被导入文件全部
#   PropertyGroup / ItemDefinitionGroup 的插入点，之后的赋值覆盖之前的值。
#   这里严格照做——先把一个 .vcxproj 连同其 Import 链压平成一个有序的求值单元
#   序列（Import 处就地插入被导入文件的单元），再按序列逐条赋值，
#   "后出现的覆盖先出现的"因此是自然结果，不需要额外优先级规则。
#   每个 (Configuration, Platform) 单独跑一遍压平流程，因为 Import 的路径和
#   Condition 都可能依赖 $(Configuration)/$(Platform)。

MSBUILD_VAR_RE = re.compile(r"\$\([A-Za-z_][A-Za-z0-9_.]*\)")

# 这些变量指向 SDK / 用户目录 / VS 安装位置。静态检查器不猜它们的真实取值，
# 因此相关 Import 视为"未展开"：必须上报 [WARN]，但不算仓库内的覆盖缺口
# （它们属于环境，不属于仓库）。带其它未定义变量的 Import 同样归入此类。
ENV_PATH_VARS = (
    "$(VCTargetsPath)",
    "$(VCInstallDir)",
    "$(VCToolsInstallDir)",
    "$(VCIDEInstallDir)",
    "$(UserRootDir)",
    "$(LocalAppDataPlatform)",
    "$(MSBuildExtensionsPath)",
    "$(MSBuildExtensionsPath32)",
    "$(MSBuildToolsPath)",
    "$(MSBuildProgramFiles32)",
    "$(VSInstallDir)",
    "$(WindowsSdkDir)",
    "$(UniversalCRTSdkDir)",
    "$(WindowsSDKVersion)",
    "$(VCToolsVersion)",
)

MAX_IMPORT_DEPTH = 8

# 求值单元类型
U_PROP = "prop"        # OutDir / IntDir / TargetName
U_CTYPE = "ctype"      # ConfigurationType
U_IMPLIB = "implib"    # ItemDefinitionGroup/Link/ImportLibrary


def _strip_ns(tag):
    return tag[len(NS):] if tag.startswith(NS) else tag


def _has_var(text):
    """路径里是否还残留未展开的 $(...)。"""
    return bool(MSBUILD_VAR_RE.search(text or ""))


def _auto_text(el):
    return (el.text or "").strip()


class ImportScan:
    """一个 .vcxproj 的 Import 展开统计（跨 16 配置汇总）。"""

    def __init__(self, project_name, source):
        self.project_name = project_name
        self.source = source
        self.resolved = {}      # 小写绝对路径 -> {"display": str, "configs": set()}
        self.unresolved = {}    # 展示用路径 -> {"reason": str, "configs": set()}
        self.skipped = {}       # 展示用路径 -> {"reason": str, "configs": set()}
        self.cycles = {}        # 展示用路径 -> {"reason": str, "configs": set()}
        self.unevaluable = {}   # condition 文本 -> set(config)
        self.max_depth = 0
        self.imports_seen = 0   # <Import> 元素命中次数（含重复配置）
        self.props_read = 0     # 逐配置累计生效的赋值单元数

    def _bump(self, bucket, key, conf_item, confidence_note=None):
        entry = bucket.setdefault(key, {"reason": confidence_note or "", "configs": set()})
        entry["configs"].add(conf_item)
        return entry

    def add_resolved(self, abspath, display, cfg, depth):
        key = os.path.normcase(os.path.abspath(abspath))
        self.resolved.setdefault(key, {"display": display, "configs": set()})["configs"].add(cfg)
        if depth > self.max_depth:
            self.max_depth = depth

    def add_unresolved(self, display, cfg, reason):
        self._bump(self.unresolved, display, cfg, reason)

    def add_skipped(self, display, cfg, reason):
        self._bump(self.skipped, display, cfg, reason)

    def add_cycle(self, display, cfg):
        self._bump(self.cycles, display, cfg, "循环 Import，已截断")

    # -- 分类
    def unresolved_env(self):
        """未展开且原因与 SDK/用户目录/未定义变量有关 —— 只影响环境面。"""
        return {k: v for k, v in self.unresolved.items() if _has_var(k)}

    def unresolved_local(self):
        """未展开但路径已是具体本地路径 —— 真·覆盖缺口（MSBuild 会 MSB4019）。"""
        return {k: v for k, v in self.unresolved.items() if not _has_var(k)}


def summarise_imports(scans):
    """把各工程的 ImportScan 汇成一个总数，供 [SCAN] 行打印。"""
    stats = {
        "imports_seen": 0, "resolved_files": 0, "unresolved_files": 0,
        "env_files": 0, "local_files": 0, "skipped_files": 0,
        "cycle_files": 0, "max_depth": 0, "props_read": 0,
    }
    resolved_union = set()
    for s in scans:
        stats["imports_seen"] += s.imports_seen
        stats["unresolved_files"] += len(s.unresolved)
        stats["env_files"] += len(s.unresolved_env())
        stats["local_files"] += len(s.unresolved_local())
        stats["skipped_files"] += len(s.skipped)
        stats["cycle_files"] += len(s.cycles)
        stats["max_depth"] = max(stats["max_depth"], s.max_depth)
        stats["props_read"] += s.props_read
        resolved_union.update(s.resolved.keys())
    stats["resolved_files"] = len(resolved_union)
    return stats


def collect_import_fails(scans):
    """Import 指向仓库内具体路径但文件不存在 —— MSBuild 会 MSB4019，算真缺陷。"""
    fails = []
    for scan in scans:
        for display, info in sorted(scan.unresolved_local().items()):
            fails.append(
                (
                    scan.project_name,
                    ",".join(sorted(info["configs"])) or "-",
                    "import-unresolved",
                    "Import 未展开（" + info["reason"] + "）: " + display,
                )
            )
    return fails


def _import_candidate(project_attr, importer_dir, conf, plat, solution_dir,
                      project_dir, project_name):
    """把 Import Project 属性展开成候选路径（可能仍含未定义变量）。"""
    raw = expand_vars(project_attr.strip(), conf, plat, solution_dir,
                      project_dir, project_name, project_name)
    if not raw:
        return "", ""
    probe = raw.replace(BSLASH, "/")
    if os.path.isabs(probe.replace("/", os.sep)):
        cand = probe.replace("/", os.sep)
    else:
        cand = os.path.join(importer_dir, probe.replace("/", os.sep))
    try:
        cand = os.path.normpath(cand)
    except (OSError, ValueError):
        pass
    return raw, cand


def _walk_units(path, conds, ctx, scan, depth, stack, tree_cache):
    """按文档顺序产出求值单元 (conds, kind, name, value)。

    Import 处就地插入被导入文件的单元，实现"MSBuild 文档顺序"语义。
    """
    key = os.path.normcase(os.path.abspath(path))
    root = tree_cache.get(key)
    if root is None:
        root = ET.parse(path).getroot()
        tree_cache[key] = root

    for child in list(root):
        tag = _strip_ns(child.tag)

        if tag == "PropertyGroup":
            gcond = child.get("Condition")
            for el in list(child):
                etag = _strip_ns(el.tag)
                if etag in ("OutDir", "IntDir", "TargetName"):
                    yield (conds + (gcond, el.get("Condition")), U_PROP, etag, _auto_text(el))
                elif etag == "ConfigurationType":
                    yield (conds + (gcond, el.get("Condition")), U_CTYPE, etag, _auto_text(el))
            continue

        if tag == "ItemDefinitionGroup":
            gcond = child.get("Condition")
            for item in list(child):
                if _strip_ns(item.tag) != "Link":
                    continue
                for node in list(item):
                    if _strip_ns(node.tag) == "ImportLibrary":
                        yield (conds + (gcond, item.get("Condition"), node.get("Condition")),
                               U_IMPLIB, "ImportLibrary", _auto_text(node))
            continue

        if tag == "Import":
            for unit in _expand_import(child, conds, ctx, scan, depth, stack,
                                       tree_cache, os.path.dirname(os.path.abspath(path))):
                yield unit
            continue

        if tag == "ImportGroup":
            gcond = child.get("Condition")
            for sub in list(child):
                if _strip_ns(sub.tag) != "Import":
                    continue
                for unit in _expand_import(sub, conds + (gcond,), ctx, scan, depth, stack,
                                           tree_cache, os.path.dirname(os.path.abspath(path))):
                    yield unit
            continue


def _expand_import(imp_el, conds, ctx, scan, depth, stack, tree_cache, importer_dir):
    """展开一个 <Import>，返回要插入的求值单元列表。"""
    scan.imports_seen += 1
    cfg = ctx["config"]
    icond = imp_el.get("Condition")
    local_conds = conds + (icond,)

    project_attr = imp_el.get("Project")
    if not project_attr or not project_attr.strip():
        scan.add_unresolved("<Import 缺少 Project 属性>", cfg, "缺少 Project 属性")
        return []

    # Import 自身的 Condition 决定"是否导入"。为真的才继续往下读；
    # 求值不了时保守地继续读（宁可多读，不少读）。
    skip = False
    for cond in (icond,):
        try:
            if not eval_condition(cond, ctx["conf"], ctx["plat"], ctx["solution_dir"],
                                  ctx["project_dir"], ctx["project_name"]):
                skip = True
        except ConditionError as exc:
            scan.unevaluable.setdefault(str(exc), set()).add(cfg)
    if skip:
        scan.add_skipped(expand_vars(project_attr.strip(), ctx["conf"], ctx["plat"],
                                     ctx["solution_dir"], ctx["project_dir"],
                                     ctx["project_name"], ctx["project_name"]),
                         cfg, "Condition 为假，MSBuild 同样不导入")
        return []

    raw, cand = _import_candidate(project_attr, importer_dir, ctx["conf"], ctx["plat"],
                                  ctx["solution_dir"], ctx["project_dir"], ctx["project_name"])
    if not raw:
        scan.add_unresolved("<空路径>", cfg, "展开后为空")
        return []

    if depth + 1 > MAX_IMPORT_DEPTH:
        scan.add_unresolved(raw, cfg, "超过最大展开深度 " + str(MAX_IMPORT_DEPTH))
        return []

    cand_key = os.path.normcase(os.path.abspath(cand))
    if cand_key in stack:
        scan.add_cycle(raw, cfg)
        return []

    if not os.path.isfile(cand):
        if _has_var(raw):
            scan.add_unresolved(raw, cfg, "含未定义变量（SDK/用户路径），无法定位")
        else:
            scan.add_unresolved(raw, cfg, "文件不存在: " + cand)
        return []

    try:
        scan.add_resolved(cand, raw, cfg, depth + 1)
        units = list(_walk_units(cand, local_conds, ctx, scan, depth + 1,
                                 stack + (cand_key,), tree_cache))
    except ET.ParseError as exc:
        scan.add_unresolved(raw, cfg, "XML 解析失败: " + str(exc))
        return []
    except OSError as exc:
        scan.add_unresolved(raw, cfg, "读取失败: " + str(exc))
        return []
    return units


def parse_vcxproj(path, project_name, solution_dir):
    """返回 (declared_configs, models, scan)，scan 为 Import 展开统计。"""
    tree = ET.parse(path)
    root = tree.getroot()
    project_dir = os.path.dirname(os.path.abspath(path))

    declared = []
    for el in root.iter(NS + "ProjectConfiguration"):
        inc = el.get("Include")
        if inc and inc.strip() and inc.strip() not in declared:
            declared.append(inc.strip())

    unevaluable = []

    scan = ImportScan(project_name, path)

    def conds_ok(conds, cfg):
        """整个 Condition 链必须全为真；求值不了则记入 unevaluable 并视为不命中。"""
        cconf, cplat = split_cfg(cfg)
        for cond in conds:
            try:
                if not eval_condition(cond, cconf, cplat, solution_dir, project_dir, project_name):
                    return False
            except ConditionError as exc:
                unevaluable.append((cfg, str(exc)))
                scan.unevaluable.setdefault(str(exc), set()).add(cfg)
                return False
        return True

    # 每个配置独立压平 + 逐条赋值：文档顺序即"后覆盖先"，无需额外优先级规则。
    props = {cfg: {} for cfg in declared}
    ctype = {}
    implib = {}
    # 复用上面已解析的 root，避免为 16 个配置重复解析本体
    tree_cache = {os.path.normcase(os.path.abspath(path)): root}
    for cfg in declared:
        conf, plat = split_cfg(cfg)
        ctx = {
            "config": cfg, "conf": conf, "plat": plat,
            "solution_dir": solution_dir, "project_dir": project_dir,
            "project_name": project_name,
        }
        stack = (os.path.normcase(os.path.abspath(path)),)
        try:
            units = list(_walk_units(path, (), ctx, scan, 0, stack, tree_cache))
        except ET.ParseError as exc:
            scan.add_unresolved("<vcxproj 本体>", cfg, "XML 解析失败: " + str(exc))
            continue
        for conds, kind, name, value in units:
            if not conds_ok(conds, cfg):
                continue
            if not value:
                continue          # 空值不构成赋值（MSBuild 里空元素等于不清空属性）
            scan.props_read += 1
            if kind == U_PROP:
                props[cfg][name] = value
            elif kind == U_CTYPE:
                ctype[cfg] = value
            elif kind == U_IMPLIB:
                implib[cfg] = value

    models = []
    for cfg in declared:
        conf, plat = split_cfg(cfg)
        p = props.get(cfg, {})
        raw_target = p.get("TargetName", project_name)
        target = expand_vars(raw_target, conf, plat, solution_dir, project_dir, project_name, project_name)
        raw_outdir = p.get("OutDir", "")
        raw_intdir = p.get("IntDir", "")
        outdir = norm_path(expand_vars(raw_outdir, conf, plat, solution_dir, project_dir, project_name, target))
        intdir = norm_path(expand_vars(raw_intdir, conf, plat, solution_dir, project_dir, project_name, target))
        raw_implib = implib.get(cfg)
        if raw_implib:
            implib_norm = norm_path(
                expand_vars(raw_implib, conf, plat, solution_dir, project_dir, project_name, target)
            )
        elif outdir and target:
            # 未显式声明时，MSBuild 默认 $(OutDir)$(TargetName).lib
            implib_norm = outdir + "/" + target.lower() + ".lib"
            raw_implib = "<默认> $(OutDir)$(TargetName).lib"
        else:
            implib_norm = ""
            raw_implib = ""
        models.append(
            {
                "config": cfg,
                "type": ctype.get(cfg, "<未声明>"),
                "outdir": outdir,
                "intdir": intdir,
                "target": (target or "").lower(),
                "implib": implib_norm,
                "raw_outdir": raw_outdir or "<空>",
                "raw_intdir": raw_intdir or "<空>",
                "raw_target": raw_target or "<空>",
                "raw_implib": raw_implib or "<空>",
                "declared": declared,
            }
        )
    return declared, models, scan


# ---------------------------------------------------------------- 检查项


def check_solution_mapping(sln, models_by_key):
    """返回 (fails, stats)；fails 元素为 (工程, 配置, 判据, 实际值)。"""
    fails = []
    checked_active = 0
    checked_build = 0
    sol_cfgs = sln["configs"]
    for proj in sln["projects"]:
        name = proj["name"]
        key = norm_path(proj["path"])
        declared = models_by_key.get(key, {}).get("declared") or []
        declared_lower = [c.lower() for c in declared]
        mapping = sln["mapping"].get(proj["guid"], {})
        for sol_cfg in sol_cfgs:
            entry = mapping.get(sol_cfg, {})
            active = entry.get("ActiveCfg")
            build = entry.get("Build.0")
            if active is None:
                fails.append((name, sol_cfg, "solution-config-mapping", "缺少 ActiveCfg 映射"))
            else:
                checked_active += 1
                if active.lower() not in declared_lower:
                    fails.append(
                        (
                            name,
                            sol_cfg,
                            "solution-config-mapping",
                            "ActiveCfg -> " + active + "，但 " + proj["path"] + " 的 ProjectConfiguration 中不存在",
                        )
                    )
            if build is None:
                fails.append((name, sol_cfg, "solution-config-mapping", "缺少 Build.0 映射"))
            else:
                checked_build += 1
                if build.lower() not in declared_lower:
                    fails.append(
                        (
                            name,
                            sol_cfg,
                            "solution-config-mapping",
                            "Build.0 -> " + build + "，但 " + proj["path"] + " 的 ProjectConfiguration 中不存在",
                        )
                    )
    stats = {
        "projects": len(sln["projects"]),
        "configs": len(sol_cfgs),
        "active": checked_active,
        "build": checked_build,
    }
    return fails, stats


def check_static_dynamic_isolation(project_name, models):
    fails = []
    statics = [m for m in models if m["type"] == "StaticLibrary"]
    dynamics = [m for m in models if m["type"] == "DynamicLibrary"]
    if not statics or not dynamics:
        return fails, {"static": len(statics), "dynamic": len(dynamics), "skipped": True}

    dyn_implibs = {m["implib"] for m in dynamics if m["implib"]}
    dyn_outdirs = {m["outdir"] for m in dynamics if m["outdir"]}
    dyn_configs_by_implib = {}
    for m in dynamics:
        if m["implib"]:
            dyn_configs_by_implib.setdefault(m["implib"], []).append(m["config"])

    static_artifacts = {}
    for m in statics:
        if m["outdir"] and m["target"]:
            static_artifacts[m["outdir"] + "/" + m["target"] + ".lib"] = m

    for m in statics:
        if m["implib"] and m["implib"] in dyn_implibs:
            fails.append(
                (
                    project_name,
                    m["config"],
                    "static-dynamic-isolation",
                    "静态库 ImportLibrary 与动态库同名同目录: " + m["raw_implib"]
                    + " (动态侧: " + ", ".join(dyn_configs_by_implib.get(m["implib"], [])) + ")",
                )
            )
        if m["outdir"] and m["outdir"] in dyn_outdirs:
            fails.append(
                (
                    project_name,
                    m["config"],
                    "static-dynamic-isolation",
                    "静态库 OutDir 与动态库相同: " + m["raw_outdir"],
                )
            )

    for m in dynamics:
        if m["implib"] and m["implib"] in static_artifacts:
            other = static_artifacts[m["implib"]]
            fails.append(
                (
                    project_name,
                    m["config"],
                    "static-dynamic-isolation",
                    "动态导入库 " + m["raw_implib"] + " 与静态配置 " + other["config"]
                    + " 的 OutDir+TargetName 冲突 (" + other["raw_outdir"] + " + " + other["raw_target"] + ")",
                )
            )
    return fails, {"static": len(statics), "dynamic": len(dynamics), "skipped": False}


def check_intdir_uniqueness(project_name, models):
    fails = []
    seen = {}
    for m in models:
        key = m["intdir"]
        if not key:
            continue
        if key in seen:
            fails.append(
                (
                    project_name,
                    m["config"],
                    "intdir-uniqueness",
                    "IntDir=" + m["raw_intdir"] + " 与配置 " + seen[key]["config"] + " 重复 (规范化后: " + key + ")",
                )
            )
        else:
            seen[key] = m

    # Platform in IntDir: the collision check above compares configurations within one
    # platform only, so an IntDir that omits $(Platform) passes while x64 is the sole
    # platform and collides the moment a second one appears. Assert it up front.
    missing_platform = [m["config"] for m in models
                        if m["raw_intdir"] and "$(Platform)" not in m["raw_intdir"]]
    if missing_platform:
        fails.append(
            (
                project_name,
                ",".join(missing_platform),
                "intdir-platform",
                "IntDir lacks $(Platform): a second platform would collide with these",
            )
        )
    return fails, {"unique": len(seen), "total": len(models)}


def check_artifact_uniqueness(project_name, models):
    fails = []
    seen = {}
    for m in models:
        if not m["target"]:
            continue
        key = m["outdir"] + "/" + m["target"]
        if key in seen:
            fails.append(
                (
                    project_name,
                    m["config"],
                    "artifact-uniqueness",
                    "OutDir+TargetName=" + m["raw_outdir"] + " + " + m["raw_target"]
                    + " 与配置 " + seen[key]["config"] + " 重复 (规范化后: " + key + ")",
                )
            )
        else:
            seen[key] = m
    return fails, {"unique": len(seen), "total": len(models)}


# ---------------------------------------------------------------- 自测


def run_self_test():
    """内存注入一个已知违规样本，断言检查器能报出。不写盘、不碰真实工程文件。"""
    injected = [
        {
            "config": "InjectedA|x64",
            "type": "DynamicLibrary",
            "outdir": norm_path("C:/inject/Bin/x64"),
            "intdir": norm_path("C:/inject/Temp/A"),
            "target": "duilib",
            "implib": norm_path("C:/inject/Bin/x64/duilib.lib"),
            "raw_outdir": "$(SolutionDir)Bin$(Platform)" + BSLASH,
            "raw_intdir": "$(SolutionDir)Temp" + BSLASH + "A" + BSLASH,
            "raw_target": "DuiLib",
            "raw_implib": "$(SolutionDir)Bin$(Platform)" + BSLASH + "DuiLib.lib",
        },
        {
            "config": "InjectedB|x64",
            "type": "StaticLibrary",
            "outdir": norm_path("C:/inject/Bin/x64"),
            "intdir": norm_path("C:/inject/Temp/B"),
            "target": "duilib",
            "implib": norm_path("C:/inject/Bin/x64/duilib.lib"),
            "raw_outdir": "$(SolutionDir)Bin$(Platform)" + BSLASH,
            "raw_intdir": "$(SolutionDir)Temp" + BSLASH + "B" + BSLASH,
            "raw_target": "DuiLib",
            "raw_implib": "$(SolutionDir)Bin$(Platform)" + BSLASH + "DuiLib.lib",
        },
        {   # 负向对照：这个配置不应被判为重复
            "config": "InjectedC|x64",
            "type": "DynamicLibrary",
            "outdir": norm_path("C:/inject/Bin/x64"),
            "intdir": norm_path("C:/inject/Temp/C"),
            "target": "duilib_other",
            "implib": norm_path("C:/inject/Bin/x64/duilib_other.lib"),
            "raw_outdir": "$(SolutionDir)Bin$(Platform)" + BSLASH,
            "raw_intdir": "$(SolutionDir)Temp" + BSLASH + "C" + BSLASH,
            "raw_target": "DuiLibOther",
            "raw_implib": "$(SolutionDir)Bin$(Platform)" + BSLASH + "DuiLibOther.lib",
        },
    ]

    art_fails, _ = check_artifact_uniqueness("InjectedProject", injected)
    iso_fails, _ = check_static_dynamic_isolation("InjectedProject", injected)

    reported_configs = {f[1] for f in art_fails} | {f[1] for f in iso_fails}
    detected = "InjectedB|x64" in reported_configs and "InjectedA|x64" in reported_configs
    over_reported = "InjectedC|x64" in reported_configs

    ok = detected and not over_reported
    print("[SELFTEST] 注入样本: 两个配置 OutDir+TargetName 完全相同 -> 判据 artifact-uniqueness")
    if not ok:
        print("[FAIL] self-test: 注入的 OutDir+TargetName 重复样本未被报出 (检出="
              + str(sorted(reported_configs)) + ", 误报=" + str(over_reported) + ")")
    else:
        for f in art_fails:
            print("[SELFTEST] 命中: " + f[1] + " " + f[2] + ": " + f[3])
        print("[PASS] self-test: 注入的重复产物路径被 artifact-uniqueness 报出，且未误报对照配置")
    return ok


def run_import_self_test():
    """在临时目录构造 vcxproj + .props，验证 Import 链展开真的生效。

    样本（全部写在 tempfile.mkdtemp() 里，不落仓库）：
      clean.vcxproj  无 Import，两配置 OutDir 各不同        -> 期望 0 冲突
      dirty.vcxproj  Import shared.props，props 无条件覆盖 OutDir 为同一值
                                                          -> 期望报出 artifact-uniqueness
      order.vcxproj  Import 之后本体再给 OutDir（后覆盖先）  -> 期望 0 冲突
    """
    import tempfile

    tmp = tempfile.mkdtemp(prefix="vmatrix_import_")
    nl = chr(10)
    try:
        head = (
            '<?xml version="1.0" encoding="utf-8"?>' + nl
            + '<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">' + nl
            + '  <ItemGroup Label="ProjectConfigurations">' + nl
        )
        head += (
            '    <ProjectConfiguration Include="Alpha|x64">' + nl
            + '<Configuration>Alpha</Configuration><Platform>x64</Platform>' + nl
            + '    </ProjectConfiguration>' + nl
        )
        head += (
            '    <ProjectConfiguration Include="Beta|x64">' + nl
            + '<Configuration>Beta</Configuration><Platform>x64</Platform>' + nl
            + '    </ProjectConfiguration>' + nl
        )
        head += '  </ItemGroup>' + nl

        def cfg_group(name, outdir):
            cond = "'$(Configuration)|$(Platform)'=='" + name + "|x64'"
            text = (
                '  <PropertyGroup Condition="' + cond + '" Label="Configuration">' + nl
                + '    <ConfigurationType>DynamicLibrary</ConfigurationType>' + nl
                + '  </PropertyGroup>' + nl
                + '  <PropertyGroup Condition="' + cond + '">' + nl
                + '    <IntDir>$(SolutionDir)Temp' + BSLASH + name + BSLASH + '$(Platform)' + BSLASH + '</IntDir>' + nl
                + '    <TargetName>shared</TargetName>' + nl
            )
            if outdir is not None:
                text += '    <OutDir>' + outdir + '</OutDir>' + nl
            return text + '  </PropertyGroup>' + nl

        # props：无条件 PropertyGroup 覆盖 OutDir —— 对所有配置生效，正是 U3 要覆盖的场景
        props = (
            '<?xml version="1.0" encoding="utf-8"?>' + nl
            + '<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">' + nl
            + '  <PropertyGroup>' + nl
            + '    <OutDir>$(SolutionDir)Collide/</OutDir>' + nl
            + '  </PropertyGroup>' + nl
            + '</Project>' + nl
        )
        imp = '  <Import Project="shared.props" />' + nl
        tail = '</Project>' + nl

        files = {
            "clean.vcxproj": head + cfg_group("Alpha", "Bin/Alpha/")
                             + cfg_group("Beta", "Bin/Beta/") + tail,
            "dirty.vcxproj": head + imp + cfg_group("Alpha", None)
                             + cfg_group("Beta", None) + tail,
            "order.vcxproj": head + imp + cfg_group("Alpha", "Bin/Alpha/")
                             + cfg_group("Beta", "Bin/Beta/") + tail,
        }
        files["shared.props"] = props
        for fname, text in files.items():
            with open(os.path.join(tmp, fname), "w", encoding="utf-8",
                      newline=chr(10)) as fh:
                fh.write(text)

        def run_one(fname):
            declared, models, scan = parse_vcxproj(os.path.join(tmp, fname), fname, tmp)
            art, _ = check_artifact_uniqueness(fname, models)
            intd, _ = check_intdir_uniqueness(fname, models)
            return declared, models, scan, art, intd

        d_clean, m_clean, s_clean, a_clean, i_clean = run_one("clean.vcxproj")
        d_dirty, m_dirty, s_dirty, a_dirty, i_dirty = run_one("dirty.vcxproj")
        d_order, m_order, s_order, a_order, i_order = run_one("order.vcxproj")

        def outdirs(models):
            return [m["outdir"] for m in models]

        print("[SELFTEST] import 注入: 临时目录 " + tmp)
        print("[SELFTEST]   clean: " + str(len(d_clean)) + " 配置, OutDir=" + str(outdirs(m_clean))
              + ", 展开 import " + str(len(s_clean.resolved)) + " 个")
        print("[SELFTEST]   dirty: OutDir=" + str(outdirs(m_dirty))
              + ", 展开 import " + str(len(s_dirty.resolved)) + " 个, 报出冲突 " + str(len(a_dirty)) + " 条")
        print("[SELFTEST]   order: OutDir=" + str(outdirs(m_order))
              + ", 展开 import " + str(len(s_order.resolved)) + " 个, 报出冲突 " + str(len(a_order)) + " 条")

        ok = True
        if a_clean or i_clean:
            ok = False
            print("[FAIL] self-test(import): 干净样本误报 " + str(a_clean + i_clean))
        if len(s_dirty.resolved) != 1:
            ok = False
            print("[FAIL] self-test(import): dirty 样本未展开 shared.props（展开数="
                  + str(len(s_dirty.resolved)) + ")")
        if not a_dirty:
            ok = False
            print("[FAIL] self-test(import): props 覆盖 OutDir 造成的产物路径冲突未被报出")
        if len(s_order.resolved) != 1:
            ok = False
            print("[FAIL] self-test(import): order 样本未展开 shared.props")
        if a_order:
            ok = False
            print("[FAIL] self-test(import): 后覆盖先失效——Import 之后的本体 OutDir 没能压过 props")
        if ok:
            for f in a_dirty:
                print("[SELFTEST] 命中: " + f[1] + " " + f[2] + ": " + f[3][:140])
            print("[PASS] self-test(import): props 覆盖 OutDir 被展开并报出；干净样本不误报；"
                  "Import 之后的本体属性正确压过 props")
        return ok
    finally:
        for cur, dirs, fnames in os.walk(tmp, topdown=False):
            for fn in fnames:
                try:
                    os.remove(os.path.join(cur, fn))
                except OSError:
                    pass
            for dn in dirs:
                try:
                    os.rmdir(os.path.join(cur, dn))
                except OSError:
                    pass
        try:
            os.rmdir(tmp)
        except OSError:
            pass


# ---------------------------------------------------------------- 主流程


def main(argv=None):
    default_root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    parser = argparse.ArgumentParser(description="DuiLib 构建配置矩阵校验门禁（不执行构建）")
    parser.add_argument("--root", default=default_root, help="仓库根目录（默认自动推断）")
    parser.add_argument("--inject", action="store_true", help="额外运行一次内存注入自测")
    args = parser.parse_args(argv)

    root = os.path.abspath(args.root)
    sln_path = os.path.join(root, "DuiLib.sln")

    print("[SCAN] root = " + root)

    self_test_ok = True
    if args.inject:
        self_test_ok = run_self_test() and run_import_self_test()

    fails = []
    fatal = []

    if not os.path.isfile(sln_path):
        fatal.append(("DuiLib.sln", "-", "solution-input", "文件不存在: " + sln_path))
        for item in fatal:
            print("[FAIL] " + item[0] + " " + item[1] + " " + item[2] + ": " + item[3])
        print("[RESULT] " + str(len(fatal)) + " failed")
        print("[NOTE] 输入缺失，退出码 2；零输入不报 PASS")
        return 2

    sln = parse_sln(sln_path)
    if not sln["configs"]:
        fatal.append(("DuiLib.sln", "-", "solution-input", "SolutionConfigurationPlatforms 为空或无法解析"))
    if not sln["projects"]:
        fatal.append(("DuiLib.sln", "-", "solution-input", "未解析到任何 Project 条目"))

    models_by_key = {}
    parsed_files = 0
    scans = []          # [ImportScan]，每工程一个
    for proj in sln["projects"]:
        vcx = os.path.join(root, proj["path"].replace(BSLASH, os.sep))
        if not os.path.isfile(vcx):
            fatal.append((proj["name"], "-", "project-input", "文件不存在: " + proj["path"]))
            continue
        try:
            declared, models, scan = parse_vcxproj(vcx, proj["name"], root)
        except ET.ParseError as exc:
            fatal.append((proj["name"], "-", "project-input", "XML 解析失败: " + str(exc)))
            continue
        except OSError as exc:
            fatal.append((proj["name"], "-", "project-input", "读取失败: " + str(exc)))
            continue
        parsed_files += 1
        scans.append(scan)
        models_by_key[norm_path(proj["path"])] = {"declared": declared, "models": models, "scan": scan}

    imp_stats = summarise_imports(scans)
    print(
        "[SCAN] 读取 1 个 sln（解决方案配置 " + str(len(sln["configs"]))
        + " 个，工程 " + str(len(sln["projects"])) + " 个），"
        + "读取 " + str(parsed_files) + " 个 vcxproj"
    )
    print(
        "[SCAN] import-expansion: <Import> 元素命中 " + str(imp_stats["imports_seen"])
        + " 次（16 配置累计），已展开文件 " + str(imp_stats["resolved_files"])
        + " 个，未展开 " + str(imp_stats["unresolved_files"])
        + " 个（其中环境/SDK 路径 " + str(imp_stats["env_files"])
        + "，仓库内缺失路径 " + str(imp_stats["local_files"]) + "），"
        + "Condition 为假跳过 " + str(imp_stats["skipped_files"])
        + " 个，循环截断 " + str(imp_stats["cycle_files"]) + " 个，"
        + "最大展开深度 " + str(imp_stats["max_depth"])
        + "；生效属性赋值 " + str(imp_stats["props_read"]) + " 条"
    )

    if fatal or parsed_files == 0:
        for item in fatal:
            print("[FAIL] " + item[0] + " " + item[1] + " " + item[2] + ": " + item[3])
        if parsed_files == 0 and not fatal:
            print("[FAIL] - - vcxproj-input: 未解析到任何 vcxproj")
        total = len(fatal) if fatal else 1
        print("[RESULT] " + str(total) + " failed")
        print("[NOTE] 输入缺失/无法解析，退出码 2；零输入不报 PASS")
        return 2

    # 1) 解决方案配置完整性
    map_fails, map_stats = check_solution_mapping(sln, models_by_key)
    fails.extend(map_fails)

    # 2)~4) 逐工程检查
    iso_stats = []
    intdir_stats = []
    art_stats = []
    for proj in sln["projects"]:
        info = models_by_key.get(norm_path(proj["path"]))
        if not info:
            continue
        name = proj["name"]
        models = info["models"]
        f2, s2 = check_static_dynamic_isolation(name, models)
        f3, s3 = check_intdir_uniqueness(name, models)
        f4, s4 = check_artifact_uniqueness(name, models)
        fails.extend(f2)
        fails.extend(f3)
        fails.extend(f4)
        iso_stats.append((name, s2))
        intdir_stats.append((name, s3))
        art_stats.append((name, s4))

    # Import 未展开的覆盖缺口：仓库内具体路径但文件不存在 —— 这是真缺陷（MSBuild 会 MSB4019）
    fails.extend(collect_import_fails(scans))

    # 输出失败明细
    for f in fails:
        print("[FAIL] " + f[0] + " " + f[1] + " " + f[2] + ": " + f[3])

    # 输出 Import 未展开告警（环境/SDK 路径等，属于检查覆盖度不足，不是工程缺陷）
    for scan in scans:
        for display, info in sorted(scan.unresolved_env().items()):
            print("[WARN] " + scan.project_name + " import(s) not resolved: " + display
                  + "  [影 响 配 置] " + str(len(info["configs"])) + " 个")
        for display, info in sorted(scan.skipped.items()):
            print("[INFO] " + scan.project_name + " import 按 Condition 跳过: " + display)
        for cond, cfgs in sorted(scan.unevaluable.items()):
            print("[WARN] " + scan.project_name + " condition 无法求值（已算作不命中，可能漏判）: "
                  + cond + " [影响配置] " + str(len(cfgs)) + " 个")

    # 输出通过项（仅当该判据无失败时）
    map_ok = not any(f[2] == "solution-config-mapping" for f in fails)
    if map_ok:
        print(
            "[PASS] solution-config-mapping: " + str(map_stats["projects"]) + " projects x "
            + str(map_stats["configs"]) + " configs (" + str(map_stats["active"])
            + " ActiveCfg + " + str(map_stats["build"]) + " Build.0 全部存在且映射目标真实存在)"
        )

    for crit, entries in (
        ("static-dynamic-isolation", iso_stats),
        ("intdir-uniqueness", intdir_stats),
        ("artifact-uniqueness", art_stats),
    ):
        crit_fails = [f for f in fails if f[2] == crit]
        if crit_fails:
            continue
        parts = []
        for name, st in entries:
            if crit == "static-dynamic-isolation":
                if st.get("skipped"):
                    parts.append(name + ": 无静态/动态混合，跳过(" + str(st["static"]) + "S/" + str(st["dynamic"]) + "D)")
                else:
                    parts.append(name + ": " + str(st["static"]) + "S/" + str(st["dynamic"]) + "D 隔离成立")
            else:
                parts.append(name + ": " + str(st["unique"]) + "/" + str(st["total"]) + " 唯一")
        print("[PASS] " + crit + ": " + "; ".join(parts))

    # 覆盖度汇总：明确区分"检查通过"与"因未展开而可能漏判"
    warn_count = sum(len(s.unresolved_env()) for s in scans)
    local_count = sum(len(s.unresolved_local()) for s in scans)
    cond_count = sum(len(s.unevaluable) for s in scans)
    print(
        "[COVERAGE] import-expansion: 已展开 " + str(imp_stats["resolved_files"])
        + " 个导入文件 / 未展开 " + str(imp_stats["unresolved_files"])
        + " 个（环境-SDK " + str(imp_stats["env_files"])
        + "，仓库内缺失 " + str(imp_stats["local_files"]) + "）"
        + "，最大深度 " + str(imp_stats["max_depth"])
    )
    if imp_stats["resolved_files"] == 0:
        print("[COVERAGE] 警告: 本次 0 个 Import 被真实展开——不存在的 .props 覆盖无法被检查，"
              "上述 PASS 仅覆盖 .vcxproj 本体属性")
    if local_count:
        print("[COVERAGE] " + str(local_count) + " 个仓库内 Import 目标缺失，已计入 FAIL（import-unresolved）")
    if warn_count or cond_count:
        print("[COVERAGE] " + str(warn_count) + " 个环境/SDK Import 未展开、"
              + str(cond_count) + " 条 Condition 无法求值：这些位置若覆盖 "
              + "/".join(CFG_PROP_NAMES) + " 或 ImportLibrary，本检查器看不见")

    print("[RESULT] " + str(len(fails)) + " failed")
    if not self_test_ok:
        print("[NOTE] --inject 自测未报出，退出码 2")
        return 2
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
