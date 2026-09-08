# ColorDetect 库：AutoDetect 判定树
add_library(hdrfix_colordetect STATIC
    ColorDetector.cpp
)
target_include_directories(hdrfix_colordetect PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(hdrfix_colordetect PUBLIC captureprobe)
