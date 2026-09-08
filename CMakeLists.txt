cmake_minimum_required(VERSION 3.24)
project(hdr_share_fix VERSION 0.1.0 LANGUAGES C CXX)

# C++20 / x64 / MSVC
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W4 /permissive- /MP /utf-8)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
endif()

# 核心模块：诊断、探测、色调映射、Hook、色彩检测、集成
add_subdirectory(src/Diagnostics)
add_subdirectory(src/CaptureProbe)
add_subdirectory(src/ToneMap)
add_subdirectory(src/Hook)
add_subdirectory(src/ColorDetect)
add_subdirectory(src/Integration)
add_subdirectory(tools/capture_format_spy)
add_subdirectory(tools/hdr_ctl)
add_subdirectory(tools/injector)
add_subdirectory(tests/ToneMapHarness)
add_subdirectory(tests/HookStressTest)
add_subdirectory(tests/CompatTest)
add_subdirectory(src/Bootstrap)

