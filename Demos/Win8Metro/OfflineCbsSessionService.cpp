// ============================================================================
//  OfflineCbsSessionService.cpp —— [R16][T8] 菜单层业务下沉
//  深链路逻辑自 FrameWnd_Menu.cpp::MenuCmdAddSourceDir 逐字搬迁
// （行为等价基线 2026-10-13）；CDuiString::Format 以 swprintf 等价替换。
// ============================================================================

#include <windows.h>
#include <string>
#include <vector>
#include <cstdarg>

#include "OfflineCbsSessionService.h"
#include "FormatW.h"
#include "OfflineCbsService.h"
#include "CbsHostClient.h"

namespace OfflineCbsSession
{

namespace
{
    // CDuiString::Format 等价（服务层不引 DuiLib）
    std::wstring FmtW(const wchar_t* pszFmt, ...)
    {
        va_list args;
        va_start(args, pszFmt);
        std::wstring s = wfmt::FormatV(pszFmt, args);
        va_end(args);
        return s;
    }
}

TPrepareResult PrepareSourceDir(const std::wstring& sMountRoot)
{
    TPrepareResult r;
    r.sTitle = L"离线 CBS 深链路";

    // ---- [R-B13] 离线 CBS 深链路 ----
    OfflineCbs::OfflineReadiness rd;
    rd = {};
    bool bReady = OfflineCbs::CheckReadiness(sMountRoot.c_str(), &rd);
    r.bReady = bReady;
    if (bReady)
    {
        // 就绪：停 TI → 挂蜂巢 → 写会话头日志（不提交卸载，由退出路径统一释放）
        DWORD dwErr = 0;
        OfflineCbs::svc::StopTrustedInstaller(30000, &dwErr);
        std::vector<std::wstring> vFail;
        bool bMounted = OfflineCbs::hives::MountAllHives(sMountRoot.c_str(), &vFail);
        OfflineCbs::log::BeginOfflineCbsSession(sMountRoot.c_str());

        // ---- [R-拆分] CBSHost.dll 接线：经客户端写 Dism++ Core 同源 CbsHost.log ----
        CbsHostClient::WriteLog(2 /* Info */,
            L"[R-B26] OfflineCbsSession begin: root=%s hives_ok=%d ti_stopped=%d",
            sMountRoot.c_str(), bMounted ? 1 : 0, 1);
        std::wstring wsScratch;
        if (CbsHostClient::GetScratchDir(&wsScratch))
        {
            CbsHostClient::WriteLog(3 /* Verbose */,
                L"[R-B26] DismGetScratchDir=%s", wsScratch.c_str());
        }

        r.bWarning = false;
        if (bMounted)
        {
            r.sMessage = FmtW(L"[离线CBS] 源目录就绪：%s\r\n"
                              L"  蜂巢 7 件全部挂载成功\r\n"
                              L"  TrustedInstaller 已停止\r\n"
                              L"  pending.xml 前置校验通过\r\n"
                              L"  OfflineCbs.log 会话已开始",
                              sMountRoot.c_str());
        }
        else
        {
            std::wstring sFailDetail;
            for (size_t k = 0; k < vFail.size(); ++k)
            {
                if (k) sFailDetail += L"; ";
                sFailDetail += vFail[k];
            }
            r.sMessage = FmtW(L"[离线CBS] 蜂巢部分挂载失败：%s\r\n"
                              L"  TrustedInstaller 已停止\r\n"
                              L"  失败项请按提示处理后再执行离线操作",
                              sFailDetail.c_str());
        }
    }
    else
    {
        std::wstring sDetail = rd.sDetail.empty() ? std::wstring(L"未知") : rd.sDetail;
        r.bWarning = true;
        r.sMessage = FmtW(L"[离线CBS] 源目录 %s 未满足前置条件：%s\r\n\r\n"
                          L"  蜂巢可挂: %s\r\n"
                          L"  TrustedInstaller 已停止: %s\r\n"
                          L"  pending.xml 校验: %s\r\n"
                          L"  日志目录可写: %s",
                          sMountRoot.c_str(), sDetail.c_str(),
                          rd.bHivesOk ? L"是" : L"否",
                          rd.bTiStopped ? L"是" : L"否",
                          rd.bPendingOk ? L"是" : L"否",
                          rd.bLogDirWritable ? L"是" : L"否");
    }
    return r;
}

} // namespace OfflineCbsSession
// [C++17 R3] 文件级内联常量（C++17 inline variables，ODR 安全）：
