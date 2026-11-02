#include "stdafx.h"

// ============================================================
// CleanExtend.cpp —— [R80] 深度扫描 / 卸载残留 / 空文件夹 / 撤销
//
// 独立于已验证的 CCleanEngine（73 规则），专注四类增强能力。
// 全部删除走 SHFileOperation FOF_ALLOWUNDO（进回收站），
// 撤销 = 把回收站中对应项还原。
// ============================================================

#include "CleanExtend.h"


#include <shlwapi.h>
#include <algorithm>
#include <cstdio>
// [C++17 阶段2] 局部 RAII 守卫：查找句柄、注册表键（见 WinHandles.h 使用规范）
struct CEFindGuard
{
    HANDLE h;
    explicit CEFindGuard(HANDLE x) : h(x) {}
    ~CEFindGuard() { if (h && h != INVALID_HANDLE_VALUE) ::FindClose(h); }
    CEFindGuard(const CEFindGuard&) = delete;
    CEFindGuard& operator=(const CEFindGuard&) = delete;
    operator HANDLE() const { return h; }
};

struct CERegGuard
{
    HKEY k = nullptr;
    ~CERegGuard() { if (k) ::RegCloseKey(k); }
    HKEY* out() { return &k; }
    operator HKEY() const { return k; }
};


#pragma comment(lib, "shlwapi.lib")

// FO_UNDELETE 在部分 SDK 头文件中缺失（MSDN 定义值 0x0004）
#ifndef FO_UNDELETE
#define FO_UNDELETE  0x0004
#endif

// 递归最大深度（防失控）
static constexpr int kMaxDepth = 12;

// 需要跳过的系统目录（相对系统盘根）
static const wchar_t* kSkipDirs[] = {
    L"Windows", L"$Recycle.Bin", L"System Volume Information",
    L"Program Files", L"Program Files (x86)",
    L"ProgramData", L"Recovery", L"$Windows.~BT",
    L"$Windows.~WS", L"Documents and Settings", L"PerfLogs",
};

CCleanExtendEngine::CCleanExtendEngine()
{
    for (size_t i = 0; i < _countof(kSkipDirs); ++i)
        m_vSkipDirs.push_back(kSkipDirs[i]);
}

CCleanExtendEngine::~CCleanExtendEngine()
{
}

// ------------------------------------------------------------------
// 1. 大文件扫描
// ------------------------------------------------------------------
static bool IsSkipDir(const std::vector<CDuiString>& vSkip, const CDuiString& sName)
{
    for (size_t i = 0; i < vSkip.size(); ++i)
        if (sName.CompareNoCase(vSkip[i]) == 0) return true;
    return false;
}

int CCleanExtendEngine::EnumBigFiles(const CDuiString& sDir,
                                     ULONGLONG nMinBytes,
                                     bool bSkipSystemDirs,
                                     const std::vector<CDuiString>& vSkipDirs,
                                     int nDepth,
                                     std::vector<CleanExtendBigFile>& out)
{
    if (nDepth > kMaxDepth) return 0;
    if (sDir.IsEmpty()) return 0;   // [B2] 防空串下标
    // [G11] 取消检查点：每进一个目录查一次。放最前面，命中即不进
    // FindFirstFile，避免取消后还要付一整次目录枚举的代价。
    if (m_bCancel.load(std::memory_order_relaxed)) return 0;

    CDuiString sFind = sDir;
    if (sFind[sFind.GetLength() - 1] != L'\\')
        sFind += L'\\';
    sFind += L"*";

    WIN32_FIND_DATA fd;
    CEFindGuard hFind(::FindFirstFile(sFind.GetData(), &fd));
    if (hFind == INVALID_HANDLE_VALUE) return 0;

    int nCount = 0;
    do
    {
        // 跳过 . / ..
        if (fd.cFileName[0] == L'.' && fd.cFileName[1] == L'\0') continue;
        if (fd.cFileName[0] == L'.' && fd.cFileName[1] == L'.'
            && fd.cFileName[2] == L'\0') continue;

        CDuiString sFull = sDir;
        if (sFull.GetLength() > 0 && sFull[sFull.GetLength() - 1] != L'\\')
            sFull += L'\\';
        sFull += fd.cFileName;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            if (bSkipSystemDirs && IsSkipDir(vSkipDirs, CDuiString(fd.cFileName)))
                continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;  // 不穿越 junction
            nCount += EnumBigFiles(sFull, nMinBytes, bSkipSystemDirs, vSkipDirs,
                                   nDepth + 1, out);
        }
        else
        {
            ULONGLONG ullSize =
                (static_cast<ULONGLONG>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            if (ullSize >= nMinBytes)
            {
                CleanExtendBigFile bf;
                bf.sPath = sFull;
                bf.ullSize = ullSize;
                bf.ftLastWrite = fd.ftLastWriteTime;
                out.push_back(bf);
                ++nCount;
            }
        }
    } while (::FindNextFile(hFind, &fd));

    return nCount;
}

int CCleanExtendEngine::DeepScanBigFiles(const CDuiString& sRoot,
                                         ULONGLONG nMinBytes,
                                         bool bSkipSystemDirs,
                                         std::vector<CleanExtendBigFile>& out)
{
    CDuiString sRootDir = sRoot;
    if (sRootDir.IsEmpty())
    {
        wchar_t szDrive[4] = { 0 };
        ::GetWindowsDirectory(szDrive, _countof(szDrive));
        sRootDir = CDuiString(szDrive).Left(3);   // "C:\"
    }

    if (sRootDir[sRootDir.GetLength() - 1] != L'\\')
        sRootDir += L'\\';

    int n = EnumBigFiles(sRootDir, nMinBytes, bSkipSystemDirs, m_vSkipDirs, 0, out);

    // 按大小降序排列
    std::sort(out.begin(), out.end(),
              [](const CleanExtendBigFile& a, const CleanExtendBigFile& b)
              { return a.ullSize > b.ullSize; });

    return n;
}

// ------------------------------------------------------------------
// 2. 卸载残留扫描
// ------------------------------------------------------------------
bool CCleanExtendEngine::ReadRegString(HKEY hRoot, const wchar_t* sSubKey,
                                       const wchar_t* sValueName, CDuiString& out)
{
    CERegGuard hKey;
    if (::RegOpenKeyExW(hRoot, sSubKey, 0, KEY_READ | KEY_WOW64_64KEY, hKey.out())
        != ERROR_SUCCESS)
        return false;

    DWORD dwType = 0, dwSize = 0;
    if (::RegQueryValueEx(hKey, sValueName, nullptr, &dwType, nullptr, &dwSize)
        != ERROR_SUCCESS
        || dwType != REG_SZ)
    {
        return false;
    }

    std::vector<wchar_t> buf(dwSize / sizeof(wchar_t) + 2, 0);
    dwSize = static_cast<DWORD>(buf.size() * sizeof(wchar_t));
    if (::RegQueryValueEx(hKey, sValueName, nullptr, &dwType,
                          reinterpret_cast<BYTE*>(buf.data()), &dwSize) != ERROR_SUCCESS)
    {
        return false;
    }
    out = buf.data();
    return true;
}

// 处理单个 Uninstall 子键
static bool IsOrphanUninstallKey(const std::vector<CleanExtendOrphan>& v, const wchar_t* sKey)
{
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i].sUninstallKey == sKey) return true;
    return false;
}

void CCleanExtendEngine::ScanUninstallKey(HKEY hKey, CDuiString& sPath,
                                          std::vector<CleanExtendOrphan>& out)
{
    DWORD dwIndex = 0;
    for (;;)
    {
        DWORD dwNameLen = 0, dwClassLen = 0;
        FILETIME ft = { 0 };
        // RegEnumKeyEx：8 参数
        if (::RegEnumKeyEx(hKey, dwIndex, nullptr, &dwNameLen, nullptr, nullptr,
                           &dwClassLen, &ft) != ERROR_SUCCESS)
            break;

        std::vector<wchar_t> subName(dwNameLen + 2, 0);
        dwClassLen = 0;
        if (::RegEnumKeyEx(hKey, dwIndex, subName.data(), &dwNameLen,
                           nullptr, nullptr, &dwClassLen, &ft) != ERROR_SUCCESS)
        {
            ++dwIndex;
            continue;
        }

        CDuiString sFull = sPath;
        sFull += L'\\';
        sFull += subName.data();

        CERegGuard hSub;
        if (::RegOpenKeyExW(hKey, subName.data(), 0,
                           KEY_READ | KEY_WOW64_64KEY, hSub.out()) == ERROR_SUCCESS)
        {
            CDuiString sName, sPath2;
            DWORD dwType = 0, dwSize = 0;

            // DisplayName
            if (::RegQueryValueEx(hSub, L"DisplayName", nullptr, &dwType,
                                  nullptr, &dwSize) == ERROR_SUCCESS)
            {
                std::vector<wchar_t> buf(dwSize / sizeof(wchar_t) + 2, 0);
                dwSize = static_cast<DWORD>(buf.size() * sizeof(wchar_t));
                if (::RegQueryValueEx(hSub, L"DisplayName", nullptr, &dwType,
                                      reinterpret_cast<BYTE*>(buf.data()), &dwSize) == ERROR_SUCCESS)
                    sName = buf.data();
            }

            // InstallLocation
            if (::RegQueryValueEx(hSub, L"InstallLocation", nullptr, &dwType,
                                  nullptr, &dwSize) == ERROR_SUCCESS)
            {
                std::vector<wchar_t> buf(dwSize / sizeof(wchar_t) + 2, 0);
                dwSize = static_cast<DWORD>(buf.size() * sizeof(wchar_t));
                if (::RegQueryValueEx(hSub, L"InstallLocation", nullptr, &dwType,
                                      reinterpret_cast<BYTE*>(buf.data()), &dwSize) == ERROR_SUCCESS)
                    sPath2 = buf.data();
            }

            // InstallLocation 路径已不存在 → 视为残留
            if (!sPath2.IsEmpty())
            {
                DWORD dwAttr = ::GetFileAttributes(sPath2.GetData());
                if (dwAttr == INVALID_FILE_ATTRIBUTES
                    && !(::GetFileAttributes(sPath2.GetData()) != INVALID_FILE_ATTRIBUTES))
                {
                    CleanExtendOrphan orp;
                    orp.sUninstallKey = sFull;
                    orp.sDisplayName = sName.IsEmpty() ? sFull : sName;
                    orp.sInstallPath  = sPath2;
                    out.push_back(orp);
                }
            }

            // 递归子键
            ScanUninstallKey(hSub, sFull, out);
        }

        ++dwIndex;
    }
}

void CCleanExtendEngine::ScanUninstallRoot(HKEY hRoot, const wchar_t* sSubPath,
                                           std::vector<CleanExtendOrphan>& out)
{
    CERegGuard hBase;   // [C++17 R-P8] 改为 RAII å®å«ï¼åä¸ºè£¸ HKEY + .out()ï¼
    if (::RegOpenKeyExW(hRoot, sSubPath, 0, KEY_READ, hBase.out()) != ERROR_SUCCESS)
        return;

    CDuiString sPath = sSubPath;
    ScanUninstallKey(hBase, sPath, out);
}

int CCleanExtendEngine::OrphanScanUninstall(std::vector<CleanExtendOrphan>& out)
{
    // [B2] 契约：每次扫描产出全新结果，避免重复调用时结果翻倍
    out.clear();

    // 扫描 3 个 Uninstall 区（64位视图 + WOW6432Node + HKCU）
    ScanUninstallRoot(HKEY_LOCAL_MACHINE,
                      L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", out);
    ScanUninstallRoot(HKEY_LOCAL_MACHINE,
                      L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall", out);
    ScanUninstallRoot(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall", out);

    return static_cast<int>(out.size());
}

// ------------------------------------------------------------------
// 3. 空文件夹扫描
// ------------------------------------------------------------------
bool CCleanExtendEngine::IsEmptyFolder(const CDuiString& sPath, int nDepth)
{
    // [B2] 空串/深度守卫：-1 下标与无限递归（深层嵌套/环）双防
    if (sPath.IsEmpty()) return false;
    if (nDepth > kMaxDepth) return false;

    CDuiString sFind = sPath;
    if (sFind[sFind.GetLength() - 1] != L'\\')
        sFind += L'\\';
    sFind += L"*";

    WIN32_FIND_DATA fd;
    CEFindGuard hFind(::FindFirstFile(sFind.GetData(), &fd));
    if (hFind == INVALID_HANDLE_VALUE) return false;

    bool bHasContent = false;
    do
    {
        if (fd.cFileName[0] == L'.' && fd.cFileName[1] == L'\0') continue;
        if (fd.cFileName[0] == L'.' && fd.cFileName[1] == L'.'
            && fd.cFileName[2] == L'\0') continue;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            // [R-EXTASYNC] 不穿越 reparse point（junction/symlink），与
            // CollectEmptyFolders/EnumBigFiles 保持一致：否则指向卷根的
            // junction 会把外部目录当作"空/非空"递归枚举，既慢又可能误判。
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            {
                bHasContent = true;   // 视为"有内容"，不报告为可删空目录
                continue;
            }
            CDuiString sSub = sPath;
            if (sSub[sSub.GetLength() - 1] != L'\\')
                sSub += L'\\';
            sSub += fd.cFileName;

            if (!IsEmptyFolder(sSub, nDepth + 1))
            {
                bHasContent = true;
                break;
            }
        }
        else
        {
            // 存在文件（含隐藏）→ 非空
            bHasContent = true;
            break;
        }
    } while (::FindNextFile(hFind, &fd));

    return !bHasContent;
}

int CCleanExtendEngine::CollectEmptyFolders(const CDuiString& sDir,
                                            std::vector<CleanExtendEmptyDir>& out,
                                            const std::vector<CDuiString>& vSkip,
                                            int nDepth)
{
    // [G11] 取消检查点（同 EnumBigFiles）
    if (m_bCancel.load(std::memory_order_relaxed)) return 0;

    // [B2] 空串/深度守卫
    if (sDir.IsEmpty()) return 0;
    if (nDepth > kMaxDepth) return 0;

    CDuiString sFind = sDir;
    if (sFind[sFind.GetLength() - 1] != L'\\')
        sFind += L'\\';
    sFind += L"*";

    WIN32_FIND_DATA fd;
    CEFindGuard hFind(::FindFirstFile(sFind.GetData(), &fd));
    if (hFind == INVALID_HANDLE_VALUE) return 0;

    int nCount = 0;
    do
    {
        if (fd.cFileName[0] == L'.' && fd.cFileName[1] == L'\0') continue;
        if (fd.cFileName[0] == L'.' && fd.cFileName[1] == L'.'
            && fd.cFileName[2] == L'\0') continue;

        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM) continue;
        // [B2] 保护目录（Windows/ProgramData/$Recycle.Bin 等）任意层级
        // 都不扫描、不报告为空目录，避免误删系统依赖的空壳目录
        if (!vSkip.empty() && IsSkipDir(vSkip, CDuiString(fd.cFileName)))
            continue;

        CDuiString sSub = sDir;
        if (sSub[sSub.GetLength() - 1] != L'\\')
            sSub += L'\\';
        sSub += fd.cFileName;

        if (IsEmptyFolder(sSub, nDepth + 1))
        {
            CleanExtendEmptyDir ed;
            ed.sPath = sSub;
            out.push_back(ed);
            ++nCount;
        }
        else
        {
            nCount += CollectEmptyFolders(sSub, out, vSkip, nDepth + 1);
        }
    } while (::FindNextFile(hFind, &fd));

    return nCount;
}

int CCleanExtendEngine::EmptyFolderScan(const CDuiString& sRoot,
                                        std::vector<CleanExtendEmptyDir>& out)
{
    CDuiString sRootDir = sRoot;
    if (sRootDir.IsEmpty())
    {
        wchar_t szDrive[4] = { 0 };
        ::GetWindowsDirectory(szDrive, _countof(szDrive));
        sRootDir = CDuiString(szDrive).Left(3);
    }
    if (sRootDir[sRootDir.GetLength() - 1] != L'\\')
        sRootDir += L'\\';

    int n = CollectEmptyFolders(sRootDir, out, m_vSkipDirs, 0);

    // 深度降序：先删最深（避免空文件夹被删后父目录也变空）
    std::sort(out.begin(), out.end(),
              [](const CleanExtendEmptyDir& a, const CleanExtendEmptyDir& b)
              {
                  return a.sPath.GetLength() > b.sPath.GetLength();
              });

    return n;
}

// ------------------------------------------------------------------
// 4. 清理执行（进回收站）
// ------------------------------------------------------------------
int CCleanExtendEngine::DeleteToRecycleBin(const std::vector<CDuiString>& vPaths,
                                           int& nFailedPaths)
{
    nFailedPaths = 0;
    if (vPaths.empty()) return 0;

    // 排序：父先子后（字典序），保证 SHFileOperation 一次到位
    std::vector<CDuiString> vAll = vPaths;
    std::sort(vAll.begin(), vAll.end(),
              [](const CDuiString& a, const CDuiString& b)
              {
                  return CDuiString(a).Compare(b) < 0;
              });

    // 去重 + 过滤已不存在的路径
    std::vector<CDuiString> vFinal;
    for (size_t i = 0; i < vAll.size(); ++i)
    {
        if (i > 0 && vAll[i] == vAll[i - 1]) continue;
        if (::GetFileAttributes(vAll[i].GetData()) == INVALID_FILE_ATTRIBUTES) continue;
        vFinal.push_back(vAll[i]);
    }

    constexpr int BATCH = 8000;
    size_t pos = 0;
    while (pos < vFinal.size())
    {
        std::vector<wchar_t> buf;
        buf.reserve(BATCH);
        size_t start = pos;
        while (pos < vFinal.size())
        {
            size_t need = wcslen(vFinal[pos].GetData()) + 1;
            if (buf.size() + need + 1 > static_cast<size_t>(BATCH) && pos > start) break;
            const wchar_t* p = vFinal[pos].GetData();
            while (*p) buf.push_back(*p++);
            buf.push_back(0);
            ++pos;
        }
        buf.push_back(0);

        SHFILEOPSTRUCT fos = { 0 };
        fos.wFunc = FO_DELETE;
        fos.pFrom = buf.data();
        // [R80] 走回收站：FOF_ALLOWUNDO，可撤销
        fos.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION
                   | FOF_NOERRORUI | FOF_SILENT;
        // [R128-B] Shell 删除失败（被占用/权限）需留痕，否则用户以为已清理
        if (::SHFileOperation(&fos) != 0 || fos.fAnyOperationsAborted)
            ::OutputDebugStringW(L"[R128][clean] SHFileOperation 未完全成功\r\n");
    }

    // 后置检查
    for (size_t i = 0; i < vFinal.size(); ++i)
        if (::GetFileAttributes(vFinal[i].GetData()) != INVALID_FILE_ATTRIBUTES)
            ++nFailedPaths;

    return static_cast<int>(vFinal.size() - nFailedPaths);
}

// ------------------------------------------------------------------
// 5. 撤销栈
// ------------------------------------------------------------------
void CCleanExtendEngine::PushUndo(const std::vector<CleanExtendUndoEntry>& vEntries,
                                  int nMax)
{
    m_vUndoStack.push_back(vEntries);
    while (static_cast<int>(m_vUndoStack.size()) > nMax)
        m_vUndoStack.erase(m_vUndoStack.begin());
}

int CCleanExtendEngine::UndoLast(std::vector<CDuiString>& outErrors)
{
    if (m_vUndoStack.empty()) return 0;

    const std::vector<CleanExtendUndoEntry>& top = m_vUndoStack.back();

    int nOk = 0;
    for (size_t i = 0; i < top.size(); ++i)
    {
        // 如果路径已存在，跳过（用户可能已手动恢复）
        if (::GetFileAttributes(top[i].sPath.GetData()) != INVALID_FILE_ATTRIBUTES)
            continue;

        // 用 SHFileOperation 从回收站恢复（FO_UNDELETE）
        SHFILEOPSTRUCT fos = { 0 };
        std::vector<wchar_t> buf;
        const wchar_t* p = top[i].sPath.GetData();
        while (*p) buf.push_back(*p++);
        buf.push_back(0);
        buf.push_back(0);
        fos.wFunc = FO_UNDELETE;
        fos.pFrom = buf.data();
        fos.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
        if (::SHFileOperation(&fos) == 0)
            ++nOk;
        else
            outErrors.push_back(top[i].sPath);
    }

    m_vUndoStack.pop_back();
    return nOk;
}
// [C++17 R3] 本文件内联常量（C++17 inline variables，多 TU ODR 一致）：
