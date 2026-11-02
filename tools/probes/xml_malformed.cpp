// 探针：结构不完整的 XML（未闭合标签 / 缺失属性引号 / 截断）在遍历时是否越界。
// 目标：确认解析器"宽松接受"不会留下不一致的元素表。
#include "StdAfx.h"
#include <cstdio>
#include <string>
#include "Core/UIMarkup.h"

static int g_fail = 0;
static void walk(DuiLib::CMarkupNode n, int depth, int* visited)
{
    if (!n.IsValid() || depth > 200) return;
    ++(*visited);
    // 触发所有访问器
    if (n.GetName())      (void)n.GetName();
    (void)n.GetValue();
    (void)n.HasAttributes();
    int cnt = n.GetAttributeCount();
    for (int i = 0; i < cnt; ++i) { (void)n.GetAttributeName(i); (void)n.GetAttributeValue(i); }
    (void)n.GetAttributeValue(L"name");
    (void)n.HasChildren();
    walk(n.GetChild(), depth + 1, visited);
    walk(n.GetSibling(), depth, visited);
}

static void probe(const char* label, const wchar_t* xml)
{
    DuiLib::CMarkup m;
    bool ok = m.Load(xml);
    int visited = 0;
    if (ok) walk(m.GetRoot(), 0, &visited);
    printf("[%-22s] Load=%d visited=%d\n", label, ok, visited);
    fflush(stdout);
}

int main()
{
    probe("unclosed",            L"<Window><Button>");
    probe("truncated-mid-attr",  L"<Window name=\"abc");
    probe("unclosed-quote",      L"<Window name=\"abc>");
    probe("stray-close",         L"</Window>");
    probe("double-root",         L"<A/><B/>");
    probe("gt-in-attr",          L"<A p=\">\"/>");
    probe("amp-unterminated",    L"<A p=\"&\"/>");
    probe("only-open-angle",     L"<");
    probe("nested-deep-ok",      L"<a><b><c><d>x</d></c></b></a>");
    printf("[probe] done (no ASan report == traversal within bounds)\n");
    return g_fail;
}
