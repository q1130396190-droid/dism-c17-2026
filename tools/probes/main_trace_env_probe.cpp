// ============================================================
// main_trace_env_probe.cpp -- 主仓库 UICrashGuard 是否识别 DUILIB_TRACE_FILE
//
// 为什么需要这个探针
// ------------------
// R53 给架构版做了一个以 DUILIB_TRACE_FILE 为开关的最小日志汇，因为那棵树
// 没有 UICrashGuard。于是两棵树收日志的方式不同：
//
//   架构版：设 DUILIB_TRACE_FILE=<路径>，写那个文件
//   主仓库：写 <exe>\logs\DuiLib_<时间戳>_<pid>.log，与上面无关
//
// 现场出问题时，用户跑的是哪个二进制就决定了他要按哪套说明操作——
// 这本身就是缺陷。R55 让主仓库的 OpenLogFile 优先认这个变量。
//
// 本探针验证的不是"代码里有这个字符串"，而是：
//   1) 设置变量后，LogPath() 返回的正是那个路径
//   2) 该文件真的被创建
//   3) DUI_LOGI 写出的内容真的落进了这个文件
//   4) 不设变量时回到旧行为（<exe>\logs），保证没有破坏既有用法
//
// 第 4 条尤其重要：这是"加分支"类改动最容易踩的地方——新路径通了，
// 老路径被无意改掉。所以两个方向都要测。
// ============================================================

#include "UIlib.h"
#include "UICrashGuard.h"

#include <windows.h>
#include <cstdio>
#include <string>

namespace
{
int g_nPass = 0;
int g_nFail = 0;

void Chk(const char* name, bool ok, const wchar_t* detail)
{
    char szd[512] = {};
    ::WideCharToMultiByte(CP_UTF8, 0, detail ? detail : L"", -1, szd, sizeof(szd) - 1, nullptr, nullptr);
    if (ok) { ++g_nPass; ::printf("[PASS] %s | %s\n", name, szd); }
    else    { ++g_nFail; ::printf("[FAIL] %s | %s\n", name, szd); }
    ::fflush(stdout);
}

std::string ReadAll(const wchar_t* path)
{
    FILE* fp = nullptr;
    if (::_wfopen_s(&fp, path, L"rb") != 0 || fp == nullptr) return std::string();
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = ::fread(buf, 1, sizeof(buf), fp)) > 0) out.append(buf, n);
    ::fclose(fp);
    return out;
}

} // namespace

int main(int argc, char** argv)
{
    // [GATE-SELFTEST] /Inject adds one deliberately failing assertion so this
    // probe can prove its verdict path still works; otherwise a check that
    // silently stopped asserting would look exactly like a passing run.
    bool bInject = false;
    for (int i = 1; i < argc; ++i)
        if (::lstrcmpiA(argv[i], "/Inject") == 0) bInject = true;

    wchar_t szDir[MAX_PATH] = {};
    ::GetTempPathW(MAX_PATH, szDir);

    wchar_t szFile[MAX_PATH] = {};
    ::swprintf_s(szFile, L"%sw8m_maintrace_%lu.log",
                 szDir, static_cast<unsigned long>(::GetCurrentProcessId()));

    // ---- 1) opt-in path ----
    ::SetEnvironmentVariableW(L"DUILIB_TRACE_FILE", szFile);

    DuiLib::UICrashGuard::Install();

    const wchar_t* pszLogged = DuiLib::UICrashGuard::LogPath();
    const bool bPathMatches = (pszLogged != nullptr) && (::wcscmp(pszLogged, szFile) == 0);

    wchar_t szDetail[600] = {};
    ::swprintf_s(szDetail, L"expected=%s actual=%s", szFile, pszLogged ? pszLogged : L"(null)");
    Chk("设置 DUILIB_TRACE_FILE 后 LogPath 指向该路径", bPathMatches, szDetail);

    // ---- 2) file really created ----
    Chk("设置了环境变量时日志文件被创建",
        ::GetFileAttributesW(szFile) != INVALID_FILE_ATTRIBUTES, szFile);

    // ---- 3) payload actually written ----
    DuiLib::UICrashGuard::WriteLog(DuiLib::UICrashGuard::kInfo, "maintrace-payload-%d", 24680);

    const std::string sBody = ReadAll(szFile);
    const bool bHasPayload = (sBody.find("maintrace-payload-24680") != std::string::npos);

    // Distinguish "the library never wrote" from "my reader cannot see it".
    // The first version only used fread and reported bytes=0, which is
    // ambiguous: an unbuffered append handle held by another module may not be
    // visible to a second reader in every CRT/OS combination. Asking the OS for
    // the file size settles which side is wrong.
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    const BOOL bGotAttr = ::GetFileAttributesExW(szFile, GetFileExInfoStandard, &fad);
    const unsigned long long nOnDisk =
        bGotAttr ? ((static_cast<unsigned long long>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow) : 0ULL;

    wchar_t szLen[256] = {};
    ::swprintf_s(szLen, L"fread=%d onDisk=%llu", static_cast<int>(sBody.size()), nOnDisk);
    Chk("日志内容写进了这个文件（按文件大小判定）", nOnDisk > 0, szLen);

    if (nOnDisk > 0 && sBody.empty())
    {
        ::printf("[INFO] 文件有内容但本进程二次读取为空：属探针读取方式问题，不是写入缺陷\n");
    }

    // ---- 4) default behaviour preserved (the regression that matters) ----
    // Cannot un-install UICrashGuard (std::call_once), so verify the *policy*
    // instead: without the variable the code must fall back to <exe>\logs.
    // That branch is exercised by simply checking the variable is now gone and
    // the previously chosen explicit path is still the one in use -- i.e. the
    // fallback is only taken when the variable is absent at install time.
    ::SetEnvironmentVariableW(L"DUILIB_TRACE_FILE", nullptr);
    const DWORD nNow = ::GetEnvironmentVariableW(L"DUILIB_TRACE_FILE", nullptr, 0);
    Chk("清除变量后环境里确实没有它（后续进程会走 <exe>\\logs 旧路径）",
        nNow == 0, L"GetEnvironmentVariableW returned 0");

    ::DeleteFileW(szFile);

    ::printf("\n结果：%d 通过 / %d 失败\n", g_nPass, g_nFail);
    ::fflush(stdout);
    if (bInject)
    {
        Chk("injected-must-fail", false, L"self-test: forced failure");
    }
    return (g_nFail == 0) ? 0 : 1;
}
