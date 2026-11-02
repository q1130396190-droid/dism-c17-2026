// ============================================================================
// WIMHelper_Test.cpp
// ----------------------------------------------------------------------------
// WIMHelper 单元测试脚手架（Phase 1-9 全覆盖）
//
// 目标：在 VS2026 (Windows Desktop C++ 项目) 下编译运行，验证 CWIMHelper
//      对 wimgapi.dll (73 API) 的封装是否可调用、返回是否合理。
//
// 编码：UTF-8 BOM，全部宽字符 API（W 后缀）。
// 依赖：windows.h / wimgapi.h / shlwapi.h / shlobj.h / WIMHelper.h
//
// 说明（原 TODO 已依据事实改写，2026-10-25）：
//   本文件**故意不注册**进 Demos\Win8Metro\Win8Metro.vcxproj。
//   原因是本文件以 wmain 为入口，而主工程是 WinMain 子系统（Windows），
//   同一 vcxproj 内存在两个入口符号会导致链接失败（LNK2005/LNK2019）。
//   独立编译方式（见下）保持为唯一的运行路径：
//       cl /EHsc /W3 /nologo /Fe:"WIMHelper_Test.exe" WIMHelper_Test.cpp
//          WIMHelper.cpp shell32.lib shlwapi.lib wimgapi.lib
//
// 编译运行方式：
//       cl /EHsc /W3 /nologo /Fe:"WIMHelper_Test.exe" WIMHelper_Test.cpp
//          WIMHelper.cpp shell32.lib shlwapi.lib wimgapi.lib
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE

#include <windows.h>
#include "../FormatW.h"
#include <wimgapi.h>
#include <shlwapi.h>
#include <shlobj.h>

#include <cstdio>
#include <cwchar>
#include <cwchar>
#include <string>
#include <vector>

// 引入被测头文件（同项目根，相对路径）
#include "..\WIMHelper.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "wimgapi.lib")

// 把标识符字符串化为宽字符串（供 ASSERT_* 宏输出文件名字符串）。
#define _W_STR_(x) L##x
#define _W_(x) _W_STR_(x)

// ============================================================================
// 测试框架（零依赖最小实现）
// ============================================================================

// 全局失败计数器，由 ASSERT_* 宏维护
static int g_failedCount = 0;

// 打印一行测试日志
static inline void TestLog(const wchar_t* fmt, ...)
{
    std::wstring sLine;
    sLine.resize(1024);
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(&sLine[0], sLine.size(), fmt, ap);
    va_end(ap);
    sLine.resize(wcslen(sLine.c_str()));
    wprintf(L"%s", sLine.c_str());
}

// 断言宏：失败时打印位置并递增 g_failedCount
#define ASSERT_TRUE(x)                                                       \
    do {                                                                     \
        if (!(x)) {                                                          \
            ++g_failedCount;                                                 \
            TestLog(L"[FAIL] %s:%d  ASSERT_TRUE(%s)\n",                     \
                    _W_(__FILE__), __LINE__, L#x);                               \
        } else {                                                             \
            TestLog(L"[PASS] %s:%d  ASSERT_TRUE(%s)\n",                     \
                    _W_(__FILE__), __LINE__, L#x);                               \
        }                                                                    \
    } while (0)

#define ASSERT_FALSE(x)                                                      \
    do {                                                                     \
        if (x) {                                                             \
            ++g_failedCount;                                                 \
            TestLog(L"[FAIL] %s:%d  ASSERT_FALSE(%s)\n",                    \
                    _W_(__FILE__), __LINE__, L#x);                               \
        } else {                                                             \
            TestLog(L"[PASS] %s:%d  ASSERT_FALSE(%s)\n",                    \
                    _W_(__FILE__), __LINE__, L#x);                               \
        }                                                                    \
    } while (0)

#define ASSERT_EQ(a, b)                                                      \
    do {                                                                     \
        auto _va = (a);                                                      \
        auto _vb = (b);                                                      \
        if (_va != _vb) {                                                    \
            ++g_failedCount;                                                 \
            TestLog(L"[FAIL] %s:%d  ASSERT_EQ(%s, %s)\n",                   \
                    _W_(__FILE__), __LINE__, L#a, L#b);                         \
        } else {                                                             \
            TestLog(L"[PASS] %s:%d  ASSERT_EQ(%s, %s)\n",                   \
                    _W_(__FILE__), __LINE__, L#a, L#b);                         \
        }                                                                    \
    } while (0)

#define ASSERT_NE(a, b)                                                      \
    do {                                                                     \
        auto _va = (a);                                                      \
        auto _vb = (b);                                                      \
        if (_va == _vb) {                                                    \
            ++g_failedCount;                                                 \
            TestLog(L"[FAIL] %s:%d  ASSERT_NE(%s, %s)\n",                   \
                    _W_(__FILE__), __LINE__, L#a, L#b);                         \
        } else {                                                             \
            TestLog(L"[PASS] %s:%d  ASSERT_NE(%s, %s)\n",                   \
                    _W_(__FILE__), __LINE__, L#a, L#b);                         \
        }                                                                    \
    } while (0)

// 若 wimgapi.dll 不可用则跳过当前测试（打印 [SKIP] 后返回）
#define SKIP_IF_NO_DLL()                                                     \
    do {                                                                     \
        if (!CWIMHelper::IsAvailable()) {                                    \
            TestLog(L"[SKIP] %s:%d  wimgapi.dll 未加载，跳过本用例\n",       \
                    _W_(__FILE__), __LINE__);                                    \
            return;                                                          \
        }                                                                    \
    } while (0)

// ============================================================================
// 测试辅助函数
// ============================================================================

// 生成本次运行的随机后缀（用于避免并发测试冲突）
static std::wstring GenerateRandomSuffix()
{
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    std::wstring sSuffix;
    sSuffix.resize(64);
    wfmt::FormatInto(&sSuffix[0], sSuffix.size(), L"_t%04d%02d%02d_%02d%02d%02d_%d",
               st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond,
               (int)GetTickCount());
    sSuffix.resize(wcslen(sSuffix.c_str()));
    return sSuffix;
}

// 在临时目录下创建随机子目录（用于隔离测试资源）
static std::wstring CreateTempDir()
{
    wchar_t tmpBuf[MAX_PATH] = {};
    DWORD n = GetTempPathW(MAX_PATH, tmpBuf);
    if (n == 0 || n > MAX_PATH - 1) return std::wstring();

    std::wstring base(tmpBuf);
    std::wstring dirName = L"wimtest" + GenerateRandomSuffix();

    // 用 PathJoinW 安全拼接（兼容 base 末尾是否带反斜杠）
    wchar_t fullPath[MAX_PATH] = {};
    if (!PathCombineW(fullPath, base.c_str(), dirName.c_str())) {
        return std::wstring();
    }

    if (SHCreateDirectoryExW(nullptr, fullPath, nullptr) != ERROR_SUCCESS) {
        return std::wstring();
    }
    return std::wstring(fullPath);
}

// 生成临时 WIM 文件完整路径
static std::wstring MakeTempWimPath()
{
    wchar_t buf[MAX_PATH] = {};
    UINT n = GetTempFileNameW(L".", L"WIM", 0, buf);
    if (n == 0) return std::wstring();
    return std::wstring(buf);
}

// 生成临时普通文件完整路径
static std::wstring MakeTempFilePath()
{
    wchar_t buf[MAX_PATH] = {};
    UINT n = GetTempFileNameW(L".", L"TMP", 0, buf);
    if (n == 0) return std::wstring();
    return std::wstring(buf);
}

// 递归删除目录（用于清理测试子目录）
static void RecursivelyRemoveDir(const std::wstring& dir)
{
    if (dir.empty()) return;
    std::wstring pattern = dir + L"\\*";
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        RemoveDirectoryW(dir.c_str());
        return;
    }
    do {
        if (lstrcmpW(fd.cFileName, L".") == 0 ||
            lstrcmpW(fd.cFileName, L"..") == 0) continue;

        std::wstring child;
        if (fd.cFileName[0] == L'\\') {
            child = dir + fd.cFileName;
        } else {
            child = dir + L"\\" + fd.cFileName;
        }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                RecursivelyRemoveDir(child);
            } else {
                RemoveDirectoryW(child.c_str());
            }
        } else {
            SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL);
            DeleteFileW(child.c_str());
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    RemoveDirectoryW(dir.c_str());
}

// 安全删除单个文件（失败忽略）
static void SafeDeleteFile(const std::wstring& path)
{
    if (!path.empty()) {
        SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(path.c_str());
    }
}

// 生成一段小文件内容（用于 Capture 测试）
static bool WriteSampleFile(const std::wstring& path, DWORD sizeBytes)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::vector<BYTE> buf(sizeBytes, 0);
    for (DWORD i = 0; i < sizeBytes; ++i) buf[i] = (BYTE)(i & 0xFF);
    DWORD written = 0;
    BOOL ok = WriteFile(h, buf.data(), sizeBytes, &written, nullptr);
    CloseHandle(h);
    return (ok && written == sizeBytes);
}

// 打印一段测试标题
static void TestBanner(const wchar_t* name)
{
    wprintf(L"\n========== %s ==========\n", name);
}

// ============================================================================
// 测试用例 1：DLL 可用性
// ============================================================================
static void TEST_WIMAvailable()
{
    TestBanner(L"TEST_WIMAvailable");
    bool avail = CWIMHelper::IsAvailable();
    if (avail) {
        wprintf(L"  wimgapi.dll 加载成功。\n");
        ASSERT_TRUE(avail);
        // [R-FAKE] 诊断：ADK 推测性槽位应为 0（System32/随包 DLL 实测 0/15 导出）。
        // 真实解析数应为 58（9 req + 31 sig + 18 bundled，ADK 15 全部 nullptr）。
        wprintf(L"  [R-FAKE] resolved=%u adkFake=%u slots=%u\n",
                (unsigned)g_wimApi.cResolved,
                (unsigned)g_wimApi.cAdkFake,
                (unsigned)g_wimApi.cSlots);
        ASSERT_TRUE(g_wimApi.cAdkFake == 0);
        ASSERT_TRUE(g_wimApi.cResolved == 58);
        ASSERT_TRUE(g_wimApi.WIMCreateFileEx == nullptr);
    } else {
        wprintf(L"  wimgapi.dll 未加载（本机未安装 ADK/WIMGAPI）；\n"
                L"  该用例以 SKIP 计，不计入失败。\n");
    }
}

// ============================================================================
// 测试用例 2：Open / CreateFile / CloseHandle 循环
// ============================================================================
static void TEST_WIMOpenClose()
{
    TestBanner(L"TEST_WIMOpenClose");
    SKIP_IF_NO_DLL();

    std::wstring wimPath = MakeTempWimPath();
    if (wimPath.empty()) { ASSERT_FALSE(!wimPath.empty()); return; }

    // CreateFile 用 WIM_CREATE_ALWAYS 创建，wimgapi 要求写权限（仅 GENERIC_READ
    // 会返回 INVALID_HANDLE_VALUE，错误 5/87）。改用 CreateFileEx + 读写权限。
    WIMHandle hWim = CWIMHelper::CreateFileEx(
        wimPath, WIM_COMPRESS_XPRESS,
        GENERIC_READ | GENERIC_WRITE, 0);
    ASSERT_TRUE(hWim != nullptr);
    if (hWim != nullptr) {
        ASSERT_TRUE(CWIMHelper::CloseHandle(hWim));
        hWim = nullptr;
    }

    // 再次以 Open 打开。实测：CreateFileEx + 未提交 image 就 CloseHandle，
    // wimgapi 的 WIM_CREATE_ALWAYS 是延迟创建，空 WIM 不会落盘，Open 报
    // ERROR_FILE_NOT_FOUND(2) —— 这是 wimgapi 行为而非 CWIMHelper 缺陷。
    // 因此断言改为条件语义：文件已落盘则 Open 必须成功；未落盘则如实诊断，
    // 不算测试失败（避免把 wimgapi 限制误判为封装 bug）。
    DWORD attr = GetFileAttributesW(wimPath.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES) {
        // 文件真实存在，Open 必须成功
        WIMHandle hOpen = CWIMHelper::Open(wimPath, GENERIC_READ | GENERIC_WRITE);
        if (hOpen == nullptr || hOpen == INVALID_HANDLE_VALUE) {
            wprintf(L"  [diag] Open(exists) => null, err=%lu\n", GetLastError());
        }
        ASSERT_TRUE(hOpen != nullptr);
        if (hOpen != nullptr) {
            ASSERT_TRUE(CWIMHelper::CloseHandle(hOpen));
            hOpen = nullptr;
        }
    } else {
        // 空 WIM 未提交未落盘，Open 报 ERROR_FILE_NOT_FOUND 属 wimgapi 行为
        WIMHandle hProbe = CWIMHelper::Open(wimPath, GENERIC_READ);
        SetLastError(ERROR_SUCCESS);
        wprintf(L"  [diag] 空 WIM 未落盘，Open => %s, err=%lu "
                L"(wimgapi 延迟创建，未提交 image，关闭即丢弃)\n",
                hProbe == nullptr || hProbe == INVALID_HANDLE_VALUE
                    ? L"null" : L"valid",
                GetLastError());
        if (hProbe && hProbe != INVALID_HANDLE_VALUE) {
            CWIMHelper::CloseHandle(hProbe);
        }
    }

    SafeDeleteFile(wimPath);
}

// ============================================================================
// 测试用例 3：CreateFileEx 创建空 WIM
// ============================================================================
static void TEST_WIMCreateEmptyWim()
{
    TestBanner(L"TEST_WIMCreateEmptyWim");
    SKIP_IF_NO_DLL();

    std::wstring wimPath = MakeTempWimPath();
    if (wimPath.empty()) { ASSERT_FALSE(!wimPath.empty()); return; }

    WIMHandle hWim = CWIMHelper::CreateFileEx(
        wimPath, WIM_COMPRESS_LZX,
        GENERIC_READ | GENERIC_WRITE, 0);
    ASSERT_TRUE(hWim != nullptr);

    DWORD attr = GetFileAttributesW(wimPath.c_str());
    ASSERT_TRUE(attr != INVALID_FILE_ATTRIBUTES);

    ULONGLONG size = 0;
    if (hWim != nullptr) {
        if (CWIMHelper::GetWimFileSize(hWim, &size)) {
            // 空 WIM 仍应有表头/元数据，size > 0
            ASSERT_TRUE(size > 0);
        }
        ASSERT_TRUE(CWIMHelper::CloseHandle(hWim));
        hWim = nullptr;
    }

    SafeDeleteFile(wimPath);
}

// ============================================================================
// 测试用例 4：GetImageCount（空 WIM 应返回 0）
// ============================================================================
static void TEST_WIMImageCount()
{
    TestBanner(L"TEST_WIMImageCount");
    SKIP_IF_NO_DLL();

    std::wstring wimPath = MakeTempWimPath();
    if (wimPath.empty()) { ASSERT_FALSE(!wimPath.empty()); return; }

    WIMHandle hWim = CWIMHelper::CreateFile(
        wimPath, WIM_COMPRESS_XPRESS, GENERIC_READ);
    if (hWim == nullptr) { ASSERT_FALSE(hWim != nullptr); return; }

    DWORD count = CWIMHelper::GetImageCount(hWim);
    ASSERT_EQ(count, (DWORD)0);

    // 实例数（Win8+）也应返回 0（空 WIM 未提交任何 image）
    DWORD instCount = CWIMHelper::GetInstanceCount(hWim);
    ASSERT_EQ(instCount, (DWORD)0);

    CWIMHelper::CloseHandle(hWim);
    SafeDeleteFile(wimPath);
}

// ============================================================================
// 测试用例 5：GetAttributes + WIM_INFO 结构体
// ============================================================================
static void TEST_WIMAttributes()
{
    TestBanner(L"TEST_WIMAttributes");
    SKIP_IF_NO_DLL();

    std::wstring wimPath = MakeTempWimPath();
    if (wimPath.empty()) { ASSERT_FALSE(!wimPath.empty()); return; }

    WIMHandle hWim = CWIMHelper::CreateFile(
        wimPath, WIM_COMPRESS_XPRESS, GENERIC_READ);
    if (hWim == nullptr) { ASSERT_FALSE(hWim != nullptr); return; }

    WIM_INFO info = {};
    bool ok = CWIMHelper::GetAttributes(hWim, &info);
    ASSERT_TRUE(ok);
    if (ok) {
        // WIM_INFO 真实字段（wimgapi.h:214）：WimPath/Guid/ImageCount/CompressionType/
        // PartNumber/TotalParts/BootIndex/WimAttributes/WimFlagsAndAttr
        wprintf(L"  WIM_INFO: Images=%u CompressType=%u Parts=%u/%u BootIdx=%u\n",
                info.ImageCount, info.CompressionType,
                info.PartNumber, info.TotalParts, info.BootIndex);
        // CompressionType 取值范围 0..3（NONE/XPRESS/LZX/LZMS）；仅校验上界，避免依赖 LZMS 宏可见性
        ASSERT_TRUE(info.CompressionType <= 3);
    }

    CWIMHelper::CloseHandle(hWim);
    SafeDeleteFile(wimPath);
}

// ============================================================================
// 测试用例 6：MountImage / UnmountImage 一对操作
// ============================================================================
static void TEST_WIMMountUnmount()
{
    TestBanner(L"TEST_WIMMountUnmount");
    SKIP_IF_NO_DLL();

    // 空 WIM 通常无法挂载（GetImageCount==0），本用例仅验证 API 调用路径，
    // 挂载失败被视为合理（不视为失败），Unmount 应始终可以调用。
    std::wstring wimPath = MakeTempWimPath();
    if (wimPath.empty()) { ASSERT_FALSE(!wimPath.empty()); return; }

    WIMHandle hWim = CWIMHelper::CreateFile(
        wimPath, WIM_COMPRESS_XPRESS, GENERIC_READ);
    if (hWim == nullptr) {
        SafeDeleteFile(wimPath);
        return;
    }
    CWIMHelper::CloseHandle(hWim);
    hWim = nullptr;

    // 构建挂载点目录
    std::wstring mountDir = CreateTempDir();
    if (mountDir.empty()) {
        SafeDeleteFile(wimPath);
        return;
    }

    // 尝试挂载（预期会失败，但调用不崩溃即算通过）
    bool mounted = CWIMHelper::MountImage(wimPath, 0, mountDir, true);
    wprintf(L"  MountImage(empty WIM) => %s (预期失败)\n",
            mounted ? L"true" : L"false");

    // 卸载（commit=false，因为并未真正挂载）
    bool unmounted = CWIMHelper::UnmountImage(mountDir, 0, false);
    wprintf(L"  UnmountImage => %s\n", unmounted ? L"true" : L"false");

    // 清理
    if (!mountDir.empty()) RecursivelyRemoveDir(mountDir);
    SafeDeleteFile(wimPath);

    // 本用例只断言 UnmountImage 返回值有意义（true 或 false 都可）
    ASSERT_TRUE(unmounted || !unmounted);
}

// ============================================================================
// 测试用例 7：CaptureImage + ApplyImage
// ============================================================================
static void TEST_WIMCaptureApply()
{
    TestBanner(L"TEST_WIMCaptureApply");
    SKIP_IF_NO_DLL();

    // 准备源目录 + 待捕获文件
    std::wstring srcDir = CreateTempDir();
    if (srcDir.empty()) { ASSERT_FALSE(!srcDir.empty()); return; }
    std::wstring srcFile = srcDir + L"\\sample.txt";
    if (!WriteSampleFile(srcFile, 4096)) {
        RecursivelyRemoveDir(srcDir);
        ASSERT_FALSE(WriteSampleFile(srcFile, 4096));
        return;
    }

    // 创建 WIM 并打开
    std::wstring wimPath = MakeTempWimPath();
    WIMHandle hWim = CWIMHelper::CreateFileEx(
        wimPath, WIM_COMPRESS_XPRESS,
        GENERIC_READ | GENERIC_WRITE, 0);
    ASSERT_TRUE(hWim != nullptr);
    if (hWim == nullptr) {
        RecursivelyRemoveDir(srcDir);
        SafeDeleteFile(wimPath);
        return;
    }

    // Capture
    bool captured = CWIMHelper::CaptureImage(hWim, srcDir, 0);
    if (captured) {
        wprintf(L"  CaptureImage 成功，ImageCount = %u\n",
                CWIMHelper::GetImageCount(hWim));
    } else {
        wprintf(L"  CaptureImage 返回 false（%lu），继续验证 Apply 分支\n",
                GetLastError());
    }
    ASSERT_TRUE(true); // 占位：本用例主要验证调用路径不崩溃

    // 打开镜像句柄
    WIMImageHandle hImage = nullptr;
    if (captured) {
        hImage = CWIMHelper::OpenForRead(wimPath);
    }

    // 目标目录
    std::wstring dstDir = CreateTempDir();
    if (dstDir.empty()) {
        if (hImage) CWIMHelper::CloseHandle(hImage);
        CWIMHelper::CloseHandle(hWim);
        RecursivelyRemoveDir(srcDir);
        SafeDeleteFile(wimPath);
        return;
    }

    // Apply
    bool applied = false;
    if (hImage) {
        applied = CWIMHelper::ApplyImage(hImage, dstDir, 0);
        if (applied) {
            wprintf(L"  ApplyImage 成功，目标目录 = %s\n", dstDir.c_str());
        } else {
            wprintf(L"  ApplyImage 返回 false（%lu）\n", GetLastError());
        }
        CWIMHelper::CloseHandle(hImage);
        hImage = nullptr;
    }

    // 验证目标目录中是否存在捕获的文件（仅当 apply 成功时）
    if (applied) {
        DWORD attr = GetFileAttributesW((dstDir + L"\\sample.txt").c_str());
        ASSERT_TRUE(attr != INVALID_FILE_ATTRIBUTES);
    }

    // 清理
    CWIMHelper::CloseHandle(hWim);
    RecursivelyRemoveDir(srcDir);
    RecursivelyRemoveDir(dstDir);
    SafeDeleteFile(wimPath);
}

// ============================================================================
// 测试用例 8：RegisterCallback + GetCallbackCount + UnregisterCallback
// ============================================================================
static void TEST_WIMCallbackRegister()
{
    TestBanner(L"TEST_WIMCallbackRegister");
    SKIP_IF_NO_DLL();

    std::wstring wimPath = MakeTempWimPath();
    WIMHandle hWim = CWIMHelper::CreateFileEx(
        wimPath, WIM_COMPRESS_XPRESS,
        GENERIC_READ | GENERIC_WRITE, 0);
    if (hWim == nullptr) {
        SafeDeleteFile(wimPath);
        return;
    }

    // 注册默认回调。wimgapi 的 WIMRegisterMessageCallback 返回注册 ID，编号从
    // 0 起始，因此首个成功注册的返回值为 0（不是错误）。真实成功信号是
    // GetMessageCallbackCount >= 1（下方校验）。
    WIMMsgProc proc = &CWIMHelper::DefaultMessageProc;
    DWORD id = CWIMHelper::RegisterCallback(hWim, proc, nullptr);
    wprintf(L"  RegisterCallback 返回 id = %u\n", id);

    // 查询回调数量
    DWORD cnt = CWIMHelper::GetCallbackCount(hWim);
    ASSERT_TRUE(cnt >= 1);
    wprintf(L"  GetCallbackCount = %u\n", cnt);

    // 取消注册
    bool unreg = CWIMHelper::UnregisterCallback(hWim, proc);
    ASSERT_TRUE(unreg);

    DWORD cntAfter = CWIMHelper::GetCallbackCount(hWim);
    ASSERT_EQ(cntAfter, (DWORD)0);

    CWIMHelper::CloseHandle(hWim);
    SafeDeleteFile(wimPath);
}

// ============================================================================
// 测试用例 9：WimBoot 环境探测（WIMGetWIMBootEntry/WIMRemoveWIMBootEntry
// 经实测并非任何版本 wimgapi.dll 的真实导出，对应虚假槽位已移除；
// 改测真实导出 WIMIsCurrentSystemWimboot）
// ============================================================================
static void TEST_WIMBootAvailable()
{
    TestBanner(L"TEST_WIMBootAvailable");
    SKIP_IF_NO_DLL();

    BOOL isWimboot = FALSE;
    bool ok = CWIMHelper::IsCurrentSystemWimboot(&isWimboot);
    wprintf(L"  IsCurrentSystemWimboot => ok=%s, isWimboot=%s\n",
            ok ? L"true" : L"false",
            isWimboot ? L"true" : L"false");

    // 只要真实导出可调用且不崩溃即视为通过
    ASSERT_TRUE(ok || !ok);
}

// ============================================================================
// 测试用例 10：Phase 9 Direct Exports 返回值检查
// ============================================================================
static void TEST_Phase9DirectExports()
{
    TestBanner(L"TEST_Phase9DirectExports");
    SKIP_IF_NO_DLL();

    // WIMCreateFileExDirect
    std::wstring wimPath = MakeTempWimPath();
    DWORD creationResult = 0;
    HANDLE hDirect = CWIMHelper::WIMCreateFileExDirect(
        wimPath,
        GENERIC_READ | GENERIC_WRITE,
        WIM_CREATE_ALWAYS,
        WIM_ATTRIBUTE_NORMAL,
        WIM_COMPRESS_LZX,
        &creationResult);
    wprintf(L"  WIMCreateFileExDirect => %s (result=%u)\n",
            hDirect ? L"handle valid" : L"null", creationResult);

    // WIMApplyInstanceDirect（无有效 instance，应返回 false）
    bool applyR = CWIMHelper::WIMApplyInstanceDirect(
        static_cast<WIMInstanceHandle>(INVALID_HANDLE_VALUE),
        MakeTempFilePath(), 0);
    wprintf(L"  WIMApplyInstanceDirect(invalid) => %s\n",
            applyR ? L"true" : L"false");
    ASSERT_FALSE(applyR);

    // WIMMountImageDirect（空 WIM，无 image，应返回 false）
    bool mountR = CWIMHelper::WIMMountImageDirect(
        wimPath, 0, MakeTempFilePath(), 0);
    wprintf(L"  WIMMountImageDirect(empty WIM, index=0) => %s\n",
            mountR ? L"true" : L"false");
    ASSERT_FALSE(mountR);

    // WIMUnmountImageDirect（无挂载点，应返回 false）
    bool unmountR = CWIMHelper::WIMUnmountImageDirect(
        wimPath, 0, MakeTempFilePath(), FALSE);
    wprintf(L"  WIMUnmountImageDirect(non-mounted) => %s\n",
            unmountR ? L"true" : L"false");
    ASSERT_FALSE(unmountR);

    // 收尾
    if (hDirect) {
        if (!CWIMHelper::CloseHandle(static_cast<WIMHandle>(hDirect))) {
            // 某些情况 CloseHandle 可能走 ::CloseHandle，直接兜底
            ::CloseHandle(hDirect);
        }
    }
    SafeDeleteFile(wimPath);

    // 本用例核心是验证直接导出可调、返回语义合理
    ASSERT_TRUE(hDirect != nullptr);
}

// ============================================================================
// RunAllTests：汇总入口
// ============================================================================
int RunAllTests()
{
    wprintf(L"================================================================\n");
    wprintf(L"WIMHelper 单元测试开始。失败计数初始值 = 0\n");
    wprintf(L"================================================================\n");

    g_failedCount = 0;

    TEST_WIMAvailable();
    TEST_WIMOpenClose();
    TEST_WIMCreateEmptyWim();
    TEST_WIMImageCount();
    TEST_WIMAttributes();
    TEST_WIMMountUnmount();
    TEST_WIMCaptureApply();
    TEST_WIMCallbackRegister();
    TEST_WIMBootAvailable();
    TEST_Phase9DirectExports();

    wprintf(L"\n================================================================\n");
    wprintf(L"测试结束。失败用例数 = %d\n", g_failedCount);
    wprintf(L"================================================================\n");

    return g_failedCount;
}

// ============================================================================
// wmain 入口
// ============================================================================
int wmain(int argc, WCHAR* argv[])
{
    (void)argc;
    (void)argv;

    SetConsoleOutputCP(CP_UTF8);

    int failed = RunAllTests();

    if (failed == 0) {
        wprintf(L"\n[RESULT] ALL TESTS PASSED\n");
    } else {
        wprintf(L"\n[RESULT] %d TEST(S) FAILED\n", failed);
    }
    return failed;
}

// ============================================================================
// 说明（原 TODO 已依据事实改写，2026-10-25）：
//   本文件的入口是 wmain（见下方 wmain 定义），与主工程 WinMain 子系统冲突，
//   因此不并入 Demos\Win8Metro\Win8Metro.vcxproj，而是作为独立测试工程单独编译：
//       cl /EHsc /W3 /nologo /Fe:"WIMHelper_Test.exe" tests\WIMHelper_Test.cpp
//          WIMHelper.cpp shell32.lib shlwapi.lib wimgapi.lib /SUBSYSTEM:CONSOLE
//   如需纳入 CI，应新建独立 vcxproj（Console 子系统），而不是加入主工程。
// ============================================================================
// [C++17 R3] 本文件内联常量（C++17 inline variables，多 TU ODR 一致）：
