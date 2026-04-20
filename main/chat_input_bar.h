/**
 * chat_input_bar — v4·C Ambient say-pill (108-h radius-54) with 84 px
 * amber orb-ball, ghost hint, text cursor, and keyboard affordance.
 *
 * Byte-matched to main/ui_home.c s_say_pill/s_say_mic constants so the
 * chat input and home say-pill look identical.
 *
 * Voice state drives the orb-ball gradient:
 *   0 idle      → amber (top FBBF24, bot F59E0B)
 *   1 listening → amber-hot (FFD45A / F59E0B)
 *   2 processing → breathing amber (base + breathing opa via anim)
 *   3 speaking  → amber-dark (F59E0B / D97706)
 *   4 done      → calm (fades back to idle)
 */
#pragma once

#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct chat_input_bar chat_input_bar_t;
typedef void (*chat_input_evt_cb_t)(void *user_data);
typedef void (*chat_input_submit_cb_t)(const char *text, void *user_data);

/** Create the pill at the bottom of `parent`. Absolute positioned so it
 *  sits at y = parent_h - PILL_BOT_PAD - PILL_H.
 *  `parent_h` is the parent's height in px (e.g. 1280 for full-screen). */
chat_input_bar_t *chat_input_bar_create(lv_obj_t *parent, int parent_h);

void chat_input_bar_destroy(chat_input_bar_t *b);

/** Ghost hint inside the pill ("Hold to speak · or type…"). */
void chat_input_bar_set_ghost(chat_input_bar_t *b, const char *hint);

/** 0..4 → orb-ball gradient state. Also used to toggle the breathing anim. */
void chat_input_bar_set_voice_state(chat_input_bar_t *b, int state);

/** Show a partial STT line above the pill (live caption). Pass NULL to hide. */
void chat_input_bar_show_partial(chat_input_bar_t *b, const char *partial);

/** The hidden textarea backing the input (for ui_keyboard_show). */
lv_obj_t *chat_input_bar_get_textarea(chat_input_bar_t *b);

/** Read + clear helpers. */
const char *chat_input_bar_get_text(chat_input_bar_t *b);
void chat_input_bar_clear(chat_input_bar_t *b);

/* Event wiring */
void chat_input_bar_on_ball_tap(chat_input_bar_t *b, chat_input_evt_cb_t cb, void *ud);
void chat_input_bar_on_keyboard(chat_input_bar_t *b, chat_input_evt_cb_t cb, void *ud);
/** Fires on keyboard "Done" or equivalent. Text pointer is LVGL-owned;
 *  callback must copy if it needs it beyond the call. */
void chat_input_bar_on_text_submit(chat_input_bar_t *b, chat_input_submit_cb_t cb, void *ud);
/** Click anywhere on the pill body (not ball / not keyboard). Used to
 *  open the keyboard when the user taps the text area. */
void chat_input_bar_on_pill_tap(chat_input_bar_t *b, chat_input_evt_cb_t cb, void *ud);
