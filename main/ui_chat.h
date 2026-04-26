#pragma once
#include "lvgl.h"
lv_obj_t *ui_chat_create(void);
void ui_chat_show(void);
void ui_chat_destroy(void);
bool ui_chat_is_active(void);
void ui_chat_add_message(const char *text, bool is_user);

/**
 * Thread-safe bridge: push a message into Chat history from any task/core.
 * Copies role+text internally (caller's buffers can be temporary/static).
 * Uses lv_async_call to schedule LVGL work on Core 0.
 * Safe to call even if Chat overlay has not been opened yet.
 *
 * @param role  "user" or "assistant"
 * @param text  Message text (will be strdup'd)
 */
void ui_chat_push_message(const char *role, const char *text);

/** Hide chat overlay (called from nav). */
void ui_chat_hide(void);

void ui_chat_push_media(const char *url, const char *media_type,
                        int width, int height, const char *alt);
void ui_chat_push_card(const char *title, const char *subtitle,
                       const char *image_url, const char *description);
void ui_chat_push_audio_clip(const char *url, float duration_s, const char *label);

/**
 * Audit U5 (#206): tap-to-play handler for inline audio_clip rows.
 * Downloads the URL (Dragon-relative or absolute) to a tmp WAV on the
 * SD card and hands the path to ui_audio_create() so the user gets the
 * standard audio-player overlay (play/pause/stop, volume).  Safe from
 * any task — internally enqueues to the shared task_worker so the
 * download doesn't block voice WS or LVGL.
 */
void ui_chat_play_audio_clip(const char *url);

/**
 * Thread-safe: push a system/status bubble (e.g. "Searching the web…",
 * "Memory saved"). Renders centered + dim per chat_msg_view MSG_SYSTEM
 * treatment. Safe from any task; uses lv_async_call internally.
 * Use for tool-call activity indicators and session-change notices.
 */
void ui_chat_push_system(const char *text);

/**
 * Thread-safe: replace the last AI bubble's text with a cleaned version.
 * Used to remove raw code blocks / image URLs after they've been rendered
 * as media bubbles, so the user doesn't see the markdown source text.
 *
 * @param text  Cleaned text to display (will be strdup'd)
 */
void ui_chat_update_last_message(const char *text);

/**
 * v4·D Phase 4d: force a msg-view refresh so a receipt attached to the
 * chat store (via chat_store_attach_receipt_ex) lands on the current
 * turn's bubble instead of the next-natural-redraw.  Thread-safe; no-op
 * if the view is not mounted.
 */
void ui_chat_refresh_receipts(void);

/**
 * U12 (#206): show/hide the live STT partial caption above the input
 * pill.  Pass NULL or "" to hide.  Thread-safe (lv_async_call hop) so
 * voice.c can call it from the WS task on every stt_partial frame.
 */
void ui_chat_show_partial(const char *partial);
