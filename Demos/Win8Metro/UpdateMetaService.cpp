#include <windows.h>
#include "FormatW.h"
#include <cstdio>
#include <cstdlib>   // [C17] _wtol（F18 tchar.h 清零）
#include <string>
#include <string_view>
#include <wininet.h>   // [原生化] DownloadFile：WinINet 直连，替代 powershell IWR

#pragma comment(lib, "wininet.lib")

#include "UpdateMetaService.h"
#include "DismApiBridge.h"   // [原生化] AddPackage/GetPackages 进程内直连 dismapi.dll
#include "DismCore.h"        // [R-P21 / Phase 1 · R1] Rust dismcore FFI（哈希/Hex）
#include "unzip.h"           // [原生化] UnzipEntry：进程内 minizip，替代 powershell 解压
#include "SafeThread.h"      // 模态消息循环异常边界

// ============================================================
// UpdateMetaService.cpp —— [R-B8] 更新机制闭环实现
// 仅依赖 Win32 + CRT，无 DuiLib 耦合，便于 x86/x64 双架构编译。
//   1) UpdateInfo.zip 四字段解析
//   2) CBS 独占倒计时对话框（自建模态窗口 + 子控件 + 30s 倒计时）
//   3) wsusscn2/3.cab 随包保障（缺失时 PowerShell 官方源下载）
// ============================================================

namespace {

// ----------------------------------------------------------
// 标签抽取：<tag>...</tag>，去首尾空白
// ----------------------------------------------------------
bool ExtractTag(const wchar_t* pszXml, const wchar_t* pszTag, wchar_t* pszOut, int cchOut)
{
    if (!pszXml || !pszTag || !pszOut || cchOut <= 0) return false;
    pszOut[0] = L'\0';

    std::wstring szOpen = L"<";
    std::wstring szClose = L"</";
    szOpen += pszTag;
    szClose += pszTag;
    szOpen += L">";
    szClose += L">";

    const wchar_t* pStart = wcsstr(pszXml, szOpen.c_str());
    if (!pStart) return false;
    pStart += wcslen(szOpen.c_str());

    const wchar_t* pEnd = wcsstr(pStart, szClose.c_str());
    if (!pEnd) return false;

    size_t nLen = (size_t)(pEnd - pStart);
    if (nLen >= static_cast<size_t>(cchOut)) nLen = static_cast<size_t>(cchOut) - 1;
    wcsncpy_s(pszOut, cchOut, pStart, nLen);
    pszOut[nLen] = L'\0';

    // 去首尾空白
    wchar_t* p = pszOut;
    while (*p == L' ' || *p == L'\t' || *p == L'\r' || *p == L'\n') ++p;
    if (p != pszOut) memmove(pszOut, p, (wcslen(p) + 1) * sizeof(wchar_t));
    size_t n = wcslen(pszOut);
    while (n > 0)
    {
        wchar_t c = pszOut[n - 1];
        if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n')
        { pszOut[--n] = L'\0'; continue; }
        break;
    }
    return pszOut[0] != L'\0';
}

// [C++17 升级阶段2] 句柄 RAII 守卫：隐式转换为原句柄类型，调用点零改动；
// 只负责早退/异常路径自动释放，不改变任何调用顺序与业务分支。
struct FileGuard
{
    FileGuard() = default;
    FileGuard(HANDLE h) : m_h(h) {}
    ~FileGuard() { if (m_h != nullptr && m_h != INVALID_HANDLE_VALUE) ::CloseHandle(m_h); }
    FileGuard(const FileGuard&) = delete;
    FileGuard& operator=(const FileGuard&) = delete;
    operator HANDLE() const { return m_h; }
private:
    HANDLE m_h = nullptr;
};

struct InetGuard
{
    InetGuard() = default;
    InetGuard(HINTERNET h) : m_h(h) {}
    ~InetGuard() { if (m_h) ::InternetCloseHandle(m_h); }
    InetGuard(const InetGuard&) = delete;
    InetGuard& operator=(const InetGuard&) = delete;
    operator HINTERNET() const { return m_h; }
private:
    HINTERNET m_h = nullptr;
};

struct FindGuard
{
    FindGuard() = default;
    FindGuard(HANDLE h) : m_h(h) {}
    ~FindGuard() { if (m_h != nullptr && m_h != INVALID_HANDLE_VALUE) ::FindClose(m_h); }
    FindGuard(const FindGuard&) = delete;
    FindGuard& operator=(const FindGuard&) = delete;
    operator HANDLE() const { return m_h; }
    void reset(HANDLE x = nullptr) { m_h = x; }   // [C++17 R-P8] 供重新接管
private:
    HANDLE m_h = nullptr;
};

// ----------------------------------------------------------
// 读整个文件
// ----------------------------------------------------------
bool ReadWholeFile(const wchar_t* pszPath, std::string& out)
{
    out.clear();
    FileGuard hFile = ::CreateFileW(pszPath, GENERIC_READ, FILE_SHARE_READ, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER li = { 0 };
    ::GetFileSizeEx(hFile, &li);
    if (li.QuadPart <= 0 || li.QuadPart > 16LL * 1024 * 1024)
    { return false; }
    DWORD dwSize = static_cast<DWORD>(li.QuadPart);
    out.resize(dwSize);
    DWORD dwRead = 0;
    BOOL bOk = ::ReadFile(hFile, &out[0], dwSize, &dwRead, nullptr);
    if (!bOk) { out.clear(); return false; }
    out.resize(dwRead);
    return !out.empty();
}

// ----------------------------------------------------------
// [原生化] zip 单条目解压到内存：DuiLib minizip 封装（unzip.cpp）
// 替代 powershell System.IO.Compression 子进程。
// ----------------------------------------------------------
bool UnzipEntry(const wchar_t* pszZip, const wchar_t* pszEntry, std::string& outContent)
{
    outContent.clear();
    if (!pszZip || !*pszZip || !pszEntry || !*pszEntry) return false;

    HZIP hz = OpenZip(pszZip);
    if (!hz) return false;

    bool bOk = false;
    int nIdx = -1;
    ZIPENTRY ze;
    ze = {};
    if (FindZipItem(hz, pszEntry, TRUE /*不区分大小写*/, &nIdx, &ze) == ZR_OK
        && nIdx >= 0 && ze.unc_size > 0)
    {
        outContent.resize(static_cast<size_t>(ze.unc_size));
        if (UnzipItem(hz, nIdx, &outContent[0], (unsigned int)ze.unc_size) == ZR_OK)
            bOk = true;
        else
            outContent.clear();
    }
    CloseZip(hz);
    return bOk;
}

// ----------------------------------------------------------
// [原生化] 官方下载：WinINet HTTP/HTTPS 直连（InternetOpenUrl），
// 替代 powershell Invoke-WebRequest 子进程。
// ----------------------------------------------------------
bool DownloadFile(const wchar_t* pszUrl, const wchar_t* pszDst)
{
    if (!pszUrl || !*pszUrl || !pszDst || !*pszDst) return false;

    InetGuard hNet = ::InternetOpenW(L"Win8Metro-Update/1.0",
                                     INTERNET_OPEN_TYPE_PRECONFIG,
                                     nullptr, nullptr, 0);
    if (!hNet) return false;

    DWORD dwTimeout = 30000;
    ::InternetSetOptionW(hNet, INTERNET_OPTION_CONNECT_TIMEOUT,
                         &dwTimeout, sizeof(dwTimeout));
    dwTimeout = 60000;
    ::InternetSetOptionW(hNet, INTERNET_OPTION_RECEIVE_TIMEOUT,
                         &dwTimeout, sizeof(dwTimeout));
    ::InternetSetOptionW(hNet, INTERNET_OPTION_SEND_TIMEOUT,
                         &dwTimeout, sizeof(dwTimeout));

    bool bOk = false;
    InetGuard hUrl = ::InternetOpenUrlW(
        hNet, pszUrl, nullptr, 0,
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE
        | INTERNET_FLAG_NO_UI | INTERNET_FLAG_SECURE,
        0);
    if (hUrl)
    {
        FileGuard hFile = ::CreateFileW(pszDst, GENERIC_WRITE, 0, nullptr,
                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile != INVALID_HANDLE_VALUE)
        {
            BYTE  buf[64 * 1024];
            DWORD dwRead = 0;
            DWORD dwTick = ::GetTickCount();
            bOk = true;
            for (;;)
            {
                if (!::InternetReadFile(hUrl, buf, static_cast<DWORD>(sizeof(buf)), &dwRead))
                { bOk = false; break; }
                if (dwRead == 0) break;   // EOF
                DWORD dwWritten = 0;
                if (!::WriteFile(hFile, buf, dwRead, &dwWritten, nullptr)
                    || dwWritten != dwRead)
                { bOk = false; break; }
                if (::GetTickCount() - dwTick > 300000)   // 5min 总上限
                { bOk = false; break; }
            }
        }
    }


    if (!bOk) ::DeleteFileW(pszDst);
    return bOk && ::GetFileAttributesW(pszDst) != INVALID_FILE_ATTRIBUTES;
}

// ----------------------------------------------------------
// 倒计时对话框：窗口上下文
// ----------------------------------------------------------
inline constexpr UINT_PTR kCountdownTimer = 1;
inline constexpr int kBtnPause = 1001;
inline constexpr int kBtnContinue = 1002;

struct CbsCtx
{
    std::wstring sTitle;
    std::wstring sText;
    int   nRemaining;
    HWND  hCntStatic;
};

// 全局结果变量（窗口销毁后读取）
static int g_lastCbsResult = IDCANCEL;

// 窗口类名（进程内唯一）
const wchar_t* kCbsClass = L"DismPP_CbsExclusiveDlg";

// ----------------------------------------------------------
// 对话框主窗口过程（无子控件，WM_PAINT 自绘）
// ----------------------------------------------------------
LRESULT CALLBACK CbsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    CbsCtx* pCtx = (CbsCtx*)::GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg)
    {
    case WM_CREATE:
    {
        CREATESTRUCTW* pCs = (CREATESTRUCTW*)lp;
        CbsCtx* pIn = (CbsCtx*)pCs->lpCreateParams;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)pIn);
        pCtx = pIn;

        HFONT hFont = (HFONT)::GetStockObject(DEFAULT_GUI_FONT);
        ::SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);

        // 子控件：正文 / 倒计时 / 两个按钮
        ::CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                          20, 20, 360, 60, hwnd, nullptr,
                          ::GetModuleHandleW(nullptr), nullptr);
        HWND hText = ::FindWindowExW(hwnd, nullptr, L"STATIC", nullptr);
        ::SetWindowTextW(hText, pCtx ? pCtx->sText.c_str() : L"");

        HWND hCnt = ::CreateWindowExW(0, L"STATIC", L"",
                          WS_CHILD | WS_VISIBLE | SS_LEFT,
                          20, 88, 360, 20, hwnd, nullptr,
                          ::GetModuleHandleW(nullptr), nullptr);
        if (pCtx) pCtx->hCntStatic = hCnt;

        HWND hBtn1 = ::CreateWindowExW(0, L"BUTTON",
                          L"暂不安装（推荐）", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                          20, 120, 170, 30, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kBtnPause)),
                          ::GetModuleHandleW(nullptr), nullptr);
        ::SetFocus(hBtn1);

        ::CreateWindowExW(0, L"BUTTON",
                          L"继续安装", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                          210, 120, 170, 30, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kBtnContinue)),
                          ::GetModuleHandleW(nullptr), nullptr);

        ::SetTimer(hwnd, kCountdownTimer, 1000, nullptr);

        std::wstring sCnt;
        sCnt.resize(128);
        wfmt::FormatInto(sCnt.data(), sCnt.size() + 1,
            L"（%d 秒后自动选择" L"暂不安装" L"）", pCtx ? pCtx->nRemaining : 0);
        sCnt.resize(wcslen(sCnt.c_str()));
        ::SetWindowTextW(hCnt, sCnt.c_str());

        RECT rc = { 0, 0, 400, 170 };
        ::AdjustWindowRect(&rc, WS_CAPTION | WS_SYSMENU, FALSE);
        ::SetWindowPos(hwnd, nullptr, 0, 0,
                       rc.right - rc.left, rc.bottom - rc.top,
                       SWP_NOMOVE | SWP_NOZORDER);
        return 0;
    }

    case WM_TIMER:
        if (wp == kCountdownTimer && pCtx)
        {
            if (pCtx->nRemaining > 0) --pCtx->nRemaining;
            std::wstring sCnt;
            sCnt.resize(128);
            wfmt::FormatInto(sCnt.data(), sCnt.size() + 1,
                L"（%d 秒后自动选择" L"暂不安装" L"）", pCtx->nRemaining);
            sCnt.resize(wcslen(sCnt.c_str()));
            ::SetWindowTextW(pCtx->hCntStatic, sCnt.c_str());
            if (pCtx->nRemaining <= 0)
            {
                g_lastCbsResult = IDCANCEL;
                ::KillTimer(hwnd, kCountdownTimer);
                ::DestroyWindow(hwnd);  // 由 GetMessage 循环退出
            }
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case kBtnContinue:
            g_lastCbsResult = IDYES;
            ::KillTimer(hwnd, kCountdownTimer);
            ::DestroyWindow(hwnd);
            return 0;
        case kBtnPause:
        case IDCANCEL:
        case IDOK:
            g_lastCbsResult = IDCANCEL;
            ::KillTimer(hwnd, kCountdownTimer);
            ::DestroyWindow(hwnd);
            return 0;
        }
        break;

    case WM_CLOSE:
        g_lastCbsResult = IDCANCEL;
        ::KillTimer(hwnd, kCountdownTimer);
        ::DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        ::KillTimer(hwnd, kCountdownTimer);
        return 0;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

// ============================================================
// ParseUpdateInfoZip
// ============================================================
bool UpdateMetaService::ParseUpdateInfoZip(const wchar_t* pszZipPath, UpdateMeta& out)
{
    out = UpdateMeta();
    if (!pszZipPath || !*pszZipPath) return false;
    if (::GetFileAttributesW(pszZipPath) == INVALID_FILE_ATTRIBUTES) return false;

    std::string sXml;
    if (!UnzipEntry(pszZipPath, L"UpdateInfo.xml", sXml)) return false;

    int nWide = ::MultiByteToWideChar(CP_UTF8, 0, sXml.c_str(), static_cast<int>(sXml.size()), nullptr, 0);
    if (nWide <= 0) return false;
    std::wstring wXml;
    wXml.resize(nWide);
    ::MultiByteToWideChar(CP_UTF8, 0, sXml.c_str(), static_cast<int>(sXml.size()), &wXml[0], nWide);

    ExtractTag(wXml.c_str(), L"Keys",        out.szKeys,  _countof(out.szKeys));
    ExtractTag(wXml.c_str(), L"URLS",        out.szUrls,  _countof(out.szUrls));
    ExtractTag(wXml.c_str(), L"Information", out.szInfo,  _countof(out.szInfo));
    ExtractTag(wXml.c_str(), L"Tag",         out.szTag,   _countof(out.szTag));

    wchar_t szThread[32] = {};
    if (ExtractTag(wXml.c_str(), L"MaxDownloadThread", szThread, _countof(szThread)))
    {
        long l = _wtol(szThread);
        if (l >= 1 && l <= 128) out.dwMaxDownloadThread = static_cast<DWORD>(l);
    }

    out.bValid = (out.szKeys[0] != L'\0' || out.szInfo[0] != L'\0' ||
                  out.szUrls[0] != L'\0');
    return out.bValid;
}

// ============================================================
// ShowExclusiveCountdown —— CBS 独占倒计时对话框
// ============================================================
int UpdateMetaService::ShowExclusiveCountdown(HWND hParent, const wchar_t* pszTitle,
                                               const wchar_t* pszText, int nTimeoutSec)
{
    if (nTimeoutSec <= 0) nTimeoutSec = 30;

    HINSTANCE hInst = ::GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = CbsProc;
    wc.hInstance = hInst;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = kCbsClass;
    ::RegisterClassExW(&wc);

    CbsCtx ctx = {};
    ctx.nRemaining = nTimeoutSec;
    ctx.sTitle = pszTitle ? pszTitle : L"Windows 更新";
    ctx.sText  = pszText  ? pszText  : L"";

    // 创建模态窗口
    HWND hwnd = ::CreateWindowExW(WS_EX_DLGMODALFRAME, kCbsClass,
                                  ctx.sTitle.c_str(),
                                  WS_CAPTION | WS_SYSMENU | WS_POPUP,
                                  CW_USEDEFAULT, CW_USEDEFAULT,
                                  400, 170,
                                  hParent, nullptr, hInst, &ctx);
    if (!hwnd)
    {
        ::UnregisterClassW(kCbsClass, hInst);
        return IDCANCEL;
    }

    // 居中于父窗口
    RECT rcParent = { 0, 0, ::GetSystemMetrics(SM_CXSCREEN), ::GetSystemMetrics(SM_CYSCREEN) };
    if (hParent && ::IsWindow(hParent)) ::GetWindowRect(hParent, &rcParent);
    RECT rcOwn;
    ::GetWindowRect(hwnd, &rcOwn);
    int w = rcOwn.right - rcOwn.left, h = rcOwn.bottom - rcOwn.top;
    ::SetWindowPos(hwnd, nullptr,
                   (rcParent.left + rcParent.right) / 2 - w / 2,
                   (rcParent.top + rcParent.bottom) / 2 - h / 2,
                   0, 0, SWP_NOSIZE | SWP_NOZORDER);

    // 禁用父窗口
    if (hParent && ::IsWindow(hParent)) ::EnableWindow(hParent, FALSE);

    ::ShowWindow(hwnd, SW_SHOW);
    ::UpdateWindow(hwnd);

    // 模态消息循环
    int nResult = IDCANCEL;
    MSG msg;
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        ::TranslateMessage(&msg);
        SafeDispatchMessage(&msg);
        if (!::IsWindow(hwnd)) break;   // 窗口被销毁即退出模态
    }
    nResult = g_lastCbsResult;

    if (hParent && ::IsWindow(hParent)) ::EnableWindow(hParent, TRUE);
    ::UnregisterClassW(kCbsClass, hInst);
    return nResult;
}

// ============================================================
// EnsureOfflineCabs —— wsusscn2/3.cab 随包保障
// ============================================================
bool UpdateMetaService::EnsureOfflineCabs(const wchar_t* pszExeDir, bool bDownloadIfMissing)
{
    if (!pszExeDir || !*pszExeDir) return false;

    static const wchar_t* kCabs[] = { L"wsusscn2.cab", L"wsusscn3.cab" };
    static const wchar_t* kUrls[] = {
        L"https://download.windowsupdate.com/wsusscn2.cab",
        L"https://download.windowsupdate.com/msdownload/update/v3/static/trustedr/en/wsusscn2.cab",
        L"https://msdl.microsoft.com/download/symbols/wsusscn2.cab",
        L"https://download.windowsupdate.com/wsusscn3.cab",
        L"https://download.windowsupdate.com/msdownload/update/v3/static/trustedr/en/wsusscn3.cab",
    };
    const int nCabCount = _countof(kCabs);
    const int nUrlCount = _countof(kUrls);

    bool bAll = true;
    for (int i = 0; i < nCabCount; ++i)
    {
        std::wstring sPath;
        sPath.resize(MAX_PATH);
        wfmt::FormatInto(sPath.data(), sPath.size() + 1, L"%s\\%s", pszExeDir, kCabs[i]);
        sPath.resize(wcslen(sPath.c_str()));
        if (::GetFileAttributesW(sPath.c_str()) != INVALID_FILE_ATTRIBUTES) continue;

        if (!bDownloadIfMissing)
        {
            bAll = false;
            continue;
        }

        // 尝试多个官方 URL
        bool bGot = false;
        for (int u = 0; u < nUrlCount; ++u)
        {
            // 文件名必须匹配当前 cab
            if (wcsstr(kUrls[u], kCabs[i]) == nullptr) continue;
            if (DownloadFile(kUrls[u], sPath.c_str()))
            {
                bGot = true;
                break;
            }
        }
        if (!bGot) bAll = false;
    }
    return bAll;
}

// ============================================================
// [R-B50] MaxDownloadThread 真实消费闸门：config.ini 持久化
// ------------------------------------------------------------
// 基线缺口：UpdateInfo.xml 的 MaxDownloadThread 此前仅解析进日志，
// 从未控制下载并发。R-B50 把它升级为"实际生效的线程上限"，并让
// 用户能在更新页选择并持久化到 config.ini [Update] MaxThread。
//   优先级：config.ini [Update] MaxThread > UpdateInfo.xml 解析值 > 默认 10。
// ============================================================
DWORD UpdateMetaService::ReadEffectiveMaxThread(const wchar_t* pszIniPath,
                                                DWORD dwFromMetaDefault)
{
    DWORD dwResult = dwFromMetaDefault;
    if (pszIniPath && *pszIniPath)
    {
        wchar_t szBuf[32] = {};
        ::GetPrivateProfileStringW(L"Update", L"MaxThread", nullptr,
                                   szBuf, _countof(szBuf), pszIniPath);
        if (szBuf[0])
        {
            long l = _wtol(szBuf);
            if (l >= 1 && l <= 128)
                dwResult = static_cast<DWORD>(l);
        }
    }
    if (dwResult < 1) dwResult = 1;
    if (dwResult > 128) dwResult = 128;
    return dwResult;
}

bool UpdateMetaService::WriteEffectiveMaxThread(const wchar_t* pszIniPath, DWORD dwThreads)
{
    if (!pszIniPath || !*pszIniPath) return false;
    if (dwThreads < 1) dwThreads = 1;
    if (dwThreads > 128) dwThreads = 128;

    std::wstring sBuf;
    sBuf.resize(32);
    wfmt::FormatInto(sBuf.data(), sBuf.size() + 1, L"%u", (unsigned)dwThreads);
    sBuf.resize(wcslen(sBuf.c_str()));
    return ::WritePrivateProfileStringW(L"Update", L"MaxThread", sBuf.c_str(),
                                        pszIniPath) != FALSE;
}

// ============================================================
// [R-B76/77/78] 本地补丁 / Range 下载器 / 回传通道
// ============================================================
#include <wincrypt.h>
#include <objbase.h>   // CoCreateGuid（任务命名辅助，不依赖 COM 初始化）

#pragma comment(lib, "advapi32.lib")

namespace b3helper {

// ----------------------------------------------------------
static bool EndsWithNoCase(const std::wstring& s, std::wstring_view svSuf)
{
    const size_t n = svSuf.size();
    if (s.size() < n) return false;
    return _wcsnicmp(s.c_str() + s.size() - n, svSuf.data(), n) == 0;
}

static std::wstring DirOf(const std::wstring& sPath)
{
    size_t p = sPath.find_last_of(L"\\/");
    return p == std::wstring::npos ? L"." : sPath.substr(0, p);
}

static void WriteTextFile(const wchar_t* pszPath, const std::string& sData)
{
    FileGuard h = ::CreateFileW(pszPath, GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD dw = 0;
    // [R128-B] 元数据缓存写出不完整会导致下次读取解析失败，留痕
    if (!::WriteFile(h, sData.data(), static_cast<DWORD>(sData.size()), &dw, nullptr)
        || dw != static_cast<DWORD>(sData.size()))
        ::OutputDebugStringW(L"[R128][updmeta] 元数据写入不完整\r\n");
}

static unsigned long Fnv1a32(const std::wstring& s)
{
    unsigned long h = 2166136261UL;
    for (size_t i = 0; i < s.size(); ++i)
    {
        h ^= (unsigned long)(s[i] & 0xFFFF);
        h *= 16777619UL;
    }
    return h;
}

// ----------------------------------------------------------
// CALG_MD5 文件摘要 → 32 位小写十六进制
//   [R-P21 / Phase 1 · R1] 已由 CryptoAPI 迁移到 Rust dismcore::DigestFileHex
//   消除 CryptAcquireContextW / CryptCreateHash / CryptHashData /
//   CryptGetHashParam 句柄生命周期与手工缓冲逻辑（Rust 侧全安全代码 + FFI 零跨边界分配）
// ----------------------------------------------------------
static std::wstring HashFileMd5(const wchar_t* pszPath)
{
    std::wstring sHexOut;
    if (!pszPath) return sHexOut;
    const std::string sHex = dismcore::DigestFileHex(DISMCORE_MD5, pszPath);
    if (sHex.size() != 32) return sHexOut;
    // ASCII 小写 hex → UTF-16
    sHexOut.reserve(32);
    for (size_t i = 0; i < 32; ++i)
        sHexOut.push_back(static_cast<wchar_t>(sHex[i]));
    return sHexOut;
}

// ----------------------------------------------------------
// [R-B77] express 差异（PSF / msdelta）尝试
//   约定：下载目录内存在 express.psf.cix.xml 与 $dpx$ 目录，
//   $dpx$ 下 *.psf 为 delta，UpdateCache 内最新同名基座 cab 为 source，
//   ApplyDeltaW 合成目标。任一步失败返回 false（调用方降级全量）。
// ----------------------------------------------------------
using PFN_BfApplyDeltaW = BOOL ( WINAPI*)(DWORD dwFlags, const wchar_t* pszSource,
                                        const wchar_t* pszDelta, const wchar_t* pszTarget);

static bool TryExpressDelta(const std::wstring& sDst, std::wstring* pWhy)
{
    std::wstring sDir = DirOf(sDst);
    std::wstring sCix = sDir + L"\\express.psf.cix.xml";
    std::wstring sDpx = sDir + L"\\$dpx$";
    if (::GetFileAttributesW(sCix.c_str()) == INVALID_FILE_ATTRIBUTES ||
        ::GetFileAttributesW(sDpx.c_str()) == INVALID_FILE_ATTRIBUTES)
        return false;  // 无差异资产 → 不算失败，走普通下载

    HMODULE hMsd = ::LoadLibraryW(L"msdelta.dll");
    if (!hMsd) { if (pWhy) *pWhy = L"msdelta.dll 加载失败"; return false; }

    PFN_BfApplyDeltaW pfn = reinterpret_cast<PFN_BfApplyDeltaW>(
        ::GetProcAddress(hMsd, "ApplyDeltaW"));
    if (!pfn) { ::FreeLibrary(hMsd); if (pWhy) *pWhy = L"ApplyDeltaW 缺失"; return false; }

    // delta：$dpx$ 下第一个 .psf
    std::wstring sDelta;
    WIN32_FIND_DATAW fd;
    FindGuard hFind = ::FindFirstFileW((sDpx + L"\\*.psf").c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE)
    {
        sDelta = sDpx + L"\\" + fd.cFileName;
    }

    // source：UpdateCache 内最新同扩展名基座文件
    std::wstring sCache = sDir + L"\\UpdateCache";
    std::wstring sSource;
    FILETIME ftBest;
    ftBest = {};
    hFind.reset(::FindFirstFileW((sCache + L"\\*.*").c_str(), &fd));
    if (hFind != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::wstring sName(fd.cFileName);
            if (EndsWithNoCase(sName, L".cab") || EndsWithNoCase(sName, L".msu") ||
                EndsWithNoCase(sName, L".psf") || sName.find(L".pending") != std::wstring::npos)
                continue;
            if (::CompareFileTime(&fd.ftLastWriteTime, &ftBest) > 0)
            {
                ftBest = fd.ftLastWriteTime;
                sSource = sCache + L"\\" + sName;
            }
        } while (::FindNextFileW(hFind, &fd));
    }

    bool bOk = false;
    if (!sDelta.empty() && !sSource.empty())
    {
        ::DeleteFileW(sDst.c_str());
        // DELTA_APPLY_FLAG_ALLOW_PA19 = 1（兼容 PA19 旧格式）
        bOk = pfn(1, sSource.c_str(), sDelta.c_str(), sDst.c_str()) != FALSE;
    }
    ::FreeLibrary(hMsd);
    if (!bOk && pWhy) *pWhy = L"ApplyDeltaW 合成失败";
    return bOk;
}

static std::string MakeJobXml(const std::wstring& sUrl,
                              const std::wstring& sMd5,
                              unsigned long ulTotal, const char* pszState,
                              unsigned long ulTask)
{
    std::string sU, sM;
    int cchU = ::WideCharToMultiByte(CP_UTF8, 0, sUrl.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (cchU > 0) { sU.resize(cchU - 1);
        ::WideCharToMultiByte(CP_UTF8, 0, sUrl.c_str(), -1, &sU[0], cchU, nullptr, nullptr); }
    int cchM = ::WideCharToMultiByte(CP_UTF8, 0, sMd5.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (cchM > 0) { sM.resize(cchM - 1);
        ::WideCharToMultiByte(CP_UTF8, 0, sMd5.c_str(), -1, &sM[0], cchM, nullptr, nullptr); }

    std::string szXml;
    {
        int nNeed = std::snprintf(nullptr, 0,
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
            "<DownloadJob Task=\"Dism++WusuDownload-%08lX\" Engine=\"WinINet-Range\">\r\n"
            "  <Url>%s</Url>\r\n"
            "  <Md5>%s</Md5>\r\n"
            "  <Size>%lu</Size>\r\n"
            "  <State>%s</State>\r\n"
            "  <Express Index=\"express.psf.cix.xml\" DeltaDir=\"$dpx$\" Apply=\"msdelta.dll\"/>\r\n"
            "</DownloadJob>\r\n",
            ulTask, sU.c_str(), sM.c_str(), ulTotal, pszState);
        if (nNeed > 0)
        {
            szXml.resize(static_cast<size_t>(nNeed) - 1);
            std::snprintf(&szXml[0], static_cast<size_t>(nNeed),
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
                "<DownloadJob Task=\"Dism++WusuDownload-%08lX\" Engine=\"WinINet-Range\">\r\n"
                "  <Url>%s</Url>\r\n"
                "  <Md5>%s</Md5>\r\n"
                "  <Size>%lu</Size>\r\n"
                "  <State>%s</State>\r\n"
                "  <Express Index=\"express.psf.cix.xml\" DeltaDir=\"$dpx$\" Apply=\"msdelta.dll\"/>\r\n"
                "</DownloadJob>\r\n",
                ulTask, sU.c_str(), sM.c_str(), ulTotal, pszState);
        }
    }
    static const char kBom[] = { static_cast<char>(0xEF), static_cast<char>(0xBB), static_cast<char>(0xBF) };
    return std::string(kBom, 3) + szXml;
}

} // namespace b3helper

// ----------------------------------------------------------
// [R-B76] 本地补丁安装（原生化：DismAddPackage 直接支持 .msu/.cab）
// ----------------------------------------------------------
bool UpdateMetaService::InstallLocalPackage(const wchar_t* pszPkg, const wchar_t* pszImageRoot,
                                            std::wstring* pErr)
{
    if (!pszPkg || !*pszPkg) { if (pErr) *pErr = L"路径为空"; return false; }
    if (::GetFileAttributesW(pszPkg) == INVALID_FILE_ATTRIBUTES)
    { if (pErr) *pErr = L"文件不存在"; return false; }

    std::wstring sPkg(pszPkg);
    bool bMsu = b3helper::EndsWithNoCase(sPkg, L".msu");
    bool bCab = b3helper::EndsWithNoCase(sPkg, L".cab");
    if (!bMsu && !bCab) { if (pErr) *pErr = L"仅支持 .msu / .cab"; return false; }

    // [原生化] DismAddPackage 原生接受 .cab 与 .msu 路径，无需 expand.exe
    // 展开，也无需 dism.exe /Add-Package 子进程。3010（需重启）在桥接层
    // 已按成功处理。
    DismApiBridge::Init();
    const wchar_t* pszRoot = (pszImageRoot && *pszImageRoot) ? pszImageRoot : nullptr;
    bool bOk = DismApiBridge::AddPackage(sPkg, pErr, pszRoot);
    if (!bOk && pErr && pErr->empty()) *pErr = L"DismAddPackage 失败";
    return bOk;
}

// ----------------------------------------------------------
// [R-B76] CBS 已安装包枚举（原生化：DismGetPackages 结构化结果）
// ----------------------------------------------------------
namespace {
// DismReleaseType → dism /English 等价字符串（仅用于界面展示，可空）
const wchar_t* DismReleaseTypeCnName(int n)
{
    switch (n)
    {
    case 1:  return L"Critical Update";
    case 2:  return L"Driver";
    case 3:  return L"Feature Pack";
    case 4:  return L"Hotfix";
    case 5:  return L"Security Update";
    case 6:  return L"Software Update";
    case 7:  return L"Update";
    case 8:  return L"Update Rollup";
    case 9:  return L"Language Pack";
    case 10: return L"Foundation";
    case 11: return L"Service Pack";
    case 12: return L"Product";
    case 13: return L"Localized Experience Pack";
    default: return L"Other";
    }
}
} // namespace

bool UpdateMetaService::EnumerateInstalledCbsPackages(const wchar_t* pszImageRoot,
                                                      std::vector<CbsPkgRecord>& vOut)
{
    vOut.clear();

    // [原生化] 替代 dism.exe /Get-Packages /Format:List 子进程与文本解析；
    // 离线会话定向到目标映像根（等价 /Image:"<root>"）。
    DismApiBridge::Init();
    std::vector<DismApiBridge::PkgInfo> vPkgs;
    std::wstring sErr;
    const wchar_t* pszRoot = (pszImageRoot && *pszImageRoot) ? pszImageRoot : nullptr;
    if (!DismApiBridge::GetPackages(vPkgs, &sErr, pszRoot))
        return false;

    // DismPackageFeatureState::DismStateInstalled == 2
    constexpr int kDismStateInstalled = 2;
    for (size_t i = 0; i < vPkgs.size(); ++i)
    {
        if (vPkgs[i].nState != kDismStateInstalled) continue;
        CbsPkgRecord rec;
        wcsncpy_s(rec.szIdentity, vPkgs[i].sName.c_str(), _TRUNCATE);
        wcsncpy_s(rec.szState, L"Installed", _TRUNCATE);
        wcsncpy_s(rec.szRelType,
                  DismReleaseTypeCnName(vPkgs[i].nReleaseType), _TRUNCATE);
        vOut.push_back(rec);
    }
    return true;
}

// ----------------------------------------------------------
// [R-B77] Range 断点续传
// ----------------------------------------------------------
bool UpdateMetaService::RangeDownloadFile(const wchar_t* pszUrl, const wchar_t* pszDst,
                                          const wchar_t* pszExpectedMd5,
                                          DWORD dwTimeoutMs)
{
    if (!pszUrl || !*pszUrl || !pszDst || !*pszDst) return false;

    std::wstring sUrl(pszUrl), sDst(pszDst);
    std::wstring sPending = sDst + L".pending";
    std::wstring sJob     = sDst + L".job.xml";
    std::wstring sMd5     = pszExpectedMd5 ? pszExpectedMd5 : L"";
    unsigned long ulTask  = b3helper::Fnv1a32(sUrl);

    // ① express 差异优先（失败/无资产 → 全量降级）
    {
        std::wstring sWhy;
        if (b3helper::TryExpressDelta(sDst, &sWhy))
        {
            if (sMd5.empty())
            {
                b3helper::WriteTextFile(sJob.c_str(),
                    b3helper::MakeJobXml(sUrl, sMd5, 0, "delta-ok", ulTask));
                return true;
            }
            const std::wstring sMd5File = b3helper::HashFileMd5(sDst.c_str());
            if (!sMd5File.empty() && lstrcmpiW(sMd5File.c_str(), sMd5.c_str()) == 0)
            {
                b3helper::WriteTextFile(sJob.c_str(),
                    b3helper::MakeJobXml(sUrl, sMd5, 0, "delta-ok-md5", ulTask));
                return true;
            }
            ::DeleteFileW(sDst.c_str());
        }
    }

    // ② 解析 URL
    URL_COMPONENTSW uc;
    uc = {};
    uc.dwStructSize = sizeof(uc);
    constexpr int kUrlHostWchars = 256;   // 256 缓冲尺寸（语义命名，数值不变）
    wchar_t szHost[kUrlHostWchars] = {};
    constexpr int kUrlPathWchars = 2048;   // 2048 缓冲尺寸（语义命名，数值不变）
    wchar_t szPath[kUrlPathWchars] = {};
    wchar_t szUser[128] = {};
    wchar_t szPwd[128] = {};
    uc.lpszHostName = szHost;  uc.dwHostNameLength = 255;
    uc.lpszUrlPath = szPath;   uc.dwUrlPathLength = 2047;
    uc.lpszUserName = szUser;  uc.dwUserNameLength = 127;
    uc.lpszPassword = szPwd;   uc.dwPasswordLength = 127;
    if (!::InternetCrackUrlW(sUrl.c_str(), 0, 0, &uc)) return false;

    InetGuard hNet = ::InternetOpenW(L"Dism++WusuDownload/10.0",
                                     INTERNET_OPEN_TYPE_PRECONFIG,
                                     nullptr, nullptr, 0);
    if (!hNet) return false;
    // 动态取 InternetSetTimeouts（规避目标 SDK 头版本差异）
    using PFN_BfSetTo = BOOL ( WINAPI*)(HINTERNET, int, DWORD, DWORD, DWORD, DWORD);
    PFN_BfSetTo pfnSetTo = reinterpret_cast<PFN_BfSetTo>(::GetProcAddress(
        ::GetModuleHandleW(L"wininet.dll"), "InternetSetTimeouts"));
    if (pfnSetTo) pfnSetTo(hNet, 2, dwTimeoutMs, dwTimeoutMs, dwTimeoutMs, dwTimeoutMs);

    InetGuard hCon = ::InternetConnectW(hNet, szHost, uc.nPort,
                                        szUser[0] ? szUser : nullptr,
                                        szPwd[0] ? szPwd : nullptr,
                                        INTERNET_SERVICE_HTTP, 0, 0);
    if (!hCon) { return false; }

    DWORD dwFlags = INTERNET_FLAG_KEEP_CONNECTION |
                    INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_RELOAD;
    if (uc.nScheme == INTERNET_SCHEME_HTTPS)
        dwFlags |= INTERNET_FLAG_SECURE;

    std::wstring sPathQ = szPath;
    if (szPath[0] == 0) sPathQ = L"/";

    InetGuard hReq = ::HttpOpenRequestW(hCon, L"GET", sPathQ.c_str(),
                                        nullptr, nullptr, nullptr, dwFlags, 0);
    if (!hReq)
    {
        return false;
    }

    // ③ 续传偏移：复用 .pending
    LONGLONG llHave = 0;
    WIN32_FIND_DATAW fdP;
    FindGuard hFindP = ::FindFirstFileW(sPending.c_str(), &fdP);
    if (hFindP != INVALID_HANDLE_VALUE)
    {
        LARGE_INTEGER li;
        li.HighPart = fdP.nFileSizeHigh;
        li.LowPart = fdP.nFileSizeLow;
        llHave = li.QuadPart;
    }

    std::wstring sRange;
    if (llHave > 0)
    {
        sRange.resize(128);
        wfmt::FormatInto(sRange.data(), sRange.size() + 1,
                     L"Range: bytes=%lld-\r\n", llHave);
        sRange.resize(wcslen(sRange.c_str()));
    }
    if (!::HttpSendRequestW(hReq, sRange.empty() ? nullptr : sRange.c_str(),
                            sRange.empty() ? 0 : static_cast<DWORD>(sRange.size()),
                            nullptr, 0))
    {
        return false;
    }

    DWORD dwStatus = 0, cbStatus = sizeof(dwStatus);
    if (!::HttpQueryInfoW(hReq,
            HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
            &dwStatus, &cbStatus, nullptr) ||
        (dwStatus != 200 && dwStatus != 206))
    {
        return false;
    }

    bool bResume = (dwStatus == 206 && llHave > 0);
    if (!bResume) llHave = 0;  // 200 → 全量重下（降级语义）

    // 总大小（Content-Length + 已有偏移；忽略 chunked 场景的预显示）
    LONGLONG llTotal = -1;
    {
        wchar_t szCl[64] = {};
        DWORD cbCl = sizeof(szCl);
        if (::HttpQueryInfoW(hReq, HTTP_QUERY_CONTENT_LENGTH,
                             szCl, &cbCl, nullptr))
            llTotal = _wtoi64(szCl) + llHave;
    }

    b3helper::WriteTextFile(sJob.c_str(),
        b3helper::MakeJobXml(sUrl, sMd5,
                             llTotal > 0 ? (unsigned long)llTotal : 0,
                             bResume ? "resuming" : "downloading", ulTask));

    FileGuard hFile = ::CreateFileW(sPending.c_str(),
                                 GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                 bResume ? OPEN_ALWAYS : CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    LARGE_INTEGER liSeek;
    liSeek.QuadPart = llHave;
    ::SetFilePointerEx(hFile, liSeek, nullptr, FILE_BEGIN);

    bool bOk = false;
    BYTE buf[64 * 1024];
    DWORD dwRead = 0;
    DWORD dwTick = ::GetTickCount();
    for (;;)
    {
        if (!::InternetReadFile(hReq, buf, sizeof(buf), &dwRead)) break;
        if (dwRead == 0) { bOk = true; break; }
        DWORD dwWritten = 0;
        if (!::WriteFile(hFile, buf, dwRead, &dwWritten, nullptr) || dwWritten != dwRead)
            break;
        if (::GetTickCount() - dwTick > dwTimeoutMs) break;
    }
    if (!bOk) return false;

    // ④ 原子落盘 + MD5
    ::DeleteFileW(sDst.c_str());
    if (!::MoveFileExW(sPending.c_str(), sDst.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return false;

    if (!sMd5.empty())
    {
        const std::wstring sMd5File = b3helper::HashFileMd5(sDst.c_str());
        if (sMd5File.empty() || lstrcmpiW(sMd5File.c_str(), sMd5.c_str()) != 0)
        {
            ::DeleteFileW(sDst.c_str());
            return false;
        }
    }

    LARGE_INTEGER liFinal;
    liFinal.QuadPart = 0;
    FileGuard hFin = ::CreateFileW(sDst.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, 0, nullptr);
    if (hFin != INVALID_HANDLE_VALUE)
    {
        LARGE_INTEGER liSize;
        if (::GetFileSizeEx(hFin, &liSize)) liFinal = liSize;
    }
    b3helper::WriteTextFile(sJob.c_str(),
        b3helper::MakeJobXml(sUrl, sMd5, (unsigned long)liFinal.QuadPart,
                             "done", ulTask));
    return true;
}

bool UpdateMetaService::DownloadWithResume(const wchar_t* pszUrl, const wchar_t* pszDst)
{
    // 主引擎：Range 断点续传；失败降级：PowerShell Invoke-WebRequest
    if (RangeDownloadFile(pszUrl, pszDst, nullptr, 300000)) return true;
    ::DeleteFileW((std::wstring(pszDst) + L".pending").c_str());
    return DownloadFile(pszUrl, pszDst);
}

// ----------------------------------------------------------
// [R-B78] 回传通道（Key / UpdateReport / CrashReport）
// ----------------------------------------------------------
static void BfEnsureDir(const std::wstring& sDir)
{
    if (sDir.size() < 3) return;
    for (size_t i = 3; i <= sDir.size(); ++i)
    {
        if (i == sDir.size() || sDir[i] == L'\\' || sDir[i] == L'/')
        {
            std::wstring sSub = sDir.substr(0, i);
            ::CreateDirectoryW(sSub.c_str(), nullptr);
        }
    }
}

static bool BfHttpPost(const std::wstring& sUrl, const std::string& sBody,
                       DWORD dwTimeoutMs)
{
    URL_COMPONENTSW uc;
    uc = {};
    uc.dwStructSize = sizeof(uc);
    constexpr int kUrlHostWchars = 256;   // 256 缓冲尺寸（语义命名，数值不变）
    wchar_t szHost[kUrlHostWchars] = {};
    constexpr int kUrlPathWchars = 2048;   // 2048 缓冲尺寸（语义命名，数值不变）
    wchar_t szPath[kUrlPathWchars] = {};
    uc.lpszHostName = szHost; uc.dwHostNameLength = 255;
    uc.lpszUrlPath = szPath;  uc.dwUrlPathLength = 2047;
    if (!::InternetCrackUrlW(sUrl.c_str(), 0, 0, &uc)) return false;

    InetGuard hNet = ::InternetOpenW(L"Dism++Report/10.0",
                                     INTERNET_OPEN_TYPE_PRECONFIG,
                                     nullptr, nullptr, 0);
    if (!hNet) return false;
    using PFN_BfSetTo = BOOL ( WINAPI*)(HINTERNET, int, DWORD, DWORD, DWORD, DWORD);
    PFN_BfSetTo pfnSetTo = reinterpret_cast<PFN_BfSetTo>(::GetProcAddress(
        ::GetModuleHandleW(L"wininet.dll"), "InternetSetTimeouts"));
    if (pfnSetTo) pfnSetTo(hNet, 2, dwTimeoutMs, dwTimeoutMs, dwTimeoutMs, dwTimeoutMs);
    InetGuard hCon = ::InternetConnectW(hNet, szHost, uc.nPort, nullptr, nullptr,
                                        INTERNET_SERVICE_HTTP, 0, 0);
    if (!hCon) { return false; }
    DWORD dwFlags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? INTERNET_FLAG_SECURE : 0;
    InetGuard hReq = ::HttpOpenRequestW(hCon, L"POST",
                                        szPath[0] ? szPath : L"/",
                                        nullptr, nullptr, nullptr, dwFlags, 0);
    if (!hReq)
    { return false; }

    BOOL bSent = ::HttpSendRequestW(hReq,
        L"Content-Type: application/xml; charset=utf-8\r\n",
        static_cast<DWORD>(wcslen(L"Content-Type: application/xml; charset=utf-8\r\n")),
        sBody.empty() ? nullptr : const_cast<void*>(static_cast<const void*>(sBody.data())),
        static_cast<DWORD>(sBody.size()));
    bool bOk = false;
    if (bSent)
    {
        DWORD dwStatus = 0, cb = sizeof(dwStatus);
        if (::HttpQueryInfoW(hReq, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                             &dwStatus, &cb, nullptr))
            bOk = (dwStatus >= 200 && dwStatus < 300);
    }
    return bOk;
}

static std::wstring BfEndpointForKind(const wchar_t* pszEndpoint, const wchar_t* pszKind)
{
    std::wstring sBase = pszEndpoint ? pszEndpoint : L"";
    // [R-B88] 默认上报站点暂时留空，后期在此填入新地址。
    //   原默认地址：http://update.chuyu.me/（未配置时事件仅入 outbox 本地队列，不联网）
    if (sBase.empty()) return std::wstring();
    if (sBase[sBase.size() - 1] != L'/') sBase += L"/";
    // 形如 http://host/Report/UpdateReport.aspx（服务端不存在时自动转 outbox）
    return sBase + L"Report/" + pszKind + L".aspx";
}

static void BfQueueOutbox(const wchar_t* pszOutboxDir, const wchar_t* pszKind,
                          const std::string& sBody)
{
    if (!pszOutboxDir || !*pszOutboxDir) return;
    std::wstring sDir(pszOutboxDir);
    BfEnsureDir(sDir);
    std::wstring sName;
    sName.resize(MAX_PATH);
    wfmt::FormatInto(sName.data(), sName.size() + 1,
                 L"%s\\%s_%lu.xml", sDir.c_str(), pszKind, ::GetTickCount());
    sName.resize(wcslen(sName.c_str()));
    b3helper::WriteTextFile(sName.c_str(), sBody);
}

bool UpdateMetaService::PostReportEvent(const wchar_t* pszEndpoint, const wchar_t* pszKind,
                                        const wchar_t* pszXml, const wchar_t* pszOutboxDir,
                                        DWORD dwTimeoutMs)
{
    if (!pszKind || !*pszKind) return false;
    std::string sBody;
    int cch = pszXml ? ::WideCharToMultiByte(CP_UTF8, 0, pszXml, -1, nullptr, 0,
                                             nullptr, nullptr) : 0;
    if (cch > 0)
    {
        sBody.resize(cch - 1);
        ::WideCharToMultiByte(CP_UTF8, 0, pszXml, -1, &sBody[0], cch, nullptr, nullptr);
    }
    if (sBody.empty()) sBody = "<Report/>";

    bool bOk = false;
    if (pszEndpoint && *pszEndpoint)
        bOk = BfHttpPost(BfEndpointForKind(pszEndpoint, pszKind), sBody, dwTimeoutMs);
    if (!bOk) BfQueueOutbox(pszOutboxDir, pszKind, sBody);
    return bOk;
}

bool UpdateMetaService::FlushReportOutbox(const wchar_t* pszEndpoint, const wchar_t* pszOutboxDir)
{
    if (!pszOutboxDir || !*pszOutboxDir) return false;
    std::wstring sDir(pszOutboxDir);
    bool bAll = true, bAny = false;
    WIN32_FIND_DATAW fd;
    FindGuard hFind = ::FindFirstFileW((sDir + L"\\*.xml").c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return true;
    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        bAny = true;
        std::wstring sFile = sDir + L"\\" + fd.cFileName;
        FileGuard h = ::CreateFileW(sFile.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) { bAll = false; continue; }
        LARGE_INTEGER li;
        std::string sBody;
        if (::GetFileSizeEx(h, &li) && li.QuadPart > 0 && li.QuadPart < 4 * 1024 * 1024)
        {
            sBody.resize(static_cast<size_t>(li.QuadPart));
            DWORD dw = 0;
            ::ReadFile(h, &sBody[0], static_cast<DWORD>(sBody.size()), &dw, nullptr);
            sBody.resize(dw);
        }

        // 从文件名解析 kind（Kind_tick.xml）
        std::wstring sKind(fd.cFileName);
        size_t pU = sKind.find(L'_');
        if (pU != std::wstring::npos) sKind = sKind.substr(0, pU);
        if (!sBody.empty() &&
            BfHttpPost(BfEndpointForKind(pszEndpoint, sKind.c_str()), sBody, 5000))
            ::DeleteFileW(sFile.c_str());
        else
            bAll = false;
    } while (::FindNextFileW(hFind, &fd));
    return bAny ? bAll : true;
}

// ---- 崩溃过滤器（写出 CrashReport XML 到 outbox，下次启动补发）----
static wchar_t g_szCrashOutbox[MAX_PATH] = {};

static LONG WINAPI BfCrashFilter(EXCEPTION_POINTERS* pEp)
{
    if (g_szCrashOutbox[0])
    {
        DWORD dwCode = pEp && pEp->ExceptionRecord
                     ? pEp->ExceptionRecord->ExceptionCode : 0;
        void* pAddr = pEp && pEp->ExceptionRecord
                    ? pEp->ExceptionRecord->ExceptionAddress : nullptr;
        SYSTEMTIME st;
        ::GetLocalTime(&st);
        wchar_t szFile[MAX_PATH];
        wfmt::FormatInto(szFile, _countof(szFile),  L"%s\\CrashReport_%lu.xml",
                  g_szCrashOutbox, ::GetTickCount());
        constexpr int kXmlBufWchars = 1024;   // 1024 缓冲尺寸（语义命名，数值不变）
        wchar_t szXml[kXmlBufWchars];
        wfmt::FormatInto(szXml, _countof(szXml),
            L"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
            L"<Report Kind=\"CrashReport\">\r\n"
            L"  <Time>%04u-%02u-%02u %02u:%02u:%02u</Time>\r\n"
            L"  <ExceptionCode>0x%08X</ExceptionCode>\r\n"
            L"  <ExceptionAddress>0x%p</ExceptionAddress>\r\n"
            L"</Report>\r\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
            dwCode, pAddr);
        std::string sBody;
        int cch = ::WideCharToMultiByte(CP_UTF8, 0, szXml, -1, nullptr, 0, nullptr, nullptr);
        if (cch > 0)
        {
            sBody.resize(cch - 1);
            ::WideCharToMultiByte(CP_UTF8, 0, szXml, -1, &sBody[0], cch, nullptr, nullptr);
        }
        BfEnsureDir(g_szCrashOutbox);
        b3helper::WriteTextFile(szFile, sBody);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void UpdateMetaService::InstallCrashReporter(const wchar_t* pszExeDir)
{
    if (!pszExeDir || !*pszExeDir) return;
    // 不覆盖已注册的过滤器（如 CBSHost 的反调试补丁链）。
    // 动态解析 GetUnhandledExceptionFilter（规避 SDK 头版本门槛）。
    using PFN_BfGetFilter = LPTOP_LEVEL_EXCEPTION_FILTER ( WINAPI*)(void);
    PFN_BfGetFilter pfnGet = reinterpret_cast<PFN_BfGetFilter>(::GetProcAddress(
        ::GetModuleHandleW(L"kernel32.dll"), "GetUnhandledExceptionFilter"));
    if (pfnGet && pfnGet() != nullptr)
        return;
    wfmt::FormatInto(g_szCrashOutbox, _countof(g_szCrashOutbox), L"%s\\Report\\outbox", pszExeDir);
    BfEnsureDir(g_szCrashOutbox);
    ::SetUnhandledExceptionFilter(BfCrashFilter);
}
