// ============================================================================
// WIMHelper.h - wimgapi.dll CBSHost 风格手工 IAT 直连封装
// ============================================================================
// Purpose : 对微软官方 wimgapi.dll 的 C++ 封装。
// Route   : [R-B87] 零导入直连——LoadLibraryExW + 逐名 GetProcAddress 填入
//           g_wimApi 函数指针表（复刻 Dism++x64.exe 的手工名字表绑定，
//           证据：主程序 ASCII 池内连续 31 个 wimgapi 导出名）；既不静态链接
//           wimgapi.lib 也不使用 delayimp，产物 PE 导入表/延迟导入表均无
//           wimgapi.dll，DLL 缺失或损坏时优雅 false，由上层回落 dism。
// Coverage: 73 槽 = 随包新版 DLL 全部 73 个 WIM 导出（DLL 共 75 导出：
//           73 WIM* + DllMain/DllCanUnloadNow）。
// License : wimgapi.dll itself is MIT-licensed Microsoft open source.
// ============================================================================

#ifndef __DUI_WIMHELPER_H__
#define __DUI_WIMHELPER_H__

// Enable access to all wimgapi.h exports regardless of NTDDI_VERSION.
#ifndef WIMGAPI_LATEST
#define WIMGAPI_LATEST
#endif

#include <windows.h>
#include "FormatW.h"
#include "WinHandles.h"   // [C++17 R11] win::HandlePtr for local probe handles
#include <wimgapi.h>
#include <vector>
#include <string>
#include <functional>

// --- [R-B87] CBSHost 风格手工导入表（wimgapi.dll 零导入绑定）---------------
// 逆向证据（Dism++10.1.1002.1B，_证据与原始数据）：
//   * CBSHost.dll 导入表 228 个函数中没有任何 wimgapi 项；主程序
//     Dism++x64.exe 的 ASCII 字符串池里连续排列 31 个 wimgapi 导出名
//     （WIMRemountImage…WIMFindNextImageFile，与 GDI+/CBSHost 的解析名同表），
//     这是"名字表 + 一次性 GetProcAddress"手工 IAT 的典型特征——Dism++ 对
//     wimgapi.dll 既无静态导入也无延迟导入，运行期按 Config\amd64 架构目录取
//     DLL 后逐名解析到函数指针槽；
//   * 随包新版 wimgapi.dll 共 75 导出（73 个 WIM* + DllMain/DllCanUnloadNow）。
// 本块完整复刻该模型：
//   1) g_wimApi 指针表覆盖随包新版 DLL 全部 73 个 WIM 导出（共 73 槽）；
//   2) EnsureWIMGAPILoaded() 仅首次调用时 LoadLibraryExW（exe 目录优先 +
//      PE 架构校验），随后逐名 GetProcAddress：9 个核心导出缺一即整体失败
//      （优雅返回 false，上层回落 dism），其余按槽可选，缺失只影响单项功能；
//   3) 头文件末尾用同名宏把既有 WIMXxx(...) 调用点透明重定向到指针表，
//      vcxproj 不再链接 wimgapi.lib / delayimp.lib —— 产物 PE 的普通导入表
//      与延迟导入表中均无 wimgapi.dll，缺失/坏损 DLL 绝不触发进程启动期
//      0xC000007B，与 Dism++ 主程序行为完全一致。
#pragma once

// --- ADK 扩展导出的手工签名（随包 SDK wimgapi.h 未声明这些原型）-----------
// 签名来源：wimgapi 官方 ADK 头 + 本工程既有调用约定（R-B86 前已逐函数使用）。
using PFN_WIMAddImagePath = BOOL ( WINAPI*)(HANDLE, PCWSTR, DWORD);
using PFN_WIMAddImagePaths = BOOL ( WINAPI*)(HANDLE, PCWSTR*, DWORD, DWORD);
using PFN_WIMAddWimbootEntry = BOOL ( WINAPI*)(PCWSTR, PCWSTR);
using PFN_WIMApplyInstance = HANDLE ( WINAPI*)(HANDLE, PCWSTR, DWORD);
using PFN_WIMCaptureInstance = HANDLE ( WINAPI*)(HANDLE, PCWSTR, DWORD);
using PFN_WIMCreateFileEx = HANDLE ( WINAPI*)(PCWSTR, DWORD, DWORD, DWORD,
                                            DWORD, WORD, unsigned long*);
using PFN_WIMCreateWofCompressedFile = HANDLE ( WINAPI*)(PCWSTR, DWORD, DWORD);
using PFN_WIMExtractImageDirectory = BOOL ( WINAPI*)(HANDLE, PCWSTR, PCWSTR, DWORD);
using PFN_WIMExtractImagePathByWimHandle = BOOL ( WINAPI*)(HANDLE, PCWSTR, PCWSTR, DWORD, DWORD);
using PFN_WIMExtractInstancePath = BOOL ( WINAPI*)(HANDLE, PCWSTR, PCWSTR, DWORD);
using PFN_WIMFindFirstInstanceFile = HANDLE ( WINAPI*)(HANDLE, PCWSTR, PWIM_FIND_DATA);
using PFN_WIMFindNextInstanceFile = BOOL ( WINAPI*)(HANDLE, PWIM_FIND_DATA);
using PFN_WIMGetInstanceCount = DWORD ( WINAPI*)(HANDLE);
using PFN_WIMGetInstanceInformation = BOOL ( WINAPI*)(HANDLE, void**, unsigned long*);
using PFN_WIMSetInstanceInformation = BOOL ( WINAPI*)(HANDLE, void*, DWORD);
using PFN_WIMGetWIMBootEntries = BOOL ( WINAPI*)(unsigned long*, void*, DWORD);
using PFN_WIMGetWIMBootWIMPath = BOOL ( WINAPI*)(unsigned long*, void*, DWORD);
using PFN_WIMGetWimFileSize = BOOL ( WINAPI*)(HANDLE, PULONGLONG);
using PFN_WIMInitializeWofDriver = BOOL ( WINAPI*)(void);
using PFN_WIMIsCurrentSystemWimboot = BOOL ( WINAPI*)(PBOOL);
using PFN_WIMIsReferenceWim = BOOL ( WINAPI*)(PCWSTR, PBOOL);
using PFN_WIMLoadInstance = HANDLE ( WINAPI*)(HANDLE, DWORD);
using PFN_WIMLoadOSInformation = BOOL ( WINAPI*)(HANDLE, void*, DWORD, unsigned long*);
using PFN_WIMProcessCustomImage = BOOL ( WINAPI*)(HANDLE, void*, DWORD);
using PFN_WIMReadFileEx = BOOL ( WINAPI*)(HANDLE, PCWSTR, void*, DWORD,
                                          LARGE_INTEGER, unsigned long*, DWORD);
using PFN_WIMReadInstanceFile = BOOL ( WINAPI*)(HANDLE, PCWSTR, void*, DWORD, unsigned long*);
using PFN_WIMRedirectFolderBeforeApply = BOOL ( WINAPI*)(HANDLE, PCWSTR, PCWSTR);
// 实测函数体：打开 pszFileName（dwFlags!=0 时请求写访问），可选 pszBasePath
// 在 \Program Files\WindowsApps\ 场景参与路径判定，pszAltName 经
// RtlDosPathNameToNtPathName_U 转 NT 路径；底层最终调用
// ntdll!NtSetCachedSigningLevel2 / NtSetCachedSigningLevel。
using PFN_WIMSetCachedSigningLevel = BOOL ( WINAPI*)(PCWSTR, PCWSTR, DWORD, PCWSTR);
using PFN_WIMSetImageUserSpecifiedCreationTime = BOOL ( WINAPI*)(HANDLE, LPFILETIME);
using PFN_WIMSetWimGuid = BOOL ( WINAPI*)(HANDLE, const GUID*);
using PFN_WIMSingleInstanceFile = BOOL ( WINAPI*)(HANDLE, PCWSTR, void*, DWORD);
using PFN_WIMUpdateWIMBootEntry = BOOL ( WINAPI*)(PCWSTR, PCWSTR, DWORD);
using PFN_WIMWriteFileWithIntegrity = BOOL ( WINAPI*)(HANDLE, PCWSTR,
                                                      const BYTE*, DWORD, DWORD);

// 9 个核心导出：任一缺失即判定 wimgapi 整体不可用（读/写映像主链路）。
#define WIM_REQUIRED_LIST(X) \
    X(WIMCreateFile)        X(WIMCloseHandle)         X(WIMLoadImage) \
    X(WIMGetImageCount)     X(WIMGetImageInformation) X(WIMSetImageInformation) \
    X(WIMApplyImage)        X(WIMCaptureImage)        X(WIMExportImage)

// wimgapi.h 已声明原型的其余 31 个导出（成员类型由 decltype 精确推导）。
// 注意：WIMMessageCallback 是回调方实现的原型，并非 wimgapi.dll 导出，不入表。
#define WIM_SIG_LIST(X) \
    X(WIMSetTemporaryPath)       X(WIMSetReferenceFile)         X(WIMSplitFile) \
    X(WIMDeleteImage)            X(WIMGetAttributes)            X(WIMSetBootImage) \
    X(WIMGetMessageCallbackCount) X(WIMRegisterMessageCallback) X(WIMUnregisterMessageCallback) \
    X(WIMCopyFile)               X(WIMMountImage)               X(WIMUnmountImage) \
    X(WIMGetMountedImages)       X(WIMInitFileIOCallbacks)      X(WIMSetFileIOCallbackTemporaryPath) \
    X(WIMMountImageHandle)       X(WIMRemountImage)             X(WIMCommitImageHandle) \
    X(WIMUnmountImageHandle)     X(WIMGetMountedImageInfo) \
    X(WIMGetMountedImageInfoFromHandle) X(WIMGetMountedImageHandle) \
    X(WIMDeleteImageMounts)      X(WIMRegisterLogFile)          X(WIMUnregisterLogFile) \
    X(WIMExtractImagePath)       X(WIMFindFirstImageFile)       X(WIMFindNextImageFile) \
    X(WIMEnumImageFiles)         X(WIMCreateImageFile)          X(WIMReadImageFile)

// 随包 DLL 存在、但本 SDK 头未声明原型的 18 个导出（手工 PFN）。
#define WIM_EXTRA_BUNDLED_LIST(X) \
    X(WIMAddImagePath)                     X(WIMAddImagePaths) \
    X(WIMAddWimbootEntry)                  X(WIMCreateWofCompressedFile) \
    X(WIMExtractImageDirectory)            X(WIMGetWIMBootEntries) \
    X(WIMGetWIMBootWIMPath)                X(WIMInitializeWofDriver) \
    X(WIMIsCurrentSystemWimboot)           X(WIMIsReferenceWim) \
    X(WIMProcessCustomImage)               X(WIMReadFileEx) \
    X(WIMRedirectFolderBeforeApply)        X(WIMSetImageUserSpecifiedCreationTime) \
    X(WIMSetWimGuid)                       X(WIMSingleInstanceFile) \
    X(WIMUpdateWIMBootEntry)               X(WIMWriteFileWithIntegrity)

// 随包新版 DLL 新增、SDK 头同样未声明原型的 15 个导出（手工 PFN）；
// [R-FAKE] 2026-10-19 实测（Python PE32+ 导出表解析）：
//   System32\wimgapi.dll（2026/10/14 更新，60 导出）与 bin64\amd64\wimgapi.dll（60 导出）
//   均不导出本表任何符号 —— BUNDLED 18/18、SIG 31/31 全部存在，本表 0/15 全部缺失。
// 结论：这批符号是「推测性/未经实证」的 ADK 扩展名，真实 wimgapi.dll 从未导出。
// 保留槽位以兼容未来真实 ADK 版本；解析失败时槽位为 nullptr，调用点已有
// `if (!g_wimApi.XXX) { SetLastError(ERROR_PROC_NOT_FOUND); return ...; }` 空槽保护，
// 行为安全，仅返回 ERROR_PROC_NOT_FOUND。
// 注意：WIMGetInstanceCount 的调用点（CWIMHelper::GetInstanceCount L800）仅
// `if (!g_wimApi.WIMGetInstanceCount) return 0;`，同为安全降级。
#define WIM_EXTRA_ADK_LIST(X) \
    X(WIMApplyInstance)            X(WIMCaptureInstance) \
    X(WIMCreateFileEx)             X(WIMExtractImagePathByWimHandle) \
    X(WIMExtractInstancePath)      X(WIMFindFirstInstanceFile) \
    X(WIMFindNextInstanceFile)     X(WIMGetInstanceCount) \
    X(WIMGetInstanceInformation)   X(WIMSetInstanceInformation) \
    X(WIMGetWimFileSize)           X(WIMLoadInstance) \
    X(WIMLoadOSInformation)        X(WIMReadInstanceFile) \
    X(WIMSetCachedSigningLevel)

// --- 全量函数指针表（手工 IAT 槽位）---------------------------------------
struct CWIMApiTable
{
    HMODULE hModule = nullptr;              // LoadLibraryExW 得到的模块句柄
    DWORD   cSlots = 0;                     // 参与解析的槽位总数（含推测性 ADK 槽位）
    DWORD   cResolved = 0;                  // 实际解析成功的【真实】槽位数（排除 ADK 推测性槽位）
    DWORD   cAdkFake = 0;                   // [R-FAKE] 推测性 ADK 槽位数（PE 实测 0/15 缺失）
    wchar_t szModulePath[MAX_PATH] = {};    // 实际加载的 DLL 全路径（诊断用）

    // wimgapi.h 已声明原型：decltype 推导，签名零手抄误差。
#define WIM_SLOT(name) decltype(::name)* name = nullptr;
    WIM_REQUIRED_LIST(WIM_SLOT)
    WIM_SIG_LIST(WIM_SLOT)
    // 头文件无原型：手工 PFN_<导出名>。
#undef WIM_SLOT
#define WIM_SLOT(name) PFN_##name name = nullptr;
    WIM_EXTRA_BUNDLED_LIST(WIM_SLOT)
    WIM_EXTRA_ADK_LIST(WIM_SLOT)
#undef WIM_SLOT
};

inline CWIMApiTable g_wimApi;
inline wchar_t g_szWimDiag[260] = {};   // 最近一次加载失败的中文诊断
inline const wchar_t* WimLastDiag() { return g_szWimDiag; }

// 读取 DLL 的 PE Machine 字段：x64 进程只接受 IMAGE_FILE_MACHINE_AMD64。
inline bool WimPeMachineMatches(const wchar_t* pszPath, unsigned short* puMachine)
{
    win::HandlePtr hfPtr = win::file_handle(::CreateFileW(pszPath, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));   // [C++17 R11] auto CloseHandle
    if (!hfPtr) return false;
    HANDLE hf = hfPtr.get();   // [C++17 R11] non-owning view; hfPtr closes at scope end
    bool bOk = false;
    IMAGE_DOS_HEADER dos = {};
    DWORD dwRd = 0;
    if (::ReadFile(hf, &dos, sizeof(dos), &dwRd, nullptr) && dwRd == sizeof(dos) &&
        dos.e_magic == IMAGE_DOS_SIGNATURE)
    {
        LARGE_INTEGER li;
        li.LowPart = dos.e_lfanew; li.HighPart = 0;
        if (::SetFilePointerEx(hf, li, nullptr, FILE_BEGIN))
        {
            DWORD dwSig = 0;
            IMAGE_FILE_HEADER fh = {};
            if (::ReadFile(hf, &dwSig, 4, &dwRd, nullptr) && dwRd == 4 &&
                dwSig == IMAGE_NT_SIGNATURE &&
                ::ReadFile(hf, &fh, sizeof(fh), &dwRd, nullptr) &&
                dwRd == sizeof(fh))
            {
                if (puMachine) *puMachine = fh.Machine;
#if defined(_M_X64)
                bOk = (fh.Machine == IMAGE_FILE_MACHINE_AMD64);
#elif defined(_M_ARM64)
                bOk = (fh.Machine == IMAGE_FILE_MACHINE_ARM64);
#else
                bOk = (fh.Machine == IMAGE_FILE_MACHINE_I386);
#endif
            }
        }
    }
    // [C++17 R11] hfPtr destructor closes handle (was: ::CloseHandle(hf))
    return bOk;
}

inline bool EnsureWIMGAPILoaded()
{
    if (g_wimApi.hModule) return true;   // 快速路径（已解析）

    // 首次解析加锁：多 worker 线程并发首调时只允许一个线程绑定。
    static SRWLOCK s_lock = SRWLOCK_INIT;
    ::AcquireSRWLockExclusive(&s_lock);

    bool bOk = false;
    HMODULE hCand = nullptr;
    auto FinishFail = [&](const wchar_t* pszMsg)
    {
        if (hCand) { ::FreeLibrary(hCand); hCand = nullptr; }
        if (pszMsg && g_szWimDiag[0] == 0)
        {
            ::wcscpy_s(g_szWimDiag, _TRUNCATE, pszMsg);
        }
        g_wimApi = CWIMApiTable();   // 半解析的槽位全部清零
        g_wimApi.szModulePath[0] = 0;
    };

    if (g_wimApi.hModule) { bOk = true; }   // 双检
    else
    {
      // 候选 1：随包副本（WinPE/精简系统优先）。架构依赖目录 amd64\ 优先，
      // 兼容主程序同目录扁平布局；逐份做 PE Machine 校验。
      wchar_t szExe[MAX_PATH] = {};
      if (::GetModuleFileNameW(nullptr, szExe, MAX_PATH) > 0)
      {
        wchar_t* pSlash = wcsrchr(szExe, L'\\');
        if (pSlash)
        {
            *(pSlash + 1) = 0;
            const wchar_t* const kRel[] = { L"amd64\\wimgapi.dll", L"wimgapi.dll" };
            for (int i = 0; i < 2 && !hCand; ++i)
            {
                std::wstring sLocal = std::wstring(szExe) + kRel[i];
                if (::GetFileAttributesW(sLocal.c_str()) == INVALID_FILE_ATTRIBUTES)
                    continue;
                unsigned short uMachine = 0;
                if (!WimPeMachineMatches(sLocal.c_str(), &uMachine))
                {
                    wfmt::FormatInto(g_szWimDiag, _countof(g_szWimDiag),
                        L"wimgapi.dll 架构不匹配（PE Machine=0x%04X），已忽略随包副本",
                        (unsigned)uMachine);
                    continue;
                }
                hCand = ::LoadLibraryExW(sLocal.c_str(), nullptr,
                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                    LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
                if (hCand) g_szWimDiag[0] = 0;
            }
        }
      }

      // 候选 2：标准搜索（System32\wimgapi.dll / ADK 路径等）。
      if (!hCand)
      {
        hCand = ::LoadLibraryExW(L"wimgapi.dll", nullptr,
                                 LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (hCand) g_szWimDiag[0] = 0;
      }

      if (!hCand)
      {
        DWORD dwErr = ::GetLastError();
        if (g_szWimDiag[0] == 0)
        {
            wfmt::FormatInto(g_szWimDiag, _countof(g_szWimDiag),
                dwErr == ERROR_BAD_EXE_FORMAT
                    ? L"wimgapi.dll 位数/格式错误（%lu）"
                    : L"wimgapi.dll 无法加载（%lu）",
                (unsigned long)dwErr);
        }
      }
      else
      {
        // —— 一次性逐名解析全部槽位（复刻 Dism++ 主程序名字表绑定法）——
        // [R-FAKE] 新增 bAdk 标记：区分 BUNDLED（实测 18/18 全导出）与
        // ADK 推测性（实测 0/15 全缺失）槽位，使 cResolved 只统计真实可用导出。
        struct WimSlotDesc { const char* psz; FARPROC* pp; bool bReq; bool bAdk; };
#define WIM_DESC_R(n) { #n, reinterpret_cast<FARPROC*>(&g_wimApi.n), true, false },
#define WIM_DESC_O(n) { #n, reinterpret_cast<FARPROC*>(&g_wimApi.n), false, false },
#define WIM_DESC_A(n) { #n, reinterpret_cast<FARPROC*>(&g_wimApi.n), false, true },
        const WimSlotDesc descs[] = {
            WIM_REQUIRED_LIST(WIM_DESC_R)
            WIM_SIG_LIST(WIM_DESC_O)
            WIM_EXTRA_BUNDLED_LIST(WIM_DESC_O)
            WIM_EXTRA_ADK_LIST(WIM_DESC_A)
        };
#undef WIM_DESC_R
#undef WIM_DESC_O
#undef WIM_DESC_A

        g_wimApi.cSlots = static_cast<DWORD>(sizeof(descs) / sizeof(descs[0]));
        g_wimApi.cResolved = 0;
        g_wimApi.cAdkFake = 0;
        const char* pszMissingReq = nullptr;
        for (DWORD i = 0; i < g_wimApi.cSlots; ++i)
        {
            FARPROC p = ::GetProcAddress(hCand, descs[i].psz);
            *(descs[i].pp) = p;
            if (p)
            {
                if (!descs[i].bAdk) ++g_wimApi.cResolved;   // 仅统计真实导出
                else ++g_wimApi.cAdkFake;                    // 推测性 ADK 意外解析成功
            }
            else if (descs[i].bReq && !pszMissingReq) pszMissingReq = descs[i].psz;
        }

        if (pszMissingReq)
        {
            // 直接用宽字符拼接（窄字面量经 /utf-8 编译，禁止 CP_ACP 转换，
            // 否则在中文系统诊断变乱码、非中文系统直接转换失败）。
            wfmt::FormatInto(g_szWimDiag, _countof(g_szWimDiag),
                L"wimgapi.dll 缺少导出 %hs", pszMissingReq);
            FinishFail(nullptr);
        }
        else
        {
            g_wimApi.hModule = hCand;   // 提交：此后模块不可卸载
            DWORD cch = ::GetModuleFileNameW(hCand, g_wimApi.szModulePath, MAX_PATH);
            (void)cch;
            // [R-FAKE] 诊断：真实解析数 / 推测性 ADK 槽位意外解析数。
            // 正常：real=58 adk=0 slots=73（9 req + 31 sig + 18 bundled + 15 adk）；
            // 若 adk>0 说明当前 wimgapi 意外导出了推测性符号，可反向验证 R-FAKE 结论。
            wfmt::FormatInto(g_szWimDiag, _countof(g_szWimDiag),
                L"wimgapi resolved: real=%u adkFake=%u slots=%u",
                (unsigned)g_wimApi.cResolved, (unsigned)g_wimApi.cAdkFake,
                (unsigned)g_wimApi.cSlots);
            bOk = true;
        }
      }
    }

    ::ReleaseSRWLockExclusive(&s_lock);
    return bOk;
}

// --- IMAGE_INFO ------------------------------------------------------------
// [R-B86] 实测（wimgapi 10.0.19041 / 随包 ADK 版一致）：WIMGetImageInformation
// 返回的缓冲区不是固定结构体，而是以 UTF-16LE BOM(FF FE) 起始的 WIM_XML 文档
// （cb=3214 实测样本），名称/描述/大小/架构全部是 XML 标签：
//   <IMAGE INDEX="1"><TOTALBYTES>…</TOTALBYTES>…<WINDOWS>
//   <ARCH>9</ARCH><EDITIONID>EnterpriseS</EDITIONID>
//   <LANGUAGES><LANGUAGE>zh-CN</LANGUAGE>…<NAME>…</NAME><DESCRIPTION>…</DESCRIPTION>
// 旧版封装按 2612 字节固定布局 memcpy，名称/大小全是 XML 乱码，本结构改为
// 解析后的逻辑视图（ParseImageInfoBlob 填充）。
#ifndef __WIMHELPER_IMAGE_INFO_DEFINED__
#define __WIMHELPER_IMAGE_INFO_DEFINED__
struct IMAGE_INFO
{
    DWORD  dwVersion;            // 固定 1
    GUID   guidImageId;          // 保留（XML 不提供，置 0）
    BOOL   fBootable;            // 由 WIM_INFO.BootIndex==当前索引 判定
    DWORD  dwCompression;        // WIM_COMPRESS_*（来自 WIM_INFO.CompressionType）
    DWORD  dwArchitecture;       // <ARCH>：0=x86 9=x64(amd64) 12=arm64 14=arm64ec
    ULONG64 ullFileCount;        // <FILECOUNT>
    ULONG64 ullUncompressedSize; // <TOTALBYTES>（dism 显示的 Size）
    ULONG64 ullCompressedSize;   // XML 不提供，置 0
    wchar_t ImageName[256];      // <NAME>
    wchar_t ImageDescription[1024]; // <DESCRIPTION>
    wchar_t EditionId[64];       // <EDITIONID>
    wchar_t Version[32];         // <VERSION>（如 6.3.9600；Win7 映像可能为空）
    wchar_t Language[32];        // <LANGUAGES> 首个 <LANGUAGE>
    wchar_t CreationDate[32];    // <CREATIONTIME> 转出的 UTC 字符串
};
using PIMAGE_INFO = IMAGE_INFO;   // [C++17 R-P8] 替代原 C 风格 typedef 别名
#endif

// --- Callback signature aliases (C-style proc pointers) -------------------
using WIMMsgProc = DWORD ( WINAPI*)(
    DWORD  dwMessageId,
    WPARAM wParam,
    LPARAM lParam,
    void*  pvUserData);

// --- Convenience typedefs ---------------------------------------------------
using WIMHandle = HANDLE;
using WIMImageHandle = HANDLE;
using WIMInstanceHandle = HANDLE;

// --- Log severity (used by WIMHelperBridge::Log) ---------------------------
enum class LogLevel : DWORD {
    Debug = 0,
    Info  = 1,
    Warn  = 2,
    Error = 3,
    Fatal = 4,
};

// --- Progress & log structs ------------------------------------------------
struct WIMProgress {
    DWORD   percent    = 0;
    DWORD   totalFiles = 0;
    DWORD   doneFiles  = 0;
    wchar_t*  fileName   = nullptr;
    wchar_t*  stageText  = nullptr;
};

struct WIMLogEntry {
    SYSTEMTIME ts;
    DWORD      level;    // WIM_MSG_* id
    wchar_t*     text;
};

// --- 魔数级容器识别（与扩展名/OS 版本无关，纯文件头判定）------------------
// 知识库 41 §2/§18：WIMCreateFile 以 MSWIM 魔数与内部 magic(0xFEEDF001/2)
// 判定容器；ISO(0xCD001)/VHD/VHDX 不属 wimgapi 容器。此处把该判定下沉为
// 统一入口，供所有"打开映像"路径复用：改名/无扩展名/加密 ESD 均可识别。
enum WIM_CONTAINER_KIND : DWORD {
    WIM_CK_UNKNOWN = 0,
    WIM_CK_WIM,            // 明文 WIM（XPRESS/LZX）
    WIM_CK_ESD,            // 明文 ESD（LZMS）
    WIM_CK_ENCRYPTED_ESD,  // 加密 ESD（需先解密）
    WIM_CK_SWM,            // 跨卷分卷集（首部分）
    WIM_CK_ISO,            // ISO9660 / UDF
    WIM_CK_VHD,            // 虚拟硬盘 VHD
    WIM_CK_VHDX,           // 虚拟硬盘 VHDX
};

struct WIM_CONTAINER_INFO {
    WIM_CONTAINER_KIND kind = WIM_CK_UNKNOWN;
    DWORD   dwFlags = 0;        // WIM 头 dwFlags
    DWORD   dwChunkSize = 0;    // WIM 头 chunk_size（LZMS 通常 131072）
    DWORD   dwImageCount = 0;
    DWORD   dwPartNumber = 0;    // 当前部分序号（SWM：1 of N）
    DWORD   dwTotalParts = 0;
    DWORD   dwBootIndex = 0;
    bool    bEncrypted = false;
    wchar_t szGuid[40] = {};
};

// --- WIMHelper class --------------------------------------------------------
// A thin wrapper around wimgapi.dll. All methods return false on failure
// and store GetLastError() / API HRESULT in the caller-supplied hrError.
class CWIMHelper {
public:
    // ---- ctor / dtor -----------------------------------------------------
    CWIMHelper();
    ~CWIMHelper();

    // ---- DLL availability ------------------------------------------------
    // Returns true if wimgapi.dll is linked / loadable.
    static bool IsAvailable();

    // ---- 魔数级容器探测（只读文件头，不依赖 wimgapi/扩展名）--------------
    static bool DetectContainer(const std::wstring& path,
                                WIM_CONTAINER_INFO& out);

    // ===================================================================
    // Phase 2: WIM lifecycle (15 APIs)
    // ===================================================================

    // Open an existing WIM for read/write.
    // access = GENERIC_READ | GENERIC_WRITE | WIM_GENERIC_MOUNT
    static WIMHandle Open(const std::wstring& wimPath, DWORD access);
    static WIMHandle OpenForRead(const std::wstring& wimPath);
    static WIMHandle OpenForWrite(const std::wstring& wimPath);

    // Create a new (or overwrite) WIM file.
    static WIMHandle CreateFile(const std::wstring& wimPath,
                                DWORD compression,
                                DWORD desiredAccess);
    static WIMHandle CreateFileEx(const std::wstring& wimPath,
                                  DWORD compression,
                                  DWORD desiredAccess,
                                  DWORD createFlags);

    // Create a bare image file (for WofCompressed / streaming scenarios).
    static WIMInstanceHandle CreateImageFile(const std::wstring& wimPath,
                                             DWORD compression,
                                             DWORD desiredAccess);

    // Create a WofCompressed "empty" image (Windows 10 RS2+).
    static WIMInstanceHandle CreateWofCompressedFile(const std::wstring& wimPath);

    // Commit a mounted image back into the WIM.
    static bool CommitImageHandle(WIMImageHandle hImage,
                                  DWORD flags,
                                  WIMHandle* pHNewWim);

    // Close any wimgapi handle (WIM, image, or instance).
    static bool CloseHandle(WIMHandle hObject);

    // Copy an existing WIM file to a new path (progress-aware).
    static bool CopyFile(const std::wstring& src,
                         const std::wstring& dst,
                         DWORD flags,
                         WIMProgress* pProgress);

    // Add existing image path(s) to an open WIM (append mode).
    static bool AddImagePath(WIMHandle hWim,
                             const std::wstring& srcImagePath,
                             DWORD flags);
    static bool AddImagePaths(WIMHandle hWim,
                              const std::vector<std::wstring>& srcPaths,
                              DWORD flags);

    // Delete an image index.
    static bool DeleteImage(WIMHandle hWim, DWORD imageIndex);
    static bool DeleteIndex(WIMHandle hWim, DWORD imageIndex);

    // ===================================================================
    // Phase 3: Mount / Unmount (8 APIs)
    // ===================================================================

    // Legacy mount by WIM path + index.
    static bool MountImage(const std::wstring& wimPath,
                           DWORD imageIndex,
                           const std::wstring& mountPath,
                           bool readOnly);

    // Mount by pre-loaded WIM handle (Win7+).
    static bool MountImageHandle(WIMImageHandle hImage,
                                 const std::wstring& mountPath,
                                 DWORD mountFlags);

    // Mount a specific "instance" (Win8+).
    static bool MountInstance(WIMInstanceHandle hInstance,
                              const std::wstring& mountPath,
                              DWORD mountFlags);

    // Unmount variants.
    static bool UnmountImage(const std::wstring& mountPath,
                             DWORD imageIndex,
                             bool commit);
    static bool UnmountImageHandle(WIMImageHandle hImage, bool commit);
    static bool UnmountInstance(WIMInstanceHandle hInstance, bool commit);

    // Query mount state.
    static bool IsImageMounted(const std::wstring& mountPath, DWORD* pIndex);
    static bool GetMountPoint(WIMImageHandle hImage, std::wstring* pPath);

    // ===================================================================
    // Phase 4: Apply / Capture (4 APIs)
    // ===================================================================

    static bool ApplyImage(WIMImageHandle hImage,
                           const std::wstring& targetPath,
                           DWORD applyFlags);
    static bool CaptureImage(WIMHandle hWim,
                             const std::wstring& sourcePath,
                             DWORD captureFlags);
    static bool ApplyInstance(WIMInstanceHandle hInstance,
                              const std::wstring& targetPath,
                              DWORD applyFlags);
    static bool CaptureInstance(WIMInstanceHandle hInstance,
                                const std::wstring& sourcePath,
                                DWORD captureFlags);

    // Export: 把源映像句柄中的映像导出到目标 WIM（WIMExportImage 直调）
    static bool ExportImage(WIMImageHandle hImage,
                            WIMHandle hWim,
                            DWORD flags);

    // ===================================================================
    // Phase 5a: Info queries (8 APIs)
    // ===================================================================

    static DWORD GetImageCount(WIMHandle hWim);
    static bool  GetAttributes(WIMHandle hWim, WIM_INFO* pInfo);
    static bool  GetImageInformation(WIMImageHandle hImage,
                                     IMAGE_INFO* pInfo);
    static bool  GetImageDescription(WIMImageHandle hImage,
                                     std::wstring* pName,
                                     std::wstring* pDescription);

    // [R-B86] 取原始 UTF-16 XML 文本（BOM 已剥离），供需要更多标签的调用方。
    static bool  GetImageInformationXml(WIMImageHandle hImage,
                                        std::wstring* pXml);
    // [R-B86] 解析 WIMGetImageInformation 的 BOM+XML blob（兼容旧版 v1 TLV）。
    static bool  ParseImageInfoBlob(const void* pv, DWORD cb,
                                    IMAGE_INFO* pInfo);
    // [R-B86] 以 Name/Description 构造 BOM+XML 并 WIMSetImageInformation
    // （实测 Set 与 Get 同为 XML 格式；旧封装传 2612 字节结构体必失败）。
    static bool  SetImageInfoStrings(WIMImageHandle hImage,
                                     const wchar_t* pszName,
                                     const wchar_t* pszDescription);
    // [R-B86] 把 wimgapi 临时目录指到 %TEMP%\wimgapi（只读 ISO/DVD 上
    // WIMLoadImage 会因默认临时目录不可写报 1632）。
    static void  PrepareTempPath(WIMHandle hWim);
    static DWORD GetInstanceCount(WIMHandle hWim);
    static bool  GetImageAttribute(WIMImageHandle hImage,
                                   DWORD attr, DWORD* pValue);
    static bool  GetImageIndexDescription(WIMHandle hWim,
                                          DWORD index,
                                          std::wstring* pName,
                                          std::wstring* pDesc);
    static bool  GetInfo(WIMHandle hWim, DWORD infoType, void* pOut);

    // ===================================================================
    // Phase 5b: Callbacks & log (5 APIs)
    // ===================================================================

    // Install a global callback (called for every WIM_MSG_* event).
    static DWORD RegisterCallback(WIMHandle hWim,
                                  WIMMsgProc proc,
                                  void* userData);
    static bool  UnregisterCallback(WIMHandle hWim, WIMMsgProc proc);
    static DWORD GetCallbackCount(WIMHandle hWim);

    // Register / unregister a wimgapi log file (UTF-8 or Unicode).
    static bool  RegisterLogFile(const std::wstring& logPath, DWORD flags);
    static bool  UnregisterLogFile(const std::wstring& logPath);

    // ===================================================================
    // Phase 6: WIMBoot / Modern deployment (4 APIs)
    // ===================================================================

    static bool WimBootAddEntry(const std::wstring& wimPath,
                                const std::wstring& entryPath);
    static bool WimBootUpdateEntry(const std::wstring& wimPath,
                                   const std::wstring& entryPath,
                                   DWORD newFlags);

    // ===================================================================
    // Phase 6.5 (optional ADK-latest): File-level WofCompressed / integrity
    // ===================================================================

    static bool WriteFileWithIntegrity(WIMInstanceHandle hInstance,
                                       const std::wstring& path,
                                       const BYTE* data,
                                       DWORD cbData,
                                       DWORD flags);

    // ===================================================================
    // Phase 7: Extended direct wimgapi.h exports (fill coverage to ~94%)
    // ===================================================================

    static bool SplitFile(WIMHandle hWim,
                          const std::wstring& partPath,
                          PLARGE_INTEGER pPartSize,
                          DWORD flags);
    static bool SetReferenceFile(WIMHandle hWim,
                                 const std::wstring& path,
                                 DWORD flags);
    static bool SetTemporaryPath(WIMHandle hWim,
                                 const std::wstring& path);
    static bool SetBootImage(WIMHandle hWim, DWORD imageIndex);
    static bool SetImageInformation(WIMImageHandle hImage,
                                    void* pvImageInfo, DWORD cbImageInfo);
    static bool RemountImage(const std::wstring& mountPath, DWORD flags);
    static bool DeleteImageMounts(DWORD flags);
    static bool ExtractImagePath(WIMImageHandle hImage,
                                 const std::wstring& imagePath,
                                 const std::wstring& destPath,
                                 DWORD flags);
    static bool GetMountedImageInfoFromHandle(WIMImageHandle hImage,
                                              WORD infoLevel,
                                              void* pBuf, DWORD cbBuf,
                                              DWORD* pNeed);
    static bool GetMountedImageHandle(const std::wstring& mountPath,
                                      DWORD flags,
                                      WIMHandle* pHWim,
                                      WIMImageHandle* pHImage);
    static HANDLE FindFirstImageFile(WIMImageHandle hImage,
                                     const std::wstring& path,
                                     WIM_FIND_DATA* pFind);
    static bool  FindNextImageFile(HANDLE hFind, WIM_FIND_DATA* pFind);
    static bool  EnumImageFiles(WIMImageHandle hImage,
                                WIMEnumImageFilesCallback cb,
                                void* ctx);
    static HANDLE CreateWimImageFile(WIMImageHandle hImage,
                                     const std::wstring& path,
                                     DWORD access, DWORD disp, DWORD attrs);
    static bool  ReadImageFile(WIMInstanceHandle hFile,
                               BYTE* buf, DWORD cb,
                               DWORD* pcbRead,
                               LPOVERLAPPED pOverlapped);
    static bool  InitFileIOCallbacks(void* pCallbacks);
    static bool  SetFileIOCallbackTemporaryPath(const std::wstring& path);
    static bool  MessageCallback(DWORD msgId, WPARAM wParam, LPARAM lParam,
                                 void* user);

    // ===================================================================
    // Phase 8: Full DLL export coverage (73/75 WIMGAPI APIs)
    // AdK-latest APIs not declared in wimgapi.h; dispatched via
    // GetProcAddress on wimgapi.dll.
    // ===================================================================

    // -- Extract path (ADK-latest variants) --------------------------------
    static bool ExtractImageDirectory(WIMHandle hWim,
                                      const std::wstring& imagePath,
                                      const std::wstring& destPath,
                                      DWORD flags);
    static bool ExtractImagePathByWimHandle(WIMHandle hWim,
                                            const std::wstring& imagePath,
                                            const std::wstring& destPath,
                                            DWORD flags);
    static bool ExtractInstancePath(WIMInstanceHandle hInstance,
                                    const std::wstring& imagePath,
                                    const std::wstring& destPath,
                                    DWORD flags);

    // -- Instance enumeration (Win8+) -------------------------------------
    static HANDLE FindFirstInstanceFile(WIMInstanceHandle hInstance,
                                        const std::wstring& path,
                                        WIM_FIND_DATA* pFind);
    static bool   FindNextInstanceFile(HANDLE hFind, WIM_FIND_DATA* pFind);

    // -- Instance info -----------------------------------------------------
    static bool   GetInstanceInformation(WIMInstanceHandle hInstance,
                                         IMAGE_INFO* pInfo);
    static bool   SetInstanceInformation(WIMInstanceHandle hInstance,
                                         void* pv, DWORD cb);

    // -- Mounted images query (legacy shim) --------------------------------
    static bool   GetMountedImages(PWIM_MOUNT_LIST pList, DWORD* pcbList);

    // -- WIMBoot extended (ADK-latest) -------------------------------------
    static bool   GetWIMBootEntries(DWORD* pCount, DWORD* pBuf, DWORD cb);
    static bool   GetWIMBootWIMPath(DWORD* pCount, DWORD* pBuf, DWORD cb);
    static bool   IsCurrentSystemWimboot(BOOL* pResult);

    // -- WIM file size -----------------------------------------------------
    static bool   GetWimFileSize(WIMHandle hWim, ULONGLONG* pSize);

    // -- Wof driver & reference WIM ---------------------------------------
    static bool   InitializeWofDriver();
    static bool   IsReferenceWim(const std::wstring& path, BOOL* pResult);

    // -- LoadImage / LoadInstance -----------------------------------------
    static HANDLE LoadImage(WIMHandle hWim, DWORD imageIndex);
    static HANDLE LoadInstance(WIMHandle hWim, DWORD instanceIndex);

    // -- LoadOSInformation (ADK-latest) ------------------------------------
    static bool   LoadOSInformation(WIMHandle hWim,
                                    void* pv, DWORD cb,
                                    DWORD* pcbActual);

    // -- ReadFileEx / ReadInstanceFile -------------------------------------
    static bool   ReadFileEx(WIMInstanceHandle hInstance,
                             const std::wstring& path,
                             BYTE* buf, DWORD cb,
                             LARGE_INTEGER offset,
                             DWORD* pcbRead,
                             DWORD flags);
    static bool   ReadInstanceFile(WIMInstanceHandle hInstance,
                                   const std::wstring& path,
                                   BYTE* buf, DWORD cb,
                                   DWORD* pcbRead);

    // -- Apply / redirect helpers -----------------------------------------
    static bool   ProcessCustomImage(WIMImageHandle hImage,
                                     void* pv, DWORD cb);
    static bool   RedirectFolderBeforeApply(WIMImageHandle hImage,
                                            const std::wstring& src,
                                            const std::wstring& dst);
    static bool   SingleInstanceFile(WIMInstanceHandle hInstance,
                                     const std::wstring& path,
                                     BYTE* buf, DWORD cb);

    // -- Signing level & image metadata ------------------------------------
    // 参数与真实导出一致：sFile 目标文件；sBasePath 可选基路径（WindowsApps
    // 判定用，无则传 L""）；dwFlags!=0 时以写访问打开；sAltName 可选替代路径。
    static bool   SetCachedSigningLevel(const std::wstring& sFile,
                                        const std::wstring& sBasePath,
                                        DWORD dwFlags,
                                        const std::wstring& sAltName);
    static bool   SetImageUserSpecifiedCreationTime(
                     WIMImageHandle hImage, LPFILETIME pTime);
    static bool   SetWimGuid(WIMHandle hWim, const GUID* pGuid);

    // ===================================================================
    // Phase 9: Direct DLL-export wrappers (name-aligned 73/73 coverage)
    // Directly invoke wimgapi.dll exports via GetProcAddress when the
    // wrapper methods need the actual DLL entry points (not in wimgapi.h).
    // ===================================================================

    static bool   WIMAddImagePathDirect(WIMHandle hWim,
                                        const std::wstring& imagePath,
                                        DWORD flags);
    static bool   WIMAddImagePathsDirect(WIMHandle hWim,
                                         const std::vector<std::wstring>& paths,
                                         DWORD flags);
    static bool   WIMApplyInstanceDirect(WIMInstanceHandle hInstance,
                                         const std::wstring& targetPath,
                                         DWORD flags);
    static HANDLE WIMCreateFileExDirect(const std::wstring& path,
                                        DWORD access, DWORD disp, DWORD attrs,
                                        DWORD compression, DWORD* pCreationResult);
    static HANDLE WIMCreateWofCompressedFileDirect(const std::wstring& path,
                                                    DWORD compression,
                                                    DWORD access);
    static bool   WIMMountImageDirect(const std::wstring& wimPath,
                                      DWORD imageIndex,
                                      const std::wstring& mountPath,
                                      DWORD flags);
    static bool   WIMUnmountImageDirect(const std::wstring& wimPath,
                                        DWORD imageIndex,
                                        const std::wstring& mountPath,
                                        BOOL commit);

    // --- Message dispatcher (C entry point used by callback wrappers) ----
    static DWORD WINAPI DefaultMessageProc(DWORD msgId, WPARAM wParam,
                                           LPARAM lParam, void* user);
};

// --- [R-B87] 调用点透明重定向到手工 IAT ------------------------------------
// 既有全部 WIMXxx(...) 裸调用点（WIMHelper.cpp / ToolDialog.cpp）经同名宏走
// g_wimApi 函数指针表；编译产物不再产生任何 wimgapi.dll 导入引用（普通/延迟
// 导入表均无）。此处只能重定向 9 核心 + 31 个头文件声明导出（裸调用仅涉及这
// 40 个）；36 个 ADK 扩展在 WIMHelper.cpp 内以 g_wimApi.X 显式调用。
// 宏定义位于类声明之后，不影响本头任何类型/成员声明。
#ifndef WIMHELPER_NO_IAT_REDIRECT
#define WIMCreateFile               g_wimApi.WIMCreateFile
#define WIMCloseHandle              g_wimApi.WIMCloseHandle
#define WIMLoadImage                g_wimApi.WIMLoadImage
#define WIMGetImageCount            g_wimApi.WIMGetImageCount
#define WIMGetImageInformation      g_wimApi.WIMGetImageInformation
#define WIMSetImageInformation      g_wimApi.WIMSetImageInformation
#define WIMApplyImage               g_wimApi.WIMApplyImage
#define WIMCaptureImage             g_wimApi.WIMCaptureImage
#define WIMExportImage              g_wimApi.WIMExportImage
#define WIMSetTemporaryPath         g_wimApi.WIMSetTemporaryPath
#define WIMSetReferenceFile         g_wimApi.WIMSetReferenceFile
#define WIMSplitFile                g_wimApi.WIMSplitFile
#define WIMDeleteImage              g_wimApi.WIMDeleteImage
#define WIMGetAttributes            g_wimApi.WIMGetAttributes
#define WIMSetBootImage             g_wimApi.WIMSetBootImage
#define WIMGetMessageCallbackCount  g_wimApi.WIMGetMessageCallbackCount
#define WIMRegisterMessageCallback  g_wimApi.WIMRegisterMessageCallback
#define WIMUnregisterMessageCallback g_wimApi.WIMUnregisterMessageCallback
#define WIMCopyFile                 g_wimApi.WIMCopyFile
#define WIMMountImage               g_wimApi.WIMMountImage
#define WIMUnmountImage             g_wimApi.WIMUnmountImage
#define WIMGetMountedImages         g_wimApi.WIMGetMountedImages
#define WIMInitFileIOCallbacks      g_wimApi.WIMInitFileIOCallbacks
#define WIMSetFileIOCallbackTemporaryPath g_wimApi.WIMSetFileIOCallbackTemporaryPath
#define WIMMountImageHandle         g_wimApi.WIMMountImageHandle
#define WIMRemountImage             g_wimApi.WIMRemountImage
#define WIMCommitImageHandle        g_wimApi.WIMCommitImageHandle
#define WIMUnmountImageHandle       g_wimApi.WIMUnmountImageHandle
#define WIMGetMountedImageInfo      g_wimApi.WIMGetMountedImageInfo
#define WIMGetMountedImageInfoFromHandle g_wimApi.WIMGetMountedImageInfoFromHandle
#define WIMGetMountedImageHandle    g_wimApi.WIMGetMountedImageHandle
#define WIMDeleteImageMounts        g_wimApi.WIMDeleteImageMounts
#define WIMRegisterLogFile          g_wimApi.WIMRegisterLogFile
#define WIMUnregisterLogFile        g_wimApi.WIMUnregisterLogFile
#define WIMExtractImagePath         g_wimApi.WIMExtractImagePath
#define WIMFindFirstImageFile       g_wimApi.WIMFindFirstImageFile
#define WIMFindNextImageFile        g_wimApi.WIMFindNextImageFile
#define WIMEnumImageFiles           g_wimApi.WIMEnumImageFiles
#define WIMCreateImageFile          g_wimApi.WIMCreateImageFile
#define WIMReadImageFile            g_wimApi.WIMReadImageFile
#endif // WIMHELPER_NO_IAT_REDIRECT

#endif // __DUI_WIMHELPER_H__
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
