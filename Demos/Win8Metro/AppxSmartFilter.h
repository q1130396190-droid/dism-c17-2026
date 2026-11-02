#pragma once

// ============================================================
// AppxSmartFilter —— 应用包页「智能筛选 + 分级中文映射」
//
// 需求来源（本轮）：
//   1) 中文映射继续完善，并且针对 Windows 11 单独给一套：
//      Win11 上加载扩展映射，Win10 上不加载、不显示；
//   2) 新增智能筛选：按「全部 / 用户应用 / 系统组件 / 运行时框架 /
//      语言资源包」分类，并对无意义的 GUID 名做同名合并；
//   3) 可选功能页同样按 Win11 分级补中文映射。
//
// 关键设计：
//   * 版本门控只在**一个地方**判：AppxMapTierApplies()。所有 Windows 11
//     独有条目都走 TierOf()，Win10 上它们既不入索引也不进下拉框，
//     因此「不显示」与「不加载」是同一件事，不会出现只在 UI 隐藏、
//     逻辑里仍然命中的半吊子状态。
//   * 查询是纯函数 + 进程内一次建索引（magic static），没有副作用，
//     可在枚举线程、UI 线程同时调用。
//   * 本文件不含任何写死的中文文案（映射表除外）——下拉框项与计数
//     文案全部来自语言包键，见 AppxSmartFilter.cpp 顶部键名清单。
// ============================================================

#include <windows.h>

#include <vector>

#include "Utils/Utils.h"

// 智能筛选分类（顺序即下拉框顺序，0 必须为「全部」）
enum class EAppxCategory : int {
    All        = 0,
    UserApp    = 1,   // 用户可见应用（天气 / 记事本 / 终端…）
    SystemComp = 2,   // 系统组件与外壳（凭据宿主 / 系统组件 GUID…）
    Runtime    = 3,   // 运行时框架（.NET Native / VCLibs / UI.Xaml…）
    LangPack   = 4    // 语言包与资源包（Language.* / ~W~~~ 资源包）
};

// [R200] Appx 中文映射分级。
//   Base  = Win10/Win11 通用（当前已上线的 126 条 + 本轮补齐的通用条目）
//   Win11 = 仅 Windows 11 及以上（Win11 独有包 / 新版改名包）
//   Unknown = 表外条目
enum class EAppxNameTier : int {
    Unknown = 0,
    Base    = 1,
    Win11   = 2
};

// [R200] 智能筛选的判定结果：一次算清，渲染与过滤共用同一份，
//   避免「下拉框命中一项、列表算另一项」的口径漂移。
struct AppxSmartInfo
{
    CDuiString    sName;      // 展示名（已做同名合并，不含版本号差异）
    CDuiString    sCategory;  // 分类文案（语言包键 appx.smart.cat_*）
    EAppxNameTier nTier = EAppxNameTier::Unknown;
    EAppxCategory nCat = EAppxCategory::All;
    bool          bResourcePkg = false;   // ~W~~~ 语言资源包
};

// 名称映射查询。sFull 为 PackageFullName（Name_Version_Arch_PublisherId）。
// 命中当前系统分级的映射时返回 true 并填 outName。
[[nodiscard]] bool AppxSmartLookupName(const DuiLib::CDuiString& sFull,
                                       DuiLib::CDuiString& outName,
                                       EAppxNameTier* pTier = nullptr);

// 一次算清展示名 + 分类 + 分级。未命中映射时 sName 为空，调用方回退原始包名。
[[nodiscard]] AppxSmartInfo AppxSmartDescribe(const DuiLib::CDuiString& sFull,
                                              bool bIsResourceFlag);

// 分类下拉框的项列表：Win10 上不含任何 Win11 独有项（当前五项均为通用）。
void AppxSmartFillCategoryCombo(DuiLib::CComboUI* pCombo);

// 分类判定（供过滤使用，不构造字符串）。
[[nodiscard]] EAppxCategory AppxSmartClassify(const DuiLib::CDuiString& sFull,
                                              bool bIsResourceFlag);

// 关键词 + 分类的联合匹配。sKwLower 必须是已转小写的关键词
// （空串表示不做关键词过滤）。
[[nodiscard]] bool AppxSmartMatches(const DuiLib::CDuiString& sFull,
                                    const AppxSmartInfo& info,
                                    const DuiLib::CDuiString& sKwLower,
                                    EAppxCategory nCat);

// 下拉框项文案（语言包键 appx.smart.cat_*），供计数/日志复用。
[[nodiscard]] const wchar_t* AppxSmartCategoryText(EAppxCategory nCat);

// Win11 分级下实际生效的映射条数（日志自证用：Win10 上应等于 Base 条数）。
[[nodiscard]] int AppxSmartActiveEntryCount();
