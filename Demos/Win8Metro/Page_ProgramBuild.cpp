#include "stdafx.h"
#include "ThemeTokens.h"   // [R201] 强调色令牌（Win10/11 均跟随系统主题色）
#include "OfflineSession.h"  // 离线会话经独立组件访问
// ============================================================
// Page_ProgramBuild.cpp —— [R76] 程序和功能页"行构建组"独立编译单元
//
// 从 Page_Program.cpp 切出的两段（函数体零逻辑改动）：
//   1) 行构建三段：ProgBuildRow / ProgBuildNameText / ProgBuildMetaText
//      （R14-03 拆解，R70 追加批量卸载复选框，R34 tooltip 四要素）
//   2) fix.log 预览纯函数：SplitFixLogLines / LogFixLogTail（N11 T2）
// 两段均为文件内 static / 成员实现，切片外无调用者（依赖探查 0 共享）。
// 成员函数通过 CPageProgram 声明访问，不改类布局、不改业务逻辑。
// ============================================================
#include "Page_Program.h"
#include "ProgramCacheService.h"
#include "SafeThread.h"
#include "WmMessages.h"
#include "NotifyBox.h"


#include "IPageHost.h"           // HostPageHost()->PageById（prog_pages Tab1 代理）
#include "CustomControls.h"      // CClickRowUI（prow_%d 行）
#include "StringManager.h"       // StringMgr 本地化宏
#include "FrameWnd_Util.h"       // EnsureIconCacheDirectory/EnsureIconPngReady
#include "Registry.h"            // RegGetString（ChangeCommandLine 读取）
#include "DataEngine.h"          // [P0] WinPE 离线会话门控
#include <memory>                // [C++17 R3] make_unique
using namespace DuiLib;
using i18n::CStringManager;      // 便捷：直接写 CStringManager 不用前缀


// [R14-03] Build 拆解：行构建（复选框 + tooltip + 名称列 + 元数据列）
void CPageProgram::ProgBuildRow(const ProgEntry& e, size_t k)
{
    auto pRow = std::make_unique<CClickRowUI>(); // [C++17 R3]
    pRow->SetFixedHeight(32);
    pRow->SetBkColor(tokens::Token(tokens::EToken::Surface));
    RECT rcInset = { 10, 0, 0, 0 };
    pRow->SetInset(rcInset);
    pRow->SetChildPadding(0);
    CDuiString sRowName;
    sRowName.Format(L"prow_%d", k);
    pRow->SetName(sRowName.GetData());
    // [R70] 批量卸载复选框（每行首个子控件，与 prow_%d 一一对应）
    auto pCk = std::make_unique<CBoxCheckUI>(); // [C++17 R3] [BOX-CHECK] 自绘方框
    CDuiString sCkName;
    sCkName.Format(L"prowck_%d", k);
    pCk->SetName(sCkName.GetData());
    pCk->SetFixedWidth(36);
    pCk->SetFixedHeight(32);
    pCk->SetAttribute(L"valign", L"vcenter");
    pRow->Add(pCk.release()); // [C++17 R3] ownership transferred to DuiLib
    // R34: tooltip 加发布者 / 签名 / 安装位置 / 卸载命令
    CDuiString sTip = e.sName;
    if (!e.sPublisher.IsEmpty())  { sTip += StringMgr.Get("misc.h752c"); sTip += e.sPublisher; }
    sTip += StringMgr.Get("misc.h269a");
    if (e.bMsft) sTip += L"Microsoft";
    else if (e.bSigned) sTip += StringMgr.Get("misc.h15a1");
    else sTip += StringMgr.Get("misc.hac5b");
    if (!e.sInstallLoc.IsEmpty()) { sTip += StringMgr.Get("misc.hed13"); sTip += e.sInstallLoc; }
    // [R47] 追加安装图标基名（作为可视"图标"提示）
    if (!e.sIconBase.IsEmpty())   { sTip += StringMgr.Get("misc.h9b6c");     sTip += e.sIconBase; }
    if (!e.sUninstall.IsEmpty())  { sTip += StringMgr.Get("misc.h9c8f"); sTip += e.sUninstall; }
    pRow->SetToolTip(sTip.GetData());
    ProgBuildNameText(pRow.get(), e);
    ProgBuildMetaText(pRow.get(), e);
    CVerticalLayoutUI* pList = static_cast<CVerticalLayoutUI*>(m_ctx.HostFindControl(L"prog_apps_list"));
    if (pList) pList->Add(pRow.release()); // [C++17 R3] ownership transferred to DuiLib (freed if list missing)
    // [R9] 行缓存：记录已构建行的指针与预计算搜索串，供过滤/选中 O(1) 命中，
    // 不再逐行拼接 "prow_%d" 后做名称查找。
    if (k >= m_vRowPtr.size()) m_vRowPtr.resize(k + 1, nullptr);
    if (k >= m_vRowSearch.size()) m_vRowSearch.resize(k + 1);
    m_vRowPtr[k] = pList ? pList->GetItemAt(pList->GetCount() - 1) : nullptr;
    m_vRowSearch[k] = e.sName;
    m_vRowSearch[k].MakeLower();
}
void CPageProgram::ProgBuildNameText(CClickRowUI* pRow, const ProgEntry& e)
{
    // [R47] 名称后附加"· {icon}"图标提示（若已注册 DisplayIcon）
    CDuiString sNameDisp = e.sName;
    if (!e.sIconBase.IsEmpty())
    {
        sNameDisp += L" · ";
        sNameDisp += e.sIconBase;
    }



    auto pName = std::make_unique<CTextUI>(); // [C++17 R3]
    pName->SetText(sNameDisp.GetData());
    pName->SetFixedWidth(0);
    pName->SetTextColor(tokens::Token(tokens::EToken::Ink));
    pName->SetAttribute(L"valign", L"vcenter");
    pName->SetAttribute(L"endellipsis", L"true");
    pName->SetMouseEnabled(false);
    pRow->Add(pName.release()); // [C++17 R3] ownership transferred to DuiLib
}


void CPageProgram::ProgBuildMetaText(CClickRowUI* pRow, const ProgEntry& e)
{
    // [R68] 来源列：按 nRoot 显示 HKLM 64bit / HKLM WOW32 / HKCU
    auto pRoot = std::make_unique<CTextUI>(); // [C++17 R3]
    CDuiString sRoot;
    if (e.nRoot == 0)      sRoot = L"HKLM 64";
    else if (e.nRoot == 1) sRoot = L"HKLM 32";
    else                      sRoot = L"HKCU";
    pRoot->SetText(sRoot.GetData());
    pRoot->SetFixedWidth(80);
    // [R223] 0xFF8B5A2B was a fixed brown: on a dark surface it drops to ~2.6:1.
    //   Keep the semantic (user-scope stands out from machine-scope) but take the
    //   colour from the theme: HKCU -> Warn, HKLM -> Accent.
    pRoot->SetTextColor(e.nRoot == 2 ? tokens::Token(tokens::EToken::Warn)
                                     : tokens::Token(tokens::EToken::Accent));
    pRoot->SetAttribute(L"align", L"center");
    pRoot->SetAttribute(L"valign", L"vcenter");
    pRoot->SetAttribute(L"font", L"6");
    pRoot->SetMouseEnabled(false);
    pRow->Add(pRoot.release()); // [C++17 R3] ownership transferred to DuiLib
    // [R119-2] 位数列：HKLM 64 = x64；HKLM WOW6432Node = x86；HKCU = 用户级
    auto pArch = std::make_unique<CTextUI>(); // [C++17 R3]
    pArch->SetText(e.nRoot == 0 ? L"x64" : (e.nRoot == 1 ? L"x86" : L"U"));
    pArch->SetFixedWidth(56);
    // [R223] Same problem for the bitness column: 0xFF5C8D32 (x86) / 0xFF8B5A2B (user)
    //   were fixed light-theme values. Map to Success / Warn so the three states stay
    //   distinguishable in both themes.
    pArch->SetTextColor(e.nRoot == 0 ? tokens::Token(tokens::EToken::Accent)
                    : (e.nRoot == 1 ? tokens::Token(tokens::EToken::Success)
                                    : tokens::Token(tokens::EToken::Warn)));
    pArch->SetAttribute(L"align", L"center");
    pArch->SetAttribute(L"valign", L"vcenter");
    pArch->SetAttribute(L"font", L"6");
    pArch->SetMouseEnabled(false);
    pRow->Add(pArch.release()); // [C++17 R3] ownership transferred to DuiLib



    // R34: 发布者列
    auto pPub = std::make_unique<CTextUI>(); // [C++17 R3]
    pPub->SetText(e.sPublisher.IsEmpty() ? L"-" : e.sPublisher.GetData());
    pPub->SetFixedWidth(180);
    pPub->SetTextColor(tokens::Token(tokens::EToken::InkSecondary));
    pPub->SetAttribute(L"valign", L"vcenter");
    pPub->SetAttribute(L"endellipsis", L"true");
    pPub->SetMouseEnabled(false);
    pRow->Add(pPub.release()); // [C++17 R3] ownership transferred to DuiLib



    // R34: 签名状态列（三态徽标）
    auto pSig = std::make_unique<CTextUI>(); // [C++17 R3]
    CDuiString sSig;
    if (e.bMsft)         { sSig = L"MFT";  }   // Microsoft
    else if (e.bSigned)  { sSig = L"SIGNED"; }
    else                    { sSig = L"NONE";   }
    pSig->SetText(sSig.GetData());
    pSig->SetFixedWidth(72);
    // [R223] Signed/unsigned markers were fixed green/red literals. They read fine on
    //   white and badly on #202020, so take the semantic tokens instead.
    pSig->SetTextColor(e.bMsft   ? tokens::Token(tokens::EToken::Accent)
                      : e.bSigned ? tokens::Token(tokens::EToken::Success)
                                  : tokens::Token(tokens::EToken::Danger));
    pSig->SetAttribute(L"align", L"center");
    pSig->SetAttribute(L"valign", L"vcenter");
    pSig->SetMouseEnabled(false);
    pRow->Add(pSig.release()); // [C++17 R3] ownership transferred to DuiLib



    auto pDate = std::make_unique<CTextUI>(); // [C++17 R3]
    pDate->SetText(e.sDate.GetData());
    pDate->SetFixedWidth(100);
    pDate->SetTextColor(tokens::Token(tokens::EToken::Ink));
    pDate->SetAttribute(L"align", L"center");
    pDate->SetAttribute(L"valign", L"vcenter");
    pDate->SetMouseEnabled(false);
    pRow->Add(pDate.release()); // [C++17 R3] ownership transferred to DuiLib



    auto pSize = std::make_unique<CTextUI>(); // [C++17 R3]
    pSize->SetText(e.sSize.GetData());
    pSize->SetFixedWidth(100);
    pSize->SetTextColor(tokens::Token(tokens::EToken::Ink));
    pSize->SetAttribute(L"align", L"center");
    pSize->SetAttribute(L"valign", L"vcenter");
    pSize->SetMouseEnabled(false);
    pRow->Add(pSize.release()); // [C++17 R3] ownership transferred to DuiLib
}
// [N11] T2 抽纯函数：按 \n 拆行，去尾部 \r，丢弃空行
std::vector<CDuiString> SplitFixLogLines(const CDuiString& sContent)
{
    std::vector<CDuiString> vLines;
    const int nLen = sContent.GetLength();
    int nStart = 0;
    for (int ii = 0; ii < nLen; ++ii)
    {
        if (sContent[ii] != L'\n') continue;
        CDuiString sLine = sContent.Left(ii - nStart);
        if (sLine.GetLength() > 0 && sLine[sLine.GetLength() - 1] == L'\r')
            sLine = sLine.Left(sLine.GetLength() - 1);
        if (!sLine.IsEmpty()) vLines.push_back(sLine);
        nStart = ii + 1;
    }
    if (nStart < nLen)
    {
        CDuiString sLast = sContent.Mid(nStart);
        if (sLast.GetLength() > 0 && sLast[sLast.GetLength() - 1] == L'\r')
            sLast = sLast.Left(sLast.GetLength() - 1);
        if (!sLast.IsEmpty()) vLines.push_back(sLast);
    }

    // [L-24] 写侧已改 JSONL，这里把每条记录还原成一行可读文本。
    //   必须兼容两种历史格式：
    //     · 新：{"ts":"...","name":"...","product":"...","cmd":"..."}
    //     · 旧：2026-10-28 12:00:00 | 名称 | GUID | 命令
    //   旧格式原样透传（它本身就是人读的），新格式做字段提取，
    //   这样"升级前写的记录"与"升级后写的记录"在同一个预览里都看得懂。
    for (size_t i = 0; i < vLines.size(); ++i)
    {
        CDuiString& s = vLines[i];
        if (s.GetLength() < 2 || s[0] != L'{') continue;   // 旧格式/噪声：保持原样

        // 取 "key":"value" 的 value；找不到返回空串
        auto Field = [&s](const wchar_t* pszKey) -> CDuiString
        {
            CDuiString sKey = L"\"";
            sKey += pszKey;
            sKey += L"\":";
            const int nAt = s.Find(sKey.GetData());
            if (nAt < 0) return CDuiString();
            int nB = nAt + sKey.GetLength();
            if (nB < s.GetLength() && s[nB] == L'"') ++nB;
            const int nE = s.Find(L"\"", nB);
            if (nE < 0) return CDuiString();
            return s.Mid(nB, nE - nB);
        };

        const CDuiString sTs   = Field(L"ts");
        const CDuiString sName = Field(L"name");
        const CDuiString sProd = Field(L"product");
        const CDuiString sCmd  = Field(L"cmd");

        CDuiString sOut;
        if (!sTs.IsEmpty())   { sOut += sTs;   sOut += L" | "; }
        if (!sName.IsEmpty()) { sOut += sName; sOut += L" | "; }
        if (!sProd.IsEmpty()) { sOut += sProd; sOut += L" | "; }
        sOut += sCmd;
        if (!sOut.IsEmpty()) s = sOut;
    }
    return vLines;
}



// [N11] T2 抽纯函数：末 10 条写入主日志区（空列表回退 misc.h7189）
void LogFixLogTail(IPageContext& ctx, const std::vector<CDuiString>& vLines)
{
    if (vLines.empty())
    {
        ctx.HostLog(StringMgr.Get("misc.h7189"));
        return;
    }
    int nShow = 10;
    if (static_cast<int>(vLines.size()) < nShow) nShow = static_cast<int>(vLines.size());
    CDuiString sSum;
    sSum.Format(StringMgr.Get("misc.hd2b9"), nShow);
    ctx.HostLog(sSum.GetData());
    for (int kk = static_cast<int>(vLines.size()) - nShow;
         kk < static_cast<int>(vLines.size()); ++kk)
    {
        CDuiString sPrefix = L"  ";
        sPrefix += vLines[kk];
        ctx.HostLog(sPrefix.GetData());
    }
}
