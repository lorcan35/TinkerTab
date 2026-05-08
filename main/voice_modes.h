/**
 * Voice six-tier mode dispatcher (Tab5).
 *
 * Owns the {LOCAL, HYBRID, CLOUD, TINKERCLAW, LOCAL_ONBOARD, SOLO_DIRECT}
 * routing decision for outbound text turns + the config_update JSON
 * frame that announces a mode/model change to Dragon.
 *
 * Wave 23 SOLID-audit closure for TT #331 (extract A2).  Pre-extract
 * the routing decision was inline at voice.c:voice_send_text inside
 * the five-tier branch ladder.  Pulling it out lets voice.c keep the
 * public API (voice_send_text) thin while the mode-specific branches
 * stay testable + extendable in voice_modes.c.
 *
 * TT #370 (2026-05-08): added SOLO_DIRECT (vmode=5) — routes to
 * voice_solo_send_text for direct-to-OpenRouter, Dragon-free turns.
 */
#pragma once

#include "esp_err.h"
#include "voice.h" /* voice_mode_t */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
   /* VMODE_LOCAL_ONBOARD active + chain currently running — caller must
    * surface a "Stop onboard chat first" toast and refuse the send. */
   VOICE_MODES_ROUTE_K144_CHAIN_BUSY = 0,
   /* Routed text to K144 successfully (vmode=4 OR Local-mode failover hit). */
   VOICE_MODES_ROUTE_K144_OK,
   /* Routed text to K144 but K144 returned an error — caller should
    * surface a toast (failure detail in `err`). */
   VOICE_MODES_ROUTE_K144_FAILED,
   /* Default — caller should send the text via the Dragon WS path. */
   VOICE_MODES_ROUTE_DRAGON_PATH,
   /* TT #370 — vmode=5 SOLO_DIRECT.  Routed to voice_solo_send_text;
    * caller treats this as terminal (no Dragon WS dispatch). */
   VOICE_MODES_ROUTE_SOLO_OK,
   /* vmode=5 selected but OpenRouter API key empty — caller surfaces
    * "Scan QR to set OpenRouter key" toast and refuses. */
   VOICE_MODES_ROUTE_SOLO_NO_KEY,
   /* voice_solo_send_text returned an error — caller surfaces toast
    * (failure detail in `err`). */
   VOICE_MODES_ROUTE_SOLO_FAILED,
} voice_modes_route_kind_t;

typedef struct {
   voice_modes_route_kind_t kind;
   esp_err_t err; /* set when kind == K144_FAILED or SOLO_FAILED/SOLO_NO_KEY */
} voice_modes_route_result_t;

/* Pure routing decision.  Called by voice_send_text BEFORE building any
 * Dragon-side JSON frame — caller dispatches based on result.kind.
 *
 *   - vmode=4 (LOCAL_ONBOARD) → always K144 (CHAIN_BUSY if chain active,
 *     OK on send success, FAILED on send error)
 *   - vmode=0 (LOCAL) AND Dragon WS down for >= M5_FAILOVER_GRACE_MS AND
 *     K144 warm → K144 failover (OK or FAILED)
 *   - Otherwise → DRAGON_PATH
 */
void voice_modes_route_text(const char *text, voice_modes_route_result_t *out);

/* config_update frame senders (formerly voice_send_config_update*). */
esp_err_t voice_send_config_update(int voice_mode, const char *llm_model);
esp_err_t voice_send_config_update_ex(int voice_mode, const char *llm_model, const char *reason);

/* Internal mode setter — voice.c calls this from voice_start_listening
 * (sets ASK), voice_start_dictation (sets DICTATE),
 * voice_call_audio_start (sets CALL), voice_call_audio_stop /
 * voice_stop_listening (sets ASK).  Keeps s_voice_mode ownership inside
 * voice_modes.c. */
void voice_modes_set_internal(voice_mode_t mode);

#ifdef __cplusplus
}
#endif
