/**
 * @file ui_typography.h
 * @brief Font-scale-aware typography accessors — Polish P10 follow-up (TT #670).
 *
 * Backs the user-facing "Text size" picker shipped in #667.  Reads the
 * `font_scale` NVS tier (0=0.85x / 1=1.0x / 2=1.15x / 3=1.3x) once at
 * boot and exposes a stable role → font pointer mapping that the
 * existing `FONT_*` macros in config.h route through.
 *
 * Apply-on-restart model: the scaled font picks live for the whole
 * session.  Changing the tier in Settings surfaces a toast asking the
 * user to restart, then the next boot picks up the new tier.  Simpler
 * + safer than live re-render (every widget would need re-styling, and
 * LVGL's invalidation surface is large).
 */
#pragma once

#include "lvgl.h"

typedef enum {
   FONT_ROLE_TITLE = 0, /* 28 default → 24/28/36/36 */
   FONT_ROLE_HEADING,   /* 24 default → 20/24/28/28 */
   FONT_ROLE_BODY,      /* 20 default → 18/20/24/28 */
   FONT_ROLE_SECONDARY, /* 18 default → 16/18/20/24 */
   FONT_ROLE_CAPTION,   /* 16 default → 14/16/18/20 */
   FONT_ROLE_SMALL,     /* 14 default → 12/14/16/18 */
   FONT_ROLE_CLOCK,     /* 48 default → 36/48/48/48 */
   FONT_ROLE_DATE,      /* 24 default → 20/24/28/28 */
   FONT_ROLE_KEY,       /* 20 default → 18/20/24/28 */
   FONT_ROLE_NAV,       /* 18 default → 16/18/20/24 */
   FONT_ROLE_COUNT,
} font_role_t;

/** Populate the session-cached font pointers from the NVS font_scale
 *  tier.  Call once at boot, after settings init, before any UI
 *  widget creation.  Safe to call repeatedly. */
void tab5_typography_init(void);

/** Return the cached scaled font pointer for the given role.  Always
 *  returns a valid non-NULL pointer (falls back to montserrat_20 on
 *  bad role / uninitialized state). */
const lv_font_t *tab5_typography_get(font_role_t role);
