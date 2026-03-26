/**
 * TinkerTab — File Browser Screen
 *
 * Browse SD card contents with folder navigation, file type icons,
 * and tap-to-open actions (audio player, image preview).
 * 720x1280 portrait, LVGL v9, dark theme with #3B82F6 accent.
 */
#pragma once
#include "lvgl.h"

/** Create and show the file browser screen. Returns the screen object. */
lv_obj_t *ui_files_create(void);

/** Delete the file browser screen and free resources. */
void ui_files_destroy(void);
