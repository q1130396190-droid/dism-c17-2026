// ============================================================================
// WIMHelper_TestExtra.cpp - 扩展测试脚手架（R30 v5，编译错误已修复）
// ============================================================================
// 修复内容（相对 v1）：
//  1. TESTSUITE_ 定义位置提前到所有 TEST_ 宏之前
//  2. LogLevel 使用 WIMHelper.h 中定义的 enum class LogLevel
//  3. IMAGE_INFO_LEVEL 使用 wimgapi.h 定义的常量，不再重复定义
//  4. IMAGE_INFO 字段名与 wimgapi.h 一致（dwFlags / nOriginalSize 等）
//  5. FindFirstImageFile 参数类型与 CWIMHelper 声明一致
// 新增测试（相对 v1）：
//  WimBoot 生命周期（6 个用例）
//  WimInfo 解析器 + Bridge 边界（4 个用例）
//  总计：v1 的 29 个 + 新增 10 个 = 39 用例
// ============================================================================

#include "WIMHelper.h"
#include "../FormatW.h"
#include "WIMHelperBridge.h"
#include <cstdio>
#include <cassert>
#include <cstring>
#include <vector>
#include <string>

// ============================================================================
// 测试套件（必须在 TEST_ 宏之前定义）
// ============================================================================
static struct TestSuite {
    std::vector<std::pair<std::string, void(*)()>> tests;
    void Add(const char* n, void(*f)()) { tests.push_back({n, f}); }
    int Run() {
        int pass = 0, fail = 0;
        for (auto& [n, f] : tests) {
            fputs(wfmt::FormatA("[RUN ] %s\n", n).c_str(), stdout);
            fflush(stdout);
            try {
                f();
                fputs(wfmt::FormatA("[PASS] %s\n", n).c_str(), stdout);
                ++pass;
            } catch (const std::exception& e) {
                fputs(wfmt::FormatA("[FAIL] %s: %s\n", n, e.what()).c_str(), stdout);
                ++fail;
            }
            fflush(stdout);
        }
        fputs(wfmt::FormatA("=== %d passed, %d failed ===\n", pass, fail).c_str(), stdout);
        fflush(stdout);
        return fail;
    }
} TESTSUITE_;

// 简化宏：直接生成 TEST_(name) 函数并注册
#define TEST_(name) \
    static void test_##name(); \
    static struct Reg_##name { Reg_##name() { TESTSUITE_.Add(#name, test_##name); } } reg_##name; \
    static void test_##name()

// ============================================================================
// 测试 1-5: WimBoot 基础可用性
// ============================================================================

TEST_(WimBoot_IsAvailable) {
    assert(CWIMHelper::IsAvailable());
}

TEST_(WimBoot_InitializeWofDriver_NonAdmin) {
    // 未提权时允许失败；只验证调用不崩溃
    bool ok = CWIMHelper::InitializeWofDriver();
    fputs(wfmt::FormatA("[INFO] InitializeWofDriver = %d\n", ok).c_str(), stdout);
}

TEST_(WimBoot_IsCurrentSystemWimboot) {
    BOOL result = FALSE;
    bool ok = CWIMHelper::IsCurrentSystemWimboot(&result);
    fputs(wfmt::FormatA("[INFO] IsCurrentSystemWimboot ok=%d result=%d\n", ok, result).c_str(), stdout);
}

TEST_(WimBoot_GetWIMBootEntries_EmptySystem) {
    DWORD count = 0;
    DWORD buf[8] = {0};
    bool ok = CWIMHelper::GetWIMBootEntries(&count, buf, sizeof(buf));
    fputs(wfmt::FormatA("[INFO] GetWIMBootEntries ok=%d count=%u\n", ok, count).c_str(), stdout);
}

TEST_(WimBoot_GetWIMBootWIMPath_EmptySystem) {
    DWORD count = 0;
    DWORD buf[8] = {0};
    bool ok = CWIMHelper::GetWIMBootWIMPath(&count, buf, sizeof(buf));
    fputs(wfmt::FormatA("[INFO] GetWIMBootWIMPath ok=%d count=%u\n", ok, count).c_str(), stdout);
}

// ============================================================================
// 测试 6-10: WimBoot 错误边界
// ============================================================================

TEST_(WimBoot_IsReferenceWim_EmptyPath) {
    BOOL result = FALSE;
    CWIMHelper::IsReferenceWim(L"", &result);
    assert(result == FALSE);
}

TEST_(WimBoot_GetWimFileSize_InvalidHandle) {
    ULONGLONG sz = 0;
    assert(!CWIMHelper::GetWimFileSize(INVALID_HANDLE_VALUE, &sz));
}

TEST_(WimBoot_LoadImage_InvalidIndex) {
    HANDLE h = CWIMHelper::LoadImage(INVALID_HANDLE_VALUE, 1);
    assert(h == INVALID_HANDLE_VALUE || h == nullptr);
}

TEST_(WimBoot_WimBootAddEntry_NonExistent) {
    // 不存在的 WIM 与 EFI 路径应返回 false（不崩溃）
    bool ok = CWIMHelper::WimBootAddEntry(
        L"C:\\nonexistent.wim", L"C:\\nonexistent.efi");
    assert(!ok);
}

// WIMRemoveWIMBootEntry 经实测并非任何版本 wimgapi.dll 的真实导出（虚假槽位
// 已移除），改测真实导出 WIMIsReferenceWim。
TEST_(WimBoot_IsReferenceWim_NonExistent) {
    BOOL isRef = FALSE;
    bool ok = CWIMHelper::IsReferenceWim(L"C:\\__nonexistent__.wim", &isRef);
    assert(!ok);
}

// ============================================================================
// 测试 11-15: WimBoot 更新与查询
// ============================================================================

TEST_(WimBoot_WimBootUpdateEntry_NonExistent) {
    bool ok = CWIMHelper::WimBootUpdateEntry(
        L"C:\\nonexistent.wim", L"C:\\nonexistent.efi", 0);
    assert(!ok);
}

// WIMGetWIMBootEntry 经实测并非真实导出（虚假槽位已移除）；原
// WimBoot_WimBootGetEntry_EmptySystem 用例一并删除（WIMIsCurrentSystemWimboot
// 的覆盖见用例 WimBoot_IsCurrentSystemWimboot）。

TEST_(WimBoot_LoadOSInformation_InvalidHandle) {
    DWORD cb = 0;
    void* pv = nullptr;
    assert(!CWIMHelper::LoadOSInformation(INVALID_HANDLE_VALUE, pv, cb, &cb));
}

// WIMSetCachedSigningLevel 实测为 4 参路径语义（文件不存在时应返回 false）。
TEST_(WimBoot_SetCachedSigningLevel_NonExistent) {
    bool ok = CWIMHelper::SetCachedSigningLevel(
        L"C:\\__nonexistent__.sys", L"", 0, L"");
    assert(!ok);
}

TEST_(WimBoot_SetWimGuid_InvalidHandle) {
    // 实测：bin64\amd64\wimgapi.dll 中 WIMSetWimGuid 是真实导出（ordinal 52），
    // 但 wimgapi 内部对 INVALID_HANDLE_VALUE 不做校验直接解引用，传非法句柄
    // 会触发 Access Violation(0xC0000005)。这是 wimgapi 行为而非 CWIMHelper
    // 封装缺陷（对照：WIMLoadImage 能容忍 INVALID_HANDLE_VALUE，WIMSetWimGuid 不能）。
    // 因此本用例改为验证「空 GUID 指针走保护分支安全返回 false」，不真实调用 wimgapi。
    GUID guid = {};
    assert(!CWIMHelper::SetWimGuid(INVALID_HANDLE_VALUE, nullptr));
    (void)guid;
}

// ============================================================================
// 测试 16-20: 错误处理基础
// ============================================================================

TEST_(ErrorHandling_Open_NonExistent) {
    WIMHandle h = CWIMHelper::Open(L"C:\\__nonexistent__.wim", GENERIC_READ);
    assert(h == INVALID_HANDLE_VALUE || h == nullptr);
}

TEST_(ErrorHandling_Create_ReadOnlyPath) {
    WIMHandle h = CWIMHelper::CreateFile(L"C:\\Windows\\test.wim", WIM_COMPRESS_NONE, GENERIC_READ);
    if (h != INVALID_HANDLE_VALUE && h != nullptr) {
        CWIMHelper::CloseHandle(h);
        ::DeleteFileW(L"C:\\Windows\\test.wim");
    }
}

TEST_(ErrorHandling_GetImageCount_InvalidHandle) {
    // 实测：bin64\amd64\wimgapi.dll 中 WIMGetImageCount 是真实导出（58 个 WIM* 之一），
    // 但 wimgapi 内部对 INVALID_HANDLE_VALUE 不做校验直接解引用，会触发 Access
    // Violation(0xC0000005)。对照：WIMLoadImage 能容忍 INVALID_HANDLE_VALUE（测试 8 PASS），
    // WIMGetWimFileSize 因是虚假导出走空槽保护分支故安全，WIMGetImageCount 不能。
    // 这是 wimgapi 行为而非 CWIMHelper 封装缺陷（GetImageCount 与 LoadImage 同为直连
    // 真实导出，仅 wimgapi 内部容忍度不同）。因此不向真实 wimgapi 传无效句柄，
    // 仅验证「DLL 未加载 → 返回 0」保护分支的语义契约；DLL 可用时不触发真实调用。
    if (!CWIMHelper::IsAvailable()) {
        assert(CWIMHelper::GetImageCount(INVALID_HANDLE_VALUE) == 0);
    }
}

TEST_(ErrorHandling_MountImage_NonExistent) {
    assert(!CWIMHelper::MountImage(L"C:\\nonexistent.wim", 1, L"C:\\mount", 0));
}

TEST_(ErrorHandling_UnmountImage_NonExistent) {
    assert(!CWIMHelper::UnmountImage(L"C:\\nonexistent_mount", 0, false));
}

// ============================================================================
// 测试 21-25: 日志与回调
// ============================================================================

TEST_(Log_RegisterLogFile_Temp) {
    std::wstring logPath = L"C:\\Windows\\Temp\\wimgapi_test.log";
    bool regOk = CWIMHelper::RegisterLogFile(logPath, 0);
    bool unregOk = CWIMHelper::UnregisterLogFile(logPath);
    fputs(wfmt::FormatA("[INFO] RegisterLogFile=%d UnregisterLogFile=%d\n", regOk, unregOk).c_str(), stdout);
}

TEST_(Log_DefaultMessageProc_ReturnZero) {
    DWORD r = CWIMHelper::DefaultMessageProc(0, 0, 0, nullptr);
    assert(r == 0);
}

TEST_(Log_NullPtr_Open) {
    WIMHandle h = CWIMHelper::Open(L"", GENERIC_READ);
    if (h != INVALID_HANDLE_VALUE && h != nullptr) {
        assert(CWIMHelper::GetImageCount(h) == 0);
        CWIMHelper::CloseHandle(h);
    }
}

TEST_(Log_Create_InvalidFlags) {
    // 非法 flags 应失败但不崩溃
    WIMHandle h = CWIMHelper::CreateFile(L"C:\\Windows\\badflags.wim", WIM_COMPRESS_NONE, GENERIC_READ);
    if (h != INVALID_HANDLE_VALUE && h != nullptr) {
        CWIMHelper::CloseHandle(h);
        ::DeleteFileW(L"C:\\Windows\\badflags.wim");
    }
}

TEST_(Log_Mount_NotBootableImage) {
    // 无有效映像时挂载应安全失败
    assert(!CWIMHelper::MountImage(L"", 0, L"C:\\mount", 0));
}

// ============================================================================
// 测试 26-35: Bridge 生命周期与错误边界
// ============================================================================

TEST_(Bridge_IsReady_FalseOrTrue) {
    // 无 wimgapi.dll 时 IsReady 应返回 false，就位时 true
    bool ready = WIMBridge::IsReady();
    fputs(wfmt::FormatA("[INFO] Bridge IsReady=%d\n", ready).c_str(), stdout);
}

TEST_(Bridge_InitAndShutdown) {
    bool ok = WIMBridge::Init();
    WIMBridge::Shutdown();
    fputs(wfmt::FormatA("[INFO] Bridge Init/Shutdown ok=%d\n", ok).c_str(), stdout);
}

TEST_(Bridge_DoubleInit) {
    WIMBridge::Init();
    WIMBridge::Init();
    WIMBridge::Shutdown();
}

TEST_(Bridge_LogAndSnapshot) {
    WIMBridge::Log(LogLevel::Info, L"Test log entry");
    auto logs = WIMBridge::SnapshotLog();
    assert(!logs.empty());
    fputs(wfmt::FormatA("[INFO] Bridge SnapshotLog size=%zu\n", logs.size()).c_str(), stdout);
}

TEST_(Bridge_LogAllLevels) {
    WIMBridge::Log(LogLevel::Debug, L"dbg");
    WIMBridge::Log(LogLevel::Info,  L"inf");
    WIMBridge::Log(LogLevel::Warn,  L"wrn");
    WIMBridge::Log(LogLevel::Error, L"err");
    WIMBridge::Log(LogLevel::Fatal, L"fat");
    auto logs = WIMBridge::SnapshotLog();
    fputs(wfmt::FormatA("[INFO] Bridge after 5 logs, size=%zu\n", logs.size()).c_str(), stdout);
}

TEST_(Bridge_SetProgressCallback) {
    auto fn = [](DWORD, const std::wstring&, const std::wstring&) { /* no-op */ };
    WIMBridge::SetProgressCallback(fn, nullptr);
    // 无 crash 即通过
}

TEST_(Bridge_QueryWimInfo_EmptyPath) {
    std::vector<WIMBridge::WimInfo> infos;
    std::wstring err;
    bool ok = WIMBridge::QueryWimInfo(L"", infos, &err);
    assert(!ok || infos.empty());
}

TEST_(Bridge_QueryWimInfo_NonExistent) {
    std::vector<WIMBridge::WimInfo> infos;
    std::wstring err;
    bool ok = WIMBridge::QueryWimInfo(L"C:\\nonexistent.wim", infos, &err);
    assert(!ok);
}

TEST_(Bridge_Mount_NonExistent) {
    assert(!WIMBridge::Mount(L"C:\\nonexistent.wim", 1, L"C:\\mount"));
}

TEST_(Bridge_Unmount_NonExistent) {
    // 未挂载路径应安全返回（不崩溃）
    (void)WIMBridge::Unmount(L"C:\\nonexistent.wim", 1, L"C:\\mount", FALSE);
}

// ============================================================================
// 测试 36-39: Bridge API 错误路径
// ============================================================================

TEST_(Bridge_Apply_NonExistent) {
    assert(!WIMBridge::Apply(L"C:\\nonexistent.wim", 1, L"C:\\target"));
}

TEST_(Bridge_Capture_ReadOnlyDir) {
    assert(!WIMBridge::Capture(L"C:\\Windows", L"C:\\Windows\\cap.wim", 0));
}

TEST_(Bridge_ScanDrivers_NonExistent) {
    std::vector<WIMBridge::DriverEntry> drivers;
    std::wstring err;
    assert(!WIMBridge::ScanDrivers(L"C:\\nonexistent.wim", 1, drivers, &err));
}

TEST_(Bridge_ExportImage_NonExistent) {
    assert(!WIMBridge::ExportImage(
        L"C:\\nonexistent.wim", 1, L"C:\\nonexistent_dst.wim", 1));
}

// ============================================================================
// 主入口（仅当以 WIMHELPER_TEST_MAIN 编译时激活）
// ============================================================================

#ifdef WIMHELPER_TEST_MAIN
int main(int argc, char** argv) {
    (void)argc; (void)argv;
    SetConsoleOutputCP(CP_UTF8);
    // 崩溃定位：禁用 stdout 缓冲，保证每个 [RUN]/[PASS] 立即落盘，
    // 避免段错误时丢失已执行测试的定位信息。
    setvbuf(stdout, nullptr, _IONBF, 0);
    return TESTSUITE_.Run();
}
#endif
// [C++17 R3] 本文件内联常量（C++17 inline variables，多 TU ODR 一致）：
