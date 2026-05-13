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

/** TT #481 (W7-E.4c follow-up): repaint the reply-context chip from
 *  the currently-armed channel_reply state.  Idempotent — when context
 *  is armed, shows the chip; when not, hides it.  Call right after
 *  `voice_arm_channel_reply` from any path where the voice overlay may
 *  already be up — `ui_voice_show` short-circuits when `s_visible` is
 *  already true so the chip-paint inside it doesn't re-run.  Safe to
 *  call before the overlay is built (bails when widget is NULL).
 *  LVGL thread only. */
void ui_voice_refresh_reply_chip(void);

/**
 * Dismiss the overlay only if voice is in a non-active state
 * (IDLE/READY/RECONNECTING/CONNECTING).  During LISTENING / PROCESSING /
 * SPEAKING the overlay is the UI so we leave it alone.  Used by nav
 * paths so switching screens doesn't leave a stale "Tap to speak." card
 * blocking whatever the user navigated to.
 */
void ui_voice_dismiss_if_idle(void);

// Get the floating mic button (for external positioning)
lv_obj_t *ui_voice_get_mic_btn(void);

// Suppress overlay during boot auto-connect (only update mic dot state)
void ui_voice_set_boot_connect(bool silent);

/**
 * Show dictation auto-stop countdown warning on the status label.
 * Called from voice.c mic task via lv_async_call when silence exceeds 3s/4s.
 * @param seconds_remaining  2 or 1 (seconds until auto-stop), or 0 to clear warning.
 */
void ui_voice_show_auto_stop_warning(int seconds_remaining);
