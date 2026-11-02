#pragma once

// ============================================================
// Page_OptimInternal.h —— 优化页跨翻译单元共享的内部辅助。
//
// [R134-1] Page_Optim.cpp 原 3728 行，规模分 2.3（全仓最大缺口）。
// 把右键菜单族（Ocm* / ShowOptimContextMenu，共 552 行）迁入
// Page_OptimOcm.cpp 后，两处都需要按 (组号, 项号) 定位 <Item> 节点，
// 故把原匿名命名空间里的 FindOptimItem 提升为本头中的 inline 实现，
// 避免在两个 .cpp 中各留一份。
//
// 仅为同一页面内部的实现细节，不属于公共接口。
// ============================================================

#include "stdafx.h"

#include "DataEngine.h"        // CMarkupNode
#include "OptimScopeColumns.h"   // optim_scope::kColumns
#include "StringManager.h"      // i18n::CStringManager
#include "HostInterfaces.h"    // IPageContext（OptimItemShown / OptimGroupMatch 的入参）
#include "SafeThread.h"        // [R3] SafeDispatchMessage：共享消息泵
#include <chrono>               // [R3] 分片计时

#include <vector>

namespace optim_detail
{

// 按 (组序号, 项序号) 定位 <Item> 节点；越界时返回无效节点。
[[nodiscard]] inline CMarkupNode FindOptimItem(CMarkupNode root, int g, int i)
{
    int gg = 0;
    CMarkupNode group = root.GetChild(L"Group");
    while (group.IsValid())
    {
        if (gg == g)
        {
            int ii = 0;
            CMarkupNode item = group.GetChild(L"Item");
            while (item.IsValid())
            {
                if (ii == i) return item;
                ++ii;
                item = item.GetSibling();
            }
            return item;
        }
        ++gg;
        group = group.GetSibling();
    }
    return group;
}


// 可见作用域的中文描述（"当前用户 / 系统"），按 optim_scope::kColumns 顺序拼接。
// 不新增语言键：复用表头键 skin.hdr_op_cur / skin.hdr_op_sys。
[[nodiscard]] inline CDuiString DescribeVisibleScopes()
{
    CDuiString s;
    for (int sc : optim_scope::kColumns)
    {
        if (!s.IsEmpty()) s += L" / ";
        s += i18n::CStringManager::GetInstance().Get(sc == 0 ? "skin.hdr_op_cur"
                                                              : "skin.hdr_op_sys");
    }
    return s;
}


// 不区分大小写的子串判定（宽字符，走 CDuiString::Find 定位）。
[[nodiscard]] inline bool ContainsCI(const wchar_t* pszHay, const wchar_t* pszNeedle)
{
    CDuiString sHay(pszHay); sHay.MakeLower();
    CDuiString sNeedle(pszNeedle); sNeedle.MakeLower();
    return sHay.Find(sNeedle.GetData()) >= 0;
}


// 收集某条目在指定作用域的警告文案；nScope < 0 表示遍历全部可见列。
// 只遍历 optim_scope::kColumns：[SCOPE-2COL] 取消「新用户」列后，
// 若仍收集其 Warning，会在用户完全看不到那一列的情况下弹出确认文案。
inline void CollectWarningTexts(CDataEngine& engine,
                                CMarkupNode nodeItem, int nScope, bool bForCheck,
                                int iComboSel, std::vector<CDuiString>& out)
{
    if (nScope < 0)
    {
        for (int s : optim_scope::kColumns)
            CollectWarningTexts(engine, nodeItem, s, bForCheck, iComboSel, out);
        return;
    }
    CMarkupNode box = nodeItem.GetChild(engine.ScopeName(nScope));
    if (!box.IsValid()) return;
    if (iComboSel < 0)
    {
        CMarkupNode branch = box.GetChild(bForCheck ? L"True" : L"False");
        if (!branch.IsValid()) return;
        CMarkupNode w = branch.GetChild(L"Warning");
        while (w.IsValid())
        {
            CDuiString sw = engine.ResolveText(w.GetValue());
            if (!sw.IsEmpty()) out.push_back(sw);
            w = w.GetSibling();
        }
    }
    else
    {
        int n = 0;
        CMarkupNode d = box.GetChild(L"Dropdown");
        while (d.IsValid())
        {
            if (n == iComboSel)
            {
                CMarkupNode w = d.GetChild(L"Warning");
                while (w.IsValid())
                {
                    CDuiString sw = engine.ResolveText(w.GetValue());
                    if (!sw.IsEmpty()) out.push_back(sw);
                    w = w.GetSibling();
                }
            }
            ++n;
            d = d.GetSibling();
        }
    }
}


// 条目是否满足 <Applicable> 条件（无该节点视为适用）。
[[nodiscard]] inline bool ItemIsApplicable(CDataEngine& engine, CMarkupNode nodeItem)
{
    CMarkupNode ap = nodeItem.GetChild(L"Applicable");
    if (!ap.IsValid()) return true;
    return engine.EvalConditions(ap);
}


// ============================================================
// [FILTER-SSOT] 条目在当前过滤条件下是否显示 —— 界面与写入的唯一判定源。
//
// 旧实现有两套口径：ApplyFindFilter 用"适用性 + 关键字 + 状态"三个条件显式
// 判定；批量/分组批量/统计用 pSw->IsVisible() 反推"用户看得见什么"。后者把
// "分组折叠"也算成不可见，导致全部组折叠时「全选/全不选」判定为没有可操作项
// 而静默不动，同一时刻分组复选框却能改同一批规则。
// 现在视图状态（折叠）与筛选状态（关键字/状态）彻底分离：折叠只管画，筛选
// 才管"算不算在内"，两条链路共用本函数。
//
// bGroupMatch：调用方已判定组名命中关键字时传 true，条目自身不再比对名称。
// ============================================================
[[nodiscard]] inline bool OptimItemShown(IPageContext& ctx, CMarkupNode item,
                                         const CDuiString& sFind, int nStateFilter,
                                         bool bGroupMatch)
{
    if (!ItemIsApplicable(ctx.DataEngine(), item)) return false;
    if (!sFind.IsEmpty() && !bGroupMatch)
    {
        const CDuiString sItemName =
            ctx.DataEngine().ResolveText(item.GetAttributeValue(L"Name"));
        if (!ContainsCI(sItemName.GetData(), sFind.GetData())) return false;
    }
    // 状态过滤：开关项按可见列"是否已启用"判定；下拉项不参与
    if (nStateFilter == 0) return true;
    if (lstrcmpiW(item.GetAttributeValue(L"Type"), L"Combo") == 0) return true;
    bool bHasScope = false;
    bool bAnyOn = false;
    for (int sc : optim_scope::kColumns)
    {
        if (!item.GetChild(ctx.DataEngine().ScopeName(sc)).IsValid()) continue;
        bHasScope = true;
        if (ctx.DataEngine().GetScopeState(item, sc) == 1) bAnyOn = true;
    }
    return (nStateFilter == 1) ? bAnyOn : (bHasScope && !bAnyOn);
}


// [FILTER-SSOT] 组名是否命中当前过滤关键字：命中时组内全部条目视为可见
// （与 ApplyFindFilter 的 bGroupMatch 完全一致，避免筛选后批量口径漂移）。
[[nodiscard]] inline bool OptimGroupMatch(IPageContext& ctx, CMarkupNode gNode,
                                          const CDuiString& sFind)
{
    if (sFind.IsEmpty() || !gNode.IsValid()) return false;
    const CDuiString sGroupName =
        ctx.DataEngine().ResolveText(gNode.GetAttributeValue(L"Name"));
    return ContainsCI(sGroupName.GetData(), sFind.GetData());
}

// [R3 2026-10-25] 共享消息泵：批量执行（G1）与切页校验（G6）都要用它。
//   原先只写在 Page_OptimExec.cpp 的文件内静态函数里，G6 想复用就必须
//   整段复制一遍——两处各一份迟早不一致。提升为 inline 后单点维护。
//   nBudgetMs 是本次让出的时间上限：到点就返回，剩余消息留给下一轮处理，
//   避免"泵一下就陷在消息循环里出不来"。
// [CRASHFIX] 重启请求绝不能在嵌套泵里消费：本泵运行于点击/批量处理栈内，
//   若把 WM_HOST_REQUEST_RESTART_EXPLORER 直接派发，RestartExplorer 会在
//   栈内重入——长时间嵌套泵送 + 杀 shell，栈上的 DuiLib 弹层半销毁，
//   正是 WmMessages.h 该消息注释所禁的 0xc0000409 CFG 崩溃路径。
//   现改为：取出后不派发，泵结束原样回投，留给主循环在栈外处理。
//   WM_CLEAN_DONE 同罪并罚：它的处理链（OnCleanDone）在用户确认后会
//   同步调 RestartExplorer（FrameWnd.cpp WM_CLEAN_DONE 分支），同样禁止
//   在栈内消费。lParam 堆对象所有权随消息转移，原样回投安全。
// [CRASHFIX-PUMP] 2026-11-01 页面切换崩溃：原实现只拦截上述两条消息，
//   WM_APPX_DATA / WM_APPX_DEPDATA / WM_CAP_DATA / WM_FEAT_DATA /
//   WM_PROG_DATA / WM_UI_DEFERRED_EXEC 等业务消息族仍会在本泵（运行于
//   切页点击/校验栈内）被派发——接收方（如 CPageAppx::OnData→BuildList）
//   对列表 RemoveAll 删除正在派发的控件 → UAF；该 AV 又被
//   SafeDispatchMessage 的 SEH 吞掉，表现为"窗口消失、进程僵尸、
//   日志只有 first-chance 没有 UNHANDLED"。action_log 实证：死前最后
//   动作正是 [optim] verified（VerifyCountAndSync→本泵）与 Appx 枚举
//   完成消息回投窗口重叠。
//   修复：与 FrameWnd.cpp PumpShellSafeMessages 同一契约——
//   WM_USER 数据族 + WM_APP 编排族（含 WM_UI_DEFERRED_EXEC，其设计
//   语义就是"点击栈退清后由主循环执行"，绝不能在嵌套泵里消费）
//   一律取出后不派发，泵结束按原序回投，所有权随消息转移。
inline void PumpUiMessages(int nBudgetMs)
{
    const auto t0 = std::chrono::steady_clock::now();
    MSG msg;
    std::vector<MSG> vRepost;
    // [R15-FIX] 嵌套泵安全边界：
    //   只派发"与控件生命周期无关"的绘制/激活/外观类消息；
    //   业务消息（WM_USER/WM_APP/WM_TIMER）回投到主循环；
    //   其余（所有输入/命令/通知/菜单）一律丢弃——在点击/校验栈中途
    //   重入输入路径等于重入点击处理器，是 UAF 的直接肇事面。
    //   白名单与 SettleAfterDialog 保持同源一致性。
    const UINT kSafeDispatch[] = {
        WM_PAINT, WM_NCPAINT, WM_ERASEBKGND,
        WM_ACTIVATE, WM_NCACTIVATE, WM_ACTIVATEAPP,
        WM_WINDOWPOSCHANGED, WM_SETTINGCHANGE, WM_THEMECHANGED,
        WM_DISPLAYCHANGE, WM_DWMCOLORIZATIONCOLORCHANGED,
        WM_QUERYENDSESSION,
    };
    auto IsSafe = [&](UINT u) -> bool {
        for (UINT s : kSafeDispatch) if (s == u) return true;
        return false;
    };
    while (::PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        if (msg.message == WM_QUIT) { ::PostQuitMessage((int)msg.wParam); break; }
        const bool bBusiness =
            (msg.message >= WM_USER && msg.message < WM_USER + 0x200)
            || (msg.message >= WM_APP && msg.message < WM_APP + 0x8000)
            || (msg.message == WM_TIMER);
        if (bBusiness)
        {
            vRepost.push_back(msg);   // 整条留存：堆对象所有权随消息转移
            continue;
        }
        if (IsSafe(msg.message))
        {
            ::TranslateMessage(&msg);
            SafeDispatchMessage(&msg);
        }
        // else: 输入/命令/通知类消息 → 直接丢弃，绝不派发
        const auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        if (dt >= nBudgetMs) break;
    }
    for (const MSG& m : vRepost)
        ::PostMessageW(m.hwnd, m.message, m.wParam, m.lParam);
}

// ============================================================
// [CRASHFIX] 模态对话框（MessageBoxW）返回后的队列稳定——替代裸 PumpUiMessages。
//
// 背景：MessageBox 有自己的模态泵，用户点"确定/取消"时，鼠标消息会先
// 投递到本应用队列（弹层关闭、命中失效控件的 WM_LBUTTONUP/WM_MOUSEMOVE
// 等）。MessageBox 返回后若用全量泵（PumpUiMessages）"稳定队列"，等于
// 在点击处理栈深处把这些陈旧消息派发出去——DuiLib 半销毁弹层收到陈旧
// 输入 → UAF/CFG 崩溃（任务2实证路径）。
//
// 策略（与 FrameWnd.cpp 重启白名单泵同源，但更保守）：
//   ① 定向丢弃：只按消息号取尽输入/命令/通知类陈旧消息，绝不派发；
//   ② 定向派发：只放行绘制/激活/外观类消息（这些与控件生命周期无关）；
//   ③ 其余消息（WM_TIMER、业务 WM_APP、重启请求）一律留在队列里，
//      交回主循环在栈外正常处理——本函数只"稳定"，不"消费"。
// ============================================================
inline void SettleAfterDialog()
{
    // ① 丢弃陈旧输入/命令/通知消息（按号取尽，不派发）
    struct URange { UINT lo, hi; };
    const URange ranges[] = {
        { WM_KEYFIRST, WM_KEYLAST },            // 键盘
        { WM_MOUSEFIRST, WM_MOUSELAST },        // 鼠标（客户区）
        { WM_NCMOUSEMOVE, WM_NCMBUTTONDBLCLK }, // 鼠标（非客户区）
        { WM_MOUSEHWHEEL, WM_MOUSEHWHEEL },     // 水平滚轮（部分 SDK 不在 MOUSELAST 内）
    };
    MSG msg;
    for (const auto& r : ranges)
        while (::PeekMessage(&msg, nullptr, r.lo, r.hi, PM_REMOVE | PM_NOYIELD)) {}

    const UINT singles[] = {
        WM_INPUT, WM_COMMAND, WM_SYSCOMMAND, WM_NOTIFY, WM_CONTEXTMENU,
        WM_PARENTNOTIFY, WM_CAPTURECHANGED, WM_MOUSEHOVER, WM_MOUSELEAVE,
        WM_SETCURSOR, WM_HSCROLL, WM_VSCROLL, WM_GESTURE, WM_GESTURENOTIFY,
        WM_NCHITTEST, WM_NCMOUSEHOVER, WM_NCMOUSELEAVE,
    };
    for (UINT u : singles)
        while (::PeekMessage(&msg, nullptr, u, u, PM_REMOVE | PM_NOYIELD)) {}

    // ② 只派发绘制/激活/外观类消息（与控件生命周期无关，安全）。
    //    注意：绝不能把 WM_HOST_REQUEST_RESTART_EXPLORER 放进白名单——
    //    它必须留在队列里由主循环在点击栈外处理。
    const UINT dispatchWhitelist[] = {
        WM_PAINT, WM_NCPAINT, WM_ERASEBKGND,
        WM_ACTIVATE, WM_NCACTIVATE, WM_ACTIVATEAPP,
        WM_WINDOWPOSCHANGED, WM_SETTINGCHANGE, WM_THEMECHANGED,
        WM_DISPLAYCHANGE, WM_DWMCOLORIZATIONCOLORCHANGED,
        WM_QUERYENDSESSION,
    };
    for (UINT u : dispatchWhitelist)
        while (::PeekMessage(&msg, nullptr, u, u, PM_REMOVE))
        {
            ::TranslateMessage(&msg);
            SafeDispatchMessage(&msg);
        }
    // 客户区颜色消息是连续区间（WM_CTLCOLORMSGBOX..WM_CTLCOLORSTATIC）
    while (::PeekMessage(&msg, nullptr, WM_CTLCOLORMSGBOX, WM_CTLCOLORSTATIC, PM_REMOVE))
    {
        ::TranslateMessage(&msg);
        SafeDispatchMessage(&msg);
    }
}

} // namespace optim_detail

