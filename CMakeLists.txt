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

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tinkertab)

# #158: apply managed-component patches (LVGL bounds/NULL guards, etc.).
# Runs AFTER project(tinkertab) so the component manager has finished
# fetching managed_components/ — otherwise on a fresh clone the target
# files don't exist yet and the script skips.  We run at configure time
# AND as a PRE_BUILD custom command so any edits land before compilation
# even if someone just ran `idf.py build` without a reconfigure.
# Idempotent — sentinel string "TinkerTab #<issue>" in each patched
# file makes reapply a no-op.
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

# Belt-and-suspenders: run the script once more as a pre-build step so
# a post-fetch `idf.py build` (without `reconfigure`) still gets the patch.
add_custom_target(apply_managed_patches ALL
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tools/apply_patches.sh"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Applying managed-component patches (#158)"
    VERBATIM
)
