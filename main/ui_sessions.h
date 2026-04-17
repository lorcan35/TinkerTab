/*
 * TinkerTab — ui_sessions (v5)
 *
 * Conversations browser — list of past chat sessions with a time column,
 * subject + last-AI-reply preview, message count, and the voice mode used.
 * Spec shot-06 right panel ("A letter, not a card stack → SESSIONS").
 *
 * v5 minimal: hand-curated demo rows. Real data will come from a future
 * sessions index persisted to SD (Dragon already owns session_id).
 */
#pragma once

#include "lvgl.h"
#include <stdbool.h>

void ui_sessions_show(void);
void ui_sessions_hide(void);
bool ui_sessions_is_visible(void);
