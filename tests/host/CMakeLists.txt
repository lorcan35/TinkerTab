cmake_minimum_required(VERSION 3.16)
project(tinkertab_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Werror -Wno-unused-parameter -O0 -g)

enable_testing()

set(MAIN_DIR ${CMAKE_SOURCE_DIR}/../../main)

# md_strip — W9-A first slice.  Plain C, no ESP-IDF deps.
add_executable(test_md_strip
    test_md_strip.c
    ${MAIN_DIR}/md_strip.c
)
target_include_directories(test_md_strip PRIVATE ${MAIN_DIR})
add_test(NAME md_strip COMMAND test_md_strip)
