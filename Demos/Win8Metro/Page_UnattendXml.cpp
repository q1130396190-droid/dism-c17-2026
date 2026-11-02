// ============================================================
// Page_UnattendXml.cpp —— 应答文件页「MSXML DOM 层 + unattend.xml
// 导入导出族」（从 Page_Unattend.cpp 拆出）。
//
// [R149] Page_Unattend.cpp 空行规范化后仍 1,918 行，规模分 7.23（全仓最低）。
// 本文件承接两条强耦合链路：
//   1) MSXML DOM 层（IDispatch 自动化访问 Msxml2.DOMDocument.6.0）：
//      LoadFile / SelectNodes / ListCount / ListItem / GetNodeText /
//      GetNodeAttr / SchemaCheck —— 均为 static，段外零引用。
//   2) 导入导出族（9 个成员）：Export / Import / ReadFileContent /
//      BackupBeforeImport / SchemaCheck / ResetImportState / ImportByDom /
//      ImportByScan / FinishImport，以及 XML 转义与 pass 映射辅助。
//
// 已提升到 Page_UnattendInternal.h 的共享符号（两 TU 共用）：
//   UcAutoWrap / UcDomCreate / UcDomParseErrorCode / UcDomLoadXmlString /
//   kUcMap / kUcGroupCount  —— 因 ApplyUnattend 与 CopyRowPass 仍在使用。
//
// 搬迁为逐字搬运，零逻辑改动。
// ============================================================

#include "stdafx.h"
#include "SafeEnv.h"   // [B6] 安全环境变量读取：替代弃用 _wgetenv

#include "Page_Unattend.h"
#include "Page_UnattendInternal.h"   // [R149] UcAutoWrap / UcDomCreate / kUcMap 等共享符号
#include "CustomControls.h"          // CTextUI
#include "StringManager.h"
#include "DismCore.h"                // [R-P26 / R5] Rust dismcore：XML 标签段定位 FindXmlTag
#include "FrameWnd_Util.h"           // CopyTextToClipboard
#include "Win32DialogService.h"      // DlgOpenFile / DlgSaveFile

#include <objbase.h>
#include <oleauto.h>
#include <comdef.h>

#include <string>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

using namespace DuiLib;
using i18n::CStringManager;

// [C++17 R6] 本 TU 特征常量：导入时备份保留份数与错误详情入日志的条数上限。
inline constexpr int kUcBackupKeepCount = 5;
inline constexpr int kUcErrDetailLogLimit = 5;
static_assert(kUcBackupKeepCount > 0, "must keep at least one backup");

static int UcDomLoadFile(IDispatch* pDoc, const wchar_t* pszPath,
                         CDuiString& sErrMsg)
{
    VARIANT vArg;
    ::VariantInit(&vArg);
    vArg.vt = VT_BSTR;
    vArg.bstrVal = ::SysAllocString(pszPath ? pszPath : L"");
    VARIANT vResult;
    ::VariantInit(&vResult);
    HRESULT hr = ::UcAutoWrap(DISPATCH_METHOD, &vResult, pDoc,
                              L"load", 1, &vArg);
    ::VariantClear(&vResult);
    ::VariantClear(&vArg);
    if (FAILED(hr)) return 4;
    CDuiString sTmp;
    int nErr = ::UcDomParseErrorCode(pDoc, sTmp);
    if (nErr != 0 && nErr != -1)
    {
        sErrMsg = sTmp;
        return 5;
    }
    return 0;
}

// selectNodes(XPath) 返回节点集合 IDispatch*
static int UcDomSelectNodes(IDispatch* pDoc, const wchar_t* pszXPath,
                            IDispatch** ppList)
{
    if (!pDoc || !ppList) return 6;
    *ppList = nullptr;
    VARIANT vArg;
    ::VariantInit(&vArg);
    vArg.vt = VT_BSTR;
    vArg.bstrVal = ::SysAllocString(pszXPath ? pszXPath : L"");
    VARIANT vResult;
    ::VariantInit(&vResult);
    HRESULT hr = ::UcAutoWrap(DISPATCH_METHOD, &vResult, pDoc,
                              L"selectNodes", 1, &vArg);
    ::VariantClear(&vArg);
    if (FAILED(hr) || vResult.vt != VT_DISPATCH || !vResult.pdispVal)
    {
        ::VariantClear(&vResult);
        return 6;
    }
    *ppList = vResult.pdispVal;   // 所有权转移给调用方
    ::VariantInit(&vResult);
    return 0;
}

// 读取 nodeList.length
static int UcDomListCount(IDispatch* pList, long& nCount)
{
    if (!pList) return 7;
    VARIANT vResult;
    ::VariantInit(&vResult);
    HRESULT hr = ::UcAutoWrap(DISPATCH_PROPERTYGET, &vResult, pList,
                              L"length", 0, nullptr);
    if (FAILED(hr) || vResult.vt != VT_I4)
    {
        ::VariantClear(&vResult);
        return 7;
    }
    nCount = vResult.lVal;
    ::VariantClear(&vResult);
    return 0;
}

// nodeList.item(idx) → 节点 IDispatch*
static int UcDomListItem(IDispatch* pList, int nIndex, IDispatch** ppNode)
{
    if (!pList || !ppNode) return 7;
    *ppNode = nullptr;
    VARIANT vArg;
    ::VariantInit(&vArg);
    vArg.vt = VT_I4;
    vArg.lVal = (long)nIndex;
    VARIANT vResult;
    ::VariantInit(&vResult);
    HRESULT hr = ::UcAutoWrap(DISPATCH_METHOD, &vResult, pList,
                              L"item", 1, &vArg);
    ::VariantClear(&vArg);
    if (FAILED(hr) || vResult.vt != VT_DISPATCH || !vResult.pdispVal)
    {
        ::VariantClear(&vResult);
        return 7;
    }
    *ppNode = vResult.pdispVal;
    ::VariantInit(&vResult);
    return 0;
}

// 读取节点 text 属性
static int UcDomGetNodeText(IDispatch* pNode, CDuiString& sText)
{
    if (!pNode) return 8;
    VARIANT vResult;
    ::VariantInit(&vResult);
    HRESULT hr = ::UcAutoWrap(DISPATCH_PROPERTYGET, &vResult, pNode,
                              L"text", 0, nullptr);
    if (FAILED(hr) || (vResult.vt != VT_BSTR && vResult.vt != (VT_BSTR|VT_BYREF)))
    {
        ::VariantClear(&vResult);
        return 8;
    }
    sText = (const wchar_t*)(_bstr_t(vResult.bstrVal, false));
    ::VariantClear(&vResult);
    return 0;
}

// 读取节点指定属性值（不存在返回 false）
static bool UcDomGetNodeAttr(IDispatch* pNode, const wchar_t* pszAttr,
                             CDuiString& sOut)
{
    if (!pNode) return false;
    VARIANT vArg;
    ::VariantInit(&vArg);
    vArg.vt = VT_BSTR;
    vArg.bstrVal = ::SysAllocString(pszAttr ? pszAttr : L"");
    VARIANT vResult;
    ::VariantInit(&vResult);
    HRESULT hr = ::UcAutoWrap(DISPATCH_PROPERTYGET, &vResult, pNode,
                              L"getAttribute", 1, &vArg);
    ::VariantClear(&vArg);
    bool bOk = SUCCEEDED(hr) && vResult.vt == VT_BSTR;
    if (bOk) sOut = (const wchar_t*)(_bstr_t(vResult.bstrVal, false));
    ::VariantClear(&vResult);
    return bOk;
}

// SchemaCheck：DOM 直连统计 settings / pass / RunSynchronousCommand 数量
// 返回分级码，失败时 sErrMsg 提供原因
static int UcDomSchemaCheck(const wchar_t* pszXml, int& nSettings, int& nPass,
                            int& nCommands, CDuiString& sErrMsg)
{
    nSettings = 0; nPass = 0; nCommands = 0;
    IDispatch* pDoc = nullptr;
    int nCode = ::UcDomCreate(&pDoc);
    if (nCode != 0) return nCode;
    nCode = ::UcDomLoadXmlString(pDoc, pszXml, sErrMsg);
    if (nCode != 0) { pDoc->Release(); return nCode; }
    IDispatch* pSettings = nullptr;
    nCode = ::UcDomSelectNodes(
        pDoc, L"//*[local-name()='settings']", &pSettings);
    if (nCode == 0 && pSettings)
    {
        long n = 0;
        ::UcDomListCount(pSettings, n);
        nSettings = static_cast<int>(n);
        for (long i = 0; i < n; ++i)
        {
            IDispatch* pNode = nullptr;
            if (SUCCEEDED(::UcDomListItem(pSettings, static_cast<int>(i), &pNode)) && pNode)
            {
                CDuiString sPass;
                if (::UcDomGetNodeAttr(pNode, L"pass", sPass)) ++nPass;
                pNode->Release();
            }
        }
        pSettings->Release();
    }
    IDispatch* pCmds = nullptr;
    nCode = ::UcDomSelectNodes(
        pDoc, L"//*[local-name()='RunSynchronousCommand']", &pCmds);
    if (nCode == 0 && pCmds)
    {
        long n = 0;
        ::UcDomListCount(pCmds, n);
        nCommands = static_cast<int>(n);
        pCmds->Release();
    }
    pDoc->Release();
    return 0;
}

// ============ UN-1/UN-2: 预应答 unattend.xml 导入导出 ============

static CDuiString UcXmlEscape(const CDuiString& s)
{
    CDuiString r = s;
    r.Replace(L"&", L"&amp;");
    r.Replace(L"<", L"&lt;");
    r.Replace(L">", L"&gt;");
    r.Replace(L"\"", L"&quot;");
    r.Replace(L"'", L"&apos;");
    return r;
}

static CDuiString UcXmlUnescape(const CDuiString& s)
{
    CDuiString r = s;
    r.Replace(L"&apos;", L"'");
    r.Replace(L"&quot;", L"\"");
    r.Replace(L"&gt;", L">");
    r.Replace(L"&lt;", L"<");
    r.Replace(L"&amp;", L"&");
    return r;
}

// [R150] Export 拆为四个成员（原 163 行 / CC26）。
// 主函数只保留「计数 → 选路径 → 拼 XML → DOM 规范化 → 写盘」的编排顺序，
// 各步骤单独成函数，判定点分散后 CC 降到阈值内。逐字搬迁零逻辑改动。
void CPageUnattend::UcBuildExportXml(CDuiString& sXml)
{
    // 构建 XML
    sXml.Format(L"<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n");
    sXml += L"<unattend xmlns=\"urn:schemas-microsoft-com:unattend\"\r\n";
    sXml += L"          xmlns:wcm=\"http://schemas.microsoft.com/WMIConfig/2002/State\">\r\n";
    for (int n = 0; n < 4; ++n)
    {
        if (m_vUcCmds[n].empty()) continue;
        {
            CDuiString s;
            s.Format(L"  <settings pass=\"%s\">\r\n", kUcMap[n].pszPass);
            sXml += s;
        }
        sXml += L"    <component name=\"";
        sXml += kUcMap[n].pszComp;
        sXml += L"\" processorArchitecture=\"*\" publicKeyToken=\"31bf3856ad364e35\" versionScope=\"nonSxS\">\r\n";
        sXml += L"      <RunSynchronous wcm:action=\"add\">\r\n";
        for (size_t k = 0; k < m_vUcCmds[n].size(); ++k)
        {
            CDuiString sEsc = UcXmlEscape(m_vUcCmds[n][k]);
            sXml += L"        <RunSynchronousCommand wcm:action=\"add\">\r\n";
            CDuiString sPart;
            sPart.Format(L"          <CommandLine>%s</CommandLine>\r\n", sEsc.GetData());
            sXml += sPart;
            sPart.Format(L"          <Description>%s #%d</Description>\r\n",
                         kUcMap[n].pszName, static_cast<int>(k) + 1);
            sXml += sPart;
            sPart.Format(L"          <Order>%d</Order>\r\n", static_cast<int>(k) + 1);
            sXml += sPart;
            sXml += L"        </RunSynchronousCommand>\r\n";
        }
        sXml += L"      </RunSynchronous>\r\n";
        sXml += L"    </component>\r\n";
        sXml += L"  </settings>\r\n";
    }
    sXml += L"</unattend>\r\n";
}

// [R130] MSXML DOM 直连校验 + 规范化序列化：成功则用 DOM 输出替换手拼串，
// 失败回退到原始 sXml，但保留分级日志。
// [R162] 从 UcNormalizeXmlViaDom 抽出：用 MSXML DOM 规范化 XML。
//   返回 true 表示已用 DOM 序列化结果替换 sXml；false 表示回退原串
//   （原因已分级记录：create 失败 WARN、parse 失败 ERROR + Toast）。
//   CoInitializeEx / CoUninitialize 必须与 pDoc 生命周期同域，故一并搬入；
//   留一半在调用方反而会造成"忘了 Uninit"或"提前 Uninit"。
// [R175] 从 UcTryExportViaDom 抽出：DOM 解析成功后读取规范化 xml 串并替换。
//   返回 true 表示成功取到非空串并已回写 sXml。
//   VARIANT 必须 VariantClear —— BSTR 归 COM 分配器管，不释放会漏。
//   取不到（hrXml 失败 / 类型不是 VT_BSTR / 空串）返回 false，调用方保留原 sXml。
[[nodiscard]] bool CPageUnattend::UcTakeDomXml(IDispatch* pDoc, CDuiString& sXml, int nTotal)
{
    VARIANT vXml;
    ::VariantInit(&vXml);
    const HRESULT hrXml = ::UcAutoWrap(DISPATCH_PROPERTYGET, &vXml, pDoc, L"xml", 0, nullptr);
    bool bOk = false;
    if (SUCCEEDED(hrXml) && vXml.vt == VT_BSTR)
    {
        CDuiString sDomXml((const wchar_t*)(_bstr_t(vXml.bstrVal, false)));
        if (!sDomXml.IsEmpty()) { sXml = sDomXml; bOk = true; }
        CDuiString sLog;
        sLog.Format(L"[unattend] DOM export ok: len=%d cmds=%d",
                    static_cast<int>(sDomXml.GetLength()), nTotal);
        m_ctx.HostLog(sLog.GetData());
    }
    ::VariantClear(&vXml);
    return bOk;
}

[[nodiscard]] bool CPageUnattend::UcTryExportViaDom(CDuiString& sXml, int nTotal)
{
    HRESULT hrCom = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool bNeedUninit = SUCCEEDED(hrCom);
    bool bDone = false;
    IDispatch* pDoc = nullptr;
    int nCode = ::UcDomCreate(&pDoc);
    if (nCode == 0 && pDoc)
    {
        CDuiString sDomErr;
        int nXmlCode = ::UcDomLoadXmlString(pDoc, sXml.GetData(), sDomErr);
        if (nXmlCode == 0)
        {
            // [R175] 成功后取 xml 串抽为 UcTakeDomXml，嵌套 4→3
            bDone = UcTakeDomXml(pDoc, sXml, nTotal);
        }
        else
        {
            // DOM 解析失败 → 分级日志 + Toast，但保留原文件写出
            CDuiString sLog;
            sLog.Format(L"[unattend] DOM export parse fail: code=%d %s",
                        nXmlCode, sDomErr.GetData());
            m_ctx.HostLog(sLog.GetData(), static_cast<int>(ELogLevel::LOG_ERROR));
            CDuiString sToast;
            sToast.Format(L"code=%d reason=%s", nXmlCode, sDomErr.GetData());
            m_ctx.HostToast(StringMgr.Get("unattend.s_dom_export_fail_title"),
                            sToast.GetData(), 2);
        }
        pDoc->Release();
    }
    else
    {
        CDuiString sLog;
        sLog.Format(L"[unattend] DOM create fail: code=%d", nCode);
        m_ctx.HostLog(sLog.GetData(), static_cast<int>(ELogLevel::LOG_WARN));
    }
    if (bNeedUninit) ::CoUninitialize();
    return bDone;
}

void CPageUnattend::UcNormalizeXmlViaDom(CDuiString& sXml, int nTotal)
{
    // [R130] MSXML DOM 直连校验 + 规范化序列化
    // 走真实 DOM 解析（loadXML → parseError 校验 → xml 序列化），
    // 成功则用 DOM 输出替换手拼串；失败回退到原始 sXml，但保留分级日志。
    // [R162] 整段抽为 UcTryExportViaDom（嵌套 5→3）
    (void)UcTryExportViaDom(sXml, nTotal);
}

// 写文件 (UTF-8 BOM) + 回读自校验 + 状态条与日志。
// 返回 false 表示写失败（调用方直接返回，不再继续后续步骤）。
// [R177] 从 UcWriteExportFile（61 行 / CC13）抽出：写 UTF-8 BOM 文件。
// 转换失败（nXmlLen<=0）与建文件失败都会走失败路径，二者语义区别在日志/弹框：
//  - 转换失败只记日志（输入问题的可能性更大，用户看日志即可）
//  - 建文件失败额外复制错误详情到剪贴板 + 弹 ERROR 框（用户须立刻知道）
// BOM 三字节必须前置，且 WideCharToMultiByte 传 -1 表示含结尾 NUL（与原实现一致）。
bool CPageUnattend::UcWriteUtf8BomFile(const CDuiString& sPath, const CDuiString& sXml)
{
    const int nXmlLen = ::WideCharToMultiByte(CP_UTF8, 0, sXml.GetData(), -1, nullptr, 0, nullptr, nullptr);
    if (nXmlLen <= 0) { m_ctx.HostLog(StringMgr.Get("unattend.s96af997f")); return false; }
    // [R162] 缓冲改 std::string（同上，消除 BYTE*->char* 别名转换）
    std::string buf(static_cast<size_t>(nXmlLen) + 3, '\0');
    buf[0] = static_cast<char>(0xEF);
    buf[1] = static_cast<char>(0xBB);
    buf[2] = static_cast<char>(0xBF);
    ::WideCharToMultiByte(CP_UTF8, 0, sXml.GetData(), -1,
                          &buf[3], nXmlLen, nullptr, nullptr);
    FileGuard h = ::CreateFile(sPath.GetData(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        // S-15: 复制错误详情到剪贴板（含 GetLastError）
        CDuiString sExpErr2;
        sExpErr2.Format(L"Export failed: path=%s err=%d\n%s",
                        sPath.GetData(), static_cast<int>(::GetLastError()),
                        StringMgr.Get("error.s656c4845"));
        FrameWnd_Util_CopyTextToClipboard(m_ctx.HostHwnd(), sExpErr2.GetData());
        m_ctx.HostAlert(StringMgr.Get("error.export_fail"),
                        StringMgr.Get("error.s656c4845"), MB_ICONERROR);
        return false;
    }
    DWORD nWritten = 0;
    // [R65-AUDIT] 写盘结果必须校验：本函数返回 bool 且调用方据此提示成功，
    //   原实现却不看返回值——磁盘满/只读/杀软拦截时导出文件为空，仍报成功。
    const DWORD cbWant = static_cast<DWORD>(buf.size());
    if (!::WriteFile(h, buf.data(), cbWant, &nWritten, nullptr) || nWritten != cbWant)
    {
        CDuiString sWrErr;
        sWrErr.Format(L"Export write failed: path=%s err=%d\n%s",
                      sPath.GetData(), static_cast<int>(::GetLastError()),
                      StringMgr.Get("error.s656c4845"));
        FrameWnd_Util_CopyTextToClipboard(m_ctx.HostHwnd(), sWrErr.GetData());
        m_ctx.HostAlert(StringMgr.Get("error.export_fail"),
                        StringMgr.Get("error.s656c4845"), MB_ICONERROR);
        return false;
    }
    return true;
}

// [R177] 从 UcWriteExportFile 抽出：回读前 3 字节校验 BOM，生成状态条文案。
// 三种结果：BOM 正确 → 成功文案（带总条数与非空组数）；BOM 错 → 校验失败；
// 文件打不开 → "无法回读"。组数统计只数非空组（与原实现逐字一致）。
CDuiString CPageUnattend::UcVerifyExportBom(const CDuiString& sPath, int nTotal)
{
    FileGuard h2 = ::CreateFile(sPath.GetData(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h2 == INVALID_HANDLE_VALUE) return CDuiString(StringMgr.Get("error.sa543629b"));
    BYTE rd[3] = { 0 };
    DWORD nRd = 0;
    ::ReadFile(h2, rd, 3, &nRd, nullptr);
    const bool bOk = (nRd == 3 && rd[0] == 0xEF && rd[1] == 0xBB && rd[2] == 0xBF);
    if (!bOk) return CDuiString(StringMgr.Get("export.s102dab7d"));
    int nGroups = 0;
    for (int n = 0; n < 4; ++n)
    {
        if (!m_vUcCmds[n].empty()) ++nGroups;
    }
    CDuiString sStatus;
    sStatus.Format(StringMgr.Get("export.s0319a73b"), nTotal, nGroups);
    return sStatus;
}

bool CPageUnattend::UcWriteExportFile(const CDuiString& sPath, const CDuiString& sXml, int nTotal)
{
    // [R177] 写盘与回读校验各抽为独立函数（原内联 47 行，CC 记账 11 个判定点）
    if (!UcWriteUtf8BomFile(sPath, sXml)) return false;
    // [R122] 记录最近一次导出路径（右键"打开导出位置"定位用）
    m_sLastExportPath = sPath;
    // UN-1b: 回读自校验
    CDuiString sStatus = UcVerifyExportBom(sPath, nTotal);
    CTextUI* pSt = static_cast<CTextUI*>(m_ctx.HostFindControl(L"lbl_uc_status"));
    if (pSt) pSt->SetText(sStatus);
    CDuiString sLog;
    sLog.Format(L"[unattend] export -> %s  (%d cmds)",
                sPath.GetData(), nTotal);
    m_ctx.HostLog(sLog.GetData());
    return true;
}

void CPageUnattend::Export()
{
    // 统计总命令数 → 空提示
    int nTotal = 0;
    for (int n = 0; n < 4; ++n) nTotal += static_cast<int>(m_vUcCmds[n].size());
    if (nTotal == 0)
    {
        m_ctx.HostAlert(StringMgr.Get("unattend.s2e183444"),
                        StringMgr.Get("unattend.s57c51941"), MB_ICONINFORMATION);
        return;
    }
    CDuiString sPath;
    if (!Win32DialogService::DlgSaveFile(sPath,
            StringMgr.Get("unattend.sb9246904"),
            StringMgr.Get("unattend.s97b166c9"),
            L"unattend.xml"))
        return;
    CDuiString sXml;
    UcBuildExportXml(sXml);
    UcNormalizeXmlViaDom(sXml, nTotal);
    UcWriteExportFile(sPath, sXml, nTotal);
}

// [R11-01] 深度拆解：原 500 行/d≈16 拆为主函数卫语句 + 7 个纯结构辅助，
// 逐字搬迁原逻辑（备份/Schema 校验/DOM/扫描/UI 重建），零行为改动。
void CPageUnattend::Import()
{
    CDuiString sPath;
    if (!Win32DialogService::DlgOpenFile(sPath,
            StringMgr.Get("misc.sfd0a1c27"),
            StringMgr.Get("unattend.s9bdc9eb0")))
        return;
    // [R31-B] 导入前自动备份：若当前内存有命令，先备份为 unattend.xml.bak（同目录）
    int nBeforeTotal = 0;
    for (int n = 0; n < 4; ++n) nBeforeTotal += static_cast<int>(m_vUcCmds[n].size());
    if (nBeforeTotal > 0)
        BackupBeforeImport(sPath, nBeforeTotal);
    // 读文件（失败已弹错误框并返回）
    CDuiString sContent;
    if (!ReadFileContent(sPath, sContent))
        return;
    // [R40/R50] Schema 结构校验（纯日志，不阻断）
    SchemaCheck(sContent);
    // 重置导入态，保留原命令数
    int nBefore = ResetImportState();
    // [R130] DOM 直连优先；成功直接重建 UI 返回
    if (ImportByDom(sPath, sContent))
    {
        int nTotalDom = 0;
        for (int n = 0; n < 4; ++n) nTotalDom += static_cast<int>(m_vUcCmds[n].size());
        FinishImport(sPath, nBefore, nTotalDom);
        return;
    }
    // 字符串扫描回退路径
    ImportByScan(sContent);
    int nTotal = 0;
    for (int n = 0; n < 4; ++n) nTotal += static_cast<int>(m_vUcCmds[n].size());
    FinishImport(sPath, nBefore, nTotal);
}

// [R11-01] 读取文件字节 + 跳过 BOM + UTF-8 解码
bool CPageUnattend::ReadFileContent(const CDuiString& sPath, CDuiString& outContent)
{
    FileGuard h = ::CreateFile(sPath.GetData(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        m_ctx.HostAlert(StringMgr.Get("error.sfddcd7c6"),
                        StringMgr.Get("error.sa16d7bb0"), MB_ICONERROR);
        return false;
    }
    DWORD nSize = ::GetFileSize(h, nullptr);
    std::vector<BYTE> buf(nSize);
    DWORD nRd = 0;
    ::ReadFile(h, buf.data(), nSize, &nRd, nullptr);
    // 跳过 UTF-8 BOM (EF BB BF)
    BYTE* pStart = buf.data();
    int nSkip = 0;
    if (nSize >= 3 && buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF) nSkip = 3;
    else if (nSize >= 2 && buf[0] == 0xFF && buf[1] == 0xFE) nSkip = 2; // UTF-16 LE BOM
    // 检测编码 (XML有 encoding 属性就用它)
    // 简化处理：直接当 UTF-8 读
    std::vector<WCHAR> wbuf(nSize + 1);
    int nW = MultiByteToWideChar(CP_UTF8, 0, (const char*)(pStart + nSkip),
                                nSize - nSkip, wbuf.data(), static_cast<int>(wbuf.size()));
    outContent = CDuiString(wbuf.data(), nW);
    return true;
}

// [R11-01] [R31-B] 导入前自动备份：把当前内存命令写成 unattend.xml.bak
void CPageUnattend::BackupBeforeImport(const CDuiString& sPath, int nBeforeTotal)
{
    CDuiString sBakPath = sPath;
    // 若已存在 .bak 追加时间戳
    if (::GetFileAttributes(sBakPath.GetData()) != INVALID_FILE_ATTRIBUTES)
    {
        SYSTEMTIME st; ::GetLocalTime(&st);
        CDuiString sTs;
        sTs.Format(L"_%04d%02d%02d_%02d%02d%02d",
                   st.wYear, st.wMonth, st.wDay,
                   st.wHour, st.wMinute, st.wSecond);
        sBakPath += sTs;
    }
    sBakPath += L".bak";
    // 生成备份 XML（复用 Export 逻辑）
    CDuiString sXml = L"<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n";
    sXml += L"<unattend xmlns=\"urn:schemas-microsoft-com:unattend\">\r\n";
    for (int n = 0; n < 4; ++n)
    {
        if (m_vUcCmds[n].empty()) continue;
        CDuiString s;
        s.Format(L"  <settings pass=\"%s\">\r\n", kUcMap[n].pszPass);
        sXml += s;
        sXml += L"    <component name=\"";
        sXml += kUcMap[n].pszComp;
        sXml += L"\" processorArchitecture=\"*\" publicKeyToken=\"31bf3856ad364e35\" versionScope=\"nonSxS\">\r\n";
        sXml += L"      <RunSynchronous wcm:action=\"add\">\r\n";
        for (size_t k = 0; k < m_vUcCmds[n].size(); ++k)
        {
            CDuiString sEsc = UcXmlEscape(m_vUcCmds[n][k]);
            sXml += L"        <RunSynchronousCommand wcm:action=\"add\">\r\n";
            s.Format(L"          <CommandLine>%s</CommandLine>\r\n", sEsc.GetData());
            sXml += s;
            s.Format(L"          <Order>%d</Order>\r\n", static_cast<int>(k) + 1);
            sXml += s;
            sXml += L"        </RunSynchronousCommand>\r\n";
        }
        sXml += L"      </RunSynchronous>\r\n";
        sXml += L"    </component>\r\n";
        sXml += L"  </settings>\r\n";
    }
    sXml += L"</unattend>\r\n";
    int nXmlLen = WideCharToMultiByte(CP_UTF8, 0, sXml.GetData(), -1, nullptr, 0, nullptr, nullptr);
    if (nXmlLen > 0)
    {
        std::string buf(static_cast<size_t>(nXmlLen) + 3, '\0');
        buf[0] = static_cast<char>(0xEF);
        buf[1] = static_cast<char>(0xBB);
        buf[2] = static_cast<char>(0xBF);
        WideCharToMultiByte(CP_UTF8, 0, sXml.GetData(), -1,
                           &buf[3], nXmlLen, nullptr, nullptr);
        FileGuard hB = ::CreateFile(sBakPath.GetData(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hB != INVALID_HANDLE_VALUE)
        {
            DWORD nW = 0;
            // [R99] 备份写失败要留痕：否则用户以为有备份，实际没有
            if (!::WriteFile(hB, buf.data(), static_cast<DWORD>(buf.size()), &nW, nullptr)
                || nW != static_cast<DWORD>(buf.size()))
                m_ctx.HostLog(L"[!] [unattend] 备份写入失败（无回退副本）",
                              static_cast<int>(ELogLevel::LOG_WARN));
            CDuiString sLog;
            sLog.Format(L"[unattend] backup -> %s (%d cmds)",
                        sBakPath.GetData(), nBeforeTotal);
            m_ctx.HostLog(sLog.GetData());
        }
    }
}

// [R11-01] pass → 分组映射（原 Import 内 static 局部表，两辅助共用）
static const struct {
    const wchar_t* pszPass;
    int     nGroup;
} kUcPassToGroup[] = {
    { L"windowsPE",  0 },
    { L"specialize", 1 },
    { L"oobeSystem", 2 },
    { L"auditSystem",3 },
};

// [R11-01] DOM 节点 pass 属性 → 分组索引（未识别返回 -1）
[[nodiscard]] static int UcDomGroupForPass(IDispatch* pSNode)
{
    CDuiString sPass;
    if (!UcDomGetNodeAttr(pSNode, L"pass", sPass)) return -1;
    for (int g = 0; g < 4; ++g)
        if (lstrcmpiW(sPass.GetData(), kUcPassToGroup[g].pszPass) == 0)
            return g;
    return -1;
}

// [R11-01] 从单个 RunSynchronousCommand 节点提取命令文本（CommandLine 优先，回退 Path）
[[nodiscard]] static bool UcDomGetNodeCommand(IDispatch* pCNode, CDuiString& outCmd)
{
    CDuiString sCmd;
    bool bGot = false;
    IDispatch* pCL = nullptr;
    if (SUCCEEDED(::UcDomSelectNodes(
            pCNode, L".//*[local-name()='CommandLine']", &pCL)) && pCL)
    {
        long nCL = 0;
        ::UcDomListCount(pCL, nCL);
        if (nCL > 0)
        {
            IDispatch* pCL0 = nullptr;
            if (SUCCEEDED(::UcDomListItem(pCL, 0, &pCL0)) && pCL0)
            {
                bGot = SUCCEEDED(::UcDomGetNodeText(pCL0, sCmd));
                pCL0->Release();
            }
        }
        pCL->Release();
    }
    if (!bGot)
    {
        IDispatch* pPA = nullptr;
        if (SUCCEEDED(::UcDomSelectNodes(
                pCNode, L".//*[local-name()='Path']", &pPA)) && pPA)
        {
            long nPA = 0;
            ::UcDomListCount(pPA, nPA);
            if (nPA > 0)
            {
                IDispatch* pPA0 = nullptr;
                if (SUCCEEDED(::UcDomListItem(pPA, 0, &pPA0)) && pPA0)
                {
                    bGot = SUCCEEDED(::UcDomGetNodeText(pPA0, sCmd));
                    pPA0->Release();
                }
            }
            pPA->Release();
        }
    }
    if (!bGot || sCmd.IsEmpty()) return false;
    outCmd = sCmd;
    return true;
}

// [R11-01] 收集单个 <settings> 节点内所有 RunSynchronousCommand 到 vCmds
static void UcDomCollectSettingsCmds(IDispatch* pSNode, int nGroup,
                                     std::vector<CDuiString>& vCmds)
{
    IDispatch* pCmds = nullptr;
    int nSelCmd = ::UcDomSelectNodes(
        pSNode, L".//*[local-name()='RunSynchronousCommand']", &pCmds);
    if (nSelCmd != 0 || !pCmds) return;
    long nC = 0;
    ::UcDomListCount(pCmds, nC);
    for (long c = 0; c < nC; ++c)
    {
        IDispatch* pCNode = nullptr;
        if (FAILED(::UcDomListItem(pCmds, static_cast<int>(c), &pCNode)) || !pCNode) continue;
        CDuiString sCmd;
        if (UcDomGetNodeCommand(pCNode, sCmd)) vCmds.push_back(sCmd);
        pCNode->Release();
    }
    pCmds->Release();
}

// [R11-01] [R40/R50] Schema 结构校验 + OOBE Pass 阶段统计（纯日志，不阻断）
// [R150] SchemaCheck 的扫描状态与三段辅助（从 112 行/CC33 的主函数抽出）。
// 原实现把「取标签名 → 计标签数 → 解析 pass 属性 → 汇总打印」全塞在一个
// while 循环里，判定点密集到 CC=33。抽出后主函数只表达扫描骨架与报告。
struct UcSchemaStats
{
    int  nRoot = 0;        // <unattend>
    int  nSettings = 0;    // <settings>
    int  nConfig = 0;      // <configuration>
    int  nCmd = 0;         // <commandline>
    int  nPassSeen = 0;    // 已识别的 pass 属性个数
    bool bPassWPE = false;
    bool bPassSpec = false;
    bool bPassAudit = false;
    bool bPassGen = false;
};

namespace
{

// 四类关键标签各计一数：unattend 为根（>=1 才合法）、configuration>1 判为
// Multi-Config，settings/commandline 仅作统计。精确相等比较，因 sTag 已裁到
// 标签名本身；未知标签静默跳过，避免刷屏。
void UcAccountTag(const CDuiString& sTag, UcSchemaStats& st)
{
    if (sTag == L"UNATTEND")           ++st.nRoot;
    else if (sTag == L"SETTINGS")      ++st.nSettings;
    else if (sTag == L"CONFIGURATION") ++st.nConfig;
    else if (sTag == L"COMMANDLINE")   ++st.nCmd;
}

// pass 属性值 → OOBE 阶段标志；顺带累计 nPassSeen（与原实现一致：
// 只要取到值就计数，即使值不属于已知阶段）。
void UcApplyPassValue(const CDuiString& sPass, UcSchemaStats& st)
{
    CDuiString sPassUp = sPass;
    sPassUp.MakeUpper();
    ++st.nPassSeen;
    // WINDOWSP / WINDOWSPS 均视为 WindowsPE（原实现即两个分支同置 true）
    if (sPassUp == L"WINDOWSP" || sPassUp == L"WINDOWSPS") st.bPassWPE  = true;
    else if (sPassUp == L"SPECIALIZE")                     st.bPassSpec = true;
    else if (sPassUp == L"AUDITSYSTEM")                    st.bPassAudit = true;
    else if (sPassUp == L"GENERALIZE")                     st.bPassGen  = true;
}

// 在标签名结束列到 '>' 之间找 pass="..."，取出引号内的值。
// 任一步越界即放弃（不抛错、不改状态），与原文的层层卫语句等价。
void UcParsePassAttr(const CDuiString& sContent, int pNameEnd, int pGt, UcSchemaStats& st)
{
    int pPassKey = -1;
    for (int ii = pNameEnd; ii < pGt; ++ii)
    {
        if (_wcsnicmp(sContent.GetData() + ii, L"pass=", 5) == 0)
        {
            pPassKey = ii + 5;
            break;
        }
    }
    if (pPassKey < 0 || pPassKey >= pGt) return;
    if (sContent[pPassKey] != L'"' || pPassKey + 1 >= pGt) return;
    const int pEnd = sContent.Find(L'"', pPassKey + 1);
    if (pEnd <= pPassKey || pEnd > pGt) return;
    CDuiString sPass(sContent.GetData() + pPassKey + 1, pEnd - pPassKey - 1);
    UcApplyPassValue(sPass, st);
}

// OOBE 阶段统计日志行："[unattend] oobe: WPE=1 Spec=0 Audit=0 Gen=0 total_pass=N)"
// 注意末尾右括号是原文既有写法（前缀含未闭合的左括号语义），逐字保留。
CDuiString UcBuildOobeLine(const UcSchemaStats& st)
{
    CDuiString s;
    s += L"[unattend] oobe: WPE=";
    s += st.bPassWPE   ? L"1 " : L"0 ";
    s += L"Spec=";
    s += st.bPassSpec  ? L"1 " : L"0 ";
    s += L"Audit=";
    s += st.bPassAudit ? L"1 " : L"0 ";
    s += L"Gen=";
    s += st.bPassGen   ? L"1 " : L"0 ";
    CDuiString sNum;
    sNum.Format(L"total_pass=%d", st.nPassSeen);
    s += sNum;
    s += L")";
    return s;
}
}   // namespace

// [R185] 从 SchemaCheck（55 行 / CC12）抽出：扫描全文填充统计结构。
// 循环内四处 break 的语义逐字保留：
//   · 找不到 '<'        → 结束（尾部残余文本不算标签）
//   · 找不到 '>'        → 结束（残缺标签不计入统计）
//   · 非 SETTINGS 标签  → 跳过属性解析但**不结束**（继续扫下一个标签）
// FetchTagName 的返回值刻意丢弃：未提取到标签名时 sTag 保持空，
// 下方四类标签比较自然全部不命中，统计行为与原实现一致。
void CPageUnattend::UcScanSchemaTags(const CDuiString& sContent, UcSchemaStats& st)
{
    const int nContentLen = sContent.GetLength();
    int nPos = 0;
    while (nPos < nContentLen)
    {
        const int pOpen = sContent.Find(L"<", nPos);
        if (pOpen < 0) break;
        // 取出 '<' 之后的标签名：FetchTagName 会跳过前导空白（含 \r\n\t），
        // 收集到空白/引号/'/'/'>' 为止（空格与 '/' 对应自闭合标签），
        // 转大写后回填 sTag，并把标签名结束列回写 pNameEnd，
        // 供 settings 的 pass="..." 属性解析作为起点——
        // 属性必然出现在标签名之后，故不能从 '<' 处开始扫。
        int pNameEnd = 0;
        CDuiString sTag;
        (void)FetchTagName(sContent.GetData(), nContentLen, pOpen, sTag, pNameEnd);
        UcAccountTag(sTag, st);
        const int pGt = sContent.Find(L'>', pOpen);
        if (pGt < 0) break;
        // [R50] 仅 settings 标签带 pass 属性，其余标签跳过属性解析
        if (sTag == L"SETTINGS") UcParsePassAttr(sContent, pNameEnd, pGt, st);
        nPos = pGt + 1;
    }
}

// [R185] 从 SchemaCheck 抽出：按统计结果判定并输出（日志 + 分级 Toast）。
// 三个输出点顺序与条件逐字保留；nPassSeen==0 时**只输出汇总行就返回**，
// 后续 OOBE 三项检查不执行（原实现同样在此 return）。
void CPageUnattend::UcReportSchemaStats(const UcSchemaStats& st)
{
    // 判定：无 <unattend 根节点 → 错误；有 <configuration> 数 > 1 → Multi-Config
    const bool bValid = (st.nRoot >= 1);
    const bool bMulti = (st.nConfig > 1);
    CDuiString sMsg;
    sMsg.Format(L"[unattend] schema: root=%d config=%d settings=%d commands=%d%s",
                st.nRoot, st.nConfig, st.nSettings, st.nCmd,
                bMulti ? L"  (MULTI-CONFIG)" : L"");
    m_ctx.HostLog(sMsg.GetData());
    if (!bValid)
    {
        m_ctx.HostLog(L"[unattend] schema invalid: missing <unattend> root element",
                      static_cast<int>(ELogLevel::LOG_WARN));
        // [R113] Schema 结构校验失败分级 Toast（错误图标 + 日志明细）
        CDuiString sToast;
        sToast.Format(L"root=%d config=%d settings=%d commands=%d",
                      st.nRoot, st.nConfig, st.nSettings, st.nCmd);
        m_ctx.HostToast(StringMgr.Get("unattend.s_schema_invalid_title"),
                        sToast.GetData(), 2);
    }
    // [R50] OOBE Pass 阶段统计
    if (st.nPassSeen == 0) return;
    m_ctx.HostLog(UcBuildOobeLine(st).GetData());
    // 完整 OOBE 检查：三阶段齐全但 settings 数量偏少
    if (st.bPassWPE && st.bPassSpec && st.nSettings < 6)
        m_ctx.HostLog(StringMgr.Get("misc.he46b"), static_cast<int>(ELogLevel::LOG_WARN));
    if (st.bPassGen)
        m_ctx.HostLog(StringMgr.Get("misc.h8331"), static_cast<int>(ELogLevel::LOG_WARN));
}


void CPageUnattend::SchemaCheck(const CDuiString& sContent)
{
    UcSchemaStats st;
    UcScanSchemaTags(sContent, st);            // [R185] 扫描抽为函数
    UcReportSchemaStats(st);                   // [R185] 判定与输出抽为函数
}

// [R11-01] 重置导入态；返回原命令总数（UI 状态标签"导入前 vs 导入后"用）
int CPageUnattend::ResetImportState()
{
    int nBefore = 0;
    for (int n = 0; n < 4; ++n)
    {
        nBefore += static_cast<int>(m_vUcCmds[n].size());
        m_vUcCmds[n].clear();
    }
    m_vUcExpanded.assign(4, false);
    RememberUcSelection(-1, -1);
    return nBefore;
}

// [R11-01] [R130] MSXML DOM 直连解析：优先走真实 DOM（XPath selectNodes + local-name）
// 成功返回 true 且已填充 m_vUcCmds；失败返回 false（调用方回退字符串扫描）。
// [R185] 从 ImportByDom（76 行 / CC12）抽出：DOM Schema 校验 + 分级日志。
// 成功记 INFO 级统计行；失败记 WARN 级并带错误码与原因。
// sDomErr 由调用方传入并在此**复用**（同一变量后续 load 失败分支也会写它）。
void CPageUnattend::UcDomReportSchema(const CDuiString& sContent, CDuiString& sDomErr)
{
    int nSettings = 0, nPass = 0, nCommands = 0;
    const int nSchemaCode = ::UcDomSchemaCheck(
        sContent.GetData(), nSettings, nPass, nCommands, sDomErr);
    CDuiString sLog;
    if (nSchemaCode == 0)
    {
        sLog.Format(L"[unattend] DOM schema: settings=%d pass=%d cmds=%d",
                    nSettings, nPass, nCommands);
        m_ctx.HostLog(sLog.GetData());
        return;
    }
    sLog.Format(L"[unattend] DOM schema fail: code=%d %s",
                nSchemaCode, sDomErr.GetData());
    m_ctx.HostLog(sLog.GetData(), static_cast<int>(ELogLevel::LOG_WARN));
}

// [R185] 从 ImportByDom 抽出：XPath 取所有 <settings>，逐个解析 pass 分组并
// 收集其内部 RunSynchronousCommand 到对应组。
// 每个节点独立 Release（中途 continue 也不漏）；列表句柄在循环后统一 Release。
// 取不到列表（nSel!=0 或 pSettings 为空）时静默返回 —— 与迁移前一致。
void CPageUnattend::UcDomCollectAllSettings(IDispatch* pDoc)
{
    IDispatch* pSettings = nullptr;
    const int nSel = ::UcDomSelectNodes(pDoc, L"//*[local-name()='settings']", &pSettings);
    if (nSel != 0 || !pSettings) return;
    long nCount = 0;
    ::UcDomListCount(pSettings, nCount);
    for (long i = 0; i < nCount; ++i)
    {
        IDispatch* pSNode = nullptr;
        if (FAILED(::UcDomListItem(pSettings, static_cast<int>(i), &pSNode)) || !pSNode)
            continue;
        const int nGroup = UcDomGroupForPass(pSNode);
        if (nGroup >= 0) UcDomCollectSettingsCmds(pSNode, nGroup, m_vUcCmds[nGroup]);
        pSNode->Release();
    }
    pSettings->Release();
}

bool CPageUnattend::ImportByDom(const CDuiString& sPath, const CDuiString& sContent)
{
    bool bDomImportOk = false;
    HRESULT hrCom = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool bNeedUninit = SUCCEEDED(hrCom);
    IDispatch* pDoc = nullptr;
    int nCode = ::UcDomCreate(&pDoc);
    if (nCode == 0 && pDoc)
    {
        CDuiString sDomErr;
        int nFileCode = ::UcDomLoadFile(pDoc, sPath.GetData(), sDomErr);
        if (nFileCode == 0)
        {
            // [R185] Schema 校验与 XPath 收集各抽为独立函数（原内联 35 行，CC 记账 8 点）
            UcDomReportSchema(sContent, sDomErr);
            UcDomCollectAllSettings(pDoc);
            bDomImportOk = true;
        }
        else
        {
            // DOM load 失败 → 分级日志 + Toast，回退文本扫描
            CDuiString sLog;
            sLog.Format(L"[unattend] DOM load fail: code=%d %s",
                        nFileCode, sDomErr.GetData());
            m_ctx.HostLog(sLog.GetData(), static_cast<int>(ELogLevel::LOG_WARN));
            CDuiString sToast;
            sToast.Format(L"code=%d reason=%s", nFileCode, sDomErr.GetData());
            m_ctx.HostToast(StringMgr.Get("unattend.s_dom_import_fail_title"),
                            sToast.GetData(), 1);
        }
        pDoc->Release();
    }
    else
    {
        CDuiString sLog;
        sLog.Format(L"[unattend] DOM create fail: code=%d", nCode);
        m_ctx.HostLog(sLog.GetData(), static_cast<int>(ELogLevel::LOG_WARN));
    }
    if (bNeedUninit) ::CoUninitialize();
    return bDomImportOk;
}

// [R11-01] 字符串扫描回退路径（DOM 失败时逐个匹配 CommandLine）
// [R173] 从 ImportByScan 抽出：把 pass 名映射到命令组号（未识别返回 -1）。
// 映射表 kUcPassToGroup 共 4 项，逐条不区分大小写比较；语义与迁移前一致。
static int UcPassNameToGroup(const CDuiString& sPass)
{
    for (int i = 0; i < 4; ++i)
    {
        if (lstrcmpiW(sPass.GetData(), kUcPassToGroup[i].pszPass) == 0)
            return kUcPassToGroup[i].nGroup;
    }
    return -1;
}

// [R173] 从 ImportByScan 抽出：在 [p, pLimit) 范围内收集全部 <CommandLine> 文本。
// 每条都过 UcXmlUnescape 反解；组号 <0 或文本为空时**照常推进游标**但不入表
// （原实现是 push_back 前加条件，游标推进与之无关），返回后游标已越过最后一条。
void CPageUnattend::UcScanCommandLinesInPass(const wchar_t*& p, const wchar_t* pLimit,
                                             int nGroup)
{
    while (p < pLimit)
    {
        const wchar_t* pCLOpen = wcsstr(p, L"<CommandLine>");
        if (!pCLOpen || pCLOpen >= pLimit) break;
        const wchar_t* pCLContent = pCLOpen + 13;
        const wchar_t* pCLClose = wcsstr(pCLContent, L"</CommandLine>");
        if (!pCLClose) break;
        CDuiString sCmd(pCLContent, static_cast<int>(pCLClose - pCLContent));
        sCmd = UcXmlUnescape(sCmd);
        // 同样找 Description 节点看看 pass 有没有下一个迹象
        if (nGroup >= 0 && !sCmd.IsEmpty())
            m_vUcCmds[nGroup].push_back(sCmd);
        p = pCLClose + 14;
    }
}

// [R11-01] 字符串扫描回退路径（DOM 失败时逐个匹配 CommandLine）
void CPageUnattend::ImportByScan(const CDuiString& sContent)
{
    const wchar_t* p = sContent.GetData();
    const wchar_t* pEnd = p + sContent.GetLength();
    while (p < pEnd)
    {
        // 找 pass 属性
        const wchar_t* pPassTag = wcsstr(p, L"pass=\"");
        if (!pPassTag) break;
        const wchar_t* pPassVal = pPassTag + 6;
        const wchar_t* pPassEnd = wcschr(pPassVal, L'"');
        if (!pPassEnd) break;
        CDuiString sPass(pPassVal, static_cast<int>(pPassEnd - pPassVal));
        const int nCurGroup = UcPassNameToGroup(sPass);   // [R173] 映射抽为函数
        p = pPassEnd + 1;
        // 找当前 pass 的结束边界：下一个 <settings pass=" 标签（或 </unattend> 作为兜底）
        const wchar_t* pPassEndBoundary = wcsstr(p, L"<settings pass=\"");
        const wchar_t* pRootEnd = wcsstr(p, L"</unattend>");
        const wchar_t* pPassEndLimit = pPassEndBoundary ? pPassEndBoundary
                                   : (pRootEnd ? pRootEnd : pEnd);
        // 在当前 pass 范围内收集 CommandLine
        UcScanCommandLinesInPass(p, pPassEndLimit, nCurGroup);
    }
}

// [R11-01] 导入后重建 UI + 更新状态标签 + 写日志（DOM/扫描共用）
void CPageUnattend::FinishImport(const CDuiString& sPath, int nBefore, int nTotal)
{
    for (int n = 0; n < 4; ++n) RebuildBody(n);
    UpdateButtons();
    CDuiString sStatus;
    sStatus.Format(StringMgr.Get("misc.s973ba84f"), nTotal, nBefore);
    CTextUI* pSt = static_cast<CTextUI*>(m_ctx.HostFindControl(L"lbl_uc_status"));
    if (pSt) pSt->SetText(sStatus);
    CDuiString sLog;
    sLog.Format(L"[unattend] import <- %s  (%d cmds)",
                sPath.GetData(), nTotal);
    m_ctx.HostLog(sLog.GetData());
}

// ============================================================
// [R99-RECOVER] 应用配置 XML 构建 / 写盘（从 Page_Unattend.cpp 717..766 行逐字迁入）
// ============================================================

void CPageUnattend::UcBuildApplyXml(CDuiString& sXml)
{
    sXml += L"<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n";
    sXml += L"<unattend xmlns=\"urn:schemas-microsoft-com:unattend\"\r\n";
    sXml += L"          xmlns:wcm=\"http://schemas.microsoft.com/WMIConfig/2002/State\">\r\n";
    for (int n = 0; n < 4; ++n)
    {
        if (m_vUcCmds[n].empty()) continue;
        CDuiString s;
        s.Format(L"  <settings pass=\"%s\">\r\n", kUcMap[n].pszPass);
        sXml += s;
        sXml += L"    <component name=\"";
        sXml += kUcMap[n].pszComp;
        sXml += L"\" processorArchitecture=\"*\" publicKeyToken=\"31bf3856ad364e35\" versionScope=\"nonSxS\">\r\n";
        sXml += L"      <RunSynchronous wcm:action=\"add\">\r\n";
        for (size_t k = 0; k < m_vUcCmds[n].size(); ++k)
        {
            sXml += L"        <RunSynchronousCommand wcm:action=\"add\">\r\n";
            CDuiString sPart;
            sPart.Format(L"          <Path>%s</Path>\r\n", m_vUcCmds[n][k].GetData());
            sXml += sPart;
            sPart.Format(L"          <Order>%d</Order>\r\n", static_cast<int>(k) + 1);
            sXml += sPart;
            sXml += L"        </RunSynchronousCommand>\r\n";
        }
        sXml += L"      </RunSynchronous>\r\n";
        sXml += L"    </component>\r\n";
        sXml += L"  </settings>\r\n";
    }
    sXml += L"</unattend>\r\n";
}

// UTF-8 BOM 写入临时工作副本；失败时不写文件（原实现静默跳过，行为一致）。
// [R99] 返回类型按当前头文件 Page_Unattend.h 的声明对齐为 bool。
// 原备份母本此处为 void，头文件已在后续轮次升级为 bool 以表达写盘成败；
// 函数体保持逐字不变，返回值按实际写盘结果给出。
bool CPageUnattend::UcWriteApplyXml(const CDuiString& sXmlPath, const CDuiString& sXml)
{
    int nLen = ::WideCharToMultiByte(CP_UTF8, 0, sXml.GetData(), -1,
                                     nullptr, 0, nullptr, nullptr);
    if (nLen <= 0) return false;
    std::string buf(static_cast<size_t>(nLen) + 3, '\0');
    buf[0] = static_cast<char>(0xEF);
    buf[1] = static_cast<char>(0xBB);
    buf[2] = static_cast<char>(0xBF);
    ::WideCharToMultiByte(CP_UTF8, 0, sXml.GetData(), -1,
                          &buf[3], nLen, nullptr, nullptr);
    FileGuard h = ::CreateFile(sXmlPath.GetData(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD dwW = 0;
    // [R99] 写盘成败如实返回（原来无条件 return，掩盖了部分写入失败）
    if (!::WriteFile(h, buf.data(), static_cast<DWORD>(buf.size()), &dwW, nullptr))
        return false;
    return dwW == static_cast<DWORD>(buf.size());
}

// 复制到 Panther 并拉起 sysprep。返回 false 表示 sysprep.exe 缺失（已提示用户）。
