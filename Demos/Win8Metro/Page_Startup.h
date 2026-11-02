#pragma once

// ============================================================
// Page_Startup —— v2 P3b 纵切页面（启动项 + 服务两个子页）
//
// 承接原 CFrameWnd 的 12 个启动/服务函数：
//   BuildStartupPage / BuildServicesPage / ToggleStartupCategory /
//   ApplyStartupFilter / RefreshStartupInfoBar / AddStartupItem /
//   OnServiceStartType / ShowServiceContextMenu / DeleteStartupEntry /
//   OpenStartupEntryLocation / ShowStartupContextMenu / OnStartupToggle
// 及视图数据 m_startup / m_services / m_bStartupHideDisabled
//（strow_/stck_/svcrow_/svccombo_ 行序号严格一一对应）。
// 注册表枚举、StartupApproved 启停、SCM 枚举/启停、增删启动项
// 全部归 StartupService（S3）。
//
// 物理搬迁护栏：函数体零逻辑改动，仅把宿主引用从 CFrameWnd
// 隐式成员改为 m_ctx.Xxx（FindControl/Hwnd/Log/Alert/Glyph）。
// ============================================================

#include <cstddef>   // [C++17] std::size_t（StBuildServiceRow 形参）
#include <vector>
#include "Utils/Utils.h"
#include "PageBase.h"
#include "StartupService.h"   // StartupEntry / ServiceEntry / SRC_*

// [R162] 启动项分类定义。原为 BuildStartup 内的局部结构体；
//   抽出分类头/分类体成员函数后必须提到类外可见作用域。
//   nSrcs 以 -1 结尾，表示该分类聚合哪些 StartupSrcType。
struct PsCatDef
{
    const wchar_t* pszName;
    const wchar_t* pszLocation;
    int     nSrcs[8];   // -1 结尾
};

class CPageStartup : public CPageBase
{
public:
    explicit CPageStartup(IPageContext& ctx)
        : CPageBase(ctx), m_bHideDisabled(false), m_bShowDrivers(false) {}

    void Build() override { BuildStartup(); }   // 懒构建入口=启动项子页
    void BuildStartup();    // 原 BuildStartupPage

    // [R162] 从 BuildStartup 抽出（原 175 行 / 嵌套 4）：分类头 / 分类体 / 启动行
    void PsBuildCatHeader(CVerticalLayoutUI* pList, const PsCatDef& cat, int nCatIdx);
    void PsCollectBodyRows(CVerticalLayoutUI* pList, const PsCatDef& cat, int nCatIdx);
    void PsBuildStartupRow(CVerticalLayoutUI* pBody, StartupEntry& e, size_t k);

    // [R168] 从 DeleteService 抽出：依赖服务列表拼接（最多 5 条 + 余量提示）
    [[nodiscard]] CDuiString StBuildDepList(const std::vector<DepServiceEntry>& vDep);

    // [R169] 从 StBuildServiceRow 抽出：启动类型下拉（含 C4 未知态处理）
    [[nodiscard]] std::unique_ptr<CHorizontalLayoutUI>
        StBuildStartTypeCombo(size_t k, const ServiceEntry& se, int nItemH, int nDropH);

    // [R162] 从 StBuildServiceRow 抽出：服务状态 / 类型的显示文案（纯查表）
    [[nodiscard]] CDuiString StServiceStateText(const ServiceEntry& se);
    [[nodiscard]] const wchar_t* StServiceKindText(DWORD dwSvcType);

    // [R162] 从 OnContextMenu 抽出：启动行 / 服务行命中解析
    [[nodiscard]] bool TryHitStartupRow(CControlUI* pHit);
    [[nodiscard]] bool TryHitServiceRow(CControlUI* pHit);

    // [R162] 从 ApplyFilter / TryHitStartupRow 抽出：行内 stck_%d → 启动项索引
    [[nodiscard]] int PsRowIndexOf(CHorizontalLayoutUI* pRow);

    // [R175] 从 ApplyFilter 抽出：启动项过滤 / 分类体条数（嵌套 4→2）
    [[nodiscard]] int PsFilterStartupRows(const CDuiString& sKw, const CDuiString& sKwLower);
    [[nodiscard]] int PsCountBodyRows();

    // [R175] 从 PsFilterStartupRows 抽出：单行是否可见（关键字 + 隐藏禁用）
    [[nodiscard]] bool PsRowVisibleByFilter(int k, const CDuiString& sKw,
                                            const CDuiString& sKwLower) const;

    // [R175] 从 OnServiceStartType 抽出：失败后回滚启动类型下拉（嵌套 4→3）
    void StRollbackStartTypeCombo(CComboUI* pCombo, const ServiceEntry& se);
    void BuildServices();   // 原 BuildServicesPage

    // [AUDIT-P2] BuildServices 原 185 行 / CC=30，按职责拆为 3 段：
    //   StAddServiceEmptyHint  枚举失败空提示；StFilterDriverServices 非 Win32 过滤；
    //   StBuildServiceRow      单条服务行装配（原 137 行循环体）。
    // 渲染顺序与控件属性逐字不变。
    void StAddServiceEmptyHint(DuiLib::CVerticalLayoutUI* pList);
    void StFilterDriverServices();
    void StBuildServiceRow(DuiLib::CVerticalLayoutUI* pList, std::size_t k,
                           int nItemH, int nDropH);

    // [R3] 懒构建=启动项+服务两个子页；DPI/最大化重建仅启动项树
    void OnDeferredBuild() override { BuildStartup(); BuildServices(); }
    void OnRebuildDynamicTree() override { BuildStartup(); }
    [[nodiscard]] bool NeedsFullRebuildOnRelayout() const override { return true; }

    // P8：第二级事件路由（原壳 Notify / WM_RBUTTONUP 启动页分支）
    void OnEvent(TNotifyUI& msg) override;

    // [AUDIT-P2] OnEvent 原 88 行 / CC=28，按事件类型拆为 5 个处理函数。
    // 路由语义逐字保留（Tab2 计划任务跨页转发、stcat_/sttab_/strow_ 前缀、
    // svccombo_ 的 itemselect 分支）。
    void StOnItemSelect(TNotifyUI& msg, const wchar_t* pszName);
    void StOnClick(TNotifyUI& msg, const CDuiString& sName, const wchar_t* pszName);
    void StOnDoubleClick(const CDuiString& sName);
    void StOnTextChanged(const CDuiString& sName);
    void StOnSelectChanged(TNotifyUI& msg, const CDuiString& sName, const wchar_t* pszName);
    bool OnContextMenu(POINT pt, bool bFromContextMsg) override;

    void ToggleCategory(int c);              // 原 ToggleStartupCategory
    void ApplyFilter();                      // 原 ApplyStartupFilter
    // [R105] 服务搜索过滤：按服务名/显示名实时隐藏非命中行（隐藏而非重建）
    void ApplySvcFilter();
    void RefreshInfoBar(int nTab);           // 原 RefreshStartupInfoBar
    int  EstimateBootSeconds();               // [R69] 原 CFrameWnd::EstimateBootSeconds

    // [AUDIT-P2] EstimateBootSeconds 原 87 行 / CC=28，拆为 3 段：
    //   EstReadBootLog  读 bootevents.log 并 UTF-8 解码（false=不可读）
    //   EstSplitLines   按换行拆行、去行尾 CR、跳过空行
    //   EstHasBootPair  反向找同时含 6005 与 12 的 stats 行
    // 说明：日志里存的是事件命中计数而非时间戳，本函数只能判断"是否有配对"，
    // 返回 0/-1 的语义保持不变。
    bool EstReadBootLog(CDuiString& sContent);
    void EstSplitLines(const CDuiString& sContent, std::vector<CDuiString>& vLines);
    bool EstHasBootPair(const std::vector<CDuiString>& vLines);
    void AddItem();                          // 原 AddStartupItem
    void OnServiceStartType(int k, int iSel);
    void ShowServiceMenu(int k);             // 原 ShowServiceContextMenu
    // [R-B25] 服务依赖图可视化（4408）
    void ShowServiceDepGraph(int k);
    // [R125-2] 服务控制失败统一分级：code 1-11 → 专用文案 + HostAlert + HostLog
    void ShowServiceCtlError(int code, DWORD dwErr, const wchar_t* pszName);
    // [R111-7] 服务行双击复用入口：4401 启动 / 4402 停止
    void OnServiceToggle(int k, int iCmd);
    // [REFACTOR] 从 OnEvent 提取：服务行双击——运行中停止，否则启动。
    // 命中 svcrow_ 并处理后返回 true。
    bool HandleServiceRowDbClick(const CDuiString& sName);
    void DeleteEntry(int k);                 // 原 DeleteStartupEntry
    void DeleteService(int k);              // [R80] 删除服务（先停止再删）
    void OpenLocation(int k);                // 原 OpenStartupEntryLocation
    // [R111] 复制启动命令到剪贴板（右键菜单 4105）
    void CopyStartupCommand(int k);
    void ShowStartupMenu(int k);             // 原 ShowStartupContextMenu
    bool OnToggle(const wchar_t* pszName, bool bEnabled);  // 原 OnStartupToggle
    // [REFACTOR] 从 OnEvent 提取：启动项复选框勾选变更（stck_ 前缀），
    // 写入失败时回滚勾选并提示。命中并处理后返回 true。
    bool HandleEntryCheckChanged(const CDuiString& sName,
                                 const wchar_t* pszName, TNotifyUI& msg);

    // startup_hide_disabled 复选框：先落视图态再立即过滤
    void OnHideDisabledChanged(bool bHide)
    {
        m_bHideDisabled = bHide;
        ApplyFilter();
    }

    // [R117-1] svc_show_drivers 复选框：控制驱动服务显示，重建服务列表
    void OnShowDriversChanged(bool bShow);

    // [REFACTOR] 从 OnEvent 提取：startup_pages Tab2 内嵌的计划任务控件
    // （sttab_2 与 btn_task_*/tasktab_*/task_filter）跨页转发给 PAGE_TASKS。
    // 命中并转发返回 true，调用方据此立即 return。
    bool ForwardTaskChildEvent(const CDuiString& sName, TNotifyUI& msg);

    const wchar_t* PageName() const override { return L"Startup"; }
    // [R229] Re-colour code-built rows after a light<->dark switch. Colours only --
    //   no Build()/Add/Remove (would drop the list and leave m_pFocusCtrl dangling).
    void RefreshThemeColors() override;
    void    OnGlobalCopy(bool bCut) override;
private:
    std::vector<StartupEntry> m_vStartup;   // 与 strow_/stck_ 全局序号一一对应
    std::vector<ServiceEntry> m_vServices;  // 与 svcrow_/svccombo_ 序号一一对应
    bool                      m_bHideDisabled;   // 原 m_bStartupHideDisabled
    bool                      m_bShowDrivers;    // [R117-1] 显示驱动服务（默认隐藏）
};

// [R75] å¯å¨é¡¹è¯æ­åå°ä»»å¡å¥å£ã
//   å®ä¹å¨ Page_Startup.cppï¼Page_StartupService.cpp ä¹è°ç¨ï¼
//   æä» static æ¹ä¸ºå¤é¨é¾æ¥å¹¶å¨æ­¤å£°æã
//   nKind: 0=å¯å¨äºä»¶è§£æï¼1=æå¡å¿«ç§ã
void PostStartupDiagAsync(HWND hWnd, int nKind);
