#pragma once

// ============================================================
// DriverService —— v2 P5/S7 驱动管理系统层
//
// 承接原 CFrameWnd 驱动页中纯系统调用部分：
//   * pnputil /enum-drivers 捕获与按块解析（原 PnpEnumDrivers）
//   * pnputil /delete-driver 执行（原 DoDriverDelete 循环内调用）
// 纯系统层，不依赖 UI / StringMgr；实现文件不使用预编译头。
// 原 CFrameWnd 嵌套结构 DriverPubInfo / DriverRowInfo 原样提升至此。
// ============================================================

#include <windows.h>

#ifndef UILIB_API
#define UILIB_API __declspec(dllimport)
#endif

#include "Utils/Utils.h"
#include <map>

// S7 实现文件不使用预编译头，需在引入 UIlib 全命名空间前显式可见该类型
using DuiLib::CDuiString;

// pnputil /enum-drivers 解析出的一条已发布第三方驱动
struct DriverPubInfo
{
    CDuiString sOem;        // Published Name: oemN.inf
    CDuiString sOriginal;   // Original Name: 原始 INF 文件名
    CDuiString sProvider;   // Provider Name: 厂商
    CDuiString sVersion;    // Driver Version（不含行首内嵌日期）
    CDuiString sDate;       // Driver Date / 版本行内嵌日期
};

// 驱动设备行（drow_n_k）回填信息：点击看详情 / 删除时取 INF
struct DriverRowInfo
{
    CDuiString sName;       // 设备名
    CDuiString sInfPath;    // DEVPKEY_Device_DriverInfPath，如 oem26.inf
    CDuiString sFile;       // 驱动 .sys 完整路径
    CDuiString sDevInstId;  // [R87] P1-14 设备实例 ID（SetupDiGetDeviceInstanceId），供驱动回滚
    CDuiString sService;    // [R114-1] SPDRP_SERVICE：设备驱动服务名，如 disk / HDAudio
    CDuiString sDevStatus;  // [R114-1] 设备状态描述（正常/有问题/未知），由 CM_Get_DevNode_Status 派生
    DWORD      dwProblem   = 0; // [R114-1] 问题代码（CM_PROB_*），0 表示正常
    CDuiString sMfg;        // [R120-2] SPDRP_MFG：设备制造商
    CDuiString sInstallDate;// [R120-2] SPDRP_INSTALL_DATE：安装日期（FILETIME → 本地化字符串）
    CDuiString sHardwareId; // [R120-2] 硬件 ID（SPDRP_HARDWAREID，取首条）
};

class DriverService
{
public:
    // 执行 pnputil /enum-drivers 并按块解析到 mapOut（key=oemN.inf 小写）。
    // pnputil 重定向后为 UTF-16 输出，经 findstr 管道转成本地 ANSI 文本；
    // 各语言字段标签已本地化，因此只按 token 形态（oemN.inf / *.inf /
    // N.N.N.N / YYYY-MM-DD）提取，不依赖英文标签。
    // 进程捕获失败：返回 false，mapOut 保持原样；
    // 捕获成功：先清空 mapOut 再解析，返回是否解析到至少一块（bGotAny）。
    static bool EnumPublished(std::map<CDuiString, DriverPubInfo>& mapOut);

    // pnputil /delete-driver <sOem>，隐藏执行并等待，返回退出码。
    static DWORD DeleteDriver(const CDuiString& sOem);

    // [P1-A6] 设备树重扫：cfgmgr32!CM_Locate_DevNodeW(nullptr 根)
    // +CM_Reenumerate_DevNodeW(CM_REENUMERATE_SYNCHRONOUS)，
    // 替代 pnputil /scan-devices。返回 0=成功，非 0=CONFIGRET/Win32 错误。
    static DWORD ScanDevicesNative();

    // [P1-A6] 导出全部第三方驱动到目标目录（每个包一个子目录，名为
    // oemN），替代 dism /Export-Driver。sOfflineRoot 非空时导出离线挂载
    // 映像（DismGetDrivers 离线会话 + FileRepository 内容匹配定位）。
    // 返回成功导出的包数；-1 表示 API 不可用/枚举失败（sErr 有文本）。
    static int  ExportAllThirdParty(const CDuiString& sDestDir,
                                    const CDuiString& sOfflineRoot,
                                    CDuiString& sErr);

    // [R83] P1-16：查询驱动文件真实磁盘占用（稀疏/压缩后）
    // 基于 GetCompressedFileSizeW；返回占用字节数，失败返回 INVALID_FILE_SIZE。
    static DWORDLONG QueryCompressedFileSize(const CDuiString& sPath);

    // [R87] P1-14：设备驱动回滚到上一版本（Windows 属性页等价实现）
    // 步骤：读 HKLM\SYSTEM\CurrentControlSet\Enum\<devinst>\Backup\Driver
    //       （Windows 自动备份的上一版驱动键，BackupInProgress=0 时有效）
    //       → 写回本设备的 Driver 子键 → SetupDiSetSelectedDevice
    //       + SetupDiCallClassInstaller(DIF_PROPERTYCHANGE) 让 PnP 重载该驱动。
    // 返回 0 表示成功；非 0 为 Win32 错误码（GetLastError）或自定义错误：
    //    ERROR_NOT_FOUND(1168) 无可回滚备份；ERROR_CALL_NOT_IMPLEMENTED(120) 类安装器失败。
    // 调用方必须是管理员权限；失败时通过 sErr 返回可展示的描述。
    static DWORD RollbackDevice(const CDuiString& sDevInstId, CDuiString& sErr);

    // [R87] 查询设备实例 ID 当前驱动键下是否存在可用的上一版驱动备份键
    // 存在返回 true，并输出上一版驱动键名（用于 UI 前置提示）。
    static bool HasRollbackCandidate(const CDuiString& sDevInstId, CDuiString& sBackupDriver);
};
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
