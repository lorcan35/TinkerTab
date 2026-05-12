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

/* TT #328 Wave 7 — ui_home_get_tileview / ui_home_get_tile removed.
 * Both were neutered shells (NULL / s_screen-on-page-0) ever since
 * v4·C retired the 4-page tileview.  Use ui_home_get_screen instead. */

/** Navigate to settings screen (spawns background task). */
void ui_home_nav_settings(void);

/** Refresh mode badge from NVS (call after debug API mode change). */
void ui_home_refresh_mode_badge(void);

/** Show a centered toast message on the home screen for ~2 seconds.
 *  Safe to call from LVGL thread only — use lv_async_call from other cores. */
void ui_home_show_toast(const char *text);

/** TT #328 Wave 3 — toast severity tones.  TONE_INFO is the legacy default
 *  (amber border).  TONE_WARN draws a yellow border + slightly longer
 *  baseline lifetime.  TONE_ERROR draws a rose border + the longest
 *  baseline lifetime so the user has time to read.  Lifetimes are also
 *  scaled by message length (60 ms/char on top of the baseline). */
typedef enum {
   UI_TOAST_INFO = 0,     /* amber  — legacy default */
   UI_TOAST_WARN = 1,     /* yellow — non-fatal degradation */
   UI_TOAST_ERROR = 2,    /* rose   — failure user must notice */
   UI_TOAST_INCOMING = 3, /* blue-violet — W7-E.1 channel_message arrival */
} ui_toast_tone_t;

/** Show a toast with a severity tone + auto-scaled lifetime.
 *  ui_home_show_toast(text) is equivalent to ui_home_show_toast_ex(text, UI_TOAST_INFO).
 *  LVGL thread only. */
void ui_home_show_toast_ex(const char *text, ui_toast_tone_t tone);

/** TT #328 Wave 4 — undo-pill toast for cost-bearing or hard-to-reverse
 *  user actions (orb long-press mode cycle, NVS mutation).  Renders
 *  "{label} · Undo (Ns)" with a tappable affordance.  Auto-dismisses
 *  after `seconds`; tap fires undo_cb on the LVGL thread.  If a second
 *  undo toast is shown before the first expires, the first is finalized
 *  WITHOUT firing its callback (the user moved on).  LVGL thread only. */
typedef void (*ui_undo_cb_t)(void);
void ui_home_show_undo_toast(const char *label, int seconds, ui_undo_cb_t undo_cb);

/** TT #328 Wave 3 — persistent error banner.  Unlike toasts (~2 s), the
 *  banner sits below the sys-label area until either dismiss_cb is invoked
 *  via tap, or ui_home_clear_error_banner() is called by the recovery path.
 *  Single-banner: a second show replaces the first.  Pass NULL dismiss_cb
 *  to make the banner non-dismissable (system-driven only).
 *  LVGL thread only. */
typedef void (*ui_banner_dismiss_cb_t)(void);
void ui_home_show_error_banner(const char *text, ui_banner_dismiss_cb_t dismiss_cb);
void ui_home_clear_error_banner(void);

/** TT #328 Wave 11 follow-up — toggle banner visibility WITHOUT
 *  destroying it (so the text + dismiss-cb survive a screen change).
 *  Non-home screens (Camera/Files/Notes/Wi-Fi/Settings) call this
 *  with `false` on enter so the banner doesn't shadow their own
 *  topbars/back-buttons; home screen calls `true` on enter to
 *  restore visibility.  No-op when no banner is currently shown. */
void ui_home_set_error_banner_visible(bool visible);

/** Wave 7 F5 crash surface: brief rose pulse on the halo orb.
 *  Used when a mid-turn Dragon drop is detected so the user has a visual
 *  signal alongside the "Dragon dropped mid-turn" toast. Reverts to the
 *  mode-default orb paint after ~2.5 s.
 *  LVGL thread only — marshal from other tasks via lv_async_call. */
void ui_home_pulse_orb_alert(void);

/** Refresh just the top-left sys label (DRAGON / OFFLINE + battery + voice
 *  state) without waiting for the 5 s poll. Called from voice state callbacks
 *  so LISTENING / THINKING / SPEAKING appear within one LVGL tick. Thread-safe
 *  via lv_async_call — callers can invoke from any task/core. */
void ui_home_refresh_sys_label(void);
