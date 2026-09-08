# ToneMap 核心库：GPU Tone Mapping
add_library(tonemap STATIC
    ToneMapCore.cpp
)
target_include_directories(tonemap PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(tonemap PUBLIC d3d11 dxgi d3dcompiler captureprobe hdrfix_diagnostics)
