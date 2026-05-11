/**
 * openrouter_client — HTTP wrapper for OpenRouter's audio + chat APIs.
 *
 * All four verbs share a base URL (https://openrouter.ai/api/v1) and an
 * Authorization: Bearer <or_key> header (read from NVS via
 * tab5_settings_get_or_key).  TLS chain validated by the bundled cert
 * store (already attached in service_network.c).  Response bodies
 * land in PSRAM so internal SRAM impact stays ~0.
 *
 * Single-flight model: voice_solo's state machine never overlaps two
 * calls, so we only track one in-flight handle.  openrouter_cancel_inflight
 * aborts whichever verb is currently in progress.
 *
 * TT #370 — see docs/superpowers/specs/2026-05-08-tab5-solo-mode-design.md
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Streaming-callback shapes for chat + tts. */
typedef void (*openrouter_chat_delta_cb_t)(const char *delta, size_t len, void *ctx);
typedef void (*openrouter_tts_chunk_cb_t)(const uint8_t *pcm, size_t len, void *ctx);

/** STT — multipart upload of PCM-16LE @ 16 kHz mono.  Writes UTF-8
 *  transcript into out_text (caller-owned, NUL-terminated).  Returns
 *  ESP_FAIL on non-200, ESP_ERR_INVALID_STATE if or_key empty. */
esp_err_t openrouter_stt(const int16_t *pcm, size_t samples, char *out_text, size_t out_cap);

/** Streaming chat completion.  messages_json is a JSON-encoded array
 *  of {role, content} objects (caller owns the build).  cb fires per
 *  delta token; the final accumulated reply is the caller's
 *  responsibility to assemble (cb is incremental). */
esp_err_t openrouter_chat_stream(const char *messages_json, openrouter_chat_delta_cb_t cb, void *ctx);

/** TTS.  Streams WAV bytes; the 44-byte RIFF header is consumed
 *  internally and PCM bytes flow to cb (PCM-16LE @ 24 kHz mono per
 *  the OpenAI tts-1 spec). */
esp_err_t openrouter_tts(const char *text, openrouter_tts_chunk_cb_t cb, void *ctx);

/** Embeddings.  Returns a malloc'd float vector in PSRAM; caller
 *  frees with heap_caps_free.  *out_dim is set to the dimensionality. */
esp_err_t openrouter_embed(const char *text, float **out_vec, size_t *out_dim);

/** TT #379 — multimodal audio chat (audio in → audio + text out).
 *
 *  Single /chat/completions call against an audio-capable model
 *  (openai/gpt-4o-audio-preview by default) that replaces the
 *  STT → LLM → TTS chain.  Streams two interleaved callback types:
 *    - audio_cb fires with decoded PCM-16LE chunks at 24 kHz mono
 *      (caller upsamples to its playback rate)
 *    - text_cb fires with assistant transcript deltas (same shape as
 *      openrouter_chat_stream — feed into the chat-overlay accumulator)
 *  Either callback may be NULL.  Returns ESP_OK on [DONE], ESP_FAIL on
 *  non-200, ESP_ERR_INVALID_STATE if or_key empty. */
esp_err_t openrouter_chat_audio(const int16_t *pcm, size_t samples, openrouter_tts_chunk_cb_t audio_cb,
                                openrouter_chat_delta_cb_t text_cb, void *ctx);

/** Abort any in-flight HTTP.  Safe from any task; idempotent. */
void openrouter_cancel_inflight(void);

#ifdef __cplusplus
}
#endif
