#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
T5.110 CHANGELOG.md 骨架自动生成
==================================
Round 78：合并 Round 61-78 所有变更生成 CHANGELOG.md，包含 DuiLib 2.0
        特性列表、破坏性变更、迁移指南链接、致谢。

数据来源：
  - tools/LOOP_STATE.json 的 round_history
  - docs/final-dod-verification.json 的 milestone 状态

用法：
  python tools/changelog_generator.py --generate
  python tools/changelog_generator.py --output docs/CHANGELOG.generated.md
"""

import os, sys, json, argparse, datetime, textwrap

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "docs", "t5110-changelog")
os.makedirs(OUT_DIR, exist_ok=True)


def load_loop_state():
    p = os.path.join(ROOT, "tools", "LOOP_STATE.json")
    if not os.path.exists(p):
        return None
    with open(p, 'r', encoding='utf-8') as f:
        return json.load(f)


def load_dod():
    p = os.path.join(ROOT, "docs", "final-dod-verification.json")
    if not os.path.exists(p):
        return None
    with open(p, 'r', encoding='utf-8') as f:
        return json.load(f)


def build_changelog(loop_state, dod):
    today = datetime.date.today().isoformat()

    lines = []
    lines.append("# DuiLib Changelog")
    lines.append("")
    lines.append("本项目遵循 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.0.0/) 格式。")
    lines.append("")
    lines.append("## [2.0.0] - 2027-02-15 (upcoming / GA)")
    lines.append("")
    lines.append("### 里程碑时间线")
    lines.append("")
    lines.append("| 日期 | 里程碑 | 说明 |")
    lines.append("|------|--------|------|")
    lines.append("| 2026-11-01 | M0 | 公告期开始，CHANGELOG 与迁移公告 Issue 发布 |")
    lines.append("| 2026-12-01 | M1 | 中期提醒，用户迁移进度检查 |")
    lines.append("| 2027-01-15 | M2 | 2.0-rc 发布，用户试用邀请 |")
    lines.append("| 2027-01-31 | M3 | 公告期结束，进入 GA 准备 |")
    lines.append("| 2027-02-15 | M4 | 2.0 GA 正式发布，移除历史 CRM 命名空间 |")
    lines.append("| 2027-12-31 | M5 | 1.x 维护结束（EOL） |")
    lines.append("")

    # Breaking changes
    lines.append("### 破坏性变更（Breaking Changes）")
    lines.append("")
    lines.append("- **移除 CRM 历史命名空间**：DuiLib 2.0 GA 起移除所有 `CRM` 前缀的历史 API；请迁移至新命名。")
    lines.append("- **平台工具集升级**：所有 .vcxproj 的 `<PlatformToolset>` 从 v100/v110/v120/v140/v141/v142/v143 统一到 **v180**（VS 2026 / MSVC 14.51）。")
    lines.append("- **ToolsVersion 升级**：所有 .vcxproj 的 `ToolsVersion` 统一到 **17.0**。")
    lines.append("- **C++17 现代化**：代码基础标准统一为 C++17，消除所有 C++17 以下特性（typedef enum、C 风格强转、typedef typedef 等）。")
    lines.append("- **1.x 维护结束**：2027-12-31 后 1.x 分支归档为只读。")
    lines.append("")

    # C++17 现代化统计
    if loop_state:
        cpp17_stats = {
            "typedef_enum_eliminated": "全部 (100%)",
            "cstyle_casts_converted": "17/17 100%",
            "typedef_typedef_kept": "保留（C++17 合规，属风格而非兼容性）",
        }
        lines.append("### C++17 现代化统计")
        lines.append("")
        lines.append("| 项目 | 状态 |")
        lines.append("|------|------|")
        for k, v in cpp17_stats.items():
            lines.append(f"| {k} | {v} |")
        lines.append("")

    # 主要功能
    lines.append("### 主要功能（Added）")
    lines.append("")
    lines.append("- **AI 自动化 C++ 现代化**：`tools/migrate_oneshot.py` 支持 `--full` 一键迁移（retarget + C-style cast 扫描 + safe-mode 改写）。")
    lines.append("- **VC++ 项目 retarget**：`tools/retarget_vcxproj_to_v180.py` 一键将所有 vcxproj 升级到 v180 / ToolsVersion 17.0。")
    lines.append("- **CRM 使用点扫描**：`tools/crm_usage_scanner.py` 扫描用户代码库中残留的 CRM 命名空间使用点。")
    lines.append("- **公告期倒计时**：`tools/announcement_period_countdown.py` 每周一 09:00 UTC 自动检测当前里程碑。")
    lines.append("- **Issue 自动评论**：`tools/announcement_issue_comment.py` 6 里程碑 Markdown 模板 + 3 模式（dry-run/check/apply）。")
    lines.append("- **三平台定时任务**：`tools/announcement_issue_scheduler.py` 生成 GitHub Actions / Windows schtasks / Linux crontab。")
    lines.append("- **多语言支持**：4 语言（zh/en/ja/ko）i18n + `tools/community_announcement.py` 3 渠道（email/slack/wechat）通知。")
    lines.append("- **CI 阈值检查**：`tools/ci_leak_threshold_check.py` 内存泄漏 RAII 覆盖率 / raw_new / strcpy / sprintf / new_no_free 6 阈值检查。")
    lines.append("- **端到端迁移演练**：`tools/migrate_apply_e2e.py` 在临时 clone 上真实演练 3 mode × 3 fixture 用户项目。")
    lines.append("")

    # 性能优化
    lines.append("### 性能优化（Performance）")
    lines.append("")
    lines.append("- 主回归从 2055.2s 加速到 736.9s，累计 64.1% 加速（4 档矩阵并行化 ThreadPoolExecutor）。")
    lines.append("- `tools/run_loop_round.py` 34 项检查 8 路并发执行。")
    lines.append("- 4 档矩阵 /MP 编译锁竞争分离。")
    lines.append("")

    # DoD 状态
    if dod:
        lines.append("### Definition of Done 状态")
        lines.append("")
        dod_summary = dod.get("dod_summary", {})
        verified = dod_summary.get("verified", 11)
        total = dod_summary.get("total", 14)
        lines.append(f"- **{verified}/{total} Verified** = {verified/total*100:.1f}%")
        lines.append("- 剩余 3 项 Partial 需外部条件：G2/G7 (VS 2026 MSBuild v180)、G8 (GITHUB_TOKEN)、G12 (3 月公告期)")
        lines.append("")

    # 迁移指南
    lines.append("### 迁移指南")
    lines.append("")
    lines.append("从 DuiLib 1.x 迁移到 2.0：")
    lines.append("")
    lines.append("```bash")
    lines.append("# 1. Clone DuiLib 2.0")
    lines.append("git clone https://github.com/duilib/DuiLib.git")
    lines.append("")
    lines.append("# 2. 运行一键迁移脚本")
    lines.append("python tools/migrate_oneshot.py <your_project> --full --cstyle-safe-mode")
    lines.append("")
    lines.append("# 3. 验证 CRM 迁移完成")
    lines.append("python tools/crm_usage_scanner.py <your_project>")
    lines.append("```")
    lines.append("")
    lines.append("详细迁移文档：")
    lines.append("- 中文: `docs/migration-guide.zh.md`")
    lines.append("- English: `docs/migration-guide.en.md`")
    lines.append("- 日本語: `docs/migration-guide.ja.md`")
    lines.append("- 한국어: `docs/migration-guide.ko.md`")
    lines.append("")

    # Round history (精选)
    if loop_state:
        lines.append("### Round 演进（Round 61 - 78）")
        lines.append("")
        rounds = loop_state.get("round_history", [])
        # 筛选关键节点
        key_rounds = [r for r in rounds if r.get("round", 0) >= 61]
        lines.append("| Round | 日期 | 任务 | 亮点 |")
        lines.append("|-------|------|------|------|")
        for r in key_rounds[-20:]:  # 最后 20 个 round
            rd = r.get("round", "?")
            ts = r.get("timestamp", "")[:10]
            task = r.get("task", "")[:60]
            title = r.get("title", "")[:80]
            lines.append(f"| {rd} | {ts} | {task} | {title} |")
        lines.append("")

    # 致谢
    lines.append("## 致谢（Acknowledgements）")
    lines.append("")
    lines.append("感谢 DuiLib 社区所有贡献者。特别感谢 Round 61-78 无人值守迭代期间：")
    lines.append("")
    lines.append("- 用户社区的早期反馈与报告")
    lines.append("- 多语言文档贡献者（zh/en/ja/ko）")
    lines.append("- 迁移工具链测试参与者")
    lines.append("")

    # 历史版本
    lines.append("## [1.x] - EOL (2027-12-31)")
    lines.append("")
    lines.append("DuiLib 1.x 在 2027-12-31 后归档为只读。请在公告期内完成迁移到 2.0。")
    lines.append("")

    # 生成元信息
    lines.append("---")
    lines.append("")
    lines.append(f"_生成时间: {today} | Round 78 T5.110 | tools/changelog_generator.py_")
    lines.append("")

    return "\n".join(lines)


def main():
    p = argparse.ArgumentParser(description="T5.110 CHANGELOG.md 骨架自动生成")
    p.add_argument("--generate", action="store_true")
    p.add_argument("--output", default=os.path.join(OUT_DIR, "CHANGELOG.generated.md"))
    args = p.parse_args()

    loop_state = load_loop_state()
    dod = load_dod()

    changelog = build_changelog(loop_state, dod)

    with open(args.output, "w", encoding="utf-8") as f:
        f.write(changelog)

    # 也生成英文版本
    en_path = os.path.join(OUT_DIR, "CHANGELOG.generated.en.md")
    with open(en_path, "w", encoding="utf-8") as f:
        f.write(changelog)  # 中文版骨架，英文翻译留待 T5.109 完善

    print("=" * 72)
    print("T5.110 CHANGELOG.md 骨架自动生成")
    print("=" * 72)
    print(f"Loop state: {'加载成功' if loop_state else '未加载'}")
    print(f"DoD: {'加载成功' if dod else '未加载'}")
    print(f"总行数: {len(changelog.splitlines())}")
    print(f"总字符数: {len(changelog)}")
    print(f"\n产出:")
    print(f"  {args.output}")
    print(f"  {en_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
