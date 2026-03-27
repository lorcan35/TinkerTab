/**
 * TinkerTab -- WiFi Configuration Screen
 *
 * Scan, select, and connect to WiFi networks from the touchscreen.
 * Credentials saved to NVS for persistence across reboots.
 * 720x1280 portrait, LVGL v9, dark theme with #3B82F6 accent.
 */
#pragma once

#include "lvgl.h"

/** Create and show the WiFi configuration screen. Returns the screen object. */
lv_obj_t *ui_wifi_create(void);

/** Refresh scan results and connection status. */
void ui_wifi_update(void);

/** Delete the WiFi screen and free resources. */
void ui_wifi_destroy(void);
