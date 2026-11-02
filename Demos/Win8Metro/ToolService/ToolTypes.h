#pragma once

// ============================================================
// ToolTypes.h —— [R6] 工具箱弹窗 Service 层通用类型（零 UI 依赖）
//
// 本头文件禁止 include UIlib.h / 任何 C*UI 控件类型（R7 契约）：
// Service 层只认 STL/windows 系统类型，UI 表现由 ToolDialogPage
// 负责渲染，边界处做 CDuiString <-> std::wstring 转换。
//
// 枚举值与旧 ToolDialog.h 的 TD_* 严格一致（同一弹窗编号契约）。
// ============================================================

#include <string>
#include <vector>
#include <cstdint>
#include <utility>

namespace toolsvc {

// 弹窗种类（值契约 = 旧 EToolDlgKind / 工具箱 toolitem_N 序号）
enum class EToolKind : int
{
    TK_CAPTURE    = 0,   // 另存为映像
    TK_APPLY      = 1,   // 释放映像
    TK_ACCOUNT    = 2,   // 账户管理
    TK_IMAGEX     = 7,   // 编辑映像文件
    TK_ISO        = 9,   // ISO 生成器
    TK_DETAILS    = 13,  // 详细设置
    TK_LOGVIEW    = 14,  // 查看日志
    TK_DEPGRAPH   = 15,  // 依赖图可视化
    TK_ESDWIZ     = 16,  // ESD 解密向导
    TK_LINKLIST   = 17,  // 超链接列表
    TK_DUALPATH   = 18,  // 双路径选择器
    TK_DELCONFIRM = 19,  // 删除确认列表
    TK_MOUNT      = 20,  // 挂载向导
    TK_PECFG      = 21,  // PE 映像设置
    TK_BOOTFIX    = 22,  // 引导修复
};

// Service 统一返回值：bOk=false 时 sErr 为中文原因（Page 决定如何展示）。
struct ToolResult
{
    bool         bOk = false;
    int          nCode = 0;          // 0=成功；非 0 为服务内错误码（Page 可映射）
    std::wstring sErr;

    static ToolResult Ok() { ToolResult r; r.bOk = true; return r; }
    static ToolResult Fail(std::wstring sWhy, int nCode = -1)
    {
        ToolResult r;
        r.bOk = false;
        r.nCode = nCode;
        r.sErr = std::move(sWhy);
        return r;
    }
};

// [R16][T4] 依赖图一个节点（服务层 DTO，自 LogDepGraphService.h 迁入，
// 使 ToolOpenArgs 可直接携带依赖图数据；字段语义不变）。
struct TDepNode
{
    std::wstring sLabel;
    int          nParent = -1;   // <0 表示根节点
    int          nLevel = 0;
};

// 打开弹窗时由 Page 注入的入参集合（替代旧 Set*/构造期零散注入）。
struct ToolOpenArgs
{
    // ESD 解密向导
    std::wstring              sInitialEsdFile;   // 外壳 /ESDDECRYPT 预填
    bool                      bHasInitialEsd = false;
    std::vector<std::wstring> vEsdKeys;          // 6 条 base64 RSA 公钥

    // PE 映像设置（挂载向导跳转预填挂载目录）
    std::wstring              sInitialPeDir;
    bool                      bHasInitialPeDir = false;

    // 超链接列表（label,target）
    std::vector<std::pair<std::wstring, std::wstring> > vLinks;
    std::wstring              sLinkTitle;

    // 双路径选择器
    std::wstring              sDualTitle;
    std::wstring              sDualLabel1;
    std::wstring              sDualLabel2;

    // 删除确认列表
    std::vector<std::wstring> vDelItems;
    std::wstring              sDelTitle;

    // 依赖图可视化（[R16][T4] 扁平节点数组，经 Open 注入 CLogDepGraphService）
    std::vector<TDepNode>     vDepNodes;
};

// WIM/ESD 映像索引（服务层 DTO，对应旧 TDialogWimIndex，字段语义不变）。
struct TWimIndex
{
    int          nIndex = 0;
    std::wstring sName;
    std::wstring sDesc;
    std::wstring sEdition;
    std::wstring sVersion;
    std::wstring sArch;
    std::wstring sProduct;
    std::wstring sCreated;
    std::wstring sSize;
};

// 引导修复分区行（对应旧 TBfPart）。
struct TBfPart
{
    std::wstring sPos;
    std::wstring sName;
    std::wstring sSize;
    std::wstring sType;
    std::wstring sVolumeGuid;
    std::wstring sFs;
    uint32_t     dwDisk = 0;
    uint32_t     dwPart = 0;
    bool         bEfi = false;
    bool         bMsr = false;
    bool         bPrimary = false;
    bool         bActive = false;
};

// [R7] wimgapi 压缩模式（取值与 SDK wimgapi.h WIM_COMPRESS_* 一致），
// 供 Page 与服务工厂共用，避免 Page 直接依赖 WIMHelperBridge.h。
const uint32_t IMG_COMPRESS_NONE = 0;
const uint32_t IMG_COMPRESS_LZX  = 2;  // XPRESS=1
const uint32_t IMG_COMPRESS_LZMS = 4;

// [R7] Apply 位标志（取值见 WIMHelperBridge.cpp 注释，源自 wimgapi.h）
const uint32_t IMG_FLAG_WIM_BOOT      = 0x2000;
const uint32_t IMG_FLAG_APPLY_COMPACT = 0x4000;

// [R7] 映像容器魔数探测结果（取值与 WIMHelper.h WIM_CONTAINER_KIND 对齐）。
enum class EImgContainerKind : int
{
    IMG_CK_UNKNOWN = 0,
    IMG_CK_WIM,          // 明文 WIM（XPRESS/LZX）
    IMG_CK_ESD,          // 明文 ESD（LZMS）
    IMG_CK_ENCRYPTED_ESD,// 加密 ESD
    IMG_CK_SWM,          // 跨卷分卷集
    IMG_CK_ISO,          // ISO9660 / UDF
    IMG_CK_VHD,
    IMG_CK_VHDX
};

// [R7] 容器探测详情（对应旧 WIM_CONTAINER_INFO 的 Page 使用字段）。
struct TContainerInfo
{
    EImgContainerKind nKind = EImgContainerKind::IMG_CK_UNKNOWN;
    uint32_t   nImageCount = 0;
    uint32_t   nPartNumber = 0;
    uint32_t   nTotalParts = 0;
};

// [R6-05h] 引导修复（bcdboot）执行结果。
struct TBfFixResult
{
    uint32_t dwExitCode = 0xFFFFFFFF; // bcdboot 退出码（0=成功）
    uint32_t dwMountErr = 0;          // 临时分配盘符失败时的 GLE（0=未发生）
    bool     bTempAssigned = false;   // 服务临时分配并已尝试回收盘符
};

// [R6-05c] 已挂载映像行（双源合并后的结果，顺序即下拉显示顺序）。
struct TMountedRow
{
    std::wstring sDir;                   // 挂载点
    std::wstring sWim;                   // 对应 WIM/ESD（记账比对后的真实值）
    uint32_t     dwIdx = 0;
    bool         bHasIdx = false;        // 是否显示索引（记账回退行无）
    bool         bRW = false;            // 可写挂载
    bool         bStale = false;         // 挂载目录已不存在
    bool         bReal = false;          // true=wimgapi 真实枚举行；false=记账回退行
};

// 后台作业进度接收器：Page 实现并把进度转发到模态进度窗；
// Service/工作线程内只认本接口，绝不接触 DuiLib 控件（R7 契约）。
class IToolJobSink
{
public:
    virtual ~IToolJobSink() {}
    virtual void OnProgress(int nPercent, const std::wstring& sText) = 0;
    // [R6-05g] 工作线程内分级日志（0/1/2=INFO/WARN/ERROR）。
    virtual void OnLog(int nLevel, const std::wstring& sLine) = 0;
    // [R6-05g] 进度窗阶段文字切换（"正在格式化…"等）。
    virtual void OnStage(const std::wstring& sText) = 0;
};

// [R6-05g] 释放映像作业参数（逐字对应旧 TApplyJob 业务字段，去掉 HWND/UI 串）。
struct TApplyJobArgs
{
    std::wstring sWim;        // 映像路径
    std::wstring sDir;        // 目标根（服务内保证带尾反斜杠）
    std::wstring sTgtDr;      // 目标卷 "D:"
    std::wstring sDrvPath;    // 离线驱动目录（可空）
    uint32_t     nIndex = 1;
    uint32_t     dwApply = 0; // WIM_FLAG_* 位
    bool         bWIMBoot = false;
    bool         bCompact = false;
    bool         bWTG = false;
    bool         bAddBoot = false;
    bool         bFormat = false;
    std::wstring sSysDrvWarn; // "目标=系统盘，跳过格式化"文案（UI 侧解析后注入）
};

// [R6-05g] 释放映像作业结果（对应旧 TJobResult 的 Apply 专用字段）。
struct TApplyJobResult
{
    bool         bOk = false;
    bool         bBootAttempted = false;
    bool         bBootOk = false;
    bool         bCloseOnOk = false;
    std::wstring sErrMsg;
    std::wstring sBootErr;
    std::wstring sOkMsg;
    // [R15] 阶段化结果：失败时指明已推进到哪一步，便于区分
    // "格式化已完成但释放失败" 与 "前置检查就失败" 等不同恢复路径。
    // 取值见 TApplyStage；仅新增字段，不改变既有字段语义。
    int          nStage = 0;
    std::wstring sStageName;
};

// [R15] Apply 作业阶段枚举（顺序即推进顺序）。
enum class TApplyStage : int
{
    Precheck = 0,   // 目标目录/卷前置校验
    Format   = 1,   // 格式化目标卷
    Apply    = 2,   // 释放映像文件
    Driver   = 3,   // 离线驱动注入
    Boot     = 4,   // 引导配置（bcdboot/bootsect）
    Done     = 5,   // 全部完成
};

} // namespace toolsvc
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
