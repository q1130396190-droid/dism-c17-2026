// WofCompact.h
// ------------------------------------------------------------------
// [P0-A2] CompactOS / WOF 文件压缩原生模块（替代 compact.exe 进程）。
//
// 原版 Dism++ 逆向证据（Ghidra，主程序 RVA）：
//   FUN_1400779b0 逐文件压缩：CreateFileW + DeviceIoControl(0x9030C)
//   FUN_14007784c 是否已压缩：DeviceIoControl(0x90310)，撤销 0x90314
//   缓冲 20 字节 = WOF_EXTERNAL_INFO{1, WOF_PROVIDER_FILE(2)}
//              + FILE_PROVIDER_EXTERNAL_INFO_V1{1, Algorithm, 0}
//   Win10+ 零驱动参与；Win7/8.x 才需随包 Config\\wofadk.sys 引导加载。
//
// 全部为公开文档化 WOF 协议（winnt.h 常量），无任何私有 IOCTL。
// ------------------------------------------------------------------
#pragma once

#include <windows.h>
#include <string>

namespace WofCompact
{

// 与 winnt.h FILE_PROVIDER_COMPRESSION_* 取值一致
enum class Algorithm : int
{
    AlgXpress4k  = 0,   // FILE_PROVIDER_COMPRESSION_XPRESS4K
    AlgXpress8k  = 1,   // FILE_PROVIDER_COMPRESSION_XPRESS8K
    AlgXpress16k = 2,   // FILE_PROVIDER_COMPRESSION_XPRESS16K
    AlgLzx       = 3,   // FILE_PROVIDER_COMPRESSION_LZX
};

// 解析名称（不区分大小写）：xpress4k/xpress8k/xpress16k/lzx
int  AlgorithmFromName(const wchar_t* pszName);

// 当前系统是否支持 WOF 外部支持（FSCTL_*_EXTERNAL_BACKING）。
// Win10/11 恒真；Win7/8.x 尝试安装加载随包 wofadk.sys 后可能为真。
bool IsSupported();

// 单文件原语（目录须带 FILE_FLAG_BACKUP_SEMANTICS，本模块内部处理）
[[nodiscard]] bool IsFileBacked(const wchar_t* pszPath);                       // 已 WOF 压缩
[[nodiscard]] bool CompactFile(const wchar_t* pszPath, int nAlgorithm,
                 DWORD* pdwLastError = nullptr);
[[nodiscard]] bool UncompactFile(const wchar_t* pszPath,
                   DWORD* pdwLastError = nullptr);

// 进度回调：返回 false 请求取消
struct Progress
{
    unsigned long long nScanned    = 0;
    unsigned long long nChanged    = 0;   // 实际压缩/解压成功数
    unsigned long long nSkipped    = 0;
    unsigned long long nFailed     = 0;
    std::wstring       sCurrent;
};
using PFN_PROGRESS = bool (*)(const Progress& st, void* pCtx);

// 对文件或目录递归施加/撤销 WOF 压缩（右键菜单 /exe 与 /U 语义）。
// bUncompact=true 时忽略 nAlgorithm。
void CompactTree(const wchar_t* pszPath, int nAlgorithm, bool bUncompact,
                 PFN_PROGRESS pfnProgress, void* pCtx);

// CompactOS 状态：0=未压缩(Never) 1=部分(Mixed) 2=已优化(Optimized) -1=不支持/失败
int  QueryCompactOsState();

// compact /CompactOS:always(optimize) 与 :never 的原生等价：
// 枚举 %SystemRoot%（reparse/系统文件/7 天内新文件/黑名单/非 PE 的 dll 跳过），
// 完成后写 SYSTEM\\CurrentControlSet\\Setup!Compact 标记。
[[nodiscard]] bool OptimizeCompactOs(PFN_PROGRESS pfnProgress, void* pCtx);
[[nodiscard]] bool UncompactCompactOs(PFN_PROGRESS pfnProgress, void* pCtx);

// 读取 HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\WOF!ForceAlgorithm
// （>3 视为 0）。返回 0..3。
int  GetForceAlgorithm();

} // namespace WofCompact
