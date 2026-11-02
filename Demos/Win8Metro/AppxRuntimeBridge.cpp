// ============================================================================
//  AppxRuntimeBridge.cpp —— [P0-A1] WinRT PackageManager 原生实现
// ============================================================================

#include "AppxRuntimeBridge.h"

// F8 禁列清零：统一字符串格式化封装
#include "FormatW.h"


#include <windows.h>
#include <roapi.h>
#include <inspectable.h>

#include <wrl.h>
#include <wrl/wrappers/corewrappers.h>

#include <windows.foundation.h>
#include <windows.foundation.collections.h>
#include <windows.storage.h>
#include <windows.applicationmodel.h>
#include <windows.management.deployment.h>

#include <functional>
#include <cstdio>
#include "SafeThread.h"   // MTA 工作线程异常边界

// [C++17 阶段2] 局部 RAII 守卫：事件/查找句柄（见 WinHandles.h 使用规范）
struct ARBHandleGuard
{
    HANDLE h;
    explicit ARBHandleGuard(HANDLE x) : h(x) {}
    ~ARBHandleGuard() { if (h && h != INVALID_HANDLE_VALUE) ::CloseHandle(h); }
    // [C++17 R-P8] 不再禁止拷贝：该守卫会被 WRL::Callback λ 按值捕获。
    // 按值捕获的副本只读取 h，不会额外释放；另外用 reset() 控制生命期。
    ARBHandleGuard(const ARBHandleGuard&) = default;
    ARBHandleGuard& operator=(const ARBHandleGuard&) = delete;
    void reset(HANDLE x = nullptr) { h = x; }
    operator HANDLE() const { return h; }
};
struct ARBFindGuard
{
    HANDLE h;
    explicit ARBFindGuard(HANDLE x) : h(x) {}
    ~ARBFindGuard() { if (h && h != INVALID_HANDLE_VALUE) ::FindClose(h); }
    ARBFindGuard(const ARBFindGuard&) = delete;
    ARBFindGuard& operator=(const ARBFindGuard&) = delete;
    operator HANDLE() const { return h; }
};

namespace WRL = Microsoft::WRL;
using Microsoft::WRL::Wrappers::HString;

namespace AbiWF = ABI::Windows::Foundation;
namespace AbiWFC = ABI::Windows::Foundation::Collections;
namespace AbiWS = ABI::Windows::Storage;
namespace AbiWAM = ABI::Windows::ApplicationModel;
namespace AbiWMD = ABI::Windows::Management::Deployment;

using DeployOp = AbiWF::IAsyncOperationWithProgress<AbiWMD::DeploymentResult*,
                                                    AbiWMD::DeploymentProgress>;
using DeployHandler = AbiWF::IAsyncOperationWithProgressCompletedHandler<AbiWMD::DeploymentResult*,
                                                                         AbiWMD::DeploymentProgress>;
using DepVector = AbiWFC::IVector<AbiWF::IUriRuntimeClass*>;

namespace
{
    // ---- 小工具 ----
    std::wstring HstrToWs(HSTRING h)
    {
        if (!h) return std::wstring();
        UINT32 nLen = 0;
        PCWSTR p = ::WindowsGetStringRawBuffer(h, &nLen);
        return std::wstring(p ? p : L"", nLen);
    }

    void SetErr(std::wstring* pErr, const std::wstring& s)
    {
        if (pErr) *pErr = s;
    }

    // 在独立 MTA 线程执行 WinRT 调用并同步等待（保护调用方 STA 套间）
    // [FIX-HANG→FIX-UAF 回退说明] 上一版把 INFINITE 改成 120s 超时返回，
    //   但 fn 按值复制进 ctx 后仍可能捕获调用方栈变量的引用（如
    //   EnumeratePackages 的 [&] 捕获）——超时返回后调用方栈帧销毁，
    //   MTA 线程继续写这些引用 = 悬垂写（比挂死更危险）。
    //   现恢复"必须等线程真正结束"的原则，但 30s 分片 + 超时告警留痕，
    //   挂死可观测；作业级解态由上层看门狗负责。
    HRESULT RunInMta(const std::function<HRESULT()>& fn)
    {
        struct Ctx
        {
            std::function<HRESULT()> fn;
            HRESULT hr;
        };
        Ctx* pCtx = new Ctx{ fn, E_FAIL };

        // 异常边界：WinRT 调用抛异常时 hr 保持 E_FAIL 返回调用方，不杀进程
        HANDLE hTh = SafeCreateThread(
            [](void* pv) -> DWORD
            {
                // [STAB] 所有权归调用方：本线程只读写、不释放。原实现
                //   unique_ptr<Ctx> 在线程退出时 delete，调用方 WaitForSingleObject
                //   返回后又读 pCtx->hr 并 delete —— 同一指针被两个线程各删一次
                //   （double free），且那次读发生在释放之后（use-after-free）。
                //   堆元数据被踩后表现为"切页随机崩溃 0xC0000374"：崩溃点与
                //   肇事点分离，时间随机。线程结束后 unique_ptr 在调用方统一回收。
                Ctx* p = static_cast<Ctx*>(pv);
                HRESULT hrInit = ::RoInitialize(RO_INIT_MULTITHREADED);
                try { p->hr = p->fn(); }
                catch (...) { p->hr = E_FAIL; }
                if (hrInit == S_OK || hrInit == S_FALSE) ::RoUninitialize();
                return 0;
            },
            pCtx);
        if (!hTh) { delete pCtx; return HRESULT_FROM_WIN32(::GetLastError()); }

        ARBHandleGuard hThGuard = ARBHandleGuard(hTh);
        for (int nSlice = 1; ; ++nSlice)
        {
            const DWORD dwW = ::WaitForSingleObject(hTh, 30000);
            if (dwW == WAIT_OBJECT_0) break;
            // 30s 未结束：留痕但继续等待（线程持有 pCtx 与 fn 的捕获，
            // 任何提前返回都可能让线程写已销毁的调用方栈）。
            // [FIX-P2O] 挂死可观测化升级：30s ODS 照旧；每 4 片（~2 分钟）
            // 追加一条汇总，超 10 分钟文案升级为疑似死锁——上线日志页可
            // 直接据此定位。等待语义保持不变（7 个调用点均为工作线程，
            // 页面看门狗负责解在途态，泄漏上限=单个挂死线程）。
            wchar_t szDbg[160];
            if (nSlice >= 20)
                wsprintfW(szDbg, L"[AppxRuntimeBridge] RunInMta STILL RUNNING after %ds (possible WinRT deadlock; worker leaked by design)\n",
                          nSlice * 30);
            else
                wsprintfW(szDbg, L"[AppxRuntimeBridge] RunInMta still running after %ds\n",
                          nSlice * 30);
            ::OutputDebugStringW(szDbg);
        }
        const HRESULT hr = pCtx->hr;
        delete pCtx;
        return hr;
    }

    // 等待 Appx 部署异步操作完成，取回 DeploymentResult 错误码/文本
    HRESULT WaitDeployment(DeployOp* pOp, HRESULT* phrDeploy, std::wstring* pErrText,
                           DWORD dwTimeoutMs = 600000)
    {
        ARBHandleGuard hEvt = ARBHandleGuard(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!hEvt) return HRESULT_FROM_WIN32(::GetLastError());

        auto spHandler = WRL::Callback<DeployHandler>(
            [hEvt](DeployOp*, AbiWF::AsyncStatus) -> HRESULT
            {
                ::SetEvent(hEvt);
                return S_OK;
            });

        HRESULT hr = pOp->put_Completed(spHandler.Get());
        // 契约：操作若已完成，handler 会在 put_Completed 内同步立即回调
        if (SUCCEEDED(hr))
        {
            DWORD dwW = ::WaitForSingleObject(hEvt, dwTimeoutMs);
            if (dwW == WAIT_TIMEOUT)
            {
                return HRESULT_FROM_WIN32(ERROR_TIMEOUT);  // ARBHandleGuard 自动关闭
            }
        }
        if (FAILED(hr)) return hr;

        // GetResults 在失败时直接返回部署 HRESULT（等价 IAsyncInfo.ErrorCode）
        WRL::ComPtr<AbiWMD::IDeploymentResult> spRes;
        HRESULT hrRes = pOp->GetResults(spRes.GetAddressOf());
        if (phrDeploy) *phrDeploy = hrRes;
        if (SUCCEEDED(hrRes)) return hrRes;

        if (spRes)
        {
            HSTRING hText = nullptr;
            if (SUCCEEDED(spRes->get_ErrorText(&hText)) && hText)
            {
                if (pErrText) *pErrText = HstrToWs(hText);
                ::WindowsDeleteString(hText);
            }
        }
        return hrRes;
    }

    bool CreatePackageManager(WRL::ComPtr<AbiWMD::IPackageManager>* ppPM)
    {
        HString hCls;
        if (FAILED(hCls.Set(RuntimeClass_Windows_Management_Deployment_PackageManager)))
            return false;

        WRL::ComPtr<IInspectable> spInsp;
        if (FAILED(::RoActivateInstance(hCls.Get(), &spInsp)) || !spInsp)
            return false;

        return SUCCEEDED(spInsp.As(ppPM)) && *ppPM;
    }

    // 递归复制目录（原生 FindFirstFile/CopyFile，跳过 reparse 点）
    bool CopyTree(const std::wstring& sSrc, const std::wstring& sDst, std::wstring* pErr)
    {
        if (!::CreateDirectoryW(sDst.c_str(), nullptr))
        {
            DWORD dwE = ::GetLastError();
            if (dwE != ERROR_ALREADY_EXISTS)
            {
                SetErr(pErr, L"CreateDirectory failed: " + sDst);
                return false;
            }
        }

        WIN32_FIND_DATAW fd = {};
        ARBFindGuard hFind(::FindFirstFileW((sSrc + L"\\*").c_str(), &fd));
        if (hFind == INVALID_HANDLE_VALUE)
        {
            SetErr(pErr, L"FindFirstFile failed: " + sSrc);
            return false;
        }

        bool bOk = true;
        do
        {
            if (lstrcmpW(fd.cFileName, L".") == 0 || lstrcmpW(fd.cFileName, L"..") == 0)
                continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                continue;

            std::wstring sF = sSrc + L"\\" + fd.cFileName;
            std::wstring sT = sDst + L"\\" + fd.cFileName;

            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                if (!CopyTree(sF, sT, pErr)) { bOk = false; break; }
            }
            else if (!::CopyFileW(sF.c_str(), sT.c_str(), FALSE))
            {
                constexpr int kErrMsgWchars = 512;   // 512 缓冲尺寸（语义命名，数值不变）
                wchar_t sz[kErrMsgWchars] = { 0 };
                FormatTo(sz, _countof(sz), L"CopyFile failed 0x%08X: %s",
                           (unsigned)::GetLastError(), sF.c_str());
                SetErr(pErr, sz);
                bOk = false;
                break;
            }
        } while (::FindNextFileW(hFind, &fd));

        return bOk;
    }

    bool HasPkgExt(const std::wstring& sFile)
    {
        static const wchar_t* const kExt[] = {
            L".appx", L".appxbundle", L".msix", L".msixbundle"
        };
        for (const wchar_t* p : kExt)
        {
            size_t n = wcslen(p);
            if (lstrcmpiW(sFile.c_str() + (sFile.size() > n ? sFile.size() - n : 0), p) == 0)
                return true;
        }
        return false;
    }

    std::wstring ToFileUri(const std::wstring& sPath)
    {
        std::wstring s = sPath;
        for (auto& ch : s) if (ch == L'\\') ch = L'/';
        return L"file:///" + s;
    }

    HRESULT MakeUri(HSTRING hUri, WRL::ComPtr<AbiWF::IUriRuntimeClass>* ppUri)
    {
        HString hCls;
        HRESULT hr = hCls.Set(RuntimeClass_Windows_Foundation_Uri);
        if (FAILED(hr)) return hr;

        WRL::ComPtr<AbiWF::IUriRuntimeClassFactory> spFactory;
        hr = AbiWF::GetActivationFactory(hCls.Get(), &spFactory);
        if (FAILED(hr)) return hr;
        return spFactory->CreateUri(hUri, ppUri->GetAddressOf());
    }
}

namespace AppxRuntimeBridge
{

// [N05] 依赖包枚举抽为独立函数（原 for(;;)>if>if>if>if>for>if 7 层压平）。
//       逐字搬迁原内层逻辑：取依赖包 Id.FullName，失败/空即跳过。
static HRESULT AppxCollectDeps(AbiWAM::IPackage* spPkg, PkgInfo& info)
{
    if (!spPkg) return E_POINTER;
    WRL::ComPtr<AbiWFC::IVectorView<AbiWAM::Package*>> spDepsView;
    if (FAILED(spPkg->get_Dependencies(&spDepsView)) || !spDepsView)
        return E_NOINTERFACE;
    UINT32 nDep = 0;
    if (FAILED(spDepsView->get_Size(&nDep)))
        return E_FAIL;
    for (UINT32 d = 0; d < nDep; ++d)
    {
        WRL::ComPtr<AbiWAM::IPackage> spDep;
        if (FAILED(spDepsView->GetAt(d, spDep.GetAddressOf())) || !spDep)
            continue;
        WRL::ComPtr<AbiWAM::IPackageId> spDepId;
        if (FAILED(spDep->get_Id(&spDepId)) || !spDepId)
            continue;
        HSTRING hDep = nullptr;
        if (SUCCEEDED(spDepId->get_FullName(&hDep)) && hDep)
        {
            info.vDeps.push_back(HstrToWs(hDep));
            ::WindowsDeleteString(hDep);
        }
    }
    return S_OK;
}

// [N06] FullName 提取抽为独立函数（原 if>if>if>if>if>if 6 层压平）。
//       逐字搬迁：包状态校验失败时取 Id.FullName 记录 sFull。
static void AppxGetBadPkgFullName(AbiWAM::IPackage* spPkg, std::wstring& sFull)
{
    if (!spPkg) return;
    WRL::ComPtr<AbiWAM::IPackageId> spId;
    if (FAILED(spPkg->get_Id(&spId)) || !spId)
        return;
    HSTRING hFull = nullptr;
    if (SUCCEEDED(spId->get_FullName(&hFull)) && hFull)
    {
        sFull = HstrToWs(hFull);
        ::WindowsDeleteString(hFull);
    }
}

// [N06 扩展] 单个包完整 PkgInfo 填充抽为独立函数（原 EnumeratePackages 内
//       for(;;)>if>if>if>if>if>if 6 层压平）。逐字搬迁原逻辑，每步卫语句早退，
//       失败静默跳过，与旧行为一致。
static void AppxPopulateInfo(AbiWAM::IPackage* spPkg, PkgInfo& info)
{
    if (!spPkg) return;
    WRL::ComPtr<AbiWAM::IPackageId> spId;
    if (FAILED(spPkg->get_Id(&spId)) || !spId)
        return;

    HSTRING hV = nullptr;
    if (SUCCEEDED(spId->get_FullName(&hV)) && hV)
    {
        info.sFullName = HstrToWs(hV);
        ::WindowsDeleteString(hV);
    }
    hV = nullptr;
    if (SUCCEEDED(spId->get_FamilyName(&hV)) && hV)
    {
        info.sFamilyName = HstrToWs(hV);
        ::WindowsDeleteString(hV);
    }
    AbiWAM::PackageVersion ver = { 0, 0, 0, 0 };
    if (SUCCEEDED(spId->get_Version(&ver)))
    {
        wchar_t sz[64] = { 0 };
        FormatTo(sz, _countof(sz), L"%u.%u.%u.%u",
                 static_cast<unsigned>(ver.Major), static_cast<unsigned>(ver.Minor),
                 static_cast<unsigned>(ver.Build), static_cast<unsigned>(ver.Revision));
        info.sVersion = sz;
    }

    WRL::ComPtr<AbiWS::IStorageFolder> spFolder;
    if (SUCCEEDED(spPkg->get_InstalledLocation(&spFolder)) && spFolder)
    {
        WRL::ComPtr<AbiWS::IStorageItem> spItem;
        if (SUCCEEDED(spFolder.As(&spItem)) && spItem)
        {
            HSTRING hP = nullptr;
            if (SUCCEEDED(spItem->get_Path(&hP)) && hP)
            {
                info.sInstallPath = HstrToWs(hP);
                ::WindowsDeleteString(hP);
            }
        }
    }

    boolean bFw = FALSE;
    if (SUCCEEDED(spPkg->get_IsFramework(&bFw)))
        info.bFramework = (bFw != FALSE);

    // 依赖包 FullName 列表（原第 7 层循环，已抽为 AppxCollectDeps）
    AppxCollectDeps(spPkg, info);
}

bool IsAvailable()
{
    bool bOk = false;
    RunInMta([&]() -> HRESULT
    {
        WRL::ComPtr<AbiWMD::IPackageManager> spPM;
        bOk = CreatePackageManager(&spPM);
        return bOk ? S_OK : E_NOINTERFACE;
    });
    return bOk;
}

bool EnumeratePackages(std::vector<PkgInfo>& vOut, std::wstring* pErr)
{
    HRESULT hrMain = RunInMta([&]() -> HRESULT
    {
        WRL::ComPtr<AbiWMD::IPackageManager> spPM;
        if (!CreatePackageManager(&spPM))
        {
            SetErr(pErr, L"RoActivateInstance(PackageManager) failed");
            return E_NOINTERFACE;
        }

        WRL::ComPtr<AbiWFC::IIterable<AbiWAM::Package*>> spPkgs;
        HRESULT hr = spPM->FindPackages(&spPkgs);
        if (FAILED(hr) || !spPkgs)
        {
            SetErr(pErr, L"IPackageManager::FindPackages failed");
            return hr;
        }

        WRL::ComPtr<AbiWFC::IIterator<AbiWAM::Package*>> spIt;
        hr = spPkgs->First(&spIt);
        if (FAILED(hr)) return hr;

        for (;;)
        {
            boolean bHas = FALSE;
            if (FAILED(spIt->get_HasCurrent(&bHas)) || !bHas) break;

            WRL::ComPtr<AbiWAM::IPackage> spPkg;
            if (SUCCEEDED(spIt->get_Current(spPkg.GetAddressOf())) && spPkg)
            {
                // [N06 扩展] PkgInfo 填充整体抽为 AppxPopulateInfo（原
                //   for(;;)>if>if>if>if>if>if 6 层压平），逐字等价。
                PkgInfo info;
                AppxPopulateInfo(spPkg.Get(), info);

                if (!info.sFullName.empty()) vOut.push_back(std::move(info));
            }

            boolean bNext = FALSE;
            if (FAILED(spIt->MoveNext(&bNext)) || !bNext) break;
        }
        return S_OK;
    });

    if (FAILED(hrMain))
    {
        if (pErr && pErr->empty())
        {
            wchar_t sz[128] = { 0 };
            FormatTo(sz, _countof(sz), L"EnumeratePackages failed 0x%08X", (unsigned)hrMain);
            *pErr = sz;
        }
        return false;
    }
    return true;
}

bool RemovePackage(const std::wstring& sFullName, std::wstring* pErr)
{
    if (sFullName.empty()) { SetErr(pErr, L"RemovePackage: empty full name"); return false; }

    bool bOk = false;
    HRESULT hrMain = RunInMta([&]() -> HRESULT
    {
        WRL::ComPtr<AbiWMD::IPackageManager> spPM;
        if (!CreatePackageManager(&spPM))
        {
            SetErr(pErr, L"RoActivateInstance(PackageManager) failed");
            return E_NOINTERFACE;
        }

        HString hFull;
        if (FAILED(hFull.Set(sFullName.c_str()))) return E_OUTOFMEMORY;

        WRL::ComPtr<DeployOp> spOp;
        HRESULT hr = spPM->RemovePackageAsync(hFull.Get(), &spOp);
        if (FAILED(hr) || !spOp)
        {
            wchar_t sz[160] = { 0 };
            FormatTo(sz, _countof(sz), L"RemovePackageAsync 0x%08X: %s", (unsigned)hr, sFullName.c_str());
            SetErr(pErr, sz);
            return hr;
        }

        HRESULT hrDeploy = S_OK;
        std::wstring sText;
        hr = WaitDeployment(spOp.Get(), &hrDeploy, &sText);
        if (FAILED(hr))
        {
            wchar_t sz[128] = { 0 };
            FormatTo(sz, _countof(sz), L"RemovePackage wait failed 0x%08X", (unsigned)hr);
            SetErr(pErr, sz);
            return hr;
        }
        if (FAILED(hrDeploy))
        {
            constexpr int kErrMsgWchars = 256;   // 256 缓冲尺寸（语义命名，数值不变）
            wchar_t sz[kErrMsgWchars] = { 0 };
            FormatTo(sz, _countof(sz), L"0x%08X %s", (unsigned)hrDeploy, sText.c_str());
            SetErr(pErr, sz);
            return hrDeploy;
        }
        bOk = true;
        return S_OK;
    });

    if (FAILED(hrMain) && pErr && pErr->empty())
    {
        wchar_t sz[128] = { 0 };
        FormatTo(sz, _countof(sz), L"RemovePackage failed 0x%08X", (unsigned)hrMain);
        *pErr = sz;
    }
    return bOk;
}

bool BackupPackage(const std::wstring& sFullName,
                   const std::wstring& sTargetDir,
                   std::wstring* pErr)
{
    if (sFullName.empty() || sTargetDir.empty())
    {
        SetErr(pErr, L"BackupPackage: bad argument");
        return false;
    }

    bool bOk = false;
    std::wstring sSrcPath;

    HRESULT hrMain = RunInMta([&]() -> HRESULT
    {
        WRL::ComPtr<AbiWMD::IPackageManager> spPM;
        if (!CreatePackageManager(&spPM))
        {
            SetErr(pErr, L"RoActivateInstance(PackageManager) failed");
            return E_NOINTERFACE;
        }

        HString hFull;
        if (FAILED(hFull.Set(sFullName.c_str()))) return E_OUTOFMEMORY;

        WRL::ComPtr<AbiWAM::IPackage> spPkg;
        HRESULT hr = spPM->FindPackageByPackageFullName(hFull.Get(), &spPkg);
        if (FAILED(hr) || !spPkg)
        {
            constexpr int kErrMsgWchars = 256;   // 256 缓冲尺寸（语义命名，数值不变）
            wchar_t sz[kErrMsgWchars] = { 0 };
            FormatTo(sz, _countof(sz), L"FindPackageByPackageFullName 0x%08X: %s",
                       (unsigned)hr, sFullName.c_str());
            SetErr(pErr, sz);
            return FAILED(hr) ? hr : E_FAIL;
        }

        WRL::ComPtr<AbiWS::IStorageFolder> spFolder;
        if (FAILED(spPkg->get_InstalledLocation(&spFolder)) || !spFolder)
        {
            SetErr(pErr, L"get_InstalledLocation failed");
            return E_FAIL;
        }
        WRL::ComPtr<AbiWS::IStorageItem> spItem;
        if (FAILED(spFolder.As(&spItem)) || !spItem)
        {
            SetErr(pErr, L"QI IStorageItem failed");
            return E_NOINTERFACE;
        }
        HSTRING hPath = nullptr;
        if (FAILED(spItem->get_Path(&hPath)) || !hPath)
        {
            SetErr(pErr, L"get_Path failed");
            return E_FAIL;
        }
        sSrcPath = HstrToWs(hPath);
        ::WindowsDeleteString(hPath);
        return S_OK;
    });

    if (FAILED(hrMain)) return false;
    if (sSrcPath.empty()) { SetErr(pErr, L"InstalledLocation path empty"); return false; }

    std::wstring sDst = sTargetDir;
    if (sDst.back() == L'\\' || sDst.back() == L'/') sDst.pop_back();
    sDst += L"\\" + sFullName;

    if (!CopyTree(sSrcPath, sDst, pErr)) return false;
    bOk = true;
    return bOk;
}

bool AddPackage(const std::wstring& sPkgFile,
                const std::wstring& sDepDir,
                std::wstring* pErr)
{
    if (sPkgFile.empty()) { SetErr(pErr, L"AddPackage: empty package file"); return false; }
    if (::GetFileAttributesW(sPkgFile.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        SetErr(pErr, L"package file not found: " + sPkgFile);
        return false;
    }

    bool bOk = false;
    HRESULT hrMain = RunInMta([&]() -> HRESULT
    {
        WRL::ComPtr<AbiWMD::IPackageManager> spPM;
        if (!CreatePackageManager(&spPM))
        {
            SetErr(pErr, L"RoActivateInstance(PackageManager) failed");
            return E_NOINTERFACE;
        }

        // 主包 URI
        HString hMainUri;
        if (FAILED(hMainUri.Set(ToFileUri(sPkgFile).c_str()))) return E_OUTOFMEMORY;
        WRL::ComPtr<AbiWF::IUriRuntimeClass> spMainUri;
        HRESULT hr = MakeUri(hMainUri.Get(), &spMainUri);
        if (FAILED(hr)) { SetErr(pErr, L"CreateUri(main) failed"); return hr; }

        // 依赖 URI 向量（IVector<IUriRuntimeClass*>，即 AddPackageAsync 的 IIterable 形参）
        WRL::ComPtr<DepVector> spDeps;
        HString hVecCls;
        if (SUCCEEDED(hVecCls.Set(L"Windows.Foundation.Collections.Vector`1")))
            AbiWF::ActivateInstance(hVecCls.Get(), &spDeps);

        if (!sDepDir.empty() && spDeps)
        {
            WIN32_FIND_DATAW fd = {};
            std::wstring sPat = sDepDir;
            if (sPat.back() != L'\\' && sPat.back() != L'/') sPat += L'\\';
            sPat += L"*";
            ARBFindGuard hFind(::FindFirstFileW(sPat.c_str(), &fd));
            if (hFind != INVALID_HANDLE_VALUE)
            {
                do
                {
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                    std::wstring sName = fd.cFileName;
                    if (!HasPkgExt(sName)) continue;

                    std::wstring sFull = sDepDir;
                    if (sFull.back() != L'\\' && sFull.back() != L'/') sFull += L'\\';
                    sFull += sName;

                    HString hDu;
                    if (FAILED(hDu.Set(ToFileUri(sFull).c_str()))) continue;
                    WRL::ComPtr<AbiWF::IUriRuntimeClass> spDu;
                    if (SUCCEEDED(MakeUri(hDu.Get(), &spDu)) && spDu)
                        spDeps->Append(spDu.Get());
                } while (::FindNextFileW(hFind, &fd));
            }
        }

        WRL::ComPtr<DeployOp> spOp;
        // IVector 与 IIterable 在 ABI 投影里是不同接口，显式 QI 转换；
        // 须使用 MIDL 生成的专用 typedef（裸 IIterable<IUriRuntimeClass*>
        // 未被 MIDL 特化，静态断言不通过）
        WRL::ComPtr<AbiWFC::__FIIterable_1_Windows__CFoundation__CUri_t> spDepIt;
        if (spDeps) spDeps.As(&spDepIt);
        hr = spPM->AddPackageAsync(spMainUri.Get(),
                                   spDepIt.Get(),
                                   AbiWMD::DeploymentOptions_ForceApplicationShutdown,
                                   spOp.GetAddressOf());
        if (FAILED(hr) || !spOp)
        {
            wchar_t sz[160] = { 0 };
            FormatTo(sz, _countof(sz), L"AddPackageAsync 0x%08X: %s", (unsigned)hr, sPkgFile.c_str());
            SetErr(pErr, sz);
            return FAILED(hr) ? hr : E_FAIL;
        }

        HRESULT hrDeploy = S_OK;
        std::wstring sText;
        hr = WaitDeployment(spOp.Get(), &hrDeploy, &sText);
        if (FAILED(hr))
        {
            wchar_t sz[128] = { 0 };
            FormatTo(sz, _countof(sz), L"AddPackage wait failed 0x%08X", (unsigned)hr);
            SetErr(pErr, sz);
            return hr;
        }
        if (FAILED(hrDeploy))
        {
            constexpr int kErrMsgWchars = 512;   // 512 缓冲尺寸（语义命名，数值不变）
            wchar_t sz[kErrMsgWchars] = { 0 };
            FormatTo(sz, _countof(sz), L"0x%08X %s", (unsigned)hrDeploy, sText.c_str());
            SetErr(pErr, sz);
            return hrDeploy;
        }
        bOk = true;
        return S_OK;
    });

    if (FAILED(hrMain) && pErr && pErr->empty())
    {
        wchar_t sz[128] = { 0 };
        FormatTo(sz, _countof(sz), L"AddPackage failed 0x%08X", (unsigned)hrMain);
        *pErr = sz;
    }
    return bOk;
}

bool ReRegisterAllPackages(int* pnOk, int* pnTotal, std::wstring* pErr)
{
    std::vector<PkgInfo> vPkgs;
    std::wstring sEnumErr;
    if (!EnumeratePackages(vPkgs, &sEnumErr))
    {
        SetErr(pErr, L"enumerate failed: " + sEnumErr);
        return false;
    }
    if (pnTotal) *pnTotal = static_cast<int>(vPkgs.size());

    int nOk = 0;
    HRESULT hrMain = RunInMta([&]() -> HRESULT
    {
        WRL::ComPtr<AbiWMD::IPackageManager> spPM;
        if (!CreatePackageManager(&spPM))
        {
            SetErr(pErr, L"RoActivateInstance(PackageManager) failed");
            return E_NOINTERFACE;
        }

        for (size_t k = 0; k < vPkgs.size(); ++k)
        {
            const std::wstring& sPath = vPkgs[k].sInstallPath;
            if (sPath.empty()) continue;

            std::wstring sManifest = sPath + L"\\AppxManifest.xml";
            if (::GetFileAttributesW(sManifest.c_str()) == INVALID_FILE_ATTRIBUTES)
                continue;

            HString hUri;
            if (FAILED(hUri.Set(ToFileUri(sManifest).c_str()))) continue;
            WRL::ComPtr<AbiWF::IUriRuntimeClass> spUri;
            if (FAILED(MakeUri(hUri.Get(), &spUri)) || !spUri) continue;

            WRL::ComPtr<DeployOp> spOp;
            HRESULT hr = spPM->RegisterPackageAsync(spUri.Get(), nullptr,
                                                   AbiWMD::DeploymentOptions_None,
                                                   spOp.GetAddressOf());
            if (FAILED(hr) || !spOp) continue;

            HRESULT hrDeploy = S_OK;
            std::wstring sText;
            if (SUCCEEDED(WaitDeployment(spOp.Get(), &hrDeploy, &sText))
                && SUCCEEDED(hrDeploy))
            {
                ++nOk;
            }
        }
        return S_OK;
    });

    if (pnOk) *pnOk = nOk;
    if (FAILED(hrMain))
    {
        if (pErr && pErr->empty()) SetErr(pErr, L"ReRegisterAllPackages failed");
        return false;
    }
    return true;
}

bool RemoveBadPackages(int* pnRemoved, int* pnTotal, std::wstring* pErr)
{
    int nTotal = 0;
    int nRemoved = 0;

    HRESULT hrMain = RunInMta([&]() -> HRESULT
    {
        WRL::ComPtr<AbiWMD::IPackageManager> spPM;
        if (!CreatePackageManager(&spPM))
        {
            SetErr(pErr, L"RoActivateInstance(PackageManager) failed");
            return E_NOINTERFACE;
        }

        WRL::ComPtr<AbiWFC::IIterable<AbiWAM::Package*>> spPkgs;
        HRESULT hr = spPM->FindPackages(&spPkgs);
        if (FAILED(hr) || !spPkgs) return hr;

        WRL::ComPtr<AbiWFC::IIterator<AbiWAM::Package*>> spIt;
        if (FAILED(spPkgs->First(&spIt))) return E_FAIL;

        for (;;)
        {
            boolean bHas = FALSE;
            if (FAILED(spIt->get_HasCurrent(&bHas)) || !bHas) break;

            WRL::ComPtr<AbiWAM::IPackage> spPkg;
            bool bBad = false;
            std::wstring sFull;

            if (SUCCEEDED(spIt->get_Current(spPkg.GetAddressOf())) && spPkg)
            {
                // IPackage3.Status (PackageStatus) → VerifyIsOK
                WRL::ComPtr<AbiWAM::IPackage3> spPkg3;
                WRL::ComPtr<AbiWAM::IPackageStatus> spStatus;
                if (SUCCEEDED(spPkg.As(&spPkg3)) && spPkg3 &&
                    SUCCEEDED(spPkg3->get_Status(&spStatus)) && spStatus)
                {
                    boolean bOk = TRUE;
                    if (SUCCEEDED(spStatus->VerifyIsOK(&bOk)) && !bOk)
                    {
                        bBad = true;
                        // [N06] FullName 提取抽为独立函数压平 6 层嵌套
                        AppxGetBadPkgFullName(spPkg.Get(), sFull);
                    }
                }
            }

            if (bBad && !sFull.empty())
            {
                ++nTotal;
                std::wstring sRmErr;
                if (RemovePackage(sFull, &sRmErr)) ++nRemoved;
            }

            boolean bNext = FALSE;
            if (FAILED(spIt->MoveNext(&bNext)) || !bNext) break;
        }
        return S_OK;
    });

    if (pnRemoved) *pnRemoved = nRemoved;
    if (pnTotal) *pnTotal = nTotal;
    if (FAILED(hrMain))
    {
        if (pErr && pErr->empty()) SetErr(pErr, L"RemoveBadPackages failed");
        return false;
    }
    return true;
}

// [N06] FullName 提取抽为独立函数（原 if>if>if>if>if>if 6 层压平）。
//       逐字搬迁：包状态校验失败时取 Id.FullName 记录 sFull。
//       定义位于 AppxCollectDeps 之后（供 EnumeratePackages/RemoveBadPackages 前置引用）。
} // namespace AppxRuntimeBridge
// [C++17 R3] 本文件内联常量（C++17 inline variables，多 TU ODR 一致）：
