/*
 * ui_video_pane.h — full-screen video pane that displays the latest
 * Dragon-sent JPEG frame (#268 / Phase 3B) + optional in-call UI
 * (#270 / Phase 3D: local-camera PIP + End-call button).
 *
 * Owned by voice_video.c::voice_video_on_downlink_frame which feeds
 * fresh frames via ui_video_pane_set_dsc on the LVGL thread.
 *
 * Two open modes:
 *   ui_video_pane_show()       — basic playback pane.  Tap-to-dismiss.
 *   ui_video_pane_show_call()  — call mode.  Adds a small local-camera
 *                                 PIP in the bottom-right + an End Call
 *                                 button bottom-left.  Tap-to-dismiss
 *                                 disabled (only the End Call button
 *                                 closes; the user is "in a call").
 *
 * Both modes share the same hide path.  On hide, voice_video also
 * stops outbound streaming (see voice_video_end_call).
 */
#pragma once

#include "lvgl.h"

/* Open the basic playback pane.  Idempotent. */
void ui_video_pane_show(void);

/* Open the in-call pane: full-screen remote feed + local-camera PIP +
 * End Call button.  Spawns its own 5fps lv_timer for the local PIP
 * (calls tab5_camera_capture, applies cam_rot, blits a downscaled
 * RGB565 into a small canvas).  Idempotent — switching from basic
 * mode upgrades in place. */
void ui_video_pane_show_call(void);

/* Hide + free the pane.  Safe even if not open.  When in call mode
 * also kills the local-PIP timer.  Does NOT call voice_video_stop_*
 * — that's the caller's job (voice_video_end_call wraps both). */
void ui_video_pane_hide(void);

/* True iff the pane is currently visible. */
bool ui_video_pane_is_visible(void);

/* True iff the pane is open in call mode (PIP + end-call button). */
bool ui_video_pane_is_in_call(void);

/* Replace the image source with the supplied descriptor.  No-op if
 * the pane is not visible (the call site doesn't need to track that).
 * `dsc` must remain valid until the next call (LVGL doesn't copy). */
void ui_video_pane_set_dsc(const lv_image_dsc_t *dsc);

/* Optional callback fired when the user taps the End Call button.
 * Set NULL to clear.  Last registration wins. */
typedef void (*ui_video_pane_end_call_cb_t)(void);
void ui_video_pane_set_end_call_cb(ui_video_pane_end_call_cb_t cb);
