// ============================================================================
//  SkinThemeDll.cpp -- 皮肤 DLL 的导出实现
//
//  设计要点：
//    1) 两个槽位（light / dark）各自一张静态表，全部在编译期固定，
//       运行时没有文件 IO、没有堆分配 —— 皮肤读取退化成一次线性查找。
//    2) 返回值指向 DLL 内部静态数据，调用方不得释放。
//       这样 exe 侧可以把字符串直接指向它（只读），省掉一次拷贝。
//    3) 导出 ABI 版本号：exe 侧先比对该号，不一致就整包拒绝并回退磁盘皮肤。
//       没有这一层的话，换皮肤格式后老 DLL 会被静默加载，
//       表现为"皮肤加载成功但界面错乱"，比直接失败更难查。
// ============================================================================

#include "SkinThemeDll.h"

// gen_skin_pack.py 生成的两个源文件各自定义一个命名空间，并通过
// SkinPack_<slot>_Entries / _Count 两个函数暴露静态表。
// 不用 extern const 对象：const 在 C++ 里默认内部链接，跨编译单元会 LNK2001。
const void* SkinPack_light_Entries(void);
int         SkinPack_light_Count(void);
const void* SkinPack_dark_Entries(void);
int         SkinPack_dark_Count(void);

namespace
{
    struct TSlot
    {
        const wchar_t* pszName;
        int            nCount;
        const void*    pEntries;   // 实际类型是上面对应命名空间里的 TEntry[]
    };

    // 槽位顺序必须与 SkinThemeDll.h 的 SKIN_SLOT_LIGHT / SKIN_SLOT_DARK 一致。
    // 经函数取表：生成侧的表是 static const，默认内部链接，不能跨 TU 直接引用。
    TSlot MakeSlot(const wchar_t* p, int (*pfnCount)(void),
                   const void* (*pfnEntries)(void))
    {
        TSlot s;
        s.pszName = p;
        s.nCount  = pfnCount();
        s.pEntries = pfnEntries();
        return s;
    }

    const TSlot g_slots[] = {
        MakeSlot(L"light", SkinPack_light_Count, SkinPack_light_Entries),
        MakeSlot(L"dark",  SkinPack_dark_Count,  SkinPack_dark_Entries),
    };
    const int g_slotCount = static_cast<int>(sizeof(g_slots) / sizeof(g_slots[0]));

    // 两个包共用的 entry 布局，这里用一份本地定义做类型转换。
    // 为什么不把 TEntry 放进公共头：公共头是 exe 与 DLL 的契约，
    // 它只需要"名字/字节/长度"三件事，不该把内部结构体也暴露出去。
    struct TAnyEntry
    {
        const wchar_t*        pszName;
        const unsigned char*  pData;
        unsigned int          cbData;
    };

    // 大小写不敏感比较：Windows 上皮肤路径大小写不应成为查找失败的原因。
    // 手写而不调 _wcsicmp：后者受区域设置影响，不同机器的排序结果可能不同，
    // 对"路径相等"这种语义没有意义。
    bool SameName(const wchar_t* a, const wchar_t* b)
    {
        if (!a || !b) return false;
        for (;;)
        {
            wchar_t ca = *a++;
            wchar_t cb = *b++;
            if (ca >= L'A' && ca <= L'Z') ca = static_cast<wchar_t>(ca - L'A' + L'a');
            if (cb >= L'A' && cb <= L'Z') cb = static_cast<wchar_t>(cb - L'A' + L'a');
            if (ca != cb) return false;
            if (ca == 0) return true;
        }
    }
}

SKIN_THEME_API int SkinTheme_ABI(void)
{
    return SKIN_THEME_ABI_VERSION;
}

SKIN_THEME_API int SkinTheme_Count(void)
{
    int n = 0;
    for (int i = 0; i < g_slotCount; ++i) n += g_slots[i].nCount;
    return n;
}

SKIN_THEME_API int SkinTheme_Enum(int nIndex, wchar_t* pszName, int cchName,
                                  unsigned int* pcbBytes)
{
    if (nIndex < 0) return 0;
    for (int s = 0; s < g_slotCount; ++s)
    {
        const TAnyEntry* p = static_cast<const TAnyEntry*>(g_slots[s].pEntries);
        for (int i = 0; i < g_slots[s].nCount; ++i)
        {
            if (nIndex-- == 0)
            {
                if (pszName && cchName > 0)
                {
                    int k = 0;
                    const wchar_t* src = p[i].pszName;
                    while (src[k] && k + 1 < cchName) { pszName[k] = src[k]; ++k; }
                    pszName[k] = 0;
                }
                if (pcbBytes) *pcbBytes = p[i].cbData;
                return s;
            }
        }
    }
    return -1;
}

SKIN_THEME_API const unsigned char* SkinTheme_Get(int nSlot, const wchar_t* pszName,
                                                  unsigned int* pcbBytes)
{
    if (pcbBytes) *pcbBytes = 0;
    if (nSlot < 0 || nSlot >= g_slotCount || !pszName) return nullptr;

    const TAnyEntry* p = static_cast<const TAnyEntry*>(g_slots[nSlot].pEntries);
    for (int i = 0; i < g_slots[nSlot].nCount; ++i)
    {
        if (SameName(p[i].pszName, pszName))
        {
            if (pcbBytes) *pcbBytes = p[i].cbData;
            return p[i].pData;
        }
    }
    return nullptr;
}
