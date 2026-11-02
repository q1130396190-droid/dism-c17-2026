// ============================================================================
//  lang_keys_probe.cpp -- 验证 102 个缺失语言键现在都能解析成中文
//
//  为什么需要独立探针：
//    StringManager 的内置表是 C++ 数组，最容易出错的地方是 \uXXXX 转义写错
//    （少一位、大小写错、被补丁层展开成真实字符）。这些错误编译期完全看不见，
//    运行时只表现为界面显示成 [key] —— 而那个症状和"键不在表里"一模一样。
//    所以必须真的把值取出来看一眼。
//
//  另外这层检查还覆盖一个常见误判：键"在表里"不等于"值是中文"。若转义坏了，
//    取出来会是乱码或空串，仍然要在界面上回退成 [key]。
// ============================================================================

#include <windows.h>
#include <cstdio>
#include <cwchar>
#include <cstring>

#include "StringManager.h"

namespace
{
    int g_pass = 0;
    int g_fail = 0;

    void Check(const char* pszKey, const char* pszUtf8ExpectedShort)
    {
        const wchar_t* p = i18n::CStringManager::GetInstance().Get(pszKey);
        if (!p || !p[0])
        {
            ++g_fail;
            std::printf("[FAIL] %-32s -> (empty)\n", pszKey);
            return;
        }
        // 回退形态长这样：[toolbox.ui_btn_save]
        if (p[0] == L'[' && std::wcslen(p) > 2 && p[std::wcslen(p) - 1] == L']')
        {
            ++g_fail;
            std::printf("[FAIL] %-32s -> still the [key] fallback\n", pszKey);
            return;
        }
        // 必须含非 ASCII 字符（中文）。纯 ASCII 值说明转义没生效或写成了英文。
        bool bHasWide = false;
        for (const wchar_t* q = p; *q; ++q)
            if (*q > 0x7F) { bHasWide = true; break; }
        if (!bHasWide)
        {
            ++g_fail;
            std::printf("[FAIL] %-32s -> ASCII only: %ls\n", pszKey, p);
            return;
        }
        ++g_pass;
        // 中文用 UTF-8 输出：控制台默认代码页下 %ls 会在首个非 ASCII 字符处截断，
        //   看起来像"值是空的"，实际数据是好的。先转 UTF-8 再打印。
        char szUtf8[512] = {};
        const int nOut = ::WideCharToMultiByte(CP_UTF8, 0, p, -1,
                                               szUtf8, sizeof(szUtf8), nullptr, nullptr);
        if (nOut <= 0) szUtf8[0] = '\0';
        std::printf("[PASS] %-32s -> %s  (%s)\n", pszKey, szUtf8, pszUtf8ExpectedShort);
    }
}

int main(int argc, char** argv)
{
    bool bInject = false;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "/Inject") == 0) bInject = true;
    std::printf("[INFO] lang key probe, inject=%d\n", bInject ? 1 : 0);

    if (bInject)
    {
        // 故意查一个必然不存在的键：若这里也报 PASS，说明 Get() 对缺失键
        //   没有走 [key] 回退，探针的判据本身就是假的。
        Check("zz.definitely.not.a.real.key", "injected missing key");
        std::printf("RESULT pass=%d fail=%d\n", g_pass, g_fail);
        // 注入模式要求检出失败，因此期望非零退出。
        return (g_fail > 0) ? 1 : 0;
    }


    // 三个最关键的：工具箱对话框标题与两个按钮
    Check("toolbox.ui_hosts_dlg_title", "hosts dialog title");
    Check("toolbox.ui_btn_save", "save button");
    Check("toolbox.ui_btn_cancel", "cancel button");

    // 各类代表性键（每组抽 1~2 个）
    Check("toolbox.ui_swm_title", "toolbox swm");
    Check("toolbox.ui_convert_title", "toolbox convert");
    // [R327] 本轮补齐的 11 个 toolbox 键：此前它们在内置表与 zh-CN 语言包
    //   里都不存在，界面会原样显示 [toolbox.ui_xxx]。这条断言把"键必须
    //   真的解析出中文"固化下来，避免以后新增文案又漏进表。
    Check("toolbox.s_hosts_write_fail", "hosts write failed title");
    Check("toolbox.ui_hosts_save_fail", "hosts save failed body");
    Check("toolbox.ui_temp_dir_fail", "temp dir prepare failed");
    Check("toolbox.ui_stage1_wim", "stage 1 wim title");
    Check("toolbox.ui_stage2_iso", "stage 2 iso title");
    Check("toolbox.ui_cancel_wim_export", "user cancelled export");
    Check("toolbox.ui_ex_esd2iso", "esd2iso exception");
    Check("toolbox.ui_ex_swm", "swm exception");
    Check("toolbox.ui_convert_fail_fmt", "convert failed with code");
    Check("toolbox.ui_wim_export_fail_fmt", "wim export failed with text");
    Check("toolbox.ui_task_running", "task running placeholder");
    Check("toolbox.win11prep_title", "toolbox win11prep");
    Check("feat.s_detailhint", "feature detail hint");
    Check("feat.s_empty_list", "feature empty list");
    Check("optim.s_copydetail_ok", "optim copy ok");
    Check("optim.s_copydetail_fail", "optim copy fail");
    Check("capability.s_enum_progress", "capability progress");
    Check("misc.logview_export", "logview export");
    Check("regclean.s_empty_hint", "regclean empty");

    std::printf("RESULT pass=%d fail=%d\n", g_pass, g_fail);
    if (g_fail != 0) return 1;
    std::printf("[PASS] every sampled key resolves to a Chinese string\n");
    return 0;
}
