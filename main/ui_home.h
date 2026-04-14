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

/** Get the tileview object (for debug server navigation). */
lv_obj_t *ui_home_get_tileview(void);

/** Get a specific tile page (0=home, 1=notes, 2=chat, 3=settings). */
lv_obj_t *ui_home_get_tile(int page);

/** Navigate to settings screen (spawns background task). */
void ui_home_nav_settings(void);

/** Refresh mode badge from NVS (call after debug API mode change). */
void ui_home_refresh_mode_badge(void);
