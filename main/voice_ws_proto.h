/**
 * Voice WebSocket protocol layer (Tab5 ↔ Dragon).
 *
 * Owns the on-the-wire dispatch:
 *   - JSON RX (text frames) → handle_text routes to typed handlers.
 *   - Binary RX (mag-tagged VID0/AUD0 + untagged PCM) → handle_binary.
 *   - WS event callback (CONNECTED / DISCONNECTED / DATA / ERROR).
 *   - TX wrappers around esp_websocket_client_send_{text,bin}.
 *   - REGISTER frame builder.
 *
 * Wave 23 SOLID-audit closure for TT #331 (extract A1).  Pre-extract
 * this all lived inline in voice.c at L525-1948; voice.c keeps the
 * state machine + mic capture + listening/dictation/call lifecycles.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_websocket_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/* TX wrappers (formerly static helpers in voice.c). */
esp_err_t voice_ws_send_text(const char *msg);
esp_err_t voice_ws_send_binary(const void *data, size_t len);
esp_err_t voice_ws_send_register(void);

/* RX dispatchers — invoked by voice_ws_proto_event_handler. */
void voice_ws_proto_handle_text(const char *data, int len);
void voice_ws_proto_handle_binary(const char *data, int len);

/* TT #568: clear the cubic-Hermite upsampler's chunk-boundary context
 * between turns so the first chunk of a new TTS playback starts clean
 * (not splicing into the previous turn's tail samples).  Invoked from
 * the tts_start / tts_end branches of the JSON RX dispatcher. */
void voice_ws_proto_upsample_reset(void);

/* esp_websocket_client event callback — register with
 * esp_websocket_register_events from voice_ws_start_client in voice.c. */
void voice_ws_proto_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data);

/* URI helper (LAN target builder).  out_cap must be at least 64. */
void voice_ws_proto_build_local_uri(char *out, size_t out_cap, const char *dragon_host, uint16_t dragon_port);

/* UI-async helpers (formerly voice.c statics).  Used by mic_capture_task
 * (in voice.c) and the JSON RX dispatcher to schedule LVGL-thread
 * notifications without taking the LVGL lock from a worker task. */
void voice_async_toast(char *text); /* takes ownership of text */

#ifdef __cplusplus
}
#endif
