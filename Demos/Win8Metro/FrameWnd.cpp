#include "stdafx.h"
#include <type_traits>   // [R220] std::true_type / std::void_t
// [REMOVED] #include <regex> — F-03 修复：未实际使用，XML 属性抽取改用展开+替换路线
#include "SafeEnv.h"   // [B6] 安全环境变量读取：替代弃用 _wgetenv
#include "OfflineSession.h"  // [R10-02] 离线会话经独立组件访问，去 CDataEngine 静态耦合
#include "SafeThread.h"        // 工作线程顶层异常边界
#include "BackgroundJobs.h"    // [CRASHFIX] 后台作业统一收口（SEH 边界+退出等待）
#include "WIMHelperBridge.h"  // Patch-4: wimgapi bridge
#include "DismApiBridge.h"    // Patch-4: dismapi bridge
#include "FrameWnd.h"
#include <optional>   // [C++17 R-P10]
#include <memory>     // [C++17 R3] std::unique_ptr / std::make_unique
#include "StringManager.h"
#include "ConfigReader.h"
// #include "OfflineCbsService.h" // [R25-F1] 删除假依赖：0 处使用   // [R-B13] 离线 CBS 深链路（蜂巢挂载 / TI 停止 / OfflineCbs.log / pending 校验）
#include "CbsHostClient.h" // [R-拆分] CBSHost 6 导出经 CBSHost.dll 动态调用（不再静态链接语义层）
#include "NCleanerClient.h" // [R-拆分] NCleaner 7 导出经 NCleaner.dll 动态调用
#include "ActivationKitClient.h" // [R01] 激活备份/还原经 ActivationKit.dll 动态调用（替代青蛙插件）
#include "FrameWnd_Util.h"   // C1 拆分试点：CopyTextToClipboard 已抽到独立模块（调用点用 FrameWnd_Util_CopyTextToClipboard）
#include "DwmTheme.h"        // [R201] Win11 圆角/Mica/强调色（Win10 上逐项降级）
#include "ThemeTokens.h"
#include "CustomControls.h"           // [BOX-CHECK] CBoxCheckUI 自绘复选框（三态方框）
#include "SkinTokenReplayTable.h"     // [R201] 强调色令牌：替代写死的 #007ACC
#include "ThemePickerWnd.h"  // [R236] 标题栏主题下拉（自绘预览）
#include "ThemeManager.h"     // [DLL] 统一主题管理器 DLL（皮肤缓存 + 状态机 + DWM）
#include "GeometryDump.h"
#include "UiSnapshot.h"       // [R209] 离屏渲染导出 PNG
#include "FontSupport.h"     // [R201] 字体族可用性探测与回退
#include "FrameWnd_Menu.h"   // [R14-01] ShowPopupMenu 菜单表驱动拆解（4 Build + Track + Dispatch + case 处理器）
#include "FrameWnd_ErrorMap.h" // 第 2 轮] 错误码 → 中文友好提示映射
#include <gdiplus.h>           // [U-3] 左侧导航 Win11 线性图标 GDI+ 自绘
#pragma comment(lib, "gdiplus.lib")
#include "SystemInfoService.h" // P1/S1：系统信息（系统盘/桌面/构建号/时间戳/临时目录）
#include "Win32DialogService.h" // P1/S2：通用文件对话框（打开/保存/浏览目录）


#include "ProcessRunner.h"       // P2b/S9 基础层：隐藏进程/管道捕获
#include "WofCompact.h"          // [P0-A2] CompactOS/WOF 原生 FSCTL（替代 compact.exe）
#include <thread>                 // [P0-A2] WOF 后台压缩线程
#include "ScheduleTaskService.h" // P2b/S11：schtasks 查询与清理计划增删
#include "Registry.h"           // P2c：注册表只读助手 RegGetString（原本文件 static）
#include "AssocService.h"       // P2c/S8：文件关联 UserChoice 写入/dism 关联命令
#include "ExternalToolService.h" // P6a/S9：DISM 管道会话/外部进程/受守护 bat 执行器
#include "CommandLineService.h" // R-B3：CLI 参数层（/C /exe /U /Image /All /? 等）
#include "ExternalCmdLedger.h"  // [R-B56] 外部命令防腐台账（清单维度 3 缺失 5）
// #include "ProcessJob.h" // [R25-F1] 删除假依赖：0 处使用        // 子进程随主进程终结（Kill-On-Close 作业）
using i18n::CStringManager;   // 便捷：直接写 CStringManager 不用前缀
#include <ctime>
#include <cstdio>
#include <commdlg.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <shobjidl.h>   // ITaskbarList3 任务栏进度
#include <objbase.h>
#include <oleauto.h>
#include <algorithm>
#include <functional>
#include <string>
#include <fstream>   // [R139] /DumpLayout 用 std::ofstream
#include <vector>
#include <map>          // [R-B56] ExternalCmdLedger::CountByCategory 返回 std::map
#include <gdiplus.h>
#include <setupapi.h>
#include <tlhelp32.h>
#include <initguid.h>   // 在本编译单元内定义 devpkey 属性键，避免链接缺 GUID 符号
#include <devpkey.h>


#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "setupapi.lib")


// file-scope GDI+ bootstrap (shutdown skipped; OS reclaims on exit)
namespace {
struct GdiplusBoot {
    ULONG_PTR m_token;
    GdiplusBoot() {
        Gdiplus::GdiplusStartupInput inp;
        Gdiplus::GdiplusStartup(&m_token, &inp, nullptr);
    }
} g_gdiplusBoot;
}


// ============================================================
// 多语言：语言菜单项与切换辅助
// ------------------------------------------------------------
// 语言名称采用 endonym 固定显示（简体中文/English/日本語/한국어），
// 不随当前 UI 语言翻译，任何语言界面下用户都能认出自己的母语。
// 顺序/ID 与 ShowPopupMenu() 的 1320~1329 一一对应。
// =========================================================



// 重启接力事件名 kRestartEventName 已迁入 FrameWnd_Messages.h（与 Win8Metro.cpp 同源）
// 语言切换后重启进程：先唤起新实例（其单实例检测会等待本事件），


// ============================================================
// P6b: ScanItemMsg / CleanDoneMsg moved to CleanJobService.h
// (included through Page_Cleanup.h)
// ============================================================
// ============================================================
// common Win32 dialogs / file helpers (toolbox tools)
// ============================================================


// S-15: 全局错误文本复制到剪贴板的静态辅助。
// 复用 R24 已在位的手工 OpenClipboard/GlobalAlloc 模式（DuiLib 无内建 API），
// 集中封装后各处 MessageBox 都能一行调用。
// 失败静默（不影响 UI），返回是否成功复制。
// C1 拆分试点（R27）：CopyTextToClipboard 已抽到 FrameWnd_Util.cpp
// 通过 FrameWnd_Util.h 的声明调用（调用点用 FrameWnd_Util_CopyTextToClipboard）


// S2：通用文件对话框已迁入 Win32DialogService.h/.cpp
// （DlgOpenFile / DlgSaveFile / DlgBrowseFolder）
// S1：系统信息函数已迁入 SystemInfoService.h/.cpp
// （GetSystemDriveStr/GetDesktopDir/GetBuildNumber/GetWindowsFamilyName/MakeTimestamp/MakeTempWorkDir）


// P6a：WriteBatFile / FindOscdimg / FindEtfsboot / FindEfisys / LaunchSystemEx /
// IsToolAvailable / IsElevated 已迁入 ExternalToolService（S9）


// [P0-A2] CompactOS 状态原生查询（WofCompact，替代 compact.exe /compactos:query
// 的本地化文本匹配）。返回：0=未压缩(Never) 1=部分(Mixed) 2=已优化(Optimized)
//      -1=不支持/查询失败。
// [P0-A2] WOF 后台线程 → UI 日志（lParam=new CDuiString，WM_WOF_LOG 释放）
static void PostWofLog(HWND hWnd, const CDuiString& sMsg)
{
    if (!hWnd || !::IsWindow(hWnd)) return;
    // [C++17 R3] ownership transferred to UI on successful post; guard frees otherwise
    std::unique_ptr<CDuiString> pMsg = std::make_unique<CDuiString>(sMsg);
    // [加固] 消息队列满等投递失败场景下回收，避免日志对象泄漏
    if (!::PostMessageW(hWnd, WM_WOF_LOG, 0, reinterpret_cast<LPARAM>(pMsg.get())))
        return;
    pMsg.release();
}
// WOF 进度回调上下文：节流回传 + 取消（当前不取消，始终 true）
struct WofUiCtx
{
    HWND hWnd = nullptr;
    unsigned long long nLastReport = 0;
};
// [CRASHFIX] WOF 目录压缩/解压后台作业上下文
struct TWofTreeJob
{
    HWND       hWnd;
    CDuiString sTarget;
    int        nAlg;
    bool       bUncompact;
};
// [CRASHFIX] 替代裸 std::thread::detach：SafeCreateThread 双层 SEH 边界兜底，
// 硬件异常（AV 等）不再逃逸线程过程直接终止进程。
static DWORD WINAPI WofTreeThreadProc(void* pv)
{
    std::unique_ptr<TWofTreeJob> guardJob(reinterpret_cast<TWofTreeJob*>(pv));   // [C++17 R3] ctx ownership from StartWofTreeWorker
    TWofTreeJob* pJob = guardJob.get();
    try
    {
        WofUiCtx ctx;
        ctx.hWnd = pJob->hWnd;
        auto pfn = [](const WofCompact::Progress& st, void* p) -> bool
        {
            WofUiCtx* c = (WofUiCtx*)p;
            CDuiString s;
            s.Format(L"[wof] scanned=%llu changed=%llu skipped=%llu failed=%llu",
                     st.nScanned, st.nChanged, st.nSkipped, st.nFailed);
            PostWofLog(c->hWnd, s);
            return true;
        };
        WofCompact::CompactTree(pJob->sTarget.GetData(), pJob->nAlg,
                                pJob->bUncompact, pfn, &ctx);
        CDuiString sDone(L"[wof] done: ");
        sDone += pJob->sTarget;
        PostWofLog(pJob->hWnd, sDone);
    }
    catch (...)
    {
        PostWofLog(pJob->hWnd, CDuiString(L"[wof] 任务异常中止: ") + pJob->sTarget);
    }
    // [C++17 R3] guardJob frees ctx at scope exit (was explicit delete here)
    return 0;
}
// [P0-A2] 启动资源管理器右键压缩/解压后台任务（替代 compact.exe bat）
static void StartWofTreeWorker(HWND hWnd, const CDuiString& sTarget,
                               WofCompact::Algorithm nAlg, bool bUncompact, const wchar_t* pszTag)
{
    PostWofLog(hWnd, CDuiString(L"[cli] ") + pszTag + L": " + sTarget);
    // [C++17 R3] thread ctx: ownership transferred to WofTreeThreadProc on success; guard frees on failure
    std::unique_ptr<TWofTreeJob> pJob = std::make_unique<TWofTreeJob>();
    pJob->hWnd = hWnd;
    pJob->sTarget = sTarget;
    pJob->nAlg = static_cast<int>(nAlg);
    pJob->bUncompact = bUncompact;
    if (!BackgroundJobs::Start(&WofTreeThreadProc, pJob.get(), "wof-tree"))
    {
        PostWofLog(hWnd, L"[wof] 后台作业启动失败（线程资源不足）");
        return;   // [C++17 R3] thread start failed, ctx not transferred; guard frees
    }
    pJob.release();
}




// ---- WIM/ESD image index info ----
// WIM/ESD 索引信息统一走 wimgapi 原生：HostQueryWim 内调用
// WIMBridge::QueryWimInfo（WIMCreateFile + WIMGetImageCount +
// 逐映像 WIMGetImageInformation 解析 XML 元数据），零 dism /Get-WimInfo
// 子进程与本地化文本解析；工具箱页经 IToolHost::HostQueryWim 消费。


// [R19] 日志列表行：支持单行自定义文本色（WARN 橙 / ERROR 红）。
// 基类 CListLabelElementUI 只能用列表统一色，故重写 DrawItemText。
class CColorListLabelElementUI : public CListLabelElementUI
{
public:
    CColorListLabelElementUI() : m_dwOwnColor(0) {}


    void SetOwnTextColor(DWORD dwColor) { m_dwOwnColor = dwColor; }


    virtual void DrawItemText(HDC hDC, const RECT& rcItem)
    {
        if (m_dwOwnColor == 0)
        {
            CListLabelElementUI::DrawItemText(hDC, rcItem);
            return;
        }


        CDuiString sText = GetText();
        if (sText.IsEmpty() || m_pOwner == nullptr) return;


        TListInfoUI* pInfo = m_pOwner->GetListInfo();
        int nLinks = 0;
        RECT rcText = rcItem;
        RECT rcTextPadding = GetManager()->GetDPIObj()->Scale(pInfo->rcTextPadding);
        rcText.left += rcTextPadding.left;
        rcText.right -= rcTextPadding.right;
        rcText.top += rcTextPadding.top;
        rcText.bottom -= rcTextPadding.bottom;


        if (pInfo->bShowHtml)
            CRenderEngine::DrawHtmlText(hDC, m_pManager, rcText, sText, m_dwOwnColor,
                nullptr, nullptr, nLinks, pInfo->nFont, pInfo->uTextStyle);
        else
            CRenderEngine::DrawText(hDC, m_pManager, rcText, sText, m_dwOwnColor,
                pInfo->nFont, pInfo->uTextStyle);
    }


private:
    DWORD m_dwOwnColor;
};


// row whose background lights up while the pointer is over it


// [D2] Win11 风格旋转进度环：灰底圆环 + 90° 蓝色旋转弧。
// 角度取 GetTickCount，由宿主窗口定时器 (~90ms) 驱动 Invalidate。
class CProgressRingUI : public CControlUI
{
public:
    CProgressRingUI()
    {
        SetMouseEnabled(false);
        SetFixedWidth(30);
        SetFixedHeight(30);
    }


    virtual void Init()
    {
        CControlUI::Init();
        if (m_pManager)
        {
            int n = m_pManager->GetDPIObj()->Scale(30);
            SetFixedWidth(n);
            SetFixedHeight(n);
        }
    }


    const wchar_t* GetClass() const { return L"ProgressRingUI"; }


    virtual bool DoPaint(HDC hDC, const RECT& rcPaint, CControlUI* pStopControl)
    {
        RECT rc = m_rcItem;
        int nW = rc.right - rc.left;
        int nH = rc.bottom - rc.top;
        int nEdge = (m_pManager ? m_pManager->GetDPIObj()->Scale(3) : 3);
        int r = (nW < nH ? nW : nH) / 2 - nEdge;
        if (r < 4) return true;
        int cx = (rc.left + rc.right) / 2;
        int cy = (rc.top + rc.bottom) / 2;


        int nTrackW = (m_pManager ? m_pManager->GetDPIObj()->Scale(2) : 2);
        int nArcW   = (m_pManager ? m_pManager->GetDPIObj()->Scale(3) : 3);
        HPEN hPenTrack = ::CreatePen(PS_SOLID, nTrackW, RGB(233, 237, 242));
        HPEN hPenArc   = ::CreatePen(PS_SOLID, nArcW, RGB(0x00, 0x7A, 0xCC));
        HGDIOBJ hOldPen = ::SelectObject(hDC, hPenTrack);
        HGDIOBJ hOldBr  = ::SelectObject(hDC, ::GetStockObject(NULL_BRUSH));


        ::Ellipse(hDC, cx - r, cy - r, cx + r, cy + r);


        // 每 900ms 一圈；GDI Arc 逆时针（屏幕坐标下视觉为顺时针）
        float fStart = (::GetTickCount() % 900) / 900.0f * 360.0f;
        constexpr float PI = 3.14159265f;
        float a0 = fStart * PI / 180.0f;
        float a1 = (fStart + 90.0f) * PI / 180.0f;
        int sx = cx + static_cast<int>(r * cosf(a0));
        int sy = cy + static_cast<int>(r * sinf(a0));
        int ex = cx + static_cast<int>(r * cosf(a1));
        int ey = cy + static_cast<int>(r * sinf(a1));


        ::SelectObject(hDC, hPenArc);
        ::Arc(hDC, cx - r, cy - r, cx + r, cy + r, sx, sy, ex, ey);


        ::SelectObject(hDC, hOldPen);
        ::SelectObject(hDC, hOldBr);

        // [R123] 释放本帧创建的两个画笔。
        //   原实现只做了 SelectObject 恢复，从未 DeleteObject ——
        //   而这是 DoPaint 路径，每重绘一次泄漏 2 个 HPEN。
        //   Windows 每进程 GDI 对象上限默认 10000，这个控件是转圈加载动画
        //   （900ms 一圈，持续重绘），长时间停留会耗尽 GDI 句柄，
        //   之后所有绘制失败（界面变白/花屏）。
        //   注意：必须在 SelectObject 恢复**之后**才删，否则会把正在被 DC
        //   使用的画笔删掉。
        if (hPenTrack) ::DeleteObject(hPenTrack);
        if (hPenArc)   ::DeleteObject(hPenArc);

        return true;
    }
};


// self-drawn sliding switch: blue #007ACC track when on (Dism++ style),
// light gray track when off; white knob with a subtle light border.


// ============================================================
// tiny GDI paint helpers (color values are 0xAABBGGRR)
// ============================================================
// [C++17 升级阶段2] 局部句柄 RAII 守卫：隐式转换为原句柄类型，调用点
// 零改动；只负责早退/异常路径自动释放，不改变任何调用顺序与业务分支。
// 提权令牌 hImpToken、vhProc 数组、跨函数移交句柄按方案保持显式管理。
struct RegKeyGuard
{
    RegKeyGuard() = default;
    ~RegKeyGuard() { if (m_h) ::RegCloseKey(m_h); }
    RegKeyGuard(const RegKeyGuard&) = delete;
    RegKeyGuard& operator=(const RegKeyGuard&) = delete;
    operator HKEY() const { return m_h; }
    HKEY* out() { return &m_h; }
private:
    HKEY m_h = nullptr;
};
struct FileGuard
{
    FileGuard() = default;
    FileGuard(HANDLE h) : m_h(h) {}
    ~FileGuard() { if (m_h != nullptr && m_h != INVALID_HANDLE_VALUE) ::CloseHandle(m_h); }
    FileGuard(const FileGuard&) = delete;
    FileGuard& operator=(const FileGuard&) = delete;
    operator HANDLE() const { return m_h; }
private:
    HANDLE m_h = nullptr;
};
struct FindGuard
{
    FindGuard() = default;
    FindGuard(HANDLE h) : m_h(h) {}
    ~FindGuard() { if (m_h != nullptr && m_h != INVALID_HANDLE_VALUE) ::FindClose(m_h); }
    FindGuard(const FindGuard&) = delete;
    FindGuard& operator=(const FindGuard&) = delete;
    operator HANDLE() const { return m_h; }
private:
    HANDLE m_h = nullptr;
};
struct GdiGuard
{
    GdiGuard() = default;
    GdiGuard(HGDIOBJ g) : m_g(g) {}
    ~GdiGuard() { if (m_g) ::DeleteObject(m_g); }
    GdiGuard(const GdiGuard&) = delete;
    GdiGuard& operator=(const GdiGuard&) = delete;
    operator HGDIOBJ() const { return m_g; }
    // [C++17 R-P8] FillRect 第 3 参为 HBRUSH，HGDIOBJ 不隐式转，补显式转换。
    operator HBRUSH() const { return (HBRUSH)m_g; }
private:
    HGDIOBJ m_g = nullptr;
};
struct SnapGuard
{
    SnapGuard() = default;
    SnapGuard(HANDLE h) : m_h(h) {}
    ~SnapGuard() { if (m_h != nullptr && m_h != INVALID_HANDLE_VALUE) ::CloseHandle(m_h); }
    SnapGuard(const SnapGuard&) = delete;
    SnapGuard& operator=(const SnapGuard&) = delete;
    operator HANDLE() const { return m_h; }
private:
    HANDLE m_h = nullptr;
};
struct HandleGuard
{
    HandleGuard() = default;
    HandleGuard(HANDLE h) : m_h(h) {}   // [C++17 R-P8] 补 HANDLE 构造（原写法为直接接收 API 返回值）
    ~HandleGuard() { if (m_h != nullptr && m_h != INVALID_HANDLE_VALUE) ::CloseHandle(m_h); }
    HandleGuard(const HandleGuard&) = delete;
    HandleGuard& operator=(const HandleGuard&) = delete;
    operator HANDLE() const { return m_h; }
    HANDLE* out() { return &m_h; }
private:
    HANDLE m_h = nullptr;
};
static void GdiFillRound(HDC hDC, int l, int t, int r, int b, int rad, COLORREF clr)
{
    GdiGuard hbr = ::CreateSolidBrush(clr);
    GdiGuard hpn = ::CreatePen(PS_SOLID, 1, clr);
    HGDIOBJ oldbr = ::SelectObject(hDC, hbr);
    HGDIOBJ oldpn = ::SelectObject(hDC, hpn);
    ::RoundRect(hDC, l, t, r, b, rad, rad);
    ::SelectObject(hDC, oldbr);
    ::SelectObject(hDC, oldpn);



}


static void GdiFillRect(HDC hDC, int l, int t, int r, int b, COLORREF clr)
{
    RECT rc = { l, t, r, b };
    GdiGuard hbr = ::CreateSolidBrush(clr);
    ::FillRect(hDC, &rc, hbr);


}


static void GdiEllipse(HDC hDC, int l, int t, int r, int b, COLORREF clr)
{
    GdiGuard hbr = ::CreateSolidBrush(clr);
    GdiGuard hpn = ::CreatePen(PS_SOLID, 1, clr);
    HGDIOBJ oldbr = ::SelectObject(hDC, hbr);
    HGDIOBJ oldpn = ::SelectObject(hDC, hpn);
    ::Ellipse(hDC, l, t, r, b);
    ::SelectObject(hDC, oldbr);
    ::SelectObject(hDC, oldpn);



}


static void GdiLine(HDC hDC, int x1, int y1, int x2, int y2, int w, COLORREF clr)
{
    GdiGuard hpn = ::CreatePen(PS_SOLID, w, clr);
    HGDIOBJ oldpn = ::SelectObject(hDC, hpn);
    ::MoveToEx(hDC, x1, y1, nullptr);
    ::LineTo(hDC, x2, y2);
    ::SelectObject(hDC, oldpn);


}


static void GdiRoundFrame(HDC hDC, int l, int t, int r, int b, int rad, int w, COLORREF clr)
{
    GdiGuard hpn = ::CreatePen(PS_SOLID, w, clr);
    HGDIOBJ oldpn = ::SelectObject(hDC, hpn);
    HGDIOBJ oldbr = ::SelectObject(hDC, ::GetStockObject(NULL_BRUSH));
    ::RoundRect(hDC, l, t, r, b, rad, rad);
    ::SelectObject(hDC, oldpn);
    ::SelectObject(hDC, oldbr);


}


static void GdiArc(HDC hDC, int l, int t, int r, int b,
                   int xs, int ys, int xe, int ye, int w, COLORREF clr)
{
    GdiGuard hpn = ::CreatePen(PS_SOLID, w, clr);
    HGDIOBJ oldpn = ::SelectObject(hDC, hpn);
    ::Arc(hDC, l, t, r, b, xs, ys, xe, ye);
    ::SelectObject(hDC, oldpn);


}


static void GdiText(HDC hDC, const wchar_t* psz, int l, int t, int r, int b,
                    int nH, COLORREF clr, bool bBold)
{
    GdiGuard hf = ::CreateFontW(-nH, 0, 0, 0, bBold ? FW_BOLD : FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS,
        L"Segoe UI");
    HGDIOBJ oldf = ::SelectObject(hDC, hf);
    int oldmode = ::SetBkMode(hDC, TRANSPARENT);
    COLORREF oldc = ::SetTextColor(hDC, clr);
    RECT rc = { l, t, r, b };
    ::DrawTextW(hDC, psz, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    ::SetTextColor(hDC, oldc);
    ::SetBkMode(hDC, oldmode);
    ::SelectObject(hDC, oldf);


}


// P6a：CToolIconUI / CClickVLayoutUI 已随工具箱页迁入 Page_Tools.cpp


// ============================================================
// helpers
// ============================================================
CDuiString ReadEditValue(CPaintManagerUI& pm, const wchar_t* pszName)
{
    CEditUI* pEdit = static_cast<CEditUI*>(pm.FindControl(pszName));
    if (!pEdit) return L"";
    return pEdit->GetText();
}


static bool ReadCheckValue(CPaintManagerUI& pm, const wchar_t* pszName)
{
    CCheckBoxUI* pCheck = static_cast<CCheckBoxUI*>(pm.FindControl(pszName));
    return pCheck && pCheck->GetCheck();
}


static void SetCtrlText(CPaintManagerUI& pm, const wchar_t* pszName, const wchar_t* pszText)
{
    CControlUI* pCtrl = pm.FindControl(pszName);
    if (pCtrl) pCtrl->SetText(pszText);
}


static CDuiString GetSkinText(CPaintManagerUI& pm, const wchar_t* pszName)
{
    // i18n 优先：语言包提供 skin.<控件名> 译文时使用译文；
    // 缺键（含 zh-CN 与未翻译条目）回退皮肤 XML 原文，保证永不出现 [key]。
    CDuiString sKey;
    sKey.Format(L"skin.%s", pszName);
    CDuiString sTr;
    if (StringMgr.TryGet(sKey.GetData(), sTr) && !sTr.IsEmpty())
        return sTr;


    CControlUI* pCtrl = pm.FindControl(pszName);
    if (!pCtrl) return L"";
    return pCtrl->GetText();
}

// [R216] Loading-hint text that never depended on a skin control existing.
//
// GetSkinText falls back to "the text of the control with this name". For the loading
// hints there is no such control on purpose -- the hint is built in code and only the
// words come from the skin. Both call sites therefore resolved to an empty string:
//   Page_Program.cpp:130  HostSkinText(L"lbl_loading")
//   Page_Appx.cpp:44      HostSkinText(L"lbl_appx_loading")
// and neither name exists in any skin XML (grep: 0 hits), so the placeholder row showed
// a spinner with no text at all. Rather than invent a control just to hold a string,
// these keys get an explicit default so a missing translation degrades to real words
// instead of nothing.
static CDuiString GetLoadingHintText(const wchar_t* pszName)
{
    CDuiString sKey;
    sKey.Format(L"skin.%s", pszName);
    CDuiString sTr;
    if (StringMgr.TryGet(sKey.GetData(), sTr) && !sTr.IsEmpty())
        return sTr;
    // No translation and no control: use a neutral default rather than "".
    return CDuiString(L"正在加载…");   // "正在加载…"
}


// 皮肤 XML 中以中文直写、且不被各页面动态重建逻辑覆盖的静态可见文本，
// 统一在皮肤加载完成后按 skin.<控件名> 应用当前语言译文；
// 语言包缺键时不执行 SetText/SetAttribute，界面保留 XML 原文。
static void ApplyChromeI18n(CPaintManagerUI& pm)
{
    static const wchar_t* s_chromeTexts[] = {
        // 顶部菜单
        L"menu_file", L"menu_recover", L"menu_option", L"menu_help",
        // 左侧导航
        L"nav_0", L"nav_1", L"nav_2", L"nav_3", L"nav_4",
        L"nav_5", L"nav_6", L"nav_7", L"nav_8", L"nav_9", L"nav_10",
        // 导航分组标题
        L"grp_common", L"grp_control", L"grp_deploy",
        // 会话信息 / 底部状态
        L"sess_type", L"app_status",
        // 各页签
        L"sttab_0", L"sttab_1",
        L"appxtab_0", L"appxtab_1",
        L"progtab_0", L"progtab_1",
        L"updtab_0", L"updtab_1",
        // 0 空间回收
        L"clean_showzero",
        L"btn_cln_default", L"btn_cln_refresh", L"btn_cln_export",
        L"btn_scan", L"btn_clean",
        L"lbl_cln_desc", L"lbl_cln_detail",
        // 1 启动项
        L"startup_hide_disabled", L"btn_startup_refresh",
        L"hdr_su_service", L"hdr_su_name", L"hdr_su_status", L"hdr_su_type",
        // 2 Appx 管理
        L"lbl_ax_filter", L"btn_appx_clear",
        L"appx_hdr_item_0", L"hdr_ax_size",
        L"hdr_ax_item2", L"hdr_ax_size2",
        L"btn_appx_selall", L"btn_appx_delete",
        // 4 系统优化
        L"hdr_op_item", L"hdr_op_cur", L"hdr_op_sys",   // [SCOPE-2COL] hdr_op_new 已随「新用户」列移除
        L"btn_find", L"btn_optim_bulk", L"btn_optim_state",
        L"btn_apply_rules", L"btn_import_cfg", L"btn_export_cfg", L"btn_refresh",
        // 5 驱动管理
        L"drv_warn", L"drv_showbuiltin",
        L"btn_drv_selall", L"btn_drv_export", L"btn_drv_add", L"btn_drv_delete",
        // 6 程序和功能
        L"prog_feat_empty", L"lbl_pg_filter",
        L"hdr_pg_item", L"hdr_pg_date", L"hdr_pg_size", L"lbl_pg_localsrc",
        L"btn_feat_browse",
        L"legend_off", L"legend_partial", L"legend_all", L"legend_del",
        L"btn_prog_delete", L"btn_prog_refresh", L"btn_prog_apply",
        // 7 可选功能
        L"cap_showall",
        // [R30] 可选功能页 /Source 浏览按钮
        L"btn_cap_browse",
        // [R38] 可选功能页 刷新按钮
        L"btn_cap_refresh",
        // 8 更新管理
        L"hdr_wu_item", L"hdr_wu_date", L"hdr_wu_status",
        L"hdr_wu_item2", L"hdr_wu_date2", L"hdr_wu_status2",
        L"btn_upd_find", L"btn_upd_refresh", L"btn_upd_delete",
        // 9 文件关联
        L"lbl_as_imgtitle", L"lbl_as_imgdesc",
        L"btn_assoc_img_import", L"btn_assoc_img_export", L"btn_assoc_img_delete",
        L"lbl_as_livetitle", L"lbl_as_livedesc",
        L"assoc_progid", L"assoc_cmd",
        L"btn_assoc_live_import", L"btn_assoc_live_export", L"btn_assoc_live_reset",
        // 10 预应答
        L"btn_uc_add", L"btn_uc_up", L"btn_uc_del",
        L"btn_uc_import", L"btn_uc_export",
    };


    for (size_t i = 0; i < _countof(s_chromeTexts); ++i)
    {
        CDuiString sKey;
        sKey.Format(L"skin.%s", s_chromeTexts[i]);
        CDuiString sTr;
        if (StringMgr.TryGet(sKey.GetData(), sTr) && !sTr.IsEmpty())
            SetCtrlText(pm, s_chromeTexts[i], sTr.GetData());
    }


    // Edit 提示文字（tipvalue 属性），键规则 skin.tip_<控件名>
    static const wchar_t* s_tipEdits[] = {
        L"appx_filter", L"prog_filter", L"find_edit",
        L"feat_src_edit", L"uc_edit",
        // [R30] 可选功能页新增过滤框 + /Source 输入
        L"cap_filter_edit", L"cap_source_edit",
    };
    for (size_t i = 0; i < _countof(s_tipEdits); ++i)
    {
        CDuiString sKey;
        sKey.Format(L"skin.tip_%s", s_tipEdits[i]);
        CDuiString sTr;
        CControlUI* pCtrl = pm.FindControl(s_tipEdits[i]);
        if (pCtrl && StringMgr.TryGet(sKey.GetData(), sTr) && !sTr.IsEmpty())
            pCtrl->SetAttribute(L"tipvalue", sTr.GetData());
    }
}


// P2c：RegGetString 已迁 Registry.h/.cpp（去 static 全局函数，调用点零改动）


// common generated checkbox style (self-drawn box, left aligned, theme colors)
// P3b：去 static（声明在 FrameWnd_Util.h），供各 Page 共享
void StyleCheckBox(CCheckBoxUI* p, const wchar_t* pszText, int nHeight, const wchar_t* pszGlyphEmpty)
{
    // [BOX-CHECK] 文字里不再塞 "☐  " 字形前缀。
    //   字形方案的问题：U+2611（☑）被字体画成一块**实心方块**，空心方框的
    //   轮廓整个消失（= "选中之后边框消失"），颜色还只能跟 textcolor 走（近黑），
    //   跨主题/跨页面都不一致。改由 CBoxCheckUI 自绘方框，文字只留条目名。
    //   pszGlyphEmpty 保留在签名里是为了不改动 12 处调用点；自绘方框不再用它。
    (void)pszGlyphEmpty;
    p->SetText(pszText ? pszText : L"");
    p->SetFixedHeight(nHeight);
    p->SetTextColor(tokens::Token(tokens::EToken::Ink));
    p->SetAttribute(L"align", L"left");
    // 左边距为自绘方框让位（6 边距 + 14 方框 + 6 间隙 = 26）
    p->SetAttribute(L"textpadding", L"26,7,10,7");
    p->SetAttribute(L"hotbkcolor", tokens::TokenHex(tokens::EToken::SurfaceHover));   // [R235] 原 #FFF5F5F5
}


// get markup Item node inside optimization Group g, item index i
static CMarkupNode FindOptimItem(CMarkupNode root, int g, int i)
{
    int gg = 0;
    CMarkupNode group = root.GetChild(L"Group");
    while (group.IsValid())
    {
        if (gg == g)
        {
            int ii = 0;
            CMarkupNode item = group.GetChild(L"Item");
            while (item.IsValid())
            {
                if (ii == i) return item;
                ++ii;
                item = item.GetSibling();
            }
            return item;
        }
        ++gg;
        group = group.GetSibling();
    }
    return group;
}


// item-level Applicable box: false means this item does not apply to this OS


// body height = sum of its currently visible fixed-height rows.
// NOTE: a hidden parent marks children m_bInternVisible=false on Add(),
// which would make IsVisible() lie; temporarily unblock intern visibility.
void RecalcBodyHeight(CVerticalLayoutUI* pBody)
{
    if (!pBody) return;
    // [B92] GetFixedHeight() returns the DPI-SCALED (physical) height, while
    // SetFixedHeight() stores a LOGICAL value that the layout engine scales
    // again. Summing scaled rows here produced bodies like 9*49=441 stored as
    // logical -> laid out at 772px, leaving a big blank gap under each group.
    // Convert back to logical heights before summing (no-op at 100% DPI).
    CDPI* pDpi = (pBody->GetManager() != nullptr) ? pBody->GetManager()->GetDPIObj() : nullptr;
    int cy = 0;
    for (int k = 0; k < pBody->GetCount(); ++k)
    {
        CControlUI* pRow = pBody->GetItemAt(k);
        if (!pRow) continue;
        pRow->SetInternVisible(true);
        if (pRow->IsVisible())
        {
            int cyRow = pRow->GetFixedHeight();
            if (pDpi != nullptr) cyRow = pDpi->ScaleBack(cyRow);
            cy += cyRow;
        }
    }
    bool bOn = pBody->IsVisible();
    for (int k = 0; k < pBody->GetCount(); ++k)
    {
        CControlUI* pRow = pBody->GetItemAt(k);
        if (pRow) pRow->SetInternVisible(bOn);
    }
    pBody->SetFixedHeight(cy);
    // [R213] 诊断：优化页组体高度是否被算成 0（0 会被折叠逻辑当成"无可显示项"
    //   而拒绝展开，界面表现为整页空白）。这里是自由函数，取不到 CFrameWnd 的
    //   日志通道，走 OutputDebugString，同时把结果缓存到组体名字上供上层读取。
    {
        const CDuiString& sName = pBody->GetName();
        if (sName.Find(L"optbody_") >= 0)
        {
            wchar_t szDbg[256] = {};
            _snwprintf_s(szDbg, _countof(szDbg), _TRUNCATE,
                         L"[dism++] [optim] %s rows=%d h=%d\n",
                         sName.GetData(), static_cast<int>(pBody->GetCount()), cy);
            ::OutputDebugStringW(szDbg);
        }
    }
}


// collect Warning texts from the branch we are about to activate
static void CollectWarningTexts(CMarkupNode nodeItem, int nScope, bool bForCheck,
                                int iComboSel, std::vector<CDuiString>& out)
{
    if (nScope < 0)
    {
        // [SCOPE-2COL] 只收集界面可见列（当前用户/系统）；本函数目前无外部调用者，
        for (int s : optim_scope::kColumns)
            CollectWarningTexts(nodeItem, s, bForCheck, iComboSel, out);
        return;
    }


    CMarkupNode box = nodeItem.GetChild(DismScopeName(nScope));
    if (!box.IsValid()) return;


    if (iComboSel < 0)
    {
        CMarkupNode branch = box.GetChild(bForCheck ? L"True" : L"False");
        if (!branch.IsValid()) return;
        CMarkupNode w = branch.GetChild(L"Warning");
        while (w.IsValid())
        {
            CDuiString sw = DismResolveText(w.GetValue());
            if (!sw.IsEmpty()) out.push_back(sw);
            w = w.GetSibling();
        }
    }
    else
    {
        int n = 0;
        CMarkupNode d = box.GetChild(L"Dropdown");
        while (d.IsValid())
        {
            if (n == iComboSel)
            {
                CMarkupNode w = d.GetChild(L"Warning");
                while (w.IsValid())
                {
                    CDuiString sw = DismResolveText(w.GetValue());
                    if (!sw.IsEmpty()) out.push_back(sw);
                    w = w.GetSibling();
                }
            }
            ++n;
            d = d.GetSibling();
        }
    }
}


// P3b：去 static（声明在 FrameWnd_Util.h），供启动页等共享
bool ParsePrefix1(const CDuiString& sName, const wchar_t* pszPrefix, int& a)
{
    size_t n = wcslen(pszPrefix);
    if (wcsncmp(sName.GetData(), pszPrefix, n) != 0) return false;
    a = _ttoi(sName.Mid(static_cast<int>(n)).GetData());
    return true;
}


// case-insensitive substring test


// ============================================================
// [U-3] 左侧导航自绘行（NavOption）
// ============================================================
// 24x24 设计空间圆角矩形
static void NavMakeRoundRect(Gdiplus::GraphicsPath& p,
                             Gdiplus::REAL x, Gdiplus::REAL y,
                             Gdiplus::REAL w, Gdiplus::REAL h, Gdiplus::REAL r)
{
    using namespace Gdiplus;
    p.Reset();
    p.AddArc(x, y, 2*r, 2*r, 180.0f, 90.0f);
    p.AddArc(x + w - 2*r, y, 2*r, 2*r, 270.0f, 90.0f);
    p.AddArc(x + w - 2*r, y + h - 2*r, 2*r, 2*r, 0.0f, 90.0f);
    p.AddArc(x, y + h - 2*r, 2*r, 2*r, 90.0f, 90.0f);
    p.CloseFigure();
}
// Win11 Fluent 线性图标：统一 1.9px 圆角笔画，24x24 设计网格，内容居 3..21。
static void PaintNavIcon(Gdiplus::Graphics& g, int kind, Gdiplus::Color col)
{
    using namespace Gdiplus;
    Pen pen(col, 1.9f);
    pen.SetStartCap(LineCapRound); pen.SetEndCap(LineCapRound);
    pen.SetLineJoin(LineJoinRound);
    Pen penThin(col, 1.45f);
    penThin.SetStartCap(LineCapRound); penThin.SetEndCap(LineCapRound);
    SolidBrush br(col);
    switch (kind)
    {
    case 0: // 空间回收 —— 扫帚
        {
            g.DrawLine(&pen, 15.4f, 4.4f, 8.4f, 13.2f);
            PointF head[4] = {
                PointF(6.2f, 13.0f), PointF(12.6f, 15.6f),
                PointF(10.2f, 20.8f), PointF(4.4f, 16.6f) };
            g.FillPolygon(&br, head, 4);
            g.DrawLine(&penThin, 6.6f, 17.6f, 4.8f, 20.2f);
            g.DrawLine(&penThin, 9.0f, 19.0f, 7.8f, 21.6f);
        }
        break;
    case 1: // 启动项管理 —— 启动窗口（播放）
        {
            GraphicsPath rc;
            NavMakeRoundRect(rc, 3.5f, 5.0f, 17.0f, 13.0f, 2.2f);
            g.DrawPath(&pen, &rc);
            PointF play[3] = {
                PointF(10.0f, 8.9f), PointF(10.0f, 14.1f), PointF(14.6f, 11.5f) };
            g.FillPolygon(&br, play, 3);
        }
        break;
    case 2: // Appx 管理 —— 四宫格
        {
            GraphicsPath a, b, c, d;
            NavMakeRoundRect(a, 4.0f, 4.0f, 7.2f, 7.2f, 1.8f);
            NavMakeRoundRect(b, 12.8f, 4.0f, 7.2f, 7.2f, 1.8f);
            NavMakeRoundRect(c, 4.0f, 12.8f, 7.2f, 7.2f, 1.8f);
            NavMakeRoundRect(d, 12.8f, 12.8f, 7.2f, 7.2f, 1.8f);
            g.DrawPath(&pen, &a); g.DrawPath(&pen, &b);
            g.DrawPath(&pen, &c); g.DrawPath(&pen, &d);
        }
        break;
    case 3: // 工具箱 —— 手提工具箱
        {
            GraphicsPath box;
            NavMakeRoundRect(box, 3.5f, 8.2f, 17.0f, 12.3f, 2.0f);
            g.DrawPath(&pen, &box);
            g.DrawLine(&pen, 9.6f, 8.2f, 9.6f, 5.4f);
            g.DrawArc(&pen, 9.6f, 3.0f, 4.8f, 3.8f, 180.0f, 180.0f);
            g.DrawLine(&pen, 14.4f, 5.0f, 14.4f, 8.2f);
            g.DrawLine(&penThin, 12.0f, 11.6f, 12.0f, 14.6f);
        }
        break;
    case 4: // 系统优化 —— 仪表盘
        {
            g.DrawArc(&pen, 4.0f, 5.0f, 16.0f, 16.0f, 150.0f, 240.0f);
            g.DrawLine(&penThin, 5.6f, 9.2f, 7.2f, 10.6f);
            g.DrawLine(&penThin, 12.0f, 6.6f, 12.0f, 8.4f);
            g.DrawLine(&penThin, 18.4f, 10.6f, 16.8f, 9.2f);
            g.FillEllipse(&br, 10.9f, 11.9f, 2.2f, 2.2f);
            g.DrawLine(&pen, 12.0f, 13.0f, 15.6f, 9.4f);
        }
        break;
    case 5: // 驱动管理 —— 芯片
        {
            GraphicsPath chip;
            NavMakeRoundRect(chip, 7.5f, 7.5f, 9.0f, 9.0f, 1.4f);
            g.DrawPath(&pen, &chip);
            g.DrawEllipse(&penThin, 10.6f, 10.6f, 2.8f, 2.8f);
            const REAL pins[3] = { 10.0f, 12.0f, 14.0f };
            for (int i = 0; i < 3; ++i) {
                g.DrawLine(&penThin, pins[i], 7.5f, pins[i], 4.9f);
                g.DrawLine(&penThin, pins[i], 16.5f, pins[i], 19.1f);
                g.DrawLine(&penThin, 7.5f, pins[i], 4.9f, pins[i]);
                g.DrawLine(&penThin, 16.5f, pins[i], 19.1f, pins[i]);
            }
        }
        break;
    case 6: // 程序和功能 —— 窗口 + 列表行
        {
            GraphicsPath rc;
            NavMakeRoundRect(rc, 4.0f, 4.0f, 16.0f, 16.0f, 2.0f);
            g.DrawPath(&pen, &rc);
            g.DrawLine(&pen, 4.0f, 8.8f, 20.0f, 8.8f);
            g.DrawLine(&penThin, 7.5f, 12.8f, 16.5f, 12.8f);
            g.DrawLine(&penThin, 7.5f, 15.8f, 16.5f, 15.8f);
        }
        break;
    case 7: // 可选功能 —— 三块 + 加号
        {
            GraphicsPath a, b, c;
            NavMakeRoundRect(a, 4.0f, 4.0f, 7.0f, 7.0f, 1.6f);
            NavMakeRoundRect(b, 13.0f, 4.0f, 7.0f, 7.0f, 1.6f);
            NavMakeRoundRect(c, 4.0f, 13.0f, 7.0f, 7.0f, 1.6f);
            g.DrawPath(&pen, &a); g.DrawPath(&pen, &b); g.DrawPath(&pen, &c);
            g.DrawLine(&pen, 14.4f, 17.8f, 19.4f, 17.8f);
            g.DrawLine(&pen, 16.9f, 15.3f, 16.9f, 20.3f);
        }
        break;
    case 8: // 更新管理 —— 双向同步圆弧
        {
            g.DrawArc(&pen, 4.0f, 4.0f, 16.0f, 16.0f, 40.0f, 250.0f);
            g.DrawLine(&pen, 14.9f, 4.7f, 12.5f, 5.2f);
            g.DrawLine(&pen, 14.9f, 4.7f, 14.5f, 7.1f);
            g.DrawArc(&pen, 4.0f, 4.0f, 16.0f, 16.0f, 220.0f, 250.0f);
            g.DrawLine(&pen, 9.1f, 19.3f, 11.5f, 18.8f);
            g.DrawLine(&pen, 9.1f, 19.3f, 9.5f, 16.9f);
        }
        break;
    case 9: // 文件关联 —— 链节
        {
            GraphicsState gs1 = g.Save();
            g.TranslateTransform(9.2f, 9.2f);
            g.RotateTransform(45.0f);
            GraphicsPath l1;
            NavMakeRoundRect(l1, -5.2f, -2.3f, 10.4f, 4.6f, 2.3f);
            g.DrawPath(&pen, &l1);
            g.Restore(gs1);
            GraphicsState gs2 = g.Save();
            g.TranslateTransform(14.8f, 14.8f);
            g.RotateTransform(45.0f);
            GraphicsPath l2;
            NavMakeRoundRect(l2, -5.2f, -2.3f, 10.4f, 4.6f, 2.3f);
            g.DrawPath(&pen, &l2);
            g.Restore(gs2);
        }
        break;
    case 10: // 预应答 —— 清单
        {
            GraphicsPath doc;
            NavMakeRoundRect(doc, 5.5f, 3.0f, 13.0f, 18.0f, 1.8f);
            g.DrawPath(&pen, &doc);
            PointF fold[3] = {
                PointF(13.5f, 3.0f), PointF(18.5f, 7.6f), PointF(13.5f, 7.6f) };
            g.DrawLines(&penThin, fold, 3);
            g.DrawLine(&pen, 8.5f, 11.6f, 9.9f, 13.0f);
            g.DrawLine(&pen, 9.9f, 13.0f, 12.4f, 10.2f);
            g.DrawLine(&pen, 8.5f, 16.0f, 9.9f, 17.4f);
            g.DrawLine(&pen, 9.9f, 17.4f, 12.4f, 14.6f);
        }
        break;
    default:
        g.FillEllipse(&br, 9.0f, 9.0f, 6.0f, 6.0f);
        break;
    }
}
// 左侧导航行：COptionUI 负责选中/底色/文字/布局，图标槽改由 GDI+ 自绘。
// 图标色随状态切换，修复旧 nav_*.png 在选中蓝底上仍为蓝色（几乎不可见）。
class CNavOptionUI : public COptionUI
{
    DECLARE_DUICONTROL(CNavOptionUI)
public:
    CNavOptionUI() : m_nIcon(0), m_bHot(false) {}
    virtual void SetAttribute(const wchar_t* pstrName, const wchar_t* pstrValue)
    {
        if (lstrcmpiW(pstrName, L"navicon") == 0)
            m_nIcon = _ttoi(pstrValue);
        else
            COptionUI::SetAttribute(pstrName, pstrValue);
    }
    virtual void DoEvent(TEventUI& event)
    {
        if (event.Type == UIEVENT_MOUSEENTER) {
            if (!m_bHot) { m_bHot = true; Invalidate(); }
        }
        else if (event.Type == UIEVENT_MOUSELEAVE) {
            if (m_bHot) { m_bHot = false; Invalidate(); }
        }
        COptionUI::DoEvent(event);
    }
    virtual bool DoPaint(HDC hDC, const RECT& rcPaint, CControlUI* pStopControl)
    {
        PaintPillBg(hDC);   // Win11 NavView：hover 灰丸 / 选中蓝丸
        bool bRet = COptionUI::DoPaint(hDC, rcPaint, pStopControl);
        PaintNavSlot(hDC);
        return bRet;
    }
private:
    int  m_nIcon;
    bool m_bHot;
    int Dp(int v) const
    {
        return (m_pManager != nullptr) ? m_pManager->GetDPIObj()->Scale(v) : v;
    }
    // Win11 NavView 选中态：圆角药丸（满蓝矩形是 Dism++ 旧风格）
    void PaintPillBg(HDC hDC)
    {
        using namespace Gdiplus;
        ARGB argb = 0;
        // [R201] 选中药丸跟随系统强调色：用户改了主题色，导航也跟着变，
        //   否则"设置里的主题色"只在部分控件生效，观感割裂。
        if (IsEnabled() && IsSelected())      argb = tokens::Token(tokens::EToken::Accent);
        else if (IsEnabled() && m_bHot)       argb = tokens::Token(tokens::EToken::SurfaceHover);   // [R204] 深色下不能是亮灰
        if (argb == 0) return;
        Graphics g(hDC);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        const REAL r = (REAL)Dp(6);
        RectF rc((REAL)(m_rcItem.left + Dp(6)) + 0.5f,
                 (REAL)(m_rcItem.top  + Dp(2)) + 0.5f,
                 (REAL)(m_rcItem.right - m_rcItem.left - Dp(10)) - 1.0f,
                 (REAL)(m_rcItem.bottom - m_rcItem.top - Dp(4)) - 1.0f);
        GraphicsPath path;
        const REAL d = r * 2.0f;
        path.AddArc(rc.X, rc.Y, d, d, 180.0f, 90.0f);
        path.AddArc(rc.GetRight() - d, rc.Y, d, d, 270.0f, 90.0f);
        path.AddArc(rc.GetRight() - d, rc.GetBottom() - d, d, d, 0.0f, 90.0f);
        path.AddArc(rc.X, rc.GetBottom() - d, d, d, 90.0f, 90.0f);
        path.CloseFigure();
        SolidBrush br((Color(argb)));  // 双括号防 most-vexing-parse
        g.FillPath(&br, &path);
    }
    void PaintNavSlot(HDC hDC)
    {
        using namespace Gdiplus;
        Color col;
        if (!IsEnabled())            col = Color(255, 154, 154, 154);  // #9A9A9A
        else if (IsSelected())       col = Color(255, 255, 255, 255);  // 选中蓝底 → 白
        else if (m_bHot)             col = Color(255, 28, 151, 234);   // hover #1C97EA
        else                         col = Color(255, 0, 120, 212);    // 默认 #0078D4
        const int nSize = Dp(18);
        Graphics g(hDC);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.TranslateTransform((REAL)(m_rcItem.left + Dp(10)),
                             (REAL)(m_rcItem.top  + Dp(9)));
        g.ScaleTransform((REAL)nSize / 24.0f, (REAL)nSize / 24.0f);
        PaintNavIcon(g, m_nIcon, col);
    }
};
IMPLEMENT_DUICONTROL(CNavOptionUI)
void RegisterWin8MetroControls()
{
    // [U-3] UIDlgBuilder 会把 XML 标签名拼成 "C<Name>UI" 再查工厂：
    // <NavOption> → "CNavOptionUI"（键内部会 MakeLower）。
    CControlFactory::GetInstance()->RegistControl(
        L"CNavOptionUI", (CreateClass)CNavOptionUI::CreateControl);
    // [BOX-CHECK] <BoxCheck> → "CBoxCheckUI"：自绘三态方框的复选框。
    //   皮肤里的 <CheckBox> 换成 <BoxCheck> 后，XML 控件与代码创建的控件
    //   用同一套自绘实现，避免"一个带框一个实心块"的不一致。
    CControlFactory::GetInstance()->RegistControl(
        L"CBoxCheckUI", (CreateClass)CBoxCheckUI::CreateControl);
}
// ============================================================
// window
// ============================================================
CFrameWnd::CFrameWnd(const wchar_t* pszXMLPath)
    : m_strXMLPath(pszXMLPath)
    , m_nDPI(96)
    , m_uTimerID(1)
    , m_bPopulating(false)
    // P6b：扫描/删除态、取消令牌、工作线程句柄已随管线迁 CleanJobService；
    // 清理页 UI 状态（m_bCleanScanned/m_vUiSize/过滤/报告等）迁 CPageCleanup。
    , m_bBufferFlushed(false)
    // [LAG-FIX] 日志批量/延迟构建状态初始化
    , m_bLogFlushArmed(false)
    , m_nPendingBuildPage(-1)
    // P7：m_bFiltering/m_nOptimStateFilter/m_dwOptimVerifyTick 已随系统优化页迁 CPageOptim
    // P4：m_bAppxLoading/m_bCapLoading/m_bUpdLoading 已随三页迁走
    // P5：m_bFeatLoaded/m_nFeatTicks/m_nFeatSelected 已随 CPageFeature 迁走 =>
    , m_nAfterAction(0)
    , m_nLanguage(0)
    // [R4] 17 个具体页对象由 CPageHost 统一装配（构造只保存 IPageContext&，
    // 此时窗口尚未就绪，页构造内不做任何调用）
    , m_pages(*this)
    // P5：m_bDrvPubEnum 已随 CPageDriver 迁走（默认 false 在页构造里）
    // P3b：m_bStartupHideDisabled 已随 CPageStartup 迁走（默认 false 在页构造里）
    // P4：m_nUpdSelected/m_bCapRetryPending 已随 CPageUpdate/CPageCapability 迁走
    , m_bCapRefreshPending(false)
    , m_pSessProgress(nullptr)
    // P7：m_pOptimProgress/m_dwLastOptimMs/m_nOptimPendingVal/m_nOptimPendingMax 已迁 CPageOptim
    , m_bPageBuilt()
    , m_bTrayAdded(false)
    , m_hwndTip(nullptr)
    , m_bFocusGuard(false)
    , m_pFocusCtrl(nullptr)
    , m_dwOldBorderColor(0)
    , m_nOldBorderSize(0)
    , m_bOldBorderSet(false)
    // P4：m_nAppxGroupMode 已随 CPageAppx 迁走
    , m_hToastWnd(nullptr)
    , m_uToastTimerId(0)
    , m_hfToast(nullptr)
    , m_nToastIcon(0)
    , m_dwStartupMs(0)    // [R51] 启动时间（毫秒）
{
    // Patch-4: init wimgapi bridge
    WIMBridge::Init();
    DismApiBridge::Init();
    // [R-拆分] 加载 CBSHost.dll（失败不中断启动；业务调用处会给出明确错误）
    {
        std::wstring sCbsErr;
        if (!CbsHostClient::Init(&sCbsErr)) {
            ::OutputDebugStringW((sCbsErr + L"\r\n").c_str());
            AppendLogWarn(sCbsErr.c_str());
        } else {
            AppendLog(L"[dep] CBSHost.dll loaded (6 exports)");
        }
    }
    // [R-拆分] 加载 NCleaner.dll（7 个 NC*Cleanup 导出；失败不中断启动，
    // 清理项分派处回退原生实现）
    {
        std::wstring sNcErr;
        if (!NCleanerClient::Init(&sNcErr)) {
            ::OutputDebugStringW((sNcErr + L"\r\n").c_str());
            AppendLogWarn(sNcErr.c_str());
        } else {
            AppendLog(L"[dep] NCleaner.dll loaded (7 exports)");
        }
    }
    // [R01] 加载 ActivationKit.dll（3 导出；激活备份/还原卡片用）。
    // 失败不中断启动：工具箱该卡片置灰，其余功能不受影响。
    {
        std::wstring sActErr;
        if (!ActivationKitClient::Init(&sActErr)) {
            ::OutputDebugStringW((sActErr + L"\r\n").c_str());
            AppendLogWarn(sActErr.c_str());
        } else {
            AppendLog(L"[dep] ActivationKit.dll loaded (3 exports)");
        }
    }
    // [R51] 记录构造函数起始时间，用于测量启动耗时
    m_dwStartupMs = ::GetTickCount();
    m_sNid = {};
    s_pToastOwner = this;
}


CFrameWnd::~CFrameWnd()
{
    DestroyToast();
    if (m_hfToast) { ::DeleteObject(m_hfToast); m_hfToast = nullptr; }
    RemoveTrayIcon();
    ::KillTimer(m_hWnd, m_uTimerID);


    // P6b：等待扫描/清理工作线程退出已下沉 CleanJobService::Join()，
    // 否则线程会在对象析构后继续访问 m_cleanEngine 并 PostMessage，
    // 造成 UAF / 消息对象泄漏。
    CleanJobService::Join();
    // [R111] 注册表清理后台线程同样需要等待退出
    RegCleanJobService::Join();
    // [FIX-P1K] DISM 尾巴线程（CBS 事务，Page_CleanupTail 后台化）不在
    // Join 集合：退出时若仍在途，强杀可能损坏组件存储。有限等待 5s +
    // 告警留痕；超时放弃（记录），避免退出序列挂死。
    if (CPageBase* pPg = PageById(static_cast<int>(EPageId::PAGE_CLEANUP)))
    {
        if (CPageCleanup* pClean = dynamic_cast<CPageCleanup*>(pPg))
        {
            if (pClean->IsCleanTailInFlight())
            {
                AppendLog(L"[exit] clean DISM tail in flight; waiting up to 5s");
                for (int i = 0; i < 50 && pClean->IsCleanTailInFlight(); ++i)
                    ::Sleep(100);
                if (pClean->IsCleanTailInFlight())
                    AppendLog(L"[exit][WARN] clean DISM tail still running; CBS transaction may be aborted");
            }
        }
    }
    // [P0] 卸载离线蜂巢（须在所有工作线程退出后；幂等）
    m_dataEngine.EndOfflineSession();
    AbortDism();
    // Patch-4: cleanup wimgapi bridge
    WIMBridge::Shutdown();
    DismApiBridge::Shutdown();
    // [R-拆分] 释放 CBSHost.dll
    CbsHostClient::Shutdown();
    // [R-拆分] 释放 NCleaner.dll
    NCleanerClient::Shutdown();
    // [R01] 释放 ActivationKit.dll（退出阶段不强制 FreeLibrary，见客户端说明）
    ActivationKitClient::Shutdown();
    // [R-B56] 外部命令防腐台账：应用退出前导出 CSV 审计快照
    // 清单维度 3 缺失 5『外部命令路线与"全内置"基线分叉』：
    // 「命令清单集中化防腐」——把本次运行所有 ProcessRunner 外部进程调用
    // 集中导出到 exe 同目录 external_cmd_ledger.csv（UTF-8 BOM，Excel 友好）。
    // 失败不阻断（best-effort），日志留痕。
    {
        wchar_t szExe[MAX_PATH] = {};
        ::GetModuleFileNameW(nullptr, szExe, MAX_PATH);
        std::wstring wsCsv(szExe);
        size_t p = wsCsv.find_last_of(L"\\/");
        if (p != std::wstring::npos) {
            wsCsv = wsCsv.substr(0, p + 1);
            wsCsv += L"external_cmd_ledger.csv";
        } else {
            wsCsv = L"external_cmd_ledger.csv";
        }
        bool bOk = ExternalCmdLedger::DumpCsv(wsCsv);
        std::map<std::wstring, int> m = ExternalCmdLedger::CountByCategory();
        if (bOk && !m.empty()) {
            std::wstring sLog;
            sLog.resize(512);
            wfmt::FormatInto(sLog.data(), sLog.size() + 1,
                         L"[rb56] external cmd ledger exported (categories=%u) -> %s",
                         (unsigned)m.size(), wsCsv.c_str());
            sLog.resize(wcslen(sLog.c_str()));
            AppendLog(sLog.c_str());
        }
    }
}


const wchar_t* CFrameWnd::GetWindowClassName() const { return L"DismppConsole"; }
namespace
{

// 单例式令牌名表：顺序与 tokens::EToken 一致，供解析与写表共用。
const wchar_t* const kTokenNames[] = {
    L"Accent", L"AccentHot", L"AccentPushed", L"AccentText",
    L"AccentSoft", L"AccentBorder",
    L"Surface", L"SurfaceAlt", L"SurfaceHover", L"SurfaceSunk",
    L"Border", L"BorderSoft",
    L"Ink", L"InkSecondary", L"InkTertiary", L"InkMuted",
    L"Danger", L"Success", L"Warn", L"WarnBg", L"OnAccent",
    L"HeaderBand", L"OnHeaderBand",
    L"DangerHot", L"DangerPushed", L"OnDanger",
    L"OnAccentHot", L"OnAccentPushed",
};
constexpr int kTokenCount = static_cast<int>(sizeof(kTokenNames) / sizeof(kTokenNames[0]));

// [R204] 名字表必须与枚举一一对应：上一版漏了 5 项，导致
//   {tok:Danger} 等被静默删成空串，控件拿到空颜色不报错、只是不显示。
static_assert(kTokenCount == static_cast<int>(tokens::EToken::Count),
              "kTokenNames must match tokens::EToken::Count");

}   // namespace

// [R202] 皮肤令牌化：GetSkinFile() 是 DuiLib 在解析皮肤前调用的唯一入口。
//
// DuiLib 没有"皮肤变量"机制（UIManager / UIDlgBuilder 里都没有 {..} 替换），
// 所以走"预展开"路线：
//   1) 读磁盘皮肤 XML（连同 Include 的文件），把 {tok:X} 就地换成 "#AARRGGBB"；
//   2) 把展开后的整段 XML 文本返回 —— CDialogBuilder::Create 见到首字符是
//      '<' 就按"XML 字符串"处理（UIDlgBuilder.cpp:23），不再回读文件。
//
// 这样根皮肤与所有 <Include> 统一展开，且不产生临时文件。
CDuiString CFrameWnd::GetSkinFile()
{
    // 展开失败（读不到文件/替换出错）就回退原路径：宁可少一层换肤，
    // 也不能因为令牌化让整个界面加载失败。
    m_sExpandedSkin.Empty();

    std::wstring sRoot(CPaintManagerUI::GetResourcePath().GetData());
    if (!sRoot.empty() && sRoot.back() != L'\\' && sRoot.back() != L'/')
        sRoot += L'\\';
    sRoot += m_strXMLPath.GetData();

    std::wstring sExpanded;
    if (!ExpandSkinFile(sRoot.c_str(), sExpanded, 0))
    {
        AppendLog(L"[skin] expand failed, fallback to file");
        return m_strXMLPath;
    }

    // [R204] log expanded size so we can tell whether it fell back
    {
        CDuiString sLog;
        sLog.Format(L"[skin] expanded %d chars", static_cast<int>(sExpanded.size()));
        AppendLog(sLog.GetData());
    }

    m_sExpandedSkin = sExpanded.c_str();
    return m_sExpandedSkin;
}

// 递归展开皮肤：读文本 → 递归处理 <Include source="..."/> → 替换 {tok:X}。
//   nDepth 上限 8：皮肤出现循环 Include 时不会无限递归。
// ============================================================
// [R242] 从皮肤 DLL 读一个皮肤文件
//
// 为什么放在 ExpandSkinFile 前面、且做成文件级 static：
//   它是"读字节"这一层的实现细节，不需要进头文件，也不该被其它页调用。
//   只暴露给 ExpandSkinFile 一个入口，改皮肤来源时只有一处需要看。
//
// 路径约定：
//   磁盘路径是 "<资源目录>\<皮肤目录>\Main.xml"，DLL 内的键是相对皮肤的
//   "Main.xml" / "pages/page_cleanup.xml"。这里从磁盘路径里截出"皮肤根之后"
//   的那一段作为键，这样两边共用同一份路径拼装逻辑，不需要第二套命名。
//
// 线程安全：
//   DllMain 由系统持有，这里只在首次调用时加载一次（函数内 static 初始化在
//   C++11 后是线程安全的）。之后只有只读的函数指针调用，无锁竞争。
// ============================================================
namespace
{
    // 皮肤 DLL 的导出签名。与 Demos/SkinThemeShared/SkinThemeDll.h 保持一致；
    // 不在 exe 侧 include 那个头，是为了让"皮肤包缺失"这件事在编译期就不可见 ——
    // 少了 DLL 也应当能编译，只是运行期回退到磁盘皮肤。
    typedef int   (*PFN_ABI)(void);
    typedef int   (*PFN_COUNT)(void);
    typedef const unsigned char* (*PFN_GET)(int nSlot, const wchar_t* pszName,
                                            unsigned int* pcbBytes);
    constexpr int kSkinAbiExpected = 1;

    struct TSkinDll
    {
        HMODULE    h = nullptr;
        PFN_GET    pfnGet = nullptr;
        int        nAbi = 0;
        bool       bTried = false;   // 只尝试加载一次，失败不反复 LoadLibrary

        void Ensure();
    };

    void TSkinDll::Ensure()
    {
        if (bTried) return;
        bTried = true;

        // 与 exe 同目录：皮肤是随程序发布的资源，不应当去 System32 找。
        wchar_t szExe[MAX_PATH] = {};
        if (::GetModuleFileNameW(nullptr, szExe, _countof(szExe)) == 0) return;
        wchar_t* pSlash = ::wcsrchr(szExe, L'\\');
        if (!pSlash) return;
        *(pSlash + 1) = L'\0';

        // amd64 子目录优先（本项目的依赖都部署在那里），再退回 exe 同目录。
        const wchar_t* aSub[] = { L"amd64\\SkinThemeDll.dll", L"SkinThemeDll.dll" };
        for (int i = 0; i < _countof(aSub) && !h; ++i)
        {
            std::wstring sPath(szExe);
            sPath += aSub[i];
            h = ::LoadLibraryW(sPath.c_str());
        }
        if (!h) return;   // 没部署：静默回退磁盘皮肤，不打扰用户

        PFN_ABI pfnAbi = reinterpret_cast<PFN_ABI>(::GetProcAddress(h, "SkinTheme_ABI"));
        pfnGet = reinterpret_cast<PFN_GET>(::GetProcAddress(h, "SkinTheme_Get"));
        PFN_COUNT pfnCount =
            reinterpret_cast<PFN_COUNT>(::GetProcAddress(h, "SkinTheme_Count"));
        if (!pfnAbi || !pfnGet || !pfnCount)
        {
            ::FreeLibrary(h); h = nullptr; pfnGet = nullptr;
            return;
        }

        nAbi = pfnAbi();
        if (nAbi != kSkinAbiExpected)
        {
            // ABI 不符必须整包拒绝：老包里的属性名/结构可能与当前解析器不符，
            // 静默加载会表现为"皮肤加载成功但界面错乱"，比直接回退难查得多。
            ::FreeLibrary(h); h = nullptr; pfnGet = nullptr;
            return;
        }

        // 零资源自检：一个 0 文件的包加载后界面全空白，且不会有任何报错。
        if (pfnCount() <= 0)
        {
            ::FreeLibrary(h); h = nullptr; pfnGet = nullptr;
            return;
        }
    }

    TSkinDll g_skinDll;
}

// 把磁盘皮肤路径转成 DLL 内的相对键。
//   "<任意前缀>\skin\Classic\pages\page_x.xml" -> "pages/page_x.xml"
//   规则：找到路径里最后一次出现的皮肤目录名（默认 "Classic"），取其后半段。
//   用"最后出现"而不是"第一次出现"，是为了容忍安装路径本身含同名目录。
static bool SkinKeyFromPath(const wchar_t* pszPath, std::wstring& outKey)
{
    if (!pszPath || !pszPath[0]) return false;

    // [CLASSIC-SKIN] 皮肤目录名从 GetSkinFolder 的约定来；这里硬编码与 bin64/skin/Classic 一致。
    // 若将来换皮肤目录，只需改这一处。
    static const wchar_t kSkinRoot[] = L"Classic";
    const wchar_t* p = pszPath;
    const wchar_t* pFound = nullptr;
    while ((p = ::wcsstr(p, kSkinRoot)) != nullptr)
    {
        pFound = p;
        p += _countof(kSkinRoot) - 1;
    }
    if (!pFound) return false;

    p = pFound + _countof(kSkinRoot) - 1;   // 指向皮肤根目录名之后
    while (*p == L'\\' || *p == L'/') ++p;
    if (!*p) return false;

    outKey.assign(p);
    for (wchar_t& ch : outKey) if (ch == L'\\') ch = L'/';
    return !outKey.empty();
}

bool CFrameWnd::ReadSkinFromDll(const wchar_t* pszPath, std::string& outBytes)
{
    g_skinDll.Ensure();
    if (!g_skinDll.pfnGet) return false;

    std::wstring sKey;
    if (!SkinKeyFromPath(pszPath, sKey)) return false;

    // 槽位按当前生效的深浅色选：这是"DLL 化"与"令牌系统"唯一的交点 ——
    // 令牌决定颜色，槽位决定用哪一份 XML。两者互不干扰。
    const int nSlot = tokens::IsDark() ? 1 : 0;

    unsigned int cb = 0;
    const unsigned char* p = g_skinDll.pfnGet(nSlot, sKey.c_str(), &cb);
    if (!p || cb == 0) return false;

    outBytes.assign(reinterpret_cast<const char*>(p), cb);
    return true;
}

bool CFrameWnd::ExpandSkinFile(const wchar_t* pszPath, std::wstring& outXml, int nDepth)
{
    if (!pszPath || nDepth > 8) return false;

    // [R242] 皮肤来源：DLL 优先，磁盘回退。
    //
    // 皮肤 DLL（SkinThemeDll.dll）把两套皮肤的 XML 与图片编译进二进制，
    // 每套是一个独立的发布单元。这里只替换"读字节"这一步，
    // 后面的 Include 递归 / BOM 剥离 / Window 外壳剥离 / {tok:} 展开全部保持原样。
    //
    // 回退条件（任一成立就退回磁盘读文件）：
    //   · DLL 不存在或加载失败（没部署 / 被杀软拦）
    //   · ABI 版本不匹配（皮肤格式变更后仍加载老包会解析出错）
    //   · 包里没有这个文件（新增页面但皮肤包未重新生成）
    // 这样"DLL 化出问题"只会退化成旧行为，不会让界面打不开 —— 崩溃面为零。
    std::string sRaw;
    if (!ReadSkinFromDll(pszPath, sRaw) || sRaw.empty())
    {
        std::ifstream ifs(pszPath, std::ios::binary);
        if (!ifs.is_open()) return false;
        sRaw.assign((std::istreambuf_iterator<char>(ifs)),
                    std::istreambuf_iterator<char>());
        ifs.close();
        if (sRaw.empty()) return false;
    }

    // UTF-8 → 宽字符
    const int nW = ::MultiByteToWideChar(CP_UTF8, 0, sRaw.data(),
                                         static_cast<int>(sRaw.size()), nullptr, 0);
    if (nW <= 0) return false;
    std::wstring sText(static_cast<size_t>(nW), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, sRaw.data(), static_cast<int>(sRaw.size()),
                          &sText[0], nW);

    // BOM 会残留成 U+FEFF，DuiLib 解析首字符时会因此判成"文件路径"
    // 而不是 XML 字符串 —— 必须去掉。
    if (!sText.empty() && sText.front() == 0xFEFF) sText.erase(sText.begin());

    // [R204-FIX] 去掉子文件自带的<?xml ...?> 声明。
    //   为什么必须去：pages/*.xml 每个文件都以 XML 声明开头，被内联进
    //   Main.xml 后，这份声明就落到"非文档开头"的位置 —— 严格的 XML 解析器
    //   直接报 "XML or text declaration not at start of entity"，整份皮肤
    //   解析失败、界面空白。根文件自己那一行保留（它在最前面，合法）。
    // [R205] 子文件要剥两层"文件包装"，这是页面加载失败的真根因：
    //
    //   pages/*.xml 的结构是：<?xml?>, <Window>, 内容, </Window>。
    //   内联进 Main.xml 后：
    //     1) <?xml?> 落在非首行 -> 严格解析器直接报错；
    //     2) <Window> 嵌套在父 <Window> 里 -> DuiLib 把它当成"子窗口
    //        节点"，16 个页面全部挂不上去，content_area 实际为空。
    //   实测展开后出现 16 个嵌套 <Window>，TabLayout 子节点全是 Window。
    //   所以子文件只保留最外层 Window 内部的节点。
    if (nDepth > 0)
    {
        // 1) 去 XML 声明
        const size_t nDecl = sText.find(L"<?xml");
        if (nDecl != std::wstring::npos)
        {
            const size_t nDeclEnd = sText.find(L"?>", nDecl);
            if (nDeclEnd != std::wstring::npos)
                sText.erase(nDecl, nDeclEnd + 2 - nDecl);
        }

        // 2) 去 <Window ...> ... </Window> 外壳（只剥一层）
        const size_t nWinOpen = sText.find(L"<Window");
        if (nWinOpen != std::wstring::npos)
        {
            const size_t nWinOpenEnd = sText.find(L'>', nWinOpen);
            const size_t nWinClose = sText.rfind(L"</Window>");
            if (nWinOpenEnd != std::wstring::npos
                && nWinClose != std::wstring::npos
                && nWinClose > nWinOpenEnd)
            {
                sText = sText.substr(nWinOpenEnd + 1, nWinClose - nWinOpenEnd - 1);
            }
        }
    }

    // ---- 递归处理 <Include source="..."/> ----
    size_t nPos = 0;
    while ((nPos = sText.find(L"<Include", nPos)) != std::wstring::npos)
    {
        const size_t nEnd = sText.find(L"/>", nPos);
        if (nEnd == std::wstring::npos) break;

        const size_t nSrcKey = sText.find(L"source=\"", nPos);
        if (nSrcKey == std::wstring::npos || nSrcKey > nEnd)
        {
            nPos = nEnd + 2;
            continue;
        }
        const size_t nSrcBegin = nSrcKey + 8;                 // strlen("source=\"")
        const size_t nSrcEnd = sText.find(L'\"', nSrcBegin);
        if (nSrcEnd == std::wstring::npos || nSrcEnd > nEnd)
        {
            nPos = nEnd + 2;
            continue;
        }
        const std::wstring sRel = sText.substr(nSrcBegin, nSrcEnd - nSrcBegin);

        std::wstring sDir(pszPath);
        const size_t nSlash = sDir.find_last_of(L"\\/");
        if (nSlash != std::wstring::npos) sDir.resize(nSlash + 1);
        const std::wstring sSub = sDir + sRel;

        std::wstring sSubXml;
        if (ExpandSkinFile(sSub.c_str(), sSubXml, nDepth + 1))
        {
            sText.replace(nPos, nEnd + 2 - nPos, sSubXml);
            nPos += sSubXml.size();
        }
        else
        {
            // 子文件缺失：保留原标签，让 DuiLib 自己报错，不静默吞掉结构
            nPos = nEnd + 2;
        }
    }

    // ---- 替换 {tok:X} ----
    nPos = 0;
    while ((nPos = sText.find(L"{tok:", nPos)) != std::wstring::npos)
    {
        const size_t nClose = sText.find(L'}', nPos);
        if (nClose == std::wstring::npos) break;
        const std::wstring sName = sText.substr(nPos + 5, nClose - nPos - 5);

        bool bFound = false;
        for (int i = 0; i < kTokenCount; ++i)
        {
            if (sName != kTokenNames[i]) continue;
            wchar_t szHex[12] = {};
            wfmt::FormatInto(szHex, _countof(szHex), L"#%08lX",
                             static_cast<unsigned long>(
                                 tokens::Token(static_cast<tokens::EToken>(i))));
            sText.replace(nPos, nClose + 1 - nPos, szHex);
            nPos += wcslen(szHex);
            bFound = true;
            break;
        }
        if (!bFound)
        {
            // 未知令牌：删掉占位符本身，避免把 "{tok:Typo}" 当颜色交给 DuiLib
            sText.erase(nPos, nClose + 1 - nPos);
        }
    }

    outXml.swap(sText);
    return true;
}
CDuiString CFrameWnd::GetSkinFolder() { return L""; }


void CFrameWnd::setDPI(int dpi) { m_nDPI = dpi; m_pm.SetDPI(dpi); }


// [R5-B1] Per-Monitor V2 完整响应：
//  1) 更新 m_nDPI + m_pm.SetDPI  → 让后续所有 CDPI::Pixel/DPIValue 用新 DPI
//  2) m_pm.ResetDPIAssets()       → 重建已缓存字体/位图（旧缓存基于旧 DPI）
//  3) ::SetWindowPos 应用系统建议矩形 → 保持跨显示器比例一致
//  4) NeedUpdate 根 + 显式 WM_SIZE 广播 → 让 CVerticalLayout / CHorizontalLayout
//     按新 DPI 重排所有子控件（这是 DuiLib 里 Layout 类唯一的重排入口）
//  5) UpdateWindow → 立即同步重绘，避免闪烁（跨 DPI 时系统会等 2-3 帧）
// ============================================================
// [R201] 修正皮肤字体族。
//
// 问题（实测）：Main.xml 的 6 个 <Font> 全部写 name="Microsoft YaHei"，
//   但用 GDI 回读实际选中字面名得到 "微软雅黑" —— 说明系统里没有
//   "Microsoft YaHei"（只有 "Microsoft YaHei UI"），GDI 静默换了字族。
//   后果：字重/字宽与设计稿不一致，且不同机器回退目标不同、不可复现。
//
// 做法：逐 id 检查 GetFontInfo(id)->sFontName，若与"解析后的可用字体名"
//   不同，则 RemoveFont(id) 后按同样的字号/粗体/斜体重建。
//   只改字族，字号与样式原样保留 —— 布局不受影响。
// ============================================================
void CFrameWnd::FixFontFaces()
{
    // 皮肤里定义过的字体 id：1..11（见 Main.xml <Font id="N">）
    constexpr int kMinFontId = 1;
    constexpr int kMaxFontId = 11;

    const wchar_t* pszText   = fontsup::UiTextFace();
    const wchar_t* pszSymbol = fontsup::UiSymbolFace();

    int nFixed = 0;
    int nQuality = 0;
    for (int id = kMinFontId; id <= kMaxFontId; ++id)
    {
        TFontInfo* pInfo = m_pm.GetFontInfo(id);
        if (!pInfo) continue;   // 该 id 未在皮肤中定义

        // 符号字体（Segoe UI Symbol）与等宽字体不做中文族替换，
        // 否则勾选字形与路径显示都会变样。
        //
        // 符号字体（Segoe UI Symbol / Marlett）与等宽字体不做中文族替换，
        // 否则勾选字形与路径显示都会变样。
        const CDuiString sCur = pInfo->sFontName;
        const bool bSymbolLike =
            sCur.Find(L"Segoe UI Symbol") >= 0
            || sCur.Find(L"Consolas") >= 0;

        // [R251] Marlett 绝不能被替换成 Segoe UI Symbol。
        //
        // Marlett 是 Windows 自带的私有字形字体：最小化/最大化/关闭/还原
        // 分别画在 '0' '1' 'r' '2' 这几个**普通 ASCII 码位**上。
        // 一旦字族换成 Segoe UI Symbol，那些码位就变回真正的数字和字母，
        // 标题栏于是显示成 "0 1 r" —— 这正是用户截图里的样子。
        //
        // 反过来说，给 Marlett 的码位换成 Unicode 几何字符也不行：
        // Marlett 字体里没有 U+2500 之类的字形，实测渲染出来是空白。
        // 结论是唯一正确的组合：**Marlett 字体 + 它自己的码位**，
        // 因此这里只保留字族不动，后面的 RebuildFont 仍会把它重建一遍
        // （拿到 ClearType 质量），字形不会丢。
        const bool bMarlett = sCur.Find(L"Marlett") >= 0;
        const wchar_t* pszWant = bMarlett          ? pszText   /*占位，下面跳过替换*/
                              : bSymbolLike        ? pszSymbol
                                                   : pszText;

        // [R212] 字族正确时也必须重建一次：皮肤解析期走的 AddFont() 没有设置
        //   lf.lfQuality，字体是"系统默认质量"而非 ClearType；深色主题下
        //   12pt 雅黑 UI 的细笔画会退化成灰度抗锯齿，这正是用户反馈的
        //   "字体不清晰 / 按钮昵称发灰"的根因。RebuildFont() 内部显式写入
        //   CLEARTYPE_QUALITY，所以统一绕它建字体，不再直接 AddFont。
        if (!bMarlett && sCur != pszWant)
        {
            // 字族不对：按旧字号/样式在目标字族上重建（保留全部样式位）
            const int  nSize   = pInfo->iSize;
            const bool bBold   = pInfo->bBold;
            const bool bUnder  = pInfo->bUnderline;
            const bool bItalic = pInfo->bItalic;
            const bool bStrike = pInfo->bStrikeout;

            m_pm.RemoveFont(id, true);    // 优先按共享字体移除（皮肤 <Font shared="true">）
            m_pm.RemoveFont(id, false);   // 非共享路径也清一次，两者互不冲突
            m_pm.AddFont(id, pszWant, nSize, bBold, bUnder, bItalic, bStrike, true);
            ++nFixed;
        }

        // 无论字族是否改过，都用 DuiLib 的 RebuildFont 重新创建一遍，
        // 使 lfQuality 落到 CLEARTYPE_QUALITY（这是抗锯齿质量的唯一保证）。
        if (TFontInfo* pRebuild = m_pm.GetFontInfo(id))
        {
            m_pm.RebuildFont(pRebuild);
            ++nQuality;
        }
    }

    if (nFixed > 0 || nQuality > 0)
    {
        CDuiString sLog;
        sLog.Format(L"[font] faces rebuilt=%d quality=%d text=%s",
                    nFixed, nQuality, pszText);
        AppendLog(sLog.GetData());
        if (CControlUI* pRoot = m_pm.GetRoot()) pRoot->NeedUpdate();
    }
}

// [R201] 窗口级系统外观：Win11 圆角 / Mica / 深色标题栏 + 强调色刷新。
//
// 为什么放在 FrameWnd 而不是 Win8Metro.cpp：
//   它会读当前 HWND 与 DPI 状态，并在主题变更时可被重复调用；
//   只有窗口类本身同时掌握这两个状态。
//
// 幂等性：ApplyWindowTheme 内部只是重复写几个窗口属性，DWM 会按最新值合成，
//   重复调用不会累积副作用 —— 这是敢在每次 WM_SETTINGCHANGE 都调用的前提。
// ============================================================
// [R220] 主题切换 → 页面级取色刷新（分发骨架）
//
// 为什么需要：重绘 != 重新取色。动态建控件时颜色来自 tokens::Token(...)
//   的字面值；主题一变，控件身上仍是旧值，RedrawWindow 只是把旧色再画一遍。
//
// 为什么用探测式分发而不是直接调虚函数：
//   本轮写权限只覆盖本文件，PageBase.h（CPageBase 声明的真实位置）无法落
//   声明，直接调用会编译失败。下面的探测在 CPageBase 还没有该成员时解析为
//   空实现（等价于今天的行为，零行为变化）；一旦 CPageBase 加上
//   virtual void RefreshThemeColors()，本处自动改为真实调用并按页生效 ——
//   届时可把这两个 helper 换成一行直接调用。
// ============================================================
namespace
{
template <typename T, typename = void>
struct HasRefreshThemeColors : std::false_type {};

template <typename T>
struct HasRefreshThemeColors<T, std::void_t<
    // T*(nullptr) form rather than std::declval: no <utility> dependency,
//   same semantics (compile-time probe of expression validity only).
decltype(static_cast<T*>(nullptr)->RefreshThemeColors())>> : std::true_type {};

template <typename T>
void CallRefreshThemeColors(T* p, std::true_type) { p->RefreshThemeColors(); }
template <typename T>
void CallRefreshThemeColors(T*, std::false_type) {}
}   // namespace

void CFrameWnd::ApplySystemTheme(bool bForceReplay)
{
    if (!m_hWnd) return;

    // [R201] 先丢弃令牌缓存：强调色可能刚被用户改过，
    //   后续任何页面重建取到的都应是新值。
    tokens::Refresh();

    // [R236] 走 tokens::IsDark() 而不是直接问系统：手动主题模式下
    //   用户的选择必须压过系统设置，否则切到"深色"后 DWM 标题栏
    //   仍是浅色，与窗口内容割裂。
    const bool bDark = tokens::IsDark();
    const unsigned int nAbility = dwmtheme::ApplyWindowTheme(m_hWnd, bDark);

    // [R213] 默认文字色必须跟随主题。
    //   根因：CPaintManagerUI 构造函数把 m_dwDefaultFontColor 写死为 0xFF000000，
    //   而任何"没有显式 SetTextColor"的控件（复选框行、部分动态 Text）在
    //   PaintText 里都会回退到这个默认色 —— 深色主题下就是纯黑压 #1F1F1F，
    //   实测对比度约 1.4:1，肉眼等同于不可见。用户反馈的"字体不清晰"
    //   有相当一部分来自这里，而不是字体的抗锯齿。
    //   放在 ApplySystemTheme 内：它是主题变更与启动初始化的唯一收口点。
    m_pm.SetDefaultFontColor(tokens::Token(tokens::EToken::Ink), true);
    m_pm.SetDefaultDisabledColor(tokens::Token(tokens::EToken::InkMuted), true);
    m_pm.SetDefaultLinkFontColor(tokens::Token(tokens::EToken::AccentText), true);

    // [R201] 高对比度：把主窗口底色/文字切到系统配色。
    //   只在开启时改，关闭时不动 —— 关闭后由 DuiLib 皮肤自己的颜色接管，
    //   避免"关掉高对比度反而变成另一种自定义配色"。
    if (dwmtheme::IsHighContrastMode())
    {
        if (CControlUI* pRoot = m_pm.GetRoot())
        {
            pRoot->SetBkColor(dwmtheme::HighContrastWindowColor());
            pRoot->NeedUpdate();
        }
        ::RedrawWindow(m_hWnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
    }

    // 只在能力集变化时写日志：WM_SETTINGCHANGE 会被广播多次
    //（改分辨率、改壁纸都会发），每来一次写一行会把日志刷满。
    static unsigned int s_nLastAbility = 0xFFFFFFFFu;
    static bool s_bLastDark = false;
    if (nAbility != s_nLastAbility || bDark != s_bLastDark)
    {
        s_nLastAbility = nAbility;
        s_bLastDark = bDark;
        CDuiString sLog;
        sLog.Format(L"[theme] ability=%s dark=%d accent=#%06lX",
                    dwmtheme::AbilityToText(nAbility).c_str(),
                    bDark ? 1 : 0,
                    static_cast<unsigned long>(dwmtheme::AccentColorRef()));
        AppendLog(sLog.GetData());
    }

    // 圆角/Mica 生效后窗口外框形状变了，必须让 DuiLib 重绘一次边框，
    // 否则会残留上一次的直角描边。
    if (nAbility & static_cast<unsigned int>(dwmtheme::EThemeAbility::RoundedCorner))
    {
        if (CControlUI* pRoot = m_pm.GetRoot()) pRoot->NeedUpdate();
        ::RedrawWindow(m_hWnd, nullptr, nullptr,
                       RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
    }

    // [R218] Light/dark switches MUST repaint the whole tree, unconditionally.
    //
    // Both RedrawWindow calls above sit inside capability-gated branches (high contrast,
    // rounded corner), so a plain light<->dark switch -- where neither capability
    // changes -- issued no repaint at all. The token table was refreshed and the DWM
    // attributes were rewritten, but every control kept painting its old colours until
    // something else happened to invalidate it. That is the "switching to dark leaves
    // parts of the window light" symptom, and it is why launching in dark looked right
    // while switching into dark did not: startup builds the tree once, after the tokens
    // are already correct.
    //
    // Detecting the change needs the previous value, so it is tracked in a member
    // rather than a function-local static: two frames (or a future second window) would
    // otherwise share one flag and the second window's first paint would be skipped.
    if (bForceReplay || m_bLastDarkTheme != bDark)
    {
        m_bLastDarkTheme = bDark;
        // [R220] 先把颜色重新取一遍，再整树重绘：否则重绘只是把旧主题的
        //   取色结果又画一遍。只刷已构建页 —— 未访问过的页在首次
        //   EnsurePageBuilt 时用的已是最新令牌，提前 Build() 反而会把它
        //   固化并拖慢主题切换，且此时该页根本没有控件可刷。
        // [R321] 先重放皮肤层。它覆盖的是"建树时就固化的 XMl 属性"，
        //   而下面的页面钩子只管代码侧动态创建的控件 —— 两者集合不重叠，
        //   必须都调，否则手动切换后骨架/标题栏仍是旧主题。
        ReplaySkinTokens();
        // [R327] 滚动条颜色来自 <Default>，不在上面那张具名控件表里，必须单独刷。
        RefreshScrollBarColors();
        int nThemeRefreshed = 0;
        constexpr bool bHookPresent = HasRefreshThemeColors<CPageBase>::value;
        if constexpr (bHookPresent)
        {
            for (int i = 0; i < 16; ++i)
            {
                if (!m_bPageBuilt[i]) continue;
                if (CPageBase* pPage = PageById(i))
                {
                    CallRefreshThemeColors(pPage, std::true_type{});
                    ++nThemeRefreshed;
                }
            }
            if (nThemeRefreshed > 0)
            {
                CDuiString sThemeLog;
                sThemeLog.Format(L"[theme] colours refreshed pages=%d", nThemeRefreshed);
                AppendLog(sThemeLog.GetData());
            }
        }
        else
        {
            // 骨架阶段：CPageBase 尚无 RefreshThemeColors()，显式留痕，
            //   避免"以为刷了其实没刷"的静默失效（R206 的教训）。
            AppendLog(L"[theme] page colour refresh hook absent (PageBase.h not yet extended)");
        }
        if (CControlUI* pRoot = m_pm.GetRoot())
        {
            pRoot->NeedUpdate();
            // Force every descendant to re-evaluate its own painting state. Without
            // this, containers that cache their layout skip a refresh and keep stale
            // child colours for one frame.
            pRoot->SetPos(pRoot->GetPos());
        }
        ::RedrawWindow(m_hWnd, nullptr, nullptr,
                       RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
        AppendLog(bDark ? L"[theme] repaint for dark" : L"[theme] repaint for light");
    }
}

void CFrameWnd::OnDpiChanged(int dpiW, int dpiH, const RECT* prcProposedRect)
{
    (void)dpiH;
    if (dpiW <= 0) return;    // 防御：wParam 低 16 位为 0 时直接忽略


    // B1 · PerMonitorV2 DPI 迁移 · 增强版
    // 1) 更新 m_nDPI + m_pm.SetDPI  → 让后续所有 CDPI::Pixel/DPIValue 用新 DPI
    // 2) ::SetWindowPos 应用系统建议矩形 → 保持跨显示器比例一致
    // 3) m_pm.ResetDPIAssets()       → 重建已缓存字体/位图（旧缓存基于旧 DPI）
    // 4) ::SendMessage WM_SIZE       → 让 CVerticalLayout / CHorizontalLayout
    //     按新 DPI 重排所有子控件（DuiLib 里 Layout 类唯一的重排入口）
    // 5) m_pm.GetRoot()->NeedUpdate  → 标记全树失效
    // 6) ::UpdateWindow              → 立即同步重绘，避免闪烁


    int nOldDpi = m_nDPI;
    setDPI(dpiW);


    if (prcProposedRect)
    {
        ::SetWindowPos(m_hWnd, nullptr,
            prcProposedRect->left, prcProposedRect->top,
            prcProposedRect->right - prcProposedRect->left,
            prcProposedRect->bottom - prcProposedRect->top,
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
    }


    m_pm.ResetDPIAssets();


    if (m_pm.GetRoot())
    {
        // 主动派发一次 WM_SIZE 让 Layout 走完整 Measure/Arrange 路径
        RECT rc;
        if (::GetClientRect(m_hWnd, &rc))
        {
            ::SendMessage(m_hWnd, WM_SIZE,
                SIZE_RESTORED,
                MAKELPARAM((unsigned)(rc.right - rc.left),
                            (unsigned)(rc.bottom - rc.top)));
        }
        m_pm.GetRoot()->NeedUpdate();
        ::UpdateWindow(m_hWnd);
    }


    // B1 增量：跨 DPI 触发时重新构建缓存了旧 DPI 尺寸的动态控件树
    // （BuildCleanupPage / BuildOptimTree / BuildStartupPage 中手写的字号、
    // 列宽、按钮宽度）。这里派发 WM_APP+100 让 FrameWnd 消息循环统一处理，
    // 避免嵌套 SendMessage 造成重入。
    if (nOldDpi > 0 && nOldDpi != dpiW)
    {
        ::SendMessage(m_hWnd, WM_APP + 100, dpiW, nOldDpi);
    }
}


// ============================================================
// [R236] 手动主题切换
// ------------------------------------------------------------
// 需求：标题栏有"主题"入口，下拉能看到各主题的配色预览，点一下即切换。
//
// 设计取舍：
//   1) 三档（跟随系统 / 浅色 / 深色）而不是两档开关。
//      "跟随系统"必须是默认值，否则系统换主题时本程序会静止不动 ——
//      那是比"不能手动切"更严重的退化。
//   2) 预览卡按**目标**主题取令牌（TokenInMode），不是按当前状态取。
//      用 Token() 的话三个选项会长得一模一样，等于没有预览。
//   3) 切换后必须同时做三件事，缺一个就会出现"切了一半"：
//        a. tokens::SetMode   -> 令牌按新模式重算
//        b. ApplySystemTheme  -> DWM 标题栏/边框跟着变（否则系统标题栏还是旧色）
//        c. 整树重绘           -> 否则控件还在画旧颜色
//      第 b 步走的是 ApplySystemTheme，而它内部判深浅已改为 tokens::IsDark()，
//      所以手动覆盖能真正压过系统设置。
// ============================================================

// 配置节：独立于 [Language]，避免与语言键混在一起被整体重写覆盖
static constexpr wchar_t kThemeSection[] = L"Theme";
static constexpr wchar_t kThemeKeyMode[] = L"Mode";

// --- [debug shim] FW_LOG —— 零开销空宏（同上 TS_LOG）---
#define FW_LOG(msg) do { } while(0)

void CFrameWnd::LoadThemeMode()
{
    ThemeService::Initialize(CPaintManagerUI::GetInstancePath(), nullptr);

    // [R253-fix] 皮肤目录必须跟着 preset 走。
    //   Win8Metro.cpp:1335 把 SetResourcePath 写死成 skin\Classic，
    //   而颜色是按 preset 的令牌走的。于是保存的模式不是 classic 时，
    //   启动就是一个"半转换"界面：
    //     · 皮肤仍是 Classic —— 会话条亮蓝 #007ACC、导航图标、表格硬编码色；
    //     · 颜色却已按 dark/light 令牌走 —— 标题栏透明、导航与内容区深色。
    //   用户看到的正是"部分页面的背景颜色没有转换过来"。
    //   本函数在 Create() 之前调用，这里改掉资源路径，
    //   Create() 展开皮肤时用的就是该 preset 对应的那套 XML。
    {
        const ThemeService::ThemePreset& cur = ThemeService::CurrentPreset();
        if (!cur.skinDir.empty())
            CPaintManagerUI::SetResourcePath(CPaintManagerUI::GetInstancePath() + cur.skinDir.c_str());
    }

    // 令牌层：按 ThemeService 当前 preset 同步 tokens 模式
    //   Classic → tokens::Classic, Auto → tokens::Auto, 等
    const wchar_t* pszName = ThemeService::CurrentPreset().name.c_str();
    tokens::EThemeMode eTok = tokens::EThemeMode::Auto;
    if (_wcsicmp(pszName, L"classic") == 0) eTok = tokens::EThemeMode::Classic;
    else if (_wcsicmp(pszName, L"light") == 0)   eTok = tokens::EThemeMode::Light;
    else if (_wcsicmp(pszName, L"dark") == 0)    eTok = tokens::EThemeMode::Dark;
    else                                          eTok = tokens::EThemeMode::Auto;
    (void)tokens::SetMode(eTok);

    CDuiString sLog;
    sLog.Format(L"[theme] ThemeService PreInit preset=%s (skin dir set)", pszName);
    AppendLog(sLog.GetData());
}

// [2026] ThemeService Observer 回调：所有主题变更最终收敛到这里
//   ThemeService 切换 preset → ApplyDwm → 广播 OnThemeChanged → Repaint
void CFrameWnd::OnThemeChanged(const ThemeService::ThemePreset& preset)
{
    AppendLog(L"[theme_step0] OnThemeChanged ENTER");
    if (!m_hWnd) { AppendLog(L"[theme_step0] no hwnd, return"); return; }


    // ==== [PERF] 高精度计时仪表 ====
    LARGE_INTEGER freq, t0;
    ::QueryPerformanceFrequency(&freq);
    ::QueryPerformanceCounter(&t0);
    auto ms = [&](LARGE_INTEGER t) -> double {
        return 1000.0 * (t.QuadPart - t0.QuadPart) / freq.QuadPart;
    };
    struct Step { const wchar_t* n; double t; };
    Step steps[12]; int si = 0;
    auto mark = [&](const wchar_t* n) {
        LARGE_INTEGER t; ::QueryPerformanceCounter(&t);
        steps[si++] = { n, ms(t) };
    };

    // ===== 步骤 1: token 状态同步 =====
    tokens::EThemeMode eTok = tokens::EThemeMode::Auto;
    if (_wcsicmp(preset.name.c_str(), L"classic") == 0) eTok = tokens::EThemeMode::Classic;
    else if (_wcsicmp(preset.name.c_str(), L"light") == 0)   eTok = tokens::EThemeMode::Light;
    else if (_wcsicmp(preset.name.c_str(), L"dark") == 0)    eTok = tokens::EThemeMode::Dark;
    AppendLog(L"[theme_step1a] before SetMode");
    tokens::SetMode(eTok);
    AppendLog(L"[theme_step1b] before Refresh");
    tokens::Refresh();
    AppendLog(L"[theme_step1c] after Refresh");
    mark(L"1_token");

    // ===== 步骤 2: 清皮肤图片缓存 =====
    m_pm.ReloadImages();
    mark(L"2_ReloadImages");

    // ===== 步骤 3: 重建字体 + 设置默认字体色 =====
    FixFontFaces();
    m_pm.SetDefaultFontColor(tokens::Token(tokens::EToken::Ink), true);
    m_pm.SetDefaultDisabledColor(tokens::Token(tokens::EToken::InkMuted), true);
    m_pm.SetDefaultLinkFontColor(tokens::Token(tokens::EToken::AccentText), true);
    mark(L"3_Font+Default");

    // ===== 步骤 4: 刷 root 层颜色 =====
    ApplyPresetColorAttrs(preset);
    mark(L"4_PresetAttrs");

    // ===== 步骤 5: 刷已构建页面 =====
    if (_wcsicmp(preset.name.c_str(), L"classic") != 0)
    {
        ReplaySkinTokens();
        mark(L"5a_ReplaySkinTokens");
        RefreshScrollBarColors();
        mark(L"5b_RefreshScrollBar");
    }
    mark(L"5_TokenReplay");

    // ===== 步骤 6: 刷新已构建页面钩子 =====
    if constexpr (HasRefreshThemeColors<CPageBase>::value) {
        for (int i = 0; i < 16; ++i) {
            if (!m_bPageBuilt[i]) continue;
            // [WIN10-FIX] 跳过 PAGE_OPTIM (i=4)
            // Win10 上 CPageOptim::RefreshThemeColors() 在启动阶段第二次
            // ApplyTheme 时触发 STATUS_STACK_BUFFER_OVERRUN (0xC000041D)。
            // 根因待定位（可能是 Build 刚完成时 optim_tree 内部状态不稳），
            // 但页面首次切入时仍会调用 EnsurePageBuilt + RefreshThemeColors，
            // 用户手动切主题时也会再次触发，不影响最终视觉效果。
            if (i == static_cast<int>(EPageId::PAGE_OPTIM)) continue;
            if (CPageBase* pPage = PageById(i))
                CallRefreshThemeColors(pPage, std::true_type{});
        }
    }
    mark(L"6_PageHooks");

    // ===== 步骤 7: DWM + 重绘 =====
    const bool bDark = tokens::IsDark();
    dwmtheme::ApplyWindowTheme(m_hWnd, bDark);
    ::RedrawWindow(m_hWnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
    mark(L"7_DWM+Redraw");

    // ===== 步骤 8: 刷新主题按钮图标 =====
    RefreshThemeButton();
    mark(L"8_ThemeBtn");

    LARGE_INTEGER tEnd; ::QueryPerformanceCounter(&tEnd);
    double totalMs = ms(tEnd);

    // ==== 完整计时日志 ====
    {
        CDuiString sLog;
        sLog.Format(L"[theme] preset=%s dark=%d total=%.2fms |",
                    preset.name.c_str(), bDark ? 1 : 0, totalMs);
        double prev = 0;
        for (int i = 0; i < si; ++i) {
            double dt = steps[i].t - prev;
            prev = steps[i].t;
            CDuiString s2;
            s2.Format(L" %s=%.2f(+%.2f)", steps[i].n, steps[i].t, dt);
            sLog += s2;
        }
        AppendLog(sLog.GetData());
    }

    // ==== [DIAG] 刷后颜色自检 ====
    {
        CControlUI* pTB = m_pm.FindControl(L"titlebar");
        CControlUI* pNav = m_pm.FindControl(L"sidenav");
        CControlUI* pRoot = m_pm.GetRoot();
        CDuiString sDbg;
        sDbg.Format(L"[theme_diag] preset=%s hasPalette=%d titlebarBk=#%08lX sidenavBk=#%08lX rootBk=#%08lX",
                    preset.name.c_str(), preset.hasPalette ? 1 : 0,
                    pTB ? (unsigned long)pTB->GetBkColor() : 0UL,
                    pNav ? (unsigned long)pNav->GetBkColor() : 0UL,
                    pRoot ? (unsigned long)pRoot->GetBkColor() : 0UL);
        AppendLog(sDbg.GetData());
    }
}

// ============================================================
// [2026] 宿主侧刷预设颜色 —— 接管旧 DLL fnSetAttr 的角色
// ============================================================
//   旧流程：ThemeManager DLL 持锁调 fnSetAttr 刷硬编码色 → 宿主 ReloadImages
//   新流程：宿主 SetResourcePath + ReloadImages + 这里刷颜色
//
//   两种模式的颜色来源完全不同：
//   Classic preset (hasPalette=true, 不走 {tok:})
//     → 刷 preset.palette 里的硬编码值 (headerBand=#FF1E5799, accent=#FF007ACC 等)
//     → 额外补 Classic 独有的 titlebar bkcolor=#FF1E5799 / menubar bkcolor=#FF174A85
//   Win8Metro preset (hasPalette=false, 走 {tok:*})
//     → 刷 tokens::TokenHex(tokens::EToken::X) 取当前系统/Flent 值
//     → titlebar 通常是透明/DWM 处理
//
//   为什么不用重建 root？
//     重建 root = CDialogBuilder::Create 新 root → 旧控件指针悬空 → 16 页重新挂
//     风险高且业务逻辑（FindControl 缓存指针）容易崩。
//     FindControl + SetAttribute 保留现有控件树，业务逻辑不受影响。
// ============================================================
void CFrameWnd::ApplyPresetColorAttrs(const ThemeService::ThemePreset& preset)
{
    AppendLog(_T("[theme] ApplyPresetColorAttrs ENTER"));

    auto SetAttr = [this](const wchar_t* name, const wchar_t* attr, LPCTSTR val) {
        CControlUI* p = m_pm.FindControl(name);
        if (p) p->SetAttribute(attr, val);
    };

    // Classic 独有的硬编码值 —— 不在 palette 里的补充项
    //   titlebar 和 menubar 是 Classic 品牌识别，有独立于 palette headerBand 的值
    constexpr LPCTSTR kClassicTitlebarBk    = _T("#FF1E5799");
    constexpr LPCTSTR kClassicMenubarBk     = _T("#FF174A85");
    constexpr LPCTSTR kClassicCloseHot      = _T("#FFE81123");
    constexpr LPCTSTR kClassicClosePushed   = _T("#FFC50F1F");
    constexpr LPCTSTR kClassicBtnHot        = _T("#FF2D6AB0");

    AppendLog(_T("[theme] ApplyPresetColorAttrs: checking hasPalette"));

    if (preset.hasPalette)
    {
        AppendLog(_T("[theme] ApplyPresetColorAttrs: CLASSIC branch"));
        const auto& pal = preset.palette;
        // 颜色转 LPCTSTR："#" + 8 位 ARGB hex
        auto H = [](DWORD v) -> CDuiString {
            CDuiString s; s.Format(_T("#%08X"), v); return s;
        };

        // --- root 层 ---
        SetAttr(_T("root"),         _T("bkcolor"),      H(pal.surface));
        SetAttr(_T("content_area"), _T("bkcolor"),      H(pal.surface));
        SetAttr(_T("titlebar"),     _T("bkcolor"),      kClassicTitlebarBk);
        SetAttr(_T("menubar"),      _T("bkcolor"),      kClassicMenubarBk);
        SetAttr(_T("menubar"),      _T("bordercolor"),  kClassicMenubarBk);
        SetAttr(_T("sidenav"),      _T("bkcolor"),      H(pal.surface));
        SetAttr(_T("sidenav"),      _T("bordercolor"),  H(pal.border));
        SetAttr(_T("bottombar"),    _T("bkcolor"),      H(pal.accent));

        // --- session 信息条 ---
        // [2026-fix] 原来拼的是 "session.cl_1".."session.cl_8"。
        //   DuiLib 的控件名不含路径分隔符，FindControl("session.cl_1")
        //   永远找不到 —— 这段是死代码，Classic 切换后 session 条颜色不会跟着变。
        //   Classic 的会话条容器在皮肤里叫 cl_1 / cl_2（见 skin\Classic\Main.xml）。
        SetAttr(_T("cl_1"), _T("bkcolor"), H(pal.accent));
        SetAttr(_T("cl_2"), _T("bkcolor"), H(pal.accent));

        // --- NavOption x11 ---
        for (int i = 0; i < 11; ++i) {
            CDuiString sName; sName.Format(_T("nav_%d"), i);
            SetAttr(sName.GetData(), _T("textcolor"),            H(pal.ink));
            SetAttr(sName.GetData(), _T("selectedbkcolor"),      H(pal.accent));
            SetAttr(sName.GetData(), _T("hotbkcolor"),            H(pal.surfaceHover));
            SetAttr(sName.GetData(), _T("selectedtextcolor"),     _T("#FFFFFFFF"));
            SetAttr(sName.GetData(), _T("disabledtextcolor"),     H(pal.inkMuted));
        }

        // --- 标题栏按钮 ---
        SetAttr(_T("closebtn"),     _T("hotbkcolor"),    kClassicCloseHot);
        SetAttr(_T("closebtn"),     _T("pushedbkcolor"), kClassicClosePushed);
        SetAttr(_T("minbtn"),       _T("hotbkcolor"),    kClassicBtnHot);
        SetAttr(_T("maxbtn"),       _T("hotbkcolor"),    kClassicBtnHot);
        SetAttr(_T("restorebtn"),   _T("hotbkcolor"),    kClassicBtnHot);

        // --- 标题栏品牌按钮 (btn_theme) ---
        SetAttr(_T("btn_theme"),    _T("textcolor"),     H(pal.onHeaderBand));
        SetAttr(_T("btn_theme"),    _T("hottextcolor"),  kClassicTitlebarBk);
        SetAttr(_T("btn_theme"),    _T("hotbkcolor"),    _T("#FFFFFFFF"));
    }
    else
    {
        // ==================== Win8Metro preset: 用 tokens::TokenHex() ====================
        using namespace tokens;

        // --- root 层 ---
        SetAttr(_T("root"),         _T("bkcolor"),      TokenHex(EToken::Surface));
        SetAttr(_T("content_area"), _T("bkcolor"),      TokenHex(EToken::Surface));
        SetAttr(_T("titlebar"),     _T("bkcolor"),      _T("#00000000"));  // 透明/DWM 处理
        SetAttr(_T("menubar"),      _T("bkcolor"),      TokenHex(EToken::SurfaceAlt));
        SetAttr(_T("menubar"),      _T("bordercolor"),  TokenHex(EToken::BorderSoft));
        SetAttr(_T("sidenav"),      _T("bkcolor"),      TokenHex(EToken::Surface));
        SetAttr(_T("sidenav"),      _T("bordercolor"),  TokenHex(EToken::Border));
        SetAttr(_T("bottombar"),    _T("bkcolor"),      TokenHex(EToken::Accent));

        // --- session 信息条 ---
        // [2026-fix] 同上：Win8Metro 皮肤的会话条容器叫 sess_card / sess_fill
        //   （见 skin\Win8Metro\Main.xml），原 "session.cl_%d" 是死代码。
        //   底色按皮肤定义是 SurfaceAlt（不是 Accent）。
        SetAttr(_T("sess_card"), _T("bkcolor"), TokenHex(EToken::SurfaceAlt));
        SetAttr(_T("sess_fill"), _T("bkcolor"), TokenHex(EToken::SurfaceAlt));

        // --- NavOption x11 ---
        for (int i = 0; i < 11; ++i) {
            CDuiString sName; sName.Format(_T("nav_%d"), i);
            SetAttr(sName.GetData(), _T("textcolor"),            TokenHex(EToken::Ink));
            SetAttr(sName.GetData(), _T("selectedbkcolor"),      TokenHex(EToken::Accent));
            SetAttr(sName.GetData(), _T("hotbkcolor"),            TokenHex(EToken::SurfaceHover));
            SetAttr(sName.GetData(), _T("selectedtextcolor"),     TokenHex(EToken::OnAccent));
            SetAttr(sName.GetData(), _T("disabledtextcolor"),     TokenHex(EToken::InkMuted));
        }

        // --- 标题栏按钮 (关闭按钮红跟随主题 Danger) ---
        SetAttr(_T("closebtn"),     _T("hotbkcolor"),    TokenHex(EToken::Danger));
        SetAttr(_T("closebtn"),     _T("pushedbkcolor"), TokenHex(EToken::DangerPushed));
        SetAttr(_T("minbtn"),       _T("hotbkcolor"),    TokenHex(EToken::SurfaceHover));
        SetAttr(_T("maxbtn"),       _T("hotbkcolor"),    TokenHex(EToken::SurfaceHover));
        SetAttr(_T("restorebtn"),   _T("hotbkcolor"),    TokenHex(EToken::SurfaceHover));

        // --- 标题栏品牌按钮 ---
        SetAttr(_T("btn_theme"),    _T("textcolor"),     TokenHex(EToken::OnAccent));
        SetAttr(_T("btn_theme"),    _T("hottextcolor"),  TokenHex(EToken::OnAccent));
        SetAttr(_T("btn_theme"),    _T("hotbkcolor"),    TokenHex(EToken::SurfaceHover));
    }

    CDuiString sFW; sFW.Format(_T("ApplyPresetColorAttrs DONE preset=%s"), preset.name.c_str());
    FW_LOG(sFW.GetData());
}

void CFrameWnd::RebuildMainTree()
{
    AppendLog(_T("[theme] RebuildMainTree ENTER"));
    AppendLog(_T("[theme] step1: ReloadImages"));
    m_pm.ReloadImages();
    AppendLog(_T("[theme] step1 done"));
    CControlUI* pOldRoot = m_pm.GetRoot();
    AppendLog(_T("[theme] step2: ReapObjects"));
    if (pOldRoot) { m_pm.ReapObjects(pOldRoot); AppendLog(_T("[theme] old root reaped")); }
    AppendLog(_T("[theme] step2 done"));
    AppendLog(_T("[theme] step3: m_bPageBuilt reset"));
    for (int i = 0; i < 16; ++i) m_bPageBuilt[i] = false;
    AppendLog(_T("[theme] step3 done"));
    AppendLog(_T("[theme] step4: GetSkinFile"));
    CDuiString sSkinXml = GetSkinFile();
    if (sSkinXml.IsEmpty()) { AppendLog(_T("[theme] GetSkinFile FAILED")); return; }
    CDuiString sL; sL.Format(_T("[theme] step4 done, xml len=%d"), sSkinXml.GetLength());
    AppendLog(sL.GetData());
    AppendLog(_T("[theme] step5: CDialogBuilder::Create"));
    CDialogBuilder builder;
    CControlUI* pNewRoot = builder.Create(sSkinXml.GetData(), nullptr, this, &m_pm);
    if (!pNewRoot) { AppendLog(_T("[theme] CDialogBuilder FAILED")); return; }
    AppendLog(_T("[theme] step5 done"));
    AppendLog(_T("[theme] step6: AttachDialog"));
    m_pm.AttachDialog(pNewRoot);
    AppendLog(_T("[theme] step6 done"));
    AppendLog(_T("[theme] step7: RebindRootControls"));
    RebindRootControls();
    AppendLog(_T("[theme] step7 done"));
    AppendLog(_T("[theme] step8: FixFontFaces"));
    FixFontFaces();
    AppendLog(_T("[theme] step8 done"));
    AppendLog(_T("[theme] RebuildMainTree DONE"));
}

void CFrameWnd::RebindRootControls()
{
    LoadSkinGlyphs();
    m_sStatusReady = GetSkinText(m_pm, L"sess_status");
    if (CTextUI* pAppTitle = static_cast<CTextUI*>(m_pm.FindControl(L"app_title")))
    {
        CDuiString sTitle = StringMgr.Get("app.product");
        const CDuiString sVer = StringMgr.Get("app.version");
        const CDuiString sEd  = StringMgr.Get("app.edition");
        if (!sVer.IsEmpty()) { sTitle += L" "; sTitle += sVer; }
        if (!sEd.IsEmpty())  { sTitle += L" "; sTitle += sEd; }
        pAppTitle->SetText(sTitle.GetData());
        ::SetWindowTextW(m_hWnd, sTitle.GetData());
    }
}

bool CFrameWnd::ApplyThemeMode(tokens::EThemeMode e, bool bPersist)
{
    // [2026] ThemeService 接管：preset name 是唯一入口
    //   这里只做 EThemeMode → preset name 的映射，然后调 ThemeService::ApplyTheme
    std::wstring presetName;
    switch (e) {
        case tokens::EThemeMode::Classic: presetName = L"classic"; break;
        case tokens::EThemeMode::Light:   presetName = L"light";   break;
        case tokens::EThemeMode::Dark:    presetName = L"dark";    break;
        default:                           presetName = L"auto";   break;
    }
    const bool bChanged = ThemeService::ApplyTheme(presetName, bPersist);
    // ThemeService 内已经做了 OnThemeChanged 广播 → ApplySystemTheme → RedrawWindow
    return bChanged;
}

void CFrameWnd::RefreshThemeButton()
{
    // [R325] 用 dynamic_cast 而不是 static_cast：btn_theme 目前是 Main.xml 里的
    //   <Button>，static_cast 恰好正确；但皮肤是可替换的，一旦某个皮肤变体把它
    //   写成 <Control> / <Text>，static_cast 就是错误的向下转型，SetText/
    //   SetToolTip 会作用到布局语义完全不同的对象上（布局类没有文本成员，
    //   轻则无效，重则踩到别的字段）。dynamic_cast 失败时静默跳过即可 ——
    //   图标刷不出来不影响主题本身生效。
    CControlUI* pCtl = m_pm.FindControl(L"btn_theme");
    CButtonUI* pBtn = pCtl ? dynamic_cast<CButtonUI*>(pCtl) : nullptr;
    if (!pBtn) return;

    // [CLASSIC-SKIN] 4 种模式对应 4 种图标
    const tokens::EThemeMode e = tokens::Mode();
    const wchar_t* pszGlyph = (e == tokens::EThemeMode::Classic) ? L"★"   // ★ 经典蓝
                            : (e == tokens::EThemeMode::Dark)    ? L"☾"   // ☾ 深色
                            : (e == tokens::EThemeMode::Light)   ? L"☀"   // ☀ 浅色
                                                                   : L"◐";  // ◐ 跟随系统
    pBtn->SetText(pszGlyph);

    // tooltip 让"当前是什么模式"无需展开下拉即可知道
    const wchar_t* pszTip = (e == tokens::EThemeMode::Classic) ? L"主题：经典蓝"
                          : (e == tokens::EThemeMode::Dark)    ? L"主题：深色"
                          : (e == tokens::EThemeMode::Light)   ? L"主题：浅色"
                                                                 : L"主题：跟随系统";
    pBtn->SetToolTip(pszTip);
}

void CFrameWnd::ShowThemePicker()
{
    CControlUI* pBtn = m_pm.FindControl(L"btn_theme");
    if (!pBtn || !m_hWnd) return;

    // [R325] 已经开着就收起（再次点击 = 关闭）。
    //   两个作用：
    //     1) 符合下拉/菜单的直觉 —— 点一下展开、再点一下收起；
    //     2) 堵住"快速双击建出两个浮层"：旧窗销毁时把激活权交还 owner，
    //        新窗随即收到 WM_KILLFOCUS 并自行关闭，用户看到的是"下拉一闪就没了"。
    if (CThemePickerWnd::IsOpen())
    {
        CThemePickerWnd::CloseOpen();
        return;
    }

    // 按钮矩形是**客户区**坐标，下拉窗口要屏幕坐标：
    //   直接拿 GetPos() 当屏幕坐标会让下拉固定在屏幕左上角附近，
    //   窗口被拖动后偏差更大（这是"弹到奇怪位置"最常见的原因）。
    RECT rcBtn = pBtn->GetPos();
    POINT ptLT = { rcBtn.left, rcBtn.top };
    POINT ptRB = { rcBtn.right, rcBtn.bottom };
    ::ClientToScreen(m_hWnd, &ptLT);
    ::ClientToScreen(m_hWnd, &ptRB);
    RECT rcScreen = { ptLT.x, ptLT.y, ptRB.x, ptRB.y };

    // 浮层自删除（OnFinalMessage 里 delete this），所以调用方 new 出来就不管了。
    CThemePickerWnd* p = new CThemePickerWnd();
    // CDPI::GetDPI() 返回当前窗口的 DPI（96 基准），浮层据此缩放
    const int nDpi = m_pm.GetDPIObj() ? static_cast<int>(m_pm.GetDPIObj()->GetDPI()) : 96;
    // [2026] 回调直接调 ThemeService::ApplyTheme —— preset name 是唯一入口
    //   ThemeService 内部做：状态切换 + ThemeManager + DWM + config.ini + Observer 广播
    if (!p->Create(m_hWnd, rcScreen, nDpi,
                   [](const std::wstring& name) { ThemeService::ApplyTheme(name, true); }))
    {
        delete p;   // Create 失败时对象还没进消息循环，必须自己回收
        AppendLog(L"[theme] picker create failed");
    }
}


// [R-B12] 品牌图标前置声明（InitWindow 早于 LoadAppBrandIcon 定义）
static HICON LoadAppBrandIcon();
void CFrameWnd::InitWindow()
{
    // ============================================================
    // [2026] ThemeService 第二阶段: FullInit + 注册 Observer
    FW_LOG("InitWindow before FullInit");
    {
        ThemeService::AddObserver(this);   // [FIX] 必须在 FullInit 之前注册！
        ThemeService::DuiLibCallbacks cb;
        cb.hWnd = m_hWnd;   // [2026] 注入真实窗口句柄 —— DWM title bar 需要它
        cb.fnSetSkinDir = [](const wchar_t* dir) {
            CPaintManagerUI::SetResourcePath(dir);
        };
        cb.fnSetAttr = [this](const wchar_t* name, const wchar_t* attr, const wchar_t* value) -> bool {
            CControlUI* p = m_pm.FindControl(name);
            if (!p) return false;
            p->SetAttribute(attr, value);
            return true;
        };
        ThemeService::FullInit(cb);
        FW_LOG("InitWindow after FullInit");
        FW_LOG("InitWindow after AddObserver");
        AppendLog(L"[theme] ThemeService FullInit + Observer registered");
    }

    FW_LOG("InitWindow after Theme block");
    LoadSkinGlyphs();
    FW_LOG("InitWindow after LoadSkinGlyphs");
    m_sStatusReady = GetSkinText(m_pm, L"sess_status");
    FW_LOG("InitWindow after GetSkinText");
    if (CTextUI* pAppTitle = static_cast<CTextUI*>(m_pm.FindControl(L"app_title")))
    {
        CDuiString sTitle = StringMgr.Get("app.product");
        const CDuiString sVer = StringMgr.Get("app.version");
        const CDuiString sEd  = StringMgr.Get("app.edition");
        if (!sVer.IsEmpty()) { sTitle += L" "; sTitle += sVer; }
        if (!sEd.IsEmpty())  { sTitle += L" "; sTitle += sEd; }
        pAppTitle->SetText(sTitle.GetData());
        ::SetWindowTextW(m_hWnd, sTitle.GetData());
    }
    FW_LOG("InitWindow after title");

    FixFontFaces();
    FW_LOG("InitWindow after FixFontFaces");


    // [B7] 移除无消费分支的 1 秒空定时器（唯一有效的定时器为 FEAT_TIMER_ID，
    // 在可选功能查询时按需创建），避免消息循环每秒空唤醒。


    // 语言菜单勾选与实际加载的语言包保持一致（来源：命令行/config.ini/系统locale）
    m_nLanguage = FrameWnd_Menu_LangCodeToIndex(StringMgr.GetCurrentLang());


    LoadSkinGlyphs();
    m_sStatusReady = GetSkinText(m_pm, L"sess_status");

    // [R201] 标题栏品牌文字走语言包（原 XML 里写死 "Dism++x64 2026.11.1 UEFI Firmware"）。
    //   与窗口标题用同一个键，两处永远一致；XML 保留原文以便皮肤预览时可见，
    //   运行时以这里为准。
    // [R219] Compose the title from the brand keys rather than reading one string, so the
    //   version/edition suffix can be changed without touching this code, and an empty
    //   edition does not leave a dangling double space.
    if (CTextUI* pAppTitle = static_cast<CTextUI*>(m_pm.FindControl(L"app_title")))
    {
        CDuiString sTitle = StringMgr.Get("app.product");
        const CDuiString sVer = StringMgr.Get("app.version");
        const CDuiString sEd  = StringMgr.Get("app.edition");
        if (!sVer.IsEmpty()) { sTitle += L" "; sTitle += sVer; }
        if (!sEd.IsEmpty())  { sTitle += L" "; sTitle += sEd; }
        pAppTitle->SetText(sTitle.GetData());
        // Keep the OS window caption identical to the in-app title: two different
        // names in the title bar and the taskbar is the classic rebrand miss.
        ::SetWindowTextW(m_hWnd, sTitle.GetData());
    }

    // [R201] 修正字体族：XML 里写的是设计名 "Microsoft YaHei"，
    //   而实测本机只有 "Microsoft YaHei UI" —— GDI 会把前者静默回退成
    //   "微软雅黑"（另一个字族），导致字重与字宽与设计稿不一致。
    //   这里按"本机真实可用"重建字体表，只改字族、不动字号与样式。
    FixFontFaces();

    // [R206] 诊断：设置 W8M_PROBE_ALLPAGES=1 时，启动后逐页构建并统计控件数。
    //   注意：kPages[7] 是 m_pgCapability（PAGE_FEATURE 只是它的子视图），
    //   因此这里**必须**用 EnsurePageBuilt 的统一入口，不能自己再 Build 一次
    //   —— 重复构建同一页会二次创建控件树并崩溃（本轮实测踩到）。
    // [R206] 皮肤结构自检：在内存里核对"每个页面节点都挂到了 content_area"。
    //   为什么不用"逐页 Build 再统计"：那会同步构建 16 棵控件树，实测直接把
    //   进程打崩（0xC000041D），而且 AppendLog 是入队批量落盘的，崩溃时队列
    //   里的日志全丢，反而什么都看不到。
    //   静态核对更可靠：展开后的 XML 本身就是"页面能否挂上"的唯一事实来源。
    {
        CTabLayoutUI* pTab = static_cast<CTabLayoutUI*>(m_pm.FindControl(L"content_area"));
        if (pTab)
        {
            int nPages = 0;
            int nEmpty = 0;
            for (int i = 0; i < pTab->GetCount(); ++i)
            {
                CControlUI* pChild = pTab->GetItemAt(i);
                if (!pChild) { ++nEmpty; continue; }
                ++nPages;
                CContainerUI* pBox = dynamic_cast<CContainerUI*>(pChild);
                if (!pBox || pBox->GetCount() == 0) ++nEmpty;
            }
            CDuiString sLine;
            sLine.Format(L"[page] content_area pages=%d empty=%d", nPages, nEmpty);
            AppendLog(sLine.GetData());
        }
    }

    // [R204] 控件级颜色自检：把关键控件的实际颜色落日志。
    //   非交互会话下截屏拿不到像素，只能靠程序自报颜色来验证
    //   令牌是否真的落到了控件上（而不只是停在展开后的 XML 里）。
    {
        const wchar_t* aNames[12] = { L"sidenav", L"root", L"titlebar", L"nav_0",
                                       L"content_area", L"page_cleanup", L"page_appx",
                                       L"page_tools", L"page_optim", L"page_feature",
                                       L"page_program", L"page_update" };
        for (int i = 0; i < 12; ++i)
        {
            CControlUI* pC = m_pm.FindControl(aNames[i]);
            if (!pC) continue;
            CDuiString sLine;
            sLine.Format(L"[color] %s bk=#%08lX", aNames[i],
                         static_cast<unsigned long>(pC->GetBkColor()));
            CTextUI* pT = dynamic_cast<CTextUI*>(pC);
            if (pT != nullptr)
            {
                CDuiString sMore;
                sMore.Format(L" text=#%08lX",
                             static_cast<unsigned long>(pT->GetTextColor()));
                sLine += sMore;
            }
            AppendLog(sLine.GetData());
        }
    }

    // [P0 2026-10-07] 启动即启用 SeBackup/SeRestore/SeSecurity 特权。
    // manifest 虽 requireAdministrator，但管理员令牌默认不含这些特权；
    // VSS 备份、离线 RegLoadKey 蜂巢加载、SCM 全权限操作都要求显式启用。
    if (SystemInfoService::EnableSystemPrivileges())
        AppendLog(L"[env] SeBackup/SeRestore/SeSecurity privileges enabled");
    else
        AppendLogWarn(L"[env] required privileges not fully available (need elevated admin)");


    // [P0 2026-10-07] WinPE 环境检测（MiniNT 键 / WinPE 版本键 / X: RAM 盘）：
    // PE 下"在线"系统即 RAM 盘，改动会被丢弃，须改用离线镜像功能；
    // X: 可用空间不足 5GB 时 DISM 还需把 ScratchDir 指向固定盘。
    if (SystemInfoService::IsWinPE())
    {
        AppendLogWarn(L"[env] Windows PE detected - use offline-image features; online targets are RAM-disk only");
        ULARGE_INTEGER cbFree = {}, cbTotal = {};
        if (::GetDiskFreeSpaceEx(L"X:\\", &cbFree, &cbTotal, nullptr)
            && cbFree.QuadPart < (5ULL * 1024 * 1024 * 1024))
        {
            ::MessageBoxW(m_hWnd,
                L"Windows PE detected.\r\n\r\n"
                L"The X: boot RAM disk has less than 5 GB free space. DISM operations require a writable scratch directory - redirect it to a fixed volume, e.g.  /ScratchDir:D:\\DismScratch .\r\n\r\n"
                L"Online cleanup/optimization only affects the RAM disk and will be discarded. Please use the offline-image features against the target Windows installation.",
                L"Dism++ - Windows PE", MB_OK | MB_ICONWARNING);
        }
        else
        {
            ::MessageBoxW(m_hWnd,
                L"Windows PE detected.\r\n\r\n"
                L"Online cleanup/optimization only affects the boot RAM disk and will be discarded. Please use the offline-image features against the target Windows installation.",
                L"Dism++ - Windows PE", MB_OK | MB_ICONINFORMATION);
        }
        // [P0] 自动探测固定盘上的已安装 Windows，并建立离线蜂巢会话：
        // 之后系统优化/注册表清理/服务开关经 CDataEngine 单点自动定向到
        // 目标蜂巢（SOFTWARE/SYSTEM/DEFAULT/NTUSER），不再误写 PE RAM 盘。
        CDuiString sTarget = SystemInfoService::FindOfflineWindowsDir();
        if (!sTarget.IsEmpty())
        {
            SystemInfoService::SetOfflineTarget(sTarget);
            CDuiString sLog;
            if (m_dataEngine.BeginOfflineSession(sTarget))
            {
                sLog.Format(L"[env] offline hive session started - target Windows at %s "
                               "(registry/service/cleanup actions redirected to offline image)",
                            sTarget.GetData());
                AppendLog(sLog.GetData());
            }
            else
            {
                sLog.Format(L"[env] WARNING: target Windows found at %s but hive mount failed "
                               "(SOFTWARE/SYSTEM busy or locked); offline registry actions unavailable",
                            sTarget.GetData());
                AppendLogWarn(sLog.GetData());
                SystemInfoService::SetOfflineTarget(nullptr);
            }
        }
        else
        {
            AppendLogWarn(L"[env] no installed Windows found on fixed volumes; "
                             "offline targeting unavailable");
        }
    }


    // load Data.xml rules
    if (m_dataEngine.Load(m_strDataPath))
    {
        AppendLog(L"[engine] Data.xml loaded:");
        AppendLog(m_strDataPath.GetData());


        if (m_cleanEngine.Init(&m_dataEngine))
        {
            CDuiString sInfo;
            sInfo.Format(L"[engine] %d cleanup rules in %d groups",
                         m_cleanEngine.GetRuleCount(), m_cleanEngine.GetGroupCount());
            AppendLog(sInfo.GetData());
        }
    }
    else
    {
        // 保留引擎给出的真实原因（含已尝试的绝对路径），不要再覆盖成
        // 无信息量的固定字符串，否则便携包缺文件时无法定位。
        m_strDataPath = m_dataEngine.GetLastError();
        AppendLog(L"[engine] load failed");
        AppendLog(m_dataEngine.GetLastError());
    }


    InitSystemInfo();


    if (m_dataEngine.IsValid())
    {
        // [R5] 初始化阶段构建经页序+基类虚入口，壳不持具体页类型
        if (CPageBase* pPage = PageById(EPageId::PAGE_CLEANUP)) pPage->Build();
        if (CPageBase* pPage = PageById(EPageId::PAGE_OPTIM)) pPage->Build();
    }
    else
    {
        // empty state: Data.xml missing/invalid, tell the user instead
        // of leaving both pages blank
        CDuiString sMsg = StringMgr.Get("error.sf044e82e");
        sMsg += m_strDataPath;


        const wchar_t* szContainers[] = { L"clean_list", L"optim_tree" };
        for (int n = 0; n < 2; ++n)
        {
            CVerticalLayoutUI* pBox = static_cast<CVerticalLayoutUI*>(
                m_pm.FindControl(szContainers[n]));
            if (!pBox) continue;
            pBox->RemoveAll();
            auto pTip = std::make_unique<CTextUI>();
            pTip->SetText(sMsg.GetData());
            pTip->SetTextColor(tokens::Token(tokens::EToken::Danger));
            pTip->SetAttribute(L"inset", L"16,16,16,16");
            pTip->SetAttribute(L"multiline", L"true");
            pBox->Add(pTip.release());   // [C++17 R3] DuiLib container takes ownership
        }


        // C-7: 右栏描述面板同样给出空态，避免 RichEdit 留白
        CRichEditUI* pDesc = static_cast<CRichEditUI*>(m_pm.FindControl(L"clean_desc"));
        if (pDesc)
        {
            pDesc->SetWordWrap(true);
            pDesc->SetText((std::wstring(StringMgr.Get("optim.s02d80789")) + std::wstring(StringMgr.Get("misc.s806e724d"))).c_str());
            pDesc->AppendText(m_strDataPath.GetData());
        }
    }


    // [R18 P1] 懒加载：首页（空间回收/系统优化）立即构建；其余 9 页
    // 在首次切入时由 EnsurePageBuilt() 按需构建/枚举，加快冷启动。
    m_bPageBuilt[static_cast<int>(EPageId::PAGE_CLEANUP)] = true;
    m_bPageBuilt[static_cast<int>(EPageId::PAGE_OPTIM)]   = true;

    // [PERF] 首次 FullInit 广播时页面没 Build，OnThemeChanged 里 PageHooks=0ms
    //   这里 cleanup+optim 已 Build，强制 ApplyTheme 再走一遍完整 OnThemeChanged。
    //   这样每次冷启动都有"页面已 Build"场景的计时数据。
    {
        const auto& cur = ThemeService::CurrentPreset();
        ThemeService::ApplyTheme(cur.name, false, true);
    }


    // 会话条动态进度条 / 托盘气泡 / 控件 Tooltip / 任务栏进度
    InitSessionProgress();
    InitTrayIcon();
    InitTooltips();


    // [R-B12] 窗口标题栏/任务栏图标使用品牌 app.ico
    {
        HICON hAppIcon = LoadAppBrandIcon();
        if (hAppIcon)
        {
            ::SendMessageW(m_hWnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hAppIcon));
            ::SendMessageW(m_hWnd, WM_SETICON, ICON_BIG,   reinterpret_cast<LPARAM>(hAppIcon));
        }
    }
    CenterWindow();
    // [R-B3] CLI 参数层消费点：窗口已显示、页面已 Build 后执行
    ApplyCommandLineActions();
}


// ============================================================
// real system information -> session strip
// ============================================================
void CFrameWnd::InitSystemInfo()
{
    SYSTEM_INFO si;
    ::GetNativeSystemInfo(&si);
    const wchar_t* arch = L"x86";
    if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64) arch = L"x64";
    else if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64) arch = L"ARM64";


    // product info from registry
    RegKeyGuard hKey;
    CDuiString strProduct = L"Windows";
    CDuiString strEdition;
    DWORD dwBuild = 0;
    if (::RegOpenKeyEx(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
            0, KEY_READ | KEY_WOW64_64KEY, hKey.out()) == ERROR_SUCCESS)
    {
        constexpr int kWinClassNameWchars = 256;   // 256 缓冲尺寸（语义命名，数值不变）
        wchar_t szBuf[kWinClassNameWchars] = { 0 };
        DWORD dwSize = sizeof(szBuf);
        if (::RegQueryValueEx(hKey, L"ProductName", nullptr, nullptr,
                              reinterpret_cast<BYTE*>(szBuf), &dwSize) == ERROR_SUCCESS)
            strProduct = szBuf;


        dwSize = sizeof(szBuf);
        if (::RegQueryValueEx(hKey, L"EditionID", nullptr, nullptr,
                              reinterpret_cast<BYTE*>(szBuf), &dwSize) == ERROR_SUCCESS)
            strEdition = szBuf;


        dwSize = sizeof(szBuf);
        if (::RegQueryValueEx(hKey, L"CurrentBuildNumber", nullptr, nullptr,
                              reinterpret_cast<BYTE*>(szBuf), &dwSize) == ERROR_SUCCESS)
            dwBuild = static_cast<DWORD>(_ttoi(szBuf));




        // without a Win11 manifest ProductName still says Windows 10
        if (dwBuild >= 22000)
        {
            int p = strProduct.Find(L"Windows 10");
            if (p >= 0)
                strProduct = strProduct.Mid(0, p) + L"Windows 11" + strProduct.Mid(p + 10);
        }


        // localize edition name, e.g. "Windows 11 Pro for Workstations"
        // → "Windows 11 专业工作站版"
        struct EdMap { const wchar_t* pszId; const wchar_t* pszZh; };
        const EdMap maps[] = {
            { L"ProfessionalWorkstation",  StringMgr.Get("system.pro_workstation") },
            { L"ProfessionalWorkstationN", StringMgr.Get("system.pro_workstation") },
            { L"Professional",             StringMgr.Get("system.pro") },
            { L"ProfessionalN",            StringMgr.Get("system.pro") },
            { L"Enterprise",               StringMgr.Get("system.enterprise") },
            { L"EnterpriseN",              StringMgr.Get("system.enterprise") },
            { L"EnterpriseS",              StringMgr.Get("system.s9d87c47a") },
            { L"Education",                StringMgr.Get("system.edu") },
            { L"EducationN",               StringMgr.Get("system.edu") },
            { L"Core",                     StringMgr.Get("system.home") },
            { L"CoreN",                    StringMgr.Get("system.home") },
            { L"CoreSingleLanguage",       StringMgr.Get("system.sa2743c79") },
            { L"CoreCountrySpecific",      StringMgr.Get("system.s88a3ac8b") },
        };
        if (!strEdition.IsEmpty())
        {
            for (int m = 0; m < _countof(maps); ++m)
            {
                if (lstrcmpiW(strEdition.GetData(), maps[m].pszId) == 0)
                {
                    int p11 = strProduct.Find(L"Windows 11");
                    int p10 = strProduct.Find(L"Windows 10");
                    if (p11 >= 0)
                        strProduct = CDuiString(L"Windows 11 ") + maps[m].pszZh;
                    else if (p10 >= 0)
                        strProduct = CDuiString(L"Windows 10 ") + maps[m].pszZh;
                    break;
                }
            }
        }
    }


    CDuiString sOS;
    sOS.Format(L"*%s %s", strProduct.GetData(), arch);
    SetCtrlText(m_pm, L"sess_os", sOS.GetData());


    // system drive
    wchar_t szSysDir[MAX_PATH] = { 0 };
    ::GetSystemDirectory(szSysDir, MAX_PATH);
    CDuiString sDrive;
    sDrive.Format(L"%c:", szSysDir[0]);
    SetCtrlText(m_pm, L"sess_drive", sDrive.GetData());
}



// [N13] T2 抽纯函数：判定指定 PID 是否以提权令牌运行（原 FindExplorerPid 内 3 层 if 内联）
static bool FwIsProcessElevated(DWORD dwPid)
{
    HandleGuard hProc = ::OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, dwPid);
    if (!hProc) return false;
    HandleGuard hTok;
    if (!::OpenProcessToken(hProc, TOKEN_QUERY, hTok.out())) return false;
    TOKEN_ELEVATION te = { 0 };
    DWORD dwCb = 0;
    if (!::GetTokenInformation(hTok, TokenElevation, &te, sizeof(te), &dwCb))
        return false;
    return te.TokenIsElevated != 0;
}
static DWORD FindExplorerPid(DWORD dwSessionId, bool bWantNonElevated)
{
    SnapGuard hSnap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return 0;


    DWORD dwPid = 0;
    PROCESSENTRY32 pe = { 0 };
    pe.dwSize = sizeof(pe);
    if (::Process32First(hSnap, &pe))
    {
        do
        {
            if (lstrcmpiW(pe.szExeFile, L"explorer.exe") != 0) continue;


            DWORD dwSess = 0xFFFFFFFF;
            if (!::ProcessIdToSessionId(pe.th32ProcessID, &dwSess)) continue;
            if (dwSess != dwSessionId) continue;


            if (bWantNonElevated && FwIsProcessElevated(pe.th32ProcessID)) continue;


            dwPid = pe.th32ProcessID;
            break;
        } while (::Process32Next(hSnap, &pe));
    }


    return dwPid;
}


// 等待期间持续泵送 UI 消息。RestartExplorer 运行在 UI 线程，旧实现用
// Sleep 盲等最长 13s 不泵消息，窗口被系统判为"未响应"（假死/崩溃观感）。
//
// [CFG-崩溃修复] 旧实现"先 EnableWindow(FALSE) 再全量泵消息就不会重入"
// 的假设不成立：①EnableWindow 不丢弃队列中已有的陈旧鼠标消息；
// ②优化页 combo/开关的重启是 PostMessage 延迟执行（见
// WM_HOST_REQUEST_RESTART_EXPLORER），但等待 shell 的数秒内队列仍可能
// 混入点击/命令/自定义消息。全量 Dispatch 会让 DuiLib 弹层（combo 下拉/
// 菜单/阴影窗口）在 shell 缺失期间半销毁，恢复后陈旧 WM_LBUTTONDOWN
// 命中失效 WndProc，触发 CFG FAST_FAIL_GUARD_ICALL_CHECK_FAILURE
// (0xc0000409 子码 0xa，dump 实证)。现改为白名单派发：只放行绘制与
// 外观/显示类系统消息，输入、命令、定时器、自定义业务消息一律丢弃。
static DWORD g_dwTaskbarCreatedMsg = 0;
static bool g_bQuitPostedWhileWaiting = false;
// [R125-5] Shell 安全消息白名单：等待期间仍需转发的消息（绘制/主题/会话/设备）。
//  原 switch 25 个 case 改为有序区间表 + constexpr 数组，查找用 std::binary_search。
static bool IsShellSafeMessage(UINT uMsg)
{
    // 说明：WM_* 在部分 SDK 配置下不是编译期常量，故用 const 数组而非 constexpr。
    static const UINT kSafe[] = {
        WM_PAINT, WM_NCPAINT, WM_ERASEBKGND, WM_DRAWITEM, WM_MEASUREITEM,
        WM_STYLECHANGING, WM_STYLECHANGED, WM_WINDOWPOSCHANGING, WM_WINDOWPOSCHANGED,
        WM_ACTIVATE, WM_NCACTIVATE, WM_ACTIVATEAPP,
        WM_QUERYUISTATE, WM_CHANGEUISTATE, WM_UPDATEUISTATE,
        WM_SETTINGCHANGE, WM_THEMECHANGED,
        WM_DWMCOMPOSITIONCHANGED, WM_DWMCOLORIZATIONCOLORCHANGED,
        WM_DWMSENDICONICTHUMBNAIL, WM_DWMSENDICONICLIVEPREVIEWBITMAP,
        WM_DISPLAYCHANGE, WM_DEVICECHANGE,
        WM_WTSSESSION_CHANGE, WM_QUERYENDSESSION, WM_ENDSESSION,
    };
    // 表必须升序（binary_search 前提），编译期校验防后续插入破坏
    // 控件颜色类消息是连续区间（WM_CTLCOLORMSGBOX..WM_CTLCOLORSTATIC）
    if (uMsg >= WM_CTLCOLORMSGBOX && uMsg <= WM_CTLCOLORSTATIC)
        return true;
    if (std::binary_search(std::begin(kSafe), std::end(kSafe), uMsg))
        return true;
    // TaskbarCreated 为运行时注册消息，单独判定
    if (uMsg >= 0xC000)
    {
        if (!g_dwTaskbarCreatedMsg)
            g_dwTaskbarCreatedMsg = ::RegisterWindowMessageW(L"TaskbarCreated");
        return g_dwTaskbarCreatedMsg != 0 && uMsg == g_dwTaskbarCreatedMsg;
    }
    return false;
}
// 取尽消息队列：白名单消息正常派发，其余（输入/命令/定时器/业务自定义）
// 直接丢弃；WM_QUIT 重新投回队列并标记，避免吞掉用户的退出意图。
static void PumpShellSafeMessages()
{
    MSG msg;
    // [CRASHFIX-LEAK] 业务消息（WM_USER 数据族 / WM_APP 编排族）按契约
    // lParam=new 堆对象、接收方 delete。旧实现连同业务消息一起丢弃 →
    // 堆泄漏 + 对应页"加载中/在途"态永不解除。现收集后在等待结束后原样
    // 回投，交回主循环在栈外正常处理（所有权随消息转移，不双删不泄漏）。
    std::vector<MSG> vRepost;
    while (::PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        if (msg.message == WM_QUIT)
        {
            ::PostQuitMessage(static_cast<int>(msg.wParam));
            g_bQuitPostedWhileWaiting = true;
            break;
        }
        if (IsShellSafeMessage(msg.message))
        {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            continue;
        }
        const bool bBusiness =
            (msg.message >= WM_USER && msg.message < WM_USER + 0x200)
            || (msg.message >= WM_APP && msg.message < WM_APP + 0x8000);
        // 例外：重启请求仍丢弃——重启刚完成，回投会立刻触发二次重启
        // （二次黑屏正是任务3 的表象来源）。
        if (bBusiness && msg.message != WM_HOST_REQUEST_RESTART_EXPLORER)
            vRepost.push_back(msg);
    }
    for (const MSG& m : vRepost)
        ::PostMessageW(m.hwnd, m.message, m.wParam, m.lParam);
}
// 清理队列中的陈旧输入/命令消息（重启前后各一次）。shell 重启会让 DuiLib
// 弹层状态全部作废，这些消息再派发只会造成重入/UAF/重复弹确认框。
static void PurgeStaleInputMessages()
{
    struct URange { UINT lo, hi; };
    const URange ranges[] = {
        { WM_KEYFIRST, WM_KEYLAST },                 // 0x100-0x109 键盘
        { WM_MOUSEFIRST, WM_MOUSELAST },             // 0x200-0x20E 鼠标
        { WM_NCMOUSEMOVE, WM_NCMBUTTONDBLCLK },      // 0x00A0-0x00AF 非客户鼠标
#ifdef WM_TABLET_FIRST
        { WM_TABLET_FIRST, WM_TABLET_LAST },         // 0x02C0-0x02DF 手写笔
#endif
    };
    for (size_t i = 0; i < sizeof(ranges)/sizeof(ranges[0]); ++i)
    {
        MSG msg;
        while (::PeekMessage(&msg, nullptr,
                             ranges[i].lo, ranges[i].hi,
                             PM_REMOVE | PM_NOYIELD))
        {
        }
    }
    const UINT singles[] = {
        WM_INPUT, WM_COMMAND, WM_SYSCOMMAND, WM_NOTIFY, WM_CONTEXTMENU,
        WM_PARENTNOTIFY, WM_CAPTURECHANGED, WM_MOUSEHOVER, WM_MOUSELEAVE,
        WM_HSCROLL, WM_VSCROLL, WM_GESTURE, WM_GESTURENOTIFY,
        WM_HOST_REQUEST_RESTART_EXPLORER
    };
    for (size_t i = 0; i < sizeof(singles)/sizeof(singles[0]); ++i)
    {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, singles[i], singles[i],
                             PM_REMOVE | PM_NOYIELD))
        {
        }
    }
}
static void PumpUiWhileWaiting(DWORD dwWaitMs)
{
    DWORD64 qwStart = ::GetTickCount64();
    do
    {
        PumpShellSafeMessages();
        if (g_bQuitPostedWhileWaiting) break;
        ::MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
    } while (::GetTickCount64() - qwStart < dwWaitMs);
}


// 可中断地等待内核句柄，期间仅白名单泵送消息（等待进程退出时不假死、不重入）。
static DWORD PumpWaitForHandle(HANDLE hObject, DWORD dwTimeoutMs)
{
    DWORD64 qwEnd = ::GetTickCount64() + dwTimeoutMs;
    for (;;)
    {
        DWORD r = ::MsgWaitForMultipleObjects(
            1, &hObject, FALSE, 50, QS_ALLINPUT);
        PumpShellSafeMessages();
        if (g_bQuitPostedWhileWaiting) return WAIT_FAILED;
        if (r == WAIT_OBJECT_0) return WAIT_OBJECT_0;
        if (::GetTickCount64() >= qwEnd) return WAIT_TIMEOUT;
    }
}
// 枚举指定会话全部 explorer.exe 的 PID。
static void EnumSessionExplorers(DWORD dwSession, std::vector<DWORD>& vOut)
{
    SnapGuard hSnap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe = { 0 };
    pe.dwSize = sizeof(pe);
    if (::Process32FirstW(hSnap, &pe))
    {
        do
        {
            if (::lstrcmpiW(pe.szExeFile, L"explorer.exe") != 0) continue;
            DWORD s = 0xFFFFFFFF;
            ::ProcessIdToSessionId(pe.th32ProcessID, &s);
            if (s == dwSession) vOut.push_back(pe.th32ProcessID);
        } while (::Process32NextW(hSnap, &pe));
    }
}
struct ExplorerProcInfo
{
    DWORD dwPid;
    bool  bFactoryProxy;   // 父进程为 svchost：DcomLaunch 激活的 /factory,-Embedding 代理
};
// 枚举本会话 explorer，并标记 Win11 的 COM 工厂代理
// （explorer.exe /factory,{CLSID} -Embedding，由 DcomLaunch 宿主 svchost
//  激活）。该代理与 shell 宿主同为 explorer.exe 映像，却不承载任务栏/桌面；
//  它存活会同时 (1) 抑制 Winlogon AutoRestartShell（会话"仍有 explorer"）、
//  (2) 使手动启动的 explorer 命中单实例转发、把启动请求交给它后自行退出，
//  而代理不处理 shell 初始化——这是"任务栏消失后永久不恢复"的关键现场
//  （Win11 25H2 实测：仅余该代理时 60s 无任务栏；清掉代理后零实例启动
//  explorer，任务栏 267ms 重建）。
static void EnumSessionExplorersEx(DWORD dwSession,
                                   std::vector<ExplorerProcInfo>& vOut)
{
    struct PInfo { DWORD pid; DWORD ppid; WCHAR name[MAX_PATH]; };
    std::vector<PInfo> vAll;
    SnapGuard hSnap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe = { 0 };
    pe.dwSize = sizeof(pe);
    if (::Process32FirstW(hSnap, &pe))
    {
        do
        {
            PInfo pi = { 0 };
            pi.pid = pe.th32ProcessID;
            pi.ppid = pe.th32ParentProcessID;
            wcsncpy_s(pi.name, MAX_PATH, pe.szExeFile, _TRUNCATE);
            vAll.push_back(pi);
        } while (::Process32NextW(hSnap, &pe));
    }
    for (size_t i = 0; i < vAll.size(); ++i)
    {
        if (::lstrcmpiW(vAll[i].name, L"explorer.exe") != 0) continue;
        DWORD s = 0xFFFFFFFF;
        ::ProcessIdToSessionId(vAll[i].pid, &s);
        if (s != dwSession) continue;
        ExplorerProcInfo ei;
        ei.dwPid = vAll[i].pid;
        ei.bFactoryProxy = false;
        for (size_t j = 0; j < vAll.size(); ++j)
        {
            if (vAll[j].pid == vAll[i].ppid &&
                ::lstrcmpiW(vAll[j].name, L"svchost.exe") == 0)
            {
                ei.bFactoryProxy = true;
                break;
            }
        }
        vOut.push_back(ei);
    }
}
// 以原 shell 的非提权令牌启动 explorer（提权进程必需）；令牌不可用或失败
// 时退回 ShellExecute（非提权进程继承自身令牌可正常承载 shell）。
static bool StartExplorerViaToken(HANDLE hImpToken)
{
    wchar_t szWinDir[MAX_PATH] = { 0 };
    ::GetWindowsDirectory(szWinDir, MAX_PATH);
    CDuiString sExe;
    sExe.Format(L"%s\\explorer.exe", szWinDir);
    if (hImpToken)
    {
        STARTUPINFO si = { 0 };
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_SHOWNORMAL;
        PROCESS_INFORMATION pi = { 0 };
        std::wstring sCmd = L"explorer.exe";
        if (::CreateProcessWithTokenW(hImpToken, 0, sExe.GetData(), &sCmd[0],
                                      0, nullptr, nullptr, &si, &pi))
        {
            // [C++17 R3] RAII close at scope exit; hProc declared last so it closes first (original order)
            HandleGuard hThr(pi.hThread);
            HandleGuard hProc(pi.hProcess);
            return true;
        }
    }
    HINSTANCE h = ::ShellExecute(nullptr, L"open", L"explorer.exe",
                                 nullptr, nullptr, SW_SHOWNORMAL);
    return (reinterpret_cast<INT_PTR>(h)) > 32;
}
// [官方 KB5072911 兜底] Win11 24H2/25H2 的任务栏/开始菜单等由
// MicrosoftWindows.Client.Core / Client.CBS / UI.Xaml.CBS 三个 XAML 包
// 承载；包注册/状态损坏时任务栏无法启动、explorer 启动即崩。官方修复为
// 在用户会话内重新注册这些包。以 shell 非提权令牌运行（与桌面会话一致），
// 清单存在才注册（Win10/旧版本自动跳过）。成功返回 true。
static bool RepairShellXamlPackages(HANDLE hImpToken)
{
    const wchar_t* pszManifests[] = {
        L"C:\\Windows\\SystemApps\\MicrosoftWindows.Client.Core_cw5n1h2txyewy\\appxmanifest.xml",
        L"C:\\Windows\\SystemApps\\MicrosoftWindows.Client.CBS_cw5n1h2txyewy\\appxmanifest.xml",
        L"C:\\Windows\\SystemApps\\Microsoft.UI.Xaml.CBS_8wekyb3d8bbwe\\appxmanifest.xml",
    };
    bool bAny = false;
    for (size_t i = 0; i < _countof(pszManifests); ++i)
        if (::GetFileAttributesW(pszManifests[i]) != INVALID_FILE_ATTRIBUTES)
            bAny = true;
    if (!bAny) return false;
    wchar_t szSys[MAX_PATH] = { 0 };
    ::GetSystemDirectory(szSys, MAX_PATH);
    CDuiString sPS;
    sPS.Format(L"%s\\WindowsPowerShell\\v1.0\\powershell.exe", szSys);
    if (::GetFileAttributesW(sPS.GetData()) == INVALID_FILE_ATTRIBUTES)
        sPS = L"powershell.exe";
    CDuiString sCmd(L"-NoProfile -ExecutionPolicy Bypass -Command \"");
    for (size_t i = 0; i < _countof(pszManifests); ++i)
    {
        if (::GetFileAttributesW(pszManifests[i]) == INVALID_FILE_ATTRIBUTES)
            continue;
        CDuiString sOne;
        sOne.Format(L"Add-AppxPackage -Register '%s' -DisableDevelopmentMode; ",
                    pszManifests[i]);
        sCmd += sOne;
    }
    sCmd += L"\"";
    std::vector<wchar_t> vCmd(sCmd.GetLength() + 1);
    wcsncpy_s(vCmd.data(), vCmd.size(), sCmd.GetData(), _TRUNCATE);
    STARTUPINFO si = { 0 };
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = { 0 };
    constexpr DWORD dwFlags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT;
    BOOL ok = FALSE;
    if (hImpToken)
    {
        ok = ::CreateProcessWithTokenW(hImpToken, 0, sPS.GetData(), vCmd.data(),
                                       dwFlags, nullptr, nullptr, &si, &pi);
    }
    else
    {
        ok = ::CreateProcessW(sPS.GetData(), vCmd.data(), nullptr, nullptr, FALSE,
                              dwFlags, nullptr, nullptr, &si, &pi);
    }
    if (!ok) return false;
    HandleGuard hThr(pi.hThread);   // [C++17 R3] RAII: auto CloseHandle at scope exit
    DWORD wr = PumpWaitForHandle(pi.hProcess, 90000);
    DWORD dwExit = 1;
    ::GetExitCodeProcess(pi.hProcess, &dwExit);
    HandleGuard hProc(pi.hProcess); // [C++17 R3] RAII: auto CloseHandle at scope exit
    return wr == WAIT_OBJECT_0 && dwExit == 0;
}
bool CFrameWnd::RestartExplorer()
{
    // 重入守卫：等待 shell 期间白名单泵已不派发业务消息，此处再兜底——
    // 任何嵌套/积压路径（清理完成编排、重复点击积压的延迟消息）在重启
    // 未完成时再次进入，直接拒绝，杜绝 explorer 二次重启（二次黑屏）。
    if (m_bRestartingExplorer)
    {
        AppendLog(L"[optim] explorer restart: rejected, already in progress");
        return false;
    }
    m_bRestartingExplorer = true;
    g_bQuitPostedWhileWaiting = false;
    HCURSOR hOldCursor = ::SetCursor(::LoadCursor(nullptr, IDC_WAIT));
    // 整个重启期间禁用主窗口输入；所有等待都泵送消息，界面不假死、不重入。
    ::EnableWindow(m_hWnd, FALSE);
    // 清掉点击"是"之后队列中残留的陈旧鼠标/命令消息（combo 弹层的
    // WM_LBUTTONUP 等），它们对应的 DuiLib 弹层在 shell 重启后状态作废。
    PurgeStaleInputMessages();
    DWORD64 qwStart = ::GetTickCount64();
    DWORD dwMySession = 0xFFFFFFFF;
    ::ProcessIdToSessionId(::GetCurrentProcessId(), &dwMySession);
    // 1) 杀 shell 之前，先从现存的非提权 explorer 复制它的"模拟令牌"。
    //    本程序 manifest 为 requireAdministrator：直接启动 explorer 会继承
    //    提权令牌，提权 explorer 不承载桌面/任务栏，且会让 AutoRestartShell
    //    误判 shell 已恢复，最终桌面永久消失。正确做法 =
    //    CreateProcessWithTokenW（仅需交互式管理员默认持有的
    //    SeImpersonatePrivilege）；不能用 CreateProcessAsUser（默认不持有
    //    SeAssignPrimaryTokenPrivilege，ACCESS_DENIED）。
    HANDLE hImpToken = nullptr;
    DWORD dwShellPid = FindExplorerPid(dwMySession, true);
    if (dwShellPid)
    {
        HandleGuard hProc = ::OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, dwShellPid);
        if (hProc)
        {
            HandleGuard hTok;
            if (::OpenProcessToken(hProc, TOKEN_DUPLICATE, hTok.out()))
            {
                constexpr DWORD dwTokenAccess = TOKEN_QUERY | TOKEN_DUPLICATE
                    | TOKEN_IMPERSONATE | TOKEN_ASSIGN_PRIMARY
                    | TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_SESSIONID;
                if (!::DuplicateTokenEx(hTok, dwTokenAccess, nullptr,
                                        SecurityImpersonation,
                                        TokenImpersonation, &hImpToken))
                    hImpToken = nullptr;
            }
        }
    }
    HWND hOldTray = ::FindWindowW(L"Shell_TrayWnd", nullptr);
    // 记录本会话当前全部 explorer PID 作为强杀"黑名单"。只杀这些旧 PID，
    // 绝不能误杀随后 Winlogon 自动拉起的新 explorer。
    std::vector<DWORD> vKill;
    EnumSessionExplorers(dwMySession, vKill);
    // 2) 先请求优雅退出（WM_USER+436/0x5B4，等价于
    //    "Ctrl+Shift+右键任务栏→退出资源管理器"）。实测在 Win11 25H2 上：
    //    单 shell 时约 330~590ms 任务栏消失，Progman/壁纸保留、全程不黑屏；
    //    更关键的是它让 SystemTray.dll（XAML 任务栏）正常落状态，避免
    //    TerminateProcess 在其渲染/状态提交途中强杀引发的 0xc0000409
    //    （事件日志实测复现）与包状态损坏。
    bool bGrace = false;
    if (hOldTray && ::PostMessageW(hOldTray, WM_USER + 436, 0, 0))
    {
        for (DWORD64 qwEnd = ::GetTickCount64() + 700;
             ::GetTickCount64() < qwEnd && ::IsWindow(hOldTray); )
        {
            PumpUiWhileWaiting(25);
        }
        bGrace = !::IsWindow(hOldTray);
    }
    // 2.5) 优雅路径再给旧进程 2s 自然退出宽限：窗口消失后 XAML/SystemTray
    //      仍需约 0.2~0.9s 落状态，让它自行退出可彻底避免 TerminateProcess
    //      打断收尾引发的 0xc0000409；宽限后仍存活才在下一步强杀。
    if (bGrace)
    {
        for (DWORD64 qwEndGrace = ::GetTickCount64() + 2000;
             ::GetTickCount64() < qwEndGrace; )
        {
            bool bAlive = false;
            std::vector<DWORD> vNow;
            EnumSessionExplorers(dwMySession, vNow);
            for (size_t a = 0; a < vNow.size() && !bAlive; ++a)
                for (size_t b = 0; b < vKill.size(); ++b)
                    if (vNow[a] == vKill[b]) { bAlive = true; break; }
            if (!bAlive) break;
            PumpUiWhileWaiting(50);
        }
    }
    // 3) 强杀黑名单中仍存活的全部旧 explorer（含独立进程的文件夹窗口）。
    //    必须"全杀"：实测只杀任务栏宿主、残留文件夹 explorer 时，Winlogon
    //    判定 shell 仍在而不自恢复；此时经单实例转发手动拉起的 explorer
    //    在 Win11 24H2/25H2 上会形成"有任务栏窗口、无 Progman 桌面层"的
    //    残缺 shell，连续重启叠加后任务栏长时间异常（本 bug 的现场根因）。
    //    代价是用户打开的文件夹窗口关闭（与 Dism++ 原版全杀语义一致），
    //    但 shell 一定能完整重建。
    DWORD dwKilled = 0;
    std::vector<HANDLE> vhProc;
    for (size_t i = 0; i < vKill.size(); ++i)
    {
        HANDLE h = ::OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE
                                    | PROCESS_QUERY_LIMITED_INFORMATION,
                                    FALSE, vKill[i]);
        if (!h) continue;
        DWORD ec = 0;
        if (::GetExitCodeProcess(h, &ec) && ec == STILL_ACTIVE)
        {
            if (::TerminateProcess(h, 1)) ++dwKilled;
        }
        vhProc.push_back(h);
    }
    // 统一轮询等待黑名单全部退出后再进入恢复阶段：Win11 25H2 实测，多
    // explorer 场景下 SystemTray(XAML) 宿主被 Terminate 后可能拖 5~10s
    // 才真正退出；若旧进程尚存活就开始恢复计时，Winlogon 尚未触发自恢复，
    // 观察窗口会被这段拖尾白白侵蚀（串行短等同样不可靠）。
    for (DWORD64 qwEndKill = ::GetTickCount64() + 12000;
         ::GetTickCount64() < qwEndKill; )
    {
        bool bStillAlive = false;
        std::vector<DWORD> vNow;
        EnumSessionExplorers(dwMySession, vNow);
        for (size_t a = 0; a < vNow.size() && !bStillAlive; ++a)
            for (size_t b = 0; b < vKill.size(); ++b)
                if (vNow[a] == vKill[b]) { bStillAlive = true; break; }
        if (!bStillAlive) break;
        PumpUiWhileWaiting(50);
    }
    for (size_t i = 0; i < vhProc.size(); ++i)
        ::CloseHandle(vhProc[i]);
    // 4) 等待 Winlogon AutoRestartShell 标准恢复（官方机制，见
    //    learn.microsoft.com cc939703：AutoRestartShell=1 时 shell 意外
    //    终止后系统自动重启）。完整 shell 判据：新句柄的 Shell_TrayWnd
    //    可见且 Progman 已创建——仅任务栏窗口存在不代表 shell 完整。
    //    轮询粒度 25ms，热路径通常 0.4~3s，不再有任何固定 Sleep 盲等。
    auto fnWaitFullShell = [&](DWORD dwTimeoutMs) -> HWND
    {
        for (DWORD64 qwEnd = ::GetTickCount64() + dwTimeoutMs;
             ::GetTickCount64() < qwEnd; )
        {
            HWND hT = ::FindWindowW(L"Shell_TrayWnd", nullptr);
            HWND hP = ::FindWindowW(L"Progman", nullptr);
            if (hT && hT != hOldTray && ::IsWindowVisible(hT) && hP)
                return hT;
            PumpUiWhileWaiting(25);
        }
        return nullptr;
    };
    // 会话内是否存在"shell 候选"explorer（父进程非 svchost：由 Winlogon
    // 链或本程序启动的无参 shell；/factory 代理不计入）。
    auto fnShellCandidateAlive = [&]() -> bool
    {
        std::vector<ExplorerProcInfo> vE;
        EnumSessionExplorersEx(dwMySession, vE);
        for (size_t i = 0; i < vE.size(); ++i)
            if (!vE[i].bFactoryProxy) return true;
        return false;
    };
    // 清除会话内全部 /factory COM 代理。COM 可能立即重拉，故循环补杀到
    // 连续无活代理（或超时）。必须在"手动启动 explorer"之前执行：只要还
    // 残留任意 explorer 映像，新启动的 explorer 就会走单实例转发自行退出，
    // shell 永不建立。
    DWORD dwProxyKilled = 0;
    auto fnKillFactoryAgents = [&](DWORD dwTimeoutMs) -> void
    {
        for (DWORD64 qwEnd = ::GetTickCount64() + dwTimeoutMs;
             ::GetTickCount64() < qwEnd; )
        {
            std::vector<ExplorerProcInfo> vE;
            EnumSessionExplorersEx(dwMySession, vE);
            bool bRound = false;
            for (size_t i = 0; i < vE.size(); ++i)
            {
                if (!vE[i].bFactoryProxy) continue;
                HandleGuard h(::OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE
                                        | PROCESS_QUERY_LIMITED_INFORMATION,
                                        FALSE, vE[i].dwPid));   // [C++17 R3] RAII close at iteration end
                if (!h) continue;
                DWORD ec = 0;
                if (::GetExitCodeProcess(h, &ec) && ec == STILL_ACTIVE &&
                    ::TerminateProcess(h, 1))
                {
                    ++dwProxyKilled;
                    bRound = true;
                }
                PumpWaitForHandle(h, 3000);
            }
            if (!bRound) break;   // 已稳定零代理
            PumpUiWhileWaiting(100);
        }
    };
    // 强杀路径的 Winlogon 等待：除双窗口判据外，若发现会话仅余 /factory
    // 代理（shell 候选为空），说明 Winlogon 已被抑制、不再可能自恢复，
    // 立即提前转入"清代理+零实例启动"，不空等。
    auto fnWaitWinlogon = [&](DWORD dwTimeoutMs) -> HWND
    {
        for (DWORD64 qwEnd = ::GetTickCount64() + dwTimeoutMs;
             ::GetTickCount64() < qwEnd; )
        {
            HWND hT = ::FindWindowW(L"Shell_TrayWnd", nullptr);
            HWND hP = ::FindWindowW(L"Progman", nullptr);
            if (hT && hT != hOldTray && ::IsWindowVisible(hT) && hP)
                return hT;
            std::vector<ExplorerProcInfo> vE;
            EnumSessionExplorersEx(dwMySession, vE);
            if (!vE.empty() && !fnShellCandidateAlive())
                return nullptr;
            PumpUiWhileWaiting(25);
        }
        return nullptr;
    };
    HWND hNewTray = nullptr;
    const wchar_t* pszPath = L"winlogon-auto";
    // 4) 恢复分两条路径：
    //  - 优雅退出成功：WM_USER+436 属官方"退出资源管理器"语义，Winlogon
    //    的 AutoRestartShell 只针对意外终止，此路径不会自动拉起（Win11
    //    25H2 三轮实测：窗口 0.1~0.5s 消失后会话保持零 explorer、不自恢复，
    //    空等 12s 已证实浪费）。旧进程已在上一步逐个 PumpWaitForHandle
    //    确认退出，立即用 shell 令牌启动，新 shell 实测约 300ms 双窗口就绪。
    //  - 强杀路径：先等 Winlogon AutoRestartShell 标准恢复（官方机制见
    //    learn.microsoft.com cc939703：AutoRestartShell=1 时 shell 意外
    //    终止后系统自动重启），热路径 0.4~3s。
    // 两种路径的完整 shell 判据一致：新句柄的 Shell_TrayWnd 可见且
    // Progman 已创建——仅任务栏窗口存在不代表 shell 完整。
    if (bGrace)
    {
        fnKillFactoryAgents(3000);
        StartExplorerViaToken(hImpToken);
        pszPath = L"graceful-manual";
        hNewTray = fnWaitFullShell(15000);
    }
    else
    {
        // 零 explorer 时给 Winlogon 5s 自恢复窗口（典型 0.4~3s）；实测
        // 某些终止路径下 Winlogon 不动作，超时即转入零实例启动兜底。
        hNewTray = fnWaitWinlogon(5000);
    }
    // 5) 未在窗口内完整恢复：区分两种情况——
    //    a) 新 explorer 已在但窗口未就绪（XAML 冷启动，Progman 可能滞后
    //       数十秒，官方 KB5072911 亦记载该组件初始化慢）：只继续等，绝不
    //       再启动一个（单实例竞争会加剧残缺）；
    //    b) 会话已无 explorer（AutoRestartShell 被策略关闭等）：用 shell
    //       令牌手动启动一次。
    if (!hNewTray)
    {
        if (fnShellCandidateAlive())
        {
            pszPath = L"cold-start-wait";
            hNewTray = fnWaitFullShell(12000);
        }
        else
        {
            fnKillFactoryAgents(3000);
            StartExplorerViaToken(hImpToken);
            pszPath = L"clean-launch";
            hNewTray = fnWaitFullShell(15000);
        }
    }
    // 6) 仍失败：按官方 KB5072911 重新注册 XAML shell 包后再启动一次。
    bool bRepaired = false;
    if (!hNewTray)
    {
        bRepaired = RepairShellXamlPackages(hImpToken);
        if (!fnShellCandidateAlive())
        {
            fnKillFactoryAgents(3000);
            StartExplorerViaToken(hImpToken);
        }
        pszPath = L"xaml-repackage";
        hNewTray = fnWaitFullShell(20000);
    }
    if (hImpToken) ::CloseHandle(hImpToken);
    // 任务栏窗口重新出现且可见即视为成功。Progman 在 XAML 冷启动/刚修复后
    // 可能滞后自行创建（实测 30~60s 内自愈），不再为此反复重启 shell
    // ——反复强杀/拉起正是 XAML 包状态损坏与残缺 shell 的直接诱因。
    bool bProgman = ::FindWindowW(L"Progman", nullptr) != nullptr;
    bool bOk = (hNewTray != nullptr);
    DWORD64 qwElapsed = ::GetTickCount64() - qwStart;
    CDuiString sLog;
    sLog.Format(L"[optim] explorer restart: ok=%d graceful=%d killed=%lu "
                   "proxy=%lu progman=%d repaired=%d path=%s %llums",
                bOk ? 1 : 0, bGrace ? 1 : 0, (unsigned long)dwKilled,
                (unsigned long)dwProxyKilled,
                bProgman ? 1 : 0, bRepaired ? 1 : 0,
                pszPath, (unsigned long long)qwElapsed);
    AppendLog(sLog);
    // 恢复输入前，再清一次重启期间误入队列的点击/命令：shell 已重建，
    // 但旧 combo 弹层（其 HWND/对象可能已失效）绝不能再收到这些消息——
    // 这正是 0xc0000409 子码 0xa（CFG icall）崩溃的最后一道闸门。
    PurgeStaleInputMessages();
    ::EnableWindow(m_hWnd, TRUE);
    m_bRestartingExplorer = false;
    ::SetForegroundWindow(m_hWnd);
    ::SetCursor(hOldCursor);
    // [FIX-TRAY] 托盘注册挂在旧 shell 进程上，explorer 被杀即整体失效。
    //   新 shell 广播 TaskbarCreated 后本窗口会走统一重注册路径
    //   （FrameWnd_SysMsg.cpp），但广播可能在窗口禁用期间到达而丢失时序；
    //   成功路径在此兜底：先清旧注册再 NIM_ADD，与 TaskbarCreated 路径
    //   互不冲突（重复 NIM_ADD 会形成双图标，故必须先删后加）。
    if (bOk)
    {
        if (m_bTrayAdded) RemoveTrayIcon();
        InitTrayIcon();
    }
    if (!bOk)
    {
        ::MessageBox(m_hWnd,
                     CDuiString(StringMgr.Get("misc.s95c1224e")) + StringMgr.Get("shortcut.s2dd29f0f") + StringMgr.Get("misc.scfdf2223"),
                     StringMgr.Get("toolbox.restart_expl"), MB_OK | MB_ICONWARNING);
    }
    return bOk;
}


void CFrameWnd::ImportConfig()
{
    wchar_t szFile[MAX_PATH] = { 0 };
    OPENFILENAME ofn = { 0 };
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = m_hWnd;
    ofn.lpstrFilter = L"DismConsole config (*.xml)\0*.xml\0All files\0*.*\0";
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!::GetOpenFileName(&ofn)) return;


    FileGuard hFile = ::CreateFile(szFile, GENERIC_READ, FILE_SHARE_READ,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        AppendLog(L"[import] cannot open file");
        return;
    }
    DWORD dwSize = ::GetFileSize(hFile, nullptr);
    if (dwSize == INVALID_FILE_SIZE || dwSize == 0)
    {


        AppendLog(L"[import] empty file");
        return;
    }
    std::vector<BYTE> bufBuf(dwSize, 0);
    BYTE* pBuf = bufBuf.data();
    DWORD dwRead = 0;
    BOOL bReadOk = ::ReadFile(hFile, pBuf, dwSize, &dwRead, nullptr);


    if (!bReadOk || dwRead != dwSize)
    {
        AppendLog(L"[import] read failed");
        return;
    }


    CMarkup mk;
    bool bLoaded = mk.LoadFromMem(pBuf, dwSize);


    if (!bLoaded || !mk.IsValid())
    {
        AppendLog(L"[import] not a valid XML config");
        return;
    }


    // name -> (g, i, combo)
    struct KeyInfo { CDuiString sName; int g; int i; bool bCombo; };
    std::vector<KeyInfo> keys;


    CMarkupNode optRoot = m_dataEngine.GetOptimizationRoot();
    int g = 0;
    CMarkupNode group = optRoot.GetChild(L"Group");
    while (group.IsValid())
    {
        int i = 0;
        CMarkupNode item = group.GetChild(L"Item");
        while (item.IsValid())
        {
            KeyInfo info;
            info.sName = DismResolveText(item.GetAttributeValue(L"Name"));
            info.g = g;
            info.i = i;
            info.bCombo = lstrcmpiW(item.GetAttributeValue(L"Type"), L"Combo") == 0;
            keys.push_back(info);
            ++i;
            item = item.GetSibling();
        }
        ++g;
        group = group.GetSibling();
    }


    int nApplied = 0, nFailed = 0, nSkipped = 0;
    bool bAnyRestart = false;
    CMarkupNode on = mk.GetRoot().GetChild(L"Optim");
    while (on.IsValid())
    {
        CDuiString sKey = on.GetAttributeValue(L"Name");
        CDuiString sVal = on.GetAttributeValue(L"Value");


        for (size_t k = 0; k < keys.size(); ++k)
        {
            if (lstrcmpiW(keys[k].sName.GetData(), sKey.GetData()) != 0) continue;


            CMarkupNode impItem = FindOptimItem(optRoot, keys[k].g, keys[k].i);
            if (!impItem.IsValid()) break;


            // [SCOPE-2COL][EXPORT-SYMMETRY] 逐列回放；旧格式无 Scope 属性时作用于全部可见列
            for (int sc : optim_scope::kColumns)
            {
                if (!optim_scope::ImportTargetsScope(on.GetAttributeValue(L"Scope"), sc)) continue;
                if (!impItem.GetChild(m_dataEngine.ScopeName(sc)).IsValid()) continue;


                std::vector<CDuiString> vOut;
                int nUnsupported = 0;
                bool bRestart = false;
                bool bOk = false;


                if (keys[k].bCombo)
                {
                    CDuiString sPrefix = L"combo:";
                    int iSel = 0;
                    if (wcsncmp(sVal.GetData(), sPrefix.GetData(), sPrefix.GetLength()) == 0)
                        iSel = _ttoi(sVal.Mid(static_cast<int>(sPrefix.GetLength())).GetData());


                    int nCur = m_dataEngine.GetScopeDropdown(impItem, sc);
                    if (iSel == nCur) { ++nSkipped; continue; }
                    bOk = m_dataEngine.SelectScopeDropdown(impItem, sc, iSel,
                                                           vOut, nUnsupported, &bRestart);
                }
                else
                {
                    bool bWanted = _ttoi(sVal.GetData()) == 1;
                    int nCur = m_dataEngine.GetScopeState(impItem, sc);
                    if (bWanted == (nCur == 1)) { ++nSkipped; continue; }
                    bOk = m_dataEngine.SetScopeState(impItem, sc, bWanted,
                                                     vOut, nUnsupported, &bRestart);
                }


                if (bOk) { ++nApplied; if (bRestart) bAnyRestart = true; }
                else     ++nFailed;
            }
            break;
        }


        on = on.GetSibling();
    }


    CDuiString sLog;
    sLog.Format(L"[import] applied=%d failed=%d unchanged=%d",
                nApplied, nFailed, nSkipped);
    AppendLog(sLog.GetData());


    if (bAnyRestart &&
        ::MessageBox(m_hWnd,
                     StringMgr.Get("confirm.sdcafe4ec"),
                     StringMgr.Get("toolbox.restart_expl"),
                     MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES)
    {
        // 延迟到当前菜单命令消息栈排空后执行（与优化页点击路径同理，
        // 见 WM_HOST_REQUEST_RESTART_EXPLORER 定义处的崩溃说明）。
        ::PostMessageW(m_hWnd, WM_HOST_REQUEST_RESTART_EXPLORER, 0, 0);
    }


    // sync all controls with the real state
    // [R5] 强制核实优化状态经命令虚入口（HCMD）
    if (CPageBase* pPage = PageById(EPageId::PAGE_OPTIM)) pPage->OnHostCommand(static_cast<int>(EHostCmd::HCMD_OPTIM_VERIFY_FORCE));
}


// O-3: 导出当前优化配置为可被「导入配置」直接读取的扁平 XML 片段。
// 格式与 ImportConfig 对齐：<Optim Name="..." Value="1|0|combo:N"/>


void CFrameWnd::RefreshAll()
{
    // 扫描/清理线程正在读写 m_cleanEngine 时重载规则会导致 UAF，直接拒绝
    // P6b：忙碌态归 CleanJobService（扫描态/删除态/工作线程句柄）
    if (CleanJobService::IsBusy())
    {
        AppendLog(L"[engine] busy (scan/clean running), reload rejected");
        ::MessageBox(m_hWnd,
            StringMgr.Get("cleanup.sf32bcaef"),
            StringMgr.Get("misc.s7c1efe79"), MB_OK | MB_ICONINFORMATION);
        return;
    }


    AppendLog(L"[engine] reload requested");


    // S-12: CleanEngine/DataEngine 加载中间态。
    //   DataEngine.Load + CleanEngine.Init 是同步阻塞操作，规则量大时可能耗时
    //   数百毫秒到数秒，期间用户看不到任何反馈会误判为"卡死"。此处：
    //   1) 禁用主操作按钮避免二次触发；
    //   2) 状态条切换到 "正在加载…" 提示；
    //   3) 强制刷新一次让状态条可见后再执行阻塞调用。
    // 加载完成或失败后恢复到 status.ready。
    if (CControlUI* p = m_pm.FindControl(L"btn_refresh")) p->SetEnabled(false);
    if (CControlUI* p = m_pm.FindControl(L"btn_scan"))    p->SetEnabled(false);
    if (CControlUI* p = m_pm.FindControl(L"btn_clean"))   p->SetEnabled(false);
    if (CTextUI* pStatus = static_cast<CTextUI*>(m_pm.FindControl(L"app_status")))
        pStatus->SetText(StringMgr.Get("status.loading_hint"));
    // 泵一下消息，保证状态条在真正阻塞前渲染出来
    ::UpdateWindow(m_hWnd);


    CDuiString strPath;
    if (m_dataEngine.Load(strPath))
    {
        m_strDataPath = strPath;
        m_cleanEngine.Init(&m_dataEngine);
        // [R5] 初始化阶段构建经页序+基类虚入口，壳不持具体页类型
        if (CPageBase* pPage = PageById(EPageId::PAGE_CLEANUP)) pPage->Build();
        if (CPageBase* pPage = PageById(EPageId::PAGE_OPTIM)) pPage->Build();
        // [R5] 强制核实优化状态经命令虚入口（HCMD）
        if (CPageBase* pPage = PageById(EPageId::PAGE_OPTIM)) pPage->OnHostCommand(static_cast<int>(EHostCmd::HCMD_OPTIM_VERIFY_FORCE));
        // [R5] BuildStartup() 即 CPageStartup::Build()，统一基类虚入口
        if (CPageBase* pPage = PageById(EPageId::PAGE_STARTUP)) pPage->Build();


        CDuiString s;
        s.Format(L"[engine] reloaded: %d rules / %d groups",
                 m_cleanEngine.GetRuleCount(), m_cleanEngine.GetGroupCount());
        AppendLog(s.GetData());
    }
    else
    {
        AppendLog(L"[engine] reload failed");
        AppendLog(m_dataEngine.GetLastError());


        // C-9: Load 失败时给状态条一个明确的错误文案（不只停留在 loading_hint）
        // + 弹一次错误框，让用户立即知道本次刷新没生效。
        // 之前失败时只是把 status 恢复到 ready，用户看不到"为什么没刷新"。
        if (CTextUI* pStatus = static_cast<CTextUI*>(m_pm.FindControl(L"app_status")))
            pStatus->SetText(StringMgr.Get("status.load_failed"));
        // UI 微调：把错误详情自动复制到剪贴板，用户可一键 Ctrl+V 到工单。
        CDuiString sErrMsg;
        sErrMsg.Format(L"Win8Metro Data.xml load failed:\n%s",
                       m_dataEngine.GetLastError());
        FrameWnd_Util_CopyTextToClipboard(m_hWnd, sErrMsg.GetData());
        // MessageBox 用独立 CDuiString 拼接，避免链式调用 .GetData() 触发二义性
        CDuiString sMsgText;
        sMsgText.Format(StringMgr.Get("misc.h4c58"),
                        StringMgr.Get("error.load_data"),
                        m_dataEngine.GetLastError());
        ::MessageBox(m_hWnd,
            sMsgText.GetData(),
            StringMgr.Get("error.load_title"),
            MB_OK | MB_ICONERROR);
    }


    // S-12: 恢复状态条与操作按钮
    if (CTextUI* pStatus = static_cast<CTextUI*>(m_pm.FindControl(L"app_status")))
        pStatus->SetText(StringMgr.Get("status.ready"));
    if (CControlUI* p = m_pm.FindControl(L"btn_refresh")) p->SetEnabled(true);
    if (CControlUI* p = m_pm.FindControl(L"btn_scan"))    p->SetEnabled(true);
    if (CControlUI* p = m_pm.FindControl(L"btn_clean"))   p->SetEnabled(true);
}


// ============================================================
// P6b: cleanup page UI moved to CPageCleanup, worker pipeline to
// CleanJobService; everything below is a one-line thin forward.
// ============================================================
void CFrameWnd::CloseWorkerThread() { CleanJobService::DetachThread(); }
// ============================================================
// optimization events: applied immediately, then re-verified
// against the real registry/service state
// ============================================================


// ============================================================
// startup page
// ============================================================


// P3b：StartupSrcType 枚举（SRC_HKCU_RUN..SRC_BOOT_EXECUTE）已随 S3 迁 StartupService.h


// P3b：以下 8 个启动/服务页函数已迁 CPageStartup（薄转发，收尾两级路由落地后删除）
// ============================================================
// P2b：以下 9 个任务页函数已迁 CPageTasks（Page_Tasks.h/.cpp）；
// 核心壳暂留同名薄转发（Notify 路由零改动），收尾两级路由落地后删除。
// ============================================================


// ============================================================
// [R4 A3 部分实现] 系统优化规则右键菜单
// 规则树（CMarkupNode）在本 DuiLib 版本中为只读树，无删除/追加/改名 API，
// 故删除/复制/剪切/粘贴如实灰化；"属性"只读展示规则名称与说明。
// ============================================================


// 来源 → Run 注册表根（StartupSrcType::SRC_BOOT_EXECUTE 无 Run 键）
// P3b：StartupFindRunKey 已随 S3 迁 StartupService::FindRunKey


// P3b：以下 4 个启动项函数已迁 CPageStartup（薄转发，收尾两级路由落地后删除）


// ============================================================
// file association page
// ============================================================
// [R55] 图标基础设施：EnsureIconPng 把 exe/.ext 图标缓存为 PNG
// 说明：SHGetFileInfo 取 HICON → 通过 SHGetFileInfo+DrawIcon 或后续 R56
//       GDI+ 编码链路转成 PNG 存盘。本版本先落地目录与命中/未命中日志，
//       让文件关联 / 程序和功能图标渲染的架构就位，UI 集成在 R56 完成。


// S12：8 个诊断快照函数已迁入 DiagnosticsService.h/.cpp
// （SysInfo/Top20 进程，另含 Services/BootEvents/DriverSign/Rollback/Fragmentation/BootTime 见下方同批迁移）


// P2c：去 static（声明在 FrameWnd_Util.h），供关联页/驱动页共享
CDuiString EnsureIconPngReady(const wchar_t* pszTarget)
{
    CDuiString sOut;
    if (!pszTarget || pszTarget[0] == 0) return sOut;


    // 1) 缓存目录
    CDuiString sDir;
    sDir.Format(L"%s\\DismTool_Clone\\icons", winenv::SafeGetEnv(L"LOCALAPPDATA").c_str());
    DWORD dwAttr = ::GetFileAttributes(sDir.GetData());
    if (dwAttr == INVALID_FILE_ATTRIBUTES)
    {
        // 逐级创建（DismTool_Clone 层可能也不存在）
        if (!FrameWnd_Util_EnsureDirectoryDeep(sDir.GetData()))
        {
            CDuiString sErr;
            sErr.Format(StringMgr.Get("misc.h7176"),
                        sDir.GetData(), (int)::GetLastError());
            // 静默降级：不追加日志（避免刷屏），直接返回空
            return sOut;
        }
    }


    // 2) 计算缓存 key（哈希 target 首 32 字节，防止路径名过长）
    unsigned long h = 2166136261UL;
    for (int i = 0; i < 32 && pszTarget[i]; ++i)
    {
        h = (h ^ (unsigned char)pszTarget[i]) * 16777619UL;
    }
    CDuiString sPngPath;
    sPngPath.Format(L"%s\\%08lx.png", sDir.GetData(), h);


    // 3) 命中缓存 → 返回路径
    if (::GetFileAttributes(sPngPath.GetData()) != INVALID_FILE_ATTRIBUTES)
    {
        return sPngPath;
    }
    return sOut;
}


// S12：SnapshotServices/ParseBootEvents/VerifyDriverSignatures/EnumerateRollbackCandidates/AnalyzeDiskFragmentation/AppendBootTimeSnapshot 已迁入 DiagnosticsService.cpp
// P2c：去 static（声明在 FrameWnd_Util.h），供关联页/驱动页共享
void EnsureIconCacheDirectory()
{
    CDuiString sDir;
    sDir.Format(L"%s\\DismTool_Clone\\icons", winenv::SafeGetEnv(L"LOCALAPPDATA").c_str());
    DWORD dwAttr = ::GetFileAttributes(sDir.GetData());
    if (dwAttr != INVALID_FILE_ATTRIBUTES) return;  // 已存在
    // 尝试逐级建目录（失败静默，图标是可选增强）
    FrameWnd_Util_EnsureDirectoryDeep(sDir.GetData());
}


// ============================================================
// P2c：关联页函数已迁 CPageAssoc（Page_Assoc.h/.cpp）；
// UserChoice 写操作/dism 关联命令在 AssocService（S8）。
// 核心壳暂留同名薄转发（Notify 路由零改动），收尾两级路由落地后删除。
// ============================================================


// ============================================================
// toolbox page (grid of 11 self-drawn colorful icons)
// ============================================================


// ============================================================
// [原生化 B1] 系统修复（菜单 1201/1202 + CLI /RestoreHealth）：
//   * 修复阶段：DismApiBridge::RestoreImageHealth（DismRestoreImageHealth
//     进程内直连，等价 dism /Online|/Image /Cleanup-Image /RestoreHealth），
//     不再生成/执行 repair.bat、不启动 dism.exe；
//   * sfc 阶段：sfc /scannow 无任何公开 Win32 API 可替代，保留系统自带的
//     sfc.exe（System32 原生系统组件，非随包载荷、非降级兜底），但改为
//     CreateProcessW 直接挂到自建控制台，彻底去除 cmd.exe 批处理包裹；
//     离线映像不支持 sfc，自动跳过（与原 bat 语义一致）。
// ============================================================
static void RepairConWrite(HANDLE hOut, const std::wstring& sLine)
{
    if (!hOut || hOut == INVALID_HANDLE_VALUE) return;
    std::wstring s = sLine;
    s += L"\r\n";
    DWORD dwWritten = 0;
    ::WriteConsoleW(hOut, s.c_str(), static_cast<DWORD>(s.size()), &dwWritten, nullptr);
}
static bool RepairRunSfc(HANDLE hOut, HANDLE hIn)
{
    wchar_t szSys[MAX_PATH] = {};
    ::GetSystemDirectoryW(szSys, MAX_PATH);
    std::wstring sExe = std::wstring(szSys) + L"\\sfc.exe";
    if (::GetFileAttributesW(sExe.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        RepairConWrite(hOut, L"[错误] 未找到 " + sExe);
        return false;
    }
    std::wstring sCmd = L"\"" + sExe + L"\" /scannow";
    std::vector<wchar_t> vBuf(sCmd.begin(), sCmd.end());
    vBuf.push_back(0);
    STARTUPINFOW si;
    si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput  = hIn;
    si.hStdOutput = hOut;
    si.hStdError  = hOut;
    PROCESS_INFORMATION pi;
    pi = {};
    if (!::CreateProcessW(nullptr, vBuf.data(), nullptr, nullptr, TRUE,
                          0, nullptr, nullptr, &si, &pi))
    {
        RepairConWrite(hOut, L"[错误] sfc.exe 启动失败");
        return false;
    }
    // [P0-#1] INFINITE 死锁风险：进程失控（如 sfc 挂死）会让此分支永久阻塞，
    // 且窗口线程因此退出失败、残留僵尸控制台。改为 2 小时限时等待：
    //   超时即按失败降级（返回 false），不杀进程、不破坏系统组件状态。
    // [C++17 R3] RAII close at scope exit; hProc declared last so it closes first (original order)
    HandleGuard hThr(pi.hThread);
    HandleGuard hProc(pi.hProcess);
    DWORD dwWait = ::WaitForSingleObject(pi.hProcess, 2 * 60 * 60 * 1000);
    if (dwWait != WAIT_OBJECT_0)
    {
        RepairConWrite(hOut, dwWait == WAIT_TIMEOUT
            ? L"[超时] sfc /scannow 超过 2 小时未结束，已降级为失败（未强杀）。"
            : L"[错误] sfc /scannow 等待失败，已降级为失败。");
        return false;
    }
    DWORD dwCode = 1;
    ::GetExitCodeProcess(pi.hProcess, &dwCode);
    RepairConWrite(hOut, dwCode == 0
        ? L"[完成] sfc /scannow 已结束。"
        : L"[提示] sfc /scannow 返回码=" + std::to_wstring(dwCode)
          + L"（部分问题需重启后处理）。");
    return dwCode == 0;
}
struct RepairJob
{
    bool        bRestore;   // true=先执行 DismRestoreImageHealth
    bool        bSfc;       // true=随后 sfc /scannow（离线时内部强制跳过）
    std::wstring sRoot;     // 离线映像根；空串=在线系统
};
static DWORD WINAPI RepairThreadProc(void* pv)
{
    std::unique_ptr<RepairJob> sp(reinterpret_cast<RepairJob*>(pv));
    bool bAlloc = (::GetConsoleWindow() == nullptr);
    if (bAlloc && !::AllocConsole())
        return 0;
    ::SetConsoleTitleW(L"Win8Metro 系统修复（系统文件检查 / 映像修复）");
    HANDLE hOut = ::GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE hIn  = ::GetStdHandle(STD_INPUT_HANDLE);
    RepairConWrite(hOut, L"============================================================");
    RepairConWrite(hOut, L" Win8Metro 系统修复");
    RepairConWrite(hOut, L" 原生化执行：DismRestoreImageHealth 进程内调用 + sfc.exe 直连");
    RepairConWrite(hOut, L"============================================================");
    RepairConWrite(hOut, L"");
    if (sp->bRestore)
    {
        RepairConWrite(hOut, sp->sRoot.empty()
            ? L"[1/2] 正在修复在线系统映像（RestoreHealth），耗时可能较长，请稍候…"
            : L"[1/2] 正在修复离线映像（/Image）：" + sp->sRoot + L"，请稍候…");
        DismApiBridge::Init();
        std::wstring sErr;
        bool bOk = DismApiBridge::RestoreImageHealth(
            L"", false, &sErr,
            sp->sRoot.empty() ? nullptr : sp->sRoot.c_str());
        if (bOk)
            RepairConWrite(hOut, L"[完成] 映像修复操作已结束（未报告致命错误）。");
        else
        {
            RepairConWrite(hOut, L"[错误] 映像修复失败：" +
                (sErr.empty() ? std::wstring(L"DismRestoreImageHealth 返回失败") : sErr));
            RepairConWrite(hOut, L"       可尝试指定安装介质 /Source 后重试。");
        }
        RepairConWrite(hOut, L"");
    }
    if (sp->bSfc && sp->sRoot.empty())
    {
        RepairConWrite(hOut, sp->bRestore ? L"[2/2] 正在执行系统文件检查（sfc /scannow）…"
                                          : L"正在执行系统文件检查（sfc /scannow）…");
        RepairRunSfc(hOut, hIn);
        RepairConWrite(hOut, L"");
    }
    else if (sp->bSfc && !sp->sRoot.empty())
    {
        RepairConWrite(hOut, L"[2/2] 离线映像不支持 sfc，已跳过（请在目标系统启动后执行）。");
        RepairConWrite(hOut, L"");
    }
    RepairConWrite(hOut, L"操作全部结束，按回车键关闭此窗口…");
    wchar_t szIn[8] = {};
    DWORD dwRead = 0;
    if (hIn && hIn != INVALID_HANDLE_VALUE)
        ::ReadConsoleW(hIn, szIn, static_cast<DWORD>(_countof(szIn)), &dwRead, nullptr);
    if (bAlloc) ::FreeConsole();
    return 0;
}
// ============================================================
// P6a：bat 公共执行器（BatRunCtx / WrapBatWithFailGuard / ParseBatRcFile /
// ArchiveToolRun / LaunchBatGuarded / FinalizeToolRun / ReadToolRunRc）
// 已迁入 ExternalToolService（S9）。RunBatFile 保留下方薄壳：
// UI 提示/日志在壳，进程/UAC/归档在 service。
// ============================================================


bool CFrameWnd::RunBatFile(const CDuiString& sWorkDir, const wchar_t* pszBatName,
                           const CDuiString& sBody, HANDLE* phProcess)
{
    // P6a：进程/UAC/归档能力下沉 ExternalToolService（S9），本壳保留 UI 收尾
    bool bOk = ExternalToolService::RunBatGuarded(m_hWnd, pszBatName, sBody, sWorkDir,
                                                  true /*bElevated*/, true /*bAppendFailGuard*/,
                                                  phProcess);
    if (!bOk)
    {
        // 用户拒绝 UAC 时给友好提示；错误详情已在归档日志里
        ::MessageBox(m_hWnd, StringMgr.Get("general.sa8a28bf8"),
                     StringMgr.Get("toolbox.s4e6bf87f"), MB_OK | MB_ICONINFORMATION);
        return false;
    }
    AppendLog(CDuiString(StringMgr.Get("toolbox.sf84b92ff")) + sWorkDir + L"\\" + pszBatName);
    // [R-B2] 选项→操作完成后 真正消费（原来只设变量不触发，审计判定为假开关）
    if (m_nAfterAction == 1 || m_nAfterAction == 2)
    {
        BOOL bReboot = (m_nAfterAction == 2);
        AppendLog(bReboot ? L"[after-action] restart after task"
                          : L"[after-action] shutdown after task");
        ::InitiateSystemShutdown(nullptr, nullptr, 5, TRUE, bReboot ? TRUE : FALSE);
        m_nAfterAction = 0;   // one-shot
    }
    return true;
}
// [原生化 B1] 启动系统修复控制台任务（替代 verify.bat/repair.bat/
// restorehealth.bat）。bRestoreHealth=执行 DismRestoreImageHealth；
// bRunSfc=随后执行 sfc /scannow（离线映像自动跳过）。
void CFrameWnd::StartSystemRepair(bool bRestoreHealth, bool bRunSfc)
{
    // [C++17 R3] thread ctx: ownership transferred to RepairThreadProc (takes it over with unique_ptr); guard frees on start failure
    std::unique_ptr<RepairJob> pJob = std::make_unique<RepairJob>();
    pJob->bRestore = bRestoreHealth;
    pJob->bSfc = bRunSfc;
    pJob->sRoot = COfflineSession::IsActive()
        ? std::wstring(COfflineSession::GetImageRoot().GetData())
        : std::wstring();
    HandleGuard h(SafeCreateThread(&RepairThreadProc, pJob.get()));   // [C++17 R3] RAII close at scope exit
    if (h) pJob.release();   // ownership transferred to RepairThreadProc
}


// ============================================================
// IToolHost — 给 CToolDialogPage 提供真实能力
// ============================================================
HWND CFrameWnd::HostHwnd() { return m_hWnd; }
int  CFrameWnd::HostDPI()  { return m_nDPI; }
// P3b：等价原页内 m_pm.GetDPIObj()->GetScale()（百分比）
int CFrameWnd::HostDPIScale() { return m_pm.GetDPIObj()->GetScale(); }
// P3b：页按名读编辑框（ReadEditValue 声明在 FrameWnd_Util.h）
CDuiString CFrameWnd::HostReadEditValue(const wchar_t* pszName) { return ReadEditValue(m_pm, pszName); }
// ---- P4：Appx/可选功能/更新页宿主 helper（原页内 static helper 薄封装）----
CDuiString CFrameWnd::HostSkinText(const wchar_t* pszName)
{
    // [R216] Never hand back an empty string for a *loading* key. Those keys have no
    // owning control by design (the hint row is built in code), so the control-text
    // fallback inside GetSkinText resolves to "" and the placeholder rendered as a
    // bare spinner. Every other key keeps its existing behaviour.
    if (pszName && wcsncmp(pszName, L"lbl_", 4) == 0 && wcsstr(pszName, L"_loading"))
    {
        const CDuiString sHint = GetLoadingHintText(pszName);
        if (!sHint.IsEmpty()) return sHint;
    }
    return GetSkinText(m_pm, pszName);
}
void CFrameWnd::HostSetCtrlText(const wchar_t* pszName, const wchar_t* pszText) { SetCtrlText(m_pm, pszName, pszText); }


bool CFrameWnd::HostOpenFile(CDuiString& sResult, const wchar_t* pszFilter, const wchar_t* pszTitle)
{
    return Win32DialogService::DlgOpenFile(sResult, pszFilter, pszTitle);
}


bool CFrameWnd::HostSaveFile(CDuiString& sResult, const wchar_t* pszFilter,
                             const wchar_t* pszTitle, const wchar_t* pszDefName)
{
    return Win32DialogService::DlgSaveFile(sResult, pszFilter, pszTitle, pszDefName);
}


bool CFrameWnd::HostBrowseFolder(CDuiString& sResult, const wchar_t* pszTitle)
{
    return Win32DialogService::DlgBrowseFolder(sResult, pszTitle);
}


bool CFrameWnd::HostRunBat(const wchar_t* pszBatName, const CDuiString& sBody, HANDLE* phProcess)
{
    CDuiString sWork = SystemInfoService::MakeTempWorkDir(L"tool");
    return RunBatFile(sWork, pszBatName, sBody, phProcess);
}


void CFrameWnd::HostLog(const CDuiString& sLine, int nLevel)
{
    AppendLogEx(sLine.GetData(), nLevel);
}


// [P1-收尾] 索引读取：wimgapi 直连唯一路径（WIMCreateFile→GetImageCount→
// LoadImage→GetImageInformation），零 dism /Get-WimInfo 外部进程。
// 失败按 GLE（5 只读 / 2 找不到 / 11、1920 加密 ESD）映射中文提示。
bool CFrameWnd::HostQueryWim(const CDuiString& sWim,
                             std::vector<TDialogWimIndex>& vOut,
                             CDuiString* psErr)
{
    WIMBridge::Init();
    if (!WIMBridge::IsReady())
    {
        if (psErr) *psErr = StringMgr.Get("error.sa093b84c");
        AppendLogEx(L"[wimgapi] wimgapi.dll 不可用，无法读取映像索引", static_cast<int>(ELogLevel::LOG_ERROR));
        return false;
    }
    std::vector<WIMBridge::WimInfo> vInfos;
    std::wstring sApiErr;
    if (!WIMBridge::QueryWimInfo(std::wstring(sWim.GetData()), vInfos, &sApiErr)
        || vInfos.empty())
    {
        // 从 "Open failed;gle=11" 抽取 GLE 映射中文原因
        DWORD dwGle = 0;
        size_t nP = sApiErr.find(L"gle=");
        if (nP != std::wstring::npos) dwGle = static_cast<DWORD>(_wtoi(sApiErr.c_str() + nP + 4));
        if (psErr)
        {
            CDuiString s;
            if (dwGle != 0)
            {
                CDuiString sGle;
                sGle.Format(L"%lu", dwGle);
                s.Format(StringMgr.Get("error.s7d9a734e"), sGle.GetData());
            }
            else
                s = StringMgr.Get("toolbox.s2da5e509");
            const wchar_t* pszHint = nullptr;
            if (dwGle == 5)        pszHint = StringMgr.Get("toolbox.sa065a836");
            else if (dwGle == 2)   pszHint = StringMgr.Get("error.s9201537f");
            else if (dwGle == 11 || dwGle == 1920)
                                           pszHint = StringMgr.Get("edit.s22e23224");
            if (pszHint) s += pszHint;
            *psErr = s;
        }
        AppendLogEx(L"[wimgapi] QueryWimInfo 失败（原生路径，无外部回退）", static_cast<int>(ELogLevel::LOG_WARN));
        return false;
    }
    for (size_t i = 0; i < vInfos.size(); ++i)
    {
        TDialogWimIndex ix;
        ix.nIndex  = static_cast<int>(vInfos[i].index);
        ix.sName   = vInfos[i].name.c_str();
        ix.sDesc   = vInfos[i].description.c_str();
        ix.sEdition = vInfos[i].edition.c_str();
        ix.sVersion = vInfos[i].version.c_str();
        ix.sArch   = vInfos[i].architecture.c_str();
        // 全版本产品分析（Win7~11 / Server 2003~2026）
        WIMBridge::ProductIdentity pi = WIMBridge::ResolveProduct(
            vInfos[i].version, vInfos[i].edition,
            vInfos[i].architecture);
        ix.sProduct = pi.summary.c_str();
        ix.sCreated = vInfos[i].creationDate.c_str();
        if (vInfos[i].size > 0)
            ix.sSize.Format(L"%llu", vInfos[i].size);
        if (ix.sName.IsEmpty()) ix.sName = StringMgr.Get("toolbox.s2061bb69");
        vOut.push_back(ix);
    }
    return true;
}


// ============================================================
// [R16][T6] toolsvc::IToolServiceHost —— 核心壳直接实现（wstring 语义）。
// 旧 CToolServiceHostBridge 转换层取消：边界转换收敛到本宿主壳，
// 各方法薄转发既有 CDuiString 实现（重载按参数类型精确匹配）。
// HostNotify/HostProgress 仅 UI 使用，服务层不需要，保留在 IToolHost。
// ============================================================
bool CFrameWnd::HostOpenFile(std::wstring& sResult, const std::wstring& sFilter,
                             const std::wstring& sTitle)
{
    CDuiString s;
    if (!HostOpenFile(s, sFilter.c_str(), sTitle.c_str())) return false;
    sResult = s.GetData();
    return true;
}
bool CFrameWnd::HostSaveFile(std::wstring& sResult, const std::wstring& sFilter,
                             const std::wstring& sTitle, const std::wstring& sDefName)
{
    CDuiString s;
    if (!HostSaveFile(s, sFilter.c_str(), sTitle.c_str(),
                      sDefName.empty() ? nullptr : sDefName.c_str())) return false;
    sResult = s.GetData();
    return true;
}
bool CFrameWnd::HostBrowseFolder(std::wstring& sResult, const std::wstring& sTitle)
{
    CDuiString s;
    if (!HostBrowseFolder(s, sTitle.c_str())) return false;
    sResult = s.GetData();
    return true;
}
bool CFrameWnd::HostQueryWim(const std::wstring& sWim,
                             std::vector<toolsvc::TWimIndex>& vOut,
                             std::wstring* psErr)
{
    std::vector<TDialogWimIndex> v;
    CDuiString sErr;
    bool b = HostQueryWim(CDuiString(sWim.c_str()), v, psErr ? &sErr : nullptr);
    if (b)
    {
        vOut.reserve(v.size());
        for (size_t i = 0; i < v.size(); ++i)
        {
            const TDialogWimIndex& q = v[i];
            toolsvc::TWimIndex z;
            z.nIndex   = q.nIndex;
            z.sName    = q.sName.GetData();
            z.sDesc    = q.sDesc.GetData();
            z.sEdition = q.sEdition.GetData();
            z.sVersion = q.sVersion.GetData();
            z.sArch    = q.sArch.GetData();
            z.sProduct = q.sProduct.GetData();
            z.sCreated = q.sCreated.GetData();
            z.sSize    = q.sSize.GetData();
            vOut.push_back(std::move(z));
        }
    }
    if (psErr) *psErr = sErr.GetData();
    return b;
}
bool CFrameWnd::HostRunBat(const std::wstring& sBatName, const std::wstring& sBody,
                           HANDLE* phProcess)
{
    return HostRunBat(sBatName.c_str(), CDuiString(sBody.c_str()), phProcess);
}
void CFrameWnd::HostLog(const std::wstring& sLine, int nLevel)
{
    HostLog(CDuiString(sLine.c_str()), nLevel);
}
void CFrameWnd::HostAlert(const std::wstring& sTitle, const std::wstring& sText,
                          UINT nIcon)
{
    HostAlert(sTitle.c_str(), sText.c_str(), nIcon);
}
bool CFrameWnd::HostConfirm(const std::wstring& sTitle, const std::wstring& sText)
{
    return HostConfirm(sTitle.c_str(), sText.c_str());
}


// 创建并模态运行工具箱弹窗
// P6a：以下 14 个工具箱函数已迁入 Page_Tools.cpp（CPageTools），本壳仅薄转发。


// ===== P5：驱动管理薄转发（UI/状态实现迁 CPageDriver；pnputil 迁 DriverService/S7）=====


// ============================================================
// R37: 更新管理搜索过滤 + 状态计数
//      遍历 upd_pending_list / upd_list 两个 Tab 容器，
//      按 sTitle 关键字（不区分大小写）过滤 updrow_%d 行；
//      行内子 Text 从 m_vUpdList 反查 title/date 匹配。
// ============================================================
// ===== P4：更新过滤/计数薄转发（实现迁 CPageUpdate）=====


// ===== P5：驱动枚举/详情/删除薄转发（解析 helper 与 pnputil 调用迁 DriverService）=====


// ============================================================
// program page (real Uninstall registry entries)
// ============================================================
// P3a：程序枚举 helper（RegGetDword2/FormatAppDate/FormatKbSize）与
// 三源 Uninstall 枚举已迁 ProgramService（S4，ProgramService.h/.cpp）。
// 5 个程序页函数已迁 CPageProgram（Page_Program.h/.cpp）；核心壳暂留同名
// 薄转发（Notify 路由零改动），收尾两级路由落地后删除。


// ============================================================
// [B1] 通用列表右键菜单
// 覆盖 CLEAN / UPDATE / APPX / ASSOC_IMG / ASSOC_LIVE / PROGRAM 六种列表容器
// 每项行为都是"调用现有函数或复制文本到剪贴板"，不新造业务逻辑
// ============================================================


// ============================================================
// [B2] F5 全局刷新
// 按当前 content_area 的 Tab 索引调用对应 Build*Page 函数
// 编辑框聚焦时不拦截（避免用户正在输入时误触发）
// ============================================================
void CFrameWnd::RefreshCurrentPage()
{
    CTabLayoutUI* pTab = static_cast<CTabLayoutUI*>(m_pm.FindControl(L"content_area"));
    int nPage = pTab ? pTab->GetCurSel() : -1;
    if (nPage < 0) return;
    // 记录刷新动作
    CDuiString sLog;
    sLog.Format(StringMgr.Get("misc.h04f7"), nPage);
    AppendLog(sLog.GetData());
    // [R3] 原 6 个刷新特例（Startup 重建/Appx 枚举+日志/Tools 仅日志/
    // Capability BuildList/Update 仅日志/Tasks 无动作）已全部收口为
    // CPageBase::OnRefreshPage 虚函数（默认等价 Build()）；新增或修改
    // 页面刷新语义只需在页内 override，核心壳不再出现按页 switch。
    if (CPageBase* pPage = PageById(nPage)) pPage->OnRefreshPage();
}


// ============================================================
// [B5] 语言选择弹窗
// 使用系统消息框循环选择，避免引入新 UI 面板；
// 写入 config.ini [Language] Current=<code>，提示重启生效。
// ============================================================
void CFrameWnd::ShowLanguagePicker()
{
    std::vector<CDuiString> vLangs = StringMgr.ListAvailableLangs();
    if (vLangs.empty())
    {
        ::MessageBoxW(m_hWnd,
            StringMgr.Get("misc.hfc57"),
            StringMgr.Get("misc.h5860"), MB_OK | MB_ICONINFORMATION);
        return;
    }


    int nSel = -1;
    // [C++17 R-P10] std::optional 替代 nullptr 判定语义
    static const std::wstring s_szCurFallback = L"zh-CN";
    std::optional<std::wstring> optCur = i18n::CConfigReader::GetLanguageCurrentOpt();
    const wchar_t* pszCur = optCur ? optCur->c_str() : s_szCurFallback.c_str();
    if (!pszCur) pszCur = L"zh-CN";


    for (size_t i = 0; i < vLangs.size() && i < 8; ++i)
    {
        CDuiString sQ;
        sQ.Format(StringMgr.Get("misc.h0ff4"),
                  vLangs[i].GetData(), pszCur);
        int r = ::MessageBoxW(m_hWnd, sQ.GetData(), StringMgr.Get("misc.hff71"),
                              MB_YESNO | MB_ICONQUESTION);
        if (r == IDYES)
        {
            nSel = static_cast<int>(i);
            break;
        }
    }


    if (nSel < 0) return;


    const wchar_t* pszNew = vLangs[nSel].GetData();
    i18n::CConfigReader::SetLanguageCurrent(pszNew);


    CDuiString sLog;
    sLog.Format(StringMgr.Get("misc.h305d"),
                pszCur, pszNew);
    AppendLog(sLog.GetData());
}


// ============================================================
// [B4] 列出 skins/ 目录下的主题名（保守版：仅枚举，不做热切换）
// 遍历 appDir/skin/ 下的子目录，输出到 vOut
// ============================================================
void CFrameWnd::ListAvailableThemes(std::vector<CDuiString>& vOut)
{
    vOut.clear();
    wchar_t szExe[MAX_PATH] = { 0 };
    ::GetModuleFileNameW(nullptr, szExe, MAX_PATH);
    wchar_t* p = ::wcsrchr(szExe, L'\\');
    if (p) *(p + 1) = L'\0';


    CDuiString sRoot(szExe);
    sRoot += L"skin\\";


    CDuiString sPattern;
    sPattern = sRoot;
    sPattern += L"*";


    WIN32_FIND_DATAW fd;
    FindGuard hFind(::FindFirstFileW(sPattern.GetData(), &fd));
    if (hFind == INVALID_HANDLE_VALUE)
    {
        AppendLog(StringMgr.Get("misc.h6033"));
        return;
    }
    do
    {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            && lstrcmpiW(fd.cFileName, L".") != 0
            && lstrcmpiW(fd.cFileName, L"..") != 0)
        {
            vOut.push_back(CDuiString(fd.cFileName));
        }
    } while (::FindNextFileW(hFind, &fd));



    CDuiString sLog;
    sLog.Format(StringMgr.Get("misc.h48af"), static_cast<int>(vOut.size()));
    AppendLog(sLog.GetData());
}


// [C2] 程序行右键菜单：仅提供卸载入口（与底部“卸载”按钮同一函数，自带确认）


// R34: 修改/修复程序（MSI /fa 修复模式）






// sum file sizes under a folder; reparse points and errors skipped.


// ============================================================
// P4 大区间替换点：Appx/Capability/Update 原 18 函数已迁三页+S6。
// [R5] ApplyProgFilter 已下沉 CPageProgram::OnEvent；本壳仅保留
// MakeLoadingHint（共享占位控件）这一纯 UI 工厂。
// ============================================================
CControlUI* CFrameWnd::MakeLoadingHint(const wchar_t* pszText, const wchar_t* pszRingName)
{
    auto pRow = std::make_unique<CHorizontalLayoutUI>();
    pRow->SetFixedHeight(170);
    pRow->SetChildPadding(10);
    pRow->SetAttribute(L"childvalign", L"vcenter");


    auto pSp1 = std::make_unique<CControlUI>(); pSp1->SetFixedWidth(0);
    auto pRing = std::make_unique<CProgressRingUI>();
    if (pszRingName) pRing->SetName(pszRingName);
    auto pTxt = std::make_unique<CTextUI>();
    pTxt->SetText(pszText);
    pTxt->SetTextColor(tokens::Token(tokens::EToken::InkTertiary));
    pTxt->SetFont(1);
    pTxt->SetAttribute(L"autocalcwidth", L"true");
    pTxt->SetMouseEnabled(false);
    auto pSp2 = std::make_unique<CControlUI>(); pSp2->SetFixedWidth(0);


    pRow->Add(pSp1.release());   // [C++17 R3] DuiLib container takes ownership
    pRow->Add(pRing.release());
    pRow->Add(pTxt.release());
    pRow->Add(pSp2.release());
    return pRow.release();   // [C++17 R3] ownership transferred to caller (added into a DuiLib container)
}


// ===== P4：Appx/可选功能/更新 薄转发（实现已迁 Page_Appx/Capability/Update + S6）=====
// ===== P4：Appx/可选功能/更新 薄转发（实现已迁 Page_Appx/Capability/Update + S6）=====
// P4 共享字符串辅助（TrimStr / AppxFamily / PrettyCapName / WuaLocalizeErrorCode /
// UpdateTypeOf）实现已迁 FrameWnd_Util.cpp，此处仅保留声明（来自 FrameWnd_Util.h）。
// P4 共享字符串辅助实现已迁 FrameWnd_Util.cpp。


// ============================================================
// unattend page (page 10) —— P2 已迁入 Page_Unattend.cpp / CPageUnattend
// 下列为核心壳薄转发（Notify 两级路由落地后随收尾阶段删除）
// ============================================================


// ============================================================
// popup menus
// ============================================================
void CFrameWnd::ShowPopupMenu(int iMenu)
{
    HMENU hMenu = ::CreatePopupMenu();
    // helper flags for the current "after action" radio item
    UINT uChk0 = (m_nAfterAction == 0) ? MF_CHECKED : MF_UNCHECKED;
    UINT uChk1 = (m_nAfterAction == 1) ? MF_CHECKED : MF_UNCHECKED;
    UINT uChk2 = (m_nAfterAction == 2) ? MF_CHECKED : MF_UNCHECKED;
    if (iMenu == 1)
    {
        BuildMenuFile(hMenu);
    }
    else if (iMenu == 2)
    {
        BuildMenuRecover(hMenu);
    }
    else if (iMenu == 3)
    {
        BuildMenuOption(hMenu, uChk0, uChk1, uChk2);
    }
    else if (iMenu == 4)
    {
        BuildMenuHelp(hMenu);
    }
    TrackMenuAndDispatch(hMenu, iMenu);
}


void CFrameWnd::SwitchToPage(int idx)
{
    // highlight the matching nav option (fires selectchanged → page switch)
    CDuiString sNav;
    sNav.Format(L"nav_%d", idx);
    COptionUI* pOpt = static_cast<COptionUI*>(m_pm.FindControl(sNav));
    if (pOpt)
    {
        if (!pOpt->IsSelected())
            pOpt->Selected(true);
    }


    CTabLayoutUI* pTab = static_cast<CTabLayoutUI*>(
        m_pm.FindControl(L"content_area"));
    if (pTab && pTab->GetCurSel() != idx)
        pTab->SelectItem(idx);
}


void CFrameWnd::ShowAboutBox()
{
    // ======================================================
    // 关于框：项目简介、开源仓库地址、许可协议 + 运行环境
    // ======================================================
    CDuiString strMsg;
    strMsg += L"Win8Metro  (Dism++ x64)  v2026.11.1\r\n";
    strMsg += L"基于 DuiLib_Ultimate 构建的 Windows 系统维护工具箱\r\n";
    strMsg += L"\r\n";
    strMsg += L"项目简介\r\n";
    strMsg += L"──────────────────────────\r\n";
    strMsg += L"面向 Windows 10/11 x64 的全能型系统部署与维护工具，\r\n";
    strMsg += L"覆盖系统清理、驱动管理、功能启用/关闭、Windows 更新、\r\n";
    strMsg += L"Appx 管理、注册表清理、无人值守安装等 17 个功能场景。\r\n";
    strMsg += L"\r\n";
    strMsg += L"开源仓库  (GitHub)\r\n";
    strMsg += L"──────────────────────────\r\n";
    strMsg += L"  https://github.com/q1130396190-droid/dism-c17-2026\r\n";
    strMsg += L"\r\n";
    strMsg += L"许可证  (MIT License)\r\n";
    strMsg += L"──────────────────────────\r\n";
    strMsg += L"Copyright (c) 2014 Troy\r\n";
    strMsg += L"Permission is hereby granted, free of charge, to any person\r\n";
    strMsg += L"obtaining a copy of this software and associated documentation\r\n";
    strMsg += L"files (the \"Software\"), to deal in the Software without\r\n";
    strMsg += L"restriction, including without limitation the rights to use,\r\n";
    strMsg += L"copy, modify, merge, publish, distribute, sublicense, and/or\r\n";
    strMsg += L"sell copies of the Software.\r\n";
    strMsg += L"\r\n";
    strMsg += L"──────────────────────────\r\n";
    strMsg += L"运行环境\r\n";
    strMsg += L"──────────────────────────\r\n";
    strMsg += StringMgr.Get("system.s58c18dd4");


    RegKeyGuard hKey;   // [C++17 R-P8] 句柄 RAII：原为裸 HKEY + hKey.out() 导致类型不匹配，改为守卫声明
    if (::RegOpenKeyEx(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
            0, KEY_READ | KEY_WOW64_64KEY, hKey.out()) == ERROR_SUCCESS)
    {
        constexpr int kWinClassNameWchars = 256;   // 256 缓冲尺寸（语义命名，数值不变）
        wchar_t szBuf[kWinClassNameWchars] = { 0 };
        DWORD dwSize = sizeof(szBuf);
        CDuiString strProduct;
        if (::RegQueryValueEx(hKey, L"ProductName", nullptr, nullptr,
                              reinterpret_cast<BYTE*>(szBuf), &dwSize) == ERROR_SUCCESS)
            strProduct = szBuf;


        dwSize = sizeof(szBuf);
        CDuiString strBuild;
        if (::RegQueryValueEx(hKey, L"CurrentBuildNumber", nullptr, nullptr,
                              reinterpret_cast<BYTE*>(szBuf), &dwSize) == ERROR_SUCCESS)
            strBuild = szBuf;




        strMsg += strProduct;
        strMsg += L"  build ";
        strMsg += strBuild;
        strMsg += L"\r\n";
    }


    SYSTEM_INFO si;
    ::GetNativeSystemInfo(&si);
    const wchar_t* arch = L"x86";
    if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64) arch = L"x64";
    else if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64) arch = L"ARM64";
    strMsg += L"  architecture: ";
    strMsg += arch;
    strMsg += L"\r\n";


    wchar_t szSysDir[MAX_PATH] = { 0 };
    ::GetSystemDirectory(szSysDir, MAX_PATH);
    strMsg += L"  ";
    strMsg += szSysDir;
    strMsg += L"\\dismapi.dll";
    strMsg += L"\r\n";
    strMsg += L"\r\n";
    strMsg += L"点击菜单 帮助 → 官方网站 / 查看日志 可直接跳转浏览器。\r\n";

    ::MessageBoxW(m_hWnd, strMsg.GetData(), L"关于 Win8Metro", MB_OK | MB_ICONINFORMATION);
}


// ============================================================
// P8：当前内容页访问器（content_area TabLayout 顺序 == EPageId）
// ============================================================
// [GC-03] 页描述符表：页序（EPageId）→ 壳值成员的唯一注册点。
// 新增/删除页面只需在此改一行；CurrentPage/EnsurePageBuilt/
// RefreshCurrentPage 不再各自维护一份 16-case 映射。
// 说明：C++ 成员指针只允许"基类成员指针→派生类成员指针"单向标准转换，
// 故不能把 派生页 CFrameWnd::* 收进 CPageBase CFrameWnd::* 表；改用
// 基类普通指针数组——派生页* → 基类* 是合法标准转换。页成员为 private，
// 取址必须位于成员函数上下文。该函数仅 UI 导航时调用，构造 16 项数组
// （16 条取址指令）开销可忽略。
CPageBase* CFrameWnd::PageById(int idx)
{
    // [R4] 16 页序映射表与页值持有一并下沉 CPageHost，核心壳仅薄转；
    // 本函数保留为壳内调用点与 IPageHost 之间的稳定入口。
    return m_pages.PageById(idx);
}
CPageBase* CFrameWnd::CurrentPage()
{
    CTabLayoutUI* pTab = static_cast<CTabLayoutUI*>(m_pm.FindControl(L"content_area"));
    int idx = pTab ? pTab->GetCurSel() : -1;
    // [GC-03] 16-case 映射收敛为页表单行查询
    return PageById(idx);
}
// ============================================================
// Notify —— P8 两级路由第一级（核心壳）
//
// 保留：SETFOCUS/KILLFOCUS 焦点高亮、标题栏按钮、弹出菜单、
//       全局按钮（导入配置/刷新）、导航页切换，以及 btn_upd_find 的
//       注册表待重启检测（系统调用纪律）。
// [R5] prog_pages 共享容器事件已由 CPageProgram::OnEvent 承接；寄宿于
//       startup_pages Tab2 的计划任务子页事件由 CPageStartup::OnEvent
//       转发 CPageTasks——均不再属于壳级例外。
// 其余 click/dbclick/textchanged/itemselect/selectchanged 一律
//       按当前 content_area 页交 CPageBase::OnEvent 第二级路由。
// ============================================================
void CFrameWnd::Notify(TNotifyUI& msg)
{
    // ----------------------------------------------------------------
    // Notify：DuiLib 全局控件消息入口（第一级路由）
    // ----------------------------------------------------------------
    // 本函数是壳层消息的两级路由的第一级，只负责「壳自己关心的」消息：
    //   1) 焦点变化（Tab 导航可见高亮）
    //   2) 标题栏按钮（关闭/最小化/最大化/还原）
    //   3) 顶部菜单（文件/恢复/选项/帮助）
    //   4) 全局按钮（导入配置/刷新/更新扫描）
    //   5) 顶部导航 nav_<index>（切页，见 OnNavClicked）
    // 其余消息一律下沉给当前内容页（CurrentPage()->OnEvent）。
    //
    // 关键设计：壳只保留「跨页或作用于整个窗口」的动作。
    // 凡属于某一页的控件事件，都在该页自己的 OnEvent 里处理，
    // 避免壳继续膨胀成上帝对象（历史包袱已大部分下沉完毕）。
    //
    // 焦点处理说明：
    //   SETFOCUS  → OnFocusChanged(pSender)，给控件加可见高亮边框；
    //   KILLFOCUS → 仅当失焦控件正是当前记录的高亮控件时才清除，
    //               否则会把刚获得焦点的另一个控件的高亮误清掉。
    //
    // 标题栏按钮说明：
    //   closebtn   → Close(0)，走正常关闭流程（含配置落盘与托盘清理）；
    //   minbtn/maxbtn/restorebtn → 转发 WM_SYSCOMMAND，交由系统处理，
    //   不自实现窗口状态机，避免与 DPI 变化、多显示器时的
    //   系统行为不一致。
    //
    // 顶部菜单说明：
    //   menu_file/recover/option/help 分别映射 ShowPopupMenu(1..4)，
    //   菜单项内容由该函数按参数构建，本函数不关心具体项。
    //
    // 全局按钮说明：
    //   btn_import_cfg → ImportConfig()，导入外部配置文件；
    //   btn_refresh    → RefreshAll()，刷新全部页面；
    //   btn_upd_find   → 注册表待重启检测 + 触发 usoclient 扫描，
    //                    因「检测」属系统调用纪律，保留在壳内，
    //                    而扫描起始时间戳/日志已下沉到 CPageUpdate。
    //
    // 两级路由的第二级在函数末尾：
    //   click/dbclick/textchanged/itemselect/selectchanged 五类消息
    //   统一交给 CurrentPage()->OnEvent(msg)，由页面自己分派。
    //   壳不解析控件名，只做「是不是我的」判断，判断不了就下沉。
    //
    // DBGFEAT 诊断：仅在 DISMTOOL_FEAT_TRACE 环境变量存在时输出，
    // 且只对 feat 相关控件名生效。历史上这里曾无条件写日志，
    // 鼠标滑过列表就刷屏，已改为开关式。
    //
    // 末尾 __super::Notify(msg) 不可省略：DuiLib 基类还需处理
    // 窗口拖动、缩放、双击标题栏最大化等默认行为，漏掉会导致窗口无法拖动。
    // 注意它是无条件调用——本函数所有分支都只做 return（提前返回），
    // 不会跳过这一句之外的基类转发。
    // ----------------------------------------------------------------


    CDuiString sName = msg.pSender ? msg.pSender->GetName() : CDuiString(L"");
    const wchar_t* pszName = sName.GetData();


    // [R19] 键盘焦点可见高亮（Tab 导航）
    if (msg.sType == DUI_MSGTYPE_SETFOCUS)
    {
        OnFocusChanged(msg.pSender);
    }
    else if (msg.sType == DUI_MSGTYPE_KILLFOCUS)
    {
        if (m_pFocusCtrl == msg.pSender) OnFocusChanged(nullptr);
    }


    if (msg.sType == L"click")
    {
        // ---- 标题栏 ----
        if      (sName == L"closebtn")   { Close(0); return; }
        else if (sName == L"minbtn")     { SendMessage(WM_SYSCOMMAND, SC_MINIMIZE, 0); return; }
        else if (sName == L"maxbtn")     { SendMessage(WM_SYSCOMMAND, SC_MAXIMIZE, 0); return; }
        else if (sName == L"restorebtn") { SendMessage(WM_SYSCOMMAND, SC_RESTORE, 0); return; }
        // [R236] 主题入口：下拉由自绘浮层实现（需要画配色预览，原生菜单做不到）
        else if (sName == L"btn_theme")  { ShowThemePicker(); return; }
        // ---- 顶部菜单 ----
        else if (sName == L"menu_file")    { ShowPopupMenu(1); return; }
        else if (sName == L"menu_recover") { ShowPopupMenu(2); return; }
        else if (sName == L"menu_option")  { ShowPopupMenu(3); return; }
        else if (sName == L"menu_help")    { ShowPopupMenu(4); return; }
        // ---- 全局按钮（优化工具栏但作用于全应用）----
        else if (sName == L"btn_import_cfg")  { ImportConfig(); return; }
        else if (sName == L"btn_refresh")     { RefreshAll(); return; }




        // btn_upd_find：注册表待重启检测属系统调用纪律，保留壳内；
        // 扫描起始时间戳/日志在 CPageUpdate。
        else if (sName == L"btn_upd_find")
        {
            // [R43] 重启检测：WindowsUpdate 挂起键 + 待重启注册表项
            //          HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\WindowsUpdate\Auto Update\RebootRequired
            //          HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\PendingFileRenameOperations
            {
                DWORD dwReboot = 0;
                RegKeyGuard hRU;
                if (::RegOpenKeyEx(HKEY_LOCAL_MACHINE,
                        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\WindowsUpdate\\Auto Update",
                        0, KEY_READ, hRU.out()) == ERROR_SUCCESS)
                {
                    DWORD nSize = sizeof(dwReboot);
                    // [B6] 原为空受控语句 `if(...) ;` 触发 C4390；改为直接调用，失败时 dwReboot 保持 0，语义等价（dwReboot 在下方 4756 被使用）。
                    (void)::RegQueryValueEx(hRU, L"RebootRequired", nullptr, nullptr,
                            (BYTE*)&dwReboot, &nSize);
                }
                DWORD dwPFR = 0;
                RegKeyGuard hPFR;
                if (::RegOpenKeyEx(HKEY_LOCAL_MACHINE,
                        L"SYSTEM\\CurrentControlSet\\Control\\Session Manager",
                        0, KEY_READ, hPFR.out()) == ERROR_SUCCESS)
                {
                    DWORD nSize = sizeof(dwPFR);
                    // [B6] 同上一处：改直接调用，消除 C4390 空受控语句。
                    (void)::RegQueryValueEx(hPFR, L"PendingFileRenameOperations", nullptr,
                            nullptr, (BYTE*)&dwPFR, &nSize);
                }
                if (dwReboot != 0 || dwPFR != 0)
                {
                    CDuiString sWarn;
                    sWarn.Format(StringMgr.Get("misc.h7dc1"),
                                 dwReboot, dwPFR);
                    AppendLogWarn(sWarn.GetData());
                }
            }


            // [R5] 扫描起始时间戳/日志已迁 CPageUpdate，经命令虚入口
            if (CPageBase* pPage = PageById(EPageId::PAGE_UPDATE)) pPage->OnHostCommand(static_cast<int>(EHostCmd::HCMD_UPD_SCAN_STARTED));


            StartTool(L"usoclient.exe", L"StartScan");
            return;
        }
    }


    if (msg.sType == L"selectchanged")
    {
        // top nav: nav_<index>
        if (wcsncmp(pszName, L"nav_", 4) == 0)
        {
            OnNavClicked(pszName);
            return;
        }


    }




    // ============================================================
    // 第二级路由：其余页面事件交当前内容页
    // ============================================================
    if (msg.sType == L"click"
        || msg.sType == L"dbclick"
        || msg.sType == L"textchanged"
        || msg.sType == DUI_MSGTYPE_ITEMSELECT
        || msg.sType == L"selectchanged")
    {
        // [DBGFEAT] 诊断开关：仅 DISMTOOL_FEAT_TRACE 存在时输出；曾无条件写日志污染日志区
        if ((msg.sType == L"click" || msg.sType == L"dbclick")
            && sName.Find(L"feat") >= 0
            && ::GetEnvironmentVariableW(L"DISMTOOL_FEAT_TRACE", nullptr, 0) > 0)
        {
            int nDbgCur = -1;
            CTabLayoutUI* pDbgTab = static_cast<CTabLayoutUI*>(
                m_pm.FindControl(L"content_area"));
            if (pDbgTab) nDbgCur = pDbgTab->GetCurSel();
            CDuiString sDbg;
            sDbg.Format(L"[DBGFEAT] shell notify type=%s name=%s curpage=%d",
                        msg.sType.GetData(), sName.GetData(), nDbgCur);
            AppendLog(sDbg.GetData());
        }
        if (CPageBase* pPage = CurrentPage()) pPage->OnEvent(msg);
    }


    __super::Notify(msg);
}


// ============================================================
// custom messages
// ============================================================
// [2026] 外部自动化接口：PostMessage WM_THEME_SWITCH 触发 ApplyTheme
//   wParam: 0=auto  1=light  2=dark  3=classic
//   lParam: 0=不持久化  1=写 config.ini
#define WM_THEME_SWITCH  (WM_USER + 0x100)

LRESULT CFrameWnd::HandleCustomMessage(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled)
{
    // ==== [AUTOMATION] WM_THEME_SWITCH: 外部进程发消息触发主题切换 ====
    if (uMsg == WM_THEME_SWITCH)
    {
        const wchar_t* names[] = { L"auto", L"light", L"dark", L"classic" };
        int idx = static_cast<int>(wParam);
        if (idx >= 0 && idx < 4)
        {
            AppendLog(L"[theme] WM_THEME_SWITCH received from automation");
            ThemeService::ApplyTheme(names[idx], lParam != 0);
        }
        bHandled = TRUE;
        return 0;
    }

    // [R1] 21 条页完成消息统一经 kPageMsgTab 注册表分发（表与 handler 在
    // FrameWnd_PageMsg.cpp；lParam/wParam 堆所有权按 EPageMsgOwn 契约固化，
    // 壳侧释放统一走 ReleaseMsgParam 单出口）。WM_CLEAN_DONE 含
    // RestartExplorer() 两段时序编排，刻意保留为下方独立分支。
    if (DispatchPageMessage(uMsg, wParam, lParam))
    {
        bHandled = TRUE;
        return 0;
    }
    // [R2] 壳系统/输入消息（托盘/拖放/定时器表/尺寸/DPI/退出序列）
    // 外迁 FrameWnd_SysMsg.cpp；返回 false（WM_SIZE/未命中右键/按键/
    // 非本窗口定时器）继续放行 DuiLib 默认处理。
    if (HandleSystemMessage(uMsg, wParam, lParam))
    {
        bHandled = TRUE;
        return 0;
    }
    if (uMsg == WM_CLEAN_DONE)
    {
        // P6b: OnCleanDone runs the file-delete tail and all synchronous custom
        // actions. It returns true when the user agreed to restart explorer:
        // the shell runs RestartExplorer() first (shared with the optimize page),
        // then OnCleanDoneTail() unlocks the UI and shows the completion balloon,
        // preserving the original "restart before unlock" ordering.
        // [R5] CleanDone 处理（含 m 释放）与尾部解锁全部经页虚入口；
        // 壳仅保留 RestartExplorer 时序编排（restart before unlock）。
        CPageBase* pCleanDone = PageById(EPageId::PAGE_CLEANUP);
        bool bRestartExplorer = pCleanDone && pCleanDone->HostOnCleanDone(lParam);
        if (bRestartExplorer) RestartExplorer();
        if (pCleanDone) pCleanDone->HostOnCleanDoneTail();
        bHandled = TRUE;
        return 0;
    }
    bHandled = FALSE;
    return 0;
}


// ============================================================
// process engine (generic console tool capture)
// ============================================================
void CFrameWnd::StartTool(const wchar_t* pszExeName, const CDuiString& strArgs)
{
    // P6a：进程/管道/读取线程下沉 ExternalToolService（S9）。
    // 互斥提示、参数回显与“starting...”状态属 UI 层，保留本壳；
    // 启动失败的具体日志也由本壳按失败原因输出（与原逐字一致）。
    if (ExternalToolService::IsRunning())
    {
        AppendLog(L"tool already running");
        return;
    }


    AppendLog(strArgs.GetData());
    SetCtrlText(m_pm, L"sess_status", L"starting...");


    int nFail = 0;
    DWORD dwErr = 0;
    if (!ExternalToolService::StartToolAsync(m_hWnd, pszExeName, strArgs, &nFail, &dwErr))
    {
        if (nFail == 1)
        {
            AppendLog(L"CreatePipe stdout failed");
        }
        else if (nFail == 2)
        {
            AppendLog(L"CreatePipe stderr failed");
        }
        else
        {
            std::wstring sErr;
            sErr.resize(128);
            wfmt::FormatInto(sErr.data(), sErr.size() + 1, L"CreateProcess failed error=%lu", dwErr);
            sErr.resize(wcslen(sErr.c_str()));
            AppendLog(sErr.c_str());
        }
    }
}


void CFrameWnd::LaunchGUI(const wchar_t* pszExeName, const wchar_t* pszArgs)
{
    // P6a：外部 GUI 进程启动下沉 ExternalToolService（S9）
    ExternalToolService::LaunchGui(pszExeName, pszArgs);
}


void CFrameWnd::AbortDism()
{
    // P6a：会话句柄随进程能力迁 ExternalToolService（S9）
    ExternalToolService::Abort();
}


// P6a：原 CFrameWnd::OnDismDone 已拆分——句柄清理在
// ExternalToolService::OnDone（WM_DISM_DONE 路由先调），
// UI 收尾（退出码日志/警告/就绪状态/进度清零）在 CPageTools::OnDismDone。


// ============================================================
// log routing
// ============================================================


// DetectLogLevel 已迁入 LogService::DetectLevel（LogService.h/.cpp）
// [L-01] AddColoredLogItem 已删除：它唯一的消费者是 FindOutputRecursive 找到的
//   out_* 列表，而全工程皮肤 XML 与动态构建代码中都不存在任何 out_* 控件，
//   整条"页面内输出区"链路从查找到着色到滚动跟随全部是死代码。
//   日志的真实出口是 LogService（审计环 + JSONL）与「帮助→查看日志」对话框；
//   保留这些死代码只会让人误判"页面里本来应该有日志区"。


// [FIX-DS-2] m_vPendingLog 跨线程互斥锁。
// 根因：工作线程（扫描/清理/DISM 尾巴/诊断）经 AppendLogEx 并发 push_back，
// UI 线程 120ms 定时器 FlushPendingLogs 同期拷贝+erase+clear —— 双端无锁并发
// 操作同一 std::vector = 堆损坏，与 2026-11-02 06:54 启动连崩簇高度相关。
// Meyers CRITICAL_SECTION（MSVC 魔法静态线程安全初始化），进程生命周期不析构，
// 规避退出序列锁销毁竞态。
namespace {
CRITICAL_SECTION* PendingLogLock()
{
    static CRITICAL_SECTION cs;
    static struct SInit { SInit() { ::InitializeCriticalSection(&cs); } } s_init;
    return &cs;
}
}

void CFrameWnd::AppendLogEx(const wchar_t* pszLine, int nLevel)
{
    CDuiString sOut;
    if (nLevel == static_cast<int>(ELogLevel::LOG_ERROR))      sOut = L"[ERROR] ";
    else if (nLevel == static_cast<int>(ELogLevel::LOG_WARN))  sOut = L"[WARN] ";
    sOut += pszLine ? pszLine : L"";


    // [FIX-P2H] OutputDebugString 仅在调试器附加时输出：后台任务高频日志
    // 在调试场景每行 3 次内核调用显著拖慢 UI；零售运行零成本跳过。
    if (::IsDebuggerPresent())
    {
        ::OutputDebugString(L"[dism++] ");
        ::OutputDebugString(sOut.GetData());
        ::OutputDebugString(L"\n");
    }


    // [LAG-FIX] 后台任务（dism/PowerShell/扫描/清理）会在数百毫秒内产生数百行日志。
    // 旧实现每行都做：递归查找输出列表 + new 列表项 + Add(触发布局) +
    // 滚动 + _wfopen/fclose 落盘，WM_DISM_LOG 把消息队列灌满后界面"未响应"。
    // 现统一入队，由 120ms 定时器批量提交（落盘每批仅一次 fopen）。
    // [FIX-DS-2] 入队段持锁：erase+push_back 必须与 FlushPendingLogs 的拷贝+clear 互斥。
    {
        ::EnterCriticalSection(PendingLogLock());
        if (m_vPendingLog.size() >= 600)
            m_vPendingLog.erase(m_vPendingLog.begin());
        PendingLogEntry e;
        e.sOut = sOut;
        e.sRaw = pszLine ? pszLine : L"";
        e.nLevel = nLevel;
        m_vPendingLog.push_back(e);
        ::LeaveCriticalSection(PendingLogLock());
    }
    ArmLogFlush();
}


void CFrameWnd::ArmLogFlush()
{
    if (m_bLogFlushArmed || !m_hWnd) return;
    m_bLogFlushArmed = true;
    ::SetTimer(m_hWnd, LOG_FLUSH_TIMER_ID, 120, nullptr);
}


void CFrameWnd::FlushPendingLogs()
{
    if (m_bLogFlushArmed)
    {
        ::KillTimer(m_hWnd, LOG_FLUSH_TIMER_ID);
        m_bLogFlushArmed = false;
    }
    if (m_vPendingLog.empty()) return;

    // [L-01] 页面内输出区链路已整体删除（详见上方 AddColoredLogItem 处说明）：
    //   out_* 控件在皮肤与动态构建中均不存在，过去这里的"批量 Add + 500 行裁剪
    //   + 滚动跟随"永远走不到；真正执行的只有"全部塞进离屏缓冲，超过 300 条丢最旧"，
    //   而回放侧同样找不到列表 —— 结果就是日志写进了文件、但页面里既没有实时输出、
    //   缓冲里也只留下最后 300 条且永远不会被读取。
    //
    //   现在这批日志的去向只有两个真实出口：
    //     1) LogService 审计环（≤1000 条，供 UI 查询/诊断页使用）；
    //     2) action_log.jsonl（统一目录 + 显式 UTF-8 + 互斥，单次 fopen 写完整批）。
    //   不再有"假装要显示到某处"的中间态，也就不会再有静默丢弃。

    // 1) 审计环 + JSONL 落盘：整批一次写入（替代逐行开关文件）
    // [FIX-DS-2] 快照段持锁：整段拷贝+clear 与 AppendLogEx 入队互斥；
    // 落盘/Toast 在锁外执行（AppendToFileBatch 自带互斥，ShowToast 只能 UI 调）。
    std::vector<std::pair<CDuiString,int> > vFiles;
    {
        ::EnterCriticalSection(PendingLogLock());
        vFiles.reserve(m_vPendingLog.size());
        for (size_t k = 0; k < m_vPendingLog.size(); ++k)
            vFiles.push_back(std::make_pair(m_vPendingLog[k].sRaw, m_vPendingLog[k].nLevel));
        m_vPendingLog.clear();
        ::LeaveCriticalSection(PendingLogLock());
    }
    m_logSvc.AppendToFileBatch(vFiles);

    // [L-15] 日志写失败此前只有 OutputDebugString，普通用户完全无感知
    //   （装到 Program Files 或磁盘满时就是这样静默丢日志的）。
    //   这里由宿主（唯一有权弹窗的层）取走一次性告警并提示，只弹一次。
    std::wstring sAlert;
    if (LogService::TakePendingAlert(sAlert) && !sAlert.empty())
        ShowToast(StringMgr.Get("misc.logview_title"), sAlert.c_str(), 2);

    m_vPendingLog.clear();
}

void CFrameWnd::RequestDeferredBuild(int idx)
{
    if (idx < 0 || idx >= 16) return;
    // 同一个一次性定时器：快速连续切页时只保留最后一个目标，构建期间用户
    // 已离开则跳过（回到该页时导航点击会再次排队），避免后台页抢 UI 线程。
    m_nPendingBuildPage = idx;
    ::SetTimer(m_hWnd, PAGE_BUILD_TIMER_ID, 1, nullptr);
}


// AppendLogToFile 已迁入 LogService::AppendToFile（LogService.h/.cpp）


void CFrameWnd::AppendLogWarn(const wchar_t* pszLine)  { AppendLogEx(pszLine, static_cast<int>(ELogLevel::LOG_WARN)); }
void CFrameWnd::AppendLogError(const wchar_t* pszLine) { AppendLogEx(pszLine, static_cast<int>(ELogLevel::LOG_ERROR)); }


void CFrameWnd::AppendLog(const wchar_t* pszLine)
{
    AppendLogEx(pszLine, LogService::DetectLevel(pszLine));
}


// [L-01] FindOutputRecursive / FindVisibleOutput / FlushBufferToOutput 三个函数
//   随 out_* 死链路一并删除。它们的存在使 FlushPendingLogs 看起来"有地方可显示"，
//   实际永远返回 nullptr，进而把每一批日志都推进一个无人读取的离屏缓冲。
//   日志的真实出口见 FlushPendingLogs 注释（审计环 + JSONL）。

// ============================================================
// [R18/R19] 会话条动态进度条（不改 Main.xml，挂载到 sess_status 同容器）
// ============================================================
void CFrameWnd::InitSessionProgress()
{
    // [R2 P1] 优先使用 Main.xml 内置的 optim_progress；无则回退动态挂载
    // [R5] optim_progress 抓取已迁 CPageOptim，经命令虚入口
    if (CPageBase* pPage = PageById(EPageId::PAGE_OPTIM)) pPage->OnHostCommand(static_cast<int>(EHostCmd::HCMD_OPTIM_INIT_PROGRESS));


    CControlUI* pStatus = m_pm.FindControl(L"sess_status");
    if (!pStatus) return;
    CVerticalLayoutUI* pBox = static_cast<CVerticalLayoutUI*>(pStatus->GetParent());
    if (!pBox) return;


    auto p = std::make_unique<CProgressUI>();
    p->SetName(L"sess_progress");
    p->SetFloat(true);                        // 悬浮叠加，不挤压原四行文本
    p->SetAttribute(L"pos", L"10,119,315,123");
    p->SetHorizontal(true);
    p->SetShowText(false);
    p->SetMinValue(0);
    p->SetMaxValue(100);
    p->SetValue(0);
    // [R235] 半透明深蓝底：alpha 0x66 是设计意图（要透出底下四行文本），
    //   RGB 跟随 Accent，粉/紫/绿等系统强调色下都不会再是"深蓝"。
    p->SetAttribute(L"bkcolor", tokens::TokenHexAlpha(tokens::EToken::Accent, 0x66));
    p->SetAttribute(L"forecolor", tokens::TokenHex(tokens::EToken::OnAccent));  // [R235] 随底色反选
    p->SetVisible(false);
    CProgressUI* pRaw = p.get();
    pBox->Add(p.release());   // [C++17 R3] DuiLib container takes ownership
    m_pSessProgress = pRaw;
}


void CFrameWnd::SetSessionProgress(int nVal, int nMax)
{
    if (m_pSessProgress)
    {
        if (nMax <= 0)
        {
            m_pSessProgress->SetVisible(false);
        }
        else
        {
            if (nVal < 0) nVal = 0;
            if (nVal > nMax) nVal = nMax;
            m_pSessProgress->SetMaxValue(nMax);
            m_pSessProgress->SetValue(nVal);
            m_pSessProgress->SetVisible(true);
        }
    }


    // 任务栏按钮进度（R19）：COM 对象只创建一次，失败静默
    static ITaskbarList3* s_pTbl = nullptr;
    static bool s_bTried = false;
    if (!s_bTried)
    {
        s_bTried = true;
        ::CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
                           IID_ITaskbarList3, (void**)&s_pTbl);
        if (s_pTbl) s_pTbl->HrInit();
    }
    if (s_pTbl)
    {
        if (nMax <= 0)
            s_pTbl->SetProgressState(m_hWnd, TBPF_NOPROGRESS);
        else
        {
            s_pTbl->SetProgressState(m_hWnd, TBPF_NORMAL);
            s_pTbl->SetProgressValue(m_hWnd,
                static_cast<ULONGLONG>(nVal < 0 ? 0 : nVal), static_cast<ULONGLONG>(nMax));
        }
    }
}


void CFrameWnd::HostProgress(int nVal, int nMax)
{
    SetSessionProgress(nVal, nMax);
}


// [R2 P1] 优化规则执行进度。XML 有 optim_progress 就用它（页面原位进度），
// 否则回退到会话条动态进度条 + 任务栏进度。


// 100ms 节流：批量执行规则时避免每条规则都触发 UI 重绘；
// nMax<=0（隐藏）与终态（nVal==nMax）立即刷新不等待。


// ============================================================
// [R18/R19] 系统托盘图标 + 气泡通知
// ============================================================
// ============================================================
// [R-B12] 品牌图标加载：skin/<当前皮肤>/app.ico（随包位图资产）
//   CPaintManagerUI::GetResourcePath() 已在 Win8Metro.cpp 设为当前皮肤目录
//   （不再硬编码 skin\Win8Metro），因此这里直接拼接路径即可。
//   失败时回退到系统 IDI_SHIELD（保持原行为）。
// ============================================================
static HICON LoadAppBrandIcon()
{
    // [CLASSIC-SKIN] 使用当前皮肤的资源路径，不再硬编码 Win8Metro
    CDuiString sRes = CPaintManagerUI::GetResourcePath();
    if (sRes.IsEmpty()) return nullptr;

    std::wstring sPath;
    sPath.resize(MAX_PATH);

    // 1) 首选 app.ico
    wfmt::FormatInto(sPath.data(), sPath.size() + 1,
                 L"%s\\app.ico", sRes.GetData());
    sPath.resize(wcslen(sPath.c_str()));
    HICON hIcon = (HICON)::LoadImageW(nullptr, sPath.c_str(), IMAGE_ICON, 0, 0,
                               LR_LOADFROMFILE | LR_DEFAULTSIZE | LR_SHARED);
    if (hIcon) return hIcon;

    // 2) 次选 app_256.png
    wfmt::FormatInto(sPath.data(), sPath.size() + 1,
                 L"%s\\app_256.png", sRes.GetData());
    sPath.resize(wcslen(sPath.c_str()));
    hIcon = (HICON)::LoadImageW(nullptr, sPath.c_str(), IMAGE_ICON, 256, 256,
                         LR_LOADFROMFILE | LR_SHARED);
    if (hIcon) return hIcon;

    // 3) 末选 app_32.png 位图
    wfmt::FormatInto(sPath.data(), sPath.size() + 1,
                 L"%s\\app_32.png", sRes.GetData());
    sPath.resize(wcslen(sPath.c_str()));
    hIcon = (HICON)::LoadImageW(nullptr, sPath.c_str(), IMAGE_BITMAP, 32, 32,
                         LR_LOADFROMFILE | LR_SHARED);
    return hIcon;
}
void CFrameWnd::InitTrayIcon()
{
    // [FIX-TRAY] 防重复注册：同 (hWnd,uID) 重复 NIM_ADD 在部分 shell 版本
    //   返回 FALSE 或形成双图标。函数内部先清旧注册，使所有调用点
    //   （启动、RestartExplorer 兜底、TaskbarCreated）可以直接重入。
    if (m_bTrayAdded) RemoveTrayIcon();
    m_sNid.cbSize = sizeof(m_sNid);
    m_sNid.hWnd = m_hWnd;
    m_sNid.uID = 1;
    m_sNid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    m_sNid.uCallbackMessage = WM_TRAYNOTIFY;
    // [R-B12] 使用随包品牌图标（app.ico）；缺失时回退系统图标
    m_sNid.hIcon = LoadAppBrandIcon();
    if (!m_sNid.hIcon) m_sNid.hIcon = ::LoadIcon(nullptr, IDI_SHIELD);
    if (!m_sNid.hIcon) m_sNid.hIcon = ::LoadIcon(nullptr, IDI_APPLICATION);
    wcsncpy_s(m_sNid.szTip, _countof(m_sNid.szTip), StringMgr.Get("misc.hf017"), _TRUNCATE);


    if (::Shell_NotifyIconW(NIM_ADD, &m_sNid))
    {
        m_bTrayAdded = true;
        m_sNid.uVersion = NOTIFYICON_VERSION_4;
        ::Shell_NotifyIconW(NIM_SETVERSION, &m_sNid);
    }
}


void CFrameWnd::RemoveTrayIcon()
{
    if (!m_bTrayAdded) return;
    ::Shell_NotifyIconW(NIM_DELETE, &m_sNid);
    m_bTrayAdded = false;
}


void CFrameWnd::SendBalloonTip(const wchar_t* pszTitle, const wchar_t* pszText, DWORD dwFlags)
{
    if (!m_bTrayAdded || ::IsWindow(m_hWnd) == FALSE) return;


    NOTIFYICONDATAW nid;
    nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = m_hWnd;
    nid.uID = 1;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = dwFlags;
    nid.uTimeout = 3000;
    wcsncpy_s(nid.szInfoTitle, _countof(nid.szInfoTitle),
                pszTitle ? pszTitle : L"Win8Metro", 64);
    wcsncpy_s(nid.szInfo, 256, pszText ? pszText : L"", _TRUNCATE);
    ::Shell_NotifyIconW(NIM_MODIFY, &nid);
}


void CFrameWnd::HostNotify(const wchar_t* pszTitle, const wchar_t* pszText, int nIcon)
{
    DWORD dw = NIIF_INFO;
    if (nIcon == 1) dw = NIIF_WARNING;
    else if (nIcon == 2) dw = NIIF_ERROR;
    SendBalloonTip(pszTitle, pszText, dw);
}


// ============================================================
// [U-10] 应用内 Toast 通知（右上角浮动，3 秒自动消失）
// ============================================================
CFrameWnd* CFrameWnd::s_pToastOwner = nullptr;


void CFrameWnd::DestroyToast()
{
    if (m_uToastTimerId)
    {
        ::KillTimer(m_hWnd, m_uToastTimerId);
        m_uToastTimerId = 0;
    }
    if (m_hToastWnd && ::IsWindow(m_hToastWnd))
    {
        ::DestroyWindow(m_hToastWnd);
    }
    m_hToastWnd = nullptr;
}


void CFrameWnd::ShowToast(const wchar_t* pszTitle, const wchar_t* pszText, int nIcon)
{
    if (!::IsWindow(m_hWnd)) return;


    // 关闭已有 Toast
    DestroyToast();


    m_sToastTitle = pszTitle ? pszTitle : StringMgr.Get("misc.h5660");
    m_sToastText = pszText ? pszText : L"";
    m_nToastIcon = nIcon;


    // 创建无边框浮动窗口
    static bool s_bRegistered = false;
    if (!s_bRegistered)
    {
        WNDCLASSEXW wc = { 0 };
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &CFrameWnd::ToastWndProc;
        wc.hInstance = CPaintManagerUI::GetInstance();
        wc.hCursor = ::LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"Win8MetroToastClass";
        ::RegisterClassExW(&wc);
        s_bRegistered = true;
    }


    m_hToastWnd = ::CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"Win8MetroToastClass", L"",
        WS_POPUP,
        0, 0, 340, 0,
        nullptr, nullptr, CPaintManagerUI::GetInstance(), nullptr);
    if (!m_hToastWnd) return;


    // 启动 3 秒自动消失定时器
    m_uToastTimerId = static_cast<UINT>(::SetTimer(m_hWnd, 9001, 3000, nullptr));   // [B6] UINT_PTR->UINT cast，消 C4244


    // 刷新绘制（先隐藏，等 WM_PAINT 后定位再显示）
    ::ShowWindow(m_hToastWnd, SW_SHOW);
}


LRESULT CALLBACK CFrameWnd::ToastWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        CFrameWnd* pOwner = CFrameWnd::s_pToastOwner;
        if (!pOwner) return 0;


        // 创建字体
        if (!pOwner->m_hfToast)
        {
            pOwner->m_hfToast = ::CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        }
        return 0;
    }


    case WM_SIZE:
        ::InvalidateRect(hwnd, nullptr, FALSE);
        return 0;


    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = ::BeginPaint(hwnd, &ps);
        if (!hdc) return 0;


        RECT rc;
        ::GetWindowRect(hwnd, &rc);
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;


        CFrameWnd* pOwner = CFrameWnd::s_pToastOwner;
        if (!pOwner) { ::EndPaint(hwnd, &ps); return 0; }


        // 背景：深灰半透明
        int nIcon = pOwner->m_nToastIcon;
        COLORREF bg = 0xFF2A2A2A;
        COLORREF accent = 0xFF2196F3;  // 蓝色
        COLORREF iconColor = 0xFF4CAF50;
        if (nIcon == 1) { accent = 0xFFFF9800; iconColor = 0xFFFF9800; }   // 警告：橙
        else if (nIcon == 2) { accent = 0xFFF44336; iconColor = 0xFFF44336; } // 错误：红


        // 圆角矩形背景
        RECT rcBg = { 0, 0, w, h };
        // [C++17 R3] was leaked brush in FillRect + redundant create/delete pair; single guarded brush
        GdiGuard hbrBg = ::CreateSolidBrush(bg);
        ::FillRect(hdc, &rcBg, hbrBg);


        // 左侧强调条
        RECT rcAccent = { 0, 0, 4, h };
        GdiGuard hbAccent = ::CreateSolidBrush(accent);   // [C++17 R3] RAII: auto DeleteObject at scope exit
        ::FillRect(hdc, &rcAccent, hbAccent);


        // 绘制图标符号
        HFONT hf = pOwner->m_hfToast;
        HFONT hfOld = (HFONT)::SelectObject(hdc, hf ? hf : ::GetStockObject(DEFAULT_GUI_FONT));


        // 图标区
        RECT rcIcon = { 14, 10, 44, 40 };
        if (nIcon == 0)       ::DrawTextW(hdc, L"i", -1, &rcIcon, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        else if (nIcon == 1)  ::DrawTextW(hdc, L"!", -1, &rcIcon, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        else if (nIcon == 2)  ::DrawTextW(hdc, L"X", -1, &rcIcon, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        ::SetTextColor(hdc, iconColor);


        // 标题
        ::SetTextColor(hdc, 0xFFFFFFFF);
        RECT rcTitle = { 52, 8, w - 12, 28 };
        ::DrawTextW(hdc, pOwner->m_sToastTitle.GetData(), -1, &rcTitle,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);


        // 正文
        ::SetTextColor(hdc, 0xFFCCCCCC);
        RECT rcBody = { 52, 30, w - 12, h - 6 };
        ::DrawTextW(hdc, pOwner->m_sToastText.GetData(), -1, &rcBody,
                    DT_LEFT | DT_WORDBREAK | DT_TOP | DT_NOPREFIX);


        ::SelectObject(hdc, hfOld);


        // 边框
        // [C++17 R3] was redundant create/delete pair; single guarded brush
        GdiGuard hbrBorder = ::CreateSolidBrush(0xFF444444);
        ::FrameRect(hdc, &rcBg, hbrBorder);


        // 定位到主窗口右上角
        if (pOwner->m_hWnd && ::IsWindow(pOwner->m_hWnd))
        {
            RECT rcMain;
            ::GetWindowRect(pOwner->m_hWnd, &rcMain);
            int nX = rcMain.right - w - 20;
            int nY = rcMain.top + 20;
            ::MoveWindow(hwnd, nX, nY, w, h, TRUE);
        }


        ::EndPaint(hwnd, &ps);
        return 0;
    }


    case WM_TIMER:
        if (wParam == 9001)
        {
            // 3 秒到期，销毁 Toast
            CFrameWnd* pOwner = CFrameWnd::s_pToastOwner;
            if (pOwner) pOwner->DestroyToast();
        }
        return 0;


    case WM_DESTROY:
        if (CFrameWnd::s_pToastOwner)
        {
            CFrameWnd::s_pToastOwner->m_hToastWnd = nullptr;
            CFrameWnd::s_pToastOwner->m_uToastTimerId = 0;
        }
        return 0;
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}


bool CFrameWnd::HostConfirm(const wchar_t* pszTitle, const wchar_t* pszText)
{
    return ::MessageBoxW(m_hWnd, pszText, pszTitle,
                         MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2) == IDOK;
}


// ============================================================
// [R19] 轻量 Tooltip（TOOLTIPS_CLASS + TTF_SUBCLASS 命中控件矩形）
// ============================================================
void CFrameWnd::InitTooltips()
{
    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    ::InitCommonControlsEx(&icc);


    m_hwndTip = ::CreateWindowExW(0, TOOLTIPS_CLASSW, nullptr,
        WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        m_hWnd, nullptr, CPaintManagerUI::GetInstance(), nullptr);
    if (!m_hwndTip) return;


    // 预留容量：tooltip 控件保存的是字符串指针，vector 扩容搬移 CDuiString
    // 会导致指针悬空（本页提示总数 ≤ 40）
    m_vTips.reserve(64);


    ::SendMessage(m_hwndTip, TTM_SETMAXTIPWIDTH, 0, 340);
    ::SetWindowPos(m_hwndTip, HWND_TOPMOST, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    ApplyChromeI18n(m_pm);   // 静态可见文本应用当前语言（缺键自动保留 XML 原文）
    RegisterStaticTooltips();
    // [R236] 皮肤刚加载完，btn_theme 此刻才存在：在这里刷一次图标，
    //   否则启动后按钮上一直是 XML 里写的默认字符 ◐，与实际模式不符。
    RefreshThemeButton();
}


void CFrameWnd::AddTooltipForControl(const wchar_t* pszName, const wchar_t* pszText)
{
    if (!m_hwndTip) return;
    CControlUI* pCtrl = m_pm.FindControl(pszName);
    if (!pCtrl) return;


    // 同名控件只注册一次
    for (size_t i = 0; i < m_vTips.size(); ++i)
        if (m_vTips[i].sName == pszName) return;


    STipItem item;
    item.sName = pszName;
    item.sText = pszText;
    item.pCtrl = pCtrl;
    m_vTips.push_back(item);


    TOOLINFOW ti;
    ti = {};
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_SUBCLASS;
    ti.hwnd = m_hWnd;
    ti.uId = static_cast<UINT_PTR>(m_vTips.size());   // 从 1 开始，与下标 +1 对应
    const RECT& rc = pCtrl->GetPos();
    ti.rect = rc;
    ti.lpszText = const_cast<wchar_t*>(m_vTips.back().sText.GetData());
    ::SendMessageW(m_hwndTip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}


void CFrameWnd::RegisterStaticTooltips()
{
    // 左侧主导航
    AddTooltipForControl(L"nav_0",  StringMgr.Get("cleanup.s04a8a112"));
    AddTooltipForControl(L"nav_1",  StringMgr.Get("optim.sefd30f7b"));
    AddTooltipForControl(L"nav_2",  StringMgr.Get("program.s6fc3b4a1"));
    AddTooltipForControl(L"nav_3",  StringMgr.Get("cleanup.sbfa1541e"));
    AddTooltipForControl(L"nav_4",  StringMgr.Get("optim.s718a46c5"));
    AddTooltipForControl(L"nav_5",  StringMgr.Get("driver.scaed5894"));
    AddTooltipForControl(L"nav_6",  StringMgr.Get("program.s2deb28e5"));
    AddTooltipForControl(L"nav_7",  StringMgr.Get("program.s3d33310d"));
    AddTooltipForControl(L"nav_8",  StringMgr.Get("update.s8b2b67fc"));
    AddTooltipForControl(L"nav_9",  StringMgr.Get("appx.sdb482308"));
    AddTooltipForControl(L"nav_10", StringMgr.Get("unattend.s754a9582"));


    // 空间回收主操作按钮
    AddTooltipForControl(L"btn_scan",    StringMgr.Get("cleanup.s7b5831a9"));
    AddTooltipForControl(L"btn_clean",   StringMgr.Get("shortcut.sd49c8125"));
    AddTooltipForControl(L"btn_refresh", StringMgr.Get("shortcut.s92edb852"));
    AddTooltipForControl(L"find_edit",   StringMgr.Get("shortcut.s53af2996"));
}


void CFrameWnd::RefreshTooltipRects()
{
    if (!m_hwndTip) return;
    for (size_t i = 0; i < m_vTips.size(); ++i)
    {
        CControlUI* pCtrl = m_pm.FindControl(m_vTips[i].sName.GetData());
        if (!pCtrl) continue;
        m_vTips[i].pCtrl = pCtrl;
        TOOLINFOW ti;
        ti = {};
        ti.cbSize = sizeof(ti);
        ti.hwnd = m_hWnd;
        ti.uId = (UINT_PTR)(i + 1);
        ti.rect = pCtrl->GetPos();
        ::SendMessageW(m_hwndTip, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
    }
}


// ============================================================
// [R18 P1] 页面懒加载：首次切入时构建/枚举
// ============================================================
// ============================================================
// [R206] 统计某页控件树规模并落日志。
//
// 只计数、不改状态，目的是给"页面正常加载"一个可量化判据：
//   nodes = 该页根节点下全部子孙控件数
//   leaf  = 其中不可再分的叶子控件数（文字/按钮/编辑框等）
// nodes == 0 说明页面没挂上（结构问题）；nodes 很小说明构建失败。
// ============================================================
// ============================================================
// [R209] 逐页离屏渲染导出 PNG（像素级验收用）。
//
// 关键顺序（每一步都踩过坑）：
//   1) 先 EnsurePageBuilt —— 否则懒构建的页是空壳，渲染出来只有底色；
//   2) 再 SelectItem 切到该页 —— 不切 TabLayout 的话 16 个页面互相重叠，
//      只有当前页参与布局，其它页量出来的位置全错；
//   3) 最后 DoPaint —— SetPos 必须在 DoPaint 之前完成，否则拿到旧尺寸。
// ============================================================
void CFrameWnd::ExportAllPageShots(const wchar_t* pszDir)
{
    if (!pszDir || !pszDir[0]) return;

    CreateDirectoryW(pszDir, nullptr);

    CTabLayoutUI* pTab = static_cast<CTabLayoutUI*>(m_pm.FindControl(L"content_area"));
    if (!pTab)
    {
        AppendLog(L"[shot] content_area not found");
        return;
    }

    RECT rcClient = {};
    ::GetClientRect(m_hWnd, &rcClient);
    const int nW = rcClient.right - rcClient.left;
    const int nH = rcClient.bottom - rcClient.top;

    CDuiString sLog;
    sLog.Format(L"[shot] client=%dx%d pages=%d", nW, nH, pTab->GetCount());
    AppendLog(sLog.GetData());

    for (int i = 0; i < pTab->GetCount(); ++i)
    {
        EnsurePageBuilt(i);
        pTab->SelectItem(i);

        // [R258] SelectItem 之后必须强制重排一次。
        //
        // CTabLayoutUI::SetPos 只给 m_iCurSel 那一页设置矩形，其余页保持旧值。
        // 而 DoPaint 的逐层遍历用的是每个控件自己的 m_rcItem：
        //   CContainerUI::DoPaint 开头
        //   `if (!IntersectRect(&rcTemp, &rcPaint, &m_rcItem)) return true;`
        // 页刚被选中时若矩形还没重算，整棵子树会被这行直接跳过 ——
        // 控件明明在树里、IsVisible() 为真、几何 dump 也有值，
        // 但一次 Paint/DoPaint 都不会进。
        // 这里补一次 SetPos 让矩形落地，与真实切页时的时序对齐。
        {
            CControlUI* pPageCtl = pTab->GetItemAt(i);
            if (pPageCtl && pPageCtl->IsVisible())
            {
                pPageCtl->SetPos(pPageCtl->GetPos());
                if (CControlUI* pRoot = m_pm.GetRoot()) pRoot->SetPos(pRoot->GetPos());
            }
        }

        // [R213] 优化页默认把 25 个组全部折叠，只渲染组头 —— 导出的位图
        //   看起来"整页空白"，实际是 216 条数据行都在折叠的组体里。
        //   验收要看真实内容，所以导出前先全展开；展开失败不影响其它页。
        if (CPageOptim* pOptim = dynamic_cast<CPageOptim*>(PageById(EPageId::PAGE_OPTIM)))
            if (i == static_cast<int>(EPageId::PAGE_OPTIM))
            {
                pOptim->SetAllGroupsExpanded(true);
                // [R213] 展开后取首组前两行的真实几何：开关列在导出位图里
                //   整列为空，必须读控件矩形才能区分"没位置"与"被裁掉"。
                pOptim->DumpOptimRowGeometry(0, 2);
            }

        CPageBase* pPage = PageById(i);
        const wchar_t* pszName = pPage ? pPage->PageName() : L"?";
        if (!pszName || !pszName[0]) pszName = L"?";

        wchar_t szPath[MAX_PATH] = {};
        wfmt::FormatInto(szPath, _countof(szPath), L"%s\\%02d_%s.bmp", pszDir, i, pszName);

        // root 是整个窗口的控件树（含侧栏）；渲染它能得到完整界面。
        CControlUI* pRoot = m_pm.GetRoot();
        const bool bOk = uisnap::RenderRootToPng(pRoot, szPath, nW, nH);

        CDuiString sLine;
        sLine.Format(L"[shot] idx=%d %s -> %s", i, pszName, bOk ? L"ok" : L"FAIL");
        AppendLog(sLine.GetData());

        // [R217] Per-page geometry dump. Written immediately after this page is
        //   rendered, while its layout is still the active one.
        {
            std::wstring sGeo = pszDir;
            sGeo += L"\\_geometry_p";
            wchar_t szIdx[16] = {};
            wfmt::FormatInto(szIdx, _countof(szIdx), L"%02d", i);
            sGeo += szIdx;
            sGeo += L"_";
            sGeo += pszName;
            sGeo += L".txt";
            ExportControlTree(sGeo.c_str());
        }
        // [R209] 逐页 flush：进程渲染完就退出，不 flush 的话队列里的日志全丢
        //   （R206 已踩过一次：崩溃时 probe 日志一条都没落盘）。
        FlushPendingLogs();

        // [R209] 像素统计也写进日志：沙箱禁止跨进程读导出文件，
        //   而日志通道可读，这样验收数据才真正取得到。
        if (bOk)
        {
            const wchar_t* pszStat = uisnap::LastStatsText();
            if (pszStat && pszStat[0]) AppendLog(pszStat);
            FlushPendingLogs();
        }
    }
    AppendLog(L"[shot] done");
}

void CFrameWnd::LogPageContent(int idx)
{
    CPageBase* pPage = PageById(idx);
    const wchar_t* pszName = pPage ? pPage->PageName() : L"?";
    if (!pszName || !pszName[0]) pszName = L"?";

    CControlUI* pRoot = nullptr;
    wchar_t szRoot[64] = {};
    // [R206] 控件名是小写 page_cleanup，而 PageName() 返回 Cleanup（首字母大写）。
    //   直接拼接永远匹配不到，会一直走下面的 fallback。这里先按原名试，
    //   失败再按"首字母小写"试一次 —— 显式处理大小写差异，而不是靠兜底。
    // [R206] 页名到控件名不是机械映射，有两处例外必须显式处理：
    //   1) PageName() 返回 "Cleanup"，XML 是 "page_cleanup"（首字母大小写）；
    //   2) PAGE_FEATURE(7) 的成员是 CPageCapability，但它的 UI 根节点在
    //      page_feature.xml 里叫 "page_feature" —— 没有 "page_capability"。
    //   不处理第 2 条会统计到错误节点或误报 MISSING（实测发现）。
    const wchar_t* pszXmlName = pszName;
    if (lstrcmpiW(pszName, L"Capability") == 0) pszXmlName = L"feature";

    wfmt::FormatInto(szRoot, _countof(szRoot), L"page_%s", pszXmlName);
    pRoot = m_pm.FindControl(szRoot);
    if (!pRoot)
    {
        wchar_t szLower[64] = {};
        size_t n = 0;
        for (; pszXmlName[n] && n + 6 < _countof(szLower); ++n)
            szLower[n] = static_cast<wchar_t>(::towlower(pszXmlName[n]));
        szLower[n] = 0;
        wfmt::FormatInto(szRoot, _countof(szRoot), L"page_%s", szLower);
        pRoot = m_pm.FindControl(szRoot);
    }
    if (pRoot)
    {
        // [R206] 命中即确认名称解析路径正确（大小写差异已被显式处理）。
        CDuiString sHit;
        sHit.Format(L"[page] idx=%d root=%s hit", idx, szRoot);
        AppendLog(sHit.GetData());
    }
    if (!pRoot)
    {
        // 仍未命中时退回页签下的第 idx 个子项，保证不误报 0
        CTabLayoutUI* pTab = static_cast<CTabLayoutUI*>(m_pm.FindControl(L"content_area"));
        if (pTab && idx >= 0 && idx < pTab->GetCount()) pRoot = pTab->GetItemAt(idx);
    }
    if (!pRoot)
    {
        CDuiString sMiss;
        sMiss.Format(L"[page] idx=%d name=%s MISSING", idx, pszName);
        AppendLog(sMiss.GetData());
        return;
    }

    int nNodes = 0;
    int nLeaf = 0;
    std::function<void(CControlUI*)> walk = [&](CControlUI* p)
    {
        if (!p) return;
        ++nNodes;
        CContainerUI* pBox = dynamic_cast<CContainerUI*>(p);
        if (!pBox || pBox->GetCount() == 0) { ++nLeaf; return; }
        const int n = pBox->GetCount();
        for (int i = 0; i < n; ++i) walk(pBox->GetItemAt(i));
    };
    walk(pRoot);

    CDuiString sLine;
    sLine.Format(L"[page] idx=%d name=%s nodes=%d leaf=%d", idx, pszName, nNodes, nLeaf);
    AppendLog(sLine.GetData());
}

void CFrameWnd::EnsurePageBuilt(int idx)
{
    if (idx < 0 || idx >= 16) return;
    if (m_bPageBuilt[idx]) return;
    m_bPageBuilt[idx] = true; NotifyWarmupYield();
    // [R3] 原 6 个懒构建特例（Cleanup 不构建/Startup 启动项+服务双子页/
    // Appx 枚举/Tools Build 后注册 11 个提示/Feature 普通 Build/
    // Update 选“已安装的更新”子页再枚举）已收口为 CPageBase::OnDeferredBuild
    // 虚函数（默认等价 Build()）；标准页与特例页统一经页表驱动。
    if (CPageBase* pPage = PageById(idx)) pPage->OnDeferredBuild();

    // [R206] 逐页渲染自证：统计本页控件树节点数与叶子数。
    //   动机：R205 之前页面 XML 的 <Window> 没剥，16 个页面全挂不上去，
    //   但日志里一条错都没有，界面只表现为某块空白。这里把"页存在"
    //   升级为"页有内容"，让同类结构问题下次直接暴露。
    LogPageContent(idx);

    // [R201] 页面建好后统一把"皮肤里写死的强调色"改成当前系统主题色。
    //   为什么放在这里：XML 不能求值，颜色只能等控件树存在后逐控件覆盖；
    //   而 EnsurePageBuilt 是"本页控件树刚建完"的唯一收口点，
    //   在这里扫一遍最省，也不会漏掉懒构建的页。
    ApplyAccentToControls(idx);
}

// ============================================================
// [R201] 把控件树上仍是"皮肤默认强调色"的元素改为当前系统主题色。
//
// 判据：只改**当前值恰好等于皮肤默认强调色**的控件。
//   这样既不误伤业务代码里刻意用的其他颜色，也让"已是令牌值"的控件
//   在下次调用时保持幂等（值不再等于默认色，自然跳过）。
//
// Win10 兼容：令牌读的是 HKCU 的 AccentColor，Win10 1607+ 同样存在；
//   读不到时令牌回落到 #007ACC，也就是皮肤默认色 —— 行为与改动前一致。
// ============================================================
// [R202] 皮肤令牌表：把 XML 里的 {tok:Surface} 这类占位符解析成实际颜色。
//
// 为什么用"占位符 + 运行期解析"而不是 XML 变量：
//   DuiLib 的 XML 解析器不支持自定义变量，而颜色又必须在控件解析属性时
//   就是合法值。于是用两端配合：
//     1) 启动时先把"令牌名 → ARGB 十六进制串"写进 CPaintManagerUI 的自定义
//        字符串表（皮肤变量），XML 里写 {tok:Surface} 就能被解析出来；
//     2) 系统主题变化时重写这张表并 NeedUpdate 根节点 —— 因为属性已被
//        解析成具体颜色，必须整树重刷才能拿到新值。
//
// 这个做法只在"令牌值变化"时才触发重刷，日常渲染零额外开销。
// ============================================================
// ============================================================
// [R321] 皮肤颜色重放：手动切换主题时，把皮肤里的 {tok:*} 属性按新主题重设。
//
// 为什么必须要它（这是"手动切换才出 bug"的根因）
// ----------------------------------------------
// ExpandSkinFile 在**建树时**就把 {tok:Surface} 换成了字面 "#FF202020"，
// 而建树只发生一次。默认启动时令牌表已经是对的，所以启动看起来正常；
// 手动切主题时令牌表变了，控件里存的仍是上次展开出来的字面值 ——
// 于是"切了主题，界面只变一部分，页面像没加载完整"。
//
// 现有补偿是 ApplyAccentToControls 里的 kLegacy 表：它"按控件当前的
// 颜色值反查令牌"。这条路有三个天生缺陷：
//   1) 只认表里列的 18 个值，皮肤里另外几百处颜色一律残留；
//   2) 值相同但语义不同时无法区分（0xFF333333 既是浅色 InkSecondary
//      也是深色 SurfaceHover，代码注释里已承认只能放弃）；
//   3) 每加一个令牌就要手工补两行，且必须与 ThemeTokens.cpp 逐字同步。
//
// 这里不反查、不猜：**按名字把同一个属性重新设一遍**，值取当前令牌。
// 语义由皮肤原本的 {tok:名} 决定，不存在歧义。
//
// 与 kLegacy 的关系是叠加，不是替换：带名控件走这里精确重放，
// 不带动画名的控件仍由 kLegacy 兜底，所以不会出现"改完反而漏掉
// 原来能修的"这种回退。
// ============================================================
namespace {

struct SkinTokProp
{
    const wchar_t* pszName;   // 控件名，与皮肤 name= 一致
    const wchar_t* pszAttr;   // 属性名，与皮肤里的写法一致
    tokens::EToken  eTok;     // 该属性在皮肤里用的令牌
};

}  // namespace

void CFrameWnd::ReplaySkinTokens()
{
    CControlUI* pRoot = m_pm.GetRoot();
    if (!pRoot) return;

    // [R238] Classic 皮肤没有 {tok:*} 占位符 —— 它的 XML 全是硬编码色，
    //   而且这些硬编码色就是 Classic 品牌识别的一部分，不需要 token 重放。
    //   ReplaySkinTokens 的 757 条条目（titlebar/menubar/nav_0/closebtn/...）
    //   全指向 Win8Metro 语义 token（SurfaceAlt/OnAccent/Danger/...），
    //   在 Classic 下跑会把深蓝标题栏重写成浅灰 SurfaceAlt，彻底破坏 Classic 外观。
    if (tokens::Mode() == tokens::EThemeMode::Classic) return;

    // [PERF] 预先算好所有令牌的 DWORD 值，避免 684 次 Token() 里的 mutex lock
    constexpr int kTokCount = static_cast<int>(tokens::EToken::Count);
    DWORD kTokDword[kTokCount];
    for (int i = 0; i < kTokCount; ++i)
        kTokDword[i] = tokens::Token(static_cast<tokens::EToken>(i));

    // [PERF] WM_SETREDRAW 暂停重绘
    ::SendMessage(m_hWnd, WM_SETREDRAW, FALSE, 0);

    int nSet = 0;
    CDuiString sHex;
    for (int i = 0; i < skin_replay::kEntryCount; ++i)
    {
        const skin_replay::Entry& e = skin_replay::kEntries[i];
        CControlUI* pCtl = m_pm.FindControl(e.pszControl);
        if (!pCtl) continue;
        // 独立缓冲区，不复用 thread_local
        sHex.Format(_T("#%08lX"), static_cast<unsigned long>(kTokDword[static_cast<int>(e.eTok)]));
        pCtl->SetAttribute(e.pszAttr, sHex.GetData());
        ++nSet;
    }

    ::SendMessage(m_hWnd, WM_SETREDRAW, TRUE, 0);
    if (nSet > 0)
    {
        CDuiString sLog;
        sLog.Format(L"[theme] skin token replay applied=%d", nSet);
        AppendLog(sLog.GetData());
    }
}

void CFrameWnd::RefreshScrollBarColors()
{
    CControlUI* pRoot = m_pm.GetRoot();
    if (!pRoot) return;

    // [R238] Classic 皮肤的滚动条颜色已在 Main.xml <Default name="VScrollBar">
    //   里硬编码（thumb=#C8C8C8 / hot=#9A9A9A / pushed=#787878），这些是参考图
    //   像素取色的结果。RefreshScrollBarColors 用 Token(Border)=#ABABAB 覆盖
    //   会改变 Classic 品牌识别，故 Classic 下跳过。
    if (tokens::Mode() == tokens::EThemeMode::Classic) return;

    // 取值口径与 Main.xml 的 <Default name="VScrollBar"> 保持一致：
    //   thumbbkcolor={tok:Border} / thumbhotbkcolor={tok:InkMuted} /
    //   thumbpushedbkcolor={tok:Border} / railbkcolor=透明
    const DWORD crThumb       = tokens::Token(tokens::EToken::Border);
    const DWORD crThumbHot    = tokens::Token(tokens::EToken::InkMuted);
    const DWORD crThumbPushed = tokens::Token(tokens::EToken::Border);
    const DWORD crRail        = 0x00000000;   // 全透明，与皮肤一致

    int nSet = 0;
    // 显式栈遍历而不是递归：控件树在列表页可以很深（树 + 表格 + 行），
    //   递归写法在极端页面有爆栈风险，而这里没有"先序处理"的语义需求。
    std::vector<CControlUI*> stack;
    stack.reserve(64);
    stack.push_back(pRoot);

    while (!stack.empty())
    {
        CControlUI* pCtl = stack.back();
        stack.pop_back();
        if (!pCtl) continue;

        // [R327] 下拉框/输入框的边框色也要跟着主题走。
        //   页面 XML 里的 bordercolor="{tok:Border}" 在皮肤加载时就被展开固化了，
        //   切主题不会重放（它们不在 SkinTokenReplayTable 的具名表里）。
        //   只改颜色不改尺寸：尺寸是设计意图（有的控件故意无边框），由 XML 决定。
        //   注意放在容器判断之前：CComboUI 是容器，但 CEditUI 不是。
        if (dynamic_cast<CComboUI*>(pCtl) || dynamic_cast<CEditUI*>(pCtl))
        {
            pCtl->SetBorderColor(tokens::Token(tokens::EToken::Border));
            pCtl->NeedUpdate();
            ++nSet;
        }

        CContainerUI* pContainer = dynamic_cast<CContainerUI*>(pCtl);
        if (!pContainer) continue;

        for (int i = 0; i < pContainer->GetCount(); ++i)
        {
            if (CControlUI* pChild = pContainer->GetItemAt(i))
                stack.push_back(pChild);
        }

        CScrollBarUI* pBars[2] = { pContainer->GetVerticalScrollBar(),
                                   pContainer->GetHorizontalScrollBar() };
        for (CScrollBarUI* pBar : pBars)
        {
            if (!pBar) continue;
            pBar->SetThumbBkColor(crThumb);
            pBar->SetThumbHotBkColor(crThumbHot);
            pBar->SetThumbPushedBkColor(crThumbPushed);
            pBar->SetRailBkColor(crRail);
            pBar->NeedUpdate();
            ++nSet;
        }
    }

    if (nSet > 0)
    {
        CDuiString sLog;
        sLog.Format(L"[theme] scrollbar colours refreshed=%d", nSet);
        AppendLog(sLog.GetData());
    }
}

void CFrameWnd::ApplyAccentToControls(int idx)
{
    CControlUI* pRoot = m_pm.GetRoot();
    if (!pRoot) return;

    // [R238] Classic 主题有独立品牌色（#007ACC 亮蓝、#1E5799 深蓝等），
    //   这些硬编码值就是它想要的最终色值。把它们重写成 Token(Accent) 会让
    //   Classic 的亮蓝变成用户系统强调色（如紫色），彻底破坏 Classic 品牌识别。
    //   Classic 不走 Legacy→Token 映射。
    if (tokens::Mode() == tokens::EThemeMode::Classic) return;

    // [R202] 皮肤里可能残留的旧字面量（历史代码里直接写死的值）。
    //   XML 已全部改为 {tok:X} 占位符并由 GetSkinFile 预展开，
    //   但代码侧（Page_*.cpp 动态建控件）仍可能写死这些值 —— 这里兜底。
    struct LegacyPair { DWORD dwOld; tokens::EToken eTok; };
    static const LegacyPair kLegacy[] = {
        { 0xFF007ACC, tokens::EToken::Accent },
        { 0xFF1C97EA, tokens::EToken::AccentHot },
        { 0xFF005A9E, tokens::EToken::AccentPushed },
        // [R326] 白色现在映射到 SurfaceAlt（卡片面）而不是 Surface（窗口底）。
        //   浅色改造后 Surface=#F3F3F3、SurfaceAlt=#FFFFFF：代码里写死白色的控件
        //   绝大多数是"卡片/内容面板"，映射到 SurfaceAlt 在浅色下保持白、在深色下
        //   变成 #282828 卡片色 —— 两个方向都对。映射到 Surface 则会让浅色下的
        //   白卡片被刷成灰底 #F3F3F3，与窗口底同色，边界消失。
        { 0xFFFFFFFF, tokens::EToken::SurfaceAlt },
        { 0xFF1A1A1A, tokens::EToken::Ink },
        { 0xFF333333, tokens::EToken::InkSecondary },
        { 0xFF666666, tokens::EToken::InkTertiary },
        { 0xFF999999, tokens::EToken::InkMuted },
        { 0xFFABABAB, tokens::EToken::Border },
        { 0xFFE5E5E5, tokens::EToken::BorderSoft },
        { 0xFFE8E8E8, tokens::EToken::SurfaceSunk },
        { 0xFFE5F1FB, tokens::EToken::SurfaceHover },
        { 0xFFEFF6FC, tokens::EToken::SurfaceAlt },

        // [R326] 浅色改造后的新字面量也必须入表，理由同下面的 R252：
        //   mapLegacy 是"按控件当前值反查令牌"。控件在浅色下持有的是新值
        //   (#D1D1D1/#616161/...)，若表里没有这些键，切到深色时一个都不命中，
        //   浅色残留会原样留在深色界面上。
        { 0xFFF3F3F3, tokens::EToken::Surface },        // Win11 light base
        { 0xFFFBFBFB, tokens::EToken::SurfaceSunk },    // light control fill
        { 0xFFF0F0F0, tokens::EToken::SurfaceHover },   // light neutral hover
        { 0xFFD1D1D1, tokens::EToken::Border },         // light control stroke
        { 0xFF616161, tokens::EToken::InkSecondary },   // light text secondary
        { 0xFF8A8A8A, tokens::EToken::InkTertiary },    // light text tertiary

        // [R252] 深色主题的令牌值也必须入表，否则"深色 -> 浅色"整片残留。
        //
        // mapLegacy 是"按控件当前值反查令牌"：
        //   浅 -> 深：当前值是浅色旧值，命中，能改。所以切深色一直看着还行。
        //   深 -> 浅：当前值是 #202020/#282828/...，一个都不命中，
        //             mapLegacy 返回 0 表示"不认识，别动"，深色全部留下。
        // 这正是用户报的"切到浅色显示不正常、深色残留比较严重"。
        //
        // 这些值必须与 ThemeTokens.cpp 的 BuildLocked 深色分支逐字一致；
        // check_theme_residue.py 会校验两边同步。
        //
        // 自我翻转由 mapLegacy 里的 `dwTarget == cr ? 0` 兜住：
        // 目标值等于当前值时不算"需要替换"，任一控件的值最多被改一次。
        { 0xFF202020, tokens::EToken::Surface },       // SolidBackgroundFillColorBase
        { 0xFF282828, tokens::EToken::SurfaceAlt },    // SolidBackgroundFillColorTertiary
        { 0xFF1C1C1C, tokens::EToken::SurfaceSunk },   // SolidBackgroundFillColorSecondary
        // 0xFF333333 刻意不入表：它是浅色 InkSecondary 与深色 SurfaceHover 的重合值，
        //   一个色值两种语义，取哪个都会错一半。悬停底色由各页
        //   RefreshThemeColors 的 SetHotBkColor 负责（显式语义，无歧义）。
        { 0xFF757575, tokens::EToken::Border },        // surfaceStrokeColorDefault
        { 0xFF323232, tokens::EToken::BorderSoft },    // dividerStrokeColorDefault
        { 0xFFCCCCCC, tokens::EToken::InkSecondary },  // textFillColorSecondary
        { 0xFF969696, tokens::EToken::InkTertiary },   // textFillColorTertiary
        { 0xFF9D9D9D, tokens::EToken::InkMuted },      // 深色
        { 0xFF707070, tokens::EToken::InkMuted },      // 浅色（R208 提对比度后）

    };
    constexpr int kLegacyCount = static_cast<int>(sizeof(kLegacy) / sizeof(kLegacy[0]));

    // 令牌快照：只算一次，循环里直接查表。
    DWORD crTok[kTokenCount] = {};
    for (int i = 0; i < kTokenCount; ++i)
        crTok[i] = tokens::Token(static_cast<tokens::EToken>(i));

    // 整套令牌没变就什么都不做 —— 省掉每次切页的整树扫描。
    //   [R202] 判据从"只看强调色"扩为"令牌指纹"：深色/浅色切换时
    //   强调色可能不变，但底色全变了，只看强调色会漏掉整个主题切换。
    static DWORD s_dwFingerprint = 0;
    DWORD dwFingerprint = 0;
    for (int i = 0; i < kTokenCount; ++i)
        dwFingerprint = dwFingerprint * 131u + crTok[i];
    const bool bChanged = (s_dwFingerprint != dwFingerprint);
    s_dwFingerprint = dwFingerprint;

    // 把旧字面量映射到令牌值（未命中返回 0 = 不替换）
    //
    // [R249] 必须排除"映射结果就是当前值"的情况，否则会自我翻转成死循环。
    //
    // 实测：kLegacy 同时收录了浅色与深色两套令牌值后，
    //   { 0xFF999999 -> InkMuted }  浅色下值为 0x707070
    //   { 0xFF707070 -> InkMuted }  深色下值为 0x9D9D9D
    // 这两个键**互指**。控件当前值是 0x999999 时被改成 0x707070，
    // 下一轮 0x707070 又命中第二项被改成 0x9D9D9D，再下一轮……
    // 每次 SetBkColor 值都不同 -> 每次都 Invalidate() -> 重绘风暴，
    // 实测表现为启动后 CPU 100%、界面刷不出来（用户截图即此现象）。
    //
    // 判据很简单：目标值与当前值相同就不算"需要替换"，返回 0 让调用方跳过。
    // 这样无论表里有多少互指的键，任一控件的值最多被改一次就稳定。
    auto mapLegacy = [&](DWORD cr) -> DWORD
    {
        for (int i = 0; i < kLegacyCount; ++i)
        {
            if (kLegacy[i].dwOld != cr) continue;
            const DWORD dwTarget = crTok[static_cast<int>(kLegacy[i].eTok)];
            return (dwTarget == cr) ? 0 : dwTarget;
        }
        return 0;
    };

    // [UI-REF] Text colours need a narrower rule than backgrounds.
    //   kLegacy maps 0xFFFFFFFF (white) to Surface, which is right for a
    //   background but wrong for text: in dark mode Surface is #1F1F1F, so a
    //   control that legitimately wants WHITE text got repainted near-black.
    //   Measured on the green header band: text came out #1F1F1F on #4E6B08,
    //   contrast 2.69:1 -- unreadable, while the token said #FFFFFF.
    //   White as a TEXT colour is therefore passed through untouched.
    // [UI-REF] Text colours need their own table. kLegacy maps old literals to a
    // token whose value tracks the theme; that is right for a BACKGROUND and wrong for
    // TEXT, because the two have opposite polarity needs:
    //
    //   background white  -> in dark mode must become dark  (Surface)      <- mapLegacy
    //   text white        -> in dark mode must STAY white  (OnAccent/OnHeaderBand)
    //
    // Measured failure before this split: 0xFFFFFFFF white text on the green header
    // band became #1F1F1F in dark mode, contrast 2.69:1 -- unreadable.
    //
    // The previous fix only special-cased 0xFFFFFFFF. That left every OTHER light
    // colour handed to text in the same trap, e.g. 0xFFEFF6FC (SurfaceAlt) used as
    // pale text would be repainted #2B2B2B on a dark field. This table states the
    // intent per value instead of relying on one hard-coded exception.
    struct TextPair { DWORD dwOld; tokens::EToken eTok; };
    static const TextPair kLegacyText[] = {
        // White text is deliberately NOT in this table.
        //
        // The skins use white text on three different dark fields (accent blue,
        // green header band, danger red). Each of those has its OWN on-colour token,
        // and picking one here would break the other two: mapping white -> OnAccent
        // makes it black in the light theme, so the accent-blue buttons would lose
        // their white label. Leaving white untouched keeps every existing pairing
        // intact, which is the conservative and correct choice.
        // Muted/grey text keeps its own token, never a surface token.
        { 0xFF999999, tokens::EToken::InkMuted },
        { 0xFF666666, tokens::EToken::InkTertiary },
        { 0xFF333333, tokens::EToken::InkSecondary },
        { 0xFF1A1A1A, tokens::EToken::Ink },
        { 0xFF007ACC, tokens::EToken::AccentText },
    };
    constexpr int kLegacyTextCount =
        static_cast<int>(sizeof(kLegacyText) / sizeof(kLegacyText[0]));
    auto mapLegacyText = [&](DWORD cr) -> DWORD
    {
        for (int i = 0; i < kLegacyTextCount; ++i)
        {
            if (kLegacyText[i].dwOld != cr) continue;
            const DWORD dwTarget = crTok[static_cast<int>(kLegacyText[i].eTok)];
            // [R249] 同 mapLegacy：目标值等于当前值时返回 0，否则同样会自我翻转。
            return (dwTarget == cr) ? 0 : dwTarget;
        }
        return 0;   // unknown: leave the control's own colour alone
    };
    int nTouched = 0;
    // 逐层遍历：DuiLib 的容器统一走 CContainerUI::GetItemAt。
    //   [R202] 前缀/后缀背景、边框、文字四类属性都要看：深色模式漏掉任何
    //   一类，都会出现"底变了字没变"或"字变了底没变"的不可读组合。
    std::function<void(CControlUI*)> walk = [&](CControlUI* p)
    {
        if (!p) return;

        if (bChanged)
        {
            if (DWORD v = mapLegacy(p->GetBkColor()))       { p->SetBkColor(v); ++nTouched; }
            if (DWORD v = mapLegacy(p->GetBorderColor()))   { p->SetBorderColor(v); ++nTouched; }

            if (CTextUI* pT = dynamic_cast<CTextUI*>(p))
            {
                if (DWORD v = mapLegacyText(pT->GetTextColor())) { pT->SetTextColor(v); ++nTouched; }
            }
            if (CButtonUI* pB = dynamic_cast<CButtonUI*>(p))
            {
                if (DWORD v = mapLegacyText(pB->GetTextColor()))     { pB->SetTextColor(v); ++nTouched; }
                if (DWORD v = mapLegacy(pB->GetHotBkColor()))    { pB->SetHotBkColor(v); ++nTouched; }
                if (DWORD v = mapLegacy(pB->GetPushedBkColor())) { pB->SetPushedBkColor(v); ++nTouched; }
            }
            if (COptionUI* pO = dynamic_cast<COptionUI*>(p))
            {
                if (DWORD v = mapLegacyText(pO->GetTextColor()))     { pO->SetTextColor(v); ++nTouched; }
                if (DWORD v = mapLegacyText(pO->GetSelectedTextColor())) { pO->SetSelectedTextColor(v); ++nTouched; }
            }
            if (CEditUI* pE = dynamic_cast<CEditUI*>(p))
            {
                if (DWORD v = mapLegacyText(pE->GetTextColor())) { pE->SetTextColor(v); ++nTouched; }
                if (DWORD v = mapLegacy(pE->GetBkColor()))   { pE->SetBkColor(v); ++nTouched; }
            }
        }

        if (CContainerUI* pBox = dynamic_cast<CContainerUI*>(p))
        {
            const int n = pBox->GetCount();
            for (int i = 0; i < n; ++i) walk(pBox->GetItemAt(i));
        }
    };
    walk(pRoot);

    if (bChanged && nTouched > 0)
    {
        CDuiString sLog;
        sLog.Format(L"[theme] tokens applied page=%d props=%d dark=%d",
                    idx, nTouched, tokens::IsDark() ? 1 : 0);
        AppendLog(sLog.GetData());
    }
}


// ============================================================
// 管理员权限兜底断言（manifest 已 requireAdministrator）
// ============================================================
bool CFrameWnd::RequireAdmin(const wchar_t* pszAction)
{
    if (ExternalToolService::IsElevated()) return true;
    CDuiString sMsg;
    sMsg.Format(StringMgr.Get("program.sd90addbf"),
                pszAction ? pszAction : StringMgr.Get("misc.sb548362e"));
    ::MessageBoxW(m_hWnd, sMsg.GetData(), StringMgr.Get("misc.safa4b723"),
                  MB_OK | MB_ICONWARNING);
    AppendLogWarn(sMsg.GetData());
    return false;
}


// ============================================================
// [R18 P0] 全局快捷键（加速键表在 Win8Metro.cpp 注册，WM_COMMAND 下发）
// ============================================================
void CFrameWnd::FocusSearchOnCurrentPage()
{
    CTabLayoutUI* pTab = static_cast<CTabLayoutUI*>(m_pm.FindControl(L"content_area"));
    if (!pTab) return;
    int idx = pTab->GetCurSel();


    // 每页实际使用的过滤框
    const wchar_t* pszEdit = nullptr;
    switch (idx)
    {
    case EPageId::PAGE_CLEANUP: pszEdit = L"clean_filter"; break;
    case EPageId::PAGE_APPX:    pszEdit = L"appx_filter";  break;
    case EPageId::PAGE_PROGRAM:
    case EPageId::PAGE_FEATURE: pszEdit = L"prog_filter";  break;
    default:           pszEdit = L"find_edit";    break;
    }
    CEditUI* pEdit = static_cast<CEditUI*>(m_pm.FindControl(pszEdit));
    if (pEdit && pEdit->IsVisible())
    {
        pEdit->SetFocus();
        pEdit->SetSelAll();
    }
}


void CFrameWnd::OnAccelCommand(int nId)
{
    // [R4] 编辑态透传：焦点在原生 EDIT / RichEdit 子窗口内时，
    // Ctrl+A/C/X/V/Y 让位给原生控件，保留打字/选择体验。
    bool bInNativeEdit = false;
    HWND hFocus0 = ::GetFocus();
    if (hFocus0)
    {
        wchar_t szCls[64] = {};
        if (::GetClassName(hFocus0, szCls, _countof(szCls)) > 0)
            bInNativeEdit = (lstrcmpiW(szCls, L"Edit") == 0
                          || lstrcmpiW(szCls, L"RichEdit20W") == 0
                          || lstrcmpiW(szCls, L"RichEdit20A") == 0);
    }


    switch (nId)
    {
    case EAccelCmd::ID_ACCEL_CANCEL:
        // [R4] P6b：扫描取消令牌在清理页，经页表+OnHostCommand 收口
        if (CPageBase* p = PageById(EPageId::PAGE_CLEANUP)) p->OnHostCommand(nId);
        break;
    case EAccelCmd::ID_ACCEL_REFRESH: RefreshAll(); break;
    case EAccelCmd::ID_ACCEL_SCAN:
    case EAccelCmd::ID_ACCEL_CLEAN:
        // [R4] F9 扫描 / F10 清理收口清理页命令入口
        if (CPageBase* p = PageById(EPageId::PAGE_CLEANUP)) p->OnHostCommand(nId);
        break;
    case EAccelCmd::ID_ACCEL_FIND:    FocusSearchOnCurrentPage(); break;
    case EAccelCmd::ID_ACCEL_UNDO:
        // 焦点在 DuiLib 原生编辑框（EDIT 子窗口）内时不抢 Ctrl+Z，保留原生撤销
        {
            HWND hFocus = ::GetFocus();
            if (hFocus && ::GetParent(hFocus) == m_hWnd) break;
        }
        if (CPageBase* p = PageById(EPageId::PAGE_OPTIM)) p->OnHostCommand(nId);
        break;
    // ---- [R4] 快捷键扩展 ----
    case EAccelCmd::ID_ACCEL_NEW:         OnHotkeyCtrlN(); break;
    case EAccelCmd::ID_ACCEL_SAVE:        OnHotkeyCtrlS(); break;
    case EAccelCmd::ID_ACCEL_SELECT_ALL:
        if (bInNativeEdit) break;
        OnHotkeyCtrlA(); break;
    case EAccelCmd::ID_ACCEL_CUT:
        if (bInNativeEdit) break;
        OnHotkeyCtrlX(); break;
    case EAccelCmd::ID_ACCEL_COPY:
        if (bInNativeEdit) break;
        OnHotkeyCtrlC(); break;
    case EAccelCmd::ID_ACCEL_PASTE:
        if (bInNativeEdit) break;
        OnHotkeyCtrlV(); break;
    case EAccelCmd::ID_ACCEL_REDO:
        if (bInNativeEdit) break;
        OnHotkeyCtrlY(); break;
    // ---- [R-B1] 原版 7 条补齐 ----
    case EAccelCmd::ID_ACCEL_OPEN:
        OnHotkeyCtrlN(); break;     // Ctrl+O 与 Ctrl+N 同义：打开规则目录
    case EAccelCmd::ID_ACCEL_SAVEAS:
        // [R-B45] 原版「备份 = 另存为映像」共用 Ctrl+Shift+S：绑定到 EToolDlgKind::TD_CAPTURE（ToolBackup → RunDialog(EToolDlgKind::TD_CAPTURE)，真实 DISM 捕获），
        // 不再只转发当前页 Export（原 OnHotkeyCtrlS），以对齐原版加速键语义。
        // [R4] 经页表+OnHostCommand 收口工具箱页。
        if (CPageBase* p = PageById(EPageId::PAGE_TOOLS)) p->OnHostCommand(nId);
        break;
    case EAccelCmd::ID_ACCEL_NEWWINDOW:
        // 单实例：聚焦已有窗口；无实例时显式重启自身（原 LaunchGUI(nullptr,nullptr)
        // 会拼出 "System32\" 非法路径，什么都启动不了）。
        {
            HWND hOld = ::FindWindowW(L"Win8MetroMainWnd", nullptr);
            if (hOld) { ::SetForegroundWindow(hOld); ShowToast(StringMgr.Get("misc.hb36d"), nullptr, 0); }
            else
            {
                wchar_t szSelf[MAX_PATH] = { 0 };
                if (::GetModuleFileNameW(nullptr, szSelf, _countof(szSelf)) > 0)
                    ::ShellExecuteW(nullptr, L"open", szSelf, nullptr, nullptr, SW_SHOWNORMAL);
            }
        }
        break;
    case EAccelCmd::ID_ACCEL_BREAK:
        // 中断当前扫描/清理（[R4] 收口清理页命令入口）
        if (CPageBase* p = PageById(EPageId::PAGE_CLEANUP)) p->OnHostCommand(nId);
        AppendLogEx(L"[accel] Ctrl+Break cancel requested", static_cast<int>(ELogLevel::LOG_WARN));
        break;
    case EAccelCmd::ID_ACCEL_DELKEY:
        {
            CPageBase* p = CurrentPage();
            if (p) p->OnDeleteKey();
            else   AppendLogEx(L"[accel] Delete no page", static_cast<int>(ELogLevel::LOG_WARN));
        }
        break;
    default: break;
    }
}


// ============================================================
// [R19] 优化设置撤销栈（Ctrl+Z，最近 32 步）
// ============================================================


// ============================================================
// ============================================================
// [R-B1] 快捷键真实接线：Ctrl+N/S/A/X/C/V/Y 由日志占位改为
// 真实动作（按当前 content_area 页分派到 CPageBase::OnGlobal*）。
// Ctrl+X 与 Ctrl+C 等价（不删除源内容，仅 Toast 提示差异）；
// Ctrl+N 打开规则目录；Ctrl+V 聚焦当前页搜索框。
// ============================================================



void CFrameWnd::OpenRulesDir()
{
    CDuiString sExe;
    wchar_t wsz[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, wsz, _countof(wsz));
    sExe = wsz;
    int p1 = sExe.ReverseFind(L'\\');
    if (p1 > 0) sExe = sExe.Left(p1);



    CDuiString sRules = sExe + L"\\Data\\CleanRules";
    if (::GetFileAttributesW(sRules.GetData()) == INVALID_FILE_ATTRIBUTES)
        sRules = sExe;



    ::ShellExecuteW(m_hWnd, L"open", L"explorer.exe",
                    sRules.GetData(), nullptr, SW_SHOWNORMAL);



    CDuiString sLog;
    sLog.Format(L"[accel] Ctrl+N open rules dir: %s", sRules.GetData());
    AppendLogEx(sLog.GetData(), static_cast<int>(ELogLevel::LOG_INFO));
    ShowToast(StringMgr.Get("misc.h4c5f"), sRules.GetData(), 0);
}



void CFrameWnd::OnHotkeyCtrlN()
{
    OpenRulesDir();
}



void CFrameWnd::OnHotkeyCtrlS()
{
    CPageBase* p = CurrentPage();
    if (p) p->OnGlobalSave();
    CDuiString sLog;
    sLog.Format(L"[accel] Ctrl+S save current page (%s)",
                p ? p->PageName() : L"none");
    AppendLogEx(sLog.GetData(), static_cast<int>(ELogLevel::LOG_INFO));
}



void CFrameWnd::OnHotkeyCtrlA()
{
    CPageBase* p = CurrentPage();
    if (p) p->OnGlobalSelectAll();
    CDuiString sLog;
    sLog.Format(L"[accel] Ctrl+A select all (%s)",
                p ? p->PageName() : L"none");
    AppendLogEx(sLog.GetData(), static_cast<int>(ELogLevel::LOG_INFO));
}



void CFrameWnd::OnHotkeyCtrlX()
{
    // 与 Ctrl+C 等价：不删除源内容，仅 Toast 提示差异
    OnHotkeyCtrlC();
    ShowToast(StringMgr.Get("misc.hbdc4"), nullptr, 0);
}



void CFrameWnd::OnHotkeyCtrlC()
{
    CPageBase* p = CurrentPage();
    if (p) p->OnGlobalCopy(false);
    CDuiString sLog;
    sLog.Format(L"[accel] Ctrl+C copy (%s)",
                p ? p->PageName() : L"none");
    AppendLogEx(sLog.GetData(), static_cast<int>(ELogLevel::LOG_INFO));
}



void CFrameWnd::OnHotkeyCtrlV()
{
    // 聚焦当前页搜索框（若该页有 find_edit 或 *_filter）
    FocusSearchOnCurrentPage();
    CDuiString sLog;
    sLog.Format(L"[accel] Ctrl+V focus search (%s)",
                CurrentPage() ? CurrentPage()->PageName() : L"none");
    AppendLogEx(sLog.GetData(), static_cast<int>(ELogLevel::LOG_INFO));
    ShowToast(StringMgr.Get("misc.ha74d"), nullptr, 0);
}



void CFrameWnd::OnHotkeyCtrlY()
{
    CPageBase* p = CurrentPage();
    if (p) p->OnGlobalRedo();
    CDuiString sLog;
    sLog.Format(L"[accel] Ctrl+Y redo (%s)",
                p ? p->PageName() : L"none");
    AppendLogEx(sLog.GetData(), static_cast<int>(ELogLevel::LOG_INFO));
}



// ============================================================
// [R19] 键盘焦点可见高亮（Tab 导航时给焦点控件描蓝边）
// ============================================================
void CFrameWnd::OnFocusChanged(CControlUI* pNewFocus)
{
    if (m_bFocusGuard) return;
    m_bFocusGuard = true;


    // 还原上一个控件
    if (m_pFocusCtrl && m_bOldBorderSet)
    {
        m_pFocusCtrl->SetBorderColor(m_dwOldBorderColor);
        m_pFocusCtrl->SetBorderSize(m_nOldBorderSize);
    }
    m_pFocusCtrl = nullptr;
    m_bOldBorderSet = false;


    if (pNewFocus)
    {
        // 只高亮可交互控件（按钮/复选/选项/编辑/组合框），跳过容器/文本
        const wchar_t* pszClass = pNewFocus->GetClass();
        static const wchar_t* szHi[] = {
            L"ButtonUI", L"OptionUI", L"CheckBoxUI",
            L"EditUI", L"ComboUI", L"SliderUI" };
        bool bHi = false;
        for (int k = 0; k < _countof(szHi); ++k)
            if (pszClass && lstrcmpiW(pszClass, szHi[k]) == 0) { bHi = true; break; }
        if (bHi)
        {
            m_pFocusCtrl = pNewFocus;
            m_dwOldBorderColor = pNewFocus->GetBorderColor();
            m_nOldBorderSize = pNewFocus->GetBorderSize();
            m_bOldBorderSet = true;
            pNewFocus->SetBorderColor(tokens::Token(tokens::EToken::AccentBorder));   // [R201]
            pNewFocus->SetBorderSize(1);
        }
    }
    m_bFocusGuard = false;
}


CControlUI* CFrameWnd::DeepFind(CControlUI* pRoot, const wchar_t* pszName)
{
    if (!pRoot) return nullptr;
    if (pRoot->GetName() == pszName) return pRoot;


    CContainerUI* pContainer = dynamic_cast<CContainerUI*>(pRoot);
    if (pContainer)
    {
        int n = pContainer->GetCount();
        for (int i = 0; i < n; ++i)
        {
            CControlUI* pFound = DeepFind(pContainer->GetItemAt(i), pszName);
            if (pFound) return pFound;
        }
    }
    return nullptr;
}


// keep the leading box glyph in sync with check state
void CFrameWnd::RefreshCheckText(CCheckBoxUI* pCheck)
{
    if (pCheck == nullptr) return;
    // [BOX-CHECK] 自绘复选框：方框由控件自己画，这里只同步勾选态。
    if (auto pBox = dynamic_cast<CBoxCheckUI*>(pCheck))
    {
        pBox->SetBoxState(pCheck->GetCheck() ? CBoxCheckUI::BoxChecked : CBoxCheckUI::BoxEmpty);
        return;
    }
    CDuiString strText = pCheck->GetText();
    if (strText.GetLength() >= 3)
    {
        wchar_t cFirst = strText[0];
        // [修复 R5 / P1-6] 同时识别半选字形 glyph_ck2，避免剥除时残留
        if (cFirst == m_sGlyphBox[0][0]
            || cFirst == m_sGlyphBox[1][0]
            || (m_sGlyphBox[2].GetLength() > 0 && cFirst == m_sGlyphBox[2][0]))
            strText = strText.Mid(3);
    }
    CDuiString strShow = pCheck->GetCheck() ? m_sGlyphBox[1] : m_sGlyphBox[0];
    strShow += L"  ";
    strShow += strText;
    pCheck->SetText(strShow.GetData());
}


// [修复 R5 / P1-6] 组复选框三态字形：0 空 / 1 半选(▣) / 2 全选(☑)
void CFrameWnd::SetGroupGlyph(CCheckBoxUI* p, int nState)
{
    if (!p) return;
    int idx = (nState >= 0 && nState <= 2) ? nState : 0;

    // [BOX-CHECK] 自绘复选框：三态直接落到控件上（方框/填充/标记全由控件画），
    //   不再改写文字里的字形前缀。
    if (auto pBox = dynamic_cast<CBoxCheckUI*>(p))
    {
        pBox->SetBoxState(idx);
        return;
    }


    // 剥掉旧的前导字形（1 字符）+ 双空格，保留组名
    CDuiString strText = p->GetText();
    if (strText.GetLength() >= 3)
    {
        wchar_t cFirst = strText[0];
        if (cFirst == m_sGlyphBox[0][0]
            || cFirst == m_sGlyphBox[1][0]
            || (m_sGlyphBox[2].GetLength() > 0 && cFirst == m_sGlyphBox[2][0]))
            strText = strText.Mid(3);
    }


    CDuiString strShow = m_sGlyphBox[idx];
    strShow += L"  ";
    strShow += strText;
    p->SetText(strShow.GetData());
}


// read glyphs from (hidden) skin controls
void CFrameWnd::LoadSkinGlyphs()
{
    CTextUI* p0 = (CTextUI*)m_pm.FindControl(L"glyph_ck0");
    CTextUI* p1 = (CTextUI*)m_pm.FindControl(L"glyph_ck1");
    CTextUI* p2 = (CTextUI*)m_pm.FindControl(L"glyph_ck2");
    if (p0) m_sGlyphBox[0] = p0->GetText();
    if (p1) m_sGlyphBox[1] = p1->GetText();
    if (p2) m_sGlyphBox[2] = p2->GetText();
    if (m_sGlyphBox[0].IsEmpty()) m_sGlyphBox[0] = L"[ ]";
    if (m_sGlyphBox[1].IsEmpty()) m_sGlyphBox[1] = L"[x]";
    // [修复 R5 / P1-6] 半选字形兜底在读取 glyph_dash 之后再做（见下方）


    CTextUI* pAr = (CTextUI*)m_pm.FindControl(L"glyph_ar");
    CTextUI* pAd = (CTextUI*)m_pm.FindControl(L"glyph_ad");
    if (pAr) m_sGlyphArrow[0] = pAr->GetText();
    if (pAd) m_sGlyphArrow[1] = pAd->GetText();
    if (m_sGlyphArrow[0].IsEmpty()) m_sGlyphArrow[0] = L">";
    if (m_sGlyphArrow[1].IsEmpty()) m_sGlyphArrow[1] = L"v";


    CTextUI* pDash = (CTextUI*)m_pm.FindControl(L"glyph_dash");
    if (pDash) m_sGlyphDash = pDash->GetText();
    if (m_sGlyphDash.IsEmpty()) m_sGlyphDash = L"-";


    // [修复 R5 / P1-6] 半选字形兜底：皮肤缺 glyph_ck2 时用连字符字形
    if (m_sGlyphBox[2].IsEmpty()) m_sGlyphBox[2] = m_sGlyphDash;
}


// ============================================================
// P5：driver 薄转发（实现迁 CPageDriver）
// ============================================================


// ============================================================
// [R51] Performance monitoring: measure startup time
// ============================================================
void CFrameWnd::MeasureStartupTime()
{
    DWORD dwNow = ::GetTickCount();
    DWORD dwElapsed = dwNow - m_dwStartupMs;
    // Log startup time for diagnostics
    CDuiString sLog;
    sLog.Format(L"[perf] Startup time: %lu ms", dwElapsed);
    AppendLog(sLog.GetData());
}
// ============================================================
// P-P2：新版工具方法转发（R59-R74 功能依赖）
// ============================================================
void CFrameWnd::GetTempDir(CDuiString& sOut)
{
    sOut = FrameWnd_Util_GetTempDir();
}
// ============================================================
// [R-B3] CLI 参数层消费点
// ------------------------------------------------------------
// Win8Metro.cpp 已在最早期 Parse(lpCmdLine)（见 _tWinMain）。
// 此处按命令行动作映射到已存在的页面/工具能力。
// ============================================================
// [R139] 递归遍历控件树并落盘。
//   为什么用 GetInterface(L"ContainerUI") 而不是 dynamic_cast：
//   仓内既有代码一律走静态接口查询以避免 RTTI 依赖（见 Page_Startup 的过滤逻辑）。
namespace
{
    // 宽字符 -> UTF-8 字节串。失败返回空串（调用方据此可发现异常）。
    std::string LayoutToUtf8(const wchar_t* w)
    {
        if (!w || !*w) return std::string();
        const int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
        if (n <= 0) return std::string();
        std::string s(static_cast<size_t>(n), 0);
        const int wr = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
        if (wr <= 0) return std::string();
        s.resize(::strlen(s.c_str()));   // 去掉结尾 NUL
        return s;
    }

    void DumpLayoutRec(CControlUI* p, int nDepth, std::ofstream& ofs, int& nTotal,
                       int& nZeroSize, int nMaxNodes)
    {
        if (!p || nTotal >= nMaxNodes) return;
        ++nTotal;
        const RECT rc = p->GetPos();
        const int nW = rc.right - rc.left;
        const int nH = rc.bottom - rc.top;
        const bool bVis = p->IsVisible();
        if (bVis && (nW <= 0 || nH <= 0)) ++nZeroSize;

        // CSV：depth,name,class,left,top,right,bottom,visible
        ofs << nDepth << ","
            << LayoutToUtf8(p->GetName()) << ","
            << LayoutToUtf8(p->GetClass()) << ","
            << rc.left << "," << rc.top << "," << rc.right << "," << rc.bottom << ","
            << (bVis ? 1 : 0) << "\n";

        CContainerUI* pc = p->GetInterface(L"ContainerUI")
            ? static_cast<CContainerUI*>(p) : nullptr;
        if (!pc) return;
        for (int i = 0; i < pc->GetCount(); ++i)
            DumpLayoutRec(pc->GetItemAt(i), nDepth + 1, ofs, nTotal, nZeroSize, nMaxNodes);
    }
}


// [R139-2] 逐页构建后各 dump 一份。
//   为什么需要：页面懒构建，DumpLayoutTree 在 ApplyCommandLineActions 时只有根节点。
//   页序取自 EPageId，覆盖 0..PAGE_WELCOME 全部。
void CFrameWnd::DumpLayoutAllPages()
{
    wchar_t szDir[MAX_PATH] = {};
    ::GetTempPathW(MAX_PATH, szDir);
    wcscat_s(szDir, L"dismtool_layout");
    ::CreateDirectoryW(szDir, nullptr);

    for (int idx = 0; idx <= static_cast<int>(EPageId::PAGE_WELCOME); ++idx)
    {
        SwitchToPage(idx);
        EnsurePageBuilt(idx);
        // [R139-5] 这个函数在 InitWindow 尾部调用，此时窗口还没进消息循环，
        //   控件树只有根节点（TOTAL=1）。SelectItem / SetPos / WM_SIZE 都补不齐：
        //   DuiLib 的布局是在 WM_PAINT 处理里按 m_bUpdateNeeded 跑的，
        //   没有消息泵就不会有真实矩形。
        //   所以先把消息队列里已经排队的 WM_PAINT/WM_SIZE 消化掉，再 dump。
        {
            CTabLayoutUI* pTab = static_cast<CTabLayoutUI*>(m_pm.FindControl(L"content_area"));
            if (pTab) pTab->SelectItem(idx);
            CControlUI* pRoot = m_pm.GetRoot();
            if (pRoot) pRoot->SetPos(pRoot->GetPos());

            RECT rcClient = {};
            ::GetClientRect(m_hWnd, &rcClient);
            ::SendMessageW(m_hWnd, WM_SIZE, SIZE_RESTORED,
                           MAKELPARAM(rcClient.right - rcClient.left,
                                      rcClient.bottom - rcClient.top));
            // 真正让布局落地的是一次 painting：DuiLib 在 WM_PAINT 里更新子项矩形。
            ::InvalidateRect(m_hWnd, nullptr, FALSE);
            MSG msg = {};
            for (int spin = 0; spin < 64 && ::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE); ++spin)
            {
                ::TranslateMessage(&msg);
                ::DispatchMessageW(&msg);
            }
            ::UpdateWindow(m_hWnd);
        }

        wchar_t szPath[MAX_PATH] = {};
        // 用仓内统一格式化封装（wprintf_s 在本 TU 不可靠）
        wfmt::FormatInto(szPath, _countof(szPath), L"%s\\page_%02d.txt", szDir, idx);
        std::ofstream ofs(szPath, std::ios::out | std::ios::trunc);
        if (!ofs.is_open()) continue;
        ofs << "depth,name,class,left,top,right,bottom,visible" << std::endl;

        const int kMaxNodes = 20000;
        int nTotal = 0, nZeroSize = 0;
        DumpLayoutRec(m_pm.GetRoot(), 0, ofs, nTotal, nZeroSize, kMaxNodes);
        ofs << "#PAGE," << idx << std::endl;
        ofs << "#TOTAL," << nTotal << std::endl;
        ofs << "#ZEROSIZE," << nZeroSize << std::endl;
    }
}
void CFrameWnd::DumpLayoutTree()
{
    wchar_t szPath[MAX_PATH] = {};
    ::GetTempPathW(MAX_PATH, szPath);
    wcscat_s(szPath, L"dismtool_layout.txt");

    // 用 ofstream 而不是 ofwstream：与自检台同款做法，避免 locale facet 失败导致
    //   输出被静默丢弃（R31 踩过这个坑）。
    std::ofstream ofs(szPath, std::ios::out | std::ios::trunc);
    if (!ofs.is_open())
    {
        ::OutputDebugStringW(L"[R139] 无法创建 layout 输出文件\r\n");
        return;
    }
    ofs << "depth,name,class,left,top,right,bottom,visible\n";

    // 上限 20000 节点：防病态布局把文件写爆；正常页面在千级。
    const int kMaxNodes = 20000;
    int nTotal = 0, nZeroSize = 0;
    DumpLayoutRec(m_pm.GetRoot(), 0, ofs, nTotal, nZeroSize, kMaxNodes);

    ofs << "#TOTAL," << nTotal << "\n";
    ofs << "#ZEROSIZE," << nZeroSize << "\n";
    ofs.close();

    wchar_t szMsg[MAX_PATH + 64] = {};
    wprintf_s(szMsg, _countof(szMsg), L"[R139] layout dumped: nodes=%d zeroSize=%d -> %s\n",
              nTotal, nZeroSize, szPath);
    ::OutputDebugStringW(szMsg);
}

void CFrameWnd::ApplyCommandLineActions()
{
    if (!CommandLineService::WasParsed()) return;
    // ---- [R139] /DumpLayout：控件树落盘（诊断用，不改变界面行为）----
    //   放在最前面：它是纯观测动作，不应受其他开关影响，也不应触发它们。
    //   [R139-2] 页面是懒构建的（EnsurePageBuilt 在首次切入时才 Build），
    //   只 dump 当前页会得到"仅根节点、0x0"的假象。改为逐页构建后各出一份，
    //   这样才能验证每个页面的控件几何。
    if (CommandLineService::Has(L"dumplayout"))
    {
        // [R139-6] 不能在 InitWindow 里直接 dump。
        //   实测：无论补多少 SetPos / WM_SIZE / PeekMessage，导出的 page_XX.txt 恒为
        //   TOTAL=1、ZEROSIZE=1 —— 因为这里窗口刚 CenterWindow 完，
        //   主消息循环还没跑，DuiLib 的布局是在 WM_PAINT 处理里做的。
        //   改为投递一条自定义消息：等主循环起来、首帧画完之后再 dump，
        //   此时控件矩形才是真实值。窗口若在首帧前退出，这条消息也就不会发生，
        //   不会写出误导性的 0x0 数据。
        ::PostMessageW(m_hWnd, WM_APP + 0x131, 0, 0);
    }
    // ---- /? 帮助 ----
    if (CommandLineService::IsHelpRequested())
    {
        ::MessageBoxW(m_hWnd, CommandLineService::GetHelpText(),
                      L"Win8Metro Command Line", MB_OK | MB_ICONINFORMATION);
        return;
    }
    // ---- /ScratchDir= 临时工作目录（影响后续 MakeTempWorkDir 前缀）----
    DuiLib::CDuiString sScratch;
    if (CommandLineService::HasScratchDir(sScratch))
    {
        AppendLog(CDuiString(L"[cli] scratch dir: ") + sScratch);
    }
    // ---- /Plugin= 插件目录（记录到日志）----
    DuiLib::CDuiString sPlugin;
    if (CommandLineService::HasPluginDir(sPlugin))
    {
        AppendLog(CDuiString(L"[cli] plugin dir: ") + sPlugin);
    }
    // ---- /exe=<alg> 压缩算法（写入 config.ini [General] Compress）----
    EClrCompress eComp = CommandLineService::GetCompress();
    if (eComp != EClrCompress::ECLR_COMP_NONE)
    {
        const wchar_t* pszAlg = nullptr;
        switch (eComp)
        {
        case EClrCompress::ECLR_COMP_XPRESS4K:  pszAlg = L"xpress4k";  break;
        case EClrCompress::ECLR_COMP_XPRESS8K:  pszAlg = L"xpress8k";  break;
        case EClrCompress::ECLR_COMP_XPRESS16K: pszAlg = L"xpress16k"; break;   // [R-B48]
        case EClrCompress::ECLR_COMP_LZX:      pszAlg = L"lzx";      break;
        case EClrCompress::ECLR_COMP_MAX:      pszAlg = L"max";      break;
        default: break;
        }
        if (pszAlg)
        {
            i18n::CConfigReader::SetString(L"General", L"Compress", pszAlg);
            AppendLog(CDuiString(L"[cli] compress algorithm: ") + pszAlg);
        }
    }
    // ---- /Image=<path> 打开映像（切到 ImageX 弹窗）----
    DuiLib::CDuiString sImage;
    if (CommandLineService::HasImagePath(sImage))
    {
        AppendLog(CDuiString(L"[cli] image: ") + sImage);
        SwitchToPage(EPageId::PAGE_TOOLS);
        EnsurePageBuilt(EPageId::PAGE_TOOLS);
        if (CPageBase* p = PageById(EPageId::PAGE_TOOLS)) p->HostRunToolDialog(EToolDlgKind::TD_IMAGEX);
        return;   // 弹窗为模态，避免继续叠加后续动作
    }
    // ---- /All 全选清理规则 ----
    if (CommandLineService::IsSelectAll())
    {
        SwitchToPage(EPageId::PAGE_CLEANUP);
        EnsurePageBuilt(EPageId::PAGE_CLEANUP);
        // [R4] R-B1 语义：清理页全选（基类全局动作虚函数，经页表调用）
        if (CPageBase* p = PageById(EPageId::PAGE_CLEANUP)) p->OnGlobalSelectAll();
    }
    // ---- /CompactOS 执行 CompactOS 优化 ----
    if (CommandLineService::GetPrimaryAction() == EClrAction::ECLR_COMPACTOS)
    {
        SwitchToPage(EPageId::PAGE_CLEANUP);
        EnsurePageBuilt(EPageId::PAGE_CLEANUP);
        // 复用清理页：置 CompactOS 规则选中并启动扫描（[R4] 命令收口）
        if (CPageBase* p = PageById(EPageId::PAGE_CLEANUP)) p->OnHostCommand(static_cast<int>(EAccelCmd::ID_ACCEL_SCAN));
        return;
    }
    // ---- /RestoreHealth 修复 Windows 映像（原生化：dismapi 直连控制台）----
    if (CommandLineService::GetPrimaryAction() == EClrAction::ECLR_RESTOREHEALTH)
    {
        // [原生化 B1] 等价 dism /Online|/Image /Cleanup-Image /RestoreHealth，
        // 不再生成 restorehealth.bat。
        StartSystemRepair(true /*RestoreHealth*/, false /*CLI 仅映像修复*/);
        return;
    }
    ApplyShellVerbs();
    // ---- /C 启动清理 ----
    if (CommandLineService::GetPrimaryAction() == EClrAction::ECLR_CLEAN)
    {
        SwitchToPage(EPageId::PAGE_CLEANUP);
        EnsurePageBuilt(EPageId::PAGE_CLEANUP);
        // /All 已在上面全选；否则按默认选择
        // [R5] 默认规则选择经页命令虚入口；启动清理走命令入口
        if (!CommandLineService::IsSelectAll())
            if (CPageBase* p = PageById(EPageId::PAGE_CLEANUP)) p->OnHostCommand(static_cast<int>(EHostCmd::HCMD_CLEAN_SELECT_DEFAULT));
        AppendLog(L"[cli] /C: starting cleanup");
        if (CPageBase* p = PageById(EPageId::PAGE_CLEANUP)) p->OnHostCommand(static_cast<int>(EAccelCmd::ID_ACCEL_CLEAN));
        return;
    }
    // ---- [R-AUTOTEST] /CleanSelect：布置默认勾选后自动启动清理（回归验证入口）----
    // 自动化回归专用：默认勾选 + 确认框自动应答 + 启动清理，一次进程覆盖
    // "进度回报→删除→尾巴收口→解冻回投" 全链路。
    if (CommandLineService::Has(L"CleanSelect"))
    {
        SwitchToPage(EPageId::PAGE_CLEANUP);
        EnsurePageBuilt(EPageId::PAGE_CLEANUP);
        AppendLog(L"[cli] /CleanSelect: applying default selection and starting cleanup");
        if (CPageBase* p = PageById(EPageId::PAGE_CLEANUP)) p->OnHostCommand(static_cast<int>(EHostCmd::HCMD_CLEAN_SELECT_DEFAULT));
        if (CPageBase* p = PageById(EPageId::PAGE_CLEANUP)) p->OnHostCommand(static_cast<int>(EAccelCmd::ID_ACCEL_CLEAN));
        return;
    }
    // ---- /U 无人值守（危险确认自动通过，已在 HostConfirm 层生效）----
    if (CommandLineService::IsUnattended())
        AppendLog(L"[cli] unattended mode enabled");
}


// [C++17] 角度->弧度换算（原 L449 函数内 PI 魔数常量化）
inline constexpr float kDegreeToRadian = 3.14159265f / 180.0f;
// [REFACTOR] 从 Notify 提取：顶部导航 nav_<index> 点击处理。
// 含 WinPE 离线会话门控（拦截仅在线可用的页面并恢复当前页）与
// 延迟构建调度（重活交 1ms 定时器，避免切页时界面未响应）。
// 逐字搬迁零逻辑改动。
void CFrameWnd::OnNavClicked(const wchar_t* pszName)
{
    // ----------------------------------------------------------------
    // 顶部导航（nav_<index>）点击处理
    // ----------------------------------------------------------------
    // 导航条控件名为 nav_0 … nav_N，序号即 EPageId 的页序。
    // 本函数分三步：参数解析 → WinPE 门控 → 延迟构建 + 切页。
    //
    // 一、参数解析
    //   pszName 形如 "nav_3"，跳过 4 字符前缀后 _ttoi 取页序。
    //   前缀长度与下方 wcsncmp 的 4 必须同步，改一处须改另一处。
    //
    // 二、WinPE 离线会话门控
    //   离线映像（挂载的 install.wim）里没有运行中的系统状态，
    //   下列页面的操作对象都不存在离线等价物：
    //     PAGE_STARTUP  自启项枚举依赖在线注册表与启动配置
    //     PAGE_APPX     AppX 包模型依赖运行中的 AppXSvc
    //     PAGE_UPDATE   Windows Update 需要在线服务
    //     PAGE_ASSOC    文件关联依赖用户配置单元
    //     PAGE_TASKS    计划任务依赖运行中的任务计划服务
    //     PAGE_CONTROL  控制面板小程序依赖在线 COM 组件
    //     PAGE_REGCLEAN 失效引用扫描是在线语义
    //   命中任一即：Toast 说明原因 → SwitchToPage 恢复到原页 → return，
    //   杜绝页面内命令误作用于 PE RAM 盘。
    //   注意：这里是「拦截并回滚」，不是「静默忽略」——
    //   用户必须看到为什么点不动，否则会误以为程序卡死。
    //
    // 三、延迟构建调度
    //   首次进入某页的重活（服务枚举、优化树构建、注册表扫描等
    //   数百行 UI 构建）若在点击消息里同步执行，切页动画会被阻塞，
    //   用户看到长时间「未响应」。做法：
    //     1) RequestDeferredBuild(idx) —— 只排队一个 1ms 一次性定时器，
    //        让切页动画与空白页先完成绘制；
    //     3) pTab->SelectItem(idx) —— 立即切页，视觉上无延迟。
    //   重活真正执行发生在定时器回调里，不在本函数。
    //
    // 四、切页后的收尾
    //   （原 FlushBufferToOutput 随 out_* 死链路删除，切页不再需要刷日志区）
    //   RefreshTooltipRects()：重算 tooltip 命中矩形，
    //     因为新页的控件位置与旧页不同，不重算会出现
    //     「提示框出现在错误位置」或「悬停无提示」。
    //
    // 关于 VerifyOptimStates 的 5s TTL：
    //   优化页的真实状态校验有 5 秒缓存，不在本函数里做，
    //   统一由延迟构建定时器执行，避免切页瞬间同步扫一遍注册表。
    // ----------------------------------------------------------------
        int idx = _ttoi(pszName + 4);
        // [P0] WinPE 离线会话门控：下列页面的操作对象只存在于运行中的
        // 系统（自启枚举/AppX 模型/WU/用户关联/计划任务/控制面板小程序/
        // 失效引用在线扫描），对挂载目标无离线等价物。拦截切换并恢复
        // 当前页，杜绝页面内命令误作用于 PE RAM 盘。
        if (COfflineSession::IsActive())
        {
            static const EPageId kPeBlockedNav[] = {
                EPageId::PAGE_STARTUP, EPageId::PAGE_APPX, EPageId::PAGE_UPDATE, EPageId::PAGE_ASSOC,
                EPageId::PAGE_TASKS, EPageId::PAGE_CONTROL, EPageId::PAGE_REGCLEAN
            };
            bool bBlocked = false;
            for (size_t k = 0;
                 k < sizeof(kPeBlockedNav) / sizeof(kPeBlockedNav[0]); ++k)
            {
                if (idx == static_cast<int>(kPeBlockedNav[k])) { bBlocked = true; break; }
            }
            if (bBlocked)
            {
                CTabLayoutUI* pCurTab = static_cast<CTabLayoutUI*>(
                    m_pm.FindControl(L"content_area"));
                int nCurPage = pCurTab ? pCurTab->GetCurSel() : static_cast<int>(EPageId::PAGE_WELCOME);
                ShowToast(L"WinPE offline mode",
                          L"This page is online-only and cannot operate on an "
                             "offline Windows image. Boot the target system to use it.",
                          2);
                SwitchToPage(nCurPage);
                return;
            }
        }
        // [LAG-FIX] 首次切页的重活（服务/优化树/注册表枚举等数百行 UI 构建）
        // 不再在点击消息里同步执行：仅排队 1ms 一次性定时器，让切页动画与
        // 空白页先完成绘制，杜绝点击导航时长时间"未响应"。
        RequestDeferredBuild(idx);
        CTabLayoutUI* pTab = static_cast<CTabLayoutUI*>(m_pm.FindControl(L"content_area"));
        if (pTab) pTab->SelectItem(idx);
        // smart load: verify real states when entering the optimize page
        // （VerifyOptimStates 有 5 秒 TTL；延迟构建定时器里统一执行）
        RefreshTooltipRects();
        return;
}
// [REFACTOR] 从 ApplyCommandLineActions 提取：资源管理器外壳右键动词。
// 覆盖 /ESD2ISO /ESD2WIM /ESDDECRYPT（ESD 外壳扩展）、
// /CompactEx=<path>（compact 右键，原生 WOF LZX）、
// /C /exe <alg>（KB57 基线动词 1，递归 WOF 压缩）、
// /U <path>（KB57 基线动词 2，递归解压）。
// 命中任一即执行并 return（外壳动词互斥，不叠加）。逐字搬迁零逻辑改动。
void CFrameWnd::ApplyShellVerbs()
{
// ---- /ESD2ISO /ESD2WIM /ESDDECRYPT 外壳右键动作（[R-B28] 资源管理器外壳右键扩展）----
{
    EClrAction eEsd = CommandLineService::GetPrimaryAction();
    if (eEsd == EClrAction::ECLR_ESD2ISO || eEsd == EClrAction::ECLR_ESD2WIM || eEsd == EClrAction::ECLR_ESDDECRYPT)
    {
        CDuiString sTarget;
        if (CommandLineService::GetEsdTarget(sTarget))
        {
            AppendLog(CDuiString(L"[cli] shell esd action: ") + sTarget);
            SwitchToPage(EPageId::PAGE_TOOLS);
            EnsurePageBuilt(EPageId::PAGE_TOOLS);
            // [R-B35] ESD 解密 → 独立解密向导（预填源 ESD）；
            //       ESD 转 ISO / 转 WIM 仍走 ImageX 弹窗。
            if (eEsd == EClrAction::ECLR_ESDDECRYPT)
                // [R5] ESD 解密向导（预填源 ESD）经页能力虚入口
                if (CPageBase* p = PageById(EPageId::PAGE_TOOLS)) p->HostEsdDecryptWithFile(sTarget);
            else
                if (CPageBase* p = PageById(EPageId::PAGE_TOOLS)) p->HostRunToolDialog(EToolDlgKind::TD_IMAGEX);
        }
        return;
    }
}
// ---- /CompactEx=<path> 资源管理器 compact 右键动作（[R-B55] 清单维度 10 缺失 4）----
// [P0-A2] 原 compact.exe bat 改原生 WOF（LZX 紧凑压缩），后台线程递归。
{
    if (CommandLineService::GetPrimaryAction() == EClrAction::ECLR_COMPACTEX)
    {
        CDuiString sTarget;
        if (CommandLineService::GetCompactTarget(sTarget))
        {
            StartWofTreeWorker(m_hWnd, sTarget, WofCompact::Algorithm::AlgLzx,
                               false, L"shell compact(LZX)");
        }
        else
        {
            AppendLog(L"[cli] shell compact: no target path");
        }
        return;
    }
}
// ---- [P0] KB57 基线外壳动词 1：/C /exe <alg> "<path>" 递归 WOF 压缩 ----
// [P0-A2] 原生 FSCTL_SET_EXTERNAL_BACKING（等价 compact /c /exe:<alg> /s）。
{
    CDuiString sTarget;
    EClrCompress eAlg = EClrCompress::ECLR_COMP_NONE;
    if (CommandLineService::GetCompactExeTarget(sTarget, eAlg))
    {
        WofCompact::Algorithm nAlg = WofCompact::Algorithm::AlgXpress4k;
        switch (eAlg)
        {
        case EClrCompress::ECLR_COMP_XPRESS4K:  nAlg = WofCompact::Algorithm::AlgXpress4k;  break;
        case EClrCompress::ECLR_COMP_XPRESS8K:  nAlg = WofCompact::Algorithm::AlgXpress8k;  break;
        case EClrCompress::ECLR_COMP_XPRESS16K: nAlg = WofCompact::Algorithm::AlgXpress16k; break;
        case EClrCompress::ECLR_COMP_LZX:       nAlg = WofCompact::Algorithm::AlgLzx;       break;
        case EClrCompress::ECLR_COMP_MAX:       nAlg = WofCompact::Algorithm::AlgLzx;       break;
        default: break;
        }
        if (static_cast<int>(nAlg) >= 0)
        {
            StartWofTreeWorker(m_hWnd, sTarget, nAlg, false,
                               L"compact-exe");
            return;
        }
    }
}
// ---- [P0] KB57 基线外壳动词 2：/U "<path>" 递归解压 ----
// [P0-A2] 原生 FSCTL_DELETE_EXTERNAL_BACKING（等价 compact /u /s）。
{
    CDuiString sTarget;
    if (CommandLineService::GetUncompactTarget(sTarget))
    {
        StartWofTreeWorker(m_hWnd, sTarget, WofCompact::Algorithm::AlgXpress4k, true, L"uncompact");
        return;
    }
}
}

// [UI-GEO] Dump the laid-out control tree. Lives here because both m_pm and AppendLog
// are members; an earlier attempt to call these from Win8Metro.cpp failed to compile,
// which is the compiler pointing out that the geometry belongs with the window.
void CFrameWnd::ExportControlTree(const wchar_t* pszPath)
{
    // Trace first: if the dump silently does nothing we need to know whether the
    // function was entered at all, which a success-only log cannot tell.
    if (!pszPath || !pszPath[0]) return;

    CControlUI* pRoot = m_pm.GetRoot();
    if (pRoot == nullptr) { AppendLog(L"[geo] root is null"); return; }

    const int nNodes = geomdump::CountNodes(pRoot);
    if (!geomdump::DumpTree(pRoot, pszPath))
    {
        AppendLog(L"[geo] dump FAILED");
        return;
    }
    // One dump at the end only ever captures the LAST selected page: measured 115
    // visible controls out of 10740, so row heights and per-page buttons were invisible.
    // Dumping per page is what makes the numbers usable.
    CTabLayoutUI* pTab = static_cast<CTabLayoutUI*>(m_pm.FindControl(L"content_area"));
    if (pTab == nullptr)
    {
        // No tabs (or not built yet): still emit the one tree we have.
        geomdump::DumpTree(pRoot, pszPath);
        CDuiString sOnly;
        sOnly.Format(L"[geo] single dump controls=%d", nNodes);
        AppendLog(sOnly.GetData());
        return;
    }

    int nDumped = 0;
    for (int i = 0; i < pTab->GetCount(); ++i)
    {
        EnsurePageBuilt(i);
        pTab->SelectItem(i);

        // SelectItem triggers the tab layout, so GetPos() is valid for this page by the
        // time we dump. No manual layout call: CPageBase exposes no root accessor, and
        // DoPaint(nullptr, ...) would be a null DC anyway.
        wchar_t szPage[MAX_PATH] = {};
        wfmt::FormatInto(szPage, _countof(szPage), L"%s.%02d.txt", pszPath, i);
        if (geomdump::DumpTree(pRoot, szPage)) ++nDumped;
    }

    CDuiString sLog;
    sLog.Format(L"[geo] controls=%d pages_dumped=%d", nNodes, nDumped);
    AppendLog(sLog.GetData());
}
