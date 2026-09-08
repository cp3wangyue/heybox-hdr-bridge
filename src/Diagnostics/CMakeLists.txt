# Diagnostics 基础设施库（配置、安全防护与版本兼容体系）
add_library(hdrfix_diagnostics STATIC
    BridgeLog.cpp
    ConfigManager.cpp
    SafetyGuard.cpp
    CompatibilityManager.cpp
)
target_include_directories(hdrfix_diagnostics PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(hdrfix_diagnostics PUBLIC version onecore)
