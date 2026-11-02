#pragma once

// ============================================================
// MountLedgerService —— R-B6 挂载/应用注册表记账
// ------------------------------------------------------------
// 原版 Dism++ 在 HKCU\Software\Dism++\ 下记录：
//   ImageFiles : 最近打开的 WIM/ESD 路径列表（多值）
//   MountPaths : 当前挂载点 <-> WIM 映射（ImagePath:idx -> mountDir）
//   ApplyPaths : 最近的应用目标目录列表（多值）
// 本服务以同样结构读写，不引 UIlib.h，可独立编译。
// ============================================================

#include <windows.h>
#include <string>
#include <vector>

namespace DismLedger
{
// HKCU\Software\Dism++\ImageFiles —— 最近打开映像（MRU，最多 nMax）
bool AddImageFile(const std::wstring& sWim);
// [R133] 显式子键变体：仅供自检/测试把数据写到隔离键，不碰 HKCU\Software\Dism++。
//   生产调用点继续用上面的默认版本（子键为编译期常量 kImageFiles）。
//   为什么不在原函数上加默认参数：那会让「生产键」出现在函数签名里，
//   调用方容易误传；独立名字能一眼看出这是测试通道。
bool AddImageFileEx(const std::wstring& sSubKey, const std::wstring& sWim);
bool GetImageFilesEx(const std::wstring& sSubKey, std::vector<std::wstring>& out);

// [R136] 写入失败注入点（仅测试用）。
//   bAllWritten 的失败分支无法在真实环境构造（内部自开 KEY_WRITE 句柄），
//   因此留一个可选的假写入函数：返回非 0 表示"这次写入失败"。
//   默认 nullptr -> 走真实 RegSetValueExW，行为与不带该参数时**完全一致**。
using TSetValueFn = LONG (*)(HKEY, const wchar_t*, const wchar_t*);

// [R136] 带注入点的变体：用于自检验证失败分支（生产代码不应调用）。
bool AddImageFileEx2(const std::wstring& sSubKey, const std::wstring& sWim,
                     TSetValueFn pSetValueOverride);
bool GetImageFiles(std::vector<std::wstring>& out);

// HKCU\Software\Dism++\MountPaths\<mountDir> = "<wim>|idx"
// 或 HKCU\Software\Dism++\MountPaths\<key> = "<wim>|idx|<mountDir>"
bool RecordMount(const std::wstring& sWim, DWORD dwIndex,
                 const std::wstring& sMountDir);
bool RemoveMount(const std::wstring& sMountDir);
bool GetMounted(std::vector<std::wstring>& outMountDirs,
                std::vector<std::wstring>& outWimPaths,
                std::vector<DWORD>& outIndices);
bool IsMounted(const std::wstring& sWim, DWORD dwIndex,
               std::wstring* pMountDir = nullptr);

// HKCU\Software\Dism++\ApplyPaths —— 最近应用目标（MRU）
bool AddApplyPath(const std::wstring& sTarget);
bool GetApplyPaths(std::vector<std::wstring>& out);

// HKCU\Software\Dism++\MountFlags —— 最近一次挂载标志（0x%.8X 日志）
bool SetLastMountFlags(DWORD dwFlags);
DWORD GetLastMountFlags();

// HKCU\Software\Dism++\ApplyFlags —— 最近一次应用标志
bool SetLastApplyFlags(DWORD dwFlags);
DWORD GetLastApplyFlags();
}
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
