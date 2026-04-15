#pragma once

#include "lvgl.h"
#include <stdint.h>

/** Initialize the recycled message view inside a parent container. */
void chat_view_init(lv_obj_t *parent);

/** Switch to a different mode's message history. Refreshes the view. */
void chat_view_set_mode(uint8_t mode);

/** Get current mode. */
uint8_t chat_view_get_mode(void);

/** Refresh the visible window from the store (call after adding messages). */
void chat_view_refresh(void);

/** Scroll to the bottom (latest message). */
void chat_view_scroll_to_bottom(void);

/** Append streaming token to the last AI message. Auto-scrolls. */
void chat_view_append_streaming(const char *token);

/** Finalize streaming — commit accumulated text to store, measure height. */
void chat_view_finalize_streaming(void);

/** Check if currently streaming. */
bool chat_view_is_streaming(void);

/** Get the scroll container (for keyboard layout callbacks). */
lv_obj_t *chat_view_get_scroll(void);
