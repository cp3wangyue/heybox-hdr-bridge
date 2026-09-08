cmake_minimum_required(VERSION 3.24)
project(hook_stress_test VERSION 0.1.0 LANGUAGES C CXX)

add_executable(hook_stress_test
    hook_stress_test.cpp
)

target_include_directories(hook_stress_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(hook_stress_test PRIVATE
    hdrfix_hook
    onecore
    ole32
    advapi32
)
