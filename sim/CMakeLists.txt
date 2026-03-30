cmake_minimum_required(VERSION 3.16)
project(tinkeros_sim C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_BUILD_TYPE Debug)

# ── SDL2 ──────────────────────────────────────────────────────────────────
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)

# ── LVGL — use the copy already in our project (same version as firmware) ──
set(LVGL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../managed_components/lvgl__lvgl")

# Enable SDL2 driver in LVGL
set(LV_CONF_INCLUDE_SIMPLE OFF)
set(LV_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h")

# Collect LVGL sources (core + SDL2 driver)
file(GLOB_RECURSE LVGL_SRCS
    "${LVGL_DIR}/src/*.c"
)
# Exclude platform-specific drivers we don't need
list(FILTER LVGL_SRCS EXCLUDE REGEX ".*/(evdev|wayland|x11|glfw|libinput|qnx|nuttx|windows)/.*")
list(FILTER LVGL_SRCS EXCLUDE REGEX ".*/lv_draw_vglite.*")
list(FILTER LVGL_SRCS EXCLUDE REGEX ".*/lv_draw_ppa.*")
list(FILTER LVGL_SRCS EXCLUDE REGEX ".*/lv_draw_dave2d.*")

# ── UI source files (shared with firmware) ────────────────────────────────
set(UI_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/../main/ui_splash.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../main/ui_home.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../main/ui_chat.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../main/ui_camera.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../main/ui_settings.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../main/ui_files.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../main/ui_audio.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../main/ui_voice.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../main/ui_wifi.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../main/ui_keyboard.c"
)

# ── Simulator-specific sources ─────────────────────────────────────────────
set(SIM_SRCS
    "${CMAKE_CURRENT_SOURCE_DIR}/main.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui_core_sim.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs.c"
)

# ── Build target ───────────────────────────────────────────────────────────
add_executable(tinkeros_sim
    ${SIM_SRCS}
    ${UI_SRCS}
    ${LVGL_SRCS}
)

# ── Include paths ──────────────────────────────────────────────────────────
target_include_directories(tinkeros_sim PRIVATE
    # 1. Sim-specific headers (ESP-IDF stubs, lv_conf.h) — FIRST
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/port"
    "${CMAKE_CURRENT_SOURCE_DIR}/port/freertos"
    "${CMAKE_CURRENT_SOURCE_DIR}/port/driver"
    # 2. LVGL headers
    "${LVGL_DIR}"
    "${LVGL_DIR}/src"
    # 3. Our project headers (ui_*.h, config.h, hardware headers)
    "${CMAKE_CURRENT_SOURCE_DIR}/../main"
    # 4. SDL2
    ${SDL2_INCLUDE_DIRS}
)

# ── Compiler flags ─────────────────────────────────────────────────────────
target_compile_definitions(tinkeros_sim PRIVATE
    LV_CONF_INCLUDE_SIMPLE=0
    LV_USE_SDL=1
    TINKEROS_SIMULATOR=1
)

target_compile_options(tinkeros_sim PRIVATE
    -Wall
    -Wno-unused-variable
    -Wno-unused-function
    -Wno-unused-parameter
    -Wno-missing-field-initializers
    -g
)

# ── Link libraries ─────────────────────────────────────────────────────────
target_link_libraries(tinkeros_sim PRIVATE
    ${SDL2_LIBRARIES}
    m
    pthread
)

# ── Output ─────────────────────────────────────────────────────────────────
set_target_properties(tinkeros_sim PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
)

message(STATUS "TinkerOS Simulator build configured")
message(STATUS "  LVGL: ${LVGL_DIR}")
message(STATUS "  SDL2: ${SDL2_INCLUDE_DIRS}")
message(STATUS "  UI sources: ${UI_SRCS}")
