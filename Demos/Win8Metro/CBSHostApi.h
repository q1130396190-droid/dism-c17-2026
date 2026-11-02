// ============================================================================
//  CBSHostApi.h
//
//  CBSHost.dll 的 C ABI 接口头 —— 主程序（Win8Metro.exe）与 CBSHost.dll
//  共同 include 同一份定义，保证跨边界结构体布局、函数签名一致。
//
//  约束（知识库《18_CBSHost_100%深度解析与复刻蓝图》）：
//    * 跨边界只传递 POD / wchar_t*（缓冲区由调用方拥有），不跨边界分配内存；
//      CBSHost.dll 使用 /MT 静态 CRT，主程序使用 /MD，故严禁跨边界
//      malloc/new 与 free/delete 混用。
//    * 所有字符串均为 UTF-16（Unicode）。
// ============================================================================

#ifndef CBS_HOST_API_H
#define CBS_HOST_API_H

#include <windows.h>

// [C++17 R-P8] 修复前轮遗留的编译回归：
//   本头同时被 C 语义（CBSHost 工程无 __cplusplus）
//   与 C++ 语义编译，故结构体两条路径都要合法。
//   C++12 起 `struct CBS_CONFIG {{ ... }};` 已同时注入类型名，
//   无需额外别名；C 路径则需补 C 风格 typedef。

#ifdef __cplusplus
extern "C" {
#endif

// ---------- 子命令数字 ID（知识库 E59/E84 权威枚举） ----------

inline constexpr unsigned CBS_CMD_START         = 0x60000u;  // Start：打开/启动会话
inline constexpr unsigned CBS_CMD_CLEANUP       = 0x60001u;  // Cleanup：组件存储清理
inline constexpr unsigned CBS_CMD_INSTALL       = 0x60002u;  // Install：安装能力/包
inline constexpr unsigned CBS_CMD_CLEANUP_IMAGE = 0x60003u;  // CleanupImage：映像清理

// [2026-10-08] 蓝图 3 个范围分支（FUN_a114/a144/a174）的显式命令化
inline constexpr unsigned CBS_CMD_CHECK_HEALTH   = 0x60004u;  // CheckHealth：快速健康标志检测
inline constexpr unsigned CBS_CMD_SCAN_HEALTH    = 0x60005u;  // ScanHealth：组件存储完整扫描
inline constexpr unsigned CBS_CMD_RESTORE_HEALTH = 0x60006u;  // RestoreHealth：自动修复映像损坏
inline constexpr unsigned CBS_CMD_QUERY_PACKAGES = 0x60007u;  // GetPackages：包状态枚举查询

// ---------- GetConfig 的配置结构（POD，跨边界固定布局） ----------

#pragma pack(push, 8)
struct CBS_CONFIG
{
    WCHAR sExeDir[MAX_PATH];       // 主程序目录
    WCHAR sScratchDir[MAX_PATH];   // 暂存目录
    WCHAR sLogDir[MAX_PATH];       // 日志目录
    WCHAR sMountRoot[MAX_PATH];    // 离线映像挂载点（空 = 在线）
    int   nLanguage;               // 多语言 ID（0 = 系统默认）
    BOOL  bNoviceMode;             // 新手模式
    BOOL  bExpertMode;             // 专家模式
};


// [C++17 R-P8] C 路径兼容：C 中 `struct CBS_CONFIG` 与裸名不同名，
//   补一个 C 风格 typedef 别名（等价原始 C++ 风格 typedef）。
#ifndef __cplusplus
typedef struct CBS_CONFIG CBS_CONFIG;
#endif
#pragma pack(pop)

// ---------- 6 个官方导出（序号 1-6，见 CBSHost.def） ----------

// 1. 创建 CBS 操作临时目录（GetTempPathW + GUID + CreateDirectoryW + ACL）。
//    成功返回 TRUE，路径写入 pszOut（cchOut 为元素数）；pdwErr 可空。
BOOL WINAPI CbsCreateTempDirectory2(wchar_t* pszOut, DWORD cchOut,
                                    DWORD* pdwErr);

// 2. 双模式对象工厂：bMode!=0 轻量包装器（pParam=外部上下文）；
//    bMode==0 完整对象（pParam 可指向 CBS_CONFIG，nullptr 用默认配置）。
//    返回不透明对象指针；nullptr 表示失败。
void* WINAPI CreateCbsHostHelper(void* pParam, unsigned char bMode);

// 3. 获取当前会话暂存目录路径。
BOOL WINAPI DismGetScratchDir(wchar_t* pszOut, DWORD cchOut);

// 4. 写日志（level 0..4：0=Error,1=Warn,2=Info,3=Verbose,4=Debug）。
void WINAPI DismWriteLog(int nLevel, const wchar_t* pszFormat, ...);

// 5. 读取配置。pszIni 为 nullptr 时使用主程序目录下 config.ini。
BOOL WINAPI GetConfig(const wchar_t* pszIni, CBS_CONFIG* pCfg);

// 6. CBS 主入口：COM 初始化 + 命令行解析 + 数字 ID 子命令派发。
//    pszErr/cchErr 可空；返回退出码（0 = 成功）。
int WINAPI RunCbsHostW(const wchar_t* pszCmdLine, wchar_t* pszErr, DWORD cchErr);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // CBS_HOST_API_H
