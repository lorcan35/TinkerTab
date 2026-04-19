/**
 * chat_suggestions — empty-state prompt cards keyed to session mode.
 * 4 cards per mode; shown when chat_store_count() == 0.
 * Tap fires on_pick(prompt_text, user_data).
 */
#pragma once

#include "lvgl.h"
#include <stdint.h>
#include <stdbool.h>

typedef struct chat_suggestions chat_suggestions_t;
typedef void (*chat_sugg_pick_cb_t)(const char *prompt, void *user_data);

chat_suggestions_t *chat_suggestions_create(lv_obj_t *parent);
void chat_suggestions_destroy(chat_suggestions_t *s);

void chat_suggestions_set_mode(chat_suggestions_t *s, uint8_t voice_mode);
void chat_suggestions_show(chat_suggestions_t *s);
void chat_suggestions_hide(chat_suggestions_t *s);
bool chat_suggestions_visible(chat_suggestions_t *s);
void chat_suggestions_on_pick(chat_suggestions_t *s, chat_sugg_pick_cb_t cb, void *ud);
