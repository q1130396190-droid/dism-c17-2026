#pragma once

// ============================================================================
//  SkinThemeDll.h -- 把两套主题皮肤做成独立 DLL（导出/导入两侧共用）
//
//  为什么需要 DLL 化（用户明确要求）：
//    当前两套配色靠"同一份皮肤 + 运行期 {tok:} 令牌分叉"实现。它工作正常，
//    但有三个实际约束：
//      1) 皮肤 XML 必须随 exe 以散文件部署，漏拷一个 pages/*.xml 就整页空白；
//      2) 想换一套完全不同的皮肤（不只是配色）没有干净的边界，
//         只能改 bin64/skin 下的文件，改动与安装包耦合；
//      3) 皮肤被任意用户随手编辑，没有"这是一版发布皮肤"的概念。
//    DLL 化后：每套皮肤是一个可独立替换的二进制发布单元，
//    exe 只在启动时按主题模式选一个加载，皮肤内容对用户不可见也不可改坏。
//
//  与令牌系统的关系（重要）：
//    皮肤 DLL **不替代** tokens::。令牌仍负责"跟随系统强调色/深浅色"，
//    皮肤 DLL 负责"这一套皮肤的 XML 从哪来"。两者正交：
//      · 同一个 DLL 里的 {tok:Surface} 在深浅模式下仍解析成不同颜色；
//      · 拆 DSP 只是把"XML 文本"换成"从 DLL 读出的文本"。
//    这样原有的 16/16 主题刷新覆盖、对比度门禁全部继续有效。
//
//  回退策略（必须保留）：
//    DLL 不存在 / 版本不匹配 / 内容为空 -> 一律回退到磁盘皮肤文件。
//    回退路径就是现在这条已验证的路径，所以"DLL 化失败"只会退化成旧行为，
//    不会变成"界面打不开"。
// ============================================================================

#include <windows.h>
#include <stdint.h>

// 导出宏：本 DLL 侧定义 SKIN_THEME_BUILD，使用侧不定义。
#ifdef SKIN_THEME_BUILD
#  define SKIN_THEME_API extern "C" __declspec(dllexport)
#else
#  define SKIN_THEME_API extern "C" __declspec(dllimport)
#endif

// 皮肤格式版本。任何影响"XML 如何被解释"的改动都必须换这个号，
// 否则老 DLL 会被新 exe 静默加载并产生难以定位的解析差异。
#define SKIN_THEME_ABI_VERSION 1

// 主题槽位：与 tokens::EThemeMode 的 Light/Dark 一一对应。
// 单独定义而不直接复用 tokens 枚举，是为了让 DLL 不依赖 exe 的头文件 ——
// 皮肤包应当能被一个只认识"浅/深"的最小契约加载。
#define SKIN_SLOT_LIGHT 0
#define SKIN_SLOT_DARK  1

// 一个皮肤资源的标识：
//   pszName  -- 相对皮肤的路径，用 '/' 分隔，例如 L"Main.xml"、L"pages/page_cleanup.xml"
//   返回值   -- UTF-8 字节（不含 BOM），写入 *pcbBytes
//   NULL     -- 该槽位没有这个文件
//
// 返回的缓冲区由 DLL 持有，调用方**不得释放**，生命周期到进程结束。
typedef const unsigned char* (*PFN_SKIN_GET)(int nSlot, const wchar_t* pszName,
                                             unsigned int* pcbBytes);

// DLL 导出签名：
//   SkinTheme_ABI()      -> 返回 SKIN_THEME_ABI_VERSION，用于拒绝不匹配的包
//   SkinTheme_Get(slot, name, &cb) -> 取一个皮肤文件
//   SkinTheme_Enum(i, name, cchName, &cb) -> 枚举第 i 个文件名，返回 false 表示越界
//   SkinTheme_Count()    -> 文件总数（用于零资源自检：0 个文件的 DLL 必须被拒绝）
SKIN_THEME_API int   SkinTheme_ABI(void);
SKIN_THEME_API int   SkinTheme_Count(void);
SKIN_THEME_API int   SkinTheme_Enum(int nIndex, wchar_t* pszName, int cchName,
                                    unsigned int* pcbBytes);
SKIN_THEME_API const unsigned char* SkinTheme_Get(int nSlot, const wchar_t* pszName,
                                                  unsigned int* pcbBytes);
