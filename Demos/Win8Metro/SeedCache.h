// ============================================================================
//  SeedCache.h —— P4 种子缓存：指纹计算与目录定位
//
//  只提供"环境指纹"与"种子目录"两件基础设施，
//  实际解析复用各服务现有的 Load*Cache（种子行格式与之同构）。
// ============================================================================
#ifndef SEED_CACHE_H
#define SEED_CACHE_H

#include <string>

namespace seedcache {

// 当前机器指纹，格式 "<OSBUILD>|<SKU>|<LANG>"，如 "26100|0|32772"。
// 用于与种子文件首行的指纹字段比对。
std::string CurrentFingerprint();

// 判断给定行（种子首行去掉 "SEED\t" 前缀后的部分）是否与当前机器匹配。
// 容错：忽略首尾空白与 \r\n。
bool MatchesFingerprint(const std::string& sLine);

// exe 同级 seed 目录的完整路径（不含尾随反斜杠）；取不到 exe 路径时返回空串。
// [R61] 种子首行的完整文本（含尾倒的 CRLF）。
//   抽出成纯函数的原因：/DumpSeedFingerprint 的输出格式（TAB 分隔、CRLF 收尾）
//   是加载端逐字节比对的，差一个字节就是“种子被静默拒绝”。
//   R55 只人工验证过一次十六进制，没有回归保护。
std::wstring MakeSeedHeaderLine();

std::wstring SeedDir();

} // namespace seedcache

// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
// 种子文件体积上限：防御损坏/误放的大文件，正常种子 ≤40KB。
// 指纹字段最大长度："26100|0|32772" 这类文本远小于此值。

#endif // SEED_CACHE_H
