# src/Bootstrap/CMakeLists.txt — P8 交付产物与加载器构建

# 核心插件动态链接库 (hdrfix.dll)
add_library(hdrfix SHARED dllmain.cpp)
target_link_libraries(hdrfix PRIVATE
    hdrfix_integration
    hdrfix_diagnostics
    hdrfix_colordetect
    tonemap
    captureprobe
    d3d11
    dxgi
    d3dcompiler
    windowsapp
)

# 简易安装、卸载与伴随加载器 (hdrfix_loader.exe)
add_executable(hdrfix_loader loader.cpp)
target_link_libraries(hdrfix_loader PRIVATE
    ole32
    shell32
)
