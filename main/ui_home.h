#pragma once
#include "lvgl.h"

/** Create and show the home screen. Returns the screen object. */
lv_obj_t *ui_home_create(void);

/** Update the status bar (call periodically). */
void ui_home_update_status(void);

/** Delete the home screen. */
void ui_home_destroy(void);

/** Return the existing home screen object (for navigation back). */
lv_obj_t *ui_home_get_screen(void);

/** Navigate tileview to page 0 (Home) and update nav/floating buttons. */
void ui_home_go_home(void);
