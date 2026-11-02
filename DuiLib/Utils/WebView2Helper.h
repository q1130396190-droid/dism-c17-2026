#ifndef __WEBVIEW2HELPER_H__
#define __WEBVIEW2HELPER_H__

#pragma once

// WebView2 运行时动态加载助手
// 1. 通过 LoadLibrary("WebView2Loader.dll") + GetProcAddress 获取
//    CreateCoreWebView2EnvironmentWithOptions / GetAvailableCoreWebView2BrowserVersionString，
//    避免对 WebView2LoaderStatic.lib 的链接期依赖，从而保证：
//    - 低版本系统（Win7/8）编译出的 DuiLib.dll 无需携带任何 WebView2 相关依赖；
//    - 老编译器（VS2010 v100，_MSC_VER < 1900）即使定义了 DUILIB_USE_WEBVIEW2 也不会被编译进来。
// 2. WebView2Loader.dll 由 WebView2 Runtime（Evergreen）安装器提供，应用只需将 SDK 自带的
//    WebView2Loader.dll 部署到 exe 同目录即可（见 3rd/WebView2/lib/x64/）。
// 3. 函数参数使用 IUnknown* 代替强类型接口指针，仅作函数指针传递，避免本头文件依赖 WebView2.h。

#include <Windows.h>
#include <unknwn.h>
#include <wchar.h>
#include <mutex>   // std::once_flag for the loader cache	// wcsrchr / swprintf_s
#include "VersionHelpers.h"

namespace DuiLib
{
	namespace WebView2Helper
	{
		// 与 WebView2Loader.dll 导出一致的函数签名（COM 接口参数以 IUnknown* 承载）
		using PFN_CreateCoreWebView2EnvironmentWithOptions = HRESULT(__stdcall*)(
			LPCWSTR browserExecutableFolder,
			LPCWSTR userDataFolder,
			IUnknown* environmentOptions,
			IUnknown* environmentCreatedHandler);

		using PFN_GetAvailableCoreWebView2BrowserVersionString = HRESULT(__stdcall*)(
			LPCWSTR browserExecutableFolder,
			LPWSTR* versionInfo);

		// 加载 WebView2Loader.dll（进程内只加载一次）
		// LoadLibrary 默认仅在 exe 同目录/系统目录搜索，若未部署到 exe 目录会返回 nullptr。
		// 为兼容开发期与不同部署布局，依次尝试：
		//   1) 标准搜索（exe 同目录，正式部署场景）
		//   2) exe 目录下的 3rd/WebView2/lib/x64（开发期从仓库 bin 运行）
		//   3) exe 目录上一级仓库根下的 3rd/WebView2/lib/x64
		//   4) DuiLib.dll 模块目录（DLL 随程序集部署场景）
		// [X64-ONLY] 只查找 x64 子目录：本框架仅支持 x64，仓库不再维护 x86 Loader。
		// [WV2-LOADER-RACE] Load exactly once, thread-safely.
		//
		// The previous body cached a `static HMODULE` with an unsynchronised
		// check-then-act: every thread arriving before the first write could see
		// nullptr and call LoadLibraryW again. That is a data race (UB) and each extra
		// load bumps the module refcount with no matching FreeLibrary, so the DLL can
		// never unload.
		//
		// Measured: a 16-thread burst did NOT reproduce a redundant load on this
		// machine, because LoadLibraryW takes the loader lock and the window is tiny.
		// The race is still real at the language level, and the fix costs one call_once
		// on a path that runs at most once per process, so it is fixed rather than
		// documented away.
		//
		// std::call_once is used instead of a function-local static initialiser because
		// the search below has several fallbacks and must return a failure value too;
		// a static initialiser would need a helper and would still not express "tried
		// and failed, do not retry".
		struct LoaderCache
		{
			HMODULE h = nullptr;
			std::once_flag once;
		};

		inline LoaderCache& LoaderSlot()
		{
			// Construct-on-first-use; its own initialisation is guaranteed thread-safe.
			static LoaderCache s_slot;
			return s_slot;
		}

		inline HMODULE SearchLoader();

		inline HMODULE LoadWebView2Loader()
		{
			LoaderCache& slot = LoaderSlot();
			::std::call_once(slot.once, [&slot]() { slot.h = SearchLoader(); });
			return slot.h;
		}

		// The original search order, unchanged: standard search, then the three
		// development-layout fallbacks, then the DuiLib.dll directory.
		inline HMODULE SearchLoader()
		{
			HMODULE s_hModule = nullptr;

			// 1) 标准搜索（优先 exe 同目录）
			s_hModule = ::LoadLibraryW(L"WebView2Loader.dll");
			if (s_hModule != nullptr) return s_hModule;

			// 组装完整路径逐级兜底
			WCHAR szBase[MAX_PATH] = { 0 };
			WCHAR szPath[MAX_PATH] = { 0 };
			// [X64-ONLY] 架构后缀固定，不再按 _WIN64 分支选择 x86/x64 目录。
			const WCHAR* szArch = L"x64";

			if (::GetModuleFileNameW(nullptr, szBase, MAX_PATH) > 0) {
				WCHAR* pSlash = wcsrchr(szBase, L'\\');
				if (pSlash != nullptr) *pSlash = 0;

				// 2) exe 目录/3rd/WebView2/lib/<arch>/
				swprintf_s(szPath, MAX_PATH, L"%s\\3rd\\WebView2\\lib\\%s\\WebView2Loader.dll", szBase, szArch);
				s_hModule = ::LoadLibraryW(szPath);
				if (s_hModule != nullptr) return s_hModule;

				// 3) exe 目录上一级/3rd/WebView2/lib/<arch>/（仓库根目录布局）
				swprintf_s(szPath, MAX_PATH, L"%s\\..\\3rd\\WebView2\\lib\\%s\\WebView2Loader.dll", szBase, szArch);
				s_hModule = ::LoadLibraryW(szPath);
				if (s_hModule != nullptr) return s_hModule;

				// 2b) exe dir/WebView2Loader.dll (explicit full path, equivalent to standard search)
				swprintf_s(szPath, MAX_PATH, L"%s\\WebView2Loader.dll", szBase);
				s_hModule = ::LoadLibraryW(szPath);
				if (s_hModule != nullptr) return s_hModule;
			}

			// 4) DuiLib.dll 模块目录
			HMODULE hSelf = ::GetModuleHandleW(L"DuiLib.dll");
			if (hSelf != nullptr && ::GetModuleFileNameW(hSelf, szBase, MAX_PATH) > 0) {
				WCHAR* pSlash = wcsrchr(szBase, L'\\');
				if (pSlash != nullptr) *pSlash = 0;
				swprintf_s(szPath, MAX_PATH, L"%s\\WebView2Loader.dll", szBase);
				s_hModule = ::LoadLibraryW(szPath);
				if (s_hModule != nullptr) return s_hModule;
			}
			return s_hModule;	// 仍为 nullptr：WebView2Loader.dll 未部署（IsWebView2Supported 返回 false，自动回退 IE）
		}

		// 获取 CreateCoreWebView2EnvironmentWithOptions 函数指针
		inline PFN_CreateCoreWebView2EnvironmentWithOptions GetCreateEnvironmentFunc()
		{
			HMODULE hModule = LoadWebView2Loader();
			if (hModule == nullptr) return nullptr;
			return reinterpret_cast<PFN_CreateCoreWebView2EnvironmentWithOptions>(::GetProcAddress(hModule, "CreateCoreWebView2EnvironmentWithOptions"));
		}

		// 获取 GetAvailableCoreWebView2BrowserVersionString 函数指针
		inline PFN_GetAvailableCoreWebView2BrowserVersionString GetVersionFunc()
		{
			HMODULE hModule = LoadWebView2Loader();
			if (hModule == nullptr) return nullptr;
			return reinterpret_cast<PFN_GetAvailableCoreWebView2BrowserVersionString>(::GetProcAddress(hModule, "GetAvailableCoreWebView2BrowserVersionString"));
		}

		// [WV2-MINRUNTIME] Lowest WebView2 Runtime that can host a WebView2 at all.
		//
		// Microsoft documents this floor as "minimum version of Microsoft Edge or the
		// WebView2 Runtime to load WebView2: 86.0.616.0" and states it only moves when
		// a breaking change lands in the web platform. Keeping the constant here (and
		// in docs/WebView2-SDK-Registry.json) makes the installer precondition and the
		// runtime check agree instead of drifting apart.
		//
		// This is a FLOOR, not a recommendation: any SDK we compile against still runs
		// on an old Evergreen runtime because WebView2 is backward compatible. What an
		// old runtime cannot do is serve APIs introduced after it shipped.
		inline const wchar_t* MinimumRuntimeVersion()
		{
			return L"86.0.616.0";
		}

		using PFN_CompareBrowserVersions = HRESULT(__stdcall*)(
			LPCWSTR version1, LPCWSTR version2, int* result);

		inline PFN_CompareBrowserVersions GetCompareVersionsFunc()
		{
			HMODULE hModule = LoadWebView2Loader();
			if (hModule == nullptr) return nullptr;
			return reinterpret_cast<PFN_CompareBrowserVersions>(
				::GetProcAddress(hModule, "CompareBrowserVersions"));
		}

		// [WV2-MINRUNTIME] Raw installed Runtime version in WIDE characters.
		//
		// The loader hands back LPWSTR (CoTaskMem) regardless of the UNICODE setting
		// of the translation unit. Storing it in CDuiString and then reading it back
		// with GetData() works only in a Unicode build: in an ANSI build GetData()
		// returns const char*, which CompareBrowserVersions (LPCWSTR) rejects with
		// C2664. That is precisely what happened in the non-Unicode probes while the
		// main target compiled clean. Callers that need to compare versions must use
		// this wide accessor, not the CDuiString one.
		inline bool GetRuntimeVersionW(WCHAR* pszBuf, int cchBuf)
		{
			if (pszBuf == nullptr || cchBuf <= 0) return false;
			pszBuf[0] = L'\0';
			PFN_GetAvailableCoreWebView2BrowserVersionString pfnGetVersion = GetVersionFunc();
			if (pfnGetVersion == nullptr) return false;
			LPWSTR lpszVersion = nullptr;
			HRESULT hr = pfnGetVersion(nullptr, &lpszVersion);
			if (FAILED(hr) || lpszVersion == nullptr) return false;
			wcsncpy_s(pszBuf, static_cast<size_t>(cchBuf), lpszVersion, _TRUNCATE);
			::CoTaskMemFree(lpszVersion);
			return pszBuf[0] != L'\0';
		}

		// Raw installed Runtime version, or empty when no Runtime answers.
		// Caller owns nothing: the string is freed here.
		// TCHAR-typed convenience wrapper; use GetRuntimeVersionW for comparison.
		inline bool GetRuntimeVersion(CDuiString& sOut)
		{
			sOut.Empty();
			PFN_GetAvailableCoreWebView2BrowserVersionString pfnGetVersion = GetVersionFunc();
			if (pfnGetVersion == nullptr) return false;
			LPWSTR lpszVersion = nullptr;
			HRESULT hr = pfnGetVersion(nullptr, &lpszVersion);
			if (FAILED(hr) || lpszVersion == nullptr) return false;
			sOut = lpszVersion;
			::CoTaskMemFree(lpszVersion);
			return !sOut.IsEmpty();
		}

		// 检测 WebView2 Runtime（Evergreen）是否已安装
		inline bool IsWebView2RuntimeAvailable()
		{
			CDuiString sVersion;
			return GetRuntimeVersion(sVersion);
		}

		// [WV2-MINRUNTIME] Is the installed Runtime at least the documented floor?
		//
		// Uses the loader's own CompareBrowserVersions rather than parsing version
		// strings here: the SDK already ships a correct segment-wise comparison, and a
		// hand-rolled one gets 100.0.0.0 vs 99.0.0.0 wrong on the first try.
		// [WV2-MINRUNTIME] Parameter is LPCWSTR, not LPCTSTR on purpose.
		//
		// CompareBrowserVersions is a wide-character export, and this header is
		// included by probes compiled without UNICODE (where LPCTSTR is const char*).
		// Using LPCTSTR here produced C2446/C2664 in exactly those translation units
		// while the Unicode build compiled cleanly -- a defect invisible to the main
		// target because DuiLib itself is Unicode.
		inline bool IsRuntimeVersionSupported(LPCWSTR pszMinimum = nullptr)
		{
			// Wide buffer, not CDuiString: see GetRuntimeVersionW for why.
			WCHAR szVersion[128] = { 0 };
			if (!GetRuntimeVersionW(szVersion, _countof(szVersion))) return false;

			PFN_CompareBrowserVersions pfnCmp = GetCompareVersionsFunc();
			if (pfnCmp == nullptr) {
				// No comparer: we can only report "a runtime answered". Say so by
				// returning true, but the caller can distinguish via GetRuntimeVersionW.
				return true;
			}
			LPCWSTR pszMin = (pszMinimum != nullptr) ? pszMinimum : MinimumRuntimeVersion();
			int nCmp = 0;
			if (FAILED(pfnCmp(szVersion, pszMin, &nCmp))) return false;
			return nCmp >= 0;	// installed >= minimum
		}

		// 完整可用性判断：
		//   Windows 10 1703+ (Creators Update, Build 15063) —— WebView2 最低系统要求
		//   且已安装满足最低版本的 WebView2 Runtime
		// 旧版 Win10（含 1607 Anniversary）自动回退 IE 控件
		inline bool IsWebView2Supported()
		{
			return IsWindows10CreatorsOrGreater() && IsRuntimeVersionSupported();
		}
	}
} // namespace DuiLib

#endif // __WEBVIEW2HELPER_H__
