// ============================================================
// Page_ToolsHostsTpl.cpp —— hosts 编辑对话框的**模板构造**。
//
// [R107] 从 Page_ToolsHosts.h 拆出。原头文件 313 行同时承担三种职责：
//   ① 二进制对话框模板构造（DLGTEMPLATE/DLGITEMTEMPLATE 逐字节拼装）
//   ② 对话框过程 HostEditProc（消息处理）
//   ③ hosts 文件读写 HostReadAll
//   三者生命周期完全不同：模板在 CreateDialogIndirect 前构建一次，
//   过程在对话框存活期反复调用，读写只在"保存"时发生。混在一个头里
//   会让任何一处改动都要重新理解另外两处。
//
// 本 TU 只放 ①。构造辅助（TplAppendW/TplAlign/PutW/PutD/Buf）也随之独立，
// 因为它们是**只服务于模板拼装**的字节原语。
// ============================================================

#include "stdafx.h"

#include <string>
#include <vector>

#include "StringManager.h"

// [R107] 需要 ToolHosts::HOSTDLG_ID_EDIT 常量（控件 ID 的单一真源仍在头里）。
#include "Page_ToolsHosts.h"

namespace ToolHosts
{

using Buf = std::vector<BYTE>;

// 两个重载：Buf（vector）用于主模板，BYTE[]（栈数组）用于 18/24 字节子模板。
//   BYTE 数组版不检查长度（调用点都是定长常量 + sizeof(元素) 的编译期组合，
//   由下面的 static_assert 保证够用），故只保留 memcpy 的字节语义。
template <size_t N>
inline void PutW(BYTE (&arr)[N], size_t off, WORD v)
{
    static_assert(N >= 2, "target array too small for WORD");
    if (off + sizeof(WORD) > N) return;
    memcpy(arr + off, &v, sizeof(WORD));
}
template <size_t N>
inline void PutD(BYTE (&arr)[N], size_t off, DWORD v)
{
    static_assert(N >= 4, "target array too small for DWORD");
    if (off + sizeof(DWORD) > N) return;
    memcpy(arr + off, &v, sizeof(DWORD));
}
inline void PutW(Buf& b, size_t off, WORD v)
{
    if (off + sizeof(WORD) > b.size()) return;
    memcpy(b.data() + off, &v, sizeof(WORD));
}
inline void PutD(Buf& b, size_t off, DWORD v)
{
    if (off + sizeof(DWORD) > b.size()) return;
    memcpy(b.data() + off, &v, sizeof(DWORD));
}

// 追加 UTF-16 字符串（含结尾 0）
static void TplAppendW(Buf& b, const std::wstring& svW)
{
    const size_t n = svW.size();
    if (n) b.insert(b.end(), (const BYTE*)svW.data(), (const BYTE*)(svW.data()+n));
    b.push_back(0); b.push_back(0);
}
// DWORD 对齐
static void TplAlign(Buf& b)
{ while (b.size() & 3) b.push_back(0); }

// 构造对话框模板
[[nodiscard]] Buf MakeHostDialogTemplate()
{
    Buf b;
    constexpr DWORD STYLE = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME
                      | DS_MODALFRAME | DS_SETFONT;
    // DLGTEMPLATE
    {
        BYTE t[18] = {};
        memcpy(t,&STYLE,4);          // style
        PutD(t,4,0);            // exstyle
        PutW(t,8,3);             // cdit
        PutW(t,10,0);PutW(t,12,0);   // x,y
        PutW(t,14,420);PutW(t,16,280);
        b.insert(b.end(),t,t+18);
    }
    // [B1 修复] 内存 DLGTEMPLATE 规定标题前必须显式写 menu/class 默认字段
    // （各为 WORD 0x0000）；缺省会让解析器把标题当菜单名、后续字段全部
    // 错位，DialogBoxIndirectParamW 失败返回 -1（点击无反应的根因之一）。
    b.push_back(0); b.push_back(0);   // menu  = 默认
    b.push_back(0); b.push_back(0);   // class = 默认
    TplAppendW(b, StringMgr.Get("toolbox.ui_hosts_dlg_title")); // title
    // DS_SETFONT: pointsize(2)+typeface
    b.push_back(9); b.push_back(0);
    TplAppendW(b, L"Microsoft YaHei UI");

    // --- Edit ---
    TplAlign(b);
    {
        BYTE it[24] = {};
        DWORD es = WS_CHILD|WS_VISIBLE|WS_BORDER|WS_VSCROLL|WS_HSCROLL|
                   ES_MULTILINE|ES_WANTRETURN|ES_AUTOVSCROLL|ES_AUTOHSCROLL;
        memcpy(it,&es,4);
        PutW(it,8,10);  PutW(it,10,8);
        PutW(it,12,400);PutW(it,14,242);
        PutW(it,16,WORD(HOSTDLG_ID_EDIT));
        b.insert(b.end(),it,it+18);
    }
    // [B3 修复] 标准（非 EX）DLGITEMTEMPLATE 的 18 字节结构体之后直接是
    // windowClass / title / creationData，不存在 helpID、menu 字段（helpID 仅
    // 属于 DLGITEMTEMPLATEEX）。B1 曾在此误插 6 字节（DWORD helpID + WORD menu），
    // 解析器将其当成"空类名"，真正的类序号 FFFF0081 整体错位，控件创建失败。
    b.push_back(0xFF); b.push_back(0xFF); b.push_back(0x81); b.push_back(0x00); // class ordinal EDIT
    TplAppendW(b,L"");                 // no title
    b.push_back(0); b.push_back(0);    // no creation data

    // --- 保存 ---
    TplAlign(b);
    {
        BYTE it[24] = {};
        DWORD bs = WS_CHILD|WS_VISIBLE|BS_DEFPUSHBUTTON;
        memcpy(it,&bs,4);
        PutW(it,8,248); PutW(it,10,256);
        PutW(it,12,72); PutW(it,14,16);
        PutW(it,16,IDOK);
        b.insert(b.end(),it,it+18);
    }
    // [B3 修复] 标准 DLGITEMTEMPLATE 结构体后直接给 windowClass（无 helpID/menu）。
    b.push_back(0xFF);b.push_back(0xFF);b.push_back(0x80);b.push_back(0x00); // BUTTON
    TplAppendW(b,StringMgr.Get("toolbox.ui_btn_save"));
    b.push_back(0);b.push_back(0);

    // --- 取消 ---
    TplAlign(b);
    {
        BYTE it[24] = {};
        DWORD bs = WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON;
        memcpy(it,&bs,4);
        PutW(it,8,330); PutW(it,10,256);
        PutW(it,12,72); PutW(it,14,16);
        PutW(it,16,IDCANCEL);
        b.insert(b.end(),it,it+18);
    }
    // [B3 修复] 标准 DLGITEMTEMPLATE 结构体后直接给 windowClass（无 helpID/menu）。
    b.push_back(0xFF);b.push_back(0xFF);b.push_back(0x80);b.push_back(0x00);
    TplAppendW(b,StringMgr.Get("toolbox.ui_btn_cancel"));
    b.push_back(0);b.push_back(0);
    TplAlign(b);
    return b;
}

// 读全部字节
} // namespace ToolHosts
