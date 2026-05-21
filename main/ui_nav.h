/**
 * @file ui_nav.h
 * @brief Centralised screen navigation with voice-aware cancel.
 *
 * Single chokepoint for any screen change.  Replaces ad-hoc
 * `lv_screen_load(home)` + `tab5_debug_set_nav_target("home")` pairs
 * scattered across ten ui_*.c files, none of which checked voice state
 * before swapping screens (TT #623).
 *
 * Default policy: if voice is mid-turn (LISTENING / PROCESSING /
 * SPEAKING / RECONNECTING / CONNECTING), call `voice_cancel()` first
 * and wait briefly for the mic/speaker to settle, THEN navigate.
 * Caller can opt out via `NAV_FLAGS_NO_VOICE_CANCEL` (rare; only
 * system-internal nav like onboarding completion).
 *
 * The actual screen-swap dispatcher lives in debug_server_nav.c
 * (`async_navigate`); this module just adds the gate + debounce on top.
 */

#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
   NAV_HOME = 0,
   NAV_CHAT,
   NAV_NOTES,
   NAV_FILES,
   NAV_CAMERA,
   NAV_SETTINGS,
   NAV_AGENTS,
   NAV_SKILLS,
   NAV_SESSIONS,
   NAV_MEMORY,
   NAV_FOCUS,
   NAV_WIFI,
   NAV_COUNT,
} tab5_nav_target_t;

typedef enum {
   NAV_FLAGS_NONE = 0,
   /** Skip voice cancel.  Use only when nav is system-internal (e.g.
    *  onboarding completion, OTA rollback) — anything the user taps
    *  should leave the default to ensure clean voice teardown. */
   NAV_FLAGS_NO_VOICE_CANCEL = 1 << 0,
   /** Bypass tap debounce.  Use only from debug HTTP / test harness. */
   NAV_FLAGS_FORCE = 1 << 1,
} tab5_nav_flags_t;

/**
 * @brief Navigate to a target screen with voice-aware cleanup.
 *
 * Sequence:
 *  1. Tap debounce (300 ms per target via `ui_tap_gate`).  Skipped if
 *     `NAV_FLAGS_FORCE` set.
 *  2. Voice cancel + settle if `voice_get_state()` is mid-turn.
 *     Skipped if `NAV_FLAGS_NO_VOICE_CANCEL` set.
 *  3. Emit `nav.go` obs event with target name.
 *  4. Hand off to `async_navigate` (LVGL-thread dispatcher).
 *
 * @return ESP_OK on dispatch, ESP_ERR_INVALID_ARG on bad target,
 *         ESP_ERR_INVALID_STATE on tap-debounce reject.
 */
esp_err_t tab5_nav_to(tab5_nav_target_t target, tab5_nav_flags_t flags);

/** Human-readable name ("home", "chat", "notes", ...) for a target. */
const char *tab5_nav_name(tab5_nav_target_t target);

/** Map a screen name string → target enum.  Returns NAV_COUNT if unknown.
 *  Used by HTTP /navigate and any other string-driven callers so they
 *  flow through the same voice-cancel gate. */
tab5_nav_target_t tab5_nav_target_from_name(const char *name);

/** Convenience wrapper: lookup target by name then nav.  Returns
 *  ESP_ERR_NOT_FOUND if @p name is unknown. */
esp_err_t tab5_nav_to_name(const char *name, tab5_nav_flags_t flags);

#ifdef __cplusplus
}
#endif
