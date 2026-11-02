// ============================================================================
//  NCleanerClient.h
//
//  主程序（Win8Metro.exe）访问 NCleaner.dll 的客户端适配层。
//
//  与 CbsHostClient 同构（R-拆分 路线）：NCleaner 的 7 个导出不静态链接进
//  主程序，运行期 LoadLibrary("NCleaner.dll") + GetProcAddress，通过本适配层
//  以 C++ 风格调用，调用方不再感知 DLL 边界。
//
//  逆向基线（知识库《21_核心DLL职责与接口》2.2 / 《08_多模块协作》3.3）：
//    NCleaner 无工厂函数，直接导出 7 个 NC*Cleanup（Base=1）：
//      1 NCCorruptedAppXOnlineCleanup
//      2 NCDOCacheOnlineCleanup
//      3 NCInstallerFolderCleanup
//      4 NCPackageCacheFolderCleanup
//      5 NCSRPointOnlineCleanup
//      6 NCVSPackageCacheCleanup
//      7 NCWinEventLogOnlineCleanup
//    返回值约定：「只有返回 S_OK 代表成功」（《50_QT6》§1.3）。
//
//  生命周期：FrameWnd 构造 Init()、析构 Shutdown()。
//  线程模型：函数指针加载后只读，业务调用可在任意工作线程执行。
//  兜底策略：DLL 缺失/导出不全时 IsReady()=false；调用方回退原生实现，
//            绝不解引用空指针。
// ============================================================================

#ifndef N_CLEANER_CLIENT_H
#define N_CLEANER_CLIENT_H

#include <windows.h>
#include <string>

namespace NCleanerClient
{

// 7 个导出对应的清理项编号（与 CleanJobService::CustomProcIdx 的
// NCleaner family 5..11 一一对应，另加 Spooler 扩展项）。
enum class ECleanup : int
{
    kCleanInstaller    = 0,   // NCInstallerFolderCleanup
    kCleanCorruptedAppx = 1,  // NCCorruptedAppXOnlineCleanup
    kCleanSRPoint      = 2,   // NCSRPointOnlineCleanup
    kCleanDOCache      = 3,   // NCDOCacheOnlineCleanup
    kCleanPackageCache = 4,   // NCPackageCacheFolderCleanup
    kCleanVSPackage    = 5,   // NCVSPackageCacheCleanup
    kCleanEventLog     = 6,   // NCWinEventLogOnlineCleanup

    kCleanCount        = 7
};

// ---- 生命周期 ----

// 加载主程序同目录下 NCleaner.dll 并解析全部 7 个导出。
// DLL 缺失返回 false（pErr 可空）；此时 IsReady()=false。
bool Init(std::wstring* pErr = nullptr);

// 卸载（FrameWnd 析构路径）。
void Shutdown();

// 是否已就绪（DLL 已加载且 7 个导出全部解析成功）。
bool IsReady();

// DLL 是否已加载（不要求导出齐全）。
bool IsLoaded();

// 取最后一次错误描述（Init 失败原因）。
const std::wstring& LastError();

// ---- 业务接口 ----

// 调用指定清理项（等价原版按选中项调用对应 NC*Cleanup）。
// 返回 HRESULT：S_OK(0) 成功；DLL 未就绪返回 E_NOTIMPL。
long Invoke(ECleanup e, std::wstring* pErr = nullptr);

// 取清理项名称（与 NCleaner.dll 导出名一致，用于日志/核对）。
const wchar_t* NameOf(ECleanup e);

}  // namespace NCleanerClient

// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：

#endif  // N_CLEANER_CLIENT_H
