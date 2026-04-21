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
