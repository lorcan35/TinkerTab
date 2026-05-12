/*
 * ui_audio_cues.h — short UI feedback chirps over the Tab5 speaker.
 *
 * Wave 8 of the cross-stack cohesion audit (2026-05-11).  Audit found
 * "device is mute except for LLM speech" and flagged audio cues as
 * the single largest perceived-quality jump per hour of work
 * available.  This module owns the small set of pre-computed PCM
 * tone buffers + a fire-and-forget API to play one.
 *
 * ## Architecture
 *
 * `ui_audio_cues_init()` is called once at boot from main.c after
 * the audio + worker subsystems are up.  It pre-computes one PCM
 * buffer per cue ID in PSRAM, sized for 48 kHz mono int16 with a
 * short envelope (10 ms attack, 10 ms release) so playback doesn't
 * click.
 *
 * `ui_audio_cue_play(id)` enqueues a job onto the shared
 * `tab5_worker`.  The worker enables the NS4150B amp, calls
 * `tab5_audio_play_raw()` (blocks for the cue duration — ~80 ms),
 * then disables the amp.  The LVGL caller never blocks.
 *
 * ## Why not voice_playback_buf_write?
 *
 * That path queues into the main TTS playback ring, which means
 * (a) the cue would play AFTER any currently-playing TTS, and
 * (b) the drain task's speaker-disable logic depends on the
 * `s_tts_done` flag from the turn lifecycle, so a cue triggered
 * outside a turn would leave the amp on indefinitely.  The
 * bypass-via-worker path keeps cues self-contained.
 *
 * ## Failure mode
 *
 * If pre-computed buffers are missing (init never ran or alloc
 * failed), `ui_audio_cue_play()` is a logged no-op — the UI flow
 * continues uninterrupted.  Cues are sugar; never load-bearing.
 */
#pragma once

#include "esp_err.h"

typedef enum {
   UI_CUE_MODE_SWITCH = 0,
   UI_CUE_COUNT,
} ui_cue_t;

/* Pre-compute all cue PCM buffers in PSRAM.  Idempotent — safe to
 * call more than once at boot.  Returns ESP_OK on success or an
 * ESP error if any allocation failed (any successful pre-allocations
 * are kept; only the failed cue is dropped). */
esp_err_t ui_audio_cues_init(void);

/* Fire-and-forget cue playback.  Returns immediately; actual audio
 * runs on the shared task_worker.  No-op if `id` is out of range
 * or the cue's pre-computed buffer is missing. */
void ui_audio_cue_play(ui_cue_t id);
