// ============================================================================
//  CbsHostBridge.h
//
//  CBSHost.dll 6 个官方导出背后的 C++ 语义层（进程内原生实现）。
//
//  架构说明（与方案《降级项审计与原生移植方案》B1 一致）：
//    本复刻在主程序进程内已通过 DismApiBridge 直连公开 dismapi.dll
//    （Cleanup-Image / Capability / Feature / Driver / Package / 健康检查），
//    通过 AppxRuntimeBridge 直连 WinRT PackageManager，通过
//    OfflineCbsService 承载离线蜂巢/KTM 深链路。原版 CBSHost 独立工作进程
//    承载的全部能力在本复刻均已进程内原生化，且主程序以 requireAdministrator
//    运行（与工作进程同完整性级别）。因此 CBSHost.dll 作为进程内原生模块
//    提供同一组 6 导出 ABI；RunCbsHostW 的数字 ID 命令直接派发到
//    DismApiBridge，不再 fork 任何外部 dism.exe / 工作进程。
//
//  本头仅被 CBSHost.dll 工程内部使用；跨主程序边界的 ABI 一律走
//  CBSHostApi.h（POD + wchar_t* 缓冲，调用方拥有内存）。
// ============================================================================

#ifndef CBS_HOST_BRIDGE_H
#define CBS_HOST_BRIDGE_H

#include <windows.h>
#include <string>

namespace CbsHost
{

// 与 CBS_CONFIG（CBSHostApi.h）等价的 C++ 配置；字符串用 std::wstring，
// 仅在 DLL 内部使用，不跨 ABI 边界。
struct Config
{
    std::wstring sExeDir;       // 主程序（本 DLL）所在目录
    std::wstring sScratchDir;   // CBS 暂存目录
    std::wstring sLogDir;       // 日志目录
    std::wstring sMountRoot;    // 离线映像挂载点（空 = 在线）
    int          nLanguage;     // 多语言 ID（0 = 系统默认）
    bool         bNoviceMode;
    bool         bExpertMode;

    Config()
        : nLanguage(0), bNoviceMode(true), bExpertMode(false) {}
};

// ---- 6 导出对应的语义实现 ----

// 1) 创建 CBS 操作临时目录（%TEMP% 下唯一 GUID 子目录，继承父级 ACL）。
[[nodiscard]] bool CbsCreateTempDirectory(std::wstring* psOut, DWORD* pdwErr);

// 2) 构造不透明 Helper 上下文（new 出的对象，模块生命周期内常驻）。
//    pCfg 非空时采用给定配置，否则用默认配置；pOuter 为外部上下文（可空）。
void* CbsHostCreateHelper(const Config* pCfg, void* pOuter, int nMode);

// 3) 返回当前会话暂存目录（未显式创建时惰性落到 %TEMP%\\Dism++Host）。
bool DismGetScratchDir(std::wstring* psOut);

// 4) 以 level 0..4 写一条 CbsHost.log 日志（变参形式）。
void DismWriteLogV(int nLevel, const wchar_t* pszFormat, va_list ap);

// 5) 读取配置；sIni 为空时用本 DLL 目录下 Config\\Config.ini / Config.ini。
//    即使 ini 缺失也返回 true（回填默认配置）。
[[nodiscard]] bool GetConfigFromIni(const std::wstring& sIni, Config* pCfg,
                      std::wstring* psErr);

// 6) CBS 主入口：解析 "<id> \"<target>\" \"<param>\" <flags>"，按
//    CBS_CMD_* 数字 ID 派发到 DismApiBridge（原生 dismapi）。返回 0=成功。
int RunCbsHostW(const std::wstring& sCmdLine, std::wstring* psErr);

} // namespace CbsHost

#endif // CBS_HOST_BRIDGE_H
