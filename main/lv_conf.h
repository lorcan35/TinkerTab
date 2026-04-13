/**
 * TinkerTab — LVGL v9 Configuration
 *
 * Configured for M5Stack Tab5 (ESP32-P4):
 *   - 720x1280 RGB565 MIPI-DSI display
 *   - PSRAM for draw buffers and large allocations
 *   - Custom tick via esp_timer
 */
#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/*====================
   COLOR SETTINGS
 *====================*/
#define LV_COLOR_DEPTH 16  /* RGB565 */

/*====================
   MEMORY SETTINGS
 *====================*/
#define LV_MEM_CUSTOM 1
#define LV_MEM_CUSTOM_INCLUDE "esp_heap_caps.h"
/* Robust allocator: PSRAM first, then internal RAM, then DEFAULT.
 * LVGL font glyph rendering MUST NOT fail or the device hangs/crashes.
 * The assert handler can't save us if draw_letter gets a NULL draw_buf. */
static inline void *_lv_robust_alloc(size_t size) {
    if (size == 0) return NULL;
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (p) return p;
    /* PSRAM failed — try any available RAM */
    p = heap_caps_malloc(size, MALLOC_CAP_DEFAULT);
    return p;
}
static inline void *_lv_robust_realloc(void *ptr, size_t size) {
    if (size == 0) { heap_caps_free(ptr); return NULL; }
    void *p = heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM);
    if (p) return p;
    p = heap_caps_realloc(ptr, size, MALLOC_CAP_DEFAULT);
    return p;
}
#define LV_MEM_CUSTOM_ALLOC(size)   _lv_robust_alloc(size)
#define LV_MEM_CUSTOM_FREE(p)       heap_caps_free(p)
#define LV_MEM_CUSTOM_REALLOC(p, s) _lv_robust_realloc(p, s)

/*====================
   DISPLAY SETTINGS
 *====================*/
#define LV_HOR_RES_MAX 720
#define LV_VER_RES_MAX 1280

/*====================
   TICK
 *====================*/
#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE "esp_timer.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR ((uint32_t)(esp_timer_get_time() / 1000))

/*====================
   LOGGING
 *====================*/
#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 1

/*====================
   DRAWING
 *====================*/
#define LV_USE_DRAW_SW 1
/* Circle cache for anti-aliased radius masks. Default 4 is way too small.
 * Notes screen: 7 cards (radius 12+8) = ~14 entries. Delete dialog adds 3 more.
 * Edit overlay adds 3 more. Settings has ~10. Overlays stack on existing screen.
 * 32 entries = ~4KB — handles any realistic screen combination. */
#define LV_DRAW_SW_CIRCLE_CACHE_SIZE 32

/*====================
   THEME
 *====================*/
#define LV_USE_THEME_DEFAULT 1

/*====================
   FONTS
 *====================*/
#define LV_FONT_MONTSERRAT_8  0
#define LV_FONT_MONTSERRAT_10 0
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_22 0
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_26 0
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_30 0
#define LV_FONT_MONTSERRAT_32 0
#define LV_FONT_MONTSERRAT_34 0
#define LV_FONT_MONTSERRAT_36 1
#define LV_FONT_MONTSERRAT_38 0
#define LV_FONT_MONTSERRAT_40 0
#define LV_FONT_MONTSERRAT_42 0
#define LV_FONT_MONTSERRAT_44 0
#define LV_FONT_MONTSERRAT_46 0
#define LV_FONT_MONTSERRAT_48 1

#define LV_FONT_DEFAULT &lv_font_montserrat_14

/*====================
   WIDGETS
 *====================*/
#define LV_USE_ARC      1
#define LV_USE_BAR      1
#define LV_USE_BTN      1
#define LV_USE_BTNMATRIX 1
#define LV_USE_CANVAS   0
#define LV_USE_CHECKBOX 1
#define LV_USE_DROPDOWN 1
#define LV_USE_IMG      1
#define LV_USE_LABEL    1
#define LV_USE_LINE     1
#define LV_USE_ROLLER   1
#define LV_USE_SCALE    1
#define LV_USE_SLIDER   1
#define LV_USE_SWITCH   1
#define LV_USE_TABLE    1
#define LV_USE_TEXTAREA 1

/*====================
   EXTRA WIDGETS
 *====================*/
#define LV_USE_CHART    1
#define LV_USE_KEYBOARD 1
#define LV_USE_MSGBOX   1
#define LV_USE_SPINNER  1
#define LV_USE_TABVIEW  1

/*====================
   LAYOUTS
 *====================*/
#define LV_USE_FLEX 1
#define LV_USE_GRID 1

/*====================
   OTHERS
 *====================*/
#define LV_USE_SNAPSHOT  0
#define LV_USE_MONKEY    0
#define LV_USE_PROFILER  0
#define LV_USE_SYSMON    0

/*====================
   ASSERT HANDLER
 *====================*/
/* Default LVGL assert is while(1) which hangs the device forever.
 * Override to log + return so the device stays alive on malloc failure. */
#define LV_ASSERT_HANDLER do { \
    LV_LOG_ERROR("LVGL ASSERT FAILED"); \
    return; \
} while(0)

/* Required by LVGL build system */
#define LV_USE_STDLIB_MALLOC    LV_STDLIB_CUSTOM
#define LV_USE_STDLIB_STRING    LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_SPRINTF   LV_STDLIB_BUILTIN

#endif /* LV_CONF_H */
