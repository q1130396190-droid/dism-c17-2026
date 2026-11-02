#pragma once

#include <atomic>   // [G11] 跨线程取消标志

// ============================================================
// CleanExtend.h —— [R80] 深度扫描 / 卸载残留 / 空文件夹 / 撤销
//
// 背景：Page_Cleanup 现有 73 规则主要覆盖临时文件/日志/系统缓存，
// 评分报告指出四类缺口：
//   * DeepScan        大文件扫描（FindBigFile / FindLargeFile）
//   * OrphanScan      卸载残留扫描（UninstallResid / FindOrphan）
//   * EmptyFolder     空文件夹扫描（FindEmpty / EmptyDir）
//   * CleanUndo       撤销栈（回收站方式保留 + 逐项还原）
//
// 设计原则：
//   * 独立服务层（不并入 CCleanEngine），避免破坏已验证的 73 规则引擎
//   * 全部走 SHFileOperation(FO_DELETE) 默认 FOF_ALLOWUNDO，删除文件进
//     回收站，可从"撤销"中还原
//   * 结果写入 CleanExtendResult 向量，UI 侧展示
//
// 依赖：DuiLib::CDuiString（Utils/Utils.h），不依赖 UI 控件。
// ============================================================

#include <vector>
#include <windows.h>
#include "Utils/Utils.h"

// 大文件扫描结果
struct CleanExtendBigFile
{
    CDuiString  sPath;
    ULONGLONG   ullSize;
    FILETIME    ftLastWrite;
};

// 卸载残留结果
struct CleanExtendOrphan
{
    CDuiString  sUninstallKey;  // 卸载注册表路径（HKLM\...\Uninstall\...）
    CDuiString  sDisplayName;
    CDuiString  sInstallPath;   // 已不存在的安装路径
};

// 空文件夹结果
struct CleanExtendEmptyDir
{
    CDuiString  sPath;
};

// 撤销条目（删除前快照）
struct CleanExtendUndoEntry
{
    CDuiString  sPath;
    ULONGLONG   ullSize;       // 删除前大小
    bool        bDir;
    FILETIME    ftDeleted;     // 删除时间
};

class CCleanExtendEngine
{
public:
    CCleanExtendEngine();
    ~CCleanExtendEngine();

    // ---- 1. 大文件扫描 ----
    // 在给定根目录（默认 %SystemDrive%）递归枚举大于 nMinMB 的文件。
    // 受保护目录跳过（Windows/Program Files/... 排除由 bSkipSystemDirs 控制）。
    // 返回枚举文件数；out 追加结果。UI 线程调用（可能耗时，建议走 worker）。
    int DeepScanBigFiles(const CDuiString& sRoot, ULONGLONG nMinBytes,
                         bool bSkipSystemDirs,
                         std::vector<CleanExtendBigFile>& out);

    // [G11 2026-10-25] 扫描取消。
    //   原先增强扫描（大文件/空文件夹）只能等它跑完：全盘遍历在 HDD 上可达数分钟，
    //   期间 UI 只有"扫描中"，用户只能干等或杀进程。这里加原子取消标志，
    //   递归枚举每进一个目录就查一次，命中即尽快返回已扫到的部分结果。
    //   注意：取消是"尽快停止"，不是"立即停止"——单层目录内仍需跑完
    //   当前 FindFirstFile 循环，这与 DISM 取消的语义一致，UI 需如实表达。
    void RequestCancel() { m_bCancel.store(true, std::memory_order_relaxed); }
    void ResetCancel()   { m_bCancel.store(false, std::memory_order_relaxed); }
    [[nodiscard]] bool IsCancelled() const { return m_bCancel.load(std::memory_order_relaxed); }

    // ---- 2. 卸载残留扫描 ----
    // 遍历 HKLM/HKCU/HKU 的 Uninstall 子键（64/32 位视图），
    // 凡 InstallLocation 或 UninstallString 指向的路径已不存在者计为残留。
    int OrphanScanUninstall(std::vector<CleanExtendOrphan>& out);

    // ---- 3. 空文件夹扫描 ----
    // 在给定根目录下递归寻找"自身不含文件、且子目录均为空"的目录。
    // 结果按路径深度降序返回（先删最深）。
    int EmptyFolderScan(const CDuiString& sRoot,
                        std::vector<CleanExtendEmptyDir>& out);

    // ---- 4. 清理执行（全部进回收站，可撤销）----
    // 传入要删除的路径列表（文件+目录），返回成功条数与失败数。
    int DeleteToRecycleBin(const std::vector<CDuiString>& vPaths,
                           int& nFailedPaths);

    // ---- 5. 撤销栈 ----
    // 每轮清理后 push 到撤销栈（最多 nMax 条）
    void PushUndo(const std::vector<CleanExtendUndoEntry>& vEntries,
                  int nMax = 20);
    // 撤销最近一次：把栈顶所有条目还原（从回收站恢复）
    // 返回还原成功条数；空栈返回 0。
    int UndoLast(std::vector<CDuiString>& outErrors);
    int UndoDepth() const { return static_cast<int>(m_vUndoStack.size()); }

private:
    // 递归枚举（大文件）
    static int EnumBigFiles(const CDuiString& sDir, ULONGLONG nMinBytes,
                            bool bSkipSystemDirs,
                            const std::vector<CDuiString>& vSkipDirs,
                            int nDepth,
                            std::vector<CleanExtendBigFile>& out);
    // 递归判定"是否为空文件夹"（内部，nDepth 防栈失控）
    static bool IsEmptyFolder(const CDuiString& sPath, int nDepth);
    // 收集空文件夹（含更深层；nDepth 深度上限，vSkip 保护目录名）
    static int CollectEmptyFolders(const CDuiString& sDir,
                                   std::vector<CleanExtendEmptyDir>& out,
                                   const std::vector<CDuiString>& vSkip,
                                   int nDepth);

    // 从注册表 Uninstall 区读取一个键的字符串值
    static bool ReadRegString(HKEY hRoot, const wchar_t* sSubKey, const wchar_t* sValueName,
                              CDuiString& out);
    static void ScanUninstallRoot(HKEY hRoot, const wchar_t* sSubPath,
                                  std::vector<CleanExtendOrphan>& out);

    // 递归遍历 Uninstall 子键（支持 Wow6432Node）
    static void ScanUninstallKey(HKEY hKey, CDuiString& sPath,
                                 std::vector<CleanExtendOrphan>& out);

    // 撤销栈：vector<vector>，每层是一次清理的快照
    std::vector<std::vector<CleanExtendUndoEntry> > m_vUndoStack;

    // 系统目录前缀集合（跳过）
    std::vector<CDuiString> m_vSkipDirs;

    // [G11] 取消标志：递归枚举的检查点读它，UI 线程置位。
    //   用 atomic 而非 bool：跨线程读写，普通 bool 属数据竞争（UB）。
    //   必须是 static：EnumBigFiles / CollectEmptyFolders 都是静态递归函数，
    //   静态函数不能访问非静态成员（首次尝试就栽在 C2228）。
    //   引擎全进程只有一个实例（由 IPageContext::CleanExtendEngine() 持有），
    //   静态化不引入"多实例互相干扰"的问题。
    static inline std::atomic<bool> m_bCancel{ false };
};
// [C++17 R3] 头文件级内联常量（C++17 inline variables，ODR 安全）：
