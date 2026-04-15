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
#include "driver/i2s_types.h"
#include "audio_codec_data_if.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// ---- ES8388 Codec (Playback) + Shared I2S Bus ----

/**
 * Initialize ES8388 codec and shared I2S bus (TX + RX on I2S_NUM_1).
 * Must be called BEFORE tab5_mic_init() as it creates the I2S channels.
 */
esp_err_t tab5_audio_init(i2c_master_bus_handle_t i2c_bus);

/** Get the I2S RX channel handle (created by audio_init, used by mic_init). */
i2s_chan_handle_t tab5_audio_get_i2s_rx(void);

/** Get shared I2S data interface for esp_codec_dev (created by audio_init). */
const audio_codec_data_if_t *tab5_audio_get_data_if(void);

/** Play raw 16-bit PCM samples through the speaker. Blocks until complete. */
esp_err_t tab5_audio_play_raw(const int16_t *data, size_t samples);

/** Set speaker volume (0-100). */
esp_err_t tab5_audio_set_volume(uint8_t vol);

/** Get current speaker volume (0-100). */
uint8_t tab5_audio_get_volume(void);

/** Enable/disable NS4150B speaker amplifier via IO expander. */
esp_err_t tab5_audio_speaker_enable(bool enable);

/** Play a test tone (triangle wave) at given frequency for duration_ms. */
esp_err_t tab5_audio_test_tone(uint32_t freq_hz, uint32_t duration_ms);

// ---- ES7210 Dual Mic (Recording) ----

/**
 * Initialize ES7210 ADC and configure I2S RX in TDM mode.
 * Must be called AFTER tab5_audio_init() (uses shared I2S RX channel).
 */
esp_err_t tab5_mic_init(i2c_master_bus_handle_t i2c_bus);

/**
 * Read mic samples into buf (4-channel TDM interleaved at 48kHz).
 * Each "sample" is 4 int16_t values: [MIC1, AEC, MIC2, MIC_HP].
 */
esp_err_t tab5_mic_read(int16_t *buf, size_t samples, uint32_t timeout_ms);

/** Set microphone gain in dB (0-36). */
esp_err_t tab5_mic_set_gain(uint8_t gain_db);

/** Print ES7210 register dump + raw TDM data diagnostic. */
void tab5_mic_diag(void);
