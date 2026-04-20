/*
 * ui_onboarding.h — First-boot welcome carousel (audit G / P0 UX).
 *
 * 3 cards: (1) Welcome + voice-first intro, (2) modes + memory overview,
 * (3) privacy + get-started.  Dismisses via "Skip" on any card or
 * "Get started" on the last one; either writes `onboard=1` to NVS so
 * the overlay never shows again.
 *
 * main.c calls ui_onboarding_show_if_needed() after ui_home_create
 * finishes; is_onboarded() gate makes subsequent boots a no-op.
 */

#pragma once

#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Show if tab5_settings_is_onboarded() is false; otherwise no-op. */
void ui_onboarding_show_if_needed(void);

/* Force-show (for /debug/onboarding or re-trigger on a "Show intro again"
 * Settings row). */
void ui_onboarding_force_show(void);

/* Hide + destroy; marks onboarded=true in NVS. */
void ui_onboarding_finish(void);

bool ui_onboarding_visible(void);

#ifdef __cplusplus
}
#endif
