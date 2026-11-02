#pragma once

// ============================================================
// RegCleanRule.h —— [R79] 注册表清理规则集数据结构
//
// 四件套之一：规则集（Rule）
// 数据来源：Data.xml 优化根下含 Reg* 子节点的 Item
//   * RegDelete 子节点 —— 删除某个值或键
//   * RegWrite 子节点 —— 写某个值
//   * RegMove  子节点 —— 搬移某个值
//
// 纯数据结构，无 UI/系统调用依赖；实现文件不使用预编译头。
// ============================================================

#include <vector>
#include <windows.h>
#include "Utils/Utils.h"

using DuiLib::CDuiString;

// 动作类型
enum class RegCleanActionType : int
{
    REGCLEAN_ACTION_DELETE = 0,   // RegDelete
    REGCLEAN_ACTION_WRITE  = 1,   // RegWrite
    REGCLEAN_ACTION_MOVE   = 2,   // RegMove
};

// 单条注册表清理动作
struct RegCleanAction
{
    RegCleanAction()
        : eType(RegCleanActionType::REGCLEAN_ACTION_DELETE)
        , bWow64(false)
        , bSkipError(false)
        , nRegType(REG_NONE)
    {}

    RegCleanActionType eType;    // 动作类型
    CDuiString         sKey;     // 目标键（HKCU\... 或 HKLM\...）
    CDuiString         sValue;   // 值名（空 = 键本身）
    CDuiString         sType;    // RegWrite 类型字符串（REG_DWORD/REG_SZ/...）
    CDuiString         sData;    // RegWrite 数据
    CDuiString         sDstKey;  // RegMove 目标键
    CDuiString         sDstValue;// RegMove 目标值名
    DWORD              nRegType; // 解析后的 REG_* 类型
    bool               bWow64;   // Wow64="True"
    bool               bSkipError;// SkipError="2"
};

// 一条注册表清理规则（对应一个 Item）
struct RegCleanRule
{
    RegCleanRule()
        : nLevel(0)
        , bSelected(true)
        , bApplicable(true)
        , nScanState(-1)   // -1 未扫描 / 0 无需清理 / 1 可清理
    {}

    CDuiString                  sName;       // Item 显示名
    CDuiString                  sGroup;      // 所属分组（Group Name）
    int                         nLevel;      // 风险档（0 安全 / 3 高风险）
    bool                        bSelected;   // 用户勾选
    bool                        bApplicable; // Applicable 条件当前是否成立
    int                         nScanState;  // 扫描后状态
    std::vector<RegCleanAction> actions;     // 该 Item 下所有 Reg 类动作
};

// RegClean 备份条目（用于撤销/还原）
struct RegCleanBackupEntry
{
    CDuiString         sKey;
    CDuiString         sValue;
    CDuiString         sType;
    CDuiString         sData;
    DWORD              nRegType;
    bool               bWow64;
    bool               bExisted;   // 清理前该值是否存在
};

// 一次备份快照
struct RegCleanBackup
{
    CDuiString                      sTime;   // yyyy-MM-dd HH:mm:ss
    CDuiString                      sFile;   // .reg 备份文件路径（64/本机视图）
    CDuiString                      sFileWow32; // [B3] 32 位视图（Wow64=True）备份；无则空
    std::vector<RegCleanBackupEntry> entries;
};
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
