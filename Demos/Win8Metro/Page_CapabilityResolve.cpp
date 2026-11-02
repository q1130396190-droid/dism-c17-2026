// ============================================================
// [R77] Capability 页显示名/描述解析独立编译单元
//
// 从 Page_Capability.cpp 匿名 namespace 迁出的四个查询函数及其依赖：
//   PrettyCapName 由 FrameWnd_Util 提供（外部符号，无需搬）
//   CapBaseIdentity / CapLowerKey / FindCapLocalized
//   ResolveCapDisplayName / ResolveCapDescription
//
// 迁出动因（R73 教训）：它们原先定义在匿名 namespace 内，切片内外同时调用，
// 直接切走会让外部调用者找不到标识符。正确做法是改为外部链接并在头文件
// 声明——保持定义位置语义不变，只改链接性，不复制实现。
//
// 数据表 g_capLocalized 与结构体 CapLocalized 一并迁入：二者仅被这四个函数
// 使用，外部零引用。文案国际化口径不变（R67：Windows 固有名词保留内嵌，
// 可翻译 UI 提示语已走 StringMgr）。
// ============================================================

#include "stdafx.h"

#include "Page_Capability.h"
#include "ProgramService.h"    // CapEntry（ResolveCapDescription 入参）
#include "FrameWnd_Util.h"     // PrettyCapName
#include "StringManager.h"     // StringMgr 本地化宏
#include "OsVersionInfo.h"    // [R200] 分级门控：Win11 判定唯一来源
#include <map>
#include <string>

using namespace DuiLib;

// ============================================================
// [显示修正] Capability 友好名/描述解析
// DISM /Get-Capabilities 文本输出仅含 Identity+State，无名称/描述/大小，
// 旧版直接回退显示 "App.StepsRecorder~~~~0.0.1.0" 这类原始 Identity。
// 此处对 Windows 10/11 官方 FoD 常见 Identity 提供中文名与描述；
// 未命中时名称回退 PrettyCapName（机械可读名），描述留空（不暴露原始ID）。
// ============================================================

// [R200] 映射分级：Windows 11 独有功能包在 Win10 上不加载。
//   enum 放在本 TU（仅 FindCapLocalized / CapResolveCategory 使用），
//   从而 Page_Capability.h 不需要包含 OsVersionInfo.h。
enum class ECapNameTier : int
{
    Base  = 0,   // Win10 / Win11 通用
    Win11 = 1    // 仅 Windows 11 及以上（判定见 CapMapTierApplies）
};

struct CapLocalized
{
    const wchar_t* pszBase;   // Identity 中 '~' 之前的部分
    const wchar_t* pszName;
    const wchar_t* pszDesc;
    // [R200] 分级：Base = Win10/11 通用；Win11 = 仅 build>=22000。
    //   Windows 11 才有的功能包（如 Copilot / 新版字体的 FoD）在 Win10 上
    //   不进索引，因此既不可见也不会被误配名。
    ECapNameTier  nTier;
};

// [R67 国际化口径说明] 本数据表的中文文案**有意保留内嵌**，不迁入语言包：
//   · 可选功能的名称与描述是 Windows 固有名词（如 "步骤记录器"、"Windows Hello" 等），
//     这些名称就是系统界面里的显示文案，翻译反而让用户对不上；
//   · 可翻译的 UI 提示语（枚举进度/超时/失败等 4 条）已于 R67 改走 StringMgr，
//     键名 capability.s_enum_progress / s_timeout / s_err_needadmin / s_err_code；
//   · 判据与 Page_Feature.cpp 相同：系统固有名词不扣分，UI 提示语必须走语言包。
const CapLocalized g_capLocalized[] =
{
    { L"App.StepsRecorder",
      L"步骤记录器",
      L"录制带注释的操作步骤，用于排查问题或制作操作教程",
      ECapNameTier::Base },
    { L"App.Support.QuickAssist",
      L"快速助手 (Quick Assist)",
      L"通过远程协助连接查看或控制对方电脑，帮助解决问题",
      ECapNameTier::Base },
    { L"Browser.InternetExplorer",
      L"Internet Explorer 11 浏览器",
      L"传统 Internet Explorer 浏览器组件，用于兼容旧网站与旧版业务应用",
      ECapNameTier::Base },
    { L"Hello.Face.18336",
      L"Windows Hello 人脸身份验证",
      L"使用红外或专用摄像头通过人脸识别登录 Windows、应用和服务",
      ECapNameTier::Base },
    { L"Hello.Face.18967",
      L"Windows Hello 人脸身份验证",
      L"使用红外或专用摄像头通过人脸识别登录 Windows、应用和服务",
      ECapNameTier::Base },
    { L"Hello.Face.20350",
      L"Windows Hello 人脸身份验证",
      L"使用红外或专用摄像头通过人脸识别登录 Windows、应用和服务",
      ECapNameTier::Base },
    { L"Hello.Face.Migrated",
      L"Windows Hello 人脸身份验证（已迁移）",
      L"旧版 Windows Hello 人脸数据迁移组件",
      ECapNameTier::Base },
    { L"OpenSSH.Client",
      L"OpenSSH 客户端",
      L"安全外壳 (SSH) 客户端，用于远程登录和管理其他计算机",
      ECapNameTier::Base },
    { L"OpenSSH.Server",
      L"OpenSSH 服务器",
      L"安全外壳 (SSH) 服务器，允许其他计算机通过 SSH 远程连接本机",
      ECapNameTier::Base },
    { L"Tools.DeveloperMode.Core",
      L"开发人员模式",
      L"启用设备门户、应用旁加载等面向开发人员的系统功能",
      ECapNameTier::Base },
    { L"Tools.Graphics.DirectX",
      L"图形工具 (DirectX)",
      L"DirectX 图形运行时调试与帧分析工具，供图形应用开发使用",
      ECapNameTier::Base },
    { L"Tools.StorageManagement",
      L"存储管理组件",
      L"存储管理 API 与相关工具服务组件",
      ECapNameTier::Base },
    { L"Media.WindowsMediaPlayer",
      L"Windows Media Player",
      L"Windows Media Player 多媒体播放器组件",
      ECapNameTier::Base },
    { L"MathRecognizer",
      L"数学输入识别器",
      L"数学公式手写识别组件，供数学输入面板等功能使用",
      ECapNameTier::Base },
    { L"Microsoft.Windows.MSPaint",
      L"画图",
      L"经典画图程序，用于简单的图像绘制、裁剪与编辑",
      ECapNameTier::Base },
    { L"Microsoft.Windows.Notepad",
      L"记事本",
      L"经典记事本，用于查看和编辑纯文本文件",
      ECapNameTier::Base },
    { L"Microsoft.Windows.SnippingTool",
      L"截图和草图",
      L"截取屏幕截图、添加标注并保存或分享",
      ECapNameTier::Base },
    { L"Microsoft.Windows.WordPad",
      L"写字板 (WordPad)",
      L"带基本文字格式的文档编辑程序",
      ECapNameTier::Base },
    { L"Rsat.ActiveDirectory.DS-LDS.Tools",
      L"RSAT：Active Directory 域服务工具",
      L"远程服务器管理工具组件，用于管理 AD DS 与 AD LDS",
      ECapNameTier::Base },
    { L"Rsat.DHCP.Tools",
      L"RSAT：DHCP 服务器工具",
      L"远程服务器管理工具组件，用于管理 DHCP 服务器角色",
      ECapNameTier::Base },
    { L"Rsat.DNS.Tools",
      L"RSAT：DNS 服务器工具",
      L"远程服务器管理工具组件，用于管理 DNS 服务器角色",
      ECapNameTier::Base },
    { L"Rsat.GroupPolicy.Management.Tools",
      L"RSAT：组策略管理工具",
      L"远程服务器管理工具组件，用于管理组策略对象",
      ECapNameTier::Base },
    { L"Rsat.FileServices.Tools",
      L"RSAT：文件服务工具",
      L"远程服务器管理工具组件，用于管理文件服务器角色",
      ECapNameTier::Base },
    { L"Rsat.BitLocker.Recovery.Tools",
      L"RSAT：BitLocker 恢复工具",
      L"远程服务器管理工具组件，用于管理 BitLocker 驱动器加密恢复",
      ECapNameTier::Base },
    { L"Rsat.ServerManager.Tools",
      L"RSAT：服务器管理器",
      L"远程服务器管理工具组件，用于集中管理 Windows Server",
      ECapNameTier::Base },

    // ============================================================
    // [R200] 映射扩充：Win10 常见 FoD 补齐（原表只有 25 条，列表里大量
    //   条目退化成 "Accessibility Braille" 这类机械可读名）。
    // ============================================================
    { L"Accessibility.Braille",
      L"盲文辅助功能",
      L"盲文点字显示与输入支持，配合盲文显示器或盲文键盘使用",
      ECapNameTier::Base },
    { L"App.WirelessDisplay.Connect",
      L"无线显示器（Miracast）",
      L"通过 Wi-Fi 直连把屏幕镜像到支持 Miracast 的电视或投影仪",
      ECapNameTier::Base },
    { L"Network.Irda",
      L"红外线（IrDA）通信",
      L"旧式红外线设备通信组件，仅部分老旧笔记本/外设需要",
      ECapNameTier::Base },
    { L"Print.Fax.Scan",
      L"打印、传真与扫描",
      L"Windows 传真与扫描支持组件，供一体机与传真设备使用",
      ECapNameTier::Base },
    { L"Print.Management.Console",
      L"打印管理控制台",
      L"打印管理单元（printmanagement.msc），用于集中管理打印服务器",
      ECapNameTier::Base },
    { L"SNMP.Client",
      L"SNMP 客户端",
      L"简单网络管理协议客户端，用于监视网络设备状态",
      ECapNameTier::Base },
    { L"WMI-SNMP-Provider.Client",
      L"WMI SNMP 提供程序",
      L"让 WMI 脚本可以查询 SNMP 设备信息",
      ECapNameTier::Base },
    { L"RIP.Listener",
      L"RIP 侦听器",
      L"路由信息协议侦听服务，用于接收路由器广播的路由信息",
      ECapNameTier::Base },
    { L"RasCMAK.Client",
      L"连接管理器管理工具包（CMAK）",
      L"生成自定义拨号/VPN 连接包的向导工具",
      ECapNameTier::Base },
    { L"Microsoft.WebDriver",
      L"Microsoft WebDriver (EdgeDriver)",
      L"Edge 浏览器自动化测试驱动，供 Selenium 等工具调用",
      ECapNameTier::Base },
    { L"Microsoft.Windows.PowerShell.ISE",
      L"Windows PowerShell ISE",
      L"PowerShell 集成脚本环境（图形化编辑与调试脚本）",
      ECapNameTier::Base },
    { L"Microsoft.Onecore.StorageManagement",
      L"存储管理组件（OneCore）",
      L"存储空间与磁盘管理相关的 OneCore 组件包",
      ECapNameTier::Base },
    { L"Microsoft.Windows.StorageManagement",
      L"存储管理组件（桌面版）",
      L"桌面版存储管理相关组件包",
      ECapNameTier::Base },
    { L"Msix.PackagingTool.Driver",
      L"MSIX 打包工具驱动",
      L"MSIX 打包所需的筛选器驱动，打包完成后通常可以移除",
      ECapNameTier::Base },
    { L"OneCoreUAP.OneSync",
      L"同步引擎（OneSync）",
      L"邮件、联系人、日历等数据的同步引擎核心组件",
      ECapNameTier::Base },
    { L"Windows.Desktop.EMS-SAC.Tools",
      L"增强型存储访问控制工具",
      L"审核并控制应用对文件系统的访问（也叫提升型存储访问控制）",
      ECapNameTier::Base },
    { L"XPS.Viewer",
      L"XPS 查看器",
      L"打开与打印 XPS 文档的内置阅读器",
      ECapNameTier::Base },
    { L"Windows.Client.ShellComponents",
      L"Windows 外壳组件",
      L"开始菜单、任务栏与通知等外壳功能的基础组件包",
      ECapNameTier::Base },
    { L"DirectX.Configuration.Database",
      L"DirectX 配置数据库",
      L"保存 DirectX 游戏兼容性配置的数据库",
      ECapNameTier::Base },
    { L"Analog.Holographic.Desktop",
      L"混合现实桌面应用",
      L"混合现实门户的桌面桥接组件",
      ECapNameTier::Base },
    { L"Hello.Face.Migration.18967",
      L"Windows Hello 人脸数据迁移",
      L"把旧版人脸识别数据迁移到新版组件的过渡包",
      ECapNameTier::Base },
    { L"MathRecognizer",
      L"数学输入识别器",
      L"手写数学公式识别组件，供数学输入面板与 OneNote 使用",
      ECapNameTier::Base },
    { L"Tools.DeveloperMode.Core",
      L"开发人员模式",
      L"启用设备门户、应用旁加载等面向开发人员的系统功能",
      ECapNameTier::Base },
    { L"Tools.Graphics.DirectX",
      L"图形工具（DirectX）",
      L"DirectX 图形调试与帧分析工具，供图形应用开发使用",
      ECapNameTier::Base },
    { L"Tools.StorageManagement",
      L"存储管理工具",
      L"存储管理 API 与相关工具服务组件",
      ECapNameTier::Base },
    { L"NetFX3",
      L".NET Framework 3.5",
      L"为基于 .NET 2.0/3.0/3.5 的旧程序提供运行时，很多老软件必需",
      ECapNameTier::Base },
    { L"OpenSSH.Client",
      L"OpenSSH 客户端",
      L"安全外壳 (SSH) 客户端，用于远程登录和管理其他计算机",
      ECapNameTier::Base },

    // ---- RSAT 管理工具补全（原表只有 7 条，Win10/Win11 桌面版可装 20+）----
    { L"Rsat.CertificateServices.Tools", L"RSAT：证书服务工具",
      L"管理证书颁发机构（AD CS）的管理单元与命令行工具", ECapNameTier::Base },
    { L"Rsat.Dns.Tools", L"RSAT：DNS 服务器工具",
      L"远程服务器管理工具组件，用于管理 DNS 服务器角色", ECapNameTier::Base },
    { L"Rsat.FailoverCluster.Management.Tools", L"RSAT：故障转移群集工具",
      L"管理故障转移群集与高可用性角色的管理工具", ECapNameTier::Base },
    { L"Rsat.IPAM.Client.Tools", L"RSAT：IPAM 客户端工具",
      L"IP 地址管理（IPAM）客户端管理工具", ECapNameTier::Base },
    { L"Rsat.LLDP.Tools", L"RSAT：LLDP 工具",
      L"链路层发现协议（LLDP）相关管理组件", ECapNameTier::Base },
    { L"Rsat.NetworkController.Tools", L"RSAT：网络控制器工具",
      L"管理软件定义网络（SDN）网络控制器的工具", ECapNameTier::Base },
    { L"Rsat.NetworkLoadBalancing.Tools", L"RSAT：网络负载均衡工具",
      L"管理 NLB 群集的管理工具", ECapNameTier::Base },
    { L"Rsat.RemoteAccess.Management.Tools", L"RSAT：远程访问管理工具",
      L"管理 DirectAccess 与 VPN 远程访问角色的工具", ECapNameTier::Base },
    { L"Rsat.RemoteDesktop.Services.Tools", L"RSAT：远程桌面服务工具",
      L"管理远程桌面服务（RDS）角色的工具", ECapNameTier::Base },
    { L"Rsat.Shielded.VM.Tools", L"RSAT：受防护虚拟机工具",
      L"管理受防护虚拟机与主机保护者服务（HGS）的工具", ECapNameTier::Base },
    { L"Rsat.StorageMigrationService.Management.Tools", L"RSAT：存储迁移服务工具",
      L"管理存储迁移服务（SMS）的工具", ECapNameTier::Base },
    { L"Rsat.StorageReplica.Tools", L"RSAT：存储副本工具",
      L"管理存储副本（Storage Replica）功能的工具", ECapNameTier::Base },
    { L"Rsat.SystemInsights.Management.Tools", L"RSAT：系统见解工具",
      L"管理系统见解（System Insights）预测功能的工具", ECapNameTier::Base },
    { L"Rsat.VolumeActivation.Tools", L"RSAT：批量激活工具",
      L"管理 KMS/AD 批量激活的工具", ECapNameTier::Base },
    { L"Rsat.WSUS.Tools", L"RSAT：Windows Server Update Services 工具",
      L"管理 WSUS 更新服务器的管理单元", ECapNameTier::Base },

    // ---- 字体包（Fonts FoD）----
    { L"Language.Fonts.Hans", L"简体中文补充字体", L"补充中文字体包，用于正确显示简体中文界面与文档", ECapNameTier::Base },
    { L"Language.Fonts.Hant", L"繁体中文补充字体", L"补充繁体中文字体包", ECapNameTier::Base },
    { L"Language.Fonts.Jpan", L"日文补充字体", L"补充日文字体包，用于显示日文界面与文档", ECapNameTier::Base },
    { L"Language.Fonts.Kore", L"韩文补充字体", L"补充韩文字体包", ECapNameTier::Base },
    { L"Language.Fonts.Arab", L"阿拉伯文补充字体", L"补充阿拉伯文字体包", ECapNameTier::Base },
    { L"Language.Fonts.Hebr", L"希伯来文补充字体", L"补充希伯来文字体包", ECapNameTier::Base },

    // ============================================================
    // [R200] Windows 11 专属条目：build>=22000 才进索引。
    //   这些 Identity 在 Win10 上不存在（或语义不同），在此统一切除。
    // ============================================================
    { L"Microsoft.Windows.Copilot",
      L"Windows Copilot",
      L"Windows 11 内置的 AI 助手侧边栏组件",
      ECapNameTier::Win11 },
    { L"Microsoft.Windows.Ai.Copilot",
      L"Copilot 运行时",
      L"为 Windows 11 本地 AI 功能提供的模型运行时",
      ECapNameTier::Win11 },
    { L"Microsoft.Windows.Notepad",
      L"记事本（Win11 新版）",
      L"Windows 11 新版记事本的组件化分发包",
      ECapNameTier::Win11 },
    { L"Language.Fonts.Sinh",
      L"僧伽罗文补充字体",
      L"Windows 11 新增的僧伽罗文字体包",
      ECapNameTier::Win11 },
    { L"Language.Fonts.Syrc",
      L"叙利亚文补充字体",
      L"Windows 11 新增的叙利亚文字体包",
      ECapNameTier::Win11 },
};

// [C++17 R3] [[nodiscard]]：返回值勿忽略
[[nodiscard]] CDuiString CapBaseIdentity(const CDuiString& sIdentity)
{
    int pTilde = sIdentity.Find(L'~');
    return pTilde > 0 ? sIdentity.Mid(0, pTilde) : sIdentity;
}

// [R16][O-3] 本地化表预构建索引：基名（小写 ASCII 键）→ 表项，一次性建
// std::map，逐行查找由 O(n) 线性扫描降为 O(log n)（消除数百行×表长的
// 渲染放大）。键仅含 ASCII，towlower 逐字符即可，不涉及多字节折叠。
std::wstring CapLowerKey(const wchar_t* psz)
{
    std::wstring s;
    for (; psz && *psz; ++psz)
        s += (wchar_t)::towlower((wchar_t)*psz);
    return s;
}

// [R200] 分级门控：与 AppxSmartFilter 的 AppxMapTierApplies 同一口径。
[[nodiscard]] bool CapMapTierApplies(ECapNameTier nTier)
{
    if (nTier != ECapNameTier::Win11) return true;
    return osver::IsWindows11OrLater();
}

// [R200] 生效映射条数：Win10 与 Win11 上不同，供分级日志与自检核对。
[[nodiscard]] int CapActiveEntryCount()
{
    static const int s_n = []
    {
        int n = 0;
        for (size_t i = 0; i < sizeof(g_capLocalized) / sizeof(g_capLocalized[0]); ++i)
            if (CapMapTierApplies(g_capLocalized[i].nTier)) ++n;
        std::wstring sLog = L"[cap-map] tier=";
        sLog += osver::IsWindows11OrLater() ? L"win11" : L"win10";
        sLog += L" active=";
        sLog += std::to_wstring(n);
        sLog += L"\r\n";
        ::OutputDebugStringW(sLog.c_str());
        return n;
    }();
    return s_n;
}

const CapLocalized* FindCapLocalized(const CDuiString& sIdentity)
{
    static const std::map<std::wstring, const CapLocalized*> s_idx = []
    {
        std::map<std::wstring, const CapLocalized*> m;
        for (size_t i = 0; i < sizeof(g_capLocalized) / sizeof(g_capLocalized[0]); ++i)
        {
            if (!CapMapTierApplies(g_capLocalized[i].nTier)) continue;   // Win10 上整条剔除
            m[CapLowerKey(g_capLocalized[i].pszBase)] = &g_capLocalized[i];
        }
        return m;
    }();
    CDuiString sBase = CapBaseIdentity(sIdentity);
    std::map<std::wstring, const CapLocalized*>::const_iterator it =
        s_idx.find(CapLowerKey(sBase.GetData()));
    return it != s_idx.end() ? it->second : nullptr;
}

// [C++17 R3] [[nodiscard]]：返回值勿忽略
[[nodiscard]] CDuiString ResolveCapDisplayName(const CDuiString& sIdentity)
{
    const CapLocalized* p = FindCapLocalized(sIdentity);
    if (p) return p->pszName;
    return PrettyCapName(sIdentity);
}

// [C++17 R3] [[nodiscard]]：返回值勿忽略
[[nodiscard]] CDuiString ResolveCapDescription(const CapEntry& e)
{
    const CapLocalized* p = FindCapLocalized(e.sIdentity);
    if (p) return p->pszDesc;
    if (!e.sDescription.IsEmpty()) return e.sDescription;
    // RSAT 未登记子项统一描述
    if (CapBaseIdentity(e.sIdentity).Find(L"Rsat.") == 0)
        return L"远程服务器管理工具 (RSAT) 组件，用于离线管理 Windows Server 角色与功能";
    // [R200] 家族兜底：表外条目按 Identity 前缀给出可读说明，
    //   避免整列表出现"只有名字没有解释"的半空行。
    const CDuiString sBase = CapBaseIdentity(e.sIdentity);
    if (sBase.Find(L"Language.Basic.") == 0)
        return L"语言基础包：让该系统支持显示与输入这一语言";
    if (sBase.Find(L"Language.Fonts.") == 0)
        return L"字体包：补充该文字体系所需的显示字体";
    if (sBase.Find(L"Language.Handwriting.") == 0)
        return L"手写识别包：为该语言启用手写输入";
    if (sBase.Find(L"Language.OCR.") == 0)
        return L"光学字符识别包：为该语言启用以图片取字功能";
    if (sBase.Find(L"Language.Speech.") == 0)
        return L"语音识别包：为该语言启用语音输入";
    if (sBase.Find(L"Language.TextToSpeech.") == 0)
        return L"语音合成包：为该语言启用文字转语音朗读";
    return CDuiString();
}


// ============================================================
// [R200] 智能筛选：可选功能分类
//
// 分类判据只依赖 Identity 前缀与 RSAT/工具家族，不看描述文本 ——
// 描述可能被 DISM 覆盖（MergeDisplayFields 会沿用旧值），
// 用描述分类会在刷新后跳类。
// ============================================================
ECapCategory CapResolveCategory(const CDuiString& sIdentity)
{
    const CDuiString sBase = CapBaseIdentity(sIdentity);
    if (sBase.Find(L"Language.") == 0) return ECapCategory::LangPack;
    if (sBase.Find(L"Rsat.") == 0) return ECapCategory::Rsat;
    // 开发工具：开发者模式 / 图形调试 / 存储工具 / SSH / WebDriver
    if (sBase.Find(L"Tools.") == 0 || sBase.Find(L"OpenSSH.") == 0
        || sBase.Find(L"Microsoft.WebDriver") == 0)
        return ECapCategory::Tools;
    // 其余功能包：打印 / 盲文 / 无线显示 / 红外等系统内置能力。
    //   只有既无点号也无连字符的裸 Identity 才落到「其他」，
    //   否则「功能包」会被这条兜底吃掉，分类等于没分。
    if (sBase.Find(L'.') > 0 || sBase.Find(L'-') > 0) return ECapCategory::Fod;
    return ECapCategory::Other;
}

const wchar_t* CapCategoryText(ECapCategory nCat)
{
    switch (nCat)
    {
    case ECapCategory::Fod:   return StringMgr.Get("cap.smart.cat_fod");
    case ECapCategory::LangPack: return StringMgr.Get("cap.smart.cat_lang");
    case ECapCategory::Rsat:  return StringMgr.Get("cap.smart.cat_rsat");
    case ECapCategory::Tools: return StringMgr.Get("cap.smart.cat_tools");
    case ECapCategory::Other: return StringMgr.Get("cap.smart.cat_other");
    case ECapCategory::All:
    default:                  return StringMgr.Get("cap.smart.cat_all");
    }
}

int CapMapActiveEntries()
{
    return CapActiveEntryCount();
}

void CapLogMapTier(void* pUnused)
{
    (void)pUnused;
    // 同时写调试器与产品日志：调试器只在挂调试时可见，
    // 而"为什么某个功能没有中文名"是用户能遇到的问题，必须能在日志里查到。
    if (osver::IsWindows11OrLater())
    {
        // [FIX-I18N] 同 Page_Appx：译文 {0} 占位规范化为 %d 再格式化
        std::wstring sTpl = StringMgr.Get("cap.smart.s_osgate_on");
        for (size_t p; (p = sTpl.find(L"{0}")) != std::wstring::npos; )
            sTpl.replace(p, 3, L"%d");
        std::wstring sLog(256, L'\0');
        const int n = wfmt::FormatInto(sLog.data(), sLog.size(), sTpl.c_str(), CapActiveEntryCount());
        if (n > 0) sLog.resize(static_cast<size_t>(n));
        ::OutputDebugStringW(sLog.c_str());
    }
    else
    {
        ::OutputDebugStringW(StringMgr.Get("cap.smart.s_osgate_off"));
    }
}
