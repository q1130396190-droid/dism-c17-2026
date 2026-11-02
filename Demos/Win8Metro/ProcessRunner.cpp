#include "ProcessRunner.h"
#include "FormatW.h"
#include "SafeThread.h"          // 嵌套消息泵异常边界
#include "ExternalCmdLedger.h"  // [R-B56] 外部命令防腐台账
#include <vector>
#include <string>
#include <wintrust.h>
#include <softpub.h>
#include <wincrypt.h>
#include <array>   // [C++11] std::array（F13 char[N] 清零）
// [C++17 升级阶段2 说明] 本文件 20 处句柄释放（pi.hThread/pi.hProcess/hWr/
// hRd/hReadPipe/hWritePipe/hW/hR）有意保留显式管理、不迁移 RAII：
// 1) hWr/hW 必须在 CreateProcess 后【立即】关闭、hRd/hR 必须在子进程退出
//    后按固定顺序关闭——该时序与 R-B83/R-B84 防死锁修复（4KB 管道写满
//    导致 dism 永久阻塞）直接相关；RAII 守卫的析构时机在函数尾，会推迟
//    关闭点、改变调用顺序，违反"只替换配对释放、不改时序"的硬约束。
// 2) PROCESS_INFORMATION 均为单出口配对关闭（CreateProcess 成功分支内），
//    无早退泄漏路径，RAII 无稳定性收益。
// 3) 认证上下文（WinVerifyTrust/CryptQueryObject）使用 WTD_STATEACTION_CLOSE
//    状态机收尾，非句柄释放语义，不适用 unique_ptr 删除器。


#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")
using namespace DuiLib;
// [修复 P1-SPOOF] 校验文件是否带受信任的微软 Authenticode 签名。
// ResolveSysTool 原本"exe 同目录同名优先"，管理员身份下，同目录被放入
// 伪造的 sfc/reagentc/diskpart 等同名程序即被提权执行（搜索顺序劫持）。
static bool IsMicrosoftSignedBinary(const wchar_t* pszPath)
{
    WINTRUST_FILE_INFO fi;
    memset(&fi, 0, sizeof(fi));
    fi.cbStruct = sizeof(fi);
    fi.pcwszFilePath = pszPath;
    GUID guidAction = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_DATA wd;
    memset(&wd, 0, sizeof(wd));
    wd.cbStruct = sizeof(wd);
    wd.dwUIChoice = WTD_UI_NONE;
    wd.fdwRevocationChecks = WTD_REVOKE_NONE;
    wd.dwUnionChoice = WTD_CHOICE_FILE;
    wd.pFile = &fi;
    wd.dwStateAction = WTD_STATEACTION_VERIFY;
    // 只用本地缓存做链检索，避免联网卡顿。
    wd.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_SAFER_FLAG;
    LONG lStatus = ::WinVerifyTrust(nullptr, &guidAction, &wd);
    bool bMicrosoft = false;
    if (lStatus == 0)
    {
        CRYPT_PROVIDER_DATA* pProv =
            WTHelperProvDataFromStateData(wd.hWVTStateData);
        CRYPT_PROVIDER_SGNR* pSigner =
            pProv ? WTHelperGetProvSignerFromChain(pProv, 0, FALSE, 0) : nullptr;
        PCCERT_CONTEXT pCert =
            (pSigner && pSigner->csCertChain > 0 && pSigner->pasCertChain)
                ? pSigner->pasCertChain[0].pCert : nullptr;
        if (pCert)
        {
            WCHAR szName[256] = { 0 };
            DWORD cch = ::CertGetNameStringW(pCert,
                CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, szName, 256);
            if (cch > 1)
            {
                // 微软系统程序签名者显示名形如 "Microsoft Windows" /
                // "Microsoft Corporation"。
                if (wcsstr(szName, L"Microsoft")) bMicrosoft = true;
            }
        }
    }
    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    ::WinVerifyTrust(nullptr, &guidAction, &wd);
    return bMicrosoft;
}
// 裸工具名（如 "diskpart.exe"）才允许 ResolveSysTool 做目录解析；
// 已含盘符/路径/引号/参数串的调用视为完整命令，原样透传。
static bool IsBareToolName(const wchar_t* psz)
{
    if (!psz || !*psz) return false;
    return !wcschr(psz, L':') && !wcschr(psz, L'\\') &&
           !wcschr(psz, L'/') && !wcschr(psz, L'"') &&
           !wcschr(psz, L' ');
}
// run %System32%\<exe> hidden and wait briefly (no output captured)
void ProcessRunner::RunHidden(const wchar_t* pszExe, const CDuiString& sArgs, DWORD dwWaitMs)
{
    // [R-B56] 外部命令防腐台账：集中登记所有外部进程调用
    ExternalCmdLedger::Note(pszExe, sArgs.GetData());
    wchar_t szSys[MAX_PATH] = { 0 };
    ::GetSystemDirectory(szSys, MAX_PATH);
    CDuiString sCmd;
    sCmd.Format(L"\"%s\\%s\" %s", szSys, pszExe, sArgs.GetData());
    std::vector<wchar_t> vBuf(sCmd.GetLength() + 3, 0);
    wfmt::FormatInto(&vBuf[0], vBuf.size(), L"%s", sCmd.GetData());



    STARTUPINFO si; memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi; memset(&pi, 0, sizeof(pi));
    if (::CreateProcess(nullptr, &vBuf[0], nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
    {
        ::WaitForSingleObject(pi.hProcess, dwWaitMs);
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
    }
}

// dism.exe 组件内置：优先使用 exe 同目录下的同名工具（bin\dism.exe），
// 不存在时回退 %System32%（绿色部署/离线机场景可用）。
CDuiString ProcessRunner::ResolveSysTool(const wchar_t* pszExe)
{
    wchar_t szExe[MAX_PATH] = { 0 };
    ::GetModuleFileName(nullptr, szExe, MAX_PATH);
    const wchar_t* pSlash = wcsrchr(szExe, L'\\');
    if (pSlash)
    {
        // 随包 ADK 工具统一存放在主程序目录下 amd64\（如 bcdboot.exe），
        // 其次才是主程序同目录；两者都必须通过微软签名校验（P1-SPOOF）。
        const wchar_t* kRel[] = { L"\\amd64\\", L"\\" };
        for (int i = 0; i < 2; ++i)
        {
            CDuiString sLocal;
            sLocal.Format(L"%.*s%s%s", static_cast<int>(pSlash - szExe), szExe,
                          kRel[i], pszExe);
            // [修复 P1-SPOOF] 随包副本只有通过微软签名校验才优先使用，
            // 否则回退受保护的 System32，杜绝同目录同名程序提权劫持。
            if (::GetFileAttributes(sLocal.GetData()) != INVALID_FILE_ATTRIBUTES &&
                IsMicrosoftSignedBinary(sLocal.GetData()))
                return sLocal;
        }
    }
    wchar_t szSys[MAX_PATH] = { 0 };
    ::GetSystemDirectory(szSys, MAX_PATH);
    CDuiString sSys;
    sSys.Format(L"%s\\%s", szSys, pszExe);
    return sSys;
}

// [FIX-P1H] RunHiddenExit / WaitHandleWithUiPump 共用的白名单派发表
//（原 RunHiddenExit 函数内 static，提升至文件级供两处共享）。
static constexpr UINT kPumpDispatchWhitelist[] = {
    WM_PAINT, WM_NCPAINT, WM_ERASEBKGND,
    WM_ACTIVATE, WM_NCACTIVATE, WM_ACTIVATEAPP,
    WM_WINDOWPOSCHANGED, WM_SETTINGCHANGE, WM_THEMECHANGED,
    WM_DISPLAYCHANGE, WM_DWMCOLORIZATIONCOLORCHANGED,
    WM_QUERYENDSESSION,
};

// [FIX-P1H] UI 线程白名单泵等待单个内核对象（hObject 可为 nullptr=纯定时泵）。
// 契约与 RunHiddenExit 泵完全同源：绘制/激活立即派发；业务消息与 WM_TIMER
// 暂存回投；输入/命令丢弃；WM_QUIT 转投退出并提前返回 WAIT_TIMEOUT。
DWORD ProcessRunner::WaitHandleWithUiPump(HANDLE hObject, DWORD dwTimeoutMs)
{
    DWORD dwElapsed = 0;
    constexpr DWORD DW_TICK = 200;
    HANDLE hWait = hObject;   // nullptr 时 MsgWaitForMultipleObjects 0 对象纯等输入
    std::vector<MSG> vRepost;
    bool bQuit = false;
    DWORD dwRet = WAIT_TIMEOUT;
    for (;;)
    {
        DWORD dwRes = ::MsgWaitForMultipleObjects(
            hObject ? 1 : 0, hObject ? &hWait : nullptr, FALSE, DW_TICK, QS_ALLINPUT);
        if (hObject && dwRes == WAIT_OBJECT_0) { dwRet = WAIT_OBJECT_0; break; }
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                ::PostQuitMessage((int)msg.wParam);
                bQuit = true;
                break;
            }
            const bool bBusiness =
                (msg.message >= WM_USER && msg.message < WM_USER + 0x200)
                || (msg.message >= WM_APP && msg.message < WM_APP + 0x8000)
                || (msg.message == WM_TIMER);
            if (bBusiness) { vRepost.push_back(msg); continue; }
            bool bSafe = false;
            for (UINT u : kPumpDispatchWhitelist)
                if (u == msg.message) { bSafe = true; break; }
            if (bSafe)
            {
                ::TranslateMessage(&msg);
                SafeDispatchMessage(&msg);
            }
            // else: 输入/命令/通知 → 丢弃，绝不派发（重入面）
        }
        if (bQuit) break;
        dwElapsed += DW_TICK;
        if (dwElapsed >= dwTimeoutMs) break;
    }
    for (const MSG& m : vRepost)
        ::PostMessageW(m.hwnd, m.message, m.wParam, m.lParam);
    return dwRet;
}

// 隐藏运行 %System32%\<exe>，等待结束并返回退出码；
// CreateProcess 失败返回 (DWORD)-1；超时返回时进程仍在运行（可能为 259）
DWORD ProcessRunner::RunHiddenExit(const wchar_t* pszExe, const CDuiString& sArgs,
                           DWORD dwWaitMs, bool bKillOnTimeout)
{
    // [R-B56] 外部命令防腐台账
    ExternalCmdLedger::Note(pszExe, sArgs.GetData());
    CDuiString sCmd;
    // [修复 P1-CMD] 裸工具名才解析目录并加引号；完整命令行（含路径/引号/
    // 参数，如更新阶段自身 exe 拉起）原样透传，避免拼接损坏导致起不来。
    if (IsBareToolName(pszExe))
        sCmd.Format(L"\"%s\" %s", ResolveSysTool(pszExe).GetData(), sArgs.GetData());
    else
    {
        sCmd = pszExe;
        if (!sArgs.IsEmpty()) { sCmd += L' '; sCmd += sArgs; }
    }
    std::vector<wchar_t> vBuf(sCmd.GetLength() + 3, 0);
    wfmt::FormatInto(&vBuf[0], vBuf.size(), L"%s", sCmd.GetData());



    STARTUPINFO si; memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi; memset(&pi, 0, sizeof(pi));
    if (!::CreateProcess(nullptr, &vBuf[0], nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return (DWORD)-1;



    // 带消息泵的等待：子进程最长可能跑 10 分钟（compact/ResetBase），
    // 直接 WaitForSingleObject 会让窗口整条消息循环停摆（界面"冻死"、
    // 连重绘都不响应）。MsgWaitForMultipleObjects 每拍抽空派发窗口消息。
    // [INTERACT-FIX] 原实现在泵内派发"全部"消息：WM_TIMER(切页构建)/
    // WM_COMMAND(按钮)/WM_LBUTTONDOWN 会在点击栈中途重入另一条点击/构建/
    // 校验栈——与 PumpUiMessages 已修复的重入 UAF 面（B3/R15）同型，且本
    // 函数被菜单/更新阶段/向导多处从 UI 线程调用（等待最长 300s）。
    // 现采用与 PumpUiMessages/SettleAfterDialog 同源一致的白名单契约：
    //   * 绘制/激活/外观消息 → 立即派发（与控件生命周期无关）；
    //   * 业务消息（WM_USER/WM_APP 族）与 WM_TIMER → 收集，等待结束后
    //     按原序回投（业务消息携带堆对象所有权不可丢；一次性定时器回投
    //     保留触发语义，如 PAGE_BUILD 丢弃会导致页面永远空白）；
    //   * 其余输入/命令/通知 → 丢弃（陈旧输入是崩溃面，用户可重新点击）；
    //   * WM_QUIT → 转投退出请求并结束等待。
    DWORD dwElapsed = 0;
    constexpr DWORD DW_TICK = 200;
    std::vector<MSG> vRepost;
    bool bQuit = false;
    for (;;)
    {
        DWORD dwRes = ::MsgWaitForMultipleObjects(
            1, &pi.hProcess, FALSE, DW_TICK, QS_ALLINPUT);
        if (dwRes == WAIT_OBJECT_0) break;
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                ::PostQuitMessage((int)msg.wParam);
                bQuit = true;
                break;
            }
            const bool bBusiness =
                (msg.message >= WM_USER && msg.message < WM_USER + 0x200)
                || (msg.message >= WM_APP && msg.message < WM_APP + 0x8000)
                || (msg.message == WM_TIMER);
            if (bBusiness) { vRepost.push_back(msg); continue; }
            bool bSafe = false;
            for (UINT u : kPumpDispatchWhitelist)
                if (u == msg.message) { bSafe = true; break; }
            if (bSafe)
            {
                ::TranslateMessage(&msg);
                SafeDispatchMessage(&msg);
            }
            // else: 输入/命令/通知 → 丢弃，绝不派发（重入面）
        }
        if (bQuit) break;
        dwElapsed += DW_TICK;
        if (dwElapsed >= dwWaitMs) break;   // 超时：进程仍在则退出码取到 STILL_ACTIVE
    }
    for (const MSG& m : vRepost)
        ::PostMessageW(m.hwnd, m.message, m.wParam, m.lParam);
    // [修复 P1-ORPHAN] 超时后旧代码直接关闭句柄，子进程仍在后台运行
    // （孤儿进程，且可能持有挂载/文件锁）。超时明确终止。
    // [FIX-P1H] 超时分级：关键引导类操作（bcdboot/bootsect 等调用方显式
    // 传 bKillOnTimeout=false）超时只告警不杀 —— 中途强杀可能留下半写
    // BCD/半完成 CBS 事务，正确性风险高于孤儿进程。
    if (::WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT)
    {
        if (bKillOnTimeout)
            ::TerminateProcess(pi.hProcess, 1);
        else
            ::OutputDebugStringW(L"[ProcessRunner] timeout, kill skipped (bKillOnTimeout=false)\r\n");
    }
    DWORD dwExit = 0;
    ::GetExitCodeProcess(pi.hProcess, &dwExit);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return dwExit;
}

// run %System32%\<exe> hidden, capture combined stdout/stderr (ANSI → wchar_t)
// 参考 Qt6 Engine/Backup.cpp::runCmdSync：
//   - 管道异步读（PeekNamedPipe + ReadFile 循环，避免管道填满死锁）
//   - 超时后 TerminateProcess 子进程，防止僵尸
//   - 可选 pdwExit 输出退出码（nullptr 时忽略）
bool ProcessRunner::RunCapture(const wchar_t* pszExe, const CDuiString& sArgs,
                       CDuiString& sOut, DWORD dwWaitMs,
                       DWORD* pdwExit)
{
    // [R-B56] 外部命令防腐台账
    ExternalCmdLedger::Note(pszExe, sArgs.GetData());


    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = nullptr;
    sa.bInheritHandle = TRUE;
    HANDLE hRd = nullptr, hWr = nullptr;
    if (!::CreatePipe(&hRd, &hWr, &sa, 0)) return false;
    ::SetHandleInformation(hRd, HANDLE_FLAG_INHERIT, 0);



    STARTUPINFO si; memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hWr;
    si.hStdError = hWr;
    si.hStdInput = nullptr;
    PROCESS_INFORMATION pi; memset(&pi, 0, sizeof(pi));



    CDuiString sCmd;
    // [修复 P1-CMD] 裸工具名才解析；完整命令行原样透传。
    if (IsBareToolName(pszExe))
        sCmd.Format(L"\"%s\" %s", ResolveSysTool(pszExe).GetData(), sArgs.GetData());
    else
    {
        sCmd = pszExe;
        if (!sArgs.IsEmpty()) { sCmd += L' '; sCmd += sArgs; }
    }
    std::vector<wchar_t> vBuf(sCmd.GetLength() + 3, 0);
    wfmt::FormatInto(&vBuf[0], vBuf.size(), L"%s", sCmd.GetData());



    BOOL bOk = ::CreateProcess(nullptr, &vBuf[0], nullptr, nullptr, TRUE,
                               CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    ::CloseHandle(hWr);
    if (!bOk) { ::CloseHandle(hRd); return false; }



    // 管道读循环（参考 Qt6 runCmdSync 的 readyRead 逻辑）
    bool bTimedOut = false;
    std::string sBytes;
    // 管道读缓冲（每次最多抽取 4KB，短轮询防子进程 WriteFile 阻塞）
    constexpr size_t kPipeReadBufSize = 4096;
    std::array<char, kPipeReadBufSize> aBuf;
    DWORD dwRead = 0;
    // [R-B83] 总截止时间（GetTickCount64 无 49 天回绕问题）
    ULONGLONG ullDeadline = ::GetTickCount64() + static_cast<ULONGLONG>(dwWaitMs);
    for (;;)
    {
        DWORD dwAvail = 0;
        if (!::PeekNamedPipe(hRd, nullptr, 0, nullptr, &dwAvail, nullptr)) break;
        if (dwAvail > 0)
        {
            // 管道有数据立即抽干：子进程（如 dism）输出是"写一段→CBS/
            // DismHost 初始化→再写一段"的突发模式，停抽会让 4KB 管道写满，
            // 子进程阻塞在 WriteFile 上永不退出。
            DWORD dwToRead = dwAvail < aBuf.size() ? dwAvail : static_cast<DWORD>(aBuf.size());
            if (!::ReadFile(hRd, aBuf.data(), dwToRead, &dwRead, nullptr) || dwRead == 0) break;
            sBytes.append(aBuf.data(), dwRead);
            continue;
        }
        // 管道瞬间为空：进程已退出则结束（残留数据循环后统一抽）；
        // 否则只短睡 50ms 后继续轮询抽管道——绝不在进程句柄上长阻塞，
        // 否则等待期间无人读管道，与子进程形成写满死锁（旧代码此处直接
        // WaitForSingleObject(dwWaitMs)，导致 dism 必挂到超时、退出码 -1）。
        if (::WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) break;
        if (::GetTickCount64() >= ullDeadline)
        {
            bTimedOut = true;
            break;
        }
        ::Sleep(50);
    }



    // 超时 → 杀进程，再等它退出
    if (bTimedOut)
    {
        ::TerminateProcess(pi.hProcess, (DWORD)-1);
        ::WaitForSingleObject(pi.hProcess, 3000);
    }
    else
    {
        ::WaitForSingleObject(pi.hProcess, 5000);
    }



    // 读最后的残留管道数据
    for (;;)
    {
        DWORD dwAvail = 0;
        if (!::PeekNamedPipe(hRd, nullptr, 0, nullptr, &dwAvail, nullptr) || dwAvail == 0) break;
        // 尾部残留读取缓冲（不超过 4KB，避免超大单块分配）
        constexpr size_t kPipeTailBufSize = 4096;
        char szTail[kPipeTailBufSize];
        DWORD dwTail = 0;
        ::ReadFile(hRd, szTail, dwAvail < sizeof(szTail) ? dwAvail : sizeof(szTail),
                   &dwTail, nullptr);
        if (dwTail == 0) break;
        sBytes.append(szTail, dwTail);
    }



    // 拿退出码
    DWORD dwExit = 0;
    ::GetExitCodeProcess(pi.hProcess, &dwExit);
    if (pdwExit) *pdwExit = dwExit;



    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(hRd);



    int nLen = ::MultiByteToWideChar(CP_ACP, 0, sBytes.c_str(),
                                     static_cast<int>(sBytes.size()), nullptr, 0);
    if (nLen > 0)
    {
        std::vector<wchar_t> vW(nLen + 1, 0);
        ::MultiByteToWideChar(CP_ACP, 0, sBytes.c_str(), static_cast<int>(sBytes.size()),
                              &vW[0], nLen);
        sOut = &vW[0];
    }
    else
        sOut.Empty();
    return !bTimedOut;
}

// [U-2] 隐藏运行程序并捕获输出（用于 schtasks.exe 查询）
int ProcessRunner::RunHiddenCapture(const wchar_t* pszExe, const CDuiString& sArgs, CDuiString& sOutput)
{
    // [R-B56] 外部命令防腐台账
    ExternalCmdLedger::Note(pszExe, sArgs.GetData());
    CDuiString sCmd;
    // [修复 P1-CMD] 裸工具名才解析目录并加引号；完整命令行（含路径/引号/
    // 参数，如更新阶段自身 exe 拉起）原样透传，避免拼接损坏导致起不来。
    if (IsBareToolName(pszExe))
        sCmd.Format(L"\"%s\" %s", ResolveSysTool(pszExe).GetData(), sArgs.GetData());
    else
    {
        sCmd = pszExe;
        if (!sArgs.IsEmpty()) { sCmd += L' '; sCmd += sArgs; }
    }
    std::vector<wchar_t> vBuf(sCmd.GetLength() + 3, 0);
    wfmt::FormatInto(&vBuf[0], vBuf.size(), L"%s", sCmd.GetData());



    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;



    HANDLE hReadPipe = nullptr, hWritePipe = nullptr;
    if (!::CreatePipe(&hReadPipe, &hWritePipe, &sa, 0))
        return -1;



    STARTUPINFO si; memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.hStdInput = nullptr;



    PROCESS_INFORMATION pi; memset(&pi, 0, sizeof(pi));
    if (!::CreateProcess(nullptr, &vBuf[0], nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
    {
        ::CloseHandle(hReadPipe);
        ::CloseHandle(hWritePipe);
        return -1;
    }



    ::CloseHandle(hWritePipe);
    // [R-B84] 旧实现先 WaitForSingleObject(30000) 等子进程退出、再 ReadFile：
    // schtasks /query /fo csv /v 输出可达数百 KB，写满 4KB 管道后子进程
    // 阻塞在 WriteFile 永不退出 → 等待必满 30 秒（调用方又是 UI 线程，
    // 切到"计划任务"页即"未响应"）。改为持续抽管道 + 60 秒总截止时间
    // 短轮询，与 RunCapture/RunCaptureBytes 的修复（R-B83）同型。
    sOutput.Empty();
    // 管道读缓冲（schtasks /query 输出可达数百 KB，持续抽 4KB 防子进程阻塞）
    constexpr size_t kPipeReadBufSize = 4096;
    std::array<char, kPipeReadBufSize> aBuf;
    DWORD nBytes = 0;
    bool bTimedOut = false;
    constexpr DWORD kTimeoutMs = 60000;
    ULONGLONG ullDeadline = ::GetTickCount64() + static_cast<ULONGLONG>(kTimeoutMs);
    for (;;)
    {
        DWORD dwAvail = 0;
        if (!::PeekNamedPipe(hReadPipe, nullptr, 0, nullptr, &dwAvail, nullptr)) break;
        if (dwAvail > 0)
        {
            DWORD dwToRead = dwAvail < aBuf.size() ? dwAvail : static_cast<DWORD>(aBuf.size());
            if (!::ReadFile(hReadPipe, aBuf.data(), dwToRead, &nBytes, nullptr) || nBytes == 0) break;
            for (DWORD i = 0; i < nBytes; ++i)
                sOutput += static_cast<wchar_t>(aBuf[i]);
            continue;
        }
        if (::WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) break;
        if (::GetTickCount64() >= ullDeadline) { bTimedOut = true; break; }
        ::Sleep(50);
    }
    // 抽干管道残余（进程已退出；超时场景杀进程后也尽力保留已得输出）
    for (;;)
    {
        DWORD dwAvail = 0;
        if (!::PeekNamedPipe(hReadPipe, nullptr, 0, nullptr, &dwAvail, nullptr) || dwAvail == 0) break;
        if (!::ReadFile(hReadPipe, aBuf.data(), static_cast<DWORD>(aBuf.size()), &nBytes, nullptr) || nBytes == 0) break;
        for (DWORD i = 0; i < nBytes; ++i)
            sOutput += static_cast<wchar_t>(aBuf[i]);
    }
    if (bTimedOut)
    {
        ::TerminateProcess(pi.hProcess, (DWORD)-1);
        ::WaitForSingleObject(pi.hProcess, 3000);
    }
    DWORD dwExit = (DWORD)-1;
    ::GetExitCodeProcess(pi.hProcess, &dwExit);
    ::CloseHandle(hReadPipe);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return bTimedOut ? -1 : static_cast<int>(dwExit);
}
// P4：逐字摘自原 FrameWnd.cpp static RunCapture（原始字节，供 DISM UTF-16 解码）
bool ProcessRunner::RunCaptureBytes(const wchar_t* pszExe, const CDuiString& sArgs,
                                    std::vector<char>& vBytes, DWORD& dwExit)
{
    // [R-B56] 外部命令防腐台账
    ExternalCmdLedger::Note(pszExe, sArgs.GetData());
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    HANDLE hR = nullptr, hW = nullptr;
    if (!::CreatePipe(&hR, &hW, &sa, 0)) return false;
    ::SetHandleInformation(hR, HANDLE_FLAG_INHERIT, 0);
    constexpr int kCmdLineWchars = 2048;   // 2048 缓冲尺寸（语义命名，数值不变）
    wchar_t szCmd[kCmdLineWchars];
    wfmt::FormatInto(szCmd, _countof(szCmd), L"\"%s\" %s", pszExe, sArgs.GetData());
    STARTUPINFO si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hW;
    si.hStdError = hW;
    si.hStdInput = nullptr;
    PROCESS_INFORMATION pi = { 0 };
    BOOL bOk = ::CreateProcess(nullptr, szCmd, nullptr, nullptr, TRUE,
                               CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    ::CloseHandle(hW);
    if (!bOk) { ::CloseHandle(hR); return false; }
    // 管道异步读循环（PeekNamedPipe 先探测，避免 ReadFile 死锁）
    bool bTimedOut = false;
    constexpr DWORD kTimeoutMs = 180000;   // 3 分钟总时限
    // [R-B83] 总截止时间（GetTickCount64 无 49 天回绕问题）
    ULONGLONG ullDeadline = ::GetTickCount64() + static_cast<ULONGLONG>(kTimeoutMs);
    // 管道异步读缓冲（PeekNamedPipe 先探测，ReadFile 每次最多 4KB，防死锁）
    constexpr size_t kPipeReadBufSize = 4096;
    std::array<char, kPipeReadBufSize> aBuf2;
    for (;;)
    {
        DWORD dwAvail = 0;
        if (!::PeekNamedPipe(hR, nullptr, 0, nullptr, &dwAvail, nullptr)) break;
        if (dwAvail > 0)
        {
            // 管道有数据立即抽干：dism 输出为"写一段→CBS/DismHost 初始化
            // →再写一段"的突发模式，停抽会让 4KB 管道写满、子进程阻塞在
            // WriteFile 永不退出。
            DWORD dwToRead = dwAvail < aBuf2.size() ? dwAvail : static_cast<DWORD>(aBuf2.size());
            DWORD rd = 0;
            if (!::ReadFile(hR, aBuf2.data(), dwToRead, &rd, nullptr) || rd == 0) break;
            vBytes.insert(vBytes.end(), aBuf2.data(), aBuf2.data() + rd);
            continue;
        }
        // 管道瞬间为空：进程退出则结束（残留数据随后统一抽）；否则只短睡
        // 50ms 继续轮询抽管道——绝不在进程句柄上长阻塞，否则等待期间无人
        // 读管道，与子进程形成写满死锁（旧代码此处 WaitForSingleObject
        // (180s)，dism 必挂到超时被杀，调用方拿到退出码 -1）。
        if (::WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) break;
        if (::GetTickCount64() >= ullDeadline) { bTimedOut = true; break; }
        ::Sleep(50);
    }
    // 超时 → TerminateProcess；否则读残留管道
    if (bTimedOut)
    {
        ::TerminateProcess(pi.hProcess, (DWORD)-1);
        ::WaitForSingleObject(pi.hProcess, 3000);
    }
    else
    {
        for (;;)
        {
            DWORD dwAvail = 0;
            if (!::PeekNamedPipe(hR, nullptr, 0, nullptr, &dwAvail, nullptr) || dwAvail == 0) break;
            DWORD rd = 0;
            ::ReadFile(hR, aBuf2.data(), dwAvail < aBuf2.size() ? dwAvail : static_cast<DWORD>(aBuf2.size()), &rd, nullptr);
            if (rd == 0) break;
            vBytes.insert(vBytes.end(), aBuf2.data(), aBuf2.data() + rd);
        }
    }
    ::GetExitCodeProcess(pi.hProcess, &dwExit);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(hR);
    return !bTimedOut;
}
// [C++17 R3] 本文件内联常量（C++17 inline variables，多 TU ODR 一致）：
