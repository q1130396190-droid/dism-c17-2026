// ============================================================================
//  ncleaner_util.h
//
//  NCleaner 7 个导出的共用工具：
//    * MSI 事务互斥量守卫（Global\_MSIExecute）—— Installer / PackageCache /
//      VSPackageCache 三处防护（知识库《52_B》2.4：旧记录仅 Installer 一处，已修正）
//    * 递归删除目录（等价 NCDeleteFile 原语的目录形态）
//    * 按过滤串匹配文件名/目录名
//    * 结果统计结构（与原版返回值语义对齐：S_OK 表示成功）
// ============================================================================

#pragma once

#include "ncleaner_stdafx.h"

namespace NCleanerUtil
{

// ---- MSI 事务互斥量守卫 ----------------------------------------------------
// 原版在清理 Installer / Package Cache / VS Packages 前先检测 Global\_MSIExecute：
// 若 MSI 正在执行安装事务则跳过本次清理，避免破坏进行中的安装。
// [BUG-19] 语义：构造时单次 CreateMutex + 零等待探测，只检测、不持有句柄；
// IsBusy()=true 表示有 MSI 事务在跑（检测与清理之间存在天然 TOCTOU 窗口）。
class CMsiGuard
{
public:
    CMsiGuard();
    ~CMsiGuard();

    [[nodiscard]] bool IsBusy() const { return m_bBusy; }

private:
    bool   m_bBusy;

    CMsiGuard(const CMsiGuard&);
    CMsiGuard& operator=(const CMsiGuard&);
};

// ---- 递归删除 --------------------------------------------------------------
// 删除目录树（含只读清除）。bDeleteRoot=true 时连根目录一起删。
// 返回实际删除的文件+目录数。
unsigned long DeleteTree(const wchar_t* pszDir, bool bDeleteRoot);

// 删除单个文件（清除只读属性后删除）。
// 返回 true 表示已删除或本来就不存在。
bool DeleteFileForce(const wchar_t* pszFile);

// ---- 匹配 ------------------------------------------------------------------
// 名称是否包含任一过滤串（大小写不敏感）。
bool NameMatchesAny(const wchar_t* pszName, const wchar_t* const* ppFilters, int nCount);

// ---- 目录展开 --------------------------------------------------------------
// 把含环境变量的路径展开为绝对路径（如 %ProgramData%\Package Cache）。
bool ExpandPath(const wchar_t* pszIn, std::wstring& sOut);

// 取系统盘根（如 "C:\"）。
[[nodiscard]] bool GetSystemDriveRoot(std::wstring& sOut);

// 取 %SystemRoot%（如 "C:\Windows"）。
[[nodiscard]] bool GetSystemRoot(std::wstring& sOut);

// ---- 日志 ------------------------------------------------------------------
// [C-05 修复] 依赖反转：util 是最底层，不得反向 include 宿主解析层。
// 宿主层（ncleaner_host）初始化时通过 SetLogSink 注册实际日志通道；
// 未注册时 Log 退化为 OutputDebugStringW，不静默丢弃。
using LogSinkFn = void (*)(int nLevel, const wchar_t* pText);
void SetLogSink(LogSinkFn pfnSink);

// 按原版模板写日志（等价 DismWriteLog）。
void Log(const wchar_t* pFormat, ...);

// ---- 安全加载（DLL 抗劫持）-------------------------------------------------
// 官方 ReadMe 权威行为：
//   NCleaner 1.0.3.8  「添加 dll 抗劫持功能」
//   NCleaner 1.0.3.10 「降低某些情况下潜在 dll 抗劫风险」
//
// 实现：只从 %SystemRoot%\System32 按完整路径加载，绝不做裸名回退
// （裸名会先搜索应用目录，正是 DLL 劫持的入口）。失败即返回 nullptr，
// 由调用方按"模块不可用"处理，不静默降级到不安全路径。
HMODULE SafeLoadSystem32Dll(const wchar_t* pszName);

}  // namespace NCleanerUtil
