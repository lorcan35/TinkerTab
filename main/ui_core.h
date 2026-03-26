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

/** Unlock the LVGL mutex. */
void tab5_ui_unlock(void);
