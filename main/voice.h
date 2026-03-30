/**
 * TinkerClaw Tab5 — Voice Streaming Module
 *
 * Streams mic audio to Dragon voice server via WebSocket,
 * receives and plays TTS audio responses.
 *
 * Dragon voice server runs on port 3502, WebSocket endpoint /ws/voice.
 * Protocol:
 *   Tab5 -> Dragon: binary frames (PCM 16-bit 16kHz mono)
 *   Tab5 -> Dragon: JSON text frames {"type":"start"}, {"type":"stop"}
 *   Dragon -> Tab5: binary frames (PCM 16-bit audio)
 *   Dragon -> Tab5: JSON text frames {"type":"stt","text":"..."}, {"type":"tts_start"}, {"type":"tts_end"}
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

// Voice states
typedef enum {
    VOICE_STATE_IDLE,        // Not connected or waiting
    VOICE_STATE_CONNECTING,  // Connecting to Dragon voice server
    VOICE_STATE_READY,       // Connected, waiting for push-to-talk
    VOICE_STATE_LISTENING,   // Recording mic, streaming to Dragon
    VOICE_STATE_PROCESSING,  // Waiting for Dragon STT/LLM
    VOICE_STATE_SPEAKING,    // Playing TTS audio from Dragon
} voice_state_t;

// Callback for state changes (for UI updates)
typedef void (*voice_state_cb_t)(voice_state_t new_state, const char *detail);

// Initialize voice module (call after WiFi and Dragon link are up)
esp_err_t voice_init(voice_state_cb_t state_cb);

// Start voice session — connect WebSocket to Dragon voice server (blocking)
esp_err_t voice_connect(const char *dragon_host, uint16_t dragon_port);

// Non-blocking connect — spawns a FreeRTOS task, calls state_cb on result.
// If auto_listen is true, automatically starts listening on success.
esp_err_t voice_connect_async(const char *dragon_host, uint16_t dragon_port,
                               bool auto_listen);

// Push-to-talk: start listening (mic -> Dragon)
esp_err_t voice_start_listening(void);

// Push-to-talk: stop listening (triggers STT -> LLM -> TTS on Dragon)
esp_err_t voice_stop_listening(void);

// Cancel current voice session (stop playback, etc.)
esp_err_t voice_cancel(void);

// Disconnect from Dragon voice server
esp_err_t voice_disconnect(void);

// Get current state
voice_state_t voice_get_state(void);

// Get last transcript (combined STT + LLM, for backward compat)
const char *voice_get_last_transcript(void);

// Get separated STT text (what the user said)
const char *voice_get_stt_text(void);

// Get separated LLM text (what Tinker is saying, streams in)
const char *voice_get_llm_text(void);

// ---- Chat integration (for ui_chat) ----

/**
 * Callback for chat events — dispatched from WS receive task.
 * Called under tab5_ui_lock() so handler can safely modify LVGL objects.
 *
 * Events:
 *   "stt"       — STT result, data = transcript text
 *   "llm_token" — single LLM token, data = token string
 *   "llm_done"  — full LLM response complete, data = full text
 *   "tts_end"   — TTS playback complete, data = NULL
 *   "error"     — pipeline error, data = error message
 */
typedef void (*voice_chat_cb_t)(const char *event, const char *data);

/** Register a chat event callback (NULL to unregister). */
void voice_set_chat_cb(voice_chat_cb_t cb);

/** Send a text message to Dragon (skips STT, goes directly to LLM). */
esp_err_t voice_send_text(const char *text);
