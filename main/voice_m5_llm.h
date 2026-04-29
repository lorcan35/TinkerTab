/**
 * @file voice_m5_llm.h
 * @brief Tab5 ↔ K144 LLM Module sidecar — synchronous, blocking inference
 *        over the stacked M5-Bus UART path.
 *
 * Composition (SOLID, DIP):
 *   uart_port_c    ── byte-stream transport (Phase 1)
 *   m5_stackflow   ── JSON marshalling     (Phase 2)
 *   voice_m5_llm   ── this file: lifecycle + streaming infer (Phase 3)
 *
 * Lifecycle a single `voice_m5_llm_infer` call performs:
 *   1. Lazy `llm.setup` once per session — sends model + params to the K144,
 *      caches the returned `work_id` (e.g. "llm.1000").
 *   2. `llm.inference` with the prompt — reads streaming chunks
 *      (`object == "llm.utf-8.stream"`) and concatenates `delta` strings
 *      into the caller's buffer until `finish == true` or timeout.
 *
 * Heavy synchronous call — TTFT ~600 ms + ~70 ms/token for the bundled
 * `qwen2.5-0.5B-prefill-20e` per Phase 0 bench.  Run from a worker task,
 * NEVER from the LVGL UI task or the voice WS RX task.
 *
 * Phase 4 (failover wiring in voice.c) will dispatch this from the existing
 * `tab5_worker_init()` job queue.
 *
 * See `docs/PLAN-m5-llm-module.md` Phase 3 for acceptance criteria.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Send a `sys.ping` to the K144 and wait briefly for the ACK.
 *
 * Cheap — doesn't touch the LLM unit, just confirms the StackFlow daemon
 * is alive and reachable.  Lazy-inits the Port C UART on first call.
 *
 * @return ESP_OK on a clean ack with `error.code == 0`;
 *         ESP_ERR_TIMEOUT on no response within ~500 ms;
 *         ESP_ERR_INVALID_RESPONSE on a parse failure or non-zero error code;
 *         propagated esp_err_t on UART init failure.
 */
esp_err_t voice_m5_llm_probe(void);

/**
 * @brief Generate a reply for @p prompt, append into @p output (NUL-terminated).
 *
 * Performs the full setup→inference→stream-collect loop:
 *   - First call after boot (or after @ref voice_m5_llm_release) issues
 *     `llm.setup` and caches the assigned `work_id`.  Subsequent calls
 *     reuse it.
 *   - Sends `llm.inference` with the prompt as `data` (string form).
 *   - Reads streaming chunks until `finish == true` or @p timeout_s elapses.
 *
 * @param prompt      User-facing prompt text.  Must be non-NULL/non-empty.
 * @param output      Destination buffer; receives concatenated deltas as a
 *                    NUL-terminated UTF-8 string.  Output may be truncated
 *                    if it would exceed @p output_cap; the function still
 *                    drains the stream so the K144 returns to ready state.
 * @param output_cap  Capacity of @p output in bytes (NUL included).
 * @param timeout_s   Hard cap on total operation including setup + streaming.
 *                    Phase 0 measured ~5 s for a 75-token reply on this
 *                    model — pass 30 s as a sane default for short replies.
 *
 * @return ESP_OK on a complete stream (finish==true) within timeout;
 *         ESP_ERR_TIMEOUT if the deadline elapsed before finish;
 *         ESP_ERR_INVALID_ARG on NULL/empty inputs;
 *         ESP_ERR_INVALID_RESPONSE on parse / non-zero error_code from the K144;
 *         propagated esp_err_t on UART or transport failure.
 */
esp_err_t voice_m5_llm_infer(const char *prompt, char *output, size_t output_cap, uint32_t timeout_s);

/**
 * @brief Release any cached `llm.setup` work_id by sending `llm.exit`.
 *
 * Safe to call when no setup is cached (no-op).  Subsequent
 * @ref voice_m5_llm_infer calls will re-setup before inference.
 */
void voice_m5_llm_release(void);

/**
 * @brief Whether a `llm.setup` work_id is currently cached locally.
 *
 * Diagnostic only — does not contact the module.  Consumers should treat
 * the K144 as available iff @ref voice_m5_llm_probe returns ESP_OK.
 */
bool voice_m5_llm_is_ready(void);

/* ---------------------------------------------------------------------- */
/*  Phase 6a — UART baud negotiation                                      */
/*                                                                        */
/*  The K144 boots at 115200 8N1.  Audio paths (Phase 6b/c — ASR + TTS)  */
/*  inflate to ~43 KB/s post-base64, which exceeds 11.5 KB/s effective    */
/*  at 115200.  Bumping both ends to 1 Mbps gives 125 KB/s headroom.      */
/*                                                                        */
/*  Wire shape (M5Module-LLM v1.7.0 api_sys.cpp):                         */
/*    {"action":"uartsetup", "object":"sys.uartsetup",                    */
/*     "data":{"baud":N, "data_bits":8, "stop_bits":1, "parity":"n"}}     */
/*                                                                        */
/*  K144 ACKs at the OLD baud, then switches.  Tab5 must mirror.          */
/* ---------------------------------------------------------------------- */

/**
 * @brief Negotiate a new UART baud with the K144.
 *
 * Adaptive contract (Option B):
 *   1. Send `sys.uartsetup` at the current baud.
 *   2. Wait for ACK at the OLD baud (per K144 docs).
 *   3. Switch Tab5's UART to @p new_baud via `uart_set_baudrate`.
 *   4. Verify with a `sys.ping` round-trip at @p new_baud.
 *   5. On ANY failure, revert Tab5 to the previous baud and re-ping;
 *      the K144 should fall back too on its own ack-timeout watchdog.
 *
 * @param new_baud  Target baud (e.g. 115200, 460800, 921600, 1000000).
 *                  AX630C UART tested up to 1.5 Mbps; ESP32-P4 supports
 *                  much higher but signal integrity over the M5-Bus +
 *                  Mate FPC chain caps the practical ceiling around 1M.
 *
 * @return ESP_OK if both sides successfully switched and ping succeeds;
 *         ESP_ERR_TIMEOUT if K144 didn't ACK uartsetup;
 *         ESP_ERR_INVALID_RESPONSE if K144 NACK'd or post-switch ping failed
 *         (Tab5 reverts in this case);
 *         propagated UART error on Tab5-side baud-set failure.
 */
esp_err_t voice_m5_llm_set_baud(uint32_t new_baud);

/**
 * @brief Tab5-side current UART baud as last successfully negotiated.
 *
 * Reports the baud Tab5's UART is configured at.  Defaults to
 * `TAB5_PORT_C_UART_BAUD` (115200) at boot before any switch.
 */
uint32_t voice_m5_llm_get_baud(void);

/* ---------------------------------------------------------------------- */
/*  Phase 6c — Text-to-Speech via the K144 TTS unit                       */
/*                                                                        */
/*  K144 has the `single_speaker_english_fast` voice model (English) and  */
/*  `melotts` (Chinese) installed.  Setup with                            */
/*    response_format = "tts.base64.wav"                                  */
/*  returns a single inference response carrying base64-encoded raw       */
/*  16-bit signed-LE PCM at 16 kHz mono in `data` (despite the .wav name  */
/*  the K144 emits headerless PCM, not RIFF — confirmed via Phase 6c      */
/*  TCP spike).                                                           */
/*                                                                        */
/*  Tab5's speaker pipeline runs at 48 kHz; callers should upsample 1:3   */
/*  before piping into `tab5_audio_play_raw`.                             */
/* ---------------------------------------------------------------------- */

/**
 * @brief Synthesize @p text into 16 kHz mono PCM via the K144 TTS unit.
 *
 * @param text       Text to speak.  Must be non-NULL/non-empty.
 * @param pcm_out    Caller buffer for 16 kHz mono int16_t samples.
 * @param max_samples Capacity of @p pcm_out in samples.
 * @param[out] out_samples  Number of samples actually written.
 * @param timeout_s  Hard cap for the full setup+inference round-trip.
 *
 * @return ESP_OK on success;
 *         ESP_ERR_TIMEOUT if the K144 doesn't finish within timeout;
 *         ESP_ERR_INVALID_ARG on NULL inputs;
 *         ESP_ERR_INVALID_RESPONSE on parse / base64 / non-zero error from K144.
 */
esp_err_t voice_m5_llm_tts(const char *text, int16_t *pcm_out, size_t max_samples, size_t *out_samples,
                           uint32_t timeout_s);

/**
 * @brief Recovery: try @p candidate_baud blindly to find a stranded K144.
 *
 * After a failed `voice_m5_llm_set_baud`, the K144 may be stuck at the
 * new baud while Tab5 has reverted.  This function force-switches Tab5
 * to @p candidate_baud and pings.  If the ping succeeds, both ends are
 * now reunited at @p candidate_baud and we attempt a graceful switch
 * back to 115200 from there.  No protocol-level uartsetup is sent at
 * the start — pure transport-level probe.
 *
 * Useful as a one-shot bench helper or last-ditch recovery before
 * power-cycle.  Costs O(1) ping at the candidate.
 *
 * @return ESP_OK if the K144 was found at @p candidate_baud and both
 *         ends are now at 115200 again;
 *         ESP_ERR_INVALID_RESPONSE if no ping at @p candidate_baud
 *         (K144 is somewhere else or genuinely dead).
 */
esp_err_t voice_m5_llm_recover_baud(uint32_t candidate_baud);

/* ---------------------------------------------------------------------- */
/*  Phase 6b — Autonomous K144 voice-assistant chain                      */
/*                                                                        */
/*  K144 has its own onboard mic (verified via /opt/usr/bin/tinycap on    */
/*  the AX630C: RMS jumps 8× when the user speaks).  Push-PCM-over-JSON   */
/*  is NOT a workable path — the asr unit's `input` field accepts a       */
/*  publish/subscribe queue name, not inline frames.  Instead, we wire    */
/*  the four units together on the K144 ITSELF and just drain output     */
/*  frames over UART.                                                     */
/*                                                                        */
/*  Chain topology (from `/tmp/m5spike/chain_probe.py` — TCP-verified):   */
/*    audio.setup  capture=card0/dev0 (onboard mic), play=card0/dev1     */
/*       └── publishes "sys.pcm" to internal ZMQ                          */
/*    asr.setup    input=["sys.pcm"], stream out                          */
/*       └── publishes "asr.NNNN" stream                                  */
/*    llm.setup    input=[asr_id], stream out                             */
/*       └── publishes "llm.NNNN" stream                                  */
/*    tts.setup    input=[llm_id], base64.wav                             */
/*       └── audible on K144's own speaker AND emits frames over UART    */
/*                                                                        */
/*  The chain runs autonomously after setup — Tab5 is purely a passive    */
/*  observer.  TTS audio also plays through the K144 speaker locally,    */
/*  but the chain emits the same audio over UART so Tab5 can render it    */
/*  through its own speaker via tab5_audio_play_raw if desired.           */
/* ---------------------------------------------------------------------- */

/** Opaque chain handle. */
typedef struct voice_m5_chain_handle voice_m5_chain_handle_t;

/** Per-frame callbacks invoked from voice_m5_llm_chain_run during drain. */
typedef void (*voice_m5_chain_text_cb)(const char *text, bool from_llm, bool finish, void *user);
typedef void (*voice_m5_chain_audio_cb)(const int16_t *pcm_16k_mono, size_t samples, void *user);

/**
 * @brief Bring up the autonomous voice-assistant chain on the K144.
 *
 * Issues four `setup` calls in sequence (audio → asr → llm → tts), each
 * subscribing to the previous unit's output.  After this call the K144's
 * mic is live and audio flows internally; ASR/LLM/TTS frames also leak
 * out on the UART for Tab5 to drain via voice_m5_llm_chain_run.
 *
 * @param[out] out_handle  Receives the new chain handle.  Free with
 *                         @ref voice_m5_llm_chain_teardown.
 *
 * @return ESP_OK on success; ESP_ERR_TIMEOUT / ESP_ERR_INVALID_RESPONSE
 *         on per-stage failure (caller must NOT call teardown on a
 *         non-OK return — handle is set to NULL); ESP_ERR_NO_MEM on
 *         allocation failure.
 */
esp_err_t voice_m5_llm_chain_setup(voice_m5_chain_handle_t **out_handle);

/**
 * @brief Drain output frames from a live chain, invoking callbacks.
 *
 * Blocking — runs until @p stop_flag transitions to true OR @p timeout_s
 * elapses.  Frames arrive in any order from the four work_ids:
 *   - asr.NNNN → text_cb(delta, from_llm=false, finish, user)
 *   - llm.NNNN → text_cb(delta, from_llm=true,  finish, user)
 *   - tts.NNNN → base64-decoded into PSRAM scratch, then audio_cb(pcm,...)
 *
 * Callbacks may be NULL — that frame type is then silently consumed.
 *
 * @return ESP_OK on stop_flag-driven exit;
 *         ESP_ERR_TIMEOUT if @p timeout_s elapsed;
 *         ESP_ERR_INVALID_ARG / ESP_ERR_INVALID_STATE on bad inputs;
 *         propagated UART error on transport failure.
 */
esp_err_t voice_m5_llm_chain_run(voice_m5_chain_handle_t *handle, voice_m5_chain_text_cb text_cb,
                                 voice_m5_chain_audio_cb audio_cb, void *user, volatile bool *stop_flag,
                                 uint32_t timeout_s);

/**
 * @brief Tear down a chain — issues `exit` to each unit in reverse order
 *        and frees the handle.  Safe with NULL.
 */
void voice_m5_llm_chain_teardown(voice_m5_chain_handle_t *handle);

#ifdef __cplusplus
}
#endif
