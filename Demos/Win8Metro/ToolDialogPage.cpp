#include "stdafx.h"
#include "ThemeTokens.h"   // [R203] 颜色令牌（深色/浅色跟随）
// [R7] WIMHelperBridge.h / DismApiBridge.h / EsdDecryptor.h 原语已全部下沉
// ToolService（IsoImageService/WizardService），Page 不再直接 include。
#include "SafeThread.h"      // ESD 解密等工作线程异常边界
#include "BackgroundJobs.h"  // [CRASHFIX] 后台作业统一收口（SEH 边界+退出等待）
#include "ToolDialogPage.h"
#include "ToolDlgInternal.h"  // [R16][T10] tdlg:: 作业 Sink/进度窗/行单元格助手
// [R6] Service 层（零 UI 业务；头文件不依赖 UIlib）
#include "ToolService/IToolServiceHost.h"
#include "ToolService/ToolServiceFactory.h"
#include <cassert>  // [R16][T5] 构造断言（Release 下 NDEBUG 编译期移除）
#include "ToolService/LogDepGraphService.h"
#include "ToolService/ListService.h"
#include "ToolService/ConfirmService.h"
#include "ToolService/IsoMount.h"   // [R6-04] ISO 直连挂载原语（virtdisk）
#include "ToolService/IIsoImageService.h" // [R6-04] wimgapi 作业工厂
#include "ToolService/WizDiskPart.h"      // [R6-05] diskpart/ESP 原语
#include "ToolService/IWizardService.h"  // [R6-05] 向导业务（BootFix…）
#include "StringManager.h"
#include "HostInterfaces.h"   // [R129-1] static_cast<int>(ELogLevel::LOG_ERROR)/static_cast<int>(ELogLevel::LOG_WARN) 分级常量
// [R6-03] ConfigReader.h 已随详细设置下沉至 ToolService/ConfirmService.cpp；
// 本文件（Page）不再直接 include 产品配置原语（R7 分层契约）。
// [R7] ProcessRunner.h 已随 Apply/BootFix 向导下沉 ToolService/WizardService.cpp
using i18n::CStringManager;
#include <algorithm>
#include <set>       // [R-B54] IsoProbeWimArchitectures 架构集合去重
#include <cwchar>    // [R-B54] IsoParseArchFromXml wcstoul
#include <cwctype>   // [R-B79] iswdigit/iswalpha
#include <lm.h>
#include <cstdio>
#include <string>
#include <shlwapi.h>   // [R-B27] PathRemoveFileSpecW（LoadWofAdk 定位 wofadk.sys）
#include "SystemInfoService.h"    // [P0] IsUEFIBoot / 临时目录
// [R6-05g] ExternalToolService.h 已随释放作业（bootsect 解析）下沉 ToolService/WizardService.cpp
// [R7] IsoWriter.h 已随 ISO 生成作业下沉 ToolService/IsoImageService.cpp
#include <thread>
#include <winioctl.h>             // [P0] IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS
// ============================================================
// [R16][T6] 旧 CToolServiceHostBridge 转换层已取消：CFrameWnd 直接实现
// toolsvc::IToolServiceHost（std::wstring 语义），弹窗把宿主指针经
// dynamic_cast 交给服务工厂，边界转换收敛在核心壳（HostInterfaces 契约）。
// Service 层内部仍不出现 CDuiString/C*UI（R7 契约保持）。
// ============================================================






#pragma comment(lib, "netapi32.lib")



// 外部命令窗口关闭后通知弹窗解除操作锁定
// [GC-06] WM_TDLG_* / WM_TP_* 消息号已迁入 FrameWnd_Messages.h 统一注册表







// ============================================================
// 工具箱各工具弹窗实现
// ============================================================



namespace
{
    // 可点击表格行：BUTTONUP 时发出 click 通知
    class CGridRowUI : public CHorizontalLayoutUI
    {
    public:
        virtual void DoEvent(TEventUI& event)
        {
            if (event.Type == UIEVENT_BUTTONUP && m_pManager != nullptr)
                m_pManager->SendNotify(this, DUI_MSGTYPE_CLICK);
            CHorizontalLayoutUI::DoEvent(event);
        }
    };






    // [R6-05g] fmifs!FormatEx 卷格式化已随释放作业下沉 ToolService/WizardService.cpp。









    // [R6-05f] AcctRank 已随账户枚举下沉 ToolService/WizardService.cpp。
}



// CTdlgCheck 定义在头文件中仅有前向声明，这里给出完整实现
// [R16][T10] CTdlgCheck 完整定义已下沉 ToolDlgInternal.h（分组渲染器共用）



// ------------------------------------------------------------
// 基础
// ------------------------------------------------------------


CToolDialogPage::CToolDialogPage(IToolHost* pHost, EToolDlgKind nKind)
    : m_pHost(pHost), m_nKind(nKind),
      m_nDPI(96), m_cx(800), m_cy(600),
      m_pRoot(nullptr), m_pBody(nullptr), m_pGrid(nullptr), m_nSel(-1),
      m_bTaskRunning(false),
      m_bTaskMarquee(false), m_bLogViewLoaded(false),
      m_bLogFollow(true), m_nLogFilter(0), m_nMarqueePos(0), m_bMarqueeDir(true),
      m_pTaskMask(nullptr), m_pTaskStage(nullptr), m_pTaskBar(nullptr), m_pTaskPct(nullptr)
{
    // [R16][T5] 工厂按 kind 装配服务束（惰性）：弹窗业务服务 + 仅需要
    // 重型服务的 kind 才创建 Iso/Wizard，取代旧"任何 kind 无条件创建"。
    // [R16][T6] 宿主（CFrameWnd）直接实现双接口：非拥有指针，无桥对象。
    toolsvc::IToolServiceHost* pSvcHost = dynamic_cast<toolsvc::IToolServiceHost*>(pHost);
    toolsvc::TServiceBundle bundle = toolsvc::CreateToolServices(static_cast<int>(m_nKind), pSvcHost);
    m_pSvc    = std::move(bundle.pSvc);
    m_pImgSvc = std::move(bundle.pImg);   // [R6-04] ISO 挂载槽（0=Apply/1=ImageX）
    m_pWizSvc = std::move(bundle.pWiz);   // [R6-05] 六大向导业务


    // [R16][T5] 构造断言：无映像/向导语义的 kind 不得持有对应重型服务。
    {
        const bool bImageKind = (m_nKind == EToolDlgKind::TD_CAPTURE || m_nKind == EToolDlgKind::TD_APPLY ||
                                 m_nKind == EToolDlgKind::TD_IMAGEX || m_nKind == EToolDlgKind::TD_ISO ||
                                 m_nKind == EToolDlgKind::TD_ESDWIZ  || m_nKind == EToolDlgKind::TD_MOUNT ||
                                 m_nKind == EToolDlgKind::TD_PECFG);
        const bool bWizKind   = (m_nKind == EToolDlgKind::TD_ACCOUNT || m_nKind == EToolDlgKind::TD_BOOTFIX ||
                                 m_nKind == EToolDlgKind::TD_APPLY   || m_nKind == EToolDlgKind::TD_MOUNT ||
                                 m_nKind == EToolDlgKind::TD_PECFG);
        assert(!m_pImgSvc || bImageKind);
        assert(!m_pWizSvc || bWizKind);
    }
}
// [R-B80] 弹窗关闭：若 ImageX 透明挂载过 ISO，此处卸载释放盘符
CToolDialogPage::~CToolDialogPage()
{
    // [R6-04] ISO 挂载所有权在 IIsoImageService（槽位 0=Apply/1=ImageX）
    if (m_pImgSvc)
        m_pImgSvc->CloseIso();
    // [R16][T4] 统一服务生命周期：业务服务析构前 Close
    // （无资源的服务走 IToolService 默认空实现）。
    if (m_pSvc)
        m_pSvc->Close();
    // [B4] 内嵌任务进度面板随窗口管理器一并销毁，无需兜底；杀定时器即可
    if (::IsWindow(m_hWnd))
        ::KillTimer(m_hWnd, TASK_ANIM_TIMER_ID);
    // [实时刷新] 跟随定时器必须在窗口销毁前停掉：否则窗口回收后仍有
    //   WM_TIMER 投递到已失效的 HWND，回调里 FindControl 会拿到空管理器。
    if (::IsWindow(m_hWnd))
    {
        ::KillTimer(m_hWnd, LOGVIEW_FOLLOW_TIMER_ID);
        ::KillTimer(m_hWnd, LOGVIEW_LOAD_TIMER_ID);
    }
    // [R6] 释放服务层：业务服务由 m_pSvc(unique_ptr) 拥有，先 reset 再
    // 释放宿主桥；两个具体别名不拥有对象，仅置空。
    m_pSvc.reset();
    m_pWizSvc.reset();   // [R6-05] 先于映像服务释放（依赖它）
    m_pImgSvc.reset();   // [R6-04]
}


// [R16][T4] 统一入参注入通路：宿主 Create 前组装 ToolOpenArgs 调用本方法，
// 转发给拥有型业务服务；kind 无业务服务（ESDWIZ/PECFG）时由映像服务持有数据。
// [R16][T10] 服务别名成员已删除：按需向下转换（非对应 kind 得 nullptr，与原别名行为一致）
toolsvc::CLogDepGraphService* CToolDialogPage::SvcLogDep() const
{
    return static_cast<toolsvc::CLogDepGraphService*>(m_pSvc.get());
}
toolsvc::CListService* CToolDialogPage::SvcList() const
{
    return static_cast<toolsvc::CListService*>(m_pSvc.get());
}
toolsvc::CConfirmService* CToolDialogPage::SvcConfirm() const
{
    return static_cast<toolsvc::CConfirmService*>(m_pSvc.get());
}
void CToolDialogPage::Open(const toolsvc::ToolOpenArgs& args)
{
    if (m_pSvc)    m_pSvc->Open(args);
    if (m_pImgSvc) m_pImgSvc->Open(args);
}


const wchar_t* CToolDialogPage::GetWindowClassName() const
{
    return L"ToolDialogWnd";
}



[[nodiscard]] CDuiString CToolDialogPage::GetSkinFile() { return L"ToolDialog.xml"; }
[[nodiscard]] CDuiString CToolDialogPage::GetSkinFolder() { return L""; }



// ============================================================
// [GC-02] 弹窗 kind 分发表
// 原先新增一种弹窗要同步改 InitWindow(尺寸/首焦点/标题)、
// BuildContent、OnClickName 共 4+ 个 switch；现在数据查表、
// Build/Click 行为经成员函数指针分发，新增弹窗只需：
//   1) 加 BuildXxx() + ClickXxx()；2) 在下列表中各加一行。
// ============================================================
// 表为 CToolDialogPage 私有静态成员（声明见 ToolDialogPage.h），使其能够引用
// 同类的私有 Build*/Click* 成员函数指针。
const CToolDialogPage::TKindSizeEnt CToolDialogPage::kKindSizeTab[15] =
{
    { EToolDlgKind::TD_CAPTURE,   820, 628 },
    { EToolDlgKind::TD_APPLY,     880, 738 },
    { EToolDlgKind::TD_ACCOUNT,   820, 536 },
    { EToolDlgKind::TD_IMAGEX,    980, 574 },
    { EToolDlgKind::TD_ISO,       820, 400 },
    { EToolDlgKind::TD_DETAILS,   720, 500 },
    { EToolDlgKind::TD_LOGVIEW,   880, 600 },
    { EToolDlgKind::TD_DEPGRAPH,  780, 560 },
    { EToolDlgKind::TD_ESDWIZ,    860, 600 },
    { EToolDlgKind::TD_LINKLIST,  780, 480 },
    { EToolDlgKind::TD_DUALPATH,  620, 320 },
    { EToolDlgKind::TD_DELCONFIRM,520, 420 },
    { EToolDlgKind::TD_MOUNT,     840, 580 },
    { EToolDlgKind::TD_PECFG,     640, 360 },   // [R-B79]
    { EToolDlgKind::TD_BOOTFIX,   760, 580 },   // 引导修复
};
const CToolDialogPage::TKindEditEnt CToolDialogPage::kKindFirstEditTab[7] =
{
    { EToolDlgKind::TD_CAPTURE, L"edt_cap_path"   },
    { EToolDlgKind::TD_APPLY,   L"edt_apply_wim"  },
    { EToolDlgKind::TD_IMAGEX,  L"edt_imgx_path"  },
    { EToolDlgKind::TD_ISO,     L"edt_iso_src"    },
    { EToolDlgKind::TD_ESDWIZ,  L"edt_esdwiz_src" },
    { EToolDlgKind::TD_MOUNT,   L"edt_mount_wim"  },
    { EToolDlgKind::TD_PECFG,   L"edt_pecfg_dir"  },   // [R-B79]
};
// 回车键对应的"确定"按钮（EToolDlgKind::TD_IMAGEX 随元数据态变化，在 HandleMessage 单独处理）
const CToolDialogPage::TKindEditEnt CToolDialogPage::kKindOkBtnTab[5] =
{
    { EToolDlgKind::TD_CAPTURE, L"btn_cap_ok"       },
    { EToolDlgKind::TD_APPLY,   L"btn_apply_ok"     },
    { EToolDlgKind::TD_ISO,     L"btn_iso_ok"       },
    { EToolDlgKind::TD_ESDWIZ,  L"btn_esdwiz_export"},
    { EToolDlgKind::TD_BOOTFIX, L"btn_bootfix_ok"   },
};
const CToolDialogPage::TKindTitleEnt CToolDialogPage::kKindTitleTab[11] =
{
    { EToolDlgKind::TD_CAPTURE,  L"toolbox.save_as",      false },
    { EToolDlgKind::TD_APPLY,    L"cleanup.release_img",  false },
    { EToolDlgKind::TD_ACCOUNT,  L"toolbox.account",      false },
    { EToolDlgKind::TD_IMAGEX,   L"toolbox.edit_img",     false },
    { EToolDlgKind::TD_ISO,      L"toolbox.isogen",       false },
    { EToolDlgKind::TD_DETAILS,  L"misc.s5f637bdc",       false },
    { EToolDlgKind::TD_LOGVIEW,  L"misc.logview_title",   false },
    { EToolDlgKind::TD_DEPGRAPH, L"misc.depgraph_title",  false },
    { EToolDlgKind::TD_ESDWIZ,   L"esdwiz.title",         false },
    { EToolDlgKind::TD_MOUNT,    L"mount.title",          false },
    { EToolDlgKind::TD_PECFG,    L"PE 映像设置",           true  }, // [R-B79]
    // EToolDlgKind::TD_BOOTFIX 标题随 UEFI/BIOS 动态决定（BuildChrome 内处理）
    // EToolDlgKind::TD_LINKLIST/EToolDlgKind::TD_DUALPATH/EToolDlgKind::TD_DELCONFIRM 保持空标题（原 default 行为）
};
const CToolDialogPage::TKindBuildEnt CToolDialogPage::kKindBuildTab[15] =
{
    { EToolDlgKind::TD_CAPTURE,   &CToolDialogPage::BuildCapture    },
    { EToolDlgKind::TD_APPLY,     &CToolDialogPage::BuildApply      },
    { EToolDlgKind::TD_ACCOUNT,   &CToolDialogPage::BuildAccount    },
    { EToolDlgKind::TD_IMAGEX,    &CToolDialogPage::BuildImageX     },
    { EToolDlgKind::TD_ISO,       &CToolDialogPage::BuildIso        },
    { EToolDlgKind::TD_DETAILS,   &CToolDialogPage::BuildDetails    },
    { EToolDlgKind::TD_LOGVIEW,   &CToolDialogPage::BuildLogView    },
    { EToolDlgKind::TD_DEPGRAPH,  &CToolDialogPage::BuildDepGraph   },
    { EToolDlgKind::TD_ESDWIZ,    &CToolDialogPage::BuildEsdWiz     },
    { EToolDlgKind::TD_LINKLIST,  &CToolDialogPage::BuildLinkList   },
    { EToolDlgKind::TD_DUALPATH,  &CToolDialogPage::BuildDualPath   },
    { EToolDlgKind::TD_DELCONFIRM,&CToolDialogPage::BuildDelConfirm },
    { EToolDlgKind::TD_MOUNT,     &CToolDialogPage::BuildMount      },
    { EToolDlgKind::TD_PECFG,     &CToolDialogPage::BuildPeCfg      },   // [R-B79]
    { EToolDlgKind::TD_BOOTFIX,   &CToolDialogPage::BuildBootFix    },   // 引导修复
};
const CToolDialogPage::TKindClickEnt CToolDialogPage::kKindClickTab[15] =
{
    { EToolDlgKind::TD_CAPTURE,   &CToolDialogPage::ClickCapture    },
    { EToolDlgKind::TD_APPLY,     &CToolDialogPage::ClickApply      },
    { EToolDlgKind::TD_ACCOUNT,   &CToolDialogPage::ClickAccount    },
    { EToolDlgKind::TD_IMAGEX,    &CToolDialogPage::ClickImageX     },
    { EToolDlgKind::TD_ISO,       &CToolDialogPage::ClickIso        },
    { EToolDlgKind::TD_DETAILS,   &CToolDialogPage::ClickDetails    },
    { EToolDlgKind::TD_LOGVIEW,   &CToolDialogPage::ClickLogView    },
    { EToolDlgKind::TD_DEPGRAPH,  &CToolDialogPage::ClickDepGraph   },
    { EToolDlgKind::TD_ESDWIZ,    &CToolDialogPage::ClickEsdWiz     },
    { EToolDlgKind::TD_LINKLIST,  &CToolDialogPage::ClickLinkList   },
    { EToolDlgKind::TD_DUALPATH,  &CToolDialogPage::ClickDualPath   },
    { EToolDlgKind::TD_DELCONFIRM,&CToolDialogPage::ClickDelConfirm },
    { EToolDlgKind::TD_MOUNT,     &CToolDialogPage::ClickMount      },
    { EToolDlgKind::TD_PECFG,     &CToolDialogPage::ClickPeCfg      },   // [R-B79]
    { EToolDlgKind::TD_BOOTFIX,   &CToolDialogPage::ClickBootFix    },   // 引导修复
};
void CToolDialogPage::InitWindow()
{
    // [GC-02] 窗口尺寸查表（未命中保持构造默认值，与原 default 行为一致）
    for (int i = 0; i < static_cast<int>(sizeof(kKindSizeTab)/sizeof(kKindSizeTab[0])); ++i)
    {
        if (kKindSizeTab[i].nKind == m_nKind)
        {
            m_cx = kKindSizeTab[i].cx;
            m_cy = kKindSizeTab[i].cy;
            break;
        }
    }



    m_pRoot = static_cast<CVerticalLayoutUI*>(m_pm.FindControl(L"dlg_root"));
    if (!m_pRoot) return;
    m_pRoot->RemoveAll();



    BuildChrome();
    BuildBodyShell();
    BuildContent();
    BuildTaskPanel();   // [B4] 内嵌任务进度浮层（默认隐藏，必须在 ResizeClient 前入树）



    // 先按 96 逻辑尺寸落地，再整体切到宿主 DPI（SetDPI 会等比放大窗口、
    // 重建字体，各控件固定尺寸在读取时经 CDPI 缩放，内容与窗口始终一致）
    ResizeClient(m_cx, m_cy);
    if (m_nDPI != 96) m_pm.SetDPI(m_nDPI);
    CenterWindow();



    // [C5] 初始焦点落到首个输入框，开箱即可输入路径
    const wchar_t* pszFirstEdit = nullptr;
    // [GC-02] 首个输入框查表
    for (int i = 0; i < static_cast<int>(sizeof(kKindFirstEditTab)/sizeof(kKindFirstEditTab[0])); ++i)
    {
        if (kKindFirstEditTab[i].nKind == m_nKind)
        {
            pszFirstEdit = kKindFirstEditTab[i].psz;
            break;
        }
    }
    if (pszFirstEdit)
    {
        if (CEditUI* pEdit = static_cast<CEditUI*>(m_pm.FindControl(pszFirstEdit)))
            m_pm.SetFocus(pEdit);
    }

    // 查看日志：把正文加载推迟到消息循环启动之后。
    //   构建期同步读盘 + RichEdit 排版会让模态窗口迟迟不出现（日志上千行时
    //   实测 >40s 都建不出窗口）；这里只挂一个一次性定时器，窗口先画出来。
    if (m_nKind == EToolDlgKind::TD_LOGVIEW && !m_bLogViewLoaded)
        ::SetTimer(m_hWnd, LOGVIEW_LOAD_TIMER_ID, 1, nullptr);
    // [实时刷新] 日志查看器默认开启自动刷新：日志是"看当下发生了什么"的工具，
    //   要求用户先手动点"跟随"才开始更新，等于默认给了一份会过期的快照。
    //   这里在窗口初始化时就把跟随定时器挂上，加载完成后即刻进入刷新循环；
    //   用户仍可点按钮关掉（关掉后不再重读，避免大文件时无谓 IO）。
    if (m_nKind == EToolDlgKind::TD_LOGVIEW)
    {
        m_bLogFollow = true;
        ::SetTimer(m_hWnd, LOGVIEW_FOLLOW_TIMER_ID, 700, nullptr);
    }
}



LRESULT CToolDialogPage::HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    // [C5] 键盘：Esc 关闭（任务执行中锁定，防止半路关窗留下 cmd 作业）；
    // 回车触发当前页“确定”按钮（与单击等价，走 OnClickName 的防重入判断）。
    if (uMsg == WM_KEYDOWN && wParam == VK_ESCAPE)
    {
        if (!m_bTaskRunning) Close(0);
        return 0;
    }
    if (uMsg == WM_KEYDOWN && wParam == VK_RETURN && !m_bTaskRunning)
    {
        // [R16][T10] 回车确定逻辑机械抽取为 TryEnterOk（行为不变）
        if (TryEnterOk()) return 0;
    }



    // 跨显示器拖动时跟随新显示器 DPI（0x02E0 == WM_DPICHANGED）
    if (uMsg == 0x02E0)
    {
        m_pm.SetDPI(LOWORD(wParam));
        return 0;
    }
    // 查看日志：正文区右键弹自绘菜单（复制选中 / 全选 / 刷新）。
    // RichEdit 内置菜单被 SetContextMenuUsed(false) 关掉，这里按控件命中判定，
    // 只在指针确实落在 logview_text 上才接管，其它区域交回默认处理。
    if (uMsg == WM_RBUTTONUP && m_nKind == EToolDlgKind::TD_LOGVIEW)
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        CControlUI* pHit = m_pm.FindControl(pt);
        if (pHit && pHit->GetName() == L"logview_text")
        {
            POINT ptScreen = pt;
            ::ClientToScreen(m_hWnd, &ptScreen);
            LogViewContextMenu(ptScreen);
            return 0;
        }
    }
    if (uMsg == WM_TDLG_TASKDONE)
    {
        // [FIX-T2-5] WIM 挂载/卸载后台完成：解遮罩并按语义收口。
        //   3=挂载成功 → 刷新已挂载列表；4=挂载/卸载失败 → 提示；
        //   5=卸载完成 → 刷新已挂载列表。
        if (wParam == 3 || wParam == 5)
        {
            m_bMountBusy = false;
            EndTaskShield();
            if (m_nKind == EToolDlgKind::TD_MOUNT) RefreshMountedList();
            return 0;
        }
        if (wParam == 4)
        {
            m_bMountBusy = false;
            EndTaskShield();
            if (m_nKind == EToolDlgKind::TD_MOUNT)
                ::MessageBoxW(m_hWnd, StringMgr.Get("mount.s_badmount"),
                              StringMgr.Get("mount.title"), MB_OK | MB_ICONWARNING);
            return 0;
        }
        // [FIX-P2M] DetailsOk 后台应用完成（wParam=2）：解遮罩并按原语义
        // 以 IDOK 关闭对话框（ApplyAdvanced 已在后台线程跑完）。
        if (wParam == 2)
        {
            m_bAdvancedApplying = false;
            EndTaskShield();
            Close(IDOK);
            return 0;
        }
        // 遮罩已在 EndTaskShield 内解除；这里只做弹窗类型相关的收尾刷新。
        // 编辑映像：删除/添加/可启动均为重建类操作，原生 wimgapi 成功后自动重读真实状态
        if (m_nKind == EToolDlgKind::TD_IMAGEX && m_imgx.bDirty)
        {
            m_imgx.bDirty = false;
            if (!m_imgx.bMeta && !m_imgx.sFile.IsEmpty())
                ImageXReload(true);
        }
        return 0;
    }
    if (uMsg == WM_TDLG_ESDDONE)
    {
        OnEsdTaskDone(reinterpret_cast<void*>(lParam));
        return 0;
    }
    if (uMsg == WM_TDLG_JOBDONE)
    {
        OnJobDone(lParam);
        return 0;
    }
    if (uMsg == WM_TDLG_JOBLOG)
    {
        // 后台任务日志：AppendLogEx 非线程安全，统一在 UI 线程入队
        std::unique_ptr<CDuiString> guard(reinterpret_cast<CDuiString*>(lParam)); // [C++17 R3] ownership from poster
        if (guard)
        {
            m_pHost->HostLog(*guard, static_cast<int>(wParam));
            // [C++17 R3] guard deletes at scope exit (was: explicit delete)
        }
        return 0;
    }
    // [B4] 工作线程 → 内嵌任务进度面板（原投递给独立原生进度窗）
    if (uMsg == WM_TP_PROGRESS)
    {
        if (!m_bTaskMarquee) SetTaskProgress(static_cast<int>(wParam));
        return 0;
    }
    if (uMsg == WM_TP_STAGETEXT)
    {
        std::unique_ptr<CDuiString> guard(reinterpret_cast<CDuiString*>(lParam)); // [C++17 R3] ownership from poster
        if (guard)
        {
            SetTaskStage(guard->GetData());
            // [C++17 R3] guard deletes at scope exit (was: explicit delete)
        }
        return 0;
    }
    if (uMsg == WM_TIMER && wParam == LOGVIEW_LOAD_TIMER_ID)
    {
        // 一次性：先杀掉定时器再加载，避免重复触发。
        ::KillTimer(m_hWnd, LOGVIEW_LOAD_TIMER_ID);
        if (!m_bLogViewLoaded)
        {
            m_bLogViewLoaded = true;
            // [R-LOGV-3] 先把"正在加载"那帧真正画出来，再开始读盘+排版：
            //   否则 SetText 与首次 WM_PAINT 撞在一起，用户看到的仍是空白，
            //   加载感完全丢失。UpdateWindow 只做同步重绘、不发新消息。
            ::UpdateWindow(m_hWnd);
            LogViewRefresh();
        }
        return 0;
    }
    if (uMsg == WM_TIMER && wParam == LOGVIEW_FOLLOW_TIMER_ID)
    {
        // [L-10] 自动跟随：只重读文件并重填，间隔 700ms。
        //   不在这里做"增量读"（记文件偏移）——写入侧 1MB 轮转会让偏移失效，
        //   而且一次 1MB 解析在本地是毫秒级，够用且没有状态一致性问题。
        if (m_bLogFollow && m_bLogViewLoaded && m_nKind == EToolDlgKind::TD_LOGVIEW)
            LogViewRefresh();
        return 0;
    }
    if (uMsg == WM_TIMER && wParam == TASK_ANIM_TIMER_ID)
    {
        // marquee 不确定进度：0..100..0 往复扫描，明确表达"正在忙"
        if (m_bTaskRunning && m_bTaskMarquee && m_pTaskBar)
        {
            m_nMarqueePos += 2;
            if (m_nMarqueePos >= 100) { m_nMarqueePos = 100; m_bMarqueeDir = false; }
            if (!m_bMarqueeDir)
            {
                m_nMarqueePos -= 4;
                if (m_nMarqueePos <= 0) { m_nMarqueePos = 0; m_bMarqueeDir = true; }
            }
            m_pTaskBar->SetValue(m_nMarqueePos);
        }
        return 0;
    }
    return WindowImplBase::HandleMessage(uMsg, wParam, lParam);
}



// [R16][T10] 自 HandleMessage 机械抽取：回车触发当前页"确定"按钮，
// 命中且按钮可见可用时经 OnClickName 分发（含防重入判断），返回 true。
[[nodiscard]] bool CToolDialogPage::TryEnterOk()
{
    const wchar_t* pszOk = nullptr;
    // [GC-02] 回车确定按钮查表；IMAGEX 随元数据态动态切换，单独处理
    if (m_nKind == EToolDlgKind::TD_IMAGEX)
    {
        pszOk = m_imgx.bMeta ? L"btn_meta_ok" : nullptr;
    }
    else
    {
        for (int i = 0; i < static_cast<int>(sizeof(kKindOkBtnTab)/sizeof(kKindOkBtnTab[0])); ++i)
        {
            if (kKindOkBtnTab[i].nKind == m_nKind)
            {
                pszOk = kKindOkBtnTab[i].psz;
                break;
            }
        }
    }
    if (!pszOk) return false;
    CControlUI* pBtn = m_pm.FindControl(pszOk);
    if (pBtn && pBtn->IsVisible() && pBtn->IsEnabled())
    {
        OnClickName(pszOk);
        return true;
    }
    return false;
}






void CToolDialogPage::BuildChrome()
{
    const wchar_t* pszTitle = L"";
    // [GC-02] 标题查表（bLiteral=true 直接用字面量，其余走多语言键）
    for (int i = 0; i < static_cast<int>(sizeof(kKindTitleTab)/sizeof(kKindTitleTab[0])); ++i)
    {
        if (kKindTitleTab[i].nKind == m_nKind)
        {
            pszTitle = kKindTitleTab[i].bLiteral
                       ? kKindTitleTab[i].psz
                       : StringMgr.Get(kKindTitleTab[i].psz);
            break;
        }
    }
    // EToolDlgKind::TD_BOOTFIX 标题随启动方式动态决定
    if (m_nKind == EToolDlgKind::TD_BOOTFIX)
        pszTitle = SystemInfoService::IsUEFIBoot()
                   ? L"引导修复-UEFI" : L"引导修复-BIOS";



    CHorizontalLayoutUI* pBar = std::make_unique<CHorizontalLayoutUI>().release(); // [C++17 R3] framework-owned via m_pRoot->Add
    pBar->SetFixedHeight(50);
    pBar->SetBkColor(tokens::Token(tokens::EToken::SurfaceAlt));   // [R234] 米色工具条底 -> 令牌



    CTextUI* pTitle = MkText(pszTitle, 3, tokens::Token(tokens::EToken::Ink));   // [R235] 原 0xFF1A1A1A 深色下不可见
    RECT rcPad = { 22, 0, 0, 0 };
    pTitle->SetTextPadding(rcPad);
    pTitle->SetMouseEnabled(false);
    pBar->Add(pTitle);



    CControlUI* pSp = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via pBar->Add
    pBar->Add(pSp);



    CButtonUI* pX = std::make_unique<CButtonUI>().release(); // [C++17 R3] framework-owned via pBar->Add
    pX->SetAttribute(L"borderround", L"4,4");   // Win11 Fluent control radius
    pX->SetName(L"btn_close");
    pX->SetText(L"\u2715");
    pX->SetFont(3);
    pX->SetTextColor(tokens::Token(tokens::EToken::InkSecondary));
    pX->SetFixedWidth(64);
    pX->SetBkColor(0x00000000);   // fully transparent, deliberately not a theme colour
    pX->SetAttribute(L"hotbkcolor", tokens::TokenHex(tokens::EToken::Danger));   // [R234] 与 closebtn 一致
    pX->SetAttribute(L"hottextcolor", tokens::TokenHex(tokens::EToken::OnDanger));
    pBar->Add(pX);



    m_pRoot->Add(pBar);
}



void CToolDialogPage::BuildBodyShell()
{
    m_pBody = std::make_unique<CVerticalLayoutUI>().release(); // [C++17 R3] framework-owned via m_pRoot->Add
    m_pBody->SetAttribute(L"inset", L"22,16,22,0");
    m_pRoot->Add(m_pBody);
}



void CToolDialogPage::BuildContent()
{
    // [GC-02] 15-case 构建分发收敛为分发表一行查询
    for (int i = 0; i < static_cast<int>(sizeof(kKindBuildTab)/sizeof(kKindBuildTab[0])); ++i)
    {
        if (kKindBuildTab[i].nKind == m_nKind)
        {
            (this->*kKindBuildTab[i].pfnBuild)();
            return;
        }
    }
}



// ------------------------------------------------------------
// 控件工厂
// ------------------------------------------------------------
CTextUI* CToolDialogPage::MkText(const wchar_t* pszText, int nFont, DWORD dwColor)
{
    CTextUI* p = std::make_unique<CTextUI>().release(); // [C++17 R3] framework-owned via caller's container Add
    p->SetText(pszText);
    p->SetFont(nFont);
    p->SetTextColor(dwColor);
    p->SetAttribute(L"valign", L"vcenter");
    return p;
}



CButtonUI* CToolDialogPage::MkBtn(const wchar_t* pszText, const wchar_t* pszName, bool bPrimary)
{
    CButtonUI* p = std::make_unique<CButtonUI>().release(); // [C++17 R3] framework-owned via caller's container Add
    p->SetAttribute(L"borderround", L"4,4");   // Win11 Fluent control radius
    p->SetName(pszName);
    p->SetText(pszText);
    p->SetFont(1);
    p->SetFixedHeight(38);
    RECT rc = { 28, 0, 28, 0 };
    p->SetTextPadding(rc);
    p->SetBkColor(tokens::Token(tokens::EToken::Surface));
    p->SetAttribute(L"bordercolor",
                    bPrimary ? tokens::TokenHex(tokens::EToken::Accent) : tokens::TokenHex(tokens::EToken::Border));
    p->SetAttribute(L"bordersize", L"1");
    p->SetTextColor(tokens::Token(tokens::EToken::Ink));
    p->SetAttribute(L"hotbkcolor", tokens::TokenHex(tokens::EToken::SurfaceHover));   // [R234]
    p->SetAttribute(L"pushedbkcolor", tokens::TokenHex(tokens::EToken::AccentSoft));   // [R234]
    return p;
}



CEditUI* CToolDialogPage::MkEdit(const wchar_t* pszName)
{
    CEditUI* p = std::make_unique<CEditUI>().release(); // [C++17 R3] framework-owned via caller's container Add
    p->SetName(pszName);
    p->SetFont(1);
    p->SetTextColor(tokens::Token(tokens::EToken::Ink));
    p->SetBkColor(tokens::Token(tokens::EToken::Surface));
    RECT rc = { 10, 7, 10, 7 };
    p->SetTextPadding(rc);
    p->SetAttribute(L"bordercolor", tokens::TokenHex(tokens::EToken::Border));
    p->SetAttribute(L"bordersize", L"1");
    p->SetFixedHeight(36);
    return p;
}



CComboUI* CToolDialogPage::MkCombo(const wchar_t* pszName, int nDropH)
{
    CComboUI* p = std::make_unique<CComboUI>().release(); // [C++17 R3] framework-owned via caller's container Add
    p->SetName(pszName);
    p->SetFont(1);
    p->SetTextColor(tokens::Token(tokens::EToken::Ink));
    RECT rc = { 10, 0, 26, 0 };
    p->SetTextPadding(rc);
    SIZE sz = { 0, nDropH };
    p->SetDropBoxSize(sz);
    p->SetBkColor(tokens::Token(tokens::EToken::Surface));
    p->SetFixedHeight(36);
    p->SetAttribute(L"bordercolor", tokens::TokenHex(tokens::EToken::Border));
    p->SetAttribute(L"bordersize", L"1");
    p->SetAttribute(L"itemtextcolor", tokens::TokenHex(tokens::EToken::Ink));
    p->SetAttribute(L"itembkcolor", tokens::TokenHex(tokens::EToken::Surface));           // [R236] 原 #FFFFFFFF 深色下白块
    p->SetAttribute(L"itemselectedtextcolor", tokens::TokenHex(tokens::EToken::OnAccent)); // [R236] 选中 Accent 底 → OnAccent 字
    p->SetAttribute(L"itemselectedbkcolor", tokens::TokenHex(tokens::EToken::Accent));   // [R210]
    p->SetAttribute(L"itemhotbkcolor", tokens::TokenHex(tokens::EToken::SurfaceHover));   // [R234] 下拉项悬停底
    p->SetAttribute(L"itemfont", L"1");
    p->SetAttribute(L"itemalign", L"left");
    p->SetAttribute(L"itemvalign", L"vcenter");
    p->SetAttribute(L"itemtextpadding", L"10,5,10,5");
    return p;
}



CVerticalLayoutUI* CToolDialogPage::MkGrid(const std::vector<TGridCol>& cols,
                                       int nTotalRows, int nRowH,
                                       bool bSelectable)
{
    m_pGrid = std::make_unique<CVerticalLayoutUI>().release(); // [C++17 R3] framework-owned via caller m_pBody->Add(MkGrid(...))
    m_pGrid->SetBkColor(tokens::Token(tokens::EToken::Surface));
    m_pGrid->SetAttribute(L"bordercolor", tokens::TokenHex(tokens::EToken::Border));
    m_pGrid->SetAttribute(L"bordersize", L"1");
    m_pGrid->SetAttribute(L"vscrollbar", L"true");



    CHorizontalLayoutUI* pHdr = std::make_unique<CHorizontalLayoutUI>().release(); // [C++17 R3] framework-owned via m_pGrid->Add
    pHdr->SetFixedHeight(nRowH);
    pHdr->SetBkColor(tokens::Token(tokens::EToken::SurfaceAlt));   // [R234] 表头底 -> 令牌
    for (int c = 0; c < static_cast<int>(cols.size()); ++c)
    {
        CTextUI* pCell = MkText(cols[c].pszTitle, 2, tokens::Token(tokens::EToken::Ink));   // [R235] 原 0xFF1A1A1A
        if (cols[c].nWidth >= 0) pCell->SetFixedWidth(cols[c].nWidth);
        RECT rc = { 12, 0, 12, 0 };
        pCell->SetTextPadding(rc);
        bool bLast = (c + 1 == static_cast<int>(cols.size()));
        pCell->SetAttribute(L"bordercolor", tokens::TokenHex(tokens::EToken::Border));
        pCell->SetAttribute(L"bordersize", bLast ? L"0,0,1,0"
                                                     : L"0,0,1,1");
        pCell->SetMouseEnabled(false);
        pHdr->Add(pCell);
    }
    m_pGrid->Add(pHdr);



    m_vRows.clear();
    for (int r = 0; r < nTotalRows; ++r)
    {
        CHorizontalLayoutUI* pRow = bSelectable
            ? static_cast<CHorizontalLayoutUI*>(std::make_unique<CGridRowUI>().release())
            : std::make_unique<CHorizontalLayoutUI>().release(); // [C++17 R3] framework-owned via m_pGrid->Add
        pRow->SetFixedHeight(nRowH);
        pRow->SetBkColor(tokens::Token(tokens::EToken::Surface));
        CDuiString sN;
        sN.Format(L"gridrow_%d", r);
        pRow->SetName(sN);



        for (int c = 0; c < static_cast<int>(cols.size()); ++c)
        {
            CTextUI* pCell = MkText(L"", 1, tokens::Token(tokens::EToken::Ink));   // [R235] 原 0xFF1A1A1A
            if (cols[c].nWidth >= 0) pCell->SetFixedWidth(cols[c].nWidth);
            RECT rc = { 12, 0, 12, 0 };
            pCell->SetTextPadding(rc);
            bool bLast = (c + 1 == static_cast<int>(cols.size()));
            pCell->SetAttribute(L"bordercolor", tokens::TokenHex(tokens::EToken::BorderSoft));
            pCell->SetAttribute(L"bordersize", bLast ? L"0,0,1,0"
                                                         : L"0,0,1,1");
            pCell->SetMouseEnabled(false);
            pRow->Add(pCell);
        }
        m_pGrid->Add(pRow);
        m_vRows.push_back(pRow);
    }



    m_nSel = -1;
    return m_pGrid;
}



void CToolDialogPage::AddBottomRow(CButtonUI* pOk, CButtonUI* pCancel)
{
    CHorizontalLayoutUI* pRow = std::make_unique<CHorizontalLayoutUI>().release(); // [C++17 R3] framework-owned via m_pBody->Add
    pRow->SetFixedHeight(74);
    pRow->SetAttribute(L"inset", L"0,18,0,18");



    CControlUI* pSp = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via pRow->Add
    pRow->Add(pSp);



    pOk->SetFixedWidth(130);
    pRow->Add(pOk);



    CControlUI* pGap = std::make_unique<CControlUI>().release(); // [C++17 R3] framework-owned via pRow->Add
    pGap->SetFixedWidth(14);
    pRow->Add(pGap);



    pCancel->SetFixedWidth(130);
    pRow->Add(pCancel);



    m_pBody->Add(pRow);
}



void CToolDialogPage::SetGridSelection(int nRow)
{
    for (int i = 0; i < static_cast<int>(m_vRows.size()); ++i)
    {
        // [R235] 原 (选中 ? 0xFFD6EAF7 : 0xFFFFFFFF)：深色模式下普通行是纯白底、
        //   选中行是浅蓝底，都压在深色窗口上刺眼；跟随 Surface/AccentSoft 才对。
        m_vRows[i]->SetBkColor(i == nRow ? tokens::Token(tokens::EToken::AccentSoft)
                                         : tokens::Token(tokens::EToken::Surface));
        m_vRows[i]->Invalidate();
    }
    m_nSel = nRow;
}



void CToolDialogPage::OnGridRow(const CDuiString& sName)
{
    int nRow = _ttoi(sName.Mid(8).GetData());
    if (nRow < 0 || nRow >= static_cast<int>(m_vRows.size())) return;



    if (m_nKind == EToolDlgKind::TD_ACCOUNT)
    {
        if (nRow >= static_cast<int>(m_acct.vList.size())) return;
        SetGridSelection(nRow);
        if (m_acct.pEnable) m_acct.pEnable->SetEnabled(true);
        if (m_acct.pClear) m_acct.pClear->SetEnabled(true);
    }
    else if (m_nKind == EToolDlgKind::TD_IMAGEX && !m_imgx.bMeta)
    {
        if (nRow >= static_cast<int>(m_imgx.vIdx.size())) return;
        SetGridSelection(nRow);
    }
    else if (m_nKind == EToolDlgKind::TD_ESDWIZ)   // [R-B35] ESD 解密向导：选择要导出的索引
    {
        if (nRow >= static_cast<int>(m_esd.vIdx.size())) return;
        SetGridSelection(nRow);
    }
    else if (m_nKind == EToolDlgKind::TD_MOUNT)    // [R-B53] 挂载向导：选择要挂载的索引
    {
        if (nRow >= static_cast<int>(m_mount.vIdx.size())) return;
        SetGridSelection(nRow);
    }
    else if (m_nKind == EToolDlgKind::TD_BOOTFIX)  // 引导修复：选择启动分区
    {
        if (nRow >= static_cast<int>(m_bf.vParts.size())) return;
        SetGridSelection(nRow);
        m_bf.nSel = nRow;
        BfRefreshTarget();
    }
}



// ------------------------------------------------------------
// 通知路由
// ------------------------------------------------------------
void CToolDialogPage::Notify(TNotifyUI& msg)
{
    if (!msg.pSender) return;
    CDuiString sName = msg.pSender->GetName();



    if (msg.sType == DUI_MSGTYPE_SELECTCHANGED)
    {
        OnSelectChangedName(sName);
        return;
    }
    // [修复] cmb_apply_index（应用镜像索引）、cmb_esdwiz_key（ESD 密钥来源）
    // 都是 CComboUI，选中发 DUI_MSGTYPE_ITEMSELECT 而非 SELECTCHANGED；旧路由
    // 漏接，导致索引切换不刷新、密钥来源手输框不显示。cmb_mount_mounted 虽无
    // 即时事件，但框架层 SelectItem 修好后 GetCurSel 才能返回正确值。
    if (msg.sType == DUI_MSGTYPE_ITEMSELECT)
    {
        OnSelectChangedName(sName);
        return;
    }



    // [L-10] 搜索框输入：实时过滤（每次输入都重填，2000 行内是毫秒级）
    if (msg.sType == DUI_MSGTYPE_TEXTCHANGED && sName == L"edt_logview_search")
    {
        if (CEditUI* pEdit = static_cast<CEditUI*>(m_pm.FindControl(L"edt_logview_search")))
        {
            m_sLogSearch = pEdit->GetText();
            LogViewFillView();
        }
        return;
    }
    if (msg.sType == DUI_MSGTYPE_CLICK)
        OnClickName(sName);
    // [R-LOGV-4] 日志正文首次获得焦点时兜底钉住尾部：
    //   即使某些路径（清理后重填、DPI 切换重建 TextServices）把插入点
    //   留在了 0，RichEdit 一聚焦就会按插入点重设视图，表现仍是"点一下
    //   跳回最早那条"。这里在真正交给 TextServices 之前先把光标推到末尾，
    //   用户主动点选/拖选时随后的鼠标消息会正常覆盖该位置，不影响选区操作。
    else if (msg.sType == DUI_MSGTYPE_SETFOCUS && sName == L"logview_text")
        LogViewKeepTail();
}



void CToolDialogPage::OnSelectChangedName(const CDuiString& sName)
{
    // [L-10] 日志级别过滤（0=全部 1=警告及以上 2=仅错误）
    if (sName == L"cmb_logview_level")
    {
        if (CComboUI* pCmb = static_cast<CComboUI*>(m_pm.FindControl(L"cmb_logview_level")))
        {
            m_nLogFilter = pCmb->GetCurSel();
            if (m_nLogFilter < 0) m_nLogFilter = 0;
            LogViewFillView();
        }
        return;
    }
    if (sName == L"cmb_apply_index")
        ApplyIndexChanged();
    else if (sName == L"cmb_esdwiz_key")
    {
        // 密钥来源切换：选中"手动输入"时显示输入框，否则隐藏
        CEditUI* pManual = static_cast<CEditUI*>(
            m_pm.FindControl(L"edt_esdwiz_keymanual"));
        if (pManual)
        {
            bool bManual = (m_esd.pKey && m_esd.pKey->GetCurSel() == 1);
            pManual->SetVisible(bManual);
        }
    }
}



void CToolDialogPage::OnClickName(const CDuiString& sName)
{
    if (sName == L"btn_close" || sName == L"btn_cancel")
    {
        // [B4] 内嵌任务面板运行中，标题栏 X 也锁定（Esc 锁定逻辑同口径），
        // 防止作业半途关窗；ISO 漂浮作业不走 BeginTaskShield，不受影响。
        if (m_bTaskRunning) return;
        Close(0);
        return;
    }



    // 外部命令执行中：忽略除关窗外的一切操作（防重入）
    if (m_bTaskRunning) return;



    // [GC-02] 点击行为按 kind 分发表分发（原 15-case 已拆为下方 Click* 处理器）
    for (int i = 0; i < static_cast<int>(sizeof(kKindClickTab)/sizeof(kKindClickTab[0])); ++i)
    {
        if (kKindClickTab[i].nKind == m_nKind)
        {
            (this->*kKindClickTab[i].pfnClick)(sName);
            return;
        }
    }
}
// ------------------------------------------------------------
// [GC-02] 各 kind 点击处理器（原 OnClickName 内 case 体原样搬迁，零逻辑改动）
// ------------------------------------------------------------

