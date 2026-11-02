// ============================================================



// StringManager.cpp



// ------------------------------------------------------------



// 多语言字符串管理器实现



// ------------------------------------------------------------



// 依赖：DuiLib CDuiString（UTF-8/const wchar_t* 混编）



// 说明：项目使用 UNICODE 宏，const wchar_t* == wchar_t*，



//       CDuiString::c_str() 返回 const wchar_t* (wchar_t*)



// ============================================================







#include "stdafx.h"



#include "StringManager.h"



#include <fstream>   // [C16] std::ifstream（F17_C_fileio 清零）



#include <cstdio>



#include <Shlwapi.h>   // PathIsDirectory



#include <shellapi.h>
#include "unzip.h"   // [原生化] 语言 ZIP 条目进程内解压，替代 powershell 压缩脚本
#include <string_view>   // [B5] C++17 提级（脚本 string_view 特征；标准库头零副作用）
#include "WinHandles.h"  // [C++17 R11] win::FindDataHandle







#pragma comment(lib, "shlwapi.lib")







namespace i18n {







// ------------------------------------------------------------



// 内置 zh-CN 回退表（编译进主程序，即使语言 DLL 全缺也能运行）



// ------------------------------------------------------------



// key -> 中文文本



// ------------------------------------------------------------



static const struct {



    const char*    key;



    const wchar_t* text;



} s_builtinZhCN[] = {



    // general



    { "general.ok",           L"\u786e\u5b9a" },               // 确定



    { "general.cancel",       L"\u53d6\u6d88" },               // 取消



    { "general.apply",        L"\u5e94\u7528" },               // 应用



    { "general.close",        L"\u5173\u95ed" },               // 关闭



    { "general.yes",          L"\u662f" },                    // 是



    { "general.no",           L"\u5426" },                    // 否



    { "general.retry",        L"\u91cd\u8bd5" },               // 重试



    { "general.exit",         L"\u9000\u51fa" },               // 退出



    { "general.confirm",      L"\u786e\u8ba4" },               // 确认



    { "general.browse",       L"\u6d4f\u89c8" },               // 浏览



    { "general.name",         L"\u540d\u79f0" },               // 名称



    { "general.cancelled",    L"\u5df2\u53d6\u6d88" },         // 已取消



    // cleanup



    { "cleanup.scan",         L"\u626b\u63cf" },               // 扫描



    { "cleanup.clean",        L"\u6e05\u7406" },               // 清理



    { "cleanup.release",      L"\u91ca\u653e" },               // 释放



    { "cleanup.scan_stop",    L"\u6b63\u5728\u505c\u6b62\u626b\u63cf\u2026" }, // 正在停止扫描…

    // [G16 2026-10-25] 缺失键补齐 + 如实表达"延迟生效"。
    //   原代码取 cleanup.s432ac3b6，但该键在 StringManager 与 lang/ 中都不存在，
    //   状态栏实际显示为空——用户按 Esc 后既无文字也无进度变化，看起来像没反应。
    //   DISM 组件清理是同步调用、系统层面不可中断，取消只能"阻止后续阶段"，
    //   文案必须说清楚这一点，否则用户会以为程序在假装响应。
    { "cleanup.s432ac3b6",    L"正在停止…（组件清理阶段需等当前步骤结束）" },



    { "cleanup.system",       L"\u6e05\u7406\u7cfb\u7edf" },  // 清理系统



    { "cleanup.release_img",  L"\u91ca\u653e\u5f71\u50cf" },  // 释放映像



    // optim



    { "optim.boot_items",     L"\u542f\u52a8\u9879" },         // 启动项



    { "optim.settings",       L"\u4f18\u5316\u8bbe\u7f6e" },   // 优化设置



    { "optim.services",       L"\u670d\u52a1" },               // 服务



    { "optim.remove_boot",    L"\u5220\u9664\u542f\u52a8\u9879" }, // 删除启动项



    // system



    { "system.pro",           L"\u4e13\u4e1a\u7248" },         // 专业版



    { "system.pro_workstation", L"\u4e13\u4e1a\u5de5\u4f5c\u7ad9\u7248" }, // 专业工作站版



    { "system.enterprise",    L"\u4f01\u4e1a\u7248" },         // 企业版



    { "system.edu",           L"\u6559\u80b2\u7248" },         // 教育版



    { "system.home",          L"\u5bb6\u5ead\u7248" },         // 家庭版



    // toolbox



    { "toolbox.edit_img",     L"\u7f16\u8f91\u5f71\u50cf\u6587\u4ef6" }, // 编辑映像文件



    { "toolbox.save_as",      L"\u53e6\u5b58\u4e3a\u5f71\u50cf" }, // 另存为映像



    { "toolbox.isogen",       L"ISO\u751f\u6210\u5668" },      // ISO生成器



    { "toolbox.account",      L"\u8d26\u6237\u7ba1\u7406" },   // 账户管理



    { "toolbox.restart_expl", L"\u91cd\u542f\u8d44\u6e90\u7ba1\u7406\u5668" }, // 重启资源管理器



    // error



    { "error.export_fail",    L"\u5bfc\u51fa\u5931\u8d25" },   // 导出失败



    { "error.write_fail",     L"\u65e0\u6cd5\u5199\u5165\u76ee\u6807\u6587\u4ef6\u3002" }, // 无法写入目标文件。



    { "error.tool_fail",      L"\u65e0\u6cd5\u542f\u52a8\u5de5\u5177\u811a\u672c\u3002" }, // 无法启动工具脚本。



    // confirm



    { "confirm.continue",     L"\u662f\u5426\u7ee7\u7eed\uff1f" }, // 是否继续？



    { "confirm.restart_now",  L"\u90e8\u5206\u4f18\u5316\u9700\u8981\u91cd\u542f\u8d44\u6e90\u7ba1\u7406\u5668\u624d\u80fd\u751f\u6548\u3002\r\n\u73b0\u5728\u91cd\u542f\u5417\uff1f" },



    // status



    { "status.ready",         L"\u5c31\u7eea" },               // 就绪



    { "status.stopping",      L"\u6b63\u5728\u505c\u6b62\u2026" }, // 正在停止…



    { "status.running",       L"\u6b63\u5728\u6267\u884c %d%%" }, // 正在执行 %d%%



    { "status.loading",       L"\u6b63\u5728\u52a0\u8f7d\u2026" }, // 正在加载…



    { "status.loading_hint",  L"\u6b63\u5728\u52a0\u8f7d\u6e05\u7406/\u4f18\u5316\u89c4\u5219\u2026" }, // 正在加载清理/优化规则…



    { "status.load_failed",   L"\u52a0\u8f7d\u5931\u8d25\uff0c\u89c4\u5219\u672c\u8f6e\u672a\u5237\u65b0" }, // 加载失败，规则本轮未刷新



    { "error.load_data",      L"\u52a0\u8f7d Data.xml \u89c4\u5219\u6587\u4ef6\u5931\u8d25\uff1a" }, // 加载 Data.xml 规则文件失败：



    { "error.load_title",     L"\u52a0\u8f7d\u5931\u8d25" },     // 加载失败



    // R23 / C-10: 扫描失败状态条与警告气泡标题



    { "status.scan_failed",           L"\u626b\u63cf\u5b8c\u6210\uff0c\u4f46\u5b58\u5728\u89c4\u5219\u626b\u63cf\u9519\u8bef" }, // 扫描完成，但存在规则扫描错误



    { "cleanup.scan_done_with_errors", L"\u626b\u63cf\u5b8c\u6210\uff08\u542b\u8b66\u544a\uff09" }, // 扫描完成（含警告）



    // R24 / A3: 扫描错误分类文案



    { "error.scan_access",  L"\u6743\u9650\u4e0d\u8db3" },  // 权限不足



    { "error.scan_path",    L"\u8def\u5f84\u4e0d\u5b58\u5728" }, // 路径不存在



    { "error.scan_io",      L"\u8bfb\u5199\u5931\u8d25" },   // 读写失败



    { "error.scan_timeout", L"\u8d85\u65f6" },               // 超时



    { "error.scan_unknown", L"\u672a\u77e5\u9519\u8bef" },   // 未知错误



    // R24 / S-14: 清理阶段标签



    { "phase.scanning",     L"\u626b\u63cf\u4e2d" },  // 扫描中



    { "phase.deleting",     L"\u5220\u9664\u4e2d" },  // 删除中



    // [R-CLEANPROG] 删除前逐规则重评阶段（与扫描同量级耗时，需要独立文案）
    { "phase.revalidating", L"\u6838\u5bf9\u89c4\u5219\u4e2d" },  // 核对规则中



    // [R-CLEANPROG] 删除阶段状态条副文案（原引用 cleanup.s89e99d91 键缺失，
    // 状态条会显示原始键名；此处补齐，语义="正在删除目标文件"）
    { "cleanup.s89e99d91",  L"\u6b63\u5728\u5220\u9664\u76ee\u6807\u6587\u4ef6" },  // 正在删除目标文件



    { "phase.custom",       L"\u81ea\u5b9a\u4e49\u547d\u4ee4" }, // 自定义命令



    // R26 / S-17: 扫描错误详情 section（追加到 clean_desc RichEdit）



    { "cleanup.scan_error_section", L"\u2588\u626b\u63cf\u9519\u8bef\u8be6\u60c5\r\n\r\n" },    // ▨扫描错误详情



    { "cleanup.scan_error_count",   L"\u5171 " },                // 共



    { "cleanup.scan_error_unit",    L" \u89c4\u5219\u62a5\u9519\r\n" }, // 条规则报错



    // hint



    { "hint.none",            L"\uff08\u65e0\uff09" },         // （无）



    { "hint.need_admin",      L"\uff08\u9700\u8981\u7ba1\u7406\u5458\u6743\u9650\uff09" }, // （需要管理员权限）



    { "hint.no_content",      L"\uff08\u65e0\u5185\u5bb9\uff09" }, // （无内容）



    // shortcut



    { "shortcut.find",        L"\u5728\u5f53\u524d\u9875\u9762\u5185\u67e5\u627e\uff08\u5feb\u6377\u952e Ctrl+F\uff09" },



    { "shortcut.refresh",     L"\u91cd\u65b0\u52a0\u8f7d\u89c4\u5219\u6570\u636e\uff08\u5feb\u6377\u952e F5\uff09" },



    { "shortcut.hard_del",    L"\u6c38\u4e45\u5220\u9664\u52fe\u9009\u9879\u76ee\uff08\u5feb\u6377\u952e F10\uff09\uff0c\u4e0d\u53ef\u6062\u590d" },



    // R106: 控制面板页硬编码兜底项（Page_Control kApplets 使用）



    // 中文名与 zh-CN_translations.json 保持一致；无 DLL 时同样正确显示



    { "ui.cp.programs",     L"\u7a0b\u5e8f\u548c\u529f\u80fd" },   // 程序和功能



    { "ui.cp.system",       L"\u7cfb\u7edf" },                     // 系统



    { "ui.cp.device",       L"\u8bbe\u5907\u7ba1\u7406\u5668" },   // 设备管理器



    { "ui.cp.network",      L"\u7f51\u7edc\u548c\u5171\u4eab\u4e2d\u5fc3" }, // 网络和共享中心



    { "ui.cp.power",        L"\u7535\u6e90\u9009\u9879" },         // 电源选项



    { "ui.cp.region",       L"\u533a\u57df\u548c\u8bed\u8a00" },   // 区域和语言



    { "ui.cp.datetime",     L"\u65e5\u671f\u548c\u65f6\u95f4" },   // 日期和时间



    { "ui.cp.defender",     L"Windows Defender" },



    { "ui.cp.admin",        L"\u7ba1\u7406\u5de5\u5177" },         // 管理工具



    { "ui.cp.user",         L"\u7528\u6237\u8d26\u6237" },         // 用户账户



    // R116-2: 控制面板兜底项扩展（管理工具 / 系统和安全 / 网络 / 外观常用项）



    { "ui.cp.gpedit",       L"\u7ec4\u7b56\u7565\u7f16\u8f91\u5668" },     // 组策略编辑器



    { "ui.cp.firewall",     L"Windows Defender \u9632\u706b\u5899" },



    { "ui.cp.internet",     L"Internet \u9009\u9879" },



    { "ui.cp.folder",       L"\u6587\u4ef6\u5939\u9009\u9879" },         // 文件夹选项



    { "ui.cp.recovery",     L"\u6062\u590d" },                          // 恢复



    { "ui.cp.remote",       L"\u8fdc\u7a0b\u684c\u9762" },              // 远程桌面



    // R117-1: 服务类型列文案（驱动服务枚举补齐后区分 Win32 服务 / 内核驱动 / 文件系统驱动 / 识别器驱动）



    { "svc.kind_win32",     L"Windows \u670d\u52a1" },



    { "svc.kind_kernel",    L"\u5185\u6838\u9a71\u52a8" },             // 内核驱动



    { "svc.kind_fs",        L"\u6587\u4ef6\u7cfb\u7edf\u9a71\u52a8" }, // 文件系统驱动



    { "svc.kind_recognizer",L"\u8bc6\u522b\u5668\u9a71\u52a8" },       // 识别器驱动

    { "svc.s_restart_badge", L"%s\uff08\u6062\u590d:%ds\uff09" },   // [R131-2] 恢复策略徽章

    { "svc.s_state_paused", L"\u5df2\u6682\u505c" },                 // [R131-2] SCM 细分状态

    { "svc.s_state_starting", L"\u6b63\u5728\u542f\u52a8" },         // [R131-2]

    { "svc.s_state_stopping", L"\u6b63\u5728\u505c\u6b62" },         // [R131-2]

    { "svc.s_state_pausing", L"\u6682\u505c/\u7ee7\u7eed\u4e2d" },   // [R131-2]

    { "optim.s_redo_empty",     L"\u6ca1\u6709\u53ef\u91cd\u505a\u7684\u4f18\u5316\u8bbe\u7f6e" },                       // [R-B1] Ctrl+Y 空栈

    { "optim.s_redo_empty_body", L"\u6700\u8fd1\u4e00\u6b21\u64a4\u9500\u540e\u6ca1\u6709\u65b0\u7684\u8bbe\u7f6e\u53ef\u91cd\u505a" },                       // [R-B1]

    // [R229] Empty-state hint for the rule list. Before a scan the list is genuinely
    //   empty and there was no text at all, so the page looked broken.
    { "regclean.s_empty_hint", L"点击工具栏的「扫描」开始检查可清理项" },
    { "regclean.s_nocopy",       L"\u8bf7\u5148\u9009\u4e2d\u4e00\u6761\u89c4\u5219" },                                   // [R-B1] Ctrl+C 无选中

    { "regclean.s_nocopy_body",  L"Ctrl+C \u9700\u8981\u5148\u70b9\u51fb\u9009\u4e2d\u4e00\u6761\u6e05\u7406\u89c4\u5219" },                              // [R-B1]

    { "regclean.s_copydone",     L"\u5df2\u590d\u5236\u89c4\u5219\u540d\uff1a%s" },                                        // [R-B1] 复制成功

    { "appx.s_nocopy",           L"\u5f53\u524d\u5217\u8868\u4e3a\u7a7a" },                                                  // [R-B1]

    { "appx.s_nocopy_body",      L"\u5f53\u524d Tab \u6ca1\u6709\u53ef\u590d\u5236\u7684 Appx \u5305" },                    // [R-B1]

    { "cap.s_nocopy",            L"\u5f53\u524d\u5217\u8868\u4e3a\u7a7a" },                                                  // [R-B1]

    { "cap.s_nocopy_body",       L"\u53ef\u9009\u529f\u80fd\u5217\u8868\u4e3a\u7a7a\uff0c\u65e0\u53ef\u590d\u5236\u9879" },                        // [R-B1]

    { "feat.s_nocopy",           L"\u5f53\u524d\u5217\u8868\u4e3a\u7a7a" },                                                  // [R-B1]

    { "feat.s_nocopy_body",      L"Windows \u529f\u80fd\u5217\u8868\u4e3a\u7a7a\uff0c\u65e0\u53ef\u590d\u5236\u9879" },                         // [R-B1]

    { "startup.s_nocopy",        L"\u5f53\u524d\u5217\u8868\u4e3a\u7a7a" },                                                  // [R-B1]

    { "startup.s_nocopy_body",   L"\u542f\u52a8\u9879\u5217\u8868\u4e3a\u7a7a\uff0c\u65e0\u53ef\u590d\u5236\u547d\u4ee4" },                      // [R-B1]

    { "unattend.s_nocopy",       L"\u5f53\u524d Pass \u7ec4\u4e3a\u7a7a" },                                                     // [R-B1]

    { "unattend.s_nocopy_body",  L"\u5f53\u524d Pass \u7ec4\u6ca1\u6709\u53ef\u590d\u5236\u7684\u547d\u4ee4" },                          // [R-B1]

    { "tasks.s_nocopy",          L"\u8bf7\u5148\u9009\u4e2d\u4e00\u4e2a\u8ba1\u5212\u4efb\u52a1" },                                 // [R-B1] Ctrl+C 无选中

    { "tasks.s_nocopy_body",     L"Ctrl+C \u9700\u8981\u5148\u70b9\u51fb\u9009\u4e2d\u4e00\u6761\u8ba1\u5212\u4efb\u52a1" },                       // [R-B1]

    { "tasks.s_nosave",          L"\u8bf7\u5148\u9009\u4e2d\u4e00\u4e2a\u8ba1\u5212\u4efb\u52a1" },                                 // [R-B1] Ctrl+S 无选中

    { "tasks.s_nosave_body",     L"Ctrl+S \u5bfc\u51fa\u9700\u8981\u5148\u9009\u4e2d\u4e00\u6761\u8ba1\u5212\u4efb\u52a1" },                       // [R-B1]

    { "program.s_nocopy",        L"\u8bf7\u5148\u9009\u4e2d\u4e00\u4e2a\u7a0b\u5e8f" },                                     // [R-B1] Ctrl+C 无选中

    { "program.s_nocopy_body",   L"Ctrl+C \u9700\u8981\u5148\u70b9\u51fb\u9009\u4e2d\u4e00\u4e2a\u5df2\u5b89\u88c5\u7a0b\u5e8f" },                     // [R-B1]

    { "program.s_copydone",      L"\u5df2\u590d\u5236\u7a0b\u5e8f\u540d\uff1a%s" },                                        // [R-B1] 复制成功

    { "assoc.s_nocopy",          L"\u5f53\u524d\u5217\u8868\u4e3a\u7a7a" },                                                  // [R-B1]

    { "assoc.s_nocopy_body",     L"\u6587\u4ef6\u5173\u8054\u5217\u8868\u4e3a\u7a7a\uff0c\u65e0\u53ef\u590d\u5236\u6269\u5c55\u540d" },                     // [R-B1]

    { "assoc.s_copydone",        L"\u5df2\u590d\u5236\u6269\u5c55\u540d\uff1a%s" },                                        // [R-B1] 复制成功





    // R118-1: 注册表清理还原点创建文案



    { "regclean.s_rp_done_title",  L"\u8fd8\u539f\u70b9\u5df2\u521b\u5efa" },   // 还原点已创建



    { "regclean.s_rp_done",        L"\u5df2\u521b\u5efa\u7cfb\u7edf\u8fd8\u539f\u70b9\uff1a\u6ce8\u518c\u8868\u6e05\u7406\u524d\u81ea\u52a8\u8fd8\u539f\u70b9" },



    { "regclean.s_rp_fail_title",  L"\u8fd8\u539f\u70b9\u521b\u5efa\u5931\u8d25" },   // 还原点创建失败



    { "regclean.s_rp_disabled",    L"\u7cfb\u7edf\u4fdd\u62a4\u672a\u542f\u7528\uff0c\u65e0\u6cd5\u521b\u5efa\u8fd8\u539f\u70b9" },



    { "regclean.s_rp_nopriv",      L"\u6743\u9650\u4e0d\u8db3\uff0c\u9700\u4ee5\u7ba1\u7406\u5458\u8eab\u4efd\u8fd0\u884c" },



    { "regclean.s_rp_nolib",       L"\u7cfb\u7edf\u8fd8\u539f\u7ec4\u4ef6\u52a0\u8f7d\u5931\u8d25" },



    { "regclean.s_rp_badparam",    L"\u53c2\u6570\u65e0\u6548" },



    { "regclean.s_rp_fail",        L"\u8fd8\u539f\u70b9\u521b\u5efa\u5931\u8d25" },

    { "regclean.s_menu_copy_key", L"\u590d\u5236\u76ee\u6807\u952e" },   // [R128-1]

    { "regclean.s_menu_copy_cmd", L"\u590d\u5236\u6e05\u7406\u547d\u4ee4" },   // [R128-1]

    { "regclean.s_menu_open_backup", L"\u6253\u5f00\u5907\u4efd\u76ee\u5f55" },   // [R128-1]

    { "regclean.s_menu_copied_key", L"\u5df2\u590d\u5236\u76ee\u6807\u952e\u5230\u526a\u8d34\u677f\uff1a\u0025\u0073" },   // [R128-1]

    { "regclean.s_menu_copied_cmd", L"\u5df2\u590d\u5236\u6e05\u7406\u547d\u4ee4\u5230\u526a\u8d34\u677f" },   // [R128-1]

    { "regclean.s_menu_nokey", L"\u8be5\u89c4\u5219\u672a\u542b\u53ef\u590d\u5236\u7684\u76ee\u6807\u952e\uff1a\u0025\u0073" },   // [R128-1]

    { "regclean.s_menu_nocmd", L"\u8be5\u89c4\u5219\u672a\u542b\u53ef\u590d\u5236\u7684\u6e05\u7406\u547d\u4ee4\uff1a\u0025\u0073" },   // [R128-1]

    { "regclean.s_menu_nobackup", L"\u672a\u627e\u5230\u5907\u4efd\u6587\u4ef6\uff1a\u0025\u0073" },   // [R128-1]

    { "regclean.s_cleanallfail", L"\u5168\u90e8\u0020\u0025\u0064\u0020\u6761\u89c4\u5219\u6267\u884c\u5931\u8d25\uff0c\u8bf7\u68c0\u67e5\u662f\u5426\u4ee5\u7ba1\u7406\u5458\u8eab\u4efd\u8fd0\u884c" },   // [R128-1]

    { "regclean.s_d3_firstfail", L"\u9996\u4e2a\u5931\u8d25\u89c4\u5219\uff1a%s\uff08\u76ee\u6807\u952e\uff1a%s\uff09" },   // [R131-1] D3 证据：失败规则定位



    // R107: 工具箱统一进度条状态文本（Page_Tools::OnToolProgressTimer 使用）



    { "toolbox.s_progress_running", L"\u6b63\u5728\u6267\u884c\u5de5\u5177\u7bb1\u5de5\u5177\uff1a%s ..." }, // 正在执行工具箱工具：%s ...



    { "toolbox.s_progress_running_pct", L"\u5de5\u5177\u7bb1\u5de5\u5177 %s \u6b63\u5728\u6267\u884c\uff0c\u5df2\u5b8c\u6210 %d%%\u3002" },



    { "toolbox.s_filter_noresult",    L"\u672a\u627e\u5230\u7b26\u5408\u6761\u4ef6\u7684\u5de5\u5177\uff0c\u8bf7\u8c03\u6574\u5173\u952e\u8bcd\u3002" },



    { "toolbox.s_count_all",          L"\u5171 %d \u4e2a\u5de5\u5177" },



    { "toolbox.s_count_filtered",     L"\u5df2\u5c55\u793a %d / %d \u4e2a\u5de5\u5177" },



    { "toolbox.s_refreshed",          L"\u5de5\u5177\u7bb1\u5df2\u5237\u65b0" },



    // R119-1: 工具箱分类筛选（分类名 / 分类 Combo 项）



    { "toolbox.cat_all",              L"\u5168\u90e8" },



    { "toolbox.cat_sys",              L"\u7cfb\u7edf" },



    { "toolbox.cat_image",            L"\u78c1\u76d8\u955c\u50cf" },



    { "toolbox.cat_util",             L"\u7cfb\u7edf\u5de5\u5177" },



    // R119-1: 工具风险等级徽章



    { "toolbox.risk_low",             L"\u4f4e\u98ce\u9669" },



    { "toolbox.risk_mid",             L"\u4e2d\u98ce\u9669" },



    { "toolbox.risk_high",            L"\u9ad8\u98ce\u9669" },



    // R119-1: 11 个工具卡片的一句话描述



    { "toolbox.td_0",                 L"DISM \u6355\u83b7\u7cfb\u7edf\u5377\u4e3a WIM \u6620\u50cf" },



    { "toolbox.td_1",                 L"\u5e94\u7528 WIM \u6620\u50cf\u8fd8\u539f\u7cfb\u7edf\u5206\u533a" },



    { "toolbox.td_2",                 L"\u679a\u4e3e\u5e76\u542f\u505c\u672c\u5730\u7528\u6237\u8d26\u6237" },



    { "toolbox.td_3",                 L"bcdboot \u91cd\u5efa\u7cfb\u7edf\u5f15\u5bfc\u73af\u5883" },



    { "toolbox.td_4",                 L"\u542f\u7528\u5185\u7f6e Administrator \u8d26\u6237" },



    { "toolbox.td_5",                 L"\u5c06 ESD \u8f6c\u6362\u4e3a\u6807\u51c6 ISO \u955c\u50cf" },



    { "toolbox.td_6",                 L"WIM \u4e0e ESD \u683c\u5f0f\u53cc\u5411\u8f6c\u6362" },



    { "toolbox.td_7",                 L"\u4f7f\u7528 ImageX \u5bfc\u51fa / \u5e94\u7528\u6620\u50cf" },



    { "toolbox.td_8",                 L"\u6253\u5f00\u7cfb\u7edf\u76ee\u5f55\u8fdb\u884c\u6587\u4ef6\u6d4f\u89c8" },



    { "toolbox.td_9",                 L"\u628a\u76ee\u5f55\u6253\u5305\u4e3a\u53ef\u542f\u52a8 ISO" },



    { "toolbox.td_10",                L"\u7528\u8bb0\u4e8b\u672c\u7f16\u8f91 hosts \u6620\u5c04" },



    { "toolbox.td_11",                L"备份与还原 Windows 激活信息" },

    { "toolbox.td_12",                L"选镜像直接处理：跳过联网向导并预置全员桌面图标" },

    { "toolbox.win11prep_title",      L"Windows 11 镜像处理" },

    { "toolbox.win11prep_no_image",   L"所选镜像文件不存在或无法访问。" },
    // [R92] 工具箱 UI 文案补迁：R91 实测本页仅剩 2 处真 UI 中文
    //   （其余 13 处是 [tools]/[compress] 日志前缀，按 R67 口径不进语言包；
    //     "春哥附体恢复说明.txt" 是文件名，不是文案）。
    { "toolbox.fdlg_image_filter",    L"Windows 映像 (*.wim;*.esd)\0*.wim;*.esd\0所有文件 (*.*)\0*.*\0\0" },
    { "toolbox.index_unnamed",        L"(未命名)" },

    { "toolbox.win11prep_read_fail",  L"读取映像索引失败：%s" },

    { "toolbox.win11prep_pick_index", L"索引 %u：%s\n版本：%s   %s\n\n（第 %d/%d 个索引）\n\n是否处理该索引？选“否”跳到下一个。" },

    { "toolbox.win11prep_bad_index",  L"所选索引不存在于该镜像中。" },

    { "toolbox.win11prep_mountdir_fail", L"无法创建临时挂载目录。请以管理员身份运行，或检查目标盘权限。" },

    { "toolbox.win11prep_confirm_image", L"即将处理镜像：\n%s\n索引：%u\n\n程序会自动挂载、写入预处理文件并提交改动。\n此操作会直接修改镜像本体，且不可撤销。\n\n是否继续？" },

    { "toolbox.win11prep_mount_fail", L"挂载镜像失败。\n\n常见原因：镜像正被其他程序占用、磁盘空间不足、或当前账户无管理员权限。" },

    { "toolbox.win11prep_unmount_fail", L"预处理已完成，但卸载镜像失败。\n\n镜像目前仍处于挂载状态，请先关闭其他占用程序后手动卸载，再进行后续操作。" },

    { "toolbox.win11prep_bad_version", L"仅支持 Windows 11 24H2（build 26100）及以上版本。" },

    { "toolbox.win11prep_bad_account", L"本地管理员账户名无效：需 1-20 个字符，且不能包含路径或账户保留字符。" },

    { "toolbox.win11prep_confirm",    L"将写入 unattend.xml 和首次登录桌面图标脚本。\\n\\n预处理不会绕过 Windows 产品激活，也不会保存账户密码。是否继续？" },

    { "toolbox.win11prep_ok",         L"处理完成，改动已提交到镜像。\n\n已写入 unattend.xml 与 Setup\\Scripts\\Win11DesktopIcons.cmd，首次开机将跳过联网向导并预置全部系统桌面图标。" },

    { "toolbox.win11prep_fail",       L"预处理失败：%s" },



    // toolbox (R01 激活备份向导，能力由独立组件 ActivationKit.dll 提供)

    { "toolbox.act_tip",              L"备份或还原系统激活信息（数字许可证 / 产品密钥）" },

    { "toolbox.act_title",            L"激活备份与还原" },

    { "toolbox.act_choose_body",      LR"(请选择要执行的操作：

“是”——备份当前系统的激活信息；
“否”——从已有备份还原激活信息；
“取消”——退出。)" },

    { "toolbox.act_pick_b",           L"请选择用于【保存】激活备份的文件夹" },

    { "toolbox.act_pick_r",           L"请选择包含【激活备份】的文件夹（其下应有“激活备份”子目录）" },

    { "toolbox.act_no_backup",        L"所选位置未找到激活备份（Backup.ini），无法还原。" },

    { "toolbox.act_backup_warn",      LR"(即将把当前系统的激活信息备份到所选文件夹。

请注意：备份仅适用于零售/OEM 产品密钥的迁移；
Windows 10/11 数字许可证与本机硬件绑定，备份不能将授权转移到其他设备。

是否继续备份？)" },

    { "toolbox.act_restore_warn",     LR"(即将用备份覆盖当前系统的激活信息，还原后通常需重启方可生效。
该操作存在风险，确定继续吗？)" },

    { "toolbox.act_backup_ok",        LR"(激活信息备份完成。

请妥善保管备份文件夹（“激活备份”子目录及其 Backup.ini）。)" },

    { "toolbox.act_restore_ok",       LR"(激活信息已提交还原。

建议重启计算机后，在系统属性中确认激活状态。)" },

    { "toolbox.act_missing",          L"激活备份组件（ActivationKit.dll）未就绪，该功能暂不可用。" },



    // toolbox (R-B24 \u5de5\u5177\u5361\u53f3\u952e\u83dc\u5355)



    { "toolbox.s_ctx_run", L"\u542f\u52a8\u5de5\u5177" },



    { "toolbox.s_ctx_copydesc", L"\u590d\u5236\u5de5\u5177\u63cf\u8ff0" },



    { "toolbox.s_ctx_copydesc_done", L"\u5df2\u590d\u5236\u5de5\u5177\u63cf\u8ff0" },



    { "toolbox.s_ctx_copydesc_done_body", L"\u5de5\u5177\u63cf\u8ff0\u5df2\u590d\u5236\u5230\u526a\u8d34\u677f\u3002" },



    { "toolbox.s_ctx_dep", L"\u67e5\u770b\u5de5\u5177\u4f9d\u8d56" },

    { "toolbox.s_ctx_depgraph",           L"\u67e5\u770b\u5de5\u5177\u4f9d\u8d56\u56fe" },



    { "toolbox.s_ctx_dep_ok", L"\u5de5\u5177\u4f9d\u8d56 %s \u5df2\u5b89\u88c5\uff0c\u53ef\u6b63\u5e38\u542f\u52a8\u3002" },



    { "toolbox.s_ctx_dep_missing", L"\u5de5\u5177\u4f9d\u8d56 %s \u672a\u5b89\u88c5\uff0c\u5c0f\u5de5\u5177\u5df2\u7f6e\u7070\uff08\u53ef\u901a\u8fc7 Windows ADK \u5b89\u88c5\uff09\u3002" },





    { "toolbox.s_progress_done",    L"\u5de5\u5177\u7bb1\u5de5\u5177 %s \u5df2\u5b8c\u6210\u3002" },         // 工具箱工具 %s 已完成。



    // R108: 文件关联页批量应用（Page_Assoc::OnBatchApply 使用）



    { "assoc.s_batch_title",          L"\u6279\u91cf\u5e94\u7528\u9ed8\u8ba4\u7a0b\u5e8f" },             // 批量应用默认程序

    { "assoc.s_menu_copy_ext", L"\u590d\u5236\u6269\u5c55\u540d" },   // [R128-2]

    { "assoc.s_menu_copy_progid", L"\u590d\u5236\u0020\u0050\u0072\u006f\u0067\u0049\u0064" },   // [R128-2]

    { "assoc.s_menu_copy_cmd", L"\u590d\u5236\u6253\u5f00\u547d\u4ee4" },   // [R128-2]

    { "assoc.s_menu_open_reg", L"\u6253\u5f00\u5173\u8054\u6ce8\u518c\u8868\u9879" },   // [R128-2]

    { "assoc.s_menu_set_default", L"\u8bbe\u4e3a\u9ed8\u8ba4\u7a0b\u5e8f" },   // [R128-2]

    { "assoc.s_menu_copy_handler", L"\u590d\u5236\u5904\u7406\u7a0b\u5e8f\u540d" },   // [R128-2]

    { "assoc.s_menu_copied_ext", L"\u5df2\u590d\u5236\u6269\u5c55\u540d\uff1a\u0025\u0073" },   // [R128-2]

    { "assoc.s_menu_copied_progid", L"\u5df2\u590d\u5236\u0020\u0050\u0072\u006f\u0067\u0049\u0064\uff1a\u0025\u0073" },   // [R128-2]

    { "assoc.s_menu_copied_cmd", L"\u5df2\u590d\u5236\u6253\u5f00\u547d\u4ee4\uff1a\u0025\u0073" },   // [R128-2]

    { "assoc.s_menu_copied_handler", L"\u5df2\u590d\u5236\u5904\u7406\u7a0b\u5e8f\u540d\uff1a\u0025\u0073" },   // [R128-2]

    { "assoc.s_menu_noprocess", L"\u8be5\u6269\u5c55\u540d\u672a\u5173\u8054\u4efb\u4f55\u0020\u0050\u0072\u006f\u0067\u0049\u0064\uff1a\u0025\u0073" },   // [R128-2]

    { "assoc.s_menu_nocmd", L"\u8be5\u6269\u5c55\u540d\u672a\u914d\u7f6e\u6253\u5f00\u547d\u4ee4\uff1a\u0025\u0073" },   // [R128-2]

    { "assoc.s_menu_nohandler", L"\u672a\u679a\u4e3e\u5230\u5173\u8054\u5904\u7406\u7a0b\u5e8f\uff1a\u0025\u0073" },   // [R128-2]

    { "assoc.s_menu_default_title", L"\u8bbe\u4e3a\u9ed8\u8ba4\u7a0b\u5e8f" },   // [R128-2]

    { "assoc.s_menu_default_confirm", L"\u5c06\u628a\u300c\u0025\u0073\u300d\u9ed8\u8ba4\u5173\u8054\u5230\u300c\u0025\u0073\u300d\uff0c\u5e76\u540c\u6b65\u0020\u004f\u0070\u0065\u006e\u0057\u0069\u0074\u0068\u0050\u0072\u006f\u0067\u0069\u0064\u0073\u002f\u004f\u0070\u0065\u006e\u0057\u0069\u0074\u0068\u004c\u0069\u0073\u0074\u3002\u662f\u5426\u7ee7\u7eed\uff1f" },   // [R128-2]

    { "assoc.s_menu_default_ok_short", L"\u5df2\u8bbe\u4e3a\u9ed8\u8ba4" },   // [R128-2]

    { "assoc.s_menu_default_ok", L"\u300c\u0025\u0073\u300d\u5df2\u8bbe\u4e3a\u9ed8\u8ba4\u5173\u8054\u7a0b\u5e8f\u3002" },   // [R128-2]

    { "assoc.s_w_title", L"\u5199\u5165\u5173\u8054\u5931\u8d25" },   // [R128-2]

    { "assoc.s_w_noperm", L"\u6743\u9650\u4e0d\u8db3\uff0c\u65e0\u6cd5\u5199\u5165\u300c\u0025\u0073\u300d\u7684\u5173\u8054\uff08\u8bf7\u4ee5\u7ba1\u7406\u5458\u8eab\u4efd\u8fd0\u884c\uff09\u3002" },   // [R128-2]

    { "assoc.s_w_invprogid", L"\u300c\u0025\u0073\u300d\u7684\u0020\u0050\u0072\u006f\u0067\u0049\u0064\u0020\u65e0\u6548\uff0c\u8bf7\u9009\u62e9\u542b\u53cd\u659c\u6760\uff08\u5982\u0020\u004e\u006f\u0074\u0065\u0070\u0061\u0064\u002e\u0065\u0078\u0065\uff09\u7684\u0020\u0050\u0072\u006f\u0067\u0049\u0064\u3002" },   // [R128-2]

    { "assoc.s_w_iassoc", L"\u300c\u0025\u0073\u300d\u5173\u8054\u679a\u4e3e\u5931\u8d25\uff08\u0049\u0041\u0073\u0073\u006f\u0063\u0020\u5c42\u9519\u8bef\uff09\u3002" },   // [R128-2]

    { "assoc.s_w_regfail", L"\u5199\u5165\u300c\u0025\u0073\u300d\u5173\u8054\u65f6\u6ce8\u518c\u8868\u5931\u8d25\uff0c\u8bf7\u68c0\u67e5\u6743\u9650\u6216\u8def\u5f84\u3002" },   // [R128-2]

    { "assoc.s_w_verify", L"\u300c\u0025\u0073\u300d\u5173\u8054\u5199\u5165\u540e\u56de\u8bfb\u6821\u9a8c\u5931\u8d25\u3002" },   // [R128-2]

    { "assoc.s_w_other", L"\u5199\u5165\u300c\u0025\u0073\u300d\u5173\u8054\u8fd4\u56de\u9519\u8bef\u7801\u0020\u0025\u0064\uff0c\u8bf7\u67e5\u770b\u65e5\u5fd7\u3002" },   // [R128-2]



    { "assoc.s_batch_confirm",        L"\u5373\u5c06\u628a %d \u4e2a\u6269\u5c55\u540d\u5173\u8054\u5230\u300c%s\u300d\u3002\r\n\u786e\u5b9a\u7ee7\u7eed\u5417\uff1f" },



    { "assoc.s_batch_noselect",       L"\u672a\u9009\u62e9\u8981\u5e94\u7528\u7684 ProgId" },



    { "assoc.s_batch_noselect_detail",L"\u8bf7\u5148\u5728\u4e0a\u65b9\u5217\u8868\u9009\u62e9\u4e00\u4e2a\u6269\u5c55\u540d\uff0c\u67e5\u770b\u5176 ProgId \u540e\u518d\u6267\u884c\u6279\u91cf\u5e94\u7528\u3002" },



    { "assoc.s_batch_noext",          L"\u6ca1\u6709\u53ef\u6279\u91cf\u5e94\u7528\u7684\u6269\u5c55\u540d" },



    { "assoc.s_batch_noneed",         L"\u6240\u9009\u6269\u5c55\u540d\u5df2\u5168\u90e8\u5173\u8054\u5230\u8be5 ProgId\uff0c\u65e0\u9700\u66f4\u65b0\u3002" },



    { "assoc.s_batch_ok_short",       L"\u6279\u91cf\u5e94\u7528\u6210\u529f" },



    { "assoc.s_batch_ok",             L"\u5df2\u5c06 %d \u4e2a\u6269\u5c55\u540d\u5173\u8054\u5230\u300c%s\u300d\u3002" },



    { "assoc.s_batch_fail_short",     L"\u6279\u91cf\u5e94\u7528\u90e8\u5206\u5931\u8d25" },



    { "assoc.s_batch_fail",           L"\u6210\u529f %d \u9879\u3001\u5931\u8d25 %d \u9879\uff0c\u8bf7\u67e5\u770b\u65e5\u5fd7\u4e86\u89e3\u8be6\u60c5\u3002" },



    { "ctrl.s_filter_canononly",      L"\u5f53\u524d %d \u9879\u4ec5\u7531 canonical \u540d\u79f0\u547d\u4e2d\uff0c\u5df2\u5c55\u793a %d \u9879\u3002" },

    { "ctrl.s_menu_runas",      L"\u4ee5\u7ba1\u7406\u5458\u8eab\u4efd\u6253\u5f00(&R)" },

    { "ctrl.s_runas_fail",      L"\u4ee5\u7ba1\u7406\u5458\u8eab\u4efd\u6253\u5f00\u5931\u8d25\uff1a%s" },

    { "ctrl.s_open_denied",      L"\u6743\u9650\u4e0d\u8db3\uff0c\u65e0\u6cd5\u6253\u5f00\uff1a%s\uff08\u8bf7\u4ee5\u7ba1\u7406\u5458\u8eab\u4efd\u91cd\u8bd5\uff09" },

    { "ctrl.s_open_noassoc",      L"\u65e0\u5173\u8054\u7a0b\u5e8f\u53ef\u6253\u5f00\uff1a%s" },

    { "ctrl.s_open_canceled",      L"\u7528\u6237\u53d6\u6d88\u4e86\u6253\u5f00\u64cd\u4f5c\uff1a%s" },



    // R111: Welcome 刷新失败分级 Toast



    { "welcome.s_refresh_title",          L"\u7cfb\u7edf\u72b6\u6001\u5237\u65b0" },



    { "welcome.s_refresh_bothfail",       L"\u7cfb\u7edf\u6982\u89c8\u83b7\u53d6\u5931\u8d25\uff08err=0x%08X\uff09\uff0c\u901a\u77e5\u72b6\u6001\u83b7\u53d6\u5931\u8d25\uff08err=0x%08X\uff09\uff0c\u5361\u7247\u663e\u793a\u515c\u5e95\u6587\u6848\u3002" },



    { "welcome.s_refresh_ovfail",         L"\u7cfb\u7edf\u6982\u89c8\u83b7\u53d6\u5931\u8d25\uff08err=0x%08X\uff09\uff0c\u64cd\u4f5c\u7cfb\u7edf/\u78c1\u76d8/\u5185\u5b58\u5361\u7247\u5df2\u663e\u793a\u515c\u5e95\u6587\u6848\u3002" },



    { "welcome.s_refresh_ntfail",         L"\u7528\u6237\u901a\u77e5\u72b6\u6001\u83b7\u53d6\u5931\u8d25\uff08err=0x%08X\uff09\uff0c\u901a\u77e5\u5361\u7247\u5df2\u663e\u793a\u515c\u5e95\u6587\u6848\u3002" },



    { "welcome.s_detail_fail_title", L"\u7cfb\u7edf\u72b6\u6001\u91c7\u96c6" },



    { "welcome.s_detail_fail_body", L"\u4ee5\u4e0b API \u91c7\u96c6\u5931\u8d25\uff1a%s" },



    { "welcome.s_detail_fail_none", L"\u5168\u90e8\u7cfb\u7edf\u72b6\u6001 API \u91c7\u96c6\u6b63\u5e38\u3002" },



    // R111: 工具箱 worker 失败分级 Toast



    { "toolbox.s_done_title",             L"\u5de5\u5177\u6267\u884c\u7ed3\u679c" },



    { "toolbox.s_done_ok",                L"\u5de5\u5177\u6267\u884c\u6210\u529f\uff08\u9000\u51fa\u7801 0\uff09\u3002" },



    { "toolbox.s_done_nowait",            L"\u5de5\u5177\u6267\u884c\u672a\u7b49\u5f85\uff08\u9000\u51fa\u7801 -1\uff09\uff0c\u65e0\u6cd5\u786e\u8ba4\u7ed3\u679c\u3002" },



    { "toolbox.s_done_fail",              L"\u5de5\u5177\u6267\u884c\u5931\u8d25\uff08\u9000\u51fa\u7801 %d\uff09\uff0c\u8bf7\u68c0\u67e5\u4e0a\u65b9\u8f93\u51fa\u3002" },



    // R129-1: 工具箱 CreateProcess/WIM 失败分级（D3/D4 证据路径）



    { "toolbox.s_done_cp",                L"\u5de5\u5177\u8fdb\u7a0b CreateProcess \u542f\u52a8\u5931\u8d25\uff08\u9519\u8bef\u7801 %lu\uff09\uff0c\u8bf7\u68c0\u67e5\u8def\u5f84\u6216\u6743\u9650\u3002" },



    { "toolbox.s_wim_noperm",             L"WIM \u91ca\u653e\uff08wimgapi\uff09\u5931\u8d25\uff1a\u6743\u9650\u4e0d\u8db3\uff0c\u8bf7\u4ee5\u7ba1\u7406\u5458\u8eab\u4efd\u8fd0\u884c\u3002" },



    { "toolbox.s_wim_diskfull",           L"WIM \u91ca\u653e\uff08wimgapi\uff09\u5931\u8d25\uff1a\u76ee\u6807\u78c1\u76d8\u7a7a\u95f4\u4e0d\u8db3\u6216\u5bb9\u91cf\u9519\u8bef\u3002" },



    { "toolbox.s_wim_locked",             L"WIM \u91ca\u653e\uff08wimgapi\uff09\u5931\u8d25\uff1a\u76ee\u6807\u5377\u88ab\u5360\u7528\u6216\u5df2\u9501\u5b9a\uff0c\u8bf7\u5173\u95ed\u5176\u4ed6\u7a0b\u5e8f\u540e\u91cd\u8bd5\u3002" },



    { "toolbox.s_wim_other",              L"WIM \u91ca\u653e\uff08wimgapi\uff09\u5931\u8d25\uff0c\u5df2\u56de\u9000\u5230 DISM \u5916\u90e8\u547d\u4ee4\u3002" },



    // R130: Unattend MSXML DOM 直连分级

    { "unattend.s_dom_export_fail_title", L"\u9884\u5e94\u7b54 XML \u5bfc\u51fa DOM \u89e3\u6790\u5931\u8d25" },

    { "unattend.s_dom_import_fail_title", L"\u9884\u5e94\u7b54 XML \u5bfc\u5165 DOM \u89e3\u6790\u5931\u8d25" },



    // R111: Startup 右键菜单复制命令



    { "startup.s_menu_copy_cmd",          L"\u590d\u5236\u547d\u4ee4(&C)" },



    { "startup.s_menu_copied_cmd",        L"\u5df2\u5c06\u542f\u52a8\u547d\u4ee4\u590d\u5236\u5230\u526a\u8d34\u677f\uff1a%s" },



    // R111: Services 右键菜单复制服务名



    { "svc.s_menu_copy_name",             L"\u590d\u5236\u670d\u52a1\u540d(&C)" },

    { "svc.s_ctx_depgraph",              L"\u67e5\u770b\u4f9d\u8d56\u56fe" },



    { "svc.s_menu_copied_name",           L"\u5df2\u5c06\u670d\u52a1\u540d\u590d\u5236\u5230\u526a\u8d34\u677f\uff1a%s" },



    // R113: 计划任务三动作失败分级 Toast（Page_Tasks::OnToggle/OnRun/OnDelete）



    { "task.s_toggle_fail_title",         L"\u8ba1\u5212\u4efb\u52a1\u542f\u505c\u5931\u8d25" },



    { "task.s_run_fail_title",            L"\u8ba1\u5212\u4efb\u52a1\u8fd0\u884c\u5931\u8d25" },



    { "task.s_delete_fail_title",         L"\u8ba1\u5212\u4efb\u52a1\u5220\u9664\u5931\u8d25" },



{ "task.s_menu_open_sched", L"\u6253\u5f00\u4efb\u52a1\u8ba1\u5212\u7a0b\u5e8f(&O)" },

{ "task.s_menu_export_xml", L"\u5bfc\u51fa\u4efb\u52a1 XML(&X)" },

{ "task.s_menu_copy_path", L"\u590d\u5236\u4efb\u52a1\u8def\u5f84(&P)" },

{ "task.s_menu_copy_name", L"\u590d\u5236\u4efb\u52a1\u540d(&N)" },

{ "task.s_export_dlg_title", L"\u5bfc\u51fa\u8ba1\u5212\u4efb\u52a1 XML" },

{ "task.s_export_ok", L"\u8ba1\u5212\u4efb\u52a1 XML \u5df2\u5bfc\u51fa" },

{ "task.s_export_fail", L"\u8ba1\u5212\u4efb\u52a1 XML \u5bfc\u51fa\u5931\u8d25" },

{ "task.s_err_write", L"\u5199\u5165\u5bfc\u51fa\u6587\u4ef6\u5931\u8d25" },

{ "task.s_err_open_sched", L"\u6253\u5f00\u4efb\u52a1\u8ba1\u5212\u7a0b\u5e8f\u5931\u8d25" },

{ "task.s_open_sched_fail", L"\u65e0\u6cd5\u6253\u5f00\u4efb\u52a1\u8ba1\u5212\u7a0b\u5e8f\uff08taskschd.msc\uff09" },

{ "task.s_copied_path", L"\u8ba1\u5212\u4efb\u52a1\u8def\u5f84\u5df2\u590d\u5236\u5230\u526a\u8d34\u677f" },

{ "task.s_copied_name", L"\u8ba1\u5212\u4efb\u52a1\u540d\u5df2\u590d\u5236\u5230\u526a\u8d34\u677f" },

    // R113: Startup 删除启动项失败分级



    { "startup.s_delete_fail_title",      L"\u542f\u52a8\u9879\u5220\u9664\u5931\u8d25" },



    { "startup.s_arch_x64",               L"x64" },



    { "startup.s_arch_x86",               L"x86" },



    { "startup.s_arch_na",                L"-" },



    { "optim.sgpol_run",                  L"\u7ec4\u7b56\u7565\u542f\u52a8\u9879" },



    // R113: Services 删除 / 停止前置步骤失败分级



    { "svc.s_delete_fail_title",          L"\u670d\u52a1\u5220\u9664\u5931\u8d25" },



    { "svc.s_stop_fail_title",            L"\u505c\u6b62\u670d\u52a1\u5931\u8d25\uff08\u5220\u9664\u524d\u7f6e\u6b65\u9aa4\uff09" },



    // R125-2: Services 控制失败统一分级（code 1-11，ControlServiceItem 全码映射）



    { "svc.s_err_scm",                L"\u65e0\u6cd5\u6253\u5f00\u670d\u52a1\u63a7\u5236\u7ba1\u7406\u5668\uff08SCM\uff09\u3002" },



    { "svc.s_err_start",              L"\u542f\u52a8\u670d\u52a1\u5931\u8d25\uff08\u9519\u8bef\u7801 %lu\uff09\u3002" },



    { "svc.s_err_stop",               L"\u505c\u6b62\u670d\u52a1\u5931\u8d25\uff08\u9519\u8bef\u7801 %lu\uff09\u3002" },



    { "svc.s_err_stop_timeout",       L"\u7b49\u5f85\u670d\u52a1\u505c\u6b62\u8d85\u65f6\uff08\u90e8\u5206\u670d\u52a1\u505c\u6b62\u8017\u65f6\u8f83\u957f\uff0c\u53ef\u7a0d\u540e\u5237\u65b0\u67e5\u770b\uff09\u3002" },



    { "svc.s_err_restart",            L"\u91cd\u65b0\u542f\u52a8\u670d\u52a1\u5931\u8d25\uff08\u9519\u8bef\u7801 %lu\uff09\u3002" },



    { "svc.s_err_state_timeout",      L"\u670d\u52a1\u72b6\u6001\u5207\u6362\u672a\u5728\u9884\u671f\u65f6\u95f4\u5185\u5b8c\u6210\uff0c\u53ef\u7a0d\u540e\u5237\u65b0\u9875\u9762\u67e5\u770b\u3002" },



    { "svc.s_err_open",               L"\u6253\u5f00\u670d\u52a1\u300c%s\u300d\u5931\u8d25\uff08\u53ef\u80fd\u53d7\u7cfb\u7edf\u4fdd\u62a4\uff09\uff0c\u9519\u8bef\u7801\uff1a%lu" },



    { "svc.s_err_unknown",            L"\u670d\u52a1\u63a7\u5236\u5931\u8d25\uff08\u5185\u90e8 code=%d\uff0c\u9519\u8bef\u7801 %lu\uff09\u3002" },



    { "svc.s5c5d6e7",                 L"\u7b49\u5f85\u670d\u52a1\u6682\u505c\u8d85\u65f6\u3002" },



    { "svc.s6d6e7f8",                 L"\u7b49\u5f85\u670d\u52a1\u7ee7\u7eed\u8d85\u65f6\u3002" },



    { "svc.sf3a3b4c",                 L"\u6682\u505c\u670d\u52a1\u5931\u8d25\uff08\u9519\u8bef\u7801 %lu\uff09\u3002" },



    { "svc.sf4b4c5d",                 L"\u7ee7\u7eed\u670d\u52a1\u5931\u8d25\uff08\u9519\u8bef\u7801 %lu\uff09\u3002" },



    // R113: Program 卸载失败分级 Toast



    { "program.s_uninst_fail_title",      L"程序卸载失败" },



    { "program.s_batch_fail_title",       L"批量卸载未全部成功" },



    { "program.s_batch_fail",             L"成功 %d 个 / 失败 %d 个（共 %d 个），详情见上方日志。" },



    { "program.s_batch_running",          L"上一批卸载仍在进行中，请等待完成后再发起。" },



    { "program.s_batch_started",          L"[program] 批量卸载已转入后台执行，完成后自动刷新列表" },



    // R113: Unattend Schema 结构校验失败分级



    { "unattend.s_schema_invalid_title",   L"预应答 Schema 结构校验失败" },



    // R114-1: Driver 详情面板增强（设备状态 / 问题代码 / 驱动服务名 / 设备实例 ID）



    { "driver.s_devstatus_lbl",          L"\u8bbe\u5907\u72b6\u6001" },



    { "driver.s_devstatus_ok",           L"\u6b63\u5e38" },



    { "driver.s_devstatus_problem",      L"\u6709\u95ee\u9898" },



    { "driver.s_devstatus_disabled",     L"\u672a\u542f\u52a8" },



    { "driver.s_problemcode_lbl",        L"\u95ee\u9898\u4ee3\u7801" },



    { "driver.s_problem_none",           L"\u65e0" },



    { "driver.s_service_lbl",            L"\u9a71\u52a8\u670d\u52a1\u540d" },



    { "driver.s_devinst_lbl",            L"\u8bbe\u5907\u5b9e\u4f8b ID" },



    // R114-2: Optimization 右键菜单增强（打开位置 / 复制名称 / 复制命令 / 恢复默认值）



    { "optim.s_menu_openloc",         L"\u6253\u5f00\u4f4d\u7f6e(&O)" },



    { "optim.s_menu_copyname",        L"\u590d\u5236\u540d\u79f0(&N)" },



    { "optim.s_menu_copycmd",         L"\u590d\u5236\u547d\u4ee4(&C)" },



    { "optim.s_menu_restore",         L"\u6062\u590d\u9ed8\u8ba4\u503c(&R)" },



    { "optim.s_menu_openloc_ok",      L"\u5df2\u6253\u5f00\u4f4d\u7f6e\uff1a%s" },



    { "optim.s_menu_openloc_fail",    L"\u6253\u5f00\u4f4d\u7f6e\u5931\u8d25\uff1a%s" },



    { "optim.s_menu_openloc_empty",   L"\u300c%s\u300d\u65e0\u53ef\u6253\u5f00\u7684\u52a8\u4f5c\u4f4d\u7f6e" },



    { "optim.s_menu_copyname_ok",     L"\u5df2\u590d\u5236\u89c4\u5219\u540d\u79f0\uff1a%s" },



    { "optim.s_menu_copyname_fail",   L"\u590d\u5236\u89c4\u5219\u540d\u79f0\u5931\u8d25\uff1a%s" },



    { "optim.s_menu_copycmd_ok",      L"\u5df2\u590d\u5236\u547d\u4ee4\uff1a%s" },



    { "optim.s_menu_copycmd_fail",    L"\u590d\u5236\u547d\u4ee4\u5931\u8d25\uff1a%s" },



    { "optim.s_menu_copycmd_empty",   L"\u300c%s\u300d\u65e0\u53ef\u7528\u547d\u4ee4" },



    { "optim.s_menu_restore_ok",      L"\u300c%s\u300d\u5df2\u6062\u590d\u9ed8\u8ba4\u503c" },











    // R114-1: Driver 详情面板增强（设备状态 / 问题代码 / 驱动服务名 / 设备实例 ID）



    { "driver.s_devstatus_lbl",          L"设备状态" },



    { "driver.s_devstatus_ok",           L"正常" },



    { "driver.s_devstatus_problem",      L"有问题" },



    { "driver.s_devstatus_disabled",     L"未启动" },



    { "driver.s_problemcode_lbl",        L"问题代码" },



    { "driver.s_problem_none",           L"无" },



    { "driver.s_service_lbl",            L"驱动服务名" },



    { "driver.s_devinst_lbl",            L"设备实例 ID" },







    { "assoc.s_hkcr_title",              L"HKCR\u6ce8\u518c\u6269\u5c55\u540d" },



    { "assoc.s_hkcr_confirm",              L"\u5c06\u628a\u300c%s\u300d\u6269\u5c55\u540d\u9ed8\u8ba4\u5173\u8054\u5230\u300c%s\u300d\uff0c\u5e76\u5199\u5165 HKCR\\%s\\OpenWithProgids \u5019\u9009\u3002\u662f\u5426\u7ee7\u7eed\uff1f" },



    { "assoc.s_hkcr_noprogid",              L"\u672a\u9009\u62e9\u8981\u6ce8\u518c\u7684 ProgId" },



    { "assoc.s_hkcr_noprogid_detail",              L"\u8bf7\u5148\u5728\u4e0a\u65b9\u5217\u8868\u9009\u62e9\u4e00\u4e2a\u6269\u5c55\u540d\uff0c\u67e5\u770b\u5176 ProgId \u540e\u518d\u6267\u884c\u6ce8\u518c\u3002" },



    { "assoc.s_hkcr_noext",              L"\u672a\u6307\u5b9a\u6269\u5c55\u540d" },



    { "assoc.s_hkcr_noext_detail",              L"\u8bf7\u5728\u8fc7\u6ee4\u6846\u8f93\u5165\u6269\u5c55\u540d\uff0c\u6216\u5728\u5217\u8868\u4e2d\u9009\u62e9\u4e00\u9879\u540e\u91cd\u8bd5\u3002" },



    { "assoc.s_hkcr_ok_short",              L"HKCR \u6ce8\u518c\u6210\u529f" },



    { "assoc.s_hkcr_ok",              L"\u300c%s\u300d\u5df2\u6ce8\u518c\u5230\u300c%s\u300d\u3002\u7cfb\u7edf\u9ed8\u8ba4\u5c42\u4e0e\u5019\u9009\u5c42\u5df2\u540c\u6b65\u5199\u5165\u3002" },



    { "assoc.s_hkcr_fail_short",              L"HKCR \u6ce8\u518c\u5931\u8d25" },



    { "assoc.s_hkcr_fail",              L"\u5199\u5165\u6ce8\u518c\u8868\u8fd4\u56de\u9519\u8bef\u7801 %d\uff0c\u8bf7\u67e5\u770b\u65e5\u5fd7\u4e86\u89e3\u8be6\u60c5\u3002" },



    { "assoc.s_hkcr_del_title",              L"\u5220\u9664 HKCR \u6269\u5c55\u540d\u6ce8\u518c" },



    { "assoc.s_hkcr_del_confirm",              L"\u5c06\u5220\u9664\u300c%s\u300d\u7684 HKCR \u6ce8\u518c\u9879\u53ca FileExts \u5173\u8054\u8bb0\u5f55\u3002\u662f\u5426\u7ee7\u7eed\uff1f" },



    { "assoc.s_hkcr_del_ok_short",              L"\u5220\u9664\u6210\u529f" },



    { "assoc.s_hkcr_del_ok",              L"\u300c%s\u300d\u6269\u5c55\u540d\u6ce8\u518c\u5df2\u5220\u9664\uff0c\u7cfb\u7edf\u5c06\u56de\u9000\u5230\u65e0\u5173\u8054\u72b6\u6001\u3002" },



    { "assoc.s_hkcr_del_fail_short",              L"\u5220\u9664\u5931\u8d25" },



    { "assoc.s_hkcr_del_fail",              L"\u5220\u9664\u300c%s\u300d\u6ce8\u518c\u8fd4\u56de\u9519\u8bef\u7801 %d\uff0c\u8bf7\u67e5\u770b\u65e5\u5fd7\u4e86\u89e3\u8be6\u60c5\u3002" },



    { "assoc.s_hkcr_restore_title",              L"\u6062\u590d\u7cfb\u7edf\u9ed8\u8ba4\u5173\u8054" },



    { "assoc.s_hkcr_restore_confirm",              L"\u5c06\u5220\u9664\u300c%s\u300d\u7684\u7528\u6237\u9009\u62e9\u4e0e HKCR \u81ea\u5b9a\u4e49\u6ce8\u518c\uff0c\u8ba9\u7cfb\u7edf\u56de\u9000\u5230\u9ed8\u8ba4\u5173\u8054\u3002\u662f\u5426\u7ee7\u7eed\uff1f" },



    { "assoc.s_hkcr_restore_ok_short",              L"\u6062\u590d\u6210\u529f" },



    { "assoc.s_hkcr_restore_ok",              L"\u300c%s\u300d\u5df2\u6062\u590d\u7cfb\u7edf\u9ed8\u8ba4\u5173\u8054\u3002" },



    { "assoc.s_hkcr_restore_fail_short",              L"\u6062\u590d\u5931\u8d25" },



    { "assoc.s_hkcr_restore_fail",              L"\u6062\u590d\u300c%s\u300d\u9ed8\u8ba4\u5173\u8054\u8fd4\u56de\u9519\u8bef\u7801 %d\uff0c\u8bf7\u67e5\u770b\u65e5\u5fd7\u4e86\u89e3\u8be6\u60c5\u3002" },



    // driver (R120-2 详情面板三字段)



    { "driver.s_mfg_lbl",             L"\u5236\u9020\u5546" },



    { "driver.s_installdate_lbl",     L"\u5b89\u88c5\u65e5\u671f" },



    { "driver.s_hardwareid_lbl",      L"\u786c\u4ef6 ID" },



    // driver (R120-2 设备属性对话框)



    { "driver.s_props_btn",           L"\u5c5e\u6027" },



    { "driver.s_props_title",         L"\u8bbe\u5907\u5c5e\u6027" },



    { "driver.s_props_noselect",      L"\u8bf7\u5148\u5728\u5217\u8868\u4e2d\u9009\u62e9\u4e00\u4e2a\u5df2\u89e3\u6790\u8bbe\u5907\u5b9e\u4f8b\u7684\u9a71\u52a8\u3002" },



    { "driver.s_props_start",         L"\u6253\u5f00\u8bbe\u5907\u5c5e\u6027\u5bf9\u8bdd\u6846\uff1a%s" },



    { "appx.s_err_not_elevated",        L"\u9700\u8981\u7ba1\u7406\u5458\u6743\u9650" },



    { "appx.s_err_not_elevated_detail", L"\u8bf7\u4ee5\u7ba1\u7406\u5458\u8eab\u4efd\u8fd0\u884c\u540e\u91cd\u8bd5\u3002" },



    { "appx.s_err_empty",                L"\u672a\u9009\u62e9\u5e94\u7528\u5305" },



    { "appx.s_err_empty_detail",         L"\u8bf7\u5148\u52fe\u9009\u8981\u64cd\u4f5c\u7684\u5e94\u7528\u5305\u3002" },



    { "appx.s_err_arg",                  L"\u53c2\u6570\u65e0\u6548" },



    { "appx.s_err_arg_detail",           L"\u5e94\u7528\u5305\u540d\u79f0\u6216\u8def\u5f84\u683c\u5f0f\u4e0d\u6b63\u786e\u3002" },



    { "appx.s_err_run",                  L"\u542f\u52a8 PowerShell \u5931\u8d25" },



    { "appx.s_err_run_detail",           L"无法启动应用部署服务，请检查系统环境。" },



    { "appx.s_err_access",               L"\u8bbf\u95ee\u88ab\u62d2\u7edd" },



    { "appx.s_err_access_detail",        L"\u5e94\u7528\u5305\u5c5e\u4e8e\u5176\u4ed6\u7528\u6237\u6216\u53d7\u4fdd\u62a4\uff0c\u8bf7\u4ee5\u7ba1\u7406\u5458\u8eab\u4efd\u8fd0\u884c\u3002" },



    { "appx.s_err_exists",               L"\u5e94\u7528\u5305\u5df2\u5b89\u88c5" },



    { "appx.s_err_exists_detail",        L"\u8be5\u5e94\u7528\u5305\u5df2\u5b58\u5728\u4e8e\u5f53\u524d\u7cfb\u7edf\u3002" },



    { "appx.s_err_blocked",              L"\u90e8\u7f72\u88ab\u963b\u6b62" },



    { "appx.s_err_blocked_detail",       L"\u7cfb\u7edf\u7b56\u7565\u6216\u7b7e\u540d\u6821\u9a8c\u963b\u6b62\u4e86\u8be5\u5e94\u7528\u5305\u90e8\u7f72\u3002" },



    { "appx.s_err_deplost",              L"\u4f9d\u8d56\u5305\u7f3a\u5931" },



    { "appx.s_err_deplost_detail",       L"\u8be5\u5e94\u7528\u5305\u4f9d\u8d56\u7684\u5176\u4ed6\u5305\u672a\u5b89\u88c5\uff0c\u8bf7\u9009\u62e9\u4f9d\u8d56\u76ee\u5f55\u540e\u91cd\u8bd5\u3002" },



    { "appx.s_err_updnotallowed",        L"\u4e0d\u5141\u8bb8\u66f4\u65b0\u5df2\u5b58\u5728\u7684\u5305" },



    { "appx.s_err_updnotallowed_detail", L"\u540c\u540d\u5e94\u7528\u5305\u5df2\u5b58\u5728\u4e14\u7248\u672c\u4e0d\u517c\u5bb9\uff0c\u8bf7\u5148\u5378\u8f7d\u65e7\u7248\u672c\u3002" },



    { "appx.s_err_license",               L"\u8bb8\u53ef\u7f3a\u5931" },



    { "appx.s_err_license_detail",        L"\u65e0\u6cd5\u83b7\u53d6\u5e94\u7528\u5305\u8bb8\u53ef\uff0c\u8bf7\u4ece\u5e94\u7528\u5546\u5e97\u4e0b\u8f7d\u3002" },



    { "appx.s_err_mem",                   L"\u5185\u5b58\u4e0d\u8db3" },



    { "appx.s_err_mem_detail",            L"\u7cfb\u7edf\u5185\u5b58\u4e0d\u8db3\uff0c\u8bf7\u5173\u95ed\u90e8\u5206\u7a0b\u5e8f\u540e\u91cd\u8bd5\u3002" },



    { "unattend.s_ctx_copycmd",              L"\u590d\u5236\u547d\u4ee4" },



    { "unattend.s_ctx_copycmd_done",         L"\u5df2\u590d\u5236\u547d\u4ee4" },



    { "unattend.s_ctx_copycmd_done_body",    L"\u547d\u4ee4\u884c\u5df2\u590d\u5236\u5230\u526a\u8d34\u677f\u3002" },



    { "unattend.s_ctx_copypass",             L"\u590d\u5236 Pass \u540d\u79f0" },



    { "unattend.s_ctx_copypass_done",        L"\u5df2\u590d\u5236 Pass \u540d\u79f0" },



    { "unattend.s_ctx_copypass_done_body",   L"\u914d\u7f6e\u8282 Pass \u540d\u79f0\u5df2\u590d\u5236\u5230\u526a\u8d34\u677f\u3002" },



    { "unattend.s_ctx_openexport",           L"\u6253\u5f00\u5bfc\u51fa\u4f4d\u7f6e" },



    { "unattend.s_ctx_openexport_none",      L"\u672a\u627e\u5230\u5bfc\u51fa\u6587\u4ef6" },



    { "unattend.s_ctx_openexport_none_body", L"\u8bf7\u5148\u5bfc\u51fa\u9884\u5e94\u7b54\u914d\u7f6e\u3002" },



    // appx (R123-1 行级右键菜单)



    { "appx.s_ctx_openloc",              L"\u6253\u5f00\u5b89\u88c5\u4f4d\u7f6e" },



    { "appx.s_ctx_openloc_fail",         L"\u65e0\u6cd5\u6253\u5f00\u4f4d\u7f6e" },



    { "appx.s_ctx_openloc_fail_body",    L"\u8be5\u5e94\u7528\u5305\u672a\u63d0\u4f9b\u5b89\u88c5\u8def\u5f84\u4fe1\u606f\u3002" },



    { "appx.s_ctx_copyfull",             L"\u590d\u5236\u5305\u540d" },



    { "appx.s_ctx_copyfull_done",        L"\u5df2\u590d\u5236\u5305\u540d" },



    { "appx.s_ctx_copyfull_done_body",   L"PackageFullName \u5df2\u590d\u5236\u5230\u526a\u8d34\u677f\u3002" },



    { "appx.s_ctx_copyfamily",           L"\u590d\u5236 FamilyName" },



    { "appx.s_ctx_copyfamily_done",      L"\u5df2\u590d\u5236 FamilyName" },



    { "appx.s_ctx_copyfamily_done_body", L"PackageFamilyName \u5df2\u590d\u5236\u5230\u526a\u8d34\u677f\u3002" },



    { "appx.s_ctx_detail",               L"\u67e5\u770b\u8be6\u60c5" },

    { "appx.s_ctx_depgraph",             L"\u67e5\u770b\u4f9d\u8d56\u56fe" },



    // cap (R124-1 行级右键菜单)



    { "cap.s_ctx_copyid", L"\u590d\u5236 Identity" },



    { "cap.s_ctx_copyid_done", L"\u5df2\u590d\u5236 Identity" },



    { "cap.s_ctx_copyid_done_body", L"Capability Identity \u5df2\u590d\u5236\u5230\u526a\u8d34\u677f\u3002" },



    { "cap.s_ctx_copyname", L"\u590d\u5236\u663e\u793a\u540d" },



    { "cap.s_ctx_copyname_done", L"\u5df2\u590d\u5236\u663e\u793a\u540d" },



    { "cap.s_ctx_copyname_done_body", L"Capability \u663e\u793a\u540d\u5df2\u590d\u5236\u5230\u526a\u8d34\u677f\u3002" },



    { "cap.s_ctx_copydesc", L"\u590d\u5236\u63cf\u8ff0" },



    { "cap.s_ctx_copydesc_done", L"\u5df2\u590d\u5236\u63cf\u8ff0" },



    { "cap.s_ctx_copydesc_done_body", L"Capability \u63cf\u8ff0\u5df2\u590d\u5236\u5230\u526a\u8d34\u677f\u3002" },



    { "cap.s_ctx_detail", L"\u67e5\u770b\u8be6\u7ec6\u63cf\u8ff0" },



    // feat (R124-2 行级右键菜单)



    { "feat.s_ctx_copysys", L"\u590d\u5236 DISM \u529f\u80fd\u540d" },



    { "feat.s_ctx_copysys_done", L"\u5df2\u590d\u5236\u529f\u80fd\u540d" },



    { "feat.s_ctx_copysys_done_body", L"DISM \u529f\u80fd\u540d\uff08sSys\uff09\u5df2\u590d\u5236\u5230\u526a\u8d34\u677f\u3002" },



    { "feat.s_ctx_copydisp", L"\u590d\u5236\u663e\u793a\u540d" },



    { "feat.s_ctx_copydisp_done", L"\u5df2\u590d\u5236\u663e\u793a\u540d" },



    { "feat.s_ctx_copydisp_done_body", L"Windows \u529f\u80fd\u663e\u793a\u540d\u5df2\u590d\u5236\u5230\u526a\u8d34\u677f\u3002" },



    { "feat.s_ctx_copying", L"\u590d\u5236\u72b6\u6001" },



    { "feat.s_ctx_copying_done", L"\u5df2\u590d\u5236\u72b6\u6001" },



    { "feat.s_ctx_copying_done_body", L"\u529f\u80fd\u72b6\u6001\u63cf\u8ff0\u5df2\u590d\u5236\u5230\u526a\u8d34\u677f\u3002" },



    { "feat.s_ctx_detail", L"\u67e5\u770b\u8be6\u7ec6\u72b6\u6001" },



    { "feat.s_state_on", L"\u5df2\u542f\u7528" },



    { "feat.s_state_off", L"\u672a\u542f\u7528" },



    { "feat.s_state_remove", L"\u6c38\u4e45\u79fb\u9664" },



    { "feat.s_state_mixed", L"\u90e8\u5206\u542f\u7528" },



    { "feat.s_yes", L"\u662f" },



    { "feat.s_no", L"\u5426" },
    // [R67] Page_Feature 纯 UI 文案（R67 国际化口径：Windows 固有功能名保留内嵌，
    //        仅迁移真正的 UI 提示语）。这 17 条此前硬编码中文，切到 en-US 仍显示中文。
    { "feat.s_loading_list",      L"正在读取 Windows 功能列表…" },
    { "feat.s_loading_progress",  L"正在读取 Windows 功能列表… %d/%d" },
    { "feat.s_load_timeout",      L"读取 Windows 功能列表超时，请点击刷新重试。" },
    { "feat.s_refresh_stale",     L"刷新失败，当前显示的是上次成功读取的 %d 项结果；点击刷新可重试。" },
    { "feat.s_load_failed",       L"读取 Windows 功能列表失败，请点击刷新重试。" },
    { "feat.s_load_failed_err",   L"读取 Windows 功能列表失败：%s；请点击刷新重试。" },
    { "feat.s_module_missing",    L"找不到指定的模块" },
    { "feat.s_empty_list",        L"未发现可用的 Windows 功能，可点击刷新重试。" },
    { "feat.s_rendering",         L"列表正在渲染，请稍候再操作。" },
    { "feat.s_pending_on",        L"待应用：启用" },
    { "feat.s_pending_remove",    L"待应用：永久删除" },
    { "feat.s_pending_off",       L"待应用：关闭" },
    { "feat.s_internal_name",     L"内部名称：%s" },
    { "feat.s_apply_done",        L"功能变更已完成：成功 %d 项%s" },
    { "feat.s_apply_done_fail",   L"功能变更结束：成功 %d 项，失败 %d 项，详见日志区" },
    { "feat.s_status_prefix",     L"状态: " },
    // [R67] Page_Capability 纯 UI 文案（同上口径：功能名保留内嵌，提示语走语言包）
    { "capability.s_enum_progress", L"正在枚举可选功能… %d/%d" },
    { "capability.s_timeout",       L"读取可选功能列表超时（已等 %u 秒），请点击刷新重试。" },
    { "capability.s_err_needadmin", L"枚举可选功能失败：需要管理员权限。请以管理员身份重启程序后点击刷新。" },
    { "capability.s_err_code",      L"无法枚举可选功能（错误码 0x%08X），请点击刷新重试。" },



    // driver (R-B24 行级右键菜单)



    { "driver.s_ctx_copy_name", L"复制设备名" },



    { "driver.s_ctx_copy_name_done", L"已复制设备名" },



    { "driver.s_ctx_copy_name_done_body", L"驱动设备名已复制到剪贴板。" },



    { "driver.s_ctx_copy_inf", L"复制 INF 路径" },



    { "driver.s_ctx_copy_inf_done", L"已复制 INF 路径" },



    { "driver.s_ctx_copy_inf_done_body", L"驱动 INF 路径已复制到剪贴板。" },



    { "driver.s_ctx_copy_devinst", L"复制设备实例 ID" },



    { "driver.s_ctx_copy_devinst_done", L"已复制设备实例 ID" },



    { "driver.s_ctx_copy_devinst_done_body", L"设备实例 ID已复制到剪贴板。" },



    { "driver.s_ctx_detail", L"查看驱动详情" },



    { "driver.s_ctx_rollback", L"回退驱动" },



    { "driver.s_ctx_props", L"设备属性" },





    // driver (R-B24 \u884c\u7ea7\u53f3\u952e\u83dc\u5355)



    { "driver.s_ctx_copy_name", L"\u590d\u5236\u8bbe\u5907\u540d" },



    { "driver.s_ctx_copy_name_done", L"\u5df2\u590d\u5236\u8bbe\u5907\u540d" },



    { "driver.s_ctx_copy_name_done_body", L"\u9a71\u52a8\u8bbe\u5907\u540d\u5df2\u590d\u5236\u5230\u526a\u8d34\u677f\u3002" },



    { "driver.s_ctx_copy_inf", L"\u590d\u5236 INF \u8def\u5f84" },



    { "driver.s_ctx_copy_inf_done", L"\u5df2\u590d\u5236 INF \u8def\u5f84" },



    { "driver.s_ctx_copy_inf_done_body", L"\u9a71\u52a8 INF \u8def\u5f84\u5df2\u590d\u5236\u5230\u526a\u8d34\u677f\u3002" },



    { "driver.s_ctx_copy_devinst", L"\u590d\u5236\u8bbe\u5907\u5b9e\u4f8b ID" },



    { "driver.s_ctx_copy_devinst_done", L"\u5df2\u590d\u5236\u8bbe\u5907\u5b9e\u4f8b ID" },



    { "driver.s_ctx_copy_devinst_done_body", L"\u8bbe\u5907\u5b9e\u4f8b ID\u5df2\u590d\u5236\u5230\u526a\u8d34\u677f\u3002" },



    { "driver.s_ctx_detail", L"\u67e5\u770b\u9a71\u52a8\u8be6\u60c5" },



    { "driver.s_ctx_rollback", L"\u56de\u9000\u9a71\u52a8" },



    { "driver.s_ctx_props", L"\u8bbe\u5907\u5c5e\u6027" },



    // intl (R120-1 区域格式)



    { "intl.s_badfmt_short",              L"\u65e0\u6548\u7684\u533a\u57df\u683c\u5f0f" },



    { "intl.s_badfmt_detail",             L"\u8bf7\u5148\u9009\u62e9\u4e00\u4e2a\u533a\u57df\u683c\u5f0f\u503c\u3002" },



    { "intl.s_apply_fmt_confirm_title",   L"\u5e94\u7528\u533a\u57df\u683c\u5f0f" },



    { "intl.s_apply_fmt_confirm",         L"\u786e\u5b9a\u628a\u533a\u57df\u683c\u5f0f\u503c\u8bbe\u4e3a %s \u5417\uff1f\n\u5c06\u5199\u5165 HKCU\\Control Panel\\International\u3002" },



    { "intl.s_apply_fmt_done_short",      L"\u533a\u57df\u683c\u5f0f\u5df2\u5e94\u7528" },



    { "intl.s_apply_fmt_done",            L"\u533a\u57df\u683c\u5f0f\u503c\u5df2\u5199\u5165\u6ce8\u518c\u8868\uff0c\u91cd\u542f\u540e\u751f\u6548\u3002" },



    { "intl.s_apply_fmt_fail_short",      L"\u533a\u57df\u683c\u5f0f\u5e94\u7528\u5931\u8d25" },



    { "intl.s_apply_fmt_fail",            L"\u65e0\u6cd5\u5199\u5165\u533a\u57df\u683c\u5f0f\u503c\uff0c\u8bf7\u68c0\u67e5\u5f53\u524d\u7528\u6237\u6743\u9650\u3002" },

    { "misc.detail_expert",     L"\u4e13\u5bb6\u6a21\u5f0f(&X)" },

    { "misc.detail_wofadk",     L"\u52a0\u8f7d WofAdk(&L)" },

    { "misc.detail_ctxmenu",    L"\u53f3\u952e\u83dc\u5355(&M)" },

    { "misc.detail_integre",    L"\u6574\u5408\u6062\u590d\u73af\u5883(&I)" },

    { "misc.detail_delta",      L"\u5dee\u5f02\u4f20\u8f93(&D)" },

    { "misc.hiso_mountfail",  L"ISO \u6302\u8F7D\u5931\u8D25\uFF0C\u65E0\u6CD5\u8BC6\u522B\u5185\u90E8\u6620\u50CF\uFF1A\n%1\n\uFF08\u8BF7\u786E\u8BA4 .iso \u6587\u4EF6\u4E0E PowerShell Mount-DiskImage \u53EF\u7528\uFF09" },
    { "misc.hiso_detect2",    L"\u5DF2\u8BC6\u522B\u5230 ISO \u5185\u90E8\u5B89\u88C5\u6620\u50CF\uFF1A\n%1\n\u5305\u542B %2 \u4E2A\u6620\u50CF\uFF0C\u67B6\u6784\u96C6\u5408= %3\uFF08%4 \u4E2A\u6620\u50CF\u89E3\u6790\u5230\u67B6\u6784\uFF09\n\u3002" },
    { "misc.hiso_detect",



      L"\u5df2\u8bc6\u522b\u5230 ISO \u5185\u90e8\u5b89\u88c5\u6620\u50cf\uff1a\n%1\n\u5305\u542b %2 \u4e2a\u6620\u50cf\uff0c\u5c5e\u6027\u6807\u5fd7=0x%3\n\u3002"



    },



    { "misc.detail_hint",       L"\u4ee5\u4e0b\u9009\u9879\u5f71\u54cd\u5168\u5c40\u884c\u4e3a\u4e0e\u754c\u9762\uff0c\u4ec5\u4f9b\u9ad8\u7ea7\u7528\u6237\u8c03\u6574\u3002\u4fdd\u5b58\u540e\u4e0b\u6b21\u542f\u52a8\u751f\u6548\u3002" },







    { "misc.logview_title", L"\u67e5\u770b\u65e5\u5fd7" },

    { "misc.logview_refresh", L"\u5237\u65b0\u0028\u0026\u0052\u0029" },

    { "misc.logview_open_folder", L"\u6253\u5f00\u65e5\u5fd7\u76ee\u5f55\u0028\u0026\u004f\u0029" },

    { "misc.logview_clear", L"\u6e05\u7a7a\u0028\u0026\u0043\u0029" },
    // [L-10-6] 清空需二次确认：文案明确"会先归档、仅清空审计流"。
    { "misc.logview_clear_confirm", L"\u786e\u5b9a\u6e05\u7a7a\u5ba1\u8ba1\u65e5\u5fd7\u5417\uff1f\u000a\u000a\u6e05\u7a7a\u524d\u4f1a\u81ea\u52a8\u5f52\u6863\u4e3a action_log.jsonl.cleared\uff0c\u4ec5\u6e05\u7a7a\u5ba1\u8ba1\u4e8b\u4ef6\u6d41\uff0c\u8bca\u65ad\u5e95\u8d26\u4e0d\u53d7\u5f71\u54cd\u3002" },


    { "misc.logview_empty", L"\u6682\u65e0\u65e5\u5fd7\u8bb0\u5f55\u3002\u8fd0\u884c\u4efb\u610f\u64cd\u4f5c\u540e\uff0c\u65e5\u5fd7\u4f1a\u8ffd\u52a0\u5199\u5165\u0020\u0065\u0078\u0065\u0020\u540c\u76ee\u5f55\u7684\u0020\u0061\u0063\u0074\u0069\u006f\u006e\u005f\u006c\u006f\u0067\u002e\u006a\u0073\u006f\u006e\u006c\u3002" },

    { "misc.logview_count", L"\u5171\u0020\u0025\u0064\u0020\u6761\u65e5\u5fd7\u8bb0\u5f55" },

    // [R-LOGV-2] 正文只渲染最近若干条时的计数文案（含两个 %d：总数、显示数）。
    // [L-10] 查看器新增交互（搜索/过滤/跟随/导出）
    { "misc.logview_search_tip", L"\u641c\u7d22\u5173\u952e\u5b57\u2026" },
    { "misc.logview_filter_all", L"\u5168\u90e8\u7ea7\u522b" },
    { "misc.logview_filter_warn", L"\u8b66\u544a\u53ca\u4ee5\u4e0a" },
    { "misc.logview_filter_error", L"\u4ec5\u9519\u8bef" },
    { "misc.logview_follow", L"\u81ea\u52a8\u8ddf\u968f" },
    { "misc.logview_follow_on", L"\u8ddf\u968f\u4e2d\u2026" },
    { "misc.logview_export", L"\u5bfc\u51fa" },
    { "misc.logview_count_filtered", L"\u5339\u914d %d / \u5171 %d \u6761\uff08\u663e\u793a\u6700\u8fd1 %d \u6761\uff09" },
    { "misc.logview_export_fail", L"\u5bfc\u51fa\u5931\u8d25\uff1a\u65e0\u6cd5\u5199\u5165\u65e5\u5fd7\u76ee\u5f55\u3002" },
    { "misc.logview_export_done", L"\u5df2\u5bfc\u51fa %d \u6761\u5230\uff1a\n%s" },

    { "misc.logview_count_tail", L"\u5171\u0020\u0025\u0064\u0020\u6761\u65e5\u5fd7\uff0c\u6b63\u6587\u53ea\u663e\u793a\u6700\u8fd1\u0020\u0025\u0064\u0020\u6761" },

    { "misc.logview_read_fail", L"\u8bfb\u53d6\u65e5\u5fd7\u6587\u4ef6\u5931\u8d25\uff1a\u0025\u0073" },

    { "misc.logview_cleared", L"\u5ba1\u8ba1\u65e5\u5fd7\u5df2\u6e05\u7a7a\uff08\u539f\u6587\u4ef6\u5df2\u5f52\u6863\u4e3a .cleared\uff09" },

    // 日志正文改为可选中文本后新增的两条提示；无对应语言包条目时回退到以下中文。
    { "misc.logview_hint", L"\u53ef\u62d6\u9009\u6587\u672c\uff0cCtrl+C \u590d\u5236\uff0c\u53f3\u952e\u83dc\u5355\u53ef\u590d\u5236\u9009\u4e2d\u6bb5\u843d" },

    { "misc.logview_copy", L"\u590d\u5236\u9009\u4e2d\u0028\u0026\u0043\u0029" },

    { "misc.logview_copy_empty", L"\u8bf7\u5148\u62d6\u9009\u8981\u590d\u5236\u7684\u65e5\u5fd7\u6bb5\u843d" },

    { "misc.logview_loading", L"\u6b63\u5728\u52a0\u8f7d\u65e5\u5fd7\u2026" },

// [I18N-APPX-NAME] Appx 包名 → 显示名的内置回退（中文）。
//   语言包 lang/*.json 提供各语言的 appx.name.<PackageName> 时优先用译文；
//   此表仅作为无语言包/缺键时的兜底，代码侧只拼 key，不写任何界面文案。
    { "appx.name.Microsoft.BingWeather", L"\u5929\u6c14" },
    { "appx.name.Microsoft.BingNews", L"\u8d44\u8baf" },
    { "appx.name.Microsoft.BingSports", L"\u4f53\u80b2" },
    { "appx.name.Microsoft.BingFinance", L"\u8d22\u7ecf" },
    { "appx.name.Microsoft.BingTranslator", L"\u7ffb\u8bd1" },
    { "appx.name.Microsoft.BingSearch", L"\u5fc5\u5e94\u641c\u7d22" },
    { "appx.name.Microsoft.BingWeatherBeta", L"\u5929\u6c14 Beta" },
    { "appx.name.Microsoft.GetHelp", L"\u83b7\u53d6\u5e2e\u52a9" },
    { "appx.name.Microsoft.Getstarted", L"\u4f7f\u7528\u6280\u5de7" },
    { "appx.name.Microsoft.MicrosoftOfficeHub", L"Microsoft Office" },
    { "appx.name.Microsoft.MicrosoftSolitaireCollection", L"Microsoft Solitaire Collection" },
    { "appx.name.Microsoft.MicrosoftStickyNotes", L"\u4fbf\u7b3a" },
    { "appx.name.Microsoft.MSPaint", L"\u753b\u56fe" },
    { "appx.name.Microsoft.Paint", L"\u753b\u56fe" },
    { "appx.name.Microsoft.Office.OneNote", L"OneNote" },
    { "appx.name.Microsoft.Office.Sway", L"Sway" },
    { "appx.name.Microsoft.People", L"\u4eba\u8109" },
    { "appx.name.Microsoft.ScreenSketch", L"\u622a\u56fe\u5de5\u5177" },
    { "appx.name.Microsoft.SkypeApp", L"Skype" },
    { "appx.name.Microsoft.StorePurchaseApp", L"\u5546\u5e97\u8d2d\u4e70" },
    { "appx.name.Microsoft.VP9VideoExtensions", L"VP9 \u89c6\u9891\u6269\u5c55" },
    { "appx.name.Microsoft.WebMediaExtensions", L"Web \u5a92\u4f53\u6269\u5c55" },
    { "appx.name.Microsoft.WebpImageExtension", L"Webp \u56fe\u50cf\u6269\u5c55" },
    { "appx.name.Microsoft.HEIFImageExtension", L"HEIF \u56fe\u50cf\u6269\u5c55" },
    { "appx.name.Microsoft.HEVCVideoExtension", L"HEVC \u89c6\u9891\u6269\u5c55" },
    { "appx.name.Microsoft.RawImageExtension", L"RAW \u56fe\u50cf\u6269\u5c55" },
    { "appx.name.Microsoft.AV1VideoExtension", L"AV1 \u89c6\u9891\u6269\u5c55" },
    { "appx.name.Microsoft.Wallet", L"\u94b1\u5305" },
    { "appx.name.Microsoft.Windows.Photos", L"\u7167\u7247" },
    { "appx.name.Microsoft.WindowsAlarms", L"\u95f9\u949f\u548c\u65f6\u949f" },
    { "appx.name.Microsoft.WindowsCalculator", L"\u8ba1\u7b97\u5668" },
    { "appx.name.Microsoft.WindowsCamera", L"\u76f8\u673a" },
    { "appx.name.Microsoft.WindowsCommunicationsApps", L"\u90ae\u4ef6\u548c\u65e5\u5386" },
    { "appx.name.Microsoft.windowscommunicationsapps", L"\u90ae\u4ef6\u548c\u65e5\u5386" },
    { "appx.name.Microsoft.WindowsFeedbackHub", L"\u53cd\u9988\u4e2d\u5fc3" },
    { "appx.name.Microsoft.WindowsMaps", L"\u5730\u56fe" },
    { "appx.name.Microsoft.WindowsSoundRecorder", L"\u5f55\u97f3\u673a" },
    { "appx.name.Microsoft.WindowsStore", L"Microsoft Store" },
    { "appx.name.Microsoft.Xbox.TCUI", L"Xbox TCUI" },
    { "appx.name.Microsoft.XboxApp", L"Xbox" },
    { "appx.name.Microsoft.XboxGameOverlay", L"Xbox \u6e38\u620f\u6d6e\u5c42" },
    { "appx.name.Microsoft.XboxGamingOverlay", L"Xbox \u6e38\u620f\u680f" },
    { "appx.name.Microsoft.XboxIdentityProvider", L"Xbox \u8eab\u4efd\u63d0\u4f9b\u7a0b\u5e8f" },
    { "appx.name.Microsoft.XboxSpeechToTextOverlay", L"Xbox \u8bed\u97f3\u8f6c\u6587\u672c" },
    { "appx.name.Microsoft.ZuneMusic", L"\u5a92\u4f53\u64ad\u653e\u5668" },
    { "appx.name.Microsoft.ZuneVideo", L"\u7535\u5f71\u548c\u7535\u89c6" },
    { "appx.name.Microsoft.Microsoft3DViewer", L"3D \u67e5\u770b\u5668" },
    { "appx.name.Microsoft.MixedReality.Portal", L"\u6df7\u5408\u73b0\u5b9e\u95e8\u6237" },
    { "appx.name.Microsoft.DesktopAppInstaller", L"\u5e94\u7528\u5b89\u88c5\u7a0b\u5e8f" },
    { "appx.name.Microsoft.WindowsTerminal", L"Windows \u7ec8\u7aef" },
    { "appx.name.Microsoft.WindowsNotepad", L"\u8bb0\u4e8b\u672c" },
    { "appx.name.Microsoft.SecHealthUI", L"Windows \u5b89\u5168\u4e2d\u5fc3" },
    { "appx.name.Microsoft.Windows.DevHome", L"\u5f00\u53d1\u4e3b\u63a7\u53f0" },
    { "appx.name.Microsoft.MicrosoftPCManager", L"\u7535\u8111\u7ba1\u5bb6" },
    { "appx.name.Microsoft.GamingApp", L"Xbox \u5e94\u7528" },
    { "appx.name.MicrosoftTeams", L"Microsoft Teams" },
    { "appx.name.Microsoft.YourPhone", L"\u624b\u673a\u8fde\u63a5" },
    { "appx.name.Microsoft.Windows.CrossDevice", L"\u8de8\u8bbe\u5907\u4f53\u9a8c" },
    { "appx.name.Microsoft.StartExperiencesApp", L"\u83b7\u53d6\u5e2e\u52a9\u63d0\u793a" },
    { "appx.name.Microsoft.Winget.Source", L"WinGet \u6e90" },
    { "appx.name.MicrosoftWindows.Client.WebExperience", L"Windows \u5c0f\u7ec4\u4ef6" },
    { "appx.name.Microsoft.Windows.Search", L"Windows \u641c\u7d22" },
    { "appx.name.Microsoft.LanguageExperiencePackzh-CN", L"\u4e2d\u6587(\u7b80\u4f53)\u8bed\u8a00\u4f53\u9a8c\u5305" },

// [I18N-APPX-NAME-2] 补齐系统级 Appx 包的中文名（.NET Native / UI.Xaml /
//   VCLibs / WindowsAppRuntime / 各类系统宿主）。这些是「用户应用」列表里
//   实际出现、但此前完全没有映射的包；缺映射时列表只能显示 PackageFullName。
//   名称依据微软官方显示名；纯运行时库按其技术名 + 中文说明。
    { "appx.name.Microsoft.AAD.BrokerPlugin", L"Microsoft \u8d26\u6237\u4ee3\u7406" },
    { "appx.name.Microsoft.AccountsControl", L"\u8d26\u6237\u63a7\u5236" },
    { "appx.name.Microsoft.AsyncTextService", L"\u5f02\u6b65\u6587\u672c\u670d\u52a1" },
    { "appx.name.Microsoft.BioEnrollment", L"\u751f\u7269\u8bc6\u522b\u767b\u5f55" },
    { "appx.name.Microsoft.CredDialogHost", L"\u51ed\u636e\u5bf9\u8bdd\u6846\u5bbf\u4e3b" },
    { "appx.name.Microsoft.ECApp", L"\u4f01\u4e1a\u4e0a\u4e0b\u6587\u5e94\u7528" },
    { "appx.name.Microsoft.LockApp", L"\u9501\u5c4f\u754c\u9762" },
    { "appx.name.Microsoft.MicrosoftEdge.Stable", L"Microsoft Edge" },
    { "appx.name.Microsoft.MicrosoftEdgeDevToolsClient", L"Edge \u5f00\u53d1\u8005\u5de5\u5177" },
    { "appx.name.Microsoft.NET.Native.Framework.1.7", L".NET Native \u6846\u67b6 1.7" },
    { "appx.name.Microsoft.NET.Native.Framework.2.2", L".NET Native \u6846\u67b6 2.2" },
    { "appx.name.Microsoft.NET.Native.Runtime.1.7", L".NET Native \u8fd0\u884c\u65f6 1.7" },
    { "appx.name.Microsoft.NET.Native.Runtime.2.2", L".NET Native \u8fd0\u884c\u65f6 2.2" },
    { "appx.name.Microsoft.OutlookForWindows", L"\u65b0\u7684 Outlook" },
    { "appx.name.Microsoft.Services.Store.Engagement", L"\u5546\u5e97\u4e92\u52a8\u670d\u52a1" },
    { "appx.name.Microsoft.StarlightCourier", L"\u661f\u5149\u5feb\u9012" },
    { "appx.name.Microsoft.UI.Xaml.2.3", L"UI.Xaml 2.3 \u8fd0\u884c\u65f6" },
    { "appx.name.Microsoft.UI.Xaml.2.4", L"UI.Xaml 2.4 \u8fd0\u884c\u65f6" },
    { "appx.name.Microsoft.UI.Xaml.2.7", L"UI.Xaml 2.7 \u8fd0\u884c\u65f6" },
    { "appx.name.Microsoft.UI.Xaml.2.8", L"UI.Xaml 2.8 \u8fd0\u884c\u65f6" },
    { "appx.name.Microsoft.VCLibs.140.00", L"Visual C++ \u8fd0\u884c\u5e93 14.0" },
    { "appx.name.Microsoft.VCLibs.140.00.UWPDesktop", L"Visual C++ \u684c\u9762\u6269\u5c55 14.0" },
    { "appx.name.Microsoft.Win32WebViewHost", L"WebView \u5bbf\u4e3b" },
    { "appx.name.Microsoft.Windows.Apprep.ChxApp", L"\u5e94\u7528\u4fe1\u8a89\u68c0\u67e5" },
    { "appx.name.Microsoft.Windows.AssignedAccessLockApp", L"\u5206\u914d\u8bbf\u95ee\u9501\u5b9a" },
    { "appx.name.Microsoft.Windows.CallingShellApp", L"\u901a\u8bdd\u754c\u9762" },
    { "appx.name.Microsoft.Windows.CapturePicker", L"\u5c4f\u5e55\u6355\u83b7\u9009\u62e9\u5668" },
    { "appx.name.Microsoft.Windows.CloudExperienceHost", L"Windows \u4e91\u4f53\u9a8c\u5bbf\u4e3b" },
    { "appx.name.Microsoft.Windows.ContentDeliveryManager", L"\u5185\u5bb9\u4f20\u9012\u7ba1\u7406\u5668" },
    { "appx.name.Microsoft.Windows.NarratorQuickStart", L"\u8bb2\u8ff0\u4eba\u5feb\u901f\u5165\u95e8" },
    { "appx.name.Microsoft.Windows.OOBENetworkCaptivePortal", L"\u7f51\u7edc\u767b\u5f55\u68c0\u6d4b" },
    { "appx.name.Microsoft.Windows.OOBENetworkConnectionFlow", L"\u7f51\u7edc\u8fde\u63a5\u5411\u5bfc" },
    { "appx.name.Microsoft.Windows.ParentalControls", L"\u5bb6\u957f\u63a7\u5236" },
    { "appx.name.Microsoft.Windows.PeopleExperienceHost", L"\u4eba\u8109\u4f53\u9a8c\u5bbf\u4e3b" },
    { "appx.name.Microsoft.Windows.PinningConfirmationDialog", L"\u56fa\u5b9a\u786e\u8ba4\u5bf9\u8bdd\u6846" },
    { "appx.name.Microsoft.Windows.SecHealthUI", L"Windows \u5b89\u5168\u4e2d\u5fc3" },
    { "appx.name.Microsoft.Windows.SecureAssessmentBrowser", L"\u5b89\u5168\u8bc4\u4f30\u6d4f\u89c8\u5668" },
    { "appx.name.Microsoft.Windows.ShellExperienceHost", L"\u5f00\u59cb\u83dc\u5355\u4e0e\u5916\u58f3" },
    { "appx.name.Microsoft.Windows.StartMenuExperienceHost", L"\u5f00\u59cb\u83dc\u5355" },
    { "appx.name.Microsoft.Windows.XGpuEjectDialog", L"\u663e\u5361\u5f39\u51fa\u5bf9\u8bdd\u6846" },
    { "appx.name.Microsoft.WindowsAppRuntime.1.4", L"Windows App \u8fd0\u884c\u65f6 1.4" },
    { "appx.name.Microsoft.WindowsAppRuntime.1.7", L"Windows App \u8fd0\u884c\u65f6 1.7" },
    { "appx.name.Microsoft.WindowsAppRuntime.1.8", L"Windows App \u8fd0\u884c\u65f6 1.8" },
    { "appx.name.Microsoft.WindowsAppRuntime.2", L"Windows App \u8fd0\u884c\u65f6 2" },
    { "appx.name.Microsoft.WorldNationalParks", L"\u4e16\u754c\u56fd\u5bb6\u516c\u56ed" },
    { "appx.name.Microsoft.XboxGameCallableUI", L"Xbox \u6e38\u620f\u754c\u9762" },
    { "appx.name.MicrosoftCorporationII.WinAppRuntime.Main.1.4", L"WinAppRuntime \u4e3b\u4f53 1.4" },
    { "appx.name.MicrosoftCorporationII.WinAppRuntime.Singleton", L"WinAppRuntime \u5355\u4f8b" },
    { "appx.name.MicrosoftCorporationII.WindowsSubsystemForLinux", L"\u9002\u7528\u4e8e Linux \u7684 Windows \u5b50\u7cfb\u7edf" },
    { "appx.name.MicrosoftWindows.Client.CBS", L"Windows \u6838\u5fc3\u7ec4\u4ef6" },
    { "appx.name.MicrosoftWindows.CrossDevice", L"\u8de8\u8bbe\u5907\u4f53\u9a8c" },
    { "appx.name.MicrosoftWindows.UndockedDevKit", L"\u79fb\u52a8\u8bbe\u5907\u5957\u4ef6" },
    { "appx.name.NcsiUwpApp", L"\u7f51\u7edc\u8fde\u63a5\u72b6\u6001\u6307\u793a" },
    { "appx.name.NVIDIACorp.NVIDIAControlPanel", L"NVIDIA \u63a7\u5236\u9762\u677f" },
    { "appx.name.PythonSoftwareFoundation.Python.3.13", L"Python 3.13" },
    { "appx.name.Windows.CBSPreview", L"\u7ec4\u4ef6\u670d\u52a1\u9884\u89c8" },
    { "appx.name.windows.immersivecontrolpanel", L"Windows \u8bbe\u7f6e" },
    { "appx.name.Windows.PrintDialog", L"\u6253\u5370\u5bf9\u8bdd\u6846" },
    { "appx.name.1527c705-839a-4832-9118-54d4Bd6a0c89", L"\u7cfb\u7edf\u7ec4\u4ef6 (GUID)" },
    { "appx.name.c5e2524a-ea46-4f67-841f-6a9465d9d515", L"\u7cfb\u7edf\u7ec4\u4ef6 (GUID)" },
    { "appx.name.E2A4F912-2574-4A75-9BB0-0D023378592B", L"\u7cfb\u7edf\u7ec4\u4ef6 (GUID)" },
    { "appx.name.F46D4000-FD22-4DB4-AC8E-4E1DDDE828FE", L"\u7cfb\u7edf\u7ec4\u4ef6 (GUID)" },
    { "appx.name.Microsoft.WinAppRuntime.DDLM.4000.1049.117.0-x6", L"WinAppRuntime DDLM" },

    // [R200] 应用包智能筛选：分类名与分级日志（Win11 独有条目在 Win10 上不加载）
    // [R204] 窗口标题 / 标题栏品牌文字（原 XML 写死 Win8 语义）
    // [R219] Brand is defined ONCE here. Every user-visible place reads these keys:
    //   the title bar, the skin fallback text, the About line and the version
    //   resource of each satellite DLL. Renaming the product now means editing
    //   these four values, not hunting 226 scattered occurrences.
    { "app.title", L"系统管理与优化" },
    // [R220] In-row uninstall button label (page_feature rows).
    { "feature.s_uninstall_btn", L"卸载" },
    { "app.product", L"系统管理与优化" },
    { "app.version", L"2026.11.1" },
    { "app.edition", L"UEFI Firmware" },
    { "app.vendor", L"系统管理与优化项目组" },
    { "appx.smart.cat_all", L"全部" },
    { "appx.smart.cat_user", L"用户应用" },
    { "appx.smart.cat_system", L"系统组件" },
    { "appx.smart.cat_runtime", L"运行时框架" },
    { "appx.smart.cat_lang", L"语言资源包" },
    { "appx.smart.s_filtered", L"显示 %d / %d 项（已筛选）" },
    { "appx.smart.s_empty_cat", L"当前分类下没有可显示的应用" },
    { "appx.smart.s_osgate_off", L"[appx] 当前为 Windows 10：已关闭 Windows 11 专属映射，仅加载通用条目" },
    { "appx.smart.s_osgate_on", L"[appx] 当前为 Windows 11：已加载全部映射，共 %d 项" },
    // [R200] 可选功能智能筛选：分类名与分级日志
    { "cap.smart.cat_all", L"全部" },
    { "cap.smart.cat_fod", L"功能包" },
    { "cap.smart.cat_lang", L"语言包" },
    { "cap.smart.cat_rsat", L"管理工具" },
    { "cap.smart.cat_tools", L"开发工具" },
    { "cap.smart.cat_other", L"其他" },
    { "cap.smart.s_empty_cat", L"当前分类下没有可选功能" },
    { "cap.smart.s_osgate_off", L"[cap] 当前为 Windows 10：已关闭 Windows 11 专属映射，仅加载通用条目" },
    { "cap.smart.s_osgate_on", L"[cap] 当前为 Windows 11：已加载全部映射，共 %d 项" },

// [I18N-TOOLBOX-DESC] 工具卡片描述（toolbox.td_<kind>）。
//   此前 13 条全部缺失，卡片描述位显示为 "[toolbox.td_0]" 这类调试占位符。
//   这里按各工具实际功能补齐；语言包存在时优先用译文。
    { "toolbox.td_0", L"\u5907\u4efd\u5f53\u524d\u7cfb\u7edf\u76d8\u4e3a WIM/ESD \u6620\u50cf" },
    { "toolbox.td_1", L"\u5c06 WIM/ESD \u6620\u50cf\u8fd8\u539f\u5230\u5f53\u524d\u7cfb\u7edf" },
    { "toolbox.td_2", L"\u7ba1\u7406\u672c\u673a\u7528\u6237\u8d26\u6237\u4e0e\u5bc6\u7801\u7b56\u7565" },
    { "toolbox.td_3", L"\u91cd\u5efa BIOS/UEFI \u542f\u52a8\u5f15\u5bfc\u6587\u4ef6" },
    { "toolbox.td_4", L"\u4ee5\u6625\u54e5\u56fa\u5316\u65b9\u5f0f\u79bb\u7ebf\u4fee\u6539\u7cfb\u7edf\u7ec4\u4ef6" },
    { "toolbox.td_5", L"\u5c06\u5fae\u8f6f ESD \u6620\u50cf\u8f6c\u6362\u4e3a\u6807\u51c6 ISO" },
    { "toolbox.td_6", L"\u5728 WIM \u4e0e ESD \u683c\u5f0f\u4e4b\u95f4\u4e92\u76f8\u8f6c\u6362" },
    { "toolbox.td_7", L"\u67e5\u770b\u4e0e\u7f16\u8f91\u6620\u50cf\u5185\u7684\u7d22\u5f15\u4fe1\u606f" },
    { "toolbox.td_8", L"\u6d4f\u89c8\u672c\u673a\u4e0e\u79bb\u7ebf\u6620\u50cf\u7684\u6587\u4ef6\u7cfb\u7edf" },
    { "toolbox.td_9", L"\u7528\u6587\u4ef6\u5939\u5185\u5bb9\u751f\u6210\u53ef\u542f\u52a8 ISO" },
    { "toolbox.td_10", L"\u7f16\u8f91 hosts \u6587\u4ef6\u4ee5\u81ea\u5b9a\u4e49\u57df\u540d\u89e3\u6790" },
    { "toolbox.td_11", L"\u5907\u4efd\u4e0e\u8fd8\u539f Windows \u6fc0\u6d3b\u4fe1\u606f" },
    { "toolbox.td_12", L"\u9884\u5904\u7406 Windows 11 \u6620\u50cf\uff08\u7ed5\u8fc7\u786c\u4ef6\u68c0\u6d4b\uff09" },

{ "esdwiz.title", L"ESD \u89e3\u5bc6\u5411\u5bfc" },

{ "esdwiz.desc", L"\u6b64\u529f\u80fd\u53ef\u4ee5\u5c06\u5fae\u8f6f\u53d1\u5e03\u7684 ESD \u89e3\u5bc6\u5bfc\u51fa\u4e3a WIM\uff0c\u8fd8\u652f\u6301\u52a0\u5bc6\u7684 ESD\u3002" },

{ "esdwiz.usesrc", L"\u9009\u62e9\u6e90 ESD \u6587\u4ef6" },

{ "esdwiz.indexhint", L"\u9009\u62e9\u8981\u5bfc\u51fa\u7684\u6620\u50cf\u7d22\u5f15" },

{ "esdwiz.keyhint", L"\u89e3\u5bc6\u5bc6\u94a5\uff08\u53ef\u81ea\u52a8\u5339\u914d\uff0c\u4e5f\u53ef\u624b\u52a8\u8f93\u5165\uff09" },

{ "esdwiz.keyauto", L"\u81ea\u52a8\u5339\u914d\uff08\u5185\u7f6e 6 \u6761\u5bc6\u94a5\uff09" },

{ "esdwiz.keymanual", L"\u624b\u52a8\u8f93\u5165\u89e3\u5bc6\u5bc6\u94a5\u2026" },

{ "esdwiz.keysrc", L"\u5bc6\u94a5\u6765\u6e90\uff1aData\\ESDDecryptKey\uff08<ESDDecryptKey> \u8282\u70b9\uff0c6 \u6761 base64 RSA\uff09" },

{ "esdwiz.export", L"\u5bfc\u51fa(&E)" },

{ "esdwiz.exportok", L"\u6210\u529f\u89e3\u5bc6\uff1a%s" },

{ "esdwiz.nosrc", L"\u8bf7\u5148\u9009\u62e9\u6e90 ESD \u6587\u4ef6\u3002" },

{ "esdwiz.noidx", L"\u672a\u80fd\u8bfb\u53d6\u8be5 ESD \u7684\u7d22\u5f15\u4fe1\u606f\u3002" },

{ "esdwiz.nodst", L"\u8bf7\u9009\u62e9\u76ee\u6807 WIM \u8def\u5f84\u3002" },

{ "esdwiz.nokey", L"\u8bf7\u9009\u62e9\u6216\u8f93\u5165\u89e3\u5bc6\u5bc6\u94a5\u3002" },

{ "esdwiz.confirm", L"\u786e\u5b9a\u5bfc\u51fa\u6240\u9009\u7d22\u5f15\u5230 %s \u5417\uff1f" },

{ "esdwiz.worktitle", L"ESD \u89e3\u5bc6\u5bfc\u51fa" },

{ "esdwiz.batched", L"\u5bfc\u51fa\u5168\u90e8\u7d22\u5f15" },

{ "esdwiz.encrypted", L"\u8be5 ESD \u53ef\u80fd\u5df2\u52a0\u5bc6\uff0c\u8bf7\u9009\u62e9\u5339\u914d\u7684\u5bc6\u94a5\u3002" },

{ "esdwiz.notenc", L"\u8be5\u6587\u4ef6\u6ca1\u6709\u88ab\u52a0\u5bc6\uff0c\u65e0\u9700\u89e3\u5bc6\u3002" },

{ "misc.depgraph_title", L"\u4f9d\u8d56\u56fe" },

{ "misc.depgraph_count", L"\u5171\u0020\u0025\u0064\u0020\u6761\u4f9d\u8d56" },

{ "misc.depgraph_empty", L"\u672a\u68c0\u6d4b\u5230\u4f9d\u8d56\u9879\u3002" },

{ "misc.depgraph_err", L"\u4f9d\u8d56\u67e5\u8be2\u5931\u8d25\uff08\u9519\u8bef\u7801\u0020\u0025\u0064\uff09" },

{ "misc.depgraph_refresh", L"\u5237\u65b0\u0028\u0026\u0052\u0029" },

{ "misc.depgraph_copy", L"\u590d\u5236\u0028\u0026\u0043\u0029" },

{ "misc.depgraph_root", L"\u6839\u8282\u70b9" },

{ "misc.depgraph_child", L"\u5b50\u8282\u70b9" },

{ "toolbox.dep_missing", L"\u8be5\u5de5\u5177\u7f3a\u5c11\u5fc5\u8981\u5916\u90e8\u7a0b\u5e8f\uff0c\u5df2\u7f6e\u7070\uff1b\u8bf7\u5b89\u88c5\u76f8\u5e94\u7ec4\u4ef6\u540e\u91cd\u8bd5\u3002" },



    // [R-B44] 原版 DLGTEMPLATEEX 三模态（ID135/ID137/ID140）

    { "linklist.desc", L"\u70b9\u51fb\u4e0b\u5217\u94fe\u63a5\u53ef\u76f4\u63a5\u6253\u5f00\u5bf9\u5e94\u9875\u9762\u6216\u76ee\u5f55\uff1a" },

    { "dualpath.desc", L"\u9009\u62e9\u4e24\u4e2a\u5df2\u6709\u8def\u5f84\uff0c\u4ee5\u4fbf\u540e\u7eed\u64cd\u4f5c\u5c06\u5728\u4e24\u8005\u4e4b\u95f4\u8fdb\u884c\uff1a" },

    { "dualpath.l1", L"\u8def\u5f84 1\uff1a" },

    { "dualpath.l2", L"\u8def\u5f84 2\uff1a" },

    { "dualpath.browse", L"\u9009\u62e9\u76ee\u6807\u76ee\u5f55" },

    { "dualpath.err", L"\u4e24\u4e2a\u8def\u5f84\u4e0d\u80fd\u4e3a\u7a7a\uff0c\u8bf7\u5b8c\u6574\u9009\u62e9\u3002" },

    { "dualpath.title", L"\u53cc\u8def\u5f84\u9009\u62e9\u5668" },

    { "delconfirm.desc", L"\u4e0b\u5217\u9879\u5c06\u88ab\u5220\u9664\uff0c\u8bf7\u786e\u8ba4\uff1a" },

    { "delconfirm.ok", L"\u5220\u9664(&D)" },


    // [R-B49] 禁用/恢复系统更新
    { "update.s_disable_title", L"\u7981\u7528\u7cfb\u7edf\u66f4\u65b0" },
    { "update.s_disable_confirm", L"\u786e\u5b9a\u8981\u5f7b\u5e95\u7981\u7528\u7cfb\u7edf\u66f4\u65b0\u5417\uff1f\n\n\u5c06\u5199\u5165\u7ec4\u7b56\u7565 NoAutoUpdate=1 \u5e76\u505c\u6b62 Windows Update \u670d\u52a1\u3002\n\n\u6b64\u64cd\u4f5c\u4f1a\u963b\u6b62 Windows \u81ea\u52a8\u4e0b\u8f7d/\u5b89\u88c5\u4efb\u4f55\u66f4\u65b0\uff08\u542b\u5b89\u5168\u66f4\u65b0\uff09\uff0c\u76f4\u5230\u60a8\u624b\u52a8\u6062\u590d\u3002" },
    { "update.s_disable_done", L"[R-B49 \u7b56\u7565] \u7981\u7528\u7cfb\u7edf\u66f4\u65b0\uff1a\u7ed3\u679c=%d" },
    { "update.s_disable_done_short", L"\u5df2\u7981\u7528\u7cfb\u7edf\u66f4\u65b0" },
    { "update.s_disable_fail_short", L"\u7981\u7528\u7cfb\u7edf\u66f4\u65b0\u5931\u8d25\uff08\u9700\u7ba1\u7406\u5458\u6743\u9650\uff09" },
    { "update.s_enable_title", L"\u6062\u590d\u7cfb\u7edf\u66f4\u65b0" },
    { "update.s_enable_confirm", L"\u786e\u5b9a\u8981\u6062\u590d\u7cfb\u7edf\u66f4\u65b0\u5417\uff1f\n\n\u5c06\u5220\u9664 NoAutoUpdate \u7ec4\u7b56\u7565\u5e76\u542f\u52a8 Windows Update \u670d\u52a1\u3002" },
    { "update.s_enable_done", L"[R-B49 \u7b56\u7565] \u6062\u590d\u7cfb\u7edf\u66f4\u65b0\uff1a\u7ed3\u679c=%d" },
    { "update.s_enable_done_short", L"\u5df2\u6062\u590d\u7cfb\u7edf\u66f4\u65b0" },
    { "update.s_enable_fail_short", L"\u6062\u590d\u7cfb\u7edf\u66f4\u65b0\u5931\u8d25\uff08\u9700\u7ba1\u7406\u5458\u6743\u9650\uff09" },

    // [R-B53] 挂载 12 步向导（清单维度 4 缺失 2）
    { "mount.title",          L"\u6302\u8f7d\u6620\u50cf\u5411\u5bfc" },
    { "mount.desc",           L"\u4ece WIM/ESD \u6620\u50cf\u6302\u8f7d\u5230\u672c\u5730\u76ee\u5f55" },
    { "mount.src",            L"\u6e90\u6620\u50cf" },
    { "mount.idx",            L"\u6620\u50cf\u7d22\u5f15" },
    { "mount.mountdir",       L"\u6302\u8f7d\u8def\u5f84" },
    { "mount.readonly",       L"\u53ea\u8bfb\u6a21\u5f0f" },
    { "mount.hint",           L"\u8bf7\u5728\u6b64\u8f93\u5165\u6302\u8f7d\u8def\u5f84\uff0c\u6bd4\u5982D:\\1" },
    { "mount.btn_mount",      L"\u6302\u8f7d\u6620\u50cf" },
    { "mount.mounted",        L"\u5df2\u6302\u8f7d\u7684\u6620\u50cf" },
    { "mount.unmount",        L"\u5378\u8f7d" },
    { "mount.nosrc",          L"\u8bf7\u5148\u9009\u62e9\u6e90 WIM/ESD \u6620\u50cf" },
    { "mount.noidx",          L"\u65e0\u6cd5\u8bfb\u53d6\u6620\u50cf\u7d22\u5f15" },
    { "mount.nodir",          L"\u8bf7\u586b\u5199\u6302\u8f7d\u8def\u5f84" },
    { "mount.s_badmount",     L"\u6302\u8f7d\u5931\u8d25" },
    { "mount.s_skip",         L"\u8df3\u8fc7 %d \u4e2a\u5f02\u5e38\u6302\u8f7d\u70b9" },
    { "mount.s_ok",           L"\u5df2\u6302\u8f7d\uff1a%s  ->  %s" },
    { "mount.s_unmounted",    L"\u5df2\u5378\u8f7d\uff1a%s" },
    { "mount.nomount",        L"\u8bf7\u9009\u62e9\u8981\u5378\u8f7d\u7684\u6302\u8f7d\u76ee\u5f55" },
    { "mount.released",       L"\u91ca\u653e\u6620\u50cf" },
    { "mount.applyhint",      L"\u8bf7\u5728\u6b64\u8f93\u5165\u5b89\u88c5\u8def\u5f84\uff0c\u6bd4\u5982C:\\" },
    // [R-B61] CrashReport 网络上报确认（清单维度 10 缺失 5）
    { "crash.report_title",   L"\u5d29\u6e83\u62a5\u544a" },
    { "crash.report_ask",     L"\u68c0\u6d4b\u5230\u672a\u4e0a\u62a5\u7684\u5d29\u6e83\u8f6c\u50a8\uff1a\n%1\n\u662f\u5426\u63d0\u4ea4\u4e0a\u62a5\uff1f\n\uff08\u9ed8\u8ba4\u5c06\u5411\u914d\u7f6e\u7684\u7aef\u70b9\u53d1\u9001\uff0c\u60a8\u53ef\u4ee5\u9009\u62e9\u201c\u5426\u201d\u4fdd\u7559\u672c\u5730\u3002\uff09" },
    { "crash.report_sent",    L"\u5d29\u6e83\u62a5\u544a\u5df2\u63d0\u4ea4\u3002" },
    { "crash.report_fail",    L"\u5d29\u6e83\u62a5\u544a\u63d0\u4ea4\u5931\u8d25\uff08\u5df2\u4fdd\u7559\u672c\u5730\u3002\uff09" },
    { "crash.report_none",    L"\u672a\u627e\u5230\u53ef\u4e0a\u62a5\u7684\u5d29\u6e83\u8f6c\u50a8\u3002" },
    { "crash.report_nocfg",   L"\u5d29\u6e83\u62a5\u544a\u672a\u914d\u7f6e\u4e0a\u62a5\u7aef\u70b9\uff0c\u5df2\u4fdd\u7559\u672c\u5730\u3002" },
    // [R162] 语言与区域页下拉项后缀（原为 UI 硬编码中文）
    { "intl.cand_installed",     L"\uff08\u5df2\u5b89\u88c5\uff09" },
    // [R162] 欢迎页卡片文案（原为 UI 硬编码中文）
    { "welcome.card_sysdrive", L"\u7cfb\u7edf\u76d8: %s" },
    // [R162] 系统更新页本地补丁安装文案（CpuPost -> HostAlert，用户可见）
    { "update.pkg_begin", L"[update] \u672c\u5730\u8865\u4e01\u5b89\u88c5\u5f00\u59cb\uff1a%s%s" },
    // [R162] 系统更新列表右键菜单项（原为 UI 硬编码中文）
    { "update.menu_addlocal", L"\u5b89\u88c5\u672c\u5730\u8865\u4e01\u5305(.msu/.cab)\u2026" },
    // [R162] 功能管理 0x8007007E 专项提示（原为 UI 硬编码长中文）
    { "feat.s_module_missing_hint", L"\uff08\u901a\u5e38\u662f\u7cfb\u7edf\u7ec4\u4ef6\u5b58\u50a8\u635f\u574f\u6216\u88ab\u7cbe\u7b80\uff0c\u8bf7\u4ee5\u7ba1\u7406\u5458\u8eab\u4efd\u8fd0\u884c\u300cDISM /Online /Cleanup-Image /RestoreHealth\u300d\u4fee\u590d\u7cfb\u7edf\u540e\u91cd\u8bd5\uff09" },
    // [R164] 本地补丁包安装确认框（原为 UI 硬编码中文拼接）
    { "update.ask_install_pkg", L"\u5373\u5c06\u628a %d \u4e2a\u672c\u5730\u8865\u4e01\u5305\uff08.msu/.cab\uff09\u5b89\u88c5\u5230%s\uff0c\u662f\u5426\u7ee7\u7eed\uff1f\n\uff08.msu \u4f1a\u5148\u5c55\u5f00\u4e3a .cab\uff0c\u518d\u7ecf DISM /Add-Package \u5b89\u88c5\uff0c\u53ef\u80fd\u8017\u65f6\u6570\u5206\u949f\uff09" },
    { "update.ask_install_target_offline", L"\u79bb\u7ebf\u6620\u50cf %s" },
    { "update.ask_install_target_online", L"\u5f53\u524d\u5728\u7ebf\u7cfb\u7edf" },
    { "update.ask_install_title", L"\u5b89\u88c5\u672c\u5730\u8865\u4e01\u5305" },
    // [R168] 服务删除前的依赖提示（原为 UI 硬编码中文拼接）
    { "svc.dep_more", L"\r\n    ... \u4ee5\u53ca\u53e6\u5916 %d \u4e2a" },
    { "svc.dep_intro", L"\u68c0\u6d4b\u5230 %d \u4e2a\u4f9d\u8d56\u670d\u52a1\uff1a" },
    { "svc.dep_confirm", L"%s\r\n\r\n\u68c0\u6d4b\u5230 %d \u4e2a\u4f9d\u8d56\u670d\u52a1\uff1a\r\n    - %s\r\n\r\n\u4ecd\u8981\u7ee7\u7eed\u5417\uff1f" },
    // [R162] OPENFILENAME 过滤器（NUL 分隔，末尾双 NUL）
    { "update.pkg_filter", L"\u66f4\u65b0\u8865\u4e01\u5305 (*.msu;*.cab)\0*.msu;*.cab\0\u6240\u6709\u6587\u4ef6 (*.*)\0*.*\0\0" },
    { "update.pkg_mode_online", L"\uff08\u5728\u7ebf\uff09" },
    { "update.pkg_mode_offline", L"\uff08\u79bb\u7ebf %s\uff09" },
    { "update.pkg_ok", L"[update] \u672c\u5730\u8865\u4e01\u5b89\u88c5\u6210\u529f\uff1a%s" },
    { "update.pkg_fail", L"[update] \u672c\u5730\u8865\u4e01\u5b89\u88c5\u5931\u8d25\uff1a%s | %s" },
    { "update.pkg_unknown_err", L"\u672a\u77e5\u9519\u8bef" },
    { "update.pkg_done_all", L"\u672c\u5730\u8865\u4e01\u5b89\u88c5\u5b8c\u6210\uff1a\u6210\u529f %d \u4e2a\u3002\u90e8\u5206\u66f4\u65b0\u53ef\u80fd\u9700\u8981\u91cd\u542f\u540e\u751f\u6548\u3002" },
    { "update.pkg_done_part", L"\u672c\u5730\u8865\u4e01\u5b89\u88c5\u7ed3\u675f\uff1a\u6210\u529f %d \u4e2a\uff0c\u5931\u8d25 %d \u4e2a\u3002\u8be6\u60c5\u89c1\u65e5\u5fd7\u3002" },
    { "welcome.card_fetch_fail", L"\u83b7\u53d6\u5931\u8d25 (err=0x%08X)" },
    // [R162] 0x8007007E 枚举失败提示（原为 UI 硬编码长中文串）
    { "capability.s_err_nomodule", L"\u65e0\u6cd5\u679a\u4e3e\u53ef\u9009\u529f\u80fd\uff08\u9519\u8bef\u7801 0x8007007E\uff1a\u627e\u4e0d\u5230\u6307\u5b9a\u7684\u6a21\u5757\uff09\u3002\u901a\u5e38\u662f\u7cfb\u7edf\u7ec4\u4ef6\u5b58\u50a8\u635f\u574f\u6216\u88ab\u7cbe\u7b80\uff0c\u8bf7\u4ee5\u7ba1\u7406\u5458\u8eab\u4efd\u8fd0\u884c\u300cDISM /Online /Cleanup-Image /RestoreHealth\u300d\u4fee\u590d\u7cfb\u7edf\u540e\u70b9\u51fb\u5237\u65b0\u91cd\u8bd5\u3002" },
    { "intl.cand_current",       L"\uff08\u5f53\u524d\uff09" },
    { "intl.cand_system",        L"\uff08\u7cfb\u7edf\uff09" },
    { "intl.cand_current_ui",    L"\uff08\u5f53\u524d UI\uff09" },
    // [R160] 提权提示与清理扫描文案迁入语言包（原为 UI 硬编码中文）
    { "optim.admin_batch_note",    L"\r\n\u5176\u4e2d %d \u9879\u9700\u8981\u7ba1\u7406\u5458\u6743\u9650\uff0c\u5f53\u524d\u7a0b\u5e8f\u672a\u4ee5\u7ba1\u7406\u5458\u8eab\u4efd\u8fd0\u884c\uff0c\u8fd9\u4e9b\u9879\u53ef\u80fd\u65e0\u6cd5\u751f\u6548\u3002\r\n" },
    { "optim.privilege_required",  L"\u8be5\u9879\u9700\u8981 %s \u6743\u9650\u624d\u80fd\u751f\u6548\uff0c\u8bf7\u4ee5\u5bf9\u5e94\u8eab\u4efd\u91cd\u65b0\u8fd0\u884c\u672c\u7a0b\u5e8f\u3002" },
    { "optim.admin_item_note",     L"\u6ce8\u610f\uff1a\u672c\u9879\u9700\u8981\u7ba1\u7406\u5458\u6743\u9650\uff0c\u5f53\u524d\u7a0b\u5e8f\u672a\u4ee5\u7ba1\u7406\u5458\u8eab\u4efd\u8fd0\u884c\uff0c\u7ee7\u7eed\u64cd\u4f5c\u53ef\u80fd\u65e0\u6cd5\u5199\u5165\u3002" },
    { "cleanup.scan_start_failed", L"\u542f\u52a8\u626b\u63cf\u5931\u8d25" },
    { "cleanup.scan_aborted",      L"\u626b\u63cf\u5f02\u5e38\u7ec8\u6b62" },
    { "cleanup.partial_done",      L"\u6e05\u7406\u5df2\u6267\u884c\uff0c\u4f46\u90e8\u5206\u9879\u76ee\u672a\u5b8c\u6574\u5b8c\u6210\uff08\u91ca\u653e\u7ea6 %s\uff09\u3002\u8bf7\u68c0\u67e5\u65e5\u5fd7\u786e\u8ba4\u3002" },
    { "cleanup.restore_point_compactos", L"CompactOS \u538b\u7f29\u524d\u8fd8\u539f\u70b9" },
    { "toolbox.ui_no_ext_dep", L"\u5185\u7F6E\u7EC4\u4EF6" },
    { "toolbox.ui_cg_done_nofile", L"\u672A\u80FD\u5728\u684C\u9762\u521B\u5EFA\u6062\u590D\u8BF4\u660E\u6587\u4EF6\uFF08\u9519\u8BEF\u7801\u0020\u0025\u0073\uFF09\uFF0C\u8BF7\u7ACB\u5373\u624B\u52A8\u6284\u5F55\u672C\u6B21\u8BBE\u7F6E\u7684\u7BA1\u7406\u5458\u5BC6\u7801\u3002" },
    { "toolbox.ui_cg_done_ok", L"\u5DF2\u5B8C\u6210\uFF0C\u6062\u590D\u8BF4\u660E\u6587\u4EF6\u5DF2\u4FDD\u5B58\u5230\u684C\u9762\u3002" },
    { "toolbox.ui_cg_autologon", L"\u5DF2\u540C\u65F6\u5F00\u542F\u81EA\u52A8\u767B\u5F55\u3002" },
    { "toolbox.ui_cg_uac_off", L"\u5DF2\u540C\u65F6\u5173\u95ED\u0020\u0055\u0041\u0043\uFF08\u91CD\u542F\u540E\u751F\u6548\uFF09\u3002" },
    { "toolbox.ui_cg_fail_fmt", L"\u8BBE\u7F6E\u7BA1\u7406\u5458\u8D26\u6237\u5931\u8D25\uFF08\u9519\u8BEF\u7801\u0020\u0025\u0073\uFF09\uFF0C\u5DF2\u6062\u590D\u539F\u8D26\u6237\u72B6\u6001\u3002" },
    { "toolbox.ui_cg_note_title", L"\u6625\u54E5\u9644\u4F53\u6062\u590D\u8BF4\u660E" },
    { "toolbox.ui_cg_note_intro", L"\u672C\u6587\u4EF6\u8BB0\u5F55\u672C\u6B21\u300C\u6625\u54E5\u9644\u4F53\u300D\u6240\u505A\u7684\u4FEE\u6539\u3002\u5982\u9700\u8FD8\u539F\uFF0C\u8BF7\u4EE5\u7BA1\u7406\u5458\u8EAB\u4EFD\u9010\u6761\u8FD0\u884C\u4E0B\u5217\u547D\u4EE4\u3002" },
    { "toolbox.ui_cg_note_s1", L"\u0031\u002E\u0020\u91CD\u65B0\u7981\u7528\u0020\u0041\u0064\u006D\u0069\u006E\u0069\u0073\u0074\u0072\u0061\u0074\u006F\u0072\u0020\u8D26\u6237\uFF1A" },
    { "toolbox.ui_cg_note_s2", L"\u0032\u002E\u0020\u53D6\u6D88\u81EA\u52A8\u767B\u5F55\uFF08\u5220\u9664\u0020\u0057\u0069\u006E\u006C\u006F\u0067\u006F\u006E\u0020\u4E0B\u7684\u81EA\u52A8\u767B\u5F55\u9879\uFF09\uFF1A" },
    { "toolbox.ui_cg_note_s3_uac", L"\u0033\u002E\u0020\u91CD\u65B0\u542F\u7528\u0020\u0055\u0041\u0043\uFF08\u81EA\u52A8\u767B\u5F55\u9879\u5DF2\u5728\u7B2C\u0020\u0032\u0020\u6B65\u5220\u9664\uFF09\uFF1A" },
    { "toolbox.ui_cg_note_s2_uac", L"\u0032\u002E\u0020\u91CD\u65B0\u542F\u7528\u0020\u0055\u0041\u0043\uFF08\u672C\u6B21\u672A\u5F00\u542F\u81EA\u52A8\u767B\u5F55\uFF09\uFF1A" },
    { "toolbox.ui_cg_note_warn", L"\u6CE8\u610F\uFF1A\u5173\u95ED\u0020\u0055\u0041\u0043\u0020\u4E0E\u5F00\u542F\u81EA\u52A8\u767B\u5F55\u90FD\u4F1A\u964D\u4F4E\u7CFB\u7EDF\u5B89\u5168\u6027\uFF0C\u8FD8\u539F\u540E\u5EFA\u8BAE\u91CD\u542F\u5E76\u91CD\u65B0\u8BBE\u7F6E\u8D26\u6237\u5BC6\u7801\u3002" },
    { "toolbox.ui_cg_note_file", L"\u6625\u54E5\u9644\u4F53\u6062\u590D\u8BF4\u660E\u002E\u0074\u0078\u0074" },
    { "toolbox.ui_hosts_dlg_title", L"\u7F16\u8F91\u0020\u0048\u006F\u0073\u0074\u0073\u0020\u6587\u4EF6" },
    { "toolbox.ui_btn_save", L"\u4FDD\u5B58" },
    { "toolbox.ui_btn_cancel", L"\u53D6\u6D88" },
    { "toolbox.ui_esd2iso_busy", L"\u6B63\u5728\u5C06\u0020\u0045\u0053\u0044\u0020\u8F6C\u6362\u4E3A\u0020\u0049\u0053\u004F\uFF0C\u8BF7\u7A0D\u5019\u2026" },
    { "toolbox.ui_esd2iso_ok_title", L"\u8F6C\u6362\u5B8C\u6210" },
    { "toolbox.ui_esd2iso_ok_msg", L"\u0049\u0053\u004F\u0020\u955C\u50CF\u5DF2\u751F\u6210\u5B8C\u6210\u3002" },
    { "toolbox.ui_esd2iso_fail_title", L"\u8F6C\u6362\u5931\u8D25" },
    { "toolbox.s_job_start_fail", L"\u65E0\u6CD5\u542F\u52A8\u540E\u53F0\u8F6C\u6362\u4F5C\u4E1A\uFF0C\u672C\u6B21\u64CD\u4F5C\u672A\u6267\u884C\u3002" },
    { "toolbox.ui_esd2iso_title", L"\u0045\u0053\u0044\u0020\u8F6C\u0020\u0049\u0053\u004F" },
    { "toolbox.ui_ex_convert", L"\u6620\u50CF\u5BFC\u51FA\u5931\u8D25\uFF0C\u53EF\u80FD\u662F\u6E90\u6587\u4EF6\u635F\u574F\u3001\u88AB\u5360\u7528\u6216\u76EE\u6807\u4F4D\u7F6E\u65E0\u5199\u5165\u6743\u9650\u3002" },
    { "toolbox.ui_act_backup_fail", L"\u6FC0\u6D3B\u4FE1\u606F\u5907\u4EFD\u5931\u8D25\u3002" },
    { "toolbox.ui_act_restore_fail", L"\u6FC0\u6D3B\u4FE1\u606F\u8FD8\u539F\u5931\u8D25\u3002" },
    { "toolbox.ui_swm_title", L"\u5408\u5E76\u0020\u0053\u0057\u004D\u0020\u5206\u5377" },
    { "toolbox.ui_swm_unavailable", L"\u672A\u627E\u5230\u53EF\u5408\u5E76\u7684\u0020\u0053\u0057\u004D\u0020\u5206\u5377\u6587\u4EF6\uFF0C\u8BF7\u786E\u8BA4\u6240\u9009\u76EE\u5F55\u4E2D\u5B58\u5728\u0020\u002E\u0073\u0077\u006D\u0020\u6587\u4EF6\u3002" },
    { "toolbox.ui_swm_busy", L"\u6B63\u5728\u5408\u5E76\u0020\u0053\u0057\u004D\u0020\u5206\u5377\uFF0C\u8BF7\u7A0D\u5019\u2026" },
    { "toolbox.ui_swm_ok_title", L"\u5408\u5E76\u5B8C\u6210" },
    { "toolbox.ui_swm_ok_msg", L"\u0053\u0057\u004D\u0020\u5206\u5377\u5DF2\u5408\u5E76\u4E3A\u0020\u0057\u0049\u004D\u0020\u6620\u50CF\u3002" },
    { "toolbox.ui_swm_fail_title", L"\u5408\u5E76\u5931\u8D25" },
    { "toolbox.ui_convert_busy", L"\u6B63\u5728\u8F6C\u6362\u6620\u50CF\u683C\u5F0F\uFF0C\u8BF7\u7A0D\u5019\u2026" },
    { "toolbox.ui_convert_ok_title", L"\u8F6C\u6362\u5B8C\u6210" },
    { "toolbox.ui_convert_ok_msg", L"\u6620\u50CF\u683C\u5F0F\u8F6C\u6362\u5B8C\u6210\u3002" },
    { "toolbox.ui_convert_fail_title", L"\u8F6C\u6362\u5931\u8D25" },
    { "toolbox.ui_convert_fail_msg", L"\u6620\u50CF\u8F6C\u6362\u5931\u8D25\uFF0C\u8BF7\u68C0\u67E5\u6E90\u6587\u4EF6\u662F\u5426\u88AB\u5360\u7528\u3001\u76EE\u6807\u78C1\u76D8\u7A7A\u95F4\u662F\u5426\u5145\u8DB3\u3002" },
    { "toolbox.ui_convert_title", L"\u0057\u0049\u004D\u0020\u4E0E\u0020\u0045\u0053\u0044\u0020\u4E92\u8F6C" },
    // Hosts 保存时 CreateFileW 失败的通知标题（正文见 ui_hosts_save_fail）
    { "toolbox.s_hosts_write_fail", L"无法写入文件" },
    // 与 s_hosts_write_fail 成对：System 对话框正文
    { "toolbox.ui_hosts_save_fail", L"保存 Hosts 文件失败，请检查该文件是否被占用或权限不足。" },
    // ESD→ISO 第一阶段开始前的工作目录准备失败
    { "toolbox.ui_temp_dir_fail", L"无法创建临时目录，请检查磁盘剩余空间与权限。" },
    // 长任务标题：WIM 阶段
    { "toolbox.ui_stage1_wim", L"正在挂载并导出映像（第 1/2 步）…" },
    // 长任务标题：ISO 阶段
    { "toolbox.ui_stage2_iso", L"正在生成 ISO 文件（第 2/2 步）…" },
    // 导出被用户主动取消（非错误，但需要告知结果）
    { "toolbox.ui_cancel_wim_export", L"用户取消：已中止映像导出。" },
    // ESD→ISO 未捕获异常时的兜底文案
    { "toolbox.ui_ex_esd2iso", L"导出 ESD 转 ISO 过程中发生异常，请查看详细日志。" },
    // WIM→SWM 未捕获异常时的兜底文案
    { "toolbox.ui_ex_swm", L"导出 SWM 分卷过程中发生异常，请查看详细日志。" },
    // 带错误码的转换失败正文（%s 由 Format 填入）
    { "toolbox.ui_convert_fail_fmt", L"映像转换失败，系统错误码 %s。请确认源文件未被占用、目标磁盘空间充足。" },
    // 带错误描述的导出失败正文（%s 由 FormatW 填入）
    { "toolbox.ui_wim_export_fail_fmt", L"导出映像失败：%s" },
    // 任务队列占位行
    { "toolbox.ui_task_running", L"任务执行中，请稍候…" },

    { "feat.s_loadinglist", L"\u8BFB\u53D6\u0020\u0057\u0069\u006E\u0064\u006F\u0077\u0073\u0020\u529F\u80FD\u5217\u8868" },
    { "feat.s_listtitle", L"\u0057\u0069\u006E\u0064\u006F\u0077\u0073\u0020\u529F\u80FD\u5217\u8868" },
    { "feat.s_detailtitle", L"\u529F\u80FD\u8BE6\u60C5" },
    { "feat.s_detailhint", L"\u5728\u5DE6\u4FA7\u5217\u8868\u4E2D\u9009\u62E9\u4E00\u9879\u529F\u80FD\uFF0C\u5373\u53EF\u67E5\u770B\u5176\u72B6\u6001\u4E0E\u8BF4\u660E\u3002" },
    { "feat.s_nodesc", L"\u6682\u65E0\u8BF4\u660E\u3002" },
    { "feat.s_groupnote", L"\u5206\u7EC4\u6807\u9898\uFF0C\u6C47\u603B\u4E0B\u65B9\u5404\u9879\u7684\u72B6\u6001\u3002" },
    { "optim.s_copydetail_ok", L"\u5DF2\u590D\u5236\u300C\u0025\u0073\u300D\u7684\u8BE6\u7EC6\u4FE1\u606F" },
    { "optim.s_copydetail_fail", L"\u590D\u5236\u300C\u0025\u0073\u300D\u7684\u8BE6\u7EC6\u4FE1\u606F\u5931\u8D25" },
};







inline constexpr size_t s_builtinCount = sizeof(s_builtinZhCN) / sizeof(s_builtinZhCN[0]);







// ------------------------------------------------------------



// 私有实现



// ------------------------------------------------------------



// ------------------------------------------------------------
// [R-B73] 外部语言加载辅助工具（文件内部 namespace rb73）
// 包含：UTF-8 文件读取、极小 JSON 解析、UTF-8→宽字符转换、
//       PowerShell 解压 zip 单条目（系统自带 .NET，无第三方依赖）。
// ------------------------------------------------------------
namespace rb73 {

static bool ReadFileBytes(const wchar_t* pszPath, std::string& out)
{
    out.clear();
    std::ifstream ifs(pszPath, std::ios::binary);
    if (!ifs) return false;
    ifs.seekg(0, std::ios::end);
    std::streamsize n = ifs.tellg();
    if (n < 0) return false;
    ifs.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(n));
    if (n > 0) ifs.read(&out[0], n);
    if (out.size() >= 3 &&
        (unsigned char)out[0] == 0xEF &&
        (unsigned char)out[1] == 0xBB &&
        (unsigned char)out[2] == 0xBF)
        out.erase(0, 3);
    return true;
}

static std::string Trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' ||
                     s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b-1] == ' ' || s[b-1] == '\t' ||
                     s[b-1] == '\r' || s[b-1] == '\n')) --b;
    return s.substr(a, b - a);
}

static std::wstring Utf8ToW(const std::string& s)
{
    if (s.empty()) return std::wstring();
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                  static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(static_cast<size_t>(n), 0);
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

// [FIX-UNESC] 语言包文本里的字面 "\uXXXX" 转义还原。
//   为什么需要：一批翻译数据在导出时被二次转义，JSON 里存的是可见的
//   反斜杠+u+4位十六进制（而不是真正的 JSON 转义），JSON 解析器按规范
//   原样交出这些字符，界面就显示成 \u5168\u90e8 而不是"全部"。
//   数据侧已修正 zh-CN（345 条）与 it（2 条）；这里再加一道运行时兜底，
//   让"外部导入/旧版本语言包"也能正确显示，而不是把乱码印到界面上。
//   只处理 \uXXXX（可多段连续），其它反斜杠序列原样保留，避免误伤
//   printf 里的 \n、\\ 等真实控制序列。
static std::wstring UnescapeUnicodeRuns(const std::wstring& in)
{
    if (in.find(L"\\u") == std::wstring::npos) return in;
    std::wstring out;
    out.reserve(in.size());
    for (size_t i = 0; i + 5 < in.size() + 1 && i < in.size(); ++i)
    {
        if (in[i] == L'\\' && i + 5 < in.size() + 1 && i + 1 < in.size()
            && in[i + 1] == L'u' && i + 5 < in.size() + 1)
        {
            // 校验紧随其后的 4 位十六进制
            bool bHex = true;
            wchar_t wcode[5] = { 0 };
            for (int k = 0; k < 4; ++k)
            {
                if (i + 2 + k >= in.size()) { bHex = false; break; }
                const wchar_t c = in[i + 2 + k];
                const bool ok = (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f')
                             || (c >= L'A' && c <= L'F');
                if (!ok) { bHex = false; break; }
                wcode[k] = c;
            }
            if (bHex)
            {
                unsigned int cp = static_cast<unsigned int>(wcstoul(wcode, nullptr, 16));
                if (cp > 0 && cp <= 0xFFFF)
                {
                    out.push_back(static_cast<wchar_t>(cp));
                    i += 5;   // 跳过 "\uXXXX"
                    continue;
                }
            }
        }
        out.push_back(in[i]);
    }
    return out;
}

static void AppendUtf8(std::string& out, unsigned int cp)
{
    if (cp <= 0x7F) out.push_back(static_cast<char>(cp));
    else if (cp <= 0x7FF)
    {
        out.push_back((char)(0xC0 | (cp >> 6)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    }
    else if (cp <= 0xFFFF)
    {
        out.push_back((char)(0xE0 | (cp >> 12)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    }
    else
    {
        out.push_back((char)(0xF0 | (cp >> 18)));
        out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    }
}

struct JVal
{
    enum class T : int { Nul, Bol, Num, Str, Arr, Obj };
    T t = T::Nul;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<JVal> arr;
    std::vector<std::pair<std::string, JVal> > obj;
};

class MiniJson
{
public:
    explicit MiniJson(const std::string& src) : m_s(src), m_i(0) {}
    bool Parse(JVal& out)
    {
        SkipWs();
        if (!ParseVal(out)) return false;
        SkipWs();
        return m_i >= m_s.size();
    }
private:
    const std::string& m_s;
    size_t m_i;
    void SkipWs()
    {
        while (m_i < m_s.size() &&
               (m_s[m_i] == ' ' || m_s[m_i] == '\t' ||
                m_s[m_i] == '\r' || m_s[m_i] == '\n'))
            ++m_i;
    }
    static int HexVal(char c)
    {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }
    bool ParseVal(JVal& v)
    {
        SkipWs();
        if (m_i >= m_s.size()) return false;
        char c = m_s[m_i];
        if (c == '{') return ParseObj(v);
        if (c == '[') return ParseArr(v);
        if (c == '"') { v.t = JVal::T::Str; return ParseStr(v.s); }
        if (c == 't' || c == 'f')
        {
            v.t = JVal::T::Bol; v.b = (c == 't');
            m_i += (c == 't' ? 4 : 5); return true;
        }
        if (c == 'n') { v.t = JVal::T::Nul; m_i += 4; return true; }
        return ParseNum(v);
    }
    bool ParseStr(std::string& out)
    {
        if (m_i >= m_s.size() || m_s[m_i] != '"') return false;
        ++m_i;
        while (m_i < m_s.size())
        {
            char c = m_s[m_i++];
            if (c == '"') return true;
            if (c != '\\') { out.push_back(c); continue; }
            if (m_i >= m_s.size()) return false;
            char e = m_s[m_i++];
            switch (e)
            {
                case '"':  out.push_back('"');  break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/');  break;
                case 'b':  out.push_back('\b'); break;
                case 'f':  out.push_back('\f'); break;
                case 'n':  out.push_back('\n'); break;
                case 'r':  out.push_back('\r'); break;
                case 't':  out.push_back('\t'); break;
                case 'u':
                {
                    if (m_i + 4 > m_s.size()) return false;
                    unsigned int cp = 0;
                    for (int k = 0; k < 4; ++k)
                    {
                        int h = HexVal(m_s[m_i++]);
                        if (h < 0) return false;
                        cp = (cp << 4) | (unsigned int)h;
                    }
                    AppendUtf8(out, cp);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }
    bool ParseNum(JVal& v)
    {
        size_t start = m_i;
        while (m_i < m_s.size() &&
               ((m_s[m_i] >= '0' && m_s[m_i] <= '9') || m_s[m_i] == '-' ||
                m_s[m_i] == '+' || m_s[m_i] == '.' ||
                m_s[m_i] == 'e' || m_s[m_i] == 'E'))
            ++m_i;
        if (m_i == start) return false;
        v.t = JVal::T::Num;
        v.n = atof(m_s.substr(start, m_i - start).c_str());
        return true;
    }
    bool ParseArr(JVal& v)
    {
        v.t = JVal::T::Arr;
        ++m_i; SkipWs();
        if (m_i < m_s.size() && m_s[m_i] == ']') { ++m_i; return true; }
        while (true)
        {
            JVal e;
            if (!ParseVal(e)) return false;
            v.arr.push_back(e);
            SkipWs();
            if (m_i >= m_s.size()) return false;
            if (m_s[m_i] == ',') { ++m_i; continue; }
            if (m_s[m_i] == ']') { ++m_i; return true; }
            return false;
        }
    }
    bool ParseObj(JVal& v)
    {
        v.t = JVal::T::Obj;
        ++m_i; SkipWs();
        if (m_i < m_s.size() && m_s[m_i] == '}') { ++m_i; return true; }
        while (true)
        {
            SkipWs();
            if (m_i >= m_s.size() || m_s[m_i] != '"') return false;
            std::string key;
            if (!ParseStr(key)) return false;
            SkipWs();
            if (m_i >= m_s.size() || m_s[m_i] != ':') return false;
            ++m_i;
            JVal val;
            if (!ParseVal(val)) return false;
            v.obj.push_back(std::make_pair(key, val));
            SkipWs();
            if (m_i >= m_s.size()) return false;
            if (m_s[m_i] == ',') { ++m_i; continue; }
            if (m_s[m_i] == '}') { ++m_i; return true; }
            return false;
        }
    }
};

static bool CollectJsonStrings(const JVal& root,
                               std::map<std::string, std::wstring>& out)
{
    if (root.t == JVal::T::Arr)
    {
        bool any = false;
        for (size_t i = 0; i < root.arr.size(); ++i)
            if (CollectJsonStrings(root.arr[i], out)) any = true;
        return any;
    }
    if (root.t != JVal::T::Obj) return false;
    const JVal* pk = nullptr;
    const JVal* pt = nullptr;
    for (size_t i = 0; i < root.obj.size(); ++i)
    {
        if (root.obj[i].first == "key")   pk = &root.obj[i].second;
        else if (root.obj[i].first == "text") pt = &root.obj[i].second;
    }
    if (pk && pt && pk->t == JVal::T::Str && pt->t == JVal::T::Str && !pk->s.empty())
    {
        out[pk->s] = UnescapeUnicodeRuns(Utf8ToW(pt->s));
        return true;
    }
    bool any = false;
    for (size_t i = 0; i < root.obj.size(); ++i)
    {
        if (root.obj[i].second.t == JVal::T::Str && !root.obj[i].first.empty())
        {
            out[root.obj[i].first] = UnescapeUnicodeRuns(Utf8ToW(root.obj[i].second.s));
            any = true;
        }
        else if (CollectJsonStrings(root.obj[i].second, out)) any = true;
    }
    return any;
}

static bool LoadJsonStringsFile(const wchar_t* pszPath,
                                std::map<std::string, std::wstring>& out)
{
    std::string data;
    if (!ReadFileBytes(pszPath, data)) return false;
    MiniJson pj(data);
    JVal root;
    if (!pj.Parse(root)) return false;
    std::map<std::string, std::wstring> tmp;
    if (!CollectJsonStrings(root, tmp) || tmp.empty()) return false;
    for (std::map<std::string, std::wstring>::const_iterator it = tmp.begin();
         it != tmp.end(); ++it)
        out[it->first] = it->second;
    return true;
}

static bool LoadIniStringsFile(const wchar_t* pszPath,
                               std::map<std::string, std::wstring>& out)
{
    std::string data;
    if (!ReadFileBytes(pszPath, data)) return false;
    size_t n = 0;
    while (n < data.size())
    {
        size_t e = data.find('\n', n);
        std::string line = Trim(data.substr(
            n, (e == std::string::npos ? data.size() : e) - n));
        n = (e == std::string::npos ? data.size() : e + 1);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = Trim(line.substr(0, eq));
        std::string val = Trim(line.substr(eq + 1));
        if (key.empty()) continue;
        if (val.size() >= 2 && val.front() == '"' && val.back() == '"')
            val = val.substr(1, val.size() - 2);
        out[key] = UnescapeUnicodeRuns(Utf8ToW(val));
    }
    return !out.empty();
}

static bool LoadStringsFile(const wchar_t* pszPath,
                            std::map<std::string, std::wstring>& out)
{
    std::wstring path(pszPath);
    std::wstring ext;
    size_t dot = path.find_last_of(L'.');
    if (dot != std::wstring::npos) ext = path.substr(dot);
    if (lstrcmpiW(ext.c_str(), L".json") == 0)
        return LoadJsonStringsFile(pszPath, out);
    return LoadIniStringsFile(pszPath, out);
}

static bool LoadMetaFile(const wchar_t* pszPath,
                         std::map<std::string, std::wstring>& out)
{
    out.clear();
    if (::GetFileAttributesW(pszPath) == INVALID_FILE_ATTRIBUTES) return false;
    std::string data;
    if (!ReadFileBytes(pszPath, data)) return false;
    MiniJson pj(data);
    JVal root;
    if (!pj.Parse(root) || root.t != JVal::T::Obj) return false;
    for (size_t i = 0; i < root.obj.size(); ++i)
        if (root.obj[i].second.t == JVal::T::Str)
            out[root.obj[i].first] = UnescapeUnicodeRuns(Utf8ToW(root.obj[i].second.s));
    return !out.empty();
}

static bool ExtractZipEntryBySuffix(const wchar_t* pszZip, const wchar_t* pszSuffix,
                                    std::wstring& outPath)
{
    outPath.clear();
    if (!pszZip || !*pszZip) return false;

    // [原生化] minizip 进程内枚举条目（等价 powershell Entries |
    // Where-Object FullName -like '*suffix'），零外部进程。
    HZIP hz = OpenZip(pszZip);
    if (!hz) return false;

    bool bOk = false;
    ZIPENTRY zeTotal;
    zeTotal = {};
    if (GetZipItem(hz, -1, &zeTotal) == ZR_OK)
    {
        wchar_t szTmp[MAX_PATH] = { 0 };
        ::GetTempPathW(MAX_PATH, szTmp);
        std::wstring dir = std::wstring(szTmp) + L"dismpp_langextra_"
                         + std::to_wstring(::GetTickCount64());
        ::CreateDirectoryW(dir.c_str(), nullptr);
        std::wstring dst = dir + L"\\langstrings.tmp";

        int nSuffix = static_cast<int>(wcslen(pszSuffix));
        for (int i = 0; i < zeTotal.index && !bOk; ++i)
        {
            ZIPENTRY ze;
            ze = {};
            if (GetZipItem(hz, i, &ze) != ZR_OK) continue;
            if (ze.attr & FILE_ATTRIBUTE_DIRECTORY) continue;
            int nName = static_cast<int>(wcslen(ze.name));
            if (nName < nSuffix) continue;
            if (_wcsnicmp(ze.name + nName - nSuffix, pszSuffix, nSuffix) != 0)
                continue;
            if (UnzipItem(hz, i, dst.c_str()) == ZR_OK
                && ::GetFileAttributesW(dst.c_str()) != INVALID_FILE_ATTRIBUTES)
            {
                outPath = dst;
                bOk = true;
            }
        }
    }
    CloseZip(hz);
    return bOk;
}

} // namespace rb73

// ------------------------------------------------------------
// [R-B73] 可选外部语言加载（松散目录 / ZIP / JSON / INI）
// 优先级：卫星 DLL > 外部 JSON/INI > 内置 zh-CN 表 > "[key]"
// ------------------------------------------------------------

bool CStringManager::IsExternalLangAvailable(const wchar_t* pszLangCode) const
{
    if (!pszLangCode || !pszLangCode[0]) return false;
    if (m_externalDir.empty()) return false;
    std::wstring code = pszLangCode;
    std::wstring dirPath = m_externalDir + L"\\" + code;
    DWORD attr = ::GetFileAttributesW(dirPath.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
        return true;
    std::wstring zipPath = m_externalDir + L"\\" + code + L".zip";
    attr = ::GetFileAttributesW(zipPath.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY))
        return true;
    return false;
}

void CStringManager::UnloadExternalLang()
{
    m_externalMap.clear();
}

bool CStringManager::LoadExternalLang(const wchar_t* pszLangCode)
{
    m_externalMap.clear();
    if (!pszLangCode || !pszLangCode[0]) return false;
    if (m_externalDir.empty()) return false;

    std::wstring code = pszLangCode;
    std::map<std::string, std::wstring> map;
    const wchar_t* cands[] = {
        L"strings.json", L"strings.ini",
        L"lang.json",    L"lang.ini"
    };

    // ---- 1) 松散目录 lang-extra/<code>/ ----
    std::wstring dirPath = m_externalDir + L"\\" + code;
    DWORD attr = ::GetFileAttributesW(dirPath.c_str());
    bool bFromDir = (attr != INVALID_FILE_ATTRIBUTES &&
                     (attr & FILE_ATTRIBUTE_DIRECTORY));
    if (bFromDir)
    {
        for (int i = 0; i < 4; ++i)
        {
            std::wstring p = dirPath + L"\\" + cands[i];
            if (::GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES &&
                rb73::LoadStringsFile(p.c_str(), map))
                break;
        }
    }

    // ---- 2) zip lang-extra/<code>.zip ----
    if (map.empty())
    {
        std::wstring zipPath = m_externalDir + L"\\" + code + L".zip";
        attr = ::GetFileAttributesW(zipPath.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES &&
            !(attr & FILE_ATTRIBUTE_DIRECTORY))
        {
            for (int i = 0; i < 4; ++i)
            {
                std::wstring tmp;
                if (rb73::ExtractZipEntryBySuffix(zipPath.c_str(),
                                                  cands[i], tmp))
                {
                    if (rb73::LoadStringsFile(tmp.c_str(), map)) break;
                    ::DeleteFileW(tmp.c_str());
                }
            }
        }
    }

    if (map.empty()) return false;

    // ---- 元数据（可选）：meta.json ----
    std::map<std::string, std::wstring> meta;
    std::wstring metaPath;
    if (bFromDir) metaPath = dirPath + L"\\meta.json";
    if (metaPath.empty())
    {
        std::wstring tmp;
        if (rb73::ExtractZipEntryBySuffix(
                (m_externalDir + L"\\" + code + L".zip").c_str(),
                L"meta.json", tmp))
            metaPath = tmp;
    }
    if (!metaPath.empty() &&
        ::GetFileAttributesW(metaPath.c_str()) != INVALID_FILE_ATTRIBUTES)
        rb73::LoadMetaFile(metaPath.c_str(), meta);

    // ---- 提交 ----
    m_externalMap.swap(map);
    std::map<std::string, std::wstring>::const_iterator it;
    if (!meta.empty())
    {
        if ((it = meta.find("langName")) != meta.end() && !it->second.empty())
            m_langName = it->second;
        m_currentLang = code;
        if ((it = meta.find("langCode")) != meta.end() && !it->second.empty())
            m_currentLang = it->second;
        if ((it = meta.find("langVersion")) != meta.end() && !it->second.empty())
            m_langVersion = it->second;
        if ((it = meta.find("langAuthor")) != meta.end() && !it->second.empty())
            m_langAuthor = it->second;
        if ((it = meta.find("langDate")) != meta.end() && !it->second.empty())
            m_langDate = it->second;
        if ((it = meta.find("langCopyright")) != meta.end() && !it->second.empty())
            m_langCopyright = it->second;
    }
    else
    {
        m_currentLang = code;
        m_langVersion = L"external";
    }

    m_usingFallback = false;
    m_keyCount = static_cast<int>(m_externalMap.size());
    return true;
}

const wchar_t* CStringManager::LookupExternal(const char* pszKeyUtf8)
{
    if (!pszKeyUtf8 || !pszKeyUtf8[0] || m_externalMap.empty()) return nullptr;
    std::map<std::string, std::wstring>::const_iterator it =
        m_externalMap.find(pszKeyUtf8);
    if (it == m_externalMap.end() || it->second.empty()) return nullptr;
    return it->second.c_str();
}

CStringManager::~CStringManager()



{



    Shutdown();



}







void CStringManager::Shutdown()



{



    UnloadLanguageDll();



    m_initialized = false;



    m_usingFallback = true;



}







void CStringManager::UnloadLanguageDll()



{



    m_pfnGetString = nullptr;



    m_pfnGetKeyCount = nullptr;



    if (m_hModule)



    {



        ::FreeLibrary(m_hModule);



        m_hModule = nullptr;



    }



}







bool CStringManager::LoadLanguageDll(const wchar_t* pszPath, const wchar_t* pszLangCode)



{



    // 路径本身就是宽字符，直接用 LoadLibraryW。



    // 旧实现先转 UTF-8 再走 LoadLibraryA：LoadLibraryA 按 ANSI(GBK)



    // 解码而非 UTF-8，路径含非 ASCII 字符时会找不到文件。



    m_hModule = ::LoadLibraryW(pszPath);



    if (!m_hModule) return false;







    m_pfnGetString   = (PFN_GetString)  ::GetProcAddress(m_hModule, "GetString");



    m_pfnGetKeyCount = (PFN_GetKeyCount)::GetProcAddress(m_hModule, "GetKeyCount");



    if (!m_pfnGetString || !m_pfnGetKeyCount)



    {



        UnloadLanguageDll();



        return false;



    }







    // 读取元数据（导出可能缺失，逐个判空，禁止对 nullptr 直接调用）



    using PFN_GetMeta = const wchar_t* (*)();



    const wchar_t* p = nullptr;



    PFN_GetMeta pfnMeta = nullptr;



    if ((pfnMeta = (PFN_GetMeta)::GetProcAddress(m_hModule, "GetLangName"))



        && (p = pfnMeta()))



    {



        m_langName = p;



    }



    if ((pfnMeta = (PFN_GetMeta)::GetProcAddress(m_hModule, "GetLangCode"))



        && (p = pfnMeta()))



    {



        m_currentLang = p;



    }



    else if (pszLangCode)



        m_currentLang = pszLangCode;



    if ((pfnMeta = (PFN_GetMeta)::GetProcAddress(m_hModule, "GetLangVersion"))



        && (p = pfnMeta()))



    {



        m_langVersion = p;



    }



    if ((pfnMeta = (PFN_GetMeta)::GetProcAddress(m_hModule, "GetAuthor"))



        && (p = pfnMeta()))



    {



        m_langAuthor = p;



    }



    if ((pfnMeta = (PFN_GetMeta)::GetProcAddress(m_hModule, "GetDate"))



        && (p = pfnMeta()))



    {



        m_langDate = p;



    }



    if ((pfnMeta = (PFN_GetMeta)::GetProcAddress(m_hModule, "GetCopyright"))



        && (p = pfnMeta()))



    {



        m_langCopyright = p;



    }







    m_keyCount = m_pfnGetKeyCount();



    return true;



}







// [S-01-FIX] LookupBuiltin 索引化：原 O(793) 线性扫描 → O(1) hash 查找。
//   793 条内置表在渲染循环中高频调用（Capability 每行多次取文案），
//   每帧累计扫描次数可观。改用 magic static 一次性构建的 unordered_map，
//   首调时 ~793 次插入（微秒级），此后全部 O(1) 查找。
//   string_view 指向 s_builtinZhCN 静态数组，生命周期为程序全量，安全。
const wchar_t* CStringManager::LookupBuiltin(const char* pszKeyUtf8)
{
    if (!pszKeyUtf8 || !pszKeyUtf8[0]) return L"";

    static const auto kIndex = []() {
        std::unordered_map<std::string_view, const wchar_t*> idx;
        idx.reserve(s_builtinCount);
        for (size_t i = 0; i < s_builtinCount; ++i)
            idx.emplace(s_builtinZhCN[i].key, s_builtinZhCN[i].text);
        return idx;
    }();

    auto it = kIndex.find(std::string_view(pszKeyUtf8));
    return (it != kIndex.end()) ? it->second : nullptr;
}







bool CStringManager::Init(const wchar_t* pszAppDir, const wchar_t* pszLangCode)



{



    if (m_initialized) return true;







    // 语言目录



    if (pszAppDir)



    {



        std::wstring dir = pszAppDir;



        if (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/'))



            dir.pop_back();



        dir += L"\\lang";



        m_langDir = dir;



    }







    // 确定语言代码



    std::wstring langCode;



    if (pszLangCode && pszLangCode[0])



    {



        langCode = pszLangCode;



    }



    else



    {



        // 系统 UI locale → "zh-CN" / "en-US" 格式



        // LOCALE_SISO639LANGNAME=0x59 / LOCALE_SISO3166CTRYNAME=0x5A



        // （旧代码误用 0x5F8/0x5F4，GetLocaleInfoW 必然失败，



        //   导致无 config 时永远回退 zh-CN，无法跟随系统语言）



        wchar_t szLang[8] = {}, szCountry[8] = {};



        ::GetLocaleInfoW(::GetUserDefaultUILanguage(), LOCALE_SISO639LANGNAME,



                         szLang, _countof(szLang));



        ::GetLocaleInfoW(::GetUserDefaultUILanguage(), LOCALE_SISO3166CTRYNAME,



                         szCountry, _countof(szCountry));



        if (szLang[0] && szCountry[0])



            langCode = std::wstring(szLang) + L"-" + std::wstring(szCountry);



        else



            langCode = L"zh-CN";



    }







    // 加载 DLL



    std::wstring dllPath = m_langDir + L"\\" + langCode + L".dll";



    bool bOk = LoadLanguageDll(dllPath.c_str(), langCode.c_str());



    if (!bOk)



    {



        // 回退：zh-CN



        if (langCode != L"zh-CN")



        {



            dllPath = m_langDir + L"\\zh-CN.dll";



            bOk = LoadLanguageDll(dllPath.c_str(), L"zh-CN");



        }



        if (!bOk)



        {



            m_usingFallback = true;



            m_langName = L"\u7b80\u4f53\u4e2d\u6587 (\u5185\u7f6e)";



            m_currentLang = L"zh-CN";



        }



        else



        {



            m_usingFallback = false;



        }



    }



    else



    {



        m_usingFallback = false;



    }







    m_initialized = true;



    return true;



}







const wchar_t* CStringManager::Get(const char* pszKeyUtf8)



{



    if (!pszKeyUtf8 || !pszKeyUtf8[0]) return L"";







    // 1) 尝试从 DLL 取



    if (!m_usingFallback && m_pfnGetString)



    {



        const wchar_t* p = m_pfnGetString(pszKeyUtf8);



        if (p && p[0]) return p;



    }







    // 2) 内置表



    const wchar_t* pBuilt = LookupBuiltin(pszKeyUtf8);



    if (pBuilt) return pBuilt;







    // 3) 兜底：返回 "[key]" 包裹，方便调试



    //    宽字符 printf 中 %s 需要 wchar_t*，char* key 必须用 %hs



    static thread_local std::wstring s_fallback;



    s_fallback.resize(256);
wfmt::FormatInto(&s_fallback[0], s_fallback.size(), L"[%hs]", pszKeyUtf8);
s_fallback.resize(wcslen(s_fallback.c_str()));



    return s_fallback.c_str();



}







const wchar_t* CStringManager::Get(const wchar_t* pszKeyW)



{



    if (!pszKeyW || !pszKeyW[0]) return L"";







    // 先把宽字符 key 转 UTF-8，再走统一路径



    wchar_t* pKey = const_cast<wchar_t*>(pszKeyW);



    int nChars = static_cast<int>(wcslen(pKey));



    std::vector<char> szUtf8(nChars * 2 + 2);



    int nOut = ::WideCharToMultiByte(CP_UTF8, 0, pKey, -1,



                                     &szUtf8[0], static_cast<int>(szUtf8.size()), nullptr, nullptr);



    if (nOut <= 0) return L"";



    return Get(&szUtf8[0]);



}







bool CStringManager::TryGet(const wchar_t* pszKeyW, CDuiString& out)



{



    if (!pszKeyW || !pszKeyW[0]) return false;







    // 宽字符 key 转 UTF-8



    int nChars = static_cast<int>(wcslen(pszKeyW));



    std::vector<char> szUtf8(nChars * 2 + 2);



    int nOut = ::WideCharToMultiByte(CP_UTF8, 0, pszKeyW, -1,



                                     &szUtf8[0], static_cast<int>(szUtf8.size()), nullptr, nullptr);



    if (nOut <= 0) return false;







    // 1) 语言 DLL 命中（非空）



    if (!m_usingFallback && m_pfnGetString)



    {



        const wchar_t* p = m_pfnGetString(&szUtf8[0]);



        if (p && p[0]) { out = p; return true; }



    }







    // 2) 内置 zh-CN 表命中



    const wchar_t* pBuilt = LookupBuiltin(&szUtf8[0]);



    if (pBuilt) { out = pBuilt; return true; }







    return false;



}







CDuiString CStringManager::Format(const wchar_t* pszKey, ...) const



{



    if (!pszKey || !pszKey[0]) return CDuiString();







    const wchar_t* pFmt = const_cast<CStringManager*>(this)->Get(pszKey);



    va_list ap;



    va_start(ap, pszKey);







    // DuiLib CDuiString 继承自 CStdString<wchar_t>，用 _vcwprintf_s 变参格式化



    std::wstring buf(1024, L'\0');



    int n = ::_vsnwprintf_s(&buf[0], buf.size() - 1, _TRUNCATE, pFmt, ap);



    buf.resize((n > 0) ? static_cast<size_t>(n) : wcslen(buf.c_str()));



    va_end(ap);







    CDuiString result(buf.c_str());



    return result;



}







bool CStringManager::SwitchLang(const wchar_t* pszLangCode)



{



    if (!pszLangCode || !pszLangCode[0]) return false;



    // 记录请求，实际生效需要调用者重启进程



    m_currentLang = pszLangCode;



    return true;



}







// [R50] 检查指定语言是否可用（DLL 存在或内置支持）



bool CStringManager::IsLangAvailable(const wchar_t* pszLangCode) const



{



    if (!pszLangCode || !pszLangCode[0]) return false;







    // 1) 内置 zh-CN 始终可用



    if (lstrcmpiW(pszLangCode, L"zh-CN") == 0)



        return true;







    // 2) 检查 lang/<code>.dll 是否存在



    if (m_langDir.empty()) return false;







    std::wstring dllPath = m_langDir + L"\\" + std::wstring(pszLangCode) + L".dll";



    DWORD attr = ::GetFileAttributesW(dllPath.c_str());



    if (attr == INVALID_FILE_ATTRIBUTES) return false;



    if (attr & FILE_ATTRIBUTE_DIRECTORY) return false;



    return true;



}







std::vector<CDuiString> CStringManager::ListAvailableLangs() const



{



    std::vector<CDuiString> vec;



    if (m_langDir.empty()) return vec;







    std::wstring pattern = m_langDir + L"\\*.dll";



    WIN32_FIND_DATAW fd;



    win::FindDataHandle hFind = win::find_handle(::FindFirstFileW(pattern.c_str(), &fd));   // [C++17 R11] auto FindClose



    if (!hFind) return vec;



    do



    {



        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))



        {



            std::wstring name = fd.cFileName;



            size_t dot = name.find('.');



            if (dot != std::wstring::npos)



            {



                CDuiString langCode(name.substr(0, dot).c_str());



                vec.push_back(langCode);



            }



        }



    } while (::FindNextFileW(hFind.get(), &fd));



    // [C++17 R11] FindClose handled by win::FindDataHandle destructor



    return vec;



}







} // namespace i18n

// [R01] 原 extern "C" const wchar_t* DismStringLookupW(const wchar_t*) ABI 桥随
//  FrogPlugins 插件协议（HostExports 的 DismMultiLanguage）整体剥离删除，
//  其唯一消费者已不存在。

