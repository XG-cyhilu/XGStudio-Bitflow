cmake_minimum_required(VERSION 3.20)
project(Bitflow VERSION 1.0.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set(XGDL_BUILD_DEMO OFF CACHE BOOL "" FORCE)
add_subdirectory(vendor/xgdl)
add_subdirectory(vendor/core)
include(cmake/embed_web_assets.cmake)
bitflow_embed_assets("${CMAKE_CURRENT_SOURCE_DIR}/web" "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_executable(Bitflow WIN32
    src/main.cpp
    src/bridge.cpp
    src/settings.cpp
    src/terminal.cpp
    src/bridge.h
    src/settings.h
    src/terminal.h
    src/json.h
    src/text.h
    ${BF_WEB_RC}
)
target_include_directories(Bitflow PRIVATE
    src
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
    vendor/webview2/include
)
target_link_libraries(Bitflow PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/webview2/lib/x64/WebView2LoaderStatic.lib"
    XGDL::xgdl
    XGCFG::xgcfg
    XGCRYPTO::xgcrypto
    XGFS::xgfs
    XGLOG::xglog
    XGNET::xgnet
    XGPROC::xgproc
    XGSECURE::xgsecure
    XGZIP::xgzip
    dwmapi shell32 shlwapi ole32 oleaut32 user32 advapi32
)
target_compile_definitions(Bitflow PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS
)
target_compile_options(Bitflow PRIVATE /utf-8 /W3 /permissive-)
set_target_properties(Bitflow PROPERTIES
    OUTPUT_NAME "Bitflow"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
)
