/**
 * TinkerTab — LVGL UI Core
 *
 * Integrates LVGL v9 with the Tab5 display (DPI framebuffer) and
 * ST7123 capacitive touch. All UI screens build on top of this layer.
 */
#pragma once

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"

/**
 * Initialize LVGL, display driver, and touch input.
 * Call after tab5_display_init() and tab5_touch_init() have completed.
 *
 * @param panel  DPI panel handle (from tab5_display_get_panel())
 * @return ESP_OK on success
 */
esp_err_t tab5_ui_init(esp_lcd_panel_handle_t panel);

/**
 * Must be called periodically (e.g., every 5ms) to drive LVGL timer handler.
 * If ui_core was started with its own FreeRTOS task, this is called automatically.
 */
void tab5_ui_tick(void);

/** Get the active LVGL display (for screen management). */
lv_display_t *tab5_ui_get_display(void);

/** Lock the LVGL mutex (for thread-safe UI updates from other tasks). */
void tab5_ui_lock(void);

/** Try to lock the LVGL mutex with timeout (ms). Returns true if locked. */
bool tab5_ui_try_lock(uint32_t timeout_ms);

/** Unlock the LVGL mutex. */
void tab5_ui_unlock(void);

/**
 * Thread-safe wrapper around lv_async_call (#256/#258).
 *
 * LVGL 9.x's lv_async_call internally calls lv_malloc + lv_timer_create
 * against the unprotected TLSF heap.  Calling it from a non-LVGL thread
 * (worker task, voice WS handler, HTTP callback, etc.) races ui_task's
 * draw-task allocations and eventually corrupts a free-list pointer →
 * search_suitable_block infinite loop → TASK_WDT.
 *
 * This wrapper takes the recursive LVGL mutex around the call.  Callers
 * already holding the mutex (typical for LVGL event handlers) re-enter
 * harmlessly.  Use this in place of lv_async_call everywhere.
 */
lv_result_t tab5_lv_async_call(lv_async_cb_t cb, void *user_data);

/** Get LVGL rendering FPS (flush callbacks per second). Updated every 1s. */
uint32_t ui_core_get_fps(void);

/* TT #328 Wave 5 — universal tap-debounce gate.
 *
 *   if (!ui_tap_gate("chat:back", 300)) return;
 *
 * Returns true if this tap should fire, false if it should be swallowed
 * because the same `site` fired within `ms` milliseconds.  Per-site
 * cooldown so independent buttons don't share the same debounce window.
 * Pre-Wave-5 only the home orb (ui_home.c:1342) and /navigate had any
 * debounce — every other clickable was vulnerable to repeat-tap stacking
 * (overlapping create/dismiss → SDIO TX copy_buff exhaustion).
 *
 * Site names are short string literals (compared by pointer via interning
 * — pass the same string literal everywhere; we store char* not strdup).
 * Up to UI_TAP_GATE_SITES sites tracked; older entries evict on overflow.
 * Thread-safe via the LVGL mutex (callers must already be on the LVGL
 * thread, which all CLICKED handlers are).
 */
bool ui_tap_gate(const char *site, int ms);

/* Audit U2 (TinkerTab #206): auto-rotate plumbing.
 *
 *   ui_core_apply_auto_rotation(true) — start the IMU poll timer (1 Hz) +
 *      apply current orientation immediately.  Persisted toggle state
 *      lives in NVS key "auto_rot" (settings.h).
 *   ui_core_apply_auto_rotation(false) — snap back to portrait + leave
 *      the timer running idle (cheap; checks NVS each tick).
 *   ui_core_init_auto_rotation_from_nvs() — call once after the display
 *      is alive so the persisted preference takes effect at boot.
 *
 * Touch coordinates are flipped automatically inside touch_read_cb when
 * the display rotation is 180°; debug-server /touch injection is in
 * display-space (post-rotation) and is NOT flipped.
 */
void ui_core_apply_auto_rotation(bool enabled);
void ui_core_init_auto_rotation_from_nvs(void);
