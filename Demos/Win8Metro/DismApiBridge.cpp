// ============================================================================
//  DismApiBridge.cpp
//
//  R30 v6.5 —— CBS 组件栈操作直连 dismapi.dll 的实现
//
//  签名说明：
//    * DismAddCapability / DismRemoveCapability / DismEnableFeature /
//      DismDisableFeature / DismAddDriver 均为 dismapi.h 官方公开导出，
//      本文件按官方头文件中的原样签名声明（不依赖 SDK 头文件路径）。
//    * _DismCleanImage 是 dismapi.dll 公开导出但未写入 dismapi.h 的扩展函数，
//      签名采用公开资料中的标准形式（Session / ResetBase / LogLevel /
//      CancelEvent / Progress / UserData），动态解析并做防御。
//
//  资源约定：
//    * 所有会话在函数栈内 Open→Use→Close，不在调用方残留。
//    * DismGetLastErrorMessage 返回的 DismString* 必须 DismDelete 释放。
//    * 动态解析的模块句柄在 Shutdown 中 FreeLibrary，析构安全。
//
// ============================================================================

#include "DismApiBridge.h"
#include "FormatW.h"

#include <memory>   // [A1] PerfScope 条件实例化用 unique_ptr
#include <cstdio>
#include <cstddef>          // offsetof
#include <cstdint>          // uintptr_t
#include <mutex>            // [卡顿修复] 多个后台枚举线程可能并发首调 Init
#include <condition_variable>   // [FIX-PERF] 枚举 single-flight 等待
#include <atomic>          // [R5-FIX] hrLast 多线程写：UI 枚举预热线程并发覆盖

// ---- dismapi.dll 官方公开导出签名（与 SDK dismapi.h 一致）----
using DismSession = UINT;
using DISM_PROGRESS_CALLBACK = void ( CALLBACK*)(_In_ UINT Current, _In_ UINT Total, _In_opt_ void* UserData);
struct DismString { PCWSTR Value; };
enum class DismLogLevel : int { DismLogErrors = 0, DismLogErrorsWarnings, DismLogErrorsWarningsInfo };
enum class DismPackageIdentifier : int { DismPackageNone = 0, DismPackageName, DismPackagePath };

inline constexpr wchar_t DISM_ONLINE_IMAGE_STR[] = L"DISM_{53BFAE52-B167-4E2F-A258-0A37B57FF845}";

// ---- 函数指针类型 ----
using PFN_DismInitialize = HRESULT ( WINAPI*)(DismLogLevel, PCWSTR, PCWSTR);
using PFN_DismShutdown = HRESULT ( WINAPI*)();
using PFN_DismOpenSession = HRESULT ( WINAPI*)(PCWSTR, PCWSTR, PCWSTR, DismSession*);
using PFN_DismCloseSession = HRESULT ( WINAPI*)(DismSession);
using PFN_DismGetLastErrorMessage = HRESULT ( WINAPI*)(DismString**);
using PFN_DismDelete = HRESULT ( WINAPI*)(void*);

using PFN_DismCleanImage = HRESULT ( WINAPI*)(DismSession, BOOL, DismLogLevel, HANDLE, DISM_PROGRESS_CALLBACK, void*);

using PFN_DismEnableFeature = HRESULT ( WINAPI*)(DismSession, PCWSTR, PCWSTR, DismPackageIdentifier,
    BOOL, PCWSTR*, UINT, BOOL, HANDLE, DISM_PROGRESS_CALLBACK, void*);
using PFN_DismDisableFeature = HRESULT ( WINAPI*)(DismSession, PCWSTR, PCWSTR, BOOL,
    HANDLE, DISM_PROGRESS_CALLBACK, void*);

using PFN_DismAddCapability = HRESULT ( WINAPI*)(DismSession, PCWSTR, BOOL, PCWSTR*, UINT,
    HANDLE, DISM_PROGRESS_CALLBACK, void*);
using PFN_DismRemoveCapability = HRESULT ( WINAPI*)(DismSession, PCWSTR,
    HANDLE, DISM_PROGRESS_CALLBACK, void*);

using PFN_DismAddDriver = HRESULT ( WINAPI*)(DismSession, PCWSTR, BOOL);

// [BUGFIX rb27 2026-10-13] DismDriverPackage 真实布局（最小复现程序对本机
// dismapi.dll 10.0.19041+ 实测；4 个完全一致的崩溃 dump 交叉验证）：
//   旧声明照搬 SDK 文档 7 字段、默认对齐(sizeof=56)解析，系统实际返回
//   pack(4) 的 100 字节扩展记录——在 InBox 后插入 CatalogFile，ClassName
//   实际在 +0x1C（旧代码按 +0x18 读），又插入 ClassGuid，ProviderName
//   后移至 +0x3C，Date 改为内嵌 SYSTEMTIME(+0x44)，Version 改为内嵌
//   4×DWORD(+0x54)。旧代码读 ClassName 时错位读跨记录拼接出的非法指针
//   （dump 观测值 0xFFFFFFFFFFFFFFFF），wcslen 解引用即 C0000005 必崩；
//   257(第三方)/892(全部)个元素实测无遗漏，数组以 PublishedName==nullptr
//   的哨兵记录结尾。该 API 无 Count 输出，真实步长运行时按哨兵探测
//   （100=本机/新布局，52=Win8/早期 SDK 公开布局），两种系统都能用。
#pragma pack(push, 4)

// v1：Windows SDK 公开布局（Win8 / 早期 Win10 dismapi.dll），52 字节
struct DismDriverPackageV1
{
    PCWSTR PublishedName;     // +0x00 oemN.inf
    PCWSTR OriginalFileName;  // +0x08 DriverStore 完整路径
    BOOL   InBox;             // +0x10
    PCWSTR ClassName;         // +0x14
    PCWSTR ProviderName;      // +0x1C
    PCWSTR Date;              // +0x24 字符串指针
    PCWSTR Version;           // +0x2C 字符串指针
};

// v2：本机实测扩展布局（dismapi.dll 19041 系列及以后），100 字节
struct DismDriverPackageV2
{
    PCWSTR    PublishedName;    // +0x00 oemN.inf
    PCWSTR    OriginalFileName; // +0x08 DriverStore 完整路径
    BOOL      InBox;            // +0x10
    PCWSTR    CatalogFile;      // +0x14 实测新增（如 iaAHCI.cat）
    PCWSTR    ClassName;        // +0x1C（SCSIAdapter/MEDIA/1394…）
    PCWSTR    ClassGuid;        // +0x24 实测新增（{4D36E97B-…}）
    PCWSTR    ClassDescription; // +0x2C 保留字段（实测内容特殊，UI 不使用）
    DWORD     dwFlags34;        // +0x34 标志（0/1，语义未文档化，不使用）
    DWORD     dwFlags38;        // +0x38 标志（实测恒 2，不使用）
    PCWSTR    ProviderName;     // +0x3C
    SYSTEMTIME stDate;          // +0x44 内嵌日期（实测 2018-08-03 等）
    DWORD     dwVerPart1;       // +0x54 版本四段（实测如 6.0.1.7899）
    DWORD     dwVerPart2;       // +0x58
    DWORD     dwVerPart3;       // +0x5C
    DWORD     dwVerPart4;       // +0x60
};

#pragma pack(pop)

static_assert(sizeof(DismDriverPackageV1) == 52,
    "DismDriverPackageV1 must be 52 bytes (pack 4)");
static_assert(sizeof(DismDriverPackageV2) == 100,
    "DismDriverPackageV2 must be 100 bytes (measured on dismapi.dll 19041+)");
static_assert(offsetof(DismDriverPackageV2, ClassName) == 0x1C,
    "DismDriverPackageV2.ClassName must be at 0x1C");
static_assert(offsetof(DismDriverPackageV2, ProviderName) == 0x3C,
    "DismDriverPackageV2.ProviderName must be at 0x3C");
static_assert(offsetof(DismDriverPackageV2, stDate) == 0x44,
    "DismDriverPackageV2.stDate must be at 0x44");
static_assert(offsetof(DismDriverPackageV2, dwVerPart1) == 0x54,
    "DismDriverPackageV2.dwVerPart1 must be at 0x54");

// 输出形参按 void** 声明：真实记录布局由 GetDrivers 运行时探测，
// 绝不让编译器按任何一种结构做指针算术语。
// [FIX-DRVCOUNT] 官方签名含 Count 输出参数（DismDriverPackage**, UINT*）：
// 旧定义漏掉 Count，导致 DISM 把栈垃圾当 UINT* 写入（栈损坏风险），且调用方
// 只能用"哨兵 0 + 步长猜测"推断元素数——单元素数组必然探测失败而报
// "unrecognized driver record layout"。补齐 Count 后按实际数量精确解析。
using PFN_DismGetDrivers = HRESULT ( WINAPI*)(DismSession, BOOL, void**, UINT*);
using PFN_DismRemoveDriver = HRESULT ( WINAPI*)(DismSession, PCWSTR);

using PFN_DismRemovePackage = HRESULT ( WINAPI*)(DismSession, PCWSTR, DismPackageIdentifier,
    HANDLE, DISM_PROGRESS_CALLBACK, void*);

// [P1-收尾] HRESULT DismAddPackage(Session, PackagePath, IgnoreCheck,
//   PreventPending, LogLevel, CustomDataPath, CancelEvent, Progress, UserData)
using PFN_DismAddPackage = HRESULT ( WINAPI*)(DismSession, PCWSTR, BOOL, BOOL,
    DismLogLevel, PCWSTR, HANDLE, DISM_PROGRESS_CALLBACK, void*);

// [P0-A1] HRESULT DismRemoveProvisionedAppxPackage(DismSession, PCWSTR PackageFamilyName)
using PFN_DismRemoveProvisionedAppx = HRESULT ( WINAPI*)(DismSession, PCWSTR);

// 健康状态码（DismImageHealthState）
inline constexpr int DISM_HEALTH_HEALTHY       = 0;
inline constexpr int DISM_HEALTH_REPAIRABLE    = 1;
inline constexpr int DISM_HEALTH_NONREPAIRABLE = 2;

using PFN_DismCheckImageHealth = HRESULT ( WINAPI*)(DismSession, BOOL, HANDLE,
    DISM_PROGRESS_CALLBACK, void*, DWORD*);
using PFN_DismRestoreImageHealth = HRESULT ( WINAPI*)(DismSession, PCWSTR*, UINT,
    BOOL, HANDLE, DISM_PROGRESS_CALLBACK, void*);

// [BUGFIX rb27] DismPackage 在系统 dismapi.h 中同样位于 #pragma pack(4)
// 区域。pack4 与默认对齐下本结构 sizeof 均为 32（SYSTEMTIME 对齐 2，
// 偏移 0x0E 恰好一致），历史上侥幸未错位；这里显式 pack(4) 与 static_assert
// 固化结论，避免编译选项变化后复发。
#pragma pack(push, 4)
struct DismPackage
{
    PCWSTR PackageName;
    int    PackageState;     // DismPackageFeatureState
    int    ReleaseType;      // DismReleaseType
    SYSTEMTIME InstallTime;
};
#pragma pack(pop)
static_assert(sizeof(DismPackage) == 32,
    "DismPackage must be 32 bytes (pack 4)");
using PFN_DismGetPackages = HRESULT ( WINAPI*)(DismSession, DismPackage**, UINT*);

// DismFeature / DismCapabilityInfo：DismGetFeatures / DismGetCapabilities
// 返回数组元素。
// [BUGFIX rb23] 经最小复现程序对系统 dismapi.dll（10.0.19041）实测确认：
//   1) 两个结构体都是 4 字节对齐的紧凑布局（PCWSTR+int，sizeof=12），
//      旧声明用默认对齐(sizeof=16)导致从第 2 个元素起全部错位；
//   2) 两个枚举 API 的真实导出参数顺序都是「数组指针在前、Count 在后」，
//      旧 typedef 把 Count 写在前面，API 返回 S_OK 却把数组指针写进
//      UINT 形参，随后解引用野指针 0xC0000005，工作线程被 SEH 吞掉、
//      UI 永久停在加载态。
#pragma pack(push, 4)

struct DismFeature
{
    PCWSTR FeatureName;
    int    State;            // DismPackageFeatureState
};

struct DismCapabilityInfo
{
    PCWSTR Name;
    int    State;            // DismPackageFeatureState
};

#pragma pack(pop)

static_assert(sizeof(DismFeature) == 12,
    "DismFeature must be 12 bytes (dismapi.dll uses #pragma pack(4))");
static_assert(sizeof(DismCapabilityInfo) == 12,
    "DismCapabilityInfo must be 12 bytes (dismapi.dll uses #pragma pack(4))");

using PFN_DismGetFeatures = HRESULT ( WINAPI*)(DismSession, PCWSTR, DismPackageIdentifier,
                                              DismFeature**, UINT*);
using PFN_DismGetCapabilities = HRESULT ( WINAPI*)(DismSession, DismCapabilityInfo**, UINT*);

// DismPackageFeatureState。本机 dismapi.dll 实测（dism.exe /english
// 交叉验证：cap 347 项、feat 135 项全量核对）：
//   0 = Not Present / Disabled，2 = Staged（功能显示 Disabled），
//   4 = Installed / Enabled（所有已安装能力、已启用功能均返回 4）。
// 旧代码按文档枚举取 Installed=5 与本机运行时不符，导致全部项显示为未安装。
// 5/6 作为其它系统版本/文档值一并按已安装处理，保证向前兼容。
inline constexpr int DISM_STATE_NOT_PRESENT   = 0;
inline constexpr int DISM_STATE_STAGED        = 2;
inline constexpr int DISM_STATE_INSTALLED     = 4;
inline constexpr int DISM_STATE_INSTALLED_DOC = 5;
inline constexpr int DISM_STATE_PERMANENT     = 6;

// ---- 状态 ----
namespace
{
    // [G2] 22 个 dismapi 函数指针 + 模块句柄 + 最近 HRESULT 收口进单一运行时
    // 结构，全文件经 Rt() 访问唯一静态实例；散放全局态归零。
    struct DismRuntime
    {
        HMODULE hMod = nullptr;
        // [R5-FIX] std::atomic<HRESULT>：多个后台枚举线程并发写 hrLast 与 UI 线程读
        // 存在数据竞争（UB），atomic 保证读写有序。operator= 和 operator HRESULT
        // 已支持隐式转换，现有赋值/读取代码无需改动。
        std::atomic<HRESULT> hrLast{ S_OK };

        PFN_DismInitialize          pInit = nullptr;
        PFN_DismShutdown            pShutdown = nullptr;
        PFN_DismOpenSession         pOpenSession = nullptr;
        PFN_DismCloseSession        pCloseSession = nullptr;
        PFN_DismGetLastErrorMessage pGetLastErr = nullptr;
        PFN_DismDelete              pDelete = nullptr;

        PFN_DismCleanImage          pCleanImage = nullptr;

        PFN_DismEnableFeature       pEnableFeature = nullptr;
        PFN_DismDisableFeature      pDisableFeature = nullptr;

        PFN_DismAddCapability       pAddCapability = nullptr;
        PFN_DismRemoveCapability    pRemoveCapability = nullptr;

        PFN_DismAddDriver           pAddDriver = nullptr;
        PFN_DismGetDrivers          pGetDrivers = nullptr;
        PFN_DismRemoveDriver        pRemoveDriver = nullptr;
        PFN_DismRemovePackage       pRemovePackage = nullptr;
        PFN_DismAddPackage          pAddPackage = nullptr;
        PFN_DismRemoveProvisionedAppx pRemoveProvAppx = nullptr;

        PFN_DismCheckImageHealth    pCheckImageHealth = nullptr;
        PFN_DismRestoreImageHealth  pRestoreImageHealth = nullptr;
        PFN_DismGetPackages         pGetPackages = nullptr;
        PFN_DismGetFeatures         pGetFeatures = nullptr;
        PFN_DismGetCapabilities     pGetCapabilities = nullptr;

        template<typename T> T LoadProc(const char* name)
        {
            return reinterpret_cast<T>(::GetProcAddress(hMod, name));
        }

        // 卸载/初始化失败时必须同步清空全部已解析指针；
        // 否则 p* 仍指向已卸载的 dismapi.dll，后续入口的非空判断失效，
        // 间接调用落入已释放内存（AV / CFG FastFail c0000409#10）。
        void ResetProcs()
        {
            pInit = nullptr;
            pShutdown = nullptr;
            pOpenSession = nullptr;
            pCloseSession = nullptr;
            pGetLastErr = nullptr;
            pDelete = nullptr;
            pCleanImage = nullptr;
            pEnableFeature = nullptr;
            pDisableFeature = nullptr;
            pAddCapability = nullptr;
            pRemoveCapability = nullptr;
            pAddDriver = nullptr;
            pGetDrivers = nullptr;
            pRemoveDriver = nullptr;
            pRemovePackage = nullptr;
            pAddPackage = nullptr;
            pRemoveProvAppx = nullptr;
            pCheckImageHealth = nullptr;
            pRestoreImageHealth = nullptr;
            pGetPackages = nullptr;
            pGetFeatures = nullptr;
            pGetCapabilities = nullptr;
        }
    };

    DismRuntime& Rt()
    {
        static DismRuntime s_rt;
        return s_rt;
    }

    // [BUG-02 修复] 只从 System32 按完整路径加载 dismapi.dll，杜绝高完整性
    // 进程下的应用目录同名 DLL 植入；不做裸名回退（裸名搜索顺序第一命中
    // 应用目录，正是劫持入口）。与 NCleaner 的 SafeLoadSystem32Dll 同策略。
    HMODULE LoadDismApiFromSystem32()
    {
        wchar_t szSys[MAX_PATH] = { 0 };
        UINT cch = ::GetSystemDirectoryW(szSys, _countof(szSys));
        if (cch == 0 || cch >= _countof(szSys)) return nullptr;

        std::wstring sFull(szSys);
        if (!sFull.empty() && sFull.back() != L'\\') sFull += L'\\';
        sFull += L"dismapi.dll";

        HMODULE h = ::LoadLibraryExW(sFull.c_str(), nullptr,
                                     LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (h != nullptr) return h;

        // 极老系统无 SEARCH_SYSTEM32 标志语义时，显式绝对路径加载仍然
        // 不经过搜索顺序，无劫持面。
        return ::LoadLibraryExW(sFull.c_str(), nullptr, 0);
    }
}

namespace DismApiBridge
{

// [卡顿修复] 驱动页/更新页/功能页的后台枚举线程可能同时首调 Init，
// 串行化 LoadLibrary/解析/DismInitialize，避免 g_p* 竞态与 DismInitialize 重入。
static std::mutex& InitMutex()
{
    static std::mutex s_mtx;
    return s_mtx;
}

bool Init(const wchar_t* pszLogFile)
{
    std::lock_guard<std::mutex> lk(InitMutex());
    if (IsReady()) return true;                       // 幂等
    if (Rt().hMod) return false;                          // 已尝试过但失败

    Rt().hMod = LoadDismApiFromSystem32();   // [BUG-02] System32 限定，禁止裸名
    if (!Rt().hMod)
    {
        Rt().hrLast = HRESULT_FROM_WIN32(::GetLastError());
        return false;
    }

    Rt().pInit           = Rt().LoadProc<PFN_DismInitialize>("DismInitialize");
    Rt().pShutdown       = Rt().LoadProc<PFN_DismShutdown>("DismShutdown");
    Rt().pOpenSession    = Rt().LoadProc<PFN_DismOpenSession>("DismOpenSession");
    Rt().pCloseSession   = Rt().LoadProc<PFN_DismCloseSession>("DismCloseSession");
    Rt().pGetLastErr     = Rt().LoadProc<PFN_DismGetLastErrorMessage>("DismGetLastErrorMessage");
    Rt().pDelete         = Rt().LoadProc<PFN_DismDelete>("DismDelete");

    Rt().pCleanImage     = Rt().LoadProc<PFN_DismCleanImage>("_DismCleanImage");

    Rt().pEnableFeature    = Rt().LoadProc<PFN_DismEnableFeature>("DismEnableFeature");
    Rt().pDisableFeature   = Rt().LoadProc<PFN_DismDisableFeature>("DismDisableFeature");

    Rt().pAddCapability    = Rt().LoadProc<PFN_DismAddCapability>("DismAddCapability");
    Rt().pRemoveCapability = Rt().LoadProc<PFN_DismRemoveCapability>("DismRemoveCapability");

    Rt().pAddDriver          = Rt().LoadProc<PFN_DismAddDriver>("DismAddDriver");
    Rt().pGetDrivers         = Rt().LoadProc<PFN_DismGetDrivers>("DismGetDrivers");
    Rt().pRemoveDriver       = Rt().LoadProc<PFN_DismRemoveDriver>("DismRemoveDriver");
    Rt().pRemovePackage      = Rt().LoadProc<PFN_DismRemovePackage>("DismRemovePackage");
    Rt().pAddPackage         = Rt().LoadProc<PFN_DismAddPackage>("DismAddPackage");
    Rt().pRemoveProvAppx     = Rt().LoadProc<PFN_DismRemoveProvisionedAppx>("DismRemoveProvisionedAppxPackage");

    Rt().pCheckImageHealth   = Rt().LoadProc<PFN_DismCheckImageHealth>("DismCheckImageHealth");
    Rt().pRestoreImageHealth = Rt().LoadProc<PFN_DismRestoreImageHealth>("DismRestoreImageHealth");
    Rt().pGetPackages        = Rt().LoadProc<PFN_DismGetPackages>("DismGetPackages");
    Rt().pGetFeatures        = Rt().LoadProc<PFN_DismGetFeatures>("DismGetFeatures");
    Rt().pGetCapabilities    = Rt().LoadProc<PFN_DismGetCapabilities>("DismGetCapabilities");

    // 核心路径缺一即判定不可用（调用方回退 dism.exe）
    if (!Rt().pInit || !Rt().pShutdown || !Rt().pOpenSession || !Rt().pCloseSession ||
        !Rt().pCleanImage || !Rt().pAddDriver)
    {
        Rt().hrLast = HRESULT_FROM_WIN32(::GetLastError());
        Rt().ResetProcs();
        ::FreeLibrary(Rt().hMod);
        Rt().hMod = nullptr;
        return false;
    }

    // DismInitialize：日志级别取 ErrorsWarningsInfo，写入可选日志文件，scratch 用默认
    //
    // [C-03 方案甲] 借编译形态下本编译单元在 EXE(Win8Metro.exe) 与
    // DLL(CBSHost.dll) 中各存在一份静态副本：DismRuntime/函数指针表不共享，
    // 各调一次 DismInitialize。dismapi 对同进程多次初始化按引用计数处理，
    // 属可接受的纯资源冗余（两份会话表），不追求单实例；System32 限定加载
    // (BUG-02) 已消除双实例原先伴随的植入口风险。LastHResult() 仅反映本副本
    // 内最后一次失败，跨副本不可见——双方各自只消费自身初始化结果。
    HRESULT hr = Rt().pInit(DismLogLevel::DismLogErrorsWarningsInfo, pszLogFile, nullptr);
    if (FAILED(hr))
    {
        // 注意：DismInitialize 失败（未提权 0x800702e4、服务锁定等）后，
        // 必须先清空所有 g_p* 再 FreeLibrary，杜绝悬垂指针调用。
        // 保存 HRESULT 供调用方给出准确失败原因（如需要管理员权限）。
        Rt().hrLast = hr;
        Rt().ResetProcs();
        ::FreeLibrary(Rt().hMod);
        Rt().hMod = nullptr;
        return false;
    }
    Rt().hrLast = S_OK;
    return true;
}

void Shutdown()
{
    // [EXIT-FIX] 进程退出阶段不再调用 DismShutdown / FreeLibrary：
    //   1) detached 工作线程可能正阻塞在 dismapi 的 CBS ALPC 同步调用中，
    //      DismShutdown 与 FreeLibrary(DLL_PROCESS_DETACH) 在加载器锁上
    //      与之互锁，是退出僵尸化的根因之一；
    //   2) 客户端异常退出时 CBS 服务端会话随 ALPC 端口关闭自动回收，
    //      无需客户端显式 Shutdown。
    // 模块与函数指针表保持原样，由操作系统在进程终结时统一回收。
    // 初始化失败路径（Init 内）的 FreeLibrary 保持不变。
}

bool IsReady()
{
    return (Rt().hMod != nullptr) &&
           Rt().pInit && Rt().pShutdown && Rt().pOpenSession && Rt().pCloseSession &&
           Rt().pCleanImage && Rt().pAddDriver;
}

std::wstring GetLastErrorText()
{
    if (!Rt().pGetLastErr || !Rt().pDelete) return L"";
    DismString* pStr = nullptr;
    HRESULT hr = Rt().pGetLastErr(&pStr);
    if (FAILED(hr) || !pStr) return L"";
    std::wstring ws = pStr->Value ? pStr->Value : L"";
    Rt().pDelete(pStr);
    return ws;
}

UINT OpenImageSession(const wchar_t* pszImageRoot, std::wstring* pErr)
{
    // 必须同时确认模块仍驻留：Init 失败/Shutdown 后 g_p* 虽已清空，
    // 但 Rt().hMod 是模块生命周期的唯一权威判据。
    if (!Rt().hMod || !Rt().pOpenSession)
    {
        if (pErr) *pErr = L"dismapi: unavailable (DismInitialize failed or unloaded)";
        return 0;
    }
    DismSession sess = 0;
    // nullptr/空串 → 在线常量；否则按离线映像根打开（等价 dism /Image:"<root>"）
    PCWSTR pszTarget = (pszImageRoot && *pszImageRoot) ? pszImageRoot : DISM_ONLINE_IMAGE_STR;
    HRESULT hr = Rt().pOpenSession(pszTarget, nullptr, nullptr, &sess);
    Rt().hrLast = hr;
    if (FAILED(hr))
    {
        if (pErr)
        {
            std::wstring e = GetLastErrorText();
            *pErr = e.empty() ? L"OpenImageSession failed" : e;
        }
        return 0;
    }
    return static_cast<UINT>(sess);
}

UINT OpenOnlineSession(std::wstring* pErr)
{
    return OpenImageSession(nullptr, pErr);
}

void CloseSession(UINT sess)
{
    if (!sess) return;
    if (Rt().pCloseSession)
    {
        Rt().hrLast = Rt().pCloseSession((DismSession)sess);
    }
}

// [R16][O-5] Feature/Capability 在线枚举共享同一 DismSession：
// DismOpenSession（CBS ALPC 建立）是枚举固有耗时大头，Init 幂等后
// 两页仍各自开会话属重复初始化。DismSession 非线程安全，共享会话
// 使用期间整调用持本互斥量串行；离线会话（pszImageRoot 非空）不
// 纳入共享，仍按调用独立开关。API 失败即作废共享句柄，下次重开。
// [B4 2026-10-25] 双会话并行（对应审核报告 O6）。
//   原实现 Feature 与 Capability 共用同一把锁 + 同一个 DismSession：
//   两页连开时第二页必须等第一页整轮枚举跑完（实测 0.3~3s 排队）。
//   但两者枚举的是完全不同的 API（GetFeatures / GetCapabilities），
//   业务上没有互斥需求——串行只是"共享一个非线程安全句柄"的副产物。
//
//   改为按用途各持一套会话与锁：同一用途内仍然串行（DismSession 非线程安全
//   这条约束不能破），不同用途之间可以并行。
//
//   代价：多一个 CBS 会话句柄（内存与 ALPC 连接各一份），可接受。
//   风险：若 CBS 内部对枚举有全局串行限制，并行收益会打折——但那只是
//   退化成原来的排队，不会出错。**真实并行度需实机采样确认**，本轮无法验证。
enum class EEnumDomain { Feature, Capability, Package };

static std::mutex& SharedOnlineMutex(EEnumDomain eDomain)
{
    static std::mutex s_mtxFeat;
    static std::mutex s_mtxCap;
    static std::mutex s_mtxPkg;
    switch (eDomain)
    {
    case EEnumDomain::Feature:    return s_mtxFeat;
    case EEnumDomain::Capability: return s_mtxCap;
    default:                      return s_mtxPkg;
    }
}

static DismSession& SharedSessionRef(EEnumDomain eDomain)
{
    static DismSession s_sessFeat = 0;
    static DismSession s_sessCap  = 0;
    static DismSession s_sessPkg  = 0;
    switch (eDomain)
    {
    case EEnumDomain::Feature:    return s_sessFeat;
    case EEnumDomain::Capability: return s_sessCap;
    default:                      return s_sessPkg;
    }
}


// ============================================================
// [FIX-PERF] 枚举 single-flight
//
// 目标：同一时刻同类枚举只真正调用一次 DISM。
// 预热线程已在跑 Feature 枚举时，前台请求不再排队后再跑一遍，
// 而是等待并复用同一份结果（失败也共享同一失败结论）。
//
// 实现边界：只包在 GetFeatures/GetCapabilities 外层，
// 不进入 SharedOnlineMutex 内部，因此不改变 DismSession 的
// “非线程安全、调用必须在锁内”这一既有约束。
// ============================================================
struct EnumFlight
{
    std::mutex              mtx;
    std::condition_variable cv;
    bool                    running = false;
    bool                    done = false;
    bool                    ok = false;
    std::wstring            err;
    std::vector<FeatureInfo>    feat;
    std::vector<CapabilityInfo> cap;
};

// [A-1 2026-10-25] flight 等待封顶。
//   原实现 f.cv.wait(...) 无超时：预热线程以 BACKGROUND 模式（CPU+I/O 双最低）
//   跑整轮枚举时，前台进页会在条件变量上干等这一整轮被降速的枚举，实测 2~15s。
//   封顶 3s：超时后前台不再干等，直接自己发起一次枚举（single-flight 仍防并发重复，
//   因为下面对 f.running 的重判在锁内）。代价是极端情况下多一次枚举，
//   但用户看到的首屏从"最多 15s 转环"变成"最多 3s 转环"。
static constexpr auto kFlightWaitCap = std::chrono::seconds(3);

static EnumFlight& FeatFlight() { static EnumFlight f; return f; }
static EnumFlight& CapFlight()  { static EnumFlight f; return f; }

// pszImageRoot 非空（离线镜像）时不做跨请求合并：不同镜像必须各自枚举。

// [G2] 会话样板折叠：按 imageRoot 开在线/离线会话 → 执行 fn(sess)（调用体内
// 完成 dismapi 调用与输出内存释放）→ 关会话 → 失败统一取 Dism 错误文本。
// pszFailTag 仅在 Dism 无错误文本时组成 "<Tag> failed"。
template<class Fn>
static bool RunWithSession(const wchar_t* pszImageRoot, std::wstring* pErr,
                           const wchar_t* pszFailTag, Fn&& fn)
{
    std::wstring sErr;
    DismSession sess = (pszImageRoot && *pszImageRoot)
        ? OpenImageSession(pszImageRoot, &sErr)
        : OpenOnlineSession(&sErr);
    if (!sess) { if (pErr) *pErr = sErr; return false; }

    HRESULT hr = fn(sess);
    Rt().hrLast = hr;
    CloseSession(static_cast<UINT>(sess));

    if (FAILED(hr))
    {
        if (pErr)
        {
            std::wstring e = GetLastErrorText();
            *pErr = e.empty() ? (std::wstring(pszFailTag) + L" failed") : e;
        }
        return false;
    }
    return true;
}

// [G2] Feature/Capability 枚举的共享/离线会话获取样板（两份枚举函数共用）。
// 在线持共享互斥量并复用共享会话；离线按映像根独立打开。
static bool AcquireEnumSession(EEnumDomain eDomain, const wchar_t* pszImageRoot,
                               std::wstring* pErr,
                               std::unique_lock<std::mutex>& lk,
                               DismSession& sess, bool& bShared)
{
    std::wstring sErr;
    bShared = (!pszImageRoot || !*pszImageRoot);
    if (bShared)
    {
        // [B4] 按用途取各自的锁与会话：同域内仍串行（DismSession 非线程安全），
        //      跨域可并行。
        lk = std::unique_lock<std::mutex>(SharedOnlineMutex(eDomain));
        DismSession& rSess = SharedSessionRef(eDomain);
        if (!rSess)
            rSess = OpenOnlineSession(&sErr);
        sess = rSess;
    }
    else
    {
        sess = OpenImageSession(pszImageRoot, &sErr);
    }
    if (!sess) { if (pErr) *pErr = sErr; return false; }
    return true;
}

// [G2] 共享会话 API 失败即作废，下次调用重开；离线会话每次关闭。
static void ReleaseEnumSession(EEnumDomain eDomain, DismSession sess, bool bShared, HRESULT hr)
{
    if (!bShared) CloseSession(static_cast<UINT>(sess));
    else if (FAILED(hr))
    {
        CloseSession(static_cast<UINT>(sess));
        SharedSessionRef(eDomain) = 0;   // 只作废本域句柄，不影响另一域
    }
}

HRESULT LastHResult() { return Rt().hrLast; }
void SetLastHResult(HRESULT hr) { Rt().hrLast = hr; }

bool CleanComponentStore(bool bResetBase, std::wstring* pErr,
                         const wchar_t* pszImageRoot)
{
    if (!Rt().pCleanImage) { if (pErr) *pErr = L"dismapi: _DismCleanImage unavailable"; return false; }

    // [G2] 会话样板折叠；ResetBase=false → 普通 StartComponentCleanup；
    // true → 加 /ResetBase。ERROR_SUCCESS_REBOOT_REQUIRED(3010) 由 FAILED 判为成功。
    return RunWithSession(pszImageRoot, pErr, L"CleanComponentStore",
        [&](DismSession sess) -> HRESULT
        {
            return Rt().pCleanImage(sess, bResetBase ? TRUE : FALSE,
                                    DismLogLevel::DismLogErrorsWarningsInfo,
                                    nullptr, nullptr, nullptr);
        });
}

// [R7] 带真实进度的组件清理。DISM 原生回调签名为 (Current, Total, UserData)，
// 这里用一个薄跳板转发到调用方的简化签名（不含 DISM 依赖的类型），
// 让上层页面无需包含 dismapi 头。
namespace
{
struct CleanProgressBridge
{
    DismApiBridge::CleanProgressFn pfn = nullptr;
    void* pUser = nullptr;
};

void CALLBACK CleanProgressTrampoline(UINT nCurrent, UINT nTotal, void* pUser)
{
    CleanProgressBridge* pB = static_cast<CleanProgressBridge*>(pUser);
    if (pB && pB->pfn) pB->pfn(nCurrent, nTotal, pB->pUser);
}
} // namespace

bool CleanComponentStoreEx(bool bResetBase, std::wstring* pErr,
                           CleanProgressFn pfnProgress, void* pUser,
                           const wchar_t* pszImageRoot)
{
    if (!Rt().pCleanImage)
    { if (pErr) *pErr = L"dismapi: _DismCleanImage unavailable"; return false; }

    CleanProgressBridge bridge;
    bridge.pfn = pfnProgress;
    bridge.pUser = pUser;
    const bool bWantProgress = (pfnProgress != nullptr);

    return RunWithSession(pszImageRoot, pErr, L"CleanComponentStoreEx",
        [&](DismSession sess) -> HRESULT
        {
            return Rt().pCleanImage(sess, bResetBase ? TRUE : FALSE,
                                    DismLogLevel::DismLogErrorsWarningsInfo,
                                    nullptr,
                                    bWantProgress ? &CleanProgressTrampoline : nullptr,
                                    bWantProgress ? &bridge : nullptr);
        });
}

bool EnableFeature(const std::wstring& sName, const std::wstring& sSource,
                   bool bLimitAccess, std::wstring* pErr, const wchar_t* pszImageRoot)
{
    if (!Rt().pEnableFeature) { if (pErr) *pErr = L"dismapi: DismEnableFeature unavailable"; return false; }

    PCWSTR szSource = sSource.empty() ? nullptr : sSource.c_str();
    PCWSTR srcArr[1] = { szSource };

    return RunWithSession(pszImageRoot, pErr, L"EnableFeature",
        [&](DismSession sess) -> HRESULT
        {
            return Rt().pEnableFeature(sess, sName.c_str(), nullptr, DismPackageIdentifier::DismPackageNone,
                                       bLimitAccess ? TRUE : FALSE,
                                       szSource ? srcArr : nullptr,
                                       szSource ? 1u : 0u,
                                       FALSE,                       // EnableAll
                                       nullptr, nullptr, nullptr);  // Cancel / Progress / UserData
        });
}

bool DisableFeature(const std::wstring& sName, bool bRemovePayload, std::wstring* pErr,
                    const wchar_t* pszImageRoot)
{
    if (!Rt().pDisableFeature) { if (pErr) *pErr = L"dismapi: DismDisableFeature unavailable"; return false; }

    return RunWithSession(pszImageRoot, pErr, L"DisableFeature",
        [&](DismSession sess) -> HRESULT
        {
            return Rt().pDisableFeature(sess, sName.c_str(), nullptr,
                                        bRemovePayload ? TRUE : FALSE,
                                        nullptr, nullptr, nullptr);
        });
}

bool AddCapability(const std::wstring& sName, const std::wstring& sSource,
                   bool bLimitAccess, std::wstring* pErr, const wchar_t* pszImageRoot)
{
    if (!Rt().pAddCapability) { if (pErr) *pErr = L"dismapi: DismAddCapability unavailable"; return false; }

    PCWSTR szSource = sSource.empty() ? nullptr : sSource.c_str();
    PCWSTR srcArr[1] = { szSource };

    return RunWithSession(pszImageRoot, pErr, L"AddCapability",
        [&](DismSession sess) -> HRESULT
        {
            return Rt().pAddCapability(sess, sName.c_str(),
                                       bLimitAccess ? TRUE : FALSE,
                                       szSource ? srcArr : nullptr,
                                       szSource ? 1u : 0u,
                                       nullptr, nullptr, nullptr);
        });
}

bool RemoveCapability(const std::wstring& sName, std::wstring* pErr,
                      const wchar_t* pszImageRoot)
{
    if (!Rt().pRemoveCapability) { if (pErr) *pErr = L"dismapi: DismRemoveCapability unavailable"; return false; }

    return RunWithSession(pszImageRoot, pErr, L"RemoveCapability",
        [&](DismSession sess) -> HRESULT
        {
            return Rt().pRemoveCapability(sess, sName.c_str(),
                                          nullptr, nullptr, nullptr);
        });
}

bool AddDriver(const std::wstring& sPath, std::wstring* pErr,
               const wchar_t* pszImageRoot, bool bForceUnsigned)
{
    if (!Rt().pAddDriver) { if (pErr) *pErr = L"dismapi: DismAddDriver unavailable"; return false; }

    // [P1-A6] pszImageRoot 非空走离线挂载会话（DismAddDriver 对离线映像有效）
    return RunWithSession(pszImageRoot, pErr, L"AddDriver",
        [&](DismSession sess) -> HRESULT
        {
            return Rt().pAddDriver(sess, sPath.c_str(),
                                   bForceUnsigned ? TRUE : FALSE);
        });
}

// [P1-A6] 移除第三方驱动：DismRemoveDriver(Session, "oemN.inf")
bool RemoveDriver(const std::wstring& sInf, std::wstring* pErr,
                  const wchar_t* pszImageRoot)
{
    if (!Rt().pRemoveDriver)
    { if (pErr) *pErr = L"dismapi: DismRemoveDriver unavailable"; return false; }
    if (sInf.empty())
    { if (pErr) *pErr = L"RemoveDriver: empty inf name"; return false; }

    return RunWithSession(pszImageRoot, pErr, L"RemoveDriver",
        [&](DismSession sess) -> HRESULT
        {
            return Rt().pRemoveDriver(sess, sInf.c_str());
        });
}

// ============================================================
// GetDrivers 运行时布局适配（rb27）
//
// dismapi.dll 不同版本返回两种记录（见文件顶部结构注释）：
//   100 字节扩展布局（19041+ 实测）/ 52 字节 SDK 公开布局（旧系统）。
// 该 API 没有 Count 输出，数组以 PublishedName==nullptr 的哨兵记录结尾。
// 下列所有对 API 返回内存的读取一律走 SEH 保护：即使未来系统再改布局、
// 返回坏指针或缓冲被截断，也只丢弃字段/整体返回失败，绝不允许 C0000005
// 穿透到工作线程杀进程。
// ============================================================

// [G2] SEH 探针族保持原位（不拆分文件），全部归入 driver_layout 具名域。
namespace driver_layout {

// 纯 SEH 内存读：不可读返回 false。本函数及下面带 __try 的函数禁止
// 出现需要析构展开的 C++ 对象（MSVC C2712）。
static bool DrvSafeRead(const void* pvSrc, void* pvDst, size_t cb)
{
    __try
    {
        memcpy(pvDst, pvSrc, cb);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// 判断整型值是否像一个指向可读、非空宽字符串的指针（不限制字符集，
// 类名/厂商可能含本地代码页字符）。
static bool DrvLooksLikeWStr(unsigned long long ull)
{
    if (ull < 0x10000ULL || ull > 0x7FFFFFFFFFFFULL) return false;
    wchar_t wFirst = 0;
    if (!DrvSafeRead((const void*)(uintptr_t)ull, &wFirst, sizeof(wchar_t)))
        return false;
    return wFirst != 0 && wFirst >= 0x20;
}

// 按候选步长向哨兵行走：每个活跃元素的 PublishedName 必须可读，
// 遇到 nullptr 槽结束。返回哨兵前元素数（>=0，0=空表）；-1=该步长不成立。
static int DrvWalkCount(const BYTE* pbBase, size_t cbStride)
{
    constexpr int kMaxElems = 8192; // all=TRUE 本机实测 892；8K 上限足够
    for (int i = 0; i < kMaxElems; ++i)
    {
        const BYTE* e = pbBase + static_cast<size_t>(i) * cbStride;
        unsigned long long ullPub = 0;
        if (!DrvSafeRead(e, &ullPub, sizeof(ullPub))) return -1;
        if (ullPub == 0) return i; // 哨兵
        if (!DrvLooksLikeWStr(ullPub)) return -1;
    }
    return -1;
}

// 探测真实记录步长：优先 100（19041+ 实测），52（SDK 公开布局）兜底。
static size_t DrvProbeStride(const BYTE* pbBase)
{
    static const size_t kCandidates[] = { 100, 52 };
    for (size_t k = 0; k < sizeof(kCandidates)/sizeof(kCandidates[0]); ++k)
    {
        if (DrvWalkCount(pbBase, kCandidates[k]) >= 0)
            return kCandidates[k];
    }
    return 0;
}

// SEH 保护的宽字符串拷贝：空/坏指针→空串；超长截断到 1023；
// 读取中途 AV 保留已拷贝的前缀（__try 内的栈赋值在异常跳出时保留）。
static void DrvDupStr(const wchar_t* pszSrc, std::wstring& wsOut)
{
    wsOut.clear();
    if (!pszSrc) return;
    constexpr int kLineBufWchars = 1024;   // 1024 缓冲尺寸（语义命名，数值不变）
    wchar_t szBuf[kLineBufWchars];
    szBuf[0] = 0;
    bool bHitTerm = false;
    __try
    {
        for (int i = 0; i < 1023; ++i)
        {
            wchar_t w = pszSrc[i];
            szBuf[i] = w;
            if (w == 0) { bHitTerm = true; break; }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        // 保留前缀，下面强制补终止符
    }
    szBuf[1023] = 0;
    if (!bHitTerm)
    {
        // AV 或超长：确保缓冲内有 0 终止
        for (int i = 0; i < 1023; ++i)
            if (szBuf[i] == 0) { bHitTerm = true; break; }
        if (!bHitTerm) szBuf[1023] = 0;
    }
    wsOut = szBuf;
}

// v2 内嵌 SYSTEMTIME → L"YYYY-MM-DD"；非法/零值→空串
static std::wstring DrvFormatStDate(const SYSTEMTIME* pstSrc)
{
    SYSTEMTIME st;
    if (!pstSrc || !DrvSafeRead(pstSrc, &st, sizeof(st))) return std::wstring();
    if (st.wYear < 1601 || st.wYear > 9999) return std::wstring();
    if (st.wMonth < 1 || st.wMonth > 12) return std::wstring();
    if (st.wDay < 1 || st.wDay > 31) return std::wstring();
    wchar_t szBuf[24];
    if (wfmt::FormatTo(szBuf, 24, L"%04u-%02u-%02u",
                   (unsigned)st.wYear, (unsigned)st.wMonth,
                   (unsigned)st.wDay) <= 0)
        return std::wstring();
    return std::wstring(szBuf);
}

// v2 内嵌 4×DWORD 版本 → L"a.b.c.d"；全零→空串
static std::wstring DrvFormatVersion(const DWORD* pdwSrc)
{
    DWORD v[4] = { 0, 0, 0, 0 };
    if (!pdwSrc || !DrvSafeRead(pdwSrc, v, sizeof(v))) return std::wstring();
    if (v[0] == 0 && v[1] == 0 && v[2] == 0 && v[3] == 0)
        return std::wstring();
    wchar_t szBuf[48];
    if (wfmt::FormatTo(szBuf, 48, L"%lu.%lu.%lu.%lu",
                   (unsigned long)v[0], (unsigned long)v[1],
                   (unsigned long)v[2], (unsigned long)v[3]) <= 0)
        return std::wstring();
    return std::wstring(szBuf);
}

// 从记录指定偏移安全读出 PCWSTR 并拷贝
static void DrvDupField(const BYTE* eRec, size_t off, std::wstring& wsOut)
{
    unsigned long long ull = 0;
    if (!DrvSafeRead(eRec + off, &ull, sizeof(ull))) { wsOut.clear(); return; }
    DrvDupStr((const wchar_t*)(uintptr_t)ull, wsOut);
}

// [N3] 原 GetDrivers 内 6 层嵌套（SUCCEEDED→步长→计数→for→if(V2)）
// 提取为单一路径解析：步长/哨兵失败即早返回；元素循环最深 2 层。
// [FIX-DRVCOUNT] nCount 由 DismGetDrivers 的 Count 出参给出（权威值）：
// nCount>0 时先按 count 校验首元素可读，再直接按 count 解析；仅当 count
// 不可用（0）时才回退旧的哨兵/步长探测。
static bool ParseDriverArray(const void* pArrRaw, UINT nCount,
                             std::vector<DriverInfo>& vOut,
                             std::wstring* pErr)
{
    const BYTE* pbBase = static_cast<const BYTE*>(pArrRaw);
    size_t cbStride = 0;
    int nElems = 0;

    if (nCount > 0)
    {
        // 权威计数路径：仍需确定步长（用于字段偏移），但计数不再靠哨兵。
        cbStride = DrvProbeStride(pbBase);
        if (!cbStride)
        {
            // 计数已知但步长未知：仍按 19041+ 主布局（100）兜底尝试，
            // 失败再退 52（SDK 公开布局）。
            cbStride = 100;
        }
        // 首元素 PublishedName 必须可读，否则视为坏数组
        unsigned long long ullFirst = 0;
        if (!DrvSafeRead(pbBase, &ullFirst, sizeof(ullFirst)) ||
            !DrvLooksLikeWStr(ullFirst))
        {
            cbStride = (cbStride == 100) ? 52 : 100;
            if (!DrvSafeRead(pbBase, &ullFirst, sizeof(ullFirst)) ||
                !DrvLooksLikeWStr(ullFirst))
            {
                if (pErr) *pErr = L"GetDrivers: driver record layout mismatch (count>0)";
                return false;
            }
        }
        nElems = static_cast<int>(nCount);
    }
    else
    {
        // 旧路径：无 count 时按哨兵探测（兼容 Count 出参为 0 的极端情况）
        cbStride = DrvProbeStride(pbBase);
        if (!cbStride)
        {
            if (pErr) *pErr = L"GetDrivers: unrecognized driver record layout";
            return false;
        }
        nElems = DrvWalkCount(pbBase, cbStride);
        if (nElems < 0)
        {
            if (pErr) *pErr = L"GetDrivers: record sentinel probe failed";
            return false;
        }
    }

    // 字段偏移：0/8/0x10 两版一致；其余按布局区分。
    const bool bV2 = (cbStride == 100);
    const size_t offClass = bV2 ? 0x1C : 0x14;
    const size_t offProvider = bV2 ? 0x3C : 0x1C;
    constexpr size_t offDateStr = 0x24; // v1 字符串日期
    constexpr size_t offVerStr = 0x2C; // v1 字符串版本
    constexpr size_t offDateSt = 0x44; // v2 SYSTEMTIME
    constexpr size_t offVerDw = 0x54; // v2 4×DWORD

    vOut.reserve(vOut.size() + static_cast<size_t>(nElems));
    for (int i = 0; i < nElems; ++i)
    {
        const BYTE* e = pbBase + static_cast<size_t>(i) * cbStride;
        DriverInfo di;
        di.bInBox = false;

        DrvDupField(e, 0x00, di.sPublished);
        DrvDupField(e, 0x08, di.sOriginal);
        DrvDupField(e, offClass, di.sClass);
        DrvDupField(e, offProvider, di.sProvider);

        int nInBox = 0;
        if (DrvSafeRead(e + 0x10, &nInBox, sizeof(nInBox)))
            di.bInBox = (nInBox != FALSE);

        if (bV2)
        {
            di.sDate    = DrvFormatStDate(
                reinterpret_cast<const SYSTEMTIME*>(e + offDateSt));
            di.sVersion = DrvFormatVersion(
                reinterpret_cast<const DWORD*>(e + offVerDw));
        }
        else
        {
            DrvDupField(e, offDateStr, di.sDate);
            DrvDupField(e, offVerStr, di.sVersion);
        }

        // PublishedName 是后续删除/匹配的主键，缺失的坏记录跳过
        if (!di.sPublished.empty())
            vOut.push_back(std::move(di));
    }
    return true;
}

}  // namespace driver_layout

// [P1-A6] 驱动枚举：DismGetDrivers（返回 PublishedName==nullptr 结尾数组，
// DismDelete 释放）。记录布局运行时探测，全部字段读取 SEH 防护。
bool GetDrivers(std::vector<DriverInfo>& vOut, std::wstring* pErr,
                const wchar_t* pszImageRoot, bool bAllDrivers)
{
    if (!Rt().pGetDrivers)
    { if (pErr) *pErr = L"dismapi: DismGetDrivers unavailable"; return false; }

    std::wstring sErr;
    DismSession sess = pszImageRoot && *pszImageRoot
        ? OpenImageSession(pszImageRoot, &sErr)
        : OpenOnlineSession(&sErr);
    if (!sess) { if (pErr) *pErr = sErr; return false; }

    void* pArrRaw = nullptr;
    UINT nCount = 0;
    HRESULT hr = Rt().pGetDrivers((DismSession)sess, bAllDrivers ? TRUE : FALSE,
                                  &pArrRaw, &nCount);
    Rt().hrLast = hr;

    bool bParsed = false;
    if (SUCCEEDED(hr) && pArrRaw)
    {
        // [N3] 布局探测与字段解析全部下沉 driver_layout::ParseDriverArray
        bParsed = driver_layout::ParseDriverArray(pArrRaw, nCount, vOut, pErr);
        // Rt().pDelete 判空加固（Shutdown 后函数指针可能已清空）
        if (Rt().pDelete) Rt().pDelete(pArrRaw);
    }
    CloseSession(static_cast<UINT>(sess));

    if (FAILED(hr))
    {
        if (pErr)
        {
            std::wstring e = GetLastErrorText();
            *pErr = e.empty() ? L"GetDrivers failed" : e;
        }
        return false;
    }
    // 成功但空数组（nCount==0）属合法空态，返回 true
    if (SUCCEEDED(hr) && !pArrRaw) return true;
    if (!bParsed) return false;
    return true;
}

bool RemovePackage(const std::wstring& sName, std::wstring* pErr)
{
    if (!Rt().pRemovePackage) { if (pErr) *pErr = L"dismapi: DismRemovePackage unavailable"; return false; }
    if (sName.empty()) { if (pErr) *pErr = L"RemovePackage: empty package name"; return false; }

    // 包移除仅在线映像（对外签名无 imageRoot）
    return RunWithSession(nullptr, pErr, L"RemovePackage",
        [&](DismSession sess) -> HRESULT
        {
            return Rt().pRemovePackage(sess, sName.c_str(), DismPackageIdentifier::DismPackageName,
                                       nullptr, nullptr, nullptr);
        });
}

// [P1-收尾] 添加 cab/msu 包：DismAddPackage（等价 dism /Add-Package /PackagePath /NoRestart）。
// API 本身不会重启系统；PreventPending=FALSE 与 dism /Add-Package 默认行为一致。
// ERROR_SUCCESS_REBOOT_REQUIRED(3010) severity=0，FAILED 判定天然为成功。
bool AddPackage(const std::wstring& sPath, std::wstring* pErr,
                const wchar_t* pszImageRoot)
{
    if (!Rt().pAddPackage) { if (pErr) *pErr = L"dismapi: DismAddPackage unavailable"; return false; }
    if (sPath.empty()) { if (pErr) *pErr = L"AddPackage: empty package path"; return false; }

    return RunWithSession(pszImageRoot, pErr, L"AddPackage",
        [&](DismSession sess) -> HRESULT
        {
            return Rt().pAddPackage(sess, sPath.c_str(),
                                    FALSE, FALSE,   // IgnoreCheck=FALSE, PreventPending=FALSE
                                    DismLogLevel::DismLogErrorsWarningsInfo,
                                    nullptr, nullptr, nullptr, nullptr);
        });
}

// [P0-A1] 移除预配 Appx：DismRemoveProvisionedAppxPackage(Session, PackageFamilyName)
bool RemoveProvisionedAppx(const std::wstring& sFamilyName, std::wstring* pErr,
                           const wchar_t* pszImageRoot)
{
    if (!Rt().pRemoveProvAppx)
    {
        if (pErr) *pErr = L"dismapi: DismRemoveProvisionedAppxPackage unavailable";
        return false;
    }
    if (sFamilyName.empty())
    {
        if (pErr) *pErr = L"RemoveProvisionedAppx: empty family name";
        return false;
    }

    std::wstring sErr;
    DismSession sess = OpenImageSession(pszImageRoot, &sErr);
    if (!sess) { if (pErr) *pErr = sErr; return false; }

    HRESULT hr = Rt().pRemoveProvAppx((DismSession)sess, sFamilyName.c_str());
    Rt().hrLast = hr;
    CloseSession(static_cast<UINT>(sess));

    if (FAILED(hr))
    {
        if (pErr)
        {
            constexpr int kErrMsgWchars = 256;   // 256 缓冲尺寸（语义命名，数值不变）
            wchar_t szBuf[kErrMsgWchars] = { 0 };
            wfmt::FormatTo(szBuf, _countof(szBuf), L"DismRemoveProvisionedAppxPackage(0x%08X)", (unsigned)hr);
            std::wstring e = GetLastErrorText();
            *pErr = e.empty() ? std::wstring(szBuf) : (std::wstring(szBuf) + L" " + e);
        }
        return false;
    }
    return true;
}

// ----------------------------------------------------------------------------
// 健康检查 / 修复 / 包查询（[2026-10-08] 对应蓝图 3 个范围分支）
// ----------------------------------------------------------------------------

bool CheckImageHealth(bool bScan, DWORD* pdwStatus, std::wstring* pErr,
                      const wchar_t* pszImageRoot)
{
    if (!Rt().pCheckImageHealth)
    { if (pErr) *pErr = L"dismapi: DismCheckImageHealth unavailable"; return false; }

    // [BUG-09] 透传映像根：离线健康检查必须打开离线会话，否则会静默
    // 扫描在线系统，返回的状态对目标映像无意义。
    std::wstring sErr;
    DismSession sess = pszImageRoot
        ? OpenImageSession(pszImageRoot, &sErr)
        : OpenOnlineSession(&sErr);
    if (!sess) { if (pErr) *pErr = sErr; return false; }

    DWORD dwStatus = DWORD(-1);
    HRESULT hr = Rt().pCheckImageHealth((DismSession)sess, bScan ? TRUE : FALSE,
                                     nullptr, nullptr, nullptr, &dwStatus);
    Rt().hrLast = hr;
    CloseSession(static_cast<UINT>(sess));
    if (pdwStatus) *pdwStatus = dwStatus;

    if (FAILED(hr))
    {
        if (pErr)
        {
            std::wstring e = GetLastErrorText();
            wchar_t szHr[64] = { 0 };
            wfmt::FormatInto(szHr, _countof(szHr),
                         L"CheckImageHealth failed hr=0x%08lX", static_cast<DWORD>(hr));
            *pErr = e.empty() ? std::wstring(szHr)
                              : std::wstring(szHr) + L" " + e;
        }
        return false;
    }
    return true;
}

bool RestoreImageHealth(const std::wstring& sSource, bool bLimitAccess,
                        std::wstring* pErr, const wchar_t* pszImageRoot)
{
    if (!Rt().pRestoreImageHealth)
    { if (pErr) *pErr = L"dismapi: DismRestoreImageHealth unavailable"; return false; }

    PCWSTR pPaths[1] = { sSource.c_str() };
    return RunWithSession(pszImageRoot, pErr, L"RestoreImageHealth",
        [&](DismSession sess) -> HRESULT
        {
            return Rt().pRestoreImageHealth(sess,
                                            sSource.empty() ? nullptr : pPaths,
                                            sSource.empty() ? 0u : 1u,
                                            bLimitAccess ? TRUE : FALSE,
                                            nullptr, nullptr, nullptr);
        });
}

// [T5 探针] 共享在线枚举会话的获取耗时计时包装（默认完全关闭）。
//
// 用途：判定计划书 T5 的触发条件——「功能页与可选功能页同时进页时，第二页
// 排队是否超 3s」。只有拿到实测数据，才能决定双会话池该不该做。
//
// 开关：环境变量 DISMTOOL_ENUM_LOCK_TRACE 存在即启用；未设置时
// EnumTraceEnabled() 的静态初值一次性算完，之后每次只读一个 bool，
// 主路径成本为一次分支判断，不产生任何输出。
//
// 测量口径（务必按此解读样本，否则会误判）：
//   量的是「进 AcquireEnumSession 到拿到会话」的整段耗时，其中包含
//   ① 抢共享互斥量的等待；② 若是首调，还包含 OpenOnlineSession 的冷启动。
//   两者在首次调用时不可分离，因此：
//     * 首次进页样本 = 冷启动 + 排队，数值偏大属正常，不能据此判 T5；
//     * 会话已建立后的样本 = 纯排队（此时不再开会话），这才是 T5 的判据。
//   判定方法：连续/并发进页，看第二个及以后的样本 ≥3000ms 才算触发。
static bool EnumTraceEnabled()
{
    static const bool s_bTrace = []() -> bool
    {
        // 仅做长度探测：变量存在即视为启用（含空值），无需缓冲。
        return ::GetEnvironmentVariableW(L"DISMTOOL_ENUM_LOCK_TRACE", nullptr, 0) > 0;
    }();
    return s_bTrace;
}

// ============================================================
// [A1 2026-10-25] 统一性能埋点。
//
// 背景：阶段 A/B 的收益全部建立在"排队 2~15s""二次重建 30~120ms"这类区间上，
// 而那些数字来自源码推断（01 报告 D5 级，明确标注"工程估算，需埋点确认"）。
// 没有实测就没法验证优化是否真的生效，也没法证伪。
//
// 开关：环境变量 DISMTOOL_PERF_TRACE 存在即启用。未设置时是一次静态 bool 读取，
// 主路径零额外开销——埋点绝不能成为新的性能负担。
//
// 输出：OutputDebugString，格式 `[perf] <节点> = <毫秒> ms<附加>`，便于用
// DebugView 抓取后直接排序统计 P50/P95。
// ============================================================
bool PerfTraceEnabled()
{
    static const bool s_bTrace = []() -> bool
    {
        return ::GetEnvironmentVariableW(L"DISMTOOL_PERF_TRACE", nullptr, 0) > 0;
    }();
    return s_bTrace;
}

PerfScope::PerfScope(const wchar_t* pszNode)
    : m_pszNode(pszNode), m_llBeg(0)
{
    LARGE_INTEGER li;
    ::QueryPerformanceCounter(&li);
    m_llBeg = li.QuadPart;
}

PerfScope::~PerfScope()
{
    LARGE_INTEGER liEnd, liFreq;
    ::QueryPerformanceCounter(&liEnd);
    ::QueryPerformanceFrequency(&liFreq);
    const long long llTicks = (liEnd.QuadPart - m_llBeg) * 1000;
    const long long llMs = (liFreq.QuadPart > 0) ? (llTicks / liFreq.QuadPart) : 0;
    std::wstring sMsg(128, L'\0');
    wfmt::FormatInto(&sMsg[0], sMsg.size(), L"[perf] %s = %lld ms\r\n",
                     m_pszNode, llMs);
    sMsg.resize(wcslen(sMsg.c_str()));
    ::OutputDebugStringW(sMsg.c_str());
    // [R25 2026-10-25] 同时落盘：埋点默认只走 OutputDebugString，需要人工开
    //   DebugView 抓取，无法自动化采集。改为同时追加写入 dist/perf_<pid>.log，
    //   统计脚本可直接读取该文件，不必再依赖 GUI 工具。
    //   失败静默：埋点是诊断功能，不能因为它写不了文件而影响主流程。
    {
        wchar_t szDir[MAX_PATH] = { 0 };
        if (::GetModuleFileNameW(nullptr, szDir, MAX_PATH))
        {
            // 去掉文件名，定位到 exe 所在目录
            wchar_t* pSlash = wcsrchr(szDir, L'\\');
            if (pSlash) *pSlash = L'\0';
            std::wstring sPath(szDir);
            sPath += L"\\perf_";
            wchar_t szPid[16] = { 0 };
            ::_snwprintf_s(szPid, _countof(szPid), _TRUNCATE, L"%lu",
                           static_cast<unsigned long>(::GetCurrentProcessId()));
            sPath += szPid;
            sPath += L".log";
            HANDLE h = ::CreateFileW(sPath.c_str(), FILE_APPEND_DATA,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE)
            {
                const int nBytes = ::WideCharToMultiByte(CP_UTF8, 0, sMsg.c_str(), -1,
                                                        nullptr, 0, nullptr, nullptr);
                if (nBytes > 1)
                {
                    std::string sUtf8(static_cast<size_t>(nBytes - 1), '\0');
                    ::WideCharToMultiByte(CP_UTF8, 0, sMsg.c_str(), -1,
                                          &sUtf8[0], nBytes, nullptr, nullptr);
                    DWORD dwWritten = 0;
                    ::WriteFile(h, sUtf8.data(), static_cast<DWORD>(sUtf8.size()),
                                &dwWritten, nullptr);
                }
                ::CloseHandle(h);
            }
        }
    }
}

static bool TimedAcquireEnumSession(EEnumDomain eDomain, const wchar_t* pszImageRoot,
                                    std::wstring* pErr,
                                    std::unique_lock<std::mutex>& lk,
                                    DismSession& sess, bool& bShared)
{
    if (!EnumTraceEnabled())
        return AcquireEnumSession(eDomain, pszImageRoot, pErr, lk, sess, bShared);

    LARGE_INTEGER liFreq;
    LARGE_INTEGER liBeg;
    LARGE_INTEGER liEnd;
    ::QueryPerformanceFrequency(&liFreq);
    ::QueryPerformanceCounter(&liBeg);
    const bool bOk = AcquireEnumSession(eDomain, pszImageRoot, pErr, lk, sess, bShared);
    ::QueryPerformanceCounter(&liEnd);

    // 转毫秒（整数运算，避免浮点副作用）
    const long long llTicks = (liEnd.QuadPart - liBeg.QuadPart) * 1000;
    const long long llMs = (liFreq.QuadPart > 0) ? (llTicks / liFreq.QuadPart) : 0;

    // 超阈值才输出：3s 即 T5 的触发判据；同时输出所有样本便于看分布
    if (llMs >= 3000)
        ::OutputDebugStringW(L"[T5] enum lock wait OVER 3000ms (T5 trigger hit)\r\n");

    // 用项目自有 FormatInto + std::wstring（避免 wsprintf 族与定长缓冲，
    // 两者分别在 F8_C_format_str / F13_fixed_charbuf 禁列内）。
    std::wstring sMsg(96, L'\0');
    wfmt::FormatInto(&sMsg[0], sMsg.size(), L"[T5] enum lock wait = %lld ms%s%s\r\n",
                     llMs,
                     bOk ? L"" : L" (acquire failed)",
                     bShared ? L" [shared]" : L" [offline]");
    sMsg.resize(wcslen(sMsg.c_str()));
    ::OutputDebugStringW(sMsg.c_str());
    return bOk;
}

bool GetPackages(std::vector<PkgInfo>& vOut, std::wstring* pErr,
                 const wchar_t* pszImageRoot)
{
    vOut.clear();
    if (!Rt().pGetPackages)
    { if (pErr) *pErr = L"dismapi: DismGetPackages unavailable"; return false; }

    // [T3 P1] 复用共享在线枚举会话（与 GetFeatures/GetCapabilities 同一把锁同一会话）：
    // 三页枚举不再各自 Open/Close 会话，省掉每次 1 次会话建立 + 日志挂载开销。
    std::unique_lock<std::mutex> lk;
    DismSession sess = 0;
    bool bShared = false;
    // [A1] 会话获取计时：包含抢锁等待 + 首次冷启动，是"排队"的直接度量。
    {
        const std::unique_ptr<PerfScope> pScope(
            PerfTraceEnabled() ? new PerfScope(L"acquire.package") : nullptr);
        if (!TimedAcquireEnumSession(EEnumDomain::Package, pszImageRoot, pErr, lk, sess, bShared)) return false;
    }

    DismPackage* pPkgs = nullptr;
    UINT nCount = 0;
    HRESULT hr = Rt().pGetPackages((DismSession)sess, &pPkgs, &nCount);
    Rt().hrLast = hr;

    if (SUCCEEDED(hr) && pPkgs)
    {
        for (UINT i = 0; i < nCount; ++i)
        {
            PkgInfo info;
            if (pPkgs[i].PackageName) info.sName = pPkgs[i].PackageName;
            info.nState = pPkgs[i].PackageState;
            info.nReleaseType = pPkgs[i].ReleaseType;
            vOut.push_back(info);
        }
    }

    if (pPkgs && Rt().pDelete) Rt().pDelete(pPkgs);
    ReleaseEnumSession(EEnumDomain::Package, sess, bShared, hr);

    if (FAILED(hr))
    {
        if (pErr)
        {
            std::wstring e = GetLastErrorText();
            *pErr = e.empty() ? L"GetPackages failed" : e;
        }
        return false;
    }
    return true;
}

// ----------------------------------------------------------------------------
// 功能 / 能力结构化枚举（[P0-A3 2026-10-11] 替代 bat+dism 文本解析）
// ----------------------------------------------------------------------------

static bool GetFeaturesCore(std::vector<FeatureInfo>& vOut, std::wstring* pErr,
                            const wchar_t* pszImageRoot)
{
    vOut.clear();
    if (!Rt().pGetFeatures)
    { if (pErr) *pErr = L"dismapi: DismGetFeatures unavailable"; return false; }

    // [G2] 共享/离线会话获取样板折叠（在线持共享锁复用会话）
    std::unique_lock<std::mutex> lk;
    DismSession sess = 0;
    bool bShared = false;
    // [A1] 会话获取计时：包含抢锁等待 + 首次冷启动，是"排队"的直接度量。
    {
        const std::unique_ptr<PerfScope> pScope(
            PerfTraceEnabled() ? new PerfScope(L"acquire.feature") : nullptr);
        if (!TimedAcquireEnumSession(EEnumDomain::Feature, pszImageRoot, pErr, lk, sess, bShared)) return false;
    }

    DismFeature* pFeats = nullptr;
    UINT nCount = 0;
    // Identifier=nullptr + DismPackageNone → 枚举映像全部功能（等价 /Get-Features）
    // [rb23] 实测参数顺序：数组指针在前、Count 在后
    HRESULT hr = Rt().pGetFeatures((DismSession)sess, nullptr, DismPackageIdentifier::DismPackageNone,
                                &pFeats, &nCount);
    Rt().hrLast = hr;

    // [P2] 会话锁分段（与 GetCapabilities 同一策略）：
    //   锁内：DismGetFeatures → 拷出原始数据 → DismDelete → 会话收尾
    //   锁外：原始数据转成对外结构体 FeatureInfo
    // 会话的关闭/作废仍留在锁内，避免与复用同一会话的线程冲突。
    struct RawFeat { std::wstring sName; DWORD nState; };
    std::vector<RawFeat> vRaw;
    if (SUCCEEDED(hr) && pFeats)
    {
        vRaw.reserve(nCount);
        for (UINT i = 0; i < nCount; ++i)
        {
            RawFeat r;
            if (pFeats[i].FeatureName) r.sName = pFeats[i].FeatureName;
            r.nState = pFeats[i].State;
            vRaw.push_back(std::move(r));
        }
    }
    // DismDelete 只释放本指针，不触碰会话（原代码顺序即如此）
    if (pFeats && Rt().pDelete) Rt().pDelete(pFeats);

    // 会话收尾（可能写共享句柄，必须在锁内）
    ReleaseEnumSession(EEnumDomain::Feature, sess, bShared, hr);

    // 释放锁，锁外做纯 CPU 转换
    lk.unlock();

    vOut.reserve(vRaw.size());
    for (size_t i = 0; i < vRaw.size(); ++i)
    {
        FeatureInfo info;
        info.sName = vRaw[i].sName.c_str();
        info.nState = vRaw[i].nState;
        // [rb23] 实测 4=Enabled/Installed；5/6 兼容文档值与其它版本
        info.bEnabled = (vRaw[i].nState == DISM_STATE_INSTALLED
                      || vRaw[i].nState == DISM_STATE_INSTALLED_DOC
                      || vRaw[i].nState == DISM_STATE_PERMANENT);
        vOut.push_back(info);
    }

    if (FAILED(hr))
    {
        if (pErr)
        {
            std::wstring e = GetLastErrorText();
            *pErr = e.empty() ? L"GetFeatures failed" : e;
        }
        return false;
    }
    return true;
}

static bool GetCapabilitiesCore(std::vector<CapabilityInfo>& vOut, std::wstring* pErr,
                                const wchar_t* pszImageRoot)
{
    vOut.clear();
    if (!Rt().pGetCapabilities)
    { if (pErr) *pErr = L"dismapi: DismGetCapabilities unavailable"; return false; }

    // [G2] 共享/离线会话获取样板折叠（在线持共享锁复用会话）
    std::unique_lock<std::mutex> lk;
    DismSession sess = 0;
    bool bShared = false;
    // [A1] 会话获取计时：包含抢锁等待 + 首次冷启动，是"排队"的直接度量。
    {
        const std::unique_ptr<PerfScope> pScope(
            PerfTraceEnabled() ? new PerfScope(L"acquire.capability") : nullptr);
        if (!TimedAcquireEnumSession(EEnumDomain::Capability, pszImageRoot, pErr, lk, sess, bShared)) return false;
    }

    DismCapabilityInfo* pCaps = nullptr;
    UINT nCount = 0;
    // [rb23] 实测参数顺序：数组指针在前、Count 在后
    HRESULT hr = Rt().pGetCapabilities((DismSession)sess, &pCaps, &nCount);
    Rt().hrLast = hr;

    // [P2] 会话锁分段：数据转换移到锁外。
    //
    // 能切到哪一步（这是本方案的关键约束）：
    //   DismSession 非线程安全，DismGetCapabilities 必须在锁内完成——锁无法跨过它。
    //   但紧随其后的"把 DismCapabilityInfo[] 拷成 CapabilityInfo[]"是纯 CPU 的
    //   本地内存操作，不触达会话、不依赖锁保护的状态，可以放到锁外执行，
    //   从而让其他等待者在这段时间里提前进场。
    //
    // 不能切的部分：
    //   会话的"作废/关闭"必须留在锁内。ReleaseEnumSession 在失败时会
    //   CloseSession 并把 g_sessSharedOnline 置 0 —— 这是对共享句柄的写操作。
    //   若放到锁外，可能与另一个刚取到锁、正在复用同一会话的线程冲突
    //   （对方手里的句柄被本线程关掉 → 对方调用返回无效句柄错误）。
    //
    // 因此顺序调整为：
    //   锁内：枚举 → 释放 Dism 数组（DismDelete 只释放该指针，不触碰会话，
    //         这一点由原代码顺序保证——它原本就在 ReleaseEnumSession 之前）
    //         → 作废/关闭会话 → 释放锁
    //   锁外：把已拷出的中间数据转换成对外结构体
    //
    // 代价说明：转换循环采用的是"先拷到本地临时 vector，锁外再转换"，
    // 因此多一次 CapabilityInfo 的拷贝。但相比让其他枚举线程排队数秒，
    // 这点拷贝可忽略。
    struct RawCap { std::wstring sName; DWORD nState; };
    std::vector<RawCap> vRaw;
    if (SUCCEEDED(hr) && pCaps)
    {
        vRaw.reserve(nCount);
        for (UINT i = 0; i < nCount; ++i)
        {
            RawCap r;
            if (pCaps[i].Name) r.sName = pCaps[i].Name;
            r.nState = pCaps[i].State;
            vRaw.push_back(std::move(r));
        }
    }
    if (pCaps && Rt().pDelete) Rt().pDelete(pCaps);

    // 会话收尾（仍在锁内：可能写共享句柄）
    ReleaseEnumSession(EEnumDomain::Capability, sess, bShared, hr);

    // 锁随 lk 析构释放；下面开始锁外转换
    lk.unlock();

    vOut.reserve(vRaw.size());
    for (size_t i = 0; i < vRaw.size(); ++i)
    {
        CapabilityInfo info;
        info.sName = vRaw[i].sName.c_str();
        info.nState = vRaw[i].nState;
        // [rb23] 实测 4=Installed；5/6 兼容文档值与其它版本
        info.bInstalled = (vRaw[i].nState == DISM_STATE_INSTALLED
                        || vRaw[i].nState == DISM_STATE_INSTALLED_DOC
                        || vRaw[i].nState == DISM_STATE_PERMANENT);
        vOut.push_back(info);
    }

    if (FAILED(hr))
    {
        if (pErr)
        {
            std::wstring e = GetLastErrorText();
            *pErr = e.empty() ? L"GetCapabilities failed" : e;
        }
        return false;
    }
    return true;
}


// [FIX-PERF] single-flight 包装：同类枚举进行中时，后到请求复用同一结果。
// [R4-FIX] 超时接管竞态修复：
//   v1：wait_for 超时后直接抢跑 f.running=true，极端情况（A 还在跑 + B 超时）
//       导致 2 个并发 DISM 会话。修复：超时后在锁内重判 f.running，若 A 已完成
//       则复用；若仍在跑则再短等 1s，避免"3s 刚过就抢跑"的并发窗口。
bool GetFeatures(std::vector<FeatureInfo>& vOut, std::wstring* pErr,
                 const wchar_t* pszImageRoot)
{
    if (pszImageRoot && *pszImageRoot)
        return GetFeaturesCore(vOut, pErr, pszImageRoot);   // 离线：每镜像独立

    EnumFlight& f = FeatFlight();
    std::unique_lock<std::mutex> lk(f.mtx);
    if (f.running)
    {
        // 已有同类枚举在跑：优先复用它，但等待封顶（见 kFlightWaitCap）。
        if (f.cv.wait_for(lk, kFlightWaitCap, [&f]{ return !f.running; }))
        {
            vOut = f.feat;
            if (pErr) *pErr = f.err;
            return f.ok;
        }
        // [R4-FIX] 等待超时：重判 f.running —— 若 A 线程已完成（极端情况下
        // wait_for 返回 false 但 running 刚被置 false），直接在锁内复用。
        // 若仍在跑，再等 1s 短窗口，只当 A 真的跑得很慢（预热模式）才抢跑。
        ::OutputDebugStringW(L"[flight] feature wait capped at 3s, rechecking...\n");
        if (f.cv.wait_for(lk, std::chrono::seconds(1), [&f]{ return !f.running; }))
        {
            vOut = f.feat;
            if (pErr) *pErr = f.err;
            return f.ok;
        }
        // 彻底超时：记录并自行发起
        ::OutputDebugStringW(L"[flight] feature wait capped at 4s, taking over\n");
    }
    f.running = true;
    f.ok = false;
    f.err.clear();
    lk.unlock();

    std::vector<FeatureInfo> vLocal;
    std::wstring sErr;
    const bool bOk = GetFeaturesCore(vLocal, &sErr, pszImageRoot);

    lk.lock();
    f.feat.swap(vLocal);
    f.ok = bOk;
    f.err = sErr;
    f.running = false;
    // [R227] Copy the result OUT while still holding the lock. Reading f.feat after
    //   unlock() raced with the next round: the waiter that just woke could clear
    //   `running` and call swap() on the same vector, reallocating it under the copy.
    //   std::vector copy is not atomic, so this was a genuine data race, not just a
    //   theoretical one. The copy is done here; the lock is released right after.
    vOut = f.feat;
    lk.unlock();
    f.cv.notify_all();

    if (pErr) *pErr = sErr;
    return bOk;
}

bool GetCapabilities(std::vector<CapabilityInfo>& vOut, std::wstring* pErr,
                     const wchar_t* pszImageRoot)
{
    if (pszImageRoot && *pszImageRoot)
        return GetCapabilitiesCore(vOut, pErr, pszImageRoot);

    EnumFlight& f = CapFlight();
    std::unique_lock<std::mutex> lk(f.mtx);
    if (f.running)
    {
        // [R4-FIX] 同 GetFeatures：优先等待 3s，超时再短等 1s 才抢跑
        if (f.cv.wait_for(lk, kFlightWaitCap, [&f]{ return !f.running; }))
        {
            vOut = f.cap;
            if (pErr) *pErr = f.err;
            return f.ok;
        }
        if (f.cv.wait_for(lk, std::chrono::seconds(1), [&f]{ return !f.running; }))
        {
            vOut = f.cap;
            if (pErr) *pErr = f.err;
            return f.ok;
        }
        ::OutputDebugStringW(L"[flight] capability wait capped at 4s, taking over\n");
    }
    f.running = true;
    f.ok = false;
    f.err.clear();
    lk.unlock();

    std::vector<CapabilityInfo> vLocal;
    std::wstring sErr;
    const bool bOk = GetCapabilitiesCore(vLocal, &sErr, pszImageRoot);

    lk.lock();
    f.cap.swap(vLocal);
    f.ok = bOk;
    f.err = sErr;
    f.running = false;
    // [R227] Same race as GetFeatures above: copy under the lock.
    vOut = f.cap;
    lk.unlock();
    f.cv.notify_all();


    if (pErr) *pErr = sErr;
    return bOk;
}
} // namespace DismApiBridge