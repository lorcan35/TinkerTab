/**
 * voice_solo — Tab5 solo (Dragon-independent) voice orchestrator.
 *
 * Runs the vmode=5 SOLO_DIRECT pipeline: mic → OpenRouter STT →
 * OpenRouter chat-completions (SSE) → OpenRouter TTS → speaker.  No
 * Dragon WS, no K144 — direct calls to openrouter.ai over TLS.
 *
 * The state machine reuses voice.h's IDLE/LISTENING/PROCESSING/SPEAKING
 * (set via voice_set_state) so the existing UI orb + chat overlay
 * "just work" without an alternate state enum.
 *
 * Public surface is tiny — voice.c calls these from the mic-stop hook
 * (audio path) and voice_modes.c calls voice_solo_send_text (text
 * path).  Cancellation is global: voice_solo_cancel aborts any
 * in-flight HTTP via openrouter_cancel_inflight.
 *
 * TT #370 Phase 1 — see docs/superpowers/specs/2026-05-08-tab5-solo-mode-design.md
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** One-time init.  Idempotent.  Allocates the playback handoff state +
 *  ensures /sdcard/sessions/ + /sdcard/rag.bin exist (later commits).
 *  Returns ESP_OK or first failing init step. */
esp_err_t voice_solo_init(void);

/** Send a text turn.  Synchronous from caller's perspective up to LLM
 *  TTFT; an internal worker streams reply chunks into the chat UI;
 *  TTS + playback follow.  Caller is voice_modes_route_text. */
esp_err_t voice_solo_send_text(const char *text);

/** Hand a captured mic buffer (PCM-16LE @ 16 kHz mono) for STT → LLM
 *  → TTS.  Caller (voice.c mic-stop hook) loses ownership; voice_solo
 *  frees with heap_caps_free when done. */
esp_err_t voice_solo_send_audio(int16_t *pcm, size_t samples);

/** Abort any in-flight HTTP + flush playback ring.  Safe from any
 *  task; idempotent. */
void voice_solo_cancel(void);

/** True iff a turn is currently in PROCESSING or SPEAKING. */
bool voice_solo_busy(void);

#ifdef __cplusplus
}
#endif
