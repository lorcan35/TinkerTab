/**
 * @file ui_skills.h
 * @brief TT #328 Wave 10 — Skills catalog screen.
 *
 * Dedicated full-screen viewer for Dragon's tool registry.
 * Distinct from ui_agents which mixes a (mostly stale) activity
 * feed with the catalog — this surface is the catalog, full stop.
 *
 * Reachable via the nav-sheet "Skills" tile.  Opens, fetches
 * `GET /api/v1/tools` (bearer-auth via tab5_settings's
 * dragon_api_token, see Wave 8), and renders one typographic card
 * per tool with name + description.  Empty / fetch-fail states
 * render a friendly hint.
 *
 * Lifecycle: hide/show pattern (overlay survives across re-shows
 * to avoid LVGL pool churn — same pattern ui_agents uses).
 */
#pragma once

#include <stdbool.h>

/** Open the Skills catalog overlay.  Idempotent — repeat calls
 *  un-hide a hidden overlay; first call creates it.  Re-fetches
 *  the catalog on every show so newly-registered tools surface
 *  without a Tab5 reboot. */
void ui_skills_show(void);

/** Hide the overlay (no-op if not visible).  Hide-not-destroy so
 *  the LVGL pool stays warm. */
void ui_skills_hide(void);

/** Visibility query for the screen-state JSON / e2e harness. */
bool ui_skills_is_visible(void);
