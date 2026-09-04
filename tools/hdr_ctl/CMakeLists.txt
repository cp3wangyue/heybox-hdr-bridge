# 系统 HDR 查询/开关控制（P0 A/B 自动化）
add_executable(hdr_ctl hdr_ctl.cpp)
target_link_libraries(hdr_ctl PRIVATE captureprobe d3d11 dxgi user32)
