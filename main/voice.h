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

/** Initialize voice module: creates mutexes, allocates PSRAM playback ring buffer. */
esp_err_t voice_init(voice_state_cb_t state_cb);

/** Connect to Dragon voice server (blocking). Tries local LAN first, then ngrok fallback. */
esp_err_t voice_connect(const char *dragon_host, uint16_t dragon_port);

/** Connect to Dragon asynchronously in a FreeRTOS task; auto-starts listening if auto_listen is true. */
esp_err_t voice_connect_async(const char *dragon_host, uint16_t dragon_port,
                               bool auto_listen);

/** Start push-to-talk ask mode: sends start signal to Dragon and spawns mic capture task. */
esp_err_t voice_start_listening(void);

/** Stop push-to-talk: halts mic capture, sends stop signal, transitions to PROCESSING. */
esp_err_t voice_stop_listening(void);

/** Cancel current voice session: stops mic, flushes playback buffer, sends cancel to Dragon. */
esp_err_t voice_cancel(void);

/** Disconnect from Dragon voice server: tears down WS transport and all background tasks. */
esp_err_t voice_disconnect(void);

/** Return the current voice state (thread-safe, protected by mutex). */
voice_state_t voice_get_state(void);

/** Return the last combined transcript (STT + LLM text). Caller must copy if persisting. */
const char *voice_get_last_transcript(void);

/** Return the last STT text (what the user said). */
const char *voice_get_stt_text(void);

/** Return the last LLM response text (what Tinker is saying, streams in incrementally). */
const char *voice_get_llm_text(void);

/** Start dictation mode: unlimited recording, STT-only, no LLM/TTS, auto-stops after 5s silence. */
esp_err_t voice_start_dictation(void);

/** Return the current voice mode (ASK or DICTATE). */
voice_mode_t voice_get_mode(void);

/** Return the accumulated dictation transcript from stt_partial results (PSRAM-backed, up to 64KB). */
const char *voice_get_dictation_text(void);

/** Send clear_history command to Dragon to reset conversation context. */
esp_err_t voice_clear_history(void);

/** Send a text message to Dragon, bypassing STT (goes straight to LLM). */
esp_err_t voice_send_text(const char *text);

/** Send voice_mode (0-3) and LLM model string to Dragon as a config_update JSON frame. */
esp_err_t voice_send_config_update(int voice_mode, const char *llm_model);

/** Return the current mic RMS level (for live waveform visualization during dictation). */
float voice_get_current_rms(void);

/** Return the auto-generated title from Dragon's dictation post-processing. */
const char *voice_get_dictation_title(void);

/** Return the auto-generated summary from Dragon's dictation post-processing. */
const char *voice_get_dictation_summary(void);

/** Handle wake word detection: starts listening from IDLE/READY, or barge-in from SPEAKING. */
void voice_on_wake(void);

/** Start the reconnect watchdog task: checks connection every 5s and auto-reconnects with backoff. */
esp_err_t voice_start_reconnect_watchdog(void);

/** Stop the reconnect watchdog task (it exits on its next check cycle). */
void voice_stop_reconnect_watchdog(void);

/** Return true if the voice WebSocket is connected and the receive task is running. */
bool voice_is_connected(void);

/** Start always-listening mode: initializes AFE pipeline (AEC + WakeNet) and spawns mic/detect tasks. */
esp_err_t voice_start_always_listening(void);

/** Stop always-listening mode: tears down AFE pipeline and stops continuous mic capture. */
esp_err_t voice_stop_always_listening(void);

/** Return true if always-listening mode (AFE + wake word) is currently active. */
bool voice_is_always_listening(void);

/** Force an immediate reconnect attempt, resetting the exponential backoff.
 *  Call when the user actively interacts (e.g., taps the orb) while disconnected.
 *  This bypasses the watchdog delay so the user doesn't wait up to 60s. */
void voice_force_reconnect(void);
