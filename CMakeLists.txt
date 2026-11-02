cmake_minimum_required(VERSION 3.20)

project(DuiLib VERSION 1.0.0 LANGUAGES CXX)

# ============================================================================
# [HARD CONSTRAINT — 项目级全局]
#   1. 仅 x64（AMD64）架构，拒绝 Win32 / ARM / ARM64
#   2. 仅 C++17 及以上，拒绝 C++11 / C++14
#   3. 仅 Win10+（_WIN32_WINNT=0x0A00）
# ============================================================================
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR
        "[DuiLib] 架构不兼容 —— 仅支持 x64。\n"
        "  当前: ${CMAKE_SYSTEM_PROCESSOR} (指针 ${CMAKE_SIZEOF_VOID_P} 字节)\n"
        "  请用 cmake -A x64 重新配置。")
endif()

# [X64-ONLY] Win32/x86/ARM/ARM64 一律拒绝：仓库已删除全部 32 位与 ARM 构建配置。
if(CMAKE_GENERATOR_PLATFORM MATCHES "^[Ww]in32$|^[xX]86$|^[Aa][Rr][Mm]")
    message(FATAL_ERROR
        "[DuiLib] CMAKE_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM} 不受支持。\n"
        "  本项目仅支持 x64，请改用 -A x64。")
endif()

message(STATUS "[DuiLib] ✓ 架构: ${CMAKE_SYSTEM_PROCESSOR} (x64 only)")

# ---- C++ 标准锁死：最低 17 ----
set(CMAKE_CXX_STANDARD 17 CACHE STRING "C++ standard (MIN 17)" FORCE)
set(CMAKE_CXX_STANDARD_REQUIRED ON CACHE BOOL "" FORCE)
set(CMAKE_CXX_EXTENSIONS OFF CACHE BOOL "" FORCE)

# ---- Windows 最低版本 ----
add_definitions(-D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00)

message(STATUS "[DuiLib] ✓ C++ 标准: >= 17    [DuiLib] ✓ Win 最低: Win10")

# ============================================================================
# 双轨构建（保留 DUI_TRACK，但不再暴露 C++11/WinXP 选项）
# ============================================================================
# DUI_TRACK = compat  -> v143 / C++17 / Win10   ← 已从 C++11 升级
# DUI_TRACK = modern  -> v143 / C++23 / Win11
set(DUI_TRACK "modern" CACHE STRING "Build track: compat(C++17) | modern(C++23)")

if(DUI_TRACK STREQUAL "modern")
    set(_dui_std_default     "23")
else()
    set(_dui_std_default     "17")
endif()

# 确保 CMAKE_CXX_STANDARD 不低于 17
if(CMAKE_CXX_STANDARD LESS 17)
    message(FATAL_ERROR
        "[DuiLib] CMAKE_CXX_STANDARD=${CMAKE_CXX_STANDARD} 低于最低要求 17。\n"
        "  请使用 C++17/20/23 编译器。")
endif()

# [TRACK-STD] 让 DUI_TRACK 真正决定编译标准。此前 _dui_std_default 算出 23 却没有
# 应用到任何目标，导致 modern 轨道实际仍按 C++17 编译，与配置名和注释矛盾。
set(CMAKE_CXX_STANDARD ${_dui_std_default} CACHE STRING "C++ standard (MIN 17)" FORCE)

# 统一编译选项
add_compile_options(/W3 /WX- /utf-8 /permissive- /EHsc /GR /MP)
add_definitions(-DCMAKE -DUNICODE -D_UNICODE)

set(CMAKE_SYSTEM_NAME Windows)

# 现代线启用 WebView2
# [WV2-MACRO] 注意：源码用 #if defined(DUILIB_USE_WEBVIEW2) 判断，宏定义为 0 也会进入
# WebView2 分支。因此 compat 轨道改为完全不定义该宏，确保真正关闭。
if(DUI_TRACK STREQUAL "modern")
    add_definitions(-DDUILIB_USE_WEBVIEW2=1)
    include_directories("${CMAKE_SOURCE_DIR}/3rd/WebView2/include")
else()
    add_definitions(-UDUILIB_USE_WEBVIEW2)
endif()

# 输出目录
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/lib ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# 子项目
add_subdirectory(DuiLib)

# [LOCAL-ADAPT 2026-11-01] Demos 目录当前没有 CMakeLists.txt（4 个 Demo 只由 DuiLib.sln
# 构建）。原先无条件 add_subdirectory(Demos) 会让整个 CMake 配置在生成阶段直接失败，
# 结果是连 DuiLib 库本身都构建不出来。改为存在性守卫：有 CMakeLists 才纳入，否则告警跳过。
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Demos/CMakeLists.txt")
    add_subdirectory(Demos)
else()
    message(WARNING
        "[DuiLib] 未找到 Demos/CMakeLists.txt —— 已跳过 Demos 子目录。\n"
        "  Demo 程序（Win8Metro / ActivationKit / CBSHost / NCleaner）请用 DuiLib.sln 构建：\n"
        "    msbuild DuiLib.sln /t:Rebuild /p:Configuration=Debug /p:Platform=x64 /m\n"
        "  本次 CMake 构建只产出 DuiLib 库。")
endif()

# ============================================================================
# 最终门禁
# ============================================================================
message(STATUS "")
message(STATUS "=============================================")
message(STATUS "[DuiLib] GLOBAL HARD CONSTRAINTS")
message(STATUS "  Architecture : x64 ONLY    (sizeof(void*)=${CMAKE_SIZEOF_VOID_P}, no Win32/x86/ARM)")
message(STATUS "  C++ Standard  : ${CMAKE_CXX_STANDARD}         (track=${DUI_TRACK}, x64 only)")
message(STATUS "  Windows Ver   : Win10+      (_WIN32_WINNT=0x0A00)")
message(STATUS "  DUI_TRACK     : ${DUI_TRACK}")
message(STATUS "=============================================")
message(STATUS "")
message(STATUS "构建示例:")
message(STATUS "  cmake -G 'Visual Studio 18 2026' -A x64 -DDUI_TRACK=modern -B build")
message(STATUS "  cmake --build build --config Release")
message(STATUS "")
message(STATUS "  [本机备注] 若 VS 生成器报 'could not find any instance of Visual Studio'，")
message(STATUS "  说明 VS 安装的实例注册信息缺失（vswhere 查不到），此时改用 Ninja：")
message(STATUS "    先在 vcvars64 环境里补 SDK 的 INCLUDE/LIB，再执行")
message(STATUS "    cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DDUI_TRACK=modern -B build")
