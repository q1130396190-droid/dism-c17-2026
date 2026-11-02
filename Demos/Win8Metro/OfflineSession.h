#pragma once

// ============================================================
// OfflineSession.h —— [R10-02] WinPE 离线蜂巢会话独立组件
//
// 原状态与逻辑全部内聚在 CDataEngine（静态成员 + 匿名命名空间），
// 导致 IntlService / CleanEngine / AppxCapUpdService / FeatureService
// 等服务层为读写离线状态被迫静态直调 CDataEngine（CP-4 上帝类耦合）。
// 现抽为零依赖独立组件：
//   * 服务层直接调 COfflineSession，不再 include/引用 CDataEngine；
//   * CDataEngine 保留同名静态 API 作薄转发（DataEngine_Reg.cpp），
//     页面/壳侧既有调用链零改动；
//   * 行为与挂载点前缀（DPP_OFF_*）逐字保持。
// ============================================================

#include "UIlib.h"   // CDuiString

class COfflineSession
{
public:
    // 挂载目标 <root>\Windows\System32\config 下 SOFTWARE/SYSTEM/
    // SAM/SECURITY/DEFAULT 以及 Default 用户 NTUSER.DAT。
    // 成功前提：SOFTWARE 与 SYSTEM 挂载成功（其余缺失降级）。
    static bool Begin(const wchar_t* pszImageRoot);
    static void End();

    static bool IsActive();

    // [FIX] 目标镜像真实 build（离线 SOFTWARE hive 的 CurrentBuildNumber）。
    // 未激活或读取失败返回 0；调用方据此拒绝把宿主版本套用到离线镜像。
    static unsigned int GetTargetBuild();
    static bool GetTargetVersion(unsigned int& nMajor, unsigned int& nMinor, unsigned int& nBuild);
    // 例 "D:\"；未激活返回空串
    static CDuiString GetImageRoot();

    // 逻辑注册表路径 → 离线挂载路径；未激活或无法定向（如 HKLM\HARDWARE）
    // 返回原文。bWow32=true 时 SOFTWARE 映射到 Wow6432Node。
    static CDuiString RemapPath(const wchar_t* pszFull, bool bWow32 = false);

    // "HKLM\Sub\Key" → (HKEY 根, 子键)；内部首步先做离线重映射
    // （含 HKEY_USERS\DEFAULT 无点别名归一化，与原 CDataEngine 版逐字一致）。
    static bool ParseRegPath(const wchar_t* pszFull, HKEY& hRoot, CDuiString& strSubKey);
};
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
