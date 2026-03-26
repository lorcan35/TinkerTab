#pragma once
#include "lvgl.h"

/** Create and show the home screen. Returns the screen object. */
lv_obj_t *ui_home_create(void);

/** Update the status bar (call periodically). */
void ui_home_update_status(void);

/** Delete the home screen. */
void ui_home_destroy(void);
