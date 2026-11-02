#ifndef __UILIB_MASTER_H__
#define __UILIB_MASTER_H__
// [SEC-42] UIlib.h is the public entry header and had no include guard.
// Without one, re-entering it (e.g. stdafx.h includes a header that itself
// includes UIlib.h) re-expands the file mid-way, so base classes declared
// later in it are still unknown at the inner include point (C2504).

#ifdef UILIB_STATIC
#define UILIB_API 
#else
#if defined(UILIB_EXPORTS)
#	if defined(_MSC_VER)
#		define UILIB_API __declspec(dllexport)
#	else
#		define UILIB_API
#	endif
#else
#	if defined(_MSC_VER)
#		define UILIB_API __declspec(dllimport)
#	else
#		define UILIB_API 
#	endif
#endif
#endif
#define UILIB_COMDAT __declspec(selectany)

// ---------------------------------------------------------------------------
// S2/T2.4a: ABI 版本守卫
//
// 每当【导出类的布局】发生变更时必须递增 DUILIB_ABI_TAG，例如：
//   - 给任何 UILIB_API 类增删/重排数据成员
//   - 改变基类、增删/重排虚函数（改变 vtable 布局）
//   - 改变 TNotifyUI / DUI_MSGMAP_ENTRY / TITEM 等跨边界结构
//   - 改变 CDuiString 的内部表示（如改为 std::wstring）
//   - 升级平台工具集（v100 -> v143）
//
// 检查是【选择加入】的：只有消费方显式定义 DUILIB_EXPECT_ABI_TAG 才参与校验。
// 因此既有用户代码零改动、零新增报错；主动开启的团队可获得编译期保护。
// 详见 docs/abi-guard-design.md
// ---------------------------------------------------------------------------
#define DUILIB_VERSION_MAJOR  2
#define DUILIB_VERSION_MINOR  0
#define DUILIB_VERSION_PATCH  0

#define DUILIB_ABI_TAG        0x0202u

#ifdef DUILIB_EXPECT_ABI_TAG
// static_assert 是 C++ 关键字，无需头文件 —— 本处位于 <assert.h> 之前
static_assert(DUILIB_ABI_TAG == DUILIB_EXPECT_ABI_TAG,
    "DuiLib ABI version mismatch: recompile and relink. "
    "Do not mix old headers with a new DLL. "
    "Define no DUILIB_EXPECT_ABI_TAG to opt out of this check.");
#endif

#pragma warning(disable:4505)
#pragma warning(disable:4251)
#pragma warning(disable:4189)
#pragma warning(disable:4121)
#pragma warning(disable:4100)

// [X64-ONLY] DuiLib is built for x64 exclusively. The x86 and ia64 manifest
// branches were removed together with the Win32 configurations: a 32-bit build is
// now rejected at compile time instead of silently emitting a different manifest.
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='amd64' publicKeyToken='6595b64144ccf1df' language='*'\"")

#if !defined(_WIN64)
    #error "DuiLib is x64-only. Use Platform=x64 (VS 2026); Win32/x86 builds are unsupported."
#endif

// [NOMINMAX] 必须在 <windows.h> 之前定义。
//   否则 windows.h 会定义 min/max 函数式宏，本库内部所有 std::min/std::max
//   都会被展开成语法错误（实测 UIPageControl.h:79 报 C2589）。
//   放在公共头而不是各 .cpp：FeatureService.cpp / IntlService.cpp 这类
//   "不使用预编译头"的翻译单元直接 include UIlib.h，拿不到 StdAfx.h 里的定义。
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <stddef.h>
#include <richedit.h>
#include <tchar.h>
#include <assert.h>
#include <crtdbg.h>
#include <malloc.h>
#include <comdef.h>
#include <gdiplus.h>

#include "Utils/Utils.h"
#include "Utils/unzip.h"
#include "Utils/VersionHelpers.h"
#include "Core/UIMarkup.h"
#include "Utils/observer_impl_base.h"
#include "Utils/UIShadow.h"
#include "Utils/UIDelegate.h"
#include "Utils/DragDropImpl.h"
#include "Utils/TrayIcon.h"
#include "Utils/DPI.h"
#include "Utils/UICrashGuard.h"
#include "Utils/UIAutomation.h"

#include "Core/UIDefine.h"
#include "Core/UIResourceManager.h"
#include "Core/UIManager.h"
#include "Core/UIBase.h"
#include "Core/ControlFactory.h"
#include "Core/UIControl.h"
#include "Core/UIContainer.h"

#include "Core/UIDlgBuilder.h"
#include "Core/UIRender.h"
#include "Utils/WinImplBase.h"

#include "Layout/UIVerticalLayout.h"
#include "Layout/UIHorizontalLayout.h"
#include "Layout/UITileLayout.h"
#include "Layout/UITabLayout.h"
#include "Layout/UIChildLayout.h"

#include "Control/UIList.h"
#include "Control/UICombo.h"
#include "Control/UIScrollBar.h"
#include "Control/UITreeView.h"

#include "Control/UILabel.h"
#include "Control/UIText.h"
#include "Control/UIEdit.h"
#include "Control/UIGifAnim.h"
#include "Control/UIGifAnimEx.h"

#include "Control/UIAnimation.h"
#include "Layout/UIAnimationTabLayout.h"
#include "Control/UIButton.h"
#include "Control/UIOption.h"

#include "Control/UIProgress.h"
#include "Control/UISlider.h"

#include "Control/UIComboBox.h"
#include "Control/UIRichEdit.h"
#include "Control/UIDateTime.h"
#include "Control/UIIPAddress.h"
#include "Control/UIIPAddressEx.h"

#include "Control/UIActiveX.h"
#include "Control/UIWebBrowser.h"
#include "Control/UIFlash.h"

#include "Control/UIMenu.h"
#include "Control/UIGroupBox.h"
#include "Control/UIRollText.h"
#include "Control/UIColorPalette.h"
#include "Control/UIListEx.h"
#include "Control/UIHotKey.h"
#include "Control/UIFadeButton.h"
#include "Control/UIRing.h"
#include "Control/UILoading.h"
#include "Control/UIPageControl.h"

#pragma comment( lib, "comctl32.lib" )
#pragma comment( lib, "GdiPlus.lib" )
#pragma comment( lib, "Imm32.lib" )


// ---------------------------------------------------------------------------
// S2/T2.4a: 跨边界布局不变量
//
// 只断言【不变量】而非硬编码 sizeof —— 后者会因 Unicode/MBCS
// 构型不同而误报，反而促使团队直接删掉断言。
// [X64-ONLY] x64 已是唯一支持的架构，指针宽度不再作为可变构型。
// 整体布局指纹由 DUILIB_ABI_TAG 人工递增维护。
// ---------------------------------------------------------------------------
// [X64-ONLY] Pointer width is a fixed build invariant now. The compile-time
// guard above rejects Win32 before reaching this point, so the value is a
// constant rather than a per-architecture conditional.
#define DUI_PTR_SIZE 8

static_assert(sizeof(DWORD)  == 4, "unexpected DWORD size");
static_assert(sizeof(POINT)  == 8, "unexpected POINT layout");
static_assert(sizeof(RECT)   == 16, "unexpected RECT layout");
static_assert(sizeof(void*)  == DUI_PTR_SIZE, "unexpected pointer width");
static_assert(alignof(POINT) <= DUI_PTR_SIZE, "unexpected POINT alignment");
#endif // __UILIB_MASTER_H__
