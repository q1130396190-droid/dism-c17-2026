#pragma once

// ============================================================
// PageHost.h —— [R4]/[R5] 页宿主：16 个页序页 + Windows 功能子视图的
// 唯一所有权持有者与装配点（审计台账 R4-01，替代 CFrameWnd 的
// 17 个具体页值成员）。
//
// 规则：
//   * 具体 CPageXxx 类型只允许在本文件/PageHost.cpp 出现；
//   * 核心壳 FrameWnd.h 不再 include 具体页头、不再声明 m_pgXxx 成员；
//   * 壳/页对页的标准通信只走 IPageHost::PageById + CPageBase 虚函数；
//   * [R5] R4 过渡用的 17 个具名访问器已全部删除；本类对外只剩
//     IPageHost 的页表查询。跨页协作（Capability→Feature 代理、
//     Program/Startup 跨页）走构造期装配与 HostPageHost() 虚接口。
// ============================================================

#include "IPageHost.h"

#include "Page_Unattend.h"  // P2：页 10 无人参与
#include "Page_Tasks.h"     // P2b：页 11 计划任务
#include "Page_Assoc.h"     // P2c：页 9 文件关联
#include "Page_Program.h"   // P3a：程序和功能
#include "Page_Startup.h"   // P3b：启动项/服务
#include "Page_Appx.h"      // P4：用户/预装 Appx
#include "Page_Capability.h"// P4：可选功能
#include "Page_Update.h"    // P4：Windows 更新
#include "Page_Driver.h"    // P5：驱动管理
#include "Page_Feature.h"   // P5：Windows 功能子视图（不占页序）
#include "Page_Tools.h"     // P6a：工具箱
#include "Page_Cleanup.h"   // P6b：系统清理
#include "Page_Optim.h"     // P7：系统优化
#include "Page_Control.h"   // Patch-3：控制面板
#include "Page_Intl.h"      // Patch-3：区域/国际化
#include "Page_RegClean.h"  // Patch-3：注册表清理
#include "Page_Welcome.h"   // Patch-3：欢迎页

class IPageContext;

class CPageHost : public IPageHost
{
public:
    explicit CPageHost(IPageContext& ctx);

    // IPageHost：16 页序表查询（原 CFrameWnd::PageById 的 GC-03 表）。
    // [R5] 对外唯一查询面；具体页类型与成员全部私有。
    CPageBase* PageById(int idx) override;
    // [B4-F7] EPageId 重载：字面量 PageById(PAGE_*) 匹配此重载，转发 int 版。
    CPageBase* PageById(EPageId id) { return PageById(static_cast<int>(id)); }

private:
    // 声明顺序与原 CFrameWnd 值成员一致（页构造只存 ctx 引用，无相互依赖）
    CPageUnattend   m_pgUnattend;
    CPageTasks      m_pgTasks;
    CPageAssoc      m_pgAssoc;
    CPageProgram    m_pgProgram;
    CPageStartup    m_pgStartup;
    CPageAppx       m_pgAppx;
    CPageCapability m_pgCapability;
    CPageUpdate     m_pgUpdate;
    CPageDriver     m_pgDriver;
    CPageFeature    m_pgFeature;   // Capability 内部 Windows 功能子视图，不占页序
    CPageTools      m_pgTools;
    CPageCleanup    m_pgCleanup;
    CPageOptim      m_pgOptim;
    CPageControl    m_pgControl;
    CPageIntl       m_pgIntl;
    CPageRegClean   m_pgRegClean;
    CPageWelcome    m_pgWelcome;
};
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
