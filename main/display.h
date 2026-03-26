/**
 * TinkerClaw Tab5 — Display driver
 *
 * MIPI DSI 720x1280 ST7123 panel on M5Stack Tab5.
 */
#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

/**
 * Initialize the MIPI DSI display.
 * Must call tab5_io_expander_init() first (handles LCD reset).
 */
esp_err_t tab5_display_init(void);

/** Fill entire screen with an RGB565 color. */
void tab5_display_fill_color(uint16_t color_rgb565);

/** Set backlight brightness (0-100%). */
esp_err_t tab5_display_set_brightness(int percent);

/** Show a status message (logged to serial). */
void tab5_display_show_status(const char *msg);

/** Show DSI test pattern (0=off, 1=vertical bars, 2=horizontal bars, 3=color bar). */
void tab5_display_test_pattern(int type);

/** Initialize hardware JPEG decoder (call once after display init). */
esp_err_t tab5_display_jpeg_init(void);

/** Decode a JPEG buffer and draw it to the DPI framebuffer. */
esp_err_t tab5_display_draw_jpeg(const uint8_t *jpeg_data, uint32_t jpeg_size);
