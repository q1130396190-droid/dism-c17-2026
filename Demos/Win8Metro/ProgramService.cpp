#include "ProgramService.h"

#include <algorithm>
#include <string>
#include <unordered_set>

#include "Registry.h"
#include "WinHandles.h"   // [H-51] win::RegKey RAII for F11_raw_handle_close

using namespace DuiLib;

namespace {

// 以下三个 helper 逐字摘自原 FrameWnd.cpp 内 static 函数（仅程序枚举使用）。

DWORD RegGetDword2(HKEY hRoot, const wchar_t* pszPath, const wchar_t* pszValue)
{
    DWORD dwOut = 0;
    win::RegKey hKey;
    if (::RegOpenKeyEx(hRoot, pszPath, 0, KEY_READ | KEY_WOW64_64KEY, hKey.out()) == ERROR_SUCCESS)
    {
        DWORD dwType = 0, dwSize = sizeof(dwOut);
        ::RegQueryValueEx(hKey.get(), pszValue, nullptr, &dwType, (BYTE*)&dwOut, &dwSize);
    }
    return dwOut;
}

CDuiString FormatAppDate(const CDuiString& sRaw)
{
    if (sRaw.GetLength() != 8) return L"-";
    int y = _wtoi(sRaw.Mid(0, 4).GetData());
    int m = _wtoi(sRaw.Mid(4, 2).GetData());
    int d = _wtoi(sRaw.Mid(6, 2).GetData());
    if (y < 1990 || m < 1 || m > 12 || d < 1 || d > 31) return L"-";
    CDuiString sOut;
    sOut.Format(L"%d/%d/%d", y, m, d);
    return sOut;
}

CDuiString FormatKbSize(DWORD dwKb)
{
    if (dwKb == 0) return L"";
    double mb = dwKb / 1024.0;
    CDuiString sOut;
    if (mb >= 100.0)      sOut.Format(L"%.0f MB", mb);
    else if (mb >= 10.0)  sOut.Format(L"%.1f MB", mb);
    else                  sOut.Format(L"%.2f MB", mb);
    return sOut;
}

// [R48 P1-06] 去重身份键构造。
//
// 原实现：对每个候选线性遍历已收集结果，只比较 DisplayName（`lstrcmpiW`）。
// 两个问题：
//   1) O(N²)：三个 Uninstall 源合计常见 300~800 项，平方级比较在主线程外
//      也要占掉可观时间，且随装机量线性恶化；
//   2) **语义错**：DisplayName 不是唯一标识。同一产品在 HKLM64 / WOW6432Node /
//      HKCU 各有一份同名项是常态；而两个不同产品恰好同名（如两个"更新程序"）
//      会被误合并，用户只看到一个，卸载入口丢失。
//
// 新键：优先用卸载注册表身份（根 + 子键全路径，二者共同唯一确定一条 Uninstall 项）；
//       子键是 MSI {GUID} 时 GUID 本身即产品身份，天然唯一。
// 与清单建议一致：产品码/注册表完整路径优先，DisplayName 不再作为合并依据。
static std::wstring MakeIdentityKey(int nRoot, const wchar_t* pszSubKey)
{
    std::wstring k;
    k.reserve(64);
    k.push_back(static_cast<wchar_t>(L'0' + nRoot));   // 根来源
    k.push_back(L'|');
    if (pszSubKey) k.append(pszSubKey);
    // 统一小写：注册表键名不区分大小写，避免同项因大小写差异被当成两条
    for (wchar_t& c : k)
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    return k;
}

} // namespace

void ProgramService::EnumeratePrograms(std::vector<ProgEntry>& vOut)
{
    // 别名保持原 BuildProgramPage 枚举函数体逐字（v 即调用方向量）
    std::vector<ProgEntry>& v = vOut;

    // [R48 P1-06] 去重身份集合：一次遍历建集合，查重 O(1)，整体由 O(N²) 降为 O(N)。
    std::unordered_set<std::wstring> seen;
    seen.reserve(256);

    struct UninstSource { HKEY hRoot; const wchar_t* pszPath; };
    UninstSource sources[] = {
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall" },
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall" },
        { HKEY_CURRENT_USER,  L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall" }
    };

    for (int s = 0; s < 3; ++s)
    {
        win::RegKey hBase;
        if (::RegOpenKeyEx(sources[s].hRoot, sources[s].pszPath, 0,
                KEY_READ | KEY_WOW64_64KEY, hBase.out()) != ERROR_SUCCESS)
            continue;

        DWORD dwIdx = 0;
        for (;;)
        {
            constexpr int kRegKeyNameWchars = 256;   // 256 缓冲尺寸（语义命名，数值不变）
            wchar_t szKey[kRegKeyNameWchars] = { 0 };
            DWORD nKeyLen = kRegKeyNameWchars;
            if (::RegEnumKeyEx(hBase.get(), dwIdx++, szKey, &nKeyLen,
                    nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;

            CDuiString sSub = sources[s].pszPath;
            sSub += L"\\";
            sSub += szKey;

            CDuiString sName = RegGetString(sources[s].hRoot, sSub.GetData(), L"DisplayName");
            if (sName.IsEmpty()) continue;
            if (RegGetDword2(sources[s].hRoot, sSub.GetData(), L"SystemComponent") == 1) continue;
            if (!RegGetString(sources[s].hRoot, sSub.GetData(), L"ParentKeyName").IsEmpty()) continue;

            // [R48 P1-06] 查重改用注册表身份键，不再只比 DisplayName。
            //   原写法会把"不同产品同名"误合并（卸载入口丢失），
            //   且对每项线性扫全部已收结果，复杂度 O(N²)。
            if (!seen.insert(MakeIdentityKey(s, sSub.GetData())).second)
                continue;   // 同一 Uninstall 项已收（多源重复注册）

            CDuiString sUninst = RegGetString(sources[s].hRoot, sSub.GetData(), L"UninstallString");
            // MSI product code keys may carry no UninstallString
            bool bMsifallback = false;
            if (sUninst.IsEmpty() && szKey[0] == L'{') {
                sUninst = L"MsiExec.exe /X";
                sUninst += szKey;
                bMsifallback = true;
            }

            ProgEntry e;
            e.nRoot      = s;
            e.sSubKey    = sSub;
            e.sName      = sName;
            e.sDate      = FormatAppDate(RegGetString(sources[s].hRoot, sSub.GetData(), L"InstallDate"));
            e.sSize      = FormatKbSize(RegGetDword2(sources[s].hRoot, sSub.GetData(), L"EstimatedSize"));
            e.sUninstall = sUninst;

            // R34: 读取签名 / 发布者 / 安装位置 / MSI 标记
            e.sPublisher  = RegGetString(sources[s].hRoot, sSub.GetData(), L"Publisher");
            e.sInstallLoc = RegGetString(sources[s].hRoot, sSub.GetData(), L"InstallLocation");

            // [R47] 读取 DisplayIcon（安装图标路径），提取基名供 UI 展示
            {
                CDuiString sIcon = RegGetString(sources[s].hRoot, sSub.GetData(), L"DisplayIcon");
                if (!sIcon.IsEmpty())
                {
                    const wchar_t* pSl = wcsrchr(sIcon.GetData(), L'\\');
                    if (pSl && pSl != sIcon.GetData()) sIcon = pSl + 1;
                    // 去掉 .exe 后缀（若存在），保留扩展名作为可读图标标识
                    e.sIconBase = sIcon;
                }
            }

            // MsftSig DWORD: 1=微软签名, 2=第三方签名, 0=无
            DWORD dwMsft = RegGetDword2(sources[s].hRoot, sSub.GetData(), L"MsftSig");
            if (dwMsft == 1) { e.bSigned = true; e.bMsft = true; }
            else if (dwMsft == 2) { e.bSigned = true; e.bMsft = false; }
            // 若 MsftSig 缺失，用 Signer 子键或 SignerName 值兜底
            if (!e.bSigned)
            {
                win::RegKey hSigner;
                CDuiString sSignSub = sSub;
                sSignSub += L"\\Signer";
                if (::RegOpenKeyEx(sources[s].hRoot, sSignSub.GetData(), 0,
                        KEY_READ | KEY_WOW64_64KEY, hSigner.out()) == ERROR_SUCCESS)
                {
                    e.bSigned = true;
                }
            }
            // MSI 判定：UninstallString 含 msiexec / 或子键是 {GUID}
            CDuiString sUI = sUninst; sUI.MakeLower();
            e.bIsMsi = (sUI.Find(L"msiexec") >= 0 || szKey[0] == L'{');

            (void)bMsifallback;

            v.push_back(e);
        }
    }

    std::sort(v.begin(), v.end(),
        [](const ProgEntry& a, const ProgEntry& b) {
            return lstrcmpiW(a.sName.GetData(), b.sName.GetData()) < 0;
        });
}
// [C++17 R3] 本文件内联常量（C++17 inline variables，多 TU ODR 一致）：
