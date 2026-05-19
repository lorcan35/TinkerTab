/**
 * @file ima_adpcm.h
 * @brief IMA ADPCM 4:1 encoder for 16 kHz mono int16 audio (TT #131).
 *
 * Compresses real-time PCM to fit Tab5↔K144 UART control-plane
 * bandwidth budget (~13 KB/s sustained at 1.5 Mbps after daemon
 * overhead).  Raw 16k mono int16 = 32 KB/s; IMA ADPCM nibbles =
 * 8 KB/s + tiny per-chunk header.
 *
 * Each encoded chunk is self-contained — predictor + step index
 * resets per call.  K144 ext_pcm decodes the same way: header has
 * the first sample + step index, nibbles follow.
 *
 * Block layout (matches Microsoft / IMA ADPCM wave format):
 *
 *     [int16 first_sample][uint8 step_index][uint8 reserved=0]
 *     [4-bit nibble × (N-1)]
 *
 * Total output bytes for N samples:  4 + ((N - 1 + 1) / 2)
 * For N = 1600 (100 ms @ 16 kHz):    804 bytes.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Bytes needed to encode @p sample_count int16 samples. */
static inline size_t ima_adpcm_encoded_size(size_t sample_count) {
    if (sample_count == 0) return 0;
    return 4 + (sample_count / 2);
}

/**
 * @brief Encode @p in_samples (int16 mono) to IMA ADPCM in @p out.
 * @return Bytes written, or 0 on overflow / invalid input.
 */
size_t ima_adpcm_encode(const int16_t *in, size_t sample_count, uint8_t *out, size_t out_cap);

/**
 * @brief Decode IMA ADPCM bytes to int16 samples.  out_cap is in
 *        SAMPLES, not bytes.  Returns sample count written.
 */
size_t ima_adpcm_decode(const uint8_t *in, size_t in_len, int16_t *out, size_t out_cap_samples);

#ifdef __cplusplus
}
#endif
