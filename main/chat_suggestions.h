#pragma once

#include "lvgl.h"
#include <stdint.h>

/** Create mode-specific suggestion cards as children of parent. */
void chat_suggestions_create(lv_obj_t *parent, uint8_t mode, lv_event_cb_t on_tap);

/** Show suggestion cards (empty conversation state). */
void chat_suggestions_show(void);

/** Hide suggestion cards (messages exist). */
void chat_suggestions_hide(void);

/** Check if suggestions are currently visible. */
bool chat_suggestions_visible(void);
