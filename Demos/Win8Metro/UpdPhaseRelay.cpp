#include "stdafx.h"

#include "UpdPhaseRelay.h"


#include "FrameWnd_Util.h"      // FrameWnd_Util_GetTempDir

#include <cstdio>
#include <vector>
#include <string>   // [R-B51] std::string
// [C++17 阶段2] 局部 RAII 守卫：文件/事件句柄（见 WinHandles.h 使用规范）
struct URHandleGuard
{
    HANDLE h;
    explicit URHandleGuard(HANDLE x) : h(x) {}
    ~URHandleGuard() { if (h && h != INVALID_HANDLE_VALUE) ::CloseHandle(h); }
    URHandleGuard(const URHandleGuard&) = delete;
    URHandleGuard& operator=(const URHandleGuard&) = delete;
    operator HANDLE() const { return h; }
};

// ============================================================
// [R-B51] 升级链阶段回传通道实现
// ------------------------------------------------------------
// 子进程：
//   WritePhase —— 写 UTF-8 单 BOM INI 状态文件 + set 命名事件
//   SignalEvent —— 仅 set 命名事件（唤醒主进程）
// 主进程：
//   WaitSignal —— 等待命名事件（超时返回 false）
//   ReadPhase  —— 读状态文件并解析 9 态 / 错误码 / 重试 / 消息
//   MakePhaseFile —— 生成 temp\updphase_<phase>_<pid>.ini
// ============================================================

namespace {

// 命名事件创建/打开
HANDLE OpenOrCreateUpdEvent()
{
    return ::CreateEventW(nullptr, TRUE, FALSE, kUpdPhaseEventName);
}

// UTF-8 BOM 字符串
const char* kBom = "\xEF\xBB\xBF";

// 写 UTF-8 BOM + CRLF 文本到文件；失败返回 false
bool WriteBomUtf8(const wchar_t* iniPath, const char* pszBody)
{
    if (!iniPath || !iniPath[0] || !pszBody) return false;

    // 转 UTF-8
    int nW = (int)::MultiByteToWideChar(CP_ACP, 0, pszBody, -1, nullptr, 0);
    if (nW <= 0) return false;
    std::vector<wchar_t> vW(nW);
    ::MultiByteToWideChar(CP_ACP, 0, pszBody, -1, &vW[0], nW);

    int nU = ::WideCharToMultiByte(CP_UTF8, 0, &vW[0], nW - 1,
                                   nullptr, 0, nullptr, nullptr);
    if (nU <= 0) return false;
    std::vector<char> vU(nU);
    ::WideCharToMultiByte(CP_UTF8, 0, &vW[0], nW - 1, &vU[0], nU,
                          nullptr, nullptr);

    URHandleGuard h = URHandleGuard(::CreateFileW(iniPath, GENERIC_WRITE, FILE_SHARE_READ,
                             nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr));
    if (h == INVALID_HANDLE_VALUE) return false;

    DWORD wb = 0;
    bool bOk = true;
    ::WriteFile(h, kBom, 3, &wb, nullptr);
    if (static_cast<DWORD>(wb) != 3) bOk = false;
    ::WriteFile(h, &vU[0], static_cast<DWORD>(nU), &wb, nullptr);
    if (static_cast<DWORD>(wb) != static_cast<DWORD>(nU)) bOk = false;
    return bOk;
}

// 读 UTF-8 文件全文（剥 BOM），失败返回 false
bool ReadAllUtf8(const wchar_t* iniPath, std::string& sOut)
{
    if (!iniPath || !iniPath[0]) return false;
    URHandleGuard h = URHandleGuard(::CreateFileW(iniPath, GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr));
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER li = {};
    ::GetFileSizeEx(h, &li);
    if (li.QuadPart <= 0) { return false; }  // URHandleGuard 自动关闭
    if (li.QuadPart > 4 * 1024 * 1024) { return false; }  // URHandleGuard 自动关闭

    std::vector<char> v(static_cast<size_t>(li.QuadPart));
    DWORD rd = 0;
    bool bOk = (::ReadFile(h, &v[0], static_cast<DWORD>(v.size()), &rd, nullptr) && rd > 0);
    if (!bOk) return false;

    size_t start = 0;
    if (v.size() >= 3 && (unsigned char)v[0] == 0xEF
        && (unsigned char)v[1] == 0xBB
        && (unsigned char)v[2] == 0xBF)
        start = 3;

    sOut.assign(v.begin() + (std::ptrdiff_t)start, v.end());
    return !sOut.empty();
}

// 取 [section] key=value（不区分大小写，CRLF/LF 都识别）
bool ReadIniKey(const std::string& sBody, const wchar_t* pszSection,
                const wchar_t* pszKey, std::string& sVal)
{
    // UNICODE 下 const wchar_t*=wchar_t*，先转窄字符 ASCII 键名
    std::string sec, key;
    for (const wchar_t* p = pszSection; p && *p; ++p)
        sec.push_back((char)(*p));
    for (const wchar_t* p = pszKey; p && *p; ++p)
        key.push_back((char)(*p));
    sec = "[" + sec + "]";
    // 小写化 section/key
    for (auto& c : sec) c = static_cast<char>(tolower((unsigned char)c));
    for (auto& c : key) c = static_cast<char>(tolower((unsigned char)c));

    bool bInSec = false;
    size_t pos = 0;
    while (pos < sBody.size())
    {
        size_t eol = sBody.find('\n', pos);
        if (eol == std::string::npos) eol = sBody.size();
        std::string line = sBody.substr(pos, eol - pos);
        pos = eol + 1;
        // 去 \r
        while (!line.empty() && (line.back() == '\r'
              || line.back() == '\n' || line.back() == ' '
              || line.back() == '\t'))
            line.pop_back();
        while (!line.empty() && (line.front() == ' '
              || line.front() == '\t'))
            line.erase(line.begin());

        if (line.empty()) continue;
        if (line.front() == '[' && line.back() == ']')
        {
            std::string s = line;
            for (auto& c : s) c = static_cast<char>(tolower((unsigned char)c));
            bInSec = (s == sec);
            continue;
        }
        if (!bInSec) continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq);
        for (auto& c : k) c = static_cast<char>(tolower((unsigned char)c));
        if (k != key) continue;
        sVal = line.substr(eq + 1);
        return true;
    }
    return false;
}

// UTF-8 → 宽字符
bool Utf8ToWide(const std::string& sIn, DuiLib::CDuiString& sOut)
{
    if (sIn.empty()) { sOut = L""; return true; }
    int nW = ::MultiByteToWideChar(CP_UTF8, 0, sIn.c_str(),
                                   static_cast<int>(sIn.size()), nullptr, 0);
    if (nW <= 0) return false;
    std::vector<wchar_t> v(nW + 1);
    ::MultiByteToWideChar(CP_UTF8, 0, sIn.c_str(), static_cast<int>(sIn.size()),
                          &v[0], nW);
    v[nW] = 0;
    sOut = v.data();
    return true;
}

} // namespace

// ============================================================
bool UpdPhaseRelay::WritePhase(const wchar_t* iniPath, int nPhase, int nState,
                               int nErrorCode, int bRetry,
                               const wchar_t* szMsg)
{
    constexpr int kBodyBufBytes = 2048;   // 2048 缓冲尺寸（语义命名，数值不变）
    char szBody[kBodyBufBytes] = {};
    const wchar_t* pMsg = szMsg ? szMsg : L"";
    // 消息直接宽→UTF-8（截断保护）
    int nW = (int)::lstrlenW(pMsg);
    if (nW > 300) nW = 300;   // 截断长消息（避免状态文件过大）
    int nU = ::WideCharToMultiByte(CP_UTF8, 0, pMsg, nW,
                                   nullptr, 0, nullptr, nullptr);
    std::string sMsgU;
    if (nU > 0 && nU < 1000)
    {
        std::vector<char> vU(nU);
        ::WideCharToMultiByte(CP_UTF8, 0, pMsg, nW, &vU[0], nU,
                              nullptr, nullptr);
        sMsgU.assign(vU.begin(), vU.end());
    }
    else sMsgU = "(msg too long)";

    sprintf_s(szBody, _countof(szBody), "[UpdPhase]\r\n"
        "Phase=%d\r\n"
        "State=%d\r\n"
        "ErrorCode=%d\r\n"
        "Retry=%d\r\n"
        "Msg=%s\r\n",
        nPhase, nState, nErrorCode, bRetry ? 1 : 0,
        sMsgU.c_str());

    bool bOk = WriteBomUtf8(iniPath, szBody);
    SignalEvent();
    return bOk;
}

bool UpdPhaseRelay::SignalEvent()
{
    URHandleGuard h(OpenOrCreateUpdEvent());
    if (!h) return false;
    ::SetEvent(h);
    return true;
}

bool UpdPhaseRelay::WaitSignal(DWORD dwTimeoutMs)
{
    URHandleGuard h(::OpenEventW(SYNCHRONIZE, FALSE, kUpdPhaseEventName));
    if (!h) return false;
    DWORD dw = ::WaitForSingleObject(h, dwTimeoutMs);
    return (dw == WAIT_OBJECT_0);
}

bool UpdPhaseRelay::ReadPhase(const wchar_t* iniPath,
                              int& nState, int& nErrorCode,
                              int& bRetry, DuiLib::CDuiString& sMsg)
{
    std::string sBody;
    if (!ReadAllUtf8(iniPath, sBody)) return false;

    std::string sVal;
    if (ReadIniKey(sBody, L"UpdPhase", L"Phase", sVal)) {}
    if (ReadIniKey(sBody, L"UpdPhase", L"State", sVal))
        nState = std::stoi(sVal);
    if (ReadIniKey(sBody, L"UpdPhase", L"ErrorCode", sVal))
        nErrorCode = std::stoi(sVal);
    if (ReadIniKey(sBody, L"UpdPhase", L"Retry", sVal))
        bRetry = std::stoi(sVal);
    if (ReadIniKey(sBody, L"UpdPhase", L"Msg", sVal))
        Utf8ToWide(sVal, sMsg);
    else
        sMsg = L"";
    return true;
}

DuiLib::CDuiString UpdPhaseRelay::MakePhaseFile(int nPhase)
{
    DuiLib::CDuiString sDir = FrameWnd_Util_GetTempDir();
    DuiLib::CDuiString s;
    s.Format(L"%s\\updphase_%d_%u.ini", sDir.GetData(), nPhase,
             (unsigned)::GetCurrentProcessId());
    return s;
}
// [C++17 R3] 本文件内联常量（C++17 inline variables，多 TU ODR 一致）：
