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
    VOICE_STATE_IDLE,          // Not connected and not trying (WiFi down / user disabled)
    VOICE_STATE_CONNECTING,    // First-try connect in progress (no prior successful connect)
    VOICE_STATE_READY,         // Connected, waiting for push-to-talk
    VOICE_STATE_LISTENING,     // Recording mic, streaming to Dragon
    VOICE_STATE_PROCESSING,    // Waiting for Dragon STT/LLM
    VOICE_STATE_SPEAKING,      // Playing TTS audio from Dragon
    /* v4·D connectivity audit T1.2: explicit reconnect state so the UI
     * can render a calm "Reconnecting..." pill instead of the red
     * "Disconnected" flash that makes the user think something's broken
     * when we're actually inside the backoff window recovering. */
    VOICE_STATE_RECONNECTING,
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

/* Wave 14 W14-M01: snapshot-under-mutex variants.  Writers mutate
 * the underlying buffers via strcat under s_state_mutex; a cross-task
 * caller of the pointer-returning getters above can observe a
 * half-written mid-strcat string.  These copy variants take the same
 * mutex so they always return a consistent snapshot.  NUL-terminates
 * the output.  Returns false on bad args. */
#include <stdbool.h>
bool voice_get_last_transcript_copy(char *buf, size_t len);
bool voice_get_stt_text_copy(char *buf, size_t len);
bool voice_get_llm_text_copy(char *buf, size_t len);

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

/** v4·D Gauntlet G7-F: like voice_send_config_update but with an optional
 *  reason string (e.g. "cap_downgrade") so Dragon can speak a brief system
 *  TTS confirming an auto mode-switch.  Pass reason=NULL for normal changes. */
esp_err_t voice_send_config_update_ex(int voice_mode, const char *llm_model,
                                      const char *reason);

/** v4·D Gauntlet G1: depth of the queued-turn buffer (0 or 1 today).
 *  Used by the voice overlay to render "+N QUEUED" while a previous turn
 *  is still processing/speaking. */
int voice_get_queue_depth(void);

/** v4·D connectivity audit T1.2: human-readable degraded-state reason
 *  for the home pill.  Returns NULL when the session is healthy (state
 *  is READY/LISTENING/PROCESSING/SPEAKING).  Otherwise one of:
 *      "WiFi offline"
 *      "Reconnecting..."
 *      "Dragon unreachable"
 *      "Remote mode (ngrok)"
 *      "Connecting..."   (first-time connect, not yet seen a success)
 *  The string is a static pointer; safe to read from the LVGL thread. */
const char *voice_get_degraded_reason(void);

/** v4·D connectivity audit T1.3: live link health snapshot.
 *  Filled by the voice_link_probe_task every 30 s.  All fields are
 *  scalar reads; atomic on ESP32-P4.  lan_tcp_ok/ngrok_tcp_ok are the
 *  last TCP-connect result to each; last_probe_us is the tick at which
 *  the probe task finished a full pass. */
typedef struct {
    bool     lan_tcp_ok;
    bool     ngrok_tcp_ok;
    bool     using_ngrok;       /* current client URI target */
    uint32_t last_probe_ms;     /* lv_tick_get() of most recent probe */
    int      connect_attempt;   /* 0 = healthy; N = N failed attempts since last success */
} voice_link_health_t;
void voice_get_link_health(voice_link_health_t *out);

/** Widget Platform v1 — emit a widget_action event back to Dragon.
 *  payload_json (optional) is parsed as JSON and attached as the "payload"
 *  field. Rate-limited at 4 events/sec.
 *  See docs/WIDGETS.md §11 and TinkerBox docs/protocol.md §17.11. */
esp_err_t voice_send_widget_action(const char *card_id, const char *event,
                                   const char *payload_json);

/** Return the current mic RMS level (for live waveform visualization during dictation). */
float voice_get_current_rms(void);

/** v4·D Phase 4b — vision capability (emitted by Dragon on config ACK).
 *  Returns true when the current LLM backend can see a camera frame.
 *  model and per_frame_mils out-params are optional (pass NULL to skip). */
bool voice_get_vision_capability(char *model_out, size_t model_len,
                                 int *per_frame_mils_out);

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

/** Force an immediate reconnect attempt, resetting the exponential backoff.
 *  Call when the user actively interacts (e.g., taps the orb) while disconnected.
 *  This bypasses the watchdog delay so the user doesn't wait up to 60s. */
void voice_force_reconnect(void);
