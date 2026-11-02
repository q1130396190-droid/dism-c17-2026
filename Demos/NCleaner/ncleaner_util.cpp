// ============================================================================
//  ncleaner_util.cpp —— 共用工具实现
// ============================================================================

#include "ncleaner_util.h"

#include "../Win8Metro/WinHandles.h"   // [C++17 R3] win::HandlePtr RAII for F11_raw_handle_close
#include <shlwapi.h>
#include <cstdarg>
// [C++17 阶段2] 局部 RAII 守卫：FindFirstFile 句柄（见 WinHandles.h 使用规范）
struct NCFindGuard
{
    HANDLE h;
    explicit NCFindGuard(HANDLE x) : h(x) {}
    ~NCFindGuard() { if (h && h != INVALID_HANDLE_VALUE) ::FindClose(h); }
    NCFindGuard(const NCFindGuard&) = delete;
    NCFindGuard& operator=(const NCFindGuard&) = delete;
    operator HANDLE() const { return h; }
};


#pragma comment(lib, "shlwapi.lib")

namespace NCleanerUtil
{

namespace
{
    // [C-05] 宿主层注册的日志通道；nullptr 时退化为调试输出。
    LogSinkFn g_pfnLogSink = nullptr;

    // [BUG-15] 清只读后删空目录；成功返回 true（目录删除共用原语）。
    bool RemoveDirForce(const wchar_t* pszDir)
    {
        DWORD dwAttr = ::GetFileAttributesW(pszDir);
        if (dwAttr == INVALID_FILE_ATTRIBUTES) return false;
        if (dwAttr & FILE_ATTRIBUTE_READONLY) {
            ::SetFileAttributesW(pszDir, dwAttr & ~FILE_ATTRIBUTE_READONLY);
        }
        return ::RemoveDirectoryW(pszDir) != FALSE;
    }
}

void SetLogSink(LogSinkFn pfnSink)
{
    g_pfnLogSink = pfnSink;
}

// ---- CMsiGuard -------------------------------------------------------------

CMsiGuard::CMsiGuard()
    : m_bBusy(false)
{
    // [BUG-19] 契约：只检测、不持有。检测与后续清理之间天然存在 TOCTOU
    // 窗口（原版亦然），本守卫仅用于跳过"明显有 MSI 事务在跑"的场景。
    //
    // 单次 CreateMutex + 一次零等待探测，不长期保留句柄：
    //   新建成功（非 ALREADY_EXISTS）—— 无人持有，空闲；
    //   ERROR_ALREADY_EXISTS         —— 对象存在，wait 0 判占用；
    //   ACCESS_DENIED                —— 权限不足无法判定，保守视为忙碌。
    const wchar_t* kMutex = L"Global\\_MSIExecute";

    // [C++17 R3] F11: RAII handle, auto CloseHandle on every exit path
    win::HandlePtr h(::CreateMutexW(nullptr, FALSE, kMutex));
    if (!h) {
        m_bBusy = (::GetLastError() == ERROR_ACCESS_DENIED);
        return;
    }

    DWORD dwWait = ::WaitForSingleObject(h.get(), 0);
    if (dwWait == WAIT_TIMEOUT) {
        m_bBusy = true;                 // 被持有 → MSI 事务进行中
    } else if (dwWait == WAIT_OBJECT_0
               || dwWait == WAIT_ABANDONED_0) {
        ::ReleaseMutex(h.get());        // 取得（含前持有者异常退出的遗弃态）→ 归还
    }
}

CMsiGuard::~CMsiGuard()
{
    // [BUG-19] 句柄已在构造期探测后立即关闭，析构无需释放。
}

// ---- 递归删除 --------------------------------------------------------------

bool DeleteFileForce(const wchar_t* pszFile)
{
    if (pszFile == nullptr || *pszFile == L'\0') return false;

    DWORD dwAttr = ::GetFileAttributesW(pszFile);
    if (dwAttr == INVALID_FILE_ATTRIBUTES) {
        // 不存在视为成功（幂等）
        return true;
    }

    if (dwAttr & FILE_ATTRIBUTE_READONLY) {
        ::SetFileAttributesW(pszFile, dwAttr & ~FILE_ATTRIBUTE_READONLY);
    }

    if (::DeleteFileW(pszFile)) return true;

    DWORD dwErr = ::GetLastError();

    // [BUG-14] 共享锁/占用类错误（Package Cache 下常见于正在修复/更新的应用）：
    // 登记重启删除可能破坏重启后的安装事务，跳过并记日志，由本次清理放弃该文件。
    if (dwErr == ERROR_SHARING_VIOLATION || dwErr == ERROR_LOCK_VIOLATION) {
        Log(L"NCDeleteFile(%s) 文件占用中，跳过 ==> 0x%08lX",
            pszFile, (unsigned long)dwErr);
        return false;
    }

    // 仅对权限/只读类错误登记重启后删除（等价原版 NCDeleteFile 兜底语义，
    // 但范围收窄）；目标参数为 nullptr 时 REPLACE_EXISTING 无意义，去掉该标志。
    if (dwErr == ERROR_ACCESS_DENIED) {
        if (::MoveFileExW(pszFile, nullptr, MOVEFILE_DELAY_UNTIL_REBOOT)) {
            return true;
        }
    }

    Log(L"NCDeleteFile(%s) ==> 0x%08lX", pszFile, (unsigned long)dwErr);
    return false;
}

unsigned long DeleteTree(const wchar_t* pszDir, bool bDeleteRoot)
{
    if (pszDir == nullptr || *pszDir == L'\0') return 0;

    unsigned long nDeleted = 0;

    // [BUG-15] 显式栈迭代替代无界递归：遍历深度只占用堆（vector），
    // 畸形深树不再因每层一个递归帧而耗尽线程栈。
    //
    // bRemove=true 的帧在其子项全部处理完后才删自身（后序）：
    // 入栈顺序为「自身后序帧 → 各子目录前序帧」，LIFO 保证子目录先处理。
    struct TreeFrame
    {
        std::wstring sPath;
        bool bRemove;   // 子项处理完后是否删除该目录
    };

    std::vector<TreeFrame> vStack;
    TreeFrame rootFrame;
    rootFrame.sPath  = pszDir;
    rootFrame.bRemove = bDeleteRoot;
    vStack.push_back(rootFrame);

    while (!vStack.empty()) {
        TreeFrame frame = vStack.back();
        vStack.pop_back();

        std::wstring sPattern(frame.sPath);
        if (!sPattern.empty() && sPattern[sPattern.size() - 1] != L'\\') {
            sPattern += L'\\';
        }
        sPattern += L'*';

        WIN32_FIND_DATAW fd;
        NCFindGuard hFind(::FindFirstFileW(sPattern.c_str(), &fd));
        if (hFind == INVALID_HANDLE_VALUE) {
            // 枚举失败：仍按后序尝试删目录（非空/无权限时 RemoveDirectory 自然失败）
            if (frame.bRemove && RemoveDirForce(frame.sPath.c_str())) ++nDeleted;
            continue;
        }

        // 后序帧先入栈，子目录帧随后入栈（先于后序帧被弹出处理）
        if (frame.bRemove) {
            TreeFrame postFrame;
            postFrame.sPath  = frame.sPath;
            postFrame.bRemove = true;
            vStack.push_back(postFrame);
        }

        do {
            if (lstrcmpW(fd.cFileName, L".") == 0
                || lstrcmpW(fd.cFileName, L"..") == 0) {
                continue;
            }

            std::wstring sChild(frame.sPath);
            if (!sChild.empty() && sChild[sChild.size() - 1] != L'\\') {
                sChild += L'\\';
            }
            sChild += fd.cFileName;

            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                // 跳过重解析点（junction/symlink）：只删链接本身，不深入目标
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                    if (RemoveDirForce(sChild.c_str())) ++nDeleted;
                    continue;
                }
                TreeFrame subFrame;
                subFrame.sPath  = sChild;
                subFrame.bRemove = true;   // 子目录等价原递归 bDeleteRoot=true
                vStack.push_back(subFrame);
            } else {
                if (DeleteFileForce(sChild.c_str())) ++nDeleted;
            }
        } while (::FindNextFileW(hFind, &fd));
        // NCFindGuard 析构自动关闭
    }

    return nDeleted;
}

// ---- 匹配 ------------------------------------------------------------------

bool NameMatchesAny(const wchar_t* pszName, const wchar_t* const* ppFilters, int nCount)
{
    if (pszName == nullptr || ppFilters == nullptr) return false;
    for (int i = 0; i < nCount; ++i) {
        const wchar_t* pFilter = ppFilters[i];
        if (pFilter == nullptr || *pFilter == L'\0') continue;
        if (::StrStrIW(pszName, pFilter) != nullptr) return true;
    }
    return false;
}

// ---- 路径 ------------------------------------------------------------------

bool ExpandPath(const wchar_t* pszIn, std::wstring& sOut)
{
    sOut.clear();
    if (pszIn == nullptr || *pszIn == L'\0') return false;

    wchar_t szBuf[MAX_PATH * 2] = { 0 };
    DWORD dw = ::ExpandEnvironmentStringsW(pszIn, szBuf, _countof(szBuf));
    if (dw == 0 || dw > _countof(szBuf)) return false;

    sOut = szBuf;
    return true;
}

bool GetSystemDriveRoot(std::wstring& sOut)
{
    wchar_t szWin[MAX_PATH] = { 0 };
    if (::GetWindowsDirectoryW(szWin, _countof(szWin)) == 0) return false;

    sOut.clear();
    sOut += szWin[0];
    sOut += L":\\";
    return true;
}

bool GetSystemRoot(std::wstring& sOut)
{
    wchar_t szWin[MAX_PATH] = { 0 };
    if (::GetWindowsDirectoryW(szWin, _countof(szWin)) == 0) return false;
    sOut = szWin;
    return true;
}

// ---- 安全加载（DLL 抗劫持）-------------------------------------------------

HMODULE SafeLoadSystem32Dll(const wchar_t* pszName)
{
    if (pszName == nullptr || *pszName == L'\0') return nullptr;

    // 若调用方已给出完整路径，直接按 System32 限定加载
    if (::wcsstr(pszName, L"\\") != nullptr) {
        return ::LoadLibraryExW(pszName, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    }

    // 构造 %SystemRoot%\System32\<name> 完整路径
    wchar_t szSys[MAX_PATH] = { 0 };
    UINT cch = ::GetSystemDirectoryW(szSys, _countof(szSys));
    if (cch == 0 || cch >= _countof(szSys)) return nullptr;

    std::wstring sFull(szSys);
    if (!sFull.empty() && sFull[sFull.size() - 1] != L'\\') sFull += L'\\';
    sFull += pszName;

    // 只从 System32 解析；不做裸名回退（裸名会先命中应用目录 = 劫持入口）
    HMODULE h = ::LoadLibraryExW(sFull.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (h != nullptr) return h;

    // 极老系统无 LOAD_LIBRARY_SEARCH_SYSTEM32 语义时，退化为显式完整路径加载
    // （仍是绝对路径，不经过搜索顺序，无劫持面）
    return ::LoadLibraryExW(sFull.c_str(), nullptr, 0);
}

// ---- 日志 ------------------------------------------------------------------

void Log(const wchar_t* pFormat, ...)
{
    if (pFormat == nullptr) return;

    std::wstring sLog;
    sLog.resize(1024);
    va_list ap;
    va_start(ap, pFormat);
    _vsnwprintf_s(&sLog[0], sLog.size(), _TRUNCATE, pFormat, ap);
    va_end(ap);
    sLog.resize(wcslen(sLog.c_str()));

    // [C-05] 经注册的 sink 输出；sink 未注册（宿主层尚未惰性初始化）时
    // 退化为调试输出，util 层不再知道宿主层的存在。
    if (g_pfnLogSink != nullptr) {
        g_pfnLogSink(0, sLog.c_str());
    } else {
        ::OutputDebugStringW(sLog.c_str());
        ::OutputDebugStringW(L"\n");
    }
}

}  // namespace NCleanerUtil
// [C++17 R3] 本文件内联常量（C++17 inline variables，多 TU ODR 一致）：
inline constexpr int kCpp17_ncleaner_util_StepTimeoutMs = 120000;
inline constexpr int kCpp17_ncleaner_util_MaxRetry = 3;
