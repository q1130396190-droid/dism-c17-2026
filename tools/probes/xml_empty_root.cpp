// 探针：空/仅空白 XML -> Load 返回 true -> GetRoot() 返回下标 1，
// 但 _Parse 立即返回，m_nElements 只有 1（index 0 = 错误槽）。
// GetRoot 只判断 m_nElements == 0，故会返回一个越界/未初始化的节点，
// 其 iChild/iNext 为未初始化内存，随后 GetChild() 按该下标索引 -> 越界读。
#include "StdAfx.h"
#include <cstdio>
#include "Core/UIMarkup.h"

static void probe(const wchar_t* label, const wchar_t* xml)
{
    DuiLib::CMarkup m;
    bool ok = m.Load(xml);
    printf("[probe] %-14ls Load()=%d IsValid()=%d\n", label, ok, m.IsValid());
    fflush(stdout);
    if (!ok) return;

    DuiLib::CMarkupNode root = m.GetRoot();
    printf("[probe]   GetRoot().IsValid()=%d\n", root.IsValid());
    fflush(stdout);
    if (!root.IsValid()) return;

    // 若拿到的是未初始化节点，这一步会按垃圾下标索引 m_pElements
    printf("[probe]   GetName()=%ls\n", root.GetName() ? root.GetName() : L"(null)");
    fflush(stdout);
    DuiLib::CMarkupNode child = root.GetChild();
    printf("[probe]   GetChild().IsValid()=%d\n", child.IsValid());
    fflush(stdout);
    if (child.IsValid()) {
        printf("[probe]   child name=%ls\n", child.GetName() ? child.GetName() : L"(null)");
        fflush(stdout);
    }
}

int main()
{
    probe(L"empty", L"");
    probe(L"whitespace", L"   \r\n\t ");
    probe(L"comment-only", L"<!-- nothing here -->");
    printf("[probe] done (no ASan report == no OOB)\n");
    return 0;
}
