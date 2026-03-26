/**
 * TinkerClaw Tab5 — Audio (ES8388 codec + ES7210 dual mic)
 *
 * ES8388: I2S DAC for speaker output via NS4150B amplifier
 * ES7210: I2S ADC for dual MEMS microphone input
 *
 * Speaker amp (NS4150B) is controlled via IO expander PI4IOE1 bit P1.
 */
#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// ---- ES8388 Codec (Playback) ----

/**
 * Initialize ES8388 codec and I2S TX channel for audio playback.
 * Must be called after tab5_io_expander_init().
 */
esp_err_t tab5_audio_init(i2c_master_bus_handle_t i2c_bus);

/** Play raw 16-bit PCM samples through the speaker. Blocks until complete. */
esp_err_t tab5_audio_play_raw(const int16_t *data, size_t samples);

/** Set speaker volume (0-100). */
esp_err_t tab5_audio_set_volume(uint8_t vol);

/** Get current speaker volume (0-100). */
uint8_t tab5_audio_get_volume(void);

/** Enable/disable NS4150B speaker amplifier via IO expander. */
esp_err_t tab5_audio_speaker_enable(bool enable);

// ---- ES7210 Dual Mic (Recording) ----

/**
 * Initialize ES7210 ADC and I2S RX channel for microphone input.
 * Uses a separate I2S port from the codec.
 */
esp_err_t tab5_mic_init(i2c_master_bus_handle_t i2c_bus);

/** Read mic samples into buf. Returns actual samples read. Blocks up to timeout_ms. */
esp_err_t tab5_mic_read(int16_t *buf, size_t samples, uint32_t timeout_ms);

/** Set microphone gain in dB (0-36). */
esp_err_t tab5_mic_set_gain(uint8_t gain_db);
