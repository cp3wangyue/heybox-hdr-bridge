# CaptureProbe：只读探测库 + 自检程序
add_library(captureprobe STATIC
    texture_info.cpp
    hdr_state.cpp
    hdr_control.cpp
    probe_logger.cpp
)
# 以 "CaptureProbe/xxx.h" 方式引用，供 Hook/Integration 复用
target_include_directories(captureprobe PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(captureprobe PUBLIC d3d11 dxgi user32)

add_executable(probe_selftest selftest/probe_selftest.cpp)
target_link_libraries(probe_selftest PRIVATE captureprobe)

# P2 只读探针 DLL（注入到客户端进程，只观察不修改帧）
# 探针迭代期间旧版本可能仍被目标进程加载锁定，因此允许用版本化文件名构建：
#   cmake -DPROBE_VARIANT=2  ->  hdrfix_probe2.dll
set(HDRFIX_PROBE_VARIANT "" CACHE STRING "probe dll 文件名后缀（避免旧实例文件锁）")
add_library(hdrfix_probe SHARED probe_dll.cpp d3d11_observer.cpp iat_hook.cpp)
target_link_libraries(hdrfix_probe PRIVATE captureprobe)
if(HDRFIX_PROBE_VARIANT)
    set_target_properties(hdrfix_probe PROPERTIES OUTPUT_NAME "hdrfix_probe${HDRFIX_PROBE_VARIANT}")
endif()

target_compile_definitions(hdrfix_probe PRIVATE UNICODE _UNICODE)

