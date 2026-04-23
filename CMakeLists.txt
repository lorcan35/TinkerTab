cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/bsp")

# Wave 14 W14-C04: merge sdkconfig.local on top of sdkconfig.defaults so
# dev-only overrides (WiFi creds, DRAGON_TOKEN) aren't required to be in
# the committed file. sdkconfig.local is gitignored.  The default IDF
# search path does NOT auto-include sdkconfig.local on modern toolchains,
# so we have to name it explicitly.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.local")
    set(SDKCONFIG_DEFAULTS
        "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.defaults"
        "${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.local"
    )
endif()

# #158: apply third-party managed-component patches before the build
# reads any of their source (bounds-check on LVGL's get_next_line, etc.).
# Idempotent — a sentinel string in each patched file skips reapply.
execute_process(
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tools/apply_patches.sh"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    RESULT_VARIABLE _patch_result
    OUTPUT_VARIABLE _patch_output
    ERROR_VARIABLE  _patch_output
)
message(STATUS "${_patch_output}")
if(NOT _patch_result EQUAL 0)
    message(FATAL_ERROR "apply_patches.sh failed (rc=${_patch_result}) — aborting build")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tinkertab)
