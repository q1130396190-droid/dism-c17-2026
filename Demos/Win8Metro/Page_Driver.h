#pragma once

// ============================================================
// Page_Driver —— v2 P5 纵切页面（驱动管理）
//
// 承接原 CFrameWnd 的 11 个函数：
//   BuildDriverPage / ToggleDriverGroup / DriverSelectAll /
//   FillDriverBody / ApplyDriverFilter / UpdateDriverCount /
//   PnpEnumDrivers / ShowDriverDetail / DoDriverDelete /
//   DoDriverExport / DoDriverAdd
// 及视图状态 m_vDriverExpanded / m_vDriverFilled / m_mapDrvPub /
// m_mapDrvRow / m_bDrvPubEnum；并承接 drv_showbuiltin 勾选处理。
// pnputil 枚举/删除系统调用归 DriverService（S7）。
//
// 物理搬迁护栏：函数体零逻辑改动，宿主引用一律改 m_ctx.Xxx。
// ============================================================

#include <vector>
#include <map>
#include "Utils/Utils.h"
#include "PageBase.h"
#include "DriverService.h"   // DriverPubInfo / DriverRowInfo

// [R122-1] setupapi.h 的类型仅在 .cpp 内使用；头文件里以不完整类型引用，
// 避免在页面头中引入 setupapi.h（防止与其它页面头互相污染包含顺序）。
// 注意：这是 Win32 SDK 的 HDEVINFO 语义等价声明（void*），参数只做转发。
struct _SP_DEVINFO_DATA;
using SP_DEVINFO_DATA = struct _SP_DEVINFO_DATA;

// [卡顿修复] 第三方驱动库后台枚举结果（WM_DRV_PUBDATA，lParam 指向此结构；
// 由工作线程 new，UI 线程在 CPageDriver::OnPubData 内 delete）。
struct TDrvPubMsg
{
    LONG  nGen;       // 代际令牌，过期结果直接丢弃
    bool  bOk;        // DriverService::EnumPublished 返回值（空表也算已完成）
    std::map<CDuiString, DriverPubInfo>* pMap;
};

// [AUDIT-P0] 驱动删除后台化结果（WM_DRV_DELDATA，lParam 指向此结构；
// 工作线程 new，UI 线程在 OnDeleteData 内 delete）。
//
// 为什么必须后台化：DismRemoveDriver 走 CBS 事务，单个驱动实测 0.3~3s，
// 批量删除时旧实现在 UI 线程同步循环，期间消息泵完全停摆，Windows 判定
// "未响应"；用户强杀进程会中断正在提交的驱动事务，导致驱动库半成品。
struct TDrvDelMsg
{
    int   nOk   = 0;          // 成功数
    int   nFail = 0;          // 失败数
    std::wstring sFailDetail; // 失败明细（每行 "INF → exit N"）
};

class CPageDriver : public CPageBase
{
public:
    explicit CPageDriver(IPageContext& ctx)
        : CPageBase(ctx), m_bPubLoading(false), m_bPubTried(false),
          m_nPubGen(0), m_bDeleting(false), m_nSelCat(-1), m_nSelRow(-1) {}

    void Build() override;                 // 原 BuildDriverPage

    // P8：第二级事件路由（原壳 Notify 驱动页分支）
    void OnEvent(TNotifyUI& msg) override;

    // [R5] 第三方驱动库枚举结果收口（WM_DRV_PUBDATA，TDrvPubMsg 本页释放）
    bool OnAsyncMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) override;

    void ToggleGroup(int n);               // 原 ToggleDriverGroup
    void SelectAll();                      // 原 DriverSelectAll
    void FillBody(int n);                 // 原 FillDriverBody
    // [REFACTOR] 从 FillBody 提取：由 INF 路径推导驱动 .sys 目录提示。
    // 只读 sInfPath、只写 ri.sFile；不触碰 FillBody 内三处白名单缓冲。
    void FillSysPathHint(const CDuiString& sInfPath, DriverRowInfo& ri);
    // [AUDIT-P2] 从 FillBody 提取：单条设备行装配（原 51 行内联块）。
    // 只依赖 (pBody, n, nRow, 设备名)，未触碰 FillBody 内三处行级白名单缓冲。
    void DrvAppendDeviceRow(DuiLib::CVerticalLayoutUI* pBody, int n, int nRow,
                            const wchar_t* pszDeviceName);
    // [R122-1] 从 FillBody 提取：单台设备的行信息装配（原 107 行内联块）。
    // 入参均为 FillBody 已采集完成的值，函数内不修改外层缓冲，
    // 因此不会移动 szName/szDrvKey/szSvc 三处行级白名单锚点。
    // [R164] 从 DrvBuildRowInfo 抽出：读设备安装日期（三段校验，只读不回写）
    void DrvReadInstallDate(const wchar_t* pszDevInstId, CDuiString& sOut);

    void DrvBuildRowInfo(DriverRowInfo& ri, void* hDev,
                         SP_DEVINFO_DATA& did,
                         const wchar_t* pszName,
                         const CDuiString& sInfPath,
                         const wchar_t* pszDevInstId);
    // [REFACTOR] 从 FillBody 提取：设备列表为空时的占位提示行。
    void AddEmptyPlaceholder(CVerticalLayoutUI* pBody, bool bShowBuiltin);
    void ApplyFilter();                    // 原 ApplyDriverFilter
    void UpdateCount(int nShown, int nTotal = -1);  // 原 UpdateDriverCount
    // 第三方驱动库枚举后台化：在线 DISM 会话在慢机上可达 30~60s，
    // 绝不能再在 UI 线程同步跑（点选设备行即未响应→被强杀）。
    // StartPubEnum 仅 UI 线程调用；工作线程经 WM_DRV_PUBDATA 回传后由
    // OnPubData 收口（拥有 pMsg 所有权，负责 delete）。
    void StartPubEnum();
    void OnPubData(struct TDrvPubMsg* pMsg);
    void ShowDetail(int nCat, int nRow);   // 原 ShowDriverDetail

    // [AUDIT-P2] ShowDetail 原 235 行 / CC=32，按职责拆为 4 个段函数：
    //   DrvHighlightSelected 高亮当前设备行；DrvAddDetailTitle 标题行；
    //   DrvAddDetailFields 属性字段表；DrvAddDetailButtons 回退/属性按钮区。
    // 拆前拆后渲染顺序与内容逐字等价，未改任何控件属性或文案。
    void DrvHighlightSelected(const CDuiString& sKey);
    void DrvAddDetailTitle(DuiLib::CVerticalLayoutUI* pPanel,
                           const DriverRowInfo& ri, const RECT& rcPad);
    void DrvAddDetailFields(DuiLib::CVerticalLayoutUI* pPanel,
                            const DriverRowInfo& ri,
                            const DriverPubInfo* pPub,
                            const RECT& rcPad);
    void DrvAddDetailButtons(DuiLib::CVerticalLayoutUI* pPanel,
                             const DriverRowInfo& ri, const RECT& rcPad);
    void Delete();                         // 原 DoDriverDelete（改为后台执行）
    // [AUDIT-P0] 驱动删除后台化：StartDeleteAsync 仅 UI 线程调用；
    // 工作线程经 WM_DRV_DELDATA 回传，OnDeleteData 收口（拥有 pMsg 所有权）。
    void StartDeleteAsync(std::vector<CDuiString> vOem);
    void OnDeleteData(struct TDrvDelMsg* pMsg);
    void Export();                         // 原 DoDriverExport
    void Add();                            // 原 DoDriverAdd
    void ScanOfflineDrivers();             // [R72] 离线驱动扫描：选择 WIM 映像
    void ScanDevices();                    // [R83] P1-15：pnputil /scan-devices 触发 PnP 设备树重扫
    void Rollback();                       // [R87] P1-14：驱动回退到上一版本（Backup\Driver + DIF_PROPERTYCHANGE）
    void ShowDeviceProperties();           // [R120-2] 打开设备管理器式属性对话框（devmgr.dll）

    // drv_showbuiltin 勾选变化（原 Notify 内分支）：同步字形并重枚举已展开分类
    void OnShowBuiltinToggled(DuiLib::CControlUI* pSender);

    const wchar_t* PageName() const override { return L"Driver"; }
    void    OnGlobalSelectAll() override;
    void    OnGlobalSave() override;

    // [R-B24] 行级右键菜单：drow_<cat>_<row> 命中 → 6 项动作菜单
    bool    OnContextMenu(POINT pt, bool bFromContextMsg) override;
    void    ShowRowContextMenu(int nCat, int nRow, POINT pt);
    void    CopyRowName(int nCat, int nRow);      // 复制设备名
    void    CopyRowInf(int nCat, int nRow);       // 复制 INF 路径
    void    CopyRowDevInst(int nCat, int nRow);   // 复制设备实例 ID
private:
    std::vector<bool>                   m_vDriverExpanded;  // 原 m_vDriverExpanded
    std::vector<bool>                   m_vDriverFilled;    // 原 m_vDriverFilled
    std::map<CDuiString, DriverPubInfo> m_mapDrvPub;        // 原 m_mapDrvPub

    // [R226] Re-colour the driver tree and detail panel after a light<->dark switch.
    //   Contract (PageBase.h): colour attributes only -- no Build()/Add/Remove, or
    //   the tree state, selection and scroll position are lost and m_pFocusCtrl dangles.
    void RefreshThemeColors() override;
    std::map<CDuiString, DriverRowInfo> m_mapDrvRow;        // 原 m_mapDrvRow
    // [R162] 跨 TU 状态单一入口。原先这几个成员在 Driver.cpp / DriverAsync.cpp /
    //   DriverDetail.cpp / DriverMenu.cpp 各自裸写（实测 4 个多写点成员）。
    //   m_nSelCat/m_nSelRow 是"当前选中设备"的两个分量，必须同步写 ——
    //   Menu.cpp 里曾两次重复 "m_nSelCat = nCat; m_nSelRow = nRow;"，
    //   谁漏写其一就会出现"回退驱动定位错行"。集中后不变式只有一份。
    void MarkPubLoading(bool bOn)                    { m_bPubLoading = bOn; }
    void MarkPubTried()                              { m_bPubTried = true; }
    void RememberSelection(int nCat, int nRow)       { m_nSelCat = nCat; m_nSelRow = nRow; }

    bool                                m_bPubLoading;      // 后台枚举进行中
    bool                                m_bPubTried;        // 本轮已发起/完成（避免反复触发 40s 枚举）
    LONG                                m_nPubGen;          // 代际令牌（Build/删除/添加后作废在途结果）
    bool                                m_bDeleting;        // [AUDIT-P0] 后台删除进行中（防重入）
    int                                 m_nSelCat;          // [R87] P1-14 当前选中设备分类（-1 未选）
    int                                 m_nSelRow;          // [R87] P1-14 当前选中设备行（-1 未选）
};
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
