#pragma once
#include "lvgl.h"
#include "voice.h"

/**
 * Voice UI overlay for TinkerOS.
 *
 * Full-screen overlay that shows:
 * - Tap-to-talk button (floating mic button, always visible)
 * - Listening state: animated orb/ring + "Listening..." text
 * - Processing state: pulsing animation + "Thinking..." text
 * - Speaking state: audio wave visualization + transcript text
 * - Cancel button to abort
 *
 * Wires directly into voice.h state callbacks.
 */

// Initialize voice UI (call once after LVGL + voice_init)
void ui_voice_init(void);

// Called by voice state callback to update visuals
void ui_voice_on_state_change(voice_state_t state, const char *detail);

// Show/hide the voice overlay manually
void ui_voice_show(void);
void ui_voice_hide(void);
bool ui_voice_is_visible(void);

// Get the floating mic button (for external positioning)
lv_obj_t *ui_voice_get_mic_btn(void);

// Suppress overlay during boot auto-connect (only update mic dot state)
void ui_voice_set_boot_connect(bool silent);
