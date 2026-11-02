#pragma once

// ============================================================
// FeatureService —— 「Windows 功能」系统层
//
// [P0-A3 2026-10-11] 原生直连重构：
//   旧实现（已删除）：_getfeat.bat → ShellExecute runas 提权 →
//   featraw.txt ANSI 输出 → featraw.sig 轮询 → 逐行文本解析。
//   新实现：后台线程直连 dismapi.dll DismGetFeatures / DismEnableFeature /
//   DismDisableFeature（DismApiBridge），结构化结果经 PostMessage 回 UI；
//   WinPE 离线会话自动定向 DismOpenSession(离线根)。
// 不依赖 UI / StringMgr；实现文件不使用预编译头。
// ============================================================

#include <windows.h>

#ifndef UILIB_API
#define UILIB_API __declspec(dllimport)
#endif

#include "Utils/Utils.h"
#include <string>
#include <vector>

// S5 实现文件不使用预编译头，需在引入 UIlib 全命名空间前显式可见该类型
using DuiLib::CDuiString;

// Windows optional feature row (程序和功能 → Windows功能)
struct FeatInfo
{
    FeatInfo()
        : bEnabled(false), nPending(-1), bExpanded(false), nParent(-1) {}
    CDuiString sSys;        // DISM feature name, e.g. Microsoft-Hyper-V-All
    CDuiString sDisp;       // localized display name
    CDuiString sDesc;       // [rb26] 中文说明注解（详情面板用；虚拟分组也有）
    bool       bEnabled;    // committed state
    // 待处理：-1 不变，0 禁用，1 启用，2 永久移除（disable-feature /remove）
    int        nPending;
    bool       bExpanded;
    int        nParent;     // parent row index, -1 for roots
    std::vector<int> vChild;
};

// DismGetFeatures 枚举出的一条原始功能（系统名 + 启用态）
struct FeatRawEntry
{
    CDuiString sSys;
    bool       bEnabled;
};

// 枚举线程完成消息体（WM_FEAT_DATA，lParam 指向，UI 线程 delete）
struct FeatDataMsg
{
    std::vector<FeatRawEntry> vRaw;
    int    nRc = 0;          // 1=有条目；0=API 失败；-1=成功但 0 条
    CDuiString sErr;         // nRc=0 时的错误文本
};

// [P1] 功能枚举分批回报。DismGetFeatures 一次性返回全部数组（无逐项回调），
// 因此"流式"在数据到手后做：工作线程每攒够 N 条 PostMessage 一批，
// 页面逐批追加渲染，首屏不必等整表构建。
// bFinal=true 为末批，携带 nRc 供页面收尾判定。
struct FeatPartialMsg
{
    std::vector<FeatRawEntry> vBatch;
    bool bFinal = false;
    int  nRc = 0;
    // [PROGRESS 2026-10-25] 总数与累计已发送数，供页面显示真实百分比。
    //   与可选功能页同口径：枚举返回时总数已知，
    //   分批只是发送节奏；把它丢掉就只能无限转圈。
    int  nTotal = 0;   // 本轮枚举总条数（0=未知）
    int  nSent  = 0;   // 截至本批累计已发送条数
};

// 一条待应用变更
struct FeatApplyItem
{
    CDuiString sName;
    int        nOp;          // 0=禁用 1=启用 2=永久移除（/remove）
};

// 应用线程完成消息体（WM_FEAT_APPLYDONE，lParam 指向，UI 线程 delete）
struct FeatApplyMsg
{
    int        nTotal = 0;
    int        nOk = 0;
    int        nFail = 0;
    bool       bRebootHint = false; // 任一操作返回 3010
    CDuiString sDetail;             // 逐项结果文本（供日志区）
};

class FeatureService
{
public:
    // 启动功能枚举后台线程；完成后向 hNotify PostMessage WM_FEAT_DATA。
    // 在线/离线（WinPE 目标映像）由 DataEngine 当前会话决定。
    static void StartEnumThread(HWND hNotify);

    // 启动功能变更后台线程；逐项调用 dismapi（带本地源 /LimitAccess 透传），
    // 完成后 PostMessage WM_FEAT_APPLYDONE。vItems 在调用线程内被复制。
    // [CRASHFIX-STUCK] 返回 false = 线程创建失败，WM_FEAT_APPLYDONE 永远不会
    // 到达——调用方必须自行复位"在途"标志并提示，否则提交入口被永久封锁
    // （旧实现静默失败，m_bApplying 卡死只能重启程序）。
    static bool StartApplyThread(HWND hNotify,
                                 const std::vector<FeatApplyItem>& vItems,
                                 const CDuiString& sSource,
                                 bool bLimitAccess);

    // [T1 P0] Windows 功能枚举结果 TTL 磁盘缓存（stale-while-revalidate，
    // 对齐 AppxCapUpdService::SaveCapCache 先例）：
    //   * SaveFeatCache  枚举成功后落盘（%LOCALAPPDATA%\DismTool_Clone\feat_enum_cache.dat）
    //   * LoadFeatCache  命中且未过期（3600s）回填 raw entries，返回 true
    //   * InvalidateFeatCache  功能启用/禁用/移除完成后立即删除，避免读旧状态
    // 离线会话（WinPE 目标映像）不读写缓存（调用方决定）；读写失败静默降级。
    static bool SaveFeatCache(const std::vector<FeatRawEntry>& vFeats);
    static bool LoadFeatCache(std::vector<FeatRawEntry>& vOut, bool bIgnoreTtl = false);
    static void InvalidateFeatCache();

    // [R79 P1] 写后精准失效：只更新命中 sSys 的行 state 并刷新首行时间戳，
    //   不再删除整个缓存。返回 false 表示「没有可更新基底」或「一项都没命中」，
    //   调用方应回退到 InvalidateFeatCache（不能假装更新成功）。
    // [R79] sPathOverride 仅供自检构造临时文件（同 LoadSeedFeatCache 约定）。
    static bool UpdateFeatCacheStates(const std::vector<FeatRawEntry>& vChanged,
                                      const std::wstring& sPathOverride = std::wstring());

    // [P4] 种子缓存：随包分发的出厂枚举快照（exe\seed\seed_feat.dat）。
    // 用于全新机器首次进页秒开，无 TTL；**必须指纹匹配**才返回 true。
    // 指纹 = OSBUILD|SKU|LANG，不匹配时一律返回 false（宁可转圈不显示幻影条目）。
    // 仅在磁盘缓存未命中后作为兜底调用；命中后页面照常发起后台枚举并整体重建。
    // [R60] 增加可选路径参数（仅供自检构造坏文件）。
    static bool LoadSeedFeatCache(std::vector<FeatRawEntry>& vOut,
                                  const std::wstring& sPathOverride = std::wstring());
};
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
