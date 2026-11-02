#pragma once

// ============================================================
// WizDiskPart.h —— [R6-05] 向导共用磁盘/diskpart 原语（零 UI）。
//
// 逐字搬迁自 ToolDialog.cpp(2026-10-13) 匿名命名空间 139-241：
//   TdVolumeDiskNumber / TdWriteTempFile(内部) / TdRunDiskpart /
//   TdFreeLetter / TdFindEspIndex。
// std::wstring 边界；头文件不依赖 UIlib.h / C*UI（R7 契约）。
// 实现内部经 DuiLib::CDuiString 调 ProcessRunner/SystemInfoService，
// 转换只发生在本 TU 边界（与 ConfirmService 同模式）。
// ============================================================

#include <string>

namespace toolsvc {

// 取卷盘符所在物理磁盘号（IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS）。
bool WizVolumeDiskNumber(wchar_t cLetter, DWORD& pdwDisk);

// 执行 diskpart 脚本；psOut 非空时捕获输出（list partition 解析用）。
bool WizRunDiskpart(const std::wstring& sBody, DWORD dwTimeout,
                    std::wstring* psOut);

// 找一个未占用盘符（Z: 起向前到 F:）；0 表示无可用盘符。
wchar_t WizFreeLetter();

// 在目标磁盘的 GPT 分区表里定位 ESP（System 分区）序号。
// 解析 `list partition` 输出；WinPE 多为英文，同时兼容中文"系统"。
bool WizFindEspIndex(DWORD dwDisk, int& pnPart);

} // namespace toolsvc
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
