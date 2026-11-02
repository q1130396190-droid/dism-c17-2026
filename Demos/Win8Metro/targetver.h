#pragma once

// Modify the following defines if you have to target a platform prior to the ones specified below.
// See MSDN for the latest info on corresponding values for different platforms.

#ifndef WINVER                          // 目标平台：Windows 10（FILE_ID_INFO 等需 0x0A00）
#define WINVER 0x0A00           // Change this to the appropriate value to target other versions of Windows.
#endif

#ifndef _WIN32_WINNT                // 目标平台：Windows 10（与 build.bat /D _WIN32_WINNT=0x0A00 一致）
#define _WIN32_WINNT 0x0A00     // Change this to the appropriate value to target other versions of Windows.
#endif

#ifndef _WIN32_WINDOWS          // Specifies that the minimum required platform is Windows 98.
#define _WIN32_WINDOWS 0x0410 // Change this to the appropriate value to target Windows Me or later.
#endif

#ifndef _WIN32_IE                 // Specifies that the minimum required platform is Internet Explorer 7.0.
#define _WIN32_IE 0x0700       // Change this to the appropriate value to target other versions of IE.
#endif
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
