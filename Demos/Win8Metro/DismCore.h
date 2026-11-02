// ============================================================================
//  dismcore.h —— Rust 内存安全模块 C++ 集成接口
// ----------------------------------------------------------------------------
//  配套 Rust 模块：rust/dismcore（crate dismcore）
//  构建：cd rust/dismcore && cargo build --release
//  产物：target/release/dismcore.dll（动态）
//        target/release/dismcore.lib（静态，/MT 兼容）
//
//  设计原则（R-P18 审计结论）：
//    * 仅迁移「纯计算、无 Win32 句柄、无 COM、无 DuiLib 依赖」的模块
//    * 跨 FFI 边界一律用「指针 + 长度 + 返回写入字节数」，不做跨边界分配/释放
//    * Rust 侧全安全代码；C++ 侧只负责提供合法缓冲区
//
//  典型用法：
//      wchar_t hex[65] = { 0 };
//      dismcore_hash_file_sha256(pszPath, hex, 65);   // 见下方封装
// ============================================================================
#ifndef DISMCORE_H
#define DISMCORE_H

#include <windows.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// ---- 哈希算法 ID（与 Rust 侧 Algo::from_u32 对应）----
enum DismCoreAlgo : unsigned int
{
    DISMCORE_MD5    = 1,
    DISMCORE_SHA1   = 2,
    DISMCORE_SHA256 = 3,
    DISMCORE_SHA512 = 4,
};

// ---- 返回码（与 Rust 侧一致）----
constexpr int64_t DISMCORE_ERR_BAD_ALGO = -1;
constexpr int64_t DISMCORE_ERR_BUF_SMALL = -2;

// ---- 原始 FFI（Rust `#[no_mangle]` 导出）----
extern "C"
{
    __declspec(dllimport) int64_t dismcore_digest_hex(
        unsigned int algo,
        const uint8_t* data, size_t dataLen,
        uint8_t* out, size_t outCap);

    __declspec(dllimport) int64_t dismcore_hex_encode(
        const uint8_t* data, size_t dataLen,
        uint8_t* out, size_t outCap);

    // [R-P22 · R2] 十六进制解码：返回写入字节数；-2=缓冲不足；-3=非法 hex
    __declspec(dllimport) int64_t dismcore_hex_decode(
        const uint8_t* s, size_t sLen,
        uint8_t* out, size_t outCap);

    // [R-P22 · R2] Base64 编码：返回写入字节数；-2=缓冲不足
    __declspec(dllimport) int64_t dismcore_base64_encode(
        const uint8_t* data, size_t dataLen,
        uint8_t* out, size_t outCap);

    // [R-P22 · R2] Base64 解码：返回写入字节数；-2=缓冲不足；-3=非法 base64
    __declspec(dllimport) int64_t dismcore_base64_decode(
        const uint8_t* s, size_t sLen,
        uint8_t* out, size_t outCap);

    // [R-P23 · R3] UTF-8 → UTF-16（u16 code units）：返回写入单元数；-2=缓冲不足；-3=非法 UTF-8
    __declspec(dllimport) int64_t dismcore_utf8_to_utf16(
        const uint8_t* data, size_t dataLen,
        uint16_t* out, size_t outCap);

    // [R-P23 · R3] UTF-16 → UTF-8 字节：返回写入字节数；-2=缓冲不足；-3=非法 UTF-16
    __declspec(dllimport) int64_t dismcore_utf16_to_utf8(
        const uint16_t* data, size_t dataLen,
        uint8_t* out, size_t outCap);

    // [R-P24 · R4] 定位 UTF-8 文本中第一个 ?Get*(...)。
    //   返回 1=命中；0=未命中；-1=非法 UTF-8；-2=缓冲不足；-3=关键指针为空。
    //   name/body 为 UTF-8 字节、不追加 NUL，长度由 outNameLen/outBodyLen 给出。
    __declspec(dllimport) int64_t dismcore_parse_data_expr(
        const uint8_t* s, size_t sLen,
        uint8_t* nameOut, size_t nameCap,
        uint8_t* bodyOut, size_t bodyCap,
        uint64_t* outQ, uint64_t* outMatchLen,
        uint64_t* outNameLen, uint64_t* outBodyLen);

    // [R-P24 · R4] 定位 UTF-16 code units 中第一个 ?Get*(...)。
    //   返回 1=命中；0=未命中；-2=缓冲不足；-3=关键指针为空。
    //   q / matchLen 为 code unit 数（与 C++ wchar 下标一一对应，无编码换算）。
    //   name 为 UTF-8 字节（不含前导 '?'，不追加 NUL），body 为 UTF-16 原文。
    __declspec(dllimport) int64_t dismcore_parse_data_expr_utf16(
        const uint16_t* s, size_t sLen,
        uint8_t* nameOut, size_t nameCap,
        uint16_t* bodyOut, size_t bodyCap,
        uint64_t* outQ, uint64_t* outMatchLen,
        uint64_t* outNameLen, uint64_t* outBodyLen);

    // [R-P25 · R6] 解析 UTF-8 版本字符串为 4 段 u32。
    //   返回 0=成功（outA/outB/outC/outD 已写入）；-1=关键指针为空。
    __declspec(dllimport) int64_t dismcore_parse_version(
        const uint8_t* s, size_t sLen,
        uint32_t* outA, uint32_t* outB, uint32_t* outC, uint32_t* outD);

    // [R-P25 · R6] 解析 UTF-16 版本字符串为 4 段 u32（版本串通常为纯 ASCII，
    //   逐 code unit 映射避免两段式编码转换）。
    //   返回 0=成功；-1=关键指针为空。
    __declspec(dllimport) int64_t dismcore_parse_version_utf16(
        const uint16_t* s, size_t sLen,
        uint32_t* outA, uint32_t* outB, uint32_t* outC, uint32_t* outD);

    // [R-P25 · R6] 无符号版本比较：把 4 段压缩为 u64 后按整数比较。
    //   返回 -1=Less / 0=Equal / 1=Greater；-2=关键指针为空。
    __declspec(dllimport) int64_t dismcore_compare_version(
        const uint32_t* la, const uint32_t* lb, const uint32_t* lc, const uint32_t* ld,
        const uint32_t* ra, const uint32_t* rb, const uint32_t* rc, const uint32_t* rd);

    // [R-P26 · R5] 在 UTF-8 文本中定位第一个匹配 tag 的 XML 标签（大小写不敏感）。
    //   返回 1=命中（outQ/outLen 已写入，为字节偏移/长度）；0=未命中；-2=关键指针为空。
    __declspec(dllimport) int64_t dismcore_find_xml_tag(
        const uint8_t* s, size_t sLen,
        const uint8_t* tag, size_t tagLen,
        uint64_t* outQ, uint64_t* outLen);

    // [R-P26 · R5] 在 UTF-16 code units 中定位第一个匹配 tag 的 XML 标签。
    //   返回 1=命中（outQ/outLen 为 code unit 数，与 C++ CDuiString::Find 下标对齐）；
    //   0=未命中；-2=关键指针为空。
    __declspec(dllimport) int64_t dismcore_find_xml_tag_utf16(
        const uint16_t* s, size_t sLen,
        const uint8_t* tag, size_t tagLen,
        uint64_t* outQ, uint64_t* outLen);

    // Windows 11 24H2+ 镜像预处理包：kind 0=unattend.xml，1=桌面图标脚本。
    __declspec(dllimport) int64_t dismcore_build_win11_preprocess(
        const uint8_t* account, size_t accountLen, uint32_t kind,
        uint8_t* out, size_t outCap);
    __declspec(dllimport) int64_t dismcore_validate_win11_preprocess(
        const uint8_t* xml, size_t xmlLen,
        const uint8_t* script, size_t scriptLen);
    __declspec(dllimport) int64_t dismcore_install_win11_preprocess(
        const uint8_t* root, size_t rootLen,
        const uint8_t* account, size_t accountLen, uint32_t build,
        uint8_t* msg, size_t msgCap);

    // [R8 2026-10-25] 可选功能页纯计算组件。
    // 输入均为 '\0' 分隔的 UTF-8 缓冲，输出为平铺位图/下标数组。
    __declspec(dllimport) int64_t dismcore_cap_visibility(
        const uint8_t* states, size_t statesLen,
        const uint8_t* identities, size_t identitiesLen,
        const uint8_t* displays, size_t displaysLen,
        uint32_t showAll,
        const uint8_t* filter, size_t filterLen,
        uint8_t* outVisible, size_t outCap);

    __declspec(dllimport) int64_t dismcore_cap_sort_order(
        const uint8_t* states, size_t statesLen,
        const uint8_t* displays, size_t displaysLen,
        uint32_t* outOrder, size_t outCap);

    __declspec(dllimport) uint32_t dismcore_capmodel_version();

    // [R8] Windows 功能页纯计算组件
    __declspec(dllimport) int64_t dismcore_feat_agg(
        const int32_t* pending, const uint8_t* enabled, const int32_t* parent,
        size_t n, uint8_t* outAgg, size_t outCap);

    __declspec(dllimport) int64_t dismcore_feat_diff(
        const uint8_t* oldSys, size_t oldSysLen, const uint8_t* oldEn, size_t oldN,
        const uint8_t* newSys, size_t newSysLen, const uint8_t* newEn, size_t newN,
        uint32_t* outChanged);

    __declspec(dllimport) uint32_t dismcore_featmodel_version();

    __declspec(dllimport) uint32_t dismcore_version();
}

namespace dismcore
{

// ---------------------------------------------------------------------------
// C++ 友好封装（薄包装，无额外内存管理）
// ---------------------------------------------------------------------------

/// 对内存缓冲区计算摘要，返回十六进制小写字符串（失败返回空串）。
inline std::string DigestHex(DismCoreAlgo algo,
                             const void* pData, size_t cbData)
{
    // SHA512 最长 128 字符
    char szOut[160] = { 0 };
    const int64_t n = ::dismcore_digest_hex(
        static_cast<unsigned int>(algo),
        static_cast<const uint8_t*>(pData), cbData,
        reinterpret_cast<uint8_t*>(szOut), sizeof(szOut) - 1);
    if (n <= 0) return std::string();
    return std::string(szOut, static_cast<size_t>(n));
}

/// 读取文件并计算摘要（Rust 只算哈希，文件 I/O 仍由 C++ 负责，避免跨边界句柄）。
/// 返回空串表示失败。
inline std::string DigestFileHex(DismCoreAlgo algo, const wchar_t* pszPath)
{
    if (!pszPath) return std::string();
    HANDLE h = ::CreateFileW(pszPath, GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::string();

    // RAII：局部守卫，早退自动关闭
    struct FileGuard
    {
        HANDLE h = nullptr;
        ~FileGuard() { if (h && h != INVALID_HANDLE_VALUE) ::CloseHandle(h); }
    } guard{ h };

    // 一次性读入（上限 64MB，防误读巨型文件）
    LARGE_INTEGER li{};
    if (!::GetFileSizeEx(h, &li) || li.QuadPart < 0 || li.QuadPart > (64LL << 20))
        return std::string();

    std::vector<uint8_t> buf(static_cast<size_t>(li.QuadPart));
    DWORD dwRead = 0;
    if (!buf.empty())
    {
        if (!::ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &dwRead, nullptr)
            || dwRead != buf.size())
            return std::string();
    }
    return DigestHex(algo, buf.data(), buf.size());
}

/// 十六进制编码（内存缓冲）。
inline std::string HexEncode(const void* pData, size_t cbData)
{
    std::string out(cbData * 2, '\0');
    const int64_t n = ::dismcore_hex_encode(
        static_cast<const uint8_t*>(pData), cbData,
        reinterpret_cast<uint8_t*>(&out[0]), out.size());
    if (n < 0) return std::string();
    out.resize(static_cast<size_t>(n));
    return out;
}

// [R-P22 · R2] 十六进制解码：UTF-8 字符串 → 字节。失败返回空 vector。
inline std::vector<uint8_t> HexDecode(const char* pszHex)
{
    std::vector<uint8_t> out;
    if (!pszHex) return out;
    const size_t n = std::strlen(pszHex);
    out.resize(n / 2 + 1);
    const int64_t r = ::dismcore_hex_decode(
        reinterpret_cast<const uint8_t*>(pszHex), n,
        out.data(), out.size());
    if (r < 0) { out.clear(); return out; }
    out.resize(static_cast<size_t>(r));
    return out;
}

/// [R-P22 · R2] Base64 编码（内存缓冲）。
inline std::string Base64Encode(const void* pData, size_t cbData)
{
    const size_t need = ((cbData + 2) / 3) * 4;
    std::string out(need, '\0');
    const int64_t n = ::dismcore_base64_encode(
        static_cast<const uint8_t*>(pData), cbData,
        reinterpret_cast<uint8_t*>(&out[0]), out.size());
    if (n < 0) return std::string();
    out.resize(static_cast<size_t>(n));
    return out;
}

/// [R-P22 · R2] Base64 解码：UTF-8 字符串 → 字节。失败返回空 vector。
inline std::vector<uint8_t> Base64Decode(const char* pszB64)
{
    std::vector<uint8_t> out;
    if (!pszB64) return out;
    const size_t n = std::strlen(pszB64);
    out.resize(n / 4 * 3 + 3);
    const int64_t r = ::dismcore_base64_decode(
        reinterpret_cast<const uint8_t*>(pszB64), n,
        out.data(), out.size());
    if (r < 0) { out.clear(); return out; }
    out.resize(static_cast<size_t>(r));
    return out;
}

/// [R-P23 · R3] UTF-8 字节 → UTF-16 code units（主机序）。
/// 失败返回空 vector。
inline std::vector<uint16_t> Utf8ToUtf16(const void* pData, size_t cbData)
{
    std::vector<uint16_t> out;
    if (!pData || cbData == 0) return out;
    out.resize(cbData + 1);
    const int64_t r = ::dismcore_utf8_to_utf16(
        static_cast<const uint8_t*>(pData), cbData,
        out.data(), out.size());
    if (r < 0) { out.clear(); return out; }
    out.resize(static_cast<size_t>(r));
    return out;
}

/// [R-P23 · R3] UTF-8 NUL 结尾字符串 → UTF-16（含 NUL 结尾）。
/// 失败返回空 vector。
inline std::vector<uint16_t> Utf8ToUtf16(const char* pszUtf8)
{
    std::vector<uint16_t> out;
    if (!pszUtf8) return out;
    const size_t n = std::strlen(pszUtf8);
    if (n == 0) return out;
    const auto v = Utf8ToUtf16(pszUtf8, n);
    if (v.empty()) return out;
    out.assign(v.begin(), v.end());
    out.push_back(0);   // NUL 结尾，兼容 const wchar_t*
    return out;
}

/// [R-P23 · R3] UTF-16 code units → UTF-8 字节。失败返回空 vector。
inline std::vector<uint8_t> Utf16ToUtf8(const void* pData, size_t cbUnits)
{
    std::vector<uint8_t> out;
    if (!pData || cbUnits == 0) return out;
    out.resize(cbUnits * 3 + 4);
    const int64_t r = ::dismcore_utf16_to_utf8(
        static_cast<const uint16_t*>(pData), cbUnits,
        out.data(), out.size());
    if (r < 0) { out.clear(); return out; }
    out.resize(static_cast<size_t>(r));
    return out;
}

// [R-P24 · R4] 一次 ?Get*(...) 解析命中的几何信息（UTF-8 变体）
struct DataExprHit
{
    size_t   q        = 0;    // '?' 在 UTF-8 输入中的字节偏移
    size_t   matchLen = 0;    // 完整 "?GetXxx(...)" 的字节长度
    std::string name;         // 函数名（不含前导 '?'，UTF-8）
    std::string body;         // 括号内原文（未 trim，UTF-8）
};

// [R-P24 · R4] 一次 ?Get*(...) 解析命中的几何信息（UTF-16 变体）
struct DataExprHitW
{
    size_t      q        = 0;    // '?' 在 UTF-16 输入中的 code unit 下标
    size_t      matchLen = 0;    // 完整 "?GetXxx(...)" 的 code unit 数
    std::wstring name;           // 函数名（不含前导 '?'）
    std::wstring body;           // 括号内原文（未 trim）
};

/// [R-P24 · R4] 在 UTF-8 文本中定位第一个 ?Get*(...)。
/// 返回 true=命中（hit 有效）；false=未命中 / 非法 UTF-8 / 缓冲异常。
/// 语义与 C++ 原 ParseFunExpr 逐字对齐（不 panic，不向后续 '?' 继续搜索）。
inline bool ParseDataExpr(const char* pszUtf8, DataExprHit& hit)
{
    if (!pszUtf8) return false;
    const size_t n = std::strlen(pszUtf8);
    std::string name(n + 1, '\0');
    std::string body(n + 1, '\0');
    uint64_t q = 0, ml = 0, nl = 0, bl = 0;
    const int64_t r = ::dismcore_parse_data_expr(
        reinterpret_cast<const uint8_t*>(pszUtf8), n,
        reinterpret_cast<uint8_t*>(&name[0]), name.size(),
        reinterpret_cast<uint8_t*>(&body[0]), body.size(),
        &q, &ml, &nl, &bl);
    if (r != 1) return false;
    hit.q        = static_cast<size_t>(q);
    hit.matchLen = static_cast<size_t>(ml);
    hit.name.assign(name.data(), static_cast<size_t>(nl));
    hit.body.assign(body.data(), static_cast<size_t>(bl));
    return true;
}

/// [R-P24 · R4] 在 UTF-16 code units 中定位第一个 ?Get*(...)。
/// 返回 true=命中（hit 有效）；false=未命中 / 缓冲异常。
/// 语义与 C++ 原 ParseFunExpr 逐字对齐；q / matchLen 为 code unit 数，
/// 与 C++ `wchar_t`/`CDuiString::Find` 下标一一对应，无编码换算，
/// name / body 直接输出 wchar，无 UTF-16↔UTF-8 往返（避免孤立代理丢失）。
inline bool ParseDataExprUtf16(const wchar_t* pszExpr, DataExprHitW& hit)
{
    if (!pszExpr) return false;
    const size_t n = std::wcslen(pszExpr);
    std::string name(n + 1, '\0');
    std::vector<uint16_t> body(n + 1, 0);
    uint64_t q = 0, ml = 0, nl = 0, bl = 0;
    const int64_t r = ::dismcore_parse_data_expr_utf16(
        reinterpret_cast<const uint16_t*>(pszExpr), n,
        reinterpret_cast<uint8_t*>(&name[0]), name.size(),
        body.data(), body.size(),
        &q, &ml, &nl, &bl);
    if (r != 1) return false;
    hit.q        = static_cast<size_t>(q);
    hit.matchLen = static_cast<size_t>(ml);
    // name：UTF-8 字节 → wchar（函数名必为 ASCII，逐字节映射即可）
    hit.name.clear();
    hit.name.reserve(static_cast<size_t>(nl));
    for (uint64_t i = 0; i < nl; ++i)
        hit.name.push_back(static_cast<wchar_t>(static_cast<unsigned char>(name[i])));
    // body：UTF-16 code units → wchar（本机 wchar_t 为 16 位，直接逐单元映射）
    hit.body.clear();
    hit.body.reserve(static_cast<size_t>(bl));
    for (uint64_t i = 0; i < bl; ++i)
        hit.body.push_back(static_cast<wchar_t>(body[i]));
    return true;
}

/// Rust 模块版本（用于启动期自检）。
inline uint32_t Version() { return ::dismcore_version(); }

inline std::string BuildWin11Preprocess(const std::string& account, uint32_t kind)
{
    std::vector<uint8_t> out(64 * 1024);
    const int64_t n = ::dismcore_build_win11_preprocess(
        reinterpret_cast<const uint8_t*>(account.data()), account.size(), kind,
        out.data(), out.size());
    return n > 0 ? std::string(reinterpret_cast<const char*>(out.data()), static_cast<size_t>(n))
                 : std::string();
}

inline bool ValidateWin11Preprocess(const std::string& xml, const std::string& script)
{
    return ::dismcore_validate_win11_preprocess(
        reinterpret_cast<const uint8_t*>(xml.data()), xml.size(),
        reinterpret_cast<const uint8_t*>(script.data()), script.size()) == 1;
}

inline int InstallWin11Preprocess(const std::string& imageRoot, const std::string& account,
                                  uint32_t build, std::string& message)
{
    char msg[512] = { 0 };
    const int64_t rc = ::dismcore_install_win11_preprocess(
        reinterpret_cast<const uint8_t*>(imageRoot.data()), imageRoot.size(),
        reinterpret_cast<const uint8_t*>(account.data()), account.size(), build,
        reinterpret_cast<uint8_t*>(msg), sizeof(msg));
    message.assign(msg);
    return static_cast<int>(rc);
}

// [R-P25 · R6] 四段版本号（缺段补 0，与 Rust Version 对齐）
struct DismVersion
{
    uint32_t a = 0, b = 0, c = 0, d = 0;
    uint64_t asU64() const
    {
        return ((static_cast<uint64_t>(a) << 16 | b) << 32)
             | (static_cast<uint64_t>(c) << 16 | d);
    }
};

/// [R-P25 · R6] 解析 UTF-16 版本字符串为 4 段 u32（版本串通常为纯 ASCII，
/// 逐 code unit 映射到 Rust 侧，避免 UTF-16↔UTF-8 两段式转换）。
/// 缺段补 0；非法字符停止解析当前段（`_ttoi` 语义）。不返回失败。
inline DismVersion ParseVersion(const wchar_t* pszVer)
{
    DismVersion v;
    if (!pszVer) return v;
    const int64_t r = ::dismcore_parse_version_utf16(
        reinterpret_cast<const uint16_t*>(pszVer), std::wcslen(pszVer),
        &v.a, &v.b, &v.c, &v.d);
    if (r < 0) v = DismVersion();
    return v;
}

/// [R-P25 · R6] 无符号版本比较（把 4 段压缩为 u64 后按整数比较）。
/// 返回 -1=Less / 0=Equal / 1=Greater。
inline int CompareVersion(const DismVersion& lhs, const DismVersion& rhs)
{
    const uint64_t a = lhs.asU64(), b = rhs.asU64();
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
}

// [R-P26 · R5] XML 标签定位命中：Span（code unit 偏移 + 长度，零拷贝）
struct XmlTagSpan
{
    size_t q   = 0;   // 起始偏移（含 '<'）
    size_t len = 0;   // 完整标签长度（含 '<' 与结尾 '>'）
};

/// [R-P26 · R5] 在 UTF-16 文本中定位第一个匹配 tag 的 XML 标签（大小写不敏感）。
/// 返回 true=命中（span 有效）；false=未命中 / 关键指针为空。
/// 语义与 C++ Page_Unattend.cpp 标签扫描对齐：'<' 后跳过空白、标签名结束于
/// 空白 / '"' / '/' / '>'；q/len 为 code unit 数，与 CDuiString::Find 下标对齐。
inline bool FindXmlTag(const wchar_t* pszText, const wchar_t* pszTag, XmlTagSpan& span)
{
    if (!pszText || !pszTag) return false;
    const size_t n = std::wcslen(pszText);
    // tag 转 UTF-8 字节（标签名必为 ASCII，逐字节映射）
    std::string tagUtf8;
    const size_t tagLen = std::wcslen(pszTag);
    tagUtf8.reserve(tagLen);
    for (size_t i = 0; i < tagLen; ++i)
        tagUtf8.push_back(static_cast<char>(static_cast<unsigned short>(pszTag[i])));
    uint64_t q = 0, len = 0;
    const int64_t r = ::dismcore_find_xml_tag_utf16(
        reinterpret_cast<const uint16_t*>(pszText), n,
        reinterpret_cast<const uint8_t*>(tagUtf8.data()), tagUtf8.size(),
        &q, &len);
    if (r != 1) return false;
    span.q   = static_cast<size_t>(q);
    span.len = static_cast<size_t>(len);
    return true;
}

// ---------------------------------------------------------------------------
// [R8 2026-10-25] 两个页面的纯计算包装层。
//   目的：页面只传业务数据，不接触裸指针与缓冲拼装。
//   失败一律返回 false，调用方回退到 C++ 旧路径——
//   宁可慢一点，也不能因 Rust 侧异常让页面白屏。
// ---------------------------------------------------------------------------

// 可选功能页：计算每条目的可见性。
//   in:  各条目的 state / identity / display（同一下标对齐）
//   out: visible 与 items 同长，1=可见 0=隐藏
inline bool CapVisibility(const std::vector<std::wstring>& states,
                          const std::vector<std::wstring>& identities,
                          const std::vector<std::wstring>& displays,
                          bool bShowAll, const std::wstring& filter,
                          std::vector<uint8_t>& outVisible)
{
    const size_t n = states.size();
    if (identities.size() != n || displays.size() != n) return false;
    if (n == 0) { outVisible.clear(); return true; }

    auto join = [](const std::vector<std::wstring>& v, std::string& out)
    {
        out.clear();
        for (size_t i = 0; i < v.size(); ++i)
        {
            if (i) out.push_back('\0');
            const int need = ::WideCharToMultiByte(CP_UTF8, 0, v[i].c_str(), -1,
                                                   nullptr, 0, nullptr, nullptr);
            if (need <= 1) continue;
            const size_t at = out.size();
            out.resize(at + static_cast<size_t>(need - 1));
            ::WideCharToMultiByte(CP_UTF8, 0, v[i].c_str(), -1,
                                  &out[at], need, nullptr, nullptr);
        }
    };

    std::string sStates, sIds, sDisplays, sFilter;
    join(states, sStates);
    join(identities, sIds);
    join(displays, sDisplays);
    join(std::vector<std::wstring>{ filter }, sFilter);

    outVisible.assign(n, 0);
    const int64_t rc = ::dismcore_cap_visibility(
        reinterpret_cast<const uint8_t*>(sStates.data()), sStates.size(),
        reinterpret_cast<const uint8_t*>(sIds.data()), sIds.size(),
        reinterpret_cast<const uint8_t*>(sDisplays.data()), sDisplays.size(),
        bShowAll ? 1u : 0u,
        reinterpret_cast<const uint8_t*>(sFilter.data()), sFilter.size(),
        outVisible.data(), outVisible.size());
    return rc == static_cast<int64_t>(n);
}

// 可选功能页：生成排序索引（已安装优先，其次按显示名大小写不敏感升序）
//   [R78-FIX] Rust 侧早有 dismcore_cap_sort_order 导出与头文件声明，
//   但 C++ 侧一直没有调用者，Capability 页的列表顺序完全由注册表枚举顺序
//   决定（已安装与未安装混排）。这里补上包装，让页面排序真正走 Rust。
inline bool CapSortOrder(const std::vector<std::wstring>& states,
                         const std::vector<std::wstring>& displays,
                         std::vector<uint32_t>& outOrder)
{
    const size_t n = states.size();
    if (displays.size() != n) return false;
    if (n == 0) { outOrder.clear(); return true; }

    auto join = [](const std::vector<std::wstring>& v, std::string& out)
    {
        out.clear();
        for (size_t i = 0; i < v.size(); ++i)
        {
            if (i) out.push_back('\0');
            const int need = ::WideCharToMultiByte(CP_UTF8, 0, v[i].c_str(), -1,
                                                   nullptr, 0, nullptr, nullptr);
            if (need <= 1) continue;
            const size_t at = out.size();
            out.resize(at + static_cast<size_t>(need - 1));
            ::WideCharToMultiByte(CP_UTF8, 0, v[i].c_str(), -1,
                                  &out[at], need, nullptr, nullptr);
        }
    };

    std::string sStates, sDisplays;
    join(states, sStates);
    join(displays, sDisplays);

    outOrder.assign(n, 0);
    const int64_t rc = ::dismcore_cap_sort_order(
        reinterpret_cast<const uint8_t*>(sStates.data()), sStates.size(),
        reinterpret_cast<const uint8_t*>(sDisplays.data()), sDisplays.size(),
        outOrder.data(), outOrder.size());
    return rc == static_cast<int64_t>(n);
}

// [R78-FIX] 模型版本协商：DLL 与 C++ 头文件对同一份模型逻辑的期望版本不一致时，
//   FFI 的入参/出参语义可能已经变了，此时宁可让调用方走 C++ 回退，也不要
//   拿新 DLL 喂旧参数（会得到看似正常、实则错位的结果）。
//   返回值：true = 版本匹配（或函数不可用时的保守策略见注释）。
inline bool CapModelVersionOk(uint32_t expected = 0x00010000u)
{
    return ::dismcore_capmodel_version() == expected;
}

// 同上：Windows 功能页模型版本协商
inline bool FeatModelVersionOk(uint32_t expected = 0x00010000u)
{
    return ::dismcore_featmodel_version() == expected;
}

// Windows 功能页：计算整棵树的聚合状态
//   返回值：0=关 1=开 2=混合 3=已移除
inline bool FeatAggregate(const std::vector<int32_t>& pending,
                          const std::vector<uint8_t>& enabled,
                          const std::vector<int32_t>& parent,
                          std::vector<uint8_t>& outAgg)
{
    const size_t n = pending.size();
    if (enabled.size() != n || parent.size() != n) return false;
    if (n == 0) { outAgg.clear(); return true; }
    outAgg.assign(n, 0);
    const int64_t rc = ::dismcore_feat_agg(pending.data(), enabled.data(),
                                         parent.data(), n,
                                         outAgg.data(), outAgg.size());
    return rc == static_cast<int64_t>(n);
}

// Windows 功能页：统计新旧两份列表的差异条数
inline bool FeatDiffCount(const std::vector<std::wstring>& oldSys,
                          const std::vector<uint8_t>& oldEn,
                          const std::vector<std::wstring>& newSys,
                          const std::vector<uint8_t>& newEn,
                          uint32_t& outChanged)
{
    if (oldEn.size() != oldSys.size() || newEn.size() != newSys.size()) return false;

    auto join = [](const std::vector<std::wstring>& v, std::string& out)
    {
        out.clear();
        for (size_t i = 0; i < v.size(); ++i)
        {
            if (i) out.push_back('\0');
            const int need = ::WideCharToMultiByte(CP_UTF8, 0, v[i].c_str(), -1,
                                                   nullptr, 0, nullptr, nullptr);
            if (need <= 1) continue;
            const size_t at = out.size();
            out.resize(at + static_cast<size_t>(need - 1));
            ::WideCharToMultiByte(CP_UTF8, 0, v[i].c_str(), -1,
                                  &out[at], need, nullptr, nullptr);
        }
    };

    std::string sOld, sNew;
    join(oldSys, sOld);
    join(newSys, sNew);
    const int64_t rc = ::dismcore_feat_diff(
        reinterpret_cast<const uint8_t*>(sOld.data()), sOld.size(),
        oldEn.data(), oldEn.size(),
        reinterpret_cast<const uint8_t*>(sNew.data()), sNew.size(),
        newEn.data(), newEn.size(),
        &outChanged);
    return rc == 0;
}

} // namespace dismcore

// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：

#endif // DISMCORE_H
