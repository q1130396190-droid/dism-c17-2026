// ============================================================
// MsgParam.h —— 消息参数与指针互转的唯一入口
//
// 为什么需要这个头：
//   页面层有 14 处 `reinterpret_cast<LPARAM>(p)` / `reinterpret_cast<T*>(lParam)`
//   用于"工作线程 new 一个消息结构体，PostMessage 把指针塞进 lParam，
//   UI 线程再取回来 delete"这一既有约定（EPageMsgOwn::kMsgOwnPage）。
//   这些转换散落在各页，写法不一，且没有一处校验指针宽度——
//   在 LLP64（x64）下 LPARAM 与指针同为 64 位，换成 32 位宿主时会静默截断。
//
//   集中在两个函数里：语义有名字、宽度有 static_assert 兜底、
//   将来若改消息契约只需改一处。
//
// 刻意不覆盖的范围（保持原样，避免"封装掩盖语义"）：
//   * HANDLE / HWND / HFONT 与整数互转 —— Win32 句柄语义，不是消息参数
//   * char* / const char* 字节缓冲 —— 类型擦除的 API 出参
//   * INT_PTR(h) —— ShellExecute 返回值判成功，属 API 契约
// ============================================================
#pragma once

#include <windows.h>
#include <cstdint>

namespace msgparam
{
// 指针宽度必须与消息参数一致，否则投递/取回会截断。
// LLP64 下 LPARAM/WPARAM 均为 64 位；若将来移植到 32 位，本断言会立即报错，
// 而不是在运行时静默丢高位。
static_assert(sizeof(void*) == sizeof(LPARAM),
              "LPARAM must be pointer-sized (message marshalling contract)");
static_assert(sizeof(void*) == sizeof(WPARAM),
              "WPARAM must be pointer-sized (message marshalling contract)");

// 指针 → LPARAM（投递方向）
template <typename T>
[[nodiscard]] inline LPARAM ToParam(T* p) noexcept
{
    return reinterpret_cast<LPARAM>(p);
}

// 指针 → WPARAM（投递方向）
template <typename T>
[[nodiscard]] inline WPARAM ToWParam(T* p) noexcept
{
    return reinterpret_cast<WPARAM>(p);
}

// LPARAM → 指针（接收方向）。调用方按自己投递时的真实类型显式实例化。
template <typename T>
[[nodiscard]] inline T* FromParam(LPARAM v) noexcept
{
    return reinterpret_cast<T*>(v);
}

// WPARAM → 指针（接收方向）
template <typename T>
[[nodiscard]] inline T* FromWParam(WPARAM v) noexcept
{
    return reinterpret_cast<T*>(v);
}

// void* → 目标句柄/指针类型（线程参数接收方向）。
// 线程过程签名固定为 DWORD WINAPI(void*)，回调体内要把 lpParam 还原成
// HWND / 自定义 ctx 等真实类型。若各线程体各写一次 reinterpret_cast，
// 转换就散落在业务代码里；统一收在本函数内，业务侧只写
// msgparam::FromVoidParam<HWND>(lpParam)。
template <typename T>
[[nodiscard]] inline T FromVoidParam(void* p) noexcept
{
    return reinterpret_cast<T>(p);
}
}   // namespace msgparam
