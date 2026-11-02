#pragma once

// ============================================================
// Page_ToolsHosts.h —— hosts 编辑器辅助层（跨编译单元共享）。
//
// [R129-1] 从 Page_Tools.cpp 抽出 233 行 hosts 编辑基础设施：
//   Buf(=std::vector<BYTE>) 别名、TplAppendW、TplAlign、
//   MakeHostDialogTemplate、HostReadAll、HostEditProc
//
// 闭包证明：本段 m_ctx 0 处、CPageTools:: 0 处，只依赖
// Win32 / StringMgr / ToolSql::HostNotify / ToolSql 的 PTFileGuard，
// 因此为自包含单元，可整体外提。
// CPageTools::ToolHosts 依赖 m_ctx，仍留在主文件，经本头调用上述辅助。
// ============================================================

#include "stdafx.h"

#include <string>
#include <vector>

#include "StringManager.h"
#include "Page_ToolsCommon.h"   // PTFileGuard / ToolSql::HostNotify

namespace ToolHosts
{
using ToolSql::ResolveProc;
using ToolSql::HostNotify;


constexpr int HOSTDLG_ID_EDIT = 1001;

struct CHostEditCtx
{
    std::wstring sPath;
    bool bBom;
    bool bSaved;
};

// 内存模板字节缓冲
using Buf = std::vector<BYTE>;

// ============================================================
// [R103] 二进制模板写入辅助：替代 *(WORD*)(buf+off)=v 这类强制转型写入。
//
// 为什么必须换掉：Buf 是 std::vector<BYTE>，元素对象是 BYTE。
// 通过 WORD*/DWORD* 写入同一块存储，违反 C++ strict-aliasing 规则；
// 本工程以 /O2 + /GL 编译，编译器有权假设 BYTE* 与 WORD* 不重叠，
// 从而把这里的写入与后续 b.insert(...) 重排顺序，产出的 DLGTEMPLATE
// 字节可能错位 —— 这与本文件历史上两次「控件创建失败 / 点击无反应」
// 的修复记录（B1、B3）症状一致，属于同一根因的未清理部分。
//
// PutW/PutD 用 memcpy 按字节落值，不引入别名假设。

// [R107] 下列两函数已迁到独立 TU（模板构造 / 文件读取），
//   但 HostEditProc 仍要调用它们，故在此保留声明。
//   不加 static：static 是内部链接，跨 TU 不可见（会变 C3861）。
[[nodiscard]] Buf  MakeHostDialogTemplate();
[[nodiscard]] bool HostReadAll(const std::wstring& path, Buf& out);


static INT_PTR CALLBACK HostEditProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    CHostEditCtx* p=(CHostEditCtx*)::GetWindowLongPtrW(h,GWLP_USERDATA);
    switch (m)
    {
    case WM_INITDIALOG:
    {
        p=(CHostEditCtx*)lp;
        ::SetWindowLongPtrW(h,GWLP_USERDATA,(LONG_PTR)p);
        HWND hEd=::GetDlgItem(h,HOSTDLG_ID_EDIT);
        // [FIX DPI-R22] 旧版把编辑框字体覆盖为 DEFAULT_GUI_FONT（8pt MS Sans
        // Serif 遗留库存字体，固定像素、不随系统 DPI 缩放），PerMonitorV2 进程
        // 下高分屏 hosts 正文显得极小。改为沿用对话框模板 DS_SETFONT 字体
        //（9pt Microsoft YaHei UI，由对话框管理器按 DPI 创建，与保存/取消按钮
        // 一致；WM_GETFONT 只读查询，不自建 GDI 对象、无泄漏）。
        HFONT hDlgFont=reinterpret_cast<HFONT>(::SendMessageW(h,WM_GETFONT,0,0));
        if (!hDlgFont) hDlgFont=static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
        ::SendMessageW(hEd,WM_SETFONT,reinterpret_cast<WPARAM>(hDlgFont),TRUE);
        Buf bb;
        if (HostReadAll(p->sPath,bb))
        {
            const BYTE* q=bb.data(); size_t n=bb.size();
            p->bBom = n>=3&&q[0]==0xEF&&q[1]==0xBB&&q[2]==0xBF;
            if (p->bBom) { q+=3; n-=3; }
            int cch=::MultiByteToWideChar(CP_UTF8,0,reinterpret_cast<const char*>(q),int(n),nullptr,0);
            std::wstring w(cch,L'\0');
            if (cch)
                ::MultiByteToWideChar(CP_UTF8,0,reinterpret_cast<const char*>(q),int(n),&w[0],cch);
            ::SetWindowTextW(hEd,w.c_str());
        }
        return TRUE;
    }
    case WM_SIZE:
    {
        int nW=LOWORD(lp), nH=HIWORD(lp);
        // [FIX DPI-R22] 旧版用固定物理像素布局（按钮 72x16px），高 DPI 下按钮
        // 缩不放进放大后的文字、且与模板 DLU 初始布局不一致。改为 MapDialogRect
        // 按本对话框实际 DPI 基数字换算 DLU（与模板 420x280/edit 10,8,400,242/
        // 按钮 72x16 完全同源的尺寸关系），随系统缩放自动适配。
        RECT rcM={0,0,10,8};   ::MapDialogRect(h,&rcM);   // 左右边距10DLU/上下8DLU
        RECT rcB={0,0,72,16};  ::MapDialogRect(h,&rcB);   // 按钮尺寸（同模板）
        RECT rcG={0,0,7,6};    ::MapDialogRect(h,&rcG);   // 按钮间隙7DLU/编辑框下间隙6DLU
        ::MoveWindow(::GetDlgItem(h,HOSTDLG_ID_EDIT),rcM.left,rcM.top,
                     nW-2*rcM.left,nH-rcM.top-rcM.bottom-rcB.bottom-rcG.bottom,TRUE);
        ::MoveWindow(::GetDlgItem(h,IDOK),
                     nW-rcM.left-2*rcB.right-rcG.right,
                     nH-rcM.bottom-rcB.bottom,rcB.right,rcB.bottom,TRUE);
        ::MoveWindow(::GetDlgItem(h,IDCANCEL),
                     nW-rcM.left-rcB.right,
                     nH-rcM.bottom-rcB.bottom,rcB.right,rcB.bottom,TRUE);
        return 0;
    }
    case WM_GETMINMAXINFO:
    {
        MINMAXINFO* mm=(MINMAXINFO*)lp;
        // [FIX DPI-R22] 最小跟踪尺寸随窗口所在显示器 DPI 缩放（旧版固定像素，
        // 高 DPI 下可把窗口拖得比内容还小）。GetDpiForWindow 动态加载，
        // 不可用时退回 GetDeviceCaps。
        UINT nDpi=96;
        using PFN_GDFW=UINT (WINAPI*)(HWND);
        // GetProcAddress 返回 FARPROC，转函数指针是 Win32 唯一合法路径；
        // 统一收进 ResolveProc 模板，页面内不再出现裸 reinterpret_cast。
        static PFN_GDFW s_pfnGdfw=ResolveProc<PFN_GDFW>(L"user32.dll","GetDpiForWindow");
        if (s_pfnGdfw) { const UINT d=s_pfnGdfw(h); if (d) nDpi=d; }
        else
        {
            HDC dc=::GetDC(h);
            if (dc) { nDpi=static_cast<UINT>(::GetDeviceCaps(dc,LOGPIXELSY)); ::ReleaseDC(h,dc); }
        }
        mm->ptMinTrackSize.x=::MulDiv(440,static_cast<int>(nDpi),96);
        mm->ptMinTrackSize.y=::MulDiv(300,static_cast<int>(nDpi),96);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp)==IDOK)
        {
            HWND hEd=::GetDlgItem(h,HOSTDLG_ID_EDIT);
            int cch=::GetWindowTextLengthW(hEd);
            // [修复 P1-HOSTS] 必须按 cch+1 分配：GetWindowTextW 最多写入
            // cch 个字符 + 结尾 NUL 共 cch+1 WCHAR；旧代码仅 cch 容量，
            // NUL 落在 size() 之外（标准层面的堆越界写）。
            std::wstring w(cch+1,L'\0');
            int cchCopied=::GetWindowTextW(hEd,&w[0],cch+1);
            w.resize(cchCopied);
            // w 现在恰好是无 NUL 的文本（size=cchCopied）
            int nb=::WideCharToMultiByte(CP_UTF8,0,w.c_str(),-1,nullptr,0,nullptr,0);
            std::vector<char> utf8(nb);
            ::WideCharToMultiByte(CP_UTF8,0,w.c_str(),-1,&utf8[0],nb,nullptr,0);
            --nb;  // 去 NUL
            Buf out;
            if (p->bBom)
            { out.push_back(0xEF);out.push_back(0xBB);out.push_back(0xBF); }
            out.insert(out.end(),utf8.begin(),utf8.begin()+nb);
            PTFileGuard hf = PTFileGuard(::CreateFileW(p->sPath.c_str(),GENERIC_WRITE,0,nullptr,
                                    CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr));
            if (hf==INVALID_HANDLE_VALUE)
            {
                HostNotify(h, StringMgr.Get("toolbox.s_hosts_write_fail"),
                               StringMgr.Get("toolbox.ui_hosts_save_fail"), MB_OK | MB_ICONERROR);
                return 0;
            }
            DWORD wr=0;
            // [R128-B] 导出文件写入失败/不全需留痕
        if (!::WriteFile(hf,out.empty()?nullptr:&out[0],DWORD(out.size()),&wr,nullptr)
            || wr != DWORD(out.size()))
            ::OutputDebugStringW(L"[R128][hosts] 导出写入不完整\r\n");
            // 刷新 DNS 解析缓存
            HMODULE hdns=::LoadLibraryW(L"dnsapi.dll");
            if (hdns)
            {
                using PFN_Flush = BOOL ( WINAPI*)(void);
                PFN_Flush pfn=(PFN_Flush)::GetProcAddress(hdns,"DnsFlushResolverCache");
                if (pfn) pfn();
                ::FreeLibrary(hdns);
            }
            p->bSaved=true;
            ::EndDialog(h,IDOK);
            return 0;
        }
        if (LOWORD(wp)==IDCANCEL)
        { ::EndDialog(h,IDCANCEL); return 0; }
        return 0;
    }
    return 0;
}

} // namespace ToolHosts
