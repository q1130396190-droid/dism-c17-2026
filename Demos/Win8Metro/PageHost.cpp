// ============================================================
// PageHost.cpp —— [R4] 页宿主装配与页序表
// ============================================================

#include "stdafx.h"
#include "PageHost.h"

CPageHost::CPageHost(IPageContext& ctx)
    : m_pgUnattend(ctx)   // P2：页对象只保存宿主引用，宿主窗口此时尚未就绪，不做调用
    , m_pgTasks(ctx)      // P2b：页 11 计划任务
    , m_pgAssoc(ctx)      // P2c：页 9 文件关联
    , m_pgProgram(ctx)    // P3a：程序和功能页
    , m_pgStartup(ctx)    // P3b：启动项/服务页
    , m_pgAppx(ctx)       // P4：Appx 页
    , m_pgCapability(ctx) // P4：可选功能页
    , m_pgUpdate(ctx)     // P4：Windows 更新页
    , m_pgDriver(ctx)     // P5：驱动管理页
    , m_pgFeature(ctx)    // P5：Windows 功能子视图
    , m_pgTools(ctx)      // P6a：工具箱页（进程能力在 ExternalToolService）
    , m_pgCleanup(ctx)    // P6b：系统清理页（工作线程管线在 CleanJobService）
    , m_pgOptim(ctx)      // P7：系统优化页
    , m_pgControl(ctx)    // Patch-3：控制面板页
    , m_pgIntl(ctx)       // Patch-3：区域/国际化页
    , m_pgRegClean(ctx)   // Patch-3：注册表清理页
    , m_pgWelcome(ctx)     // Patch-3：欢迎页
{
    // [R5] Windows 功能子视图不占页序（PAGE_FEATURE=7 的页表项是
    // m_pgCapability）；装配完成后把 Feature 绑给 Capability 代理，
    // 壳/他页经 PageById(PAGE_FEATURE) 的虚函数即可到达子视图。
    m_pgCapability.BindFeature(&m_pgFeature);
}

CPageBase* CPageHost::PageById(int idx)
{
    if (idx < 0 || idx >= 16) return nullptr;
    CPageBase* kPages[16] =
    {
        &m_pgCleanup,    // PAGE_CLEANUP  = 0
        &m_pgStartup,    // PAGE_STARTUP  = 1
        &m_pgAppx,       // PAGE_APPX     = 2
        &m_pgTools,      // PAGE_TOOLS    = 3
        &m_pgOptim,      // PAGE_OPTIM    = 4
        &m_pgDriver,     // PAGE_DRIVER   = 5
        &m_pgProgram,    // PAGE_PROGRAM  = 6
        &m_pgCapability, // PAGE_FEATURE  = 7（功能页壳成员即 m_pgCapability；
                         //   m_pgFeature 为其内部 Windows 功能子视图，不占页序）
        &m_pgUpdate,     // PAGE_UPDATE   = 8
        &m_pgAssoc,      // PAGE_ASSOC    = 9
        &m_pgUnattend,   // PAGE_UNATTEND = 10
        &m_pgTasks,      // PAGE_TASKS    = 11
        &m_pgControl,    // PAGE_CONTROL  = 12
        &m_pgIntl,       // PAGE_INTL     = 13
        &m_pgRegClean,   // PAGE_REGCLEAN = 14
        &m_pgWelcome     // PAGE_WELCOME  = 15
    };
    return kPages[idx];
}
// [C++17 R3] 本文件内联常量（C++17 inline variables，多 TU ODR 一致）：
