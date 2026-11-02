@echo off
rem ============================================================================
rem  DuiLib_Ultimate one-click build  (adapted for this machine, 2026-11-01)
rem
rem  Usage:
rem      build.cmd [Config] [Target] [/SkipRust]
rem      build.cmd                      -> Debug   / Build
rem      build.cmd Release Rebuild      -> Release / Rebuild
rem      build.cmd Debug Rebuild /SkipRust
rem
rem  Why this script exists instead of relying on vswhere:
rem    1) This machine's VS install has an EMPTY instance registry
rem       (C:\ProgramData\Microsoft\VisualStudio\Packages\_Instances),
rem       so vswhere returns nothing and the CMake "Visual Studio 18 2026"
rem       generator cannot find any instance. We locate MSBuild by path.
rem    2) vcvars64.bat resolves the Windows SDK through reg.exe, which is
rem       blocked by security policy here, so INCLUDE/LIB end up without the
rem       SDK. We append the SDK paths manually.
rem    3) MSBuild throws MSB6001 when both http_proxy and HTTP_PROXY exist in
rem       the environment (ProcessStartInfo env dictionary collision). We clear
rem       the lower-case duplicates first.
rem
rem  Style rules for this file (learned the hard way):
rem    - ASCII only. Chinese comments in a .bat get mangled under code page 936
rem      and can swallow line breaks, corrupting the commands that follow.
rem    - No multi-line ( ... ) blocks. Calling vcvars64.bat inside a block
rem      clashes with its own setlocal/endlocal and breaks the parent parser.
rem      Use goto labels instead.
rem ============================================================================
setlocal

set "CFG=%~1"
if "%CFG%"=="" set "CFG=Debug"
set "TGT=%~2"
if "%TGT%"=="" set "TGT=Build"
if /i "%TGT%"=="/SkipRust" set "TGT=Build"
set "SKIPRUST=0"
if /i "%~3"=="/SkipRust" set "SKIPRUST=1"
if /i "%~2"=="/SkipRust" set "SKIPRUST=1"

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "PF86=%ProgramFiles(x86)%"

echo ============================================================
echo  DuiLib_Ultimate Build
echo    Config : %CFG%
echo    Target : %TGT%
echo    Root   : %ROOT%
echo ============================================================
echo.

rem ---- 1. locate Visual Studio -------------------------------------------
set "VSDIR="
for /d %%d in ("%ProgramFiles%\Microsoft Visual Studio\18\*") do set "VSDIR=%%~fd"
if not defined VSDIR for /d %%d in ("%ProgramFiles%\Microsoft Visual Studio\2022\*") do set "VSDIR=%%~fd"
if not defined VSDIR goto :no_vs
set "MSB=%VSDIR%\MSBuild\Current\Bin\MSBuild.exe"
if not exist "%MSB%" goto :no_msbuild
echo [1/4] VS      : %VSDIR%

rem ---- 2. locate MSVC toolset + Windows SDK -------------------------------
set "MSVCVER="
for /d %%d in ("%VSDIR%\VC\Tools\MSVC\14.*") do set "MSVCVER=%%~nxd"
if not defined MSVCVER goto :no_msvc

set "SDKVER="
for /d %%d in ("%PF86%\Windows Kits\10\Include\10.*") do set "SDKVER=%%~nxd"
if not defined SDKVER goto :no_sdk
echo [2/4] MSVC    : %MSVCVER%
echo       SDK     : %SDKVER%

rem ---- 3. environment ----------------------------------------------------
rem 3a. drop lower-case proxy duplicates (MSB6001 guard)
set "http_proxy="
set "https_proxy="
set "all_proxy="
set "no_proxy="
set "ftp_proxy="

rem 3b. vcvars64 gives MSVC include/lib; it cannot find the SDK (reg.exe blocked)
set "VCVARS=%VSDIR%\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" goto :no_vcvars
call "%VCVARS%" >nul 2>&1
goto :env_sdk

:no_vcvars
echo [WARN] vcvars64.bat missing - relying on manual paths only.

:env_sdk
set "SDKI=%PF86%\Windows Kits\10\Include\%SDKVER%"
set "SDKL=%PF86%\Windows Kits\10\Lib\%SDKVER%"
set "PATH=%PF86%\Windows Kits\10\bin\%SDKVER%\x64;%PATH%"
set "INCLUDE=%SDKI%\ucrt;%SDKI%\shared;%SDKI%\um;%SDKI%\winrt;%SDKI%\cppwinrt;%INCLUDE%"
set "LIB=%SDKL%\ucrt\x64;%SDKL%\um\x64;%LIB%"

if not exist "%SDKI%\um\windows.h" goto :no_winsdk_h

rem ---- 4. Rust dismcore (duilibcalc.dll) ---------------------------------
if "%SKIPRUST%"=="1" goto :rust_skipped
echo [3/4] cargo   : building rust\dismcore ...
pushd "%ROOT%\rust\dismcore"
cargo build --release
set "RUSTRC=%errorlevel%"
popd
if not "%RUSTRC%"=="0" goto :rust_failed
if not exist "%ROOT%\rust\dismcore\target\release\duilibcalc.dll" goto :rust_ok
copy /y "%ROOT%\rust\dismcore\target\release\duilibcalc.dll" "%ROOT%\bin64\amd64\" >nul
echo       duilibcalc.dll deployed to bin64\amd64
goto :rust_ok

:rust_skipped
echo [3/4] cargo   : skipped

:rust_ok
rem ---- 5. MSBuild --------------------------------------------------------
echo [4/4] msbuild : %TGT% ^| %CFG% ^| x64
if not exist "%ROOT%\_build" mkdir "%ROOT%\_build"
set "LOG=%ROOT%\_build\%CFG%_x64.log"

"%MSB%" "%ROOT%\DuiLib.sln" /t:%TGT% /p:Configuration=%CFG% /p:Platform=x64 /m /nologo /v:normal /nodeReuse:false > "%LOG%" 2>&1
set "MSBRC=%errorlevel%"

set "ERRS=0"
rem Use the absolute System32 path: Git for Windows ships a GNU find.exe that
rem shadows the Windows one, and "find /c" would make it scan C:\ instead of
rem counting lines (that is what made an earlier run appear to hang for minutes).
for /f %%c in ('findstr /c:"error " "%LOG%" ^| "%SystemRoot%\System32\find.exe" /c /v ""') do set "ERRS=%%c"

echo.
echo ------------------------------------------------------------
echo  log    : %LOG%
echo  error  : %ERRS% matching line^(s^)
echo  exit   : %MSBRC%
echo ------------------------------------------------------------
if not "%MSBRC%"=="0" goto :build_failed
echo BUILD SUCCEEDED
endlocal & exit /b 0

rem ---- failure paths -----------------------------------------------------
:no_vs
echo [FAIL] Visual Studio 18 / 2022 not found under "%ProgramFiles%\Microsoft Visual Studio".
endlocal & exit /b 1

:no_msbuild
echo [FAIL] MSBuild not found: %MSB%
endlocal & exit /b 1

:no_msvc
echo [FAIL] MSVC toolset 14.x not found under %VSDIR%\VC\Tools\MSVC
endlocal & exit /b 1

:no_sdk
echo [FAIL] Windows SDK 10.x not found under "%PF86%\Windows Kits\10\Include"
endlocal & exit /b 1

:no_winsdk_h
echo [FAIL] windows.h not found under %SDKI%\um - SDK layout unexpected.
endlocal & exit /b 1

:rust_failed
echo [FAIL] cargo build failed with exit code %RUSTRC%
endlocal & exit /b 1

:build_failed
echo BUILD FAILED
endlocal & exit /b 1
