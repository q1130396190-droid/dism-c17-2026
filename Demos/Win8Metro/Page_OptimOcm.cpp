#include "stdafx.h"

// ============================================================
// Page_OptimOcm.cpp —— 优化页右键菜单族（从 Page_Optim.cpp 拆出）。
//
// [R134-1] Page_Optim.cpp 原 3685 行，规模分 2.3（全仓最大缺口）。
// 本文件承接 Ocm* 系列：菜单装配、属性框、详情/JSON 复制、增强命令，
// 以及动作标签白名单、Warning 收集/格式化、可恢复判定等私有辅助。
//
// 闭包说明：这些函数全部是 CPageOptim 的成员（声明见 Page_Optim.h），
// 文件级辅助只在 Ocm 族内部使用；唯一跨翻译单元的自由函数
// FindOptimItem 已提升到 Page_OptimInternal.h 的 inline 实现。
//
// 搬迁为逐字搬运，零逻辑改动。
// ============================================================

#include "Page_Optim.h"
#include "Page_OptimInternal.h"   // optim_detail::FindOptimItem

#include "StringManager.h"
#include "NotifyBox.h"          // [R143] ToolSql::HostNotify / HostConfirm（页面统一弹框入口）
#include "ExternalToolService.h"  // [R160] ShellExecFailed（ShellExecute 返回值判错单点）
#include "FrameWnd_Util.h"          // CopyTextToClipboard
#include "FrameWnd_UiHelpers.h"    // UiHelpers_OpenExplorerSelect
#include "CustomControls.h"

#include <string>
#include <vector>

using i18n::CStringManager;

using optim_detail::FindOptimItem;
using optim_detail::DescribeVisibleScopes;

// 计为动作的 XML 标签白名单（大小写不敏感）——原 4601/4602/4606 三处重复的 13 联判断
// [R169] 原为 13 路 lstrcmpiW 的 || 长链（CC 记账 12 个判定点）。
// 改为 constexpr 表 + 线性遍历：语义逐条等价（不区分大小写、任一命中即为动作标签），
// 新增动作标签只需扩表。cname 为 nullptr 时返回 false，与原短路行为一致。
inline constexpr const wchar_t* kOptimActionTags[] = {
    L"RegWrite", L"RegDelete", L"RegMove", L"RegSetEx", L"Custom", L"Exec",
    L"SetDesktop", L"SetServiceRecovery", L"SetAppxPackage",
    L"FileCreateByZIP", L"ImportReg", L"AddDriver", L"Rule",
};

static bool OcmIsActionTag(const wchar_t* cname)
{
    if (!cname) return false;
    for (const wchar_t* pszTag : kOptimActionTags)
    {
        if (lstrcmpiW(cname, pszTag) == 0) return true;
    }
    return false;
}

// ============================================================
// [AUDIT-P2] OcmShowProperties / OcmCopyDetails / OcmCopyJson 三处
// 「遍历可见作用域 × 三种 Warning 节点」的收集块逐字同构（各约 29 行），
// 抽出共用收集器 + 两个格式化器。
//
// 只统一「收集」，不统一「拼装」：属性框/详情是 "· [Scope] 文本"，
// JSON 是对象字面量，两者输出格式不同，强行合并会改变对外输出。
// ============================================================
struct OcmWarnItem
{
    CDuiString     sScope;    // 已解析的作用域名（Current / System）
    const wchar_t* pszType;   // Warning / WarningOn / WarningOff
    CDuiString     sText;     // 已 ResolveText 的正文
};

[[nodiscard]] static std::vector<OcmWarnItem> OcmCollectWarnings(CMarkupNode node,
                                                                 IPageContext& ctx)
{
    static constexpr const wchar_t* kWarnTags[] = { L"Warning", L"WarningOn", L"WarningOff" };
    std::vector<OcmWarnItem> out;
    for (int sc : optim_scope::kColumns)
    {
        CMarkupNode scopeNode = node.GetChild(ctx.DataEngine().ScopeName(sc));
        if (!scopeNode.IsValid()) continue;
        for (int w = 0; w < 3; ++w)
        {
            CMarkupNode wn = scopeNode.GetChild(kWarnTags[w]);
            while (wn.IsValid())
            {
                const wchar_t* pText = wn.GetAttributeValue(L"Text");
                if (!pText) pText = wn.GetValue();
                if (pText && *pText)
                {
                    OcmWarnItem it;
                    it.sScope  = ctx.DataEngine().ScopeName(sc);
                    it.pszType = kWarnTags[w];
                    it.sText   = ctx.DataEngine().ResolveText(pText);
                    out.push_back(it);
                }
                wn = wn.GetSibling();
            }
        }
    }
    return out;
}

// 文本格式："· [Scope] 正文"，条目间以 CRLF 分隔（与属性框/详情原输出一致）
[[nodiscard]] static CDuiString OcmWarningsToText(CMarkupNode node, IPageContext& ctx)
{
    CDuiString s;
    for (const OcmWarnItem& w : OcmCollectWarnings(node, ctx))
    {
        if (!s.IsEmpty()) s += L"\r\n";
        s += L"· [";
        s += w.sScope;
        s += L"] ";
        s += w.sText;
    }
    return s;
}

// [R169] 从 OcmCopyJson 抽出：把已是单行 JSON 的条目逐条追加到数组体。
// 语义逐字迁移：每条后跟 CRLF，末条不带逗号（避免生成非法 JSON 尾逗号）。
static void OcmAppendJsonArray(CDuiString& sOut, const std::vector<CDuiString>& vEntries)
{
    for (size_t k = 0; k < vEntries.size(); ++k)
    {
        sOut += vEntries[k];
        if (k + 1 < vEntries.size()) sOut += L",";
        sOut += L"\r\n";
    }
}

// JSON 条目格式：4 空格缩进的 {"scope":..,"type":..,"text":..}（与 4606 原输出一致）
[[nodiscard]] static std::vector<CDuiString> OcmWarningsToJson(CMarkupNode node,
                                                               IPageContext& ctx)
{
    std::vector<CDuiString> out;
    for (const OcmWarnItem& w : OcmCollectWarnings(node, ctx))
    {
        CDuiString sLine;
        sLine.Format(L"    {\"scope\":\"%s\",\"type\":\"%s\",\"text\":\"%s\"}",
                     w.sScope.GetData(), w.pszType, w.sText.GetData());
        out.push_back(sLine);
    }
    return out;
}


// [RESTORE-SEMANTICS] 递归扫描动作标签。
//
// 必须递归：数据里的形态是 <False><Activate><RegWrite .../></Activate></False>，
// <False> 的**直接子节点只有 <Activate>**，动作在再下一层。初版只遍历直接
// 子节点，导致 bHasDelete/bHasWrite 恒为 false，231 个作用域全部被判为
// "不可恢复"→ 菜单项整片置灰。该回归由机器验证（A2 覆盖度统计）发现：
// 置灰率 100% 与预期 42% 严重不符。
static void OcmScanActionTags(CMarkupNode node, bool& bHasDelete, bool& bHasWrite)
{
    for (CMarkupNode c = node.GetChild(); c.IsValid(); c = c.GetSibling())
    {
        const wchar_t* pn = c.GetName();
        if (pn != nullptr)
        {
            if (lstrcmpiW(pn, L"RegDelete") == 0 || lstrcmpiW(pn, L"RegMove") == 0)
                bHasDelete = true;
            else if (lstrcmpiW(pn, L"RegWrite") == 0 || lstrcmpiW(pn, L"RegSetEx") == 0)
                bHasWrite = true;
        }
        OcmScanActionTags(c, bHasDelete, bHasWrite);
    }
}

// [RESTORE-SEMANTICS] 该作用域是否真的存在"系统默认值"可恢复。
//
// 旧实现只探测作用域盒子里的 <Default> 子分支，而 Data_Win11.xml 的
// 333 个作用域盒子中该子分支出现 **0 次** → bHasDefault 恒为 false →
// "恢复默认值"实际永远等价于"关闭"，与旁边的开关完全重复；用户点它还
// 以为回到了出厂状态。
//
// 判据：<Default> 分支存在，或关闭分支只删不写（删除即回系统默认）。
// 实测（仅界面可见两列，231 个作用域）：可恢复 133 个（57.6%），
// 置灰 98 个（42.4%，其中 18 个关闭分支完全无动作）。
static bool OcmCanRestoreDefault(CMarkupNode node, int nScope)
{
    CMarkupNode box = node.GetChild(DismScopeName(nScope));
    if (!box.IsValid()) return false;
    if (box.GetChild(L"Default").IsValid()) return true;

    CMarkupNode fb = box.GetChild(L"False");
    if (!fb.IsValid()) return false;

    bool bHasDelete = false;
    bool bHasWrite  = false;
    OcmScanActionTags(fb, bHasDelete, bHasWrite);
    return bHasDelete && !bHasWrite;
}

void CPageOptim::ShowOptimContextMenu(int g, int i, int nScope, POINT pt)
{
    const bool bHitRule = (g >= 0 && i >= 0);

    // [RESTORE-SEMANTICS] 只有真正存在默认值可恢复时才让菜单项可用，
    // 其余情况置灰——比点下去什么都没变、或悄悄改成"关闭"更诚实。
    bool bCanRestore = false;
    if (bHitRule)
    {
        CMarkupNode r = m_ctx.DataEngine().GetOptimizationRoot();
        CMarkupNode n = FindOptimItem(r, g, i);
        if (n.IsValid())
            bCanRestore = OcmCanRestoreDefault(n, (nScope >= 0) ? nScope : 0);
    }

    HMENU hMenu = OcmBuildMenu(bHitRule, bCanRestore);

    ::SetForegroundWindow(m_ctx.HostHwnd());
    int iCmd = ::TrackPopupMenu(hMenu,
        TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
        pt.x, pt.y, 0, m_ctx.HostHwnd(), nullptr);
    ::DestroyMenu(hMenu);

    // [R169] 命令分派抽为 OcmDispatchCommand（原 4 段 else-if 链，CC 记账 5 个判定点）
    OcmDispatchCommand(iCmd, bHitRule, g, i, nScope);
}

// [R169] 从 ShowOptimContextMenu 抽出：右键菜单命令分派。
// 判定顺序与条件逐字迁移：4601 属性 / 4602 复制详情 / 4606 复制 JSON /
// 4607-4610 增强命令；每条都要求 bHitRule（未命中规则时一律不响应）。
bool CPageOptim::OcmDispatchCommand(int iCmd, bool bHitRule,
                                                 int g, int i, int nScope)
{
    if (!bHitRule) return false;
    switch (iCmd)
    {
    case 4601: OcmShowProperties(g, i, nScope); return true;
    // [O-11 修复] 原条件是 (iCmd==4601 || iCmd==4602) 走属性框，
    // 导致菜单里「复制」永远弹出属性框、复制功能整条不可达。菜单项承诺的动作必须兑现。
    case 4602: OcmCopyDetails(g, i, nScope);    return true;
    case 4606: OcmCopyJson(g, i, nScope);       return true;
    case 4607: case 4608: case 4609: case 4610:
        // [R114-2] 右键菜单增强：打开位置 / 复制名称 / 复制命令 / 恢复默认值
        OcmHandleEnhancedCmds(iCmd, g, i, nScope);
        return true;
    default: return false;
    }
}

HMENU CPageOptim::OcmBuildMenu(bool bHitRule, bool bCanRestore)
{
    HMENU hMenu = ::CreatePopupMenu();
    ::AppendMenu(hMenu, MF_STRING | (bHitRule ? 0 : MF_GRAYED), 4601, StringMgr.Get("misc.h0fdf"));
    // O-11: "复制" 从 MF_GRAYED 变为 bHitRule 时可用，把 O-10 已拼装的
    // 完整详情字符串写入系统剪贴板（CF_UNICODETEXT）。
    ::AppendMenu(hMenu, MF_STRING | (bHitRule ? 0 : MF_GRAYED), 4602, StringMgr.Get("misc.h61cc"));
    // S-16: 追加 JSON 结构复制（脚本处理友好）
    ::AppendMenu(hMenu, MF_STRING | (bHitRule ? 0 : MF_GRAYED), 4606, StringMgr.Get("misc.h371e"));
    ::AppendMenu(hMenu, MF_SEPARATOR, 0, nullptr);
    // [R114-2] 右键菜单增强：打开位置 / 复制名称 / 复制命令 / 恢复默认值
    ::AppendMenu(hMenu, MF_STRING | (bHitRule ? 0 : MF_GRAYED), 4607, StringMgr.Get("optim.s_menu_openloc"));
    ::AppendMenu(hMenu, MF_STRING | (bHitRule ? 0 : MF_GRAYED), 4608, StringMgr.Get("optim.s_menu_copyname"));
    ::AppendMenu(hMenu, MF_STRING | (bHitRule ? 0 : MF_GRAYED), 4609, StringMgr.Get("optim.s_menu_copycmd"));
    // 4610「恢复默认值」：除命中规则外还要求该作用域真有默认值可恢复
    ::AppendMenu(hMenu, MF_STRING | ((bHitRule && bCanRestore) ? 0 : MF_GRAYED),
                 4610, StringMgr.Get("optim.s_menu_restore"));
    // [G10 2026-10-25] 删除 4603/4604/4605 三项"永久置灰"死菜单。
    //   核查依据：它们取的文案键 misc.h1ff0 / misc.h66fd / misc.h2dd1
    //   在 StringManager.cpp 与 lang/ 语言包中均不存在 —— 即使将来实现，
    //   菜单上显示的也只会是空文本或原样的键名。
    //   保留一项永远点不动的空菜单，比不显示更伤：用户会反复尝试并怀疑程序坏了。
    //   若日后要恢复这些功能，应先补齐文案键与处理器，再一并加回菜单。
    return hMenu;
}

void CPageOptim::OcmShowProperties(int g, int i, int nScope)
{
    CMarkupNode root = m_ctx.DataEngine().GetOptimizationRoot();
    CMarkupNode node = FindOptimItem(root, g, i);
    if (!node.IsValid()) return;
    const CDuiString sName = m_ctx.DataEngine().ResolveText(node.GetAttributeValue(L"Name"));
    const CDuiString sDesc = m_ctx.DataEngine().ResolveText(node.GetAttributeValue(L"Description"));

    // O-10: 属性面板从"名称+说明"扩展为完整详情：
    //   组名 / 项名 / 类型 / 作用域 / 说明 / 启用分支动作数 / 警示汇总
    // [R186] 详情文案与 4602「复制详情」**逐字同构**（已逐段 diff：语言键序列
    // 完全一致，仅本处多一个对话框标题 misc.h4740）。故复用 OcmBuildCopyDetails，
    // 不再各留一份 —— 两处文案一旦分叉，用户看到的"属性框"与"复制的详情"就会不一致。
    const CDuiString sMsg = OcmBuildCopyDetails(node, nScope, sName, sDesc);

    ToolSql::HostNotify(m_ctx.HostHwnd(), sMsg.GetData(), StringMgr.Get("misc.h4740"),
                        MB_OK | MB_ICONINFORMATION);
}

// [R176] 从 OcmCopyDetails（86 行 / CC13）抽出：统计该规则的动作节点总数。
// 遍历可见作用域 × 该作用域下所有子节点，命中动作标签白名单才计数。
// 双层遍历与原实现一致；作用域节点不存在则跳过该列。
int CPageOptim::OcmCountActions(CMarkupNode node)
{
    int nActions = 0;
    for (int sc : optim_scope::kColumns)
    {
        CMarkupNode scopeNode = node.GetChild(m_ctx.DataEngine().ScopeName(sc));
        if (!scopeNode.IsValid()) continue;
        CMarkupNode child = scopeNode.GetChild();
        while (child.IsValid())
        {
            if (OcmIsActionTag(child.GetName())) ++nActions;
            child = child.GetSibling();
        }
    }
    return nActions;
}

// [R176] 从 OcmCopyDetails 抽出：复制到剪贴板的详情文本拼装。
// 段落顺序与语言键逐字保留：标题 → 分组 → 名称 → 类型 → 作用域 → 动作数 →
// 说明 → 警示。Type 缺省为 "Switch"；作用域 <0 时按可见列生成描述（A-2）；
// 警示段仅在非空时追加（不产生多余空行）。
CDuiString CPageOptim::OcmBuildCopyDetails(CMarkupNode node, int nScope,
                                           const CDuiString& sName,
                                           const CDuiString& sDesc)
{
    CDuiString sMsg;
    sMsg += StringMgr.Get("misc.s758ec357");
    CMarkupNode pGroup = node.GetParent();
    CDuiString sGroupName;
    if (pGroup.IsValid())
        sGroupName = m_ctx.DataEngine().ResolveText(pGroup.GetAttributeValue(L"Name"));
    if (!sGroupName.IsEmpty())
    {
        sMsg += StringMgr.Get("misc.hec8d"); sMsg += sGroupName; sMsg += L"\r\n";
    }
    sMsg += StringMgr.Get("misc.h0615"); sMsg += sName; sMsg += L"\r\n";
    CDuiString sType = node.GetAttributeValue(L"Type");
    if (sType.IsEmpty()) sType = L"Switch";
    sMsg += StringMgr.Get("misc.he91f"); sMsg += sType; sMsg += L"\r\n";
    CDuiString sScopeStr;
    if (nScope >= 0) sScopeStr = m_ctx.DataEngine().ScopeName(nScope);
    else             sScopeStr = DescribeVisibleScopes();   // [A-2] 按可见列生成，不再说"全部作用域"
    sMsg += StringMgr.Get("misc.h5342"); sMsg += sScopeStr; sMsg += L"\r\n";

    CDuiString sActCount;
    sActCount.Format(StringMgr.Get("misc.h81a0"), OcmCountActions(node));
    sMsg += sActCount; sMsg += L"\r\n";

    sMsg += StringMgr.Get("misc.s6000de8e");
    sMsg += sDesc.IsEmpty() ? StringMgr.Get("misc.h69d2") : sDesc;
    sMsg += L"\r\n";

    // 警示（文本格式；与属性框共用同一拼装）
    const CDuiString sWarnings = OcmWarningsToText(node, m_ctx);
    if (!sWarnings.IsEmpty())
    {
        sMsg += StringMgr.Get("misc.sa8c26203");
        sMsg += sWarnings;
    }
    return sMsg;
}

void CPageOptim::OcmCopyDetails(int g, int i, int nScope)
{
    // O-11: 把属性详情复制到系统剪贴板
    //   OpenClipboard → EmptyClipboard → SetClipboardData(CF_UNICODETEXT) → CloseClipboard
    //   GlobalAlloc(GMEM_MOVEABLE) 分配内存，SetClipboardData 转移所有权。
    //   任何一步失败都保证 CloseClipboard，不留悬空锁。
    CMarkupNode root = m_ctx.DataEngine().GetOptimizationRoot();
    CMarkupNode node = FindOptimItem(root, g, i);
    if (!node.IsValid()) return;

    // 复用与"属性"命令相同的详情字符串构建逻辑
    const CDuiString sName = m_ctx.DataEngine().ResolveText(node.GetAttributeValue(L"Name"));
    const CDuiString sDesc = m_ctx.DataEngine().ResolveText(node.GetAttributeValue(L"Description"));
    // [R176] 详情文本拼装抽为 OcmBuildCopyDetails（含动作计数 OcmCountActions）
    const CDuiString sMsg = OcmBuildCopyDetails(node, nScope, sName, sDesc);

    // 剪贴板写入（S-15 通用辅助）
    // [UX] 同菜单里其它命令都用 HostToast 回执，这里原来只写日志，
    // 用户点了「复制」看不到任何反馈，会以为菜单没生效。
    // [UX-TEXT] 回执文案必须与真正复制的内容一致。4602 复制的是**规则详情**
    // （名称/类型/作用域/动作数/说明/警示），原先复用了"已复制规则名称"，
    // 用户会以为只复制了名字。这里用专用键 optim.s_copydetail_ok/fail
    // （11 个语言包均已补齐）。不能复用菜单文本键 misc.h61cc——它带
    // "(&C)" 快捷键标记，塞进 toast 会把 "(&C)" 一起显示出来。
    if (FrameWnd_Util_CopyTextToClipboard(m_ctx.HostHwnd(), sMsg.GetData()))
    {
        m_ctx.HostToast(StringMgr.Get("optim.s_copydetail_ok"), sName.GetData(), 0);
        AppendLog(L"[optim] rule info copied to clipboard");
    }
    else
    {
        m_ctx.HostToast(StringMgr.Get("optim.s_copydetail_fail"), sName.GetData(), 2);
        AppendLog(L"[optim] clipboard busy, copy aborted");
    }

    // S-16: 追加复制完整 JSON 结构（结构化、便于脚本处理）
    // 与 4602 共用同一份详情，但序列化格式为 JSON。
    // 独立写入剪贴板会覆盖 4602 的内容，改为 4602 直接构建 JSON。
}

// [R169] 从 OcmCopyJson 抽出：按可见作用域 × 动作标签白名单收集 JSON 条目。
// 双层遍历语义逐字迁移：外层走 optim_scope::kColumns 的可见列，
// 内层走该作用域节点的全部兄弟子节点，命中 OcmIsActionTag 即按
// 4 空格缩进的 {"scope":..,"action":..} 追加。作用域无效则跳过该列。
void CPageOptim::OcmCollectActionEntries(CMarkupNode node, std::vector<CDuiString>& vActions)
{
    for (int sc : optim_scope::kColumns)
    {
        CMarkupNode scopeNode = node.GetChild(m_ctx.DataEngine().ScopeName(sc));
        if (!scopeNode.IsValid()) continue;
        CMarkupNode child = scopeNode.GetChild();
        while (child.IsValid())
        {
            const wchar_t* cname = child.GetName();
            if (OcmIsActionTag(cname))
            {
                CDuiString sScopeName = m_ctx.DataEngine().ScopeName(sc);
                CDuiString sEntry;
                sEntry.Format(L"    {\"scope\":\"%s\",\"action\":\"%s\"}",
                              sScopeName.GetData(), cname);
                vActions.push_back(sEntry);
            }
            child = child.GetSibling();
        }
    }
}

void CPageOptim::OcmCopyJson(int g, int i, int nScope)
{
    // S-16: 复制完整 JSON 结构
    CMarkupNode root = m_ctx.DataEngine().GetOptimizationRoot();
    CMarkupNode node = FindOptimItem(root, g, i);
    if (!node.IsValid()) return;

    CDuiString sName = m_ctx.DataEngine().ResolveText(node.GetAttributeValue(L"Name"));
    CDuiString sDesc = m_ctx.DataEngine().ResolveText(node.GetAttributeValue(L"Description"));
    CMarkupNode pGroup = node.GetParent();
    CDuiString sGroupName;
    if (pGroup.IsValid())
        sGroupName = m_ctx.DataEngine().ResolveText(pGroup.GetAttributeValue(L"Name"));
    CDuiString sType = node.GetAttributeValue(L"Type");
    if (sType.IsEmpty()) sType = L"Switch";
    CDuiString sScopeStr;
    if (nScope >= 0) sScopeStr = m_ctx.DataEngine().ScopeName(nScope);
    else             sScopeStr = DescribeVisibleScopes();   // [A-2] 按可见列生成，不再说"全部作用域"

    // [R169] 动作节点收集抽为 OcmCollectActionEntries（原双层遍历内联，CC 记账 4 个判定点）
    std::vector<CDuiString> vActions;
    OcmCollectActionEntries(node, vActions);

    // 提取警告（JSON 条目；收集逻辑见 OcmCollectWarnings）
    const std::vector<CDuiString> vWarnings = OcmWarningsToJson(node, m_ctx);

    CDuiString sJson;
    // 干净版本（不用 Format 避免参数个数错配）
    sJson = L"{\r\n";
    sJson += L"  \"group\":\""; sJson += sGroupName; sJson += L"\",\r\n";
    sJson += L"  \"name\":\""; sJson += sName; sJson += L"\",\r\n";
    sJson += L"  \"type\":\""; sJson += sType; sJson += L"\",\r\n";
    sJson += L"  \"scope\":\""; sJson += sScopeStr; sJson += L"\",\r\n";
    sJson += L"  \"description\":\""; sJson += sDesc; sJson += L"\",\r\n";
    // [R169] 两个数组的拼装同构，抽为 OcmAppendJsonArray；
    // 分隔符位置（末项无逗号）与换行沿用原实现。
    sJson += L"  \"actions\":[\r\n";
    OcmAppendJsonArray(sJson, vActions);
    sJson += L"  ],\r\n";
    sJson += L"  \"warnings\":[\r\n";
    OcmAppendJsonArray(sJson, vWarnings);
    sJson += L"  ]\r\n";
    sJson += L"}\r\n";

    if (FrameWnd_Util_CopyTextToClipboard(m_ctx.HostHwnd(), sJson.GetData()))
        AppendLog(L"[optim] rule JSON copied to clipboard");
    else
        AppendLog(L"[optim] clipboard busy, JSON copy aborted");
}

void CPageOptim::OcmHandleEnhancedCmds(int iCmd, int g, int i, int nScope)
{
    CMarkupNode root = m_ctx.DataEngine().GetOptimizationRoot();
    CMarkupNode node = FindOptimItem(root, g, i);
    if (!node.IsValid()) return;
    CDuiString sName = m_ctx.DataEngine().ResolveText(node.GetAttributeValue(L"Name"));
    CDuiString sKey, sValue, sCmd;
    OcmResolveCommand(node, nScope, sKey, sValue, sCmd);

    if (iCmd == 4608) { OcmCmdCopyName(sName); return; }

    if (iCmd == 4607) { OcmCmdOpenLocation(sKey, sName); return; }

    if (iCmd == 4609) { OcmCmdCopyCommand(sCmd, sName); return; }

    if (iCmd == 4610) { OcmCmdRestoreDefault(node, g, i, nScope, sName); return; }
}

// [R146] 三种动作的文案装配，从 OcmResolveCommand 提取为具名函数。
// 原实现是 while 循环里的三段 if/else-if 字符串比较，共 12 个判定点；
// 提出后循环体只剩"取名字 → 查表 → 命中即走"，动作语义单独可读、可测。
namespace
{

// <RegWrite Key Value Type Data> → reg add 命令行。
void OcmBuildRegWrite(CMarkupNode child, CDuiString& sKey, CDuiString& sValue, CDuiString& sCmd)
{
    const wchar_t* pK = child.GetAttributeValue(L"Key");
    const wchar_t* pV = child.GetAttributeValue(L"Value");
    const wchar_t* pT = child.GetAttributeValue(L"Type");
    const wchar_t* pD = child.GetAttributeValue(L"Data");
    if (pK) sKey = pK;
    if (pV) sValue = pV;
    CDuiString sTmp;
    sTmp.Format(L"reg add \"%s\" /v \"%s\" /t %s /d \"%s\" /f",
                pK ? pK : L"", pV ? pV : L"(Default)",
                pT ? pT : L"REG_SZ", pD ? pD : L"");
    sCmd = sTmp;
}

// <RegDelete Key Value> → reg delete 命令行。
void OcmBuildRegDelete(CMarkupNode child, CDuiString& sKey, CDuiString& sValue, CDuiString& sCmd)
{
    const wchar_t* pK = child.GetAttributeValue(L"Key");
    const wchar_t* pV = child.GetAttributeValue(L"Value");
    if (pK) sKey = pK;
    if (pV) sValue = pV;
    CDuiString sTmp;
    sTmp.Format(L"reg delete \"%s\" /v \"%s\" /f",
                pK ? pK : L"", pV ? pV : L"(Default)");
    sCmd = sTmp;
}

// <Exec Path Arg> → "path arg" 命令行（Key 记 Path）。
void OcmBuildExec(CMarkupNode child, CDuiString& sKey, CDuiString& sCmd)
{
    const wchar_t* pPath = child.GetAttributeValue(L"Path");
    const wchar_t* pArg  = child.GetAttributeValue(L"Arg");
    if (pPath) sKey = pPath;
    CDuiString sTmp;
    sTmp.Format(L"%s %s", pPath ? pPath : L"", pArg ? pArg : L"");
    sCmd = sTmp.Trim();
}

// 按元素名分派；返回 true 表示已处理（调用方应结束遍历）。
[[nodiscard]] bool OcmBuildCommandFromChild(CMarkupNode child,
                                            CDuiString& sKey, CDuiString& sValue, CDuiString& sCmd)
{
    const wchar_t* cname = child.GetName();
    if (!cname) return false;
    if (lstrcmpiW(cname, L"RegWrite") == 0)  { OcmBuildRegWrite(child, sKey, sValue, sCmd);  return true; }
    if (lstrcmpiW(cname, L"RegDelete") == 0) { OcmBuildRegDelete(child, sKey, sValue, sCmd); return true; }
    if (lstrcmpiW(cname, L"Exec") == 0)      { OcmBuildExec(child, sKey, sCmd);              return true; }
    return false;
}

}   // namespace

// [REFACTOR] 从 OcmHandleEnhancedCmds 提取：解析规则首个动作的 Key/Value/命令行。
// 逐字搬迁零逻辑改动。原实现内联在函数开头，占 48 行、含 5 层嵌套。
void CPageOptim::OcmResolveCommand(CMarkupNode node, int nScope,
                                   CDuiString& sKey, CDuiString& sValue, CDuiString& sCmd)
{
    CMarkupNode scopeBox = node.GetChild(m_ctx.DataEngine().ScopeName(nScope >= 0 ? nScope : 0));
    if (!scopeBox.IsValid()) return;
    CMarkupNode branch = scopeBox.GetChild(L"True");
    if (!branch.IsValid()) return;
    for (CMarkupNode child = branch.GetChild(); child.IsValid(); child = child.GetSibling())
    {
        if (OcmBuildCommandFromChild(child, sKey, sValue, sCmd)) return;
    }
}

// [REFACTOR] 从 OcmHandleEnhancedCmds 提取：iCmd==4608 分支。逐字搬迁零改动。
void CPageOptim::OcmCmdCopyName(const CDuiString& sName)
{
    if (FrameWnd_Util_CopyTextToClipboard(m_ctx.HostHwnd(), sName.GetData()))
        m_ctx.HostToast(StringMgr.Get("optim.s_menu_copyname_ok"), sName.GetData(), 0);
    else
        m_ctx.HostToast(StringMgr.Get("optim.s_menu_copyname_fail"), sName.GetData(), 2);
    AppendLog(L"[optim] rule name copied to clipboard");
    return;
}

// [REFACTOR] 从 OcmHandleEnhancedCmds 提取：iCmd==4607 分支。逐字搬迁零改动。
void CPageOptim::OcmCmdOpenLocation(const CDuiString& sKey, const CDuiString& sName)
{
    // 打开位置：注册表根键走 regedit，文件路径走 Explorer 选中
    if (!sKey.IsEmpty())
    {
        const wchar_t* pk = sKey.GetData();
        // [R169] 原为 10 路 lstrcmpiW 的 || 长链（CC 记账 9 个判定点）。
        // 改为根键短名/长名对照表 + 一次遍历：语义逐条等价（不区分大小写、
        // 任一命中即视为根键），新增根键只需扩表，不再加 || 分支。
        // pk 为 nullptr 时表内比较一律不等 → bRegKey=false，与原短路行为一致。
        static const wchar_t* kRegRootKeys[] = {
            L"HKLM", L"HKCU", L"HKCR", L"HKU", L"HKCC",
            L"HKEY_LOCAL_MACHINE", L"HKEY_CURRENT_USER", L"HKEY_CLASSES_ROOT",
            L"HKEY_USERS", L"HKEY_CURRENT_CONFIG",
        };
        bool bRegKey = false;
        for (const wchar_t* pszRoot : kRegRootKeys)
        {
            if (pk && lstrcmpiW(pk, pszRoot) == 0) { bRegKey = true; break; }
        }
        if (bRegKey)
        {
            HINSTANCE h = ::ShellExecuteW(m_ctx.HostHwnd(), L"open",
                                          L"regedit.exe", pk, nullptr, SW_SHOWNORMAL);
            if (!ExternalToolService::ShellExecFailed(h))
                m_ctx.HostToast(StringMgr.Get("optim.s_menu_openloc_ok"), pk, 0);
            else
                m_ctx.HostToast(StringMgr.Get("optim.s_menu_openloc_fail"), pk, 2);
        }
        else
        {
            if (!UiHelpers_OpenExplorerSelect(m_ctx.HostHwnd(), pk))
                m_ctx.HostToast(StringMgr.Get("optim.s_menu_openloc_fail"), pk, 2);
            else
                m_ctx.HostToast(StringMgr.Get("optim.s_menu_openloc_ok"), pk, 0);
        }
    }
    else
        m_ctx.HostToast(StringMgr.Get("optim.s_menu_openloc_empty"), sName.GetData(), 1);
    AppendLog(L"[optim] open location");
    return;
}

// [REFACTOR] 从 OcmHandleEnhancedCmds 提取：iCmd==4609 分支。逐字搬迁零改动。
void CPageOptim::OcmCmdCopyCommand(const CDuiString& sCmd, const CDuiString& sName)
{
    // 复制命令：RegWrite / RegDelete / Exec 拼接后的命令行
    if (!sCmd.IsEmpty())
    {
        if (FrameWnd_Util_CopyTextToClipboard(m_ctx.HostHwnd(), sCmd.GetData()))
            m_ctx.HostToast(StringMgr.Get("optim.s_menu_copycmd_ok"), sCmd.GetData(), 0);
        else
            m_ctx.HostToast(StringMgr.Get("optim.s_menu_copycmd_fail"), sCmd.GetData(), 2);
    }
    else
        m_ctx.HostToast(StringMgr.Get("optim.s_menu_copycmd_empty"), sName.GetData(), 1);
    AppendLog(L"[optim] rule command copied");
    return;
}

// [REFACTOR] 从 OcmHandleEnhancedCmds 提取：iCmd==4610 分支。逐字搬迁零改动。
void CPageOptim::OcmCmdRestoreDefault(CMarkupNode node, int g, int i, int nScope, const CDuiString& sName)
{
    // [RESTORE-SEMANTICS] 与 OcmBuildMenu 的置灰判定共用同一判据。
    //
    // 菜单已按 OcmCanRestoreDefault 置灰，但命令本体原先自己再算一遍
    // （只看 <Default> 分支，恒为 false），两套语义并存。即便菜单守卫正常，
    // 键盘/自动化路径也可能直接走到这里，届时它会静默执行"关闭"——
    // 用户点的是"恢复默认值"，得到的是"关闭"，两者在 112 个只写 0 的
    // 作用域上并不等价。此处显式拒绝并回执，不再退化成关闭。
    const int nScope2 = (nScope >= 0) ? nScope : 0;
    if (!OcmCanRestoreDefault(node, nScope2))
    {
        // 不能复用 "已恢复默认值" 文案——什么都没做却说恢复了是最坏的反馈。
        // 复用 optim.s_menu_openloc_empty（"「%s」无…" ，11 个语言包齐全），
        // 语义为"该项没有可执行的内容"，与"无默认值可恢复"一致。
        m_ctx.HostToast(StringMgr.Get("optim.s_menu_openloc_empty"), sName.GetData(), 1);
        AppendLog(L"[optim] restore skipped: scope has no system default to return to");
        return;
    }

    CMarkupNode dBox = node.GetChild(m_ctx.DataEngine().ScopeName(nScope2));
    bool bHasDefault = dBox.IsValid() && dBox.GetChild(L"Default").IsValid();
    bool bTarget = bHasDefault;
    int nBefore = m_ctx.DataEngine().GetScopeState(node, nScope2);
    PushOptimUndo(g, i, nScope2, nBefore == 1);
    std::vector<CDuiString> vOut;
    int nUnsupported = 0;
    bool bRestart = false;
    bool bOk = m_ctx.DataEngine().SetScopeState(node, nScope2, bTarget,
                                               vOut, nUnsupported, &bRestart);
    if (!bOk && !bHasDefault)
    {
        // 无 <Default> 分支：按旧语义回退为"关闭"（关闭分支只删不写 = 回系统默认）
        bOk = m_ctx.DataEngine().SetScopeState(node, nScope2, false,
                                              vOut, nUnsupported, &bRestart);
    }
    if (!bOk)
    {
        // [F-OPT-3] 两级写入全部失败 = 系统未发生任何变更：回收入栈记录，
        //   否则 Ctrl+Z 会"撤销"一次从未发生的变更，撤销栈与真实状态错位
        //   （单项路径 OnSwitchToggle 失败即弹出，此处对齐）。原先只在
        //   "走了回退分支"时 pop+重push 同一条记录（等效于不 pop），而
        //   "<Default> 存在但写失败" 与 "两级全失败" 两条路径记录泄漏在栈里。
        if (!m_vOptimUndo.empty()) m_vOptimUndo.pop_back();
    }
    OcmPromptRestartIfNeeded(bRestart);
    int nAfter = m_ctx.DataEngine().GetScopeState(node, nScope2);
    OcmSyncSwitchAfterChange(g, i, nScope2, nAfter);

    m_ctx.HostToast(StringMgr.Get("optim.s_menu_restore_ok"), sName.GetData(),
                    (nAfter == (bTarget ? 1 : 0)) ? 0 : 1);
    AppendLog(L"[optim] rule restored to default");
    return;
}

// [R169] 从 OcmCmdRestoreDefault 抽出：写入后若需重启 explorer 则询问并投递消息。
// 语义逐字迁移：仅在 bRestart 为真时询问；选「是」才 PostMessage。
// [CRASHFIX] 确认框返回后统一稳定队列再交还调用方：OcmCmdRestoreDefault 在
//   本函数返回后还要操作开关控件（OcmSyncSwitchAfterChange），旧实现让
//   MessageBox 模态泵漏下的陈旧输入消息直接命中后续控件操作。
void CPageOptim::OcmPromptRestartIfNeeded(bool bRestart)
{
    if (!bRestart) return;
    const int nAns = ToolSql::HostConfirm(m_ctx.HostHwnd(),
                                          StringMgr.Get("toolbox.restart_expl"),
                                          StringMgr.Get("toolbox.restart_expl"),
                                          MB_YESNO, MB_ICONQUESTION);
    if (nAns == IDYES)
        ::PostMessageW(m_ctx.HostHwnd(), WM_HOST_REQUEST_RESTART_EXPLORER, 0, 0);
    optim_detail::SettleAfterDialog();
}

// [R169] 从 OcmCmdRestoreDefault 抽出：按写入后的实际状态回写开关控件与组级徽标。
// 命名规则 sw_<组>_<项>_<作用域> 与 g>=0 才同步徽标，均与迁移前逐字一致。
void CPageOptim::OcmSyncSwitchAfterChange(int g, int i, int nScope, int nAfter)
{
    CDuiString sSwName;
    sSwName.Format(L"sw_%d_%d_%d", g, i, nScope);
    CSwitchUI* pSw = static_cast<CSwitchUI*>(m_ctx.HostFindControl(sSwName.GetData()));
    if (pSw && nAfter >= 0) pSw->SetOn(nAfter == 1, false);
    if (g >= 0) SyncOptimGroupGlyph(g);
}
