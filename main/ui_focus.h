/*
 * TinkerTab — ui_focus (v5 FOCUS state)
 *
 * When user swipes up on the ambient home, the orb collapses to the upper-
 * right corner and the screen reveals what the device has been doing — the
 * current agent heartbeat narrative, a compact task stream, and the 'earlier
 * today' feed. Compared to the Chat overlay this is a read-first view: you
 * see activity without committing to a conversation.
 *
 * v5-minimal: uses static demo data for the heartbeat + feed until Dragon's
 * agent_status aggregation protocol is wired.
 */
#pragma once

#include "lvgl.h"
#include <stdbool.h>

void ui_focus_show(void);
void ui_focus_hide(void);
bool ui_focus_is_visible(void);
