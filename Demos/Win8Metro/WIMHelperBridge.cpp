// ============================================================================
// WIMHelperBridge.cpp - FrameWnd <-> WIMHelper 桥接实现（R30 v4，接口对齐版）
// ============================================================================
// 与 WIMHelperBridge.h 配对。所有调用均严格使用 WIMHelper.h 实际声明的
// 方法名，确保 patch 应用后可直接编译通过。
// ============================================================================

#include "stdafx.h"
#include "WIMHelperBridge.h"
#include <mutex>
#include <chrono>
#include <cstring>
#include <sstream>
#include <wtsapi32.h>

#include "MountLedgerService.h" // R-B6：挂载/应用注册表记账

using namespace std::chrono;

// ============================================================================
// 内部状态
// ============================================================================
namespace {

// [C++17 升级阶段2] WIM 句柄 RAII 守卫：隐式转换为 WIMHandle，现有调用点
// 零改动；只负责早退/异常路径自动 CloseHandle，不改变任何调用顺序与业务
// 分支。跨函数移交所有权请用 release() 并注释去向。
struct WimHandleGuard
{
    WimHandleGuard() = default;
    WimHandleGuard(WIMHandle h) : m_h(h) {}
    ~WimHandleGuard() { close(); }
    WimHandleGuard(const WimHandleGuard&) = delete;
    WimHandleGuard& operator=(const WimHandleGuard&) = delete;
    WimHandleGuard& operator=(WIMHandle h) { reset(h); return *this; }
    operator WIMHandle() const { return m_h; }
    bool valid() const { return m_h != nullptr && m_h != INVALID_HANDLE_VALUE; }
    WIMHandle release() { WIMHandle h = m_h; m_h = nullptr; return h; }
    void reset(WIMHandle h) { close(); m_h = h; }
private:
    void close() { if (m_h != nullptr && m_h != INVALID_HANDLE_VALUE) CWIMHelper::CloseHandle(m_h); m_h = nullptr; }
    WIMHandle m_h = nullptr;
};

// [C++17 升级阶段2] WIM 内文件枚举句柄 RAII（FindClose）。
struct WimFindGuard
{
    WimFindGuard() = default;
    WimFindGuard(HANDLE h) : m_h(h) {}
    ~WimFindGuard() { if (m_h != nullptr && m_h != INVALID_HANDLE_VALUE) ::FindClose(m_h); }
    WimFindGuard(const WimFindGuard&) = delete;
    WimFindGuard& operator=(const WimFindGuard&) = delete;
    operator HANDLE() const { return m_h; }
    bool valid() const { return m_h != nullptr && m_h != INVALID_HANDLE_VALUE; }
private:
    HANDLE m_h = nullptr;
};

// [C++17 升级阶段2] 普通 Win32 文件句柄 RAII（::CloseHandle）。
struct RawHandleGuard
{
    RawHandleGuard() = default;
    RawHandleGuard(HANDLE h) : m_h(h) {}
    ~RawHandleGuard() { if (m_h != nullptr && m_h != INVALID_HANDLE_VALUE) ::CloseHandle(m_h); }
    RawHandleGuard(const RawHandleGuard&) = delete;
    RawHandleGuard& operator=(const RawHandleGuard&) = delete;
    operator HANDLE() const { return m_h; }
    bool valid() const { return m_h != nullptr && m_h != INVALID_HANDLE_VALUE; }
private:
    HANDLE m_h = nullptr;
};

std::mutex             g_logMutex;
std::vector<WIMLogEntry> g_logBuffer;
size_t                 g_logMaxEntries = 4096;

WIMBridge::WIMProgressFn  g_progressFn  = nullptr;
void*                     g_progressCtx = nullptr;

// wimgapi 消息回调（知识库 41 §24.2 步骤6）：转发进度到任务接收器。
// pvUserData 即各 *Ex 注册时传入的 IWimProgress*（每任务独立、线程安全）；
// 为空时回退到旧的全局 g_progressFn（兼容旧测试）。
// 返回 WIM_MSG_SUCCESS(0) 表示继续处理；出错消息仅记录不阻断。
DWORD WINAPI BridgeMsgProc(DWORD dwMsgId, WPARAM wParam, LPARAM lParam,
                           void* pvUserData)
{
    constexpr DWORD kWimMsgError = 0x00000003;
    constexpr DWORD kWimMsgProgress = 0x00000009;
    (void)lParam;
    if (dwMsgId == kWimMsgProgress)
    {
        WIMBridge::IWimProgress* pTask = static_cast<WIMBridge::IWimProgress*>(pvUserData);
        if (pTask)
            pTask->OnWimProgress(static_cast<DWORD>(wParam), dwMsgId);
        else if (g_progressFn)
            g_progressFn(static_cast<DWORD>(wParam), L"apply", std::wstring());
    }
    return 0;   // WIM_MSG_SUCCESS
}

// RAII 小工具：在单个 wimgapi 句柄上挂载/卸载进度回调
struct ScopedMsgCallback
{
    WIMHandle       hWim;
    WIMBridge::IWimProgress*   pProg;
    bool            bRegistered;
    ScopedMsgCallback(WIMHandle h, WIMBridge::IWimProgress* p)
        : hWim(h), pProg(p), bRegistered(false)
    {
        if (pProg && hWim && hWim != INVALID_HANDLE_VALUE)
        {
            CWIMHelper::RegisterCallback(hWim, BridgeMsgProc, pProg);
            bRegistered = true;
        }
    }
    ~ScopedMsgCallback()
    {
        if (bRegistered)
            CWIMHelper::UnregisterCallback(hWim, BridgeMsgProc);
    }
};

void AppendLog(const WIMLogEntry& e)
{
    std::lock_guard<std::mutex> lk(g_logMutex);
    g_logBuffer.push_back(e);
    if (g_logBuffer.size() > g_logMaxEntries)
        g_logBuffer.erase(g_logBuffer.begin());
}

SYSTEMTIME NowST()
{
    SYSTEMTIME st; GetLocalTime(&st); return st;
}

} // anonymous namespace

// ============================================================================
// 生命周期
// ============================================================================
namespace WIMBridge {

bool Init(const std::wstring& logPath)
{
    if (!CWIMHelper::IsAvailable()) return false;
    if (!logPath.empty())
        CWIMHelper::RegisterLogFile(logPath, 0);  // 失败不阻塞
    return true;
}

bool Shutdown()
{
    {
        std::lock_guard<std::mutex> lk(g_logMutex);
        g_logBuffer.clear();
    }
    return true;
}

bool IsReady()
{
    return CWIMHelper::IsAvailable();
}

void Log(LogLevel level, const std::wstring& msg)
{
    WIMLogEntry e{};
    e.ts    = NowST();
    e.level = static_cast<DWORD>(level);
    e.text  = const_cast<wchar_t*>(msg.c_str());
    AppendLog(e);
}

std::vector<WIMLogEntry> SnapshotLog()
{
    std::lock_guard<std::mutex> lk(g_logMutex);
    return g_logBuffer;
}

void SetProgressCallback(WIMProgressFn fn, void* ctx)
{
    g_progressFn  = fn;
    g_progressCtx = ctx;
}

// ============================================================================
// 全版本产品矩阵（Win7~11 客户端 / Server 2003~2026 服务器）
// 原理（知识库 41）：WIM 是与 OS 版本解耦的容器，VERSION/EDITIONID 由捕获
// 时写入 XML；同一 Build 可对应客户端/服务器（如 17763=1809/2019），故必须
// 结合 EDITIONID 判定。未知高 Build 前向兼容，绝不误判为低版本。
// ========================================================================= namespace
namespace {

bool EditionLooksServer(const std::wstring& e)
{
    static const wchar_t* kMark[] = {
        L"Server", L"Solution", L"Cloud", L"Datacenter"
    };
    for (auto p : kMark)
        if (e.find(p) != std::wstring::npos) return true;
    return e == L"Standard";
}

std::wstring EditionCn(const std::wstring& e)
{
    bool bN = (e.size() >= 1 && e[e.size() - 1] == L'N');
    std::wstring r;
    if (e.find(L"ProfessionalWorkstation") != std::wstring::npos) r = L"专业工作站版";
    else if (e.find(L"ProfessionalEducation") != std::wstring::npos) r = L"专业教育版";
    else if (e.find(L"Professional") != std::wstring::npos) r = L"专业版";
    else if (e.find(L"Education") != std::wstring::npos) r = L"教育版";
    else if (e.find(L"EnterpriseS") != std::wstring::npos) r = L"企业版 LTSC";
    else if (e.find(L"Enterprise") != std::wstring::npos) r = L"企业版";
    else if (e.find(L"Core") != std::wstring::npos) r = L"家庭版";
    else if (e.find(L"ServerDatacenter") != std::wstring::npos) r = L"Datacenter";
    else if (e.find(L"ServerStandard") != std::wstring::npos) r = L"Standard";
    else if (e.find(L"ServerSolution") != std::wstring::npos) r = L"Essentials";
    else if (e.find(L"CloudStorage") != std::wstring::npos) r = L"Cloud Storage";
    else r = e;
    if (bN && r.find(L"N") == std::wstring::npos) r += L" N";
    return r;
}

// Win10/11 客户端发布名（按 Build）
std::wstring ClientRelease(int b, const std::wstring& e)
{
    if (e == L"EnterpriseS")
    {
        switch (b) {
        case 10240: return L"LTSB 2015";
        case 14393: return L"LTSB 2016";
        case 17763: return L"LTSC 2019";
        case 19044: return L"LTSC 2021";
        case 26100: return L"LTSC 2024";
        default:    return L"LTSC Build " + std::to_wstring(b);
        }
    }
    switch (b) {
    case 10240: return L"1507";
    case 10586: return L"1511";
    case 14393: return L"1607";
    case 15063: return L"1703";
    case 16299: return L"1709";
    case 17134: return L"1803";
    case 17763: return L"1809";
    case 18362: return L"1903";
    case 18363: return L"1909";
    case 19041: return L"2004";
    case 19042: return L"20H2";
    case 19043: return L"21H1";
    case 19044: return L"21H2";
    case 22000: return L"21H2";
    case 22621: return L"22H2";
    case 22631: return L"23H2";
    case 26100: return L"24H2";
    default:    return L"Build " + std::to_wstring(b);
    }
}

} // namespace

// [R124-3] 按 (主版本*10+次版本) 与 build 号映射产品名与版本号。
//  switch 长链改为常量表 + 少量特殊分支，降低圈复杂度且保持逐分支等价。
namespace {

// 主版本键 → 产品名；release 由 build 号规则单独决定。
struct SpanTable { int key; const wchar_t* product; };
constexpr SpanTable kSrvBySpan[] = {
    { 52, L"Windows Server 2003" },
    { 60, L"Windows Server 2008" },
    { 61, L"Windows Server 2008 R2" },
    { 62, L"Windows Server 2012" },
    { 63, L"Windows Server 2012 R2" },
};
constexpr SpanTable kCliBySpan[] = {
    { 51, L"Windows XP" },
    { 52, L"Windows XP x64 Edition" },
    { 60, L"Windows Vista" },
    { 61, L"Windows 7" },
    { 62, L"Windows 8" },
    { 63, L"Windows 8.1" },
};
constexpr SpanTable kSrvByBuild[] = {
    { 14393, L"Windows Server 2016" },
    { 17763, L"Windows Server 2019" },
    { 20348, L"Windows Server 2022" },
    { 26100, L"Windows Server 2025" },
};

// 在表里查 key；命中返回产品名，未命中返回 nullptr。
const wchar_t* LookupSpan(const SpanTable* pTab, size_t n, int key)
{
    for (size_t i = 0; i < n; ++i)
        if (pTab[i].key == key) return pTab[i].product;
    return nullptr;
}

// 旧世代产品的 SP 级别：6001/6002 → SP1/SP2，7601 → SP1，2600 → SP3。
std::wstring OldGenRelease(int b)
{
    if (b >= 6002) return L"SP2";
    if (b == 6001) return L"SP1";
    if (b >= 7601) return L"SP1";
    if (b >= 2600) return L"SP3";
    return L"";
}

} // namespace

static void ResolveProductName(ProductIdentity& pi, bool bServer, int M, int m, int b,
                               const std::wstring& version, const std::wstring& editionId)
{
    const int span = M * 10 + m;
    const wchar_t* pName = nullptr;

    if (bServer)
    {
        pName = LookupSpan(kSrvBySpan, std::size(kSrvBySpan), span);
        if (pName != nullptr)
        {
            pi.product = pName;
            // 2003/2008/2008R2 需要按 build 细化 SP；2012 系无 SP。
            if (span == 60 || span == 61 || span == 52) pi.release = OldGenRelease(b);
            return;
        }
        if (M != 10)
        {
            pi.product = L"Windows Server";
            pi.release = version.empty() ? L"" : (L"Build " + pi.build);
            return;
        }
        pName = LookupSpan(kSrvByBuild, std::size(kSrvByBuild), b);
        pi.product = (pName != nullptr) ? pName : L"Windows Server";
        if (pName == nullptr) pi.release = L"Build " + std::to_wstring(b);
        return;
    }

    pName = LookupSpan(kCliBySpan, std::size(kCliBySpan), span);
    if (pName != nullptr)
    {
        pi.product = pName;
        if (span == 60 || span == 61 || span == 51) pi.release = OldGenRelease(b);
        return;
    }
    if (M != 10)
    {
        pi.product = L"Windows";
        pi.release = version.empty() ? L"" : (L"Build " + pi.build);
        return;
    }
    pi.release = ClientRelease(b, editionId);
    // LTSC 2024 虽为 Build 26100，品牌仍是 Windows 10
    pi.product = (editionId == L"EnterpriseS" || b < 22000) ? L"Windows 10" : L"Windows 11";
}


ProductIdentity ResolveProduct(const std::wstring& version,
                               const std::wstring& editionId,
                               const std::wstring& arch)
{
    ProductIdentity pi;
    pi.arch = arch;

    int M = 0, m = 0, b = 0, u = 0;
    swscanf_s(version.c_str(), L"%d.%d.%d.%d", &M, &m, &b, &u);
    pi.build = u > 0 ? std::to_wstring(b) + L"." + std::to_wstring(u)
                     : std::to_wstring(b);

    const bool bServer = EditionLooksServer(editionId) || b == 20348;
    pi.family  = bServer ? L"服务器" : L"客户端";
    pi.editionCn = EditionCn(editionId);

    ResolveProductName(pi, bServer, M, m, b, version, editionId);


    // 一句话汇总
    pi.summary = pi.product;
    if (!pi.release.empty()) pi.summary += L" " + pi.release;
    if (!pi.editionCn.empty()) pi.summary += L" " + pi.editionCn;
    if (!pi.arch.empty()) pi.summary += L" (" + pi.arch + L")";
    if (!pi.build.empty() && pi.build != L"0") pi.summary += L" Build " + pi.build;
    return pi;
}

// ============================================================================
// WimInfo 解析（替代 dism /Get-WimInfo）
// 使用 CWIMHelper::Open / GetImageCount / LoadImage / GetImageInformation
// ============================================================================
bool QueryWimInfo(const std::wstring& wimPath,
                  std::vector<WimInfo>& outInfos,
                  std::wstring* pErr)
{
    WimHandleGuard hWim = CWIMHelper::Open(wimPath, WIM_GENERIC_READ);
    if (hWim == INVALID_HANDLE_VALUE || hWim == nullptr) {
        // [P1-收尾] 回传 GLE 供 UI 映射 5/2/11/1920（加密 ESD/只读/找不到）
        // 等中文提示，替代已删除的 dism /Get-WimInfo 回退。
        if (pErr) {
            std::wstring sErr;
            sErr.resize(64);
            wfmt::FormatInto(sErr.data(), sErr.size() + 1, L"Open failed;gle=%lu", ::GetLastError());
            sErr.resize(wcslen(sErr.c_str()));
            *pErr = sErr;
        }
        return false;
    }

    DWORD nCount = CWIMHelper::GetImageCount(hWim);
    if (nCount == 0) {
        if (pErr) {
            std::wstring sErr;
            sErr.resize(64);
            wfmt::FormatInto(sErr.data(), sErr.size() + 1, L"No images;gle=%lu", ::GetLastError());
            sErr.resize(wcslen(sErr.c_str()));
            *pErr = sErr;
        }
        return false;
    }

    // [R-B86] 启动索引在 WIM_INFO.BootIndex（不在逐映像 XML 里）。
    WIM_INFO wfi = {};
    CWIMHelper::GetAttributes(hWim, &wfi);

    for (DWORD i = 1; i <= nCount; ++i) {
        WimInfo info;
        info.index = i;

        WimHandleGuard hImg = (WIMImageHandle)CWIMHelper::LoadImage(hWim, i);
        if (hImg != INVALID_HANDLE_VALUE && hImg != nullptr) {
            IMAGE_INFO iinfo = {};
            if (CWIMHelper::GetImageInformation(hImg, &iinfo)) {
                info.name         = iinfo.ImageName;
                info.description  = iinfo.ImageDescription;
                info.size         = static_cast<ULONGLONG>(iinfo.ullUncompressedSize);
                info.compressedSize = static_cast<ULONGLONG>(iinfo.ullCompressedSize);
                info.edition      = iinfo.EditionId;
                info.version      = iinfo.Version;
                info.language     = iinfo.Language;
                info.creationDate = iinfo.CreationDate;
                // <ARCH>：0=x86 9=x64 12=arm64 14=arm64ec
                switch (iinfo.dwArchitecture) {
                case 9:  info.architecture = L"x64"; break;
                case 12: info.architecture = L"arm64"; break;
                case 14: info.architecture = L"arm64ec"; break;
                case 0:  info.architecture = L"x86"; break;
                default: info.architecture = L""; break;
                }
                info.isBootable = (wfi.BootIndex == i);
                info.isEnterprise =
                    (info.edition.find(L"Enterprise") != std::wstring::npos);
            }
        }

        outInfos.push_back(info);
    }

    return true;
}

// ============================================================================
// Mount / Unmount（替代 dism /Mount-Wim / /Unmount-Wim）
// 使用 CWIMHelper::MountImage / UnmountImage
// ============================================================================
bool Mount(const std::wstring& wimPath, DWORD imageIndex,
           const std::wstring& mountPath, DWORD flags)
{
    bool bOk = CWIMHelper::MountImage(wimPath, imageIndex, mountPath, flags);
    if (bOk)
    {
        // [R-B6] 挂载成功后记账：ImageFiles MRU + MountPaths 映射 + MountFlags 日志
        DismLedger::AddImageFile(wimPath);
        DismLedger::RecordMount(wimPath, imageIndex, mountPath);
        DismLedger::SetLastMountFlags(flags);
        std::wstring sFlags;
        sFlags.resize(16);
        wfmt::FormatInto(sFlags.data(), sFlags.size() + 1, L"%.8X", flags);
        sFlags.resize(wcslen(sFlags.c_str()));
        static std::wstring sLog;
        sLog = L"WIMGAPI MountFlags=0x";
        sLog += sFlags;
        sLog += L" path=" + wimPath + L" idx=" + std::to_wstring(imageIndex);
        {
            WIMLogEntry e{};
            e.ts = NowST();
            e.level = 1;
            e.text = const_cast<wchar_t*>(sLog.c_str());
            AppendLog(e);
        }
    }
    else
    {
        static std::wstring sLog;
        sLog = L"wim=" + wimPath + L" idx=" + std::to_wstring(imageIndex) + L" mount=" + mountPath;
        {
            WIMLogEntry e{};
            e.ts = NowST();
            e.level = 2;
            e.text = const_cast<wchar_t*>(sLog.c_str());
            AppendLog(e);
        }
    }
    return bOk;
}

// ============================================================================
// EnumMounted（镜像仓）—— WIMGetMountedImages 直连
//
// 知识库《06_镜像处理专题》§1.2：#29 WIMGetMountedImages 列出所有已挂载图像。
// 用途：镜像仓列表 / 失效挂载点识别（原版"已经失效""不存在"状态判定）。
// 语义：先以空缓冲探测所需字节数（返回 FALSE + ERROR_INSUFFICIENT_BUFFER），
//       再按实际容量重取；wimgapi 要求缓冲按 WIM_MOUNT_LIST 对齐。
// ============================================================================
bool EnumMounted(std::vector<MountedImage>& outList)
{
    outList.clear();

    DWORD cbNeeded = 0;

    // 第一趟：探测所需大小（预期失败并置 ERROR_INSUFFICIENT_BUFFER）
    CWIMHelper::GetMountedImages(nullptr, &cbNeeded);
    if (cbNeeded == 0) {
        return true;   // 无已挂载映像
    }

    // 多留一格余量，避免探测与读取之间新增挂载导致再次不足
    cbNeeded += static_cast<DWORD>(sizeof(WIM_MOUNT_LIST));

    std::vector<BYTE> vBuf(cbNeeded, 0);
    DWORD cbBuf = cbNeeded;

    if (!CWIMHelper::GetMountedImages(
            reinterpret_cast<PWIM_MOUNT_LIST>(vBuf.data()), &cbBuf)) {
        return false;
    }

    DWORD cbEntry = static_cast<DWORD>(sizeof(WIM_MOUNT_LIST));
    if (cbEntry == 0) return false;

    DWORD nCount = cbBuf / cbEntry;
    const WIM_MOUNT_LIST* pList =
        reinterpret_cast<const WIM_MOUNT_LIST*>(vBuf.data());

    for (DWORD i = 0; i < nCount; ++i) {
        if (pList[i].WimPath[0] == L'\0' && pList[i].MountPath[0] == L'\0') {
            continue;   // 空槽位
        }
        MountedImage mi;
        mi.wimPath      = pList[i].WimPath;
        mi.mountPath    = pList[i].MountPath;
        mi.imageIndex   = pList[i].ImageIndex;
        mi.mountedForRW = pList[i].MountedForRW;
        outList.push_back(mi);
    }

    return true;
}

bool Unmount(const std::wstring& wimPath, DWORD imageIndex,
             const std::wstring& mountPath, BOOL commit)
{
    bool bOk = CWIMHelper::UnmountImage(mountPath, imageIndex, commit != FALSE);
    if (bOk)
    {
        // [R-B6] 卸载成功后移除记账
        DismLedger::RemoveMount(mountPath);
        std::wstring sLog = L"wim=" + wimPath + L" idx=" + std::to_wstring(imageIndex)
                          + L" commit=" + (commit ? L"yes" : L"no");
        {
            WIMLogEntry e{};
            e.ts = NowST();
            e.level = 1;
            e.text = const_cast<wchar_t*>(sLog.c_str());
            AppendLog(e);
        }
    }
    return bOk;
}

// ============================================================================
// ExportImage（替代 dism /Export-Image）
// 知识库 41 §24.4：WIMCreateFile(源)→WIMLoadImage(源)→WIMCreateFile(目标)
//                 →WIMExportImage→清理。
// 修正：旧实现误用 ExtractImagePath（子路径导出到目录语义），无法把整个
//       映像追加进目标 WIM；改为直调 WIMExportImage。
// ============================================================================
bool ExportImage(const std::wstring& srcWim, DWORD srcIdx,
                 const std::wstring& dstWim, DWORD dstIdx)
{
    (void)dstIdx;   // 目标 WIM 内的索引由导出顺序自然决定
    WimHandleGuard hSrc = CWIMHelper::Open(srcWim, WIM_GENERIC_READ);
    if (hSrc == INVALID_HANDLE_VALUE || hSrc == nullptr) return false;

    WimHandleGuard hImg = (WIMImageHandle)CWIMHelper::LoadImage(hSrc, srcIdx);
    if (hImg == INVALID_HANDLE_VALUE || hImg == nullptr) {
        return false;
    }

    // 打开目标（写入模式），必要时新建
    WimHandleGuard hDst = CWIMHelper::OpenForWrite(dstWim);
    if (hDst == INVALID_HANDLE_VALUE || hDst == nullptr) {
        hDst = CWIMHelper::CreateFile(dstWim, 0, WIM_GENERIC_READ | WIM_GENERIC_WRITE);
        if (hDst == INVALID_HANDLE_VALUE || hDst == nullptr) {
            return false;
        }
    }

    // 把源映像整体导出（追加）到目标 WIM
    // 0x00000001 = WIM_EXPORT_ALLOW_DUPLICATES（允许同名映像）
    bool ok = CWIMHelper::ExportImage(hImg, hDst, 0x00000001);

    return ok;
}

// ============================================================================
// Apply（替代 dism /Apply-Image）
// ============================================================================
bool Apply(const std::wstring& wimPath, DWORD imageIndex,
           const std::wstring& targetPath, DWORD flags)
{
    WimHandleGuard hWim = CWIMHelper::Open(wimPath, WIM_GENERIC_READ);
    if (hWim == INVALID_HANDLE_VALUE || hWim == nullptr) return false;

    WimHandleGuard hImg = (WIMImageHandle)CWIMHelper::LoadImage(hWim, imageIndex);
    if (hImg == INVALID_HANDLE_VALUE || hImg == nullptr) {
        return false;
    }

    bool ok = CWIMHelper::ApplyImage(hImg, targetPath, flags);
    if (ok)
    {
        // [R-B6] 应用成功后记账：ApplyPaths MRU + ApplyFlags 日志
        DismLedger::AddApplyPath(targetPath);
        DismLedger::SetLastApplyFlags(flags);
        std::wstring sLog = L"Apply ok: wim=" + wimPath + L" idx="
                          + std::to_wstring(imageIndex) + L" target=" + targetPath;
        {
            WIMLogEntry e{};
            e.ts = NowST();
            e.level = 1;
            e.text = const_cast<wchar_t*>(sLog.c_str());
            AppendLog(e);
        }
    }
    return ok;
}

// ============================================================================
// Capture（替代 dism /Capture-Image）
// ============================================================================
bool Capture(const std::wstring& srcDir, const std::wstring& wimPath,
             DWORD compression, const std::wstring& imageDesc)
{
    (void)imageDesc; // ImageDesc 通过 SetImageInformation 后续设置，此处仅创建+捕获
    WimHandleGuard hWim = CWIMHelper::CreateFile(wimPath, compression, WIM_GENERIC_READ | WIM_GENERIC_WRITE);
    if (hWim == INVALID_HANDLE_VALUE || hWim == nullptr) return false;

    bool ok = CWIMHelper::CaptureImage(hWim, srcDir, 0);
    return ok;
}

// ============================================================================
// ScanDrivers（替代 dism /Get-Drivers）
// 通过 FindFirstImageFile 枚举驱动目录，并对每个驱动包内的 INF 做
// 轻量解析，回填 provider / version / description。
// ============================================================================

namespace {

// 从 WIM 映像中抽取单个文件到本地临时文件；失败返回 false
bool ExtractOneFileToTemp(WIMImageHandle hImg,
                          const std::wstring& wimPath,
                          const std::wstring& targetPath,
                          std::wstring& outTemp)
{
    wchar_t szTmp[MAX_PATH] = {0};
    if (!::GetTempPathW(MAX_PATH, szTmp)) return false;

    wchar_t szName[MAX_PATH] = {0};
    if (!::GetTempFileNameW(szTmp, L"wimdrv", 0, szName)) return false;

    outTemp = szName;
    // WIM 内路径统一使用 '\'，不区分大小写
    if (!CWIMHelper::ExtractImagePath(hImg, targetPath.c_str(),
                                      szName, 0)) {
        ::DeleteFileW(szName);
        outTemp.clear();
        return false;
    }
    (void)wimPath;
    return true;
}

// 解析 INF 文本：DriverVer = 版本 , 日期    ; Provider = 厂商
// INF 的 DriverVer 与 Provider 通常位于 [Version] 节，
// 但为避免依赖节头顺序，只做宽松行级扫描（首匹配即取）。
// [R124-4] 从 "DriverVer=..." 行提取 N.N.N.N 形态版本号；提取失败返回空串。
static std::string ExtractDriverVer(const std::string& line)
{
    size_t eq = line.find('=');
    if (eq == std::string::npos) return {};
    std::string val = line.substr(eq + 1);
    size_t cm = val.find(',');
    if (cm != std::string::npos) val = val.substr(cm + 1);

    std::string v;
    for (char c : val)
    {
        if ((c >= '0' && c <= '9') || c == '.') v.push_back(c);
        else if (c == ' ' || c == '\t') continue;
        else break;
    }
    while (v.size() > 1 && v.back() == '.') v.pop_back();
    if (v.empty() || v.find('.') == std::string::npos) return {};
    return v;
}

// [R124-4] 从 "Provider=..." 行提取厂商名（去引号与首尾空白）；提取失败返回空串。
static std::string ExtractProvider(const std::string& line)
{
    size_t eq = line.find('=');
    if (eq == std::string::npos) return {};
    std::string val = line.substr(eq + 1);
    size_t s = val.find_first_not_of(" \t\"");
    size_t e = val.find_last_not_of(" \t\"");
    if (s == std::string::npos || e == std::string::npos) return {};
    return val.substr(s, e - s + 1);
}

void ParseInfForDriver(const std::string& text,
                       std::wstring& outProvider,
                       std::wstring& outVersion)
{
    outProvider.clear();
    outVersion.clear();

    std::istringstream is(text);
    std::string line;
    while (std::getline(is, line)) {

        while (!line.empty() && (line.back() == '\r' ||
                                 line.back() == '\n' ||
                                 line.back() == ' ')) line.pop_back();
        size_t b = line.find_first_not_of(" \t");
        if (b == std::string::npos) continue;
        if (line[b] == ';') continue;   // 注释行

        std::string low = line.substr(b);
        for (auto& c : low) c = static_cast<char>(::toupper((unsigned char)c));

        if (outVersion.empty() && low.rfind("DRIVERVER", 0) == 0) {
            const std::string v = ExtractDriverVer(line);
            if (!v.empty()) outVersion.assign(v.begin(), v.end());
        } else if (outProvider.empty() && low.rfind("PROVIDER", 0) == 0) {
            const std::string p = ExtractProvider(line);
            if (!p.empty()) outProvider.assign(p.begin(), p.end());
        }

        if (!outProvider.empty() && !outVersion.empty()) break;
    }

}

// 读取本地临时文件全部字节（上限 256KB，足够解析 INF）
bool ReadAllBytes(const std::wstring& path, std::string& out)
{
    RawHandleGuard h = ::CreateFileW(path.c_str(), GENERIC_READ,
                             FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER li{}; ::GetFileSizeEx(h, &li);
    if (li.QuadPart > 256 * 1024) li.QuadPart = 256 * 1024;
    DWORD cb = static_cast<DWORD>(li.QuadPart);
    out.resize(cb);
    DWORD got = 0;
    bool ok = ::ReadFile(h, &out[0], cb, &got, nullptr) != FALSE;
    if (!ok) { out.clear(); return false; }
    out.resize(got);
    return true;
}

} // anonymous namespace

// [R13-01] ScanDrivers 深度拆解辅助（d7->d<=5）
// 仅提取 INF 抽取与解析、同名缺 INF 通配回退，逐字搬迁原逻辑，零行为改动。
namespace
{
// 尝试抽取并解析驱动包内 INF；返回是否取得 provider/version
bool TryParseDriverInf(WIMImageHandle hImg, const std::wstring& wimPath,
                       const std::wstring& infWimPath, DriverEntry& de)
{
    std::wstring tmpPath;
    if (!ExtractOneFileToTemp(hImg, wimPath, infWimPath, tmpPath))
        return false;
    std::string text;
    if (ReadAllBytes(tmpPath, text))
        ParseInfForDriver(text, de.provider, de.version);
    ::DeleteFileW(tmpPath.c_str());
    return !(de.provider.empty() && de.version.empty());
}

// 同名 INF 缺失时的通配回退：遍历子目录找 *.inf
void ScanDriversFallbackInf(WIMImageHandle hImg, const std::wstring& wimPath,
                            const std::wstring& dirPath,
                            const std::wstring& subDir, DriverEntry& de)
{
    std::wstring search = dirPath + L"\\" + subDir;
    WIM_FIND_DATA fd2{};
    WimFindGuard hFind2 = CWIMHelper::FindFirstImageFile(hImg, search, &fd2);
    if (hFind2 == INVALID_HANDLE_VALUE || hFind2 == nullptr) return;
    do
    {
        std::wstring n = fd2.cFileName;
        size_t len = n.size();
        if (len > 4 && n.substr(len - 4) == L".inf")
        {
            std::wstring altWim = search + L"\\" + n;
            if (TryParseDriverInf(hImg, wimPath, altWim, de))
                break;
        }
    } while (CWIMHelper::FindNextImageFile(hFind2, &fd2));
}
} // anonymous namespace

bool ScanDrivers(const std::wstring& wimPath, DWORD imageIndex,
                 std::vector<DriverEntry>& outDrivers,
                 std::wstring* pErr)
{
    outDrivers.clear();

    WimHandleGuard hWim = CWIMHelper::Open(wimPath, WIM_GENERIC_READ);
    if (hWim == INVALID_HANDLE_VALUE || hWim == nullptr) {
        if (pErr) *pErr = L"Open failed";
        return false;
    }

    WimHandleGuard hImg = (WIMImageHandle)CWIMHelper::LoadImage(hWim, imageIndex);
    if (hImg == INVALID_HANDLE_VALUE || hImg == nullptr) {
        if (pErr) *pErr = L"LoadImage failed";
        return false;
    }

    // 遍历 WIM 内 DriverStore/FileRepository
    const std::wstring dirPath = L"Windows\\System32\\DriverStore\\FileRepository";
    WIM_FIND_DATA fd{};
    WimFindGuard hFind = CWIMHelper::FindFirstImageFile(hImg, dirPath, &fd);
    if (hFind != INVALID_HANDLE_VALUE && hFind != nullptr) {
        do {
            std::wstring subDir = fd.cFileName;
            if (subDir == L"." || subDir == L"..") continue;

            DriverEntry de;
            de.name = subDir;

            // 提取本驱动包内同名 INF 并解析元数据
            std::wstring infWimPath = dirPath + L"\\" + subDir + L"\\" +
                                      subDir + L".inf";
            TryParseDriverInf(hImg, wimPath, infWimPath, de);
            // 若同名 INF 不存在，尝试抽取 *.inf 通配目录下的首个子项
            if (de.provider.empty() && de.version.empty())
                ScanDriversFallbackInf(hImg, wimPath, dirPath, subDir, de);

            // description 暂时取子目录名
            de.description = de.name;

            outDrivers.push_back(de);
        } while (CWIMHelper::FindNextImageFile(hFind, &fd));
    }

    return true;
}


// ============================================================================
// ToolDialog 层专用 API（R30 v6.5）
// 把 ToolDialog.cpp 的 dism 批处理命令替换为 wimgapi 直接 API。
// 所有函数保持与 dism 命令等价的语义（含错误回传）。
// ============================================================================

DWORD CompressFromName(const std::wstring& sCompress)
{
    // max → LZX(2)；recovery → LZMS(3)；xpress16k → XPRESS(1)；其他默认 LZX
    // [R-B48] 说明：wimgapi 官方枚举仅有 NONE/XPRESS/LZX/LZMS 四值，
    //         XPRESS(1) 覆盖 xpress4k/xpress8k/xpress16k 家族（4K/8K/16K 是 WIM
    //         字典尺寸，wimgapi 无独立导出控制），故 xpress16k 落到 WIM_COMPRESS_XPRESS。
    std::wstring s = sCompress;
    for (auto& c : s) c = (wchar_t)::towlower((wchar_t)c);
    if (s.find(L"recovery") != std::wstring::npos)
        return WIM_COMPRESS_LZMS;
    if (s.find(L"xpress") != std::wstring::npos)
        return WIM_COMPRESS_XPRESS;
    return WIM_COMPRESS_LZX;
}

bool CaptureImageEx(const std::wstring& wimPath,
                    const std::wstring& srcDir,
                    const std::wstring& imageName,
                    const std::wstring& imageDesc,
                    DWORD compression,
                    bool bootable,
                    std::wstring* pErr,
                    IWimProgress* pProg)
{
    // 1) 创建目标 WIM（压缩方式在创建时指定）
    WimHandleGuard hWim = CWIMHelper::CreateFile(
        wimPath, compression, WIM_GENERIC_READ | WIM_GENERIC_WRITE);
    if (hWim == INVALID_HANDLE_VALUE || hWim == nullptr) {
        // [R16][F-4] 错误文案中文化+错误码（原英文裸抛不可读）。
        std::wstring sErr;
        sErr.resize(128);
        wfmt::FormatInto(sErr.data(), sErr.size() + 1, L"创建映像文件失败（错误码 %lu）", ::GetLastError());
        sErr.resize(wcslen(sErr.c_str()));
        if (pErr) *pErr = sErr;
        return false;
    }

    // 进度回调必须在 CaptureImage 之前注册（WIM_MSG_PROGRESS 0-100）
    ScopedMsgCallback scb(hWim, pProg);

    // 2) 捕获目录为映像
    DWORD capFlags = WIM_FLAG_VERIFY;
    if (!CWIMHelper::CaptureImage(hWim, srcDir, capFlags)) {
        // [R16][F-4] 先取 gle 再关句柄（WIMCloseHandle 可能覆盖错误码）；文案中文化。
        DWORD gle = ::GetLastError();
        if (pErr) {
            std::wstring sErr;
            sErr.resize(128);
            wfmt::FormatInto(sErr.data(), sErr.size() + 1, L"映像捕获失败（错误码 %lu）", gle);
            sErr.resize(wcslen(sErr.c_str()));
            *pErr = sErr;
        }
        return false;
    }

    // [R16][F-3] 产物校验：捕获成功必须产生 ≥1 个镜像，否则判失败
    // （防"假成功"兜底，与 F-1 句柄双判形成双保险）。
    if (CWIMHelper::GetImageCount(hWim) < 1) {
        if (pErr) *pErr = L"捕获后校验失败：映像中无有效镜像";
        return false;
    }

    // 3) 设置映像元信息（Name / Description）
    // [R-B86] Set 必须是 BOM+UTF16 XML（旧版传 2612 字节结构体必被拒 1465）。
    WimHandleGuard hImg = (WIMImageHandle)CWIMHelper::LoadImage(hWim, 1);
    if (hImg != INVALID_HANDLE_VALUE && hImg != nullptr) {
        if (!imageName.empty() || !imageDesc.empty())
            CWIMHelper::SetImageInfoStrings(hImg, imageName.c_str(),
                                            imageDesc.c_str());
    }

    // 4) 可选：标记为可启动映像
    if (bootable)
        CWIMHelper::SetBootImage(hWim, 1);

    return true;
}

bool ExportImageEx(const std::wstring& srcWim, DWORD srcIdx,
                   const std::wstring& dstWim,
                   DWORD compression,
                   const std::wstring& dstName,
                   const std::wstring& dstDesc,
                   bool bootable,
                   std::wstring* pErr,
                   IWimProgress* pProg)
{
    // 1) 打开源 WIM 并装载源索引
    WimHandleGuard hSrc = CWIMHelper::OpenForRead(srcWim);
    if (hSrc == INVALID_HANDLE_VALUE || hSrc == nullptr) {
        if (pErr) *pErr = L"Open src failed";
        return false;
    }

    // WIMExportImage 的进度由源句柄回调上报（WIM_MSG_PROGRESS 0-100）
    ScopedMsgCallback scb(hSrc, pProg);

    WimHandleGuard hImg = (WIMImageHandle)CWIMHelper::LoadImage(hSrc, srcIdx);
    if (hImg == INVALID_HANDLE_VALUE || hImg == nullptr) {
        if (pErr) *pErr = L"LoadImage src failed";
        return false;
    }

    // 2) 创建目标 WIM（压缩方式）
    WimHandleGuard hDst = CWIMHelper::CreateFile(
        dstWim, compression, WIM_GENERIC_READ | WIM_GENERIC_WRITE);
    if (hDst == INVALID_HANDLE_VALUE || hDst == nullptr) {
        if (pErr) *pErr = L"CreateFile dst failed";
        return false;
    }

    // 3) 执行导出（WIMExportImage 从 hImg 复制映像到 hDst）
    if (!CWIMHelper::ExportImage(hImg, hDst, WIM_EXPORT_VERIFY_SOURCE)) {
        if (pErr) *pErr = L"WIMExportImage failed";
        return false;
    }

    // 4) 设置目标映像元信息（在导出后的映像索引 1 上）
    WimHandleGuard hDstImg = (WIMImageHandle)CWIMHelper::LoadImage(hDst, 1);
    if (hDstImg != INVALID_HANDLE_VALUE && hDstImg != nullptr) {
        if (!dstName.empty() || !dstDesc.empty()) {
            // [R-B86] BOM+UTF16 XML 设置导出映像名/描述
            CWIMHelper::SetImageInfoStrings(hDstImg, dstName.c_str(),
                                            dstDesc.c_str());
        }
    }

    // 5) 可选：标记为可启动映像
    if (bootable)
        CWIMHelper::SetBootImage(hDst, 1);

    return true;
}

// [rb28 2026-10-13] 多索引线性进度适配器：wimgapi 每导出一个索引都会
// 重新上报一次 0-100，直接转发会让进度条反复回跳。按 (当前索引, 总索引数)
// 把单次 0-100 映射到总区间内不回退的一段。回调来自同一工作线程，
// wimgapi 消息回调串行化，普通成员即线程安全。
namespace {
class CScaledExportProgress : public IWimProgress
{
public:
    CScaledExportProgress(IWimProgress* pOuter, DWORD dwIdx, DWORD dwTotal)
        : m_pOuter(pOuter), m_dwIdx(dwIdx), m_dwTotal(dwTotal),
          m_dwLast(0xFFFFFFFF) {}

    void OnWimProgress(DWORD dwPercent, DWORD dwMsgId) override
    {
        if (!m_pOuter || m_dwTotal == 0) return;
        if (dwPercent > 100) dwPercent = 100;
        DWORD v = (DWORD)((static_cast<ULONGLONG>(m_dwIdx) * 100ULL
                         + static_cast<ULONGLONG>(dwPercent)) / static_cast<ULONGLONG>(m_dwTotal));
        if (v > 100) v = 100;
        if (v != m_dwLast)
        {
            m_dwLast = v;
            m_pOuter->OnWimProgress(v, dwMsgId);
        }
    }
private:
    IWimProgress* m_pOuter;
    DWORD m_dwIdx;
    DWORD m_dwTotal;
    DWORD m_dwLast;
};
} // namespace

bool ExportAllImagesEx(const std::wstring& srcWim,
                       const std::wstring& dstWim,
                       DWORD compression,
                       std::wstring* pErr,
                       IWimProgress* pProg,
                       const CancelQueryFn& fnCancel)
{
    // 1) 打开源映像（进度消息挂源句柄，与单索引导出一致）
    WimHandleGuard hSrc = CWIMHelper::OpenForRead(srcWim);
    if (hSrc == INVALID_HANDLE_VALUE || hSrc == nullptr)
    {
        if (pErr) *pErr = L"打开源映像失败";
        return false;
    }

    DWORD nCount = CWIMHelper::GetImageCount(hSrc);
    if (nCount == 0) nCount = 1;

    // 2) 目标映像只创建一次：旧 WecRunConvert 的关键语义就是“一个 hDst
    //    承接全部索引”。逐索引 CreateFile(WIM_CREATE_ALWAYS) 会把前面的
    //    索引全部截断，这也是互转不能直接复用单索引 ExportImageEx 的原因。
    WimHandleGuard hDst = CWIMHelper::CreateFileEx(dstWim, compression,
                                              WIM_GENERIC_READ | WIM_GENERIC_WRITE,
                                              WIM_CREATE_ALWAYS);
    if (hDst == INVALID_HANDLE_VALUE || hDst == nullptr)
    {
        if (pErr) *pErr = L"创建目标映像失败";
        return false;
    }

    bool bOk = true;
    bool bCancelled = false;
    for (DWORD i = 0; i < nCount; ++i)
    {
        // [R105] 取消检查点：置于索引边界。
        //   wimgapi 的 WIMExportImage 是不可分割的单次调用，中途无法打断；
        //   索引之间是唯一安全的位置。单个大索引开始后必须等它结束。
        if (fnCancel && fnCancel())
        {
            bCancelled = true;
            bOk = false;
            break;
        }

        // 每索引独立的区间适配器 + 回调挂载（出循环体自动反注册）
        CScaledExportProgress sp(pProg, i, nCount);
        ScopedMsgCallback scb(hSrc, pProg ? &sp : nullptr);

        WimHandleGuard hImg = (WIMImageHandle)CWIMHelper::LoadImage(hSrc, i + 1);
        if (hImg == INVALID_HANDLE_VALUE || hImg == nullptr) { bOk = false; break; }

        // 保留旧实现的 ALLOW_DUPLICATES：多版本镜像可能同名，转换不应被拒
        if (!CWIMHelper::ExportImage(hImg, hDst, WIM_EXPORT_ALLOW_DUPLICATES))
            bOk = false;

        if (!bOk) break;
    }


    if (!bOk)
    {
        // [R105] 取消与失败共用同一条清理路径：半成品目标必须删掉，
        //   否则用户会拿到一个打不开的 .wim（与超时/崩溃残留难以区分）。
        if (pErr)
            *pErr = bCancelled ? L"用户取消：导出已中止"
                               : L"WIMExportImage 导出某个索引失败";
        // [R120] 删半成品 .wim。删不掉时留痕：残留文件打不开，
        //   不说明会被误当成"导出成功但文件损坏"。不改变返回值（本来就失败）。
        if (!::DeleteFileW(dstWim.c_str()))
            ::OutputDebugStringW(L"[WIMBridge] 半成品 .wim 删除失败，请手动清理\r\n");
        return false;
    }

    if (pProg) pProg->OnWimProgress(100, 0x00000009 /*WIM_MSG_PROGRESS*/);
    return true;
}

bool DeleteImageEx(const std::wstring& wimPath, DWORD imageIndex,
                   std::wstring* pErr,
                   IWimProgress* pProg)
{
    WimHandleGuard hWim = CWIMHelper::OpenForWrite(wimPath);
    if (hWim == INVALID_HANDLE_VALUE || hWim == nullptr) {
        if (pErr) *pErr = L"OpenForWrite failed";
        return false;
    }
    ScopedMsgCallback scb(hWim, pProg);
    bool ok = CWIMHelper::DeleteImage(hWim, imageIndex);
    if (!ok && pErr) *pErr = L"DeleteImage failed";
    return ok;
}

bool AppendImageEx(const std::wstring& wimPath,
                   const std::wstring& srcDir,
                   const std::wstring& imageName,
                   std::wstring* pErr,
                   IWimProgress* pProg)
{
    // dism /Append-Image：把 srcDir 捕获为映像并追加到 wimPath 现有映像末尾。
    // wimgapi 中 WIMCaptureImage 会创建新映像并追加到已有映像之后（保持压缩方式）。
    WimHandleGuard hWim = CWIMHelper::OpenForWrite(wimPath);
    if (hWim == INVALID_HANDLE_VALUE || hWim == nullptr) {
        if (pErr) *pErr = L"OpenForWrite failed";
        return false;
    }

    ScopedMsgCallback scb(hWim, pProg);
    DWORD nBefore = CWIMHelper::GetImageCount(hWim);

    if (!CWIMHelper::CaptureImage(hWim, srcDir, WIM_FLAG_VERIFY)) {
        if (pErr) *pErr = L"CaptureImage append failed";
        return false;
    }

    // 设置追加映像的 Name（追加后的索引 = nBefore + 1）
    if (!imageName.empty()) {
        WimHandleGuard hImg = (WIMImageHandle)CWIMHelper::LoadImage(
            hWim, nBefore + 1);
        if (hImg != INVALID_HANDLE_VALUE && hImg != nullptr) {
            // [R-B86] BOM+UTF16 XML 设置追加映像名
            CWIMHelper::SetImageInfoStrings(hImg, imageName.c_str(), L"");
        }
    }

    return true;
}

bool ApplyImageEx(const std::wstring& wimPath, DWORD imageIndex,
                  const std::wstring& targetPath,
                  DWORD applyFlags,
                  std::wstring* pErr,
                  IWimProgress* pProg)
{
    // 装载源映像索引
    WimHandleGuard hWim = CWIMHelper::OpenForRead(wimPath);
    if (hWim == INVALID_HANDLE_VALUE || hWim == nullptr) {
        if (pErr) *pErr = L"Open src failed";
        return false;
    }

    // 进度回调必须在 ApplyImage 之前注册（WIM_MSG_PROGRESS 0-100）
    ScopedMsgCallback scb(hWim, pProg);

    WimHandleGuard hImg = (WIMImageHandle)CWIMHelper::LoadImage(hWim, imageIndex);
    if (hImg == INVALID_HANDLE_VALUE || hImg == nullptr) {
        if (pErr) *pErr = L"LoadImage src failed";
        return false;
    }

    // 目标路径规范化：wimgapi 要求以反斜杠结尾（知识库 41 §24.2 步骤2）
    std::wstring sTarget = targetPath;
    if (!sTarget.empty() && sTarget.back() != L'\\')
        sTarget += L'\\';

    // 确保目标目录存在（WIMApplyImage 要求目录已存在）
    SHCreateDirectoryExW(nullptr, sTarget.c_str(), nullptr);

    // WIM_FLAG_WIM_BOOT(0x2000) / WIM_FLAG_APPLY_COMPACT(0x4000)
    // 由调用方通过 applyFlags 精确控制
    DWORD flags = WIM_FLAG_VERIFY | applyFlags;
    bool ok = CWIMHelper::ApplyImage(hImg, sTarget, flags);

    if (!ok && pErr) *pErr = L"ApplyImage failed";
    return ok;
}

// Split（替代 dism /Split-Image /FileSize /CheckIntegrity）
// dstSwm 为首卷 .swm 全路径；partSizeMB 为每卷上限（MB）。
bool SplitImageEx(const std::wstring& wimPath,
                  const std::wstring& dstSwm,
                  DWORD partSizeMB,
                  std::wstring* pErr,
                  IWimProgress* pProg)
{
    WimHandleGuard hWim = CWIMHelper::OpenForRead(wimPath);
    if (hWim == INVALID_HANDLE_VALUE || hWim == nullptr) {
        if (pErr) *pErr = L"Open src WIM failed";
        return false;
    }
    ScopedMsgCallback scb(hWim, pProg);
    LARGE_INTEGER liPart;
    liPart.QuadPart = (LONGLONG)partSizeMB * 1024ULL * 1024ULL;
    bool ok = CWIMHelper::SplitFile(hWim, dstSwm, &liPart, 0) != FALSE;
    if (!ok && pErr) {
        std::wstring sErr;
        sErr.resize(160);
        DWORD gle = ::GetLastError();
        wfmt::FormatInto(sErr.data(), sErr.size() + 1,
                       L"WIMSplitFile failed (gle=%lu)", gle);
        sErr.resize(wcslen(sErr.c_str()));
        *pErr = sErr;
    }
    return ok;
}

} // namespace WIMBridge

// [C++17] wimgapi 消息 ID（原 BridgeMsgProc 函数内 magic number 常量化）
inline constexpr DWORD kWimMsgErrorId = 0x00000003;
inline constexpr DWORD kWimMsgProgressId = 0x00000009;
