// ============================================================
// Page_ToolsDirs.cpp —— 工具箱页「常用目录菜单」（从 Page_Tools.cpp 拆出）。
//
// [R170] Page_Tools.cpp 拆前 1,373 行、规模分 8.66；本文件承接一整组
// 自成体系、原本整体封闭在匿名命名空间里的实体（114 行）：
//   · TCommonDir          目录项（路径 + 显示名）
//   · FillStaticDirs      4 项静态系统目录（Windows / WinSxS / Program Files ×2）
//   · FillDynamicDirs     3 项用户目录（配置目录 / 桌面 / 文档）
//   · FillToolDirs        3 项 DismTool 工作目录（Logs / HostsBackup / BackupBCD）
//   · FillCommonDirs      按容量裁剪拼装以上三段
//   · ShowCommonDirMenu   弹出常用目录菜单并跳转 Explorer
//
// 闭包说明：整组不依赖 CPageTools 任何成员（原为文件级 static，无成员访问），
// 外部唯一入口是 ShowCommonDirMenu，声明已提升到 Page_ToolsCommon.h 的
// namespace tool_dirs 内，主文件经该头使用。
//
// [C++17] 本文件用 inline constexpr 承载目录项数量上限，替代原数组字面量 4/3。
// ============================================================

#include "stdafx.h"
#include "LogService.h"   // 统一日志目录

#include <shlobj.h>              // SHGetFolderPathW / CSIDL_*

#include "Page_ToolsCommon.h"    // 声明：tool_dirs::ShowCommonDirMenu
#include "StringManager.h"       // StringMgr
#include "ExternalToolService.h" // ExploreFolder

using i18n::CStringManager;

// [R170] 三段目录项的固定条数。原为函数内数组维度 4/3/3 与循环上界三处重复字面量。
inline constexpr int kStaticDirCount = 4;
inline constexpr int kDynamicDirCount = 3;
inline constexpr int kToolDirCount = 3;

// P2-1 文件浏览器增强 + P2-4 日志/备份目录一键访问

// 用户在 Win32DialogService::DlgBrowseFolder 取消时，弹常用目录菜单，避免"点错了就无事发生"

// ====================================================================

namespace tool_dirs {
struct TCommonDir
{
    CDuiString sDir;
    CDuiString sLabel;
};
// [R170] 静态系统目录（4 项，恒定存在，不受环境变量影响）。
// 从 FillCommonDirs 抽出：原函数内三段数据块各带自己的判空/取值逻辑，
// CC 记账 13 个判定点集中在三段里。拆出后每段独立可测，主函数只负责拼接。
static int FillStaticDirs(TCommonDir* out, int nCap)
{
    TCommonDir statics[kStaticDirCount];
    wchar_t szWin[MAX_PATH] = { 0 };
    // [R95] 取不到就回落到字面量 C:\Windows：原来不检查返回值，
    //   失败时 szWin 全零 → statics[1].sDir 变成 "\WinSxS"（无盘符），
    //   列表里会出现一个点不进去的假目录。
    if (::GetWindowsDirectoryW(szWin, MAX_PATH) == 0)
        ::wcsncpy_s(szWin, MAX_PATH, L"C:\\Windows", _TRUNCATE);
    statics[0].sDir = L"C:\\Windows";
    statics[0].sLabel = StringMgr.Get("misc.hf003");
    statics[1].sDir = CDuiString(szWin) + L"\\WinSxS";
    statics[1].sLabel = StringMgr.Get("misc.h138f");
    statics[2].sDir = L"C:\\Program Files";
    statics[2].sLabel = L"Program Files";
    statics[3].sDir = L"C:\\Program Files (x86)";
    statics[3].sLabel = L"Program Files (x86)";
    int n = 0;
    for (int i = 0; i < kStaticDirCount && n < nCap; ++i) out[n++] = statics[i];
    return n;
}

// [R170] 动态用户目录（3 项）：任一环境查询失败则该项留空，由调用方按 IsEmpty 过滤。
// 「留空再过滤」的语义逐字保留：失败项不是不追加，而是先置空、后在主函数跳过。
static int FillDynamicDirs(TCommonDir* out, int nCap)
{
    TCommonDir dynamics[kDynamicDirCount];
    wchar_t szBuf[MAX_PATH] = { 0 };
    if (::SHGetFolderPathW(nullptr, CSIDL_PROFILE, nullptr, 0, szBuf) == S_OK)
    {
        dynamics[0].sDir = szBuf;
        dynamics[0].sLabel = StringMgr.Get("misc.h75de");
    }
    if (::GetEnvironmentVariable(L"USERPROFILE", szBuf, MAX_PATH))
    {
        dynamics[1].sDir = CDuiString(szBuf) + L"\\Desktop";
        dynamics[1].sLabel = StringMgr.Get("misc.h2828");
    }
    if (::SHGetFolderPathW(nullptr, CSIDL_MYDOCUMENTS, nullptr, 0, szBuf) == S_OK)
    {
        dynamics[2].sDir = szBuf;
        dynamics[2].sLabel = StringMgr.Get("misc.h999b");
    }
    int n = 0;
    for (int i = 0; i < kDynamicDirCount && n < nCap; ++i)
        if (!dynamics[i].sDir.IsEmpty()) out[n++] = dynamics[i];
    return n;
}

// [R170] DismTool 工作目录（3 项，P2-4）：LOCALAPPDATA 取不到则三项全空。
static int FillToolDirs(TCommonDir* out, int nCap)
{
    TCommonDir toolDirs[kToolDirCount];
    wchar_t szLa[MAX_PATH] = { 0 };
    if (::GetEnvironmentVariable(L"LOCALAPPDATA", szLa, MAX_PATH))
    {
        // [M-9 联动] "打开日志目录"必须指向统一目录，否则用户点进去看到的是
        //   已停用的旧目录（新日志全在 EXE\Logs），等于把人引到空文件夹。
        toolDirs[0].sDir = CDuiString(LogService::UnifiedLogDir().c_str());
        toolDirs[0].sLabel = StringMgr.Get("misc.hdc01");
        toolDirs[1].sDir = CDuiString(szLa) + L"\\DismTool\\HostsBackup";
        toolDirs[1].sLabel = StringMgr.Get("misc.h4dfd");
        toolDirs[2].sDir = CDuiString(szLa) + L"\\DismTool\\BackupBCD";
        toolDirs[2].sLabel = StringMgr.Get("misc.h69ce");
    }
    int n = 0;
    for (int i = 0; i < kDynamicDirCount && n < nCap; ++i)
        if (!toolDirs[i].sDir.IsEmpty()) out[n++] = toolDirs[i];
    return n;
}

static void FillCommonDirs(TCommonDir arr[], int nCap, int& nOut)
{
    nOut = 0;
    if (nCap <= 0) return;
    // 顺序固定：静态系统目录 → 动态用户目录 → DismTool 工作目录。
    // 每段都按剩余容量裁剪，保证不越界（原实现用 nOut < nCap 逐项判断）。
    nOut += FillStaticDirs(arr + nOut, nCap - nOut);
    nOut += FillDynamicDirs(arr + nOut, nCap - nOut);
    nOut += FillToolDirs(arr + nOut, nCap - nOut);
}
void ShowCommonDirMenu(HWND hWnd)
{
    TCommonDir arr[10];
    int n = 0;
    FillCommonDirs(arr, 10, n);
    if (n <= 0) return;
    HMENU h = ::CreatePopupMenu();
    // 用 SYSTEM_FONT 设置菜单字体：菜单本身不接受 WM_SETFONT，通过 SetMenuFont 消息处理
    // 简化处理：直接跳过，菜单会跟随系统默认字体
    (void)h;
    for (int i = 0; i < n; ++i)
        ::InsertMenuW(h, 0, MF_BYPOSITION | MF_STRING, (UINT_PTR)(1000 + i),
                      arr[i].sLabel.GetData());
    POINT pt;
    ::GetCursorPos(&pt);
    DWORD id = ::TrackPopupMenu(h,
        TPM_LEFTALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_TOPALIGN,
        pt.x, pt.y, 0, hWnd, nullptr);
    ::PostMessageW(hWnd, WM_NULL, 0, 0);
    ::DestroyMenu(h);
    if (id >= 1000 && static_cast<int>(id - 1000) < n)
        ExternalToolService::ExploreFolder(hWnd, arr[id - 1000].sDir);
}
} // namespace tool_dirs
