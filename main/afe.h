/**
 * TinkerTab — ESP-SR Audio Front End (AEC + Wake Word)
 *
 * Wraps Espressif's ESP-SR AFE pipeline for:
 *   - AEC (Acoustic Echo Cancellation) using hardware reference signal
 *   - NS (Noise Suppression)
 *   - VAD (Voice Activity Detection) via VADNet
 *   - KWS (Keyword Spotting) via WakeNet9
 *
 * Feed interleaved [mic1, mic2, ref] audio at 16kHz 16-bit.
 * Fetch returns clean (AEC'd) audio + wake/VAD state.
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * Initialize the AFE pipeline.
 * Allocates models in PSRAM, creates internal state.
 * Must be called after audio/mic init.
 *
 * @return ESP_OK on success
 */
esp_err_t tab5_afe_init(void);

/**
 * Shut down the AFE pipeline and free resources.
 */
void tab5_afe_deinit(void);

/**
 * Feed a 20ms frame of interleaved audio to the AFE.
 *
 * @param data  Interleaved [mic1, mic2, ref] int16 samples.
 *              320 samples per channel × 3 channels = 960 int16_t values.
 * @param len   Number of int16_t values (must be 960 for 20ms @ 16kHz, 3ch)
 * @return ESP_OK on success
 */
esp_err_t tab5_afe_feed(const int16_t *data, int len);

/**
 * Fetch processed audio + detection results from the AFE.
 * Blocks until a result is available (~20ms cadence).
 *
 * @param[out] out_data     Pointer to clean audio buffer (single-channel 16kHz)
 * @param[out] out_samples  Number of samples in out_data
 * @param[out] out_wake     True if wake word was detected in this frame
 * @param[out] out_vad      True if voice activity detected
 * @return ESP_OK on success
 */
esp_err_t tab5_afe_fetch(int16_t **out_data, int *out_samples,
                         bool *out_wake, bool *out_vad);

/**
 * Check if the AFE is initialized and running.
 */
bool tab5_afe_is_active(void);

/**
 * Get the wake word model name (e.g. "hilexin").
 */
const char *tab5_afe_wake_word_name(void);

/**
 * Get the number of int16_t samples expected per feed() call.
 * This is total samples across all channels (e.g. 480*3=1440 for "MMR").
 */
int tab5_afe_get_feed_chunksize(void);
