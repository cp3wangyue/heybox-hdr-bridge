# Hook 基础设施库 (基于成熟 x64 MinHook 引擎)
add_library(hdrfix_hook STATIC
    inline_hook.cpp
    minhook/src/buffer.c
    minhook/src/hook.c
    minhook/src/trampoline.c
    minhook/src/hde/hde32.c
    minhook/src/hde/hde64.c
)

target_include_directories(hdrfix_hook PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/minhook/include
)

if(MSVC)
    # MinHook C 源文件放宽部分 C 语法警告
    set_source_files_properties(
        minhook/src/buffer.c
        minhook/src/hook.c
        minhook/src/trampoline.c
        minhook/src/hde/hde32.c
        minhook/src/hde/hde64.c
        PROPERTIES COMPILE_FLAGS "/wd4100 /wd4706 /wd4206 /wd4201 /wd4310 /wd4244 /wd4701"
    )
endif()
