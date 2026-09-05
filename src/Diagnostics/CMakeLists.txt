# Diagnostics 基础设施库（计划书 §10.2 / §10.3）
add_library(hdrfix_diagnostics STATIC
    ConfigManager.cpp
    SafetyGuard.cpp
)
target_include_directories(hdrfix_diagnostics PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
