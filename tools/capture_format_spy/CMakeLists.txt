# 只读系统级捕获格式探测（独立进程，不注入）
add_executable(capture_format_spy spy_main.cpp)
target_link_libraries(capture_format_spy PRIVATE d3d11 dxgi user32 windowsapp)
