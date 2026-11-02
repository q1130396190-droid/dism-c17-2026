#pragma once

// ============================================================
// CommandLineService —— R-B3 CLI 参数层
// ------------------------------------------------------------
// 解析命令行参数并把动作映射到已存在的页面/工具能力。
// 头文件仅依赖 windows.h + CDuiString，不引 UIlib.h，与
// SystemInfoService 同风格，可独立编译测试。
//
// 支持参数（大小写不敏感，/ 或 - 开头均可）：
//   /C            启动后自动执行清理（要求 GUI 已加载）
//   /exe=<alg>    设置导出压缩算法：xpress4k|xpress8k|xpress16k|lzx|max
//   /U            无人值守模式（危险确认自动通过）
//   /ScratchDir=  设置临时工作目录
//   /Image=       打开指定 WIM/ESD（切到 ImageX 页）
//   /All          清理时全选所有规则
//   /Plugin=      插件目录（本次仅记录到日志）
//   /CompactOS    清理页执行 CompactOS 优化
//   /lang=        语言代码（原 ResolveInitialLanguage 已处理）
//   /RestoreHealth 打开"修复 Windows 映像"
//   /? 或 /Help   显示命令行帮助并退出
//
// 设计原则：解析层纯函数，动作映射在 CFrameWnd::InitWindow 尾部
// 一次性消费（EnsurePageBuilt 已就绪），不在 CLI 层持有 UI 指针。
// ============================================================

#include <windows.h>
#include <string>
#include <vector>
#include "Utils/Utils.h"

#ifndef UILIB_API
#define UILIB_API __declspec(dllimport)
#endif

enum class EClrAction : int
{
    ECLR_NONE = 0,
    ECLR_CLEAN,          // /C 启动清理
    ECLR_IMAGE,          // /Image= 打开映像
    ECLR_HELP,           // /? 帮助
    ECLR_COMPACTOS,      // /CompactOS
    ECLR_COMPACTEX,      // [R-B55] /CompactEx= 资源管理器 compact 右键：CompactOS 优化指定目录
    ECLR_RESTOREHEALTH,  // /RestoreHealth
    ECLR_ESD2ISO,        // [R-B28] /ESD2ISO= 外壳右键：ESD→ISO
    ECLR_ESD2WIM,        // [R-B28] /ESD2WIM= 外壳右键：ESD→WIM
    ECLR_ESDDECRYPT,     // [R-B28] /ESDDECRYPT= 外壳右键：ESD 解密
    // [R-B51] 三阶段自调用（清单维度 7 缺失 3）：升级链阶段回传
    ECLR_PREDOWNLOAD,    // /PreDownload 阶段 0
    ECLR_INSTALL,        // /Install 阶段 1
    ECLR_FINALIZE        // /Finalize 阶段 2
};

enum class EClrCompress : int
{
    ECLR_COMP_NONE = 0,
    ECLR_COMP_XPRESS4K,
    ECLR_COMP_XPRESS8K,
    ECLR_COMP_XPRESS16K,   // [R-B48] Xpress16k 压缩算法（清单维度 4 缺失 1）
    ECLR_COMP_LZX,
    ECLR_COMP_MAX
};

class CommandLineService
{
public:
    // ---- 解析：从原始命令行解析所有 /key[=value] ----
    static void Parse(const wchar_t* pszCmdLine);

    // ---- 查询 ----
    static bool  Has(const wchar_t* pszKey);              // 是否存在某开关
    static std::wstring Get(const wchar_t* pszKey);       // 取键值，无则空串
    static int   GetInt(const wchar_t* pszKey, int nDef);

    // ---- 动作汇总 ----
    static EClrAction   GetPrimaryAction();
    static EClrCompress GetCompress();             // /exe=
    static bool         IsUnattended();            // /U
    static bool         IsSelectAll();             // /All
    static bool         HasImagePath(DuiLib::CDuiString& sOut);   // /Image=
    static bool         HasScratchDir(DuiLib::CDuiString& sOut);  // /ScratchDir=
    static bool         HasPluginDir(DuiLib::CDuiString& sOut);   // /Plugin=
    static bool         GetEsdTarget(DuiLib::CDuiString& sOut);   // [R-B28] 外壳右键 ESD 动作目标
    // [R-B55] 资源管理器 compact 右键：取得 CompactOS 目标目录（%1 由注册表 command 传入）
    static bool         GetCompactTarget(DuiLib::CDuiString& sOut);
    // [P0] KB57 基线：/C /exe xpress4k "%1" —— 对指定路径执行 EXE 压缩
    static bool         GetCompactExeTarget(DuiLib::CDuiString& sOut, EClrCompress& eAlg);
    // [P0] KB57 基线：/U "%1" —— 对指定路径执行解压（此时 /U 不是无人值守）
    static bool         GetUncompactTarget(DuiLib::CDuiString& sOut);
    static size_t       GetPositionalCount();   // 无开关位置参数（含外壳 %1）
    // [R-B51] 升级阶段附加参数：/path=（状态文件目标目录/路径）
    static bool         GetUpdPhasePath(DuiLib::CDuiString& sOut);
    // [R-B51] /noreboot：安装阶段禁止自动重启（缺省 false）
    static bool         IsUpdNoReboot();
    static bool         IsFunSelfTest();          // [R-B39] /FunSelfTest ?Get* 对拍单测
    // [R31] /DismCoreSelfTest：Rust dismcore FFI 自检（无 GUI，结果写 %TEMP%\dismcore_selftest.txt）
    static bool         IsDismCoreSelfTest();
    // [R53] /DumpSeedFingerprint：输出当前机器种子指纹后退出。
    //   为什么需要：种子包（seed_cap.dat / seed_feat.dat）首行必须写
    //   "SEED	<OSBUILD>|<SKU>|<LANG>"，而这个三元组只能从运行它的机器上取。
    //   原实现只在 SeedCache.cpp 内部计算、无任何对外出口——
    //   **制作种子包的人拿不到正确指纹，只能猜**，猜错则加载端直接拒绝，
    //   表现为"放了种子包但页面照样转圈"，且没有明显报错。
    static bool         IsDumpSeedFingerprint();
    // [R81] /BenchEnum：DISM 枚举分段耗时基准（无 GUI，结果写 %TEMP%\win8metro_bench_enum.txt）
    static bool         IsBenchEnum();
    // [R81] /BenchRender：控件树构建耗时基准（无 GUI，结果写 %TEMP%\win8metro_bench_render.txt）
    //   可带 /Rows=<n> 指定行数，缺省 300（与两页典型条目数一致）。
    static bool         IsBenchRender();
    // [R84] /MakeSeed：生成种子包（枚举一次 → 写两个 seed 文件 → 回读自验证）
    static bool         IsMakeSeed();
    // [R85] /SeedProbe：探测种子失效信号（装/卸功能前后各跑一次对比）
    static bool         IsSeedProbe();
    static bool         IsHelpRequested();

    // ---- 帮助文本（供控制台/MessageBox 展示）----
    static const wchar_t* GetHelpText();

    // ---- 清理状态 ----
    static void Clear();
    static bool WasParsed() { return s_bParsed; }

private:
    static std::vector<std::pair<std::wstring, std::wstring>> s_map;
    static std::vector<std::wstring> s_positional;   // [P0] 无开关位置参数
    static bool s_bParsed;
};
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
