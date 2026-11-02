#pragma once

// ============================================================
// StartupService —— S3（v2 P3b）
// 启动项枚举（HKCU/HKLM/HKU .DEFAULT 的 Run/RunOnce、Wow6432Node、
// 启动文件夹 *.lnk、BootExecute REG_MULTI_SZ、StartupApproved 启停字节）、
// SCM 服务枚举 / 启动类型修改 / 启动-停止-重启（20 秒轮询）、
// 启动项的新增与删除。
// 纯 Win32（注册表 / SCM / 文件枚举），无 UI 控件 / StringMgr 依赖；
// 实现文件不使用预编译头、不 include UIlib.h / StringManager.h。
//
// 全部结构体、枚举与函数体逐字摘自原 CFrameWnd 嵌套结构
// 与 12 个启动/服务页函数的系统调用段（物理搬迁，零逻辑改动）：
// 错误文案映射留在 Page_Startup（StringMgr 只允许存在于 Page 层）。
// ============================================================

#include <windows.h>

#ifndef UILIB_API
#define UILIB_API __declspec(dllimport)
#endif

#include "Utils/Utils.h"
#include <vector>

// 启动项来源类型（StartupEntry::nSrcType 取值）：
// 0 HKCU Run / 1 HKLM Run / 2 HKCU RunOnce / 3 HKLM RunOnce
// 4 HKLM Wow6432Node Run / 5 Wow6432Node RunOnce
// 6 HKU\.DEFAULT Run / 7 HKU\.DEFAULT RunOnce
// 8 用户启动文件夹 / 9 公共启动文件夹 / 10 BootExecute
// 11 [R126-2] 组策略 Run 键（HKLM\Software\Microsoft\Windows\CurrentVersion\Policies\Explorer\Run）
// 12 [R126-2] 启动文件夹非 .lnk 可执行文件（.exe/.com/.bat/.cmd）
enum class StartupSrcType : int
{
    SRC_HKCU_RUN = 0,
    SRC_HKLM_RUN,
    SRC_HKCU_RUNONCE,
    SRC_HKLM_RUNONCE,
    SRC_HKLM_WOW_RUN,
    SRC_HKLM_WOW_RUNONCE,
    SRC_HKU_DEFAULT_RUN,
    SRC_HKU_DEFAULT_RUNONCE,
    SRC_STARTUP_FOLDER_USER,
    SRC_STARTUP_FOLDER_COMMON,
    SRC_BOOT_EXECUTE,
    SRC_GPOLICY_RUN,
    SRC_STARTUP_FOLDER_USER_EXE
};

// One startup entry read from the Run keys / startup folders / BootExecute.
struct StartupEntry
{
    StartupEntry()
        : nScope(0), nSrcType(StartupSrcType::SRC_HKCU_RUN), bEnabled(false), nArch(0) {}

    int        nScope;       // 0 HKCU, 1 HKLM, 2 HKU(.DEFAULT)
    StartupSrcType nSrcType; // 来源类型，取值见 StartupSrcType
    DuiLib::CDuiString sName;
    DuiLib::CDuiString sCommand;
    DuiLib::CDuiString sLocation;    // 来源注册表键路径或启动文件夹路径
    bool       bEnabled;
    // [R126-2] 启动命令位宽：0 未知 / 32 x86 / 64 x64。
    // 由 IsWow64Process2 对 sCommand 解析出的真实可执行文件精确判定。
    int        nArch;
};

// One Win32 service read from the Service Control Manager.
struct ServiceEntry
{
    ServiceEntry() : bRunning(false), nStartType(0), nSvcType(SERVICE_WIN32),
                     nRestartDelay(-1), nCurrentState(0) {}

    DuiLib::CDuiString sName;         // service key name
    DuiLib::CDuiString sDisplayName;  // friendly display name
    bool       bRunning;
    int        nStartType;    // SERVICE_AUTO_START / DEMAND_START / DISABLED
    // [R117-1] 服务类型：SERVICE_WIN32 / SERVICE_DRIVER / SERVICE_KERNEL_DRIVER /
    //          SERVICE_FILE_SYSTEM_DRIVER / SERVICE_RECOGNIZER_DRIVER。
    int        nSvcType;
    // [R131-2] D3：服务恢复策略（SERVICE_CONFIG_FAILURE_ACTIONS）首条 SC_ACTION_RESTART
    //          的延迟秒数；-1 = 未配置恢复策略或查询失败。
    int        nRestartDelay;
    // [R131-2] D3：SCM 原始当前状态（RUNNING/PAUSED/START_PENDING/STOP_PENDING/
    //          STOPPED/PAUSE_PENDING/CONTINUE_PENDING 全量细分，替代布尔 bRunning）。
    DWORD      nCurrentState;
};

// [R93] 一个依赖某服务的服务项（EnumDependentServices 结果）。
struct DepServiceEntry
{
    DepServiceEntry() : nDependType(0), bRunning(false) {}

    DuiLib::CDuiString sName;         // dependent service key name
    DuiLib::CDuiString sDisplayName;  // friendly display name
    int        nDependType;  // SERVICE_DEPENDENCY_LIST / SERVICE_DEPENDENCY_START
    bool       bRunning;     // SERVICE_RUNNING 及以上状态
};

class StartupService
{
public:
    // 按 nSrcType 返回 Run/RunOnce 来源键的根键 / 子键路径 / 注册表视图。
    // 启动文件夹与 BootExecute 无对应 Run 键，返回 false。
    static bool FindRunKey(StartupSrcType nSrc, HKEY& hRootOut,
                           DuiLib::CDuiString& sPathOut, REGSAM& samOut);

    // ---- 枚举 ----
    // 枚举全部 11 个来源的启动项，结果追加进 vOut（调用方传入空向量）。
    static void EnumerateStartup(std::vector<StartupEntry>& vOut);

    // SCM 枚举 Win32 服务（含 QueryServiceConfig 启动类型）。
    // 返回：0 成功；1 OpenSCManager 失败；2 无服务字节；3 EnumServicesStatusEx 失败。
    static int  EnumerateServices(std::vector<ServiceEntry>& vOut);

    // [R93] 枚举依赖某个服务的服务项（EnumDependentServices）。
    // 用于删除/停止服务前的依赖提醒。
    // 返回：0 成功（vOut 为空即无依赖）；1 OpenSCManager 失败；2 OpenService 失败；
    //       3 无依赖字节；4 EnumDependentServices 失败。
    static int  EnumerateDependentServices(const wchar_t* pszName,
                                           std::vector<DepServiceEntry>& vOut,
                                           DWORD& dwErrOut);

    // ---- 启停 / 增删 ----
    // 写 StartupApproved 12 字节启停位（首字节 3=启用 / 2=禁用）。
    // 返回：0 成功；1 键创建/打开失败；2 值写入失败。
    static int  SetStartupApproved(const StartupEntry& e, bool bEnabled);

    // 删除启动项：启动文件夹删 .lnk；BootExecute 重建 REG_MULTI_SZ；
    // Run/RunOnce 删除注册表值。
    // 返回：0 成功；1 通用失败（调用方保留原错误文案）；2 文件删除失败；
    //       3 BootExecute 键打开失败；4 BootExecute 写回失败；
    //       5 Run 键打开失败；6 Run 值删除失败。
    static int  DeleteStartupEntry(const StartupEntry& e);

    // 向 HKCU\...\Run 写入 sFile（REG_SZ），值名取文件名去扩展名。
    // sNameOut 回传生成的值名。返回：0 成功；1 键打开失败；2 值写入失败。
    static int  AddCurrentUserRunItem(const wchar_t* pszFile, DuiLib::CDuiString& sNameOut);

    // 修改服务启动类型（SERVICE_AUTO_START / DEMAND_START / DISABLED）。
    // 返回：0 成功；1 OpenSCManager 失败；2 ChangeServiceConfig 失败
    //      （dwErrOut 回传 GetLastError）。
    static int  SetServiceStartType(const wchar_t* pszName, DWORD dwStart, DWORD& dwErrOut);

    // 服务右键操作结果（iCmd：4401 启动 / 4402 停止 / 4403 重启 / 4404 暂停 / 4405 继续）。
    struct SvcCtlResult
    {
        SvcCtlResult() : code(0), dwErr(0), bWantRun(false) {}
        int   code;       // 0 成功；1 OpenSCManager 失败；2 启动失败；
                          // 3 停止失败；4 等待停止超时；5 重启再启动失败；
                          // 6 等待最终状态超时；7 OpenService 失败；
                          // 8 暂停失败；9 继续失败
        DWORD dwErr;
        bool  bWantRun;   // 成功后期望的运行态
    };
    static SvcCtlResult ControlServiceItem(const wchar_t* pszName, int iCmd);

    // [R80] 删除服务（DeleteService）。返回：0 成功；1 OpenSCManager 失败；
    //       2 OpenService 失败；3 DeleteService 失败（dwErrOut 回传 GetLastError）。
    // 调用方需先确认用户意愿，并先停止服务（ControlServiceItem 4402）。
    static int DeleteServiceItem(const wchar_t* pszName, DWORD& dwErrOut);
};
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
