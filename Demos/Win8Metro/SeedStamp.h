// ============================================================================
//  SeedStamp.h —— [R85] 种子失效检测（sidecar 时间戳）
//
//  为什么不改进种子文件首行：
//    `SEED\t<指纹>\r\n` 是加载端逐字节比对的（R61 用自检钉死了 TAB 与 CRLF），
//    改格式会让所有已分发的种子失效，代价远大于收益。
//
//  所以用旁挂文件：`bin64/seed/seed_stamp.txt`，记录生成种子时的系统状态快照。
//  加载端先按原逻辑读种子（格式不变），需要时再调本模块判断"是否可能过期"。
//
//  信号选择（R85 实测：装一个功能后变化的才采信）：
//    · `WinSxS\FileMaps` 目录 mtime   —— 变了（1792081480 → 1793048915）
//    · CBS 注册表 `NextExecutionSequence` —— 变了（82 → 83）
//    · `WinSxS` 目录 mtime           —— **不变，不采用**（只看它会漏判）
//
//  三个信号读取实测均为 0ms（对比枚举 880ms），成本可忽略。
// ============================================================================
#ifndef SEED_STAMP_H
#define SEED_STAMP_H

#include <string>

namespace seedstamp {

// 当前系统状态快照，格式 "filemaps=<mtime>|cbseq=<n>"。
// 任一信号取不到时对应位写 0，不做失败——宁可少一个信号也不阻断。
std::string CurrentStamp();

// 写入 `SeedDir()\seed_stamp.txt`（不存在则创建）。返回是否成功。
bool SaveStamp(const std::string& sStamp);

// 读回已保存的快照。文件不存在或格式坏时返回空串。
std::string LoadStamp();

// 判断种子是否可能过期。
//   返回 false = 无法判断（没有 stamp 文件 / 格式坏）→ 调用方应维持现有行为
//   返回 true  = 正常完成判断，结果写入 bStale
//   bStale = true 表示信号已变，种子内容可能不反映当前系统状态。
bool IsSeedStale(bool& bStale);

} // namespace seedstamp

#endif // SEED_STAMP_H
