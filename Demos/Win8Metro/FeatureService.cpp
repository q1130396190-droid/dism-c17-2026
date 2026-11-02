// ============================================================
// FeatureService.cpp —— 「Windows 功能」系统层（不使用预编译头）
//
// [P0-A3 2026-10-11] 原生直连：枚举/应用均走 dismapi.dll 官方 API
// （DismApiBridge），删除旧的 _getfeat.bat 生成、runas 提权执行、
// featraw.sig 定时器轮询、featraw.txt ANSI 文本解析全链路。
// WinPE 离线会话由 DataEngine 决定，DismOpenSession 直接打开离线根。
// ============================================================

#include "FeatureService.h"

#include "UIlib.h"      // CMarkupNode（DataEngine.h 依赖；本文件不用 PCH）
using namespace DuiLib; // DataEngine.h 的裸类型名依赖（等价 stdafx.h 末尾 using）
#include "DataEngine.h" // WinPE 离线会话定向
#include "OfflineSession.h" // [R10-02] 离线状态经独立组件访问（去 CDataEngine 静态耦合）
#include "DismApiBridge.h"
#include "WmMessages.h"
#include "SafeThread.h"   // 工作线程顶层异常边界
#include "WinHandles.h"   // [C++17 R3] win::HandlePtr RAII for F11_raw_handle_close
#include "SafeEnv.h"
#include "SeedCache.h"    // [P4] 种子缓存指纹与目录      // [T1 P0] winenv::SafeGetEnv：缓存目录定位（对齐 AppxCapUpdService）
#include "DismCore.h"     // [T1 P0] dismcore::Utf16ToUtf8/Utf8ToUtf16（与 Cap 缓存同转换路径）

#include <string>
#include <algorithm>      // [T1 P0] std::reverse（头行 epoch 数字拼接）
#include <memory>         // [C++17 R3] make_unique / unique_ptr
#include <string_view>    // [T1 P0] 缓存行零拷贝切分
#include <optional>       // [T1 P0] TTL 解析（缺失视为无缓存）
#include <ctime>          // [T1 P0] TTL 时间戳

namespace {

// [C++17 R3] nOp 语义常量：0=禁用（保留负载）1=启用 2=永久移除
inline constexpr int kFeatOpDisable = 0;   // 禁用（保留负载）
inline constexpr int kFeatOpEnable  = 1;   // 启用
inline constexpr int kFeatOpRemove  = 2;   // 永久移除 /remove

// 取当前会话目标映像根（在线返回 nullptr → DISM_ONLINE_IMAGE）
const wchar_t* SessionImageRoot(std::wstring& sRootHolder)
{
    if (COfflineSession::IsActive())
    {
        sRootHolder = COfflineSession::GetImageRoot().GetData();
        return sRootHolder.c_str();
    }
    return nullptr;
}

struct EnumCtx
{
    HWND hNotify = nullptr;
};

struct ApplyCtx
{
    HWND                       hNotify = nullptr;
    std::vector<FeatApplyItem> vItems;
    CDuiString                 sSource;
    bool                       bLimitAccess = false;
};

[[nodiscard]] CDuiString OpVerb(int nOp)   // [C++17 R3] 返回值勿忽略
{
    switch (nOp)
    {
    case 1:  return L"启用";
    case 2:  return L"永久移除";
    default: return L"禁用";
    }
}

// ============================================================
// [T1 P0] Feature 枚举结果磁盘缓存辅助（stale-while-revalidate）
// 文件：%LOCALAPPDATA%\DismTool_Clone\feat_enum_cache.dat（UTF-8 TSV）
// 首行 "FEATCACHE\t<epoch秒>"，其后每行 2 字段（sys\tstate，制表/换行转义为空格）。
// TTL 3600s（与 Cap/Upd 缓存对齐，正确性由写后 InvalidateFeatCache 保证）。
// ============================================================

// [T2 对齐] Cap/Upd 缓存 TTL 已由 600s 提到 3600s；功能页缓存沿用同一策略，
// 变更（启用/禁用/移除）完成后由 InvalidateFeatCache 立即删除文件。
inline constexpr time_t kFeatCacheTtlSec = 3600;

bool FeatCachePath(std::wstring& sPath)
{
    // [B6] winenv::SafeGetEnv 替代弃用 _wgetenv（C4996 清零）
    std::wstring sDir = winenv::SafeGetEnv(L"LOCALAPPDATA");
    if (sDir.empty()) return false;
    sDir += L"\\DismTool_Clone";
    ::CreateDirectoryW(sDir.c_str(), nullptr);   // 已存在时失败可忽略
    sPath = sDir + L"\\feat_enum_cache.dat";
    return true;
}

// 单字段转义：控制字符（制表/回车/换行）替换为空格，避免破坏 TSV 结构
void FeatCacheEscapeField(const CDuiString& sIn, std::string& sOut)
{
    std::wstring w = sIn.GetData() ? sIn.GetData() : std::wstring();
    for (size_t i = 0; i < w.size(); ++i)
    {
        wchar_t ch = w[i];
        if (ch == L'\t' || ch == L'\r' || ch == L'\n') ch = L' ';
        w[i] = ch;
    }
    // [R-P23 · R3] Rust dismcore::Utf16ToUtf8 替代 WideCharToMultiByte 两段式
    const auto bytes = dismcore::Utf16ToUtf8(w.data(), w.size());
    sOut.assign(bytes.begin(), bytes.end());
}

CDuiString FeatCacheUnescapeField(const std::string& s)
{
    // [R-P23 · R3] Rust dismcore::Utf8ToUtf16 替代 MultiByteToWideChar 两段式
    const auto units = dismcore::Utf8ToUtf16(s.data(), s.size());
    if (units.empty()) return CDuiString();
    std::wstring w(units.begin(), units.end());
    return CDuiString(w.c_str());
}

} // anonymous namespace

// ============================================================
// [T1 P0] Feature 枚举结果 TTL 磁盘缓存（stale-while-revalidate）
// ============================================================

bool FeatureService::SaveFeatCache(const std::vector<FeatRawEntry>& vFeats)
{
    if (vFeats.empty()) return false;
    std::wstring sPath;
    if (!FeatCachePath(sPath)) return false;

    std::string sBody;
    {
        // [T1 P0] 头行 FEATCACHE\t<epoch秒>：用 std::string 承载，避免固定 char 缓冲
        //（F13_fixed_charbuf）；epoch 经整数转十进制拼接，无 printf 族依赖。
        const long long llNow = static_cast<long long>(::time(nullptr));
        std::string sEpoch;
        if (llNow < 0) return false;
        if (llNow == 0) sEpoch = "0";
        else
        {
            long long v = llNow;
            char digit;
            while (v > 0)
            {
                digit = static_cast<char>('0' + (v % 10));
                sEpoch.push_back(digit);
                v /= 10;
            }
            std::reverse(sEpoch.begin(), sEpoch.end());
        }
        sBody.reserve(16 + sEpoch.size() + 2);
        sBody += "FEATCACHE\t";
        sBody += sEpoch;
        sBody += "\r\n";
    }
    for (size_t i = 0; i < vFeats.size(); ++i)
    {
        const FeatRawEntry& e = vFeats[i];
        std::string a;
        FeatCacheEscapeField(e.sSys, a);
        sBody += a;
        sBody += '\t';
        sBody += e.bEnabled ? '1' : '0';
        sBody += "\r\n";
    }

    // [STAB] 原子写：预热线程与页面枚举线程可能并发写同一缓存文件，
    // 旧实现直接用 CREATE_ALWAYS 覆盖原文件，读取方（LoadFeatCache）
    // 可能在半写状态下打开，读到截断内容后按"缓存损坏"丢弃，
    // 表现为随机"缓存不生效/二次进页仍转圈"。改为先写 .tmp 再替换。
    const std::wstring sTmp = sPath + L".tmp";
    {
        win::HandlePtr hTmp = win::file_handle(::CreateFileW(
            sTmp.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!win::valid(hTmp)) return false;
        DWORD dwWritten = 0;
        const BOOL bOk = ::WriteFile(hTmp.get(), sBody.data(),
                                     static_cast<DWORD>(sBody.size()), &dwWritten, nullptr);
        if (!bOk || dwWritten != static_cast<DWORD>(sBody.size())) return false;
    }
    return ::MoveFileExW(sTmp.c_str(), sPath.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
}

bool FeatureService::LoadFeatCache(std::vector<FeatRawEntry>& vOut, bool bIgnoreTtl)
{
    vOut.clear();
    std::wstring sPath;
    if (!FeatCachePath(sPath)) return false;

    // [STAB] 顺手清理原子写遗留的 .tmp：进程被强杀时可能留下半成品，
    // 长期不清理会在 LOCALAPPDATA 里堆积。只删 .tmp，绝不碰正式缓存。
    ::DeleteFileW((sPath + L".tmp").c_str());

    win::HandlePtr hFile = win::file_handle(::CreateFileW(
        sPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!win::valid(hFile)) return false;
    DWORD dwSize = ::GetFileSize(hFile.get(), nullptr);
    if (dwSize == 0 || dwSize == INVALID_FILE_SIZE || dwSize > 4u * 1024u * 1024u)
        return false;

    std::string sBody(dwSize, '\0');
    DWORD dwRead = 0;
    BOOL bOk = ::ReadFile(hFile.get(), &sBody[0], dwSize, &dwRead, nullptr);
    if (!bOk || dwRead != dwSize) return false;

    // 逐行切分：std::string_view 零拷贝定位，仅对实际需要的字段做 substr 拷贝
    std::string_view sView(sBody.data(), sBody.size());
    size_t pos = 0;
    auto nextLine = [&sView, &pos]() -> std::string_view
    {
        if (pos >= sView.size()) return {};
        size_t e = sView.find("\r\n", pos);
        std::string_view s;
        if (e == std::string_view::npos) { s = sView.substr(pos); pos = sView.size(); }
        else { s = sView.substr(pos, e - pos); pos = e + 2; }
        return s;
    };

    std::string_view sHead = nextLine();
    if (sHead.size() < 10 || sHead.substr(0, 10) != "FEATCACHE\t") return false;
    // [T1] TTL 时间戳解析：std::optional 显式表达"缺失=无缓存"，避免 0 值歧义
    std::optional<long long> ullSaved;
    {
        std::string_view sNum = sHead.substr(10);
        if (!sNum.empty())
        {
            long long v = 0;
            bool bDigits = true;
            for (char ch : sNum) { if (ch < '0' || ch > '9') { bDigits = false; break; } v = v * 10 + (ch - '0'); }
            if (bDigits) ullSaved = v;
        }
    }
    time_t nNow = ::time(nullptr);
    if (!ullSaved.has_value()) return false;
    if (static_cast<unsigned long long>(nNow) < static_cast<unsigned long long>(*ullSaved))
        return false;   // 时钟回拨异常按无缓存处理
    if (!bIgnoreTtl && (nNow - static_cast<time_t>(*ullSaved)) > kFeatCacheTtlSec)
        return false;   // TTL 过期按无缓存处理

    while (pos < sView.size())
    {
        std::string_view sLine = nextLine();
        if (sLine.empty()) continue;
        size_t t = sLine.find('\t');
        if (t == std::string_view::npos || t == 0) continue;   // 格式容错：跳过

        FeatRawEntry e;
        e.sSys = FeatCacheUnescapeField(std::string(sLine.substr(0, t)));
        if (e.sSys.IsEmpty()) continue;
        std::string_view sSt = sLine.substr(t + 1);
        e.bEnabled = (!sSt.empty() && sSt[0] == '1');
        vOut.push_back(e);
    }
    return !vOut.empty();
}

void FeatureService::InvalidateFeatCache()
{
    std::wstring sPath;
    if (FeatCachePath(sPath))
        ::DeleteFileW(sPath.c_str());
}


// ============================================================
// [R79 P1] 写后精准失效：按系统名单行更新缓存状态
//
// 问题（R78 遗留）：启用/禁用/移除成功后直接 InvalidateFeatCache() 删掉整个
// 缓存文件，下次进页必然全量重扫（500~15000ms）。而用户实际只改了 1~N 项。
//
// 做法：读现有缓存 → 只把命中 sSys 的行的 state 改成目标值 → 刷新首行时间戳
// → 原子写回。文件格式完全不变（仍 FEATCACHE\t<epoch> + 每行 sys\tstate），
// 因此不需要版本迁移，LoadFeatCache 与种子缓存都不用改。
//
// 返回 false 的两种情况（调用方据此回退到 InvalidateFeatCache）：
//   · 缓存不存在/格式坏 —— 没有可更新的基底
//   · 一个目标系统名都没命中 —— 说明缓存已不能反映真实状态（例如枚举新增了项）
// 这两种情况都**不能**假装更新成功，否则下次进页会读到与实际不符的缓存。
// ============================================================
bool FeatureService::UpdateFeatCacheStates(const std::vector<FeatRawEntry>& vChanged,
                                             const std::wstring& sPathOverride)
{
    if (vChanged.empty()) return false;

    std::wstring sPath = sPathOverride;
    if (sPath.empty() && !FeatCachePath(sPath)) return false;
    ::DeleteFileW((sPath + L".tmp").c_str());

    // 原样读入全部字节：只做行级定位与替换，不重新序列化未变化的行，
    // 保证「未变化的行字节不变」（自检里有对应断言）。
    std::string sBody;
    {
        win::HandlePtr hFile = win::file_handle(::CreateFileW(
            sPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!win::valid(hFile)) return false;
        DWORD dwSize = ::GetFileSize(hFile.get(), nullptr);
        if (dwSize == 0 || dwSize == INVALID_FILE_SIZE || dwSize > 4u * 1024u * 1024u)
            return false;
        sBody.resize(dwSize);
        DWORD dwRead = 0;
        if (!::ReadFile(hFile.get(), &sBody[0], dwSize, &dwRead, nullptr) || dwRead != dwSize)
            return false;
    }

    // 建立「系统名(转义后) → 目标 state」索引
    std::map<std::string, char> mapWant;
    for (size_t i = 0; i < vChanged.size(); ++i)
    {
        if (vChanged[i].sSys.IsEmpty()) continue;
        std::string a;
        FeatCacheEscapeField(vChanged[i].sSys, a);
        if (!a.empty()) mapWant[a] = vChanged[i].bEnabled ? '1' : '0';
    }
    if (mapWant.empty()) return false;

    // 首行必须是 FEATCACHE\t<epoch>，否则不是本函数认识的格式
    const size_t nHeadEnd = sBody.find("\r\n");
    if (nHeadEnd == std::string::npos) return false;
    if (sBody.compare(0, 10, "FEATCACHE\t") != 0) return false;

    std::string sOut;
    sOut.reserve(sBody.size() + 32);
    {
        const long long llNow = static_cast<long long>(::time(nullptr));
        if (llNow <= 0) return false;
        char szEpoch[32] = { 0 };
        std::snprintf(szEpoch, sizeof(szEpoch), "FEATCACHE\t%lld\r\n", llNow);
        sOut += szEpoch;
    }

    size_t nMatched = 0;
    size_t nPos = nHeadEnd + 2;
    while (nPos < sBody.size())
    {
        size_t nEol = sBody.find("\r\n", nPos);
        if (nEol == std::string::npos) nEol = sBody.size();
        const std::string sLine = sBody.substr(nPos, nEol - nPos);

        const size_t t = sLine.find('\t');
        if (t != std::string::npos && t > 0)
        {
            const std::string sKey = sLine.substr(0, t);
            std::map<std::string, char>::const_iterator it = mapWant.find(sKey);
            if (it != mapWant.end())
            {
                // 命中：只替换 state 字段，键名部分逐字节保持原样
                sOut += sKey;
                sOut += '\t';
                sOut += it->second;
                sOut += "\r\n";
                ++nMatched;
                nPos = (nEol >= sBody.size()) ? sBody.size() : nEol + 2;
                continue;
            }
        }
        // 未命中：整行原样保留（含异常行，交给 LoadFeatCache 的容错继续跳过）
        sOut.append(sLine);
        sOut += "\r\n";
        nPos = (nEol >= sBody.size()) ? sBody.size() : nEol + 2;
    }

    // 一项都没命中：缓存与真实状态已脱节，交由调用方删缓存重扫
    if (nMatched == 0) return false;

    const std::wstring sTmp = sPath + L".tmp";
    {
        win::HandlePtr hTmp = win::file_handle(::CreateFileW(
            sTmp.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!win::valid(hTmp)) return false;
        DWORD dwWritten = 0;
        if (!::WriteFile(hTmp.get(), sOut.data(),
                         static_cast<DWORD>(sOut.size()), &dwWritten, nullptr)
            || dwWritten != static_cast<DWORD>(sOut.size()))
            return false;
    }
    return ::MoveFileExW(sTmp.c_str(), sPath.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
}

// [P4] 种子缓存加载：exe\seed\seed_feat.dat
//  首行 "SEED\t<OSBUILD>|<SKU>|<LANG>"，指纹不匹配直接返回 false。
//  其后与磁盘缓存同构：首行 "FEATCACHE\t<epoch>"，再逐行 "系统名\t0/1"。
//  无 TTL 校验 —— 出厂数据不随时间失效，但受指纹约束。
bool FeatureService::LoadSeedFeatCache(std::vector<FeatRawEntry>& vOut,
                                        const std::wstring& sPathOverride)
{
    vOut.clear();

    // [R60] 同 AppxCapUpdService::LoadSeedCapCache：可选路径供自检构造坏文件。
    std::wstring sPath;
    if (!sPathOverride.empty())
    {
        sPath = sPathOverride;
    }
    else
    {
        const std::wstring sDir = seedcache::SeedDir();
        if (sDir.empty()) return false;
        sPath = sDir + L"\\seed_feat.dat";
    }

    win::HandlePtr hFile = win::file_handle(::CreateFileW(
        sPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!win::valid(hFile)) return false;

    const DWORD dwSize = ::GetFileSize(hFile.get(), nullptr);
    // 种子文件应当很小（最坏 ~6KB），设 4MB 上限仅为防御损坏文件
    if (dwSize == 0 || dwSize == INVALID_FILE_SIZE || dwSize > 4u * 1024u * 1024u)
        return false;

    std::string sBody(dwSize, '\0');
    DWORD dwRead = 0;
    if (!::ReadFile(hFile.get(), &sBody[0], dwSize, &dwRead, nullptr)
        || dwRead != dwSize)
        return false;

    std::string_view sView(sBody.data(), sBody.size());
    size_t pos = 0;
    auto nextLine = [&sView, &pos]() -> std::string_view
    {
        if (pos >= sView.size()) return {};
        const size_t e = sView.find("\r\n", pos);
        std::string_view s;
        if (e == std::string_view::npos) { s = sView.substr(pos); pos = sView.size(); }
        else { s = sView.substr(pos, e - pos); pos = e + 2; }
        return s;
    };

    // ---- 第 1 行：指纹 ----
    const std::string_view sSeedHead = nextLine();
    if (sSeedHead.size() <= 5 || sSeedHead.substr(0, 5) != "SEED\t") return false;
    const std::string sFp(sSeedHead.substr(5));
    if (!seedcache::MatchesFingerprint(sFp))
    {
        // 指纹不符：本种子属于别的 OS build / SKU / 语言，禁止渲染
        ::OutputDebugStringW(L"[seed] feat seed fingerprint mismatch, ignored\r\n");
        return false;
    }

    // ---- 第 2 行：与磁盘缓存同构的数据头（校验存在即可，不做 TTL） ----
    const std::string_view sDataHead = nextLine();
    if (sDataHead.size() < 10 || sDataHead.substr(0, 10) != "FEATCACHE\t") return false;

    // ---- 其余行：条目 ----
    while (pos < sView.size())
    {
        const std::string_view sLine = nextLine();
        if (sLine.empty()) continue;
        const size_t t = sLine.find('\t');
        if (t == std::string_view::npos || t == 0) continue;

        FeatRawEntry e;
        e.sSys = FeatCacheUnescapeField(std::string(sLine.substr(0, t)));
        if (e.sSys.IsEmpty()) continue;
        const std::string_view sSt = sLine.substr(t + 1);
        e.bEnabled = (!sSt.empty() && sSt[0] == '1');
        vOut.push_back(e);
    }

    if (!vOut.empty())
        ::OutputDebugStringW(L"[seed] feat seed loaded (fingerprint matched)\r\n");
    return !vOut.empty();
}

void FeatureService::StartEnumThread(HWND hNotify)
{
    // [C++17 R3] thread ctx: transferred to worker thread on success; reclaimed here on creation failure
    auto ctxOwner = std::make_unique<EnumCtx>();
    ctxOwner->hNotify = hNotify;

    HANDLE h = SafeCreateThread(
        [](void* pv) -> DWORD
        {
            std::unique_ptr<EnumCtx> guard(static_cast<EnumCtx*>(pv));   // [C++17 R3] thread owns ctx, auto-freed at end
            HWND hWnd = guard->hNotify;

            auto msg = std::make_unique<FeatDataMsg>();   // [C++17 R3] transferred to UI on successful Post; auto-reclaimed on failure
            FeatDataMsg* pMsg = msg.get();

            // 内层 C++ 异常边界：任何失败都必须回传消息，否则 UI 永久转圈；
            // SEH 硬件异常由外层 SafeThreadTrampoline 兜底。
            try
            {
                // FrameWnd 启动时已 Init；此处幂等兜底（独立线程调用安全，
                // DismInitialize 全局一次，Init 内部判重）。
                DismApiBridge::Init();

                std::wstring sRoot;
                const wchar_t* pszRoot = SessionImageRoot(sRoot);

                std::vector<DismApiBridge::FeatureInfo> vApi;
                std::wstring sErr;
                if (DismApiBridge::GetFeatures(vApi, &sErr, pszRoot))
                {
                    // [P1] 边转换边分批 Post：DismGetFeatures 一次返回全量数组，
                    // 无逐项回调，故"流式"在数据到手后做。页面收到批即可渲染，
                    // 首屏不必等整表。末批发 bFinal=true 通知收尾。
                    constexpr size_t kPartial = 100;
                    std::vector<FeatRawEntry> vBatch;
                    vBatch.reserve(kPartial);
                    // [PROGRESS 2026-10-25] 带上总数与累计已发送数。
                    //   与可选功能页同口径：总数在枚举返回时就已确定，
                    //   旧实现把它丢了，页面只能无限转圈。
                    const int nGrandTotal = static_cast<int>(vApi.size());
                    int nSentSoFar = 0;
                    auto flushFeat = [&hWnd, &vBatch, kPartial, nGrandTotal, &nSentSoFar](bool bFinal)
                    {
                        auto part = std::make_unique<FeatPartialMsg>();
                        nSentSoFar += static_cast<int>(vBatch.size());
                        part->nTotal = nGrandTotal;
                        part->nSent = nSentSoFar;
                        part->vBatch.swap(vBatch);
                        part->bFinal = bFinal;
                        if (::IsWindow(hWnd) && ::PostMessageW(hWnd, WM_FEAT_PARTIAL, 0,
                                reinterpret_cast<LPARAM>(part.get()))) part.release();
                        vBatch.reserve(kPartial);
                    };
                    for (size_t i = 0; i < vApi.size(); ++i)
                    {
                        FeatRawEntry r;
                        r.sSys = vApi[i].sName.c_str();
                        r.bEnabled = vApi[i].bEnabled;
                        pMsg->vRaw.push_back(r);
                        vBatch.push_back(r);
                        if (vBatch.size() >= kPartial) flushFeat(false);
                    }
                    flushFeat(true);
                    pMsg->nRc = pMsg->vRaw.empty() ? -1 : 1;
                }
                else
                {
                    pMsg->nRc = 0;
                    pMsg->sErr = sErr.c_str();
                }
            }
            catch (...)
            {
                pMsg->nRc = 0;
                if (pMsg->sErr.IsEmpty()) pMsg->sErr = L"枚举过程中发生异常";
            }

            if (::IsWindow(hWnd) &&
                ::PostMessageW(hWnd, WM_FEAT_DATA, 0, reinterpret_cast<LPARAM>(msg.get())))
            {
                msg.release();   // [C++17 R3] ownership transferred to UI
            }
            return 0;
        },
        ctxOwner.get());

    win::HandlePtr hGuard(h);   // [C++17 R3] F11: RAII auto CloseHandle (was: if (h) ::CloseHandle(h))
    if (h)
        ctxOwner.release();   // [C++17 R3] ownership transferred to worker thread
    // creation failure: ctxOwner reclaims EnumCtx at scope exit (was: else delete ctx)
}

bool FeatureService::StartApplyThread(HWND hNotify,
                                      const std::vector<FeatApplyItem>& vItems,
                                      const CDuiString& sSource,
                                      bool bLimitAccess)
{
    // [C++17 R3] thread ctx: transferred to worker thread on success; reclaimed here on creation failure
    auto ctxOwner = std::make_unique<ApplyCtx>();
    ctxOwner->hNotify = hNotify;
    ctxOwner->vItems = vItems;
    ctxOwner->sSource = sSource;
    ctxOwner->bLimitAccess = bLimitAccess;

    HANDLE h = SafeCreateThread(
        [](void* pv) -> DWORD
        {
            std::unique_ptr<ApplyCtx> guard(static_cast<ApplyCtx*>(pv));   // [C++17 R3] thread owns ctx, auto-freed at end
            HWND hWnd = guard->hNotify;

            auto msg = std::make_unique<FeatApplyMsg>();   // [C++17 R3] transferred to UI on successful Post; auto-reclaimed on failure
            FeatApplyMsg* pMsg = msg.get();
            pMsg->nTotal = static_cast<int>(guard->vItems.size());

            // 内层 C++ 异常边界：保证 UI 收到 WM_FEAT_APPLYDONE 解除等待
            try
            {
                DismApiBridge::Init();

                std::wstring sRoot;
                const wchar_t* pszRoot = SessionImageRoot(sRoot);

                const std::wstring sSrc = guard->sSource.GetData();

                for (size_t i = 0; i < guard->vItems.size(); ++i)
                {
                    const FeatApplyItem& it = guard->vItems[i];
                    const std::wstring sName = it.sName.GetData();

                    std::wstring sErr;
                    bool bOk = false;
                    if (it.nOp == kFeatOpEnable)
                    {
                        bOk = DismApiBridge::EnableFeature(
                                  sName, sSrc, guard->bLimitAccess, &sErr, pszRoot)
                          || DismApiBridge::LastHResult() == HRESULT_FROM_WIN32(ERROR_SUCCESS_REBOOT_REQUIRED);
                    }
                    else
                    {
                        // nOp 0=禁用（保留负载），2=永久移除（/remove）
                        bOk = DismApiBridge::DisableFeature(
                                  sName, it.nOp == kFeatOpRemove, &sErr, pszRoot)
                          || DismApiBridge::LastHResult() == HRESULT_FROM_WIN32(ERROR_SUCCESS_REBOOT_REQUIRED);
                    }

                    CDuiString sLine;
                    if (bOk)
                    {
                        ++pMsg->nOk;
                        if (DismApiBridge::LastHResult() == HRESULT_FROM_WIN32(ERROR_SUCCESS_REBOOT_REQUIRED))
                            pMsg->bRebootHint = true;
                        sLine.Format(L"[feat] %s 成功：%s\r\n",
                                     OpVerb(it.nOp).GetData(), it.sName.GetData());
                    }
                    else
                    {
                        ++pMsg->nFail;
                        sLine.Format(L"[feat] %s 失败：%s —— %s\r\n",
                                     OpVerb(it.nOp).GetData(), it.sName.GetData(),
                                     sErr.empty() ? CDuiString(L"未知错误") : CDuiString(sErr.c_str()).GetData());
                    }
                    pMsg->sDetail += sLine;
                }
            }
            catch (...)
            {
                // 未计入的项全部记为失败，避免 nOk+nFail 与 nTotal 不符
                int nDone = pMsg->nOk + pMsg->nFail;
                if (nDone < pMsg->nTotal) pMsg->nFail += (pMsg->nTotal - nDone);
                pMsg->sDetail += L"[feat] 应用过程中发生异常，已中止\r\n";
            }

            if (::IsWindow(hWnd) &&
                ::PostMessageW(hWnd, WM_FEAT_APPLYDONE, 0, reinterpret_cast<LPARAM>(msg.get())))
            {
                msg.release();   // [C++17 R3] ownership transferred to UI
            }
            return 0;
        },
        ctxOwner.get());

    win::HandlePtr hGuard(h);   // [C++17 R3] F11: RAII auto CloseHandle (was: if (h) ::CloseHandle(h))
    if (h)
    {
        ctxOwner.release();   // [C++17 R3] ownership transferred to worker thread
        return true;
    }
    // [CRASHFIX-STUCK] 线程创建失败：ctxOwner 作用域出口自动回收；返回 false
    // 让调用方复位"在途"标志（旧实现 void 静默吞掉 → m_bApplying 永久卡死）。
    // creation failure: ctxOwner reclaims ApplyCtx at scope exit (was: else delete ctx)
    return false;
}
