# Hook 基础设施库
add_library(hdrfix_hook STATIC
    inline_hook.cpp
)
target_include_directories(hdrfix_hook PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
