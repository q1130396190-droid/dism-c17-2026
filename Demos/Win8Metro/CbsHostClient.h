// ============================================================================
//  CbsHostClient.h
//
//  主程序（Win8Metro.exe）访问 CBSHost.dll 的客户端适配层。
//
//  自 R-拆分 起，CBSHost 6 导出的实现不再静态链接进主程序，主程序在运行时
//  LoadLibrary("CBSHost.dll") + GetProcAddress，通过本适配层以原有 C++
//  风格（std::wstring）调用，调用方不再感知 DLL 边界。
//
//  生命周期：FrameWnd 构造 Init()、析构 Shutdown()。
//  线程模型：Init/Shutdown 在 UI 线程；函数指针加载后只读，业务调用可在
//            任意工作线程并发执行。
//  兜底策略：DLL 缺失/导出不全时 IsReady()=false；各调用返回失败并给出
//            中文错误说明，绝不解引用空指针。
// ============================================================================

#ifndef CBS_HOST_CLIENT_H
#define CBS_HOST_CLIENT_H

#include <windows.h>
#include <string>

#include "CBSHostApi.h"   // CBS_CMD_* 命令 ID 与 CBS_CONFIG 结构（共享 C ABI 头）

namespace CbsHostClient
{

// 子命令 ID（与 CBSHostApi.h 的 CBS_CMD_* 等价的 C++ 枚举）
// [C++17 R20] scoped enum; all users are in-cluster (CbsHostClient.h/.cpp)
enum class ECmd
{
    kCmdStart        = CBS_CMD_START,
    kCmdCleanup      = CBS_CMD_CLEANUP,
    kCmdInstall      = CBS_CMD_INSTALL,
    kCmdCleanupImage = CBS_CMD_CLEANUP_IMAGE,

    // [2026-10-08] 健康检查/修复/包查询
    kCmdCheckHealth   = CBS_CMD_CHECK_HEALTH,
    kCmdScanHealth    = CBS_CMD_SCAN_HEALTH,
    kCmdRestoreHealth = CBS_CMD_RESTORE_HEALTH,
    kCmdQueryPackages = CBS_CMD_QUERY_PACKAGES,
};

// 配置结构（与 DLL 侧 CBS_CONFIG 同布局的 C++ 版本）
struct Config
{
    std::wstring sExeDir;
    std::wstring sScratchDir;
    std::wstring sLogDir;
    std::wstring sMountRoot;
    int          nLanguage;
    bool         bNoviceMode;
    bool         bExpertMode;

    Config() : nLanguage(0), bNoviceMode(true), bExpertMode(false) {}
};

// ---- 生命周期 ----
// 加载主程序同目录下 CBSHost.dll 并解析全部 6 个导出。
// DLL 缺失返回 false（pErr 可空）；此时 IsReady()=false。
bool Init(std::wstring* pErr = nullptr);
void Shutdown();
bool IsReady();

// ---- 业务接口 ----

// 派发 CBS 子命令（等价旧 CbsHost::DispatchCommand）。
// 内部通过官方导出 RunCbsHostW 以数字 ID 命令行派发，返回退出码（0=成功）。
int DispatchCommand(ECmd cmd, const std::wstring& sTarget,
                    const std::wstring& sParam, DWORD dwFlags,
                    std::wstring* pErr);

// 获取暂存目录（等价旧 CbsHost::DismGetScratchDir）。
[[nodiscard]] bool GetScratchDir(std::wstring* pOut);

// 写日志（等价旧 CbsHost::DismWriteLog）。level 0..4。
void WriteLog(int nLevel, const wchar_t* pszFormat, ...);

// ---- 其余官方导出的完整包装（供后续功能直接使用）----

// 创建/销毁 CBS 临时目录。
[[nodiscard]] bool CreateTempDirectory(std::wstring* pOut, DWORD* pdwErr = nullptr);

// 读取配置；sIni 空串表示默认 config.ini。
[[nodiscard]] bool ReadConfig(const std::wstring& sIni, Config* pCfg, std::wstring* pErr);

} // namespace CbsHostClient

#endif // CBS_HOST_CLIENT_H
