// ============================================================================
//  HostExports.h
//
//  宿主 6 回调导出 —— 等价原版 Dism++x64.exe 的导出表子集。
//
//  逆向基线（Dism++10.1.1002.1B\Dism++x64.exe 导出表实测，共 62 个导出）：
//  本文件仅实现 NCleaner.dll 反向导入所需的 6 个：
//    ord=27  DismFreeMemory
//    ord=28  DismGetAllUsersAppx
//    ord=40  DismGetSystemInfoBySession
//    ord=46  DismRegOpenKeyEx
//    ord=47  DismRemoveAppx
//    ord=59  DismWriteLog
//
//  与原版的差异说明：
//    原版这 6 个符号定义在 Dism++x64.exe 内部（Chuyu 闭源），NCleaner 通过
//    静态导入表直接引用。本工程宿主为 Win8Metro.exe，改为导出同名符号，
//    NCleaner 侧经运行期 GetProcAddress 解析（ncleaner_host.cpp），
//    语义等价，且避免链接期硬依赖。
//
//  约束：
//    * 本层是纯 ABI 面，不 include UIlib.h / 不依赖 DuiLib 控件；
//    * 跨边界只传 POD / wchar_t*（缓冲区由调用方拥有），
//      严禁跨模块 malloc/free（NCleaner 用 /MT，宿主用 /MD）。
//    * 所有字符串均为 UTF-16。
// ============================================================================

#ifndef HOST_EXPORTS_H
#define HOST_EXPORTS_H

#include <windows.h>

// 导出宏：extern "C" 保证 x64 下符号不修饰，与 NCleaner 的 GetProcAddress 名匹配
#define HOST_API extern "C" __declspec(dllexport)

// ---- 6 个官方导出 ----------------------------------------------------------

// 枚举所有用户的 Appx 包全名。
// pNames：调用方缓冲区，每项 [MAX_PATH] 宽字符；可为 nullptr（仅计数）。
// 返回：条目数（>=0）；失败返回负数。
HOST_API int __cdecl DismGetAllUsersAppx(wchar_t* pNames, int nCap);

// 打开注册表键。成功返回 ERROR_SUCCESS(0)，句柄经 phKey 回传。
HOST_API int __cdecl DismRegOpenKeyEx(HKEY hRoot, const wchar_t* pSubKey,
                                      DWORD dwSam, HKEY* phKey);

// 写日志（level 0..4）。文本写入宿主日志文件并 Debug 输出。
HOST_API void __cdecl DismWriteLog(int nLevel, const wchar_t* pText);

// 按会话取系统信息。返回 1 成功 / 0 失败，文本写入 pBuf。
HOST_API int __cdecl DismGetSystemInfoBySession(DWORD dwSession,
                                                wchar_t* pBuf, DWORD cch);

// 移除 Appx 包（全名）。返回 HRESULT（0 = S_OK）。
HOST_API long __cdecl DismRemoveAppx(const wchar_t* pFullName);

// 释放由本模块分配的内存。
HOST_API void __cdecl DismFreeMemory(void* p);

// [R01] FrogPlugins 插件框架已整体剥离：原插件协议使用的 6 个导出
//  （DismExpandEnvironmentStrings / DismFormatMessage / DismMultiLanguage /
//   DismRegOpenKey / DismApplyDPI / DismCreateInterface）同步删除。
//  宿主 EXE 导出表收敛为 NCleaner 反向导入所需的这 6 个回调。

// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：

#endif  // HOST_EXPORTS_H
