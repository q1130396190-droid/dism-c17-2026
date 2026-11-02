#ifndef __DUILIB_UICRASHGUARD_H__
#define __DUILIB_UICRASHGUARD_H__
// [H1] Crash-guard & rolling file log for DuiLib. C++17 only.
namespace DuiLib {

class UILIB_API UICrashGuard
{
public:
    enum Level { kDebug = 0, kInfo, kWarn, kError, kFatal };

    // Idempotent & thread-safe (std::once_flag). Honors env var
    // DUILIB_CRASHGUARD_DISABLE=1 to skip installation entirely.
    static void Install();
    static bool IsInstalled();

    // printf-style logging into <exe>\logs\DuiLib_<ts>_<pid>.log.
    // Safe to call before Install() (silently no-op).
    static void WriteLog(Level lv, const char* fmt, ...);
    static const wchar_t* LogPath();
};

} // namespace DuiLib

#define DUI_LOGD(...) ::DuiLib::UICrashGuard::WriteLog(::DuiLib::UICrashGuard::kDebug, __VA_ARGS__)
#define DUI_LOGI(...) ::DuiLib::UICrashGuard::WriteLog(::DuiLib::UICrashGuard::kInfo,  __VA_ARGS__)
#define DUI_LOGW(...) ::DuiLib::UICrashGuard::WriteLog(::DuiLib::UICrashGuard::kWarn,  __VA_ARGS__)
#define DUI_LOGE(...) ::DuiLib::UICrashGuard::WriteLog(::DuiLib::UICrashGuard::kError, __VA_ARGS__)
#define DUI_LOGF(...) ::DuiLib::UICrashGuard::WriteLog(::DuiLib::UICrashGuard::kFatal, __VA_ARGS__)

#endif // __DUILIB_UICRASHGUARD_H__