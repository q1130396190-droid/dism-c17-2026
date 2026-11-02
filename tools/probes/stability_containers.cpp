// 稳定性探针 #1：容器 / 字符串 / 哈希表 的空指针与越界路径
// 每个用例由命令行参数选择，独立进程运行：某用例崩溃不影响其余用例的结论。
#include "StdAfx.h"
#include <cstdio>
#include <cstring>
#include "Utils/Utils.h"

typedef void (*CaseFn)();

#define CASE(name) static void name()
#define REG(name) { #name, name }

CASE(c_str_null)          { DuiLib::CDuiString s((LPCTSTR)nullptr); printf("  len=%d\n", s.GetLength()); }
CASE(c_compare_null)      { DuiLib::CDuiString s(_T("abc")); printf("  cmp=%d\n", s.Compare(nullptr)); }
CASE(c_comparenocase_null){ DuiLib::CDuiString s(_T("abc")); printf("  cmp=%d\n", s.CompareNoCase(nullptr)); }
CASE(c_find_null)         { DuiLib::CDuiString s(_T("abc")); printf("  find=%d\n", s.Find((LPCTSTR)nullptr)); }
CASE(c_replace_null)      { DuiLib::CDuiString s(_T("abc")); printf("  n=%d\n", s.Replace((LPCTSTR)nullptr, (LPCTSTR)nullptr)); }
CASE(c_mid_neg)           { DuiLib::CDuiString s(_T("abc")); printf("  mid='%ls'\n", s.Mid(-5, -5).GetData()); }
CASE(c_right_big)         { DuiLib::CDuiString s(_T("abc")); printf("  right='%ls'\n", s.Right(999).GetData()); }
CASE(c_assign_null)       { DuiLib::CDuiString s; s = (LPCTSTR)nullptr; printf("  len=%d\n", s.GetLength()); }
CASE(c_add_null)          { DuiLib::CDuiString s(_T("abc")); s += (LPCTSTR)nullptr; printf("  len=%d\n", s.GetLength()); }

CASE(m_find_null)   { DuiLib::CStdStringPtrMap m; printf("  found=%p\n", m.Find(nullptr)); }
CASE(m_insert_null) { DuiLib::CStdStringPtrMap m; printf("  ok=%d\n", m.Insert(nullptr, (LPVOID)1)); }
CASE(m_set_null)    { DuiLib::CStdStringPtrMap m; printf("  old=%p\n", m.Set(nullptr, (LPVOID)1)); }
CASE(m_remove_null) { DuiLib::CStdStringPtrMap m; printf("  ok=%d\n", m.Remove(nullptr)); }
CASE(m_empty_getat) { DuiLib::CStdStringPtrMap m; printf("  getat=%p size=%d\n", (void*)m.GetAt(0), m.GetSize()); }
CASE(m_resize0)     {
    DuiLib::CStdStringPtrMap m;
    m.Resize(0);
    printf("  insert_after_resize0=%d\n", m.Insert(_T("k"), (LPVOID)1));
    printf("  find_after_resize0=%p\n", m.Find(_T("k")));
}
CASE(m_churn) {
    DuiLib::CStdStringPtrMap m;
    TCHAR buf[32];
    for (int i = 0; i < 2000; ++i) {
        _stprintf_s(buf, _countof(buf), _T("k%d"), i);
        m.Insert(buf, (LPVOID)(intptr_t)(i + 1));
    }
    int found = 0;
    for (int i = 0; i < 2000; ++i) {
        _stprintf_s(buf, _countof(buf), _T("k%d"), i);
        if (m.Find(buf)) ++found;
    }
    printf("  inserted=2000 found=%d size=%d\n", found, m.GetSize());
    for (int i = 0; i < 2000; i += 2) {
        _stprintf_s(buf, _countof(buf), _T("k%d"), i);
        m.Remove(buf);
    }
    int found2 = 0;
    for (int i = 0; i < 2000; ++i) {
        _stprintf_s(buf, _countof(buf), _T("k%d"), i);
        if (m.Find(buf)) ++found2;
    }
    printf("  after_remove_evens found=%d size=%d (expect 1000/1000)\n", found2, m.GetSize());
}
CASE(m_getat_order) {
    // GetAt 的遍历顺序必须与从前一致；这里只验证可达性与不重不漏
    DuiLib::CStdStringPtrMap m;
    for (int i = 0; i < 5; ++i) {
        TCHAR b[16]; _stprintf_s(b, _countof(b), _T("k%d"), i);
        m.Insert(b, (LPVOID)(intptr_t)(i + 1));
    }
    for (int i = 0; i < m.GetSize(); ++i) printf("  getat(%d)=%ls\n", i, m.GetAt(i));
}

CASE(pa_index_empty) { DuiLib::CStdPtrArray a; LPVOID v = a[0]; printf("  size=%d v=%p\n", a.GetSize(), v); }
CASE(pa_bad_ops)     {
    DuiLib::CStdPtrArray a;
    a.Add((LPVOID)0x11);
    printf("  rm(-1)=%d getat(99)=%p setat(9)=%d\n", a.Remove(-1), a.GetAt(99), a.SetAt(9, (LPVOID)2));
}
CASE(pa_churn) {
    DuiLib::CStdPtrArray a;
    for (int i = 0; i < 500; ++i) a.Add((LPVOID)(intptr_t)(i + 1));
    for (int i = 0; i < 250; ++i) a.InsertAt(0, (LPVOID)(intptr_t)(1000 + i));
    for (int i = 0; i < 300; ++i) a.Remove(0);
    printf("  size=%d at0=%p last=%p\n", a.GetSize(), a.GetAt(0), a.GetAt(a.GetSize() - 1));
}

CASE(va_bad_ops) { DuiLib::CStdValArray va((int)sizeof(int), 1); int v = 7; va.Add(&v); printf("  size=%d rm(5)=%d\n", va.GetSize(), va.Remove(5)); }
CASE(va_small)   { DuiLib::CStdValArray va(1, 0); int v = 7; va.Add(&v); printf("  size=%d\n", va.GetSize()); }

struct Item { const char* name; CaseFn fn; };
static const Item kItems[] = {
    REG(c_str_null), REG(c_compare_null), REG(c_comparenocase_null), REG(c_find_null),
    REG(c_replace_null), REG(c_mid_neg), REG(c_right_big), REG(c_assign_null), REG(c_add_null),
    REG(m_find_null), REG(m_insert_null), REG(m_set_null), REG(m_remove_null),
    REG(m_empty_getat), REG(m_resize0), REG(m_churn), REG(m_getat_order),
    REG(pa_index_empty), REG(pa_bad_ops), REG(pa_churn),
    REG(va_bad_ops), REG(va_small),
};

int main(int argc, char** argv)
{
    if (argc < 2) {
        for (const auto& it : kItems) printf("%s\n", it.name);
        return 0;
    }
    for (const auto& it : kItems) {
        if (::strcmp(it.name, argv[1]) == 0) {
            printf("[case] %s\n", it.name);
            fflush(stdout);
            it.fn();
            printf("[ok] %s\n", it.name);
            return 0;
        }
    }
    printf("[case] UNKNOWN %s\n", argv[1]);
    return 2;
}
