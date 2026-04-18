/**
 * TinkerTab — Settings Screen
 *
 * Full settings UI with display, network, storage, battery, and about sections.
 * 720x1280 portrait, LVGL v9, dark theme with #3B82F6 accent.
 */
#pragma once

#include "lvgl.h"
#include <stdbool.h>

/** Create and show the settings screen. Returns the screen object. */
lv_obj_t *ui_settings_create(void);

/** True if the settings overlay exists and is not hidden. */
bool ui_settings_is_visible(void);

/** Refresh live values (battery, heap, PSRAM). Call periodically. */
void ui_settings_update(void);

/** Delete the settings screen and free resources. */
void ui_settings_destroy(void);
