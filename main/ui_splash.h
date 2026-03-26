#pragma once
#include "lvgl.h"

/** Create and show the boot splash screen. Returns the screen object. */
lv_obj_t *ui_splash_create(void);

/** Update splash status text (e.g., "Connecting to WiFi..."). */
void ui_splash_set_status(const char *text);

/** Update splash progress bar (0-100). */
void ui_splash_set_progress(int percent);

/** Delete the splash screen (transition to home). */
void ui_splash_destroy(void);
