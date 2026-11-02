// ============================================================
// AppxSmartFilter.cpp —— 应用包中文映射（Win10/Win11 分级）+ 智能筛选
//
// 与 AppxDisplayName（FrameWnd_Util.cpp）的关系：
//   后者按 appx.name.<PackageName> 查语言包，是**多语言**通道；
//   本文件是**内嵌中文映射**通道，按系统分级取表，Win11 独有条目
//   在 Win10 上完全不进入索引（既不入表也不进下拉框）。
//   解析顺序见 AppxSmartLookupName：语言包 -> 本文件映射 -> 原始包名。
//
// [R67 国际化口径] 表内中文文案有意保留内嵌：包名映射是 Windows
//   固有名词（"记事本""Xbox 游戏栏"），与可选功能页的能力名同口径。
//   UI 提示语（分类名 / 计数文案）一律走语言包键，见文件末尾键名清单。
// ============================================================

#include "stdafx.h"

#include "AppxSmartFilter.h"

#include "OsVersionInfo.h"   // [R200] Win11 分级门控唯一来源
#include "StringManager.h"   // 分类名 / 计数文案
#include "FrameWnd_Util.h"  // TrimStr / AppxFamily 等共享字符串辅助

#include <algorithm>
#include <map>
#include <memory>   // [C++17 R3] make_unique
#include <string>

namespace
{

// ------------------------------------------------------------
// 映射表条目
//   pszPkg  : PackageName（FullName 首个 '_' 之前的部分）
//   pszName : 展示名
//   nTier   : Base = Win10/11 通用；Win11 = 仅 build>=22000
//   nCat    : 分类
// ------------------------------------------------------------
struct AppxNameRow
{
    const wchar_t* pszPkg;
    const wchar_t* pszName;
    EAppxNameTier  nTier;
    EAppxCategory  nCat;
};

// ---- 通用条目（Win10 / Win11 均生效）----
// 与 lang/*_translations.json 的 appx.name.* 保持同义；新增条目时两边
// 应同步，否则切换语言后名称会在中文映射与译文之间跳变。
const AppxNameRow kRowsBase[] =
{
    // --- 系统组件：外壳与基础设施 ---
    { L"Microsoft.AccountsControl", L"账户控制", EAppxNameTier::Base, EAppxCategory::SystemComp },
    { L"Microsoft.AAD.BrokerPlugin", L"Microsoft 账户代理", EAppxNameTier::Base, EAppxCategory::SystemComp },
    { L"Microsoft.AsyncTextService", L"异步文本服务（输入法框架）", EAppxNameTier::Base, EAppxCategory::SystemComp },
    { L"Microsoft.BioEnrollment", L"生物识别登录", EAppxNameTier::Base, EAppxCategory::SystemComp },
    { L"Microsoft.CredDialogHost", L"凭据对话框宿主", EAppxNameTier::Base, EAppxCategory::SystemComp },
    { L"Microsoft.LockApp", L"锁屏界面", EAppxNameTier::Base, EAppxCategory::SystemComp },
    { L"Microsoft.Windows.ShellExperienceHost", L"开始菜单与外壳", EAppxNameTier::Base, EAppxCategory::SystemComp },
    { L"Microsoft.Windows.StartMenuExperienceHost", L"开始菜单", EAppxNameTier::Base, EAppxCategory::SystemComp },
    { L"Microsoft.Windows.Search", L"Windows 搜索", EAppxNameTier::Base, EAppxCategory::SystemComp },
    { L"Microsoft.Windows.ContentDeliveryManager", L"内容传递管理器（推荐内容缓存）", EAppxNameTier::Base, EAppxCategory::SystemComp },
    { L"Microsoft.Windows.CloudExperienceHost", L"Windows 云体验宿主", EAppxNameTier::Base, EAppxCategory::SystemComp },
    { L"Microsoft.Windows.PeopleExperienceHost", L"人脉体验宿主", EAppxNameTier::Base, EAppxCategory::SystemComp },
    { L"Microsoft.Windows.OOBENetworkCaptivePortal", L"网络登录检测（强制门户）", EAppxNameTier::Base, EAppxCategory::SystemComp },
    { L"Microsoft.Windows.OOBENetworkConnectionFlow", L"网络连接向导", EAppxNameTier::Base, EAppxCategory::SystemComp },
    { L"Microsoft.Windows.PinningConfirmationDialog", L"固定确认对话框", EAppxNameTier::Base, EAppxCategory::SystemComp },
    { L"Microsoft.Windows.AssignedAccessLockApp", L"分配访问锁定（单应用展台）", EAppxNameTier::Base, EAppxCategory::SystemComp },
    { L"Microsoft.Windows.ParentalControls", L"家长控制", EAppxNameTier::Base, EAppxCategory::SystemComp },
    { L"Microsoft.Windows.Apprep.ChxApp", L"应用信誉检查", EAppxNameTier::Base, EAppxCategory::SystemComp },
    { L"Microsoft.Windows.SecHealthUI", L"Windows 安全中心", EAppxNameTier::Base, EAppxCategory::SystemComp },
    { L"Microsoft.SecHealthUI", L"Windows 安全中心", EAppxNameTier::Base, EAppxCategory::SystemComp },
    { L"Microsoft.Windows.SecureAssessmentBrowser", L"安全评估浏览器", EAppxNameTier::Base, EAppxCategory::SystemComp },
    { L"Microsoft.Win32WebViewHost", L"WebView 宿主", EAppxNameTier::Base, EAppxCategory::SystemComp },
    { L"NcsiUwpApp", L"网络连接状态指示", EAppxNameTier::Base, EAppxCategory::SystemComp },
    { L"Windows.CBSPreview", L"组件服务预览（CBS）", EAppxNameTier::Base, EAppxCategory::SystemComp },
    { L"Microsoft.ECApp", L"企业上下文应用", EAppxNameTier::Base, EAppxCategory::SystemComp },
    { L"Microsoft.Services.Store.Engagement", L"商店互动服务", EAppxNameTier::Base, EAppxCategory::SystemComp },
    { L"Microsoft.Windows.XGpuEjectDialog", L"显卡弹出对话框", EAppxNameTier::Base, EAppxCategory::SystemComp },
    { L"windows.immersivecontrolpanel", L"Windows 设置", EAppxNameTier::Base, EAppxCategory::SystemComp },
    { L"Microsoft.Windows.CallingShellApp", L"通话界面（手机通话集成）", EAppxNameTier::Base, EAppxCategory::SystemComp },
    { L"Microsoft.Windows.CapturePicker", L"屏幕捕获选择器", EAppxNameTier::Base, EAppxCategory::SystemComp },
    { L"Microsoft.Windows.NarratorQuickStart", L"讲述人快速入门", EAppxNameTier::Base, EAppxCategory::SystemComp },
    { L"Microsoft.StartExperiencesApp", L"获取帮助提示（首次使用引导）", EAppxNameTier::Base, EAppxCategory::SystemComp },

    // --- 用户应用 ---
    { L"Microsoft.BingWeather", L"天气", EAppxNameTier::Base, EAppxCategory::UserApp },
    { L"Microsoft.BingWeatherBeta", L"天气 Beta", EAppxNameTier::Base, EAppxCategory::UserApp },
    { L"Microsoft.BingNews", L"资讯", EAppxNameTier::Base, EAppxCategory::UserApp },
    { L"Microsoft.BingFinance", L"财经", EAppxNameTier::Base, EAppxCategory::UserApp },
    { L"Microsoft.BingSports", L"体育", EAppxNameTier::Base, EAppxCategory::UserApp },
    { L"Microsoft.BingSearch", L"必应搜索", EAppxNameTier::Base, EAppxCategory::UserApp },
    { L"Microsoft.BingTranslator", L"翻译", EAppxNameTier::Base, EAppxCategory::UserApp },
    { L"Microsoft.WindowsCalculator", L"计算器", EAppxNameTier::Base, EAppxCategory::UserApp },
    { L"Microsoft.WindowsCamera", L"相机", EAppxNameTier::Base, EAppxCategory::UserApp },
    { L"Microsoft.WindowsMaps", L"地图", EAppxNameTier::Base, EAppxCategory::UserApp },
    { L"Microsoft.WindowsAlarms", L"闹钟和时钟", EAppxNameTier::Base, EAppxCategory::UserApp },
    { L"Microsoft.WindowsSoundRecorder", L"录音机", EAppxNameTier::Base, EAppxCategory::UserApp },
    { L"Microsoft.Windows.Photos", L"照片", EAppxNameTier::Base, EAppxCategory::UserApp },
    { L"Microsoft.WindowsFeedbackHub", L"反馈中心", EAppxNameTier::Base, EAppxCategory::UserApp },
    { L"Microsoft.GetHelp", L"获取帮助", EAppxNameTier::Base, EAppxCategory::UserApp },
    { L"Microsoft.Getstarted", L"使用技巧", EAppxNameTier::Base, EAppxCategory::UserApp },
    { L"Microsoft.MicrosoftSolitaireCollection", L"Microsoft Solitaire Collection（纸牌）", EAppxNameTier::Base, EAppxCategory::UserApp },
    { L"Microsoft.MicrosoftStickyNotes", L"便笺", EAppxNameTier::Base, EAppxCategory::UserApp },
    { L"Microsoft.Microsoft3DViewer", L"3D 查看器", EAppxNameTier::Base, EAppxCategory::UserApp },
    { L"Microsoft.MixedReality.Portal", L"混合现实门户", EAppxNameTier::Base, EAppxCategory::UserApp },
    { L"Microsoft.SkypeApp", L"Skype", EAppxNameTier::Base, EAppxCategory::UserApp },
    { L"Microsoft.YourPhone", L"手机连接", EAppxNameTier::Base, EAppxCategory::UserApp },
    { L"Microsoft.People", L"人脉", EAppxNameTier::Base, EAppxCategory::UserApp },
    { L"Microsoft.Wallet", L"钱包", EAppxNameTier::Base, EAppxCategory::UserApp },
    { L"Microsoft.MicrosoftOfficeHub", L"Microsoft Office 中心", EAppxNameTier::Base, EAppxCategory::UserApp },
    { L"Microsoft.Office.OneNote", L"OneNote", EAppxNameTier::Base, EAppxCategory::UserApp },
    { L"Microsoft.Office.Sway", L"Sway", EAppxNameTier::Base, EAppxCategory::UserApp },
    { L"Microsoft.OutlookForWindows", L"新的 Outlook", EAppxNameTier::Base, EAppxCategory::UserApp },
    { L"Microsoft.Teams", L"Microsoft Teams", EAppxNameTier::Base, EAppxCategory::UserApp },
    { L"Microsoft.ZuneMusic", L"媒体播放器", EAppxNameTier::Base, EAppxCategory::UserApp },
    { L"Microsoft.ZuneVideo", L"电影和电视", EAppxNameTier::Base, EAppxCategory::UserApp },
    { L"Microsoft.WindowsCommunicationsApps", L"邮件和日历", EAppxNameTier::Base, EAppxCategory::UserApp },
    { L"Microsoft.windowscommunicationsapps", L"邮件和日历", EAppxNameTier::Base, EAppxCategory::UserApp },
    { L"Microsoft.WindowsStore", L"Microsoft Store（应用商店）", EAppxNameTier::Base, EAppxCategory::UserApp },
    { L"Microsoft.StorePurchaseApp", L"商店购买", EAppxNameTier::Base, EAppxCategory::UserApp },
    { L"Microsoft.WindowsTerminal", L"Windows 终端", EAppxNameTier::Base, EAppxCategory::UserApp },
    { L"Microsoft.WindowsNotepad", L"记事本（新版）", EAppxNameTier::Base, EAppxCategory::UserApp },
    { L"Microsoft.Paint", L"画图（新版）", EAppxNameTier::Base, EAppxCategory::UserApp },
    { L"Microsoft.ScreenSketch", L"截图工具（新版）", EAppxNameTier::Base, EAppxCategory::UserApp },
    { L"Microsoft.DesktopAppInstaller", L"应用安装程序（winget）", EAppxNameTier::Base, EAppxCategory::UserApp },
    { L"Microsoft.GamingApp", L"Xbox 应用", EAppxNameTier::Base, EAppxCategory::UserApp },
    { L"Microsoft.XboxApp", L"Xbox（旧版主机伴侣）", EAppxNameTier::Base, EAppxCategory::UserApp },
    { L"Microsoft.XboxGamingOverlay", L"Xbox 游戏栏", EAppxNameTier::Base, EAppxCategory::UserApp },
    { L"Microsoft.XboxGameOverlay", L"Xbox 游戏浮层", EAppxNameTier::Base, EAppxCategory::UserApp },
    { L"Microsoft.Xbox.TCUI", L"Xbox 界面组件（TCUI）", EAppxNameTier::Base, EAppxCategory::UserApp },
    { L"Microsoft.XboxGameCallableUI", L"Xbox 游戏界面", EAppxNameTier::Base, EAppxCategory::UserApp },
    { L"Microsoft.XboxIdentityProvider", L"Xbox 身份提供程序", EAppxNameTier::Base, EAppxCategory::UserApp },
    { L"Microsoft.XboxSpeechToTextOverlay", L"Xbox 语音转文本", EAppxNameTier::Base, EAppxCategory::UserApp },
    { L"Microsoft.MicrosoftEdge.Stable", L"Microsoft Edge", EAppxNameTier::Base, EAppxCategory::UserApp },
    { L"Microsoft.MicrosoftEdgeDevToolsClient", L"Edge 开发者工具", EAppxNameTier::Base, EAppxCategory::UserApp },
    { L"Microsoft.MicrosoftPCManager", L"电脑管家（微软）", EAppxNameTier::Base, EAppxCategory::UserApp },
    { L"Microsoft.WorldNationalParks", L"世界国家公园（内容包）", EAppxNameTier::Base, EAppxCategory::UserApp },
    { L"Microsoft.StarlightCourier", L"星光快递（内容包）", EAppxNameTier::Base, EAppxCategory::UserApp },
    { L"NVIDIACorp.NVIDIAControlPanel", L"NVIDIA 控制面板", EAppxNameTier::Base, EAppxCategory::UserApp },
    { L"PythonSoftwareFoundation.Python.3.13", L"Python 3.13（商店版）", EAppxNameTier::Base, EAppxCategory::UserApp },

    // --- 运行时框架 ---
    { L"Microsoft.NET.Native.Framework.1.7", L".NET Native 框架 1.7", EAppxNameTier::Base, EAppxCategory::Runtime },
    { L"Microsoft.NET.Native.Framework.2.2", L".NET Native 框架 2.2", EAppxNameTier::Base, EAppxCategory::Runtime },
    { L"Microsoft.NET.Native.Runtime.1.7", L".NET Native 运行时 1.7", EAppxNameTier::Base, EAppxCategory::Runtime },
    { L"Microsoft.NET.Native.Runtime.2.2", L".NET Native 运行时 2.2", EAppxNameTier::Base, EAppxCategory::Runtime },
    { L"Microsoft.VCLibs.140.00", L"Visual C++ 运行库 14.0（UWP）", EAppxNameTier::Base, EAppxCategory::Runtime },
    { L"Microsoft.VCLibs.140.00.UWPDesktop", L"Visual C++ 桌面扩展 14.0（UWP）", EAppxNameTier::Base, EAppxCategory::Runtime },
    { L"Microsoft.UI.Xaml.2.3", L"UI.Xaml 2.3 运行时", EAppxNameTier::Base, EAppxCategory::Runtime },
    { L"Microsoft.UI.Xaml.2.4", L"UI.Xaml 2.4 运行时", EAppxNameTier::Base, EAppxCategory::Runtime },
    { L"Microsoft.UI.Xaml.2.7", L"UI.Xaml 2.7 运行时", EAppxNameTier::Base, EAppxCategory::Runtime },
    { L"Microsoft.UI.Xaml.2.8", L"UI.Xaml 2.8 运行时", EAppxNameTier::Base, EAppxCategory::Runtime },
    { L"Microsoft.WindowsAppRuntime.1.4", L"Windows App 运行时 1.4", EAppxNameTier::Base, EAppxCategory::Runtime },
    { L"Microsoft.WindowsAppRuntime.1.7", L"Windows App 运行时 1.7", EAppxNameTier::Base, EAppxCategory::Runtime },
    { L"Microsoft.WindowsAppRuntime.1.8", L"Windows App 运行时 1.8", EAppxNameTier::Base, EAppxCategory::Runtime },
    { L"Microsoft.WindowsAppRuntime.2", L"Windows App 运行时 2", EAppxNameTier::Base, EAppxCategory::Runtime },
    { L"MicrosoftCorporationII.WinAppRuntime.Main.1.4", L"WinAppRuntime 主体 1.4", EAppxNameTier::Base, EAppxCategory::Runtime },
    { L"MicrosoftCorporationII.WinAppRuntime.Singleton", L"WinAppRuntime 单例", EAppxNameTier::Base, EAppxCategory::Runtime },
    { L"Microsoft.AV1VideoExtension", L"AV1 视频扩展（解码器）", EAppxNameTier::Base, EAppxCategory::Runtime },
    { L"Microsoft.HEIFImageExtension", L"HEIF 图像扩展", EAppxNameTier::Base, EAppxCategory::Runtime },
    { L"Microsoft.HEVCVideoExtension", L"HEVC 视频扩展（解码器）", EAppxNameTier::Base, EAppxCategory::Runtime },
    { L"Microsoft.RawImageExtension", L"RAW 图像扩展", EAppxNameTier::Base, EAppxCategory::Runtime },
    { L"Microsoft.WebpImageExtension", L"Webp 图像扩展", EAppxNameTier::Base, EAppxCategory::Runtime },
    { L"Microsoft.WebMediaExtensions", L"Web 媒体扩展", EAppxNameTier::Base, EAppxCategory::Runtime },
    { L"Microsoft.VP9VideoExtensions", L"VP9 视频扩展（解码器）", EAppxNameTier::Base, EAppxCategory::Runtime },
    { L"Microsoft.Winget.Source", L"WinGet 源", EAppxNameTier::Base, EAppxCategory::Runtime },
    { L"Microsoft.MSIXCore", L"MSIX 核心组件", EAppxNameTier::Base, EAppxCategory::Runtime },
    { L"Microsoft.MSIXPackagingTool", L"MSIX 打包工具", EAppxNameTier::Base, EAppxCategory::Runtime },
    { L"MicrosoftCorporationII.WindowsSubsystemForLinux", L"适用于 Linux 的 Windows 子系统（WSL）", EAppxNameTier::Base, EAppxCategory::Runtime },
    { L"Microsoft.DotNet.NativeRuntime", L".NET 原生运行时", EAppxNameTier::Base, EAppxCategory::Runtime },

    // --- 语言包 / 资源包（其余语言由基名规则兜底）---
    { L"Microsoft.LanguageExperiencePackzh-CN", L"中文（简体）语言体验包", EAppxNameTier::Base, EAppxCategory::LangPack },
    { L"Microsoft.LanguageExperiencePacken-US", L"英语（美国）语言体验包", EAppxNameTier::Base, EAppxCategory::LangPack },
};

// ---- Windows 11 独有条目（build >= 22000 才生效）----
// 为什么单独一张表：这些包在 Win10 上要么根本不存在，要么是另一套
// 名称/分类。放在同一数组里用 nTier 标记，再由 AppxMapTierApplies()
// 在建索引时整体剔除 —— 索引里没有，下拉框与列表也就无从命中。
const AppxNameRow kRowsWin11[] =
{
    { L"MicrosoftWindows.Client.CBS", L"Windows 核心组件（Win11 外壳）", EAppxNameTier::Win11, EAppxCategory::SystemComp },
    { L"MicrosoftWindows.Client.WebExperience", L"Windows 小组件", EAppxNameTier::Win11, EAppxCategory::SystemComp },
    { L"MicrosoftWindows.Client.Core", L"Windows 外壳核心组件", EAppxNameTier::Win11, EAppxCategory::SystemComp },
    { L"MicrosoftWindows.Client.FileExp", L"资源管理器扩展组件", EAppxNameTier::Win11, EAppxCategory::SystemComp },
    { L"MicrosoftWindows.Client.OOBE", L"开箱体验（OOBE）", EAppxNameTier::Win11, EAppxCategory::SystemComp },
    { L"MicrosoftWindows.Client.Photon", L"Photon 外壳组件", EAppxNameTier::Win11, EAppxCategory::SystemComp },
    { L"MicrosoftWindows.Client.AIX", L"AI 体验组件", EAppxNameTier::Win11, EAppxCategory::SystemComp },
    { L"MicrosoftWindows.CrossDevice", L"跨设备体验", EAppxNameTier::Win11, EAppxCategory::SystemComp },
    { L"Microsoft.Windows.CrossDevice", L"跨设备体验", EAppxNameTier::Win11, EAppxCategory::SystemComp },
    { L"Microsoft.Windows.Shell.SurfaceHub", L"Surface Hub 外壳组件", EAppxNameTier::Win11, EAppxCategory::SystemComp },
    { L"Microsoft.Copilot", L"Windows Copilot（AI 助手）", EAppxNameTier::Win11, EAppxCategory::SystemComp },
    { L"Microsoft.Windows.Ai.Copilot.Provider", L"Copilot 提供程序", EAppxNameTier::Win11, EAppxCategory::SystemComp },
    { L"Microsoft.WindowsAI.Copilot.Runtime", L"Windows AI 运行时（Phi 模型）", EAppxNameTier::Win11, EAppxCategory::Runtime },
    { L"MicrosoftWindows.UndockedDevKit", L"移动设备套件（对接工作站）", EAppxNameTier::Win11, EAppxCategory::SystemComp },
    { L"Microsoft.Windows.Widgets", L"小组件", EAppxNameTier::Win11, EAppxCategory::SystemComp },
    { L"MicrosoftWindows.Client.Widgets", L"小组件面板", EAppxNameTier::Win11, EAppxCategory::SystemComp },
    { L"Microsoft.WindowsAppRuntime.1.5", L"Windows App 运行时 1.5", EAppxNameTier::Win11, EAppxCategory::Runtime },
    { L"Microsoft.WindowsAppRuntime.1.6", L"Windows App 运行时 1.6", EAppxNameTier::Win11, EAppxCategory::Runtime },
    { L"Microsoft.UI.Xaml.2.5", L"UI.Xaml 2.5 运行时", EAppxNameTier::Win11, EAppxCategory::Runtime },
    { L"Microsoft.UI.Xaml.2.6", L"UI.Xaml 2.6 运行时", EAppxNameTier::Win11, EAppxCategory::Runtime },
    { L"Microsoft.UI.Xaml.CBS", L"UI.Xaml 组件包", EAppxNameTier::Win11, EAppxCategory::Runtime },
    { L"Microsoft.Windows.DevHome", L"开发主控台（Dev Home）", EAppxNameTier::Win11, EAppxCategory::UserApp },
    { L"Microsoft.Windows.Terminal.Preview", L"Windows 终端预览版", EAppxNameTier::Win11, EAppxCategory::UserApp },
    { L"Microsoft.WindowsNotepad.Search", L"记事本（搜索附页）", EAppxNameTier::Win11, EAppxCategory::SystemComp },
    { L"Microsoft.Windows.SecHealthUI.Narrator", L"安全中心（讲述人资源）", EAppxNameTier::Win11, EAppxCategory::SystemComp },
    { L"Clipchamp.Clipchamp", L"Clipchamp（视频编辑器）", EAppxNameTier::Win11, EAppxCategory::UserApp },
    { L"MicrosoftWindows.LKG.Accounts", L"账户组件（LKG）", EAppxNameTier::Win11, EAppxCategory::SystemComp },
    { L"MicrosoftWindows.LKG.TwinSxS", L"组件存储（LKG）", EAppxNameTier::Win11, EAppxCategory::SystemComp },
    { L"Microsoft.Windows.Photos.DLC.MediaCore", L"照片（媒体核心扩展）", EAppxNameTier::Win11, EAppxCategory::Runtime },
    { L"Microsoft.Windows.Photos.DLC.VideoExtension", L"照片（视频编辑扩展）", EAppxNameTier::Win11, EAppxCategory::Runtime },
    { L"Microsoft.Windows.ApplicationModel.Background.UniversalBGTask", L"通用后台任务宿主", EAppxNameTier::Win11, EAppxCategory::Runtime },
    { L"Microsoft.Windows.Notepad.System", L"记事本（系统组件）", EAppxNameTier::Win11, EAppxCategory::SystemComp },
};

// ------------------------------------------------------------
// 分级门控：**唯一**判定点。
//   Windows 11（build>=22000，含将来版本 —— 阈值只来自
//   OsVersionInfo.h 的 kWin11Build21H2，这里不自造第二套数字）：
//   启用全部 Win11 条目；Win10 及更早：整表不加载。
// ------------------------------------------------------------
[[nodiscard]] bool AppxMapTierApplies(EAppxNameTier nTier)
{
    if (nTier != EAppxNameTier::Win11) return true;
    return osver::IsWindows11OrLater();
}

// PackageName = FullName 首个 '_' 之前的部分（与 AppxDisplayName 同口径）。
[[nodiscard]] CDuiString AppxPackageName(const CDuiString& sFull)
{
    const int nCut = sFull.Find(L'_');
    return (nCut > 0) ? sFull.Mid(0, nCut) : sFull;
}

// [R16][O-3] 同款索引：基名小写 ASCII 键 → 表项，一次构建 O(n log n) 查找。
std::wstring AppxLowerKey(const wchar_t* psz)
{
    std::wstring s;
    for (; psz && *psz; ++psz)
        s += static_cast<wchar_t>(::towlower(static_cast<wint_t>(*psz)));
    return s;
}

using AppxRowIndex = std::map<std::wstring, const AppxNameRow*>;

[[nodiscard]] const AppxRowIndex& AppxNameIndex()
{
    // magic static：C++11 起线程安全；枚举线程与 UI 线程并发首调也只建一次。
    static const AppxRowIndex s_idx = []
    {
        AppxRowIndex m;
        int nActive = 0;
        auto addRows = [&m, &nActive](const AppxNameRow* pRows, size_t nCount)
        {
            for (size_t i = 0; i < nCount; ++i)
            {
                if (!AppxMapTierApplies(pRows[i].nTier)) continue;   // Win10 上整条剔除
                m[AppxLowerKey(pRows[i].pszPkg)] = &pRows[i];
                ++nActive;
            }
        };
        addRows(kRowsBase, sizeof(kRowsBase) / sizeof(kRowsBase[0]));
        addRows(kRowsWin11, sizeof(kRowsWin11) / sizeof(kRowsWin11[0]));
        // 分级生效条数写一次调试输出：Win10 与 Win11 上这个数不同，
        // 排查"为什么某个包没有中文名"时先看这一行。
        std::wstring sLog = L"[appx-map] tier=";
        sLog += osver::IsWindows11OrLater() ? L"win11" : L"win10";
        sLog += L" active=";
        sLog += std::to_wstring(nActive);
        sLog += L"\r\n";
        ::OutputDebugStringW(sLog.c_str());
        return m;
    }();
    return s_idx;
}

// ------------------------------------------------------------
// 同名合并：去掉名称尾部的版本/Beta 等修饰后作为"合并键"。
//   动机：UI.Xaml 2.3/2.4/2.7/2.8 四条在列表里各占一行，用户看到的是
//   同一个组件被列了四遍。合并只作用于**展示名**，包全名与勾选状态
//   各自保留 —— 删除仍然按包精确下发，不会误删。
// ------------------------------------------------------------
[[nodiscard]] CDuiString AppxMergeKey(const CDuiString& sName)
{
    CDuiString s = sName;
    // 版本号尾部：只剥离"空格 + 纯数字点串"（如 " 2.3" / " 14.0"）。
    //   必须要求被剥离段是纯 数字/. 且以数字开头，否则会把
    //   ".NET Native 框架" 里的"架"当成数字串一起吃掉（会真正发生）。
    for (;;)
    {
        const int n = s.GetLength();
        int i = n - 1;
        while (i >= 0 && (s[i] == L'.' || (s[i] >= L'0' && s[i] <= L'9'))) --i;
        const bool bNumTail = (i < n - 1) && s[i] == L' '
                           && (s[i + 1] >= L'0' && s[i + 1] <= L'9')
                           && s.Mid(i + 1).Find(L'.') >= 0;
        if (!bNumTail) break;
        s = s.Mid(0, i);
        s = TrimStr(s);
    }
    // 常见修饰语（只影响展示，不改变分类与勾选）
    static const wchar_t* kSuffixes[] = { L" Beta", L"（新版）", L"（旧版）", L"（系统组件）", L" 预览版" };
    for (const wchar_t* psz : kSuffixes)
    {
        const size_t nLen = wcslen(psz);
        const int n = s.GetLength();
        if (n < static_cast<int>(nLen)) continue;
        if (wcsncmp(s.GetData() + n - nLen, psz, static_cast<int>(nLen) + 1) == 0)
        {
            s = s.Mid(0, n - static_cast<int>(nLen));
            s = TrimStr(s);
        }
    }
    return s;
}

// 表外兜底：按包名前缀识别常见家族，避免整列表都是生包名。
// 只产出分类，不编造名称（名称仍回退原始包名）。
[[nodiscard]] bool AppxFamilyClassify(const CDuiString& sPkg, const CDuiString& sFull,
                                      EAppxCategory& outCat)
{
    if (sFull.Find(L"~W~~~") >= 0) { outCat = EAppxCategory::LangPack; return true; }
    if (wcsncmp(sPkg.GetData(), L"Language.", 9) == 0) { outCat = EAppxCategory::LangPack; return true; }
    if (wcsncmp(sPkg.GetData(), L"Microsoft.LanguageExperiencePack", 32) == 0)
    { outCat = EAppxCategory::LangPack; return true; }

    static const wchar_t* kRuntimePrefixes[] = {
        L"Microsoft.NET.Native.", L"Microsoft.VCLibs.", L"Microsoft.UI.Xaml.",
        L"Microsoft.WindowsAppRuntime.", L"MicrosoftCorporationII.WinAppRuntime.",
        L"Microsoft.Windows.CsWinRT", L"Microsoft.Windows.SDK.NET",
    };
    for (const wchar_t* psz : kRuntimePrefixes)
    {
        const size_t nLen = wcslen(psz);
        if (wcsncmp(sPkg.GetData(), psz, static_cast<int>(nLen)) == 0)
        { outCat = EAppxCategory::Runtime; return true; }
    }

    static const wchar_t* kSystemPrefixes[] = {
        L"Microsoft.Windows.Shell", L"MicrosoftWindows.Client.",
        L"Microsoft.Windows.StartMenu", L"Microsoft.Windows.Search",
        L"Microsoft.Windows.CloudExperience", L"Microsoft.Windows.OOBE",
    };
    for (const wchar_t* psz : kSystemPrefixes)
    {
        const size_t nLen = wcslen(psz);
        if (wcsncmp(sPkg.GetData(), psz, static_cast<int>(nLen)) == 0)
        { outCat = EAppxCategory::SystemComp; return true; }
    }

    // 纯 GUID 包名（形如 1527c705-839a-4832-…）：系统注册的 COM/代理占位
    if (sPkg.GetLength() >= 36 && sPkg.Find(L'-') > 0 && sPkg.Find(L'.') < 0)
    { outCat = EAppxCategory::SystemComp; return true; }

    return false;
}

}   // namespace

// ============================================================
// 对外接口
// ============================================================

bool AppxSmartLookupName(const CDuiString& sFull, CDuiString& outName, EAppxNameTier* pTier)
{
    outName.Empty();
    if (pTier) *pTier = EAppxNameTier::Unknown;
    if (sFull.IsEmpty()) return false;

    const CDuiString sPkg = AppxPackageName(sFull);
    const AppxRowIndex& idx = AppxNameIndex();
    const AppxRowIndex::const_iterator it = idx.find(AppxLowerKey(sPkg.GetData()));
    if (it == idx.end()) return false;

    outName = it->second->pszName;
    if (pTier) *pTier = it->second->nTier;
    return true;
}

EAppxCategory AppxSmartClassify(const CDuiString& sFull, bool bIsResourceFlag)
{
    if (bIsResourceFlag) return EAppxCategory::LangPack;
    const CDuiString sPkg = AppxPackageName(sFull);
    const AppxRowIndex& idx = AppxNameIndex();
    const AppxRowIndex::const_iterator it = idx.find(AppxLowerKey(sPkg.GetData()));
    if (it != idx.end()) return it->second->nCat;

    EAppxCategory nCat = EAppxCategory::UserApp;
    if (AppxFamilyClassify(sPkg, sFull, nCat)) return nCat;
    return nCat;
}

AppxSmartInfo AppxSmartDescribe(const CDuiString& sFull, bool bIsResourceFlag)
{
    AppxSmartInfo info;
    EAppxNameTier nTier = EAppxNameTier::Unknown;
    CDuiString sMapped;
    if (AppxSmartLookupName(sFull, sMapped, &nTier))
    {
        info.sName = AppxMergeKey(sMapped);
        info.nTier = nTier;
    }
    info.bResourcePkg = bIsResourceFlag || sFull.Find(L"~W~~~") >= 0;
    info.nCat = AppxSmartClassify(sFull, info.bResourcePkg);
    info.sCategory = AppxSmartCategoryText(info.nCat);
    return info;
}

const wchar_t* AppxSmartCategoryText(EAppxCategory nCat)
{
    switch (nCat)
    {
    case EAppxCategory::UserApp:    return StringMgr.Get("appx.smart.cat_user");
    case EAppxCategory::SystemComp: return StringMgr.Get("appx.smart.cat_system");
    case EAppxCategory::Runtime:    return StringMgr.Get("appx.smart.cat_runtime");
    case EAppxCategory::LangPack:   return StringMgr.Get("appx.smart.cat_lang");
    case EAppxCategory::All:
    default:                        return StringMgr.Get("appx.smart.cat_all");
    }
}

void AppxSmartFillCategoryCombo(CComboUI* pCombo)
{
    if (!pCombo) return;
    if (pCombo->GetCount() > 0) return;   // 幂等：重复进页不重复追加
    static constexpr EAppxCategory kCats[] = {
        EAppxCategory::All, EAppxCategory::UserApp,
        EAppxCategory::SystemComp, EAppxCategory::Runtime, EAppxCategory::LangPack,
    };
    for (const EAppxCategory nCat : kCats)
    {
        auto pItem = std::make_unique<CListLabelElementUI>();
        pItem->SetText(AppxSmartCategoryText(nCat));
        pItem->SetFixedHeight(24);
        pCombo->Add(pItem.release());
    }
    pCombo->SelectItem(0);
}

bool AppxSmartMatches(const CDuiString& sFull, const AppxSmartInfo& info,
                      const CDuiString& sKwLower, EAppxCategory nCat)
{
    if (nCat != EAppxCategory::All && info.nCat != nCat) return false;
    if (sKwLower.IsEmpty()) return true;

    // 关键词命中面：中文名 / 原始包名 / 分类名。三类都要能搜到，
    // 否则用户搜"运行时"却搜不到任何条目（名称里没有这三个字）。
    CDuiString sName = info.sName;
    sName.MakeLower();
    if (!sName.IsEmpty() && wcsstr(sName.GetData(), sKwLower.GetData())) return true;

    CDuiString sFullLow = sFull;
    sFullLow.MakeLower();
    if (wcsstr(sFullLow.GetData(), sKwLower.GetData())) return true;

    CDuiString sCat = info.sCategory;
    sCat.MakeLower();
    return !sCat.IsEmpty() && wcsstr(sCat.GetData(), sKwLower.GetData()) != nullptr;
}

int AppxSmartActiveEntryCount()
{
    return static_cast<int>(AppxNameIndex().size());
}

// ------------------------------------------------------------
// 语言包键名清单（appx.smart.*，11 种语言均已登记）：
//   appx.smart.cat_all       全部
//   appx.smart.cat_user      用户应用
//   appx.smart.cat_system    系统组件
//   appx.smart.cat_runtime   运行时框架
//   appx.smart.cat_lang      语言资源包
//   appx.smart.s_filtered    显示 %d / %d 项（已筛选）
//   appx.smart.s_osgate_off  Windows 10 已关闭 Windows 11 专属映射
//   appx.smart.s_osgate_on   已加载 Windows 11 专属映射（%d 项）
// ------------------------------------------------------------
