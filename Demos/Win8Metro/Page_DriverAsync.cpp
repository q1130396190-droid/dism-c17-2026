#include "stdafx.h"
#include <string>   // [C++17] std::to_wstring：失败明细拼接
#include "OfflineSession.h"  // [R10-02] 离线会话经独立组件访问，去 CDataEngine 静态耦合

// ============================================================
// Page_DriverAsync.cpp —— 驱动页「后台枚举」段（R70 拆出）
// 承接 DrvPubEnumThreadProc / StartPubEnum / OnPubData（第三方驱动库枚举后台化）。
// 物理搬迁：函数体零逻辑改动；已核实不依赖 Page_Driver.cpp 匿名 namespace 符号。
// ============================================================

#include "Page_Driver.h"


#include "CustomControls.h"
#include "StringManager.h"
#include "FrameWnd_Util.h"
#include "FrameWnd_UiHelpers.h" // P8：ParsePrefix2
#include "WmMessages.h"        // [R13 P0收敛] 仅需 WM_* 宏；UI 专属常量零使用
#include "DiagnosticsService.h" // Build 末尾驱动签名/回滚快照
#include "Registry.h"          // RegGetString
#include "ExternalToolService.h"  // [R72] StartToolAsync
#include "ProcessRunner.h"  // [R83] P1-15 pnputil /scan-devices 同步执行
#include <thread>               // [卡顿修复] 第三方驱动库后台枚举
#include <memory>               // [C++17 R3] make_unique/unique_ptr
#include "SafeThread.h"          // [BUGFIX rb27] 驱动枚举线程顶层 SEH 边界
#include "BackgroundJobs.h"      // [FIX-P2K] 枚举线程收编：退出等待覆盖
#include "DismApiBridge.h"        // [P1-A6] 离线驱动删除/添加走 dismapi 会话
#include "DriverService.h"        // [P1-A6] 原生导出/重扫
#include "WIMHelperBridge.h"      // [P1-A6] 离线 WIM 驱动扫描（替代 dism /Get-Drivers）
#include "DataEngine.h"           // [P0] WinPE 离线会话 /Image 定向

#include <setupapi.h>
#include <cfgmgr32.h>        // [R114-1] CM_Get_DevNode_Status 读设备状态/问题代码
#include <shlobj.h>            // DoDriverExport：SHBrowseForFolder（保留原直调）
#include <commdlg.h>           // DoDriverAdd：GetOpenFileName（保留原直调）
// [R70] 从 Page_Driver.cpp 随切片一并迁入的线程上下文结构。
//   原定义在 Page_Driver.cpp 匿名命名空间外、切片起点之前，
//   属于本段代码的专属结构（仅 StartPubEnum / DrvPubEnumThreadProc 使用），
//   故随函数一起搬过来，避免两处各定义一份。
struct TDrvPubThreadCtx
{
    HWND hNotify;
    LONG nGen;
};

// [R-TAILFIX 同型] 枚举体 SEH 边界：本函数无 C++ 对象，允许 __try。
// EnumPublished 内部虽有 DrvSafeRead 级探针防护，但 dismapi/setupapi 任何
// 一处硬件异常都会穿透 catch(...)（/EHsc）杀死本线程 —— WM_DRV_PUBDATA
// 永不投递，驱动页永久停在"枚举中"。__except 同时兜住 C++ 异常（SEH 视角
// 0xE06D7363 也是异常），被跳出帧上的对象析构由 SEH 展开正常执行。
static void DrvPubEnumSehBody(TDrvPubMsg* pMsg)
{
    __try
    {
        // 注意：空第三方驱动表也返回 false，但这里只代表"已完成一次枚举"，
        // 结果（含空表）同样回 UI 收口，避免无第三方驱动的机器上反复卡 40s。
        pMsg->bOk = DriverService::EnumPublished(*pMsg->pMap);
    }
    __except (CrashGuardFilter(GetExceptionInformation(), "drv-pub-seh"))
    {
        pMsg->bOk = false;
    }
}

static DWORD WINAPI DrvPubEnumThreadProc(void* pv)
{
    // [C++17 R3] thread ctx: receiver takes ownership, freed at scope end (was: delete right after field copies)
    std::unique_ptr<TDrvPubThreadCtx> ctxGuard(static_cast<TDrvPubThreadCtx*>(pv));
    HWND hNotify = ctxGuard->hNotify;
    LONG nGen = ctxGuard->nGen;

    // [C++17 R3] msg ownership transferred to UI on successful post (incl. pMap member); unique_ptr frees it if post fails
    auto pMsg = std::make_unique<TDrvPubMsg>();
    pMsg->nGen  = nGen;
    pMsg->bOk   = false;
    // [C++17 R3] pMap stays a raw member (struct defined in frozen header); freed here or by UI receiver
    pMsg->pMap  = std::make_unique<std::map<CDuiString, DriverPubInfo>>().release();

    DrvPubEnumSehBody(pMsg.get());

    if (::IsWindow(hNotify)
        && ::PostMessageW(hNotify, WM_DRV_PUBDATA, 0, msgparam::ToParam(pMsg.get())))
    {
        pMsg.release(); // [C++17 R3] ownership transferred to UI thread
    }
    else
    {
        std::unique_ptr<std::map<CDuiString, DriverPubInfo>> mapGuard(pMsg->pMap); // [C++17 R3] detached map member freed at scope end
    }
    return 0;
}

void CPageDriver::StartPubEnum()
{
    // 仅 UI 线程调用。已在跑/本轮已试过则不重复发起：
    // DismGetDrivers 在线会话在慢机上耗时数十秒，禁止每次点行都重跑。
    if (m_bPubLoading || m_bPubTried) return;

    MarkPubLoading(true);
    ++m_nPubGen;
    const LONG nGen = m_nPubGen;
    HWND hNotify = m_ctx.HostHwnd();

    // [C++17 R3] thread ctx: ownership transferred to DrvPubEnumThreadProc; unique_ptr frees it if creation fails
    auto pCtx = std::make_unique<TDrvPubThreadCtx>();
    pCtx->hNotify = hNotify;
    pCtx->nGen   = nGen;
    // [FIX-P2K] 收编 BackgroundJobs：第三方库枚举线程纳入退出等待集合
    HANDLE hTh = BackgroundJobs::Start(&DrvPubEnumThreadProc, pCtx.get(), "drv-pub");
    if (!hTh)
    {
        // 线程都没起来：立即解除加载态，下次进入仍可重试
        MarkPubLoading(false);
        m_ctx.HostLog(L"[drv] thread create failed, pub enum skipped");
        return;
    }
    pCtx.release(); // [C++17 R3] ownership transferred to thread proc (may already be freed there; release does not deref)
}

void CPageDriver::OnPubData(TDrvPubMsg* pMsg)
{
    if (!pMsg) return;

    std::map<CDuiString, DriverPubInfo>* pMap = pMsg->pMap;
    const bool bCurrent = (pMsg->nGen == m_nPubGen);
    // [C++17 R3] receiver takes ownership, freed at scope end (was: delete pMap; delete pMsg; at tail)
    std::unique_ptr<TDrvPubMsg> msgGuard(pMsg);
    std::unique_ptr<std::map<CDuiString, DriverPubInfo>> mapGuard(pMap);

    if (bCurrent)
    {
        MarkPubLoading(false);
        MarkPubTried();
        if (pMap) m_mapDrvPub.swap(*pMap);
        m_ctx.HostLog(L"[drv] 第三方驱动库后台枚举完成");

        // 若用户已选中某设备行，用新到的发布名/厂商/版本/日期重渲染详情
        if (m_nSelCat >= 0 && m_nSelRow >= 0)
        {
            CDuiString sKey;
            sKey.Format(L"drow_%d_%d", m_nSelCat, m_nSelRow);
            if (m_mapDrvRow.find(sKey) != m_mapDrvRow.end())
                ShowDetail(m_nSelCat, m_nSelRow);
        }
    }

}
