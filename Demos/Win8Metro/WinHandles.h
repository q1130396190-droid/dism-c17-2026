#ifndef __DUI_WINHANDLES_H__
#define __DUI_WINHANDLES_H__

// ============================================================================
// WinHandles.h —— Win32 句柄/资源 RAII 包装（C++17 升级阶段2）
//
// 基于 std::unique_ptr 定制删除器；删除器全部 noexcept。
// 用法：
//   win::HandlePtr  hFile(::CreateFileW(...));   // 自动 CloseHandle
//   win::RegKey     hKey;  RegOpenKeyExW(..., win::handle_out(hKey));
//   win::FindDataHandle hFind(::FindFirstFileW(..., &fd)); // 自动 FindClose
//
// 跨函数/跨线程移交所有权：显式调用 release()，并注释所有权去向。
// 注意：RAII 只负责自动释放本身，不改变任何调用顺序与业务分支。
// ============================================================================

#include <windows.h>
// [R215] CoTaskMemFree is declared in <combaseapi.h>, which <windows.h> does NOT
// pull in when WIN32_LEAN_AND_MEAN is defined. Without this include the call at
// line ~72 below fails with C2039 'not a member of global namespace' -- and only
// in translation units that build with WIN32_LEAN_AND_MEAN, so it surfaced in some
// probes and not others. A header must be self-contained.
#include <combaseapi.h>
#include <memory>
#include <utility>

namespace win
{

// ---------- HANDLE / CloseHandle ----------
struct HandleCloser { void operator()(HANDLE h) noexcept { if (h && h != INVALID_HANDLE_VALUE) ::CloseHandle(h); } };
using HandlePtr = std::unique_ptr<void, HandleCloser>;

// 判空辅助：CreateFile 失败返回 INVALID_HANDLE_VALUE
inline bool valid(const HandlePtr& h) { return h != nullptr && h.get() != INVALID_HANDLE_VALUE; }
// CreateFile 类返回值专用（INVALID_HANDLE_VALUE 视为空）
inline HandlePtr file_handle(HANDLE h) { return HandlePtr(h == INVALID_HANDLE_VALUE ? nullptr : h); }

// 供 Win32 出参场景取句柄地址：RegOpenKeyExW(..., &hKey) → &hKey.get()
// 用法改为：RegOpenKeyExW(..., win::handle_out(hKey))
struct HandleOut
{
    explicit HandleOut(HandlePtr& h) : m_h(h) {}
    operator PHANDLE() { return reinterpret_cast<PHANDLE>(m_h.get()); } // NOLINT
private:
    HandlePtr& m_h;
};

// ---------- HKEY / RegCloseKey ----------
struct RegKeyCloser { void operator()(HKEY k) noexcept { if (k) ::RegCloseKey(k); } };
struct RegKey
{
    RegKey() = default;
    ~RegKey() { reset(); }   // [H-51] RAII：析构自动 RegCloseKey（原 =default 会导致句柄泄漏）
    explicit operator bool() const { return m_k != nullptr; }
    HKEY get() const { return m_k; }
    HKEY* out() { return &m_k; }          // RegOpenKeyExW(..., &key.out())
    HKEY release() { HKEY k = m_k; m_k = nullptr; return k; }
    void reset(HKEY k = nullptr) { if (m_k) ::RegCloseKey(m_k); m_k = k; }
private:
    HKEY m_k = nullptr;
};

// ---------- FindFirstFile / FindClose ----------
struct FindCloser { void operator()(HANDLE h) noexcept { if (h && h != INVALID_HANDLE_VALUE) ::FindClose(h); } };
using FindDataHandle = std::unique_ptr<void, FindCloser>;
inline FindDataHandle find_handle(HANDLE h) { return FindDataHandle(h == INVALID_HANDLE_VALUE ? nullptr : h); }

// ---------- GDI 对象 ----------
struct GdiCloser { void operator()(HGDIOBJ g) noexcept { if (g) ::DeleteObject(g); } };
using GdiObject = std::unique_ptr<void, GdiCloser>;
struct IconCloser { void operator()(HICON i) noexcept { if (i) ::DestroyIcon(i); } };
using IconPtr = std::unique_ptr<HICON__, IconCloser>;

// ---------- 内存块 ----------
struct VirtualFreeDeleter { void operator()(void* p) noexcept { if (p) ::VirtualFree(p, 0, MEM_RELEASE); } };
using MemBlock = std::unique_ptr<void, VirtualFreeDeleter>;
struct CoTaskMemDeleter { void operator()(void* p) noexcept { if (p) ::CoTaskMemFree(p); } };
using CoMemBlock = std::unique_ptr<void, CoTaskMemDeleter>;
struct LocalFreeDeleter { void operator()(HLOCAL p) noexcept { if (p) ::LocalFree(p); } };
using LocalBlock = std::unique_ptr<void, LocalFreeDeleter>;

} // namespace win

// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：

#endif // __DUI_WINHANDLES_H__
