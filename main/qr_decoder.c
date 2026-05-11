/* qr_decoder — quirc wrapper.
 *
 * Tab5 ↔ OpenRouter solo-mode key provisioning works by scanning a
 * QR encoding the JSON payload (see spec).  The Tab5 camera path
 * already has a 1280×720 grayscale path; this module just carries
 * the quirc state + glue.
 *
 * TT #370 — see qr_decoder.h.
 */

#include "qr_decoder.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "quirc/quirc.h"

static const char *TAG = "qr_decoder";

struct qr_decoder {
   struct quirc *q;
};

esp_err_t qr_decoder_init(qr_decoder_t **out, int width, int height) {
   if (!out || width <= 0 || height <= 0) return ESP_ERR_INVALID_ARG;
   qr_decoder_t *d = heap_caps_calloc(1, sizeof *d, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (!d) return ESP_ERR_NO_MEM;
   d->q = quirc_new();
   if (!d->q) {
      heap_caps_free(d);
      return ESP_ERR_NO_MEM;
   }
   if (quirc_resize(d->q, width, height) < 0) {
      ESP_LOGE(TAG, "quirc_resize(%d,%d) failed", width, height);
      quirc_destroy(d->q);
      heap_caps_free(d);
      return ESP_ERR_NO_MEM;
   }
   *out = d;
   return ESP_OK;
}

esp_err_t qr_decoder_decode_frame(qr_decoder_t *d, const uint8_t *gray, char *out_buf, size_t out_cap) {
   if (!d || !gray || !out_buf || out_cap < 2) return ESP_ERR_INVALID_ARG;
   int w, h;
   uint8_t *fb = quirc_begin(d->q, &w, &h);
   memcpy(fb, gray, (size_t)w * (size_t)h);
   quirc_end(d->q);
   int n = quirc_count(d->q);
   if (n <= 0) return ESP_ERR_NOT_FOUND;
   for (int i = 0; i < n; i++) {
      struct quirc_code code;
      struct quirc_data data;
      quirc_extract(d->q, i, &code);
      quirc_decode_error_t err = quirc_decode(&code, &data);
      if (err == QUIRC_SUCCESS) {
         size_t copy = data.payload_len < (int)(out_cap - 1) ? (size_t)data.payload_len : out_cap - 1;
         memcpy(out_buf, data.payload, copy);
         out_buf[copy] = '\0';
         return ESP_OK;
      }
   }
   return ESP_ERR_NOT_FOUND;
}

void qr_decoder_free(qr_decoder_t *d) {
   if (!d) return;
   quirc_destroy(d->q);
   heap_caps_free(d);
}
