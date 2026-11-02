// UIShadow 生命周期崩溃验证（按真实所有权路径）
//
// 真实结构：CShadowUI 是 CPaintManagerUI 的成员（GetShadow() 返回 &m_shadow），
// 生命周期跟随管理器，而不是某个局部作用域。
//
// 崩溃路径：
//   1) CShadowUI::Create() 用 SetWindowLongPtr 子类化父窗口 WndProc -> ParentProc
//   2) WM_NCDESTROY 分支执行 GetShadowMap().erase(hwnd)，把 (hwnd -> this) 删掉
//   3) 子类化并未解除，窗口后续任何消息仍进入 ParentProc
//   4) 修复前：map[hwnd] 会**默认插入 nullptr**，紧接着 pThis->IsDisableShadow()
//      解引用 -> ASan: stack/heap-use-after-... / access violation
//   修复后：受控查找，缺失即回退 DefWindowProc，并解除子类化。
#include "StdAfx.h"
#include "UIlib.h"
#include <cstdio>
#include "Utils/UIShadow.h"
#include "Core/UIManager.h"

static LRESULT CALLBACK TestProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    return ::DefWindowProc(h, m, w, l);
}

int main()
{
    using namespace DuiLib;

    HINSTANCE hInst = ::GetModuleHandle(nullptr);
    WNDCLASSEX wc = { 0 };
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = TestProc;
    wc.hInstance = hInst;
    wc.lpszClassName = _T("DuiLibShadowLifeProbe");
    if (!::RegisterClassEx(&wc)) {
        printf("[probe] RegisterClassEx failed\n");
        return 2;
    }
    HWND hwnd = ::CreateWindowEx(0, wc.lpszClassName, _T("probe"),
                                 WS_OVERLAPPEDWINDOW, 0, 0, 320, 240,
                                 nullptr, nullptr, hInst, nullptr);
    if (!hwnd) { printf("[probe] CreateWindowEx failed\n"); return 2; }
    printf("[probe] window created\n");
    fflush(stdout);

    CPaintManagerUI pm;
    pm.Init(hwnd);

    CShadowUI* pShadow = pm.GetShadow();
    pShadow->ShowShadow(true);
    pShadow->Create(&pm);
    printf("[probe] shadow attached via manager (real ownership)\n");
    fflush(stdout);

    ::SendMessage(hwnd, WM_ACTIVATEAPP, TRUE, 0);
    printf("[probe] normal message with shadow present: ok\n");
    fflush(stdout);

    printf("[probe] WM_NCDESTROY -> map entry erased, subclass still installed\n");
    fflush(stdout);
    ::SendMessage(hwnd, WM_NCDESTROY, 0, 0);

    printf("[probe] post-destroy message (this is the crash path)\n");
    fflush(stdout);
    ::SendMessage(hwnd, WM_ACTIVATEAPP, TRUE, 0);
    printf("[probe] survived post-destroy message\n");

    ::DestroyWindow(hwnd);
    printf("[probe] OK: no dangling/null dereference\n");
    return 0;
}
