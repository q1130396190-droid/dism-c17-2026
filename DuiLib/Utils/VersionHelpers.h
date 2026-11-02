#ifndef _VERSIONHELPERS_H_INCLUDED_
#define _VERSIONHELPERS_H_INCLUDED_
#include <specstrings.h>
#include <wchar.h>

namespace DuiLib 
{
#define _WIN32_WINNT_NT4                    0x0400
#define _WIN32_WINNT_WIN2K                  0x0500
#define _WIN32_WINNT_WINXP                  0x0501
#define _WIN32_WINNT_WS03                   0x0502
#define _WIN32_WINNT_WIN6                   0x0600
#define _WIN32_WINNT_VISTA                  0x0600
#define _WIN32_WINNT_WS08                   0x0600
#define _WIN32_WINNT_LONGHORN               0x0600
#define _WIN32_WINNT_WIN7                   0x0601
#define _WIN32_WINNT_WIN8                   0x0602
#define _WIN32_WINNT_WINBLUE                0x0603
#define _WIN32_WINNT_WINTHRESHOLD           0x0A00 /* ABRACADABRA_THRESHOLD*/
#define _WIN32_WINNT_WIN10                  0x0A00 /* ABRACADABRA_THRESHOLD*/

#define WM_DPICHANGED                   0x02E0

#if(WINVER < 0x0601)
#define WM_TOUCH                        0x0240

/*
 * Touch Input defines and functions
 */
DECLARE_HANDLE(HTOUCHINPUT);

typedef struct tagTOUCHINPUT {
    LONG x;
    LONG y;
    HANDLE hSource;
    DWORD dwID;
    DWORD dwFlags;
    DWORD dwMask;
    DWORD dwTime;
    ULONG_PTR dwExtraInfo;
    DWORD cxContact;
    DWORD cyContact;
} TOUCHINPUT, *PTOUCHINPUT;
typedef TOUCHINPUT const * PCTOUCHINPUT;

#define TOUCH_COORD_TO_PIXEL(l)         ((l) / 100)

#define TOUCHEVENTF_MOVE            0x0001
#define TOUCHEVENTF_DOWN            0x0002
#define TOUCHEVENTF_UP              0x0004
#define TOUCHEVENTF_INRANGE         0x0008
#define TOUCHEVENTF_PRIMARY         0x0010
#define TOUCHEVENTF_NOCOALESCE      0x0020
#define TOUCHEVENTF_PEN             0x0040
#define TOUCHEVENTF_PALM            0x0080

#define TOUCHINPUTMASKF_TIMEFROMSYSTEM  0x0001
#define TOUCHINPUTMASKF_EXTRAINFO       0x0002
#define TOUCHINPUTMASKF_CONTACTAREA     0x0004

#define TWF_FINETOUCH       (0x00000001)
#define TWF_WANTPALM        (0x00000002)
#endif

	// ==================== 统一系统版本底层封装（消除重复ntdll加载逻辑） ====================
	// RtlGetVersion 不受应用manifest兼容虚拟化限制，返回真实系统版本
	typedef struct _DUILIB_RTL_OSVERSIONINFOW {
		ULONG dwOSVersionInfoSize;
		ULONG dwMajorVersion;
		ULONG dwMinorVersion;
		ULONG dwBuildNumber;
		ULONG dwPlatformId;
		WCHAR szCSDVersion[128];
	} DUILIB_RTL_OSVERSIONINFOW, *PDUILIB_RTL_OSVERSIONINFOW;

	typedef LONG(NTAPI *PFN_DUILIB_RtlGetVersion)(PDUILIB_RTL_OSVERSIONINFOW);

	/// <summary>
	/// 获取真实系统版本信息（全局唯一入口，所有判断函数复用）
	/// </summary>
	static BOOL GetRealOSVersion(PDUILIB_RTL_OSVERSIONINFOW pVersion)
	{
		if (pVersion == nullptr)
			return FALSE;

		HMODULE hNtdll = ::GetModuleHandleW(L"ntdll.dll");
		if (hNtdll == nullptr)
			return FALSE;

		PFN_DUILIB_RtlGetVersion pfnRtlGetVersion = (PFN_DUILIB_RtlGetVersion)::GetProcAddress(hNtdll, "RtlGetVersion");
		if (pfnRtlGetVersion == nullptr)
			return FALSE;

		ZeroMemory(pVersion, sizeof(DUILIB_RTL_OSVERSIONINFOW));
		pVersion->dwOSVersionInfoSize = sizeof(DUILIB_RTL_OSVERSIONINFOW);
		return pfnRtlGetVersion(pVersion) == 0;
	}

	/// <summary>
	/// 从 szCSDVersion 字符串提取 Service Pack 数字，提取逻辑统一复用
	/// </summary>
	static WORD ExtractServicePack(const WCHAR* szCSDVersion)
	{
		if (szCSDVersion == nullptr)
			return 0;

		const WCHAR* pszSP = wcsstr(szCSDVersion, L"Service Pack");
		if (pszSP == nullptr)
			return 0;

		for (const WCHAR* p = pszSP; *p != L'\0'; ++p)
		{
			if (*p >= L'0' && *p <= L'9')
			{
				return (WORD)(*p - L'0');
			}
		}
		return 0;
	}

	/// <summary>
	/// 通用版本比对：判断当前系统 >= 指定 Major.Minor.SP
	/// 统一封装版本+SP比较逻辑，所有系统判断函数全部复用此方法
	/// </summary>
	static BOOL IsWindowsVersionOrGreater(WORD wMajorVersion, WORD wMinorVersion, WORD wServicePackMajor)
	{
		DUILIB_RTL_OSVERSIONINFOW osvi;
		if (!GetRealOSVersion(&osvi))
			return FALSE;

		WORD wCurrentSP = ExtractServicePack(osvi.szCSDVersion);

		// 主版本优先比较
		if (osvi.dwMajorVersion != wMajorVersion)
			return osvi.dwMajorVersion > wMajorVersion;
		// 次版本次之
		if (osvi.dwMinorVersion != wMinorVersion)
			return osvi.dwMinorVersion > wMinorVersion;
		// 最后比对Service Pack
		return wCurrentSP >= wServicePackMajor;
	}

	// ==================== 对外系统判断API（全部复用通用IsWindowsVersionOrGreater，无重复底层代码） ====================
	static BOOL IsWindowsXPOrGreater()
	{
		return IsWindowsVersionOrGreater(HIBYTE(_WIN32_WINNT_WINXP), LOBYTE(_WIN32_WINNT_WINXP), 0);
	}

	static BOOL IsWindowsXPSP1OrGreater()
	{
		return IsWindowsVersionOrGreater(HIBYTE(_WIN32_WINNT_WINXP), LOBYTE(_WIN32_WINNT_WINXP), 1);
	}

	static BOOL IsWindowsXPSP2OrGreater()
	{
		return IsWindowsVersionOrGreater(HIBYTE(_WIN32_WINNT_WINXP), LOBYTE(_WIN32_WINNT_WINXP), 2);
	}

	static BOOL IsWindowsXPSP3OrGreater()
	{
		return IsWindowsVersionOrGreater(HIBYTE(_WIN32_WINNT_WINXP), LOBYTE(_WIN32_WINNT_WINXP), 3);
	}

	static BOOL IsWindowsVistaOrGreater()
	{
		return IsWindowsVersionOrGreater(HIBYTE(_WIN32_WINNT_VISTA), LOBYTE(_WIN32_WINNT_VISTA), 0);
	}

	static BOOL IsWindowsVistaSP1OrGreater()
	{
		return IsWindowsVersionOrGreater(HIBYTE(_WIN32_WINNT_VISTA), LOBYTE(_WIN32_WINNT_VISTA), 1);
	}

	static BOOL IsWindowsVistaSP2OrGreater()
	{
		return IsWindowsVersionOrGreater(HIBYTE(_WIN32_WINNT_VISTA), LOBYTE(_WIN32_WINNT_VISTA), 2);
	}

	static BOOL IsWindows7OrGreater()
	{
		return IsWindowsVersionOrGreater(HIBYTE(_WIN32_WINNT_WIN7), LOBYTE(_WIN32_WINNT_WIN7), 0);
	}

	static BOOL IsWindows7SP1OrGreater()
	{
		return IsWindowsVersionOrGreater(HIBYTE(_WIN32_WINNT_WIN7), LOBYTE(_WIN32_WINNT_WIN7), 1);
	}

	static BOOL IsWindows8OrGreater()
	{
		return IsWindowsVersionOrGreater(HIBYTE(_WIN32_WINNT_WIN8), LOBYTE(_WIN32_WINNT_WIN8), 0);
	}

	static BOOL IsWindows8Point1OrGreater()
	{
		return IsWindowsVersionOrGreater(HIBYTE(_WIN32_WINNT_WINBLUE), LOBYTE(_WIN32_WINNT_WINBLUE), 0);
	}

	static BOOL IsWindowsThresholdOrGreater()
	{
		return IsWindowsVersionOrGreater(HIBYTE(_WIN32_WINNT_WINTHRESHOLD), LOBYTE(_WIN32_WINNT_WINTHRESHOLD), 0);
	}

	static BOOL IsWindows10OrGreater()
	{
		return IsWindowsVersionOrGreater(HIBYTE(_WIN32_WINNT_WINTHRESHOLD), LOBYTE(_WIN32_WINNT_WINTHRESHOLD), 0);
	}

	// Windows11判定：主版本10且Build>=22000，或主版本>10
	static BOOL IsWindows11OrGreater()
	{
		DUILIB_RTL_OSVERSIONINFOW osvi;
		if (!GetRealOSVersion(&osvi))
			return FALSE;

		return (osvi.dwMajorVersion == 10 && osvi.dwBuildNumber >= 22000) || osvi.dwMajorVersion > 10;
	}

	// ==================== Win10 Build 精准版本判定 ====================
	// 仅当 Major==10 时有效；用于区分 Win10 内部功能发布阈值。
	// 各 Win10 Build 对应：
	//   10240 = RTM (1507)
	//   10586 = 1511
	//   14393 = 1607 Anniversary Update    ← 本次降级目标
	//   15063 = 1703 Creators Update        ← WebView2 / GetDpiForWindow / PerMonitorV2 需要此版本
	//   16299 = 1709 Fall Creators
	//   17134 = 1803 (DWMWA_USE_IMMERSIVE_DARK_MODE=20 正式版)
	//   17763 = 1809
	//   18362 = 1903
	//   18363 = 1909
	//   19041 = 2004
	//   19042 = 20H2
	//   19043 = 21H1
	//   19044 = 21H2
	//   19045 = 22H2  ← 当前开发平台
	static BOOL IsWindows10BuildOrGreater(DWORD dwBuild)
	{
		DUILIB_RTL_OSVERSIONINFOW osvi;
		if (!GetRealOSVersion(&osvi))
			return FALSE;
		if (osvi.dwMajorVersion != 10)
			return osvi.dwMajorVersion > 10;   // Win11+ 视为满足所有 Win10 Build 阈值
		return osvi.dwBuildNumber >= dwBuild;
	}

	// ---- 常用别名 ----
	static BOOL IsWindows10AnniversaryOrGreater()   { return IsWindows10BuildOrGreater(14393); }
	static BOOL IsWindows10CreatorsOrGreater()       { return IsWindows10BuildOrGreater(15063); }   // WebView2 / GetDpiForWindow
	static BOOL IsWindows10FallCreatorsOrGreater()   { return IsWindows10BuildOrGreater(16299); }
	static BOOL IsWindows10DarkModeOrGreater()       { return IsWindows10BuildOrGreater(17134); }   // DWMWA_USE_IMMERSIVE_DARK_MODE=20
	static BOOL IsWindows10May2020OrGreater()        { return IsWindows10BuildOrGreater(19041); }

	static BOOL IsWindowsServer()
	{
		OSVERSIONINFOEXW osvi = { sizeof(osvi), 0, 0, 0, 0, {0}, 0, 0, 0, VER_NT_WORKSTATION };
		DWORDLONG const dwlConditionMask = VerSetConditionMask(0, VER_PRODUCT_TYPE, VER_EQUAL);
		return !VerifyVersionInfoW(&osvi, VER_PRODUCT_TYPE, dwlConditionMask);
	}
}
#endif // _VERSIONHELPERS_H_INCLUDED_