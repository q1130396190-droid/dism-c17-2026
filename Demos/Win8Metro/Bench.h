#pragma once

// ============================================================
// [R81] 无界面性能基准入口
//
// 目的：把"1 秒内加载"从结构推算变成可复现的数字。
//   /BenchEnum   —— 真实 DISM 枚举分段耗时（session/api/第二轮）
//   /BenchRender —— 控件树构建耗时（按两页真实每行控件数建模）
//
// 两者都不建窗口、不进消息循环，因此能在 CI / 无人值守环境跑。
// 结果写 %TEMP%\win8metro_bench_*.txt（UTF-8, key=value 逐行）。
// 退出码：0 = 全部成功；1 = 存在失败项（数字仍写盘）。
// ============================================================

namespace bench
{
// 写基准结果文件；返回是否写盘成功。
bool WriteBenchFile(const char* pszName, const std::string& sBody);

// /BenchEnum：DISM 枚举分段耗时
int RunBenchEnum();

// /BenchRender：控件树构建耗时（N 行 × 实际每行控件数）
int RunBenchRender(int nRows);

// /MakeSeed：跑一次真实枚举，按加载端格式生成 seed_feat.dat / seed_cap.dat
//   并立刻用加载端自己的 Load*SeedCache 回读验证。需能成功调用 DISM。
int RunMakeSeed();

// /SeedProbe：探测种子失效候选信号（时间戳类）的值与读取耗时
int RunSeedProbe();
} // namespace bench
