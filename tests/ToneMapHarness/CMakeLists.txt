# ToneMapHarness：P4 独立算法测试台
add_executable(tone_map_harness main.cpp)
target_link_libraries(tone_map_harness PRIVATE tonemap captureprobe d3d11 dxgi d3dcompiler)
