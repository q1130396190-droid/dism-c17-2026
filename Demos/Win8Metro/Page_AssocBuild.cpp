// ============================================================
// Page_AssocBuild.cpp —— 文件关联页「离线枚举 → 去重 → 预计算分组」阶段。
//
// [R100] 从 Page_Assoc.cpp 拆出。原文件同时承担六件事：查询、批量应用、
//   注册表枚举、分组预计算、行渲染、事件分发。本 TU 只保留「把注册表里
//   的扩展名读成内存列表」这一段，与 UI 渲染 / 消息分发无交叉引用。
//
// 拆分前后函数体逐字未改，仅搬迁位置。
// ============================================================

#include "stdafx.h"
#include "ThemeTokens.h"   // [R203] 颜色令牌（深色/浅色跟随）

#include "Page_Assoc.h"

#include "CustomControls.h"      // CClickRowUI（assocrow_%d 行）
#include "StringManager.h"       // StringMgr 本地化宏
#include "FrameWnd_Util.h"       // EnsureIconCacheDirectory/EnsureIconPngReady
#include "Registry.h"            // RegGetString

#include <algorithm>             // std::sort/std::unique
#include <memory>                // make_unique/unique_ptr

// [C++17 阶段2] 局部 RAII 守卫：注册表键。
// [R100] 原定义在 Page_Assoc.cpp 文件作用域，两个 TU 都要用，
//        故在两处各留一份同定义（struct 无链接性问题，非匿名命名空间）。
struct ASRegGuard
{
    HKEY k = nullptr;
    ~ASRegGuard() { if (k) ::RegCloseKey(k); }
    HKEY* out() { return &k; }
    operator HKEY() const { return k; }
};

using namespace DuiLib;
using i18n::CStringManager;

void CPageAssoc::AssocBuildOfflineTip()

{
// 离线卡片：导入关联 XML 前仅显示提示
CVerticalLayoutUI* pImg = static_cast<CVerticalLayoutUI*>(m_ctx.HostFindControl(L"assoc_img_list"));
if (pImg)
{
    pImg->RemoveAll();
    auto pTip = std::make_unique<CTextUI>(); // [C++17 R3]
    pTip->SetText(StringMgr.Get("toolbox.s39438ec7"));
    pTip->SetTextColor(tokens::Token(tokens::EToken::InkMuted));
    pTip->SetFixedHeight(24);
    pTip->SetAttribute(L"inset", L"8,4,8,0");
    pTip->SetAttribute(L"valign", L"vcenter");
    pImg->Add(pTip.release()); // [C++17 R3] ownership transferred to DuiLib
}
}


void CPageAssoc::AssocBuildEnumHkcu()

{
ASRegGuard hRoot;
if (::RegOpenKeyEx(HKEY_CURRENT_USER,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts",
        0, KEY_READ, hRoot.out()) == ERROR_SUCCESS)
{
    DWORD dw = 0;
    for (;;)
    {
        constexpr int kRegKeyNameWchars = 256;   // 256 缓冲尺寸（语义命名，数值不变）
        wchar_t szName[kRegKeyNameWchars] = { 0 };
        DWORD nLen = kRegKeyNameWchars;
        LONG rc = ::RegEnumKeyEx(hRoot, dw++, szName, &nLen, nullptr, nullptr, nullptr, nullptr);
        if (rc == ERROR_NO_MORE_ITEMS) break;
        if (rc != ERROR_SUCCESS) continue;
        if (szName[0] != L'.') continue;   // 仅保留含"."的扩展名
        m_vAssocExt.push_back(szName);
    }

}
}


void CPageAssoc::AssocBuildEnumHklm()

{
ASRegGuard hRoot;
if (::RegOpenKeyEx(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes",
        0, KEY_READ, hRoot.out()) == ERROR_SUCCESS)
{
    DWORD dw = 0;
    for (;;)
    {
        constexpr int kRegKeyNameWchars = 256;   // 256 缓冲尺寸（语义命名，数值不变）
        wchar_t szName[kRegKeyNameWchars] = { 0 };
        DWORD nLen = kRegKeyNameWchars;
        LONG rc = ::RegEnumKeyEx(hRoot, dw++, szName, &nLen, nullptr, nullptr, nullptr, nullptr);
        if (rc == ERROR_NO_MORE_ITEMS) break;
        if (rc != ERROR_SUCCESS) continue;
        if (szName[0] != L'.') continue;
        m_vAssocExt.push_back(szName);
    }

}
}


void CPageAssoc::AssocBuildEnumHkcr()

{
ASRegGuard hRoot;
if (::RegOpenKeyEx(HKEY_CLASSES_ROOT, L"",
        0, KEY_READ, hRoot.out()) == ERROR_SUCCESS)
{
    DWORD dw = 0;
    int nHKCR = 0;
    // [C++17 R3] 块级 constexpr：作用域限于函数内（命名空间级 inline constexpr 见文件尾）
constexpr int HKCR_LIMIT = 1000;  // R33: 上限提升到 1000，覆盖常见扩展名
    for (; nHKCR < HKCR_LIMIT; ++dw)
    {
        constexpr int kRegKeyNameWchars = 256;   // 256 缓冲尺寸（语义命名，数值不变）
        wchar_t szName[kRegKeyNameWchars] = { 0 };
        DWORD nLen = kRegKeyNameWchars;
        LONG rc = ::RegEnumKeyEx(hRoot, dw, szName, &nLen, nullptr, nullptr, nullptr, nullptr);
        if (rc == ERROR_NO_MORE_ITEMS) break;
        if (rc != ERROR_SUCCESS) continue;
        if (szName[0] != L'.') continue;
        m_vAssocExt.push_back(szName);
        ++nHKCR;
    }

}
}


void CPageAssoc::AssocBuildDedupExts()

{
// 去重排序（扩展名按不区分大小写比较）
std::sort(m_vAssocExt.begin(), m_vAssocExt.end(),
    [](const CDuiString& a, const CDuiString& b)
    { return ::lstrcmpW(a.GetData(), b.GetData()) < 0; });
m_vAssocExt.erase(
    std::unique(m_vAssocExt.begin(), m_vAssocExt.end(),
        [](const CDuiString& a, const CDuiString& b)
        { return ::lstrcmpW(a.GetData(), b.GetData()) == 0; }),
    m_vAssocExt.end());
}

void CPageAssoc::AssocBuildPrecomputeGroup(
        std::vector<DuiLib::CDuiString>& vGroupKey,
        std::vector<DuiLib::CDuiString>& vProgId,
        std::vector<DuiLib::CDuiString>& vAssocExe)
{


for (size_t k = 0; k < m_vAssocExt.size(); ++k)
{
    CDuiString sExt = m_vAssocExt[k];
    CDuiString sUCPath;
    sUCPath.Format(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\%s\\UserChoice",
                   sExt.GetData());
    CDuiString sProgId = RegGetString(HKEY_CURRENT_USER, sUCPath.GetData(),
                                      L"ProgId", KEY_READ);
    if (sProgId.IsEmpty())
        sProgId = RegGetString(HKEY_CLASSES_ROOT, sExt.GetData(), L"",
                               KEY_READ | KEY_WOW64_64KEY);
    vProgId[k] = sProgId;


    // [R46] 从 HKCR\{ProgId}\shell\open\command 解析默认打开的可执行文件名
    // 让用户在列表上直接看到关联的目标程序，比"只看到 ProgId"更可读
    CDuiString sCmd = L"";
    CDuiString sExe = L"";
    if (!sProgId.IsEmpty())
    {
        CDuiString sShellPath;
        sShellPath.Format(L"%s\\shell\\open\\command", sProgId.GetData());
        sCmd = RegGetString(HKEY_CLASSES_ROOT, sShellPath.GetData(),
                            L"", KEY_READ | KEY_WOW64_64KEY);
    }
    if (!sCmd.IsEmpty())
    {
        int nQ1 = sCmd.Find(L'"');
        if (nQ1 >= 0)
        {
            int nQ2 = sCmd.Find(L'"', nQ1 + 1);
            if (nQ2 > nQ1 + 1) sExe = sCmd.Mid(nQ1 + 1, nQ2 - nQ1 - 1);
        }
        else
        {
            int nEnd = sCmd.Find(L' ');
            if (nEnd < 0) nEnd = sCmd.GetLength();
            sExe = sCmd.Left(nEnd);
        }
        // 提取基名（去掉目录前缀）
        const wchar_t* pSlashExe = wcsrchr(sExe.GetData(), L'\\');
        if (pSlashExe && pSlashExe != sExe.GetData()) sExe = pSlashExe + 1;
    }
    vAssocExe[k] = sExe;


    // 应用分组 key：取 ProgId 首段（"\\" 前）作为分组标识
    CDuiString sKey = sProgId;
    int pSlash = sKey.Find(L'\\');
    if (pSlash > 0) sKey = sKey.Mid(0, pSlash);
    if (sKey.IsEmpty()) sKey = StringMgr.Get("misc.h2926");
    vGroupKey[k] = sKey;
}


// 按分组 key 排序 m_vAssocExt 及并行数组（保持三数组同步）
struct SortHelper
{
    std::vector<CDuiString> keys;
} sortCtx;
sortCtx.keys = vGroupKey;
std::vector<int> vIdx(m_vAssocExt.size());
for (size_t k = 0; k < m_vAssocExt.size(); ++k) vIdx[k] = static_cast<int>(k);
std::sort(vIdx.begin(), vIdx.end(), [&](int a, int b)
{
    if (lstrcmpiW(vGroupKey[a].GetData(), vGroupKey[b].GetData()) != 0)
        return lstrcmpiW(vGroupKey[a].GetData(), vGroupKey[b].GetData()) < 0;
    return lstrcmpiW(m_vAssocExt[a].GetData(), m_vAssocExt[b].GetData()) < 0;
});
std::vector<CDuiString> vNewExt, vNewKey, vNewProg, vNewExe;
vNewExt.reserve(m_vAssocExt.size());
vNewKey.reserve(m_vAssocExt.size());
vNewProg.reserve(m_vAssocExt.size());
vNewExe.reserve(m_vAssocExt.size());
for (size_t k = 0; k < vIdx.size(); ++k)
{
    int n = vIdx[k];
    vNewExt.push_back(m_vAssocExt[n]);
    vNewKey.push_back(vGroupKey[n]);
    vNewProg.push_back(vProgId[n]);
    vNewExe.push_back(vAssocExe[n]);
}
m_vAssocExt.swap(vNewExt);
vGroupKey.swap(vNewKey);
vProgId.swap(vNewProg);
vAssocExe.swap(vNewExe);
}
