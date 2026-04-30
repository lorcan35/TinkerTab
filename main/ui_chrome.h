/**
 * ui_chrome — global persistent floating chrome (home button + future
 * floating widgets that need a single-source-of-truth lifecycle).
 *
 * TT #328 Wave 10 P0 #11 — pre-Wave-10 the user could tap into Camera /
 * Files / Notes / Wi-Fi and find themselves on screens with NO escape
 * hatch other than swipe (also missing) or a dedicated back button (in
 * inconsistent positions and tap-target sizes).  Audit a11y called for
 * a persistent floating home button as the canonical escape on every
 * non-home screen.
 *
 * Mounted on lv_layer_top() at boot, hidden by default.  Each screen
 * controller calls ui_chrome_set_home_visible(true/false) on
 * show/destroy.  Tap routes to ui_home_get_screen() via lv_screen_load.
 */
#pragma once

#include <stdbool.h>

/** Initialise the persistent floating chrome.  Idempotent.  Must be
 *  called after ui_core init + before any screen create that wants to
 *  toggle the home button. */
void ui_chrome_init(void);

/** Show / hide the persistent home button.  No-op if init hasn't run. */
void ui_chrome_set_home_visible(bool visible);
