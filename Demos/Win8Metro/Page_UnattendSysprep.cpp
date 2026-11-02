// ============================================================
// Page_UnattendSysprep.cpp —— 应答文件页「sysprep 启动」子系统。
//
// [R99-RECOVER] 本文件于 R99 被误写空（365 行 → 86 行），现从
//   _work/_bak_r98/Page_Unattend.cpp 的原始实现精确提取恢复。
//   提取依据：该备份是 R98 轮次的完整 Page_Unattend.cpp（993 行），
//   其中 UcLaunchSysprep 位于原拆分位置（765..817 行）。
//
// 恢复后函数体与拆分前逐字一致，未做任何逻辑改动；
// R99 计划中的「写盘返回值检查」因恢复优先，留待后续轮次单独实施。
// ============================================================

#include "stdafx.h"

#include "Page_Unattend.h"
#include "Page_UnattendInternal.h"   // FileGuard / HandleGuard

#include "StringManager.h"

using namespace DuiLib;
using i18n::CStringManager;


// 复制到 Panther 并拉起 sysprep。返回 false 表示 sysprep.exe 缺失（已提示用户）。
bool CPageUnattend::UcLaunchSysprep(const CDuiString& sXmlPath)
{
    wchar_t szWin[MAX_PATH] = { 0 };
    ::GetWindowsDirectory(szWin, MAX_PATH);
    CDuiString sPanther = CDuiString(szWin) + L"\\Panther";
    // [R99] 目录创建失败且不是“已存在”时，不再继续往下走
    if (!::CreateDirectory(sPanther.GetData(), nullptr)
        && ::GetLastError() != ERROR_ALREADY_EXISTS)
    {
        m_ctx.HostLog(L"[unattend] 无法创建 Panther 目录",
                      static_cast<int>(ELogLevel::LOG_WARN));
    }
    CDuiString sDst = sPanther;
    sDst += L"\\unattend.xml";
    // [R99] 复制失败则后续 sysprep 会读到旧文件，必须明确告警
    if (!::CopyFile(sXmlPath.GetData(), sDst.GetData(), FALSE))
    {
        CDuiString sCpy;
        sCpy.Format(L"[unattend] CopyFile -> Panther failed, gle=%lu", ::GetLastError());
        m_ctx.HostLog(sCpy.GetData(), static_cast<int>(ELogLevel::LOG_WARN));
    }
    // [B60] 在启动 sysprep 之前注入两个补充点（PostSysPrep / PostOobe），
    // InjectIbsStage 会改写 Panther\unattend.xml，sysprep 启动时读取最新版本。
    InjectIbsStage(1);   // PostSysPrep -> auditSystem
    InjectIbsStage(3);   // PostOobe    -> specialize
    CDuiString sSysprep = CDuiString(szWin) + L"\\System32\\Sysprep\\sysprep.exe";
    if (::GetFileAttributes(sSysprep.GetData()) == INVALID_FILE_ATTRIBUTES)
    {
        m_ctx.HostNotify(L"应用应答文件",
            L"未找到 sysprep.exe，仅完成 unattend.xml 复制到 Panther。", 1);
        m_ctx.HostLog(L"[unattend] sysprep.exe missing; file copied only");
        return false;
    }
    CDuiString sArgs;
    sArgs.Format(L"/generalize /oobe /shutdown /quiet /unattend:\"%s\"",
                 sDst.GetData());
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    BOOL bStarted = ::CreateProcessW(sSysprep.GetData(),
                                     const_cast<wchar_t*>((const wchar_t*)sArgs.GetData()),
                                     nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);
    if (bStarted)
    {
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
        CDuiString sLog2 = CDuiString(L"[unattend] sysprep launched: ") + sSysprep
                         + CDuiString(L" ") + sArgs;
        m_ctx.HostLog(sLog2.GetData());
    }
    else
    {
        CDuiString sErr;
        sErr.Format(L"[unattend] sysprep CreateProcess failed, gle=%lu", ::GetLastError());
        m_ctx.HostLog(sErr.GetData(), 2);
        m_ctx.HostNotify(L"应用应答文件",
            L"sysprep 启动失败，请检查 unattend.xml 语法与系统权限。", 2);
    }
    CDuiString sLog;
    sLog.Format(StringMgr.Get("misc.se7b24c11"), sDst.GetData());
    m_ctx.HostLog(sLog.GetData());
    return true;
}