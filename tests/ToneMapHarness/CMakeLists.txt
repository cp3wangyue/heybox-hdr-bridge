# ToneMapHarness：P4 独立算法测试台（Gate P2 前仅做环境记录与 shader 编译检查）
add_executable(tone_map_harness main.cpp)
target_link_libraries(tone_map_harness PRIVATE captureprobe d3d11 dxgi d3dcompiler)
