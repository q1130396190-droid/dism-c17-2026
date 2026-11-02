"""皮肤 DLL 部署门禁。

为什么需要这个门禁
------------------
程序启动时 CFrameWnd::ReadSkinFromDll() 会尝试加载 SkinThemeDll.dll。
加载失败时它会**静默回退**到磁盘皮肤 bin64/skin/Win8Metro/ —— 这是有意的
降级设计（崩溃面为零），但带来一个发布风险：

  漏拷 SkinThemeDll.dll 时，程序照常启动、界面照常显示（走磁盘皮肤），
  没有任何报警。等到磁盘皮肤也被清理，才会突然整页空白，且无从判断原因。

所以"能不能回退"和"该不该回退"是两件事：回退保证不崩，门禁保证不漏发。

判据
----
1. bin64/amd64/SkinThemeDll.dll 必须存在。
2. 存在时必须可加载，且 SkinTheme_ABI() == 1、SkinTheme_Count() > 0。
   0 个文件等于空皮肤包：加载成功但界面全空白，比加载失败更糟。
3. Debug 版 _d 仅在 bin64/Win8Metro_d.exe 同时存在时要求；否则记 SKIP
   并单独计数 —— SKIP 不算通过，也不能混进通过数里。

退出码：0 干净，1 发现问题，2 输入问题，3 --inject 未报出。
"""
# -*- coding: utf-8 -*-

from __future__ import annotations

import argparse
import ctypes
import glob
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BIN = os.path.join(ROOT, "bin64")
AMD = os.path.join(BIN, "amd64")

EXPECTED_ABI = 1

# (路径, 标签, 是否仅在对应配置的主程序存在时才要求)
TARGETS = [
    (os.path.join(AMD, "SkinThemeDll.dll"), "Release", None),
    (os.path.join(AMD, "SkinThemeDll_d.dll"), "Debug",
     os.path.join(BIN, "Win8Metro_d.exe")),
]


def probe(path):
    """加载 DLL 并读导出。返回 (ok, detail)。

    detail 明确区分三种失败：
      · 文件不存在        -> "missing"
      · LoadLibrary 失败  -> 原始异常文本（多为缺 VC 运行时）
      · 导出值不对        -> ABI / 文件数
    """
    if not os.path.isfile(path):
        return False, "missing"

    try:
        # 传绝对路径，避免受当前工作目录与 PATH 搜索顺序影响。
        dll = ctypes.WinDLL(os.path.abspath(path))
    except OSError as e:
        # 缺依赖（VC 运行时/其它 DLL）时会走到这里，与"没部署"是两回事。
        return False, "load-failed: %s" % e

    try:
        dll.SkinTheme_ABI.restype = ctypes.c_int
        dll.SkinTheme_Count.restype = ctypes.c_int
        abi = dll.SkinTheme_ABI()
        count = dll.SkinTheme_Count()
    except AttributeError as e:
        return False, "missing-export: %s" % e

    if abi != EXPECTED_ABI:
        return False, "abi=%d (expect %d)" % (abi, EXPECTED_ABI)
    if count <= 0:
        # 空包：加载成功但界面会全空白，必须当失败处理。
        return False, "entries=%d (empty pack)" % count
    return True, "abi=%d entries=%d" % (abi, count)


def xml_digest_in_dll(path):
    """从 DLL 里取出皮肤 XML 的"内容指纹"，与磁盘皮肤比对。

    为什么需要这一层
    ----------------
    只查 ABI 和条目数会漏掉最危险的一种状态：DLL 存在、可加载、ABI 正确、
    条目数正确，但里面打包的是**旧版本的 XML**。

    本轮实测就撞上了这个：给 btn_theme 加 borderround 后重新生成了 pack，
    Release DLL 重建了，Debug DLL 没有 —— 而 check_skin_dll_deploy 对两个
    都报 PASS。原因是它只验证"能加载、条目数对"，完全看不到内容差异。
    这种陈旧打包在运行时的表现是"改了皮肤但界面没变"，极难定位。

    做法：把磁盘上每份 .xml 的字节读取出来，在 DLL 的只读数据段里
    逐份查找。全部命中才算同步。为避免把大文件塞进比较开销，只比对
    每份文件完整字节（皮肤 XML 都是几十 KB，47 份也不过 1MB 量级）。
    """
    skin_dir = os.path.join(BIN, "skin", "Win8Metro")
    if not os.path.isdir(skin_dir):
        return None, "skin source dir missing: %s" % skin_dir
    if not os.path.isfile(path):
        return None, "dll missing"

    blob = open(path, "rb").read()
    # 递归：页面皮肤在 skin/Win8Metro/pages/ 下，只扫顶层会漏掉 17/19 份，
    #   而那 17 份正是最常改的页面 XML —— 漏扫等于这道检查基本没用。
    xmls = sorted(glob.glob(os.path.join(skin_dir, "**", "*.xml"), recursive=True))
    if not xmls:
        return None, "no .xml in skin source dir"

    stale = []
    for x in xmls:
        data = open(x, "rb").read()
        if data not in blob:
            stale.append((os.path.relpath(x, skin_dir), len(data)))
    return stale, "%d/%d xml in sync" % (len(xmls) - len(stale), len(xmls))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--inject", action="store_true",
                    help="temporarily rename the DLL; the gate must report it")
    ap.add_argument("--skip-content", action="store_true",
                    help="only check presence/ABI (use when skin dir is not the source)")
    args = ap.parse_args()

    if not os.path.isdir(BIN):
        print("[FAIL] bin64 not found under " + ROOT)
        return 2

    backups = []   # [(原路径, 备份路径)]，finally 里无条件还原
    if args.inject:
        target = TARGETS[0][0]
        if not os.path.isfile(target):
            print("[FAIL] inject: target DLL absent, cannot self-test")
            return 2
        bak = target + ".__inject_bak"
        try:
            os.rename(target, bak)
        except OSError as e:
            print("[FAIL] inject: rename failed: %s" % e)
            return 2
        backups.append((target, bak))

    passed = failed = skipped = 0
    problems = []
    try:
        for path, label, gate_on in TARGETS:
            rel = os.path.relpath(path, ROOT)
            ok, detail = probe(path)

            if ok:
                print("[PASS] %-6s %-42s %s" % (label, rel, detail))

                # 内容同步：能加载不等于装的是新皮肤。
                if not args.skip_content:
                    stale, note = xml_digest_in_dll(path)
                    if stale is None:
                        print("[WARN] %-6s content check skipped: %s" % (label, note))
                    elif stale:
                        print("[FAIL] %-6s stale pack: %d xml differ from %s"
                              % (label, len(stale), "bin64/skin/Win8Metro"))
                        for nm, sz in stale[:6]:
                            print("       stale: %s (%d bytes)" % (nm, sz))
                        print("       rebuild: python tools/gen_skin_pack.py ... then "
                              "MSBuild Demos/SkinThemeDll/SkinThemeDll.vcxproj /t:Rebuild")
                        problems.append((label, rel,
                                         "%d xml out of sync with the on-disk skin"
                                         % len(stale)))
                        failed += 1
                        continue
                    else:
                        print("       %s" % note)

                passed += 1
                continue

            # 未要求部署的 Debug 包：记 SKIP，不进通过数。
            if detail == "missing" and gate_on is not None and not os.path.exists(gate_on):
                print("[SKIP] %-6s %-42s not required (%s absent)"
                      % (label, rel, os.path.basename(gate_on)))
                skipped += 1
                continue

            if detail == "missing":
                problems.append((label, rel,
                                 "not deployed -- the app will silently fall back "
                                 "to the on-disk skin; nothing warns the user"))
            elif detail.startswith("load-failed"):
                problems.append((label, rel, "cannot be loaded: " + detail))
            elif detail.startswith("missing-export"):
                problems.append((label, rel, "does not export the skin API: " + detail))
            else:
                problems.append((label, rel, "unusable pack: " + detail))
            failed += 1
    finally:
        for orig, bak in backups:
            try:
                if os.path.exists(bak):
                    os.replace(bak, orig)
            except OSError as e:
                # 还原失败必须大声报出来：留着备份名等于发布包里永久缺 DLL。
                print("[FATAL] failed to restore %s: %s" % (orig, e))

    print("[INFO] passed=%d failed=%d skipped=%d" % (passed, failed, skipped))

    # 自证：注入后必须精确报出"未部署"，而不是"有东西被报了"。
    if args.inject:
        hit = [p for p in problems if "not deployed" in p[2]]
        print("[INFO] inject: reported %d 'not deployed' problem(s) (expect 1)" % len(hit))
        if len(hit) != 1:
            print("[FAIL] inject: renaming the DLL was not reported -- the gate is blind")
            return 3

    if problems:
        print("[FAIL] %d skin DLL problem(s):" % len(problems))
        for label, rel, why in problems:
            print("       %-6s %-42s %s" % (label, rel, why))
        print("       SkinThemeDll.dll ships with the app; rebuild it from "
              "Demos/SkinThemeDll/SkinThemeDll.vcxproj after regenerating the packs")
        return 1

    print("[PASS] every required skin DLL is present and loadable")
    return 0


if __name__ == "__main__":
    sys.exit(main())
