// EsdDecryptor.cpp
// [知识库 41 §26] 加密 ESD 自主解密实现，详见 EsdDecryptor.h
#include <Windows.h>
#include "FormatW.h"


#include <Wincrypt.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cwctype>
#include "EsdDecryptor.h"
#include "DismCore.h"   // [R-P22 / R2] Rust dismcore：Base64Decode 纯计算
// [C++17 阶段2] 局部 RAII 守卫：文件句柄（见 WinHandles.h 使用规范）
struct EsdFileGuard
{
    HANDLE h;
    explicit EsdFileGuard(HANDLE x) : h(x) {}
    ~EsdFileGuard() { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    EsdFileGuard(const EsdFileGuard&) = delete;
    EsdFileGuard& operator=(const EsdFileGuard&) = delete;
    operator HANDLE() const { return h; }
};


namespace EsdDecryptor {

namespace {

// ------------------------------------------------------------------------
// WIM 磁盘结构（与参考实现 defines.h 一致，1 字节对齐）
// ------------------------------------------------------------------------
#pragma pack(push, 1)

struct ResHdrDisk {
    unsigned long long size_in_wim : 56;
    unsigned long long flags       : 8;
    long long          offset_in_wim;
    long long          original_size;
};

struct WimHeaderDisk {
    unsigned long long magic;          // "MSWIM\0\0\0"
    unsigned long      hdr_size;
    unsigned long      version;
    unsigned long      flags;
    unsigned long      chunk_size;
    unsigned char      guid[16];
    unsigned short     part_number;
    unsigned short     total_parts;
    unsigned long      image_count;
    ResHdrDisk         lookup;
    ResHdrDisk         xml;
    ResHdrDisk         boot;
    unsigned long      boot_index;
    ResHdrDisk         integrity;
    unsigned char      unused[60];
};

#pragma pack(pop)

static const unsigned long long kMswimMagic = 0x0000004d4957534dULL;

struct RangeInfo {
    long long     offset;
    unsigned long bytes;
};

// ------------------------------------------------------------------------
// 小工具
// ------------------------------------------------------------------------
bool ReadAt(HANDLE h, void* buf, unsigned long len, long long off,
            unsigned long* pcbRead)
{
    LARGE_INTEGER li;
    li.QuadPart = off;
    if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) return false;
    unsigned long cb = 0;
    if (!ReadFile(h, buf, len, &cb, nullptr)) return false;
    if (pcbRead) *pcbRead = cb;
    return cb == len;
}

bool WriteAt(HANDLE h, const void* buf, unsigned long len, long long off,
             unsigned long* pcbWritten)
{
    LARGE_INTEGER li;
    li.QuadPart = off;
    if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) return false;
    unsigned long cb = 0;
    if (!WriteFile(h, buf, len, &cb, nullptr)) return false;
    if (pcbWritten) *pcbWritten = cb;
    return cb == len;
}

bool ReadAllBytes(const std::wstring& path, std::vector<unsigned char>& out,
                  long long fromOff, unsigned long want)
{
    EsdFileGuard h = EsdFileGuard(CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr));
    if (h == INVALID_HANDLE_VALUE) return false;
    out.resize(want);
    unsigned long cb = 0;
    bool ok = ReadAt(h, out.data(), want, fromOff, &cb);
    if (!ok) { out.resize(cb); return cb == want; }
    return true;
}

// 去除所有空白字符（base64 片段中可能含换行/制表符）
//   [R-P22 / R2] Rust dismcore::Base64Decode 已内建空白容忍，此辅助函数不再需要
//   但保留（未引用则删，避免 dead code）—— 直接删除本函数。

bool Base64Decode(const std::wstring& text, std::vector<unsigned char>& out)
{
    if (text.empty()) return false;
    // base64 纯 ASCII：逐字符映射到 UTF-8；非 ASCII 视为非法
    std::string s(text.size(), '\0');
    for (size_t i = 0; i < text.size(); ++i)
    {
        const wchar_t c = text[i];
        if (c > 0x7F) return false;
        s[i] = static_cast<char>(c);
    }
    const auto v = dismcore::Base64Decode(s.c_str());
    if (v.empty()) return false;   // Rust 侧校验失败
    out.assign(v.begin(), v.end());
    return true;
}

// 在字符串中做大小写不敏感查找
size_t FindNoCase(const std::wstring& hay, const wchar_t* needle, size_t from = 0)
{
    if (!needle) return std::wstring::npos;
    size_t nlen = wcslen(needle);
    for (size_t i = from; i + nlen <= hay.size(); ++i) {
        size_t j = 0;
        for (; j < nlen; ++j) {
            wchar_t a = hay[i + j];
            wchar_t b = needle[j];
            if (towlower(a) != towlower(b)) break;
        }
        if (j == nlen) return i;
    }
    return std::wstring::npos;
}

// 从 <RANGE ...> 标签文本中读 "Name="value"" 属性（十进制）
bool ReadRangeAttr(const std::wstring& tag, const wchar_t* name,
                   unsigned long long& val)
{
    size_t p = FindNoCase(tag, name);
    if (p == std::wstring::npos) return false;
    p += wcslen(name);
    while (p < tag.size() && (tag[p] == L' ' || tag[p] == L'\t')) ++p;
    if (p >= tag.size() || tag[p] != L'=') return false;
    ++p;
    if (p >= tag.size() || (tag[p] != L'"' && tag[p] != L'\'')) return false;
    wchar_t q = tag[p];
    ++p;
    size_t start = p;
    while (p < tag.size() && tag[p] != q) ++p;
    if (p <= start) return false;
    // 十进制解析
    val = _wcstoui64(tag.substr(start, p - start).c_str(), nullptr, 10);
    return true;
}

// 解析 <ENCRYPTED> 文档：取 KEY 文本与全部 RANGE
bool ParseEncryptedXml(const std::wstring& xml,
                       std::wstring& keyB64,
                       std::vector<RangeInfo>& ranges)
{
    // <KEY ...>text</KEY>
    size_t k = FindNoCase(xml, L"<KEY");
    if (k == std::wstring::npos) return false;
    size_t gt = xml.find(L'>', k);
    if (gt == std::wstring::npos) return false;
    size_t ke = FindNoCase(xml, L"</KEY", gt);
    if (ke == std::wstring::npos) return false;
    keyB64 = xml.substr(gt + 1, ke - gt - 1);

    // 逐个 <RANGE .../>
    size_t p = 0;
    while (true) {
        size_t r = FindNoCase(xml, L"<RANGE", p);
        if (r == std::wstring::npos) break;
        size_t end = xml.find(L'>', r);
        if (end == std::wstring::npos) break;
        std::wstring tag = xml.substr(r, end - r + 1);
        unsigned long long off = 0, bytes = 0;
        if (ReadRangeAttr(tag, L"Offset", off) &&
            ReadRangeAttr(tag, L"Bytes", bytes)) {
            RangeInfo ri;
            ri.offset = (long long)off;
            ri.bytes  = (unsigned long)bytes;
            ranges.push_back(ri);
        }
        p = end + 1;
    }
    return !ranges.empty();
}

// 把原始 XML 字节转成宽文本（UTF-16LE BOM/无 BOM、UTF-8 三种都兼容）
bool BytesToWideXml(const std::vector<unsigned char>& raw, std::wstring& wide)
{
    if (raw.size() >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) {
        size_t n = (raw.size() - 2) / 2;
        wide.assign(reinterpret_cast<const wchar_t*>(raw.data() + 2), n);
        return true;
    }
    if (raw.size() >= 2 && raw[0] == 0x3C && raw[1] == 0x00) {
        wide.assign(reinterpret_cast<const wchar_t*>(raw.data()), raw.size() / 2);
        return true;
    }
    int cp = CP_UTF8;
    if (raw.size() >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF)
        cp = CP_UTF8;
    int n = MultiByteToWideChar(cp, 0, (const char*)raw.data(),
                                static_cast<int>(raw.size()), nullptr, 0);
    if (n <= 0) return false;
    wide.resize(n);
    MultiByteToWideChar(cp, 0, (const char*)raw.data(), static_cast<int>(raw.size()),
                        &wide[0], n);
    return true;
}

// 读 WIM 头 + XML 资源并解析；加密 ESD 才返回 true
bool LoadEncryptedDoc(const std::wstring& path,
                      WimHeaderDisk& hdr,
                      std::vector<unsigned char>& xmlRaw,
                      std::wstring& keyB64,
                      std::vector<RangeInfo>& ranges,
                      std::wstring* pErr)
{
    EsdFileGuard h = EsdFileGuard(CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr));
    if (h == INVALID_HANDLE_VALUE) {
        if (pErr) *pErr = L"无法打开 ESD 文件。";
        return false;
    }
    long long fileSize = 0;
    LARGE_INTEGER liSize;
    liSize.QuadPart = 0;
    GetFileSizeEx(h, &liSize);
    fileSize = liSize.QuadPart;
    if (fileSize < (long long)sizeof(WimHeaderDisk)) {
        if (pErr) *pErr = L"文件过小，不是有效的 WIM/ESD。";
        return false;
    }
    unsigned long cb = 0;
    if (!ReadAt(h, &hdr, (unsigned long)sizeof(WimHeaderDisk), 0, &cb) ||
        cb != sizeof(WimHeaderDisk)) {
        if (pErr) *pErr = L"读取 WIM 头失败。";
        return false;
    }

    if (hdr.magic != kMswimMagic) {
        if (pErr) *pErr = L"头部魔数不是 MSWIM。";
        return false;
    }

    long long xmlOff = hdr.xml.offset_in_wim;
    unsigned long xmlSize = (unsigned long)hdr.xml.size_in_wim;
    if (xmlSize == 0 || xmlOff < (long long)sizeof(WimHeaderDisk) ||
        xmlOff + (long long)xmlSize > fileSize) {
        if (pErr) *pErr = L"WIM 头中的 XML 资源位置无效。";
        return false;
    }
    if (!ReadAllBytes(path, xmlRaw, xmlOff, xmlSize)) {
        if (pErr) *pErr = L"读取 XML 资源失败。";
        return false;
    }
    std::wstring wide;
    if (!BytesToWideXml(xmlRaw, wide)) {
        if (pErr) *pErr = L"XML 资源编码无法识别。";
        return false;
    }
    if (!ParseEncryptedXml(wide, keyB64, ranges))
        return false;   // 无 ENCRYPTED/RANGE：普通（未加密）ESD
    return true;
}

// 构造 SIMPLEBLOB：12 字节头 + 逆序的 <KEY> 解码数据
bool BuildSessionBlob(const std::wstring& keyB64,
                      std::vector<unsigned char>& blob)
{
    std::vector<unsigned char> raw;
    if (!Base64Decode(keyB64, raw) || raw.empty()) return false;

    blob.resize(12 + raw.size());
    // PUBLICKEYSTRUC: SIMPLEBLOB / v2 / reserved 0 / CALG_AES_256
    blob[0] = 0x01;
    blob[1] = 0x02;
    blob[2] = 0x00;
    blob[3] = 0x00;
    blob[4] = 0x10;
    blob[5] = 0x66;
    blob[6] = 0x00;
    blob[7] = 0x00;
    // SIMPLEBLOB 附加 ALG_ID：CALG_RSA_KEYX
    blob[8]  = 0x00;
    blob[9]  = 0xA4;
    blob[10] = 0x00;
    blob[11] = 0x00;
    // 解码字节逆序
    for (size_t i = 0; i < raw.size(); ++i)
        blob[12 + i] = raw[raw.size() - 1 - i];
    return true;
}

// 整文件复制（用于把源 ESD 复制为目标后再就地解密）
bool CopyWholeFile(const std::wstring& src, const std::wstring& dst)
{
    EsdFileGuard hs = EsdFileGuard(CreateFileW(src.c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, 0, nullptr));
    if (hs == INVALID_HANDLE_VALUE) return false;
    EsdFileGuard hd = EsdFileGuard(CreateFileW(dst.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (hd == INVALID_HANDLE_VALUE) { return false; }  // EsdFileGuard 自动关闭 hs

    const unsigned long kChunk = 4 * 1024 * 1024;
    std::vector<unsigned char> buf(kChunk);
    bool ok = true;
    for (;;) {
        unsigned long cbRead = 0;
        if (!ReadFile(hs, buf.data(), kChunk, &cbRead, nullptr)) { ok = false; break; }
        if (cbRead == 0) break;
        unsigned long cbWrite = 0;
        if (!WriteFile(hd, buf.data(), cbRead, &cbWrite, nullptr) ||
            cbWrite != cbRead) { ok = false; break; }
    }
    FlushFileBuffers(hd);
    return ok;
}

} // anonymous namespace

bool IsEncryptedEsd(const std::wstring& path)
{
    WimHeaderDisk hdr;
    std::vector<unsigned char> xmlRaw;
    std::wstring keyB64;
    std::vector<RangeInfo> ranges;
    return LoadEncryptedDoc(path, hdr, xmlRaw, keyB64, ranges, nullptr);
}

// [R125-1] 从候选私钥中挑出能解包 sessionBlob 的那一把（返回其导入后的 hRsa）。
//  任一候选失败即销毁并继续；全部失败返回 0（调用方负责释放 hProv 并报错）。
static HCRYPTKEY PickSessionKey(HCRYPTPROV hProv,
                                const std::vector<std::wstring>& base64Keys,
                                const std::vector<unsigned char>& sessionBlob)
{
    HCRYPTKEY hRsa = 0;
    for (size_t ki = 0; ki < base64Keys.size(); ++ki) {
        std::vector<unsigned char> keyBlob;
        if (!Base64Decode(base64Keys[ki], keyBlob)) continue;
        if (!CryptImportKey(hProv, keyBlob.data(), static_cast<DWORD>(keyBlob.size()), 0,
                            CRYPT_OAEP, &hRsa)) {
            hRsa = 0;
            continue;
        }
        HCRYPTKEY hTry = 0;
        BOOL ok = CryptImportKey(hProv, sessionBlob.data(),
                                 static_cast<DWORD>(sessionBlob.size()), hRsa,
                                 CRYPT_OAEP, &hTry);
        if (ok) {
            CryptDestroyKey(hTry);
            break;   // 匹配
        }
        CryptDestroyKey(hRsa);
        hRsa = 0;
    }
    return hRsa;
}

// [R125-1] 逐区间解密：读 16 字节对齐块 → AES 解密 → 原位置回写。
//  失败返回 false 并通过 dwWinErr 带出系统错误码；WIM 头/XML 回写由调用方负责。
static bool DecryptRanges(HANDLE hd, HCRYPTPROV hProv, HCRYPTKEY hRsa,
                          const std::vector<unsigned char>& sessionBlob,
                          const std::vector<RangeInfo>& ranges, unsigned long& dwWinErr)
{
    for (size_t i = 0; i < ranges.size(); ++i) {
        // Bytes 按 16 字节对齐（原版：uVar5 & 0xfffffff0）
        unsigned long aligned = ranges[i].bytes & 0xFFFFFFF0;
        if (aligned == 0) continue;

        std::vector<unsigned char> buf(aligned);
        unsigned long cbRead = 0;
        if (!ReadAt(hd, buf.data(), aligned, ranges[i].offset, &cbRead) ||
            cbRead != aligned) {
            dwWinErr = GetLastError();
            return false;
        }

        HCRYPTKEY hAes = 0;
        if (!CryptImportKey(hProv, sessionBlob.data(),
                            static_cast<DWORD>(sessionBlob.size()), hRsa, CRYPT_OAEP, &hAes)) {
            dwWinErr = GetLastError();
            return false;
        }
        DWORD cbData = aligned;
        if (!CryptDecrypt(hAes, 0, FALSE, 0, buf.data(), &cbData)) {
            dwWinErr = GetLastError();
            CryptDestroyKey(hAes);
            return false;
        }
        CryptDestroyKey(hAes);

        unsigned long cbWrite = 0;
        if (!WriteAt(hd, buf.data(), aligned, ranges[i].offset, &cbWrite) ||
            cbWrite != aligned) {
            dwWinErr = GetLastError();
            return false;
        }
    }
    return true;
}


bool DecryptToFile(const std::wstring& srcEsd,
                   const std::wstring& dstWim,
                   const std::vector<std::wstring>& base64Keys,
                   std::wstring* pErr)
{
    WimHeaderDisk hdr;
    std::vector<unsigned char> xmlRaw;
    std::wstring keyB64;
    std::vector<RangeInfo> ranges;
    if (!LoadEncryptedDoc(srcEsd, hdr, xmlRaw, keyB64, ranges, pErr)) {
        if (pErr && pErr->empty())
            *pErr = L"该 ESD 未加密或不含 ENCRYPTED/RANGE 节点，无需解密。";
        return false;
    }

    std::vector<unsigned char> sessionBlob;
    if (!BuildSessionBlob(keyB64, sessionBlob)) {
        if (pErr) *pErr = L"ESD 内置 KEY 数据无法解码。";
        return false;
    }
    if (base64Keys.empty()) {
        if (pErr) *pErr = L"缺少解密私钥（Data.xml 未提供 ESDDecryptKey）。";
        return false;
    }

    // 先复制源文件，之后对目标就地解密
    if (!CopyWholeFile(srcEsd, dstWim)) {
        if (pErr) *pErr = L"创建目标文件失败（磁盘空间不足或路径不可写）。";
        return false;
    }

    HCRYPTPROV hProv = 0;
    if (!CryptAcquireContextW(&hProv, nullptr, nullptr, PROV_RSA_AES,
                              CRYPT_VERIFYCONTEXT)) {
        if (pErr) *pErr = L"CryptAcquireContext 失败，系统加密服务不可用。";
        DeleteFileW(dstWim.c_str());   // [R120] 失败路径：删半成品，删不掉留痕见下
        return false;
    }

    // 逐条尝试 Data.xml 私钥：能用 SIMPLEBLOB 成功解包者即为匹配密钥


    HCRYPTKEY hRsa = PickSessionKey(hProv, base64Keys, sessionBlob);
    if (!hRsa) {
        CryptReleaseContext(hProv, 0);
        if (pErr) *pErr = L"提供的私钥均无法解包该 ESD 的会话密钥（密钥与 ESD 版本不匹配）。";
        DeleteFileW(dstWim.c_str());
        return false;
    }

    EsdFileGuard hd = EsdFileGuard(CreateFileW(dstWim.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                nullptr, OPEN_EXISTING, 0, nullptr));
    if (hd == INVALID_HANDLE_VALUE) {
        CryptDestroyKey(hRsa);
        CryptReleaseContext(hProv, 0);
        if (pErr) *pErr = L"无法重新打开目标文件进行解密。";
        DeleteFileW(dstWim.c_str());
        return false;
    }

    bool ok = true;
    unsigned long dwWinErr = 0;
    if (!DecryptRanges(hd, hProv, hRsa, sessionBlob, ranges, dwWinErr)) {
        ok = false;
    }

    if (ok) {
        unsigned long cbW = 0;
        if (!WriteAt(hd, &hdr, (unsigned long)sizeof(WimHeaderDisk), 0, &cbW) ||
            cbW != sizeof(WimHeaderDisk)) {
            ok = false; dwWinErr = GetLastError();
        }
        if (ok && !xmlRaw.empty()) {
            if (!WriteAt(hd, xmlRaw.data(), (unsigned long)xmlRaw.size(),
                         hdr.xml.offset_in_wim, &cbW) ||
                cbW != (unsigned long)xmlRaw.size()) {
                ok = false; dwWinErr = GetLastError();
            }
        }
    }

    FlushFileBuffers(hd);
    CryptDestroyKey(hRsa);
    CryptReleaseContext(hProv, 0);

    if (!ok) {
        if (pErr) {
            std::wstring sErr;
            sErr.resize(128);
            wfmt::FormatInto(sErr.data(), sErr.size() + 1,
                         L"解密过程中发生错误（Windows 错误码 %lu）。", dwWinErr);
            sErr.resize(wcslen(sErr.c_str()));
            *pErr = sErr;
        }
        DeleteFileW(dstWim.c_str());
        return false;
    }
    return true;
}

} // namespace EsdDecryptor
// [C++17 R3] 本文件内联常量（C++17 inline variables，多 TU ODR 一致）：
