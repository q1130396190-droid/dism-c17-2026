#include "stdafx.h"

// ============================================================
// Page_UnattendIbs.cpp —— 应答文件页「IBS 首次启动注入」子系统。
//
// [R139-1] 从 Page_Unattend.cpp（2393 行）拆出 274 行：
//   BfFbEnablePrivilege / BfFbWriteStageKey / BfFbPersistFirstBoot
//   CPageUnattend::InjectIbsStage（四个注入点编排）
//
// 闭包说明：CPageUnattend::InjectIbsStage 声明见 Page_Unattend.h；
// 其余三个为文件级辅助，段外零引用；RegKeyGuard 原在主文件但仅本段使用，
// 故随段迁入，避免主文件留死代码。
//
// 搬迁为逐字搬运，零逻辑改动。
// ============================================================

#include "Page_Unattend.h"
#include "Page_UnattendInternal.h"   // FileGuard / HandleGuard

#include "OfflineSession.h"       // COfflineSession::IsActive / GetImageRoot
#include "HostInterfaces.h"       // IPageContext / ELogLevel
#include "FormatW.h"              // wfmt::FormatInto
#include "StringManager.h"
#include "Registry.h"          // [R159] AsRegSzBytes（REG_SZ 数据指针单点转换）

#include <windows.h>
#include <string>
#include <vector>

using namespace DuiLib;
using i18n::CStringManager;

// 注册表键 RAII（原主文件 57..67，仅本段使用）
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

// FirstBoot 阶段数（PreSysPrep / PostSysPrep / PreOOBE / PostOOBE）
inline constexpr int kBfFbStageCount = 4;
// FirstBoot 阶段键名前缀（SYSTEM 蜂巢内相对路径）
inline constexpr int kBfFbHiveNameMax = 64;
// 提权名称：注册表蜂巢装载/卸载所需

namespace
{
    // 启用本进程特权（RegLoadKey 需要 SeRestore/SeBackup）
    bool BfFbEnablePrivilege(const wchar_t* pszName)
    {
        HandleGuard hTok;
        if (!::OpenProcessToken(::GetCurrentProcess(),
                                TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, hTok.out()))
            return false;
        LUID uid = { 0 };
        TOKEN_PRIVILEGES tp = { 0 };
        bool bOk = false;
        if (::LookupPrivilegeValue(nullptr, pszName, &uid))
        {
            tp.PrivilegeCount = 1;
            tp.Privileges[0].Luid = uid;
            tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            bOk = ::AdjustTokenPrivileges(hTok, FALSE, &tp, sizeof(tp), nullptr, nullptr)
                  && ::GetLastError() == ERROR_SUCCESS;
        }
        return bOk;
    }
}

// 写入一个 FirstBoot 阶段键（hRoot + 相对子键），默认值 = 命令行
static bool BfFbWriteStageKey(HKEY hRoot, const std::wstring& sSub,
                              const std::wstring& sCmdLine,
                              IPageContext& ctx)
{
    RegKeyGuard hk;
    DWORD dwDisp = 0;
    LSTATUS st = ::RegCreateKeyExW(hRoot, sSub.c_str(), 0, nullptr, 0,
                                   KEY_WRITE | KEY_WOW64_64KEY, nullptr, hk.out(), &dwDisp);
    if (st != ERROR_SUCCESS)
    {
        CDuiString sLog;
        sLog.Format(L"[ibs] RegCreateKeyEx 失败 %ld：%s", (long)st, sSub.c_str());
        ctx.HostLog(sLog, static_cast<int>(ELogLevel::LOG_ERROR));
        return false;
    }
    // [R159] REG_SZ 数据指针：wchar_t* → BYTE* 收敛到 Registry.h 的 AsRegSzBytes
    const BYTE* pData = AsRegSzBytes(sCmdLine.c_str());
    DWORD cbData = (DWORD)((sCmdLine.size() + 1) * sizeof(wchar_t));
    st = ::RegSetValueExW(hk, nullptr, 0, REG_SZ, pData, cbData);
    if (st != ERROR_SUCCESS)
    {
        CDuiString sLog;
        sLog.Format(L"[ibs] RegSetValueEx 失败 %ld：%s", (long)st, sSub.c_str());
        ctx.HostLog(sLog, static_cast<int>(ELogLevel::LOG_ERROR));
        return false;
    }
    return true;
}

// 把某阶段命令持久化到离线 SYSTEM 蜂巢的 Setup\FirstBoot\<阶段>
// [R164] 命令行拼装（从 BfFbPersistFirstBoot 抽出，原含 for + if 共 2 个判定点）。
// 单条原样；多条用 cmd /c "a && b" 链式执行。
[[nodiscard]] static std::wstring BfFbBuildCommandLine(const std::vector<DuiLib::CDuiString>& vCmds)
{
    if (vCmds.size() == 1) return vCmds[0].GetData();
    std::wstring sCmd = L"cmd /c \"";
    for (size_t i = 0; i < vCmds.size(); ++i)
    {
        if (i) sCmd += L" && ";
        sCmd += vCmds[i].GetData();
    }
    sCmd += L"\"";
    return sCmd;
}

// [R164] 离线 SYSTEM 蜂巢路径解析 + 存在性校验（从 BfFbPersistFirstBoot 抽出）。
// 返回 false 表示路径为空或蜂巢不存在（已写日志），调用方直接返回。
[[nodiscard]] static bool BfFbResolveHivePath(DuiLib::CDuiString sRoot,
                                              std::wstring& sHive, IPageContext& ctx)
{
    sHive = sRoot.GetData();
    if (!sHive.empty() && sHive.back() != L'\\' && sHive.back() != L'/')
        sHive += L'\\';
    sHive += L"Windows\\System32\\config\\SYSTEM";
    if (::GetFileAttributesW(sHive.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    CDuiString sLog;
    sLog.Format(L"[ibs] 离线 SYSTEM 蜂巢不存在：%s，FirstBoot 落键跳过", sHive.c_str());
    ctx.HostLog(sLog, static_cast<int>(ELogLevel::LOG_WARN));
    return false;
}

static void BfFbPersistFirstBoot(int nStage,
                                 const std::vector<DuiLib::CDuiString>& vCmds,
                                 IPageContext& ctx)
{
    static const wchar_t* kFbNames[4] = {
        L"PreSysPrep", L"PostSysPrep", L"PreOOBE", L"PostOOBE"
    };
    if (nStage < 0 || nStage > 3 || vCmds.empty()) return;

    // 知识库功能矩阵：部署注入 FirstBoot 钩子仅离线/WinPE 侧
    if (!COfflineSession::IsActive())
    {
        ctx.HostLog(L"[ibs] 在线会话跳过 FirstBoot 注册表落键（仅离线映像持久化）");
        return;
    }

    CDuiString sRoot = COfflineSession::GetImageRoot();
    sRoot.Trim();
    if (sRoot.IsEmpty())
    {
        ctx.HostLog(L"[ibs] 离线根为空，FirstBoot 落键跳过", static_cast<int>(ELogLevel::LOG_WARN));
        return;
    }

    const std::wstring sCmd = BfFbBuildCommandLine(vCmds);
    std::wstring sHive;
    if (!BfFbResolveHivePath(sRoot, sHive, ctx)) return;

    BfFbEnablePrivilege(SE_RESTORE_NAME);
    BfFbEnablePrivilege(SE_BACKUP_NAME);

    std::wstring sMount(64, L'\0');
    wfmt::FormatInto(&sMount[0], sMount.size(),  L"DismPP_FB_%08lX", ::GetTickCount());
    sMount.resize(wcslen(sMount.c_str()));

    const LSTATUS st = ::RegLoadKeyW(HKEY_LOCAL_MACHINE, sMount.c_str(), sHive.c_str());
    if (st != ERROR_SUCCESS)
    {
        CDuiString sLog;
        sLog.Format(L"[ibs] RegLoadKey 失败 %ld：%s（需管理员权限）",
                    (long)st, sHive.c_str());
        ctx.HostLog(sLog, static_cast<int>(ELogLevel::LOG_ERROR));
        return;
    }

    std::wstring sSub = sMount;
    sSub += L"\\Setup\\FirstBoot\\";
    sSub += kFbNames[nStage];
    const bool bOk = BfFbWriteStageKey(HKEY_LOCAL_MACHINE, sSub, sCmd, ctx);

    // 必须先关闭所有子键句柄才能卸载蜂巢（BfFbWriteStageKey 内已关闭）
    const LRESULT stU = ::RegUnLoadKeyW(HKEY_LOCAL_MACHINE, sMount.c_str());
    if (stU != ERROR_SUCCESS)
    {
        CDuiString sLog;
        sLog.Format(L"[ibs] RegUnLoadKey 失败 %ld，蜂巢改动可能未落盘", (long)stU);
        ctx.HostLog(sLog, static_cast<int>(ELogLevel::LOG_ERROR));
        return;
    }

    if (!bOk) return;
    CDuiString sLog;
    sLog.Format(L"[ibs] FirstBoot 落键成功：System\\Setup\\FirstBoot\\%s（%d 条命令）",
                kFbNames[nStage], static_cast<int>(vCmds.size()));
    ctx.HostLog(sLog);
}

// ============================================================
// [R-B29] IBSProvider 四注入点：PreSysPrep / PostSysPrep / PreOobe / PostOobe
// ------------------------------------------------------------
// 基线（缺失与完善清单_20261006 维度 10 缺失 1）：原版 IBSProvider 在
// sysprep 执行前后、首次 OOBE 前后向应答文件注入同步命令节点。
// 本函数在四个时机把当前四阶段配置（m_vUcCmds[0..3]）追加写入
// %SystemRoot%\Panther\unattend.xml 对应 pass 的 RunSynchronous/命令节点，
// 形成"注入脚本/注入时机"闭环；nStage 取值 0..3。
// ============================================================
// [R162] 从 InjectIbsStage 抽出：读取 %SystemRoot%\Panther\unattend.xml。
//   文件不存在 / 打开失败 → sExisting 保持空，调用方据此走"建骨架"分支。
//   上限 16MB：Panther 下的 unattend.xml 正常只有几十 KB，
//   超过这个量级基本可以断定不是我们要处理的文件，整读会吃掉大量内存。
void CPageUnattend::UcReadPantherXml(const CDuiString& sPath, std::string& sExisting)
{
    sExisting.clear();
    FileGuard hR = ::CreateFile(sPath.GetData(), GENERIC_READ, FILE_SHARE_READ,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hR == INVALID_HANDLE_VALUE) return;
    const DWORD sz = ::GetFileSize(hR, nullptr);
    if (sz == 0 || sz >= 16u * 1024u * 1024u) return;
    sExisting.resize(sz);
    DWORD rd = 0;
    ::ReadFile(hR, &sExisting[0], sz, &rd, nullptr);
    sExisting.resize(rd);
}

// [R162] 从 InjectIbsStage 抽出：拼 RunSynchronous 注入片段。
//   Order 从 1 开始，按命令在原组内的顺序排列 —— 顺序即执行顺序，不能重排。
void CPageUnattend::UcBuildRunSyncFragment(int nStage, CDuiString& sInject)
{
    sInject.Empty();
    sInject += L"    <RunSynchronous>\r\n";
    for (size_t i = 0; i < m_vUcCmds[nStage].size(); ++i)
    {
        CDuiString sLine;
        sLine.Format(L"      <RunSynchronousCommand Action=\"add\">\r\n"
                     L"        <Order>%d</Order>\r\n"
                     L"        <CommandLine>%s</CommandLine>\r\n"
                     L"      </RunSynchronousCommand>\r\n",
                     static_cast<int>(i + 1), m_vUcCmds[nStage][i].GetData());
        sInject += sLine;
    }
    sInject += L"    </RunSynchronous>\r\n";
}

// [R168] 从 InjectIbsStage 抽出：把注入片段合进目标 unattend.xml。
//   两种情形：
//     · 原文件不存在 → 以最小骨架创建（xml 声明 + unattend 根 + 该 pass 的 settings）
//     · 原文件存在   → 把片段插到第一个 </settings> 之前；找不到则退到 </unattend> 之前
//   两处锚点都找不到说明结构不可识别 —— 返回 false 并记 ERROR，调用方放弃本次注入。
//   **不做 XML DOM 解析**：目标文件可能是用户手改过的、带注释或非标准缩进，
//   字符串插入对这类文件更宽容；代价是依赖 </settings> 这个标记存在。
bool CPageUnattend::UcComposeInjectedXml(const std::string& sExisting,
                                         const CDuiString& sInject, int nStage,
                                         std::string& sXml)
{
    // [R-B60] pass 名映射：与 kUcMap / UI 分组严格对齐。
    //   generalize 在 unattend.xml 中不存在，PostSysPrep 对应 auditSystem。
    static const wchar_t* kPassNames[4] = {
        L"windowsPE",    // PreSysPrep
        L"auditSystem",  // PostSysPrep（sysprep /generalize /oobe 完成后）
        L"oobeSystem",   // PreOobe
        L"specialize",   // PostOobe（OOBE 完成后首次登录前）
    };

    if (sExisting.empty())
    {
        CDuiString sHead;
        sHead.Format(L"<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
                     L"<unattend xmlns=\"urn:schemas-microsoft-com:unattend\">\r\n"
                     L"  <settings pass=\"%s\">\r\n",
                     kPassNames[nStage]);
        sHead += sInject;
        sHead += L"  </settings>\r\n</unattend>\r\n";

        const int nLen = ::WideCharToMultiByte(CP_UTF8, 0, sHead.GetData(), -1,
                                               nullptr, 0, nullptr, nullptr);
        if (nLen <= 0) return false;
        sXml.assign(static_cast<size_t>(nLen), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, sHead.GetData(), -1, &sXml[0], nLen, nullptr, nullptr);
        return true;
    }

    sXml = sExisting;
    size_t nPos = sXml.find("</settings>");
    if (nPos == std::string::npos) nPos = sXml.find("</unattend>");
    if (nPos == std::string::npos)
    {
        m_ctx.HostLog(L"[ibs] unattend.xml 结构不可识别，注入失败",
                      static_cast<int>(ELogLevel::LOG_ERROR));
        return false;
    }

    const int nLen = ::WideCharToMultiByte(CP_UTF8, 0, sInject.GetData(), -1,
                                           nullptr, 0, nullptr, nullptr);
    if (nLen <= 0) return false;
    std::string sIns(static_cast<size_t>(nLen), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, sInject.GetData(), -1, &sIns[0], nLen, nullptr, nullptr);
    sXml.insert(nPos, sIns);
    return true;
}

// [R168] 从 InjectIbsStage 抽出：带 UTF-8 BOM 写回。
//   [R99] 写盘结果如实判定，不再无条件当成功 —— 但失败只记 WARN 不 return：
//   调用方后续还要做双通道持久化（注册表侧），那部分可能成功。
//   缓冲直接用 std::string：BOM 三字节 + 内容一次拼好，不再 vector<BYTE> + copy。
[[nodiscard]] bool CPageUnattend::UcWriteXmlUtf8Bom(const CDuiString& sPath,
                                                    const std::string& sXml)
{
    std::string buf;
    buf.reserve(sXml.size() + 3);
    buf.push_back(static_cast<char>(0xEF));
    buf.push_back(static_cast<char>(0xBB));
    buf.push_back(static_cast<char>(0xBF));
    buf.append(sXml);

    FileGuard h(::CreateFile(sPath.GetData(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (h == INVALID_HANDLE_VALUE)
    {
        m_ctx.HostLog(L"[ibs] 无法打开 unattend.xml 写入",
                      static_cast<int>(ELogLevel::LOG_WARN));
        return false;
    }
    DWORD dwW = 0;
    const DWORD cbWant = static_cast<DWORD>(buf.size());
    if (!::WriteFile(h, buf.data(), cbWant, &dwW, nullptr) || dwW != cbWant)
    {
        m_ctx.HostLog(L"[ibs] unattend.xml 写回失败",
                      static_cast<int>(ELogLevel::LOG_WARN));
        return false;
    }
    return true;
}

void CPageUnattend::InjectIbsStage(int nStage)
{
    if (nStage < 0 || nStage > 3) return;

    // [R-B60] 四注入点 Pass 映射修正：与 kUcMap / UI 分组（WindowsPE/Specialize/
    //         OOBESystem/AuditSystem）严格对齐。
    //   * 旧实现 kPassNames 用 generalize / specialize 与 kUcMap 的 auditSystem 不一致，
    //     且 generalize 在 Windows unattend.xml 中**不存在**（MSDN：generalize pass 无效，
    //     R31 已修正 kUcMap 第 4 组为 auditSystem）。
    //   * PostSysPrep 语义 = sysprep /generalize /oobe 完成后，实际生效的是 auditSystem
    //     阶段（sysprep 完成 generalize 后进入 auditSystem），故 stage 1 用 auditSystem。
    //   * PostOobe 语义 = 首次 OOBE 完成后首次登录前，对应 specialize 阶段。
    static const wchar_t* kPassNames[4] = {
        L"windowsPE",    // PreSysPrep（注入 windowsPE 的 RunSynchronous 前段）
        L"auditSystem",  // PostSysPrep（sysprep /generalize /oobe 完成后 auditSystem 阶段）
        L"oobeSystem",   // PreOobe（首次 OOBE 前）
        L"specialize",   // PostOobe（OOBE 完成后首次登录前）
    };
    (void)0;

    if (m_vUcCmds[nStage].empty())
    {
        m_ctx.HostLog(L"[ibs] stage empty, skip injection");
        return;
    }

    // 1) 读取现有 %SystemRoot%\Panther\unattend.xml（若存在）
    wchar_t szWin[MAX_PATH] = { 0 };
    ::GetWindowsDirectory(szWin, MAX_PATH);
    CDuiString sPanther = CDuiString(szWin) + L"\\Panther";
    ::CreateDirectory(sPanther.GetData(), nullptr);
    CDuiString sPath = sPanther;
    sPath += L"\\unattend.xml";

    // [R162] 读现有文件抽为 UcReadPantherXml
    std::string sExisting;
    UcReadPantherXml(sPath, sExisting);

    // 2) 组装注入 XML 片段（RunSynchronous + RunSynchronousCommand 节点）
    // [R162] 片段拼装抽为 UcBuildRunSyncFragment
    CDuiString sInject;
    UcBuildRunSyncFragment(nStage, sInject);

    // 3)+4) 组装目标 XML 并按 UTF-8 BOM 写回
    // [R168] 两段各抽一个函数（105→约 45 行）
    std::string sXml;
    if (!UcComposeInjectedXml(sExisting, sInject, nStage, sXml)) return;
    (void)UcWriteXmlUtf8Bom(sPath, sXml);

    CDuiString sLog;
    sLog.Format(L"[ibs] stage=%d pass=%s injected %d cmds",
                nStage, kPassNames[nStage], static_cast<int>(m_vUcCmds[nStage].size()));
    m_ctx.HostLog(sLog.GetData());

    // [R-B79] 33号缺口：同一批命令双通道持久化——离线会话下写入
    // System\Setup\FirstBoot\{PreSysPrep,PostSysPrep,PreOOBE,PostOOBE}
    // （IbsSetFirstBootCommandLine 注册表侧；在线会话内部自行跳过）
    BfFbPersistFirstBoot(nStage, m_vUcCmds[nStage], m_ctx);
}
