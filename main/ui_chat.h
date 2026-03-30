/**
 * TinkerOS — Chat Screen
 *
 * Full-screen iMessage-style chat interface. Supports both text input
 * (via on-screen keyboard) and voice input (hold-to-talk mic button).
 * Messages stream from Dragon conversation engine via WebSocket.
 */
#pragma once

#include "lvgl.h"

/** Create and show the chat screen. Returns screen object. */
lv_obj_t *ui_chat_create(void);

/** Destroy the chat screen and free resources. */
void ui_chat_destroy(void);

/** Returns true if the chat screen is currently active. */
bool ui_chat_is_active(void);
