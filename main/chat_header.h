/**
 * chat_header — composite header widget (chat v4·C Ambient).
 *
 * Layout (720 × 96):
 *   [← 44]  Chat ▾         [mode chip 44h radius-22]  [+ 44]
 * Followed by a 140 × 2 amber accent bar at y=96 (color inherits the
 * session's mode tint via chat_header_set_accent_color).
 *
 * Reusable by Notes / voice overlay (pass hide_chip / hide_plus later).
 * Tokens sourced from ui_theme.h + config.h.
 */
#pragma once

#include "lvgl.h"
#include <stdint.h>

typedef struct chat_header chat_header_t;

typedef void (*chat_header_evt_cb_t)(void *user_data);

/** Create the header at the top of `parent`. `title` may be NULL (defaults to "Chat"). */
chat_header_t *chat_header_create(lv_obj_t *parent, const char *title);

void chat_header_destroy(chat_header_t *h);

/** Set title label (used when drawer opens -> "Conversations"). */
void chat_header_set_title(chat_header_t *h, const char *title);

/** Paint the mode chip + sub-label from (voice_mode, llm_model).
 *  Also repaints the accent bar in the matching mode color. */
void chat_header_set_mode(chat_header_t *h, uint8_t voice_mode, const char *llm_model);

/** TT #328 Wave 1 (audit visibility) — paint the daily-spend badge.
 *  `mils` = today's accumulated cost (1/1000 of a US cent), `cap_mils`
 *  = the configured per-day cap.  When `cap_mils == 0` the cap is
 *  unset and the slash-side is omitted ("$0.47").  Otherwise renders
 *  "$0.47 / $1.00" with colour tinting:
 *    < 80 % of cap : dim grey
 *    80–100 %      : amber
 *    over cap      : red
 *  Pass `mils=0, cap_mils=0` to clear the badge entirely. */
void chat_header_set_spend(chat_header_t *h, uint32_t mils, uint32_t cap_mils);

/** Override the 140×2 accent bar color. */
void chat_header_set_accent_color(chat_header_t *h, uint32_t hex);

/** Flip the chevron glyph. Chat spec §4.2: down = closed, up = open. */
void chat_header_set_chevron_open(chat_header_t *h, bool open);

/* Event wiring */
void chat_header_on_back(chat_header_t *h, chat_header_evt_cb_t cb, void *ud);
void chat_header_on_chevron(chat_header_t *h, chat_header_evt_cb_t cb, void *ud);
void chat_header_on_plus(chat_header_t *h, chat_header_evt_cb_t cb, void *ud);
void chat_header_on_mode_long_press(chat_header_t *h, chat_header_evt_cb_t cb, void *ud);
