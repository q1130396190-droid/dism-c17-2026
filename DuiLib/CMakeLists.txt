# cmake file for DuiLib

set(LIBRARY_OUTPUT_PATH ${CMAKE_BINARY_DIR}/lib)
set(RUNTIME_OUTPUT_PATH ${CMAKE_BINARY_DIR}/bin)

aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR} RootSrc)
aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR}/Control ControlSrc)
aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR}/Core CoreSrc)
aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR}/Layout LayoutSrc)
aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR}/Utils UtilsSrc)

include_directories(${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/Control)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/Core)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/Layout)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/Utils)

add_library(DuiLib SHARED ${ControlSrc} ${CoreSrc} ${LayoutSrc} ${UtilsSrc} ${RootSrc})

add_definitions(-DUILIB_EXPORTS)

# [CMAKE-LINK] Match the Visual Studio project's link dependencies.
#
# The VS project links oleacc.lib and UIAutomationCore.lib in every configuration, but
# CMake linked only comctl32. The accessibility bridge calls into both:
#   UiaReturnRawElementProvider / UiaHostProviderFromHwnd  -> UIAutomationCore.lib
#   LresultFromObject                                      -> oleacc.lib
# and neither translation unit declares them with #pragma comment(lib), so a CMake
# build would fail at link time with unresolved externals while the VS build succeeded.
#
# PUBLIC because they are part of DuiLib's own ABI surface: consumers that link the
# import library need the same system libraries. A STATIC build in particular must
# propagate them or the consumer gets the unresolved symbols instead.
target_link_libraries(DuiLib
    PUBLIC
        comctl32
        oleacc
        UIAutomationCore
)
set_target_properties(DuiLib PROPERTIES
    DEBUG_POSTFIX ""
    OUTPUT_NAME "DuiLib" 
)

add_custom_command(TARGET DuiLib POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
  ${CMAKE_BINARY_DIR}/bin/$<CONFIG>/DuiLib.dll ${CMAKE_BINARY_DIR}/../bin/DuiLib.dll)

# 复制 WebView2 Loader（运行时需与 exe 同目录；Win10+ 且已安装 WebView2 Runtime 时生效）
# [X64-ONLY] 只构建 x64，必须部署同架构的 Loader：此前这里固定复制 x86 Loader，
# 而 LoadLibrary 在 64 位进程中无法加载 32 位 DLL（ERROR_BAD_EXE_FORMAT），
# 会让 WebView2 静默回退到 IE 后端。工具集统一为 VS 2026 (v145)。
#
# [LOCAL-ADAPT 2026-11-01] 仓库里 3rd/WebView2/lib/ 长期只有 x86/，x64/ 是本次补齐的
# （来源 tools/probes/WebView2Loader.dll，PE machine 0x8664）。但 Loader 属于可选的
# 运行时组件，缺了不该让整个构建失败——因此加存在性判断，缺失时只告警。
set(_dui_wv2_loader "${CMAKE_SOURCE_DIR}/3rd/WebView2/lib/x64/WebView2Loader.dll")
if(EXISTS "${_dui_wv2_loader}")
    add_custom_command(TARGET DuiLib POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${_dui_wv2_loader}" "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/WebView2Loader.dll")
else()
    message(WARNING
        "[DuiLib] 未找到 3rd/WebView2/lib/x64/WebView2Loader.dll，已跳过 WebView2 Loader 部署。\n"
        "  WebView2 控件将在运行时回退（找不到 Loader 即不可用），DuiLib 其余功能不受影响。")
endif()

