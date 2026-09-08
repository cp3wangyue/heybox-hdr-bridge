# Integration 库：帧替换与编码前链路接入
add_library(hdrfix_integration STATIC
    FramePoolProxy.cpp
    WgcHookManager.cpp
)
target_include_directories(hdrfix_integration PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(hdrfix_integration PUBLIC
    hdrfix_hook
    tonemap
    captureprobe
    hdrfix_diagnostics
    hdrfix_colordetect
    d3d11
    dxgi
    windowsapp
)
