// ============================================================================
//  DismCore_SelfTest_Runtime.cpp —— R132/R133 运行期服务层自检
//
//  背景：R126-R131 修的四处「失败路径」最初只在编译层验证过。
//       真机 UI 点击无法无人值守完成，但其中一部分是**逻辑正确性**而非界面
//       表现 —— 这部分可以无头验证，本文件就是这部分。
//
//  覆盖：
//    1. 挂载台账：隔离子键上的真实写入 / 往返一致 / MRU 去重 / 清理无残留
//    2. 关联服务：参数校验分支的返回码（不写 HKCR）
//    3. 清理引擎：空输入与不存在路径的行为（不删真实文件）
//
//  严格隔离（R132 教训）：
//    第一版曾用 AddImageFile 写生产键 HKCU / Software / Dism++ / ImageFiles,
//    把 9 条 "dismtool_selftest_*.wim" 写进了用户真实的「最近打开的映像」列表。
//    R133 给服务加了 *_Ex(subKey) 变体，本文件一律只写隔离键，
//    并在结束时整棵删除、回读确认不存在。
//
//  明确不做：
//    * 崩溃自重启（会真的重启进程，自检台里跑等于自我中断）
//    * UI 点击路径（需真实桌面会话）
// ============================================================================
#include "stdafx.h"

#include "DismCore_SelfTest_Runtime.h"

#include "AssocService.h"
#include "CleanExtend.h"
#include "FormatW.h"          // wfmt::FormatInto（仓内统一格式化封装）
#include "MountLedgerService.h"

#include <string>
#include <vector>

namespace
{
    // [R133] 自检专用 HKCU 根（与生产键 Dism++ 完全隔离）
    const wchar_t* kSelfTestKey = L"Software\\Dism++_SelfTest_R133";

    // 清理 %TEMP% 下 dismtool_selftest_<pid> 目录
    void CleanTempDir(const std::wstring& sDir)
    {
        const std::wstring sAll = sDir + L"\\*";
        WIN32_FIND_DATAW fd = {};
        HANDLE hFind = ::FindFirstFileW(sAll.c_str(), &fd);
        if (hFind != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                ::DeleteFileW((sDir + L"\\" + fd.cFileName).c_str());
            } while (::FindNextFileW(hFind, &fd));
            ::FindClose(hFind);
        }
        ::RemoveDirectoryW(sDir.c_str());
    }

    std::wstring MakeTempDir()
    {
        wchar_t szTmp[MAX_PATH] = {};
        if (::GetTempPathW(MAX_PATH, szTmp) == 0) return std::wstring();
        std::wstring sDir = szTmp;
        if (!sDir.empty() && sDir.back() == L'\\') sDir.pop_back();

        wchar_t szName[64] = {};
        wfmt::FormatInto(szName, _countof(szName), L"\\dismtool_selftest_%u",
                         static_cast<unsigned>(::GetCurrentProcessId()));
        sDir += szName;
        ::CreateDirectoryW(sDir.c_str(), nullptr);
        return sDir;
    }

    bool Contains(const std::vector<std::wstring>& v, const std::wstring& x)
    {
        for (size_t i = 0; i < v.size(); ++i)
        {
            if (v[i] == x) return true;
        }
        return false;
    }

    // 台账用例：隔离子键上的真实往返。独立函数便于整体核对，不夹在主流程里。
    // [R136] 假写入函数：总是失败。用来验证 bAllWritten 的失败分支。
    LONG AlwaysFailSetValue(HKEY, const wchar_t*, const wchar_t*)
    {
        return ERROR_ACCESS_DENIED;   // 模拟"键不可写"这类真实故障
    }

    // [R137] 混合成败注入：第 1 次调用成功、之后全部失败。
    //   用来验证 bAllWritten 在「部分写入成功 + 部分失败」时仍能正确置 false。
    //   计数器放在函数外，便于自检每轮复位（静态局部变量无法从外部清零）。
    int g_nFromSecondCalls = 0;

    LONG FromSecondFailSetValue(HKEY, const wchar_t*, const wchar_t*)
    {
        ++g_nFromSecondCalls;
        return (g_nFromSecondCalls == 1) ? ERROR_SUCCESS : ERROR_ACCESS_DENIED;
    }


    int RunLedgerCases(void* pCtx, SelfTestChkFn pChk)
    {
        int n = 0;
        const std::wstring sSub = std::wstring(kSelfTestKey) + L"\\ImageFiles";

        // 清理隔离键：键不存在时返回失败属正常，故刻意不检查返回值。
        ::RegDeleteTreeW(HKEY_CURRENT_USER, sSub.c_str());

        std::vector<std::wstring> v0;
        DismLedger::GetImageFilesEx(sSub, v0);
        pChk(pCtx, v0.empty(), "runtime: 隔离子键初始为空（前置条件成立）");
        ++n;

        pChk(pCtx, !DismLedger::AddImageFileEx(sSub, std::wstring()),
             "runtime: Ex 版拒绝空路径（不写注册表）");
        ++n;

        pChk(pCtx, !DismLedger::AddImageFileEx(std::wstring(), L"x.wim"),
             "runtime: Ex 版拒绝空子键（防误写 HKCU 根）");
        ++n;

        // 成功写入：R133 补回的「成功分支」。
        //   验证 R128 的 bAllWritten 在正常写入时不会被误判为失败。
        const std::wstring sProbe = L"C:/__dismtool_selftest__/probe.wim";
        const bool bAdd = DismLedger::AddImageFileEx(sSub, sProbe);
        pChk(pCtx, bAdd,
             "runtime: Ex 版正常写入返回 true（R128 bAllWritten 成功路径不误判）");
        ++n;

        // 往返一致：不假设 v1[0] —— ReadMulti 用 RegEnumValueW 枚举，
        //   返回顺序按值名排序，新写入的值不一定在第一位。
        std::vector<std::wstring> v1;
        const bool bGet1 = DismLedger::GetImageFilesEx(sSub, v1);
        pChk(pCtx, bGet1 && Contains(v1, sProbe),
             "runtime: 写入值出现在读回集合中（往返一致）");
        ++n;

        const size_t nBefore = v1.size();
        DismLedger::AddImageFileEx(sSub, sProbe);
        std::vector<std::wstring> v2;
        DismLedger::GetImageFilesEx(sSub, v2);
        pChk(pCtx, v2.size() == nBefore,
             "runtime: 重复写入不增加条目（MRU 去重生效）");
        ++n;

        // 清理隔离键并确认读不回。刻意不检查返回值：键不存在时返回失败属正常，
        //   真正的判定在下面一行回读断言里。
        ::RegDeleteTreeW(HKEY_CURRENT_USER, sSub.c_str());
        std::vector<std::wstring> v3;
        const bool bGet3 = DismLedger::GetImageFilesEx(sSub, v3);
        pChk(pCtx, !bGet3 && v3.empty(),
             "runtime: 隔离子键已删除且读不回（无残留）");
        ++n;

        // [R136] 失败分支：注入「写入必失败」，验证 bAllWritten 真的会置 false。
        //   这是 R133/R135 遗留的技术债 —— 成功路径早已验证，失败路径一直没证据。
        {
            std::vector<std::wstring> vB;
            const bool bFailAdd = DismLedger::AddImageFileEx2(
                sSub, L"C:/__dismtool_selftest__/fail.wim", &AlwaysFailSetValue);
            pChk(pCtx, !bFailAdd,
                 "runtime: 写入必失败时 AddImageFileEx2 返回 false（bAllWritten 失败分支）");
            ++n;

            // 且不得留下任何值（失败时不应产生半写状态）
            DismLedger::GetImageFilesEx(sSub, vB);
            pChk(pCtx, vB.empty(),
                 "runtime: 写入失败后无残留值（不产生半写状态）");
            ++n;

        // [R137] 混合成败：先有 1 条旧值，再写新值使 out 有 2 条，
        //   第 2 条写入失败 -> 整体应报 false。
        //   为什么必须预先有条目：空键时 out 只有新值 1 条，循环只跑 1 次，
        //   "第 2 次失败"根本不会触发（这是第一次写用例时的错误假设）。
        {
            // 清理隔离键：失败不影响结论，故刻意不检查返回值。
            ::RegDeleteTreeW(HKEY_CURRENT_USER, sSub.c_str());

            // 先正常写入一条，建立"已有条目"的前置状态
            DismLedger::AddImageFileEx(sSub, L"C:/__dismtool_selftest__/old.wim");

            // 再写一条新值：out = [新值, 旧值]，第 1 次写入成功、第 2 次失败
            g_nFromSecondCalls = 0;
            const bool bMix = DismLedger::AddImageFileEx2(
                sSub, L"C:/__dismtool_selftest__/mix.wim", &FromSecondFailSetValue);
            pChk(pCtx, !bMix,
                 "runtime: 混合成败时返回 false（部分失败即整体失败）");
            ++n;

            // 已写入成功的那条不会被回滚（当前实现语义）：这是**已知取舍**，
            //   不是缺陷 —— 台账是"尽力而为"的 MRU 记录，回滚反而可能删掉
            //   用户原本就在的条目。此处断言该语义，防止将来被误改。
            std::vector<std::wstring> vMix;
            DismLedger::GetImageFilesEx(sSub, vMix);
            pChk(pCtx, !vMix.empty(),
                 "runtime: 混合失败时不回滚已写入条目（尽力而为语义，非缺陷）");
            ++n;

            // 清理隔离键：失败不影响结论，故刻意不检查返回值。
            ::RegDeleteTreeW(HKEY_CURRENT_USER, sSub.c_str());
        }
        }

        {   // 复原：清掉子键，后续收尾断言仍成立
            // 清理隔离键：键不存在时返回失败属正常，故刻意不检查返回值。
            ::RegDeleteTreeW(HKEY_CURRENT_USER, sSub.c_str());
        }

        return n;
    }
}   // namespace



int SelfTestRuntimeServices(void* pCtx, SelfTestChkFn pChk)
{
    if (!pChk) return 0;
    int nItems = 0;

    // ---- 0) 临时目录 ----
    const std::wstring sTempDir = MakeTempDir();
    pChk(pCtx, !sTempDir.empty(),
         "runtime: 临时目录可创建（隔离于用户数据）");
    ++nItems;

    // ---- 1) 挂载台账：隔离子键上的真实往返 ----
    nItems += RunLedgerCases(pCtx, pChk);

    // ---- 2) 关联服务：参数校验路径（不写 HKCR）----
    //   这些分支在写任何注册表之前就返回，属于纯逻辑判定，可安全验证。
    {
        const EAssocWriteResult rEmpty =
            AssocService::SetUserFileAssociation(L"", L"");
        pChk(pCtx, rEmpty != EAssocWriteResult::ASSOC_WRITE_OK,
             "runtime: 空参数被拒绝（不返回 OK）");
        ++nItems;

        const EAssocWriteResult rBadProgId =
            AssocService::SetUserFileAssociation(L".selftest", L"no-backslash");
        pChk(pCtx, rBadProgId == EAssocWriteResult::ASSOC_WRITE_INVALID_PROGID,
             "runtime: ProgId 缺少反斜杠被判为 INVALID_PROGID");
        ++nItems;
    }

    // ---- 3) 清理引擎：空输入与不存在路径（不删任何真实文件）----
    {
        // CCleanExtendEngine 不是单例（由 CFrameWnd 持有的值成员）。
        // 自检里就地构造局部实例：构造函数只初始化容器，无副作用。
        CCleanExtendEngine eng;

        std::vector<CDuiString> vEmpty;
        int nFailed = -1;
        const int nEmpty = eng.DeleteToRecycleBin(vEmpty, nFailed);
        pChk(pCtx, nEmpty == 0 && nFailed == 0,
             "runtime: DeleteToRecycleBin 空输入返回 0 且失败计数归零");
        ++nItems;

        // 不存在的路径：引擎会先过滤（GetFileAttributes 判不存在），
        //   既不进 SHFileOperation、也不计失败 —— 这是设计如此的幂等语义。
        std::vector<CDuiString> vMissing;
        vMissing.push_back(CDuiString(L"C:/__dismtool_never_exists__/probe.bin"));
        int nFailed2 = -1;
        const int nOk2 = eng.DeleteToRecycleBin(vMissing, nFailed2);
        pChk(pCtx, nFailed2 == 0 && nOk2 == 0,
             "runtime: 不存在的路径被前置过滤（不计失败，幂等语义）");
        ++nItems;
    }

    // ---- 4) 收尾：清理隔离环境 ----
    // 清理隔离键：失败不影响结论（下面有回读断言兜底），故刻意不检查返回值。
    ::RegDeleteTreeW(HKEY_CURRENT_USER, kSelfTestKey);
    if (!sTempDir.empty()) CleanTempDir(sTempDir);

    pChk(pCtx, !sTempDir.empty() &&
               ::GetFileAttributesW(sTempDir.c_str()) == INVALID_FILE_ATTRIBUTES,
         "runtime: 自检结束后临时目录已删除（无残留）");
    ++nItems;

    HKEY hLeft = nullptr;
    const LONG lLeft = ::RegOpenKeyExW(HKEY_CURRENT_USER, kSelfTestKey, 0,
                                       KEY_READ, &hLeft);
    if (hLeft) ::RegCloseKey(hLeft);
    pChk(pCtx, lLeft == ERROR_FILE_NOT_FOUND || lLeft == ERROR_PATH_NOT_FOUND,
         "runtime: 自检结束后隔离键已删除（无残留）");
    ++nItems;

    return nItems;
}

// [C++17 R3] 本文件内联常量（C++17 inline variables，多 TU ODR 一致）：
