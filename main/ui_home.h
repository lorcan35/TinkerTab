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

/** Show a centered toast message on the home screen for ~2 seconds.
 *  Safe to call from LVGL thread only — use lv_async_call from other cores. */
void ui_home_show_toast(const char *text);

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
