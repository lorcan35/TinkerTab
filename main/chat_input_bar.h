#pragma once

#include "lvgl.h"
#include <stdint.h>

typedef struct {
    lv_obj_t *container;     /* horizontal flex row */
    lv_obj_t *mic_btn;       /* microphone button (left) */
    lv_obj_t *textarea;      /* text input (center, flex-grow) */
    lv_obj_t *send_btn;      /* send button (right) */
} chat_input_bar_t;

/**
 * Create a reusable input bar widget. Returns heap-allocated struct.
 * Uses flex row layout. Textarea fills available space.
 * Touch targets use DPI_SCALE for portability.
 */
chat_input_bar_t *chat_input_bar_create(lv_obj_t *parent, uint32_t accent_color);

void chat_input_bar_set_callbacks(chat_input_bar_t *bar,
                                  lv_event_cb_t on_send,
                                  lv_event_cb_t on_mic,
                                  lv_event_cb_t on_ta_click);
const char *chat_input_bar_get_text(chat_input_bar_t *bar);
void chat_input_bar_clear(chat_input_bar_t *bar);
void chat_input_bar_set_text(chat_input_bar_t *bar, const char *text);
lv_obj_t *chat_input_bar_get_textarea(chat_input_bar_t *bar);
