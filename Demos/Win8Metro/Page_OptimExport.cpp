#include "stdafx.h"

// ============================================================
// Page_OptimExport.cpp —— 优化页配置导出子系统。
//
// [R137-1] 从 Page_Optim.cpp（3016 行）拆出 231 行：
//   OptimXmlEsc            XML 属性值转义
//   OptimAppendItemXml     单条目逐列导出（按可见作用域各写一行 <Optim>）
//   CPageOptim::ExportOptimConfig  导出入口
//
// 闭包说明：两个自由函数仅在本文件内互相引用，段外零引用；
// ExportOptimConfig 声明见 Page_Optim.h；唯一外部成员依赖 AppendLog。
//
// 搬迁为逐字搬运，零逻辑改动。
// ============================================================

#include "Page_Optim.h"
#include "Page_OptimInternal.h"   // optim_detail::ItemIsApplicable

#include "StringManager.h"
#include "NotifyBox.h"          // [R143] ToolSql::HostNotify / HostConfirm（页面统一弹框入口）
#include "FrameWnd_Util.h"       // CopyTextToClipboard
#include "Win32DialogService.h"  // DlgSaveFile

#include <string>
#include <vector>

using i18n::CStringManager;
using optim_detail::ItemIsApplicable;

// 导出文件头/列数上限（语义命名，替代裸数字）
inline constexpr int kExportMaxComboOptions = 64;

// [EXPORT-SYMMETRY] XML 属性值转义（原 ExportOptimConfig 内的局部 lambda，
// 外提为文件级函数，供逐列导出辅助共用）。
[[nodiscard]] static CDuiString OptimXmlEsc(const wchar_t* sIn)
{
    CDuiString r;
    for (const wchar_t* p = sIn; p && *p; ++p)
    {
        switch (*p)
        {
        case L'<':  r += L"&lt;";   break;
        case L'>':  r += L"&gt;";   break;
        case L'&':  r += L"&amp;";  break;
        case L'"':  r += L"&quot;"; break;
        case L'\'': r += L"&apos;"; break;
        default:    r += *p;        break;
        }
    }
    return r;
}

// [EXPORT-SYMMETRY] 单条目逐列导出：每个可见作用域各写一行带 Scope 属性的
// <Optim>，使导入侧按属性精确回放，而不是把当前用户的值整批覆盖到系统列。
//
// 为什么提取成独立函数而不是内联：内联形态为
//   while(组) > while(条目) > if(名称非空) > for(列) > if(下拉) > if(档位有效)
// 共 6 层，触发 Demos 嵌套深度门禁（≥6 层命中须为 0）。这里按"提取函数"
// 治理，而不是压缩行数或合并条件来伪装通过。
static void OptimAppendItemXml(CDataEngine& engine, CMarkupNode item,
                               CDuiString& sXml, int& nBool, int& nCombo)
{
    const CDuiString sName = engine.ResolveText(item.GetAttributeValue(L"Name"));
    if (sName.IsEmpty()) return;
    const bool bCombo = lstrcmpiW(item.GetAttributeValue(L"Type"), L"Combo") == 0;
    const CDuiString sEsc = OptimXmlEsc(sName.GetData());
    // [EXPORT-SYMMETRY] 计数口径 = 条目数，不是输出行数。
    // 按作用域分行后同一条目会写出 1~2 行；若在循环内逐行累加，导出完成
    // 提示「开关项 %d 条」会接近翻倍，与界面上的条目数对不上。
    int nWrote = 0;
    for (int sc : optim_scope::kColumns)
    {
        if (!item.GetChild(engine.ScopeName(sc)).IsValid()) continue;
        CDuiString sLine;
        if (bCombo)
        {
            const int iSel = engine.GetScopeDropdown(item, sc);
            if (iSel < 0) continue;   // 未配置的档位不导出
            sLine.Format(L"  <Optim Name=\"%s\" Scope=\"%s\" Value=\"combo:%d\"/>\r\n",
                         sEsc.GetData(), engine.ScopeName(sc), iSel);
        }
        else
        {
            const int nSt = engine.GetScopeState(item, sc);
            sLine.Format(L"  <Optim Name=\"%s\" Scope=\"%s\" Value=\"%d\"/>\r\n",
                         sEsc.GetData(), engine.ScopeName(sc), nSt == 1 ? 1 : 0);
        }
        sXml += sLine;
        ++nWrote;
    }
    if (nWrote > 0) { if (bCombo) ++nCombo; else ++nBool; }
}

// [R193] 从 ExportOptimConfig（78 行 / CC9）抽出：UTF-8 BOM 写盘与失败回执。
// 三处易错点逐字保留：
//   · 转换传 -1 表示**含结尾 NUL**（nLen 已含），故缓冲必须 3 + nLen 字节；
//     历史上少分 1 字节，WideCharToMultiByte 写结尾 NUL 时堆越界。
//   · 写盘只写 3 + (nLen - 1) 字节：BOM 3 字节 + 内容（不含结尾 NUL）。
//   · win::HandlePtr RAII 自动关句柄；INVALID_HANDLE_VALUE 在析构时是 no-op。
// 失败返回 false，调用方直接 return，不写"已导出"提示。
bool CPageOptim::OptimWriteXmlFile(IPageContext& ctx, const CDuiString& sPath,
                                   const CDuiString& sXml)
{
    const int nLen = ::WideCharToMultiByte(CP_UTF8, 0, sXml.GetData(), -1, nullptr, 0, nullptr, nullptr);
    if (nLen <= 0) return false;
    // [R160] 缓冲改为 char：UTF-8 字节序列本就以 char 为载体，
    //   这样 WideCharToMultiByte 直接拿到 char*，WriteFile 直接吃 data()，
    //   两处 BYTE*->char* 的别名转换从类型根源消失（而非换成 static_cast 掩盖）。
    std::string buf(static_cast<size_t>(3) + static_cast<size_t>(nLen), '\0');
    buf[0] = static_cast<char>(0xEF);
    buf[1] = static_cast<char>(0xBB);
    buf[2] = static_cast<char>(0xBF);
    ::WideCharToMultiByte(CP_UTF8, 0, sXml.GetData(), -1,
                          &buf[3], nLen, nullptr, nullptr);
    // [C++17 R3] RAII file handle: win::HandlePtr auto-closes
    win::HandlePtr h(::CreateFile(sPath.GetData(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (h.get() == INVALID_HANDLE_VALUE)
    {
        // S-15: 复制错误详情到剪贴板（含 GetLastError）
        CDuiString sExpErr2;
        sExpErr2.Format(L"Export failed: path=%s err=%d\n%s",
                        sPath.GetData(), static_cast<int>(::GetLastError()),
                        StringMgr.Get("error.s656c4845"));
        FrameWnd_Util_CopyTextToClipboard(ctx.HostHwnd(), sExpErr2.GetData());
        ToolSql::HostNotify(ctx.HostHwnd(), StringMgr.Get("error.s656c4845"),
                            StringMgr.Get("error.export_fail"), MB_OK | MB_ICONERROR);
        return false;
    }
    DWORD nWritten = 0;
    // [R65-AUDIT] 写入结果必须校验。
    //   原实现不看 WriteFile 返回值与字节数，直接 return true；
    //   磁盘满 / 只读 / 杀软拦截时导出文件为空或截断，
    //   调用方仍提示导出成功 —— 配置丢失而用户不知情。
    //   与 Tools 密码恢复说明（P0）同一类型：写盘失败不得声称成功。
    const DWORD cbWant = 3 + static_cast<DWORD>(nLen - 1);
    if (!::WriteFile(h.get(), buf.data(), cbWant, &nWritten, nullptr) || nWritten != cbWant)
    {
        // S-15: 失败详情复制到剪贴板（含 GetLastError），与上方打开失败同口径
        CDuiString sWrErr;
        sWrErr.Format(L"Export write failed: path=%s err=%d\n%s",
                      sPath.GetData(), static_cast<int>(::GetLastError()),
                      StringMgr.Get("error.s656c4845"));
        FrameWnd_Util_CopyTextToClipboard(ctx.HostHwnd(), sWrErr.GetData());
        ToolSql::HostNotify(ctx.HostHwnd(), StringMgr.Get("error.s656c4845"),
                            StringMgr.Get("error.export_fail"), MB_OK | MB_ICONERROR);
        return false;
    }
    return true;
}

void CPageOptim::ExportOptimConfig()

{
    // XML 属性值转义已外提为文件级 OptimXmlEsc（见本函数上方）
    CDuiString sPath;
    if (!Win32DialogService::DlgSaveFile(sPath,
            StringMgr.Get("optim.s8bf9f920"),
            StringMgr.Get("optim.sa0405281"), L"OptimConfig.xml")) return;
    SYSTEMTIME st; ::GetLocalTime(&st);
    CDuiString sXml;
    sXml  = L"<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n";
    CDuiString sStamp;
    sStamp.Format(L"<!-- Win8Metro optim config export  %04d-%02d-%02d %02d:%02d:%02d -->\r\n",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    sXml += sStamp;
    sXml += (std::wstring(StringMgr.Get("optim.s92c9ecf2")) + std::wstring(StringMgr.Get("appx.s7819a144"))).c_str();
    sXml += L"<DismConsoleConfig>\r\n";
    int nBool = 0, nCombo = 0;
    CMarkupNode optRoot = m_ctx.DataEngine().GetOptimizationRoot();
    CMarkupNode group = optRoot.GetChild(L"Group");
    while (group.IsValid())
    {
        // [OS-Filter] 导出与界面保持一致：不属于当前系统的组不写入配置文件
        CMarkupNode grpApp = group.GetChild(L"Applicable");
        if (grpApp.IsValid() && !m_ctx.DataEngine().EvalConditions(grpApp))
        {
            group = group.GetSibling();
            continue;
        }
        CMarkupNode item = group.GetChild(L"Item");
        while (item.IsValid())
        {
            // [OS-Filter] 条目级同理：不适用的优化项不写进导出文件
            if (!ItemIsApplicable(m_ctx.DataEngine(), item))
            {
                item = item.GetSibling();
                continue;
            }
            // [EXPORT-SYMMETRY] 逐列导出（含 XML 转义）已收敛到 OptimAppendItemXml：
            // 内联形态为 while>while>if>for>if>if 共 6 层，会触发嵌套深度门禁。
            OptimAppendItemXml(m_ctx.DataEngine(), item, sXml, nBool, nCombo);
            item = item.GetSibling();
        }
        group = group.GetSibling();
    }
    sXml += L"</DismConsoleConfig>\r\n";
    // [R193] 「UTF-8 BOM 写盘 + 失败回执」抽为 OptimWriteXmlFile（原内联 24 行）
    if (!OptimWriteXmlFile(m_ctx, sPath, sXml)) return;
    CDuiString sLog;
    sLog.Format(L"[optim] config exported: %d switches, %d combos -> %s",
                nBool, nCombo, sPath.GetData());
    AppendLog(sLog.GetData());
    CDuiString sOk;
    sOk.Format(StringMgr.Get("optim.sdaa0ff99"), nBool, nCombo);
    ToolSql::HostNotify(m_ctx.HostHwnd(), sOk.GetData(), StringMgr.Get("export.s446e67b3"), MB_OK | MB_ICONINFORMATION);
}
