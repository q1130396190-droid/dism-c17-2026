#include "stdafx.h"
#include "ThemeTokens.h"   // [R203] 颜色令牌（深色/浅色跟随）
// [R16][T10] 工具箱弹窗按 kind 拆分的渲染器分组文件（自 ToolDialogPage.cpp 物理搬迁，
// 零行为改动）。公共壳（Chrome/通用控件工厂/消息路由/作业框架）仍在 ToolDialogPage.cpp；
// 跨组共享的内部设施（tdlg:: 作业 Sink/进度窗/行单元格助手）见 ToolDlgInternal.h。
#include "ToolDialogPage.h"
#include "ToolDlgInternal.h"
#include "WinHandles.h"   // [H-51] win::HandlePtr RAII for F11_raw_handle_close
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
// [ESD 自主解密] 后台线程上下文
// [R16-T9] 派生统一信封 TJobDoneEnvelope：bOk/sMsg（原 sErr）由信封承载，
// 工作线程 new，OnEsdTaskDone 单出口 delete（所有权契约见 FrameWnd_Messages.h）。
struct TEsdTaskCtx : TJobDoneEnvelope
{
    HWND                        hWnd;
    std::shared_ptr<toolsvc::IIsoImageService> pSvc;  // [R16][T2] 拥有型副本，线程与弹窗生命周期解耦
    std::wstring                sSrc;
    std::wstring                sDst;
    std::vector<std::wstring>   vKeys;
};
    // DISM 的 Size 字段（字节数字串）→ 可读展开大小
    // [C++17 R3] [[nodiscard]]：返回值勿忽略
[[nodiscard]] CDuiString FormatImgxSize(const CDuiString& sBytes)
    {
        if (sBytes.IsEmpty()) return CDuiString();
        ULONGLONG ull = 0;
        for (int i = 0; i < sBytes.GetLength(); ++i)
        {
            wchar_t ch = sBytes[i];
            if (ch < L'0' || ch > L'9') continue;
            ull = ull * 10 + static_cast<ULONGLONG>(ch - L'0');
        }
        if (ull == 0) return CDuiString(L"0 B");
        static const wchar_t s_szUnit[] = L"BKMGTPE";
        double dV = static_cast<double>(ull);
        int nU = 0;
        while (dV >= 1024.0 && nU < 6) { dV /= 1024.0; ++nU; }
        CDuiString s;
        if (nU == 0) s.Format(L"%llu B", ull);
        else s.Format(L"%.2f %cB", dV, s_szUnit[nU]);
        return s;
    }
// [ESD 自主解密] 后台执行 CryptoAPI 解密
// [R7] 解密原语下沉 IIsoImageService::DecryptEsd；本过程仅线程/消息编排。
// 服务经 TEsdTaskCtx 持有 shared_ptr 副本（[R16][T2] 弹窗析构不悬垂）。
// [R-TAILFIX 同型] 解密体 SEH 边界：CryptoAPI/文件原语硬件异常若穿透线程
// 过程，WM_TDLG_ESDDONE 永不投递，弹窗遮罩永久锁死。本函数无 C++ 对象，
// 允许 __try；异常降级为失败，完成消息照常回投。
static void EsdDecryptSehBody(TEsdTaskCtx* pCtx)
{
    __try
    {
        pCtx->bOk = pCtx->pSvc->DecryptEsd(pCtx->sSrc, pCtx->sDst,
                                           pCtx->vKeys, pCtx->sMsg);
    }
    __except (CrashGuardFilter(GetExceptionInformation(), "esd-decrypt-seh"))
    {
        pCtx->bOk = false;
        if (pCtx->sMsg.empty()) pCtx->sMsg = L"ESD 解密发生硬件异常（已兜底收口）";
    }
}
DWORD WINAPI CToolDialogPage::EsdDecryptThread(void* pv)
{
    std::unique_ptr<TEsdTaskCtx> guard(static_cast<TEsdTaskCtx*>(pv)); // [C++17 R3] ownership from thread launcher
    TEsdTaskCtx* pCtx = guard.get();
    EsdDecryptSehBody(pCtx);
    // [加固] 窗口失效或投递失败时在线程内回收，避免 TEsdTaskCtx 泄漏
    if (::IsWindow(pCtx->hWnd)
        && ::PostMessage(pCtx->hWnd, WM_TDLG_ESDDONE, 0, reinterpret_cast<LPARAM>(pCtx)))
    {
        // 所有权移交 OnEsdTaskDone
        guard.release(); // [C++17 R3] receiver deletes
    }
    // [C++17 R3] on post failure the guard deletes the ctx (was: else delete pCtx)
    return 0;
}
void CToolDialogPage::OnEsdTaskDone(void* pvResult)
{
    EndTaskShield();
    std::unique_ptr<TEsdTaskCtx> guard(reinterpret_cast<TEsdTaskCtx*>(pvResult)); // [C++17 R3] ownership from poster
    TEsdTaskCtx* pCtx = guard.get();
    if (pCtx)
    {
        if (pCtx->bOk)
        {
            CDuiString sMsg;
            sMsg.Format(L"ESD 解密完成，已生成：\r\n%s\r\n\r\n可直接读取/挂载该 WIM 映像。",
                        pCtx->sDst.c_str());
            ::MessageBoxW(m_hWnd, sMsg.GetData(),
                          StringMgr.Get("esdwiz.title"), MB_OK | MB_ICONINFORMATION);
        }
        else
        {
            CDuiString sMsg = L"ESD 解密失败。";
            if (!pCtx->sMsg.empty()) { sMsg += L"\r\n\r\n"; sMsg += pCtx->sMsg.c_str(); }
            ::MessageBoxW(m_hWnd, sMsg.GetData(),
                          StringMgr.Get("esdwiz.title"), MB_OK | MB_ICONERROR);
        }
        // [C++17 R3] receiver-side delete happens at guard scope exit
    }
}



void CToolDialogPage::ClickImageX(const CDuiString& sName)
{
    if (m_imgx.bMeta)
    {
        if (sName == L"btn_meta_ok") ImageXMetaOk();
        else if (sName == L"btn_meta_back")
        {
            m_pBody->RemoveAll();
            BuildImageX();
            if (!m_imgx.sFile.IsEmpty())
            {
                CEditUI* pE = static_cast<CEditUI*>(
                    m_pm.FindControl(L"edt_imgx_path"));
                if (pE) pE->SetText(m_imgx.sFile);
                m_imgx.vIdx.clear();
                if (m_pHost->HostQueryWim(m_imgx.sFile, m_imgx.vIdx)
                    && !m_imgx.vIdx.empty())
                {
                    for (int i = 0;
                         i < static_cast<int>(m_imgx.vIdx.size()) && i < static_cast<int>(m_vRows.size());
                         ++i)
                    {
                        const TDialogWimIndex& ix = m_imgx.vIdx[i];
                        tdlg::SetRowCell(m_vRows[i], 0,
                                   ix.sDesc.IsEmpty() ? ix.sName.GetData()
                                                      : ix.sDesc.GetData());
                        tdlg::SetRowCell(m_vRows[i], 1, ix.sName);
                        tdlg::SetRowCell(m_vRows[i], 2, ix.sVersion);
                        tdlg::SetRowCell(m_vRows[i], 3, ix.sCreated);
                    }
                    SetGridSelection(0);
                }
            }
        }
    }
    else
    {
        if (sName.Left(8) == L"gridrow_") OnGridRow(sName);
        else if (sName.Left(5) == L"imgx_")
            ImageXAction(_ttoi(sName.Mid(5).GetData()));
        else if (sName == L"btn_imgx_browse") ImageXBrowse();
    }
}
void CToolDialogPage::ClickEsdWiz(const CDuiString& sName)
{
    if (sName == L"btn_esdwiz_browse") EsdWizBrowse();
    else if (sName == L"btn_esdwiz_export") EsdWizExport();
}
void CToolDialogPage::BuildImageX()
{
    m_imgx.bMeta = false;



    std::vector<TGridCol> cols;
    cols.push_back(TGridCol(StringMgr.Get("misc.sfdf6f7f6"), 240));
    cols.push_back(TGridCol(StringMgr.Get("toolbox.s46a27cf4"), 220));
    cols.push_back(TGridCol(L"产品分析", 320));
    cols.push_back(TGridCol(StringMgr.Get("misc.sc1835196"), 130));
    cols.push_back(TGridCol(StringMgr.Get("misc.s696f5a97"), -1));
    m_pBody->Add(MkGrid(cols, 12, 32, true));



    CControlUI* pGap1 = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pGap1->SetFixedHeight(16);
    m_pBody->Add(pGap1);



    CHorizontalLayoutUI* pPath = std::make_unique<CHorizontalLayoutUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pPath->SetFixedHeight(38);
    pPath->Add(MkEdit(L"edt_imgx_path"));
    CControlUI* pGW = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via pPath->Add
    pGW->SetFixedWidth(12);
    pPath->Add(pGW);
    CButtonUI* pB = MkBtn(StringMgr.Get("general.browse"), L"btn_imgx_browse", false);
    pB->SetFixedWidth(104);
    pPath->Add(pB);
    m_pBody->Add(pPath);



    CControlUI* pGap2 = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pGap2->SetFixedHeight(8);
    m_pBody->Add(pGap2);



    // [R16][T10] 批量模式面板机械抽取为 ImxBuildBatchBox
    ImxBuildBatchBox();



    CControlUI* pGap3 = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pGap3->SetFixedHeight(8);
    m_pBody->Add(pGap3);



    // [R16][T10] 底部链接行机械抽取为 ImxBuildLinkRow
    ImxBuildLinkRow();
}
// [R16][T10] 自 BuildImageX 机械抽取：批量模式面板
void CToolDialogPage::ImxBuildBatchBox()
{
    // ===== P1-5: 批量模式面板（不影响现有 5 个链接的单索引操作） =====
    CVerticalLayoutUI* pBatchBox = std::make_unique<CVerticalLayoutUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pBatchBox->SetFixedHeight(64);
    pBatchBox->SetAttribute(L"bordercolor", tokens::TokenHex(tokens::EToken::Border));
    pBatchBox->SetAttribute(L"bordersize", L"1");
    pBatchBox->SetBkColor(tokens::Token(tokens::EToken::SurfaceAlt));   // [R220] 批量区次级底（原淡黄 #FFFAF0，仅作区域区分，非警告）
    pBatchBox->SetAttribute(L"inset", L"12,8,12,8");



    CHorizontalLayoutUI* pBatchRow1 = std::make_unique<CHorizontalLayoutUI>().release(); // [C++17 R3] framework-owned via pBatchBox->Add
    pBatchRow1->SetFixedHeight(28);
    m_imgx.pBatchMode = std::make_unique<CTdlgCheck>().release(); // [C++17 R3] framework-owned via pBatchRow1->Add
    m_imgx.pBatchMode->Setup(StringMgr.Get("misc.hc4d4"), false);
    m_imgx.pBatchMode->SetFixedWidth(110);
    pBatchRow1->Add(m_imgx.pBatchMode);
    CControlUI* pBSpace1 = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via pBatchRow1->Add
    pBatchRow1->Add(pBSpace1);
    CTextUI* pBatchHint = std::make_unique<CTextUI>().release(); // [C++17 R3] framework-owned via pBatchRow1->Add
    pBatchHint->SetText(StringMgr.Get("misc.h7e01"));
    pBatchHint->SetFont(10);
    pBatchHint->SetTextColor(tokens::Token(tokens::EToken::InkTertiary));
    pBatchRow1->Add(pBatchHint);
    pBatchBox->Add(pBatchRow1);



    CHorizontalLayoutUI* pBatchRow2 = std::make_unique<CHorizontalLayoutUI>().release(); // [C++17 R3] framework-owned via pBatchBox->Add
    pBatchRow2->SetFixedHeight(28);
    CTextUI* pBatchLbl = std::make_unique<CTextUI>().release(); // [C++17 R3] framework-owned via pBatchRow2->Add
    pBatchLbl->SetText(StringMgr.Get("misc.hade9"));
    pBatchLbl->SetFixedWidth(76);
    pBatchLbl->SetFont(10);
    pBatchLbl->SetTextColor(tokens::Token(tokens::EToken::InkSecondary));
    pBatchLbl->SetAttribute(L"valign", L"vcenter");
    pBatchLbl->SetMouseEnabled(false);
    pBatchRow2->Add(pBatchLbl);
    m_imgx.pBatchEdit = MkEdit(L"edt_imgx_batch");
    m_imgx.pBatchEdit->SetText(L"1 2 3");
    m_imgx.pBatchEdit->SetAttribute(L"tip", StringMgr.Get("misc.h474a"));
    pBatchRow2->Add(m_imgx.pBatchEdit);
    pBatchBox->Add(pBatchRow2);



    // [P0] 第 3 行：SWM 拆分分卷大小（MB），供 imgx_5（dism /Split-Image）使用
    CHorizontalLayoutUI* pBatchRow3 = std::make_unique<CHorizontalLayoutUI>().release(); // [C++17 R3] framework-owned via pBatchBox->Add
    pBatchRow3->SetFixedHeight(28);
    CTextUI* pSwmLbl = std::make_unique<CTextUI>().release(); // [C++17 R3] framework-owned via pBatchRow3->Add
    pSwmLbl->SetText(L"SWM part (MB):");
    pSwmLbl->SetFixedWidth(110);
    pSwmLbl->SetFont(10);
    pSwmLbl->SetTextColor(tokens::Token(tokens::EToken::InkSecondary));
    pSwmLbl->SetAttribute(L"valign", L"vcenter");
    pSwmLbl->SetMouseEnabled(false);
    pBatchRow3->Add(pSwmLbl);
    CEditUI* pSwmEdit = MkEdit(L"edt_imgx_swm_size");
    pSwmEdit->SetText(L"4096");
    pSwmEdit->SetAttribute(L"tip", L"Part size in MB (32 - 1000000)");
    pBatchRow3->Add(pSwmEdit);
    pBatchBox->Add(pBatchRow3);
    pBatchBox->SetFixedHeight(96);
    m_pBody->Add(pBatchBox);
}
// [R16][T10] 自 BuildImageX 机械抽取：底部 6 个操作链接行
void CToolDialogPage::ImxBuildLinkRow()
{
    CHorizontalLayoutUI* pLinks = std::make_unique<CHorizontalLayoutUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pLinks->SetFixedHeight(30);
    // [P0] 第 6 个入口 imgx_5：拆分 SWM（dism /Split-Image）
    const wchar_t* pszLinks[6] =
    {
        StringMgr.Get("toolbox.s045ebe22"), StringMgr.Get("misc.s9f5141f4"), StringMgr.Get("toolbox.s1e2263b3"),
        StringMgr.Get("toolbox.s68bab772"), StringMgr.Get("toolbox.s5ca794c0"), L"Split SWM"
    };
    for (int i = 0; i < 6; ++i)
    {
        tdlg::CLinkLabelUI* pL = std::make_unique<tdlg::CLinkLabelUI>().release(); // [C++17 R3] framework-owned via pLinks->Add
        CDuiString sN;
        sN.Format(L"imgx_%d", i);
        pL->SetName(sN);
        pL->SetText(pszLinks[i]);
        pL->SetFont(10);
        pL->SetTextColor(tokens::Token(tokens::EToken::AccentText));
        pL->SetAttribute(L"align", L"center");
        pL->SetAttribute(L"valign", L"vcenter");
        pLinks->Add(pL);
    }
    m_pBody->Add(pLinks);
}



void CToolDialogPage::ImageXBrowse()
{
    CDuiString sFile;
    // [底层通用] 过滤器纳入 .swm：分卷映像首部分直接打开，wimgapi 自动发现后续部分
    if (!m_pHost->HostOpenFile(
            sFile,
            L"WIM/ESD/SWM/ISO 映像 (*.wim;*.esd;*.swm;*.iso)\0*.wim;*.esd;*.swm;*.iso\0所有文件 (*.*)\0*.*\0",
            StringMgr.Get("toolbox.sa4ef8ca2"))) return;



    CEditUI* pE = static_cast<CEditUI*>(m_pm.FindControl(L"edt_imgx_path"));
    // [R6-04] 先前槽位 ISO 的卸载在服务 MountIsoSlot(1,...) 内部完成。
    // [底层通用] 魔数级容器探测（知识库 41）：wimgapi 仅认 MSWIM 魔数，
    // ISO 的 0xCD001 喂入会返回 0xC1420000，加密 ESD 须先内存解密。
    // 不再依赖扩展名——任何后缀的容器都能被正确识别与分流。
    const wchar_t* pszExt = ::PathFindExtension(sFile.GetData());
    // [R7] 魔数容器探测下沉 IIsoImageService（EImgContainerKind 与旧枚举同值）
    toolsvc::TContainerInfo ci;
    m_pImgSvc->DetectContainer(sFile.GetData(), ci);
    if (ci.nKind == toolsvc::EImgContainerKind::IMG_CK_ISO
        || (ci.nKind == toolsvc::EImgContainerKind::IMG_CK_UNKNOWN && pszExt
            && lstrcmpiW(pszExt, L".iso") == 0))
    {
        // [R6-04] 挂载/解析/槽位所有权下沉 IIsoImageService（槽位 1=ImageX）
        std::wstring wInner, wErr;
        if (!m_pImgSvc->MountIsoSlot(1, sFile.GetData(), wInner, wErr))
        {
            ::MessageBoxW(m_hWnd, CDuiString(wErr.c_str()).GetData(),
                StringMgr.Get("toolbox.edit_img"), MB_OK | MB_ICONWARNING);
            return;
        }
        CDuiString sInner = wInner.c_str();
        m_imgx.sFile = sInner;
        if (pE) pE->SetText(sInner.GetData());
        CDuiString sLogIso;
        sLogIso.Format(L"[imagex] ISO 透明挂载：%s -> %s（关闭窗口自动卸载）",
                       sFile.GetData(), sInner.GetData());
        m_pHost->HostLog(sLogIso);
    }
    else if (ci.nKind == toolsvc::EImgContainerKind::IMG_CK_ENCRYPTED_ESD)
    {
        // 加密 ESD：wimgapi 无法直接加载。原版在主程序内做独有内存解密
        // （RSA 解会话密钥 + AES-CBC 解 LZMS 流）。本项目对应能力为工具箱
        // 的「ESD 解密向导」，引导用户先行解密，源文件不会被修改。
        ::MessageBoxW(m_hWnd,
            L"检测到该 ESD 已加密，无法直接加载。\r\n\r\n"
            L"程序将使用独有内存解密，无需担心源文件被修改：\r\n"
            L"请先在工具箱中打开「ESD 解密向导」，将其解密导出为 WIM，\r\n"
            L"再回到此处读取生成的 WIM 映像。",
            StringMgr.Get("esdwiz.title"), MB_OK | MB_ICONINFORMATION);
        if (pE) pE->SetText(sFile.GetData());
        return;
    }
    else
    {
        // WIM / ESD（未加密）/ SWM 分卷：统一直连 wimgapi 打开。
        // SWM 传入首部分即可，引擎自动按 GUID 发现同目录后续分卷。
        if (pE) pE->SetText(sFile.GetData());
        m_imgx.sFile = sFile;
        const wchar_t* pszKind = L"WIM";
        if (ci.nKind == toolsvc::EImgContainerKind::IMG_CK_ESD)      pszKind = L"ESD";
        else if (ci.nKind == toolsvc::EImgContainerKind::IMG_CK_SWM) pszKind = L"SWM 分卷";
        CDuiString sLogOpen;
        sLogOpen.Format(L"[imagex] 容器识别：%s（映像数 %u，分卷 %u/%u）",
                        pszKind, ci.nImageCount, ci.nPartNumber, ci.nTotalParts);
        m_pHost->HostLog(sLogOpen);
    }
    ImageXReload(true);
}



// 重新读取 m_imgx.sFile 的全部索引并刷新表格。
// bAlert=true（手动浏览/任务后刷新失败）时弹错误框；返回是否成功。
bool CToolDialogPage::ImageXReload(bool bAlert)
{
    m_imgx.vIdx.clear();
    CDuiString sErr;
    if (!m_pHost->HostQueryWim(m_imgx.sFile, m_imgx.vIdx, &sErr)
        || m_imgx.vIdx.empty())
    {
        if (bAlert)
        {
            CDuiString sMsg = StringMgr.Get("error.s1f6bf2f0");
            if (!sErr.IsEmpty())
            {
                sMsg += L"\r\n\r\n";
                sMsg += sErr;
            }
            ::MessageBoxW(m_hWnd, sMsg.GetData(),
                          StringMgr.Get("toolbox.edit_img"), MB_OK | MB_ICONWARNING);
        }
        else
        {
            m_pHost->HostLog(CDuiString(StringMgr.Get("error.sbdaa7ab7")) + sErr);
        }
        return false;
    }



    ImageXFillRows();
    return true;
}



void CToolDialogPage::ImageXFillRows()
{
    for (int i = 0; i < static_cast<int>(m_vRows.size()); ++i)
    {
        if (i < static_cast<int>(m_imgx.vIdx.size()))
        {
            const TDialogWimIndex& ix = m_imgx.vIdx[i];
            tdlg::SetRowCell(m_vRows[i], 0,
                       ix.sDesc.IsEmpty() ? ix.sName.GetData()
                                          : ix.sDesc.GetData());
            tdlg::SetRowCell(m_vRows[i], 1, ix.sName);
            // 产品分析（Win7~11 / Server 全版本统一识别）
            tdlg::SetRowCell(m_vRows[i], 2, ix.sProduct);
            // “展开...”列：优先展开大小（Win7 映像无版本号），其次版本
            CDuiString sExpand = FormatImgxSize(ix.sSize);
            if (sExpand.IsEmpty()) sExpand = ix.sVersion;
            tdlg::SetRowCell(m_vRows[i], 3, sExpand);
            tdlg::SetRowCell(m_vRows[i], 4, ix.sCreated);
        }
        else
        {
            for (int c = 0; c < 5; ++c) tdlg::SetRowCell(m_vRows[i], c, L"");
        }
    }
    SetGridSelection(0);
}



// ====================================================================
// P1-5: 批量模式索引列表解析
// 输入格式示例："1 2 3"、"1,3,5"、"1 3-5"（支持空格/逗号分隔 + 短横线范围）
// 输出：升序去重的 1-based 索引列表
// ====================================================================
// [R16][T10] 自 ParseIndexList 机械抽取：冲刷当前 token（区间展开或单值）。
// 原函数内两处逐字相同的收尾/分隔符分支收编为一（行为不变）。
static void FlushIndexTok(std::vector<int>& outList, CDuiString& sTok,
                          CDuiString& sRangeFrom, bool& bInRange)
{
    if (sTok.IsEmpty()) return;
    int n = _ttoi(sTok.GetData());
    if (bInRange && !sRangeFrom.IsEmpty())
    {
        int nLo = _ttoi(sRangeFrom.GetData());
        int nHi = n;
        if (nLo > nHi) { int tmp = nLo; nLo = nHi; nHi = tmp; }
        for (int k = nLo; k <= nHi; ++k)
            if (k > 0) outList.push_back(k);
    }
    else if (n > 0)
    {
        outList.push_back(n);
    }
    sTok.Empty();
    sRangeFrom.Empty();
    bInRange = false;
}
static void ParseIndexList(const CDuiString& sInput, std::vector<int>& outList)
{
    outList.clear();
    const wchar_t* p = sInput.GetData();
    if (!p) return;



    CDuiString sTok;
    CDuiString sRangeFrom;
    bool bInRange = false;



    for (; *p; ++p)
    {
        wchar_t c = *p;
        if (c >= L'0' && c <= L'9')
        {
            sTok += c;
        }
        else if (c == L'-')
        {
            // 短横线表示范围（仅当前面已有数字）
            if (!sTok.IsEmpty())
            {
                sRangeFrom = sTok;
                bInRange = true;
                sTok.Empty();
            }
        }
        else
        {
            // 分隔符：空格、逗号、制表符、分号等
            FlushIndexTok(outList, sTok, sRangeFrom, bInRange);
        }
    }
    // 收尾
    FlushIndexTok(outList, sTok, sRangeFrom, bInRange);
}



// 将索引列表去重升序
static void SortUniqueIndex(std::vector<int>& v)
{
    // 简单冒泡（列表通常 < 100 项）
    int n = static_cast<int>(v.size());
    for (int i = 0; i < n; ++i)
    {
        for (int j = i + 1; j < n; ++j)
        {
            if (v[j] < v[i])
            {
                int t = v[i]; v[i] = v[j]; v[j] = t;
            }
        }
    }
    // 去重
    int nWrite = 0;
    for (int i = 0; i < static_cast<int>(v.size()); ++i)
    {
        if (i == 0 || v[i] != v[i - 1])
        {
            v[nWrite++] = v[i];
        }
    }
    v.resize(nWrite);
}



void CToolDialogPage::ImageXAction(int nAction)
{
    SImgxCtx c;
    if (!ImxPrepare(nAction, c)) return;
    switch (nAction)
    {
    case 0: ImxCaseDelete(c); break;
    case 1: ImxCaseBootable(c); break;
    case 2: ImxCaseExport(c); break;
    case 3: ImxCaseAppend(c); break;
    case 4: ImxCaseMeta(c); break;
    default: break;
    }
}
[[nodiscard]] bool CToolDialogPage::ImxPrepare(int nAction, SImgxCtx& c)
{
    c.nAction = nAction;
    c.bBatch = false;
    c.bEsd = false;
    c.nCount = 0;
    c.nIndex = 0;
    if (m_imgx.sFile.IsEmpty())
    {
        ::MessageBoxW(m_hWnd, StringMgr.Get("toolbox.sa6732e8a"),
                      StringMgr.Get("toolbox.edit_img"), MB_OK | MB_ICONWARNING);
        return false;
    }
    // [P0] SWM 拆分：对整个映像文件操作，不需要索引选择
    if (nAction == 5)
    {
        ImageXSplit();
        return false;
    }
    // [R16][T10] 批量模式解析机械抽取为 ImxParseBatch
    ImxParseBatch(c);
    if (!c.bBatch && (m_nSel < 0 || m_nSel >= static_cast<int>(m_imgx.vIdx.size())))
    {
        ::MessageBoxW(m_hWnd, StringMgr.Get("toolbox.sa6732e8a"),
                      StringMgr.Get("toolbox.edit_img"), MB_OK | MB_ICONWARNING);
        return false;
    }
    c.sFile = m_imgx.sFile;
    c.nIndex = m_imgx.vIdx[m_nSel].nIndex;
    int nDot = c.sFile.ReverseFind(L'.');
    c.sExt = nDot >= 0 ? c.sFile.Mid(nDot) : CDuiString(L"");
    c.sExt.MakeLower();
    c.bEsd = (c.sExt == L".esd");
    c.sComp = c.bEsd ? CDuiString(L"recovery") : CDuiString(L"max");
    c.nCount = static_cast<int>(m_imgx.vIdx.size());
    // 临时文件必须放在源映像同目录（bat 工作目录在系统 Temp 可能在别的盘）
    int nSlash = c.sFile.ReverseFind(L'\\');
    c.sDir = nSlash >= 0 ? c.sFile.Mid(0, nSlash + 1) : CDuiString(L"");
    // 全文件索引编号快照（后台线程不再访问 UI 成员 m_imgx.vIdx）
    c.vAllIdx.clear();
    for (int i = 0; i < c.nCount; ++i)
        c.vAllIdx.push_back(static_cast<DWORD>(m_imgx.vIdx[i].nIndex));
    return true;
}
// [R16][T10] 自 ImxPrepare 机械抽取：批量模式索引列表解析
void CToolDialogPage::ImxParseBatch(SImgxCtx& c)
{
    // ===== P1-5: 批量模式解析 =====
    if (m_imgx.pBatchMode && m_imgx.pBatchMode->Checked())
    {
        CDuiString sList = m_imgx.pBatchEdit ? m_imgx.pBatchEdit->GetText() : CDuiString(L"");
        ParseIndexList(sList, c.vBatchIdx);
        SortUniqueIndex(c.vBatchIdx);
        std::vector<int> vValid;
        for (size_t k = 0; k < c.vBatchIdx.size(); ++k)
        {
            int n = c.vBatchIdx[k];
            if (n >= 1 && n <= static_cast<int>(m_imgx.vIdx.size()))
                vValid.push_back(n);
        }
        c.vBatchIdx = vValid;
        if (!vValid.empty())
        {
            c.bBatch = true;
            m_nSel = vValid[0] - 1;   // 单索引模式下 m_nSel 兼容
        }
    }
}
bool CToolDialogPage::ImxCaseDelete(SImgxCtx& c)
{
    // case 0: 删除映像（支持批量）
    const CDuiString sTitleImgx = StringMgr.Get("toolbox.edit_img");
    if (c.bBatch)
    {
        return ImxDeleteBatch(c, sTitleImgx);
    }
    // ---- 单索引删除 ----
    if (c.nCount <= 1)
    {
        ::MessageBoxW(m_hWnd, StringMgr.Get("toolbox.se66f8597"),
                      sTitleImgx, MB_OK | MB_ICONWARNING);
        return true;
    }
    if (::MessageBoxW(m_hWnd,
                      c.bEsd ? (CDuiString(StringMgr.Get("export.s98f70ae4")) + StringMgr.Get("toolbox.saaf9726d"))
                             : CDuiString(StringMgr.Get("toolbox.se8abc259")),
                      sTitleImgx,
                      MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return true;
    if (!m_pImgSvc->WimReady())
    {
        ::MessageBoxW(m_hWnd, L"wimgapi.dll 不可用，无法删除映像索引。",
                      sTitleImgx, MB_OK | MB_ICONERROR);
        return true;
    }
    std::wstring wFile(c.sFile.GetData());
    DWORD nIdxDel = static_cast<DWORD>(c.nIndex);
    if (c.bEsd)
    {
        std::vector<DWORD> vKeep;
        for (int i = 0; i < c.nCount; ++i)
            if (c.vAllIdx[i] != nIdxDel) vKeep.push_back(c.vAllIdx[i]);
        std::wstring wTmp((c.sDir + L"_imgx_del.esd").GetData());
        RunImageXJob(L"正在删除 ESD 映像索引（导出重建），请勿关闭窗口…",
            sTitleImgx, L"映像索引已删除。", true, false,
            m_pImgSvc->MakeDeleteEsdRebuildSingleJob(wFile, wTmp, vKeep));
        return true;
    }
    RunImageXJob(L"正在删除 WIM 映像索引，请勿关闭窗口…",
        sTitleImgx, L"映像索引已删除。", true, false,
        m_pImgSvc->MakeDeleteWimSingleJob(wFile, nIdxDel));
    return true;
}
// [R16][T10] self-extracted from ImxCaseDelete: batch delete branch
bool CToolDialogPage::ImxDeleteBatch(SImgxCtx& c, const CDuiString& sTitleImgx)
{
        // ===== P1-5: 批量删除 =====
        if (static_cast<int>(c.vBatchIdx.size()) >= c.nCount)
        {
            CDuiString sMsgAll;
            sMsgAll.Format(StringMgr.Get("misc.hc237"), c.nCount);
            ::MessageBoxW(m_hWnd, sMsgAll.GetData(),
                          sTitleImgx, MB_OK | MB_ICONWARNING);
            return true;
        }
        CDuiString sMsgDel;
        sMsgDel.Format(StringMgr.Get("misc.hb5a2"), static_cast<int>(c.vBatchIdx.size()));
        if (::MessageBoxW(m_hWnd, sMsgDel.GetData(), sTitleImgx,
                          MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return true;
        if (!m_pImgSvc->WimReady())
        {
            ::MessageBoxW(m_hWnd, L"wimgapi.dll 不可用，无法删除映像索引。",
                          sTitleImgx, MB_OK | MB_ICONERROR);
            return true;
        }
        std::wstring wFile(c.sFile.GetData());
        if (c.bEsd)
        {
            std::vector<DWORD> vKeep;
            for (int i = 0; i < c.nCount; ++i)
            {
                bool bInBatch = false;
                for (size_t k = 0; k < c.vBatchIdx.size(); ++k)
                    if (c.vBatchIdx[k] == (i + 1)) { bInBatch = true; break; }
                if (!bInBatch) vKeep.push_back(c.vAllIdx[i]);
            }
            std::wstring wTmp((c.sDir + L"_imgx_del.esd").GetData());
            RunImageXJob(L"正在删除 ESD 映像索引（导出重建），请勿关闭窗口…",
                sTitleImgx, L"选定的映像索引已删除。", true, false,
                m_pImgSvc->MakeDeleteEsdRebuildJob(wFile, wTmp, vKeep));
            return true;
        }
        std::vector<DWORD> vDel;
        {
            std::vector<int> vDesc = c.vBatchIdx;
            for (size_t i = 0; i < vDesc.size(); ++i)
                for (size_t j = i + 1; j < vDesc.size(); ++j)
                    if (vDesc[j] > vDesc[i])
                    { int t = vDesc[i]; vDesc[i] = vDesc[j]; vDesc[j] = t; }
            for (size_t k = 0; k < vDesc.size(); ++k)
            {
                int nSlot = vDesc[k] - 1;
                if (nSlot >= 0 && nSlot < c.nCount)
                    vDel.push_back(c.vAllIdx[nSlot]);
            }
        }
        RunImageXJob(L"正在删除 WIM 映像索引，请勿关闭窗口…",
            sTitleImgx, L"选定的映像索引已删除。", true, false,
            m_pImgSvc->MakeDeleteWimBatchJob(wFile, vDel));
        return true;
}



// [R16][T10] 自 ImxCaseExport 机械抽取：批量导出分支
bool CToolDialogPage::ImxExportBatch(SImgxCtx& c, const CDuiString& sTitleImgx, CDuiString& sOut)
{
        if (!m_pHost->HostBrowseFolder(sOut, StringMgr.Get("edit.s056c4868"))) return true;
        int nODot = c.sFile.ReverseFind(L'.');
        CDuiString sBaseExt = nODot >= 0 ? c.sFile.Mid(nODot) : CDuiString(L".wim");
        sBaseExt.MakeLower();
        if (!m_pImgSvc->WimReady())
        {
            ::MessageBoxW(m_hWnd, L"wimgapi.dll 不可用，无法导出映像。",
                          sTitleImgx, MB_OK | MB_ICONERROR);
            return true;
        }
        std::vector<toolsvc::TExportItem> vItems;
        for (size_t k = 0; k < c.vBatchIdx.size(); ++k)
        {
            int nSlot = c.vBatchIdx[k] - 1;
            if (nSlot < 0 || nSlot >= c.nCount) continue;
            CDuiString sOutFile;
            sOutFile.Format(L"%s\\export_%u%s",
                sOut.GetData(), c.vAllIdx[nSlot], sBaseExt.GetData());
            toolsvc::TExportItem it; it.nIdx = c.vAllIdx[nSlot]; it.sOut = sOutFile.GetData();
            vItems.push_back(it);
        }
        std::wstring wFile(c.sFile.GetData());
        DWORD dwCompress = sBaseExt == L".esd" ? toolsvc::IMG_COMPRESS_LZMS : toolsvc::IMG_COMPRESS_NONE;
        RunImageXJob(L"正在批量导出映像，请勿关闭窗口…",
            sTitleImgx,
            (CDuiString(L"批量导出完成。\r\n\r\n输出目录：") + sOut),
            true, false,
            m_pImgSvc->MakeExportBatchJob(wFile, vItems, dwCompress));
        return true;
}
bool CToolDialogPage::ImxCaseBootable(SImgxCtx& c)
{
    // case 1: 可启动：全索引导出，选中索引加可启动标记（支持批量）
    const CDuiString sTitleImgx = StringMgr.Get("toolbox.edit_img");
    CDuiString sMsg;
    if (c.bBatch)
    {
        CDuiString sSel;
        for (size_t k = 0; k < c.vBatchIdx.size(); ++k)
        {
            if (k > 0) sSel += L",";
            CDuiString sN;
            sN.Format(L"%d", c.vBatchIdx[k]);
            sSel += sN;
        }
        sMsg.Format(StringMgr.Get("misc.heb2b"),
                    sSel.GetData(), StringMgr.Get("toolbox.s78023dcd"));
    }
    else
    {
        sMsg = CDuiString(StringMgr.Get("toolbox.s752bf9ff"))
             + StringMgr.Get("toolbox.s78023dcd");
    }
    if (::MessageBoxW(m_hWnd, sMsg.GetData(), sTitleImgx,
                      MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return true;
    if (!m_pImgSvc->WimReady())
    {
        ::MessageBoxW(m_hWnd, L"wimgapi.dll 不可用，无法设置可启动标记。",
                      sTitleImgx, MB_OK | MB_ICONERROR);
        return true;
    }
    std::vector<toolsvc::TBootItem> vItems;
    for (int i = 0; i < c.nCount; ++i)
    {
        bool bSel = false;
        if (c.bBatch)
        {
            for (size_t k = 0; k < c.vBatchIdx.size(); ++k)
                if (c.vBatchIdx[k] == (i + 1)) { bSel = true; break; }
        }
        else
        {
            bSel = (i == m_nSel);
        }
        toolsvc::TBootItem it; it.nIdx = c.vAllIdx[i]; it.bBoot = bSel;
        vItems.push_back(it);
    }
    std::wstring wFile(c.sFile.GetData());
    std::wstring wTmp((c.sDir + (c.bEsd ? CDuiString(L"_imgx_boot.esd") : L"_imgx_boot.wim")).GetData());
    DWORD dwComp = c.bEsd ? toolsvc::IMG_COMPRESS_LZMS : toolsvc::IMG_COMPRESS_NONE;
    RunImageXJob(L"正在设置可启动映像（导出重建），请勿关闭窗口…",
        sTitleImgx, L"可启动标记设置完成。", true, false,
        m_pImgSvc->MakeBootableRebuildJob(wFile, wTmp, vItems, dwComp));
    return true;
}
bool CToolDialogPage::ImxCaseExport(SImgxCtx& c)
{
    // case 2: 导出映像（支持批量）
    const CDuiString sTitleImgx = StringMgr.Get("toolbox.edit_img");
    CDuiString sOut;
    if (c.bBatch)
    {
        return ImxExportBatch(c, sTitleImgx, sOut);
    }
    if (!m_pHost->HostSaveFile(
            sOut,
            StringMgr.Get("toolbox.sf39f80db"),
            StringMgr.Get("edit.s056c4868"),
            L"Export.wim")) return true;
    int nODot = sOut.ReverseFind(L'.');
    CDuiString sOExt = nODot >= 0 ? sOut.Mid(nODot) : CDuiString(L"");
    sOExt.MakeLower();
    if (!m_pImgSvc->WimReady())
    {
        ::MessageBoxW(m_hWnd, L"wimgapi.dll 不可用，无法导出映像。",
                      sTitleImgx, MB_OK | MB_ICONERROR);
        return true;
    }
    std::wstring wFile(c.sFile.GetData());
    std::wstring wOut(sOut.GetData());
    DWORD dwCompress = sOExt == L".esd" ? toolsvc::IMG_COMPRESS_LZMS : toolsvc::IMG_COMPRESS_NONE;
    RunImageXJob(L"正在导出映像，请勿关闭窗口…",
        sTitleImgx,
        (CDuiString(L"映像导出完成。\r\n\r\n输出文件：") + sOut),
        true, false,
        m_pImgSvc->MakeExportSingleJob(
            wFile, wOut, static_cast<DWORD>(c.nIndex), dwCompress));
    return true;
}
bool CToolDialogPage::ImxCaseAppend(SImgxCtx& c)
{
    // case 3: 添加映像（捕获文件夹并追加到现有映像）
    const CDuiString sTitleImgx = StringMgr.Get("toolbox.edit_img");
    CDuiString sSrcDir;
    if (!m_pHost->HostBrowseFolder(sSrcDir, StringMgr.Get("toolbox.sae4c8e21")))
        return true;
    SYSTEMTIME st;
    GetLocalTime(&st);
    CDuiString sStamp, sName;
    sStamp.Format(L"%04d%02d%02d_%02d%02d%02d",
                  st.wYear, st.wMonth, st.wDay,
                  st.wHour, st.wMinute, st.wSecond);
    sName.Format(L"Added %s", sStamp.GetData());
    if (!m_pImgSvc->WimReady())
    {
        ::MessageBoxW(m_hWnd, L"wimgapi.dll 不可用，无法追加映像。",
                      sTitleImgx, MB_OK | MB_ICONERROR);
        return true;
    }
    std::wstring wFile(c.sFile.GetData());
    std::wstring wTmp((c.sDir + L"_imgx_add.wim").GetData());
    std::wstring wName(sName.GetData());
    std::wstring wSrc(sSrcDir.GetData());
    RunImageXJob(L"正在追加映像，请勿关闭窗口…",
        sTitleImgx, L"映像追加完成。", true, false,
        m_pImgSvc->MakeAppendJob(wFile, wTmp, wName, wSrc, c.bEsd));
    return true;
}
bool CToolDialogPage::ImxCaseMeta(SImgxCtx& c)
{
    // case 4: 编辑映像 → 元信息编辑模式（批量索引列表透传）
    m_imgx.sBatchList.Empty();
    if (c.bBatch)
    {
        for (size_t k = 0; k < c.vBatchIdx.size(); ++k)
        {
            if (k > 0) m_imgx.sBatchList += L" ";
            CDuiString sN;
            sN.Format(L"%d", c.vBatchIdx[k]);
            m_imgx.sBatchList += sN;
        }
    }
    m_pBody->RemoveAll();
    BuildImageXMeta();
    return true;
}
void CToolDialogPage::ImageXSplit()
{
    CDuiString sFile = m_imgx.sFile;
    int nDot = sFile.ReverseFind(L'.');
    CDuiString sExt = nDot >= 0 ? sFile.Mid(nDot) : L"";
    sExt.MakeLower();
    if (sExt != L".wim")
    {
        ::MessageBoxW(m_hWnd,
            L"SWM split supports .wim only. Export the ESD image to WIM first.",
            L"Split SWM", MB_OK | MB_ICONWARNING);
        return;
    }
    int nSlash = sFile.ReverseFind(L'\\');
    CDuiString sBase;
    if (nSlash >= 0)
        sBase = sFile.Mid(nSlash + 1, nDot - nSlash - 1);
    else
        sBase = sFile.Mid(0, nDot);
    CDuiString sOut;
    CDuiString sDef = sBase + L".swm";
    if (!m_pHost->HostSaveFile(sOut,
            L"Spanned WIM (*.swm)|*.swm|All files (*.*)|*.*",
            L"Split SWM", sDef.GetData())) return;
    if (sOut.IsEmpty()) return;
    int nODot = sOut.ReverseFind(L'.');
    if (nODot < 0 || sOut.Mid(nODot).CompareNoCase(L".swm") != 0)
        sOut += L".swm";
    // 分卷大小（MB）：取动态编辑框，缺省/非法回退 4096（单张 DVD 余量）
    DWORD dwPartMB = 4096;
    CEditUI* pSize = static_cast<CEditUI*>(m_pm.FindControl(L"edt_imgx_swm_size"));
    if (pSize)
    {
        long lSize = _ttol(pSize->GetText().GetData());
        if (lSize >= 32 && lSize <= 1000000L) dwPartMB = static_cast<DWORD>(lSize);
    }
    // [原生强制] WIMSplitFile 直连，禁止 dism /Split-Image 批处理
    if (!m_pImgSvc->WimReady())
    {
        ::MessageBoxW(m_hWnd, L"wimgapi.dll 不可用，无法拆分 SWM。",
                      L"Split SWM", MB_OK | MB_ICONERROR);
        return;
    }
    // ===== [2026-10-12] 后台拆分 + 真实进度，完成回 UI 弹窗 =====
    std::wstring wFile(sFile.GetData());
    std::wstring wOut(sOut.GetData());
    CDuiString sOk;
    sOk.Format(L"SWM 拆分完成。\r\n\r\n首个分卷：%s\r\n分卷大小：%u MB",
               sOut.GetData(), dwPartMB);
    // [R6-04] 作业体下沉 IIsoImageService（WIMSplitFile 直连）
    RunImageXJob(L"正在拆分 SWM 分卷，请勿关闭窗口…",
        L"Split SWM", sOk, false, false,
        m_pImgSvc->MakeSplitJob(wFile, wOut, dwPartMB));
}



void CToolDialogPage::BuildImageXMeta()
{
    m_imgx.bMeta = true;



    CVerticalLayoutUI* pBox = std::make_unique<CVerticalLayoutUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pBox->SetFixedHeight(122);
    pBox->SetAttribute(L"bordercolor", tokens::TokenHex(tokens::EToken::Border));
    pBox->SetAttribute(L"bordersize", L"1");
    pBox->SetBkColor(tokens::Token(tokens::EToken::Surface));
    pBox->SetAttribute(L"inset", L"18,16,18,16");



    CHorizontalLayoutUI* pRow1 = std::make_unique<CHorizontalLayoutUI>().release(); // [C++17 R3] framework-owned via pBox->Add
    pRow1->SetFixedHeight(38);
    CTextUI* pL1 = MkText(StringMgr.Get("misc.s66b615ec"), 1, 0xFF1A1A1A);
    pL1->SetFixedWidth(100);
    pL1->SetMouseEnabled(false);
    pRow1->Add(pL1);
    CEditUI* pName = MkEdit(L"edt_meta_name");
    pName->SetText(m_imgx.vIdx[m_nSel].sName.GetData());
    pRow1->Add(pName);
    pBox->Add(pRow1);



    CControlUI* pS = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via pBox->Add
    pS->SetFixedHeight(14);
    pBox->Add(pS);



    CHorizontalLayoutUI* pRow2 = std::make_unique<CHorizontalLayoutUI>().release(); // [C++17 R3] framework-owned via pBox->Add
    pRow2->SetFixedHeight(38);
    CTextUI* pL2 = MkText(StringMgr.Get("misc.s73bce646"), 1, 0xFF1A1A1A);
    pL2->SetFixedWidth(100);
    pL2->SetMouseEnabled(false);
    pRow2->Add(pL2);
    CEditUI* pDesc = MkEdit(L"edt_meta_desc");
    pDesc->SetText(m_imgx.vIdx[m_nSel].sDesc.GetData());
    pRow2->Add(pDesc);
    pBox->Add(pRow2);



    // 弹性占位（最小 16）：窗口变大时底部按钮始终贴底
    CControlUI* pTop = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pTop->SetMinHeight(16);
    m_pBody->Add(pTop);
    m_pBody->Add(pBox);



    AddBottomRow(MkBtn(StringMgr.Get("general.ok"), L"btn_meta_ok", true),
                 MkBtn(StringMgr.Get("misc.s5f411223"), L"btn_meta_back", false));
}



void CToolDialogPage::ImageXMetaOk()
{
    if (m_nSel < 0 || m_nSel >= static_cast<int>(m_imgx.vIdx.size())) return;



    CEditUI* pName = static_cast<CEditUI*>(m_pm.FindControl(L"edt_meta_name"));
    CEditUI* pDesc = static_cast<CEditUI*>(m_pm.FindControl(L"edt_meta_desc"));
    CDuiString sName = pName ? pName->GetText() : CDuiString(L"");
    CDuiString sDesc = pDesc ? pDesc->GetText() : CDuiString(L"");
    sName.Trim();
    sDesc.Trim();
    if (sName.IsEmpty())
    {
        ::MessageBoxW(m_hWnd, StringMgr.Get("misc.s2cb4096c"),
                      StringMgr.Get("toolbox.s5ca794c0"), MB_OK | MB_ICONWARNING);
        return;
    }



    sName.Replace(L"\"", L"");
    sDesc.Replace(L"\"", L"");



    CDuiString sFile = m_imgx.sFile;
    int nDot = sFile.ReverseFind(L'.');
    CDuiString sExt = nDot >= 0 ? sFile.Mid(nDot) : L"";
    sExt.MakeLower();
    bool bEsd = (sExt == L".esd");
    CDuiString sComp = bEsd ? L"recovery" : L"max";
    int nSlash = sFile.ReverseFind(L'\\');
    CDuiString sDir = nSlash >= 0 ? sFile.Mid(0, nSlash + 1) : CDuiString(L"");
    CDuiString sTmp = sDir + (bEsd ? L"_imgx_meta.esd" : L"_imgx_meta.wim");
    int nCount = static_cast<int>(m_imgx.vIdx.size());



    // [R16][T10] 批量解析+二次确认机械抽取为 ImxMetaBatchPhase
    std::vector<int> vBatchIdx;
    bool bBatchMeta = false;
    if (!ImxMetaBatchPhase(sName, nCount, vBatchIdx, bBatchMeta)) return;



    // ===== wimgapi 原生元信息修改（禁止 dism 批处理兜底）=====
    if (!m_pImgSvc->WimReady())
    {
        ::MessageBoxW(m_hWnd, L"wimgapi.dll 不可用，无法修改映像元信息。",
                      StringMgr.Get("toolbox.edit_img"), MB_OK | MB_ICONERROR);
        return;
    }
    // ===== [2026-10-12] 后台导出重建修改元信息 + 真实进度，完成弹窗并关闭 =====
    // [R7] 作业体下沉 IIsoImageService::MakeMetaRenameJob
    std::vector<toolsvc::TMetaItem> vItems;
    for (int i = 0; i < nCount; ++i)
    {
        bool bSelected = false;
        if (bBatchMeta)
        {
            for (size_t k = 0; k < vBatchIdx.size(); ++k)
                if (vBatchIdx[k] == (i + 1)) { bSelected = true; break; }
        }
        else
        {
            bSelected = (i == m_nSel);
        }
        toolsvc::TMetaItem it;
        it.nIdx = (uint32_t)m_imgx.vIdx[i].nIndex;
        it.bSel = bSelected;
        vItems.push_back(it);
    }
    DWORD dwComp = bEsd ? toolsvc::IMG_COMPRESS_LZMS : toolsvc::IMG_COMPRESS_LZX;
    CDuiString sOk;
    sOk.Format(L"映像信息修改完成。\r\n\r\n映像文件：%s", sFile.GetData());
    RunImageXJob(L"正在修改映像信息（导出重建），请勿关闭窗口…",
        StringMgr.Get("toolbox.edit_img"), sOk, true /*dirty*/, true /*close*/,
        m_pImgSvc->MakeMetaRenameJob(sFile.GetData(), sTmp.GetData(),
                                     sName.GetData(), sDesc.GetData(),
                                     dwComp, vItems));
}
// [R16][T10] 自 ImageXMetaOk 机械抽取：批量索引解析+二次确认（用户取消返回 false）
bool CToolDialogPage::ImxMetaBatchPhase(const CDuiString& sName, int nCount,
                                        std::vector<int>& vBatchIdx, bool& bBatchMeta)
{
    // ===== P1-5: 批量元信息修改 =====
    // 解析 m_imgx.sBatchList 得到批量索引列表；空则退化为单索引 m_nSel


    if (!m_imgx.sBatchList.IsEmpty())
    {
        ParseIndexList(m_imgx.sBatchList, vBatchIdx);
        SortUniqueIndex(vBatchIdx);
        std::vector<int> vValid;
        for (size_t k = 0; k < vBatchIdx.size(); ++k)
            if (vBatchIdx[k] >= 1 && vBatchIdx[k] <= nCount) vValid.push_back(vBatchIdx[k]);
        vBatchIdx = vValid;
    }
    bBatchMeta = !vBatchIdx.empty();



    // 批量时二次确认
    if (bBatchMeta && static_cast<int>(vBatchIdx.size()) > 1)
    {
        CDuiString sSel;
        for (size_t k = 0; k < vBatchIdx.size(); ++k)
        {
            if (k > 0) sSel += L",";
            CDuiString sN;
            sN.Format(L"%d", vBatchIdx[k]);
            sSel += sN;
        }
        CDuiString sMsg;
        sMsg.Format(StringMgr.Get("misc.hec3c"),
                    sName.GetData(), sSel.GetData());
        if (::MessageBoxW(m_hWnd, sMsg.GetData(),
                          StringMgr.Get("toolbox.edit_img"),
                          MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return false;
    }
    return true;
}




// ============================================================
// 9 ISO生成器
// ============================================================
void CToolDialogPage::BuildEsdWiz()
{
    // 顶部描述
    CVerticalLayoutUI* pDesc = std::make_unique<CVerticalLayoutUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pDesc->SetFixedHeight(28);
    pDesc->SetAttribute(L"inset", L"2,4,2,4");
    CTextUI* pDescTxt = MkText(StringMgr.Get("esdwiz.desc"), 1, 0xFF606060);
    pDescTxt->SetTextPadding(RECT{ 0, 0, 0, 0 });
    pDesc->Add(pDescTxt);
    m_pBody->Add(pDesc);
    CControlUI* pGap0 = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pGap0->SetFixedHeight(6);
    m_pBody->Add(pGap0);
    // 源 ESD 路径
    CTextUI* pLblSrc = MkText(StringMgr.Get("esdwiz.usesrc"), 1, 0xFF333333);
    pLblSrc->SetFixedHeight(24);
    m_pBody->Add(pLblSrc);
    CHorizontalLayoutUI* pSrcRow = std::make_unique<CHorizontalLayoutUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pSrcRow->SetFixedHeight(38);
    CEditUI* pSrc = MkEdit(L"edt_esdwiz_src");
    m_esd.pSrcEdit = pSrc;
    pSrcRow->Add(pSrc);
    CControlUI* pGW1 = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via pSrcRow->Add
    pGW1->SetFixedWidth(12);
    pSrcRow->Add(pGW1);
    CButtonUI* pBrowse = MkBtn(StringMgr.Get("general.browse"), L"btn_esdwiz_browse", false);
    pBrowse->SetFixedWidth(104);
    pSrcRow->Add(pBrowse);
    m_pBody->Add(pSrcRow);
    CControlUI* pGap1 = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pGap1->SetFixedHeight(8);
    m_pBody->Add(pGap1);
    // 索引表格（12 行）
    std::vector<TGridCol> cols;
    cols.push_back(TGridCol(StringMgr.Get("misc.sb271e427"), 60));
    cols.push_back(TGridCol(StringMgr.Get("misc.sfdf6f7f6"), 240));
    cols.push_back(TGridCol(StringMgr.Get("misc.sc1835196"), 130));
    cols.push_back(TGridCol(StringMgr.Get("misc.s696f5a97"), -1));
    m_pBody->Add(MkGrid(cols, 12, 32, true));
    CControlUI* pGap2 = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pGap2->SetFixedHeight(8);
    m_pBody->Add(pGap2);
    EsdWizBuildKeyRow();
    // 手动输入框（默认隐藏）
    CEditUI* pManual = MkEdit(L"edt_esdwiz_keymanual");
    pManual->SetFixedHeight(36);
    pManual->SetVisible(false);
    m_pBody->Add(pManual);
    CControlUI* pGap3 = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pGap3->SetFixedHeight(8);
    m_pBody->Add(pGap3);
    // 目标 WIM
    CTextUI* pLblDst = MkText(L"目标 WIM 映像 (*.wim)", 1, 0xFF333333);
    pLblDst->SetFixedHeight(24);
    m_pBody->Add(pLblDst);
    CHorizontalLayoutUI* pDstRow = std::make_unique<CHorizontalLayoutUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pDstRow->SetFixedHeight(38);
    pDstRow->Add(MkEdit(L"edt_esdwiz_dst"));
    CControlUI* pGW2 = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via pDstRow->Add
    pGW2->SetFixedWidth(12);
    pDstRow->Add(pGW2);
    CButtonUI* pDstB = MkBtn(StringMgr.Get("general.browse"), L"btn_esdwiz_dst", false);
    pDstB->SetFixedWidth(104);
    pDstRow->Add(pDstB);
    m_pBody->Add(pDstRow);
    // 底部按钮
    AddBottomRow(MkBtn(StringMgr.Get("esdwiz.export"), L"btn_esdwiz_export", true),
                 MkBtn(StringMgr.Get("general.cancel"), L"btn_cancel", false));
    // 密钥数据留痕（证明 <ESDDecryptKey> 数据源已接入）
    {
        CDuiString sLog;
        sLog.Format(L"[esdwiz] 已加载 ESD 解密密钥数: %d",
                    static_cast<int>(m_pImgSvc->EsdKeys().size()));
        m_pHost->HostLog(sLog);
    }
    // 外壳右键 /ESDDECRYPT 场景：宿主经 ToolOpenArgs 预填源 ESD，
    // 自动填入并加载索引（[R16][T4] 预填数据由 IIsoImageService 持有）。
    if (m_pImgSvc->HasInitialEsd() && !m_pImgSvc->InitialEsdFile().empty())
    {
        m_esd.sFile = m_pImgSvc->InitialEsdFile().c_str();
        CEditUI* pSrcE = static_cast<CEditUI*>(m_pm.FindControl(L"edt_esdwiz_src"));
        if (pSrcE) pSrcE->SetText(m_esd.sFile.GetData());
        EsdWizReload(false);
    }
}
// [R16][T10] 自 BuildEsdWiz 机械抽取：解密密钥下拉区
void CToolDialogPage::EsdWizBuildKeyRow()
{
    // 解密密钥
    CTextUI* pLblKey = MkText(StringMgr.Get("esdwiz.keyhint"), 1, 0xFF333333);
    pLblKey->SetFixedHeight(24);
    m_pBody->Add(pLblKey);
    CHorizontalLayoutUI* pKeyRow = std::make_unique<CHorizontalLayoutUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pKeyRow->SetFixedHeight(38);
    CComboUI* pKey = MkCombo(L"cmb_esdwiz_key", 160);
    m_esd.pKey = pKey;
    {
        CListLabelElementUI* pAuto = std::make_unique<CListLabelElementUI>().release(); // [C++17 R3] framework-owned via pKey->Add
        pAuto->SetFixedHeight(28);
        pAuto->SetText(StringMgr.Get("esdwiz.keyauto"));
        pKey->Add(pAuto);
        CListLabelElementUI* pManual = std::make_unique<CListLabelElementUI>().release(); // [C++17 R3] framework-owned via pKey->Add
        pManual->SetFixedHeight(28);
        pManual->SetText(StringMgr.Get("esdwiz.keymanual"));
        pKey->Add(pManual);
        pKey->SelectItem(0);
    }
    pKeyRow->Add(pKey);
    m_pBody->Add(pKeyRow);
}
void CToolDialogPage::EsdWizBrowse()
{
    CDuiString sFile;
    if (!m_pHost->HostOpenFile(
            sFile,
            L"ESD 映像 (*.esd)\0*.esd\0所有文件 (*.*)\0*.*\0",
            StringMgr.Get("esdwiz.usesrc"))) return;
    CEditUI* pE = static_cast<CEditUI*>(m_pm.FindControl(L"edt_esdwiz_src"));
    if (pE) pE->SetText(sFile.GetData());
    m_esd.sFile = sFile;
    EsdWizReload(true);
}
void CToolDialogPage::EsdWizReload(bool bAlert)
{
    // [R127-A] 读取与提示提取为 tdlg::ReloadWimIndex
    //   （原与 MountReload 逐字重复，仅成员名与文案键不同）
    if (!tdlg::ReloadWimIndex(m_pHost, m_hWnd, bAlert, m_esd.sFile, m_esd.vIdx,
                              StringMgr.Get("esdwiz.nosrc"),
                              StringMgr.Get("esdwiz.noidx"),
                              StringMgr.Get("esdwiz.title"))) return;
    EsdWizFillRows();
}
void CToolDialogPage::EsdWizFillRows()
{
    // [R127-A] 逐格填充提取为 tdlg::FillWimIndexRows
    //   （原与 MountFillRows 逐字重复，唯一差异是索引数组成员名）
    tdlg::FillWimIndexRows(m_vRows, m_esd.vIdx);
    SetGridSelection(0);
}
void CToolDialogPage::EsdWizExport()
{
    if (m_esd.sFile.IsEmpty() || m_esd.vIdx.empty())
    {
        ::MessageBoxW(m_hWnd, StringMgr.Get("esdwiz.nosrc"),
                      StringMgr.Get("esdwiz.title"), MB_OK | MB_ICONWARNING);
        return;
    }
    CEditUI* pDst = static_cast<CEditUI*>(m_pm.FindControl(L"edt_esdwiz_dst"));
    CDuiString sDst = pDst ? pDst->GetText() : CDuiString(L"");
    sDst.Trim();
    if (sDst.IsEmpty())
    {
        ::MessageBoxW(m_hWnd, StringMgr.Get("esdwiz.nodst"),
                      StringMgr.Get("esdwiz.title"), MB_OK | MB_ICONWARNING);
        return;
    }
    // 密钥：手动输入 / 内置自动（Data.xml <ESDDecryptKey>）
    std::vector<std::wstring> vKeys;
    if (m_esd.pKey && m_esd.pKey->GetCurSel() == 1)
    {
        CEditUI* pManual = static_cast<CEditUI*>(
            m_pm.FindControl(L"edt_esdwiz_keymanual"));
        CDuiString sKey = pManual ? pManual->GetText() : CDuiString(L"");
        sKey.Trim();
        if (sKey.IsEmpty())
        {
            ::MessageBoxW(m_hWnd, StringMgr.Get("esdwiz.nokey"),
                          StringMgr.Get("esdwiz.title"), MB_OK | MB_ICONWARNING);
            return;
        }
        vKeys.push_back(std::wstring(sKey.GetData()));
    }
    else
    {
        // [R16][T4] 密钥已由 Open 注入 IIsoImageService（wstring 形态，直接取用）。
        vKeys = m_pImgSvc->EsdKeys();
    }
    if (vKeys.empty())
    {
        ::MessageBoxW(m_hWnd, L"未提供 ESD 解密私钥（Data.xml 无 ESDDecryptKey），请改用手动输入。",
                      StringMgr.Get("esdwiz.title"), MB_OK | MB_ICONWARNING);
        return;
    }
    CDuiString sMsg;
    sMsg.Format(StringMgr.Get("esdwiz.confirm"), sDst.GetData());
    if (::MessageBoxW(m_hWnd, sMsg.GetData(), StringMgr.Get("esdwiz.title"),
                      MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
        return;
    // [ESD 自主解密 2026-10-08] 知识库 41 §26：CryptoAPI RSA+AES-CBC
    // 进程内解密，不再生成 dism /Export-Image 批处理。
    if (m_bTaskRunning) return;   // 防重入
    // [C++17 阶段3] 线程 ctx 兼 Post 载荷：所有权经 WM_TDLG_ESDDONE 移交 OnEsdTaskDone，Post 失败线程内自删
    auto ctx = std::make_unique<TEsdTaskCtx>(); // [C++17 R3] ownership to EsdDecryptThread on successful create
    TEsdTaskCtx* pCtx = ctx.get();
    pCtx->hWnd  = m_hWnd;
    pCtx->pSvc  = m_pImgSvc;   // [R16][T2] shared_ptr 副本：弹窗析构后线程仍可安全使用服务
    pCtx->sSrc  = std::wstring(m_esd.sFile.GetData());
    pCtx->sDst  = std::wstring(sDst.GetData());
    pCtx->vKeys = vKeys;
    pCtx->bOk   = false;
    // [rb28 2026-10-13] 解密过程（整文件 CopyFile + CryptoAPI 就地解密）
    // 无可靠的百分比来源：旧的确定态进度条永远停在 0%，看起来像卡死。
    // 改为 marquee 不确定滚动条，明确表达“正在忙、请稍候”。
    BeginTaskShield(L"正在解密 ESD（整文件复制 + CryptoAPI 就地解密），请稍候……",
                    true);
    // [FIX-P2K] 收编 BackgroundJobs：ESD 解密中退出时由 WaitAll 覆盖，
    // 不再留半截解密文件（原 SafeCreateThread 直发不在退出等待集合）。
    HANDLE hThread = BackgroundJobs::Start(&CToolDialogPage::EsdDecryptThread, pCtx, "esd-decrypt");
    if (!hThread)
    {
        EndTaskShield();
        ctx.reset(); // [C++17 R3] delete ctx at the original failure-cleanup site
        ::MessageBoxW(m_hWnd, L"启动解密任务失败。",
                      StringMgr.Get("esdwiz.title"), MB_OK | MB_ICONERROR);
    }
    else
    {
        ctx.release(); // [C++17 R3] thread proc deletes via its unique_ptr guard; handle owned by BackgroundJobs
    }
}
// ============================================================
// [R-B44] 原版 DLGTEMPLATEEX 补齐三模态：
//   ID135 超链接列表（列表 + 5 个 SysLink）
//   ID137 双路径选择器（Edit×2 + 浏览×2）
//   ID140 删除确认列表（危险项清单）
// 均 DuiLib 动态构建，不依赖 PE 资源；宿主注入数据、收集结果。
// ============================================================
// 内部工具：构造带下划线的可点击链接（SysLink 语义）
