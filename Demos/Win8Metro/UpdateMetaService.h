#pragma once
// ============================================================
// UpdateMetaService.h —— [R-B8] 更新机制闭环服务
// ------------------------------------------------------------
// 三项审计缺口（Dism复刻审计报告_知识库基线_20261003.md §C 12/13）：
//   1) UpdateInfo.zip 四字段（Keys/URLS/Information/Tag）解析
//   2) CBS 独占请求倒计时对话框（"暂不安装（推荐）/继续安装"）
//   3) wsusscn2.cab / wsusscn3.cab 离线扫描数据随包保障
// 全部为静态纯函数，无宿主依赖，便于 Page_Update 一行接入。
// ============================================================

#include <windows.h>
#include <vector>
#include <string>

class UpdateMetaService
{
public:
    // ------------------------------------------------------------
    // UpdateInfo 四字段结构
    // ------------------------------------------------------------
    struct UpdateMeta
    {
        wchar_t  szKeys[512];          // <Keys>      导出密钥（含导出地址）
        wchar_t  szUrls[2048];         // <URLS>      更新服务器地址列表
        wchar_t  szInfo[1024];         // <Information> 更新说明
        wchar_t  szTag[256];           // <Tag>       版本标签
        DWORD  dwMaxDownloadThread;  // MaxDownloadThread（缺省 10）
        bool   bValid;

        UpdateMeta() : dwMaxDownloadThread(10), bValid(false)
        {
            szKeys[0] = szUrls[0] = szInfo[0] = szTag[0] = L'\0';
        }
    };

    // ------------------------------------------------------------
    // 解压 UpdateInfo.zip 到临时目录，解析 UpdateInfo.xml 四字段。
    // 返回 false 表示解析失败（调用方可按降级路径继续）。
    // ------------------------------------------------------------
    static bool ParseUpdateInfoZip(const wchar_t* pszZipPath, UpdateMeta& out);

    // ------------------------------------------------------------
    // CBS 独占请求倒计时对话框。
    //   文本：sText；标题：sTitle；倒计时：nTimeoutSec（归零自动选"暂不安装"）。
    //   返回 IDYES = 继续安装；IDCANCEL = 暂不安装（推荐）。
    // 使用独立顶层窗口 + 本地模态消息循环，不阻塞主消息泵；
    // 调用期间暂时禁用父窗口（hParent 可为 nullptr）。
    // ------------------------------------------------------------
    static int ShowExclusiveCountdown(HWND hParent, const wchar_t* pszTitle,
                                      const wchar_t* pszText, int nTimeoutSec = 30);

    // ------------------------------------------------------------
    // wsusscn2.cab / wsusscn3.cab 随包保障。
    //   在 pszExeDir 下检查两个 cab 文件；缺失则经 PowerShell
    //   Invoke-WebRequest 从微软官方目录下载。返回是否有全部文件。
    //   失败不抛错，仅记录日志（由 Page_Update 层输出）。
    // ------------------------------------------------------------
    static bool EnsureOfflineCabs(const wchar_t* pszExeDir, bool bDownloadIfMissing = true);

    // ------------------------------------------------------------
    // [R-B50] 下载线程数上限持久化（MaxDownloadThread 真实消费闸门）
    //   ReadEffectiveMaxThread：优先级 config.ini [Update] MaxThread > UpdateInfo.xml 解析值 > 默认 10。
    //   WriteEffectiveMaxThread：把用户选择写入 config.ini [Update] MaxThread，clamp 到 [1,128]。
    // 均纯 Win32（GetPrivateProfileStringW / WritePrivateProfileStringW），无宿主依赖。
    // ------------------------------------------------------------
    static DWORD ReadEffectiveMaxThread(const wchar_t* pszIniPath,
                                        DWORD dwFromMetaDefault = 10);
    static bool  WriteEffectiveMaxThread(const wchar_t* pszIniPath, DWORD dwThreads);

    // ============================================================
    // [R-B76] 本地补丁包安装（知识库：更新页"添加"本地补丁入口）
    //   pszPkg       .msu / .cab 完整路径；
    //   pszImageRoot 离线映像根（nullptr/空串 = 在线系统）；
    //   原生化：DismAddPackage 进程内直连（API 原生支持 .msu/.cab），
    //   无 expand.exe/dism.exe 子进程；ERROR_SUCCESS_REBOOT_REQUIRED 按成功。
    // ============================================================
    struct CbsPkgRecord
    {
        wchar_t szIdentity[512];   // PackageName（Package_for_KB...~31bf...）
        wchar_t szState[64];       // 固定 "Installed"（仅收录已安装包）
        wchar_t szRelType[64];     // Critical Update / Security Update ... 英文展示名
        CbsPkgRecord() { szIdentity[0] = szState[0] = szRelType[0] = L'\0'; }
    };
    static bool InstallLocalPackage(const wchar_t* pszPkg, const wchar_t* pszImageRoot,
                                    std::wstring* pErr);
    // 原生化：DismGetPackages 结构化枚举（离线可定向 /Image 根）
    static bool EnumerateInstalledCbsPackages(const wchar_t* pszImageRoot,
                                              std::vector<CbsPkgRecord>& vOut);

    // ============================================================
    // [R-B77] Range 断点续传下载器（知识库 Dism++WusuDownload 任务语义）
    //   * 任务目录 UpdateCache，任务前缀 Dism++WusuDownload-<hash8>；
    //   * 落盘 <dst>.pending，Range: bytes=<cur>- 续传，200 全量重下；
    //   * job.xml 记录 Url/MD5/Size/State 与 express.psf.cix.xml/$dpx$；
    //   * psf 差异经 msdelta.dll!ApplyDeltaW 应用，失败自动降级全量下载；
    //   * pszExpectedMd5 非空时完成后 CALG_MD5 校验。
    //   DownloadWithResume：Range 引擎失败时回退 PowerShell IWR（引擎降级）。
    // ============================================================
    static bool RangeDownloadFile(const wchar_t* pszUrl, const wchar_t* pszDst,
                                  const wchar_t* pszExpectedMd5, DWORD dwTimeoutMs);
    static bool DownloadWithResume(const wchar_t* pszUrl, const wchar_t* pszDst);

    // ============================================================
    // [R-B78] 回传通道（UpdateInfo 解析后 Key 回传 / UpdateReport /
    //   CrashReport）。pszEndpoint 为 UpdateInfo.xml <URLS> 首地址；
    //   空地址或发送失败时落盘到 pszOutboxDir 排队，FlushReportOutbox
    //   下次启动补发。Crash 过滤器写出的崩溃 XML 也进同一 outbox。
    // ============================================================
    static bool PostReportEvent(const wchar_t* pszEndpoint, const wchar_t* pszKind,
                                const wchar_t* pszXml, const wchar_t* pszOutboxDir,
                                DWORD dwTimeoutMs = 8000);
    static bool FlushReportOutbox(const wchar_t* pszEndpoint, const wchar_t* pszOutboxDir);
    static void InstallCrashReporter(const wchar_t* pszExeDir);
};
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
