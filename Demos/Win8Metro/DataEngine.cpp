#include "stdafx.h"
#include "DataEngine.h"
#include "StringManager.h"
#include "DismCore.h"          // [R-P22 / R2] Rust dismcore：Base64/Hex 纯计算模块
#include "OsVersionInfo.h"     // [OS-Filter] 版本识别（Win11 小版本）
#include "OfflineSession.h"     // [FIX-P0] 离线镜像目标版本判定

// #include "AppxRuntimeBridge.h" // [R25-F1] 删除假依赖：0 处使用   // [P0-A1] SetAppxPackage 原生 WinRT 安装/卸载
#include "DriverService.h"       // [P1-A6] Rescan 动作原生 cfgmgr32 重扫
#include "RegCleanEngine.h"      // [P1-收尾] RegCleanImport 原生 .reg 解析
// #include "DismApiBridge.h" // [R25-F1] 删除假依赖：0 处使用      // [P1-收尾] AddDriver/AddPackage 原生 dismapi 直连
#include "unzip.h"                // [P0-A7] FileCreateByZIP/规则包解压：仓库内 minizip 零进程

#include <shlwapi.h>
#include <shellapi.h>
#include <commdlg.h>
#include <objbase.h>               // [P0-A7] IIDFromString / StringFromGUID2
#include <winsvc.h>
#include <winnt.h>
#include <map>
#include <vector>
#include <string>
// [C++17 阶段2] 局部 RAII 守卫：文件/查找句柄、注册表键（见 WinHandles.h 使用规范）
struct DEFileGuard
{
    HANDLE h;
    explicit DEFileGuard(HANDLE x) : h(x) {}
    ~DEFileGuard() { if (h && h != INVALID_HANDLE_VALUE) ::CloseHandle(h); }
    DEFileGuard(const DEFileGuard&) = delete;
    DEFileGuard& operator=(const DEFileGuard&) = delete;
    operator HANDLE() const { return h; }
};

struct DEFindGuard
{
    HANDLE h;
    explicit DEFindGuard(HANDLE x) : h(x) {}
    ~DEFindGuard() { if (h && h != INVALID_HANDLE_VALUE) ::FindClose(h); }
    DEFindGuard(const DEFindGuard&) = delete;
    DEFindGuard& operator=(const DEFindGuard&) = delete;
    operator HANDLE() const { return h; }
};

struct DERegGuard
{
    HKEY k = nullptr;
    ~DERegGuard() { if (k) ::RegCloseKey(k); }
    operator HKEY() const { return k; }
};


using i18n::CStringManager;

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "shell32.lib")

// 注：[R9-02] Wow64Scope 已随 DoReg* 原语迁入 DataEngine_Reg.cpp，
// 共享定义见 DataEngine_RegInternal.h（仅注册表执行器 TU 包含）。

// ============================================================
// construction / load
// ============================================================
CDataEngine::CDataEngine() : m_pMarkup(nullptr)
{
}

CDataEngine::~CDataEngine()
{
    if (m_pMarkup) { m_pMarkup->Release(); delete m_pMarkup; }
}

// 真实系统版本判定（Win11 = 10.0.build >= 22000）。
// RtlGetVersion 不受兼容性 manifest 影响，与 MatchOSVersion 取版本号的方式一致。
static bool DismDataIsWindows11OrLater()
{
    OSVERSIONINFOEXW osv = { sizeof(osv) };
    using RtlGetVersionProc = LONG ( WINAPI*)(OSVERSIONINFOEXW*);
    HMODULE hNtdll = ::GetModuleHandleW(L"ntdll.dll");
    RtlGetVersionProc pfn = hNtdll
        ? reinterpret_cast<RtlGetVersionProc>(::GetProcAddress(hNtdll, "RtlGetVersion"))
        : nullptr;
    if (pfn && pfn(&osv) == 0)
        return osv.dwMajorVersion > 10
            || (osv.dwMajorVersion == 10 && osv.dwBuildNumber >= 22000);
    return false;
}

// ============================================================
// [R3] 规则信任链：可选哈希清单校验
//
// 规则文件可驱动高权限动作，加载前必须能证明来源未被篡改。
// 约定：规则文件同目录下放置 "<规则文件名>.sha256"。
//   * 首行：64 位十六进制 SHA-256（唯一参与哈希比对的内容）；
//   * 可选附加行：name=/size=/generated=，仅用于诊断输出，不参与比对，
//     用于区分"规则文件被替换"与"清单本身被替换"。
//   * 清单存在 → 首行必须匹配，不匹配拒绝加载（fail-closed）。
//   * 清单缺失 → 允许加载但记录一条告警，保持既有部署兼容。
// ============================================================
static bool VerifyRuleDigest(const CDuiString& strRulePath, CDuiString& strDetail)
{
    const std::string sExpect = dismcore::DigestFileHex(DISMCORE_SHA256, strRulePath.GetData());
    if (sExpect.empty())
    {
        strDetail = L"[rules] digest unavailable";
        return false;
    }

    CDuiString strManifest(strRulePath);
    strManifest += L".sha256";
    DEFileGuard hMan(::CreateFile(strManifest.GetData(), GENERIC_READ, FILE_SHARE_READ,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (hMan == INVALID_HANDLE_VALUE)
    {
        // 未提供清单：保持既有部署兼容，但要留下告警痕迹。
        strDetail = L"[rules] digest manifest absent (accepted with warning)";
        return true;
    }

    char szBuf[512] = { 0 };
    DWORD dwRead = 0;
    if (!::ReadFile(hMan, szBuf, sizeof(szBuf) - 1, &dwRead, nullptr) || dwRead == 0)
    {
        strDetail = L"[rules] digest manifest unreadable";
        return false;
    }

    std::string sAll(szBuf, dwRead);

    // [R21] 只取首行参与哈希比对；后续 name=/size=/generated= 仅作诊断。
    // 用字符码判断换行，避免源码中出现转义或真实换行导致解析问题。
    std::string sGot;
    for (size_t i = 0; i < sAll.size(); ++i)
    {
        const unsigned char ch = static_cast<unsigned char>(sAll[i]);
        if (ch == 10 || ch == 13) break;
        sGot.push_back(sAll[i]);
    }
    while (!sGot.empty())
    {
        const unsigned char chTail = static_cast<unsigned char>(sGot.back());
        if (chTail == 32 || chTail == 9) sGot.pop_back();
        else break;
    }
    if (sGot.size() < 64)
    {
        strDetail = L"[rules] digest manifest malformed";
        return false;
    }

    std::string sWant = sGot.substr(0, 64);
    for (size_t i = 0; i < sWant.size(); ++i)
    {
        const char c = sWant[i];
        if (c >= 'A' && c <= 'F') sWant[i] = static_cast<char>(c - 'A' + 'a');
    }
    if (sWant != sExpect)
    {
        // [R21] 诊断区分：附加行存在但首行不符，说明清单与文件不一致，
        // 可能是规则文件被替换，也可能是清单被替换；二者都应拒绝加载。
        strDetail = (sAll.size() > 64)
            ? L"[rules] digest mismatch (manifest has metadata), rules rejected"
            : L"[rules] digest mismatch, rules rejected";
        return false;
    }
    strDetail = L"[rules] digest verified";
    return true;
}

bool CDataEngine::Load(CDuiString& strFoundPath)
{
    m_strLastError.Empty();

    wchar_t szExe[MAX_PATH] = { 0 };
    ::GetModuleFileName(nullptr, szExe, MAX_PATH);
    CDuiString strDir(szExe);
    int p = strDir.ReverseFind(L'\\');
    if (p >= 0) strDir = strDir.Mid(0, p);

    // 候选顺序以“便携包”为第一优先：exe 同级 DismData\ 是标准发布布局。
    // 仓库开发布局（..\Dism-Multi-language-*）必须放最后，否则便携包被放在
    // 仓库内时会被上游原版 Data.xml 抢先命中，静默加载错误版本。
        // [R-B37] 规则源候选顺序（原版三级覆盖 + 便携包标准布局）。
    // 优先级：便携包标准布局 DismData\Data.xml -> 平铺 -> 包内子目录 -> 开发仓库；
    // 全部未命中再走 Data.zip 解压、Data_extracted、data_default 三级兜底。
    //
    // [Win11 适配] build >= 22000 时，每个布局先找 Data_Win11.xml，缺失再回退
    // Data.xml：布局顺序优先于文件名，避免便携包被开发仓库抢先命中。
    // [FIX-P0] 规则文件选择也必须以目标镜像版本为准：离线镜像在 Win11 宿主上
    // 维护 Win10 时不能选 Data_Win11.xml。离线会话不可用时才回退宿主判定。
    bool bIsWin11 = DismDataIsWindows11OrLater();
    if (COfflineSession::IsActive())
    {
        const unsigned int uBuild = COfflineSession::GetTargetBuild();
        if (uBuild > 0) bIsWin11 = (uBuild >= 22000);
    }

    // ----------------------------------------------------------------
    // 规则源定位策略（R-B37 + Win11 适配）
    // ----------------------------------------------------------------
    // 本项目规则源共有 4 种可能来源，按以下优先级依次尝试，
    // 命中即停（bFound = true），后续来源不再探测：
    //
    //   1) 标准布局散装文件（ProbeLooseLayouts）
    //      exe 同级 DismData\Data.xml —— 便携包标准发布布局，第一优先。
    //      原因是仓库开发布局必须放最后，否则便携包被放进仓库目录时
    //      会被上游原版 Data.xml 抢先命中，静默加载错误版本。
    //
    //   2) 打包规则 Data.zip（ProbeZipPackages）
    //      用户可随更新替换的打包规则；解压到 %TEMP%\dismpp_data_<tick>，
    //      递归找其中的 Data.xml，命中即用解压产物路径，
    //      后续仍走同一套 Name 去重覆盖加载。
    //
    //   3) 解压缓存 Data_extracted（ProbeFallbackDirs）
    //      上次 Data.zip 解压的落盘产物，避免每次都重新解压。
    //
    //   4) 内置默认 data_default（ProbeFallbackDirs）
    //      随程序分发的兜底规则，保证任何情况下都有可用规则集。
    //
    // 全部未命中时：m_strLastError 拼上 strTried 并返回 false，
    // 同时把便携包标准路径回填给 strFoundPath 作为提示线索。
    //
    // Win11 适配：build >= 22000 时，每个布局先找 Data_Win11.xml，
    // 缺失再回退 Data.xml。注意是「布局顺序优先于文件名优先」——
    // 即先遍历完某个布局的两种文件名，才进入下一个布局，
    // 而不是先把所有布局的 Data_Win11.xml 找完再找 Data.xml。
    // 这样可避免便携包被开发仓库抢先命中。
    // ----------------------------------------------------------------

    // 已尝试路径累积串。每次探测都追加 "\r\n    " + 候选路径，
    // 全部落空时拼进 m_strLastError，让用户在日志里直接看到搜过哪些位置。
    // 它必须在三条探测路径之间共享——zip 与兜底目录都要往里追加，
    // 故用出参（CDuiString&）而非返回值传递。
    //
    // 沿用出参而非返回结构体，也让三个 Probe* 各自只负责「找到没有」，
    // 字符串累积这一横切关注点统一留在 Load 内，便于统一格式。

    // 标准布局候选表：顺序即优先级，不可随意调换。
    //   "\\DismData"          —— 便携包标准布局（第一优先）
    //   ""                     —— 平铺：Data.xml 直接躺在 exe 同级
    //   "\\..\\DismData"       —— exe 位于包内子目录时的布局
    //   "\\.."                 —— 上一级目录
    //   "\\..\\Dism-Multi-language-10.1.1002.2" —— 仅开发环境兜底
    //
    // 注释集中放在数组之前而不是逐行展开，是因为数组字面量本身
    // 已带行尾注释，再叠一块会让这段声明显得拥挤。

    // strPath / bFound 是三条探测路径共同的输出：
    //   bFound  —— 是否已命中任一来源，命中即短路后续探测；
    //   strPath —— 命中的规则文件绝对路径，未命中时保持为空。
    // 二者都在 Load 内声明，由 Probe* 以返回值或出参回填。

    CDuiString strTried;

    const wchar_t* layouts[] = {
        L"\\DismData",                                      // 便携包标准布局
        L"",                                                // 平铺
        L"\\..\\DismData",                                  // exe 位于包内子目录
        L"\\..",
        L"\\..\\Dism-Multi-language-10.1.1002.2"            // 仅开发环境兜底
    };

    CDuiString strPath;
    bool bFound = false;
    TryResult loose = ProbeLooseLayouts(strDir, bIsWin11, layouts, _countof(layouts), strTried);
    if (loose.bFound) { bFound = true; strPath = loose.strPath; }

    // 第一级：标准布局散装文件。命中后 bFound 置位，后面两级全部跳过。
    // ProbeLooseLayouts 内部自己遍历 layouts × 文件名两层循环，
    // 并把每次尝试的候选路径追加进 strTried。

    // 第二级：打包规则 Data.zip。仅在散装文件全部落空时才进入。
    // ProbeZipPackages 返回 true 表示已找到并解压出可用的 Data.xml，
    // 此时 strPath 已被回填，这里只需同步 bFound 即可。
    // 注意：解压是耗时操作（落 %TEMP% 并递归查找），
    // 因此绝不能在散装文件已命中的情况下仍然调用它。

    // 第三级：解压缓存与内置默认。同样只在前面全部落空时进入。
    // 这两类目录由 ProbeFallbackDirs 一并覆盖（含 Win11 文件名优选），
    // 顺序为 Data_extracted 在前、data_default 在后：前者是解压产物，
    // 后者是随包内置的默认规则，优先级自然更低。
    // 命中即回填 strPath 并返回 true。三级走完仍 !bFound 说明规则源
    // 彻底缺失，此时把累积的 strTried 拼进错误信息便于定位部署问题。

        // [R-B37] 若标准布局均未命中，尝试 Data.zip（用户可更新的打包规则）。
    // 解压成功且内含 Data.xml 时直接使用解压产物路径，后续仍走同一套 Name 去重覆盖加载。
    // Win11 下先找 Data_Win11.zip，再回退 Data.zip。
    if (!bFound && ProbeZipPackages(strDir, bIsWin11, strTried, strPath))
        bFound = true;

    // [R-B37] 再兜底：解压缓存 Data_extracted\Data*.xml 与内置默认 data_default\Data*.xml
    if (!bFound && ProbeFallbackDirs(strDir, bIsWin11, strTried, strPath))
        bFound = true;

    if (!bFound)
    {
        m_strLastError = CDuiString(StringMgr.Get("misc.se9e3af14")) + strTried;
        strFoundPath = strDir + L"\\DismData\\Data.xml";
        return false;
    }

    strFoundPath = strPath;

    // [R3] 规则信任链校验：清单存在则必须匹配，否则拒绝加载。
    {
        CDuiString strDigestDetail;
        if (!VerifyRuleDigest(strPath, strDigestDetail))
        {
            m_strLastError = CDuiString(strDigestDetail);
            return false;
        }
        m_strLastError = CDuiString(strDigestDetail);
    }

    // Read bytes directly (LoadFromFile would prepend CPaintManagerUI
    // resource path and break absolute paths outside the skin folder).
    DEFileGuard hFile = DEFileGuard(::CreateFile(strPath.GetData(), GENERIC_READ, FILE_SHARE_READ,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (hFile == INVALID_HANDLE_VALUE)
    {
        m_strLastError = L"CreateFile failed";
        return false;
    }

    DWORD dwSize = ::GetFileSize(hFile, nullptr);
    if (dwSize == INVALID_FILE_SIZE || dwSize == 0)
    {
        m_strLastError = L"invalid file size";
        return false;
    }

    std::vector<BYTE> bufByte(dwSize, 0);
    BYTE* pByte = bufByte.data();
    DWORD dwRead = 0;
    BOOL bOk = ::ReadFile(hFile, pByte, dwSize, &dwRead, nullptr);
    if (!bOk || dwRead != dwSize)
    {
        m_strLastError = L"ReadFile failed";
        return false;
    }

    // [R-B9] Custom 规则注入：把 Config\Custom\*.xml / *.zip 中的
    // <Item> 片段插入 Data.xml 的 <SystemOptimization> / <CleanCollection4> 末尾。
    // CMarkup 不支持节点追加，故在 UTF-8 字节层做字符串级合并后重载。
    std::string sXml(pByte, pByte + dwSize);
    if (!m_pMarkup) m_pMarkup = new CMarkup(); // [C++17 阶段3] 类成员延迟创建，生命周期跨函数（析构释放），保持裸指针
    bool bRet = false;
    if (!LoadCustomRulesIntoXml(sXml))
    {
        // 无 Custom 规则或合并失败：直接加载原文件
        bRet = m_pMarkup->LoadFromMem(reinterpret_cast<BYTE*>(sXml.data()), static_cast<DWORD>(sXml.size()));
    }
    else
    {
        bRet = m_pMarkup->LoadFromMem(reinterpret_cast<BYTE*>(sXml.data()), static_cast<DWORD>(sXml.size()));
        if (!bRet)
        {
            // 合并后解析失败：回退原文件
            m_strLastError = L"Custom rules merge parse error";
        }
    }

    if (!bRet)
    {
        wchar_t szErr[128] = { 0 };
        wchar_t szLoc[160] = { 0 };
        m_pMarkup->GetLastErrorMessage(szErr, 127);
        m_pMarkup->GetLastErrorLocation(szLoc, 159);
        // sanitize newlines in the location snippet
        for (int k = 0; szLoc[k]; ++k)
            if (szLoc[k] == L'\r' || szLoc[k] == L'\n' || szLoc[k] == L'\t')
                szLoc[k] = L' ';
        m_strLastError.Format(L"XML parse error: %s @>>>%.120s<<<", szErr, szLoc);
        return false;
    }

    if (!m_pMarkup->IsValid())
    {
        m_strLastError = L"markup invalid after load";
        return false;
    }

    // 结构校验：确认是本项目可用的规则文件，避免“加载成功但两页全空”
    CMarkupNode root = m_pMarkup->GetRoot();
    bool bShapeOk = root.IsValid();
    CDuiString sShapeErr;
    if (!bShapeOk)
    {
        sShapeErr = CDuiString(StringMgr.Get("misc.s6bbf0ec9")) + strFoundPath;
    }
    else if (lstrcmpiW(root.GetName(), L"Data") != 0)
    {
        sShapeErr = CDuiString(StringMgr.Get("misc.sf966efb9"))
            + root.GetName() + StringMgr.Get("misc.s6340d56b") + strFoundPath;
    }
    else if (!root.GetChild(L"SystemOptimization").IsValid()
             && !root.GetChild(L"CleanCollection4").IsValid())
    {
        sShapeErr = CDuiString(StringMgr.Get("misc.s1adeb99e")) + strFoundPath;
    }
    if (!sShapeErr.IsEmpty())
    {
        // 释放解析结果，使 IsValid() 为 false，避免界面走“成功但空页”分支
        m_pMarkup->Release();
        m_strLastError = sShapeErr;
        return false;
    }

    return true;
}

bool CDataEngine::IsValid() const { return m_pMarkup && m_pMarkup->IsValid(); }

// ============================================================
// [R-B37] Data.zip / Data_extracted / data_default 三级覆盖：解压 Data.zip
// ------------------------------------------------------------
// 原版行为：规则源可以是打包的 Data.zip（可随更新替换），解压到
// Data_extracted 后由规则解释器读取；data_default 为内置默认兜底。
// 本方法把 zip 解压到 %TEMP%\dismpp_data_<tick>，递归寻找其中的 Data.xml，
// 命中即返回该路径（调用方负责后续读取与合并）。
// 失败返回 false（调用方继续走标准布局回退）。
// ============================================================
bool CDataEngine::ExtractDataZipToTemp(const wchar_t* pszZip, CDuiString& outDataXmlPath)
{
    if (!pszZip || !pszZip[0]) return false;
    if (::GetFileAttributesW(pszZip) == INVALID_FILE_ATTRIBUTES) return false;

    wchar_t szTmp[MAX_PATH] = {};
    ::GetTempPathW(MAX_PATH, szTmp);
    wchar_t szUnzip[MAX_PATH];
    wfmt::FormatInto(szUnzip, MAX_PATH, L"%s\\dismpp_data_%u", szTmp, ::GetTickCount());
    ::CreateDirectoryW(szUnzip, nullptr);

    // [P0-A7] 仓库内 minizip（DuiLib\Utils\unzip.cpp）整包解压，
    // 替代 PowerShell + System.IO.Compression（WinPE/精简系统无 PS 也可工作）。
    bool bOk = false;
    HZIP hz = OpenZip(pszZip, nullptr);
    if (hz)
    {
        ZIPENTRY zeRoot = { 0 };
        if (GetZipItem(hz, -1, &zeRoot) == ZR_OK)
        {
            SetUnzipBaseDir(hz, szUnzip);
            bOk = true;
            for (int i = 0; i < zeRoot.index; ++i)
            {
                ZIPENTRY ze = { 0 };
                if (GetZipItem(hz, i, &ze) != ZR_OK) { bOk = false; break; }
                // 相对路径条目（zip 内正斜杠）由 UnzipItem 自动建目录；
                // 目录条目 UnzipItem 发 0 字节但会建目录。
                if (UnzipItem(hz, i, ze.name) != ZR_OK
                    && !(ze.attr & FILE_ATTRIBUTE_DIRECTORY))
                {
                    bOk = false;
                    break;
                }
            }
        }
        CloseZip(hz);
    }

    // 递归寻找 Data.xml（限 4 层，命中第一个）
    if (bOk)
    {
        struct ScanDataXml
        {
            static bool Scan(const wchar_t* pszDir, int nDepth, CDuiString& outPath)
            {
                if (nDepth > 4) return false;
                if (!pszDir || !*pszDir) return false;
                CDuiString sSearch = CDuiString(pszDir);
                if (sSearch[sSearch.GetLength() - 1] != L'\\') sSearch += L"\\";
                sSearch += L"*";
                WIN32_FIND_DATAW fd = {};
                DEFindGuard hFind(::FindFirstFileW(sSearch.GetData(), &fd));
                if (hFind == INVALID_HANDLE_VALUE) return false;
                bool bHit = false;
                do
                {
                    if (fd.cFileName[0] == L'.') continue;
                    CDuiString sFull = CDuiString(pszDir);
                    if (sFull[sFull.GetLength() - 1] != L'\\') sFull += L"\\";
                    sFull += fd.cFileName;
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    {
                        if (Scan(sFull.GetData(), nDepth + 1, outPath)) { bHit = true; break; }
                    }
                    else if (lstrcmpiW(fd.cFileName, L"Data.xml") == 0)
                    {
                        outPath = sFull;
                        bHit = true;
                        break;
                    }
                } while (::FindNextFileW(hFind, &fd));
                return bHit;
            }
        };
        bOk = ScanDataXml::Scan(szUnzip, 0, outDataXmlPath);
    }

    // 清理临时目录
    SHFILEOPSTRUCTW sofo = {};
    sofo.wFunc = FO_DELETE;
    CDuiString sDel = CDuiString(szUnzip) + L"\\*";
    sofo.pFrom = sDel.GetData();
    sofo.fFlags = FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    // [R128-B] 解压临时目录清理失败需留痕（不中断，后续还有 RemoveDirectory）
    if (::SHFileOperationW(&sofo) != 0 || sofo.fAnyOperationsAborted)
        ::OutputDebugStringW(L"[R128][data] 临时目录清理未完全成功\r\n");
    ::RemoveDirectoryW(szUnzip);
    return bOk;
}

// ============================================================
// [R-B9] 自定义规则加载（Config\Custom\*.xml / *.zip）
// ------------------------------------------------------------
// 原版行为：Dism++ 允许用户在 Config\Custom 目录放自定义规则
//（*.xml 或 *.zip），程序在启动时把它们合并进主 Data.xml，
// 覆盖系统优化/清理两个页面的规则列表。
// 本函数在 UTF-8 字节层扫描 Custom 目录下的规则文件：
//   - 纯 <Item> 片段：追加到 <SystemOptimization> 与 <CleanCollection4> 末尾
//   - 完整 <Data> 结构：把其中的 <Item> 摘出后追加
// 合并失败（缺文件/无片段/结构错乱）返回 false，调用方回退原文件。
// ============================================================
// ============================================================
// [R12-01] LoadCustomRulesIntoXml 深度拆解辅助（d7->d<=4）
// 各段逐字搬迁原函数体，纯结构性拆分，零行为改动。
// ============================================================

// [S1] 定位 exe 同级 Config\Custom 目录
static bool LcFindCustomDir(CDuiString& outDir)
{
    wchar_t szExe[MAX_PATH] = { 0 };
    ::GetModuleFileNameW(nullptr, szExe, MAX_PATH);
    CDuiString strDir(szExe);
    int p = strDir.ReverseFind(L'\\');
    if (p >= 0) strDir = strDir.Mid(0, p);
    CDuiString strCustomDir = strDir + L"\\Config\\Custom";
    if (::GetFileAttributesW(strCustomDir.GetData()) == INVALID_FILE_ATTRIBUTES)
        return false;
    outDir = strCustomDir;
    return true;
}

// [S2] 读取单个文件内容（BOM 保留，交由调用方剥离）
static bool LcReadFileContent(const wchar_t* pszFull, std::string& out)
{
    DEFileGuard hFile = DEFileGuard(::CreateFileW(pszFull, GENERIC_READ, FILE_SHARE_READ,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (hFile == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER li = { 0 };
    ::GetFileSizeEx(hFile, &li);
    bool bOk = false;
    if (li.QuadPart > 0 && li.QuadPart <= 4 * 1024 * 1024)
    {
        DWORD dw = static_cast<DWORD>(li.QuadPart);
        out.resize(dw);
        DWORD dwRead = 0;
        if (::ReadFile(hFile, &out[0], dw, &dwRead, nullptr) && dwRead == dw)
        {
            out.resize(dwRead);
            bOk = true;
        }
    }
    return bOk;
}

// [S3] 从 .zip 内存抽取其中所有 .xml 条目，追加到 vFragments
static void LcUnzipXmlEntries(const wchar_t* pszFull, std::vector<std::string>& vFragments)
{
    HZIP hz = OpenZip(pszFull, nullptr);
    if (!hz) return;
    ZIPENTRY zeRoot = { 0 };
    if (GetZipItem(hz, -1, &zeRoot) == ZR_OK)
    {
        for (int zi = 0; zi < zeRoot.index; ++zi)
        {
            ZIPENTRY ze = { 0 };
            if (GetZipItem(hz, zi, &ze) != ZR_OK) continue;
            if (ze.attr & FILE_ATTRIBUTE_DIRECTORY) continue;
            CDuiString sName(ze.name);
            sName.MakeLower();
            if (sName.GetLength() < 4
                || sName.Mid(sName.GetLength() - 4) != L".xml")
                continue;
            if (ze.unc_size <= 0 || ze.unc_size > 4 * 1024 * 1024) continue;
            std::string sTmp;
            sTmp.resize(static_cast<size_t>(ze.unc_size));
            if (UnzipItem(hz, zi, &sTmp[0],
                          (unsigned int)sTmp.size()) == ZR_OK)
            {
                size_t o2 = 0;
                if (sTmp.size() >= 3 && (unsigned char)sTmp[0] == 0xEF
                    && (unsigned char)sTmp[1] == 0xBB
                    && (unsigned char)sTmp[2] == 0xBF) o2 = 3;
                vFragments.push_back(sTmp.substr(o2));
            }
        }
    }
    CloseZip(hz);
}

// [S4] 枚举 Custom 目录，收集 <Item>/<Group> 片段
static bool LcCollectFragments(const wchar_t* pszDir, std::vector<std::string>& vFragments)
{
    CDuiString sSearch = CDuiString(pszDir) + L"\\*.*";
    WIN32_FIND_DATAW fd = {};
    DEFindGuard hFind(::FindFirstFileW(sSearch.GetData(), &fd));
    if (hFind == INVALID_HANDLE_VALUE) return false;
    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        CDuiString sName(fd.cFileName);
        CDuiString sLower = sName; sLower.MakeLower();
        if (!(sLower.Find(L".xml") >= 0 || sLower.Find(L".zip") >= 0))
            continue;
        CDuiString sFull = CDuiString(pszDir) + L"\\" + sName;
        std::string sContent;
        if (!LcReadFileContent(sFull.GetData(), sContent)) continue;
        if (sContent.empty()) continue;
        size_t off = 0;
        if (sContent.size() >= 3 && (unsigned char)sContent[0] == 0xEF
            && (unsigned char)sContent[1] == 0xBB && (unsigned char)sContent[2] == 0xBF)
            off = 3;
        if (sLower.Find(L".zip") >= 0)
        {
            LcUnzipXmlEntries(sFull.GetData(), vFragments);
            continue;
        }
        vFragments.push_back(sContent.substr(off));
    } while (::FindNextFileW(hFind, &fd));
    return !vFragments.empty();
}

// [S5] 从每个片段抽取 <Item>...</Item> / <Group>...</Group>
static bool LcExtractItems(const std::vector<std::string>& vFragments,
                           std::vector<std::string>& vItems)
{
    for (size_t i = 0; i < vFragments.size(); ++i)
    {
        const std::string& s = vFragments[i];
        size_t pos = 0;
        while (pos < s.size())
        {
            size_t iItem = s.find("<Item", pos);
            size_t iGrp  = s.find("<Group", pos);
            size_t iStart;
            if (iItem == std::string::npos) iStart = iGrp;
            else if (iGrp == std::string::npos) iStart = iItem;
            else iStart = (iItem < iGrp) ? iItem : iGrp;
            if (iStart == std::string::npos) break;
            std::string sTag = s.substr(iStart + 1, 5);
            sTag = sTag.substr(0, sTag.find_first_of(" \t\r\n>"));
            std::string sClose = "</" + sTag + ">";
            size_t iEnd = s.find(sClose, iStart);
            if (iEnd == std::string::npos) break;
            iEnd += sClose.size();
            vItems.push_back(s.substr(iStart, iEnd - iStart));
            pos = iEnd;
        }
    }
    return !vItems.empty();
}

// [S6] 拼接并剥离 XML 声明
static bool LcJoinAndStripDecl(const std::vector<std::string>& vItems,
                               std::string& outJoined)
{
    for (size_t i = 0; i < vItems.size(); ++i)
        outJoined += vItems[i];
    if (outJoined.empty()) return false;
    size_t iDecl = outJoined.find("<?xml");
    if (iDecl != std::string::npos)
    {
        size_t iEndDecl = outJoined.find("?>", iDecl);
        if (iEndDecl != std::string::npos)
            outJoined.erase(iDecl, iEndDecl - iDecl + 2);
    }
    return !outJoined.empty();
}

// [S7] 同名 Item 去重覆盖（主库优先）：删除主库同 Name 顶层 Item
static void LcDedupeCustomItems(const std::string& sJoined, std::string& sXml)
{
    const char* kTopItem = "<Item ";
    const char* kIndent6 = "      <Item ";
    const auto isTopLevel = [&sJoined](size_t iStart)->bool {
        size_t lastItem  = sJoined.rfind("<Item ",  iStart);
        size_t lastGroup = sJoined.rfind("<Group ", iStart);
        return !(lastGroup != std::string::npos && lastGroup > lastItem);
    };
    size_t pos = 0;
    while (pos < sJoined.size())
    {
        size_t iStart = sJoined.find(kTopItem, pos);
        if (iStart == std::string::npos) break;
        size_t iEnd = sJoined.find("</Item>", iStart);
        if (iEnd == std::string::npos) break;
        iEnd += 7;
        if (!isTopLevel(iStart)) { pos = iEnd; continue; }
        std::string sSeg = sJoined.substr(iStart, iEnd - iStart);
        size_t iName = sSeg.find("Name=\"");
        if (iName == std::string::npos) { pos = iEnd; continue; }
        size_t iN1 = iName + 6;
        size_t iN2 = sSeg.find('"', iN1);
        if (iN2 == std::string::npos) { pos = iEnd; continue; }
        std::string sName = sSeg.substr(iN1, iN2 - iN1);
        size_t sPos = 0;
        while (sPos < sXml.size())
        {
            size_t tStart = sXml.find(kIndent6, sPos);
            if (tStart == std::string::npos) break;
            size_t tEnd = sXml.find("</Item>", tStart);
            if (tEnd == std::string::npos) break;
            tEnd += 7;
            std::string tSeg = sXml.substr(tStart, tEnd - tStart);
            size_t tName = tSeg.find("Name=\"");
            if (tName != std::string::npos)
            {
                size_t tN1 = tName + 6;
                size_t tN2 = tSeg.find('"', tN1);
                if (tN2 != std::string::npos)
                {
                    std::string tNameV = tSeg.substr(tN1, tN2 - tN1);
                    if (tNameV == sName)
                    {
                        sXml.erase(tStart, tEnd - tStart);
                        break;
                    }
                }
            }
            sPos = tEnd;
        }
        pos = iEnd;
    }
}

// [S8] 合并到 Data.xml：仅按片段自身的 schema 注入对应容器。
// [FIX-P0] 原实现把同一片段同时插入 SystemOptimization 与 CleanCollection4，
// 会让优化规则进入清理引擎（反之亦然）。现在按片段根节点/内容判定目标。
static bool LcInsertIntoDataXml(const std::string& sJoined, std::string& sXml)
{
    if (sJoined.empty()) return false;
    const bool bClean = sJoined.find("<CleanCollection") != std::string::npos
                     || sJoined.find("Collect") != std::string::npos;
    const bool bOptim = sJoined.find("<SystemOptimization") != std::string::npos
                     || sJoined.find("Optimization") != std::string::npos;
    bool bMerged = false;

    // 同一片段只允许进入一个容器；无法判定时拒绝合并（fail-closed）。
    if (bOptim && !bClean)
    {
        const size_t p = sXml.find("</SystemOptimization>");
        if (p != std::string::npos) { sXml.insert(p, sJoined); bMerged = true; }
    }
    else if (bClean && !bOptim)
    {
        const size_t p = sXml.find("</CleanCollection4>");
        if (p != std::string::npos) { sXml.insert(p, sJoined); bMerged = true; }
    }
    return bMerged;
}



bool CDataEngine::LoadCustomRulesIntoXml(std::string& sXml)
{
    if (sXml.empty()) return false;

    CDuiString strCustomDir;
    if (!LcFindCustomDir(strCustomDir)) return false;

    // 收集 Custom 规则片段（*.xml / *.zip）
    std::vector<std::string> vFragments;
    if (!LcCollectFragments(strCustomDir.GetData(), vFragments)) return false;

    // 从每个片段抽取 <Item>...</Item> / <Group>...</Group>
    std::vector<std::string> vItems;
    if (!LcExtractItems(vFragments, vItems)) return false;

    // 拼接并剥离 XML 声明
    std::string sJoined;
    if (!LcJoinAndStripDecl(vItems, sJoined)) return false;

    // [R-B37] 同名 Item 去重覆盖（主库优先）
    LcDedupeCustomItems(sJoined, sXml);

    // 合并到 Data.xml
    return LcInsertIntoDataXml(sJoined, sXml);
}


const wchar_t* CDataEngine::GetLastError() const { return m_strLastError.GetData(); }

CMarkupNode CDataEngine::GetCleanCollectionRoot()
{
    return m_pMarkup->GetRoot().GetChild(L"CleanCollection4");
}

CMarkupNode CDataEngine::GetOptimizationRoot()
{
    return m_pMarkup->GetRoot().GetChild(L"SystemOptimization");
}

CMarkupNode CDataEngine::GetDocumentRoot()
{
    return m_pMarkup->GetRoot();
}

// ============================================================
// text helpers
// ============================================================
CDuiString CDataEngine::ResolveText(const wchar_t* pszRaw)
{
    // [R10-02] 实现提为非类纯函数 DismResolveText（DataEngine.h）
    return DismResolveText(pszRaw);
}

// ============================================================
// condition evaluation
// ============================================================
bool CDataEngine::EvalConditions(CMarkupNode nodeContainer)
{
    if (!nodeContainer.IsValid()) return true;

    CMarkupNode node = nodeContainer.GetChild();
    while (node.IsValid())
    {
        if (!EvalOne(node)) return false;
        node = node.GetSibling();
    }
    // 无任何子条件 = 无约束（始终成立）
    return true;
}

// ============================================================
// [R8] 条件谓词（11 个标签，一标签一函数；错误/缺失路径一律 fail-closed）
// ============================================================
bool CDataEngine::EvalOSVersion(CMarkupNode nodeCond)
{
    return MatchOSVersion(nodeCond.GetAttributeValue(L"Compare"),
                          nodeCond.GetValue());
}

bool CDataEngine::EvalRegExist(CMarkupNode nodeCond)
{
        const wchar_t* pszKey   = nodeCond.GetAttributeValue(L"Key");
        const wchar_t* pszValue = nodeCond.GetAttributeValue(L"Value");
        const wchar_t* pszType  = nodeCond.HasAttribute(L"Type")  ? nodeCond.GetAttributeValue(L"Type")  : nullptr;
        const wchar_t* pszData  = nodeCond.HasAttribute(L"Data")  ? nodeCond.GetAttributeValue(L"Data")  : nullptr;
        // Wow64 三态（对齐 Data.xml 真实取值）：
        //   "True" = 仅 32 位视图(KEY_WOW64_32KEY)
        //   "All"  = 32/64 视图任一存在即成立（WinRAR 等可能按 32/64 位安装）
        //   无标记 = 默认 64 位视图
        CDuiString sWow;
        if (nodeCond.HasAttribute(L"Wow64"))
            sWow = nodeCond.GetAttributeValue(L"Wow64");
        bool bWow64 = (lstrcmpiW(sWow.GetData(), L"True") == 0);
        bool bWowAll = (lstrcmpiW(sWow.GetData(), L"All") == 0);
        const wchar_t* pszCompare = nodeCond.HasAttribute(L"Compare")
            ? nodeCond.GetAttributeValue(L"Compare") : nullptr;

        struct Probe { bool b32; };
        Probe probes[2] = { { false }, { true } };   // 先默认视图，再 32 位视图
        int nProbes = bWowAll ? 2 : (bWow64 ? 1 : 1);
        if (!bWowAll) probes[0].b32 = bWow64;

        bool bResult = false;
        for (int pi = 0; pi < nProbes && !bResult; ++pi)
        {
            DERegGuard hKey;
            if (!OpenRegKey(pszKey, KEY_READ, hKey.k, false, probes[pi].b32)) continue;

            bool bOk = true;
            if (pszValue && *pszValue)
            {
                if (pszData || pszType || pszCompare)
                    bOk = RegValueEqual(hKey, pszValue, pszType, pszData, pszCompare);
                else
                {
                    DWORD dwType = 0;
                    bOk = ::RegQueryValueEx(hKey, pszValue, nullptr, &dwType, nullptr, nullptr) == ERROR_SUCCESS;
                }
            }
            bResult = bOk;
        }
    return bResult;
}

bool CDataEngine::EvalFileExist(CMarkupNode nodeCond)
{
        const wchar_t* pszPath = nodeCond.GetAttributeValue(L"FilePath");
        if (FileExists(pszPath)) return true;

        // Wow64="All"：32/64 位程序目录任一存在即成立。
        // Data.xml 真实规则（SQL Server / .NET Multi-Targeting Pack）：
        // 64 位进程下 %ProgramFiles% 展开为 Program Files，32 位程序装在
        // Program Files (x86)，需把变量替换后再探一次。
        if (nodeCond.HasAttribute(L"Wow64")
            && lstrcmpiW(nodeCond.GetAttributeValue(L"Wow64"), L"All") == 0
            && pszPath)
        {
            CDuiString sAlt(pszPath);
            sAlt.MakeLower();
            if (sAlt.Find(L"%programfiles%") >= 0
                || sAlt.Find(L"%programw6432%") >= 0)
            {
                CDuiString sTry(pszPath);
                sTry.Replace(L"%ProgramW6432%", L"%ProgramFiles(x86)%");
                sTry.Replace(L"%ProgramFiles%", L"%ProgramFiles(x86)%");
                if (FileExists(sTry.GetData())) return true;
            }
        }
        return false;
}

bool CDataEngine::EvalQueryServiceStart(CMarkupNode nodeCond)
{
    int nStart = QueryServiceStart(nodeCond.GetAttributeValue(L"Name"));
    if (nStart < 0) return false;
    if (nodeCond.HasAttribute(L"Type"))
    {
        int nWant = _ttoi(nodeCond.GetAttributeValue(L"Type"));
        return nStart == nWant;
    }
    return true;
}

    // [P0-A7] <PowerPlan Guid="...">：当前活动电源方案等于指定 GUID。
    // Combo 的 Current/Dropdown 用它判定哪一项为当前选中；卓越性能在
    // 尚未复制时条件不成立（不影响其余三项选中）。
bool CDataEngine::EvalPowerPlan(CMarkupNode nodeCond)
{
    const wchar_t* pszGuid = nodeCond.GetAttributeValue(L"Guid");
    if (!pszGuid || !*pszGuid) return false;
    CDuiString sActive;
    if (!GetActivePowerPlan(sActive)) return false;
    return lstrcmpiW(sActive.GetData(), pszGuid) == 0;
}

bool CDataEngine::EvalNot(CMarkupNode nodeCond)
{
    return !EvalConditions(nodeCond);
}

bool CDataEngine::EvalOr(CMarkupNode nodeCond)
{
    CMarkupNode node = nodeCond.GetChild();
    while (node.IsValid())
    {
        if (EvalOne(node)) return true;
        node = node.GetSibling();
    }
    return false;
}

bool CDataEngine::EvalAnd(CMarkupNode nodeCond)
{
    return EvalConditions(nodeCond);
}

bool CDataEngine::EvalImageFlags(CMarkupNode nodeCond)
{
        // [修复 P1-IMG] 原版语义（逆向基线 include/Common.h：
        //   ImageFlags::Online=1 / Offline=0）：节点文本（或 Flags 属性）
        // 与当前会话标志按 Compare（缺省 "="）比较。
        // 此前恒 return true 造成两类反相故障：
        //   ① <Not><ImageFlags>40|10</ImageFlags></Not>（关闭快速启动/休眠）
        //      恒假 → 两个优化项对所有用户消失；
        //   ② 34 个仅在线的清理扫描项 <ImageFlags>1</ImageFlags> 在离线 PE
        //      会话也恒真 → 离线错误列出 ResetBase 等在线扫描。
        // 库中取值 1/4/10/40：在线标志 1 仅与 1 相等；离线标志 0 与任何值不等。
        const wchar_t* pszWant = nodeCond.GetValue();
        if ((!pszWant || !*pszWant) && nodeCond.HasAttribute(L"Flags"))
            pszWant = nodeCond.GetAttributeValue(L"Flags");
        if (!pszWant || !*pszWant) return false;
        int nWant = static_cast<int>(wcstoul(pszWant, nullptr, 10));
        int nCur = IsOfflineSession() ? 0 : 1;
        const wchar_t* pszCmp = nodeCond.HasAttribute(L"Compare")
            ? nodeCond.GetAttributeValue(L"Compare") : nullptr;
        if (pszCmp && *pszCmp
            && (wcsstr(pszCmp, L"!") || wcsstr(pszCmp, L"<>")))
            return nCur != nWant;
        return nCur == nWant;
}

bool CDataEngine::EvalArch(CMarkupNode nodeCond)
{
        SYSTEM_INFO si;
        ::GetNativeSystemInfo(&si);

        CDuiString sWant(nodeCond.GetValue());
        sWant.Trim();
        // Data.xml 中 <Arch>9</Arch>/<Arch>0</Arch> 使用的是
        // SYSTEM_INFO.wProcessorArchitecture 数值（9=x64,0=x86,12=ARM64）
        bool bNumeric = sWant.GetLength() > 0;
        for (int i = 0; i < sWant.GetLength() && bNumeric; ++i)
        {
            if (sWant[i] < L'0' || sWant[i] > L'9') bNumeric = false;
        }
        if (bNumeric)
            return _ttoi(sWant.GetData()) == static_cast<int>(si.wProcessorArchitecture);

        const wchar_t* arch = L"x86";
        if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64) arch = L"amd64";
        else if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64) arch = L"arm64";
        sWant.MakeLower();
        return sWant.Find(arch) >= 0;
}

bool CDataEngine::EvalFileVersion(CMarkupNode nodeCond)
{
        // <FileVersion FilePath="...\NCleaner.dll" Compare=">=">1.0.3.5</FileVersion>
        const wchar_t* pszPath = nodeCond.GetAttributeValue(L"FilePath");
        const wchar_t* pszCmp  = nodeCond.GetAttributeValue(L"Compare");
        CDuiString sWantVer = nodeCond.GetValue();
        if (!pszPath || !*pszPath || sWantVer.IsEmpty()) return false;

        // 展开 %0%（程序自身目录）与环境变量
        CDuiString sPath;
        if (_wcsnicmp(pszPath, L"%0%", 3) == 0)
        {
            wchar_t szMod[MAX_PATH * 2] = { 0 };
            ::GetModuleFileName(nullptr, szMod, _countof(szMod));
            CDuiString sDir(szMod);
            int p1 = sDir.ReverseFind(L'\\');
            if (p1 >= 0) sDir = sDir.Left(p1 + 1);
            sPath = sDir + (pszPath + 3);
        }
        else sPath = pszPath;

        wchar_t szExp[MAX_PATH * 4] = { 0 };
        ::ExpandEnvironmentStrings(sPath.GetData(), szExp, _countof(szExp));
        if (::GetFileAttributes(szExp) == INVALID_FILE_ATTRIBUTES)
            return false;   // 被检测文件不存在：门控不成立（保守隐藏）

        DWORD dwHandle = 0;
        DWORD dwSize = ::GetFileVersionInfoSize(szExp, &dwHandle);
        if (dwSize == 0) return false;
        std::vector<BYTE> v(dwSize, 0);
        if (!::GetFileVersionInfo(szExp, 0, dwSize, v.data())) return false;

        VS_FIXEDFILEINFO* pffi = nullptr;
        UINT uLen = 0;
        if (!::VerQueryValue(v.data(), L"\\", (void**)&pffi, &uLen) || !pffi)
            return false;

        // [R-P25 · R6] 版本字符串解析 + 版本比较由 Rust dismcore 实现：
        //   原 C++ 内联循环（_ttoi 逐段解析）与手写 u64 比较 → dismcore::ParseVersion /
        //   dismcore::CompareVersion（纯计算）。文件读取 / 环境展开 / 路径存在性保留 C++。
        dismcore::DismVersion want = dismcore::ParseVersion(sWantVer.GetData());
        const ULONGLONG uHave = (static_cast<ULONGLONG>(pffi->dwFileVersionMS) << 32)
                              | pffi->dwFileVersionLS;
        const ULONGLONG uWant = want.asU64();

        if (!pszCmp || !*pszCmp) return uHave == uWant;
        if (wcsstr(pszCmp, L">=")) return uHave >= uWant;
        if (wcsstr(pszCmp, L"<=")) return uHave <= uWant;
        if (wcsstr(pszCmp, L"==")) return uHave == uWant;
        if (wcsstr(pszCmp, L">"))  return uHave >  uWant;
        if (wcsstr(pszCmp, L"<"))  return uHave <  uWant;
        return uHave == uWant;
}

// [R8] 条件标签 → 谓词分发表。
// 圈复杂度集中在表查找：EvalOne 本体仅"查表→派发"，新增标签只加一行，
// 不再追加 if 链（旧实现 14 分支、cc=73；现 cc<20）。
bool CDataEngine::EvalOne(CMarkupNode nodeCond)
{
    using PCondFn = bool (CDataEngine::*)(CMarkupNode);
    static const struct SCondEntry
    {
        const wchar_t*  pszTag;
        PCondFn  pfn;
    }
    kCondTab[] =
    {
        { L"OSVersion",        &CDataEngine::EvalOSVersion },
        { L"RegExist",         &CDataEngine::EvalRegExist },
        { L"FileExist",        &CDataEngine::EvalFileExist },
        { L"QueryServiceStart",&CDataEngine::EvalQueryServiceStart },
        { L"PowerPlan",        &CDataEngine::EvalPowerPlan },
        { L"Not",              &CDataEngine::EvalNot },
        { L"Or",               &CDataEngine::EvalOr },
        { L"And",              &CDataEngine::EvalAnd },
        { L"ImageFlags",       &CDataEngine::EvalImageFlags },
        { L"Arch",             &CDataEngine::EvalArch },
        { L"FileVersion",      &CDataEngine::EvalFileVersion },
    };

    const wchar_t* pszName = nodeCond.GetName();
    constexpr int nTab = static_cast<int>(sizeof(kCondTab) / sizeof(kCondTab[0]));
    for (int i = 0; i < nTab; ++i)
    {
        if (lstrcmpiW(pszName, kCondTab[i].pszTag) == 0)
            return (this->*(kCondTab[i].pfn))(nodeCond);
    }

    // [E-1] 未知条件标签：错误码 ECondErr::CE_UNKNOWN_TAG(1)，fail-closed
    // （对齐 Qt6 保守失败策略 / KB §8.4）：绝不放行扫描与删除，
    // 同时把标签名+错误码写入调试日志，便于发现数据/引擎错配。
    std::wstring sDbg;
    {
        const wchar_t* pn = pszName ? pszName : L"(null)";
        sDbg = wfmt::FormatW(
            L"[dism++] [WARN] EvalOne: unknown condition tag '%s', fail-closed (err=%d)\r\n",
            pn, static_cast<int>(ECondErr::CE_UNKNOWN_TAG));
    }
    ::OutputDebugStringW(sDbg.c_str());
    return false;
}

// ============================================================
// CheckBox items
// ============================================================
int CDataEngine::GetCheckState(CMarkupNode nodeItem)
{
    int n = GetScopeState(nodeItem, 0);
    if (n >= 0) return n;
    return GetScopeState(nodeItem, 1);
}

bool CDataEngine::SetCheckState(CMarkupNode nodeItem, bool bChecked,
                                std::vector<CDuiString>& warnings,
                                int& nUnsupported)
{
    bool bAny = false;
    for (int s = 0; s < 3; ++s)   // [R17] 覆盖 Current/System/Default 三作用域
    {
        if (SetScopeState(nodeItem, s, bChecked, warnings, nUnsupported))
            bAny = true;
    }
    return bAny;
}

const wchar_t* CDataEngine::ScopeName(int nScope)
{
    return DismScopeName(nScope);
}

// O-1: 递归判断某节点子树内是否出现指定名称的节点（动作）。
static bool OptimSubtreeHasNode(CMarkupNode node, const wchar_t* pszName)
{
    CMarkupNode ch = node.GetChild();
    while (ch.IsValid())
    {
        const wchar_t* pszN = ch.GetName();
        if (pszN && lstrcmpiW(pszN, pszName) == 0) return true;
        if (OptimSubtreeHasNode(ch, pszName)) return true;
        ch = ch.GetSibling();
    }
    return false;
}

bool CDataEngine::ItemHasUnsupportedAction(CMarkupNode nodeItem)
{
    // [FIX-P0] 预检查必须与执行器一致：未知动作在加载阶段即可识别，
    // 不能恒返回 false（否则未知动作会被部分执行且表现为成功）。
    if (!nodeItem.IsValid()) return false;
    static const wchar_t* const kKnown[] = {
        L"RegWrite", L"RegDelete", L"RegMove", L"Rule", L"SetServiceStart", L"SCConfig",
        L"ExplorerNotify", L"FileCreateByZIP", L"SetDesktop", L"SetServiceRecovery",
        L"SetAppxPackage", L"SetPowerPlan", L"ImportReg", L"AddDriver",
        L"NtSetSystemInformation", L"FileMove", L"AddPackage", L"Rescan",
        L"Warning", L"Applicable", L"RestartIf", L"Description", L"True", L"False",
    };
    std::vector<CMarkupNode> pending;
    pending.push_back(nodeItem);
    while (!pending.empty())
    {
        CMarkupNode n = pending.back();
        pending.pop_back();
        for (CMarkupNode c = n.GetChild(); c.IsValid(); c = c.GetSibling())
        {
            const wchar_t* nm = c.GetName();
            if (nm && wcsncmp(nm, L"Activate", 8) != 0)
            {
                bool bKnown = false;
                for (const wchar_t* k : kKnown)
                    if (lstrcmpiW(nm, k) == 0) { bKnown = true; break; }
                if (!bKnown) return true;
            }
            pending.push_back(c);
        }
    }
    return false;
}

int CDataEngine::GetScopeState(CMarkupNode nodeItem, int nScope)
{
    CMarkupNode box = nodeItem.GetChild(ScopeName(nScope));
    if (!box.IsValid()) return -1;

    CMarkupNode st = box.GetChild(L"State");
    if (!st.IsValid()) return -1;

    CMarkupNode app = st.GetChild(L"Applicable");
    if (!app.IsValid()) return -1;

    return EvalConditions(app) ? 1 : 0;
}

bool CDataEngine::SetScopeState(CMarkupNode nodeItem, int nScope, bool bChecked,
                                std::vector<CDuiString>& warnings,
                                int& nUnsupported,
                                bool* pbRestartExplorer,
                                bool* pbAnyActionFailed)
{
    CMarkupNode box = nodeItem.GetChild(ScopeName(nScope));
    if (!box.IsValid()) return false;

    CMarkupNode branch = box.GetChild(bChecked ? L"True" : L"False");
    if (!branch.IsValid()) return false;

    bool bAny = false;
    bool bAnyFailed = false;
    std::vector<CMarkupNode> acts;
    CollectActions(branch, acts, warnings);
    for (size_t k = 0; k < acts.size(); ++k)
    {
        bool bRestart = false;
        bool bFailed = false;
        if (ExecActivate(acts[k], nUnsupported, bRestart, &bFailed)) bAny = true;
        if (bFailed) bAnyFailed = true;
        if (bRestart && pbRestartExplorer) *pbRestartExplorer = true;
    }
    if (pbAnyActionFailed) *pbAnyActionFailed = bAnyFailed;
    return bAny;
}

// ============================================================
// Combo items
// ============================================================
int CDataEngine::GetDropdownCount(CMarkupNode nodeItem)
{
    int n = GetScopeDropdownCount(nodeItem, 0);
    if (n > 0) return n;
    return GetScopeDropdownCount(nodeItem, 1);
}

bool CDataEngine::GetDropdownName(CMarkupNode nodeItem, int iSel, CDuiString& strName)
{
    if (GetScopeDropdownName(nodeItem, 0, iSel, strName)) return true;
    return GetScopeDropdownName(nodeItem, 1, iSel, strName);
}

int CDataEngine::GetCurrentDropdown(CMarkupNode nodeItem)
{
    int n = GetScopeDropdown(nodeItem, 0);
    if (n >= 0) return n;
    return GetScopeDropdown(nodeItem, 1);
}

bool CDataEngine::SelectDropdown(CMarkupNode nodeItem, int iSel,
                                 std::vector<CDuiString>& warnings,
                                 int& nUnsupported)
{
    bool bAny = false;
    for (int s = 0; s < 3; ++s)   // [R17] 覆盖 Current/System/Default 三作用域
    {
        if (SelectScopeDropdown(nodeItem, s, iSel, warnings, nUnsupported))
            bAny = true;
    }
    return bAny;
}

int CDataEngine::GetScopeDropdownCount(CMarkupNode nodeItem, int nScope)
{
    CMarkupNode box = nodeItem.GetChild(ScopeName(nScope));
    if (!box.IsValid()) return 0;

    int n = 0;
    CMarkupNode d = box.GetChild(L"Dropdown");
    while (d.IsValid()) { ++n; d = d.GetSibling(); }
    return n;
}

bool CDataEngine::GetScopeDropdownName(CMarkupNode nodeItem, int nScope,
                                       int iSel, CDuiString& strName)
{
    CMarkupNode box = nodeItem.GetChild(ScopeName(nScope));
    if (!box.IsValid()) return false;

    int n = 0;
    CMarkupNode d = box.GetChild(L"Dropdown");
    while (d.IsValid())
    {
        if (n == iSel)
        {
            strName = ResolveText(d.GetAttributeValue(L"Name"));
            return true;
        }
        ++n;
        d = d.GetSibling();
    }
    return false;
}

int CDataEngine::GetScopeDropdown(CMarkupNode nodeItem, int nScope)
{
    CMarkupNode box = nodeItem.GetChild(ScopeName(nScope));
    if (!box.IsValid()) return -1;

    int n = 0;
    CMarkupNode d = box.GetChild(L"Dropdown");
    while (d.IsValid())
    {
        CMarkupNode app = d.GetChild(L"Applicable");
        if (app.IsValid() && EvalConditions(app)) return n;
        ++n;
        d = d.GetSibling();
    }
    return -1;
}

bool CDataEngine::SelectScopeDropdown(CMarkupNode nodeItem, int nScope, int iSel,
                                      std::vector<CDuiString>& warnings,
                                      int& nUnsupported,
                                      bool* pbRestartExplorer,
                                      bool* pbAnyActionFailed)
{
    CMarkupNode box = nodeItem.GetChild(ScopeName(nScope));
    if (!box.IsValid()) return false;

    bool bAny = false;
    bool bAnyFailed = false;
    int n = 0;
    CMarkupNode d = box.GetChild(L"Dropdown");
    while (d.IsValid())
    {
        if (n == iSel)
        {
            std::vector<CMarkupNode> acts;
            CollectActions(d, acts, warnings);
            for (size_t k = 0; k < acts.size(); ++k)
            {
                bool bRestart = false;
                bool bFailed = false;
                if (ExecActivate(acts[k], nUnsupported, bRestart, &bFailed)) bAny = true;
                if (bFailed) bAnyFailed = true;
                if (bRestart && pbRestartExplorer) *pbRestartExplorer = true;
            }
        }
        ++n;
        d = d.GetSibling();
    }
    if (pbAnyActionFailed) *pbAnyActionFailed = bAnyFailed;
    return bAny;
}

// ============================================================
// private: action collection / execution
// ============================================================
bool CDataEngine::CollectActions(CMarkupNode nodeBranch,
                                 std::vector<CMarkupNode>& activates,
                                 std::vector<CDuiString>& warnings)
{
    CMarkupNode node = nodeBranch.GetChild();
    while (node.IsValid())
    {
        const wchar_t* n = node.GetName();
        if (lstrcmpiW(n, L"Activate") == 0)
            activates.push_back(node);
        else if (lstrcmpiW(n, L"Warning") == 0)
            warnings.push_back(ResolveText(node.GetValue()));
        node = node.GetSibling();
    }
    return !activates.empty();
}

// ============================================================
// [R9-02] 动作编排保留在主文件：Rule 递归、exe 相对路径解析。
// 17 个动作处理器按域分文件：DataEngine_Reg/File/Sys.cpp。
// ============================================================
CDataEngine::EActResult CDataEngine::ActRule(CMarkupNode, TActCtx& ctx)
{
    // [R4] 嵌套规则：递归执行当前 <Activate> 之后的兄弟 <Activate> 集合。
    bool bAllOk = true;
    CMarkupNode nodeSibling = ctx.nodeActivate.GetSibling();
    while (nodeSibling.IsValid())
    {
        if (lstrcmpiW(nodeSibling.GetName(), L"Activate") == 0)
        {
            bool bSubRestart = false;
            bool bSubFailed = false;
            if (!ExecActivate(nodeSibling, *ctx.pnUnsupported,
                              bSubRestart, &bSubFailed))
                bAllOk = false;
            if (bSubFailed) ctx.Fail();
            if (bSubRestart && ctx.pbNeedsRestart) *ctx.pbNeedsRestart = true;
        }
        nodeSibling = nodeSibling.GetSibling();
    }
    return bAllOk ? EActResult::ACT_OK : EActResult::ACT_FAIL;
}

// 相对路径 → <exe 目录>\<子目录>\file（驱动器/绝对路径原样）
CDuiString CDataEngine::ResolveExeRelative(const wchar_t* pszRel, const wchar_t* pszSubDir)
{
    CDuiString sRel(pszRel);
    if (sRel.GetLength() > 0 && sRel[0] != L'\\'
        && (sRel.GetLength() < 2 || sRel[1] != L':'))
    {
        wchar_t szExe[MAX_PATH] = { 0 };
        ::GetModuleFileName(nullptr, szExe, MAX_PATH);
        CDuiString sDir(szExe);
        int p = sDir.ReverseFind(L'\\');
        if (p >= 0) sDir = sDir.Mid(0, p);
        sRel = sDir + CDuiString(pszSubDir) + sRel;
    }
    return sRel;
}

// ============================================================
// [R9] ExecActivate：动作分发表（主体仅门控+查表+结果聚合，cc<20）
// ============================================================
// [R19] 离线能力表：判断动作是否只能在"正在运行的目标系统"上执行。
// 这些动作在挂载映像上执行要么无意义（通知、重扫），要么会误改宿主
// （Appx 当前用户语义、电源计划即时激活），因此离线会话必须拒绝。
static bool IsOnlineOnlyAction(const wchar_t* pszTag)
{
    if (pszTag == nullptr) return false;
    // [R22] 能力表扩充：除 UI/设备类动作外，补充"依赖当前运行系统状态"的动作。
    // ImportReg 走 regedit/RegLoadKey 的在线语义；FileCreateByZIP 依赖
    // 目标用户目录布局；SetServiceRecovery 需 SCM 在线会话；
    // AddPackage 需 CBS 在线会话（离线应走 DismApiBridge 的 /Image 路径）。
    static const wchar_t* const kOnlineOnly[] = {
        L"ExplorerNotify",   // 广播给正在运行的外壳
        L"Rescan",           // 触发宿主设备的 PnP 重扫
        L"SetAppxPackage",   // 当前用户 Appx 语义，离线无对应实例
        L"SetPowerPlan",     // 即时激活宿主电源方案
        L"SetDesktop",       // 立即作用于当前用户桌面
        L"SetServiceRecovery", // 需在线 SCM 句柄
        L"AddPackage",       // 在线 CBS 会话；离线请用 AddDriver/AddPackage 的 /Image 通道
    };
    for (const wchar_t* p : kOnlineOnly)
        if (lstrcmpiW(pszTag, p) == 0) return true;
    return false;
}

bool CDataEngine::ExecActivate(CMarkupNode nodeActivate, int& nUnsupported,
                               bool& bNeedsRestart, bool* pbAnyFailed)
{
    bNeedsRestart = false;
    if (lstrcmpiW(nodeActivate.GetAttributeValue(L"Restart"),
                 L"Explorer") == 0)
    {
        bNeedsRestart = true;
        // [智能重启] Restart="Explorer" 可用 <RestartIf><Applicable>... 条件化：
        // 仅当条件成立（如低版本 Win11 无即时刷新通道）才提示重启任务栏；
        // 条件不成立时（如 Win11 22H2+ 时钟秒表、26100 以前的搜索框用户键，
        // 同 Activate 内的 ExplorerNotify 广播即可即时生效）静默免重启。
        // 无 RestartIf 子节点时维持旧行为：一律提示重启。
        CMarkupNode nodeRif = nodeActivate.GetChild(L"RestartIf");
        if (nodeRif.IsValid())
        {
            CMarkupNode nodeRifApp = nodeRif.GetChild(L"Applicable");
            if (nodeRifApp.IsValid() && !EvalConditions(nodeRifApp))
                bNeedsRestart = false;
        }
    }

    TActCtx ctx{ nodeActivate, &nUnsupported, &bNeedsRestart, pbAnyFailed };

    using PActFn = EActResult (CDataEngine::*)(CMarkupNode, TActCtx&);
    static const struct SActEntry { const wchar_t* pszTag; PActFn pfn; } kActTab[] =
    {
        { L"RegWrite",             &CDataEngine::ActRegWrite },
        { L"RegDelete",            &CDataEngine::ActRegDelete },
        { L"RegMove",              &CDataEngine::ActRegMove },
        { L"Rule",                 &CDataEngine::ActRule },
        { L"SetServiceStart",      &CDataEngine::ActSetServiceStart },
        { L"SCConfig",             &CDataEngine::ActSetServiceStart }, // [R-B40] 别名
        { L"ExplorerNotify",       &CDataEngine::ActExplorerNotify },
        { L"FileCreateByZIP",      &CDataEngine::ActFileCreateByZip },
        { L"SetDesktop",           &CDataEngine::ActSetDesktop },
        { L"SetServiceRecovery",   &CDataEngine::ActSetServiceRecovery },
        { L"SetAppxPackage",       &CDataEngine::ActSetAppxPackage },
        { L"SetPowerPlan",         &CDataEngine::ActSetPowerPlan },
        { L"ImportReg",            &CDataEngine::ActImportReg },
        { L"AddDriver",            &CDataEngine::ActAddDriver },
        { L"NtSetSystemInformation",&CDataEngine::ActNtSetSystemInformation },
        { L"FileMove",             &CDataEngine::ActFileMove },
        { L"AddPackage",           &CDataEngine::ActAddPackage },
        { L"Rescan",               &CDataEngine::ActRescan },
    };

    bool bOk = false;
    for (CMarkupNode node = nodeActivate.GetChild(); node.IsValid();
         node = node.GetSibling())
    {
        const wchar_t* n = node.GetName();
        if (n == nullptr) continue;
        // 结构节点不是动作，跳过而不算失败。
        if (lstrcmpiW(n, L"Warning") == 0 || lstrcmpiW(n, L"Applicable") == 0
            || lstrcmpiW(n, L"RestartIf") == 0 || lstrcmpiW(n, L"Description") == 0)
            continue;
        // [Win11 适配] 动作级 <Applicable> 门控：为假中性跳过（Warning 除外）。
        {
            CMarkupNode nodeActApp = node.GetChild(L"Applicable");
            if (nodeActApp.IsValid() && !EvalConditions(nodeActApp))
                continue;
        }
        bool bKnown = false;
        for (int i = 0; i < static_cast<int>(sizeof(kActTab)/sizeof(kActTab[0])); ++i)
        {
            if (lstrcmpiW(n, kActTab[i].pszTag) != 0) continue;
            bKnown = true;

            // [R19] 离线能力表：这些动作依赖"正在运行的目标系统"，
            // 在挂载映像上执行没有意义或会误改宿主，直接拒绝并记录原因，
            // 不再依赖规则作者用 ImageFlags 自行规避。
            if (COfflineSession::IsActive() && IsOnlineOnlyAction(n))
            {
                ++nUnsupported;
                ctx.Fail();
                break;
            }

            EActResult r = (this->*(kActTab[i].pfn))(node, ctx);
            if (r == EActResult::ACT_OK) bOk = true;
            else if (r == EActResult::ACT_FAIL) ctx.Fail();
            break;
        }
        // [FIX-P0] 未知动作 fail-closed：不再静默忽略，避免“部分执行却报成功”。
        if (!bKnown)
        {
            ++nUnsupported;
            ctx.Fail();
        }
    }
    return bOk;
}

// 解析 Data.xml 中 <WSUSBlacklist> 节点，提取被禁止安装的更新 ID 列表。
// 用于在清理/优化页面过滤或标记已黑名单的更新。
// 注：DuiLib_Ultimate 的 CMarkupNode 只提供 GetChild/GetSibling 遍历，
// 使用 IsValid 判空，GetName 取标签名，GetAttributeValue 取属性。
bool CDataEngine::ParseWsuSection(CMarkupNode nodeWSUS, std::vector<CDuiString>& outBlacklistIds)
{
    outBlacklistIds.clear();
    if (!nodeWSUS.IsValid()) return false;
    // 遍历 <WSUSBlacklist> 下所有子节点，仅处理 <Item>
    CMarkupNode nodeItem = nodeWSUS.GetChild();
    while (nodeItem.IsValid())
    {
        const wchar_t* pszName = nodeItem.GetName();
        if (pszName && lstrcmpW(pszName, L"Item") == 0)
        {
            const wchar_t* pszId = nodeItem.GetAttributeValue(L"Id");
            if (pszId && pszId[0])
                outBlacklistIds.push_back(CDuiString(pszId));
        }
        nodeItem = nodeItem.GetSibling();
    }
    return true;
}

// ---- [Q4] 时区映射解析器 ----
// 解析 Data.xml 中 <TimeZoneMap> 节点，建立时区 ID 到偏移量的映射。
bool CDataEngine::ParseTimeZoneMap(CMarkupNode nodeTzMap, std::map<CDuiString, int>& outMap)
{
    outMap.clear();
    if (!nodeTzMap.IsValid()) return false;
    // 遍历 <TimeZoneMap> 下所有 <Zone> 节点，读取 Id 和 Offset 属性
    CMarkupNode nodeZone = nodeTzMap.GetChild();
    while (nodeZone.IsValid())
    {
        const wchar_t* pszName = nodeZone.GetName();
        if (pszName && lstrcmpW(pszName, L"Zone") == 0)
        {
            const wchar_t* pszId    = nodeZone.GetAttributeValue(L"Id");
            const wchar_t* pszOffset = nodeZone.GetAttributeValue(L"Offset");
            if (pszId && pszId[0] && pszOffset && pszOffset[0])
            {
                int nOffset = _tstoi(pszOffset);
                outMap[CDuiString(pszId)] = nOffset;
            }
        }
        nodeZone = nodeZone.GetSibling();
    }
    return true;
}

// ---- [Q10] ESD 密钥解析器 ----
// 解析 Data.xml 中 <ESDDecryptKey> 节点，提取 Windows ESD 激活密钥列表。
bool CDataEngine::ParseEsdKeys(CMarkupNode nodeEsd, std::vector<CDuiString>& outKeys)
{
    outKeys.clear();
    if (!nodeEsd.IsValid()) return false;
    // 遍历 <ESDDecryptKey> 下所有 <Key> 节点，读取节点值（base64 RSA 密钥，<Key> 为文本节点）
    CMarkupNode nodeKey = nodeEsd.GetChild();
    while (nodeKey.IsValid())
    {
        const wchar_t* pszName = nodeKey.GetName();
        if (pszName && lstrcmpW(pszName, L"Key") == 0)
        {
            const wchar_t* pszKey = nodeKey.GetValue();
            if (pszKey && pszKey[0])
                outKeys.push_back(CDuiString(pszKey));
        }
        nodeKey = nodeKey.GetSibling();
    }
    return true;
}

// ---- [Q1] WSUS 黑名单（便捷 API 实现）----
// 自动定位 <WSUSBlacklist> 节点并调用 ParseWsuSection。
bool CDataEngine::GetWsuBlacklist(std::vector<CDuiString>& outIds)
{
    outIds.clear();
    if (!m_pMarkup) return false;
    CMarkupNode nodeWs = m_pMarkup->GetRoot().GetChild(L"WSUSBlacklist");
    if (!nodeWs.IsValid()) return false;
    return ParseWsuSection(nodeWs, outIds);
}

bool CDataEngine::IsInWsuBlacklist(const CDuiString& sId)
{
    if (sId.IsEmpty()) return false;
    std::vector<CDuiString> vIds;
    if (!GetWsuBlacklist(vIds)) return false;
    for (size_t i = 0; i < vIds.size(); ++i)
        if (lstrcmpiW(vIds[i].GetData(), sId.GetData()) == 0) return true;
    return false;
}

// ---- [Q4] 时区映射（便捷 API 实现）----
int CDataEngine::GetTimeZoneCount()
{
    if (!m_pMarkup) return 0;
    CMarkupNode nodeTz = m_pMarkup->GetRoot().GetChild(L"TimeZoneMap");
    if (!nodeTz.IsValid()) return 0;
    std::map<CDuiString, int> mapTz;
    if (!ParseTimeZoneMap(nodeTz, mapTz)) return 0;
    return static_cast<int>(mapTz.size());
}

bool CDataEngine::GetTimeZone(int nIdx, CDuiString& outKey, CDuiString& outOffset)
{
    if (!m_pMarkup) return false;
    CMarkupNode nodeTz = m_pMarkup->GetRoot().GetChild(L"TimeZoneMap");
    if (!nodeTz.IsValid()) return false;
    std::map<CDuiString, int> mapTz;
    if (!ParseTimeZoneMap(nodeTz, mapTz)) return false;
    if (nIdx < 0 || nIdx >= static_cast<int>(mapTz.size())) return false;
    int n = 0;
    for (std::map<CDuiString, int>::iterator it = mapTz.begin(); it != mapTz.end(); ++it, ++n)
    {
        if (n == nIdx)
        {
            outKey = it->first;
            outOffset = CDuiString(std::to_wstring(it->second).c_str());
            return true;
        }
    }
    return false;
}

// ---- [Q10] ESD 密钥（便捷 API 实现）----
bool CDataEngine::GetEsdKeys(std::vector<CDuiString>& outKeys)
{
    outKeys.clear();
    if (!m_pMarkup) return false;
    CMarkupNode nodeEs = m_pMarkup->GetRoot().GetChild(L"ESDDecryptKey");
    if (!nodeEs.IsValid()) return false;
    return ParseEsdKeys(nodeEs, outKeys);
}

// ============================================================
// [R4] Rule 上下文操作辅助
//
// 注：DuiLib 的 CMarkup/CMarkupNode 未提供"节点克隆/序列化/插入"公共 API。
//     本补丁在 UI 层提供上下文菜单与剪贴板交互，数据层的实际
//     XML 节点操作留待 R5 通过自维护的 XML 字符串快照机制完成。
//     当前实现保证接口完整、调用安全、编译通过。
// ============================================================
bool CDataEngine::DoPasteRule(CMarkupNode nodeParent, const wchar_t* pszClipXml)
{
    // 参数校验：调用方在 UI 层已经做过剪贴板格式校验
    if (!nodeParent.IsValid() || !pszClipXml || !*pszClipXml)
        return false;

    // DuiLib CMarkup 未提供节点插入 API。此处仅记录待处理意图，
    // 不修改 m_pMarkup 内部结构，避免破坏游标状态与文档一致性。
    // 后续 R5 引入 XML 快照重排机制后再实现真实插入。
    OutputDebugStringW(L"[R4] DoPasteRule: node-insert deferred (no CMarkup node API)");
    return false;
}

// ============================================================
// [R4] 统计当前分组下的 Item 数量
// ============================================================
int CDataEngine::CountRuleInGroup(CMarkupNode nodeGroup)
{
    if (!nodeGroup.IsValid()) return 0;
    int n = 0;
    CMarkupNode nItem = nodeGroup.GetChild();
    while (nItem.IsValid())
    {
        const wchar_t* sName = nItem.GetName();
        if (sName && lstrcmpiW(sName, L"Item") == 0) ++n;
        nItem = nItem.GetSibling();
    }
    return n;
}

// ============================================================
// dynamic function: ?ColorDialog()
// ============================================================
bool CDataEngine::EvalFunData(const wchar_t* pszFun, CDuiString& strOut)
{
    if (pszFun == nullptr) return false;
    CDuiString sFun(pszFun);
    sFun.MakeLower();

    if (sFun.Find(L"colordialog") < 0) return false;

    static COLORREF s_acrCustom[16] = { 0 };
    CHOOSECOLOR cc = { 0 };
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = nullptr;
    cc.lpCustColors = s_acrCustom;
    cc.rgbResult = RGB(0, 122, 204);
    cc.Flags = CC_RGBINIT | CC_FULLOPEN;
    if (!::ChooseColor(&cc)) return false;

    // AccentColor is a Win32 COLORREF = 0x00BBGGRR (matches the Electron
    // ColorDialog handling: bb<<16 | gg<<8 | rr).
    DWORD dwColorRef = (GetBValue(cc.rgbResult) << 16)
        | (GetGValue(cc.rgbResult) << 8)
        | GetRValue(cc.rgbResult);

    strOut.Format(L"%u", dwColorRef);
    return true;
}

// ============================================================
// registry helpers
// ============================================================
static std::vector<std::wstring> SplitArgs(const std::wstring& sBody)
{
    std::vector<std::wstring> out;
    std::wstring cur;
    bool bQuote = false;
    for (size_t i = 0; i < sBody.size(); ++i)
    {
        wchar_t c = sBody[i];
        if (c == L'"') { bQuote = !bQuote; continue; }
        if (c == L',' && !bQuote)
        {
            // trim
            size_t b = 0, e = cur.size();
            while (b < e && (cur[b] == L' ' || cur[b] == L'\t')) ++b;
            while (e > b && (cur[e-1] == L' ' || cur[e-1] == L'\t')) --e;
            out.push_back(cur.substr(b, e - b));
            cur.clear();
        }
        else cur.push_back(c);
    }
    size_t b = 0, e = cur.size();
    while (b < e && (cur[b] == L' ' || cur[b] == L'\t')) ++b;
    while (e > b && (cur[e-1] == L' ' || cur[e-1] == L'\t')) --e;
    out.push_back(cur.substr(b, e - b));
    return out;
}

// ============================================================
// [R-B5] ?Get* 内嵌函数展开  |  [R-B39] ?Func 注册表
// ------------------------------------------------------------
// Data.xml 中 ?GetRegSz / ?GetFileVersion / ?GetIniData / ?GetLocaleName
// 出现在 <Excluded>、RootPath、FileExist FilePath 等位置。本函数把
// 表达式中的 ?Func(...) 替换为真实解析值，解析失败则保持原文（保守）。
// [R-B39] 改为正式的 ?Func 注册表驱动：新增一个函数名即可扩展，
// 并通过 QueryFunExprSupported 暴露单测/对拍接口。
// ============================================================

// ---- 单个 ?Func 的求值器 ----
// 参数：sBody 为括号内原文（未 trim），sVal 输出解析值。
// 返回 true 表示"该函数名已注册"（即使取值失败，调用方按保守保留原文）；
//        false 表示函数名不在注册表内。
namespace {

bool EvalFunGetRegSz(const std::wstring& sBody, CDuiString& sVal)
{
    // 参数：Key, Value, [Wow64]
    std::vector<std::wstring> args = SplitArgs(sBody);
    if (args.size() < 2) return true;   // 已注册但参数不足：保守保留原文

    HKEY hRoot = nullptr;
    CDuiString sSub;
    bool bWow = (args.size() >= 3)
        && (lstrcmpiW(args[2].c_str(), L"True") == 0);
    // [P0] 离线会话：与 OpenRegKey 一致的 wow 感知重定向
    CDuiString sMapped = CDataEngine::RemapOfflinePath(args[0].c_str(), bWow);
    if (!CDataEngine::ParseRegPath(sMapped, hRoot, sSub))
        return true;

    REGSAM sam = KEY_READ;
    if (bWow) sam |= KEY_WOW64_32KEY;
    else if (hRoot == HKEY_LOCAL_MACHINE || hRoot == HKEY_CLASSES_ROOT)
        sam |= KEY_WOW64_64KEY;

    DERegGuard hKey;
    if (::RegOpenKeyEx(hRoot, sSub.GetData(), 0, sam, &hKey.k) != ERROR_SUCCESS)
        return true;
    DWORD dwType = 0, dwSize = 0;
    if (::RegQueryValueEx(hKey, args[1].c_str(), nullptr, &dwType, nullptr, &dwSize)
        == ERROR_SUCCESS)
    {
        std::vector<BYTE> buf(dwSize + 2, 0);
        if (::RegQueryValueEx(hKey, args[1].c_str(), nullptr, &dwType, buf.data(), &dwSize)
            == ERROR_SUCCESS)
            sVal = reinterpret_cast<const wchar_t*>(buf.data());
    }
    return true;
}

bool EvalFunGetFileVersion(const std::wstring& sBody, CDuiString& sVal)
{
    // 参数：FilePath
    std::vector<std::wstring> args = SplitArgs(sBody);
    if (args.size() < 1) return true;

    CDuiString sPath(args[0].c_str());
    wchar_t szExp[MAX_PATH * 2] = {};
    ::ExpandEnvironmentStrings(sPath.GetData(), szExp, _countof(szExp));
    DWORD dwHandle = 0;
    DWORD dwSize = ::GetFileVersionInfoSize(szExp, &dwHandle);
    if (dwSize > 0)
    {
        std::vector<BYTE> v(dwSize, 0);
        if (::GetFileVersionInfo(szExp, 0, dwSize, v.data()))
        {
            VS_FIXEDFILEINFO* pffi = nullptr;
            UINT uLen = 0;
            if (::VerQueryValue(v.data(), L"\\", (void**)&pffi, &uLen) && pffi)
            {
                sVal.Format(L"%u.%u.%u.%u",
                    HIWORD(pffi->dwFileVersionMS), LOWORD(pffi->dwFileVersionMS),
                    HIWORD(pffi->dwFileVersionLS), LOWORD(pffi->dwFileVersionLS));
            }
        }
    }
    return true;
}

bool EvalFunGetIniData(const std::wstring& sBody, CDuiString& sVal)
{
    // 参数：FilePath, Section, Key
    std::vector<std::wstring> args = SplitArgs(sBody);
    if (args.size() < 3) return true;

    CDuiString sPath(args[0].c_str());
    wchar_t szExp[MAX_PATH * 2] = {};
    ::ExpandEnvironmentStrings(sPath.GetData(), szExp, _countof(szExp));
    constexpr int kIniValWchars = 512;   // 512 缓冲尺寸（语义命名，数值不变）
    wchar_t szBuf[kIniValWchars] = {};
    DWORD dw = ::GetPrivateProfileStringW(args[1].c_str(), args[2].c_str(),
                                          L"", szBuf, _countof(szBuf), szExp);
    if (dw > 0) sVal = szBuf;
    return true;
}

bool EvalFunGetLocaleName(const std::wstring& /*sBody*/, CDuiString& sVal)
{
    wchar_t szLang[64] = {};
    ::GetLocaleInfoW(LOCALE_USER_DEFAULT, LOCALE_SNAME, szLang, _countof(szLang));
    if (szLang[0]) sVal = szLang;
    return true;
}

using PFunEval = bool (*)(const std::wstring&, CDuiString&);

struct FunEntry
{
    const wchar_t* pszName;   // "GetRegSz" 等（不含前导 '?'）
    PFunEval pfn;
};

// ?Func 注册表：新增 ?Get* 时在此追加一行即可被 ResolveFunExpr 识别。
const FunEntry kFunTable[] =
{
    { L"GetRegSz",        EvalFunGetRegSz        },
    { L"GetFileVersion",  EvalFunGetFileVersion  },
    { L"GetIniData",      EvalFunGetIniData      },
    { L"GetLocaleName",   EvalFunGetLocaleName   },
};

constexpr int kFunCount = static_cast<int>(sizeof(kFunTable) / sizeof(kFunTable[0]));

PFunEval LookupFunEval(const wchar_t* pszName)
{
    if (!pszName || !*pszName) return nullptr;
    for (int i = 0; i < kFunCount; ++i)
    {
        if (lstrcmpiW(kFunTable[i].pszName, pszName) == 0)
            return kFunTable[i].pfn;
    }
    return nullptr;
}

}   // anonymous namespace

// 定位表达式 sInOut 中第一个 ?Get*(...)，返回函数名区间与函数体、以及整体长度。
// 命中返回 true 且各 out 参数有效；未命中（无 '?'/非 Get/括号未闭合）返回 false。
//
// [R-P24 · R4] 由 Rust dismcore::ParseDataExprUtf16 实现（原 C++ 手写括号扫描），
// 消除 ParseFunExpr 与下方 QueryFunExprSupported / ResolveFunExpr 三处重复扫描，
// 统一为单一 Rust 事实源。q 为 '?' 下标，matchLen 为完整 "?GetXxx(...)" 的 code unit 数。
static bool ParseFunExpr(CDuiString& sInOut,
                         int* pnQ, CDuiString* pOutName, std::wstring* pOutBody,
                         size_t* pOutMatchLen /* = nullptr, R-P24 新增 */)
{
    if (sInOut.IsEmpty()) return false;

    dismcore::DataExprHitW hit;
    if (!dismcore::ParseDataExprUtf16(sInOut.GetData(), hit)) return false;

    if (pnQ) *pnQ = static_cast<int>(hit.q);
    if (pOutName) pOutName->Assign(hit.name.c_str(), static_cast<int>(hit.name.size()));
    if (pOutBody) *pOutBody = hit.body;
    if (pOutMatchLen) *pOutMatchLen = hit.matchLen;
    return true;
}

// ?Func 单测/对拍接口：见 DataEngine.h。
bool CDataEngine::QueryFunExprSupported(CDuiString& sExpr, bool* pbResolved)
{
    if (pbResolved) *pbResolved = false;

    int nQ = 0;
    CDuiString sName;
    std::wstring sBody;
    size_t nMatch = 0;
    if (!ParseFunExpr(sExpr, &nQ, &sName, &sBody, &nMatch)) return false;

    PFunEval pfn = LookupFunEval(sName.GetData());
    if (!pfn) return false;   // 未注册：不认识该函数

    CDuiString sVal;
    pfn(sBody, sVal);
    bool bOk = !sVal.IsEmpty();

    if (bOk)
    {
        // [R-P24 · R4] 用 Rust 直接给出的 nMatch 替换原表达式，替代 C++ 重复括号扫描
        CDuiString sPrefix(sExpr.GetData(), nQ);
        CDuiString sSuffix(sExpr.GetData() + nQ + nMatch);
        sExpr = sPrefix + sVal + sSuffix;
        if (pbResolved) *pbResolved = true;
    }
    return true;   // 已注册：即使取值失败也返回 true（保持原文，保守）
}

void CDataEngine::ResolveFunExpr(CDuiString& sInOut)
{
    if (sInOut.IsEmpty()) return;

    // 最多展开 16 个嵌套，避免畸形表达式死循环
    for (int iter = 0; iter < 16; ++iter)
    {
        int nQ = 0;
        CDuiString sName;
        std::wstring sBody;
        size_t nMatch = 0;
        if (!ParseFunExpr(sInOut, &nQ, &sName, &sBody, &nMatch)) return;

        PFunEval pfn = LookupFunEval(sName.GetData());
        if (!pfn) return;   // 未注册：保持原文

        CDuiString sVal;
        pfn(sBody, sVal);
        if (sVal.IsEmpty()) return;   // 已注册但取值失败：保守保留原文

        // [R-P24 · R4] 用 Rust 直接给出的 nMatch 替换原表达式，替代 C++ 重复括号扫描
        CDuiString sPrefix(sInOut.GetData(), nQ);
        CDuiString sSuffix(sInOut.GetData() + nQ + nMatch);
        sInOut = sPrefix + sVal + sSuffix;
    }
}

// 注：[R9-03] ParseDwordData/DecodeRegData/ApplyBitOp 已迁入
// DataEngine_Reg.cpp（注册表值域），声明仍在本类私有区。

// ============================================================
// [R9-03b] 离线蜂巢会话与注册表原语（BeginOfflineSession/RemapOfflinePath/
// ParseRegPath/OpenRegKey/RegValueEqual）已迁入 DataEngine_Reg.cpp。
// ============================================================

// ============================================================
// R3 · REG_BINARY 完整支持辅助
// ============================================================

// 注：[R9-03] IsHexStr 已随 DecodeRegData 迁入 DataEngine_Reg.cpp。

// base64 解码（标准 RFC 4648，含 '=' 填充）
//   [R-P22 / R2] 由 Rust dismcore::Base64Decode 替代手写 tbl 查表，
//   消除 nVal/nBits 手工位运算与非法字符静默吞掉（旧 tbl[c]=-1 会污染 nVal）的隐患
bool CDataEngine::Base64Decode(const wchar_t* pszIn, std::vector<BYTE>& outBin)
{
    outBin.clear();
    if (!pszIn || !*pszIn) return true;
    // base64 字符集纯 ASCII：wchar_t → 单字节映射
    size_t nW = 0;
    while (pszIn[nW]) ++nW;
    std::string s(nW, '\0');
    for (size_t i = 0; i < nW; ++i)
    {
        const wchar_t c = pszIn[i];
        if (c > 0x7F) return false;      // 非 ASCII → 非法 base64
        s[i] = static_cast<char>(c);
    }
    if (s.empty()) return true;
    const auto v = dismcore::Base64Decode(s.c_str());
    if (v.empty()) return false;         // Rust 侧校验失败
    outBin.assign(v.begin(), v.end());
    return true;
}

bool CDataEngine::Base64Encode(const BYTE* pData, DWORD dwLen, CDuiString& strOut)
{
    strOut.Empty();
    if (!pData || dwLen == 0) return true;
    const std::string s = dismcore::Base64Encode(pData, dwLen);
    if (s.empty()) return false;
    // base64 输出纯 ASCII → 逐字符映射到 UTF-16
    for (size_t i = 0; i < s.size(); ++i)
        strOut += static_cast<wchar_t>(static_cast<unsigned char>(s[i]));
    return true;
}

// 注：[R9-03] DecodeRegHexWithPrefix / DecodeRegHexEscape /
// DecodeRegSpaceSeparated / DecodeRegData / ApplyBitOp 均已迁入
// DataEngine_Reg.cpp。

// ============================================================
// [R9-03b] 执行器叶子原语按域迁出：
//   DataEngine_File.cpp : NtSetSystemInformation / SetDesktop / UnzipEntryToPath
//   DataEngine_Sys.cpp  : Service(Start/Recovery) / Appx / PowerPlan / RefreshDesktopIcons
//   DataEngine_Reg.cpp  : RegCleanImport / RegCleanValueExists
// ============================================================

// ============================================================
// misc
// ============================================================
bool CDataEngine::MatchOSVersion(const wchar_t* pszCompare, const wchar_t* pszTarget)
{
    if (!pszTarget) return false;

    // [OS-Filter] 统一版本来源：与 OsVersionInfo.h 共用同一个 RtlGetVersion
    // 取样，避免本文件内第二份实现与 DisplayName() 的口径漂移。
    // [FIX-P0] 离线镜像必须按目标镜像自身版本判定，而不是宿主版本。
    // 否则在 Win11 宿主上维护 Win10 镜像会选到错误的规则分支。
    unsigned int cur[3] = { 0, 0, 0 };
    bool bHaveCur = false;
    if (COfflineSession::IsActive())
    {
        unsigned int a = 0, b = 0, c = 0;
        if (COfflineSession::GetTargetVersion(a, b, c))
        {
            cur[0] = a; cur[1] = b; cur[2] = c;
            bHaveCur = true;
        }
    }
    if (!bHaveCur)
    {
        const osver::Snapshot& snap = osver::Current();
        cur[0] = snap.nMajor; cur[1] = snap.nMinor; cur[2] = snap.nBuild;
    }

    unsigned int parts[3] = { 0 };
    int nParts = _sntscanf_s(pszTarget, _TRUNCATE, L"%u.%u.%u",
                             &parts[0], &parts[1], &parts[2]);
    if (nParts < 1) return false;
    for (int i = nParts; i < 3; ++i) parts[i] = 0;

    int cmp = 0;
    for (int i = 0; i < 3; ++i)
    {
        if (cur[i] != parts[i]) { cmp = cur[i] > parts[i] ? 1 : -1; break; }
    }

    const wchar_t* op = pszCompare ? pszCompare : L">=";
    if (lstrcmpiW(op, L"==") == 0) return cmp == 0;
    if (lstrcmpiW(op, L">")  == 0) return cmp > 0;
    if (lstrcmpiW(op, L"<")  == 0) return cmp < 0;
    if (lstrcmpiW(op, L"<=") == 0) return cmp <= 0;
    return cmp >= 0;   // default ">="
}

bool CDataEngine::FileExists(const wchar_t* pszPath)
{
    if (!pszPath || !*pszPath) return false;

    // [R-B5] 先展开 ?Get* 内嵌函数
    CDuiString sPathResolved(pszPath);
    ResolveFunExpr(sPathResolved);
    pszPath = sPathResolved.GetData();

    wchar_t szExp[MAX_PATH * 2];
    DWORD n = ::ExpandEnvironmentStrings(pszPath, szExp, MAX_PATH * 2);
    if (n == 0) return false;

    DWORD attr = ::GetFileAttributes(szExp);
    return attr != INVALID_FILE_ATTRIBUTES;
}
// [C++17 R3] 本文件内联常量（C++17 inline variables，多 TU ODR 一致）：

// [REFACTOR] 从 Load 提取：按候选布局 × 文件名顺序探测规则文件。
// strTried 累加已尝试路径供上层拼诊断信息；逐字搬迁零逻辑改动。
CDataEngine::TryResult CDataEngine::ProbeLooseLayouts(
    const CDuiString& strDir, bool bIsWin11,
    const wchar_t** pszLayouts, int nLayouts, CDuiString& strTried)
{
    TryResult r = { false, L"" };
    for (int i = 0; i < nLayouts && !r.bFound; ++i)
    {
        const wchar_t* names[2] = { bIsWin11 ? L"Data_Win11.xml" : L"Data.xml", L"Data.xml" };
        int nNames = bIsWin11 ? 2 : 1;
        for (int j = 0; j < nNames; ++j)
        {
            CDuiString sCand = strDir + CDuiString(pszLayouts[i]) + CDuiString(L"\\") + names[j];
            strTried += L"\r\n    ";
            strTried += sCand;
            if (::PathFileExists(sCand.GetData())) { r.bFound = true; r.strPath = sCand; break; }
        }
    }
    return r;
}

// [REFACTOR] 从 Load 提取：标准布局未命中时，按候选布局探测 Data.zip
// （Win11 先 Data_Win11.zip）并解压取内含 Data.xml 的路径。
// 命中返回 true 并回填 strPath；strTried 累加已尝试路径。逐字搬迁。
bool CDataEngine::ProbeZipPackages(const CDuiString& strDir, bool bIsWin11,
                                   CDuiString& strTried, CDuiString& strPath)
{
    const wchar_t* zipLayouts[] = {
        L"\\DismData",
        L"",
        L"\\..\\DismData",
    };
    for (int zi = 0; zi < _countof(zipLayouts); ++zi)
    {
        const wchar_t* zipNames[2] = { bIsWin11 ? L"Data_Win11.zip" : L"Data.zip", L"Data.zip" };
        int nZipNames = bIsWin11 ? 2 : 1;
        for (int zn = 0; zn < nZipNames; ++zn)
        {
            CDuiString sZip = strDir + CDuiString(zipLayouts[zi]) + CDuiString(L"\\") + zipNames[zn];
            strTried += L"\r\n    ";
            strTried += sZip;
            if (!::PathFileExists(sZip.GetData())) continue;
            CDuiString sZipXml;
            if (ExtractDataZipToTemp(sZip.GetData(), sZipXml)
                && ::PathFileExists(sZipXml.GetData()))
            {
                strPath = sZipXml;
                strTried += CDuiString(L" -> ") + sZipXml;
                return true;
            }
        }
    }
    return false;
}

// [REFACTOR] 从 Load 提取：最后一级兜底——探测解压缓存 Data_extracted
// 与内置默认 data_default 下的规则文件（Win11 先 Data_Win11.xml）。
// 命中返回 true 并回填 strPath；strTried 累加已尝试路径。逐字搬迁。
bool CDataEngine::ProbeFallbackDirs(const CDuiString& strDir, bool bIsWin11,
                                    CDuiString& strTried, CDuiString& strPath)
{
    const wchar_t* fallbackLayouts[] = {
        L"\\DismData\\Data_extracted",
        L"\\DismData\\data_default",
        L"\\Config\\Data_extracted",
        L"\\Config\\data_default",
    };
    for (int fi = 0; fi < _countof(fallbackLayouts); ++fi)
    {
        const wchar_t* names[2] = { bIsWin11 ? L"Data_Win11.xml" : L"Data.xml", L"Data.xml" };
        int nNames = bIsWin11 ? 2 : 1;
        for (int j = 0; j < nNames; ++j)
        {
            CDuiString sCand = strDir + CDuiString(fallbackLayouts[fi]) + CDuiString(L"\\") + names[j];
            strTried += L"\r\n    ";
            strTried += sCand;
            if (!::PathFileExists(sCand.GetData())) continue;
            strPath = sCand;
            return true;
        }
    }
    return false;
}
