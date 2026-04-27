/*
 * ui_video_pane.h — full-screen video pane that displays the latest
 * Dragon-sent JPEG frame (#268 / Phase 3B).
 *
 * Owned by voice_video.c::voice_video_on_downlink_frame which feeds
 * fresh frames via ui_video_pane_set_dsc on the LVGL thread.
 *
 * The pane is an opt-in overlay — call ui_video_pane_show() to open
 * it and ui_video_pane_hide() to close.  Phase 3C will hook these
 * to the call-accept / call-end UI; for Phase 3B a debug endpoint is
 * the only caller.
 */
#pragma once

#include "lvgl.h"

/* Open the full-screen video pane.  Idempotent. */
void ui_video_pane_show(void);

/* Hide + free the pane.  Safe even if not open. */
void ui_video_pane_hide(void);

/* True iff the pane is currently visible. */
bool ui_video_pane_is_visible(void);

/* Replace the image source with the supplied descriptor.  No-op if
 * the pane is not visible (the call site doesn't need to track that).
 * `dsc` must remain valid until the next call (LVGL doesn't copy). */
void ui_video_pane_set_dsc(const lv_image_dsc_t *dsc);
