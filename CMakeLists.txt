cmake_minimum_required(VERSION 3.24)
project(hdr_share_fix VERSION 0.1.0 LANGUAGES CXX)

# C++20 / x64 / MSVC（计划书 §3.2 推荐技术栈）
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W4 /permissive- /MP /utf-8)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
endif()

# 观察器先行（Gate P2 前只做只读探测）
add_subdirectory(src/CaptureProbe)
add_subdirectory(tools/capture_format_spy)
add_subdirectory(tools/hdr_ctl)
add_subdirectory(tools/injector)
add_subdirectory(tests/ToneMapHarness)

# 以下子目录在对应 Gate 通过后加入构建（当前为占位说明）：
#   src/Hook          — Gate P2/P3 之后
#   src/ToneMap       — Gate P2 之后（P4）
#   src/Integration   — Gate P3/P4 之后（P5）
