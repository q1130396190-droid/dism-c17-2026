#include "stdafx.h"
#include "ThemeTokens.h"   // [R203] 颜色令牌（深色/浅色跟随）
// [R16][T10] 工具箱弹窗按 kind 拆分的渲染器分组文件（自 ToolDialogPage.cpp 物理搬迁，
// 零行为改动）。公共壳（Chrome/通用控件工厂/消息路由/作业框架）仍在 ToolDialogPage.cpp；
// 跨组共享的内部设施（tdlg:: 作业 Sink/进度窗/行单元格助手）见 ToolDlgInternal.h。
#include "ToolDialogPage.h"
#include "ToolDlgInternal.h"
#include "WinHandles.h"   // [H-51] win::FindDataHandle RAII for F11_raw_handle_close
#include "SafeThread.h"      // 工作线程异常边界
#include "BackgroundJobs.h"  // 后台作业统一收口（SEH 边界+退出等待）
// [R6] Service 层（零 UI 业务）
#include "ToolService/IToolServiceHost.h"
#include "ToolService/ToolServiceFactory.h"
#include "ToolService/LogDepGraphService.h"
#include "ToolService/ListService.h"
#include "ToolService/ConfirmService.h"
#include "ToolService/IsoMount.h"          // ISO 直连挂载原语（virtdisk）
#include "ToolService/IIsoImageService.h"  // wimgapi 作业工厂
#include "ToolService/WizDiskPart.h"       // diskpart/ESP 原语
#include "ToolService/IWizardService.h"    // 向导业务（BootFix…）
#include "DependencyGraphService.h"
#include "StringManager.h"
#include "HostInterfaces.h"   // static_cast<int>(ELogLevel::LOG_ERROR)/static_cast<int>(ELogLevel::LOG_WARN) 分级常量
#include "SystemInfoService.h"
#include <algorithm>
#include <set>
#include <cwchar>
#include <cwctype>
#include <lm.h>
#include <cstdio>
#include <string>
#include <shlwapi.h>
#include <thread>
#include <winioctl.h>
using i18n::CStringManager;
#pragma comment(lib, "netapi32.lib")
void CToolDialogPage::ClickIso(const CDuiString& sName)
{
    if (sName == L"btn_iso_src") IsoBrowseSrc();
    else if (sName == L"btn_iso_iso") IsoBrowseIso();
    else if (sName == L"btn_iso_ok") IsoOk();
}
void CToolDialogPage::ClickBootFix(const CDuiString& sName)
{
    if (sName.Left(8) == L"gridrow_") OnGridRow(sName);
    else if (sName == L"btn_bootfix_ok") BootFixOk();
    else if (sName == L"btn_bootfix_basic") BootFixToggleBasic();
}



// ------------------------------------------------------------
// 注册表工具
// ------------------------------------------------------------
void CToolDialogPage::BuildIso()
{
    CControlUI* pTop = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pTop->SetFixedHeight(24);
    m_pBody->Add(pTop);



    CVerticalLayoutUI* pBox = std::make_unique<CVerticalLayoutUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pBox->SetFixedHeight(126);
    pBox->SetAttribute(L"bordercolor", tokens::TokenHex(tokens::EToken::Border));
    pBox->SetAttribute(L"bordersize", L"1");
    pBox->SetBkColor(tokens::Token(tokens::EToken::Surface));
    pBox->SetAttribute(L"inset", L"18,18,18,18");



    CHorizontalLayoutUI* pRow1 = std::make_unique<CHorizontalLayoutUI>().release(); // [C++17 R3] framework-owned via pBox->Add
    pRow1->SetFixedHeight(38);
    pRow1->Add(MkEdit(L"edt_iso_src"));
    CControlUI* pG1 = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via pRow1->Add
    pG1->SetFixedWidth(12);
    pRow1->Add(pG1);
    CButtonUI* pB1 = MkBtn(StringMgr.Get("general.browse"), L"btn_iso_src", false);
    pB1->SetFixedWidth(104);
    pRow1->Add(pB1);
    pBox->Add(pRow1);



    CControlUI* pS = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via pBox->Add
    pS->SetFixedHeight(14);
    pBox->Add(pS);



    CHorizontalLayoutUI* pRow2 = std::make_unique<CHorizontalLayoutUI>().release(); // [C++17 R3] framework-owned via pBox->Add
    pRow2->SetFixedHeight(38);
    pRow2->Add(MkEdit(L"edt_iso_iso"));
    CControlUI* pG2 = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via pRow2->Add
    pG2->SetFixedWidth(12);
    pRow2->Add(pG2);
    CButtonUI* pB2 = MkBtn(StringMgr.Get("general.browse"), L"btn_iso_iso", false);
    pB2->SetFixedWidth(104);
    pRow2->Add(pB2);
    pBox->Add(pRow2);



    m_pBody->Add(pBox);



    // 弹性间隔（最小 18）：窗口变高时标签与底部按钮整体保持合理位置
    CControlUI* pGapL = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pGapL->SetMinHeight(18);
    m_pBody->Add(pGapL);



    m_pBody->Add(MkEdit(L"edt_iso_label"));



    AddBottomRow(MkBtn(StringMgr.Get("general.ok"), L"btn_iso_ok", true),
                 MkBtn(StringMgr.Get("general.cancel"), L"btn_cancel", false));
}



// [R-B11] 前置声明：ISO 内 install.wim/esd 自动识别
// ====================================================================
// [R-B54] ISO 内 install.wim/esd 双架构识别
// 清单 04 缺失 3：挂载 ISO 后按 sources\install.(wim|esd) 枚举，
//               并用 WIMGetImageCount + WIMGetImageInformation 判架构。
// 说明：旧实现只读前 64 字节手工解析 WIM 头，既不支持 .iso 文件本身，
//       也从未调用 wimgapi 判架构，故整体重写。
// ====================================================================
// 架构值 -> 名称（微软 WIM_XML <ARCH> 值：0=x86 9=amd64 12=arm64 14=arm64ec）
// [R7] IsoArchName/IsoParseArchFromXml/IsoProbeWimArchitectures 已下沉 IIsoImageService::ProbeWimArchitectures。
// [R6-04] ISO 直连挂载原语（virtdisk.dll）已下沉 ToolService/IsoMount.cpp；
// 这里保留 3 个 CDuiString 薄委托，使既有调用点零改动（R6-05 后随向导移除）。
// [C++17 R3] [[nodiscard]]：返回值勿忽略
[[nodiscard]] static CDuiString IsoMountIsoFile(const CDuiString& sIso)
{
    return CDuiString(toolsvc::IsoMount(sIso.GetData()).c_str());
}
static void IsoDismountIsoFile(const CDuiString& sIso)
{
    toolsvc::IsoDismount(sIso.GetData());
}
// [C++17 R3] [[nodiscard]]：返回值勿忽略
[[nodiscard]] static CDuiString IsoResolveInnerImage(const CDuiString& sMount)
{
    return CDuiString(toolsvc::IsoResolveInner(sMount.GetData()).c_str());
}
static void ReportIsoInstallImage(toolsvc::IIsoImageService* pSvc,
                                  const CDuiString& sSrc);


void CToolDialogPage::IsoBrowseSrc()
{
    CDuiString sDir;
    if (!m_pHost->HostBrowseFolder(sDir, StringMgr.Get("misc.sc413f1c8")))
        return;
    CEditUI* pE = static_cast<CEditUI*>(m_pm.FindControl(L"edt_iso_src"));
    if (pE) pE->SetText(sDir.GetData());
    // [R-B11] ISO 内 install.wim/esd 自动识别 + 架构探测
    ReportIsoInstallImage(m_pImgSvc.get(), sDir);
}



void CToolDialogPage::IsoBrowseIso()
{
    CDuiString sFile;
    if (!m_pHost->HostSaveFile(
            sFile, StringMgr.Get("toolbox.s3e21c064"),
            StringMgr.Get("edit.s310946f7"), L"image.iso")) return;
    CEditUI* pE = static_cast<CEditUI*>(m_pm.FindControl(L"edt_iso_iso"));
    if (pE) pE->SetText(sFile.GetData());
}



// ====================================================================
// [R-B11] ISO 内 install.wim/esd 自动识别 + 架构探测
// 方法：选中 ISO 源目录后，探测 sources\install.wim / install.esd；
// 命中则读 WIM 文件头中的 Architecture 字段并报告。
// ====================================================================
// [R-B54] 主入口：目录或 .iso 文件，均按 sources\install.(wim|esd) 探测
//         并调用 wimgapi 判架构集合（WIMGetImageCount / WIMGetImageInformation）。
// 辅助函数 IsoArchName/IsoParseArchFromXml/IsoProbeWimArchitectures/
//        IsoMountIsoFile/IsoDismountIsoFile 见前置声明之前。
static void ReportIsoInstallImage(toolsvc::IIsoImageService* pSvc,
                                  const CDuiString& sSrc)
{
    if (sSrc.IsEmpty() || !pSvc) return;
    // 判定输入是 .iso 文件还是目录
    DWORD dwAttr = ::GetFileAttributesW(sSrc.GetData());
    if (dwAttr == INVALID_FILE_ATTRIBUTES) return;
    CDuiString sIso;      // 若非空表示需要挂载
    CDuiString sBase = sSrc;
    if (dwAttr & FILE_ATTRIBUTE_DIRECTORY) {
        // 目录：直接在其下找 install.wim/esd
        if (sBase[sBase.GetLength() - 1] != L'\\') sBase += L"\\";
    } else {
        // .iso 文件：需先挂载；记录原 ISO 供卸载
        sIso = sSrc;
        sBase = IsoMountIsoFile(sIso);
        if (sBase.IsEmpty()) {
            CDuiString sMsg;
            sMsg.Format(StringMgr.Get("misc.hiso_mountfail"), sIso.GetData());
            ::MessageBoxW(::GetActiveWindow(), sMsg.GetData(),
                          StringMgr.Get("toolbox.isogen"),
                          MB_OK | MB_ICONWARNING);
            return;
        }
    }
    // [R-B70] 知识库 06_镜像处理专题.md L121-130：双架构 ISO 中安装映像
    // 还可能位于 x86\sources 与 x64\sources 子目录，候选全集 8 项。
    CDuiString sCandidates[] = {
        sBase + L"sources\\install.wim",
        sBase + L"sources\\install.esd",
        sBase + L"x64\\sources\\install.wim",
        sBase + L"x64\\sources\\install.esd",
        sBase + L"x86\\sources\\install.wim",
        sBase + L"x86\\sources\\install.esd",
        sBase + L"install.wim",
        sBase + L"install.esd",
    };
    // [C++17 R3] 块级 constexpr：作用域限于函数内（命名空间级 inline constexpr 见文件尾）
constexpr int nCount = static_cast<int>(sizeof(sCandidates) / sizeof(sCandidates[0]));
    CDuiString sHit;
    for (int i = 0; i < nCount; ++i) {
        if (::GetFileAttributesW(sCandidates[i].GetData()) != INVALID_FILE_ATTRIBUTES) {
            sHit = sCandidates[i];
            break;
        }
    }
    // 无论命中与否都先卸载临时挂载的 ISO
    if (!sIso.IsEmpty()) IsoDismountIsoFile(sIso);
    if (sHit.IsEmpty()) return;
    // [R7] 架构探测下沉 IIsoImageService::ProbeWimArchitectures
    std::wstring sArchW;
    uint32_t dwImageCount = 0, dwArchFound = 0;
    bool bProbe = pSvc->ProbeWimArchitectures(sHit.GetData(),
                                              sArchW, dwImageCount, dwArchFound);
    CDuiString sArch = sArchW.c_str();
    CDuiString sMsg;
    if (bProbe) {
        // 命中文件 + 映像数 + 架构集合
        sMsg.Format(StringMgr.Get("misc.hiso_detect2"),
                    sHit.GetData(), static_cast<int>(dwImageCount),
                    sArch.GetData(), static_cast<int>(dwArchFound));
    } else {
        sMsg.Format(StringMgr.Get("misc.hiso_detect"),
                    sHit.GetData(), static_cast<int>(dwImageCount), static_cast<int>(0));
    }
    ::MessageBoxW(::GetActiveWindow(), sMsg.GetData(),
                  StringMgr.Get("toolbox.isogen"), MB_OK | MB_ICONINFORMATION);
}


// ====================================================================
// P1-3 ISO 生成器增强：ISO9660 长度预检 + 引导方式自动检测
// 说明：不改 UI 布局，只在 Ok 逻辑前置预检，并自动拼 -b / -bootdata
// ====================================================================
// [R16][T10] 自 ScanIso9660Paths 机械抽取：单条目 ISO9660 检查。
// 原循环体中的 continue（跳过当前条目）在本助手中等价改写为 return。
static void ScanIso9660Paths(const CDuiString& sDir, const CDuiString& sBase, int nDepth,
                             std::vector<CDuiString>& out);
static void ScanIso9660Entry(const WIN32_FIND_DATAW& fd, const CDuiString& sDir,
                             const CDuiString& sBase, int nDepth, std::vector<CDuiString>& out)
{
        CDuiString sName = fd.cFileName;
        if (sName == L"." || sName == L"..") return;



        CDuiString sFull = sDir;
        if (sFull.IsEmpty() || sFull[sFull.GetLength() - 1] != L'\\')
            sFull += L"\\";
        sFull += sName;



        // 单段 > 31 字符：ISO9660 Level 1/2 每段限 31（-J/Joliet 例外，
        // 但 MSFT 官方文档仍建议避免，跨工具兼容性差）
        if (sName.GetLength() > 31)
        {
            CDuiString sItem = sBase.IsEmpty() ? sName : (sBase + L"\\" + sName);
            if (out.size() < 200)
                out.push_back(CDuiString(StringMgr.Get("misc.h669b")) + sItem);
        }



        // 8.3 检查：若名字 > 8 或 > 8.3 且没有合法短名，可能读不出（无 Joliet 时）
        int nDot = sName.ReverseFind(L'.');
        bool bLong = false;
        if (nDot <= 0) {
            bLong = sName.GetLength() > 8;
        } else {
            bLong = (nDot > 8) || (sName.GetLength() - nDot - 1 > 3);
        }
        if (bLong)
        {
            wchar_t szShort[MAX_PATH] = {0};
            DWORD dw = ::GetShortPathNameW(sFull.GetData(), szShort, MAX_PATH);
            bool bHasShort = (dw > 0 && dw < MAX_PATH && szShort[0] != L'\0');
            if (!bHasShort)
            {
                CDuiString sItem = sBase.IsEmpty() ? sName : (sBase + L"\\" + sName);
                if (out.size() < 200)
                    out.push_back(CDuiString(StringMgr.Get("misc.hcce2")) + sItem);
            }
        }



        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            && nDepth < 10)
        {
            CDuiString sSubBase = sBase.IsEmpty() ? sName : (sBase + L"\\" + sName);
            ScanIso9660Paths(sFull, sSubBase, nDepth + 1, out);
        }
}
static void ScanIso9660Paths(const CDuiString& sDir, const CDuiString& sBase, int nDepth,
                             std::vector<CDuiString>& out)
{
    if (nDepth > 10) return;
    if (out.size() >= 200) return;



    CDuiString sSearch = sDir;
    if (sSearch.IsEmpty() || sSearch[sSearch.GetLength() - 1] != L'\\')
        sSearch += L"\\";
    sSearch += L"*";



    WIN32_FIND_DATAW fd;
    win::FindDataHandle h = win::find_handle(::FindFirstFileW(sSearch.GetData(), &fd)); // [C++17 R11] RAII FindClose
    if (!h) return;
    do {
        ScanIso9660Entry(fd, sDir, sBase, nDepth, out);
    } while (::FindNextFileW(h.get(), &fd));
    // [C++17 R11] FindClose runs at scope exit (was the last statement here)
}



// ============================================================================
//  [2026-10-08] ISO生成器 内置化（知识库 40 §2.1 行10）
// ============================================================================
// [R16-T9] CIsoJobSink 已删除：ISO 生成完成改用统一 CJobDoneSink
// （BackgroundJobs.h），文案经 Cfg 表达，消息号收敛为 JOB_WM_DONE。
// [rb28 2026-10-13] ISO 生成等待窗已改用 tdlg::CTpProgressWnd（带真实百分比），
// IsoWriter 按写入字节回调 WM_TP_PROGRESS，旧纯 STATIC IsoJobCreateWait 移除。
// 探测 BIOS/UEFI 引导映像路径（等价 BuildIsoBootArg 的候选链，输出路径）
static void IsoJobDetectBoot(const CDuiString& sSrc,
                             std::wstring& wEtfs, std::wstring& wEfisys)
{
    wchar_t szWin[MAX_PATH] = { 0 };
    ::GetWindowsDirectoryW(szWin, MAX_PATH);
    CDuiString sWin = szWin;
    // 随包 ADK 可再发行件：主程序目录 amd64\ 优先，兼容旧扁平布局
    CDuiString sShipEtfs, sShipEfi;
    {
        wchar_t szExeDir[MAX_PATH] = { 0 };
        ::GetModuleFileNameW(nullptr, szExeDir, MAX_PATH);
        ::PathRemoveFileSpecW(szExeDir);
        const wchar_t* kEtfsRel[] = {
            L"\\amd64\\etfsboot.com", L"\\etfsboot.com",
            L"\\bin64\\etfsboot.com" };
        for (int i = 0; i < 3; ++i)
        {
            sShipEtfs.Format(L"%s%s", szExeDir, kEtfsRel[i]);
            if (::GetFileAttributesW(sShipEtfs.GetData()) != INVALID_FILE_ATTRIBUTES)
                break;
            sShipEtfs.Empty();
        }
        const wchar_t* kEfiRel[] = {
            L"\\amd64\\efisys.bin", L"\\efisys.bin",
            L"\\bin64\\efisys.bin" };
        for (int i = 0; i < 3; ++i)
        {
            sShipEfi.Format(L"%s%s", szExeDir, kEfiRel[i]);
            if (::GetFileAttributesW(sShipEfi.GetData()) != INVALID_FILE_ATTRIBUTES)
                break;
            sShipEfi.Empty();
        }
    }
    CDuiString aEtfs[] = {
        sShipEtfs,
        [](const wchar_t* p)->CDuiString{ CDuiString s; s.Format(L"%s\\Boot\\DVD\\PCAT\\etfsboot.com", p); return s; }(sWin.GetData()),
        L"C:\\Program Files (x86)\\Windows Kits\\10\\Assessment and Deployment Kit\\Deployment Tools\\amd64\\Oscdimg\\etfsboot.com",
        L"C:\\Program Files (x86)\\Windows Kits\\10\\Assessment and Deployment Kit\\Deployment Tools\\x86\\Oscdimg\\etfsboot.com",
        L"C:\\Program Files (x86)\\Windows Kits\\10\\Assessment and Deployment Kit\\Deployment Tools\\amd64\\Boot\\etfsboot.com"
    };
    for (int i=0;i<5;++i)
        if (!aEtfs[i].IsEmpty() &&
            ::GetFileAttributesW(aEtfs[i].GetData()) != INVALID_FILE_ATTRIBUTES)
        { wEtfs = aEtfs[i].GetData(); break; }
    CDuiString aEfi[] = {
        sShipEfi,
        [](const wchar_t* p)->CDuiString{ CDuiString s; s.Format(L"%s\\Boot\\DVD\\EFI\\en-US\\efisys.bin", p); return s; }(sWin.GetData()),
        L"C:\\Program Files (x86)\\Windows Kits\\10\\Assessment and Deployment Kit\\Deployment Tools\\amd64\\Oscdimg\\efisys.bin",
        L"C:\\Program Files (x86)\\Windows Kits\\10\\Assessment and Deployment Kit\\Deployment Tools\\amd64\\Boot\\efisys.bin"
    };
    for (int i=0;i<4;++i)
        if (!aEfi[i].IsEmpty() &&
            ::GetFileAttributesW(aEfi[i].GetData()) != INVALID_FILE_ATTRIBUTES)
        { wEfisys = aEfi[i].GetData(); break; }
    // 仅当源目录自带 efi 子目录时启用 UEFI 引导项
    if (!wEfisys.empty())
    {
        CDuiString sEfiDir = sSrc;
        if (!sEfiDir.IsEmpty() && sEfiDir[sEfiDir.GetLength()-1] != L'\\')
            sEfiDir += L"\\";
        sEfiDir += L"efi";
        if (::GetFileAttributesW(sEfiDir.GetData()) == INVALID_FILE_ATTRIBUTES)
            wEfisys.clear();
    }
}
// [CRASHFIX] ISO 生成作业上下文
struct TIsoCreateJob
{
    toolsvc::IIsoImageService::TIsoCreateArgs a;  // [R7] 组装参数（服务层）
    // [FIX-S0-1] 非 owning 裸指针 → shared_ptr 拥有型副本：IsoOk 启动作业后
    // 立即 Close(IDOK)，弹窗析构 m_pImgSvc.reset() 会在作业写盘中释放服务，
    // 工作线程随后解引用已释放对象 = UAF/堆损坏（随机崩溃）。作业持副本后
    // 服务生命周期与弹窗解耦（同 ESD 解密 [R16][T2] 做法）。
    std::shared_ptr<toolsvc::IIsoImageService> spSvc;
    HWND         hWait;
    HWND         hSink;
};
// [R-TAILFIX 同型] 作业体 SEH 边界：CreateIsoImage 内含文件遍历/压缩原语，
// 硬件异常若穿透线程过程，JOB_WM_DONE 永不投递，进度窗与作业登记永久悬挂。
// 本函数无 C++ 对象，允许 __try；异常降级为失败，完成信封照常回投。
static void IsoCreateJobSehBody(toolsvc::IIsoImageService* pSvc,
                                const toolsvc::IIsoImageService::TIsoCreateArgs& a,
                                toolsvc::IToolJobSink* pSink,
                                bool& bOk, std::wstring& sErr)
{
    __try
    {
        bOk = pSvc && pSvc->CreateIsoImage(a, pSink, sErr);
    }
    __except (CrashGuardFilter(GetExceptionInformation(), "iso-create-seh"))
    {
        bOk = false;
        if (sErr.empty()) sErr = L"ISO 生成发生硬件异常（已兜底收口）";
    }
}

// [CRASHFIX] ISO 生成线程：IsoWriter 原语下沉 IIsoImageService::CreateIsoImage
// （内含双层异常边界）；本过程仅进度/完成消息编排，契约保持不变。
static DWORD WINAPI IsoCreateThreadProc(void* pv)
{
    std::unique_ptr<TIsoCreateJob> guard(reinterpret_cast<TIsoCreateJob*>(pv)); // [C++17 R3] ownership from Start caller
    TIsoCreateJob* j = guard.get();
    HWND hWait = j->hWait;
    HWND hSink = j->hSink;
    struct TIsoSink : toolsvc::IToolJobSink
    {
        HWND hWait = nullptr;
        void OnProgress(int nPercent, const std::wstring&) override
        {
            if (hWait && ::IsWindow(hWait))
                ::PostMessageW(hWait, WM_TP_PROGRESS, static_cast<WPARAM>(nPercent), 0);
        }
        void OnLog(int, const std::wstring&) override {}
        void OnStage(const std::wstring&) override {}
    } sink;
    sink.hWait = hWait;
    std::wstring sTaskErr;
    bool bTaskOk = false;
    if (j->spSvc) IsoCreateJobSehBody(j->spSvc.get(), j->a, &sink, bTaskOk, sTaskErr);
    if (!j->spSvc) sTaskErr = L"内部错误：映像服务未初始化。";
    // [R16-T9] 统一信封投递（nKind=3：ISO 生成，弹窗侧作业）
    // [C++17 R3] helper PostHeapOrDelete always takes ownership (posts or deletes in place)
    TJobDoneEnvelope* pEnv = std::make_unique<TJobDoneEnvelope>().release();
    pEnv->nKind = 3;
    pEnv->bOk   = bTaskOk;
    if (!bTaskOk) pEnv->sMsg = sTaskErr;
    if (hWait && ::IsWindow(hWait)) ::PostMessageW(hWait, WM_CLOSE, 0, 0);
    // [GC-07] 统一收尾：投递失败由助手就地回收，不再各写一套
    BackgroundJobs::PostHeapOrDelete(hSink, JOB_WM_DONE, 0, pEnv);
    // [C++17 R3] thread ctx deleted by guard scope exit (ownership from Start caller)
    return 0;
}
void CToolDialogPage::IsoOk()
{
    CEditUI* pSrc = static_cast<CEditUI*>(m_pm.FindControl(L"edt_iso_src"));
    CEditUI* pIso = static_cast<CEditUI*>(m_pm.FindControl(L"edt_iso_iso"));
    CEditUI* pLabel = static_cast<CEditUI*>(m_pm.FindControl(L"edt_iso_label"));



    CDuiString sSrc = pSrc ? pSrc->GetText() : CDuiString(L"");
    CDuiString sIso = pIso ? pIso->GetText() : CDuiString(L"");
    CDuiString sLabel = pLabel ? pLabel->GetText() : CDuiString(L"");
    sSrc.Trim();
    sIso.Trim();
    sLabel.Trim();



    if (sSrc.IsEmpty() || sIso.IsEmpty())
    {
        ::MessageBoxW(m_hWnd, StringMgr.Get("misc.sa639f338"),
                      StringMgr.Get("toolbox.isogen"), MB_OK | MB_ICONWARNING);
        return;
    }



    // [2026-10-08] 不再要求 ADK oscdimg：IsoWriter 全内置
    sLabel.Replace(L"\"", L"");
    if (sLabel.GetLength() > 32) sLabel = sLabel.Left(32);
    if (sLabel.IsEmpty()) sLabel = L"ISO_VOL";



    // [R16][T10] ISO9660 长度预检机械抽取为 IsoPrecheck9660
    if (!IsoPrecheck9660(sSrc)) return;


    // [2026-10-08] 引导映像探测（内置 El Torito）
    std::wstring wEtfs, wEfisys;
    IsoJobDetectBoot(sSrc, wEtfs, wEfisys);
    // ---- 后台内置执行（零 bat/oscdimg）----
    // [rb28] 百分比进度窗：IsoWriter 按实际写入字节自算 done/total
    HWND hWait = tdlg::CTpProgressWnd::Create(m_hWnd,
        CDuiString(L"ISO 生成进行中，正在写入文件数据……"), false);
    // [R16-T9] CIsoJobSink → 统一 CJobDoneSink（文案逐字等价迁入 Cfg）
    CJobDoneSink::Cfg cfg;
    cfg.sOkTitle   = L"ISO 生成完成";
    cfg.sOkMsg     = L"ISO 文件已成功生成。\r\n是否打开所在文件夹？";
    cfg.sFailTitle = L"ISO 生成失败";
    cfg.sFailMsg   = L"ISO 生成失败。";
    cfg.bAskOpen   = true;
    HWND hSink = CJobDoneSink::Create(m_hWnd, std::wstring(sIso.GetData()), cfg);
    // [CRASHFIX] 裸 detach 迁移为受 SEH 边界保护、可被 WM_CLOSE 等待的统一作业
    // [C++17 阶段3] 线程 ctx：所有权移交 IsoCreateThreadProc，Start 失败自删
    auto ctx = std::make_unique<TIsoCreateJob>(); // [C++17 R3] ownership to thread proc on successful start
    TIsoCreateJob* pJob = ctx.get();
    pJob->a.sSrc          = std::wstring(sSrc);
    pJob->a.sOut          = std::wstring(sIso);
    pJob->a.sVolumeLabel  = std::wstring(sLabel);
    pJob->a.sBiosBootFile = wEtfs;
    pJob->a.sUefiBootFile = wEfisys;
    // [FIX-S0-1] 作业持 shared_ptr 副本：Close(IDOK) 析构弹窗后服务仍存活
    pJob->spSvc          = m_pImgSvc;
    pJob->hWait = hWait;
    pJob->hSink = hSink;
    if (!BackgroundJobs::Start(&IsoCreateThreadProc, ctx.get(), "iso-create"))
    {
        if (hWait && ::IsWindow(hWait)) ::PostMessageW(hWait, WM_CLOSE, 0, 0);
        ::MessageBoxW(m_hWnd, L"后台作业启动失败（线程资源不足），ISO 生成已中止。",
                      L"ISO 生成器", MB_OK | MB_ICONERROR);
        ctx.reset(); // [C++17 R3] delete holder at the original failure-cleanup site
    }
    else
    {
        ctx.release(); // [C++17 R3] thread proc deletes via its unique_ptr guard
    }
    Close(IDOK);
}
// [R16][T10] 自 IsoOk 机械抽取：ISO9660 长度预检（用户放弃继续返回 false）
bool CToolDialogPage::IsoPrecheck9660(const CDuiString& sSrc)
{
    // P1-3a: ISO9660 长度预检
    std::vector<CDuiString> vIssues;
    if (sSrc.GetLength() > 65535)
    {
        vIssues.push_back(CDuiString(sSrc).Format(StringMgr.Get("misc.hc398"), sSrc.GetLength()));
    }
    if (::GetFileAttributesW(sSrc.GetData()) != INVALID_FILE_ATTRIBUTES)
        ScanIso9660Paths(sSrc, CDuiString(), 0, vIssues);
    if (!vIssues.empty())
    {
        CDuiString sMsg = StringMgr.Get("misc.h62e3");
        for (size_t i = 0; i < vIssues.size() && i < 20; ++i)
            sMsg += vIssues[i] + L"\r\n";
        if (vIssues.size() > 20)
        {
            CDuiString sMore;
            sMore.Format(StringMgr.Get("misc.hde78"), static_cast<int>(vIssues.size() - 20));
            sMsg += sMore;
        }
        sMsg += StringMgr.Get("misc.h083f");
        if (::MessageBoxW(m_hWnd, sMsg.GetData(),
                          StringMgr.Get("misc.h8682"),
                          MB_YESNO | MB_ICONWARNING) != IDYES)
            return false;
    }
    return true;
}





// ============================================================
// 详细设置（对应原版 DLGTEMPLATEEX ID133，含 5 选项）
//   . 专家模式          1009
//   . 加载 WofAdk       1006
//   . 右键菜单          1007
//   . 整合 RE           1008
//   . 差异传输          1011
// 选项状态持久化到 exe 同目录 config.ini 的 [Advanced] 段；
// 打开时读取，确定时写回，取消不落盘。
// ============================================================
bool CToolDialogPage::EnumerateBfParts()
{
    // [R6-05b] 枚举/排序/推荐下沉 IWizardService；Page 仅 wstring→CDuiString
    std::vector<toolsvc::TBfPart> vZ;
    int nRec = -1;
    m_pWizSvc->BfEnumerateParts(vZ, nRec);
    m_bf.vParts.clear();
    for (size_t i = 0; i < vZ.size(); ++i)
    {
        const toolsvc::TBfPart& z = vZ[i];
        TBfPart pt;
        pt.sPos        = z.sPos.c_str();
        pt.sName       = z.sName.c_str();
        pt.sSize       = z.sSize.c_str();
        pt.sType       = z.sType.c_str();
        pt.sVolumeGuid = z.sVolumeGuid.c_str();
        pt.sFs         = z.sFs.c_str();
        pt.dwDisk      = static_cast<DWORD>(z.dwDisk);
        pt.dwPart      = static_cast<DWORD>(z.dwPart);
        pt.bEfi        = z.bEfi;
        pt.bMsr        = z.bMsr;
        pt.bPrimary    = z.bPrimary;
        pt.bActive     = z.bActive;
        m_bf.vParts.push_back(pt);
    }
    m_bf.nSel = nRec;
    return !m_bf.vParts.empty();
}
void CToolDialogPage::BuildBootFix()
{
    m_bf.nSel = -1;
    m_bf.bBasic = false;
    m_bf.pTarget = nullptr;
    m_bf.pBasicBtn = nullptr;
    m_bf.pGrid = nullptr;
    m_bf.sWork = SystemInfoService::MakeTempWorkDir(L"bootfix");
    if (!EnumerateBfParts())
        m_pHost->HostAlert(L"引导修复",
            L"未能枚举到磁盘分区，请确认以管理员身份运行。", MB_ICONWARNING);
    // 第一行：目标分区（随选中刷新）
    m_bf.pTarget = MkText(L"", 1, 0xFF1A1A1A);
    m_bf.pTarget->SetFixedHeight(30);
    m_bf.pTarget->SetAttribute(L"endellipsis", L"true");
    m_pBody->Add(m_bf.pTarget);
    // 第二行
    CTextUI* pTip = MkText(L"没有异议或者看不懂的请点击 确定。",
                           1, 0xFF1A1A1A);
    pTip->SetFixedHeight(30);
    m_pBody->Add(pTip);
    CControlUI* pGap = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pGap->SetFixedHeight(12);
    m_pBody->Add(pGap);
    // 分区表：位置 / 名称 / 大小 / 类型
    int n = static_cast<int>(m_bf.vParts.size());
    std::vector<TGridCol> cols;
    cols.push_back(TGridCol(L"位置", 250));
    cols.push_back(TGridCol(L"名称", 160));
    cols.push_back(TGridCol(L"大小", 110));
    cols.push_back(TGridCol(L"类型", -1));
    m_bf.pGrid = MkGrid(cols, n > 0 ? n : 1, 32, true);
    int nVis = n > 0 ? (n < 7 ? n : 7) : 1;
    m_bf.pGrid->SetFixedHeight(34 + nVis * 32 + 2);
    m_pBody->Add(m_bf.pGrid);
    for (int r = 0; r < n; ++r)
    {
        tdlg::SetRowCell(m_vRows[r], 0, m_bf.vParts[r].sPos);
        tdlg::SetRowCell(m_vRows[r], 1, m_bf.vParts[r].sName);
        tdlg::SetRowCell(m_vRows[r], 2, m_bf.vParts[r].sSize);
        tdlg::SetRowCell(m_vRows[r], 3, m_bf.vParts[r].sType);
    }
    // 底部按钮：确定 / 取消 / 基本
    CHorizontalLayoutUI* pRow = std::make_unique<CHorizontalLayoutUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pRow->SetFixedHeight(74);
    pRow->SetAttribute(L"inset", L"0,18,0,18");
    CControlUI* pSp = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via pRow->Add
    pRow->Add(pSp);
    CButtonUI* pOk = MkBtn(L"确定", L"btn_bootfix_ok", true);
    pOk->SetFixedWidth(130);
    pRow->Add(pOk);
    CControlUI* pG1 = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via pRow->Add
    pG1->SetFixedWidth(14);
    pRow->Add(pG1);
    CButtonUI* pCancel = MkBtn(L"取消", L"btn_cancel", false);
    pCancel->SetFixedWidth(130);
    pRow->Add(pCancel);
    CControlUI* pG2 = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via pRow->Add
    pG2->SetFixedWidth(14);
    pRow->Add(pG2);
    m_bf.pBasicBtn = MkBtn(L"基本", L"btn_bootfix_basic", false);
    m_bf.pBasicBtn->SetFixedWidth(130);
    pRow->Add(m_bf.pBasicBtn);
    m_pBody->Add(pRow);
    // 默认选中推荐分区并刷新目标描述
    if (m_bf.nSel >= 0) SetGridSelection(m_bf.nSel);
    BfRefreshTarget();
}
void CToolDialogPage::BfRefreshTarget()
{
    if (!m_bf.pTarget) return;
    CDuiString s;
    if (m_bf.nSel >= 0 && m_bf.nSel < static_cast<int>(m_bf.vParts.size()))
        s.Format(L"即将把启动信息写入启动分区- %s",
                 m_bf.vParts[m_bf.nSel].sPos.GetData());
    else
        s = L"即将把启动信息写入启动分区-（未选择）";
    m_bf.pTarget->SetText(s);
}
void CToolDialogPage::BootFixToggleBasic()
{
    m_bf.bBasic = !m_bf.bBasic;
    if (m_bf.bBasic)
    {
        if (m_bf.pGrid) m_bf.pGrid->SetVisible(false);
        if (m_bf.pBasicBtn) m_bf.pBasicBtn->SetText(L"更多");
    }
    else
    {
        if (m_bf.pGrid) m_bf.pGrid->SetVisible(true);
        if (m_bf.pBasicBtn) m_bf.pBasicBtn->SetText(L"基本");
    }
}
void CToolDialogPage::BootFixOk()
{
    if (m_bf.vParts.empty())
    {
        m_pHost->HostAlert(L"引导修复",
            L"没有可操作的磁盘分区。", MB_ICONWARNING);
        return;
    }
    if (m_bf.nSel < 0 || m_bf.nSel >= static_cast<int>(m_bf.vParts.size()))
    {
        m_pHost->HostAlert(L"引导修复",
            L"请先在列表中选择一个启动分区。", MB_ICONWARNING);
        return;
    }
    const TBfPart& pt = m_bf.vParts[m_bf.nSel];
    bool bUefi = SystemInfoService::IsUEFIBoot();
    // 系统映像必须在分区根目录（原版 4496）
    wchar_t szSys[8] = { 0 };
    ::GetEnvironmentVariable(L"SystemDrive", szSys, _countof(szSys));
    CDuiString sWin;
    sWin.Format(L"%s\\Windows", szSys);
    if (::GetFileAttributesW(sWin) == INVALID_FILE_ATTRIBUTES)
    {
        m_pHost->HostAlert(L"引导修复",
            L"此映像并不在分区根目录，所以无法执行修复操作。", MB_ICONWARNING);
        return;
    }
    if (pt.sVolumeGuid.IsEmpty())
    {
        m_pHost->HostAlert(L"引导修复",
            L"所选分区没有可识别的卷，无法写入启动信息。", MB_ICONWARNING);
        return;
    }
    // [R16][T10] 分区/固件匹配校验机械抽取为 BfValidateFirmwareMatch
    if (!BfValidateFirmwareMatch(pt, bUefi)) return;


    // [R6-05h] 盘符解析/临时挂载点/bcdboot 执行/回收全部下沉 IWizardService。
    // 本调用在 UI 线程：sink 仅把日志转给宿主（进度/阶段回调为空操作）。
    struct TBfUiSink : toolsvc::IToolJobSink
    {
        IToolHost* pHost = nullptr;
        void OnProgress(int, const std::wstring&) override {}
        void OnStage(const std::wstring&) override {}
        void OnLog(int nLevel, const std::wstring& sLine) override
        { if (pHost) pHost->HostLog(CDuiString(sLine.c_str()), nLevel); }
    };
    TBfUiSink sink;
    sink.pHost = m_pHost;
    toolsvc::TBfPart z;
    z.sPos        = pt.sPos.GetData();
    z.sName       = pt.sName.GetData();
    z.sSize       = pt.sSize.GetData();
    z.sType       = pt.sType.GetData();
    z.sVolumeGuid = pt.sVolumeGuid.GetData();
    z.sFs         = pt.sFs.GetData();
    z.dwDisk      = pt.dwDisk;
    z.dwPart      = pt.dwPart;
    z.bEfi        = pt.bEfi;
    z.bMsr        = pt.bMsr;
    z.bPrimary    = pt.bPrimary;
    z.bActive     = pt.bActive;
    toolsvc::TBfFixResult br;
    if (!m_pWizSvc->BfFixBoot(z, bUefi, &sink, br))
    {
        if (br.dwMountErr == 0)
        {
            m_pHost->HostAlert(L"引导修复",
                L"没有空闲盘符可临时分配给目标分区。", MB_ICONWARNING);
        }
        else
        {
            CDuiString sErr;
            sErr.Format(L"无法为目标分区临时分配盘符（错误码 %lu），请确认以管理员身份运行。",
                        br.dwMountErr);
            m_pHost->HostAlert(L"引导修复", sErr, MB_ICONERROR);
        }
        return;
    }
    if (br.dwExitCode == 0)
    {
        m_pHost->HostAlert(L"引导修复",
            L"[成功] 引导信息已写入目标分区。", MB_ICONINFORMATION);
    }
    else
    {
        CDuiString sFail;
        sFail.Format(L"[失败] bcdboot 退出码=%u，请记录代码并排查。", br.dwExitCode);
        m_pHost->HostAlert(L"引导修复", sFail, MB_ICONERROR);
    }
}
// [R16][T10] 自 BootFixOk 机械抽取：分区/固件匹配校验（中止返回 false）
bool CToolDialogPage::BfValidateFirmwareMatch(const TBfPart& pt, bool bUefi)
{
    // 分区/固件匹配校验（原版 4096/4098/4099）
    if (bUefi)
    {
        CDuiString sFs = pt.sFs;
        sFs.MakeUpper();
        bool bFat = (sFs == L"FAT32" || sFs == L"FAT"
                     || sFs == L"FAT16");
        if (!bFat &&
            !m_pHost->HostConfirm(L"引导修复",
                L"UEFI启动需要FAT16/FAT32分区。所选分区文件系统不匹配，仍要继续吗？"))
            return false;
        if (pt.sType != L"GPT" &&
            !m_pHost->HostConfirm(L"引导修复",
                L"当前为 UEFI 固件，但所选分区位于 MBR 磁盘，这可能无法启动，确定继续吗？"))
            return false;
    }
    else
    {
        if (!pt.bPrimary)
        {
            m_pHost->HostAlert(L"引导修复",
                L"BIOS启动分区必须是个主分区。", MB_ICONWARNING);
            return false;
        }
        if (pt.sType != L"MBR" &&
            !m_pHost->HostConfirm(L"引导修复",
                L"当前为 BIOS 固件，但所选分区位于 GPT 磁盘，这可能无法启动，确定继续吗？"))
            return false;
    }
    return true;
}



