// ============================================================
// Page_ToolsHostsIO.cpp —— hosts 文件的读取原语。
//
// [R107] 从 Page_ToolsHosts.h 拆出（拆分理由见 Page_ToolsHostsTpl.cpp 头注释）。
//   本 TU 只负责"把 hosts 读成字节缓冲"，不碰对话框、不碰模板。
//   写入路径仍在 HostEditProc 内（它需要从对话框控件取文本，
//   与过程强耦合，强行拆出只会增加参数噪声）。
// ============================================================

#include "stdafx.h"

#include <string>
#include <vector>

#include "Page_ToolsCommon.h"   // PTFileGuard

namespace ToolHosts
{

using Buf = std::vector<BYTE>;

[[nodiscard]] bool HostReadAll(const std::wstring& path, Buf& out)
{
    PTFileGuard hf = PTFileGuard(::CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,
                            nullptr,OPEN_EXISTING,0,nullptr));
    if (hf==INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER li;
    if (!::GetFileSizeEx(hf,&li)||li.QuadPart>64*1024*1024)
    { return false; }  // PTFileGuard 自动关闭
    out.resize(size_t(li.QuadPart));
    DWORD rd=0;
    BOOL bok=TRUE;
    if (!out.empty())
        bok=::ReadFile(hf,&out[0],DWORD(out.size()),&rd,nullptr)&&rd==out.size();
    return bok!=FALSE;
}
} // namespace ToolHosts
