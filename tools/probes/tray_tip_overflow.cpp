// 探针：CTrayIcon::SetTooltipText 用 _tcscpy 写入 NOTIFYICONDATA::szTip[128]
// 无长度校验 -> 超长 tooltip 越界写入 CTrayIcon 成员（栈上对象，ASan 直接可见）。
#include "StdAfx.h"
#include <cstdio>
#include <string>
#include "Utils/TrayIcon.h"

int main()
{
    printf("[probe] sizeof(CTrayIcon)=%zu sizeof(NOTIFYICONDATAW)=%zu szTip cap=%d\n",
           sizeof(DuiLib::CTrayIcon), sizeof(NOTIFYICONDATAW), 128);
    fflush(stdout);

    DuiLib::CTrayIcon tray;   // 栈对象：越界写会越过整个对象边界

    const size_t kLen = 2000;  // 2000 TCHAR = 4000 字节，远超对象大小
    std::wstring tip = L"tip_" + std::wstring(kLen - 4, L'T');

    printf("[probe] writing %zu TCHAR into szTip[128] ...\n", tip.size());
    fflush(stdout);

    tray.SetTooltipText(tip.c_str());

    size_t back = tray.GetTooltipText().GetLength();
    printf("[probe] readback length=%zu (capacity=128) -> %s\n", back,
           back > 127 ? "OVERFLOW CONFIRMED" : "within capacity");
    return 0;
}
