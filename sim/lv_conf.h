/**
 * sim/lv_conf.h — LVGL 9.2.x configuration for desktop SDL2 simulator
 *
 * Based on main/lv_conf.h but with:
 *   - LV_USE_SDL 1 (SDL2 display + input driver)
 *   - Standard malloc (no heap_caps)
 *   - Standard tick (no esp_timer)
 *   - 4MB LVGL heap
 */

#ifndef LV_CONF_H
#define LV_CONF_H

/* Color depth */
#define LV_COLOR_DEPTH 16  /* RGB565 */

/* ---- Memory ---- */
/* Use built-in allocator with 4MB pool */
#define LV_USE_STDLIB_MALLOC    LV_STDLIB_BUILTIN
#define LV_MEM_SIZE             (4 * 1024 * 1024U)

/* Resolution hints (actual display set at runtime) */
#define LV_HOR_RES_MAX 720
#define LV_VER_RES_MAX 1280

/* ---- SDL2 display + input driver ---- */
#define LV_USE_SDL 1

/* ---- Logging ---- */
#define LV_USE_LOG       1
#define LV_LOG_LEVEL     LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF    1

/* ---- Drawing ---- */
#define LV_USE_DRAW_SW 1

/* ---- String/sprintf ---- */
#define LV_USE_STDLIB_STRING    LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_SPRINTF   LV_STDLIB_BUILTIN

/* ---- Theme ---- */
#define LV_USE_THEME_DEFAULT 1

/* ---- Fonts (same as firmware) ---- */
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_36 1
#define LV_FONT_MONTSERRAT_48 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

/* ---- Widgets ---- */
#define LV_USE_ARC       1
#define LV_USE_BAR       1
#define LV_USE_BTN       1
#define LV_USE_BUTTON    1
#define LV_USE_BTNMATRIX 1
#define LV_USE_CANVAS    1
#define LV_USE_CHECKBOX  1
#define LV_USE_DROPDOWN  1
#define LV_USE_IMG       1
#define LV_USE_IMAGE     1
#define LV_USE_LABEL     1
#define LV_USE_LINE      1
#define LV_USE_ROLLER    1
#define LV_USE_SCALE     1
#define LV_USE_SLIDER    1
#define LV_USE_SWITCH    1
#define LV_USE_TABLE     1
#define LV_USE_TEXTAREA  1

/* ---- Extra widgets ---- */
#define LV_USE_CHART     1
#define LV_USE_KEYBOARD  1
#define LV_USE_MSGBOX    1
#define LV_USE_SPINNER   1
#define LV_USE_TABVIEW   1

/* ---- Layouts ---- */
#define LV_USE_FLEX 1
#define LV_USE_GRID 1

/* ---- Animations ---- */
#define LV_USE_ANIM 1

/* ---- OS ---- */
#define LV_USE_OS LV_OS_NONE

#endif /* LV_CONF_H */
