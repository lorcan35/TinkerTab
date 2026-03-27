/**
 * TinkerTab — HTTP Debug Server
 *
 * Lightweight HTTP server on port 8080 providing:
 *   GET  /             — Interactive HTML remote control page
 *   GET  /screenshot   — Framebuffer capture as BMP
 *   GET  /screenshot.bmp — Same, explicit BMP
 *   GET  /info         — Device info JSON
 *   POST /touch        — Inject touch event
 *   POST /reboot       — Restart device
 *   GET  /log          — Heap and task info
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * Start the debug HTTP server.
 * Call after WiFi is connected and LVGL is initialized.
 * Logs the URL to the console on success.
 */
esp_err_t tab5_debug_server_init(void);

/**
 * Check if the debug server is injecting a touch event.
 * Call from the LVGL touch read callback. If true, use the returned
 * coordinates instead of the physical touch controller.
 *
 * @param[out] x       Touch X coordinate
 * @param[out] y       Touch Y coordinate
 * @param[out] pressed true = pressed, false = released
 * @return true if an override is active (use the output values)
 */
bool tab5_debug_touch_override(int32_t *x, int32_t *y, bool *pressed);
