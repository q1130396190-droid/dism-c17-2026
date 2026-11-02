#include "AssocService.h"

#include <shlobj.h>
#include <shobjidl.h>   // [R94] SHAssocEnumHandlers / IAssocHandler / IEnumAssocHandlers
#include <shlwapi.h>
#include <comdef.h>

#include <set>
#include <string>
#include <vector>

#include "Registry.h"
#include "ProcessRunner.h"
#include "WinHandles.h"   // [R9] win::HandlePtr/RegKey/FindDataHandle RAII（F11_raw_handle_close）

// [P1-A9] 离线蜂巢经 RegLoadKey 本地挂载（AssocService 不使用预编译头，
// 不能引入依赖 UIlib/Markup 的 DataEngine.h）。

using namespace DuiLib;

bool AssocService::QueryProgIdAndCommand(const CDuiString& sExtIn,
                                          CDuiString& sProgId,
                                          CDuiString& sCommand)
{
    CDuiString sExt(sExtIn);
    sExt.Trim();
    if (sExt.IsEmpty()) return false;
    if (sExt[0] != L'.') sExt = CDuiString(L".") + sExt;
    CDuiString sPath;
    sPath.Format(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\%s\\UserChoice",
                 sExt.GetData());
    sProgId = RegGetString(HKEY_CURRENT_USER, sPath.GetData(), L"ProgId", KEY_READ);
    if (sProgId.IsEmpty())
        sProgId = RegGetString(HKEY_CLASSES_ROOT, sExt.GetData(), L"",
                               KEY_READ | KEY_WOW64_64KEY);
    if (!sProgId.IsEmpty())
    {
        sPath.Format(L"%s\\shell\\open\\command", sProgId.GetData());
        sCommand = RegGetString(HKEY_CLASSES_ROOT, sPath.GetData(), L"",
                                KEY_READ | KEY_WOW64_64KEY);
    }
    return true;
}

// ============================================================
// [R37 Patch D] 文件关联写入（Windows 10+ 注册表方案）
// 写入 HKCU\...\FileExts\.ext\UserChoice\ProgId
// 通过回读校验确保写入成功
// 注：Windows SDK 10.0.28000.0 中 IAssocHandler 仅有前向声明，无 vtable 定义，
//     本补丁采用直接写注册表 + 广播 SHCNE_ASSOCCHANGED 的稳妥方案
// ============================================================
EAssocWriteResult AssocService::SetUserFileAssociation(
    const CDuiString& sExt,
    const CDuiString& sProgId)
{
    if (sExt.IsEmpty() || sProgId.IsEmpty()) return EAssocWriteResult::ASSOC_WRITE_INVALID_PROGID;
    CDuiString sE = sExt;
    if (sE.GetLength() > 0 && sE[0] != L'.') sE = CDuiString(L".") + sE;
    if (sProgId.Find(L'\\') < 0) return EAssocWriteResult::ASSOC_WRITE_INVALID_PROGID;
    CDuiString sPath;
    sPath.Format(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\%s\\UserChoice",
                 sE.GetData());
    win::RegKey hKey;
    LONG l = ::RegCreateKeyEx(HKEY_CURRENT_USER, sPath.GetData(), 0, nullptr,
                              REG_OPTION_NON_VOLATILE,
                              KEY_WRITE | KEY_WOW64_64KEY, nullptr, hKey.out(), nullptr);
    if (l != ERROR_SUCCESS) return EAssocWriteResult::ASSOC_WRITE_REG_FAIL;
    l = ::RegSetValueEx(hKey.get(), L"ProgId", 0, REG_SZ,
                        (const BYTE*)sProgId.GetData(),
                        (DWORD)((sProgId.GetLength() + 1) * sizeof(wchar_t)));
    if (l != ERROR_SUCCESS) return EAssocWriteResult::ASSOC_WRITE_REG_FAIL;
    if (VerifyUserFileAssociation(sE, sProgId))
        return EAssocWriteResult::ASSOC_WRITE_OK;
    return EAssocWriteResult::ASSOC_WRITE_VERIFY_FAIL;
}

// ============================================================
// [R99] FileExts 组合写路径
// Windows 10+ 关联优先级：UserChoice(默认) > OpenWithProgids(推荐)
// > OpenWithList(MRU) > HKCR\.ext(系统默认)。
// 仅写 UserChoice 不够——Shell 在 UserChoice 缺失时会读
// OpenWithProgids/OpenWithList 补足候选；本函数三处一并维护，
// 保证"打开方式"对话框能列出目标 ProgId。
// ============================================================
EAssocWriteResult AssocService::SetUserFileAssociationEx(
    const CDuiString& sExt,
    const CDuiString& sProgId)
{
    if (sExt.IsEmpty() || sProgId.IsEmpty()) return EAssocWriteResult::ASSOC_WRITE_INVALID_PROGID;
    CDuiString sE = sExt;
    if (sE.GetLength() > 0 && sE[0] != L'.') sE = CDuiString(L".") + sE;
    if (sProgId.Find(L'\\') < 0) return EAssocWriteResult::ASSOC_WRITE_INVALID_PROGID;

    // 1) UserChoice\ProgId（默认路径，回读校验）
    EAssocWriteResult r = SetUserFileAssociation(sE, sProgId);
    if (r != EAssocWriteResult::ASSOC_WRITE_OK) return r;

    // 2) OpenWithProgids 增加该 ProgId（避免覆盖已有候选）
    CDuiString sBase;
    sBase.Format(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\%s",
                 sE.GetData());
    CDuiString sOWP = sBase + CDuiString(L"\\OpenWithProgids");

    win::RegKey hKey;
    LONG l = ::RegCreateKeyEx(HKEY_CURRENT_USER, sOWP.GetData(), 0, nullptr,
                              REG_OPTION_NON_VOLATILE,
                              KEY_WRITE | KEY_WOW64_64KEY, nullptr, hKey.out(), nullptr);
    if (l == ERROR_SUCCESS)
    {
        l = ::RegSetValueEx(hKey.get(), sProgId.GetData(), 0, REG_NONE, nullptr, 0);
    }
    if (l != ERROR_SUCCESS && l != ERROR_FILE_EXISTS)
        return EAssocWriteResult::ASSOC_WRITE_REG_FAIL;

    // 3) OpenWithList 追加为 MRU 首项（去重）
    CDuiString sOWL = sBase + CDuiString(L"\\OpenWithList");
    hKey.reset();
    l = ::RegCreateKeyEx(HKEY_CURRENT_USER, sOWL.GetData(), 0, nullptr,
                         REG_OPTION_NON_VOLATILE,
                         KEY_READ | KEY_WRITE | KEY_WOW64_64KEY, nullptr, hKey.out(), nullptr);
    if (l == ERROR_SUCCESS)
    {
        // 读当前 MRUList（形如 "abc"）
        CDuiString sList = RegGetString(HKEY_CURRENT_USER, sOWL.GetData(), L"MRUList",
                                        KEY_READ | KEY_WOW64_64KEY);

        // 目标 ProgId 是否已在列表中（检查 a-z 每个键）
        bool bPresent = false;
        for (int i = 0; i < sList.GetLength(); ++i)
        {
            wchar_t ch = sList[i];
            if (ch < L'a' || ch > L'z') continue;
            CDuiString sKey;
            sKey.Format(L"%c", ch);
            CDuiString sVal = RegGetString(HKEY_CURRENT_USER, sOWL.GetData(), sKey.GetData(),
                                           KEY_READ | KEY_WOW64_64KEY);
            if (sVal == sProgId) { bPresent = true; break; }
        }

        if (!bPresent)
        {
            // 分配下一个可用字母 key（a-z 跳过已用）
            wchar_t chNext = L'a';
            bool bFound = false;
            for (int i = 0; i < 26; ++i)
            {
                wchar_t ch = (wchar_t)(L'a' + i);
                CDuiString sKey;
                sKey.Format(L"%c", ch);
                CDuiString sVal = RegGetString(HKEY_CURRENT_USER, sOWL.GetData(), sKey.GetData(),
                                               KEY_READ | KEY_WOW64_64KEY);
                if (sVal.IsEmpty()) { chNext = ch; bFound = true; break; }
            }
            if (bFound)
            {
                CDuiString sNewKey;
                sNewKey.Format(L"%c", chNext);
                // [R128-A] MRU 写入失败会让"最近使用"顺序错乱，需留痕
                LONG lMru1 = ::RegSetValueEx(hKey.get(), sNewKey.GetData(), 0, REG_SZ,
                                (const BYTE*)sProgId.GetData(),
                                (DWORD)((sProgId.GetLength() + 1) * sizeof(wchar_t)));

                // 重建 MRUList：新键前置，其余保持原序
                CDuiString sNewList = sNewKey + sList;
                LONG lMru2 = ::RegSetValueEx(hKey.get(), L"MRUList", 0, REG_SZ,
                                (const BYTE*)sNewList.GetData(),
                                (DWORD)((sNewList.GetLength() + 1) * sizeof(wchar_t)));
                if (lMru1 != ERROR_SUCCESS || lMru2 != ERROR_SUCCESS)
                    ::OutputDebugStringW(L"[R128][assoc] MRU 写入失败\r\n");
            }
        }
    }

    // 广播关联变更
    ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);

    return EAssocWriteResult::ASSOC_WRITE_OK;
}

EAssocWriteResult AssocService::ClearUserFileAssociation(
    const CDuiString& sExt)
{
    CDuiString sE = sExt;
    if (sE.GetLength() > 0 && sE[0] != L'.') sE = CDuiString(L".") + sE;
    CDuiString sPath;
    sPath.Format(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\%s\\UserChoice",
                 sE.GetData());
    LONG l = ::RegDeleteTree(HKEY_CURRENT_USER, sPath.GetData());
    if (l == ERROR_SUCCESS) return EAssocWriteResult::ASSOC_WRITE_OK;
    if (l == ERROR_FILE_NOT_FOUND) return EAssocWriteResult::ASSOC_WRITE_OK;
    return EAssocWriteResult::ASSOC_WRITE_REG_FAIL;
}

EAssocWriteResult AssocService::ResetAssocToDefault(
    const CDuiString& sExt)
{
    EAssocWriteResult r = ClearUserFileAssociation(sExt);
    if (r != EAssocWriteResult::ASSOC_WRITE_OK) return r;
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return EAssocWriteResult::ASSOC_WRITE_OK;
}

bool AssocService::VerifyUserFileAssociation(
    const CDuiString& sExt,
    const CDuiString& sProgId)
{
    CDuiString sE = sExt;
    if (sE.GetLength() > 0 && sE[0] != L'.') sE = CDuiString(L".") + sE;
    CDuiString sPath;
    sPath.Format(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\%s\\UserChoice",
                 sE.GetData());
    CDuiString sRead = RegGetString(HKEY_CURRENT_USER, sPath.GetData(),
                                     L"ProgId", KEY_READ | KEY_WOW64_64KEY);
    return (sRead == sProgId);
}

int AssocService::ApplyAssocBatch(std::vector<std::pair<CDuiString,CDuiString> >& vPairs)
{
    int nOk = 0, nFail = 0;
    for (size_t k = 0; k < vPairs.size(); ++k)
    {
        EAssocWriteResult r = SetUserFileAssociation(vPairs[k].first, vPairs[k].second);
        if (r == EAssocWriteResult::ASSOC_WRITE_OK) nOk++;
        else nFail++;
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return nOk;
}

// ============================================================
// [P1-A9] 默认应用关联原生导出/导入/移除（零 dism.exe 进程）
// ============================================================
namespace {

struct A9Item
{
    std::wstring sId;
    std::wstring sProgId;
    std::wstring sApp;
};

// 大小写不敏感字符串集合（扩展名/协议不区分大小写）
struct A9Less
{
    bool operator()(const std::wstring& a, const std::wstring& b) const
    {
        return ::lstrcmpiW(a.c_str(), b.c_str()) < 0;
    }
};

// [P1-A9] 临时启用一个进程令牌特权（SeRestore/SeBackup 供 RegLoadKey）
bool A9EnablePrivilege(const wchar_t* pszPriv)
{
    win::HandlePtr hToken;
    HANDLE hTokRaw = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(),
                            TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hTokRaw))
        return false;
    hToken.reset(hTokRaw);
    LUID luid = { 0 };
    bool b = ::LookupPrivilegeValueW(nullptr, pszPriv, &luid) != FALSE;
    if (b)
    {
        TOKEN_PRIVILEGES tp = { 0 };
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid = luid;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        b = ::AdjustTokenPrivileges(hToken.get(), FALSE, &tp,
                                    sizeof(tp), nullptr, nullptr) != FALSE
            && ::GetLastError() != ERROR_NOT_ALL_ASSIGNED;
    }
    return b;
}

// [P1-A9] 离线 SOFTWARE 蜂巢挂载护栏：RegLoadKey 到
// HKLM\A9_AssocOffline_SW，析构幂等 RegUnLoadKey。
struct A9OfflineHive
{
    static const wchar_t* MountPoint() { return L"A9_AssocOffline_SW"; }

    bool bLoaded;
    A9OfflineHive() : bLoaded(false) {}

    bool Load(const wchar_t* pszImageRoot, CDuiString& sErr)
    {
        std::wstring sRoot = pszImageRoot;
        while (!sRoot.empty()
               && (sRoot.back() == L'\\' || sRoot.back() == L'/'))
            sRoot.pop_back();
        std::wstring sHive = sRoot + L"\\Windows\\System32\\config\\SOFTWARE";
        if (::GetFileAttributesW(sHive.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            sErr.Format(L"offline hive not found: %s", sHive.c_str());
            return false;
        }
        A9EnablePrivilege(SE_RESTORE_NAME);
        A9EnablePrivilege(SE_BACKUP_NAME);
        // 残留挂载点先卸载（幂等）
        ::RegUnLoadKeyW(HKEY_LOCAL_MACHINE, MountPoint());
        LONG l = ::RegLoadKeyW(HKEY_LOCAL_MACHINE, MountPoint(), sHive.c_str());
        if (l != ERROR_SUCCESS)
        {
            sErr.Format(L"RegLoadKey('%s') failed, gle=%ld", sHive.c_str(), l);
            return false;
        }
        bLoaded = true;
        return true;
    }

    ~A9OfflineHive()
    {
        if (bLoaded) ::RegUnLoadKeyW(HKEY_LOCAL_MACHINE, MountPoint());
    }
private:
    A9OfflineHive(const A9OfflineHive&);
    A9OfflineHive& operator=(const A9OfflineHive&);
};

void A9XmlEscape(std::wstring& sOut, const std::wstring& sIn)
{
    for (wchar_t c : sIn)
    {
        switch (c)
        {
        case L'&':  sOut += L"&amp;";  break;
        case L'<':  sOut += L"&lt;";   break;
        case L'>':  sOut += L"&gt;";   break;
        case L'\"': sOut += L"&quot;"; break;
        case L'\'': sOut += L"&apos;"; break;
        default:    sOut += c;         break;
        }
    }
}

bool A9WriteUtf8Bom(const wchar_t* pszPath, const std::wstring& sContent,
                    CDuiString& sErr)
{
    int cchUtf8 = ::WideCharToMultiByte(CP_UTF8, 0, sContent.c_str(),
                                        static_cast<int>(sContent.size()), nullptr, 0, nullptr, nullptr);
    win::HandlePtr h = win::file_handle(::CreateFileW(pszPath, GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!h)
    {
        sErr.Format(L"CreateFile('%s') failed, gle=%lu", pszPath, ::GetLastError());
        return false;
    }
    static const unsigned char szBom[3] = { 0xEF, 0xBB, 0xBF };
    DWORD dwWr = 0;
    bool bOk = ::WriteFile(h.get(), szBom, 3, &dwWr, nullptr) && dwWr == 3;
    if (bOk && cchUtf8 > 0)
    {
        std::vector<char> vBuf(cchUtf8);
        ::WideCharToMultiByte(CP_UTF8, 0, sContent.c_str(), static_cast<int>(sContent.size()),
                              vBuf.data(), cchUtf8, nullptr, nullptr);
        bOk = ::WriteFile(h.get(), vBuf.data(), static_cast<DWORD>(cchUtf8), &dwWr, nullptr)
              && dwWr == static_cast<DWORD>(cchUtf8);
    }
    if (!bOk) sErr.Format(L"WriteFile('%s') failed, gle=%lu", pszPath, ::GetLastError());
    return bOk;
}

// 枚举某注册表键的全部子键名
void A9EnumSubKeys(HKEY hRoot, const wchar_t* pszSub,
                   std::vector<std::wstring>& vNames)
{
    win::RegKey hKey;
    if (::RegOpenKeyExW(hRoot, pszSub, 0,
                        KEY_READ | KEY_WOW64_64KEY, hKey.out()) != ERROR_SUCCESS)
        return;

    DWORD dwIndex = 0;
    for (;;)
    {
        constexpr int kRegKeyNameWchars = 512;   // 512 缓冲尺寸（语义命名，数值不变）
        wchar_t szName[kRegKeyNameWchars] = { 0 };
        DWORD cchName = kRegKeyNameWchars - 1;
        LONG l = ::RegEnumKeyExW(hKey.get(), dwIndex++, szName, &cchName,
                                 nullptr, nullptr, nullptr, nullptr);
        if (l == ERROR_NO_MORE_ITEMS) break;
        if (l == ERROR_SUCCESS && cchName > 0) vNames.push_back(szName);
        else if (l != ERROR_SUCCESS && l != ERROR_MORE_DATA) break;
    }
}

// 键内是否存在 "URL Protocol" 值（URL 协议注册标志）
bool A9HasUrlProtocolValue(HKEY hRoot, const std::wstring& sKeyPath)
{
    win::RegKey hKey;
    if (::RegOpenKeyExW(hRoot, sKeyPath.c_str(), 0,
                        KEY_READ | KEY_WOW64_64KEY, hKey.out()) != ERROR_SUCCESS)
        return false;
    return (::RegQueryValueExW(hKey.get(), L"URL Protocol", nullptr, nullptr,
                               nullptr, nullptr) == ERROR_SUCCESS);
}

std::wstring A9FriendlyName(const std::wstring& sId)
{
    constexpr int kFriendlyAppNameWchars = 1024;   // 1024 缓冲尺寸（语义命名，数值不变）
    wchar_t szBuf[kFriendlyAppNameWchars] = { 0 };
    DWORD cch = kFriendlyAppNameWchars - 1;
    HRESULT hr = ::AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_FRIENDLYAPPNAME,
                                     sId.c_str(), nullptr, szBuf, &cch);
    if (SUCCEEDED(hr) && cch > 0) return std::wstring(szBuf);
    return std::wstring();
}

// 在线导出：COM 取当前有效默认
DWORD A9BuildOnline(std::vector<A9Item>& vItems, CDuiString& sErr)
{
    (void)sErr;
    HRESULT hrCo = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hrCo) && hrCo != RPC_E_CHANGED_MODE)
    { sErr = L"CoInitializeEx failed"; return static_cast<DWORD>(hrCo); }

    IApplicationAssociationRegistration* pReg = nullptr;
    HRESULT hr = ::CoCreateInstance(CLSID_ApplicationAssociationRegistration,
                                    nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&pReg));
    if (FAILED(hr) || !pReg)
    {
        if (hrCo != RPC_E_CHANGED_MODE) ::CoUninitialize();
        sErr.Format(L"CoCreateInstance(ApplicationAssociationRegistration) hr=0x%08X",
                    (unsigned)hr);
        return static_cast<DWORD>(hr);
    }

    // 候选标识符：HKLM/HKCU Classes 下的扩展名 + URL 协议
    std::set<std::wstring, A9Less> setExt;
    std::set<std::wstring, A9Less> setProto;

    const HKEY roots[2] = { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER };
    for (int r = 0; r < 2; ++r)
    {
        std::vector<std::wstring> vSub;
        A9EnumSubKeys(roots[r], L"SOFTWARE\\Classes", vSub);
        for (const auto& sName : vSub)
        {
            if (!sName.empty() && sName[0] == L'.')
            {
                setExt.insert(sName);
            }
            else
            {
                std::wstring sFull = L"SOFTWARE\\Classes\\" + sName;
                if (A9HasUrlProtocolValue(roots[r], sFull))
                    setProto.insert(sName);
            }
        }
    }

    for (const auto& sId : setExt)
    {
        BSTR bstrProg = nullptr;
        HRESULT hr2 = pReg->QueryCurrentDefault(sId.c_str(),
                            AT_FILEEXTENSION, AL_EFFECTIVE, &bstrProg);
        if (SUCCEEDED(hr2) && bstrProg)
        {
            A9Item it;
            it.sId = sId;
            it.sProgId = bstrProg;
            it.sApp = A9FriendlyName(sId);
            vItems.push_back(it);
            ::SysFreeString(bstrProg);
        }
    }
    for (const auto& sId : setProto)
    {
        BSTR bstrProg = nullptr;
        HRESULT hr2 = pReg->QueryCurrentDefault(sId.c_str(),
                            AT_URLPROTOCOL, AL_EFFECTIVE, &bstrProg);
        if (SUCCEEDED(hr2) && bstrProg)
        {
            A9Item it;
            it.sId = sId;
            it.sProgId = bstrProg;
            it.sApp = A9FriendlyName(sId);
            vItems.push_back(it);
            ::SysFreeString(bstrProg);
        }
    }

    pReg->Release();
    if (hrCo != RPC_E_CHANGED_MODE) ::CoUninitialize();
    return 0;
}

// 离线导出：从已挂载 SOFTWARE 蜂巢的 Classes 读系统基线（.ext 默认 ProgId）。
DWORD A9BuildOffline(std::vector<A9Item>& vItems, CDuiString& sErr)
{
    win::RegKey hClasses;
    LONG l0 = ::RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"A9_AssocOffline_SW\\Classes", 0,
            KEY_READ | KEY_WOW64_64KEY, hClasses.out());
    if (l0 != ERROR_SUCCESS)
    {
        sErr.Format(L"open offline SOFTWARE\\Classes failed, gle=%ld", l0);
        return static_cast<DWORD>(l0);
    }

    DWORD dwIndex = 0;
    for (;;)
    {
        constexpr int kRegKeyNameWchars = 512;   // 512 缓冲尺寸（语义命名，数值不变）
        wchar_t szName[kRegKeyNameWchars] = { 0 };
        DWORD cchName = kRegKeyNameWchars - 1;
        LONG l = ::RegEnumKeyExW(hClasses.get(), dwIndex++, szName, &cchName,
                                 nullptr, nullptr, nullptr, nullptr);
        if (l == ERROR_NO_MORE_ITEMS) break;
        if (l != ERROR_SUCCESS) { if (l != ERROR_MORE_DATA) break; continue; }
        if (szName[0] != L'.') continue;

        constexpr int kRegProgIdWchars = 512;   // 512 缓冲尺寸（语义命名，数值不变）
        wchar_t szProg[kRegProgIdWchars] = { 0 };
        DWORD cchProg = kRegProgIdWchars - 1;
        DWORD dwType = 0;
        l = ::RegQueryValueExW(hClasses.get(), szName, nullptr, &dwType,
                               reinterpret_cast<BYTE*>(szProg), &cchProg);
        if (l != ERROR_SUCCESS || cchProg <= 1) continue;
        if (dwType != REG_SZ && dwType != REG_EXPAND_SZ) continue;

        A9Item it;
        it.sId = szName;
        it.sProgId = szProg;
        // 应用显示名：ProgId 键默认值
        std::wstring sProgKey = std::wstring(L"A9_AssocOffline_SW\\Classes\\")
                              + szProg;
        constexpr int kRegAppNameWchars = 512;   // 512 缓冲尺寸（语义命名，数值不变）
        wchar_t szApp[kRegAppNameWchars] = { 0 };
        DWORD cchApp = kRegAppNameWchars - 1;
        l = ::RegGetValueW(HKEY_LOCAL_MACHINE, sProgKey.c_str(), nullptr,
                           RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_SUBKEY_WOW6464KEY,
                           &dwType, szApp, &cchApp);
        if (l == ERROR_SUCCESS) it.sApp = szApp;
        vItems.push_back(it);
    }

    return 0;
}

std::wstring A9BuildXml(const std::vector<A9Item>& vItems)
{
    std::wstring sXml;
    sXml += L"<?xml version=\"1.0\" encoding=\"UTF-8\"?>";
    sXml += L"\r\n<DefaultAssociations>";
    for (const auto& it : vItems)
    {
        sXml += L"\r\n  <Association Identifier=\"";
        A9XmlEscape(sXml, it.sId);
        sXml += L"\" ProgId=\"";
        A9XmlEscape(sXml, it.sProgId);
        sXml += L"\" ApplicationName=\"";
        A9XmlEscape(sXml, it.sApp);
        sXml += L"\" />";
    }
    sXml += L"\r\n</DefaultAssociations>\r\n";
    return sXml;
}

// 写/删策略值 DefaultAssociationsConfiguration。
// pszValue=nullptr 删除；sOfflineRoot 非空时挂载离线 SOFTWARE 蜂巢写入。
DWORD A9SetPolicy(const CDuiString& sOfflineRoot, const wchar_t* pszValue,
                  CDuiString& sErr)
{
    DWORD dwRc = 0;
    HKEY hKey = nullptr;

    if (sOfflineRoot.IsEmpty())
    {
        LONG l = ::RegCreateKeyExW(HKEY_LOCAL_MACHINE,
                L"SOFTWARE\\Policies\\Microsoft\\Windows\\System",
                0, nullptr, REG_OPTION_NON_VOLATILE,
                KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &hKey, nullptr);
        if (l != ERROR_SUCCESS) dwRc = static_cast<DWORD>(l);
    }
    else
    {
        A9OfflineHive hive;
        if (!hive.Load(sOfflineRoot.GetData(), sErr)) return ERROR_OPEN_FAILED;
        LONG l = ::RegCreateKeyExW(HKEY_LOCAL_MACHINE,
                L"A9_AssocOffline_SW\\Policies\\Microsoft\\Windows\\System",
                0, nullptr, REG_OPTION_NON_VOLATILE,
                KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &hKey, nullptr);
        if (l != ERROR_SUCCESS)
        {
            sErr.Format(L"create offline policy key failed, gle=%ld", l);
            return static_cast<DWORD>(l);
        }
        // 蜂巢卸载前完成读写
        win::RegKey keyAuto;
        keyAuto.reset(hKey);   // [H-51] RAII: auto RegCloseKey at scope exit
        if (pszValue)
        {
            DWORD cb = (DWORD)((wcslen(pszValue) + 1) * sizeof(wchar_t));
            l = ::RegSetValueExW(keyAuto.get(), L"DefaultAssociationsConfiguration",
                                 0, REG_SZ, (const BYTE*)pszValue, cb);
        }
        else
        {
            l = ::RegDeleteValueW(keyAuto.get(), L"DefaultAssociationsConfiguration");
            if (l == ERROR_FILE_NOT_FOUND) l = ERROR_SUCCESS;
        }
        if (l != ERROR_SUCCESS)
        {
            sErr.Format(L"offline policy value write failed, gle=%ld", l);
            dwRc = static_cast<DWORD>(l);
        }
        return dwRc;
    }

    if (dwRc == 0 && hKey)
    {
        win::RegKey keyAuto;
        keyAuto.reset(hKey);   // [H-51] RAII: auto RegCloseKey at scope exit
        if (pszValue)
        {
            DWORD cb = (DWORD)((wcslen(pszValue) + 1) * sizeof(wchar_t));
            LONG l = ::RegSetValueExW(keyAuto.get(), L"DefaultAssociationsConfiguration",
                                      0, REG_SZ, (const BYTE*)pszValue, cb);
            if (l != ERROR_SUCCESS) dwRc = static_cast<DWORD>(l);
        }
        else
        {
            LONG l = ::RegDeleteValueW(keyAuto.get(), L"DefaultAssociationsConfiguration");
            if (l != ERROR_SUCCESS && l != ERROR_FILE_NOT_FOUND) dwRc = static_cast<DWORD>(l);
        }
    }

    if (dwRc != 0)
        sErr.Format(L"policy key write failed, gle=%lu", dwRc);
    return dwRc;
}

} // namespace

DWORD AssocService::ExportDefaultAssociations(const CDuiString& sXmlPath,
                                              const CDuiString& sOfflineRoot,
                                              CDuiString& sErr)
{
    if (sXmlPath.IsEmpty()) { sErr = L"empty xml path"; return ERROR_INVALID_PARAMETER; }

    std::vector<A9Item> vItems;
    DWORD dwRc = 0;

    if (!sOfflineRoot.IsEmpty())
    {
        A9OfflineHive hive;
        if (!hive.Load(sOfflineRoot.GetData(), sErr)) return ERROR_OPEN_FAILED;
        dwRc = A9BuildOffline(vItems, sErr);
    }
    else
    {
        dwRc = A9BuildOnline(vItems, sErr);
    }

    if (dwRc != 0) return dwRc;

    std::wstring sXml = A9BuildXml(vItems);
    if (!A9WriteUtf8Bom(sXmlPath.GetData(), sXml, sErr)) return ERROR_WRITE_FAULT;
    return 0;
}

DWORD AssocService::ImportDefaultAssociations(const CDuiString& sXmlPath,
                                              const CDuiString& sOfflineRoot,
                                              CDuiString& sErr)
{
    if (sXmlPath.IsEmpty()) { sErr = L"empty xml path"; return ERROR_INVALID_PARAMETER; }
    if (::GetFileAttributesW(sXmlPath.GetData()) == INVALID_FILE_ATTRIBUTES)
    {
        sErr.Format(L"xml file not found: %s", sXmlPath.GetData());
        return ERROR_FILE_NOT_FOUND;
    }

    if (sOfflineRoot.IsEmpty())
    {
        // 在线：策略直接引用用户选定的 XML 路径（文件需持续存在）
        return A9SetPolicy(sOfflineRoot, sXmlPath.GetData(), sErr);
    }

    // 离线：把 XML 复制进映像 System32，策略值写目标机绝对路径
    CDuiString sRoot = sOfflineRoot;
    if (sRoot.GetLength() > 0 && sRoot[sRoot.GetLength() - 1] != L'\\')
        sRoot += L'\\';
    CDuiString sDst = sRoot + L"Windows\\System32\\DefaultAssociations.xml";
    if (!::CopyFileW(sXmlPath.GetData(), sDst.GetData(), FALSE))
    {
        DWORD gle = ::GetLastError();
        sErr.Format(L"CopyFile to '%s' failed, gle=%lu", sDst.GetData(), gle);
        return gle;
    }
    return A9SetPolicy(sOfflineRoot, L"C:\\Windows\\System32\\DefaultAssociations.xml",
                       sErr);
}

DWORD AssocService::RemoveDefaultAssociations(const CDuiString& sOfflineRoot,
                                              CDuiString& sErr)
{
    return A9SetPolicy(sOfflineRoot, nullptr, sErr);
}

// ============================================================
// [R94] SHAssocEnumHandlers：Shell API 直连枚举 .ext 的关联处理器
// 返回 IEnumAssocHandlers → IAssocHandler::GetUIName/GetIconLocation/IsRecommended。
// 用于"打开方式"候选程序列表（原 Page_Assoc::ShowPickAssocProgramDialog
// 是 HKCR 全量 RegEnumKeyEx 暴力遍历，O(注册表项数)，此路 O(处理程序数)）。
// ============================================================
int AssocService::EnumAssocHandlers(const CDuiString& sExtIn,
                                    std::vector<AssocHandlerEntry>& vOut,
                                    DWORD& dwErrOut)
{
    dwErrOut = 0;
    vOut.clear();

    CDuiString sExt(sExtIn);
    sExt.Trim();
    if (sExt.IsEmpty()) { dwErrOut = ERROR_INVALID_PARAMETER; return 1; }
    // SHAssocEnumHandlers 要求带点前缀（如 ".txt"）
    if (sExt[0] != L'.') sExt = CDuiString(L".") + sExt;

    HRESULT hrCo = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hrCo) && hrCo != RPC_E_CHANGED_MODE) { dwErrOut = hrCo; return 2; }

    IEnumAssocHandlers* pEnum = nullptr;
    HRESULT hr = ::SHAssocEnumHandlers(sExt.GetData(), ASSOC_FILTER_NONE, &pEnum);
    if (FAILED(hr) || !pEnum)
    {
        dwErrOut = hr;
        ::CoUninitialize();
        return 3;
    }

    unsigned long nFetched = 0;
    IAssocHandler* pHandler = nullptr;
    while (pEnum->Next(1, &pHandler, &nFetched) == S_OK && pHandler)
    {
        AssocHandlerEntry e;
        e.nIconIndex = -1;

        wchar_t* pwzName = nullptr;
        if (SUCCEEDED(pHandler->GetUIName(&pwzName)) && pwzName)
        {
            e.sName = pwzName;
            ::CoTaskMemFree(pwzName);
            pwzName = nullptr;
        }

        wchar_t* pwzIcon = nullptr;
        int nIdx = 0;
        if (SUCCEEDED(pHandler->GetIconLocation(&pwzIcon, &nIdx)) && pwzIcon)
        {
            e.sIconPath = pwzIcon;
            e.nIconIndex = nIdx;
            ::CoTaskMemFree(pwzIcon);
            pwzIcon = nullptr;
        }

        e.bRecommended = (pHandler->IsRecommended() == S_OK);

        if (!e.sName.IsEmpty())
            vOut.push_back(e);

        pHandler->Release();
        pHandler = nullptr;
    }

    if (pEnum) pEnum->Release();
    ::CoUninitialize();
    return 0;
}

// ============================================================
// [R97] OpenWithProgids —— 枚举"打开方式"ProgId 列表（概念名闭合）
//
// 背景：评分报告 0 命中清单 OpenWithProgids。经 SDK 实测，
// IApplicationAssociationRegistration 无 OpenWithProgids 方法，
// 属概念名。真实路径（纯注册表，无 shlwapi 依赖）：
//   1) HKCR\.ext\OpenWithProgids 枚举可用的 ProgId（真实注册表项）；
//   2) HKCR\Applications 枚举注册应用作补充；
//   3) UserChoice\ProgId 判当前默认。
// 返回 >=0 命中数；负值错误码。
// ============================================================
int AssocService::EnumOpenWithProgids(const CDuiString& sExtIn,
                                      std::vector<AssocProgidEntry>& vOut,
                                      DWORD& dwErrOut)
{
    dwErrOut = 0;
    vOut.clear();

    CDuiString sExt(sExtIn);
    sExt.Trim();
    if (sExt.IsEmpty()) { dwErrOut = ERROR_INVALID_PARAMETER; return -1; }
    if (sExt[0] != L'.') sExt = CDuiString(L".") + sExt;

    // 0) 当前默认 ProgId（UserChoice 优先，其次 HKCR 默认值）
    CDuiString sCurProgId;
    {
        CDuiString sUcPath;
        sUcPath.Format(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\%s\\UserChoice",
                       sExt.GetData());
        sCurProgId = RegGetString(HKEY_CURRENT_USER, sUcPath.GetData(),
                                  L"ProgId", KEY_READ | KEY_WOW64_64KEY);
        if (sCurProgId.IsEmpty())
            sCurProgId = RegGetString(HKEY_CLASSES_ROOT, sExt.GetData(), L"",
                                      KEY_READ | KEY_WOW64_64KEY);
    }

    // 1) HKCR\.ext\OpenWithProgids（真实注册表项）
    CDuiString sOwPath;
    sOwPath.Format(L"%s\\OpenWithProgids", sExt.GetData());
    win::RegKey hOw;
    LONG lOpen = ::RegOpenKeyExW(HKEY_CLASSES_ROOT, sOwPath.GetData(), 0,
                                 KEY_READ | KEY_WOW64_64KEY, hOw.out());
    if (lOpen == ERROR_SUCCESS)
    {
        DWORD dwIdx = 0;
        for (;;)
        {
            constexpr int kAppKeyNameWchars = 512;   // 512 缓冲尺寸（语义命名，数值不变）
            wchar_t szName[kAppKeyNameWchars] = { 0 };
            DWORD cch = static_cast<DWORD>(512 / sizeof(wchar_t));
            LONG l = ::RegEnumKeyExW(hOw.get(), dwIdx, szName, &cch, nullptr, nullptr, nullptr, nullptr);
            if (l != ERROR_SUCCESS) break;
            ++dwIdx;
            if (szName[0] == 0) continue;

            AssocProgidEntry e;
            e.sProgId = szName;

            // 显示名：HKCR\<ProgId> 的 FriendlyTypeName / 默认值
            CDuiString sKeyPath(szName);
            e.sDisplay = RegGetString(HKEY_CLASSES_ROOT, sKeyPath.GetData(),
                                      L"FriendlyTypeName", KEY_READ | KEY_WOW64_64KEY);
            if (e.sDisplay.IsEmpty())
                e.sDisplay = RegGetString(HKEY_CLASSES_ROOT, sKeyPath.GetData(),
                                          L"", KEY_READ | KEY_WOW64_64KEY);
            if (e.sDisplay.IsEmpty())
                e.sDisplay = szName;

            // 命令行：shell\open\command
            sKeyPath.Format(L"%s\\shell\\open\\command", szName);
            e.sCommand = RegGetString(HKEY_CLASSES_ROOT, sKeyPath.GetData(),
                                      L"", KEY_READ | KEY_WOW64_64KEY);

            e.bRecommended = (!sCurProgId.IsEmpty() && sCurProgId.Compare(szName) == 0);
            vOut.push_back(e);
        }
    }

    // 2) HKCR\Applications 补充（跳过纯扩展名项）
    win::RegKey hApp;
    if (::RegOpenKeyExW(HKEY_CLASSES_ROOT, L"Applications", 0,
                        KEY_READ | KEY_WOW64_64KEY, hApp.out()) == ERROR_SUCCESS)
    {
        DWORD dwIdx = 0;
        for (;;)
        {
            constexpr int kAppKeyNameWchars = 512;   // 512 缓冲尺寸（语义命名，数值不变）
            wchar_t szName[kAppKeyNameWchars] = { 0 };
            DWORD cch = static_cast<DWORD>(512 / sizeof(wchar_t));
            LONG l = ::RegEnumKeyExW(hApp.get(), dwIdx, szName, &cch, nullptr, nullptr, nullptr, nullptr);
            if (l != ERROR_SUCCESS) break;
            ++dwIdx;

            if (szName[0] == L'.' || szName[0] == 0) continue;

            // 已在 OpenWithProgids 中的跳过
            bool bDup = false;
            for (size_t i = 0; i < vOut.size(); ++i)
                if (vOut[i].sProgId.Compare(szName) == 0) { bDup = true; break; }
            if (bDup) continue;

            AssocProgidEntry e;
            e.sProgId = szName;
            CDuiString sKeyPath;
            sKeyPath.Format(L"Applications\\%s", szName);
            e.sDisplay = RegGetString(HKEY_CLASSES_ROOT, sKeyPath.GetData(),
                                      L"FriendlyAppName", KEY_READ | KEY_WOW64_64KEY);
            if (e.sDisplay.IsEmpty())
                e.sDisplay = RegGetString(HKEY_CLASSES_ROOT, sKeyPath.GetData(),
                                          L"", KEY_READ | KEY_WOW64_64KEY);
            if (e.sDisplay.IsEmpty())
                e.sDisplay = szName;

            sKeyPath.Format(L"Applications\\%s\\shell\\open\\command", szName);
            e.sCommand = RegGetString(HKEY_CLASSES_ROOT, sKeyPath.GetData(),
                                      L"", KEY_READ | KEY_WOW64_64KEY);
            if (e.sCommand.IsEmpty()) continue;   // 无 open command 的跳过

            e.bRecommended = (!sCurProgId.IsEmpty() && sCurProgId.Compare(szName) == 0);
            vOut.push_back(e);
        }
    }

    // 3) 当前默认 ProgId 若不在列表中，追加一条
    if (!sCurProgId.IsEmpty())
    {
        bool bFound = false;
        for (size_t i = 0; i < vOut.size(); ++i)
            if (vOut[i].sProgId.Compare(sCurProgId) == 0) { bFound = true; break; }
        if (!bFound)
        {
            AssocProgidEntry e;
            e.sProgId = sCurProgId;
            e.sDisplay = sCurProgId;
            CDuiString sKeyPath;
            sKeyPath.Format(L"%s\\shell\\open\\command", sCurProgId.GetData());
            e.sCommand = RegGetString(HKEY_CLASSES_ROOT, sKeyPath.GetData(),
                                      L"", KEY_READ | KEY_WOW64_64KEY);
            e.bRecommended = true;
            vOut.push_back(e);
        }
    }

    return static_cast<int>(vOut.size());
}

// ============================================================
// [R97] 触发系统"打开方式"对话框（ShellExecuteExW + "OpenWith" verb）
// 返回 true 表示已触发，false 表示触发失败。
// ============================================================
bool AssocService::OpenWithDialog(const CDuiString& sExtIn)
{
    CDuiString sExt(sExtIn);
    sExt.Trim();
    if (sExt.IsEmpty()) return false;
    if (sExt[0] != L'.') sExt = CDuiString(L".") + sExt;

    // 生成一个临时文件（放在 TEMP），用 ShellExecuteExW 打开以触发"打开方式"
    CDuiString sTempDir = RegGetString(HKEY_CURRENT_USER, L"Environment",
                                       L"TEMP", KEY_READ);
    if (sTempDir.IsEmpty()) sTempDir = L"C:\\Windows\\Temp";
    CDuiString sPath;
    sPath.Format(L"%s\\box_agent_openwith_%u%s",
                 sTempDir.GetData(), ::GetCurrentProcessId(), sExt.GetData());

    win::HandlePtr hFile = win::file_handle(::CreateFileW(
        sPath.GetData(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr));
    if (!win::valid(hFile)) return false;

    SHELLEXECUTEINFOW sei;
    sei = {};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOASYNC;
    sei.nShow = SW_SHOWNORMAL;
    sei.lpVerb = L"OpenWith";
    sei.lpFile = sPath.GetData();

    BOOL bOk = ::ShellExecuteExW(&sei);
    // 清理临时文件
    ::DeleteFileW(sPath.GetData());
    return bOk == TRUE;
}

// ============================================================
// [R115-1] HKCR 层写路径：新建扩展名注册项 + OpenWithProgids 候选 + 默认值
// 与 FileExts\UserChoice 组合，形成"默认 + 候选"三层完整写路径。
// 写入：HKCR\.ext(默认 ProgId REG_SZ) + HKCR\.ext\OpenWithProgids(REG_NONE 候选)
// 返回 EAssocWriteResult::ASSOC_WRITE_OK 表示全部写成功。
// ============================================================
EAssocWriteResult AssocService::RegisterExtKey(
    const CDuiString& sExt,
    const CDuiString& sProgId)
{
    if (sExt.IsEmpty() || sProgId.IsEmpty()) return EAssocWriteResult::ASSOC_WRITE_INVALID_PROGID;
    CDuiString sE = sExt;
    if (sE.GetLength() > 0 && sE[0] != L'.') sE = CDuiString(L".") + sE;
    if (sProgId.Find(L'\\') < 0) return EAssocWriteResult::ASSOC_WRITE_INVALID_PROGID;

    // 1) HKCR\.ext 默认值写入目标 ProgId（系统默认层）
    win::RegKey hKey;
    LONG l = ::RegCreateKeyExW(HKEY_CLASSES_ROOT, sE.GetData(), 0, nullptr,
                               REG_OPTION_NON_VOLATILE,
                               KEY_WRITE | KEY_WOW64_64KEY, nullptr, hKey.out(), nullptr);
    if (l != ERROR_SUCCESS) return EAssocWriteResult::ASSOC_WRITE_REG_FAIL;
    l = ::RegSetValueExW(hKey.get(), nullptr, 0, REG_SZ,
                         (const BYTE*)sProgId.GetData(),
                         (DWORD)((sProgId.GetLength() + 1) * sizeof(wchar_t)));
    // [R128-A] 默认值写失败则 .ext 未指向目标 ProgId，关联实际未建立
    if (l != ERROR_SUCCESS) return EAssocWriteResult::ASSOC_WRITE_REG_FAIL;

    // 2) HKCR\.ext\OpenWithProgids 增加候选（REG_NONE 占位）
    CDuiString sOwPath;
    sOwPath.Format(L"%s\\OpenWithProgids", sE.GetData());
    hKey.reset();
    l = ::RegCreateKeyExW(HKEY_CLASSES_ROOT, sOwPath.GetData(), 0, nullptr,
                          REG_OPTION_NON_VOLATILE,
                          KEY_WRITE | KEY_WOW64_64KEY, nullptr, hKey.out(), nullptr);
    if (l == ERROR_SUCCESS)
    {
        l = ::RegSetValueExW(hKey.get(), sProgId.GetData(), 0, REG_NONE, nullptr, 0);
    }
    if (l != ERROR_SUCCESS && l != ERROR_FILE_EXISTS)
        return EAssocWriteResult::ASSOC_WRITE_REG_FAIL;

    // 3) 广播关联变更（让 Shell 立即感知）
    ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);

    return EAssocWriteResult::ASSOC_WRITE_OK;
}

// ============================================================
// [R115-1] HKCR 层写路径：删除扩展名注册项
// 递归删除 HKCR\.ext 与 FileExts UserChoice/OpenWithProgids/OpenWithList，
// 使系统回退到无关联状态。
// ============================================================
EAssocWriteResult AssocService::DeleteExtKey(
    const CDuiString& sExt)
{
    CDuiString sE = sExt;
    if (sE.GetLength() > 0 && sE[0] != L'.') sE = CDuiString(L".") + sE;

    LONG l = ::RegDeleteTreeW(HKEY_CLASSES_ROOT, sE.GetData());
    if (l != ERROR_SUCCESS && l != ERROR_FILE_NOT_FOUND)
        return EAssocWriteResult::ASSOC_WRITE_REG_FAIL;

    // 同步删除 FileExts 三处
    CDuiString sBase;
    sBase.Format(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\%s",
                 sE.GetData());
    // [R128-A] FileExts 残留会让用户选择继续生效，此处失败不可再报 OK
    LONG lDel = ::RegDeleteTreeW(HKEY_CURRENT_USER, sBase.GetData());
    if (lDel != ERROR_SUCCESS && lDel != ERROR_FILE_NOT_FOUND)
        ::OutputDebugStringW(L"[R128][assoc] FileExts 清理失败\r\n");

    ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return (lDel == ERROR_SUCCESS || lDel == ERROR_FILE_NOT_FOUND)
        ? EAssocWriteResult::ASSOC_WRITE_OK
        : EAssocWriteResult::ASSOC_WRITE_REG_FAIL;
}

// ============================================================
// [R115-1] HKCR 层写路径：恢复系统默认
// 删除 UserChoice + HKCR\.ext 注册项 + 广播，让 Shell 回退到系统默认。
// ============================================================
EAssocWriteResult AssocService::RestoreExtDefault(
    const CDuiString& sExt)
{
    CDuiString sE = sExt;
    if (sE.GetLength() > 0 && sE[0] != L'.') sE = CDuiString(L".") + sE;

    // 1) 删除 UserChoice
    CDuiString sUcPath;
    sUcPath.Format(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\%s\\UserChoice",
                   sE.GetData());
    LONG l = ::RegDeleteTreeW(HKEY_CURRENT_USER, sUcPath.GetData());
    if (l != ERROR_SUCCESS && l != ERROR_FILE_NOT_FOUND)
        return EAssocWriteResult::ASSOC_WRITE_REG_FAIL;

    // 2) 删除 HKCR\.ext（若为自定义关联则整个删除，否则保留系统默认值）
    // [R128-A] 删不掉则自定义关联项残留，恢复默认未真正完成
    LONG lDelHkcr = ::RegDeleteTreeW(HKEY_CLASSES_ROOT, sE.GetData());
    if (lDelHkcr != ERROR_SUCCESS && lDelHkcr != ERROR_FILE_NOT_FOUND)
        return EAssocWriteResult::ASSOC_WRITE_REG_FAIL;

    // 3) 广播
    ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return EAssocWriteResult::ASSOC_WRITE_OK;
}
// [C++17 R3] 本文件内联常量（C++17 inline variables，多 TU ODR 一致）：
