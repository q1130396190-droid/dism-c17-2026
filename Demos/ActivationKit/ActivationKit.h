// ============================================================================
//  ActivationKit.h —— Windows 激活信息备份/还原 独立卫星组件 C ABI
//
//  能力来源：R01 重构将原 FrogPlugins（Plugin.amd64.dll）vtable
//  slot[77]/[81] 的激活备份/还原能力抽离为本独立组件，插件框架同步退场。
//
//  约定：
//    * x64 / /MT 静态 CRT / 仅依赖系统 DLL（ADVAPI32、KERNEL32）；
//    * 导出经 ActivationKit.def 序号冻结（@1..@3），名+序双稳定；
//    * 组件无窗口、不联网、不执行激活动作（不调 slmgr），只做注册表与
//      Backup.ini 之间的数据搬运；
//    * 宿主经 LoadLibrary/GetProcAddress 软依赖调用，组件缺失须可降级。
//
//  备份布局：<pszOutDir>\激活备份\Backup.ini
//  注册表键：HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion
//    DigitalProductId / DigitalProductId4 (REG_BINARY, HEX 编码)
//    ProductId / ProductName / EditionID   (REG_SZ, 登记信息)
//
//  兼容性：INI 段名/键名/HEX 编码与原青蛙插件逐字一致，历史备份件可还原。
// ============================================================================

#pragma once

#include <windows.h>

constexpr int ACTKIT_API_VERSION = 1;

#ifdef ACTKIT_DLL_BUILD
#define ACTKIT_API extern "C" __declspec(dllexport)
#else
#define ACTKIT_API extern "C" __declspec(dllimport)
#endif

// 备份激活信息。
//   pszOutDir : 备份输出目录（其下创建「激活备份」子目录），不可为 nullptr/空。
//               [BUG-20] 路径契约：组件不做规范化/绝对化处理（来源为调用方
//               UI 选择的目录），调用方须保证为规范绝对路径（无 "."/".."、
//               无多余分隔符、环境变量已展开），组件仅原样拼接子目录。
//   pszNote   : 备份说明，可为 nullptr（使用内置默认说明）。
//   pszErr    : 中文错误描述缓冲，可为 nullptr；cchErr 为其容量（含终止符）。
// 返回：
//   S_OK                                      成功；
//   E_INVALIDARG                              目录参数非法；
//   HRESULT_FROM_WIN32(ERROR_NOT_FOUND)       当前系统读不到 DigitalProductId(4)；
//   HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)  保留；目录已存在不作为失败；
//   其余注册表/文件失败 = HRESULT_FROM_WIN32(GetLastError())。
ACTKIT_API HRESULT __cdecl AK_BackupActivation(const wchar_t* pszOutDir,
                                               const wchar_t* pszNote,
                                               wchar_t* pszErr,
                                               DWORD cchErr);

// 从 <pszBaseDir>\激活备份\Backup.ini 还原 DigitalProductId(4)。
//   pszBaseDir : 备份所在目录（非 INI 路径本身）。
//                [BUG-20] 路径契约同 AK_BackupActivation：由调用方保证
//                规范化为绝对路径，组件原样拼接，不做路径规范化。
// 返回：
//   S_OK                                          成功；
//   E_INVALIDARG                                  参数非法；
//   HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)      备份文件不存在；
//   E_FAIL                                        INI 无有效 HEX 数据 / 写注册表失败。
ACTKIT_API HRESULT __cdecl AK_RestoreActivation(const wchar_t* pszBaseDir,
                                                wchar_t* pszErr,
                                                DWORD cchErr);

// 探测备份件是否存在且至少含一个非空 DigitalProductId 键。
//   pszBaseDir : [BUG-20] 路径契约同上：调用方保证规范绝对路径。
ACTKIT_API BOOL __cdecl AK_ActivationBackupExists(const wchar_t* pszBaseDir);
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
inline constexpr int kCpp17H_ActivationKit_PageSize = 4096;
inline constexpr int kCpp17H_ActivationKit_MaxBuf = 8192;
