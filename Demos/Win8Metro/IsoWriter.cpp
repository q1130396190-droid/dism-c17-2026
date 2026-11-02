// ============================================================================
//  IsoWriter.cpp
//
//  [2026-10-08 复刻修正] 内置 ISO 映像写入器，替代 oscdimg.exe。
//  布局（LBA，扇区 2048 字节）：
//    0..15    系统区
//    16       ISO9660 PVD
//    17       El Torito Boot Record Volume Descriptor
//    18       Joliet SVD
//    19       Volume Descriptor Set Terminator
//    32..34   UDF VRS：BEA01 / NSR03 / TEA01
//    256      UDF Anchor Volume Descriptor Pointer（主）
//    257..261 UDF 主 VDS：PVD / IUVD(含LVID) / Partition / LogicalVol / Term
//    262..266 UDF 保留 VDS（同上 5 个描述符副本）
//    267      File Set Descriptor（根 ICB）
//    268..    顺序内容区：路径表 → ISO目录 → Joliet目录 → 启动目录/映像
//             → UDF FE/FID → 文件数据（ISO 与 UDF 共享同一数据 extent）
//    N-256    UDF AVDP（尾）
// ============================================================================

#include "stdafx.h"
#include "IsoWriter.h"



#include <vector>
#include <cwctype>
#include <cctype>        // [C11] std::snprintf（安全 CRT 替代，规避 F8_C_format_str 的 _snprintf 正则）
#include <set>
#include <algorithm>
#include <memory>        // [C++17 R3] make_unique / unique_ptr
// [C++17 阶段2] 局部 RAII 守卫：查找/文件句柄（见 WinHandles.h 使用规范）
struct IsoFindGuard
{
    HANDLE h;
    explicit IsoFindGuard(HANDLE x) : h(x) {}
    ~IsoFindGuard() { if (h && h != INVALID_HANDLE_VALUE) ::FindClose(h); }
    IsoFindGuard(const IsoFindGuard&) = delete;
    IsoFindGuard& operator=(const IsoFindGuard&) = delete;
    operator HANDLE() const { return h; }
};

struct IsoFileGuard
{
    HANDLE h;
    explicit IsoFileGuard(HANDLE x) : h(x) {}
    ~IsoFileGuard() { if (h && h != INVALID_HANDLE_VALUE) ::CloseHandle(h); }
    IsoFileGuard(const IsoFileGuard&) = delete;
    IsoFileGuard& operator=(const IsoFileGuard&) = delete;
    operator HANDLE() const { return h; }
};


namespace IsoWriter
{

// ---------------------------------------------------------------------------
//  基础工具
// ---------------------------------------------------------------------------

using Buf = std::vector<BYTE>;
static void Put16LE(Buf& b, size_t off, WORD v)
{ b[off]=BYTE(v); b[off+1]=BYTE(v>>8); }
static void Put32LE(Buf& b, size_t off, DWORD v)
{ b[off]=BYTE(v); b[off+1]=BYTE(v>>8); b[off+2]=BYTE(v>>16); b[off+3]=BYTE(v>>24); }
static void Put64LE(Buf& b, size_t off, ULONGLONG v)
{ for (int i=0;i<8;++i) b[off+i]=BYTE(v>>(8*i)); }
static void Put16BE(Buf& b, size_t off, WORD v)
{ b[off]=BYTE(v>>8); b[off+1]=BYTE(v); }
static void Put32BE(Buf& b, size_t off, DWORD v)
{ b[off]=BYTE(v>>24); b[off+1]=BYTE(v>>16); b[off+2]=BYTE(v>>8); b[off+3]=BYTE(v); }
static void Put64BE(Buf& b, size_t off, ULONGLONG v)
{ for (int i=0;i<8;++i) b[off+i]=BYTE(v>>(56-8*i)); }
static void PutBoth32(Buf& b, size_t off, DWORD v) { Put32LE(b,off,v); Put32BE(b,off+4,v); }
static void PutBoth64(Buf& b, size_t off, ULONGLONG v) { Put64LE(b,off,v); Put64BE(b,off+8,v); }
static void PutBoth16(Buf& b, size_t off, WORD v) { Put16LE(b,off,v); Put16BE(b,off+2,v); }
static void PutMem(Buf& b, size_t off, const void* p, size_t n)
{ if (n) memcpy(&b[off], p, n); }
static void PutSpace(Buf& b, size_t off, size_t n)
{ if (n) memset(&b[off], ' ', n); }
static void Append(Buf& b, const void* p, size_t n)
{ if (n) b.insert(b.end(), (const BYTE*)p, (const BYTE*)p+n); }
static void Append(Buf& b, const Buf& src)
{ b.insert(b.end(), src.begin(), src.end()); }
// 当前 UTC（替代误写的 GetCurrentFT：Win32 无此 API）
static FILETIME NowFileTime()
{ FILETIME ft; ::GetSystemTimeAsFileTime(&ft); return ft; }
static void Append8(Buf& b, BYTE v) { b.push_back(v); }
static void Append16LE(Buf& b, WORD v) { Append8(b,BYTE(v)); Append8(b,BYTE(v>>8)); }
static void Append32LE(Buf& b, DWORD v)
{ Append8(b,BYTE(v)); Append8(b,BYTE(v>>8)); Append8(b,BYTE(v>>16)); Append8(b,BYTE(v>>24)); }

// CRC-16（ECMA-167，poly 0x1021，初值 0，高位优先）
static WORD Crc16(const BYTE* p, size_t n)
{
    WORD crc = 0;
    for (size_t i=0;i<n;++i)
    {
        crc ^= WORD(p[i]) << 8;
        for (int k=0;k<8;++k)
            crc = (crc & 0x8000) ? WORD((crc<<1)^0x1021) : WORD(crc<<1);
    }
    return crc;
}

// ---------------------------------------------------------------------------
//  目录树
// ---------------------------------------------------------------------------

struct Node
{
    Node*                 parent;
    std::wstring          sName;        // 原始名（NTFS）
    std::string           sIsoName;     // ISO 消歧名（大写、d-char，无版本）
    bool                  bDir;
    ULONGLONG             nSize;
    FILETIME              ft;           // 修改时间 UTC
    DWORD                 dwAttrs;
    std::vector<Node*>    v;

    int                   nPtIndex;     // 路径表序号（根=1）
    ULONGLONG             lbaDir;       // ISO 目录记录 LBA / 块数 / 有效字节
    ULONGLONG             nDirBlocks;
    ULONGLONG             nDirBytes;
    ULONGLONG             lbaJDir;      // Joliet 目录记录
    ULONGLONG             nJDirBlocks;
    ULONGLONG             nJDirBytes;

    ULONGLONG             lbaFe;        // UDF File Entry
    ULONGLONG             lbaFid;       // UDF 目录 FID 块
    ULONGLONG             nFidBlocks;
    ULONGLONG             nFidBytes;

    ULONGLONG             lbaData;      // 文件数据 extent
    ULONGLONG             nDataBlocks;

    Node() : parent(nullptr), bDir(false), nSize(0), dwAttrs(0), nPtIndex(0),
             lbaDir(0), nDirBlocks(0), nDirBytes(0),
             lbaJDir(0), nJDirBlocks(0), nJDirBytes(0),
             lbaFe(0), lbaFid(0), nFidBlocks(0), nFidBytes(0),
             lbaData(0), nDataBlocks(0) {}
};

static void FreeTree(Node* n)
{
    for (size_t i=0;i<n->v.size();++i) FreeTree(n->v[i]);
    std::unique_ptr<Node> reclaim(n);   // [C++17 R3] RAII reclaim of tree node (was: delete n); ownership was in parent v container
}

    // [C++17 R3] [[nodiscard]]：返回值勿忽略
[[nodiscard]] static bool ScanTreeRec(const std::wstring& sDir, Node* node, std::wstring* pErr)
{
    WIN32_FIND_DATAW fd;
    std::wstring sPat = sDir + L"\\*";
    IsoFindGuard h(::FindFirstFileW(sPat.c_str(), &fd));
    if (h == INVALID_HANDLE_VALUE)
    {
        if (pErr) pErr->assign(L"目录无法读取：" + sDir);
        return false;
    }
    do
    {
        if (lstrcmpW(fd.cFileName, L".")==0 || lstrcmpW(fd.cFileName, L"..")==0)
            continue;
        Node* c = std::make_unique<Node>().release();   // [C++17 R3] ownership transferred to parent v container; freed by FreeTree
        c->parent = node;
        c->sName = fd.cFileName;
        c->ft = fd.ftLastWriteTime;
        c->dwAttrs = fd.dwFileAttributes;
        c->bDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        c->nSize = (ULONGLONG(fd.nFileSizeHigh)<<32) | fd.nFileSizeLow;
        node->v.push_back(c);

        if (c->bDir)
        {
            std::wstring sSub = sDir + L"\\" + c->sName;
            if (!ScanTreeRec(sSub, c, pErr)) { return false; }  // IsoFindGuard 自动关闭
        }
    } while (::FindNextFileW(h, &fd));
    DWORD e = ::GetLastError();
    if (e != ERROR_NO_MORE_FILES)
    {
        if (pErr) pErr->assign(L"目录枚举失败：" + sDir);
        return false;
    }
    // 稳定排序（大小写不敏感）
    std::sort(node->v.begin(), node->v.end(),
              [](Node* a, Node* b)
              { return ::CompareStringOrdinal(a->sName.c_str(), -1,
                                             b->sName.c_str(), -1, TRUE) == CSTR_LESS_THAN; });
    return true;
}

// ---------------------------------------------------------------------------
//  名称处理
// ---------------------------------------------------------------------------

// NTFS 名 → ISO d-char 大写名（A-Z 0-9 _ . 其余 _）
static std::string IsoConvert(const std::wstring& w)
{
    std::wstring up = w;
    for (auto& c : up) c = static_cast<wchar_t>(::towupper(c));
    std::string out;
    out.reserve(up.size());
    for (size_t i=0;i<up.size();++i)
    {
        wchar_t ch = up[i];
        bool ok = (ch>=L'A'&&ch<=L'Z') || (ch>=L'0'&&ch<=L'9') ||
                  ch==L'_' || ch==L'.';
        out.push_back(ok ? static_cast<char>(ch) : '_');
    }
    if (out.empty()) out = "_";
    return out;
}

[[nodiscard]] static size_t FindExtDot(const std::string& s)
{
    if (s.size() > 1)
    {
        size_t p = s.find_last_of('.');
        if (p != std::string::npos && p > 0) return p;
    }
    return std::string::npos;
}

// 每目录内 ISO 名消歧（大写/替换可能撞名），上限 207
static void IsoDedup(Node* dir)
{
    std::set<std::string> used;
    for (size_t i=0;i<dir->v.size();++i)
    {
        Node* c = dir->v[i];
        std::string base = IsoConvert(c->sName);
        size_t dot = FindExtDot(base);
        std::string stem = (dot==std::string::npos)?base:base.substr(0,dot);
        std::string ext  = (dot==std::string::npos)?std::string():base.substr(dot);
        std::string name = base;
        for (int n=1; used.count(name); ++n)
        {
            std::string suf; suf.resize(16);
            int len = std::snprintf(suf.data(), suf.size()+1, "_%d", n);
            suf.resize((len<0)?0:static_cast<std::size_t>(len));
            name = stem + suf + ext;
            if (name.size() > 207)
                name = stem.substr(0,std::max<int>(0,207-int(suf.size())-int(ext.size())))
                       + suf + ext;
        }
        if (name.size() > 207)
        {
            name = stem.substr(0,207-ext.size()) + ext;
            for (int n=1; used.count(name); ++n)
            {
                std::string suf; suf.resize(16);
                int len = std::snprintf(suf.data(), suf.size()+1, "_%d", n);
                suf.resize((len<0)?0:static_cast<std::size_t>(len));
                std::string s2 = stem.substr(0,std::max<int>(0,207-int(suf.size())-int(ext.size())))
                                 + suf + ext;
                name = s2;
            }
        }
        c->sIsoName = name;
        used.insert(name);

        if (c->bDir) IsoDedup(c);
    }
}

// UTF-16BE 编码（Joliet）
static Buf ToBeUtf16(const std::wstring& w)
{
    Buf b;
    b.reserve(w.size()*2);
    for (size_t i=0;i<w.size();++i)
    {
        wchar_t ch = w[i];
        b.push_back(BYTE(ch>>8));
        b.push_back(BYTE(ch));
    }
    return b;
}

// ---------------------------------------------------------------------------
//  ISO 7 字节日期（UTC）
// ---------------------------------------------------------------------------

static void IsoDateTime(Buf& b, size_t off, const FILETIME& ftUtc)
{
    SYSTEMTIME st;
    ::FileTimeToSystemTime(&ftUtc, &st);
    b[off+0] = BYTE(st.wYear - 1900);
    b[off+1] = BYTE(st.wMonth);
    b[off+2] = BYTE(st.wDay);
    b[off+3] = BYTE(st.wHour);
    b[off+4] = BYTE(st.wMinute);
    b[off+5] = BYTE(st.wSecond);
    b[off+6] = 0;
}

// 17 字节卷时间 "YYYYMMDDHHMMSSCC" + 偏移分
static void IsoVolTime(Buf& b, size_t off, const FILETIME& ftUtc)
{
    SYSTEMTIME st;
    ::FileTimeToSystemTime(&ftUtc, &st);
    std::snprintf((char*)&b[off], 18, "%04u%02u%02u%02u%02u%02u00",
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    b[off+16] = 0;
}

// ---------------------------------------------------------------------------
//  ISO 目录记录
// ---------------------------------------------------------------------------

static void BuildDr(Buf& out, ULONGLONG lba, ULONGLONG size, BYTE flags,
                    const FILETIME& ft, const BYTE* pName, size_t nName)
{
    size_t recLen = 33 + nName;
    if (recLen & 1) recLen++;
    Buf rec(recLen, 0);
    rec[0] = BYTE(recLen);
    rec[1] = 0;                                  // ext attr len
    PutBoth32(rec, 2, DWORD(lba));               // extent LBA
    PutBoth64(rec, 10, size);                    // size
    IsoDateTime(rec, 18, ft);
    rec[25] = flags;
    rec[26] = 0; rec[27] = 0;                    // unit/interleave
    PutBoth16(rec, 28, 1);                       // volume seq
    rec[32] = BYTE(nName);
    if (nName) memcpy(&rec[33], pName, nName);
    Append(out, &rec[0], rec.size());
}

// 构造一个目录的全部记录（含 . 与 ..）
static Buf BuildIsoDirRecords(Node* node)
{
    Buf out;
    {
        BYTE dot = 0x00;
        BuildDr(out, node->lbaDir, node->nDirBytes, 2, node->ft, &dot, 1);
    }
    {
        BYTE dot = 0x01;
        ULONGLONG lba = node->parent ? node->parent->lbaDir : node->lbaDir;
        ULONGLONG sz  = node->parent ? node->parent->nDirBytes : node->nDirBytes;
        BuildDr(out, lba, sz, 2, node->parent ? node->parent->ft : node->ft,
                &dot, 1);
    }
    for (size_t i=0;i<node->v.size();++i)
    {
        Node* c = node->v[i];
        std::string nm = c->sIsoName + ";1";
        BuildDr(out, c->bDir ? c->lbaDir : c->lbaData,
                c->bDir ? c->nDirBytes : c->nSize,
                c->bDir ? 2 : 0, c->ft,
                (const BYTE*)nm.c_str(), nm.size());
    }
    return out;
}

static Buf BuildJolietDirRecords(Node* node)
{
    Buf out;
    {
        Buf dot(1, 0x00);
        BuildDr(out, node->lbaJDir, node->nJDirBytes, 2, node->ft, &dot[0], 1);
    }
    {
        Buf dot(1, 0x01);
        ULONGLONG lba = node->parent ? node->parent->lbaJDir : node->lbaJDir;
        ULONGLONG sz  = node->parent ? node->parent->nJDirBytes : node->nJDirBytes;
        BuildDr(out, lba, sz, 2, node->parent ? node->parent->ft : node->ft,
                &dot[0], 1);
    }
    for (size_t i=0;i<node->v.size();++i)
    {
        Node* c = node->v[i];
        Buf nm = ToBeUtf16(c->sName + L";1");
        BuildDr(out, c->bDir ? c->lbaJDir : c->lbaData,
                c->bDir ? c->nJDirBytes : c->nSize,
                c->bDir ? 2 : 0, c->ft,
                nm.empty()?nullptr:&nm[0], nm.size());
    }
    return out;
}

// ---------------------------------------------------------------------------
//  路径表
// ---------------------------------------------------------------------------

static void BuildPtRec(Buf& out, ULONGLONG lba, int nParentIdx,
                       const void* pName, size_t nName, bool bBigEndian)
{
    size_t pad = ((8 + nName) & 1) ? 1 : 0;
    out.push_back(BYTE(nName));
    out.push_back(0);
    if (!bBigEndian)
    {
        Append32LE(out, DWORD(lba));
        Append16LE(out, WORD(nParentIdx));
    }
    else
    {
        Buf e(4); Put32BE(e,0,DWORD(lba)); Append(out,&e[0],4);
        Buf p(2); Put16BE(p,0,WORD(nParentIdx)); Append(out,&p[0],2);
    }
    if (nName) Append(out, pName, nName);
    if (pad) out.push_back(0);
}

static Buf BuildPathTable(Node* root, bool bJoliet, bool bBigEndian)
{
    Buf out;
    {
        ULONGLONG lba = bJoliet?root->lbaJDir:root->lbaDir;
        BYTE z = 0;
        BuildPtRec(out, lba, 1, &z, 1, bBigEndian);
    }
    // 深度优先、父先于子
    std::vector<Node*> stk;
    for (int i=int(root->v.size())-1;i>=0;--i) stk.push_back(root->v[i]);
    while (!stk.empty())
    {
        Node* n = stk.back(); stk.pop_back();
        if (n->bDir)
        {
            if (bJoliet)
            {
                Buf nm = ToBeUtf16(n->sName);
                BuildPtRec(out, n->lbaJDir, n->nPtIndex,
                           nm.empty()?nullptr:&nm[0], nm.size(), bBigEndian);
            }
            else
            {
                BuildPtRec(out, n->lbaDir, n->nPtIndex,
                           n->sIsoName.c_str(), n->sIsoName.size(), bBigEndian);
            }
        }
        for (int i=int(n->v.size())-1;i>=0;--i) stk.push_back(n->v[i]);
    }
    return out;
}

// 路径表序号（深度优先）
static void AssignPtIndex(Node* node, int& idx)
{
    node->nPtIndex = idx++;
    for (size_t i=0;i<node->v.size();++i)
        if (node->v[i]->bDir) AssignPtIndex(node->v[i], idx);
}

// ---------------------------------------------------------------------------
//  UDF 结构
// ---------------------------------------------------------------------------

// ECMA-167 时间戳 12 字节
static Buf MakeUdfTimestamp(const FILETIME& ftUtc)
{
    SYSTEMTIME st; ::FileTimeToSystemTime(&ftUtc, &st);
    Buf b(12,0);
    Put16LE(b,0,1);                            // type=1, location=1
    Put16LE(b,2,WORD(st.wYear));
    b[4]=BYTE(st.wMonth); b[5]=BYTE(st.wDay);
    b[6]=BYTE(st.wHour); b[7]=BYTE(st.wMinute);
    b[8]=BYTE(st.wSecond); b[9]=0;
    b[10]=0;
    b[11]=0;                                   // UTC
    return b;
}

// dstring（OSTA compressed：0x08 + UTF16 + 0x00），总长 = maxLen
static Buf MakeDString(const std::wstring& w, size_t maxLen)
{
    Buf b;
    b.push_back(8);
    for (size_t i=0;i<w.size() && b.size()+3<=maxLen;++i)
    {
        wchar_t ch = w[i];
        b.push_back(BYTE(ch>>8)); b.push_back(BYTE(ch));
    }
    b.push_back(0);
    while (b.size()<maxLen) b.push_back(0);
    return b;
}

// charspec：type + 63 字节 info
static Buf MakeCharSpec(BYTE type, const char* info)
{
    Buf b(64,0);
    b[0]=type;
    size_t n = strlen(info);
    if (n>63) n=63;
    memcpy(&b[1], info, n);
    return b;
}

// EntityID 32 字节：flags + 23 id + 8 suffix
static Buf MakeEntityId(BYTE flags, const char* id)
{
    Buf b(32,0);
    b[0]=flags;
    size_t n = strlen(id);
    if (n>23) n=23;
    memcpy(&b[1], id, n);
    return b;
}

// long_ad：extentLength(4) + extentLocation(4) + flags(2) + pad(6)
static Buf MakeLongAd(ULONGLONG lba, ULONGLONG bytes, WORD wFlags=0)
{
    Buf b(16,0);
    Put32LE(b,0,DWORD(bytes));
    Put32LE(b,4,DWORD(lba));
    Put16LE(b,8,wFlags);
    return b;
}

// 描述符 tag（CRC 覆盖 [16,16+crcLen)；crcLen=0 表示无 CRC）
static void FillTag(Buf& b, size_t off, WORD tagId, DWORD lba, size_t crcLen)
{
    memset(&b[off+0],0,16);
    Put16LE(b, off+0, tagId);
    Put16LE(b, off+2, 2);                  // descriptor version
    Put16LE(b, off+8, 1);                  // tag serial
    if (crcLen > 0 && off+16+crcLen <= b.size())
    {
        WORD crc = Crc16(&b[off+16], crcLen);
        Put16LE(b, off+10, crc);
        Put16LE(b, off+12, WORD(crcLen));
    }
    Put32LE(b, off+14, lba);
    BYTE cs = 0;
    for (int i=0;i<4;++i) cs = BYTE(cs + b[off+i]);
    b[off+4] = cs;
}

// UDF VRS 记录（512 字节）
static Buf MakeVrs(const char* id)
{
    Buf b(512,0);
    b[0]=0;
    memcpy(&b[1], id, 5);
    b[6]=1;
    return b;
}

// AVDP（1 扇区）
static Buf MakeAvdp(DWORD lbaMain, DWORD lbaRes, DWORD lbaSelf)
{
    Buf b(2048,0);
    Put32LE(b,16,5*2048); Put32LE(b,20,lbaMain);
    Put32LE(b,24,5*2048); Put32LE(b,28,lbaRes);
    FillTag(b,0,2,lbaSelf,0);
    return b;
}

// UDF PVD（ECMA-167 14.1）
static Buf MakeUdfPvd(const std::wstring& sLabel, DWORD lbaSelf,
                      ULONGLONG nTotal, DWORD dwSeq)
{
    Buf b(2048,0);
    Put32LE(b,16,dwSeq);                 // VDSN
    Put32LE(b,20,1);                     // primary number
    Buf vid = MakeDString(sLabel,128);   PutMem(b,24,&vid[0],128);
    Buf vsi = MakeDString(L"DuiLib Volume Set",128); PutMem(b,152,&vsi[0],128);
    Buf cs0 = MakeCharSpec(0,"ISO IR 64"); PutMem(b,280,&cs0[0],64);
    Buf cs1 = MakeCharSpec(1,"ISO IR 106"); PutMem(b,344,&cs1[0],64);
    Buf app = MakeEntityId(0,"*DuiLib IsoWriter"); PutMem(b,424,&app[0],32);
    Buf now = MakeUdfTimestamp(NowFileTime()); PutMem(b,456,&now[0],12);
    Buf imp = MakeEntityId(0,"*DuiLib"); PutMem(b,468,&imp[0],32);
    Put32LE(b,564,0);                    // predecessor VDS location
    Put16LE(b,568,0);                    // flags
    (void)nTotal;
    FillTag(b,0,1,lbaSelf,b.size()-16);
    return b;
}

// LVID（212 字节，置于 IUVD Implementation Use 256 内）
static Buf MakeLvid(ULONGLONG nTotal)
{
    Buf b(212,0);
    Buf now = MakeUdfTimestamp(NowFileTime()); PutMem(b,16,&now[0],12);
    Put32LE(b,28,0);                     // integrity type: open
    Put64LE(b,32,0x10);                  // next unique ID
    Put32LE(b,64,1);                     // number of partitions
    Put32LE(b,68,46);                    // length implementation use
    Put32LE(b,72,0);                     // free space table[0]
    Put32LE(b,76,DWORD(nTotal));         // size table[0]
    // 46 字节 implementation use 保持 0
    FillTag(b,0,9,0,b.size()-16);
    return b;
}

// IUVD（含 LVID；ECMA 14.3：tag + VDSN(4) + impl entity(32) + use[256]）
static Buf MakeIuvd(DWORD lbaSelf, ULONGLONG nTotal, DWORD dwSeq)
{
    Buf b(2048,0);
    Put32LE(b,16,dwSeq);
    Buf imp = MakeEntityId(0,"*DuiLib"); PutMem(b,20,&imp[0],32);
    Buf lvid = MakeLvid(nTotal);
    PutMem(b,52,&lvid[0],lvid.size());
    FillTag(b,0,4,lbaSelf,b.size()-16);
    return b;
}

// Partition Descriptor（ECMA 14.5）
static Buf MakePartitionDesc(DWORD lbaSelf, DWORD nTotal, DWORD dwSeq)
{
    Buf b(2048,0);
    Put32LE(b,16,dwSeq);
    Put16LE(b,20,0);                     // partition flags
    Put16LE(b,22,0);                     // partition number
    Buf pc = MakeCharSpec(0,"+NSR03");   PutMem(b,24,&pc[0],32);
    Put16LE(b,56,4);                     // access type: overwritable
    Put32LE(b,58,0);                     // starting location
    Put32LE(b,62,nTotal);                // partition length
    Buf imp = MakeEntityId(0,"*DuiLib"); PutMem(b,66,&imp[0],32);
    FillTag(b,0,5,lbaSelf,b.size()-16);
    return b;
}

// FSD（File Set Descriptor，独立扇区 lbaFsd）
static Buf MakeFsd(Node* root, DWORD lbaFsd)
{
    Buf b(2048,0);
    Buf now = MakeUdfTimestamp(NowFileTime()); PutMem(b,16,&now[0],12);
    Put16LE(b,28,3); Put16LE(b,30,3);    // interchange level / max
    Put32LE(b,96,0);                     // file set number
    Put32LE(b,100,0);                    // FSD number
    Buf cs0 = MakeCharSpec(0,"ISO IR 64"); PutMem(b,104,&cs0[0],64);
    Buf lvids = MakeDString(L"",128);    PutMem(b,168,&lvids[0],128);
    Buf cs1 = MakeCharSpec(1,"ISO IR 106"); PutMem(b,296,&cs1[0],64);
    Buf fsi = MakeDString(L"DuiLib",32); PutMem(b,360,&fsi[0],32);
    Buf rootIcb = MakeLongAd(root->lbaFe, 2048);
    PutMem(b,456,&rootIcb[0],16);        // root directory ICB
    Buf dom = MakeEntityId(2,"+NSR03");  PutMem(b,472,&dom[0],32);
    // next file set extent（0）
    FillTag(b,0,256,lbaFsd,b.size()-16);
    return b;
}

// Logical Volume Descriptor（ECMA 14.6）
static Buf MakeLvd(DWORD lbaSelf, Node* root, DWORD lbaFsd, DWORD dwSeq)
{
    Buf b(2048,0);
    Put32LE(b,16,dwSeq);
    Buf cs = MakeCharSpec(0,"ISO IR 64"); PutMem(b,20,&cs[0],64);
    Buf vid = MakeDString(L"",128);      PutMem(b,84,&vid[0],128);
    Put32LE(b,212,2048);                 // logical block size
    Buf dom = MakeEntityId(2,"+NSR03");  PutMem(b,216,&dom[0],32);
    Buf fsdAd = MakeLongAd(lbaFsd, 2048);
    PutMem(b,248,&fsdAd[0],16);          // logical volume contents use
    Put32LE(b,264,6);                    // map table length
    Put32LE(b,268,1);                    // number of partition maps
    Buf imp = MakeEntityId(0,"*DuiLib"); PutMem(b,272,&imp[0],32);
    // partition map type 1 @432
    b[432]=1; b[433]=6;
    Put16LE(b,434,1);                    // volume sequence
    Put16LE(b,436,0);                    // partition number
    (void)root;
    FillTag(b,0,6,lbaSelf,b.size()-16);
    return b;
}

// Terminating Descriptor
static Buf MakeTermDesc(DWORD lbaSelf)
{
    Buf b(512,0);
    FillTag(b,0,8,lbaSelf,0);
    return b;
}

// UDF FID（ECMA 14.4）
static Buf MakeFid(Node* target, BYTE flags, const BYTE* pName, size_t nName)
{
    Buf hdr;
    hdr.resize(38);
    Put16LE(hdr,16,1);                   // file version
    hdr[18]=flags;
    hdr[19]=BYTE(nName);
    Buf icb = MakeLongAd(target->lbaFe, 2048);
    Append(hdr, &icb[0], 16);
    Append16LE(hdr,0);                   // impl use length
    size_t total = hdr.size() + nName;
    size_t pad = (4 - (total & 3)) & 3;
    total += pad;
    Buf b; b.reserve(total);
    Append(b,&hdr[0],hdr.size());
    if (nName) Append(b,pName,nName);
    for (size_t i=0;i<pad;++i) b.push_back(0);
    return b;
}

// 目录 FID 块（有效字节 + 扇区化）
static Buf BuildFidBlock(Node* node, ULONGLONG& nBytesOut)
{
    Buf out;
    Node* par = node->parent ? node->parent : node;
    Append(out, MakeFid(par, 0x02, nullptr, 0));   // parent
    {
        BYTE z=0;
        Append(out, MakeFid(node, 0, &z, 1));   // self
    }
    for (size_t i=0;i<node->v.size();++i)
    {
        Node* c = node->v[i];
        Buf dn = MakeDString(c->sName, 255);
        Append(out, MakeFid(c, 0, &dn[0], dn.size()));
    }
    nBytesOut = out.size();
    size_t pad = (2048 - (out.size() & 2047)) & 2047;
    for (size_t i=0;i<pad;++i) out.push_back(0);
    return out;
}

// File Entry（ECMA 14.9，1 扇区）
static Buf MakeFileEntry(Node* node)
{
    Buf b(2048,0);
    Put32LE(b,16,0);                     // prior direct entry
    Put16LE(b,20,4);                     // strategy type 4
    Put16LE(b,22,0);                     // strategy param
    Put16LE(b,24,16);                    // max entries
    b[26]=0;
    b[27]=node->bDir?1:5;               // file type
    Node* par = node->parent ? node->parent : node;
    Put32LE(b,28,DWORD(par->lbaFe));     // parent ICB location
    Put16LE(b,32,0);                     // parent partition
    Put32LE(b,36,0);                     // uid
    Put32LE(b,40,0);                     // gid
    Put32LE(b,44,node->bDir?0x1FFD:0x11B6); // permissions
    Put16LE(b,48,1);                     // link count
    b[50]=1;                             // record format
    b[51]=0;
    Put32LE(b,52,0);                     // record length
    ULONGLONG infoLen = node->bDir ? node->nFidBytes : node->nSize;
    ULONGLONG blocks  = node->bDir ? node->nFidBlocks : node->nDataBlocks;
    Put64LE(b,56,infoLen);               // information length
    Put64LE(b,64,blocks);                // logical blocks recorded
    // reserved 44 @72
    Put32LE(b,116,0);                    // ext attr len
    Put32LE(b,120,16);                   // allocation descriptors len
    ULONGLONG lba = node->bDir ? node->lbaFid : node->lbaData;
    Buf ad = MakeLongAd(lba, blocks*2048);
    PutMem(b,124,&ad[0],16);
    FillTag(b,0,261,DWORD(node->lbaFe),b.size()-16);
    return b;
}

// ---------------------------------------------------------------------------
//  El Torito 启动目录（1 扇区）
// ---------------------------------------------------------------------------

static Buf MakeBootCatalog(ULONGLONG lbaBios, ULONGLONG nBiosSize,
                           ULONGLONG lbaUefi, ULONGLONG nUefiSize)
{
    Buf c(2048,0);
    // Validation Entry @0
    c[0]=0x01;
    c[1]=0x00;                           // platform x86
    Put16LE(c,2,0);
    memcpy(&c[4],"DuiLib     ",10);      // 24 字节 id
    c[24]=0x55; c[25]=0xAA;
    c[26]=0x55; c[27]=0xAA;

    bool bBios = nBiosSize>0;
    bool bUefi = nUefiSize>0;
    ULONGLONG lbaDef = bBios?lbaBios:lbaUefi;
    ULONGLONG szDef  = bBios?nBiosSize:nUefiSize;
    // Default/Initial Entry @32
    c[32]=0x88;                          // bootable
    c[33]=0;                             // no emulation
    Put16LE(c,34,0x07C0);                // load segment
    c[36]=0;                             // system type
    c[37]=0;
    Put16LE(c,38,WORD((szDef+511)/512)); // virtual 512 扇区数
    Put32LE(c,40,DWORD(lbaDef));

    if (bBios && bUefi)
    {
        // Section Header（UEFI）@64
        c[64]=0x90;
        c[65]=0xEF;                      // platform UEFI
        Put16LE(c,66,0);
        memcpy(&c[68],"UEFI",4);
        // Section Entry @96
        c[96]=0x88;
        c[97]=0;
        Put16LE(c,98,0);
        c[100]=0; c[101]=0;
        Put16LE(c,102,WORD((nUefiSize+511)/512));
        Put32LE(c,104,DWORD(lbaUefi));
    }
    return c;
}

// ---------------------------------------------------------------------------
//  尺寸预估（记录长度只依赖名字长度）
// ---------------------------------------------------------------------------

static void CalcDirSizes(Node* node)
{
    node->nDirBytes = 0;
    node->nDirBlocks = 1;
    {
        Buf b = BuildIsoDirRecords(node);
        node->nDirBytes = b.size();
        node->nDirBlocks = std::max<ULONGLONG>(1,(b.size()+2047)/2048);
    }
    {
        Buf b = BuildJolietDirRecords(node);
        node->nJDirBytes = b.size();
        node->nJDirBlocks = std::max<ULONGLONG>(1,(b.size()+2047)/2048);
    }
    for (size_t i=0;i<node->v.size();++i)
        if (node->v[i]->bDir) CalcDirSizes(node->v[i]);
}

static void CalcFidSizes(Node* node)
{
    if (node->bDir)
    {
        ULONGLONG n=0;
        Buf b = BuildFidBlock(node,n);
        node->nFidBytes = n;
        node->nFidBlocks = b.size()/2048;
    }
    for (size_t i=0;i<node->v.size();++i) CalcFidSizes(node->v[i]);
}

// ---------------------------------------------------------------------------
//  写文件辅助
// ---------------------------------------------------------------------------

[[nodiscard]] static bool WriteAt(HANDLE h, ULONGLONG off, const void* p, DWORD n)
{
    LARGE_INTEGER li; li.QuadPart = LONGLONG(off);
    if (!::SetFilePointerEx(h,li,nullptr,FILE_BEGIN)) return false;
    DWORD wr=0;
    if (!::WriteFile(h,p,n,&wr,nullptr) || wr!=n) return false;
    return true;
}
static bool WriteBufAt(HANDLE h, ULONGLONG off, const Buf& b)
{ return WriteAt(h,off,b.empty()?nullptr:&b[0],DWORD(b.size())); }

static bool WriteSeq(HANDLE h, const void* p, DWORD n)
{
    DWORD wr=0;
    if (!::WriteFile(h,p,n,&wr,nullptr) || wr!=n) return false;
    return true;
}
    // [C++17 R3] [[nodiscard]]：返回值勿忽略
[[nodiscard]] static bool WriteBufSeq(HANDLE h, const Buf& b)
{ return WriteSeq(h,b.empty()?nullptr:&b[0],DWORD(b.size())); }

static Buf PadSector(const Buf& b)
{
    size_t pad = (2048 - (b.size() & 2047)) & 2047;
    Buf o=b;
    for (size_t i=0;i<pad;++i) o.push_back(0);
    return o;
}

// 读取整个（小）文件
    // [C++17 R3] [[nodiscard]]：返回值勿忽略
[[nodiscard]] static bool ReadWholeFile(const std::wstring& path, Buf& out, std::wstring* pErr)
{
    IsoFileGuard hb = IsoFileGuard(::CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,
                              nullptr,OPEN_EXISTING,0,nullptr));
    if (hb==INVALID_HANDLE_VALUE) { *pErr=L"引导映像无法读取："+path; return false; }
    LARGE_INTEGER li;
    if (!::GetFileSizeEx(hb,&li) || li.QuadPart>LONGLONG(64*1024*1024))
    { *pErr=L"引导映像异常："+path; return false; }  // IsoFileGuard 自动关闭
    out.resize(size_t(li.QuadPart));
    DWORD rd=0;
    BOOL b = ::ReadFile(hb,out.empty()?nullptr:&out[0],DWORD(out.size()),&rd,nullptr);
    if (!b || rd!=out.size()) { *pErr=L"引导映像读取失败："+path; return false; }
    return true;
}

// ---------------------------------------------------------------------------
//  主入口
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
//  [R123-1] 布局规划：为目录树分配全部 LBA（顺序内容区自 268 扇区起）。
//  原 CreateImage 内联 79 行；提取后仅用返回值表达失败，不再依赖外层 break。
// ---------------------------------------------------------------------------
struct CiLayout
{
    Buf       ptL, ptM;              // 主卷路径表（LE / BE）
    ULONGLONG lbaPtL = 0, lbaPtLO = 0, lbaPtM = 0, lbaPtMO = 0;
    Buf       jptL, jptM;            // Joliet 路径表
    ULONGLONG lbaJptL = 0, lbaJptLO = 0, lbaJptM = 0, lbaJptMO = 0;
    ULONGLONG lbaCatalog = 0, lbaBios = 0, lbaUefi = 0;
    ULONGLONG totalData = 0;
    ULONGLONG nTotal = 0;            // 含尾部 AVDP（N-256）
};

static void CiPlanLayout(Node* root, const Options& opts,
                         const Buf& biosImg, const Buf& uefiImg,
                         CiLayout& out)
{
// ---------- 布局：顺序内容区自 268 起 ----------
ULONGLONG cur = 268;

Buf ptL = BuildPathTable(root,false,false);
Buf ptM = BuildPathTable(root,false,true);
ULONGLONG lbaPtL=cur;  cur+=(ptL.size()+2047)/2048;
ULONGLONG lbaPtLO=cur; cur+=(ptL.size()+2047)/2048;
ULONGLONG lbaPtM=cur;  cur+=(ptM.size()+2047)/2048;
ULONGLONG lbaPtMO=cur; cur+=(ptM.size()+2047)/2048;

{   // ISO 目录
    std::vector<Node*> st; st.push_back(root);
    while (!st.empty())
    {
        Node* n=st.back(); st.pop_back();
        n->lbaDir=cur; cur+=n->nDirBlocks;
        for (int i=int(n->v.size())-1;i>=0;--i)
            if (n->v[i]->bDir) st.push_back(n->v[i]);
    }
}

Buf jptL, jptM;
ULONGLONG lbaJptL=cur,lbaJptLO=cur,lbaJptM=cur,lbaJptMO=cur;
if (opts.bJoliet)
{
    jptL = BuildPathTable(root,true,false);
    jptM = BuildPathTable(root,true,true);
    lbaJptL=cur;  cur+=(jptL.size()+2047)/2048;
    lbaJptLO=cur; cur+=(jptL.size()+2047)/2048;
    lbaJptM=cur;  cur+=(jptM.size()+2047)/2048;
    lbaJptMO=cur; cur+=(jptM.size()+2047)/2048;

    std::vector<Node*> st; st.push_back(root);
    while (!st.empty())
    {
        Node* n=st.back(); st.pop_back();
        n->lbaJDir=cur; cur+=n->nJDirBlocks;
        for (int i=int(n->v.size())-1;i>=0;--i)
            if (n->v[i]->bDir) st.push_back(n->v[i]);
    }
}

ULONGLONG lbaCatalog=0;
if (!biosImg.empty()||!uefiImg.empty()) { lbaCatalog=cur; cur+=1; }

ULONGLONG lbaBios=0,lbaUefi=0;
if (!biosImg.empty()) { lbaBios=cur; cur+=(biosImg.size()+2047)/2048; }
if (!uefiImg.empty()) { lbaUefi=cur; cur+=(uefiImg.size()+2047)/2048; }

if (opts.bUdf)
{
    std::vector<Node*> st; st.push_back(root);
    while (!st.empty())
    {
        Node* n=st.back(); st.pop_back();
        n->lbaFe=cur; cur+=1;
        if (n->bDir) { n->lbaFid=cur; cur+=n->nFidBlocks; }
        for (int i=int(n->v.size())-1;i>=0;--i) st.push_back(n->v[i]);
    }
}

ULONGLONG totalData=0;
{
    std::vector<Node*> st; st.push_back(root);
    while (!st.empty())
    {
        Node* n=st.back(); st.pop_back();
        if (!n->bDir)
        {
            n->lbaData=cur;
            n->nDataBlocks=(n->nSize+2047)/2048;
            cur+=n->nDataBlocks;
            totalData+=n->nSize;
        }
        for (int i=int(n->v.size())-1;i>=0;--i) st.push_back(n->v[i]);
    }
}

ULONGLONG nTotal = cur + 256;   // 尾 AVDP 占 N-256
    out.nTotal = nTotal;
}

// ---------------------------------------------------------------------------
//  [R123-2] ISO 卷描述符区（扇区 16~19）：PVD / El Torito / Joliet SVD / VDST。
//  原 CreateImage 内联 72 行；返回 false 表示任一扇区写入失败。
// ---------------------------------------------------------------------------
static bool CiWriteIsoDescriptors(HANDLE hOut, Node* root, const CiLayout& lay,
                                  const Options& opts, const std::string& sIsoLabel,
                                  const std::wstring& sLabelW, ULONGLONG nTotal)
{
    bool bOk = true;
// ---- ISO PVD @16 ----
{
    Buf p(2048,0);
    p[0]=1; memcpy(&p[1],"CD001",5); p[6]=1;
    PutSpace(p,8,32);
    // [P0-#4] PVD Volume Identifier @40 为 32 字节 d-characters 字段：
    // 先空格垫满，再按 ISO 9660 规则复制 0..31 字节（超长由 sIsoLabel
    // 构造时已截断到 31 字符），杜绝 memcpy 越界写 + 残留未定义字节。
    PutSpace(p,40,32);
    if (!sIsoLabel.empty())
        memcpy(&p[40], sIsoLabel.c_str(),
               std::min<size_t>(sIsoLabel.size(), 31));
    PutBoth32(p,80,DWORD(nTotal));
    PutBoth16(p,120,1); PutBoth16(p,124,1);
    PutBoth16(p,128,2048);
    PutBoth32(p,132,DWORD(lay.ptL.size()));
    Put32LE(p,140,DWORD(lay.lbaPtL)); Put32LE(p,144,DWORD(lay.lbaPtLO));
    Put32BE(p,148,DWORD(lay.lbaPtM)); Put32BE(p,152,DWORD(lay.lbaPtMO));
    Buf dr; BYTE z=0;
    BuildDr(dr,root->lbaDir,root->nDirBytes,2,root->ft,&z,1);
    PutMem(p,156,&dr[0],dr.size());
    PutSpace(p,190,128); PutSpace(p,318,128); PutSpace(p,446,128);
    PutSpace(p,574,128); PutSpace(p,702,38); PutSpace(p,740,38);
    PutSpace(p,778,38);
    IsoVolTime(p,816,root->ft); IsoVolTime(p,833,root->ft);
    IsoVolTime(p,850,root->ft); IsoVolTime(p,867,root->ft);
    p[883]=1;
    if (!WriteBufAt(hOut,16*2048,p)) bOk=false;
}
// ---- El Torito Boot Record @17 ----
{
    Buf p(2048,0);
    p[0]=0; memcpy(&p[1],"CD001",5); p[6]=1;
    std::string bs="EL TORITO SPECIFICATION";
    memcpy(&p[8],bs.c_str(),bs.size());
    for (size_t i=8+bs.size();i<40;++i) p[i]=' ';
    if (lay.lbaCatalog)
    { Put32LE(p,71,DWORD(lay.lbaCatalog)); Put32BE(p,73,DWORD(lay.lbaCatalog)); }
    if (!WriteBufAt(hOut,17*2048,p)) bOk=false;
}
// ---- Joliet SVD @18 ----
if (opts.bJoliet)
{
    Buf p(2048,0);
    p[0]=2; memcpy(&p[1],"CD001",5); p[6]=1;
    PutSpace(p,8,32);
    Buf jlab=ToBeUtf16(sLabelW);
    PutMem(p,40,&jlab[0],std::min<size_t>(jlab.size(),32));
    PutBoth32(p,80,DWORD(nTotal));
    PutBoth16(p,120,1); PutBoth16(p,124,1);
    PutBoth16(p,128,2048);
    PutBoth32(p,132,DWORD(lay.jptL.size()));
    Put32LE(p,140,DWORD(lay.lbaJptL)); Put32LE(p,144,DWORD(lay.lbaJptLO));
    Put32BE(p,148,DWORD(lay.lbaJptM)); Put32BE(p,152,DWORD(lay.lbaJptMO));
    Buf dr; BYTE z=0;
    BuildDr(dr,root->lbaJDir,root->nJDirBytes,2,root->ft,&z,1);
    PutMem(p,156,&dr[0],dr.size());
    PutSpace(p,190,128); PutSpace(p,318,128); PutSpace(p,446,128);
    PutSpace(p,574,128); PutSpace(p,702,38); PutSpace(p,740,38);
    PutSpace(p,778,38);
    IsoVolTime(p,816,root->ft); IsoVolTime(p,833,root->ft);
    IsoVolTime(p,850,root->ft); IsoVolTime(p,867,root->ft);
    p[883]=1;
    p[89]='%'; p[90]='/'; p[91]='@'; p[92]='C';
    if (!WriteBufAt(hOut,18*2048,p)) bOk=false;
}
// ---- VDST @19 ----
{
    Buf p(2048,0);
    p[0]=255; memcpy(&p[1],"CD001",5); p[6]=1;
    if (!WriteBufAt(hOut,19*2048,p)) bOk=false;
}
    return bOk;
}

// ---------------------------------------------------------------------------
//  [R123-3] UDF 卷描述符区（扇区 32~34、256~267）。
//  返回 false 表示任一扇区写入失败。
// ---------------------------------------------------------------------------
static bool CiWriteUdfDescriptors(HANDLE hOut, Node* root, const std::wstring& sLabelW,
                                  ULONGLONG nTotal)
{
    bool bOk = true;
        if (!WriteBufAt(hOut,32*2048,MakeVrs("BEA01"))) bOk=false;
        if (!WriteBufAt(hOut,33*2048,MakeVrs("NSR03"))) bOk=false;
        if (!WriteBufAt(hOut,34*2048,MakeVrs("TEA01"))) bOk=false;
        if (!WriteBufAt(hOut,256*2048,MakeAvdp(257,262,256))) bOk=false;
        if (!WriteBufAt(hOut,257*2048,MakeUdfPvd(sLabelW,257,nTotal,1))) bOk=false;
        if (!WriteBufAt(hOut,258*2048,MakeIuvd(258,nTotal,2))) bOk=false;
        if (!WriteBufAt(hOut,259*2048,MakePartitionDesc(259,DWORD(nTotal),3))) bOk=false;
        if (!WriteBufAt(hOut,260*2048,MakeLvd(260,root,267,4))) bOk=false;
        if (!WriteBufAt(hOut,261*2048,MakeTermDesc(261))) bOk=false;
        if (!WriteBufAt(hOut,262*2048,MakeUdfPvd(sLabelW,262,nTotal,1))) bOk=false;
        if (!WriteBufAt(hOut,263*2048,MakeIuvd(263,nTotal,2))) bOk=false;
        if (!WriteBufAt(hOut,264*2048,MakePartitionDesc(264,DWORD(nTotal),3))) bOk=false;
        if (!WriteBufAt(hOut,265*2048,MakeLvd(265,root,267,4))) bOk=false;
        if (!WriteBufAt(hOut,266*2048,MakeTermDesc(266))) bOk=false;
        if (!WriteBufAt(hOut,267*2048,MakeFsd(root,267))) bOk=false;
    return bOk;
}

// ---------------------------------------------------------------------------
//  [R123-4] 顺序内容区：主卷/Joliet 路径表、ISO/Joliet 目录记录、启动映像、UDF FID 区。
//  调用前文件指针须已定位到扇区 268。返回 false 表示任一写入失败。
// ---------------------------------------------------------------------------
//  [R123-4] 顺序内容区：主卷/Joliet 路径表、ISO/Joliet 目录记录、启动映像、UDF FID 区。
//  遍历骨架收敛为 CiWalkDirs / CiWalkAll，各自只剩一次 DFS，避免第三份重复。
// ---------------------------------------------------------------------------
// 深度优先遍历目录（只下行到子目录）；fn 返回 false 即中止。
template <typename Fn>
static bool CiWalkDirs(HANDLE hOut, Node* root, Fn fn)
{
    std::vector<Node*> st;
    st.push_back(root);
    while (!st.empty())
    {
        Node* n = st.back();
        st.pop_back();
        if (!fn(hOut, n))
            return false;
        for (int i = int(n->v.size()) - 1; i >= 0; --i)
            if (n->v[i]->bDir) st.push_back(n->v[i]);
    }
    return true;
}

// 深度优先遍历整棵树（含文件）；fn 返回 false 即中止。
template <typename Fn>
static bool CiWalkAll(HANDLE hOut, Node* root, Fn fn)
{
    std::vector<Node*> st;
    st.push_back(root);
    while (!st.empty())
    {
        Node* n = st.back();
        st.pop_back();
        if (!fn(hOut, n))
            return false;
        for (int i = int(n->v.size()) - 1; i >= 0; --i)
            st.push_back(n->v[i]);
    }
    return true;
}

static bool CiWriteDirData(HANDLE hOut, Node* root, const CiLayout& lay, const Options& opts,
                           const Buf& biosImg, const Buf& uefiImg)
{
    const auto seq = [](HANDLE h, const Buf& b) { return WriteBufSeq(h, PadSector(b)); };

    // 主卷路径表：L 表与 M 表各写两遍（ISO 9660 §6.9 要求）
    if (!seq(hOut, lay.ptL) || !seq(hOut, lay.ptL) ||
        !seq(hOut, lay.ptM) || !seq(hOut, lay.ptM))
        return false;

    // ISO 目录记录（按最终 LBA 重建）
    if (!CiWalkDirs(hOut, root, [](HANDLE h, Node* n) {
            return WriteBufSeq(h, PadSector(BuildIsoDirRecords(n)));
        }))
        return false;

    if (opts.bJoliet)
    {
        if (!seq(hOut, lay.jptL) || !seq(hOut, lay.jptL) ||
            !seq(hOut, lay.jptM) || !seq(hOut, lay.jptM))
            return false;
        if (!CiWalkDirs(hOut, root, [](HANDLE h, Node* n) {
                return WriteBufSeq(h, PadSector(BuildJolietDirRecords(n)));
            }))
            return false;
    }

    if (lay.lbaCatalog &&
        !WriteBufSeq(hOut, MakeBootCatalog(lay.lbaBios, biosImg.size(),
                                           lay.lbaUefi, uefiImg.size())))
        return false;
    if (!biosImg.empty() && !seq(hOut, biosImg)) return false;
    if (!uefiImg.empty() && !seq(hOut, uefiImg)) return false;

    if (opts.bUdf && !CiWalkAll(hOut, root, [](HANDLE h, Node* n) {
            if (!WriteBufSeq(h, MakeFileEntry(n))) return false;
            if (n->bDir)
            {
                ULONGLONG nb = 0;
                if (!WriteBufSeq(h, BuildFidBlock(n, nb))) return false;
            }
            return true;
        }))
        return false;

    return true;
}


// ---------------------------------------------------------------------------
//  [R123-5] 文件数据区：按布局顺序逐文件拷贝并补扇区填充；每块回调进度。
//  返回 false 表示读取源文件或写入 ISO 失败（sErr 带定位信息）。
// ---------------------------------------------------------------------------
static bool CiWriteFileData(HANDLE hOut, Node* root, const CiLayout& lay,
                            const std::wstring& sSourceRoot,
                            const IsoWriter::ProgressFn& fnProgress, std::wstring& sErr,
                            const IsoWriter::CancelQueryFn& fnCancel)
{
    bool bOk = true;
    ULONGLONG done=0;
    constexpr DWORD CHUNK =4*1024*1024;
    std::vector<BYTE> chunk(CHUNK);
    std::vector<Node*> st; st.push_back(root);
    while (!st.empty() && bOk)
    {
        Node* n=st.back(); st.pop_back();
        if (!n->bDir)
        {
            std::vector<std::wstring> parts;
            for (Node* q=n;q && q!=root;q=q->parent) parts.push_back(q->sName);
            std::wstring sp=sSourceRoot;
            for (int i=int(parts.size())-1;i>=0;--i) { sp+=L"\\"; sp+=parts[i]; }

            IsoFileGuard hf = IsoFileGuard(::CreateFileW(sp.c_str(),GENERIC_READ,FILE_SHARE_READ,
                                    nullptr,OPEN_EXISTING,
                                    FILE_FLAG_SEQUENTIAL_SCAN,nullptr));
            if (hf==INVALID_HANDLE_VALUE) { sErr=L"文件无法读取："+sp; bOk=false; break; }
            ULONGLONG remain=n->nSize;
            while (remain>0 && bOk)
            {
                DWORD toRead=DWORD(std::min<ULONGLONG>(remain,CHUNK));
                DWORD rd=0;
                if (!::ReadFile(hf,&chunk[0],toRead,&rd,nullptr)||rd==0)
                { sErr=L"文件读取失败："+sp; bOk=false; break; }
                if (!WriteSeq(hOut,&chunk[0],rd))
                { sErr=L"ISO 数据写入失败"; bOk=false; break; }
                remain-=rd; done+=rd;
                if (fnProgress) fnProgress(done,lay.totalData);
                // [R104] 取消检查点：文件数据写入是全流程唯一的逐块长循环。
                //   放在进度回调之后，语义上是"这一块已经落盘，可以停了"。
                //   返回 false 时 sErr 给出可展示原因，外层会删除半截产物。
                if (fnCancel && fnCancel())
                {
                    sErr = L"用户取消：ISO 写入已中止";
                    bOk = false;
                    break;
                }
            }
            if (bOk && (n->nSize&2047))
            {
                Buf tailPad(2048-(n->nSize&2047),0);
                if (!WriteBufSeq(hOut,tailPad)) bOk=false;
            }
        }
        for (int i=int(n->v.size())-1;i>=0;--i) st.push_back(n->v[i]);
    }
    return bOk;
}

// [R123-6] 卷标规整：ISO d-char 集合（A-Z/a-z/0-9/_），截断 31 字符并转大写。
//  sLabelW 供 UDF 使用（与 sIsoLabel 逐字节一致）。
static void CiMakeLabels(const Options& opts, std::string& sIsoLabel, std::wstring& sLabelW)
{
    std::wstring sLabel = opts.sVolumeLabel.empty()?L"DUI_ISO":opts.sVolumeLabel;
    // 形参 sIsoLabel 由调用方持有，此处只写入，不重复声明。
    sIsoLabel.clear();
    for (size_t i=0;i<sLabel.size() && sIsoLabel.size()<31;++i)
    {
        wchar_t ch = sLabel[i];
        bool ok = (ch>=L'A'&&ch<=L'Z')||(ch>=L'a'&&ch<=L'z')||
                  (ch>=L'0'&&ch<=L'9')||ch==L'_';
        sIsoLabel.push_back(ok?char(ch):'_');
    }
    if (sIsoLabel.empty()) sIsoLabel = "DUI_ISO";
    for (auto& c : sIsoLabel) c = static_cast<char>(::toupper(static_cast<unsigned char>(c)));
    sLabelW.assign(sIsoLabel.begin(),sIsoLabel.end());
}

// [R123-7] 打开输出文件并按 nTotal 预置大小（尾部 AVDP 需要 N-256 处可写）。
//  句柄所有权返回给调用方，由外层的 IsoFileGuard 负责关闭。
static HANDLE CiOpenOutput(const std::wstring& sIsoPath, ULONGLONG nTotal, std::wstring& sErr)
{
    IsoFileGuard hOut = IsoFileGuard(::CreateFileW(sIsoPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (hOut == INVALID_HANDLE_VALUE)
    {
        sErr = L"\u65e0\u6cd5\u5199\u5165 ISO \u6587\u4ef6\uff1a" + sIsoPath;
        return INVALID_HANDLE_VALUE;
    }
    LARGE_INTEGER liEnd; liEnd.QuadPart = LONGLONG(nTotal * 2048);
    ::SetFilePointerEx(hOut, liEnd, nullptr, FILE_BEGIN);
    ::SetEndOfFile(hOut);
    LARGE_INTEGER li0; li0.QuadPart = 0;
    ::SetFilePointerEx(hOut, li0, nullptr, FILE_BEGIN);
    return hOut.h;
}

bool CreateImage(const std::wstring& sSourceRootIn,
                 const std::wstring& sIsoPath,
                 const Options& optsIn,
                 std::wstring* pErr,
                 const ProgressFn& fnProgress,
                 const CancelQueryFn& fnCancel)
{
    std::wstring sSourceRoot = sSourceRootIn;
    Options opts = optsIn;

    while (sSourceRoot.size()>1 &&
           (sSourceRoot[sSourceRoot.size()-1]==L'\\' ||
            sSourceRoot[sSourceRoot.size()-1]==L'/'))
        sSourceRoot.erase(sSourceRoot.size()-1);

    Node* root = std::make_unique<Node>().release();   // [C++17 R3] tree root; freed by FreeTree at function end
    root->bDir = true;
    root->ft = NowFileTime();

    bool bOk = false;
    std::wstring sErr;

    do
    {
        if (!ScanTreeRec(sSourceRoot, root, &sErr)) break;
        IsoDedup(root);
        int idx = 1;
        AssignPtIndex(root, idx);
        CalcDirSizes(root);
        CalcFidSizes(root);

        // 卷标（ISO 字段 31 d-char；UDF 32）
        std::string sIsoLabel;
        std::wstring sLabelW;
        CiMakeLabels(opts, sIsoLabel, sLabelW);


        // 启动映像
        Buf biosImg, uefiImg;
        if (!opts.sBiosBootFile.empty() && !ReadWholeFile(opts.sBiosBootFile,biosImg,&sErr)) break;
        if (!opts.sUefiBootFile.empty() && !ReadWholeFile(opts.sUefiBootFile,uefiImg,&sErr)) break;

        // ---------- 布局：顺序内容区自 268 起 ----------
        CiLayout lay;
        CiPlanLayout(root, opts, biosImg, uefiImg, lay);
        const ULONGLONG nTotal = lay.nTotal;


        // ---------- 打开输出 ----------
        IsoFileGuard hOut = IsoFileGuard(CiOpenOutput(sIsoPath, nTotal, sErr));
        if (hOut == INVALID_HANDLE_VALUE) break;


        bool bWriteOk = true;

        // ---- ISO 描述符区（16~19）----
        if (!CiWriteIsoDescriptors(hOut, root, lay, opts, sIsoLabel, sLabelW, nTotal))
            bWriteOk = false;


        // ---- UDF 区 ----
        // ---- UDF 描述符区（32~34、256~267）----
        if (opts.bUdf && !CiWriteUdfDescriptors(hOut, root, sLabelW, nTotal))
            bWriteOk = false;


        if (!bWriteOk) { sErr=L"ISO 描述符写入失败"; break; }  // IsoFileGuard 自动关闭

        // ---- 顺序内容区 ----
        {
            LARGE_INTEGER li; li.QuadPart=268*2048;
            ::SetFilePointerEx(hOut,li,nullptr,FILE_BEGIN);
        }

        if (!CiWriteDirData(hOut, root, lay, opts, biosImg, uefiImg))
            bWriteOk = false;

        // 文件数据
        if (!CiWriteFileData(hOut, root, lay, sSourceRoot, fnProgress, sErr, fnCancel))
            bWriteOk = false;
        if (!bWriteOk) break;


        // 尾部 AVDP @N-256
        if (bWriteOk &&
            !WriteBufAt(hOut,(nTotal-256)*2048,MakeAvdp(257,262,DWORD(nTotal-256))))
            bWriteOk=false;

        if (!bWriteOk) break;

        bOk = true;
    } while (false);

    FreeTree(root);

    if (!bOk)
    {
        if (pErr) pErr->assign(sErr.empty()?L"ISO 生成失败":sErr);
        // [R120] 删半截 ISO。删不掉时留痕：残留的 ISO 用户双击打不开，
        //   不说明的话会被误当成"生成成功但文件损坏"。
        //   不改变返回值 —— 这里本来就返回失败。
        if (!::DeleteFileW(sIsoPath.c_str()))
            ::OutputDebugStringW(L"[IsoWriter] 半截 ISO 删除失败，请手动清理目标目录\r\n");
    }
    return bOk;
}

} // namespace IsoWriter
