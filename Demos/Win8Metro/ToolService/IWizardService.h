#pragma once

// ============================================================
// IWizardService.h —— [R6-05] 六大向导业务服务（零 UI）。
//
// 覆盖（逐切片绞杀迁入）：Capture / Apply / Account / Mount /
// PeCfg / BootFix。UI 渲染（Build*）与交互确认仍在 Page；
// 服务只做文件/diskpart/进程/注册表等副作用与纯计算。
//
// 唯一服务间依赖：Wizard -> IIsoImageService（构造注入，
// 释放映像向导需复用 wimgapi 作业工厂与 ISO 槽位）。
// ============================================================

#include <memory>
#include <string>
#include <vector>

#include "ToolTypes.h"          // toolsvc::TBfPart ...

namespace toolsvc {

class IIsoImageService;
class IToolServiceHost;

class IWizardService
{
public:
    virtual ~IWizardService() {}

    // ================= BootFix（引导修复）=================
    // 物理磁盘 IOCTL 真实枚举分区，给出 UEFI/BIOS 推荐项序号。
    // 成功且 vOut 非空返回 true；nRecommended 为推荐下标（-1=无推荐）。
    virtual bool BfEnumerateParts(std::vector<TBfPart>& vOut,
                                  int& nRecommended) = 0;
    // 从 Z 向前找空闲盘符（C 起步），0=无可用盘符。
    virtual wchar_t BfFreeLetter() = 0;

    // ================= Mount（挂载向导）==================
    // 行结构 TMountedRow 定义在 ToolTypes.h（与 TBfPart 同层）。

    // 双源合并枚举（wimgapi 真实枚举优先，MountLedger 回退），
    // 逻辑逐字搬自 Page RefreshMountedList(2026-10-13)。
    virtual void MountEnumRows(std::vector<TMountedRow>& vRows) = 0;
    // 挂载失败后统计异常挂载点数量（记账目录已不存在）。
    virtual int  MountCountStale() = 0;

    // ================= PeCfg（PE 映像设置）================
    // 写离线蜂巢：nScratch!=0 写 SYSTEM\...\fbwf!WinPECacheThreshold；
    // sTarget 非空写 SOFTWARE\...\WinPE!InstRoot（统一为 X:\ 根）。
    // 返回 false=SYSTEM/SOFTWARE 蜂巢加载失败（页提示不可写挂载）；
    // nFail 回填单项写入失败数（0=全部成功）。过程日志经宿主输出。
    virtual bool PeCfgWriteHive(const std::wstring& sMountDir,
                                int nScratch,
                                const std::wstring& sTarget,
                                int& nFail) = 0;
    // 读离线蜂巢当前两项（空串=未设置）；false=蜂巢加载失败。警告日志经宿主。
    virtual bool PeCfgReadHive(const std::wstring& sMountDir,
                               std::wstring& sScratch,
                               std::wstring& sTarget) = 0;

    // ================= Account（账户管理）=================
    struct TAcctInfo
    {
        std::wstring sName;
        uint32_t     dwFlags = 0;
        std::wstring sDesc;
    };
    // NetUserEnum 枚举本机普通账户并按原版权重排序（Administrator→
    // Guest→DefaultAccount→其他→WDAG）。失败返回 false 且 dwStatus=NET_API_STATUS。
    virtual bool AcctLoad(std::vector<TAcctInfo>& vOut, uint32_t& dwStatus) = 0;
    // 启用账户（清 UF_ACCOUNTDISABLE）；返回 NET_API_STATUS（0=NERR_Success）。
    virtual uint32_t AcctEnable(const std::wstring& sName, uint32_t dwFlags) = 0;
    // 清空账户密码；返回 NET_API_STATUS。
    virtual uint32_t AcctClearPassword(const std::wstring& sName) = 0;

    // ================= Apply（释放映像，最高危作业）=================
    // 工作线程内执行：WOF 驱动→BCD 备份→快速格式化→wimgapi 释放→
    // 离线驱动注入→bcdboot/bootsect 引导链并验证。全程只经 pSink
    // 回传进度/阶段/日志，不建线程、不碰控件、不弹框。
    // 异常由调用方（Page 线程过程）SEH 兜底；结果填 r。
    virtual void ApplyRunJob(const TApplyJobArgs& a,
                             IToolJobSink* pSink,
                             TApplyJobResult& r) = 0;

    // ================= BootFix 执行（bcdboot）=================
    // 目标盘符优先取 sPos 括号内既有盘符；无则临时分配空闲盘符、执行后
    // 立即回收。返回 false=临时挂载点分配失败（r.dwMountErr=GLE）；
    // true=bcdboot 已执行，退出码在 r.dwExitCode。日志经 pSink。
    virtual bool BfFixBoot(const TBfPart& pt, bool bUefi,
                           IToolJobSink* pSink, TBfFixResult& r) = 0;
};

// 工厂：pIso 映像服务、pHost 宿主能力（均构造注入，Wizard 不持有所有权）。
std::unique_ptr<IWizardService> CreateWizardService(IIsoImageService* pIso,
                                                    IToolServiceHost* pHost);

} // namespace toolsvc
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
