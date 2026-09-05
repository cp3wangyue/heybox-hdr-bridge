# ToneMap 核心库：P4 GPU Tone Mapping（计划书 §8）
add_library(tonemap STATIC
    ToneMapCore.cpp
)
target_include_directories(tonemap PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(tonemap PUBLIC d3d11 dxgi d3dcompiler captureprobe)
