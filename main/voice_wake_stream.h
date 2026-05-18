/**
 * @file voice_wake_stream.h
 * @brief Tab5 → Dragon mic streaming for wakeword detection (TT #615).
 *
 * Path B from PLAN-tab5-mic-to-k144-asr.md: Tab5's ES7210 quad-mic ships
 * 16 kHz mono PCM continuously to Dragon as WAK0-tagged binary frames
 * whenever voice WS is connected AND the voice state machine is in a
 * quiescent state (IDLE/READY).  Dragon runs a sliding-window whisper.cpp
 * pass on the stream and emits {"type":"wake"} back when it matches
 * "hey tinker" / "hey thinker" — Tab5 routes that to
 * ui_home_start_voice_turn() (same path as orb tap).
 *
 * Lifecycle:
 *   - voice_wake_stream_init()    — called once from main.c after voice_init
 *   - voice_wake_stream_arm()     — start the background task; called when
 *                                   WS goes READY and the user wants
 *                                   always-on wake (default on)
 *   - voice_wake_stream_disarm()  — stop the background task
 *   - voice_wake_stream_on_state_change() — informs the module of voice
 *                                   state transitions so it can pause
 *                                   sending during a real voice turn
 *                                   (the existing mic_task owns the
 *                                   mic during LISTENING)
 *
 * Mic ownership: only ONE consumer of the I2S RX channel at a time.
 * When voice_state ∈ {LISTENING, PROCESSING, SPEAKING, RECONNECTING}
 * the existing mic_task is the active consumer.  The wake-stream task
 * is paused (no I2S reads).  On the SPEAKING→READY edge the wake-stream
 * task resumes — though in practice conversation mode (TT #613) will
 * usually re-listen first, so wake-stream resumes only at the end of
 * the conversation.
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Spawn the wake-stream background task in the suspended state.
 *  Idempotent; safe to call from any task.  Returns ESP_OK on success. */
esp_err_t voice_wake_stream_init(void);

/** @brief Allow the wake-stream task to capture + transmit when the
 *  voice state machine is quiescent.  Has no effect if the task isn't
 *  initialized yet. */
void voice_wake_stream_arm(void);

/** @brief Disarm — the task stays alive but won't capture or transmit. */
void voice_wake_stream_disarm(void);

/** @brief Whether wake-stream is currently armed AND actively pumping
 *  frames (i.e. armed && voice_state == IDLE/READY && WS connected). */
bool voice_wake_stream_is_active(void);

/** @brief Inform the module that voice_state has transitioned.  Wake-
 *  stream needs to know this so it pauses while a real voice turn
 *  consumes the mic.  Called from voice.c::voice_set_state. */
void voice_wake_stream_on_state_change(int new_state);

#ifdef __cplusplus
}
#endif
