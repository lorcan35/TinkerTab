cmake_minimum_required(VERSION 3.16)
project(tinkertab_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Werror -Wno-unused-parameter -O0 -g)

enable_testing()

set(MAIN_DIR ${CMAKE_SOURCE_DIR}/../../main)
set(SHIM_DIR ${CMAKE_SOURCE_DIR}/shim)

# md_strip — W9-A first slice.  Plain C, no ESP-IDF deps.
add_executable(test_md_strip
    test_md_strip.c
    ${MAIN_DIR}/md_strip.c
)
target_include_directories(test_md_strip PRIVATE ${MAIN_DIR})
add_test(NAME md_strip COMMAND test_md_strip)

# openrouter_sse — W9-A slice 2.  Pulls in ESP-IDF heap + log shims.
# Tests run quietly via -DHOST_LOG_QUIET.
add_executable(test_openrouter_sse
    test_openrouter_sse.c
    ${MAIN_DIR}/openrouter_sse.c
)
target_include_directories(test_openrouter_sse PRIVATE ${SHIM_DIR} ${MAIN_DIR})
target_compile_definitions(test_openrouter_sse PRIVATE HOST_LOG_QUIET=1)
# When HOST_LOG_QUIET is on, module-level static `TAG` strings become
# unused; suppress just that warning here without weakening -Werror
# elsewhere.
target_compile_options(test_openrouter_sse PRIVATE -Wno-unused-variable -Wno-unused-const-variable)
add_test(NAME openrouter_sse COMMAND test_openrouter_sse)
