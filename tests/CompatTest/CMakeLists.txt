cmake_minimum_required(VERSION 3.24)
project(compat_test VERSION 0.1.0 LANGUAGES C CXX)

add_executable(compat_test
    compat_test.cpp
)

target_include_directories(compat_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(compat_test PRIVATE
    hdrfix_diagnostics
    onecore
    ole32
    advapi32
)
