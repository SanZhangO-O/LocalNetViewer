cmake_minimum_required(VERSION 3.24)
project(LocalNetViewerServerCpp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

if(MSVC)
    add_compile_options(/W3 /permissive- /Zc:__cplusplus /utf-8 /bigobj)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS -DNOMINMAX -DWIN32_LEAN_AND_MEAN)
endif()

# ---------------------------------------------------------------- third-party
set(THIRDPARTY ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty)

# yaml-cpp (config.yaml)
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(YAML_CPP_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(${THIRDPARTY}/yaml-cpp yaml-cpp-build)

# pugixml (EPUB/FB2 XML)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(${THIRDPARTY}/pugixml pugixml-build)

# freetype (字体光栅化，含 CFF/OTF 中文字形)
set(FT_DISABLE_ZLIB TRUE CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 TRUE CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG TRUE CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ TRUE CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI TRUE CACHE BOOL "" FORCE)
add_subdirectory(${THIRDPARTY}/freetype freetype-build)

# miniz（新版分体源：common/tdef/infl/zip；zip API 在 miniz_zip.c）
#
# miniz.h 无条件 #include "miniz_export.h"，而上游仓库里**没有**这个文件（由 miniz 自己的
# CMake 用 configure_file 生成）。我们直接编译源码，因此必须自己生成：静态库场景下
# MINIZ_EXPORT 留空即可。少了这一步，干净构建会直接 C1083 失败（此前能构建成功只是因为
# build/ 里残留了上一次生成的同名文件 —— 换台机器/清一次 build 就崩）。
file(WRITE ${CMAKE_BINARY_DIR}/miniz_export.h
"/* 由 LocalNetViewer 的 CMakeLists.txt 生成（静态 miniz，无导出宏） */\n#ifndef MINIZ_EXPORT\n#define MINIZ_EXPORT\n#endif\n")
add_library(miniz STATIC
    ${THIRDPARTY}/miniz/miniz.c
    ${THIRDPARTY}/miniz/miniz_tdef.c
    ${THIRDPARTY}/miniz/miniz_tinfl.c
    ${THIRDPARTY}/miniz/miniz_zip.c)
target_include_directories(miniz PUBLIC ${THIRDPARTY}/miniz ${CMAKE_BINARY_DIR})

# header-only
add_library(lnv_headers INTERFACE)
target_include_directories(lnv_headers INTERFACE
    ${THIRDPARTY}/cpp-httplib
    ${THIRDPARTY}/json/single_include
    ${THIRDPARTY}/stb
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# ---------------------------------------------------------------- server lib
file(GLOB_RECURSE LNV_SOURCES CONFIGURE_DEPENDS src/lnv/*.cpp)
add_library(lnv_core STATIC ${LNV_SOURCES})
# windowscodecs/ole32：imaging.cpp 的 WIC 兜底解码（stb 不支持 WebP/TIFF/AVIF）
# bcrypt：util/md5.cpp —— Starlette FileResponse 的 ETag 是 md5(f"{mtime}-{size}")，
#         /render/fonts/{id}/file 要与 Python 逐字一致
target_link_libraries(lnv_core PUBLIC lnv_headers yaml-cpp pugixml freetype miniz
                                  windowscodecs ole32 bcrypt)
target_precompile_headers(lnv_core PRIVATE src/lnv/pch.h)

# ---------------------------------------------------------------- executable
add_executable(lnvserver src/main.cpp)
target_link_libraries(lnvserver PRIVATE lnv_core ws2_32)
