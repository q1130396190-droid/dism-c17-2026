#ifndef WIN8METRO_RESOURCE_H
#define WIN8METRO_RESOURCE_H

// ============================================================
// [R-B58] 清单维度 10 缺失 6「PE 原生资源形态」—— RT_MENU / DLGTEMPLATEEX
// 编入 PE 资源段所需的资源 ID。
//
// 说明：
//   * IDR_MAIN_MENU = 129 对应 Dism++ 原版 RT_MENU ID129 基线。
//     资源仅提供 PE 资源段形态（忠实度），运行期菜单仍由
//     CFrameWnd::ShowPopupMenu() 按多语言键与单选状态动态构建。
//   * IDD_LINKLIST / IDD_DUALPATH / IDD_DELCONFIRM 对应 EToolDlgKind::TD_LINKLIST /
//     EToolDlgKind::TD_DUALPATH / EToolDlgKind::TD_DELCONFIRM 三个 DuiLib 模态对话框（R-B44）。
//     资源模板仅作资源段镜像，运行期 UI 仍走 DuiLib XML。
// ============================================================

#define IDR_MAIN_MENU       129

#define IDD_LINKLIST        301
#define IDD_DUALPATH        302
#define IDD_DELCONFIRM      303

// 菜单命令 ID（独立于运行期 ShowPopupMenu 的 1100~1500 段，
// 避免与 WM_COMMAND 运行期分派冲突）
#define IDM_FILE_RELEASE    5001
#define IDM_FILE_MOUNT      5002
#define IDM_FILE_ADDPATH    5003
#define IDM_FILE_OPEN       5004
#define IDM_FILE_SAVE       5005
#define IDM_FILE_SAVEAS     5006
#define IDM_FILE_DISMISS    5007
#define IDM_RECOVER_1       5101
#define IDM_RECOVER_2       5102
#define IDM_RECOVER_3       5103
#define IDM_RECOVER_4       5104
#define IDM_RECOVER_5       5105
#define IDM_RECOVER_6       5106
#define IDM_RECOVER_7       5107
#define IDM_RECOVER_8       5108
#define IDM_OPTION_SETTINGS 5201
#define IDM_OPTION_AFTER    5202
#define IDM_HELP_1          5301
#define IDM_HELP_2          5302
#define IDM_HELP_3          5303
#define IDM_HELP_4          5304
#define IDM_HELP_5          5305
#define IDM_HELP_LOG        5306

// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：

#endif // WIN8METRO_RESOURCE_H
