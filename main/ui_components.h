/**
 * @file ui_components.h
 * @brief Shared UI atom library — the design-system foundation.
 *
 * Polish Wave P1 (TT #648): every screen now uses the same vocabulary
 * for top-bars, cards, list rows, filter pills, action buttons, chips,
 * empty states, and value formatting.  Atoms are intentionally small
 * + composable so consumers don't fight the helper's opinions.
 *
 * All atoms read from `ui_theme.h` tokens — never hard-code colors,
 * radii, or padding in callers.  When a token needs to change app-wide,
 * the theme file is the single source of truth.
 *
 * Memory: all atoms allocate LVGL widgets only (children of the parent
 * passed in), so caller owns lifecycle via `lv_obj_delete(parent)` /
 * `lv_obj_clean(parent)`.  No persistent allocations; safe to call
 * repeatedly during screen rebuild.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Tone tokens (passed to atoms that vary by semantic role) ────── */
typedef enum {
   UI_TONE_NEUTRAL = 0, /**< default — card / chip with no semantic */
   UI_TONE_ACCENT,      /**< amber — primary action / selected state */
   UI_TONE_SUCCESS,     /**< green — positive state */
   UI_TONE_DANGER,      /**< red — destructive / error */
   UI_TONE_INFO,        /**< blue — informational / link */
   UI_TONE_DIM,         /**< gray — passive / disabled */
} ui_tone_t;

/* ── Top-bar ────────────────────────────────────────────────────────
 *
 * 48 px header at the top of every full-screen UI: back arrow (when
 * back_cb != NULL), centered title, optional right-chip (status pill
 * or action).  Matches the Settings + Files + Chat header rhythm.
 *
 * Returns the topbar container so callers can stash a pointer for
 * later updates.  Children of the topbar use box-relative positioning. */
typedef void (*ui_topbar_cb_t)(lv_event_t *e);
lv_obj_t *ui_topbar(lv_obj_t *parent, const char *title, ui_topbar_cb_t back_cb, lv_obj_t **out_right_slot);

/* ── Card ───────────────────────────────────────────────────────────
 *
 * Rounded `TH_CARD` background with 1 px `TH_CARD_BORDER` outline.
 * Drop-in replacement for the previous `mk_card_bg` retroactive
 * helper — `ui_card` is for new layouts that compose children inside
 * the card directly.
 *
 * Caller positions the card via lv_obj_set_pos / lv_obj_set_size on
 * the returned handle; children parent to the returned object. */
lv_obj_t *ui_card(lv_obj_t *parent, int x, int y, int w, int h);

/* ── List row ───────────────────────────────────────────────────────
 *
 * 64 px row with FONT_BODY label on the left and an optional
 * right-aligned secondary text (size, count, status).  When tone is
 * UI_TONE_ACCENT a 3 px amber bar is drawn on the row's left edge
 * (for selected-state highlighting). */
lv_obj_t *ui_list_row(lv_obj_t *parent, int y, const char *label, const char *secondary, ui_tone_t tone);

/* ── Filter pills ───────────────────────────────────────────────────
 *
 * Horizontal flex row of pill buttons.  Selected pill renders with
 * TH_AMBER fill, others with TH_CARD.  The selected index updates
 * automatically when a pill is tapped, and cb is invoked with the
 * newly-selected index.  Used by Notes / Sessions filter rows. */
typedef void (*ui_filter_cb_t)(int new_index);
lv_obj_t *ui_filter_pills(lv_obj_t *parent, int y, int w, const char *const *labels, int n_labels, int selected,
                          ui_filter_cb_t cb);

/* ── Action button ──────────────────────────────────────────────────
 *
 * Pill button with optional icon + label.  Tone selects border + label
 * color (UI_TONE_DANGER = red, UI_TONE_ACCENT = amber, etc).  Built-in
 * pressed-state feedback via `ui_fb_button`.  Icon string can be any
 * LV_SYMBOL_* or NULL. */
lv_obj_t *ui_action_btn(lv_obj_t *parent, const char *icon, const char *label, ui_tone_t tone, ui_topbar_cb_t cb,
                        void *user_data);

/* ── Chip ──────────────────────────────────────────────────────────
 *
 * Passive status chip: rounded pill with single-line label.  No tap
 * affordance.  Used for status badges (LOCAL / CLAW / OFFLINE), mode
 * indicators, and label tags. */
lv_obj_t *ui_chip(lv_obj_t *parent, int x, int y, const char *label, ui_tone_t tone);

/* ── Empty state ───────────────────────────────────────────────────
 *
 * Centered icon + dim title + secondary hint.  Used inside empty
 * lists (Notes, Files, Sessions) when there's nothing to render.
 * Caller is responsible for sizing the parent to give the helper room. */
lv_obj_t *ui_empty_state(lv_obj_t *parent, const char *icon_text, const char *title, const char *hint);

/* ── Time formatter ────────────────────────────────────────────────
 *
 * Unified date/time formatting across the app.  STYLE picks the
 * display granularity:
 *   UI_TIME_RELATIVE — "Today 22:33", "Yesterday 17:42", "Mon 09:15",
 *                       "May 17 16:07" for >7 days
 *   UI_TIME_ABSOLUTE — always "MMM DD HH:MM" (e.g. "May 17 16:07")
 *   UI_TIME_SHORT    — just "HH:MM"
 *
 * @p ts is a Unix timestamp (seconds).  Buffer must be ≥24 bytes. */
typedef enum {
   UI_TIME_RELATIVE = 0,
   UI_TIME_ABSOLUTE,
   UI_TIME_SHORT,
} ui_time_style_t;
void ui_fmt_time(char *buf, size_t cap, int64_t ts, ui_time_style_t style);

/* ── Byte/count formatters ─────────────────────────────────────────
 *
 * Consistent K / M / G suffix for sizes and counts.  Buffer ≥16 bytes. */
void ui_fmt_bytes(char *buf, size_t cap, uint64_t n);
void ui_fmt_count(char *buf, size_t cap, uint64_t n);

#ifdef __cplusplus
}
#endif
