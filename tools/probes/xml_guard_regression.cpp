// 回归探针：确认深度护栏不破坏正常解析，且边界行为正确。
#include "StdAfx.h"
#include <cstdio>
#include <string>
#include "Core/UIMarkup.h"

static int g_fail = 0;
static void check(bool cond, const char* what) {
    printf("[%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) ++g_fail;
}

static void walk(DuiLib::CMarkupNode n, int depth, int* visited)
{
    if (!n.IsValid() || depth > 200) return;
    ++(*visited);
    (void)n.GetName();
    (void)n.GetValue();
    (void)n.HasAttributes();
    int cnt = n.GetAttributeCount();
    for (int i = 0; i < cnt; ++i) { (void)n.GetAttributeName(i); (void)n.GetAttributeValue(i); }
    (void)n.GetAttributeValue(L"name");
    (void)n.HasChildren();
    walk(n.GetChild(), depth + 1, visited);
    walk(n.GetSibling(), depth, visited);
}

static std::wstring Nest(int depth) {    std::wstring s;
    for (int i = 0; i < depth; ++i) s += L"<a>";
    s += L"x";
    for (int i = 0; i < depth; ++i) s += L"</a>";
    return s;
}

int main() {
    // 1) 真实皮肤规模（8 层）必须正常解析，且结构可遍历
    {
        const wchar_t* xml =
            L"<Window name=\"w\" size=\"100,100\">"
            L"  <VerticalLayout name=\"root\">"
            L"    <HorizontalLayout>"
            L"      <Button name=\"b1\" text=\"OK\"/>"
            L"      <Button name=\"b2\" text=\"Cancel\"/>"
            L"    </HorizontalLayout>"
            L"  </VerticalLayout>"
            L"</Window>";
        DuiLib::CMarkup m;
        check(m.Load(xml), "realistic skin parses");
        DuiLib::CMarkupNode root = m.GetRoot();
        check(root.IsValid(), "root node valid");
        check(std::wstring(root.GetName()) == L"Window", "root name == Window");
        check(std::wstring(root.GetAttributeValue(L"name")) == L"w", "root attr name == w");
        DuiLib::CMarkupNode layout = root.GetChild();
        check(layout.IsValid() && std::wstring(layout.GetName()) == L"VerticalLayout",
              "child == VerticalLayout");
        DuiLib::CMarkupNode h = layout.GetChild();
        check(h.IsValid() && std::wstring(h.GetName()) == L"HorizontalLayout",
              "grandchild == HorizontalLayout");
        DuiLib::CMarkupNode b1 = h.GetChild();
        check(b1.IsValid() && std::wstring(b1.GetAttributeValue(L"text")) == L"OK",
              "nested Button text == OK");
        DuiLib::CMarkupNode b2 = b1.GetSibling();
        check(b2.IsValid() && std::wstring(b2.GetAttributeValue(L"text")) == L"Cancel",
              "sibling Button text == Cancel");
        check(!b2.GetSibling().IsValid(), "no third sibling");
    }

    // 2) 边界：64 层恰好在限内 -> 通过
    {
        DuiLib::CMarkup m;
        bool ok = m.Load(Nest(64).c_str());
        check(ok, "64 levels accepted (boundary)");
    }
    // 3) 边界：65 层超出 -> 拒绝（不崩溃）
    {
        DuiLib::CMarkup m;
        bool ok = m.Load(Nest(65).c_str());
        check(!ok, "65 levels rejected (over limit)");
        wchar_t msg[128] = {0};
        m.GetLastErrorMessage(msg, 128);
        printf("        error message: %ls\n", msg);
    }
    // 4) 深层 100000 层 -> 拒绝（原为栈溢出）
    {
        DuiLib::CMarkup m;
        check(!m.Load(Nest(100000).c_str()), "100000 levels rejected, no stack overflow");
    }
    // 5) 空/空指针输入 -> 安全拒绝
    {
        DuiLib::CMarkup m;
        check(!m.Load(L""), "empty string rejected");
        check(!m.Load(nullptr), "null pointer rejected");
        check(!m.LoadFromMem(nullptr, 0), "null mem buffer rejected");
    }
    // 6) 属性数量超过 MAX_XML_ATTRIBUTES(64) 不越界
    {
        std::wstring xml = L"<a";
        for (int i = 0; i < 200; ++i) {
            xml += L" p" + std::to_wstring(i) + L"=\"v" + std::to_wstring(i) + L"\"";
        }
        xml += L"/>";
        DuiLib::CMarkup m;
        bool ok = m.Load(xml.c_str());
        check(ok, "element with 200 attributes parses");
        if (ok) {
            DuiLib::CMarkupNode root = m.GetRoot();
            int n = root.GetAttributeCount();
            printf("        attribute count capped at %d\n", n);
            check(n <= 64, "attribute count within MAX_XML_ATTRIBUTES");
        }
    }
    // 7) 未闭合标签：上游 _Parse 在 iParent<=1 时刻意宽容（*pstrText=='\0' 即返回 true）。
    //    该行为经 xml_malformed 探针证明遍历安全，因此契约是
    //    「要么拒绝，要么接受且可安全遍历」，而不是「必须拒绝」。
    {
        DuiLib::CMarkup m;
        bool ok = m.Load(L"<Window><Button>");
        printf("        unclosed tag: Load=%d (upstream leniency at top level)\n", ok);
        int visited = 0;
        if (ok) walk(m.GetRoot(), 0, &visited);
        check(!ok || visited > 0, "unclosed tag rejected, or accepted and traversable");
        DuiLib::CMarkup m2;
        check(!m2.Load(L"<Window></Other>"), "mismatched closing tag rejected");
    }

    printf("\n%s (%d failures)\n", g_fail == 0 ? "ALL GREEN" : "FAILURES", g_fail);
    return g_fail;
}
