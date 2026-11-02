// ============================================================================
//  ncleaner_stdafx.h
//
//  NCleaner.dll 独立工程的精简公共头。
//  与 CBSHost 工程同构：NCleaner 必须与 DuiLib / 主程序 UI 层完全隔离
//  （知识库《21_核心DLL职责与接口》2.5 / 《52_B_CBSHost_NCleaner_wimgapi核心DLL逆向》2.2）。
//
//  逆向基线（Dism++10.1.1002.1B\Config\amd64\NCleaner.dll，v10.1.1001.10，
//  sha256 141587ad11529d23…，2020-01-18 编译，55,808 B）：
//    * 7 个导出 NC*Cleanup；
//    * 导入 6 个 DLL / 95 函数：KERNEL32(38) / ADVAPI32(6) / ole32(3) /
//      msvcrt(39) / Dism++x64.exe(6，反向导入宿主回调) / SHLWAPI(3)；
//    * 无 delay-import（实测 DELAY_IMPORT 目录 RVA=0）；
//    * 无 .rsrc 加密表，仅 RT_16/ID_1 884 B VS_VERSION_INFO。
//
//  与 CBSHost 的差异：NCleaner 反向导入宿主 6 个回调，形成进程内微服务；
//  本工程通过 ncleaner_host.h 的动态解析层实现（GetProcAddress 宿主自身），
//  避免在链接期对宿主可执行文件产生硬依赖。
// ============================================================================

#pragma once

// 与 NCleaner.dll 一致：最低 Windows 7 / Server 2008 R2
#ifndef WINVER
#define WINVER 0x0601
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <windows.h>
#include <cstdlib>
#include <cstdlib>
#include <memory.h>

#include <string>
#include <vector>
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
inline constexpr int kCpp17H_ncleaner_stdafx_PageSize = 4096;
inline constexpr int kCpp17H_ncleaner_stdafx_MaxBuf = 8192;
