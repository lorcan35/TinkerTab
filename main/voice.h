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

// Voice modes
typedef enum {
    VOICE_MODE_ASK,      // Short-form: STT -> LLM -> TTS (30s max)
    VOICE_MODE_DICTATE,  // Long-form: STT only, unlimited, no LLM/TTS
} voice_mode_t;

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

// Dictation mode: unlimited recording, STT-only, no LLM/TTS
esp_err_t voice_start_dictation(void);

// Get current voice mode
voice_mode_t voice_get_mode(void);

// Get accumulated dictation transcript (concatenated stt_partial results)
const char *voice_get_dictation_text(void);

// Clear conversation history on Dragon (start fresh context)
esp_err_t voice_clear_history(void);

// Send text message to Dragon (skips STT, goes straight to LLM)
esp_err_t voice_send_text(const char *text);

// Send three-tier voice config to Dragon (voice_mode 0/1/2 + LLM model string)
esp_err_t voice_send_config_update(int voice_mode, const char *llm_model);

// Get current mic RMS level (for live waveform during dictation)
float voice_get_current_rms(void);

// Get dictation post-processing results (arrives after dictation_summary event)
const char *voice_get_dictation_title(void);
const char *voice_get_dictation_summary(void);

// Wake word: called when AFE detects wake word. Starts listening or barge-in.
void voice_on_wake(void);

// Reconnect watchdog: auto-reconnects voice WS after disconnect
esp_err_t voice_start_reconnect_watchdog(void);
void voice_stop_reconnect_watchdog(void);

// True if voice WebSocket is actually connected and receive task is running
bool voice_is_connected(void);

// Always-listening mode: init/start/stop the AFE pipeline
esp_err_t voice_start_always_listening(void);
esp_err_t voice_stop_always_listening(void);
bool voice_is_always_listening(void);
