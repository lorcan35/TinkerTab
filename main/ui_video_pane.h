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

/* #278 Incoming-call mode: full-screen remote feed + Accept/Decline
 * buttons.  No local camera, no mic — those start only on Accept.
 * Used by voice_video when downlink frames arrive while no call is
 * active (peer-initiated). */
void ui_video_pane_show_incoming(void);

/* Whether the pane is currently in incoming-call mode (Accept/Decline
 * buttons visible, no PIP).  False when in basic playback or active
 * call mode. */
bool ui_video_pane_is_incoming(void);

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

/* #278 Accept / Decline callbacks for the incoming-call mode.  Last
 * registration wins; pass NULL to clear.  Tapping Accept calls
 * accept_cb (caller upgrades to a full call).  Tapping Decline calls
 * decline_cb + hides the pane. */
typedef void (*ui_video_pane_accept_cb_t)(void);
typedef void (*ui_video_pane_decline_cb_t)(void);
void ui_video_pane_set_accept_cb (ui_video_pane_accept_cb_t cb);
void ui_video_pane_set_decline_cb(ui_video_pane_decline_cb_t cb);
