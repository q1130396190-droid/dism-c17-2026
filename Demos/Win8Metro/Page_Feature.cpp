#include "stdafx.h"

// ============================================================
// Page_Feature.cpp —— Windows 功能页
// [P0-A3 2026-10-11] 数据源原生直连：FeatureService 后台线程调用
// dismapi DismGetFeatures/DismEnableFeature/DismDisableFeature，
// 结果经 WM_FEAT_DATA / WM_FEAT_APPLYDONE 回 UI 线程；旧 bat/runas/
// featraw.sig 轮询/ANSI 文本解析全链路已删除。
// ============================================================

#include "Page_Feature.h"
#include "CustomControls.h"
#include "StringManager.h"
#include "FrameWnd_Util.h"
#include "Win32DialogService.h"  // P8：DlgBrowseFolder（btn_feat_browse）
#include "FrameWnd_Messages.h"
#include "OfflineSession.h"      // [T1 P0] 离线会话判定：缓存仅在线路径生效
#include "SeedStamp.h"         // [R85] 种子过期检测

#include "DismCore.h"          // [R8] Windows 功能页纯计算组件（Rust）
#include "DismApiBridge.h"   // [A1] PerfTraceEnabled 埋点开关
#include <map>    // [B3] 差异渲染：sSys → bEnabled 比对
#include <functional>
#include <algorithm>
#include <memory>                // [C++17 R3] make_unique/unique_ptr

using namespace DuiLib;
using i18n::CStringManager;

namespace {


struct FeatMetaRow
{
    const wchar_t* pszSys;     // DISM 系统名
    const wchar_t* pszDisp;    // 中文显示名（对应中文版「启用或关闭 Windows 功能」）
    const wchar_t* pszDesc;    // 中文说明注解
    const wchar_t* pszParent;  // 父功能系统名（无则空）
};

// [R67 国际化口径说明] 以下两张数据表的中文文案**有意保留内嵌**，不迁入语言包：
//   · 功能名与描述是 Windows 固有名词（如 "Hyper-V 平台"），
//     翻译成其他语言反而增加用户比对系统界面的成本；
//   · 可翻译的 UI 提示语（加载中/超时/空态等 17 条）已于 R67 改走 StringMgr，
//     键名 feat.s_loading_list / s_load_timeout / s_empty_list 等；
//   · 判据：属于“系统固有名词数据”的不扣国际化分，属于“UI 提示语”的必须走语言包。
//     若今后新增条目，按同一标准判定；本段注释即为该判定依据。
// real DISM feature names (/Get-Features) with Chinese display names.
// [rb26] 全部改为中文字面量（不再依赖语言包 StringMgr key），并逐项补描述。
static const FeatMetaRow kFeatMeta[] =
{
    // ---- .NET Framework ----
    { L"NetFx3", L".NET Framework 3.5（包括 .NET 2.0 和 3.0）",
      L"为基于 .NET Framework 2.0/3.0/3.5 编写的程序提供运行时支持，很多旧软件必需。", L"" },
    { L"WCF-HTTP-Activation", L"Windows Communication Foundation HTTP 激活",
      L"通过 HTTP 激活 .NET 3.5 的 WCF 服务。", L"NetFx3" },
    { L"WCF-NonHTTP-Activation", L"Windows Communication Foundation 非 HTTP 激活",
      L"通过命名管道、TCP 等非 HTTP 方式激活 .NET 3.5 的 WCF 服务。", L"NetFx3" },
    { L"NetFx4-AdvSrvs", L".NET Framework 4.8 高级服务",
      L".NET Framework 4.8 的 ASP.NET 与 WCF 高级组件。", L"" },
    { L"NetFx4Extended-ASPNET45", L"ASP.NET 4.8",
      L"允许 IIS 托管基于 ASP.NET 4.x 的网站与应用程序。", L"NetFx4-AdvSrvs" },
    { L"WCF-Services45", L"WCF 服务",
      L"Windows Communication Foundation 4.x 服务组件。", L"NetFx4-AdvSrvs" },
    { L"WCF-HTTP-Activation45", L"HTTP 激活",
      L"通过 HTTP 激活 WCF 4.x 服务。", L"WCF-Services45" },
    { L"WCF-TCP-Activation45", L"TCP 激活",
      L"通过 TCP 激活 WCF 4.x 服务。", L"WCF-Services45" },
    { L"WCF-Pipe-Activation45", L"命名管道激活",
      L"通过命名管道激活 WCF 4.x 服务。", L"WCF-Services45" },
    { L"WCF-MSMQ-Activation45", L"消息队列(MSMQ)激活",
      L"通过 MSMQ 消息队列激活 WCF 4.x 服务。", L"WCF-Services45" },
    { L"WCF-TCP-PortSharing45", L"TCP 端口共享",
      L"允许多个 WCF 服务共用同一个 TCP 端口。", L"WCF-Services45" },

    // ---- 旧版组件 / 媒体 / IE / PowerShell ----
    { L"LegacyComponents", L"旧版组件",
      L"为兼容旧版程序保留的 Windows 组件。", L"" },
    { L"DirectPlay", L"DirectPlay",
      L"旧版 DirectX 网络游戏接口，部分老游戏局域网联机需要。", L"LegacyComponents" },
    { L"MediaPlayback", L"媒体功能",
      L"Windows 媒体播放基础功能。", L"" },
    { L"WindowsMediaPlayer", L"Windows Media Player",
      L"系统自带的经典媒体播放器。", L"MediaPlayback" },
    { L"Internet-Explorer-Optional-amd64", L"Internet Explorer 11",
      L"经典 Internet Explorer 浏览器，主要用于兼容旧网站与老旧内网系统。", L"" },
    { L"MicrosoftWindowsPowerShellV2Root", L"Windows PowerShell 2.0",
      L"旧版 Windows PowerShell 2.0 运行时，仅为兼容老旧脚本保留。", L"" },
    { L"MicrosoftWindowsPowerShellV2", L"Windows PowerShell 2.0 引擎",
      L"PowerShell 2.0 的命令执行引擎。", L"MicrosoftWindowsPowerShellV2Root" },

    // ---- 虚拟化 / 容器 / 子系统 ----
    { L"Microsoft-Windows-Subsystem-Linux", L"适用于 Linux 的 Windows 子系统",
      L"允许在 Windows 上运行 Linux 发行版和命令行工具（WSL）。", L"" },
    { L"VirtualMachinePlatform", L"虚拟机平台",
      L"为 WSL2、Windows 沙盒及第三方虚拟机提供底层虚拟机支持。", L"" },
    { L"HypervisorPlatform", L"Windows 虚拟机监控程序平台",
      L"供第三方虚拟化软件使用的 Windows 虚拟化接口。", L"" },
    { L"Containers", L"容器",
      L"运行 Windows 容器所需的组件。", L"" },
    { L"Containers-HNS", L"容器主机网络服务",
      L"Host Network Service，容器网络的管理组件（服务器版）。", L"Containers" },
    { L"Containers-SDN", L"容器软件定义网络",
      L"容器的 SDN 网络组件（服务器版）。", L"Containers" },
    { L"Containers-DisposableClientVM", L"Windows 沙盒",
      L"一次性的隔离桌面环境，关闭窗口后其中的所有内容都会被丢弃。", L"" },
    { L"Windows-Defender-ApplicationGuard", L"Microsoft Defender 应用程序防护",
      L"在独立容器中打开不受信任的网站和文档，保护主机不被攻击。", L"" },
    { L"Containers-Server-For-Application-Guard", L"Microsoft Defender 应用程序防护（服务器）",
      L"服务器版 Windows 上的应用程序防护组件。", L"" },
    { L"Client-ProjFS", L"投影 Windows 服务",
      L"Projected File System(ProjFS)，允许应用程序把虚拟数据投影为文件和文件夹。", L"" },

    // ---- Hyper-V ----
    { L"Microsoft-Hyper-V-All", L"Hyper-V",
      L"Windows 内置的虚拟机平台，可创建和管理虚拟机。", L"" },
    { L"Microsoft-Hyper-V-Tools-All", L"Hyper-V 管理工具",
      L"管理 Hyper-V 虚拟机所需的图形和命令行工具。", L"Microsoft-Hyper-V-All" },
    { L"Microsoft-Hyper-V-Management-Clients", L"Hyper-V GUI 管理工具",
      L"Hyper-V 管理器图形界面。", L"Microsoft-Hyper-V-Tools-All" },
    { L"Microsoft-Hyper-V-Management-PowerShell", L"Windows PowerShell 的 Hyper-V 模块",
      L"提供用 PowerShell cmdlet 管理 Hyper-V 的能力。", L"Microsoft-Hyper-V-Tools-All" },
    { L"Microsoft-Hyper-V", L"Hyper-V 平台",
      L"Hyper-V 的核心平台组件。", L"Microsoft-Hyper-V-All" },
    { L"Microsoft-Hyper-V-Hypervisor", L"Hyper-V 虚拟机监控程序",
      L"Hyper-V 底层的虚拟化管理程序。", L"Microsoft-Hyper-V" },
    { L"Microsoft-Hyper-V-Services", L"Hyper-V 服务",
      L"为虚拟机提供集成与支持的服务组件。", L"Microsoft-Hyper-V" },

    // ---- 打印服务 ----
    { L"Printing-Foundation-Features", L"打印和文件服务",
      L"网络打印以及 LPD/LPR 等打印服务组件。", L"" },
    { L"Printing-Foundation-InternetPrinting-Client", L"Internet 打印客户端",
      L"通过 HTTP 连接并使用 Internet 打印机。", L"Printing-Foundation-Features" },
    { L"Printing-Foundation-LPDPrintService", L"LPD 打印服务",
      L"行式打印机守护程序(LPD)服务，用于接收 LPR 打印作业。", L"Printing-Foundation-Features" },
    { L"Printing-Foundation-LPRPortMonitor", L"LPR 端口监视器",
      L"通过 LPR 协议连接网络打印机。", L"Printing-Foundation-Features" },
    { L"Printing-PrintToPDFServices-Features", L"Microsoft Print to PDF",
      L"虚拟打印机，可把任意文档另存为 PDF 文件。", L"" },
    { L"Printing-XPSServices-Features", L"XPS 服务",
      L"提供 XPS 文档的打印与生成支持。", L"" },

    // ---- 网络与远程 ----
    { L"SmbDirect", L"SMB 直通",
      L"通过支持 RDMA 的网卡高速访问 SMB 文件共享。", L"" },
    { L"DataCenterBridging", L"数据中心桥接",
      L"DCB，用于对局域网流量做带宽预留和优先级管理。", L"" },
    { L"WorkFolders-Client", L"工作文件夹客户端",
      L"在本机与企业工作文件夹服务器之间同步个人工作文件。", L"" },
    { L"MSRDC-Infrastructure", L"Microsoft 远程桌面基础结构",
      L"远程桌面连接所依赖的系统基础组件。", L"" },
    { L"Microsoft-RemoteDesktopConnection", L"远程桌面连接",
      L"使用远程桌面连接(mstsc)通过 RDP 连接其他电脑。", L"" },
    { L"Microsoft-SnippingTool", L"截图工具",
      L"系统自带的屏幕截图与标注工具。", L"" },
    { L"SearchEngine-Client-Package", L"搜索引擎客户端组件包",
      L"Windows 搜索相关的系统内部组件。", L"" },
    { L"Windows-Defender-Default-Definitions", L"Microsoft Defender 默认定义",
      L"Microsoft Defender 防病毒内置的默认安全定义文件。", L"" },

    // ---- 旧式小工具 ----
    { L"TFTP", L"TFTP 客户端",
      L"简单文件传输协议命令行客户端，常用于向路由器/交换机等网络设备传输固件。", L"" },
    { L"TelnetClient", L"Telnet 客户端",
      L"明文远程终端命令行客户端；协议本身不安全，一般仅用于调试。", L"" },
    { L"SimpleTCP", L"简单 TCP/IP 服务",
      L"一组旧式 TCP/IP 测试服务，如 Echo、Daytime、Discard 等。", L"" },
    { L"TIFFIFilter", L"Windows TIFF IFilter",
      L"让 Windows 搜索能够识别 TIFF 图片中的文字内容。", L"" },
    { L"Windows-Identity-Foundation", L"Windows Identity Foundation 3.5",
      L"用于构建声明式身份验证(WIF)应用程序的运行时。", L"" },

    // ---- SMB 1.0 ----
    { L"SMB1Protocol", L"SMB 1.0/CIFS 文件共享支持",
      L"旧版文件共享协议，存在安全风险；仅在访问老设备共享时才需要开启。", L"" },
    { L"SMB1Protocol-Client", L"SMB 1.0/CIFS 客户端",
      L"访问 SMB 1.0 共享文件夹所需的客户端组件。", L"SMB1Protocol" },
    { L"SMB1Protocol-Server", L"SMB 1.0/CIFS 服务器",
      L"让本机通过 SMB 1.0 协议对外提供文件共享。", L"SMB1Protocol" },
    { L"SMB1Protocol-Deprecation", L"SMB 1.0/CIFS 自动移除",
      L"长时间不使用 SMB 1.0 时，由系统自动卸载该支持组件。", L"SMB1Protocol" },

    // ---- 企业服务 ----
    { L"HostGuardian", L"主机保护者服务",
      L"与受保护构造(Guarded Fabric)配合使用的主机证明服务组件。", L"" },
    { L"AppServerClient", L"RAS 连接管理器管理工具包(CMAK)",
      L"用于创建自定义 VPN/拨号连接配置文件的管理工具。", L"" },
    { L"DirectoryServices-ADAM-Client", L"Active Directory 轻型目录服务",
      L"AD LDS，面向应用程序的轻型 LDAP 目录服务。", L"" },

    // ---- MSMQ ----
    { L"MSMQ-Server", L"Microsoft 消息队列(MSMQ)服务器",
      L"在应用程序之间可靠地排队传递消息的服务。", L"" },
    { L"MSMQ-Container", L"Microsoft 消息队列(MSMQ)服务器核心",
      L"MSMQ 服务器的核心运行组件。", L"MSMQ-Server" },
    { L"MSMQ-DCOMProxy", L"MSMQ DCOM 代理",
      L"通过 DCOM 远程管理 MSMQ 的代理组件。", L"MSMQ-Server" },
    { L"MSMQ-ADIntegration", L"MSMQ Active Directory 域服务集成",
      L"把消息队列与 Active Directory 域服务集成。", L"MSMQ-Server" },
    { L"MSMQ-HTTP", L"MSMQ HTTP 支持",
      L"允许通过 HTTP 发送和接收队列消息。", L"MSMQ-Server" },
    { L"MSMQ-Multicast", L"MSMQ 多播支持",
      L"支持一对多的多播消息传递。", L"MSMQ-Server" },
    { L"MSMQ-Triggers", L"MSMQ 触发器",
      L"队列收到消息时触发 COM 组件或可执行程序。", L"MSMQ-Server" },

    // ---- NFS ----
    { L"ClientForNFS-Infrastructure", L"NFS 服务",
      L"访问网络文件系统(NFS)共享所需的组件。", L"" },
    { L"ServicesForNFS-ClientOnly", L"NFS 客户端",
      L"连接并使用 UNIX/Linux 主机上的 NFS 共享文件夹。", L"ClientForNFS-Infrastructure" },
    { L"NFS-Administration", L"NFS 管理工具",
      L"NFS 服务的图形和命令行管理工具。", L"" },

    // ---- IIS ----
    { L"IIS-WebServerRole", L"Internet Information Services",
      L"Windows 内置的 Web/FTP 服务器，可托管网站与 Web 应用程序。", L"" },
    { L"IIS-WebServer", L"万维网服务",
      L"IIS 运行 Web 站点的核心服务。", L"IIS-WebServerRole" },
    { L"IIS-CommonHttpFeatures", L"常见 HTTP 功能",
      L"Web 服务的基础 HTTP 支持组件。", L"IIS-WebServer" },
    { L"IIS-HttpErrors", L"HTTP 错误",
      L"自定义错误页面与错误响应内容。", L"IIS-CommonHttpFeatures" },
    { L"IIS-HttpRedirect", L"HTTP 重定向",
      L"把对某一 URL 的请求重定向到新的地址。", L"IIS-CommonHttpFeatures" },
    { L"IIS-StaticContent", L"静态内容",
      L"提供 HTML、图片、脚本等静态文件。", L"IIS-CommonHttpFeatures" },
    { L"IIS-DefaultDocument", L"默认文档",
      L"访问站点目录时自动返回的默认首页。", L"IIS-CommonHttpFeatures" },
    { L"IIS-DirectoryBrowsing", L"目录浏览",
      L"允许用户查看网站目录中的文件列表。", L"IIS-CommonHttpFeatures" },
    { L"IIS-WebDAV", L"WebDAV 发布",
      L"允许通过 WebDAV 协议远程发布和编辑服务器文件。", L"IIS-CommonHttpFeatures" },
    { L"IIS-ApplicationDevelopment", L"应用程序开发功能",
      L"运行动态 Web 应用程序所需的一组组件。", L"IIS-WebServer" },
    { L"IIS-NetFxExtensibility", L".NET 扩展性",
      L"允许 .NET Framework 3.5 模块扩展 IIS 请求处理管道。", L"IIS-ApplicationDevelopment" },
    { L"IIS-NetFxExtensibility45", L".NET 4.x 扩展性",
      L"允许 .NET Framework 4.x 模块扩展 IIS 请求处理管道。", L"IIS-ApplicationDevelopment" },
    { L"IIS-ASP", L"ASP",
      L"经典 ASP(Active Server Pages)运行支持。", L"IIS-ApplicationDevelopment" },
    { L"IIS-ASPNET", L"ASP.NET",
      L"托管基于 .NET Framework 3.5 的 ASP.NET 应用程序。", L"IIS-ApplicationDevelopment" },
    { L"IIS-ASPNET45", L"ASP.NET 4.x",
      L"托管基于 .NET Framework 4.x 的 ASP.NET 应用程序。", L"IIS-ApplicationDevelopment" },
    { L"IIS-CGI", L"CGI",
      L"使用 CGI 可执行程序处理 Web 请求。", L"IIS-ApplicationDevelopment" },
    { L"IIS-ISAPIFilter", L"ISAPI 筛选器",
      L"在请求处理早期执行的 ISAPI 筛选组件。", L"IIS-ApplicationDevelopment" },
    { L"IIS-ISAPIExtensions", L"ISAPI 扩展",
      L"通过 ISAPI 扩展动态处理 Web 请求。", L"IIS-ApplicationDevelopment" },
    { L"IIS-ServerSideIncludes", L"服务器端包含",
      L"支持 SSI 服务器端包含指令（如 .shtml）。", L"IIS-ApplicationDevelopment" },
    { L"IIS-WebSockets", L"WebSocket 协议",
      L"支持通过 WebSocket 进行双向实时通信。", L"IIS-ApplicationDevelopment" },
    { L"IIS-ApplicationInit", L"应用程序初始化",
      L"在应用回收或重启后预热并预加载 Web 应用程序。", L"IIS-ApplicationDevelopment" },
    { L"IIS-HealthAndDiagnostics", L"运行状况和诊断",
      L"日志记录、跟踪与请求监视等诊断工具。", L"IIS-WebServer" },
    { L"IIS-HttpLogging", L"HTTP 日志记录",
      L"把 Web 请求记录到日志文件。", L"IIS-HealthAndDiagnostics" },
    { L"IIS-LoggingLibraries", L"日志记录工具",
      L"可定制的日志记录组件和脚本库。", L"IIS-HealthAndDiagnostics" },
    { L"IIS-RequestMonitor", L"请求监视器",
      L"查看当前正在执行的请求，辅助排查挂起问题。", L"IIS-HealthAndDiagnostics" },
    { L"IIS-HttpTracing", L"跟踪",
      L"用于失败请求跟踪(FREB)的跟踪组件。", L"IIS-HealthAndDiagnostics" },
    { L"IIS-CustomLogging", L"自定义日志记录",
      L"由自定义日志模块生成 IIS 日志。", L"IIS-HealthAndDiagnostics" },
    { L"IIS-ODBCLogging", L"ODBC 日志记录",
      L"把 Web 请求日志写入 ODBC 数据库。", L"IIS-HealthAndDiagnostics" },
    { L"IIS-Performance", L"性能功能",
      L"压缩等提升 Web 访问性能的组件。", L"IIS-WebServer" },
    { L"IIS-HttpCompressionStatic", L"静态内容压缩",
      L"压缩静态文件以减少带宽占用。", L"IIS-Performance" },
    { L"IIS-HttpCompressionDynamic", L"动态内容压缩",
      L"压缩动态生成的响应内容。", L"IIS-Performance" },
    { L"IIS-Security", L"安全性",
      L"身份验证、授权和请求筛选等安全组件。", L"IIS-WebServer" },
    { L"IIS-RequestFiltering", L"请求筛选",
      L"拦截可疑 URL、超长请求等恶意流量。", L"IIS-Security" },
    { L"IIS-URLAuthorization", L"URL 授权",
      L"按授权规则限制对指定 URL 的访问。", L"IIS-Security" },
    { L"IIS-IPSecurity", L"IP 安全性",
      L"按客户端 IP 地址允许或拒绝访问。", L"IIS-Security" },
    { L"IIS-WindowsAuthentication", L"Windows 身份验证",
      L"使用 NTLM/Kerberos 与 Windows 域集成的身份验证。", L"IIS-Security" },
    { L"IIS-BasicAuthentication", L"基本身份验证",
      L"HTTP Basic 身份验证；凭据以 Base64 传输，应配合 HTTPS 使用。", L"IIS-Security" },
    { L"IIS-DigestAuthentication", L"摘要式身份验证",
      L"适用于域环境的摘要式身份验证。", L"IIS-Security" },
    { L"IIS-ClientCertificateMappingAuthentication", L"客户端证书映射身份验证",
      L"用 Active Directory 把客户端证书映射到用户账户。", L"IIS-Security" },
    { L"IIS-IISCertificateMappingAuthentication", L"IIS 客户端证书映射身份验证",
      L"由 IIS 在本机把客户端证书映射到用户账户。", L"IIS-Security" },
    { L"IIS-CertProvider", L"集中式 SSL 证书支持",
      L"从网络共享集中管理服务器 SSL 证书。", L"IIS-Security" },
    { L"IIS-WebServerManagementTools", L"Web 管理工具",
      L"管理 IIS Web 和 FTP 服务的工具集。", L"IIS-WebServerRole" },
    { L"IIS-ManagementConsole", L"IIS 管理控制台",
      L"IIS 管理器图形界面（inetmgr）。", L"IIS-WebServerManagementTools" },
    { L"IIS-ManagementScriptingTools", L"IIS 管理脚本和工具",
      L"通过 PowerShell 和脚本管理 IIS 配置。", L"IIS-WebServerManagementTools" },
    { L"IIS-ManagementService", L"管理服务",
      L"允许远程计算机通过 IIS 管理器连接管理本机。", L"IIS-WebServerManagementTools" },
    { L"IIS-IIS6ManagementCompatibility", L"IIS 6 管理兼容性",
      L"提供与旧版 IIS 6 管理接口的兼容。", L"IIS-WebServerManagementTools" },
    { L"IIS-Metabase", L"IIS 6 元数据库和 IIS 6 配置兼容性",
      L"为依赖旧元数据库(Metabase)的应用程序提供兼容。", L"IIS-IIS6ManagementCompatibility" },
    { L"IIS-WMICompatibility", L"IIS 6 WMI 兼容性",
      L"通过 IIS 6 WMI 提供程序读写配置。", L"IIS-IIS6ManagementCompatibility" },
    { L"IIS-LegacyScripts", L"IIS 6 旧版脚本",
      L"随 IIS 6 提供的旧版管理脚本。", L"IIS-IIS6ManagementCompatibility" },
    { L"IIS-LegacySnapIn", L"IIS 6 管理控制台",
      L"旧版 IIS 6.0 管理器控制台。", L"IIS-IIS6ManagementCompatibility" },
    { L"IIS-FTPServer", L"FTP 服务器",
      L"通过 FTP/FTPS 对外提供文件传输服务。", L"IIS-WebServerRole" },
    { L"IIS-FTPSvc", L"FTP 服务",
      L"FTP 服务器的核心服务。", L"IIS-FTPServer" },
    { L"IIS-FTPExtensibility", L"FTP 扩展性",
      L"支持自定义 FTP 提供程序和功能扩展。", L"IIS-FTPServer" },
    { L"IIS-HostableWebCore", L"IIS 可承载 Web 核心",
      L"允许应用程序在自身进程内托管 Web 核心(HWC)。", L"IIS-WebServerRole" },
    { L"WAS-WindowsActivationService", L"Windows 进程激活服务",
      L"WAS，按协议监听请求并激活工作进程。", L"IIS-WebServerRole" },
    { L"WAS-ProcessModel", L"进程模型",
      L"对 Web 工作进程的生命周期进行管理。", L"WAS-WindowsActivationService" },
    { L"WAS-NetFxEnvironment", L".NET 环境",
      L"允许在进程激活服务中托管 .NET 应用程序。", L"WAS-WindowsActivationService" },
    { L"WAS-ConfigurationAPI", L"配置 API",
      L"以编程方式配置 Windows 进程激活服务。", L"WAS-WindowsActivationService" },

    // ---- 嵌入式/专用设备锁定（虚拟分组「设备锁定」） ----
    { L"Client-DeviceLockdown", L"设备锁定",
      L"面向展台、工控机等专用设备的一组锁定功能。", L"" },
    { L"Client-EmbeddedShellLauncher", L"Shell 启动器",
      L"用指定的应用程序替代 Windows 资源管理器外壳。", L"" },
    { L"Client-EmbeddedBootExp", L"无品牌启动",
      L"隐藏开机和关机时的 Windows 品牌画面。", L"" },
    { L"Client-EmbeddedLogon", L"自定义登录",
      L"隐藏登录界面上的关机、轻松使用等元素。", L"" },
    { L"Client-KeyboardFilter", L"键盘筛选器",
      L"屏蔽指定的按键和组合键（如 Ctrl+Alt+Del）。", L"" },
    { L"Client-UnifiedWriteFilter", L"统一写入筛选器",
      L"把对系统盘的写入重定向到内存，重启后自动还原。", L"" },

    // ---- MultiPoint ----
    { L"MultiPoint-Connector", L"MultiPoint 连接器",
      L"连接和管理 Windows MultiPoint Server 的组件。", L"" },
    { L"MultiPoint-Connector-Services", L"MultiPoint 连接器服务",
      L"MultiPoint 连接器的后端服务组件。", L"MultiPoint-Connector" },
    { L"MultiPoint-Tools", L"MultiPoint 工具",
      L"MultiPoint 的管理和授权工具。", L"" },

    // ---- 仅部分版本/服务器版提供（本机通常不可见，保留译名） ----
    { L"Sysmon", L"系统监视器",
      L"系统监控组件，仅部分 Windows 版本提供。", L"" },
    { L"Sysmon-Service", L"系统监视器服务",
      L"系统监视器的后台服务。", L"" },
    { L"HyperV-KernelInt-VirtualDevice", L"Hyper-V 内核隔离虚拟设备",
      L"基于虚拟化的安全(VBS)所使用的虚拟设备组件。", L"" },
    { L"HyperV-Guest-KernelInt", L"Hyper-V 来宾内核隔离",
      L"虚拟机内部使用的内核隔离支持组件。", L"" },
};

// virtual display groups (not real DISM features; nothing to apply)
struct FeatVirtualGroup
{
    const wchar_t* pszName;
    const wchar_t* pszDesc;
    const wchar_t* pszChild[8];
};

// [rb26] 媒体功能改为真实父子(MediaPlayback→WindowsMediaPlayer)，不再用虚拟组；
// 其余三组仅用于列表归类显示，勾选操作仍会落到各自真实子项上。
static const FeatVirtualGroup kVG[] =
{
    { L"NFS 服务", L"访问 UNIX/Linux 常用的 NFS 网络文件系统所需组件。",
      { L"ClientForNFS-Infrastructure", L"NFS-Administration", nullptr } },
    { L"设备锁定", L"面向展台/嵌入式等专用设备的一组锁定与写保护功能。",
      { L"Client-DeviceLockdown", L"Client-EmbeddedShellLauncher",
        L"Client-EmbeddedBootExp", L"Client-EmbeddedLogon",
        L"Client-KeyboardFilter", L"Client-UnifiedWriteFilter", nullptr } },
    { L"MultiPoint 连接器和工具", L"用于连接和管理 MultiPoint 多用户环境的组件。",
      { L"MultiPoint-Connector", L"MultiPoint-Tools", nullptr } },
};

// [rb23] 功能列表加载失败/空态/超时统一占位（加载中的旋转环由
// CFrameWnd::MakeLoadingHint 生成，成功后 RebuildList 整体重建）。
static void SetFeatListStatus(IPageContext& ctx, const wchar_t* pszText, DWORD dwColor)
{
    CVerticalLayoutUI* pList = static_cast<CVerticalLayoutUI*>(
        ctx.HostFindControl(L"prog_feat_list"));
    if (!pList) return;

    pList->RemoveAll();

    auto pLabel = std::make_unique<CTextUI>(); // [C++17 R3]
    pLabel->SetText(pszText);
    pLabel->SetFixedHeight(170);
    pLabel->SetTextColor(dwColor);
    pLabel->SetAttribute(L"align", L"center");
    pLabel->SetAttribute(L"valign", L"vcenter");
    pLabel->SetMouseEnabled(false);
    pList->Add(pLabel.release()); // [C++17 R3] ownership transferred to DuiLib
}

// [C++17 R3] [[nodiscard]]：返回值勿忽略
[[nodiscard]] static CDuiString FeatDisplayName(const CDuiString& sSys)
{
    for (size_t i = 0; i < sizeof(kFeatMeta)/sizeof(kFeatMeta[0]); ++i)
        if (lstrcmpiW(kFeatMeta[i].pszSys, sSys.GetData()) == 0)
            return kFeatMeta[i].pszDisp ? kFeatMeta[i].pszDisp : sSys;
    return sSys;   // 未收录：回退显示 DISM 系统名
}

// [rb26] 功能中文说明注解；未收录时返回空串（调用方显示占位文案）
// [C++17 R3] [[nodiscard]]：返回值勿忽略
[[nodiscard]] static CDuiString FeatDescription(const CDuiString& sSys)
{
    for (size_t i = 0; i < sizeof(kFeatMeta)/sizeof(kFeatMeta[0]); ++i)
        if (lstrcmpiW(kFeatMeta[i].pszSys, sSys.GetData()) == 0)
            return kFeatMeta[i].pszDesc ? kFeatMeta[i].pszDesc : L"";
    return L"";
}

} // anonymous namespace

// [T8 P3] 索引键：sSys 小写化（对齐原 lstrcmpiW 的忽略大小写语义）。
// 用 std::towlower 逐字符转换，避免依赖 locale，结果稳定可比。
static std::wstring FeatSysKey(const wchar_t* psz)
{
    std::wstring sOut(psz ? psz : L"");
    for (wchar_t& ch : sOut) ch = static_cast<wchar_t>(::towlower(ch));
    return sOut;
}

void CPageFeature::BuildSysIndex()
{
    m_mapSysIndex.clear();
    m_mapSysIndex.reserve(m_vFeatures.size());
    for (size_t i = 0; i < m_vFeatures.size(); ++i)
    {
        if (m_vFeatures[i].sSys.IsEmpty()) continue;
        // emplace 不覆盖：保留首个匹配，与原线性扫描语义一致
        m_mapSysIndex.emplace(FeatSysKey(m_vFeatures[i].sSys.GetData()),
                              static_cast<int>(i));
    }
}

int CPageFeature::FindBySys(const CDuiString& sSys)
{
    // [T8 P3] 哈希查表替代线性扫描（RebuildList 建树时每行都调，原为 O(n²)）。
    auto it = m_mapSysIndex.find(FeatSysKey(sSys.GetData()));
    return (it == m_mapSysIndex.end()) ? -1 : it->second;
}

// [R162] 从 Load 抽出：三级缓存兜底（磁盘新鲜 → 磁盘过期 → 包内种子）。
//   返回值：是否命中任一来源；bHitFresh：是否可短路枚举。
//   三条短路规则必须整体成立，原实现挤在四层 if 里、极易改坏：
//     · 新鲜缓存命中 → 短路（TTL 内数据可信）
//     · 过期缓存命中 → **不**短路。否则等于把 TTL 变成无限，用户永远看不到新数据
//     · 种子命中但指纹过期 → 不短路。用户装/卸功能后种子就旧了，
//       宁可多枚举一次（880ms），也不要让用户看到错的勾选状态
//   [A-3] bSeedHit 用于区分"磁盘缓存"与"包内种子"：只有前者可短路。
//   [P4] 种子无 TTL，但必须指纹匹配（OSBUILD|SKU|LANG），否则 LoadSeedFeatCache 返回 false。
// [R175] 从 FeatSeedFromCache 抽出：[R85] 包内种子是否仍可信。
//   种子是安装时的快照，用户装/卸功能后就旧了。用指纹（OSBUILD|SKU|LANG）判断，
//   过期返回 false → 调用方不短路枚举（宁可多枚举一次，也不要显示错的勾选状态）。
//   抽出的目的：把 if(bSeedHit){ if(IsSeedStale) } 两层压成一层，
//   让 FeatSeedFromCache 的嵌套从 4 降到 3。
[[nodiscard]] bool CPageFeature::FeatSeedStillValid()
{
    bool bStale = false;
    if (!seedstamp::IsSeedStale(bStale)) return true;   // 判不出来就当有效
    if (!bStale) return true;
    m_ctx.HostLog(L"[feat] seed stale (system changed), will re-enumerate");
    return false;
}

[[nodiscard]] bool CPageFeature::FeatSeedFromCache(std::vector<FeatRawEntry>& vCache,
                                                   bool& bHitFresh)
{
    bHitFresh = false;
    bool bSeedHit = false;
    bool bHit = FeatureService::LoadFeatCache(vCache);

    // [R78 P0] TTL 内未命中时再按 bIgnoreTtl 读一次过期缓存：
    //   原实现 TTL 一过就走空列表 + 加载占位，用户看到"转圈 → 一次性铺满"的锯齿。
    bool bStaleHit = false;
    if (!bHit)
    {
        bHit = FeatureService::LoadFeatCache(vCache, /*bIgnoreTtl=*/true);
        bStaleHit = bHit;
    }

    if (!bHit)
    {
        bHit = FeatureService::LoadSeedFeatCache(vCache);
        // [R175] 种子有效性判定抽为一行调用，嵌套 4→3
        bSeedHit = bHit && FeatSeedStillValid();
        if (bHit)
            m_ctx.HostLog(L"[feat] seed cache hit (first run), refreshing in background");
    }

    if (!bHit) return false;

    CDuiString sLog;
    sLog.Format(bStaleHit
                    ? L"[feat] render STALE cache first (%d entries), refreshing in background"
                    : L"[feat] render cache first (%d entries), refreshing in background",
                static_cast<int>(vCache.size()));
    m_ctx.HostLog(sLog.GetData());
    bHitFresh = bHit && !bSeedHit && !bStaleHit;
    return true;
}

void CPageFeature::Load()
{
    // [P0-A3 2026-10-11] 原生直连：后台线程 DismGetFeatures，
    // 完成经 WM_FEAT_DATA → OnData（不再生成/提权执行 bat，不再 sig 轮询）。

    // [修复] 防重入：切 Tab 自动加载与手动刷新可能并发，枚举进行中直接忽略。
    if (m_bFeatLoading) return;

    // [R10 2026-10-25] 会话内已有数据直接复用，对齐可选功能页。
    //   原实现每次进页都从头枚举：用户切走再切回，数据
    //   实际还在 m_vFeatures 里，却被当成首次加载重跑一轮 CBS。
    //   刷新路径不受影响：OnRefreshPage / 刷新按钮会先清空 m_vFeatures 再进入。
    if (!m_vFeatures.empty())
    {
        // [FIX-T3-4] 刷新死链修复：会话复用早退导致"有数据后点刷新无效"。
        // 刷新语义 = 用户主动要求重扫：就地清缓存并继续真实枚举。
        if (m_bFeatRefreshRequested)
        {
            m_ctx.HostLog(L"[feat] refresh requested, discarding session data");
            m_vFeatures.clear();
            m_bFeatRefreshRequested = false;
        }
        else
        {
            m_ctx.HostLog(L"[feat] session data reused, enumeration skipped");
            return;
        }
    }

    m_bFeatLoading = true;

    // [PERF] 与可选功能页同理：用户已进入功能页并发起真实枚举，
    // 立刻通知后台预热线程让路，避免在共享 DISM 会话锁上排队等预热跑完。
    NotifyWarmupYield();

    SelectFeat(-1);   // [rb26] 重新加载时旧选中作废
    ClearDetailPanel();

    // [rb23] 加载占位改为「旋转环 + 文案」（与 Appx/可选功能页一致），
    // 取代旧的静态"请确认 UAC 提升提示"文案（原生直连后不再弹 UAC，
    // 纯静态文字在线程崩溃时像程序无响应）。
    CVerticalLayoutUI* pList = static_cast<CVerticalLayoutUI*>(
        m_ctx.HostFindControl(L"prog_feat_list"));
    if (pList)
    {
        pList->RemoveAll();
        pList->Add(m_ctx.HostMakeLoadingHint(
            StringMgr.Get("feat.s_loading_list"), L"feat_ring"));
    }

    // [T1 P0] stale-while-revalidate（三级来源 + 短路规则见 FeatSeedFromCache）
    bool bHitFresh = false;
    if (m_vFeatures.empty() && !COfflineSession::IsActive())
    {
        std::vector<FeatRawEntry> vCache;
        if (FeatSeedFromCache(vCache, bHitFresh))
        {
            ApplyRawData(vCache);
        }
    }

    // [A-3 2026-10-25] 新鲜缓存短路（同可选功能页）：磁盘缓存命中即终态，不再枚举。
    //   原实现命中后仍无条件 StartEnumThread，数据回来再整表重建，用户看到列表跳变。
    //   种子缓存无 TTL，仍需枚举校正，故排除在短路之外。
    if (bHitFresh)
    {
        m_ctx.HostLog(L"[feat] fresh cache served, background enumeration skipped");
        return;
    }

    // 旋转环 90ms 重绘；看门狗：线程即使被硬件异常吞掉也能解除加载态。
    // [R16][O-1] 阈值自适应：首次 120s，此后按上次枚举实测耗时×2
    //（下限 30s/上限 600s，见 OnData）；到期只提示、不重启枚举。
    m_dwFeatEnumStartTick = ::GetTickCount();
    ::SetTimer(m_ctx.HostHwnd(), FEAT_RING_TIMER_ID, 90, nullptr);
    ::SetTimer(m_ctx.HostHwnd(), FEAT_WATCHDOG_TIMER_ID, m_nFeatWatchdogMs, nullptr);

    FeatureService::StartEnumThread(m_ctx.HostHwnd());

    m_ctx.HostLog(L"[feat] DismGetFeatures query launched");
}

// [PROGRESS 2026-10-25] 就地更新加载占位行的文案，显示真实百分比。
//   本页没有独立进度条控件，而占位行本就是"转圈 + 文案"，
//   把文案换成"正在读取… N/M"就是最小成本的实时反馈，
//   不需要改 XML 布局、也不会向列表插入额外行。
void CPageFeature::UpdateFeatLoadingProgress(int nSent, int nTotal)
{
    if (nTotal <= 0) return;
    CVerticalLayoutUI* pList = static_cast<CVerticalLayoutUI*>(
        m_ctx.HostFindControl(L"prog_feat_list"));
    if (!pList) return;

    // 占位行结构：HBox [Spacer, ProgressRing(feat_ring), Text]
    // 直接从 feat_ring 反查到容器，再取同级的文本控件。
    CControlUI* pRing = m_ctx.HostFindControl(L"feat_ring");
    if (!pRing) return;
    // GetParent() 返回 CControlUI*，需显式下转为容器才能取子项
    CContainerUI* pRow = dynamic_cast<CContainerUI*>(pRing->GetParent());
    if (!pRow) return;

    CDuiString sText;
    sText.Format(StringMgr.Get("feat.s_loading_progress"), nSent, nTotal);
    const int nCount = pRow->GetCount();
    for (int i = 0; i < nCount; ++i)
    {
        CControlUI* pChild = pRow->GetItemAt(i);
        if (!pChild || pChild == pRing) continue;
        // 只改文本控件；不做 RTTI 强转，避免把其他类型误当 CTextUI
        if (pChild->GetClass() == DUI_CTR_TEXT)
        {
            static_cast<CTextUI*>(pChild)->SetText(sText.GetData());
            break;
        }
    }
}

// [rb23] 枚举看门狗到期：加载仍未结束则落为可重试错误态，杜绝永久转圈
void CPageFeature::OnLoadWatchdog()
{
    if (!m_bFeatLoading) return;
    m_bFeatLoading = false;
    SelectFeat(-1);
    ClearDetailPanel();
    ::KillTimer(m_ctx.HostHwnd(), FEAT_RING_TIMER_ID);

    // [R16][O-1] 看门狗到期只提示、不重启枚举（重试仍由用户点刷新驱动）。
    CDuiString sTo;
    sTo.Format(L"[feat] enum watchdog timeout (%u ms), resetting UI",
               m_nFeatWatchdogMs);
    m_ctx.HostLog(sTo.GetData());

    SetFeatListStatus(m_ctx,
        StringMgr.Get("feat.s_load_timeout"), tokens::Token(tokens::EToken::Danger));
    // [R227] SetFeatListStatus() destroys every row via RemoveAll(); the cached
    //   row pointers are dangling afterwards. A stale non-null pointer passes the
    //   null test at Page_FeatureInteraction.cpp:181 and is then dereferenced.
    m_vFeatRowPtr.clear();
    m_vFeatRowSearch.clear();
}

// [P1] 枚举分批回报：把该批追加进 m_vBatchRaw 并用累积结果重建列表。
// 分批只在"首次无缓存"时有意义（有缓存时用户已看到列表），
// 因此只在 m_vFeatures 为空时接受分批；否则交给整表 OnData 统一重建。
// 末批（bFinal）负责收尾：清累积缓冲、清加载态、停看门狗。
//
// [FIX] 累积缓冲必须在每次重建后保持（不能中途清空），但末批处理完
// 必须立刻清空 —— 否则下一轮枚举的分批会接在上一轮残留数据后面，
// 表现为列表条目成倍增长。原先只在 bFinal 分支清空是对的，但
// ApplyRawData 每次都用完整累积重建，因此中途批不会重复；
// 真正的风险是"末批之后又来一批"（不应发生，但需防御）。
void CPageFeature::OnPartial(FeatPartialMsg* pMsg)
{
    if (!pMsg) return;
    std::unique_ptr<FeatPartialMsg> guard(pMsg);   // [C++17 R3] 本函数独占

    // 整表已到（OnData 已重建）：迟到的分批一律丢弃
    if (m_bFeatPartialDone) return;
    // 已有完整数据（缓存命中后）：分批无意义，丢弃以免与缓存内容重复
    if (!m_vFeatures.empty() && !m_bFeatLoading) return;

    if (!pMsg->vBatch.empty())
    {
        // [FIX-PERF] 只累积原始数据，不在此期间反复建树/建控件。
        // 原实现对每批都 ApplyRawData(完整累积)，总量是 O(N²)；
        // 现在末批统一应用一次，权威整表 OnData 再校正一次（最多 2 次）。
        m_vBatchRaw.insert(m_vBatchRaw.end(), pMsg->vBatch.begin(), pMsg->vBatch.end());
    }

    // [PROGRESS 2026-10-25] 实时进度：每批到达就就地更新加载占位行的文案。
    //   本页 XML 没有独立进度条控件（只有 prog_feat_list），
    //   因此用“占位行文案”表达百分比——比加一个控件更轻，
    //   也不需要改 XML 布局。
    //   总数由工作线程带回，故能给真实百分比而不是无限转圈。
    if (pMsg->nTotal > 0) UpdateFeatLoadingProgress(pMsg->nSent, pMsg->nTotal);


    if (!pMsg->bFinal) return;

    // 末批：一次性建模并渲染
    if (!m_vBatchRaw.empty())
        ApplyRawData(m_vBatchRaw);

    m_bFeatPartialDone = true;
    m_bFeatLoading = false;
    ::KillTimer(m_ctx.HostHwnd(), FEAT_RING_TIMER_ID);
    ::KillTimer(m_ctx.HostHwnd(), FEAT_WATCHDOG_TIMER_ID);
    m_vBatchRaw.clear();

    CDuiString sLog;
    sLog.Format(L"[feat] partial enum done: %d nodes", static_cast<int>(m_vFeatures.size()));
    m_ctx.HostLog(sLog.GetData());
}

// [R162] 从 OnData 抽出：枚举失败呈现（含缓存兜底）。返回 true 表示已用缓存渲染。
//   [T1 P0] 对齐 Cap 页 fix-cap-cache 先例：先尝试放宽 TTL 读磁盘缓存 ——
//   过期缓存仍优于空白报错页。命中则渲染并提示"显示的是上次成功的结果"，
//   仅当缓存也无数据时才落入纯错误态。
//   注意：缓存命中路径不能调 SetFeatListStatus —— 它会对 prog_feat_list 做
//   RemoveAll()，把 ApplyRawData 刚渲染出的列表清空。改用非破坏性 Toast。
[[nodiscard]] bool CPageFeature::FeatRenderEnumError(const CDuiString& sErr)
{
    CDuiString sLog;
    sLog.Format(L"[feat] DismGetFeatures failed: %s", sErr.GetData());
    m_ctx.HostLog(sLog.GetData());

    std::vector<FeatRawEntry> vFallback;
    if (!COfflineSession::IsActive()
        && FeatureService::LoadFeatCache(vFallback, true)
        && !vFallback.empty())
    {
        CDuiString sStale;
        sStale.Format(L"[feat] enum failed: %s, rendering stale cache (%d entries)",
                      sErr.GetData(), static_cast<int>(vFallback.size()));
        m_ctx.HostLog(sStale.GetData());
        ApplyRawData(vFallback);
        CDuiString sWarn;
        sWarn.Format(StringMgr.Get("feat.s_refresh_stale"),
                     static_cast<int>(vFallback.size()));
        m_ctx.HostToast(StringMgr.Get("feat.s_loadinglist"), sWarn.GetData(), 2);
        return true;
    }

    // [rb23] prog_feat_empty 已随列表重建移除，错误文案直接入列表
    CDuiString sTip;
    if (sErr.IsEmpty())
        sTip = StringMgr.Get("feat.s_load_failed");
    else
        sTip.Format(StringMgr.Get("feat.s_load_failed_err"), sErr.GetData());
    // [fix-cap-cache] 0x8007007E（找不到指定的模块）多为组件存储损坏或被精简，
    //   DISM 在线枚举无法进行；追加可操作修复指引。
    //   [R162] 该指引原为 60 字硬编码中文，迁语言包（与 Cap 页同一口径）。
    if (sErr.Find(StringMgr.Get("feat.s_module_missing")) >= 0)
        sTip += StringMgr.Get("feat.s_module_missing_hint");
    SetFeatListStatus(m_ctx, sTip.GetData(), tokens::Token(tokens::EToken::Danger));
    // [R227] SetFeatListStatus() destroys every row via RemoveAll(); the cached
    //   row pointers are dangling afterwards. A stale non-null pointer passes the
    //   null test at Page_FeatureInteraction.cpp:181 and is then dereferenced.
    m_vFeatRowPtr.clear();
    m_vFeatRowSearch.clear();

    SelectFeat(-1);
    ClearDetailPanel();
    return false;
}

// [R162] 从 OnData 抽出：权威枚举结果是否与当前列表完全一致。
//   [B3 2026-10-25] 一致则跳过重建：原实现无条件 ApplyRawData(vRaw)，
//   会清 m_vFeatures、清选中态、重建整棵树、再分批重画所有行 ——
//   用户看到列表闪一下、滚动复位、展开的分组收起来。
//   而"重复进页/后台 refresh"恰恰最常见，数据通常一模一样。
//   [R11 2026-10-25] 比对改走 Rust（dismcore_feat_diff）以消除两份实现，
//   目的不是提速（C++ 版已是 O(N) 哈希查表，同量级）。
[[nodiscard]] bool CPageFeature::FeatIsSameAsCurrent(const std::vector<FeatRawEntry>& vRaw)
{
    if (m_vFeatures.empty() || vRaw.empty()) return false;

    std::vector<std::wstring> vOldSys, vNewSys;
    std::vector<uint8_t> vOldEn, vNewEn;
    vOldSys.reserve(m_vFeatures.size());
    vOldEn.reserve(m_vFeatures.size());
    for (const FeatInfo& f : m_vFeatures)
    {
        if (f.sSys.IsEmpty()) continue;   // 虚拟分组无系统名，不参与比对
        vOldSys.push_back(f.sSys.GetData());
        vOldEn.push_back(f.bEnabled ? 1u : 0u);
    }
    vNewSys.reserve(vRaw.size());
    vNewEn.reserve(vRaw.size());
    for (const FeatRawEntry& r : vRaw)
    {
        vNewSys.push_back(r.sSys.GetData());
        vNewEn.push_back(r.bEnabled ? 1u : 0u);
    }

    uint32_t nChanged = 0;
    if (dismcore::FeatDiffCount(vOldSys, vOldEn, vNewSys, vNewEn, nChanged))
        return nChanged == 0;   // 新增/删除/翻转全为 0 才算一致

    // [回退] Rust 不可用（长度不匹配等）时走原 C++ 判定
    if (vRaw.size() > m_vFeatures.size()) return false;
    std::map<std::wstring, bool> mapOld;
    for (const FeatInfo& f : m_vFeatures)
    {
        if (!f.sSys.IsEmpty())
            mapOld[std::wstring(f.sSys.GetData())] = f.bEnabled;
    }
    for (const FeatRawEntry& r : vRaw)
    {
        auto it = mapOld.find(std::wstring(r.sSys.GetData()));
        if (it == mapOld.end() || it->second != r.bEnabled) return false;
    }
    return true;
}

void CPageFeature::OnData(FeatDataMsg* pData)
{
    // [C++17 R3] receiver takes ownership, freed at scope end (was: delete pData at three exits)
    std::unique_ptr<FeatDataMsg> dataGuard(pData);
    m_bFeatLoading = false;

    // [rb23] 枚举已回报：停止旋转环动画与超时看门狗
    ::KillTimer(m_ctx.HostHwnd(), FEAT_RING_TIMER_ID);
    ::KillTimer(m_ctx.HostHwnd(), FEAT_WATCHDOG_TIMER_ID);

    // [R16][O-1] 实测本次枚举耗时，动态设定下次看门狗阈值（×2，
    // 下限 30s/上限 600s；耗时 <1s 视为异常样本，保持原阈值不动）。
    if (m_dwFeatEnumStartTick)
    {
        DWORD dwElapsed = ::GetTickCount() - m_dwFeatEnumStartTick;
        m_dwFeatEnumStartTick = 0;
        if (dwElapsed >= 1000)
        {
            UINT nNew = dwElapsed * 2;
            if (nNew < 30000) nNew = 30000;
            if (nNew > 600000) nNew = 600000;
            m_nFeatWatchdogMs = nNew;
        }
    }

    std::vector<FeatRawEntry> vRaw;

    if (pData)
    {
        vRaw.swap(pData->vRaw);
    }

    int nRc = pData ? pData->nRc : 0;

    if (nRc == 0)
    {
        // [R162] 整段抽为 FeatRenderEnumError（原 60 行、嵌套 5）
        (void)FeatRenderEnumError(pData ? pData->sErr : CDuiString());
        return;
    }

    if (nRc < 0)

    {
        // [修复] 枚举成功但 0 项：旧实现只写日志后直接返回，页面永久停在
        // "正在加载功能列表"，用户以为卡死。给出明确空态（仍可点刷新重试）。
        m_ctx.HostLog(L"[feat] no features returned by DismGetFeatures");

        SetFeatListStatus(m_ctx,
            StringMgr.Get("feat.s_empty_list"), tokens::Token(tokens::EToken::InkMuted));

        SelectFeat(-1);
        ClearDetailPanel();

        return;

    }

    // [T1 P0] 枚举成功：把原始条目落盘到磁盘缓存（stale-while-revalidate），
    // 下次进页若命中缓存即可在 WM_FEAT_DATA 到达前秒开渲染。
    if (FeatureService::SaveFeatCache(vRaw))
    {
        CDuiString sCached;
        sCached.Format(L"[feat] feat cache saved: %d entries",
                       static_cast<int>(vRaw.size()));
        m_ctx.HostLog(sCached.GetData());
    }
    // [B3 2026-10-25] 差异渲染：权威结果与当前列表完全一致时不做重建。
    // [R162] 比对逻辑抽为 FeatIsSameAsCurrent（Rust 优先 + C++ 回退两路都在里面）
    if (FeatIsSameAsCurrent(vRaw)) return;   // 不重建、不清选中、不动滚动与展开态
    ApplyRawData(vRaw);

    CDuiString sLog;
    sLog.Format(L"[feat] loaded %d features", static_cast<int>(vRaw.size()));
    m_ctx.HostLog(sLog.GetData());
}

// ============================================================
// [T1 P0] 公共建树入口：OnData 成功分支与磁盘缓存命中路径共用，
// 保证两条路径渲染结果完全一致（缓存只是数据源不同）。
// ============================================================
void CPageFeature::ApplyRawData(const std::vector<FeatRawEntry>& vRaw)
{
    m_vFeatures.clear();
    InvalidateAggCache();

    // [rb26] 重新枚举后旧索引失效，清空选中态（详情面板随之复位）
    SelectFeat(-1);

    for (size_t i = 0; i < vRaw.size(); ++i)
    {
        FeatInfo f;
        f.sSys     = vRaw[i].sSys;
        f.sDisp    = FeatDisplayName(vRaw[i].sSys);
        f.sDesc    = FeatDescription(vRaw[i].sSys);   // [rb26] 中文说明注解
        f.bEnabled = vRaw[i].bEnabled;
        m_vFeatures.push_back(f);
    }

    // [T8 P3] 建索引：真实功能填完后立即建表，供后续 FindBySys 哈希查表。
    // 必须在 append 虚拟组之前建，否则虚拟组（sSys 为空）会占位但无键、
    // 且真实功能下标才是建树语义要的；虚拟组不进索引。
    BuildSysIndex();

    // link real feature parents
    for (size_t i = 0; i < sizeof(kFeatMeta)/sizeof(kFeatMeta[0]); ++i)
    {
        if (!kFeatMeta[i].pszParent || !*kFeatMeta[i].pszParent) continue;
        int nP = FindBySys(kFeatMeta[i].pszParent);
        int nC = FindBySys(kFeatMeta[i].pszSys);
        if (nP >= 0 && nC >= 0) m_vFeatures[nC].nParent = nP;
    }

    // append virtual display groups, attach their (previously root) children
    for (size_t g = 0; g < sizeof(kVG)/sizeof(kVG[0]); ++g)
    {
        FeatInfo vg;
        vg.sSys.Empty();
        vg.sDisp = kVG[g].pszName;
        vg.sDesc = kVG[g].pszDesc;
        int nVG = static_cast<int>(m_vFeatures.size());
        m_vFeatures.push_back(vg);
        for (int c = 0; c < 8 && kVG[g].pszChild[c]; ++c)
        {
            int nC = FindBySys(kVG[g].pszChild[c]);
            if (nC >= 0 && m_vFeatures[nC].nParent == -1)
                m_vFeatures[nC].nParent = nVG;
        }
    }

    // populate child lists; ascending index = DISM discovery order
    for (size_t i = 0; i < m_vFeatures.size(); ++i)
    {
        int nP = m_vFeatures[i].nParent;
        if (nP >= 0) m_vFeatures[nP].vChild.push_back(static_cast<int>(i));
    }
    for (size_t i = 0; i < m_vFeatures.size(); ++i)
        std::sort(m_vFeatures[i].vChild.begin(), m_vFeatures[i].vChild.end());

    m_bFeatLoaded = true;

    // [fix-dismcore] 建树完成落日志（含缓存路径复用同一入口）
    {
        CDuiString sOk;
        sOk.Format(L"[feat] features built: %d nodes",
                   static_cast<int>(m_vFeatures.size()));
        m_ctx.HostLog(sOk.GetData());
    }

    RebuildList();

    CTabLayoutUI* pPages = static_cast<CTabLayoutUI*>(m_ctx.HostFindControl(L"prog_pages"));
    OnTabChanged(pPages ? pPages->GetCurSel() : 1);
}



// [T8 P3] 页属定时器分发：壳 kTimerTab 表驱动入口
void CPageFeature::OnHostTimer(UINT uId)
{
    if (uId == FEAT_RENDER_BATCH_TIMER_ID)
    {
        ::KillTimer(m_ctx.HostHwnd(), FEAT_RENDER_BATCH_TIMER_ID);
        RebuildListBatchStep();
    }
}


// ============================================================
// P8：第二级事件路由（原 CFrameWnd::Notify 功能页分支逐字迁入。
// 例外：progtab_（跨 Program/Feature）与 btn_prog_refresh/delete
// 共享按钮保留核心壳分派）
// ============================================================

// [R175] 从 OnEvent 抽出：选择功能源目录并回填到编辑框。
//   用户取消选择时什么都不做（不覆盖已有路径）——
//   若取消后也回填空串，用户会以为自己选的路径被清掉了。
void CPageFeature::FeatPickSourceDir()
{
    CDuiString sDir;
    if (!Win32DialogService::DlgBrowseFolder(sDir, StringMgr.Get("misc.sfc6fe03a"))) return;
    CEditUI* pEdit = static_cast<CEditUI*>(m_ctx.HostFindControl(L"feat_src_edit"));
    if (pEdit) pEdit->SetText(sDir.GetData());
}

void CPageFeature::OnEvent(TNotifyUI& msg)
{
    CDuiString sName = msg.pSender ? msg.pSender->GetName() : CDuiString(L"");

    if (msg.sType == L"click")
    {
        // ---- windows feature rows ----
        int nFeatRow = -1;

        if (ParsePrefix1(sName, L"feat_row_", nFeatRow))
        {
            ToggleCheck(nFeatRow);
            return;
        }

        int nFeatAr = -1;

        // [R220] In-row uninstall key. DeleteSelected() operates on m_nFeatSelected,
        //   so set the selection to THIS row first; otherwise clicking row B's button
        //   while row A was selected would uninstall A -- a destructive wrong-target
        //   bug that is easy to miss in testing.
        int nFeatUn = -1;
        if (ParsePrefix1(sName, L"featun_", nFeatUn))
        {
            if (nFeatUn >= 0 && nFeatUn < static_cast<int>(m_vFeatures.size()))
            {
                m_nFeatSelected = nFeatUn;
                DeleteSelected();
            }
            return;
        }

        if (ParsePrefix1(sName, L"feat_arrow_", nFeatAr))
        {
            ToggleExpand(nFeatAr);
            return;
        }

        else if (sName == L"btn_prog_apply")      { Apply(); return; }

        else if (sName == L"btn_feat_browse")
        {
            // [R175] 抽为 FeatPickSourceDir，嵌套 4→3
            FeatPickSourceDir();
            return;
        }
    }

    if (msg.sType == L"dbclick")
    {
        int a = -1;
        if (ParsePrefix1(sName, L"feat_row_", a)) { ToggleCheck(a); return; }
    }
}
// ============================================================
// [R124-2] 行级右键菜单增强：feat_row_{n} 命中 → 弹出行级菜单
// 比统一 EListCtx::LCX_CAP 更精细：可复制 DISM 功能名 / 显示名 / 状态。
// ============================================================
// [R175] 从 OnContextMenu 抽出：从命中控件向上找 feat_row_N，返回有效行号。
//   找不到或行号越界 → 返回 -1（调用方直接 return false）。
//   越界也当"未命中"而非断言：列表可能正在重建，此时旧行名仍短暂存在。
[[nodiscard]] int CPageFeature::FeatRowFromHit(CControlUI* pHit)
{
    for (CControlUI* q = pHit; q; q = q->GetParent())
    {
        const wchar_t* pn = q->GetName();
        if (!pn) continue;
        if (wcsncmp(pn, L"feat_row_", 9) != 0) continue;
        const int n = _ttoi(pn + 9);
        if (n < 0 || n >= static_cast<int>(m_vFeatures.size())) return -1;
        return n;
    }
    return -1;
}

bool CPageFeature::OnContextMenu(POINT pt, bool bFromContextMsg)
{
    if (bFromContextMsg) return false;

    CControlUI* pHit = m_ctx.HostFindControlAt(pt);

    // [R175] 命中行解析抽为 FeatRowFromHit，嵌套 4→3
    const int nRow = FeatRowFromHit(pHit);
    if (nRow < 0) return false;
    ShowFeatContextMenu(nRow, pt);
    return true;
}

CDuiString CPageFeature::FeatStateText(int n)
{
    CDuiString sText;
    if (n < 0 || n >= static_cast<int>(m_vFeatures.size()))
    {
        sText = L"-";
        return sText;
    }

    const FeatInfo& f = m_vFeatures[n];
    int nAgg = Agg(n);   // 0 关 / 1 开 / 2 混合 / 3 永久移除

    if (nAgg == 1)
        sText = StringMgr.Get("feat.s_state_on");
    else if (nAgg == 0)
        sText = StringMgr.Get("feat.s_state_off");
    else if (nAgg == 3)
        sText = StringMgr.Get("feat.s_state_remove");
    else
        sText = StringMgr.Get("feat.s_state_mixed");

    if (!f.bEnabled && nAgg != 1)
        sText = sText;   // 保持描述
    return sText;
}

// ============================================================
// [R-B1] Ctrl+C / Ctrl+X 复制首个功能项系统名
// ============================================================

void CPageFeature::OnGlobalCopy(bool /*bCut*/)
{
    if (m_vFeatures.empty())
    {
        m_ctx.HostToast(StringMgr.Get("feat.s_nocopy"),
                        StringMgr.Get("feat.s_nocopy_body"), 1);
        return;
    }
    CopyFeatSysName(0);
}
