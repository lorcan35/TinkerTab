/**
 * @file ima_adpcm.c
 * @brief IMA ADPCM 4-bit encoder/decoder.  Standard reference impl.
 */

#include "ima_adpcm.h"

#include <stddef.h>
#include <stdint.h>

static const int16_t step_table[89] = {
    7,    8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,   28,
    31,   34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,  118,
    130,  143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,  494,
    544,  598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878, 2066,
    2272, 2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845, 8630,
    9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

static const int8_t index_table[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8,
};

static inline int clamp_index(int idx) {
   if (idx < 0) return 0;
   if (idx > 88) return 88;
   return idx;
}

static inline int16_t clamp_int16(int32_t v) {
   if (v > 32767) return 32767;
   if (v < -32768) return -32768;
   return (int16_t)v;
}

/* Encode one sample with current predictor/step state.  Updates state.
 * Returns the 4-bit nibble. */
static uint8_t encode_sample(int16_t sample, int16_t *predictor, int *step_index) {
   int diff = sample - *predictor;
   int sign = (diff < 0) ? 8 : 0;
   if (diff < 0) diff = -diff;
   int step = step_table[*step_index];
   int delta = 0;
   int diff_q = step >> 3;
   if (diff >= step) {
      delta |= 4;
      diff -= step;
      diff_q += step;
   }
   if (diff >= step >> 1) {
      delta |= 2;
      diff -= step >> 1;
      diff_q += step >> 1;
   }
   if (diff >= step >> 2) {
      delta |= 1;
      diff -= step >> 2;
      diff_q += step >> 2;
   }
   if (sign) {
      *predictor = clamp_int16((int32_t)*predictor - diff_q);
   } else {
      *predictor = clamp_int16((int32_t)*predictor + diff_q);
   }
   *step_index = clamp_index(*step_index + index_table[delta | sign]);
   return (uint8_t)(delta | sign);
}

static int16_t decode_sample(uint8_t nibble, int16_t *predictor, int *step_index) {
   int step = step_table[*step_index];
   int diff_q = step >> 3;
   if (nibble & 4) diff_q += step;
   if (nibble & 2) diff_q += step >> 1;
   if (nibble & 1) diff_q += step >> 2;
   if (nibble & 8) {
      *predictor = clamp_int16((int32_t)*predictor - diff_q);
   } else {
      *predictor = clamp_int16((int32_t)*predictor + diff_q);
   }
   *step_index = clamp_index(*step_index + index_table[nibble]);
   return *predictor;
}

size_t ima_adpcm_encode(const int16_t *in, size_t sample_count, uint8_t *out, size_t out_cap) {
   if (in == NULL || out == NULL || sample_count == 0) return 0;
   size_t need = ima_adpcm_encoded_size(sample_count);
   if (out_cap < need) return 0;

   int16_t predictor = in[0];
   int step_index = 0;
   /* Header: int16 first_sample LE, uint8 step_index, uint8 reserved. */
   out[0] = (uint8_t)(predictor & 0xff);
   out[1] = (uint8_t)((predictor >> 8) & 0xff);
   out[2] = (uint8_t)step_index;
   out[3] = 0;

   /* Body: nibbles for samples [1..N-1].  Pack low-nibble then high. */
   size_t out_idx = 4;
   int low = 1;
   uint8_t accum = 0;
   for (size_t i = 1; i < sample_count; i++) {
      uint8_t nib = encode_sample(in[i], &predictor, &step_index);
      if (low) {
         accum = nib & 0x0f;
         low = 0;
      } else {
         accum |= (nib & 0x0f) << 4;
         out[out_idx++] = accum;
         low = 1;
      }
   }
   if (!low) {
      /* Odd sample count — flush the half-byte (high nibble = 0). */
      out[out_idx++] = accum;
   }
   return out_idx;
}

size_t ima_adpcm_decode(const uint8_t *in, size_t in_len, int16_t *out, size_t out_cap_samples) {
   if (in == NULL || out == NULL || in_len < 4) return 0;

   int16_t predictor = (int16_t)((uint16_t)in[0] | ((uint16_t)in[1] << 8));
   int step_index = clamp_index(in[2]);

   if (out_cap_samples == 0) return 0;
   out[0] = predictor;
   size_t out_idx = 1;
   /* nibbles start at byte 4 */
   for (size_t b = 4; b < in_len && out_idx < out_cap_samples; b++) {
      uint8_t low = in[b] & 0x0f;
      out[out_idx++] = decode_sample(low, &predictor, &step_index);
      if (out_idx >= out_cap_samples) break;
      uint8_t high = (in[b] >> 4) & 0x0f;
      out[out_idx++] = decode_sample(high, &predictor, &step_index);
   }
   return out_idx;
}
