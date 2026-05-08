/* openrouter_sse — line-buffered SSE parser.
 *
 * Tab5 ↔ OpenRouter chat-completions streaming uses SSE (text/event-
 * stream) framing.  Each "event" is a `data: <json>` line followed by
 * a blank line; the stream terminates with `data: [DONE]`.  This
 * module owns the buffering + line-splitting; cJSON parsing of the
 * `<json>` payload happens in the caller (openrouter_client.c).
 *
 * TT #370 — see openrouter_sse.h.
 */

#include "openrouter_sse.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#define LINE_BUF_BYTES 4096

struct openrouter_sse_state {
   char *line;
   size_t line_len;
   bool done;
   openrouter_sse_data_cb_t cb;
   void *ctx;
};

static const char *TAG = "or_sse";

esp_err_t openrouter_sse_init(openrouter_sse_state_t **out,
                              openrouter_sse_data_cb_t cb, void *ctx) {
   if (!out || !cb) return ESP_ERR_INVALID_ARG;
   openrouter_sse_state_t *s =
       heap_caps_calloc(1, sizeof(*s), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (!s) return ESP_ERR_NO_MEM;
   s->line = heap_caps_malloc(LINE_BUF_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (!s->line) {
      heap_caps_free(s);
      return ESP_ERR_NO_MEM;
   }
   s->cb = cb;
   s->ctx = ctx;
   *out = s;
   return ESP_OK;
}

static void emit_line(openrouter_sse_state_t *s) {
   if (s->line_len == 0) return;
   size_t n = s->line_len;
   /* Trim trailing CR (CRLF servers). */
   if (s->line[n - 1] == '\r') n--;
   if (n == 0) return;
   /* Comment line — SSE spec says ignore. */
   if (s->line[0] == ':') return;
   /* Only `data:` lines are interesting for us.  `event:`/`id:`/
    * `retry:` lines exist in the spec but OpenRouter chat-completions
    * doesn't use them. */
   static const char kPrefix[] = "data:";
   if (n < sizeof(kPrefix) - 1 ||
       memcmp(s->line, kPrefix, sizeof(kPrefix) - 1) != 0) {
      return;
   }
   const char *p = s->line + sizeof(kPrefix) - 1;
   size_t pn = n - (sizeof(kPrefix) - 1);
   /* Optional single space after the colon (per SSE spec). */
   if (pn > 0 && *p == ' ') {
      p++;
      pn--;
   }
   if (pn == 6 && memcmp(p, "[DONE]", 6) == 0) {
      s->done = true;
      return;
   }
   s->cb(p, pn, s->ctx);
}

bool openrouter_sse_feed(openrouter_sse_state_t *s, const char *buf, size_t len) {
   if (!s || !buf) return s ? s->done : false;
   for (size_t i = 0; i < len; i++) {
      char c = buf[i];
      if (c == '\n') {
         emit_line(s);
         s->line_len = 0;
      } else if (s->line_len + 1 < LINE_BUF_BYTES) {
         s->line[s->line_len++] = c;
      } else {
         ESP_LOGW(TAG, "SSE line overflow at %u bytes — dropping", (unsigned)s->line_len);
         s->line_len = 0;
      }
   }
   return s->done;
}

void openrouter_sse_free(openrouter_sse_state_t *s) {
   if (!s) return;
   heap_caps_free(s->line);
   heap_caps_free(s);
}
