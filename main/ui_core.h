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

/** Get LVGL rendering FPS (flush callbacks per second). Updated every 1s. */
uint32_t ui_core_get_fps(void);

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
