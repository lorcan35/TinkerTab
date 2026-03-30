#pragma once
#include "lvgl.h"
#include <stdbool.h>

/* Create the chat screen and load it. Returns the screen object. */
lv_obj_t *ui_chat_create(void);

/* Add a single message bubble. Call from LVGL task only. */
void ui_chat_add_message(const char *text, bool is_user);

/* Append a streaming token to the last AI bubble. */
void ui_chat_stream_token(const char *token);

/* Destroy and free all chat screen objects. */
void ui_chat_destroy(void);

/* True if chat screen is currently active. */
bool ui_chat_is_active(void);
