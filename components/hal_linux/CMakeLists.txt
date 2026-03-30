# hal_linux — ESP-IDF component for Linux target only.
#
# Provides:
#   - Stub implementations of all hardware APIs (camera, audio, touch, etc.)
#   - SDL2 display backend via LVGL's lv_sdl_window driver
#   - LVGL init + event loop (ui_core_linux.c replacing ui_core.c)
#
# Only compiled when IDF_TARGET == linux.
# On ESP32-P4, the real drivers in main/ are used instead.

if(NOT IDF_TARGET STREQUAL "linux")
    # Not needed on hardware targets — register as empty component
    idf_component_register()
    return()
endif()

# ── SDL2 via pkg-config ────────────────────────────────────────────────
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)

# ── Register IDF component ─────────────────────────────────────────────
idf_component_register(
    SRCS
        "stubs.c"
        "ui_core_linux.c"
    INCLUDE_DIRS
        "include"          # stub headers (esp_lcd_panel_ops.h, driver/i2c_master.h, etc.)
    PRIV_INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}/../../main"  # access main/'s hardware .h files
        "${SDL2_INCLUDE_DIRS}"
    PRIV_REQUIRES
        "lvgl" "freertos" "log" "esp_timer" "esp_system"
)

# ── Link SDL2 to the component and expose to consumers ─────────────────
target_include_directories(${COMPONENT_LIB} PRIVATE ${SDL2_INCLUDE_DIRS})
target_link_libraries(${COMPONENT_LIB} INTERFACE ${SDL2_LIBRARIES})

message(STATUS "hal_linux: SDL2 found at ${SDL2_INCLUDE_DIRS}")
