// ============================================================
// [R94] ToolWin11ImagePreprocess 的拆分辅助
//
// 拆分依据：原函数 168 行、9 个顺序段。其中四段是"取输入 → 返回结果"的
// 纯形态，与后续流程只通过局部变量交换数据，可无状态抽出：
//   1 选镜像文件 + 存在性校验
//   2+3 枚举索引 + 让用户选（单索引不打扰）
//   4 计算临时挂载点（必须与镜像同卷）+ 确保目录存在
//   7 挂载后读离线映像真实 build
//
// 刻意**不拆**第 5/6/8/9 段（确认/挂载/Rust/提交或丢弃）：
// 它们串行依赖 mountDir 与 dwIndex 的时序，拆开需要引入中间状态对象，
// 属结构性改动，收益是可读性而非正确性，超出本轮范围。
//
// 行为零改动：每个辅助的返回 false / 0 语义与原函数中对应的 return 一致。
// ============================================================

#include "stdafx.h"

#include "Page_Tools.h"
#include "Page_ToolsCommon.h"
#include "Page_ToolsWim.h"       // WIMBridge::QueryWimInfo
#include "NotifyBox.h"           // HostNotify / HostConfirm
#include "StringManager.h"
#include "Win32DialogService.h"

using namespace DuiLib;
using i18n::CStringManager;

using ToolSql::HostNotify;
using ToolSql::HostConfirm;

// [R94] 段 1：选镜像文件并校验存在。
bool CPageTools::WpPickImage(std::wstring& wim)
{
    const wchar_t* kTitle = StringMgr.Get("toolbox.win11prep_title");

    CDuiString sImage;
    if (!Win32DialogService::DlgOpenFile(sImage,
            // [R92] 改走语言包：文件对话框过滤器是可见文案，属「可翻译 UI」。
            StringMgr.Get("toolbox.fdlg_image_filter"), kTitle))
        return false;

    wim = sImage.GetData();
    if (::GetFileAttributesW(wim.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        HostNotify(m_ctx.HostHwnd(), StringMgr.Get("toolbox.win11prep_no_image"), kTitle,
                   MB_OK | MB_ICONWARNING);
        return false;
    }
    return true;
}

// [R94] 段 2+3：枚举索引并让用户选。单索引镜像不打扰用户；
//   多索引逐个询问（是/否），避免引入输入框控件。
//   多索引常见于 MSDN 原版 ISO，索引间 Edition 不同，选错会处理错版本。
bool CPageTools::WpPickIndex(const std::wstring& wim, DWORD& dwIndex)
{
    const wchar_t* kTitle = StringMgr.Get("toolbox.win11prep_title");

    std::vector<WIMBridge::WimInfo> vInfos;
    std::wstring sErr;
    if (!WIMBridge::QueryWimInfo(wim, vInfos, &sErr) || vInfos.empty())
    {
        CDuiString sMsg;
        sMsg.Format(StringMgr.Get("toolbox.win11prep_read_fail"),
                    sErr.empty() ? L"unknown" : sErr.c_str());
        HostNotify(m_ctx.HostHwnd(), sMsg.GetData(), kTitle, MB_OK | MB_ICONERROR);
        return false;
    }

    dwIndex = vInfos[0].index;
    if (vInfos.size() > 1)
    {
        bool bPicked = false;
        for (size_t i = 0; i < vInfos.size(); ++i)
        {
            CDuiString sAsk;
            sAsk.Format(StringMgr.Get("toolbox.win11prep_pick_index"),
                        vInfos[i].index,
                        vInfos[i].name.empty() ? StringMgr.Get("toolbox.index_unnamed")
                                               : vInfos[i].name.c_str(),
                        vInfos[i].edition.empty() ? L"-" : vInfos[i].edition.c_str(),
                        vInfos[i].version.empty() ? L"-" : vInfos[i].version.c_str(),
                        i + 1, vInfos.size());
            if (HostConfirm(m_ctx.HostHwnd(), sAsk.GetData(), kTitle,
                            MB_YESNO | MB_ICONQUESTION) == IDYES)
            {
                dwIndex = vInfos[i].index;
                bPicked = true;
                break;
            }
        }
        if (!bPicked) return false;   // 全部否定 = 用户取消
    }
    return true;
}

// [R94] 段 4：临时挂载点必须与镜像同卷，否则 wimgapi 挂载失败。
bool CPageTools::WpMakeMountDir(const std::wstring& wim, std::wstring& mountDir)
{
    const wchar_t* kTitle = StringMgr.Get("toolbox.win11prep_title");

    wchar_t szTmp[MAX_PATH] = { 0 };
    ::GetTempPathW(MAX_PATH, szTmp);
    mountDir = szTmp;
    mountDir += L"DismPrepMount";
    // 临时目录常在 C:，镜像常在 D:/E:；跨卷时改到镜像所在盘。
    if (wim.size() > 1 &&
        _wcsicmp(wim.substr(0, 2).c_str(), mountDir.substr(0, 2).c_str()) != 0)
    {
        mountDir = wim.substr(0, 2) + L"\\DismPrepMount";
    }

    if (::GetFileAttributesW(mountDir.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        if (!::CreateDirectoryW(mountDir.c_str(), nullptr))
        {
            HostNotify(m_ctx.HostHwnd(), StringMgr.Get("toolbox.win11prep_mountdir_fail"), kTitle,
                       MB_OK | MB_ICONERROR);
            return false;
        }
    }
    return true;
}

// [R94] 段 7：读离线映像真实 build，作为参数传给 Rust。
//   C++ 只负责"取值"，不实现版本策略：build < 26100 的拒绝逻辑只在 Rust 里。
//   读不到返回 0，由 Rust 拒绝，不在此处猜版本。
DWORD CPageTools::WpReadImageBuild(const std::wstring& mountDir)
{
    DWORD dwBuild = 0;
    const std::wstring softwareHive = mountDir + L"\\Windows\\System32\\config\\SOFTWARE";
    if (::RegLoadKeyW(HKEY_LOCAL_MACHINE, L"DismPrepSW", softwareHive.c_str()) == ERROR_SUCCESS)
    {
        wchar_t szBuild[32] = { 0 };
        DWORD cb = sizeof(szBuild) - sizeof(wchar_t);
        const LONG lv = ::RegGetValueW(HKEY_LOCAL_MACHINE,
            L"DismPrepSW\\Microsoft\\Windows NT\\CurrentVersion", L"CurrentBuildNumber",
            RRF_RT_REG_SZ, nullptr, szBuild, &cb);
        ::RegUnLoadKeyW(HKEY_LOCAL_MACHINE, L"DismPrepSW");
        if (lv != ERROR_SUCCESS || swscanf_s(szBuild, L"%lu", &dwBuild) != 1)
            dwBuild = 0;
    }
    return dwBuild;
}
