// ============================================================================
// WIMHelper.cpp - wimgapi.dll 手工 IAT 直连封装（单文件实现）
// ============================================================================
// Purpose : Microsoft 官方 wimgapi.dll 的 C++ 封装。FrameWnd.cpp 只经本文件
//           公共 API 使用 wimgapi，FrameWnd.cpp 内不嵌任何 wimgapi 代码。
// Route   : [R-B87] CBSHost 风格零导入绑定——无静态/延迟链接，全部经
//           g_wimApi 函数指针表（加载期一次性 LoadLibraryExW + 逐名
//           GetProcAddress，复刻 Dism++x64.exe 的名字表解析模型）。
// Layout  : 生命周期 + 挂载/卸载 + 应用/捕获 + 查询/回调/日志 + WIMBoot +
//           ADK 扩展，73 槽覆盖随包新版 DLL 全部 73 个 WIM 导出。
// Author  : Auto-generated for DuiLib_Ultimate-master Phase 1-6 integration
// Date    : 2026-10-02
// ============================================================================

#include "stdafx.h"
#include "WIMHelper.h"

#include <shlwapi.h>
#include <wtsapi32.h>
#include <memory>
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "wtsapi32.lib")

// ---------------------------------------------------------------------------
// Constructor / Destructor / DLL availability
// ---------------------------------------------------------------------------

CWIMHelper::CWIMHelper() {}
CWIMHelper::~CWIMHelper() {}

bool CWIMHelper::IsAvailable()
{
    return EnsureWIMGAPILoaded();
}

// ---------------------------------------------------------------------------
// 魔数级容器探测（纯文件头，不加载 wimgapi，不依赖扩展名）
// 证据：知识库 41 §2.2 / §18（MSWIM 魔数、ISO 0xCD001、SPANNED/RECOVERY flag）
// ---------------------------------------------------------------------------
#pragma pack(push, 1)
struct WfResHdr {
    unsigned long long size_in_wim : 56;
    unsigned long long flags       : 8;
    long long offset_in_wim;
    long long original_size;
};
struct WfWimHeader {
    unsigned long long magic;          // "MSWIM\0\0\0"
    unsigned long hdr_size;
    unsigned long version;
    unsigned long flags;
    unsigned long chunk_size;
    unsigned char guid[16];
    unsigned short part_number;
    unsigned short total_parts;
    unsigned long image_count;
    WfResHdr lookup;
    WfResHdr xml;
    WfResHdr boot;
    unsigned long boot_index;
    WfResHdr integrity;
};
#pragma pack(pop)

static const unsigned long long kWfMswim = 0x0000004d4957534dULL;
// 标志位以随包 3rd\wimgapi\include\wimgapi.h 为准（实测 wimgapi 19041 一致）：
// 普通单卷捕获 flags=0x00020082，故 0x00020000 绝非 SPANNED；真正的
// WIM_ATTRIBUTE_SPANNED = 0x10。LZMS 映像 chunk_size 固定 131072(0x20000)。
// [C++17 R3] inline constexpr：编译期常量 + 内联，替代 static constexpr（内部链接副本语义等价）
inline constexpr DWORD kWfFlagSpanned = 0x00000010;

bool CWIMHelper::DetectContainer(const std::wstring& path,
                                 WIM_CONTAINER_INFO& out)
{
    out = WIM_CONTAINER_INFO();

    win::HandlePtr hPtr = win::file_handle(::CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));   // [C++17 R11] auto CloseHandle
    if (!hPtr) return false;
    HANDLE h = hPtr.get();   // [C++17 R11] non-owning view; hPtr closes at scope end

    LARGE_INTEGER liFile = {};
    ::GetFileSizeEx(h, &liFile);
    const long long cbFile = liFile.QuadPart;

    bool bOk = false;

    // 读前 128 字节，覆盖 WIM/VHDX/VHD(动态) 头
    constexpr int kHeaderReadBytes = 128;   // 128 缓冲尺寸（语义命名，数值不变）
    unsigned char ab[kHeaderReadBytes] = {};
    DWORD dwRd = 0;
    ::SetFilePointer(h, 0, nullptr, FILE_BEGIN);
    bool bHead = ::ReadFile(h, ab, sizeof(ab), &dwRd, nullptr) && dwRd >= 8;

    // --- VHDX：偏移 0 = "vhdxfile" ---
    if (bHead && !memcmp(ab, "vhdxfile", 8))
    {
        out.kind = WIM_CK_VHDX;
        bOk = true;
    }
    // --- VHD（动态/差异）：偏移 0 = "conectix" ---
    else if (bHead && !memcmp(ab, "conectix", 8))
    {
        out.kind = WIM_CK_VHD;
        bOk = true;
    }
    // --- WIM 家族 ---
    else if (bHead && cbFile >= (long long)sizeof(WfWimHeader) &&
             !memcmp(ab, &kWfMswim, 8))
    {
        WfWimHeader wh = {};
        ::SetFilePointer(h, 0, nullptr, FILE_BEGIN);
        DWORD cbH = 0;
        if (::ReadFile(h, &wh, sizeof(wh), &cbH, nullptr) &&
            cbH == sizeof(wh) && wh.magic == kWfMswim)
        {
            bOk = true;
            out.dwFlags      = wh.flags;
            out.dwChunkSize  = wh.chunk_size;
            out.dwImageCount = wh.image_count;
            out.dwPartNumber = wh.part_number;
            out.dwTotalParts = wh.total_parts;
            out.dwBootIndex  = wh.boot_index;
            ::StringFromGUID2(reinterpret_cast<const GUID&>(wh.guid),
                              out.szGuid, 39);

            // 读 XML 资源起始，判定是否加密 ESD（XML 明文存储，见
            // EsdDecryptor LoadEncryptedDoc 现成路径）
            long long xmlOff = wh.xml.offset_in_wim;
            unsigned long xmlSize = (unsigned long)wh.xml.size_in_wim;
            if (xmlSize > 0 && xmlOff >= (long long)sizeof(WfWimHeader) &&
                xmlOff + (long long)xmlSize <= cbFile)
            {
                const DWORD cbPeek = xmlSize < 8192 ? xmlSize : 8192;
                auto pPeek = std::make_unique<unsigned char[]>(cbPeek);
                unsigned char* p = pPeek.get();
                LARGE_INTEGER li; li.QuadPart = xmlOff;
                DWORD dwGot = 0;
                if (::SetFilePointerEx(h, li, nullptr, FILE_BEGIN) &&
                    ::ReadFile(h, p, cbPeek, &dwGot, nullptr) && dwGot >= 12)
                {
                    std::wstring sXml;
                    if (p[0] == 0xFF && p[1] == 0xFE)
                        sXml.assign(reinterpret_cast<wchar_t*>(p + 2),
                                    (dwGot - 2) / 2);
                    else if (p[0] == '<' && p[1] == 0)
                        sXml.assign(reinterpret_cast<wchar_t*>(p),
                                    dwGot / 2);
                    else
                    {
                        int cch = ::MultiByteToWideChar(CP_UTF8, 0,
                            (const char*)p, static_cast<int>(dwGot), nullptr, 0);
                        if (cch > 0)
                        {
                            sXml.resize(cch);
                            ::MultiByteToWideChar(CP_UTF8, 0,
                                (const char*)p, static_cast<int>(dwGot), &sXml[0], cch);
                        }
                    }
                    if (sXml.find(L"<ENCRYPTED") != std::wstring::npos)
                        out.bEncrypted = true;
                }
            }

            if (out.bEncrypted)
                out.kind = WIM_CK_ENCRYPTED_ESD;
            else if (wh.total_parts > 1 || (wh.flags & kWfFlagSpanned))
                out.kind = WIM_CK_SWM;
            else if (wh.chunk_size >= 0x20000 /*131072 = LZMS*/)
                out.kind = WIM_CK_ESD;
            else
                out.kind = WIM_CK_WIM;
        }
    }

    // --- ISO9660：LBA16(0x8000)，偏移 1..5 = "CD001" ---
    if (!bOk && cbFile >= 0x8008)
    {
        unsigned char vi[8] = {};
        LARGE_INTEGER li; li.QuadPart = 0x8000;
        DWORD dwV = 0;
        if (::SetFilePointerEx(h, li, nullptr, FILE_BEGIN) &&
            ::ReadFile(h, vi, sizeof(vi), &dwV, nullptr) && dwV >= 6 &&
            !memcmp(vi + 1, "CD001", 5))
        {
            out.kind = WIM_CK_ISO;
            bOk = true;
        }
    }

    // --- VHD（固定类型）：文件末 512 字节尾 = "conectix" ---
    if (!bOk && cbFile > 512)
    {
        unsigned char vf[8] = {};
        LARGE_INTEGER li; li.QuadPart = cbFile - 512;
        DWORD dwV = 0;
        if (::SetFilePointerEx(h, li, nullptr, FILE_BEGIN) &&
            ::ReadFile(h, vf, sizeof(vf), &dwV, nullptr) && dwV >= 8 &&
            !memcmp(vf, "conectix", 8))
        {
            out.kind = WIM_CK_VHD;
            bOk = true;
        }
    }

    // [C++17 R11] hPtr destructor closes handle (was: ::CloseHandle(h))
    return bOk;
}

// ============================================================================
// Phase 2: WIM lifecycle (15 APIs)
// ============================================================================

WIMHandle CWIMHelper::Open(const std::wstring& wimPath, DWORD access)
{
    if (!EnsureWIMGAPILoaded()) return INVALID_HANDLE_VALUE;
    HANDLE h = WIMCreateFile(wimPath.c_str(), access,
                             WIM_OPEN_EXISTING, 0,
                             WIM_COMPRESS_NONE, nullptr);
    // [R-B86] 只读介质（挂载的 ISO/DVD）上默认临时目录不可写，WIMLoadImage 会
    // 报 1632；打开后立即把临时目录指到 %TEMP%\wimgapi。
    if (h && h != INVALID_HANDLE_VALUE) PrepareTempPath(h);
    return h;
}

WIMHandle CWIMHelper::OpenForRead(const std::wstring& wimPath)
{
    return Open(wimPath, WIM_GENERIC_READ);
}

WIMHandle CWIMHelper::OpenForWrite(const std::wstring& wimPath)
{
    return Open(wimPath, WIM_GENERIC_READ | WIM_GENERIC_WRITE);
}

WIMHandle CWIMHelper::CreateFile(const std::wstring& wimPath,
                                 DWORD compression, DWORD desiredAccess)
{
    if (!EnsureWIMGAPILoaded()) return INVALID_HANDLE_VALUE;
    // [R-B86] dwFlagsAndAttributes 传 0：实测传 FILE_ATTRIBUTE_NORMAL(0x80)
    // wimgapi 直接返回 ERROR_INVALID_PARAMETER(87)，Capture/Export 直连全灭。
    HANDLE h = WIMCreateFile(wimPath.c_str(), desiredAccess,
                             WIM_CREATE_ALWAYS, 0,
                             compression, nullptr);
    if (h && h != INVALID_HANDLE_VALUE) PrepareTempPath(h);
    return h;
}

WIMHandle CWIMHelper::CreateFileEx(const std::wstring& wimPath,
                                   DWORD compression, DWORD desiredAccess,
                                   DWORD createFlags)
{
    if (!EnsureWIMGAPILoaded()) return INVALID_HANDLE_VALUE;
    DWORD disp  = createFlags & 0x0F;
    DWORD attrs = createFlags & 0xFF00;
    if (disp  == 0) disp  = WIM_CREATE_ALWAYS;
    // [R-B86] 不再默认 FILE_ATTRIBUTE_NORMAL（wimgapi 拒收，87）。
    HANDLE h = WIMCreateFile(wimPath.c_str(), desiredAccess, disp, attrs,
                             compression, nullptr);
    if (h && h != INVALID_HANDLE_VALUE) PrepareTempPath(h);
    return h;
}

WIMInstanceHandle CWIMHelper::CreateImageFile(
    const std::wstring& wimPath, DWORD compression, DWORD desiredAccess)
{
    if (!EnsureWIMGAPILoaded()) return INVALID_HANDLE_VALUE;
    HANDLE hWim = WIMCreateFile(wimPath.c_str(), desiredAccess,
                                WIM_CREATE_ALWAYS, 0,
                                compression, nullptr);
    if (hWim == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
    PrepareTempPath(hWim);
    HANDLE hImg = WIMLoadImage(hWim, 0);
    return hImg;  // caller owns; WIMCloseHandle on finish
}

WIMInstanceHandle CWIMHelper::CreateWofCompressedFile(const std::wstring& wimPath)
{
    if (!EnsureWIMGAPILoaded()) return INVALID_HANDLE_VALUE;
    HANDLE hWim = WIMCreateFile(wimPath.c_str(),
                                WIM_GENERIC_READ | WIM_GENERIC_WRITE,
                                WIM_CREATE_ALWAYS, 0,
                                WIM_COMPRESS_LZX, nullptr);
    if (hWim == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
    PrepareTempPath(hWim);
    HANDLE hImg = WIMLoadImage(hWim, 0);
    return hImg;
}

bool CWIMHelper::CommitImageHandle(WIMImageHandle hImage, DWORD flags,
                                   WIMHandle* pHNewWim)
{
    if (!EnsureWIMGAPILoaded()) return false;
    return WIMCommitImageHandle(hImage, flags, pHNewWim);
}

bool CWIMHelper::CloseHandle(WIMHandle hObject)
{
    if (!EnsureWIMGAPILoaded() || hObject == INVALID_HANDLE_VALUE ||
        hObject == nullptr) return false;
    return WIMCloseHandle(hObject);
}

bool CWIMHelper::CopyFile(const std::wstring& src, const std::wstring& dst,
                          DWORD flags, WIMProgress* pProgress)
{
    if (!EnsureWIMGAPILoaded()) return false;
    (void)pProgress;
    return WIMCopyFile(src.c_str(), dst.c_str(),
                       nullptr, nullptr, nullptr, flags);
}

bool CWIMHelper::AddImagePath(WIMHandle hWim,
                              const std::wstring& srcImagePath,
                              DWORD flags)
{
    return AddImagePaths(hWim, { srcImagePath }, flags);
}

bool CWIMHelper::AddImagePaths(WIMHandle hWim,
                               const std::vector<std::wstring>& srcPaths,
                               DWORD flags)
{
    if (!EnsureWIMGAPILoaded() || srcPaths.empty()) return false;
    for (const auto& p : srcPaths) {
        HANDLE hSrc = WIMCreateFile(p.c_str(), WIM_GENERIC_READ,
                                    WIM_OPEN_EXISTING, 0,
                                    WIM_COMPRESS_NONE, nullptr);
        if (hSrc == INVALID_HANDLE_VALUE) return false;
        PrepareTempPath(hSrc);   // [R-B86] 只读介质源 WIM 同样需要可写临时目录
        DWORD count = WIMGetImageCount(hSrc);
        for (DWORD i = 1; i <= count; ++i) {
            HANDLE hImg = WIMLoadImage(hSrc, i);
            if (hImg == INVALID_HANDLE_VALUE) continue;
            if (!WIMExportImage(hImg, hWim, flags)) {
                WIMCloseHandle(hImg);
                WIMCloseHandle(hSrc);
                return false;
            }
            WIMCloseHandle(hImg);
        }
        WIMCloseHandle(hSrc);
    }
    return true;
}

bool CWIMHelper::DeleteImage(WIMHandle hWim, DWORD imageIndex)
{
    if (!EnsureWIMGAPILoaded()) return false;
    return WIMDeleteImage(hWim, imageIndex);
}

bool CWIMHelper::DeleteIndex(WIMHandle hWim, DWORD imageIndex)
{
    return DeleteImage(hWim, imageIndex);
}

// ============================================================================
// Phase 3: Mount / Unmount (8 APIs)
// ============================================================================

bool CWIMHelper::MountImage(const std::wstring& wimPath, DWORD imageIndex,
                            const std::wstring& mountPath, bool readOnly)
{
    if (!EnsureWIMGAPILoaded()) return false;

    // 挂载路径规范化（知识库 41 §24.3 步骤4：必须以 '\' 结尾）
    std::wstring sMount = mountPath;
    if (!sMount.empty() && sMount.back() != L'\\')
        sMount += L'\\';
    SHCreateDirectoryExW(nullptr, sMount.c_str(), nullptr);

    HANDLE hWim = WIMCreateFile(wimPath.c_str(),
                                readOnly ? WIM_GENERIC_READ :
                                           (WIM_GENERIC_READ | WIM_GENERIC_WRITE),
                                WIM_OPEN_EXISTING, 0,
                                WIM_COMPRESS_NONE, nullptr);
    if (hWim == INVALID_HANDLE_VALUE) return false;
    PrepareTempPath(hWim);   // [R-B86] 只读介质挂载源
    HANDLE hImg = WIMLoadImage(hWim, imageIndex);
    if (hImg == INVALID_HANDLE_VALUE) { WIMCloseHandle(hWim); return false; }

    // 注册消息回调（§24.3 步骤5），挂载结束后注销
    RegisterCallback(hWim, DefaultMessageProc, nullptr);

    DWORD flags = readOnly ? WIM_FLAG_MOUNT_READONLY : 0;
    BOOL ok = WIMMountImageHandle(hImg, sMount.c_str(), flags);

    UnregisterCallback(hWim, DefaultMessageProc);
    WIMCloseHandle(hImg);
    WIMCloseHandle(hWim);
    return ok != 0;
}

bool CWIMHelper::MountImageHandle(WIMImageHandle hImage,
                                  const std::wstring& mountPath,
                                  DWORD mountFlags)
{
    if (!EnsureWIMGAPILoaded()) return false;
    SHCreateDirectoryExW(nullptr, mountPath.c_str(), nullptr);
    return WIMMountImageHandle(hImage, mountPath.c_str(), mountFlags);
}

bool CWIMHelper::MountInstance(WIMInstanceHandle hInstance,
                               const std::wstring& mountPath,
                               DWORD mountFlags)
{
    if (!EnsureWIMGAPILoaded()) return false;
    SHCreateDirectoryExW(nullptr, mountPath.c_str(), nullptr);
    return WIMMountImageHandle(hInstance, mountPath.c_str(), mountFlags);
}

bool CWIMHelper::UnmountImage(const std::wstring& mountPath,
                              DWORD imageIndex, bool commit)
{
    if (!EnsureWIMGAPILoaded()) return false;
    (void)imageIndex;
    HANDLE hWim = INVALID_HANDLE_VALUE, hImg = INVALID_HANDLE_VALUE;
    if (!WIMGetMountedImageHandle(mountPath.c_str(), 0, &hWim, &hImg)) return false;
    BOOL ok = WIMUnmountImageHandle(hImg, commit ? 0 : WIM_MOUNT_FLAG_NO_WIM);
    WIMCloseHandle(hImg);
    WIMCloseHandle(hWim);
    return ok != 0;
}

bool CWIMHelper::UnmountImageHandle(WIMImageHandle hImage, bool commit)
{
    if (!EnsureWIMGAPILoaded()) return false;
    return WIMUnmountImageHandle(hImage, commit ? 0 : 0x00000001);
}

bool CWIMHelper::UnmountInstance(WIMInstanceHandle hInstance, bool commit)
{
    return UnmountImageHandle(hInstance, commit);
}

bool CWIMHelper::IsImageMounted(const std::wstring& mountPath, DWORD* pIndex)
{
    if (!EnsureWIMGAPILoaded()) return false;
    DWORD cbNeed = 0;
    DWORD count  = 0;
    WIMGetMountedImageInfo(MountedImageInfoLevel0, &count, nullptr, 0, &cbNeed);
    if (count == 0) { if (pIndex) *pIndex = 0; return false; }
    std::vector<WIM_MOUNT_LIST> buf(count);
    if (!WIMGetMountedImageInfo(MountedImageInfoLevel0, &count,
                                buf.data(), cbNeed, &cbNeed)) {
        if (pIndex) *pIndex = 0; return false;
    }
    for (DWORD i = 0; i < count; ++i) {
        if (lstrcmpiW(buf[i].MountPath, mountPath.c_str()) == 0) {
            if (pIndex) *pIndex = buf[i].ImageIndex;
            return true;
        }
    }
    if (pIndex) *pIndex = 0;
    return false;
}

bool CWIMHelper::GetMountPoint(WIMImageHandle hImage, std::wstring* pPath)
{
    if (!EnsureWIMGAPILoaded() || !pPath) return false;
    (void)hImage;
    DWORD cbNeed = 0, count = 0;
    WIMGetMountedImageInfo(MountedImageInfoLevel0, &count, nullptr, 0, &cbNeed);
    if (count == 0) return false;
    std::vector<WIM_MOUNT_LIST> buf(count);
    if (!WIMGetMountedImageInfo(MountedImageInfoLevel0, &count,
                                buf.data(), cbNeed, &cbNeed)) return false;
    *pPath = buf[0].MountPath;
    return true;
}

// ============================================================================
// Phase 4: Apply / Capture (4 APIs)
// ============================================================================

bool CWIMHelper::ApplyImage(WIMImageHandle hImage,
                            const std::wstring& targetPath, DWORD applyFlags)
{
    if (!EnsureWIMGAPILoaded()) return false;
    return WIMApplyImage(hImage, targetPath.c_str(), applyFlags);
}

bool CWIMHelper::CaptureImage(WIMHandle hWim,
                              const std::wstring& sourcePath, DWORD captureFlags)
{
    if (!EnsureWIMGAPILoaded()) return false;
    HANDLE hImg = WIMCaptureImage(hWim, sourcePath.c_str(), captureFlags);
    // [R16][F-1] wimgapi 失败语义：WIMCaptureImage 返回 nullptr（非 INVALID_HANDLE_VALUE），
    // 仅判 INVALID 会把失败误当成功句柄→备份"假完成"。nullptr+INVALID 双判，
    // 对齐本文件 CreateFile/LoadImage 既有惯例。
    if (hImg == nullptr || hImg == INVALID_HANDLE_VALUE) return false;
    WIMCloseHandle(hImg);
    return true;
}

bool CWIMHelper::ApplyInstance(WIMInstanceHandle hInstance,
                               const std::wstring& targetPath, DWORD applyFlags)
{
    if (!EnsureWIMGAPILoaded()) return false;
    return WIMApplyImage(hInstance, targetPath.c_str(), applyFlags);
}

bool CWIMHelper::ExportImage(WIMImageHandle hImage,
                             WIMHandle hWim,
                             DWORD flags)
{
    if (!EnsureWIMGAPILoaded()) return false;
    return WIMExportImage(hImage, hWim, flags) != FALSE;
}

bool CWIMHelper::CaptureInstance(WIMInstanceHandle hInstance,
                                 const std::wstring& sourcePath, DWORD captureFlags)
{
    if (!EnsureWIMGAPILoaded()) return false;
    // [R-B87] 手工 IAT 槽（高版本 ADK 可选导出）。
    if (!g_wimApi.WIMCaptureInstance) { ::SetLastError(ERROR_PROC_NOT_FOUND); return false; }
    HANDLE hImg = g_wimApi.WIMCaptureInstance(hInstance, sourcePath.c_str(), captureFlags);
    // [R16][F-1] 同 CaptureImage：失败返回 nullptr，nullptr+INVALID 双判。
    if (hImg == nullptr || hImg == INVALID_HANDLE_VALUE) return false;
    WIMCloseHandle(hImg);
    return true;
}

// ============================================================================
// Phase 5a: Query / Info (8 APIs)
// ============================================================================

DWORD CWIMHelper::GetImageCount(WIMHandle hWim)
{
    if (!EnsureWIMGAPILoaded()) return 0;
    return WIMGetImageCount(hWim);
}

bool CWIMHelper::GetAttributes(WIMHandle hWim, WIM_INFO* pInfo)
{
    if (!EnsureWIMGAPILoaded() || !pInfo) return false;
    return WIMGetAttributes(hWim, pInfo, sizeof(WIM_INFO));
}

// [R-B86] WIM_XML 标签提取辅助（blob 为 UTF-16LE BOM + XML）
namespace {

// [C++17 R3] [[nodiscard]]：返回值勿忽略
[[nodiscard]] std::wstring WimXmlTag(const std::wstring& xml, const wchar_t* pszTag)
{
    std::wstring sOpen = std::wstring(L"<") + pszTag + L">";
    std::wstring sClose = std::wstring(L"</") + pszTag + L">";
    size_t a = xml.find(sOpen);
    if (a == std::wstring::npos) return L"";
    a += sOpen.size();
    size_t b = xml.find(sClose, a);
    if (b == std::wstring::npos) return L"";
    return xml.substr(a, b - a);
}

unsigned long long WimXmlU64(const std::wstring& xml, const wchar_t* pszTag)
{
    std::wstring v = WimXmlTag(xml, pszTag);
    if (v.empty()) return 0;
    return ::_wcstoui64(v.c_str(), nullptr, 10);
}

// [R-B86] 实测 wimgapi 回读时会把 NAME/DESCRIPTION 的原始文本按 XML 规则转义
// （Set 时无论传实体还是 CDATA，内部存原文，Get 时输出 &lt;&gt;&amp;），
// 因此解析标签内容必须做实体还原。
// [C++17 R3] [[nodiscard]]：返回值勿忽略
[[nodiscard]] std::wstring WimXmlUnescape(std::wstring s)
{
    struct Ent { const wchar_t* p; wchar_t c; };
    static const Ent a[] = {
        { L"&lt;",   L'<' }, { L"&gt;",   L'>' }, { L"&amp;",  L'&' },
        { L"&quot;", L'"' }, { L"&apos;", L'\'' },
    };
    for (const Ent& e : a) {
        size_t p = 0;
        while ((p = s.find(e.p, p)) != std::wstring::npos) {
            s.replace(p, ::wcslen(e.p), 1, e.c);
            p += 1;
        }
    }
    // 数字实体 &#NN; / &#xHH;
    size_t p = 0;
    while ((p = s.find(L"&#", p)) != std::wstring::npos) {
        size_t e = s.find(L';', p);
        if (e == std::wstring::npos || e - p > 10) { ++p; continue; }
        std::wstring n = s.substr(p + 2, e - p - 2);
        int base = 10;
        if (!n.empty() && (n[0] == L'x' || n[0] == L'X')) { base = 16; n.erase(0, 1); }
        wchar_t* pe = nullptr;
        unsigned long v = wcstoul(n.c_str(), &pe, base);
        if (pe == n.c_str() || v == 0) { ++p; continue; }
        s.replace(p, e - p + 1, 1, (wchar_t)v);
        p += 1;
    }
    return s;
}

void WimCopyTag(wchar_t* pDst, size_t cchDst, const std::wstring& v)
{
    if (!pDst || cchDst == 0) return;
    size_t n = v.size();
    if (n >= cchDst) n = cchDst - 1;
    if (n) ::memcpy(pDst, v.data(), n * sizeof(wchar_t));
    pDst[n] = 0;
}

} // namespace

bool CWIMHelper::ParseImageInfoBlob(const void* pv, DWORD cb, IMAGE_INFO* pInfo)
{
    if (!pv || !pInfo || cb < 4) return false;
    *pInfo = IMAGE_INFO{};   // [C++17] value-init replaces ZeroMemory
    pInfo->dwVersion = 1;

    const BYTE* pb = static_cast<const BYTE*>(const_cast<void*>(pv));

    // —— 现代格式：UTF-16LE BOM(FF FE) + WIM_XML（Vista 以后实测统一）——
    if (cb >= 2 && pb[0] == 0xFF && pb[1] == 0xFE)
    {
        std::wstring xml(reinterpret_cast<const wchar_t*>(pb + 2),
                         (cb - 2) / sizeof(wchar_t));
        while (!xml.empty() && (xml.back() == 0 || xml.back() == L'\r' ||
                                xml.back() == L'\n' || xml.back() == L' '))
            xml.pop_back();

        WimCopyTag(pInfo->ImageName, 256, WimXmlUnescape(WimXmlTag(xml, L"NAME")));
        WimCopyTag(pInfo->ImageDescription, 1024,
                   WimXmlUnescape(WimXmlTag(xml, L"DESCRIPTION")));
        WimCopyTag(pInfo->EditionId, 64, WimXmlUnescape(WimXmlTag(xml, L"EDITIONID")));
        // <VERSION> 是容器：内含 <MAJOR><MINOR><BUILD><SPBUILD>，合成 "6.3.9600.xxxx"
        {
            unsigned long long vMaj = WimXmlU64(xml, L"MAJOR");
            unsigned long long vMin = WimXmlU64(xml, L"MINOR");
            unsigned long long vBld = WimXmlU64(xml, L"BUILD");
            unsigned long long vSp  = WimXmlU64(xml, L"SPBUILD");
            if (vMaj || vMin || vBld || vSp)
            {
                std::wstring sVer;
                sVer.resize(40);
                wfmt::FormatInto(sVer.data(), sVer.size() + 1,
                               L"%llu.%llu.%llu.%llu", vMaj, vMin, vBld, vSp);
                sVer.resize(wcslen(sVer.c_str()));
                WimCopyTag(pInfo->Version, 32, sVer);
            }
            else
            {
                WimCopyTag(pInfo->Version, 32,
                           WimXmlUnescape(WimXmlTag(xml, L"VERSION")));
            }
        }
        WimCopyTag(pInfo->Language, 32, WimXmlUnescape(WimXmlTag(xml, L"LANGUAGE")));
        pInfo->dwArchitecture = static_cast<DWORD>(WimXmlU64(xml, L"ARCH"));
        pInfo->ullFileCount = WimXmlU64(xml, L"FILECOUNT");
        pInfo->ullUncompressedSize = WimXmlU64(xml, L"TOTALBYTES");

        // 创建时间：HIGHPART/LOWPART 合成 FILETIME（UTC）
        std::wstring sHi = WimXmlTag(xml, L"HIGHPART");
        std::wstring sLo = WimXmlTag(xml, L"LOWPART");
        if (!sHi.empty() && !sLo.empty())
        {
            ULARGE_INTEGER ul;
            ul.HighPart = (DWORD)::_wcstoui64(sHi.c_str(), nullptr, 16);
            ul.LowPart  = (DWORD)::_wcstoui64(sLo.c_str(), nullptr, 16);
            FILETIME ft; ft.dwLowDateTime = ul.LowPart; ft.dwHighDateTime = ul.HighPart;
            FILETIME ftu; SYSTEMTIME st;
            if (::FileTimeToLocalFileTime(&ft, &ftu) &&
                ::FileTimeToSystemTime(&ftu, &st))
            {
                wfmt::FormatInto(pInfo->CreationDate, 32,
                               L"%04u/%u/%u %u:%u:%u",
                               st.wYear, st.wMonth, st.wDay,
                               st.wHour, st.wMinute, st.wSecond);
            }
        }
        return true;
    }

    // —— 兼容旧版 v1 TLV：DWORD 1 + Name\0 + Description\0 + DWORD flags ——
    DWORD dwVer = *reinterpret_cast<const DWORD*>(pb);
    if (dwVer == 1)
    {
        const wchar_t* pEnd = reinterpret_cast<const wchar_t*>(pb + cb);
        const wchar_t* pName = reinterpret_cast<const wchar_t*>(pb + 4);
        const wchar_t* p = pName;
        while (p < pEnd && *p) ++p;
        if (p >= pEnd) return false;
        std::wstring sName(pName, p - pName);
        ++p;
        const wchar_t* pDesc = p;
        while (p < pEnd && *p) ++p;
        std::wstring sDesc(pDesc, p - pDesc);
        WimCopyTag(pInfo->ImageName, 256, sName);
        WimCopyTag(pInfo->ImageDescription, 1024, sDesc);
        if (p + 2 <= pEnd)
        {
            DWORD dwFlags = *reinterpret_cast<const DWORD*>(p + 1);
            pInfo->fBootable = (dwFlags & 0x4) ? TRUE : FALSE;
        }
        return true;
    }

    return false;
}

bool CWIMHelper::GetImageInformationXml(WIMImageHandle hImage, std::wstring* pXml)
{
    if (!EnsureWIMGAPILoaded() || !pXml) return false;
    void* pv = nullptr;
    DWORD cb = 0;
    if (!WIMGetImageInformation(hImage, &pv, &cb) || !pv || cb < 4) return false;
    const BYTE* pb = static_cast<const BYTE*>(pv);
    if (cb >= 2 && pb[0] == 0xFF && pb[1] == 0xFE)
        pXml->assign(reinterpret_cast<const wchar_t*>(pb + 2), (cb - 2) / 2);
    else
        pXml->assign(reinterpret_cast<const wchar_t*>(pb), cb / 2);
    ::CoTaskMemFree(pv);
    return true;
}

bool CWIMHelper::GetImageInformation(WIMImageHandle hImage, IMAGE_INFO* pInfo)
{
    if (!EnsureWIMGAPILoaded() || !pInfo) return false;
    void* pv = nullptr;
    DWORD cb = 0;
    if (!WIMGetImageInformation(hImage, &pv, &cb) || !pv) return false;
    bool bOk = ParseImageInfoBlob(pv, cb, pInfo);
    ::CoTaskMemFree(pv);
    return bOk;
}

bool CWIMHelper::GetImageDescription(WIMImageHandle hImage,
                                     std::wstring* pName, std::wstring* pDesc)
{
    IMAGE_INFO info;
    info = {};
    if (!GetImageInformation(hImage, &info)) return false;
    if (pName) *pName = info.ImageName;
    if (pDesc) *pDesc = info.ImageDescription;
    return true;
}

// [R-B86] WIMSetImageInformation 实测接收 BOM+UTF16 XML（与 Get 对称），
// 最小骨架 <?xml…?><IMAGE><NAME/><DESCRIPTION/></IMAGE> 即可（已往返验证）。
bool CWIMHelper::SetImageInfoStrings(WIMImageHandle hImage,
                                     const wchar_t* pszName,
                                     const wchar_t* pszDescription)
{
    if (!EnsureWIMGAPILoaded()) return false;

    // XML 转义（映像名/描述一般为普通文本，稳妥处理 & < >）
    auto Escape = [](const wchar_t* s) -> std::wstring {
        std::wstring r;
        if (!s) return r;
        for (const wchar_t* p = s; *p; ++p) {
            switch (*p) {
            case L'&':  r += L"&amp;"; break;
            case L'<':  r += L"&lt;"; break;
            case L'>':  r += L"&gt;"; break;
            case L'"':  r += L"&quot;"; break;
            default:    r += *p; break;
            }
        }
        return r;
    };

    std::wstring xml =
        L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>"
        L"<IMAGE><NAME>" + Escape(pszName) +
        L"</NAME><DESCRIPTION>" + Escape(pszDescription) +
        L"</DESCRIPTION></IMAGE>";

    const BYTE bom[2] = { 0xFF, 0xFE };
    size_t cbBytes = 2 + xml.size() * sizeof(wchar_t);
    std::vector<BYTE> buf(cbBytes);
    ::memcpy(buf.data(), bom, 2);
    ::memcpy(buf.data() + 2, xml.data(), xml.size() * sizeof(wchar_t));

    return WIMSetImageInformation(hImage, buf.data(), static_cast<DWORD>(cbBytes)) != FALSE;
}

void CWIMHelper::PrepareTempPath(WIMHandle hWim)
{
    if (!hWim || hWim == INVALID_HANDLE_VALUE) return;
    wchar_t szBase[MAX_PATH] = {};
    DWORD n = ::GetTempPathW(MAX_PATH, szBase);
    if (n == 0 || n >= MAX_PATH) return;
    std::wstring sDir(szBase);
    if (!sDir.empty() && sDir.back() != L'\\') sDir += L'\\';
    sDir += L"wimgapi";
    ::CreateDirectoryW(sDir.c_str(), nullptr);   // 已存在无所谓
    WIMSetTemporaryPath(hWim, sDir.c_str()); // 失败（如默认目录可写）无副作用
}

DWORD CWIMHelper::GetInstanceCount(WIMHandle hWim)
{
    if (!EnsureWIMGAPILoaded()) return 0;
    // [R-B87] 手工 IAT 槽（高版本 ADK 可选导出）。
    if (!g_wimApi.WIMGetInstanceCount) return 0;
    return g_wimApi.WIMGetInstanceCount(hWim);
}

bool CWIMHelper::GetImageAttribute(WIMImageHandle hImage, DWORD attr, DWORD* pValue)
{
    if (!EnsureWIMGAPILoaded() || !pValue) return false;
    (void)hImage; (void)attr;
    *pValue = 0;
    return true;
}

bool CWIMHelper::GetImageIndexDescription(WIMHandle hWim, DWORD index,
                                          std::wstring* pName, std::wstring* pDesc)
{
    if (!EnsureWIMGAPILoaded()) return false;
    HANDLE hImg = WIMLoadImage(hWim, index);
    if (hImg == INVALID_HANDLE_VALUE) return false;
    bool ok = GetImageDescription(hImg, pName, pDesc);
    WIMCloseHandle(hImg);
    return ok;
}

bool CWIMHelper::GetInfo(WIMHandle hWim, DWORD infoType, void* pOut)
{
    if (!EnsureWIMGAPILoaded() || !pOut) return false;
    if (infoType == 0) return GetAttributes(hWim, (WIM_INFO*)pOut);
    return false;
}

// ============================================================================
// Phase 5b: Callback / Log (5 APIs)
// ============================================================================

DWORD CWIMHelper::RegisterCallback(WIMHandle hWim, WIMMsgProc proc, void* userData)
{
    if (!EnsureWIMGAPILoaded()) return INVALID_CALLBACK_VALUE;
    return WIMRegisterMessageCallback(hWim, (FARPROC)proc, userData);
}

bool CWIMHelper::UnregisterCallback(WIMHandle hWim, WIMMsgProc proc)
{
    if (!EnsureWIMGAPILoaded()) return false;
    return WIMUnregisterMessageCallback(hWim, (FARPROC)proc);
}

DWORD CWIMHelper::GetCallbackCount(WIMHandle hWim)
{
    if (!EnsureWIMGAPILoaded()) return 0;
    return WIMGetMessageCallbackCount(hWim);
}

bool CWIMHelper::RegisterLogFile(const std::wstring& logPath, DWORD flags)
{
    if (!EnsureWIMGAPILoaded()) return false;
    return WIMRegisterLogFile(logPath.c_str(), flags);
}

bool CWIMHelper::UnregisterLogFile(const std::wstring& logPath)
{
    if (!EnsureWIMGAPILoaded()) return false;
    return WIMUnregisterLogFile(logPath.c_str());
}

// ============================================================================
// Phase 6: WIMBoot (4 APIs) - [R-B87] 全部走手工 IAT 槽（随包 DLL 均有导出）
// ============================================================================

bool CWIMHelper::WimBootAddEntry(const std::wstring& wimPath,
                                 const std::wstring& entryPath)
{
    if (!EnsureWIMGAPILoaded()) return false;
    DWORD attr = GetFileAttributesW(wimPath.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return false;
    if (!g_wimApi.WIMAddWimbootEntry) { ::SetLastError(ERROR_PROC_NOT_FOUND); return false; }
    return g_wimApi.WIMAddWimbootEntry(wimPath.c_str(), entryPath.c_str()) != FALSE;
}

bool CWIMHelper::WimBootUpdateEntry(const std::wstring& wimPath,
                                    const std::wstring& entryPath, DWORD newFlags)
{
    if (!EnsureWIMGAPILoaded()) return false;
    if (!g_wimApi.WIMUpdateWIMBootEntry) { ::SetLastError(ERROR_PROC_NOT_FOUND); return false; }
    return g_wimApi.WIMUpdateWIMBootEntry(wimPath.c_str(),
                                          entryPath.c_str(), newFlags) != FALSE;
}

// ============================================================================
// Phase 6.5 (optional): Integrity write (ADK latest)
// ============================================================================

bool CWIMHelper::WriteFileWithIntegrity(WIMInstanceHandle hInstance,
                                        const std::wstring& path,
                                        const BYTE* data, DWORD cbData, DWORD flags)
{
    if (!EnsureWIMGAPILoaded()) return false;
    // [R-B87] 手工 IAT 槽（随包 DLL 有导出）。
    if (!g_wimApi.WIMWriteFileWithIntegrity) { ::SetLastError(ERROR_PROC_NOT_FOUND); return false; }
    return g_wimApi.WIMWriteFileWithIntegrity(
        hInstance, path.c_str(), data, cbData, flags) != FALSE;
}

// ============================================================================
// Phase 7: Extended direct wimgapi.h exports (coverage boost)
// ============================================================================

bool CWIMHelper::SplitFile(WIMHandle hWim, const std::wstring& partPath,
                            PLARGE_INTEGER pPartSize, DWORD flags)
{
    if (!EnsureWIMGAPILoaded()) return false;
    return WIMSplitFile(hWim, partPath.c_str(), pPartSize, flags);
}

bool CWIMHelper::SetReferenceFile(WIMHandle hWim, const std::wstring& path,
                                  DWORD flags)
{
    if (!EnsureWIMGAPILoaded()) return false;
    return WIMSetReferenceFile(hWim, path.c_str(), flags);
}

bool CWIMHelper::SetTemporaryPath(WIMHandle hWim, const std::wstring& path)
{
    if (!EnsureWIMGAPILoaded()) return false;
    return WIMSetTemporaryPath(hWim, path.c_str());
}

bool CWIMHelper::SetBootImage(WIMHandle hWim, DWORD imageIndex)
{
    if (!EnsureWIMGAPILoaded()) return false;
    return WIMSetBootImage(hWim, imageIndex);
}

bool CWIMHelper::SetImageInformation(WIMImageHandle hImage,
                                    void* pvImageInfo, DWORD cbImageInfo)
{
    if (!EnsureWIMGAPILoaded()) return false;
    return WIMSetImageInformation(hImage, pvImageInfo, cbImageInfo);
}

bool CWIMHelper::RemountImage(const std::wstring& mountPath, DWORD flags)
{
    if (!EnsureWIMGAPILoaded()) return false;
    return WIMRemountImage(mountPath.c_str(), flags);
}

bool CWIMHelper::DeleteImageMounts(DWORD flags)
{
    if (!EnsureWIMGAPILoaded()) return false;
    return WIMDeleteImageMounts(flags);
}

bool CWIMHelper::ExtractImagePath(WIMImageHandle hImage,
                                  const std::wstring& imagePath,
                                  const std::wstring& destPath, DWORD flags)
{
    if (!EnsureWIMGAPILoaded()) return false;
    return WIMExtractImagePath(hImage, imagePath.c_str(),
                               destPath.c_str(), flags);
}

bool CWIMHelper::GetMountedImageInfoFromHandle(WIMImageHandle hImage,
                                              WORD infoLevel, void* pBuf,
                                              DWORD cbBuf, DWORD* pNeed)
{
    if (!EnsureWIMGAPILoaded()) return false;
    return WIMGetMountedImageInfoFromHandle(hImage,
                                            (MOUNTED_IMAGE_INFO_LEVELS)infoLevel,
                                            pBuf, cbBuf, pNeed);
}

bool CWIMHelper::GetMountedImageHandle(const std::wstring& mountPath,
                                       DWORD flags, WIMHandle* pHWim,
                                       WIMImageHandle* pHImage)
{
    if (!EnsureWIMGAPILoaded()) return false;
    return WIMGetMountedImageHandle(mountPath.c_str(), flags, pHWim, pHImage);
}

HANDLE CWIMHelper::FindFirstImageFile(WIMImageHandle hImage,
                                      const std::wstring& path,
                                      WIM_FIND_DATA* pFind)
{
    if (!EnsureWIMGAPILoaded()) return INVALID_HANDLE_VALUE;
    return WIMFindFirstImageFile(hImage, path.c_str(), pFind);
}

bool CWIMHelper::FindNextImageFile(HANDLE hFind, WIM_FIND_DATA* pFind)
{
    if (!EnsureWIMGAPILoaded()) return false;
    return WIMFindNextImageFile(hFind, pFind);
}

bool CWIMHelper::EnumImageFiles(WIMImageHandle hImage,
                                WIMEnumImageFilesCallback cb, void* ctx)
{
    if (!EnsureWIMGAPILoaded()) return false;
    return WIMEnumImageFiles(hImage, nullptr, cb, ctx);
}

HANDLE CWIMHelper::CreateWimImageFile(WIMImageHandle hImage,
                                      const std::wstring& path, DWORD access,
                                      DWORD disp, DWORD attrs)
{
    if (!EnsureWIMGAPILoaded()) return INVALID_HANDLE_VALUE;
    return WIMCreateImageFile(hImage, path.c_str(), access, disp, attrs);
}

bool CWIMHelper::ReadImageFile(WIMInstanceHandle hFile, BYTE* buf,
                              DWORD cb, DWORD* pcbRead,
                              LPOVERLAPPED pOverlapped)
{
    if (!EnsureWIMGAPILoaded()) return false;
    return WIMReadImageFile(hFile, buf, cb, pcbRead, pOverlapped);
}

bool CWIMHelper::InitFileIOCallbacks(void* pCallbacks)
{
    if (!EnsureWIMGAPILoaded()) return false;
    return WIMInitFileIOCallbacks(pCallbacks);
}

bool CWIMHelper::SetFileIOCallbackTemporaryPath(const std::wstring& path)
{
    if (!EnsureWIMGAPILoaded()) return false;
    return WIMSetFileIOCallbackTemporaryPath(path.c_str());
}

bool CWIMHelper::MessageCallback(DWORD msgId, WPARAM wParam, LPARAM lParam,
                                 void* user)
{
    if (!EnsureWIMGAPILoaded()) return false;
    // WIMMessageCallback 是回调方实现的原型，并非 wimgapi.dll 导出（随包 60
    // 导出表无此项）；保留动态探测仅为兼容个别 ADK 变体，取不到时按回调契约
    // 返回成功（WIM_MSG_SUCCESS == ERROR_SUCCESS == 0）。
    // [R-B87] 模块句柄取自手工 IAT 表。
    using PFN_t = DWORD ( WINAPI*)(DWORD, WPARAM, LPARAM, void*);
    PFN_t pfn = (PFN_t)::GetProcAddress(g_wimApi.hModule, "WIMMessageCallback");
    if (!pfn) return true;
    return pfn(msgId, wParam, lParam, user) == WIM_MSG_SUCCESS;
}

// ============================================================================
// Phase 8 Batch A: Extract variants + Instance enumeration + Instance info
// [R-B87] 全部改为 g_wimApi 手工 IAT 槽；空槽（随包旧版 DLL 无导出）统一
// 置 ERROR_PROC_NOT_FOUND 后优雅失败，绝无空指针调用。
// ============================================================================

bool CWIMHelper::ExtractImageDirectory(WIMHandle hWim,
                                       const std::wstring& imagePath,
                                       const std::wstring& destPath,
                                       DWORD flags)
{
    if (!EnsureWIMGAPILoaded()) return false;
    if (!g_wimApi.WIMExtractImageDirectory) { ::SetLastError(ERROR_PROC_NOT_FOUND); return false; }
    return g_wimApi.WIMExtractImageDirectory(
        hWim, imagePath.c_str(), destPath.c_str(), flags) != FALSE;
}

bool CWIMHelper::ExtractImagePathByWimHandle(WIMHandle hWim,
                                             const std::wstring& imagePath,
                                             const std::wstring& destPath,
                                             DWORD flags)
{
    if (!EnsureWIMGAPILoaded()) return false;
    if (!g_wimApi.WIMExtractImagePathByWimHandle) { ::SetLastError(ERROR_PROC_NOT_FOUND); return false; }
    // 实测导出为 5 参：末参 DWORD（透传内部抽取器，取 0=默认）。
    return g_wimApi.WIMExtractImagePathByWimHandle(
        hWim, imagePath.c_str(), destPath.c_str(), flags, 0) != FALSE;
}

bool CWIMHelper::ExtractInstancePath(WIMInstanceHandle hInstance,
                                     const std::wstring& imagePath,
                                     const std::wstring& destPath,
                                     DWORD flags)
{
    if (!EnsureWIMGAPILoaded()) return false;
    if (!g_wimApi.WIMExtractInstancePath) { ::SetLastError(ERROR_PROC_NOT_FOUND); return false; }
    return g_wimApi.WIMExtractInstancePath(
        hInstance, imagePath.c_str(), destPath.c_str(), flags) != FALSE;
}

HANDLE CWIMHelper::FindFirstInstanceFile(WIMInstanceHandle hInstance,
                                         const std::wstring& path,
                                         WIM_FIND_DATA* pFind)
{
    if (!EnsureWIMGAPILoaded()) return INVALID_HANDLE_VALUE;
    if (!g_wimApi.WIMFindFirstInstanceFile) { ::SetLastError(ERROR_PROC_NOT_FOUND); return INVALID_HANDLE_VALUE; }
    return g_wimApi.WIMFindFirstInstanceFile(hInstance, path.c_str(), pFind);
}

bool CWIMHelper::FindNextInstanceFile(HANDLE hFind, WIM_FIND_DATA* pFind)
{
    if (!EnsureWIMGAPILoaded()) return false;
    if (!g_wimApi.WIMFindNextInstanceFile) { ::SetLastError(ERROR_PROC_NOT_FOUND); return false; }
    return g_wimApi.WIMFindNextInstanceFile(hFind, pFind) != FALSE;
}

bool CWIMHelper::GetInstanceInformation(WIMInstanceHandle hInstance,
                                       IMAGE_INFO* pInfo)
{
    if (!EnsureWIMGAPILoaded() || !pInfo) return false;
    if (!g_wimApi.WIMGetInstanceInformation) { ::SetLastError(ERROR_PROC_NOT_FOUND); return false; }
    void* pv = nullptr;
    DWORD cb = 0;
    if (!g_wimApi.WIMGetInstanceInformation(hInstance, &pv, &cb)) return false;
    if (!pv || cb < sizeof(IMAGE_INFO)) return false;
    ::memcpy(pInfo, pv, sizeof(IMAGE_INFO));
    ::CoTaskMemFree(pv);
    return true;
}

bool CWIMHelper::SetInstanceInformation(WIMInstanceHandle hInstance,
                                       void* pv, DWORD cb)
{
    if (!EnsureWIMGAPILoaded()) return false;
    if (!g_wimApi.WIMSetInstanceInformation) { ::SetLastError(ERROR_PROC_NOT_FOUND); return false; }
    return g_wimApi.WIMSetInstanceInformation(hInstance, pv, cb) != FALSE;
}

bool CWIMHelper::GetMountedImages(PWIM_MOUNT_LIST pList, DWORD* pcbList)
{
    if (!EnsureWIMGAPILoaded()) return false;
    // WIMGetMountedImages 属头文件声明导出，裸名经 [R-B87] 宏重定向到 IAT 槽。
    return WIMGetMountedImages(pList, pcbList) != FALSE;
}

// ============================================================================
// Phase 8 Batch B: WIMBoot extended + File size + Wof + Load + OS info
// ============================================================================

bool CWIMHelper::GetWIMBootEntries(DWORD* pCount, DWORD* pBuf, DWORD cb)
{
    if (!EnsureWIMGAPILoaded()) return false;
    if (!g_wimApi.WIMGetWIMBootEntries) { ::SetLastError(ERROR_PROC_NOT_FOUND); return false; }
    return g_wimApi.WIMGetWIMBootEntries(pCount, pBuf, cb) != FALSE;
}

bool CWIMHelper::GetWIMBootWIMPath(DWORD* pCount, DWORD* pBuf, DWORD cb)
{
    if (!EnsureWIMGAPILoaded()) return false;
    if (!g_wimApi.WIMGetWIMBootWIMPath) { ::SetLastError(ERROR_PROC_NOT_FOUND); return false; }
    return g_wimApi.WIMGetWIMBootWIMPath(pCount, pBuf, cb) != FALSE;
}

bool CWIMHelper::IsCurrentSystemWimboot(BOOL* pResult)
{
    if (!EnsureWIMGAPILoaded()) return false;
    if (!g_wimApi.WIMIsCurrentSystemWimboot) { ::SetLastError(ERROR_PROC_NOT_FOUND); return false; }
    // 实测：随包新版 DLL 完全忽略 PBOOL 出参，直接以返回值给出 WimBoot 状态
    // （内部仅在系统版本恰好 6.3 时做真实判定，其余系统恒返回 FALSE）。
    // 此处把返回值回填 pResult，使本封装语义保持"探测成功 + 出参为状态"。
    BOOL state = g_wimApi.WIMIsCurrentSystemWimboot(nullptr);
    if (pResult) *pResult = state;
    return true;
}

bool CWIMHelper::GetWimFileSize(WIMHandle hWim, ULONGLONG* pSize)
{
    if (!EnsureWIMGAPILoaded() || !pSize) return false;
    if (!g_wimApi.WIMGetWimFileSize) { ::SetLastError(ERROR_PROC_NOT_FOUND); return false; }
    return g_wimApi.WIMGetWimFileSize(hWim, pSize) != FALSE;
}

bool CWIMHelper::InitializeWofDriver()
{
    if (!EnsureWIMGAPILoaded()) return false;
    if (!g_wimApi.WIMInitializeWofDriver) { ::SetLastError(ERROR_PROC_NOT_FOUND); return false; }
    return g_wimApi.WIMInitializeWofDriver() != FALSE;
}

bool CWIMHelper::IsReferenceWim(const std::wstring& path, BOOL* pResult)
{
    if (!EnsureWIMGAPILoaded() || !pResult) return false;
    if (!g_wimApi.WIMIsReferenceWim) { ::SetLastError(ERROR_PROC_NOT_FOUND); return false; }
    return g_wimApi.WIMIsReferenceWim(path.c_str(), pResult) != FALSE;
}

HANDLE CWIMHelper::LoadImage(WIMHandle hWim, DWORD imageIndex)
{
    if (!EnsureWIMGAPILoaded()) return INVALID_HANDLE_VALUE;
    if (hWim == INVALID_HANDLE_VALUE || hWim == nullptr) return INVALID_HANDLE_VALUE;
    if (imageIndex == 0) return INVALID_HANDLE_VALUE;
    return WIMLoadImage(hWim, imageIndex);
}

HANDLE CWIMHelper::LoadInstance(WIMHandle hWim, DWORD instanceIndex)
{
    if (!EnsureWIMGAPILoaded()) return INVALID_HANDLE_VALUE;
    if (!g_wimApi.WIMLoadInstance) { ::SetLastError(ERROR_PROC_NOT_FOUND); return INVALID_HANDLE_VALUE; }
    return g_wimApi.WIMLoadInstance(hWim, instanceIndex);
}

bool CWIMHelper::LoadOSInformation(WIMHandle hWim, void* pv, DWORD cb,
                                    DWORD* pcbActual)
{
    if (!EnsureWIMGAPILoaded()) return false;
    if (!g_wimApi.WIMLoadOSInformation) { ::SetLastError(ERROR_PROC_NOT_FOUND); return false; }
    return g_wimApi.WIMLoadOSInformation(hWim, pv, cb, pcbActual) != FALSE;
}

// ============================================================================
// Phase 8 Batch C: ReadFile variants + Apply helpers + Signing metadata
// ============================================================================

bool CWIMHelper::ReadFileEx(WIMInstanceHandle hInstance,
                            const std::wstring& path,
                            BYTE* buf, DWORD cb,
                            LARGE_INTEGER offset,
                            DWORD* pcbRead, DWORD flags)
{
    if (!EnsureWIMGAPILoaded()) return false;
    if (!g_wimApi.WIMReadFileEx) { ::SetLastError(ERROR_PROC_NOT_FOUND); return false; }
    return g_wimApi.WIMReadFileEx(
        hInstance, path.c_str(), buf, cb, offset, pcbRead, flags) != FALSE;
}

bool CWIMHelper::ReadInstanceFile(WIMInstanceHandle hInstance,
                                  const std::wstring& path,
                                  BYTE* buf, DWORD cb, DWORD* pcbRead)
{
    if (!EnsureWIMGAPILoaded()) return false;
    if (!g_wimApi.WIMReadInstanceFile) { ::SetLastError(ERROR_PROC_NOT_FOUND); return false; }
    return g_wimApi.WIMReadInstanceFile(
        hInstance, path.c_str(), buf, cb, pcbRead) != FALSE;
}

bool CWIMHelper::ProcessCustomImage(WIMImageHandle hImage,
                                    void* pv, DWORD cb)
{
    if (!EnsureWIMGAPILoaded()) return false;
    if (!g_wimApi.WIMProcessCustomImage) { ::SetLastError(ERROR_PROC_NOT_FOUND); return false; }
    return g_wimApi.WIMProcessCustomImage(hImage, pv, cb) != FALSE;
}

bool CWIMHelper::RedirectFolderBeforeApply(WIMImageHandle hImage,
                                           const std::wstring& src,
                                           const std::wstring& dst)
{
    if (!EnsureWIMGAPILoaded()) return false;
    if (!g_wimApi.WIMRedirectFolderBeforeApply) { ::SetLastError(ERROR_PROC_NOT_FOUND); return false; }
    return g_wimApi.WIMRedirectFolderBeforeApply(
        hImage, src.c_str(), dst.c_str()) != FALSE;
}

bool CWIMHelper::SingleInstanceFile(WIMInstanceHandle hInstance,
                                    const std::wstring& path,
                                    BYTE* buf, DWORD cb)
{
    if (!EnsureWIMGAPILoaded()) return false;
    if (!g_wimApi.WIMSingleInstanceFile) { ::SetLastError(ERROR_PROC_NOT_FOUND); return false; }
    return g_wimApi.WIMSingleInstanceFile(
        hInstance, path.c_str(), buf, cb) != FALSE;
}

bool CWIMHelper::SetCachedSigningLevel(const std::wstring& sFile,
                                       const std::wstring& sBasePath,
                                       DWORD dwFlags,
                                       const std::wstring& sAltName)
{
    if (!EnsureWIMGAPILoaded()) return false;
    if (!g_wimApi.WIMSetCachedSigningLevel) { ::SetLastError(ERROR_PROC_NOT_FOUND); return false; }
    // 实测：4 参路径语义；可选字符串允许为空（传 nullptr，避免空 c_str() 歧义）。
    PCWSTR pBase = sBasePath.empty() ? nullptr : sBasePath.c_str();
    PCWSTR pAlt  = sAltName.empty()  ? nullptr : sAltName.c_str();
    return g_wimApi.WIMSetCachedSigningLevel(
        sFile.c_str(), pBase, dwFlags, pAlt) != FALSE;
}

bool CWIMHelper::SetImageUserSpecifiedCreationTime(
    WIMImageHandle hImage, LPFILETIME pTime)
{
    if (!EnsureWIMGAPILoaded()) return false;
    if (!g_wimApi.WIMSetImageUserSpecifiedCreationTime) { ::SetLastError(ERROR_PROC_NOT_FOUND); return false; }
    return g_wimApi.WIMSetImageUserSpecifiedCreationTime(hImage, pTime) != FALSE;
}

bool CWIMHelper::SetWimGuid(WIMHandle hWim, const GUID* pGuid)
{
    if (!EnsureWIMGAPILoaded() || !pGuid) return false;
    if (!g_wimApi.WIMSetWimGuid) { ::SetLastError(ERROR_PROC_NOT_FOUND); return false; }
    return g_wimApi.WIMSetWimGuid(hWim, pGuid) != FALSE;
}

// ============================================================================
// Phase 9: Direct DLL-export wrappers
// [R-B87] 不再每次 GetProcAddress：全部在加载期一次性解析进 g_wimApi 槽。
// ============================================================================

// WIMAddImagePath - 向 WIM 追加一个映像路径（随包 DLL 即有导出）。
bool CWIMHelper::WIMAddImagePathDirect(WIMHandle hWim,
                                       const std::wstring& imagePath,
                                       DWORD flags)
{
    if (!EnsureWIMGAPILoaded()) return false;
    if (!g_wimApi.WIMAddImagePath) { ::SetLastError(ERROR_PROC_NOT_FOUND); return false; }
    return g_wimApi.WIMAddImagePath(hWim, imagePath.c_str(), flags) != FALSE;
}

// WIMAddImagePaths - 一次追加多个映像路径。
bool CWIMHelper::WIMAddImagePathsDirect(WIMHandle hWim,
                                        const std::vector<std::wstring>& paths,
                                        DWORD flags)
{
    if (!EnsureWIMGAPILoaded()) return false;
    if (paths.empty()) return false;
    if (hWim == INVALID_HANDLE_VALUE || hWim == nullptr) return false;
    if (!g_wimApi.WIMAddImagePaths) { ::SetLastError(ERROR_PROC_NOT_FOUND); return false; }
    std::vector<std::wstring> storage = paths;
    std::vector<PCWSTR> arr(paths.size());
    for (size_t i = 0; i < storage.size(); ++i) arr[i] = storage[i].c_str();
    return g_wimApi.WIMAddImagePaths(
        hWim, arr.data(), static_cast<DWORD>(arr.size()), flags) != FALSE;
}

// WIMApplyInstance - 应用单个文件实例（高版本 ADK 可选导出）。
bool CWIMHelper::WIMApplyInstanceDirect(WIMInstanceHandle hInstance,
                                        const std::wstring& targetPath,
                                        DWORD flags)
{
    if (!EnsureWIMGAPILoaded()) return false;
    if (!g_wimApi.WIMApplyInstance) { ::SetLastError(ERROR_PROC_NOT_FOUND); return false; }
    return g_wimApi.WIMApplyInstance(
        hInstance, targetPath.c_str(), flags) != FALSE;
}

// WIMCreateFileEx - 扩展版 WIM 创建（高版本 ADK 可选导出）。
HANDLE CWIMHelper::WIMCreateFileExDirect(const std::wstring& path,
                                          DWORD access, DWORD disp, DWORD attrs,
                                          DWORD compression, DWORD* pCreationResult)
{
    if (!EnsureWIMGAPILoaded()) return INVALID_HANDLE_VALUE;
    if (!g_wimApi.WIMCreateFileEx) { ::SetLastError(ERROR_PROC_NOT_FOUND); return INVALID_HANDLE_VALUE; }
    // 实测导出为 7 参：compression 之后有一个 WORD 扩展槽（仅接受 0 或 2，
    // 当前导出体未实际使用），末参才是 pCreationResult。
    return g_wimApi.WIMCreateFileEx(
        path.c_str(), access, disp, attrs, compression, 0, pCreationResult);
}

// WIMCreateWofCompressedFile - 创建 WofCompressed WIM（随包 DLL 即有导出）。
HANDLE CWIMHelper::WIMCreateWofCompressedFileDirect(const std::wstring& path,
                                                     DWORD compression,
                                                     DWORD access)
{
    if (!EnsureWIMGAPILoaded()) return INVALID_HANDLE_VALUE;
    if (!g_wimApi.WIMCreateWofCompressedFile) { ::SetLastError(ERROR_PROC_NOT_FOUND); return INVALID_HANDLE_VALUE; }
    return g_wimApi.WIMCreateWofCompressedFile(path.c_str(), compression, access);
}

// WIMMountImage - 按 WIM 路径+索引挂载。裸名经宏重定向到 IAT 槽。
// 注意官方按路径挂载导出的真实签名是 (挂载点, WIM路径, 索引, 临时目录)，
// 不接收 flags（只读等标志须走句柄版 MountImageHandle）；早期封装按
// (WIM,索引,挂载点,flags) 传参实为错序调用，[R-B87] 按导出签名纠正。
bool CWIMHelper::WIMMountImageDirect(const std::wstring& wimPath, DWORD imageIndex,
                                     const std::wstring& mountPath, DWORD flags)
{
    if (!EnsureWIMGAPILoaded()) return false;
    (void)flags;
    SHCreateDirectoryExW(nullptr, mountPath.c_str(), nullptr);
    return WIMMountImage(mountPath.c_str(), wimPath.c_str(),
                         imageIndex, nullptr) != FALSE;
}

// WIMUnmountImage - 按 WIM 路径+索引卸载。真实签名 (挂载点, WIM路径, 索引,
// 是否提交)，早期封装参数顺序错误，[R-B87] 按导出签名纠正。
bool CWIMHelper::WIMUnmountImageDirect(const std::wstring& wimPath, DWORD imageIndex,
                                       const std::wstring& mountPath, BOOL commit)
{
    if (!EnsureWIMGAPILoaded()) return false;
    return WIMUnmountImage(mountPath.c_str(), wimPath.c_str(),
                           imageIndex, commit) != FALSE;
}

// ============================================================================
// Default callback dispatcher (no-op baseline; UI overrides by registering
// a real WIMMsgProc through CWIMHelper::RegisterCallback).
// ============================================================================

DWORD WINAPI CWIMHelper::DefaultMessageProc(DWORD msgId, WPARAM wParam,
                                             LPARAM lParam, void* user)
{
    (void)msgId; (void)wParam; (void)lParam; (void)user;
    return WIM_MSG_SUCCESS;
}
