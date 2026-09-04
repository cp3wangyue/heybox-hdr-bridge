# CaptureProbe：只读探测库 + 自检程序
add_library(captureprobe STATIC
    texture_info.cpp
    hdr_state.cpp
    probe_logger.cpp
)
# 以 "CaptureProbe/xxx.h" 方式引用，供 Hook/Integration 复用
target_include_directories(captureprobe PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(captureprobe PUBLIC d3d11 dxgi user32)

add_executable(probe_selftest selftest/probe_selftest.cpp)
target_link_libraries(probe_selftest PRIVATE captureprobe)
