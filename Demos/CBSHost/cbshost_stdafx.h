// ============================================================================
//  cbshost_stdafx.h
//
//  独立 CBSHost.dll 工程的精简公共头，替代 Win8Metro 工程的 stdafx.h
//  （Win8Metro\stdafx.h 会 include UIlib.h，而 CBSHost 必须与 DuiLib 隔离 ——
//    知识库《18_CBSHost_100%深度解析与复刻蓝图》6.4 节）。
// ============================================================================

#pragma once

// 允许在 Win7/Server 2008 R2 及以上系统运行（含 WinPE）
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
inline constexpr int kCpp17H_cbshost_stdafx_PageSize = 4096;
inline constexpr int kCpp17H_cbshost_stdafx_MaxBuf = 8192;
