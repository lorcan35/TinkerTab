/**
 * @file voice_ext_pcm_stream.h
 * @brief Tab5 mic → K144 ext_pcm streamer (TT #131 — Path A).
 *
 * Production path for wake_src=ext_pcm: Tab5's ES7210 quad-mic ships 16 kHz
 * mono PCM continuously to the K144 LLM Module over the existing Port C
 * UART JSON bridge, wrapped as base64 inside the custom `ingest` RPC the
 * StackFlow ext_pcm unit exposes:
 *
 *   { "request_id":"epN", "work_id":"ext_pcm",
 *     "action":"ingest", "data":"<base64 int16 LE 16kHz mono>" }
 *
 * ext_pcm decodes + publishes the PCM on /tmp/llm/pcm.cap.socket, which
 * llm_asr's sherpa-ncnn subscriber consumes.  Wake fires arrive back on
 * the same `asr.utf-8.stream` path the K144's onboard mic uses today.
 *
 * Bandwidth: 16 kHz × 16-bit × continuous → 256 kbps raw → 341 kbps after
 * base64 → ~360 kbps with JSON envelope.  Comfortably within UART's
 * 1.5 Mbps (1.5 Mbit/s = ~1500 kbps) ceiling on the M5-Bus + Mate FPC chain.
 *
 * Lifecycle:
 *   - voice_ext_pcm_stream_init()      — call once from voice_init
 *   - voice_ext_pcm_stream_arm()       — boot the K144 stream (issues the
 *                                        asr.setup + audio.cap_stop_all +
 *                                        ext_pcm.rebind handshake) and
 *                                        spawn the pump task
 *   - voice_ext_pcm_stream_disarm()    — stop pumping; tears down ASR
 *   - voice_ext_pcm_stream_on_state_change(int) — pauses while in-turn
 *                                        mic_task owns I2S
 *
 * Mic ownership: same rules as voice_wake_stream — only ONE consumer of
 * I2S RX at a time.  voice_mic_is_active() gates each pump iteration.
 */

#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t voice_ext_pcm_stream_init(void);
void voice_ext_pcm_stream_arm(void);
void voice_ext_pcm_stream_disarm(void);
bool voice_ext_pcm_stream_is_active(void);
void voice_ext_pcm_stream_on_state_change(int new_state);

/** TT #131 — diagnostic stats for /tinkeron/extpcm endpoint. */
typedef struct {
   bool task_running;
   bool armed;
   int voice_state;
   bool wakeword_active;
   const char *asr_id; /* borrowed; may be NULL */
   uint32_t frames_pumped;
   uint32_t last_mic_rms;    /* int16 abs-mean of last chunk; 0=silence */
   uint32_t last_tx_bytes;   /* size of last UART send */
   uint32_t last_send_ok;    /* 1 = last tab5_port_c_send returned ==tx_len */
   int64_t last_pump_age_ms; /* ms since last successful pump */
} voice_ext_pcm_stream_stats_t;

void voice_ext_pcm_stream_get_stats(voice_ext_pcm_stream_stats_t *out);

#ifdef __cplusplus
}
#endif
