# 注入器：本地研究加载器（仅本地加载；不做持久化与隐藏）
add_executable(injector injector.cpp)
target_link_libraries(injector PRIVATE user32)

# 探针管线测试宿主（不接触客户端）
add_executable(probe_testhost ${CMAKE_CURRENT_SOURCE_DIR}/../probe_testhost/probe_testhost.cpp)
target_link_libraries(probe_testhost PRIVATE hdrfix_integration hdrfix_diagnostics hdrfix_colordetect tonemap captureprobe d3d11 dxgi windowsapp)
