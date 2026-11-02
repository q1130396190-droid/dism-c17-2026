@echo off
REM ============================================================
REM Win8Metro.exe build script (Release x64)
REM Full compile: core + Service + Page (182 .cpp files)
REM MAX-OUT: /MP0 + /CGTHREADS:8 + /FS + /DEBUG:FASTLINK
REM ============================================================
setlocal enabledelayedexpansion

rem ---- Locate Visual Studio (vswhere first, then known drive letters) ----
set "PF86=%ProgramFiles(x86)%"
set "VS="
if exist "%PF86%\Microsoft Visual Studio\Installer\vswhere.exe" (
    for /f "usebackq delims=" %%I in (`"%PF86%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath`) do set "VS=%%I"
)
if not defined VS if exist "D:\Program Files\Microsoft Visual Studio\18\Community" set "VS=D:\Program Files\Microsoft Visual Studio\18\Community"
if not defined VS if exist "C:\Program Files\Microsoft Visual Studio\18\Community" set "VS=C:\Program Files\Microsoft Visual Studio\18\Community"
if not defined VS (
    echo [ERROR] Visual Studio installation not found
    exit /b 1
)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo [ERROR] vcvars failed
    exit /b 1
)

REM Extra Windows SDK include/lib paths
set "WIN_SDK_ROOT=C:\Program Files (x86)\Windows Kits\10"
for %%V in (10.0.28000.0 10.0.26100.0) do (
    if exist "%WIN_SDK_ROOT%\Include\%%V\um" set "INCLUDE=%WIN_SDK_ROOT%\Include\%%V\um;%WIN_SDK_ROOT%\Include\%%V\ucrt;%WIN_SDK_ROOT%\Include\%%V\shared;%WIN_SDK_ROOT%\Include\%%V\winrt;%INCLUDE%"
    if exist "%WIN_SDK_ROOT%\Lib\%%V\um\x64" set "LIB=%WIN_SDK_ROOT%\Lib\%%V\um\x64;%WIN_SDK_ROOT%\Lib\%%V\ucrt\x64;%LIB%"
)

set ROOT=%~dp0..\..
set SRC=%ROOT%\Demos\Win8Metro
set BIN=%ROOT%\bin64
set TMP=%ROOT%\temp\Win8Metro\x64\Release

if not exist "%TMP%" mkdir "%TMP%"

echo === [1/4] Preparing object dir ===
if exist "%BIN%\Win8Metro.exe" move /Y "%BIN%\Win8Metro.exe" "%BIN%\Win8Metro.exe.old" >nul 2>&1

rem ============================================================
rem MAX-OUT COMPILE FLAGS
rem   /MP   = spawn N cl.exe workers (N = all logical cores, 8 here)
rem   /FS   = force synchronous PDB writes (mandatory with /MP, else races)
rem Set CL as env var so every cl subprocess (spawned by /MP) inherits these.
rem ============================================================
set "CL=/MP /FS"

echo === [2/4] Compiling C++ sources (%CL%) ===
cd /d "%SRC%"
rem ---- Auto-discover all .cpp sources (prevents missed-source LNK errors) ----
if exist "%TMP%\srcs.rsp" del /Q "%TMP%\srcs.rsp" >nul 2>&1
for /f "delims=" %%f in ('dir /s /b /a-d *.cpp ^| findstr /v /i /c:"\\tests\\"') do (
    echo "%%f" >> "%TMP%\srcs.rsp"
)
echo "..\..\DuiLib\Utils\unzip.cpp" >> "%TMP%\srcs.rsp"

cl /nologo /W3 /EHsc /O2 /Zi /std:c++17 /source-charset:utf-8 /D _UNICODE /D UNICODE /D WIN32 /D NDEBUG ^
   /D _WIN32_WINNT=0x0A00 ^
   /I . ^
   /I "..\ThemeManager" ^
   /I "..\..\DuiLib" /I "..\..\DuiLib\Utils" /I "..\..\DuiLib\Core" ^
   /I "..\..\DuiLib\Control" /I "..\..\DuiLib\Layout" ^
   /I "..\wimgapi\include" ^
   /Fo"%TMP%\\" /c @"%TMP%\srcs.rsp"

if errorlevel 1 (
    echo [ERROR] cl compile failed
    exit /b 1
)

echo === [3/4] Linking (x64, /DEBUG:FASTLINK) ===
REM FASTLINK = incremental debug symbols, 2-3x faster than /DEBUG full PDB gen
set "RSP=%TMP%\link_objs.rsp"
if exist "%RSP%" del /Q "%RSP%" >nul 2>&1
pushd "%TMP%"
for %%f in (*.obj) do @echo "%%f" >> "link_objs.rsp"
link /nologo /OUT:"%BIN%\Win8Metro.exe" /SUBSYSTEM:WINDOWS /MACHINE:X64 ^
     /DEBUG:FASTLINK /PDB:"%BIN%\Win8Metro.pdb" ^
     /DELAYLOAD:DuiLib.dll /DELAYLOAD:duilibcalc.dll delayimp.lib ^
     @"link_objs.rsp" ^
     /LIBPATH:"%BIN%\amd64" /LIBPATH:"%BIN%" /LIBPATH:"%ROOT%\rust\dismcore\target\release" ^
     "%BIN%\amd64\DuiLib.lib" ^
     "%BIN%\ThemeManager.lib" ^
     runtimeobject.lib ^
     vssapi.lib duilibcalc.dll.lib ^
     user32.lib gdi32.lib comdlg32.lib shell32.lib ole32.lib oleaut32.lib ^
     uuid.lib advapi32.lib ws2_32.lib winmm.lib comctl32.lib ^
     wininet.lib wintrust.lib crypt32.lib ^
     dwmapi.lib uxtheme.lib shlwapi.lib psapi.lib gdiplus.lib version.lib
set "LINKRC=%ERRORLEVEL%"
popd
if not "%LINKRC%"=="0" (
    echo [ERROR] link failed
    exit /b 1
)

echo === [4/4] Ensuring x64 language packs ===
if exist "%ROOT%\lang\build_all_x64.bat" if errorlevel 1 (
    echo [WARN] x64 language packs build failed - UI may show [key] placeholders
)

echo === [5/5] Cleanup ===
if exist "%BIN%\Win8Metro.exe.old" del /Q "%BIN%\Win8Metro.exe.old" >nul 2>&1

echo === BUILD SUCCESS: %BIN%\Win8Metro.exe ===
exit /b 0
