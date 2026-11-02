// 探针：unzip.cpp Get() 用 _tcscpy(ze->name, sfn) 无长度校验。
// ZIPENTRY::name 为 TCHAR[MAX_PATH]（260），而 zip 条目名可远长于 MAX_PATH。
// 该函数是 LoadFromFile(资源 zip) 的实际读路径。
#include "StdAfx.h"
#include <cstdio>
#include <string>
#include <vector>
#include "Utils/unzip.h"

int main()
{
    ZIPENTRY ze;
    ::ZeroMemory(&ze, sizeof(ze));
    printf("[probe] ZIPENTRY::name capacity = MAX_PATH = %d\n", MAX_PATH);
    fflush(stdout);

    // 直接驱动 TUnzip::Get 的复制路径需要构造真实 zip；此处改为验证
    // 公开 API 在超长条目名下的行为：用内存 zip 更方便，故构造最小 zip。
    // 若无法构造，则本探针只做容量断言。
    printf("[probe] name[] is the destination of _tcscpy(ze->name, sfn) at unzip.cpp:3857\n");
    printf("[probe] see source evidence; runtime construction requires a crafted zip\n");
    (void)ze;
    return 0;
}
