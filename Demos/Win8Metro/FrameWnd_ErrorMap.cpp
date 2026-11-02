// FrameWnd_ErrorMap.cpp
// 错误码映射实现 + clean_errors.log 追加写入
#include "StdAfx.h"
#include "FrameWnd_ErrorMap.h"
#include "LogService.h"   // 统一日志入口（clean_errors.log）
#include <cstdio>
#include <fstream>   // [C17] std::wofstream
#include <iomanip>    // [C17] std::setw/setfill
#include <windows.h>

namespace ErrorMap {

    // exit code -> 友好中文提示 & 技术说明
    struct Entry {
        DWORD     dwExit;
        const wchar_t*   szFriendly;
        const wchar_t*   szTechnical;
    };

    static const Entry s_table[] = {
        // dism.exe / CBS 常见错误
        {   1, L"系统组件已损坏，建议先运行「系统清理」再重试。", L"dism exited 1: component store corruption" },
        { 1200, L"当前组件操作已完成，无需重复执行。",           L"DISM_S_ALREADY_INSTALLED / clean state" },
        { 2147942401, L"组件存储不一致，建议重启后重试。",      L"HRESULT 0x800F0906: component store inconsistent" },
        { 2147942410, L"无法定位组件信息，可尝试「还原系统」。", L"HRESULT 0x800F0912: no component info" },

        // Windows Update / WU 常见错误
        { 642,  L"服务未启动，请确认 Windows Update 服务已启用。", L"ERROR_SERVICE_NOT_INSTALLED" },
        { 665,  L"Windows Update 服务不可用，稍后重试。",          L"ERROR_NO_SUCH_LOGON_SESSION / WU unavailable" },
        { 740,  L"更新会话冲突，请关闭其它更新程序后重试。",       L"ERROR_IP_ADDRESS_CONFLICT / session conflict" },
        { 741,  L"更新会话忙，请稍后重试。",                       L"session busy" },
        { 800,  L"Windows Update 组件缺失，建议启用相关功能。",   L"WU API missing" },

        // CBS API 常见错误
        { 1618, L"另一个 Windows 更新进程正在运行，请稍后再试。",  L"ERROR_NO_WINDOWS_UPDATE_SERVER 1618" },
        { 3010, L"操作已成功，需要重启系统以完成更改。",           L"ERROR_SUCCESS_REBOOT_REQUIRED" },

        // 组件化 Windows / sfc / compact
        { 5000, L"组件已损坏，正在准备修复，请保持耐心。",        L"COMPACT_SFC_REPAIR_REQUIRED" },
        { 5001, L"需要重启以应用组件修复。",                       L"COMPACT_REBOOT_REQUIRED" },

        // 权限与文件锁
        {  5,   L"权限不足，请右键「以管理员身份运行」。",         L"ERROR_ACCESS_DENIED" },
        { 32,   L"文件被占用，请关闭相关程序后重试。",             L"ERROR_SHARING_VIOLATION" },
        {1066,   L"驱动或服务未正确加载。",                        L"ERROR_FAILED_SERVICE_START" },

        // WinVerifyTrust 失败
        { 692,  L"驱动签名校验失败，已跳过该驱动。",               L"WINERROR 692: signature invalid" },
        {1379,   L"驱动未通过 Windows 认证，已跳过。",             L"WINERROR 1379: not authenticated" },

        // ==== [A3] 扩展：dism.exe / HRESULT 常见值 ====
        {  16,  L"路径超出最大长度，请缩短目标文件名。",           L"ERROR_FILENAME_EXCESIVELY_LONG" },
        {  23,  L"路径未找到，请检查目录是否存在。",               L"ERROR_FILE_NOT_FOUND" },
        {  57,  L"目录不存在，请确认路径。",                       L"ERROR_BAD_NETPATH" },
        {  67,  L"指定的设备不存在。",                             L"ERROR_BAD_NET_NAME" },
        {  74,  L"无法重定位目录条目，可能被占用。",               L"ERROR_DIR_NOT_EMPTY" },
        {  91,  L"磁盘空间不足，请清理后重试。",                   L"ERROR_NOT_ENOUGH_MEMORY" },
        {1117,   L"Windows Installer 未安装。",                    L"ERROR_NO_MORE_ITEMS" },
        {1168,   L"文件已存在，无法创建。",                        L"ERROR_CANNOT_OPEN_FILE" },
        {2015,   L"更新组件存储已损坏，运行 sfc 后重试。",         L"0x80070000: component store inconsistent (2015)" },
        {0x800, L"网络请求失败，请检查网络后重试。",              L"network error 0x800" },
        {0x1000,L"WUA 返回通用错误，请查看错误详情。",            L"WUA generic error 0x1000" },

        // ==== [A3] HRESULT 0x800Fxxxx（组件存储相关） ====
        { 0x800F0806, L"组件存储不一致，建议重启后运行 sfc /scannow。", L"HRESULT 0x800F0806: component store inconsistent" },
        { 0x800F0906, L"组件存储不一致，建议重启后重试。",       L"HRESULT 0x800F0906: corrupted" },
        { 0x800F0912, L"无法定位组件信息。",                     L"HRESULT 0x800F0912" },
        { 0x800F0923, L"无匹配组件，跳过。",                     L"HRESULT 0x800F0923" },
        { 0x800F0F05, L"组件存储已损坏，建议 sfc /scannow。",    L"HRESULT 0x800F0F05" },
        { 0x800F0F07, L"组件存储已损坏（版本不匹配）。",          L"HRESULT 0x800F0F07" },
        { 0x800F0F08, L"组件存储需要重建，建议 sfc /scannow。",  L"HRESULT 0x800F0F08" },
        { 0x800F0F09, L"组件存储需要重建，建议 sfc /scannow。",  L"HRESULT 0x800F0F09" },
        { 0x800F0F10, L"组件存储损坏且不一致。",                  L"HRESULT 0x800F0F10" },
        { 0x800F0F11, L"组件存储不一致（需重建）。",              L"HRESULT 0x800F0F11" },
        { 0x800F0F12, L"组件存储不一致（需重建）。",              L"HRESULT 0x800F0F12" },
        { 0x800F0F16, L"Windows 需要修复，建议 sfc /scannow。",  L"HRESULT 0x800F0F16" },
        { 0x800F0F17, L"Windows 需要修复。",                      L"HRESULT 0x800F0F17" },
        { 0x800F0F18, L"Windows 需要修复，建议 sfc /scannow。",  L"HRESULT 0x800F0F18" },
        { 0x800F0F19, L"Windows 需要修复。",                      L"HRESULT 0x800F0F19" },
        { 0x800F0F20, L"Windows 需要修复。",                      L"HRESULT 0x800F0F20" },
        { 0x800F0F21, L"Windows 需要修复。",                      L"HRESULT 0x800F0F21" },
        { 0x800F0F22, L"Windows 需要修复。",                      L"HRESULT 0x800F0F22" },
        { 0x800F0F23, L"Windows 需要修复。",                      L"HRESULT 0x800F0F23" },
        { 0x800F0F24, L"Windows 需要修复。",                      L"HRESULT 0x800F0F24" },
        { 0x800F0F25, L"Windows 需要修复。",                      L"HRESULT 0x800F0F25" },
        { 0x800F0F26, L"Windows 需要修复。",                      L"HRESULT 0x800F0F26" },
        { 0x800F0F27, L"Windows 需要修复。",                      L"HRESULT 0x800F0F27" },
        { 0x800F0F28, L"Windows 需要修复。",                      L"HRESULT 0x800F0F28" },
        { 0x800F0F29, L"Windows 需要修复。",                      L"HRESULT 0x800F0F29" },
        { 0x800F0F30, L"Windows 需要修复。",                      L"HRESULT 0x800F0F30" },

        // ==== [A3] pnputil / 驱动管理 ====
        { 1658, L"pnputil 无法识别该驱动包。",                    L"ERROR_PNP_NO_DRIVER" },
        { 1659, L"pnputil 驱动包已损坏。",                        L"ERROR_PNP_DRIVER_CORRUPT" },
        { 1660, L"pnputil 驱动包正在被使用，稍后重试。",          L"ERROR_PNP_DRIVER_IN_USE" },
        { 1661, L"pnputil 驱动包未安装。",                        L"ERROR_PNP_NOT_INSTALLED" },

        // ==== [A3] 服务/系统状态 ====
        { 1040, L"服务未运行。",                                  L"ERROR_SERVICE_NOT_ACTIVE" },
        { 1060, L"服务未启动，请先启动 Windows Update。",         L"ERROR_DEPENDENT_SERVICES_RUNNING" },
        { 1064, L"服务启动超时。",                                L"ERROR_TIMEOUT" },

        // ==== [A3] Appx / 应用管理 ====
        { 1070, L"Appx 包未安装。",                                L"ERROR_APPX_NOT_INSTALLED" },
        { 1071, L"Appx 包已损坏，建议重新注册。",                  L"ERROR_APPX_CORRUPT" },

        // ==== [A3] 通用 HRESULT（HRESULT_FROM_WIN32） ====
        { 0x80070001, L"通用错误，请查看 clean_errors.log 详情。", L"HRESULT_FROM_WIN32(ERROR_GENERIC)" },
        { 0x80070002, L"找不到指定的文件。",                       L"HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)" },
        { 0x80070005, L"访问被拒绝，请右键「以管理员身份运行」。",  L"HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)" },
        { 0x8007001F, L"网络请求失败，请检查网络后重试。",        L"HRESULT_FROM_WIN32(ERROR_NO_MORE_ITEMS)" },
        { 0x80070057, L"系统资源不足。",                            L"HRESULT_FROM_WIN32(ERROR_BAD_NETPATH)" },
        { 0x8007007E, L"无法加载指定模块，组件可能未安装。",       L"HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND)" },
        { 0x800700FF, L"操作无法完成，请重试。",                   L"HRESULT_FROM_WIN32(ERROR_CANT_ACCESS_FILE)" },

        // ==== [R49] Appx / WinRT 常见错误码（扩充） ====
        { 1072, L"Appx 包已安装。",                                L"ERROR_APPX_ALREADY_INSTALLED" },
        { 1073, L"Appx 包依赖项缺失，请先安装依赖。",              L"ERROR_APPX_DEPENDENCY_MISSING" },
        { 1074, L"Appx 包注册失败，建议重新注册。",                L"ERROR_APPX_REGISTRATION_FAILED" },
        { 1075, L"Appx 包清理失败，请手动删除。",                  L"ERROR_APPX_CLEANUP_FAILED" },
        { 1076, L"Appx 包与当前系统不兼容。",                      L"ERROR_APPX_INCOMPATIBLE" },

        // ==== [R49] 计划任务常见错误码（扩充） ====
        { 15700, L"计划任务执行失败，请查看任务属性。",            L"SCHED_E_TASK_NOT_READY" },
        { 15701, L"计划任务已禁用，请启用后重试。",               L"SCHED_E_TASK_DISABLED" },
        { 15702, L"计划任务名称已存在，请修改后重试。",           L"SCHED_E_TASK_NAME_EXISTS" },
        { 15703, L"计划任务路径无效，请检查目录。",               L"SCHED_E_TASK_PATH_INVALID" },
        { 15704, L"计划任务权限不足，请右键「以管理员身份运行」。", L"SCHED_E_TASK_ACCESS_DENIED" },

        // ==== [R49] 文件关联常见错误码（扩充） ====
        { 15705, L"文件关联写入失败，请检查注册表权限。",         L"ASSOC_E_WRITE_FAILED" },
        { 15706, L"文件关联验证失败，请重新设置。",               L"ASSOC_E_VERIFY_FAILED" },
        { 15707, L"文件关联已存在，无需重复设置。",               L"ASSOC_E_ALREADY_EXISTS" },

        // ==== [R49] 网络相关错误码（扩充） ====
        { 7,   L"无法连接到网络，请检查网络连接。",               L"ERROR_INVALID_FUNCTION" },
        { 22,  L"网络请求参数无效，请重试。",                     L"ERROR_INVALID_DATA" },
        { 51,  L"网络连接被拒绝，请重试。",                       L"ERROR_NETOPEN_EXEC" },
        { 53,  L"网络连接失败，请检查网络。",                     L"ERROR_SEM_TIMEOUT" },
        { 64,  L"网络连接超时，请重试。",                         L"ERROR_INVALID_SUBARRAY" },
        { 87,  L"网络请求格式错误，请重试。",                     L"ERROR_INVALID_ADDRESS" },

        // ==== [R49] 磁盘相关错误码（扩充） ====
        { 112, L"磁盘空间不足，请清理后重试。",                   L"ERROR_DISK_FULL" },
        { 115, L"磁盘访问超时，请重试。",                         L"ERROR_IO_TIMEOUT" },
        { 116, L"磁盘写入失败，请检查磁盘状态。",                 L"ERROR_SEEK" },
        { 119, L"磁盘已写保护，请移除保护后重试。",               L"ERROR_NOT_ENOUGH_QUOTA" },

        // ==== [R49] 系统相关错误码（扩充） ====
        { 1794, L"系统组件损坏，建议运行 sfc /scannow。",        L"ERROR_SXS_CANT_GEN_ACTCTX" },
        { 1795, L"系统组件版本不匹配，建议更新。",               L"ERROR_SXS_ACTCTX_DEACTIVATION_FAILURE" },
        { 1796, L"系统组件加载失败，请重启。",                   L"ERROR_SXS_PARSE_FAILURE" },

        // ==== [R49] 常见 HRESULT（扩充） ====
        { 0x8007010E, L"网络请求被拒绝，请重试。",               L"HRESULT_FROM_WIN32(ERROR_NETNAME_DELETED)" },
        { 0x80070112, L"网络请求超时，请重试。",                 L"HRESULT_FROM_WIN32(ERROR_PIPE_DISCONNECTED)" },
        { 0x8007012F, L"网络服务不可用，请重试。",               L"HRESULT_FROM_WIN32(ERROR_SERVICE_NOTIFICATION)" },
    };

    inline constexpr int s_tableCount = static_cast<int>(sizeof(s_table) / sizeof(s_table[0]));

    static volatile LONG s_errorCount = 0;

    const wchar_t* GetFriendlyMessage(DWORD dwExit)
    {
        for (int i = 0; i < s_tableCount; ++i) {
            if (s_table[i].dwExit == dwExit) return s_table[i].szFriendly;
        }
        return L"操作未完成，请查看日志获取详细信息。";
    }

    const wchar_t* GetTechnicalMessage(DWORD dwExit)
    {
        for (int i = 0; i < s_tableCount; ++i) {
            if (s_table[i].dwExit == dwExit) return s_table[i].szTechnical;
        }
        return L"unknown exit code";
    }

    BOOL LogError(DWORD dwExit, const wchar_t* szSource, const wchar_t* szTarget)
    {
        InterlockedIncrement(&s_errorCount);

        // [M-路径统一] clean_errors.log 此前由本文件自行拼 EXE 根目录 + wofstream 直写，
        //   属于"绕过唯一入口的旁路"：落点在 EXE 根（装到 Program Files 会静默失败）、
        //   编码走 C locale（中文会被截断）、且与 UI 线程的批量写无互斥。
        //   改为走 LogService::Write：显式 UTF-8 + 互斥 + 统一目录 + 失败可计数。
        //   原格式里的 friendly/technical 说明是本模块的价值所在，保留在正文里。
        CDuiString sMsg;
        sMsg.Format(L"[clean-error] exit=%lu src=%s target=%s friendly=%s technical=%s",
                    static_cast<unsigned long>(dwExit),
                    szSource ? szSource : L"unknown",
                    szTarget ? szTarget : L"unknown",
                    GetFriendlyMessage(dwExit),
                    GetTechnicalMessage(dwExit));
        LogService::Write(ELogSeverity::kError, L"clean", sMsg.GetData());
        return TRUE;
    }

    [[nodiscard]] DWORD GetErrorCount()
    {
        return static_cast<DWORD>(s_errorCount);
    }
}
