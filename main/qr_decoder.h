/**
 * qr_decoder — quirc wrapper for Tab5.
 *
 * Owns the quirc state machine; caller feeds 8-bit grayscale frames
 * (W × H, row-major) and gets back a decoded QR payload string when
 * one is found.  Working buffer (~W*H bytes) lives in PSRAM and is
 * allocated on init; freed by qr_decoder_free.
 *
 * One-shot use is fine: init → decode_frame loop → free.
 *
 * TT #370 — vendored quirc 1.2 (ISC) under main/quirc/.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct qr_decoder qr_decoder_t;

esp_err_t qr_decoder_init(qr_decoder_t **out, int width, int height);

/** Decode one frame.  Returns ESP_OK + writes NUL-terminated payload
 *  into out_buf if a QR was decoded, ESP_ERR_NOT_FOUND if no code
 *  was visible, ESP_ERR_INVALID_ARG on bad inputs. */
esp_err_t qr_decoder_decode_frame(qr_decoder_t *d, const uint8_t *gray, char *out_buf, size_t out_cap);

void qr_decoder_free(qr_decoder_t *d);

#ifdef __cplusplus
}
#endif
