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

/** U11 photo-share follow-up: set ONCE before calling ui_camera_create
 *  to mark the next capture as "send to chat" — capture_btn_cb saves
 *  the file as usual, then hands it to voice_upload_chat_image() so
 *  Dragon broadcasts back the signed `media` event the chat overlay
 *  renders as a user image bubble.  Auto-clears after one capture so
 *  subsequent normal-camera captures don't accidentally upload. */
void ui_camera_arm_chat_share(void);
