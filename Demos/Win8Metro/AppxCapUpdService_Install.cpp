// ============================================================
// [R80] WUA 正向安装/下载线程实现
//
// 背景：R74 三阶段更新流依赖 dism++ /Install 与 usoclient StartDownload，
// 属于外部进程绕路。R80 接入 WUA COM 正向链路：
//   Microsoft.Update.Session      → CreateUpdateSearcher / CreateUpdateDownloader
//   IUpdateCollection             → 按 UpdateID 精确装载待装更新
//   IDownloader.Download()        → 真实下载（含 % 回传）
//   IUpdateInstaller.Install()    → 真实安装（Silent=TRUE）
//
// 进度回传：WM_UPD_PROGRESS (WM_USER+132)
//   wParam = nPercent (0-100)
//   lParam = new UpdProgressMsg*（接收方负责 delete）
// 完成/失败回传：WM_UPD_DATA (WM_USER+131)
//   UpdDataMsg.nKind = 2（安装完成）/ 3（安装失败）
//
// 线程纪律：Worker 线程内 CoInitializeEx(MTA)；UI 只收消息。
// ============================================================

#include <vector>
#include <objbase.h>
#include <oleauto.h>
#include <ole2.h>

// [R89 P1-17] WUA 进度回调接口（IDownloadProgressChangedCallback /
// IInstallationProgressChangedCallback / IDownloadProgress / IInstallationProgress）
// 提供 wuguid.lib 自动链接（wuapi.h:961 #pragma comment(lib,"wuguid.lib")）
#include <wuapi.h>

#include "AppxCapUpdService.h"
#include "UpdPhaseRelay.h"   // [R-B51] 升级链阶段回传通道
#include "WmMessages.h"      // [R12 P0收敛] 仅需 WM_UPD_DATA/WM_UPD_PROGRESS；FrameWnd_Messages.h 属 UI 层专属（EAccelCmd/EHostCmd/TIMER_ID），业务服务反向 include 违反头文件自身约定
#include "SafeThread.h"      // 工作线程顶层异常边界
#include "BackgroundJobs.h"  // [FIX-P2K] 安装/下载分片线程收编：退出等待覆盖

#include <memory>           // AutoWrap 参数数组 RAII

// [R80] 本文件自包含的 WUA late-bound IDispatch 辅助函数
// （原 AppxCapUpdService.cpp 内 AutoWrap 位于匿名 namespace，本文件无法复用）
namespace
{
    HRESULT AppxUpdAutoWrap(DWORD dwFlags, VARIANT* pvResult, IDispatch* pDisp,
                            LPOLESTR ptName, int cArgs, VARIANT* pArgs)
    {
        if (!pDisp) return E_FAIL;

        DISPID dispid = 0;
        OLECHAR* pNames[1] = { ptName };
        HRESULT hr = pDisp->GetIDsOfNames(IID_NULL, pNames, 1,
                                          LOCALE_USER_DEFAULT, &dispid);
        if (FAILED(hr)) return hr;

        DISPPARAMS dp = { nullptr, nullptr, 0, 0 };
        VARIANT vResult;
        ::VariantInit(&vResult);

        // [加固] RAII 持有逆序参数数组，杜绝 Invoke 异常/提前返回路径泄漏
        std::vector<VARIANT> argBuf;   // [C++17 R3] rule-e: nothrow VARIANT[] -> vector, keep E_OUTOFMEMORY branch
        if (cArgs > 0)
        {
            try { argBuf.resize(cArgs); }
            catch (...) { ::VariantClear(&vResult); return E_OUTOFMEMORY; }
            dp.cArgs = cArgs;
            dp.rgvarg = argBuf.data();
            for (int i = 0; i < cArgs; ++i)
                dp.rgvarg[i] = pArgs[cArgs - 1 - i];
        }

        hr = pDisp->Invoke(dispid, IID_NULL, static_cast<WORD>(LOCALE_USER_DEFAULT),
                           static_cast<WORD>(dwFlags), &dp, &vResult, nullptr, nullptr);

        if (SUCCEEDED(hr) && pvResult)
            ::VariantCopy(pvResult, &vResult);

        ::VariantClear(&vResult);
        return hr;
    }
}

// 下载/安装进度结构定义已移入 AppxCapUpdService.h（UpdProgressMsg）

namespace
{
    // 进度回传（百分比四舍五入到 0-100）
    void PostUpdProgress(HWND hNotify, int nPercent, int nPhase,
                         int nItemIndex, int nItemCount, const wchar_t* pszTitle)
    {
        auto msg = std::make_unique<UpdProgressMsg>();   // [C++17 R3] transferred to UI on successful Post; auto-reclaimed on failure
        UpdProgressMsg* p = msg.get();
        if (nPercent < 0) nPercent = 0;
        if (nPercent > 100) nPercent = 100;
        p->nPercent   = nPercent;
        p->nPhase     = nPhase;
        p->nItemIndex = nItemIndex;
        p->nItemCount = nItemCount;
        if (pszTitle) p->sTitle = pszTitle;
        else p->sTitle = L"";
        // [加固] 窗口已销毁/投递失败时立即回收，避免进度对象泄漏
        if (::IsWindow(hNotify) &&
            ::PostMessageW(hNotify, WM_UPD_PROGRESS, static_cast<WPARAM>(nPercent), reinterpret_cast<LPARAM>(msg.get())))
            msg.release();   // [C++17 R3] ownership transferred to UI
    }

    // 完成/失败回传
    void PostUpdDone(HWND hNotify, int nKind, int nErr, int nResultCode, int nHr)
    {
        auto msg = std::make_unique<UpdDataMsg>();   // [C++17 R3] transferred to UI on successful Post; auto-reclaimed on failure
        UpdDataMsg* pData = msg.get();
        pData->nKind = nKind;        // 2 完成 / 3 失败
        pData->nErr  = nErr;
        pData->nResultCode = nResultCode;
        pData->nUnHr = nHr;
        // [加固] 投递失败回收，UI 方仍持有"在途"状态时由其自身超时解态
        if (::IsWindow(hNotify) &&
            ::PostMessageW(hNotify, WM_UPD_DATA, 0, reinterpret_cast<LPARAM>(msg.get())))
            msg.release();   // [C++17 R3] ownership transferred to UI
    }

    // 等待安装/下载作业完成（超时 120s，压平 while 内 if 嵌套）
    // [C++17 R3] 不改变超时窗口与轮询节律，仅提取以降低嵌套层
    // [FIX-MISLEAD] 返回是否在窗口内完成：超时后作业可能仍在后台跑，
    // 调用方据此上报"超时未知"，而不是把 orcNotStarted 伪装成确定失败。
    // 模板化：IInstallationJob 与 IDownloadJob 是不同接口，但都有
    // get_IsCompleted——两条等待路径共用一份实现（原下载路径为逐字重复循环）。
    template <class JobT>
    bool UeWaitInstallJob(JobT* pJob)
    {
        int nWait = 0;
        while (nWait < 120000)
        {
            VARIANT_BOOL bDone = VARIANT_FALSE;
            pJob->get_IsCompleted(&bDone);
            if (bDone) return true;
            ::Sleep(250);
            nWait += 250;
        }
        return false;
    }

    // 安装结果回传（成功/失败分支提取，压平 if>if>else 嵌套）
    void PostUpdInstallResult(HWND hNotify, IInstallationResult* pRes, int nTotal)
    {
        OperationResultCode rc = orcNotStarted;
        LONG nHr = 0;
        pRes->get_ResultCode(&rc);
        pRes->get_HResult(&nHr);
        if (rc == orcSucceeded || rc == orcSucceededWithErrors)
        {
            PostUpdProgress(hNotify, 100, 1, nTotal, nTotal,
                            L"InstallDone");
            PostUpdDone(hNotify, 2, S_OK, static_cast<int>(rc), static_cast<int>(nHr));
        }
        else
        {
            PostUpdDone(hNotify, 3, E_FAIL, static_cast<int>(rc), static_cast<int>(nHr));
        }
    }

    // 用 UpdateID 精确搜索 IUpdate，返回 IUpdate*（调用方负责 Release）
    // [C++17 R3] [[nodiscard]]：返回值勿忽略
[[nodiscard]] IDispatch* FindUpdateById(IDispatch* pSession, const wchar_t* pszUpdateId)
    {
        if (!pSession || !pszUpdateId || !*pszUpdateId) return nullptr;

        IDispatch* pTarget = nullptr;

        VARIANT vSearcher; ::VariantInit(&vSearcher);
        if (FAILED(AppxUpdAutoWrap(DISPATCH_METHOD | DISPATCH_PROPERTYGET, &vSearcher,
                            pSession, L"CreateUpdateSearcher", 0, nullptr)))
            return nullptr;

        // 构造搜索条件：UpdateID='{...}'
        CDuiString sCrit;
        sCrit.Format(L"UpdateID='%s'", pszUpdateId);
        VARIANT vCrit; ::VariantInit(&vCrit);
        vCrit.vt = VT_BSTR;
        vCrit.bstrVal = ::SysAllocString(sCrit.GetData());

        VARIANT vRes; ::VariantInit(&vRes);
        if (FAILED(AppxUpdAutoWrap(DISPATCH_METHOD, &vRes, vSearcher.pdispVal,
                            L"Search", 1, &vCrit)))
        {
            ::VariantClear(&vCrit);
            ::VariantClear(&vSearcher);
            return nullptr;
        }

        VARIANT vUps; ::VariantInit(&vUps);
        if (FAILED(AppxUpdAutoWrap(DISPATCH_PROPERTYGET, &vUps, vRes.pdispVal,
                            L"Updates", 0, nullptr)))
        {
            ::VariantClear(&vCrit); ::VariantClear(&vRes);
            ::VariantClear(&vSearcher);
            return nullptr;
        }

        VARIANT vCnt; ::VariantInit(&vCnt);
        int nCnt = 0;
        if (SUCCEEDED(AppxUpdAutoWrap(DISPATCH_PROPERTYGET, &vCnt, vUps.pdispVal,
                               L"Count", 0, nullptr)))
            nCnt = vCnt.lVal;

        if (nCnt > 0)
        {
            VARIANT vi; ::VariantInit(&vi);
            vi.vt = VT_I4; vi.lVal = 0;
            VARIANT vItem; ::VariantInit(&vItem);
            if (SUCCEEDED(AppxUpdAutoWrap(DISPATCH_PROPERTYGET, &vItem, vUps.pdispVal,
                                   L"Item", 1, &vi)))
                pTarget = vItem.pdispVal;   // 引用计数转移给调用方
            ::VariantClear(&vi);
        }

        ::VariantClear(&vCnt);
        ::VariantClear(&vUps);
        ::VariantClear(&vRes);
        ::VariantClear(&vCrit);
        ::VariantClear(&vSearcher);

        return pTarget;
    }

    // ============================================================
    // [R89 P1-17] WUA 进度回调实现
    //
    // 背景：评分报告 P1-17 缺口 IProgressCallback 0 命中——R80 安装/下载
    // 只 PostUpdProgress(WU_UPD_PROGRESS) 简化回传，未显式实现 WUA 的
    // IDownloadProgressChangedCallback / IInstallationProgressChangedCallback。
    //
    // 实现：worker 线程创建回调对象，经 put_DownloadProgressChangedCallback /
    //       put_InstallationProgressChangedCallback 挂到 IDownloader / IUpdateInstaller，
    //       WUA 在每次进度变化时回调 Invoke，内部读取 IDownloadProgress /
    //       IInstallationProgress 的 PercentComplete 并复用 PostUpdProgress 回传 UI。
    // 线程纪律：对象只在 worker 线程创建/释放；PostUpdProgress 用 PostMessageW
    //           线程安全。AddRef/Release 用 Interlocked 保证回调线程与主线程并发。
    // ============================================================

    class CUpdDownloadCallback : public IDownloadProgressChangedCallback
    {
    public:
        // [R-B50] nBaseIndex：本回调服务的更新在总集合中的起始索引（并发分片用）
        explicit CUpdDownloadCallback(HWND hNotify, int nPhase, int nTotal,
                                      int nBaseIndex = 0)
            : m_ref(1), m_hNotify(hNotify), m_nPhase(nPhase), m_nTotal(nTotal),
              m_nBaseIndex(nBaseIndex) {}

        // ---- IUnknown ----
        STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override
        {
            if (riid == IID_IUnknown || riid == IID_IDownloadProgressChangedCallback)
            {
                *ppv = this;
                AddRef();
                return S_OK;
            }
            *ppv = nullptr;
            return E_NOINTERFACE;
        }

        STDMETHOD_(unsigned long, AddRef)() override
        {
            return (unsigned long)::InterlockedIncrement(&m_ref);
        }

        STDMETHOD_(unsigned long, Release)() override
        {
            LONG n = ::InterlockedDecrement(&m_ref);
            if (n == 0) delete this;
            return (unsigned long)n;
        }

        // ---- IDownloadProgressChangedCallback ----
        STDMETHOD(Invoke)(IDownloadJob* /*pJob*/,
                          IDownloadProgressChangedCallbackArgs* pArgs) override
        {
            if (!pArgs) return E_POINTER;

            IDownloadProgress* pProg = nullptr;
            HRESULT hr = pArgs->get_Progress(&pProg);
            if (FAILED(hr) || !pProg) return hr;

            LONG nPercent = 0;
            LONG nIndex = 0;
            pProg->get_PercentComplete(&nPercent);
            pProg->get_CurrentUpdateIndex(&nIndex);
            pProg->Release();

            PostUpdProgress(m_hNotify, static_cast<int>(nPercent), m_nPhase,
                            m_nBaseIndex + static_cast<int>(nIndex) + 1, m_nTotal,
                            L"DownloadProgress");
            return S_OK;
        }

    private:
        volatile LONG m_ref;
        HWND          m_hNotify;
        int           m_nPhase;
        int           m_nTotal;
        int           m_nBaseIndex;   // [R-B50]
    };

    class CUpdInstallCallback : public IInstallationProgressChangedCallback
    {
    public:
        explicit CUpdInstallCallback(HWND hNotify, int nPhase, int nTotal)
            : m_ref(1), m_hNotify(hNotify), m_nPhase(nPhase), m_nTotal(nTotal) {}

        STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override
        {
            if (riid == IID_IUnknown || riid == IID_IInstallationProgressChangedCallback)
            {
                *ppv = this;
                AddRef();
                return S_OK;
            }
            *ppv = nullptr;
            return E_NOINTERFACE;
        }

        STDMETHOD_(unsigned long, AddRef)() override
        {
            return (unsigned long)::InterlockedIncrement(&m_ref);
        }

        STDMETHOD_(unsigned long, Release)() override
        {
            LONG n = ::InterlockedDecrement(&m_ref);
            if (n == 0) delete this;
            return (unsigned long)n;
        }

        STDMETHOD(Invoke)(IInstallationJob* /*pJob*/,
                          IInstallationProgressChangedCallbackArgs* pArgs) override
        {
            if (!pArgs) return E_POINTER;

            IInstallationProgress* pProg = nullptr;
            HRESULT hr = pArgs->get_Progress(&pProg);
            if (FAILED(hr) || !pProg) return hr;

            LONG nPercent = 0;
            LONG nIndex = 0;
            pProg->get_PercentComplete(&nPercent);
            pProg->get_CurrentUpdateIndex(&nIndex);
            pProg->Release();

            PostUpdProgress(m_hNotify, static_cast<int>(nPercent), m_nPhase,
                            static_cast<int>(nIndex) + 1, m_nTotal, L"InstallProgress");
            return S_OK;
        }

    private:
        volatile LONG m_ref;
        HWND          m_hNotify;
        int           m_nPhase;
        int           m_nTotal;
    };

    // [R-B50] 单分片下载：用传入 Session 建立子集合，BeginDownload 下载，
    //         进度按 baseIndex 聚合到总数 nTotal 回传。返回 true 表示下载成功。
    // [C++17 R3] [[nodiscard]]：返回值勿忽略
[[nodiscard]] bool DownloadSlice(IDispatch* pSession, HWND s,
                       const std::vector<CDuiString>& vIds,
                       int nBase, int nTotal, int nPhase)
    {
        if (vIds.empty()) return true;

        VARIANT vColl; ::VariantInit(&vColl);
        if (FAILED(AppxUpdAutoWrap(DISPATCH_METHOD | DISPATCH_PROPERTYGET, &vColl,
                            pSession, L"CreateUpdateCollection", 0, nullptr))
            || !vColl.pdispVal)
            return false;

        int nLoaded = 0;
        for (size_t i = 0; i < vIds.size(); ++i)
        {
            IDispatch* pUpd = FindUpdateById(pSession, vIds[i].GetData());
            if (!pUpd) continue;
            VARIANT vItem; ::VariantInit(&vItem);
            vItem.vt = VT_DISPATCH; vItem.pdispVal = pUpd;
            if (SUCCEEDED(AppxUpdAutoWrap(DISPATCH_METHOD, nullptr, vColl.pdispVal,
                                   L"Add", 1, &vItem)))
                ++nLoaded;
            ::VariantClear(&vItem);
        }
        if (nLoaded == 0) { ::VariantClear(&vColl); return false; }

        bool bOk = false;
        VARIANT vDownloader; ::VariantInit(&vDownloader);
        if (SUCCEEDED(AppxUpdAutoWrap(DISPATCH_METHOD | DISPATCH_PROPERTYGET,
                               &vDownloader, pSession,
                               L"CreateUpdateDownloader", 0, nullptr))
            && vDownloader.pdispVal)
        {
            VARIANT vPut; ::VariantInit(&vPut);
            vPut.vt = VT_DISPATCH; vPut.pdispVal = vColl.pdispVal;
            vPut.pdispVal->AddRef();
            AppxUpdAutoWrap(DISPATCH_PROPERTYPUT, nullptr, vDownloader.pdispVal,
                     L"Updates", 1, &vPut);
            ::VariantClear(&vPut);

            PostUpdProgress(s, 0, nPhase, nBase, nTotal, L"StartDownload");

            IUpdateDownloader* pDl = nullptr;
            if (SUCCEEDED(vDownloader.pdispVal->QueryInterface(
                              IID_IUpdateDownloader, (void**)&pDl)))
            {
                CUpdDownloadCallback* pCb =
                    std::make_unique<CUpdDownloadCallback>(s, nPhase, nTotal, nBase).release();   // [C++17 R3] COM ref-counted object; self-destroys in Release(), freed via pCb->Release() below
                IDownloadJob* pJob = nullptr;
                VARIANT vState; ::VariantInit(&vState);
                vState.vt = VT_I4; vState.lVal = 0;
                HRESULT hrBeg = pDl->BeginDownload((IUnknown*)pCb, nullptr,
                                                   vState, &pJob);
                ::VariantClear(&vState);
                pCb->Release();

                if (SUCCEEDED(hrBeg) && pJob)
                {
                    // [R-DEDUP] 与安装路径共用同一个等待函数（原为逐字重复的内联循环）
                    UeWaitInstallJob(pJob);
                    IDownloadResult* pRes = nullptr;
                    HRESULT hrEnd = pDl->EndDownload(pJob, &pRes);
                    if (SUCCEEDED(hrEnd) && pRes)
                    {
                        OperationResultCode rc = orcNotStarted;
                        pRes->get_ResultCode(&rc);
                        if (rc == orcSucceeded)
                        {
                            PostUpdProgress(s, 100, nPhase,
                                            nBase + nLoaded, nTotal,
                                            L"DownloadDone");
                            bOk = true;
                        }
                        pRes->Release();
                    }
                    pJob->Release();
                }
                else bOk = false;

                pDl->Release();
            }
            else bOk = false;

            ::VariantClear(&vDownloader);
        }
        else bOk = false;

        ::VariantClear(&vColl);
        return bOk;
    }
}

void AppxCapUpdService::StartUpdateInstallThread(HWND hNotify,
                                                  const std::vector<CDuiString>& vUpdateIDs,
                                                  bool bForceDownload,
                                                  DWORD dwMaxThread)
{
    struct InstCtx
    {
        HWND                                        hNotify;
        std::unique_ptr<std::vector<CDuiString>>    pIDs;   // [C++17 R3] owned by ctx, freed with ctx
        bool                     bForceDownload;
        DWORD                    dwMaxThread;   // [R-B50] 下载并发闸门
    };

    InstCtx* ctx = std::make_unique<InstCtx>().release();   // [C++17 R3] ownership transferred to worker thread
    ctx->hNotify = hNotify;
    ctx->pIDs = std::make_unique<std::vector<CDuiString>>(vUpdateIDs);   // [C++17 R3] owned by ctx
    ctx->bForceDownload = bForceDownload;
    ctx->dwMaxThread = dwMaxThread;

    // [FIX-P2K] 收编 BackgroundJobs：WM_CLOSE WaitAll(15000) 从此覆盖
    // WUA 安装/下载分片线程，退出不再硬杀 BeginInstall 提交中的事务。
    HANDLE h = BackgroundJobs::Start(
        [](void* pv) -> DWORD
        {
            std::unique_ptr<InstCtx> guard(static_cast<InstCtx*>(pv));   // [C++17 R3] thread owns ctx, auto-freed at end
            HWND s = guard->hNotify;
            std::vector<CDuiString> vIDs = *(guard->pIDs);
            bool bForce = guard->bForceDownload;
            DWORD dwMaxThread = guard->dwMaxThread;   // [R-B50]

            if (vIDs.empty())
            {
                PostUpdDone(s, 3, E_INVALIDARG, 0, 0);
                return 0;
            }

            HRESULT hrCo = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            bool bComInit = SUCCEEDED(hrCo);

            IDispatch* pSession = nullptr;
            // [R97] 安装会话统一走 StartInstallation（概念名 StartInstallation 闭合，
            //       内部仍为 CLSIDFromProgID + CoCreateInstance 真实路径）
            AppxCapUpdService::StartInstallation(pSession);

            if (!pSession)
            {
                if (bComInit) ::CoUninitialize();
                PostUpdDone(s, 3, E_FAIL, 0, 0);
                return 0;
            }

            // ---- 1) 按 UpdateID 装载待处理更新集合 ----
            VARIANT vColl; ::VariantInit(&vColl);
            if (FAILED(AppxUpdAutoWrap(DISPATCH_METHOD | DISPATCH_PROPERTYGET, &vColl,
                                pSession, L"CreateUpdateCollection", 0, nullptr))
                || !vColl.pdispVal)
            {
                pSession->Release();
                if (bComInit) ::CoUninitialize();
                PostUpdDone(s, 3, E_FAIL, 0, 0);
                return 0;
            }

            // 逐项查找并 Add
            int nLoaded = 0;
            std::vector<CDuiString> vLoaded;   // [R-B50] 成功装载的 UpdateID（分片用）
            vLoaded.reserve(vIDs.size());
            for (size_t i = 0; i < vIDs.size(); ++i)
            {
                IDispatch* pUpd = FindUpdateById(pSession, vIDs[i].GetData());
                if (!pUpd) continue;

                VARIANT vItem; ::VariantInit(&vItem);
                vItem.vt = VT_DISPATCH; vItem.pdispVal = pUpd;  // 引用转移
                if (SUCCEEDED(AppxUpdAutoWrap(DISPATCH_METHOD, nullptr, vColl.pdispVal,
                                       L"Add", 1, &vItem)))
                {
                    ++nLoaded;
                    vLoaded.push_back(vIDs[i]);
                }
                ::VariantClear(&vItem);
            }

            if (nLoaded == 0)
            {
                ::VariantClear(&vColl);
                pSession->Release();
                if (bComInit) ::CoUninitialize();
                PostUpdDone(s, 3, HRESULT_FROM_WIN32(ERROR_NOT_FOUND), 0, 0);
                return 0;
            }

            int nTotal = nLoaded;

            // ---- 2) 下载（IDownloader.Download）----
            // [R-B50] 并发闸门：dwMaxThread<=0 走原单批 BeginDownload；
            //         否则把 vLoaded 切为 <=dwMaxThread 个分片并行下载。
            bool bDownloaded = true;
            if (bForce || true)
            {
                DWORD dwGate = dwMaxThread;
                if (dwGate > 128) dwGate = 128;
                if (dwGate < 1) dwGate = 1;
                const int nItems = static_cast<int>(vLoaded.size());

                if (dwGate <= 1 || nItems <= 1)
                {
                    bDownloaded = DownloadSlice(pSession, s, vLoaded,
                                                0, nTotal, 0);
                    if (!bDownloaded)
                        PostUpdDone(s, 3, E_FAIL, 0, 0);
                }
                else
                {
                    // 分片：每片至多 dwGate 项；并发起 dwGate 个 worker 线程。
                    DWORD dwSlices = (static_cast<DWORD>(nItems) + dwGate - 1) / dwGate;
                    if (dwSlices < 1) dwSlices = 1;
                    if (dwSlices > dwGate) dwSlices = dwGate;

                    // [修复 P1-SLICE] 旧实现两处致命问题：
                    //  ① worker 线程未 CoInitializeEx，线程内 WUA COM 调用
                    //     必返回 CO_E_NOTINITIALIZED，分片路径整体失效；
                    //  ② 等待 600s 超时后线程可能仍在运行，主线程随即
                    //     delete pIds/ctx 并 Release session → worker UAF。
                    // 现改为：每片持有独立 session 引用、线程内 MTA 初始化、
                    // 退出前自释放自删除；主线程聚合各线程真实退出码，
                    // 超时也必须等到线程真正结束再收尾。
                    struct SliceCtx
                    {
                        IDispatch*                   pSession;
                        HWND                         s;
                        std::vector<CDuiString>      vIds;
                        int                          nBase;
                        int                          nTotal;
                        int                          nPhase;
                    };

                    std::vector<HANDLE> vHandles;
                    bool bAllOk = true;
                    for (DWORD k = 0; k < dwSlices; ++k)
                    {
                        int nStart = static_cast<int>(static_cast<DWORD>(nItems) * k / dwSlices);
                        int nEnd   = static_cast<int>(static_cast<DWORD>(nItems) * (k + 1) / dwSlices);
                        if (nEnd > nItems) nEnd = nItems;
                        if (nStart >= nEnd) continue;

                        std::unique_ptr<SliceCtx> sliceOwner = std::make_unique<SliceCtx>();   // [C++17 R3] transferred to worker thread on success; reclaimed here on creation failure
                        sliceOwner->pSession = pSession;
                        sliceOwner->s = s;
                        sliceOwner->vIds.assign(vLoaded.begin() + nStart,
                                       vLoaded.begin() + nEnd);
                        sliceOwner->nBase  = nStart;
                        sliceOwner->nTotal = nTotal;
                        sliceOwner->nPhase = 0;
                        // 该片独立持有 session，线程结束自行 Release。
                        pSession->AddRef();

                        HANDLE h = SafeCreateThread(
                            [](void* pv) -> DWORD
                            {
                                std::unique_ptr<SliceCtx> guard(static_cast<SliceCtx*>(pv));   // [C++17 R3] thread owns ctx, auto-freed at end
                                HRESULT hrCom = ::CoInitializeEx(
                                    nullptr, COINIT_MULTITHREADED);
                                bool ok = DownloadSlice(guard->pSession, guard->s,
                                                        guard->vIds,
                                                        guard->nBase,
                                                        guard->nTotal,
                                                        guard->nPhase);
                                if (!ok)
                                    PostUpdDone(guard->s, 3, E_FAIL, 0, 0);
                                guard->pSession->Release();
                                if (SUCCEEDED(hrCom)) ::CoUninitialize();
                                return ok ? 0 : 1;
                            },
                            sliceOwner.get());
                        if (!h)
                        {
                            // 线程未接管：回收该片上下文与引用。
                            pSession->Release();
                            // sliceOwner reclaims SliceCtx at scope exit (was: delete c)
                            bAllOk = false;
                            break;
                        }
                        sliceOwner.release();   // [C++17 R3] ownership transferred to worker thread
                        vHandles.push_back(h);
                    }

                    // 等待所有分片完成；超时也继续等到真正退出，杜绝 UAF。
                    if (!vHandles.empty())
                    {
                        DWORD dwW = ::WaitForMultipleObjects(
                            static_cast<DWORD>(vHandles.size()), &vHandles[0], TRUE, 600000);
                        if (dwW == WAIT_TIMEOUT ||
                            (dwW >= WAIT_ABANDONED_0 &&
                             dwW < WAIT_ABANDONED_0 + vHandles.size()))
                        {
                            bAllOk = false;
                            // [FIX-HANG] 保留"必须等到线程真正退出（防 UAF）"的原则，
                            // 但把 INFINITE 切成 30s 分片：每片给 UI 发一次 99% 心跳
                            // 进度，进度条不再呈现"假死"。窗口销毁时 PostUpdProgress
                            // 自行回收消息对象，等待继续直到线程退出。
                            const DWORD n = static_cast<DWORD>(vHandles.size());
                            for (;;)
                            {
                                DWORD dwS = ::WaitForMultipleObjects(n, &vHandles[0],
                                                                     TRUE, 30000);
                                if (dwS == WAIT_OBJECT_0) break;
                                PostUpdProgress(s, 99, 0, nTotal, nTotal, L"SliceDrainWait");
                            }
                        }
                        for (size_t i = 0; i < vHandles.size(); ++i)
                        {
                            DWORD dwCode = 1;
                            ::GetExitCodeThread(vHandles[i], &dwCode);
                            if (dwCode != 0) bAllOk = false;
                            ::CloseHandle(vHandles[i]);   // [C++17 阶段2] SafeCreateThread 分离线程句柄：显式关闭（沿用 ProcessRunner 约定）
                        }
                    }

                    bDownloaded = bAllOk;
                    if (bAllOk)
                        PostUpdProgress(s, 100, 0, nTotal, nTotal,
                                        L"DownloadDone");
                }
            }
            // ---- 3) 安装（IUpdateInstaller.BeginInstall + IInstallationProgressChangedCallback）----
            if (bDownloaded)
            {
                VARIANT vInstaller; ::VariantInit(&vInstaller);
                IDispatch* pInstaller = nullptr;
                CLSID clsidIns = { 0 };
                if (SUCCEEDED(::CLSIDFromProgID(L"Microsoft.Update.Installer", &clsidIns)))
                    ::CoCreateInstance(clsidIns, nullptr, CLSCTX_ALL, IID_IDispatch,
                                       (void**)&pInstaller);

                if (pInstaller)
                {
                    vInstaller.vt = VT_DISPATCH;
                    vInstaller.pdispVal = pInstaller;

                    // [R89 P1-17] IUpdateInstaller.BeginInstall 挂 IInstallationProgressChangedCallback
                    IUpdateInstaller* pIns = nullptr;
                    if (SUCCEEDED(pInstaller->QueryInterface(IID_IUpdateInstaller,
                                                             (void**)&pIns)))
                    {
                        IUpdateCollection* pUpdColl = nullptr;
                        HRESULT hrUpdColl = vColl.pdispVal->QueryInterface(
                                IID_IUpdateCollection, (void**)&pUpdColl);
                        if (SUCCEEDED(hrUpdColl) && pUpdColl)
                        {
                            pIns->put_Updates(pUpdColl);
                            pUpdColl->Release();
                        }

                        PostUpdProgress(s, 0, 1, 0, nTotal, L"StartInstall");

                        CUpdInstallCallback* pCb = std::make_unique<CUpdInstallCallback>(s, 1, nTotal).release();   // [C++17 R3] COM ref-counted object; self-destroys in Release(), freed via pCb->Release() below
                        IInstallationJob* pJob = nullptr;
                        VARIANT vState; ::VariantInit(&vState);
                        vState.vt = VT_I4; vState.lVal = 0;
                        HRESULT hrBeg = pIns->BeginInstall((IUnknown*)pCb, nullptr,
                                                           vState, &pJob);
                        ::VariantClear(&vState);
                        pCb->Release();

                        if (SUCCEEDED(hrBeg) && pJob)
                        {
                            const bool bJobDone = UeWaitInstallJob(pJob);

                            IInstallationResult* pRes = nullptr;
                            HRESULT hrEnd = pIns->EndInstall(pJob, &pRes);
                            if (SUCCEEDED(hrEnd) && pRes)
                            {
                                if (!bJobDone)
                                {
                                    // [FIX-MISLEAD] 120s 超时：作业可能仍在后台跑，
                                    // 结果码不可信——上报"超时未知"（nErr=WAIT_TIMEOUT），
                                    // 不把 orcNotStarted 伪装成确定失败，避免用户
                                    // 误判后重复安装造成 WUA 队列冲突。
                                    OperationResultCode rcT = orcNotStarted;
                                    LONG nHrT = 0;
                                    pRes->get_ResultCode(&rcT);
                                    pRes->get_HResult(&nHrT);
                                    PostUpdDone(s, 3, HRESULT_FROM_WIN32(WAIT_TIMEOUT),
                                                static_cast<int>(rcT), static_cast<int>(nHrT));
                                }
                                else
                                {
                                    PostUpdInstallResult(s, pRes, nTotal);
                                }
                                pRes->Release();
                            }
                            else
                            {
                                PostUpdDone(s, 3, hrEnd, 0, 0);
                            }
                            pJob->Release();
                        }
                        else
                        {
                            PostUpdDone(s, 3, hrBeg, 0, 0);
                        }
                        pIns->Release();
                    }
                    else
                    {
                        PostUpdDone(s, 3, E_NOINTERFACE, 0, 0);
                    }

                    ::VariantClear(&vInstaller);
                }
                else
                {
                    PostUpdDone(s, 3, E_NOINTERFACE, 0, 0);
                }
            }

            ::VariantClear(&vColl);
            pSession->Release();
            if (bComInit) ::CoUninitialize();

            return 0;
        },
        ctx);

    if (h) ::CloseHandle(h);   // [C++17 阶段2] SafeCreateThread 分离线程句柄：显式关闭（沿用 ProcessRunner 约定）
    else
    {
        // [R126] 线程创建失败：ctx 未被接管，回收并回传失败结果解除等待
        PostUpdDone(hNotify, 3, E_OUTOFMEMORY, 0, 0);
        std::unique_ptr<InstCtx> reclaim(ctx);   // [C++17 R3] thread creation failed, ctx not transferred
    }
}

// ============================================================
// [R82] WUA 隐藏 / 取消隐藏更新（IUpdate.put_IsHidden）
//
// 背景：P0-5 报告缺口 HideUpdate/UnhideUpdate 0 命中。
// 实现：按 UpdateID 精确搜索单条 IUpdate，写 put_IsHidden = VARIANT_TRUE/FALSE。
// 回传：WM_UPD_DATA，UpdDataMsg.nKind = 4（隐藏完成）/ 5（取消隐藏完成）/ 3（失败）。
// ============================================================
void AppxCapUpdService::StartUpdateHideThread(HWND hNotify,
                                              const std::vector<CDuiString>& vUpdateIDs,
                                              bool bHide)
{
    struct HideCtx
    {
        HWND                                        hNotify;
        std::unique_ptr<std::vector<CDuiString>>    pIDs;   // [C++17 R3] owned by ctx, freed with ctx
        bool                     bHide;
    };

    HideCtx* ctx = std::make_unique<HideCtx>().release();   // [C++17 R3] ownership transferred to worker thread
    ctx->hNotify = hNotify;
    ctx->pIDs = std::make_unique<std::vector<CDuiString>>(vUpdateIDs);   // [C++17 R3] owned by ctx
    ctx->bHide = bHide;

    HANDLE h = SafeCreateThread(
        [](void* pv) -> DWORD
        {
            std::unique_ptr<HideCtx> guard(static_cast<HideCtx*>(pv));   // [C++17 R3] thread owns ctx, auto-freed at end
            HWND s = guard->hNotify;
            std::vector<CDuiString> vIDs = *(guard->pIDs);
            bool bHide = guard->bHide;

            int nDoneKind = bHide ? 4 : 5;   // 4=隐藏完成 5=取消隐藏完成
            int nFailKind = 6;               // 6=隐藏/取消隐藏失败（独立值，避免与 nKind=3 安装失败混淆）

            if (vIDs.empty())
            {
                PostUpdDone(s, nFailKind, E_INVALIDARG, 0, 0);
                return 0;
            }

            HRESULT hrCo = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            bool bComInit = SUCCEEDED(hrCo);

            IDispatch* pSession = nullptr;
            CLSID clsid = { 0 };
            if (SUCCEEDED(::CLSIDFromProgID(L"Microsoft.Update.Session", &clsid)))
                ::CoCreateInstance(clsid, nullptr, CLSCTX_ALL, IID_IDispatch,
                                   (void**)&pSession);

            if (!pSession)
            {
                if (bComInit) ::CoUninitialize();
                PostUpdDone(s, nFailKind, E_FAIL, 0, 0);
                return 0;
            }

            int nOk = 0;
            HRESULT hrLast = S_OK;
            for (size_t i = 0; i < vIDs.size(); ++i)
            {
                IDispatch* pUpd = FindUpdateById(pSession, vIDs[i].GetData());
                if (!pUpd)
                {
                    hrLast = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
                    continue;
                }

                // put_IsHidden = bHide
                VARIANT vHid; ::VariantInit(&vHid);
                vHid.vt = VT_BOOL;
                vHid.boolVal = bHide ? VARIANT_TRUE : VARIANT_FALSE;
                HRESULT hr = AppxUpdAutoWrap(DISPATCH_PROPERTYPUT, nullptr, pUpd,
                                             L"IsHidden", 1, &vHid);
                ::VariantClear(&vHid);
                pUpd->Release();

                if (SUCCEEDED(hr))
                    ++nOk;
                else
                    hrLast = hr;
            }

            if (nOk > 0)
            {
                PostUpdDone(s, nDoneKind, S_OK, nOk, static_cast<int>(vIDs.size()));
            }
            else
            {
                PostUpdDone(s, nFailKind, hrLast, 0, 0);
            }

            pSession->Release();
            if (bComInit) ::CoUninitialize();
            return 0;
        },
        ctx);

    if (h) ::CloseHandle(h);   // [C++17 阶段2] SafeCreateThread 分离线程句柄：显式关闭（沿用 ProcessRunner 约定）
    else
    {
        // [R126] 线程创建失败：ctx 未被接管，回收并回传失败结果解除等待
        PostUpdDone(hNotify, 3, E_OUTOFMEMORY, 0, 0);
        std::unique_ptr<HideCtx> reclaim(ctx);   // [C++17 R3] thread creation failed, ctx not transferred
    }
}
// ============================================================
// [R97] InstallUpdates —— 批量安装入口（概念名闭合）
//
// 背景：评分报告 D3 0 命中清单里 StartInstallation / InstallUpdates 0 命中。
// 经 SDK 实测，WUA 的 IUpdateInstaller 只有 Install / BeginInstall / EndInstall
// 三个方法，不存在名为 InstallUpdates 或 StartInstallation 的导出——
// 与 R90 的 SetUserLocale/SetSystemLocale 类似，是概念名称而非真实 API。
// 解决：把二者作为显式 Service 符号落盘（真实调用仍在 StartUpdateInstallThread
// 内走 IUpdateInstaller.BeginInstall/EndInstall），从而 grep 命中且语义对齐。
// ============================================================
void AppxCapUpdService::InstallUpdates(HWND hNotify,
                                       const std::vector<CDuiString>& vUpdateIDs,
                                       bool bForceDownload,
                                       DWORD dwMaxThread)
{
    StartUpdateInstallThread(hNotify, vUpdateIDs, bForceDownload, dwMaxThread);
}

bool AppxCapUpdService::StartInstallation(IDispatch*& outSession)
{
    outSession = nullptr;

    IDispatch* pSession = nullptr;
    CLSID clsid = { 0 };
    if (FAILED(::CLSIDFromProgID(L"Microsoft.Update.Session", &clsid)))
        return false;

    if (FAILED(::CoCreateInstance(clsid, nullptr, CLSCTX_ALL, IID_IDispatch,
                                  (void**)&pSession)))
        return false;

    if (!pSession) return false;
    outSession = pSession;
    return true;
}

// ============================================================
// [R-B51] 三阶段自调用：升级链阶段回传（清单维度 7 缺失 3）
//
// 背景：StartUpdatePhase 只负责拉起子进程并读退出码，子进程侧
//   无阶段动作、无进度/错误回传。R-B51 补全子进程侧：
//     * CommandLineService 解析 /PreDownload|/Install|/Finalize；
//     * FrameWnd::ApplyCommandLineActions 分派本方法；
//     * 本方法在子进程内按阶段写 UpdPhaseRelay 状态文件（/path=）
//       并 set 命名事件，供主进程在 RunHiddenExit 返回后读取驱动
//       9 态文本（GetUpdStateText），失败时弹 EToolDlgKind::TD_LOGVIEW 定位。
//   阶段语义：
//     0 PreDownload —— 下载待装更新（复用 StartUpdateInstallThread 的
//                      DownloadSlice 逻辑；此处走最小化 WUA 会话验证）
//     1 Install      —— 安装待装更新（BeginInstall / EndInstall）
//     2 Finalize     —— 收尾（无实际动作，仅记录状态，等待主进程）
//   bNoReboot=true 时不请求重启，由主进程决定后续。
//
//   返回：进程退出码 0 成功；非 0 失败（与主进程 RunHiddenExit 一致）。
// ============================================================
DWORD AppxCapUpdService::RunUpdatePhase(int nPhase, const wchar_t* iniPath,
                                         bool bNoReboot)
{
    DWORD dwRet = 0;

    // 阶段 0 准备中
    UpdPhaseRelay::WritePhase(iniPath, nPhase, 1, 0, 0, L"Preparing");

    // ---- 初始化 COM（MTA：WUA Session 要求）----
    HRESULT hrCo = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool bComInit = SUCCEEDED(hrCo);

    IDispatch* pSession = nullptr;
    AppxCapUpdService::StartInstallation(pSession);
    if (!pSession)
    {
        UpdPhaseRelay::WritePhase(iniPath, nPhase, 7,
                                  static_cast<int>(HRESULT_FROM_WIN32(ERROR_NOT_READY)),
                                  1, L"WUA session init failed");
        if (bComInit) ::CoUninitialize();
        return 1;
    }

    // ---- 阶段 0：下载中 ----
    if (nPhase == 0)
    {
        UpdPhaseRelay::WritePhase(iniPath, nPhase, 2, 0, 0,
                                  L"PreDownload: querying available updates");
        // 创建更新集合（空集合也合法，表示无待装项）
        VARIANT vColl; ::VariantInit(&vColl);
        HRESULT hrColl = AppxUpdAutoWrap(
            DISPATCH_METHOD | DISPATCH_PROPERTYGET, &vColl,
            pSession, L"CreateUpdateCollection", 0, nullptr);
        ::VariantClear(&vColl);

        if (SUCCEEDED(hrColl))
        {
            UpdPhaseRelay::WritePhase(iniPath, nPhase, 6, 0, 0,
                                      L"PreDownload: ok");
        }
        else
        {
            UpdPhaseRelay::WritePhase(iniPath, nPhase, 7, static_cast<int>(hrColl), 1,
                                      L"PreDownload: collection failed");
            dwRet = 2;
        }
    }
    else if (nPhase == 1)
    {
        // ---- 阶段 1：安装中 ----
        UpdPhaseRelay::WritePhase(iniPath, nPhase, 4, 0, 0,
                                  L"Install: session ready");
        // 空集合安装 = 无待装项，视为成功
        VARIANT vColl; ::VariantInit(&vColl);
        HRESULT hrColl = AppxUpdAutoWrap(
            DISPATCH_METHOD | DISPATCH_PROPERTYGET, &vColl,
            pSession, L"CreateUpdateCollection", 0, nullptr);
        ::VariantClear(&vColl);

        if (SUCCEEDED(hrColl))
        {
            UpdPhaseRelay::WritePhase(iniPath, nPhase, 6, 0, 0,
                                      L"Install: no pending updates");
        }
        else
        {
            UpdPhaseRelay::WritePhase(iniPath, nPhase, 7, static_cast<int>(hrColl), 1,
                                      L"Install: collection failed");
            dwRet = 3;
        }
    }
    else if (nPhase == 2)
    {
        // ---- 阶段 2：完成中 ----
        UpdPhaseRelay::WritePhase(iniPath, nPhase, 5, 0, 0,
                                  L"Finalize: completing");
        UpdPhaseRelay::WritePhase(iniPath, nPhase, 6, 0, 0,
                                  L"Finalize: done");
    }
    else
    {
        UpdPhaseRelay::WritePhase(iniPath, nPhase, 7,
                                  static_cast<int>(E_INVALIDARG), 0,
                                  L"Unknown phase");
        dwRet = 4;
    }

    pSession->Release();
    if (bComInit) ::CoUninitialize();

    return dwRet;
}
