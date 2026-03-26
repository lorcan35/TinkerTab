/**
 * TinkerTab — Camera viewfinder screen
 *
 * Full-screen live preview with capture controls.
 * 720x1280 portrait, LVGL v9, dark theme.
 */
#pragma once
#include "lvgl.h"

/** Create and show the camera viewfinder screen. Returns the screen object. */
lv_obj_t *ui_camera_create(void);

/** Delete the camera screen, stop preview timer, free canvas buffer. */
void ui_camera_destroy(void);
