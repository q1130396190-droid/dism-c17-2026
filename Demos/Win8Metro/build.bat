@echo off
REM ============================================================
REM Win8Metro.exe build script (Release Win32 x86)
REM Full compile: core + Service + Page (51 .cpp files)
REM ============================================================
setlocal enabledelayedexpansion

if exist "D:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat" (
    call "D:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat" >nul
) else (
    call "D:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
)
if errorlevel 1 (
    echo [ERROR] vcvars failed
    exit /b 1
)

REM Extra Windows SDK include/lib paths
set "WIN_SDK_ROOT=C:\Program Files (x86)\Windows Kits\10"
for %%V in (10.0.28000.0 10.0.26100.0) do (
    if exist "%WIN_SDK_ROOT%\Include\%%V\um" set "INCLUDE=%WIN_SDK_ROOT%\Include\%%V\um;%WIN_SDK_ROOT%\Include\%%V\ucrt;%WIN_SDK_ROOT%\Include\%%V\shared;%WIN_SDK_ROOT%\Include\%%V\winrt;%INCLUDE%"
    if exist "%WIN_SDK_ROOT%\Lib\%%V\um\x86" set "LIB=%WIN_SDK_ROOT%\Lib\%%V\um\x86;%WIN_SDK_ROOT%\Lib\%%V\ucrt\x86;%LIB%"
)

set ROOT=%~dp0..\..
set SRC=%ROOT%\Demos\Win8Metro
set BIN=%ROOT%\bin
set TMP=%ROOT%\temp\Win8Metro\Win32\Release

if not exist "%TMP%" mkdir "%TMP%"

echo === [1/4] Preparing object dir ===
if exist "%BIN%\Win8Metro.exe" move /Y "%BIN%\Win8Metro.exe" "%BIN%\Win8Metro.exe.old" >nul 2>&1

echo === [2/4] Compiling C++ sources (x86, all modules) ===
cd /d "%SRC%"
cl /nologo /W3 /EHsc /O2 /std:c++17 /source-charset:utf-8 /D _UNICODE /D UNICODE /D WIN32 /D NDEBUG ^
   /D _WIN32_WINNT=0x0A00 ^
   /I "..\..\DuiLib" /I "..\..\DuiLib\Utils" /I "..\..\DuiLib\Core" ^
   /I "..\..\ControlLib" /I "..\..\ControlLib\Utils" /I "..\..\DuiLib\Base" ^
   /I "..\..\DuiLib\Control" /I "..\..\DuiLib\Layout" ^
   /I "..\wimgapi\include" ^
   /Fo"%TMP%\\" /c stdafx.cpp Win8Metro.cpp StringManager.cpp ConfigReader.cpp ^
                 DataEngine.cpp CleanEngine.cpp FrameWnd.cpp FrameWnd_Util.cpp ^
                 FrameWnd_ErrorMap.cpp FrameWnd_UiHelpers.cpp ToolDialog.cpp ^
                 Win32DialogService.cpp LogService.cpp DiagnosticsService.cpp ^
                 ProcessRunner.cpp Registry.cpp ExternalToolService.cpp ^
                 CleanJobService.cpp AppxCapUpdService.cpp WIMHelper.cpp ^
                 WIMHelperBridge.cpp DismApiBridge.cpp AssocService.cpp ^
                 DriverService.cpp FeatureService.cpp ProgramService.cpp ^
                 ScheduleTaskService.cpp StartupService.cpp SystemInfoService.cpp ^
                 CommandLineService.cpp ^
                 MountLedgerService.cpp ^
                 UpdateMetaService.cpp ^
                 Page_Welcome.cpp Page_Control.cpp Page_Intl.cpp Page_RegClean.cpp ^
                 Page_Cleanup.cpp Page_Startup.cpp Page_Appx.cpp Page_Tools.cpp ^
                 Page_Optim.cpp Page_Driver.cpp Page_Program.cpp Page_Feature.cpp ^
                 Page_Update.cpp Page_Assoc.cpp Page_Unattend.cpp Page_Tasks.cpp Page_Capability.cpp ^
                 AppxDeployService.cpp AppxCapUpdService_Install.cpp ControlService.cpp ^
                 CleanExtend.cpp IntlService.cpp RegCleanEngine.cpp RegCleanJobService.cpp ^
                 WelcomeService.cpp ^
                 OfflineCbsService.cpp ^
                 UpdPhaseRelay.cpp ^
                 CbsHostBridge.cpp ^
                 ExternalCmdLedger.cpp ^
                 VssNativeService.cpp ^
                 CleanNativePrimitives.cpp ^
                 DependencyGraphService.cpp

if errorlevel 1 (
    echo [ERROR] cl compile failed
    exit /b 1
)

echo === [3/4] Linking (x86) ===
link /nologo /OUT:"%BIN%\Win8Metro.exe" /SUBSYSTEM:WINDOWS /MACHINE:X86 ^
     /PDB:"%BIN%\Win8Metro.pdb" ^
     /LIBPATH:"..\wimgapi\lib\x86" ^
     "%TMP%\stdafx.obj" "%TMP%\Win8Metro.obj" "%TMP%\StringManager.obj" ^
     "%TMP%\ConfigReader.obj" "%TMP%\DataEngine.obj" "%TMP%\CleanEngine.obj" ^
     "%TMP%\FrameWnd.obj" "%TMP%\FrameWnd_Util.obj" "%TMP%\FrameWnd_ErrorMap.obj" ^
     "%TMP%\FrameWnd_UiHelpers.obj" "%TMP%\ToolDialog.obj" ^
     "%TMP%\Win32DialogService.obj" "%TMP%\LogService.obj" "%TMP%\DiagnosticsService.obj" ^
     "%TMP%\ProcessRunner.obj" "%TMP%\Registry.obj" "%TMP%\ExternalToolService.obj" ^
     "%TMP%\CleanJobService.obj" "%TMP%\AppxCapUpdService.obj" "%TMP%\WIMHelper.obj" ^
     "%TMP%\WIMHelperBridge.obj" "%TMP%\DismApiBridge.obj" "%TMP%\AssocService.obj" ^
     "%TMP%\DriverService.obj" "%TMP%\FeatureService.obj" "%TMP%\ProgramService.obj" ^
     "%TMP%\ScheduleTaskService.obj" "%TMP%\StartupService.obj" "%TMP%\SystemInfoService.obj" ^
     "%TMP%\CommandLineService.obj" ^
     "%TMP%\MountLedgerService.obj" ^
     "%TMP%\UpdateMetaService.obj" ^
     "%TMP%\Page_Welcome.obj" "%TMP%\Page_Control.obj" "%TMP%\Page_Intl.obj" "%TMP%\Page_RegClean.obj" ^
     "%TMP%\Page_Cleanup.obj" "%TMP%\Page_Startup.obj" "%TMP%\Page_Appx.obj" "%TMP%\Page_Tools.obj" ^
     "%TMP%\Page_Optim.obj" "%TMP%\Page_Driver.obj" "%TMP%\Page_Program.obj" "%TMP%\Page_Feature.obj" ^
     "%TMP%\Page_Update.obj" "%TMP%\Page_Assoc.obj" "%TMP%\Page_Unattend.obj" "%TMP%\Page_Tasks.obj" ^
     "%TMP%\Page_Capability.obj" "%TMP%\AppxDeployService.obj" ^
     "%TMP%\AppxCapUpdService_Install.obj" "%TMP%\ControlService.obj" ^
     "%TMP%\CleanExtend.obj" "%TMP%\IntlService.obj" "%TMP%\RegCleanEngine.obj" ^
     "%TMP%\RegCleanJobService.obj" "%TMP%\WelcomeService.obj" ^
     "%TMP%\OfflineCbsService.obj" ^
     "%TMP%\UpdPhaseRelay.obj" ^
     "%TMP%\CbsHostBridge.obj" ^
     "%TMP%\ExternalCmdLedger.obj" ^
     "%TMP%\VssNativeService.obj" ^
     "%TMP%\CleanNativePrimitives.obj" ^
     "%TMP%\DependencyGraphService.obj" ^
     "%BIN%\DuiLib.lib" ^
     wimgapi.lib ^
     user32.lib gdi32.lib comdlg32.lib shell32.lib ole32.lib oleaut32.lib ^
     uuid.lib advapi32.lib ws2_32.lib winmm.lib comctl32.lib ^
     wininet.lib wintrust.lib crypt32.lib ^
     dwmapi.lib uxtheme.lib shlwapi.lib psapi.lib gdiplus.lib
if errorlevel 1 (
    echo [ERROR] link failed
    exit /b 1
)

echo === [4/4] Cleanup ===
if exist "%BIN%\Win8Metro.exe.old" del /Q "%BIN%\Win8Metro.exe.old" >nul 2>&1

echo === BUILD SUCCESS: %BIN%\Win8Metro.exe ===
exit /b 0