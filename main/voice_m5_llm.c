/**
 * @file voice_m5_llm.c
 * @brief Implementation — see voice_m5_llm.h for API contract.
 *
 * SOLID notes:
 *   - DIP: depends only on the abstract APIs of `uart_port_c` (transport)
 *     and `m5_stackflow` (marshalling).  Neither concrete protocol nor
 *     pin numbers leak into this file.
 *   - SRP: this module only owns the LLM lifecycle.  No UART byte poking,
 *     no JSON parsing — those live in their respective layers.
 *   - OCP: the streaming-collect loop dispatches via
 *     `m5_stackflow_response_is_stream` so future K144 units (whisper.asr,
 *     melotts) can reuse the same loop with a different work_id+action,
 *     no edits here.
 *
 * The K144's documented LLM lifecycle (per the M5Module-LLM Arduino lib):
 *   1. sys.reset       — implicit on boot; we don't issue it (rate-limited
 *                        and risks dropping any concurrent unit that's set up).
 *   2. llm.setup       — returns work_id ("llm.1000" etc.) we cache.
 *   3. llm.inference   — streams chunks until finish==true.
 *   4. llm.exit        — releases the work_id.
 *
 * We treat `llm.setup` as session-scoped: cache the work_id and reuse it
 * across calls.  `voice_m5_llm_release` issues `llm.exit` and clears the
 * cache.
 */

#include "voice_m5_llm.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "m5_stackflow.h"
#include "uart_port_c.h"

static const char *TAG = "voice_m5_llm";

/* ---------------------------------------------------------------------- */
/*  Tunables — keep co-located with the constants they constrain          */
/* ---------------------------------------------------------------------- */

/* Bundled model on the K144 (Phase 0 bench package).  When M5 ships
 * additional models we'll want a setter, but for now this is fixed. */
#define M5_LLM_MODEL "qwen2.5-0.5B-prefill-20e"

/* Setup parameters (per M5 docs + Phase 0 bench). */
#define M5_LLM_RESPONSE_FORMAT "llm.utf-8.stream"
#define M5_LLM_INPUT_FORMAT "llm.utf-8"
#define M5_LLM_MAX_TOKENS 512
#define M5_LLM_PROMPT_PREFIX "You are a helpful, concise assistant."

/* Timeouts (ms) for the discrete protocol stages.  Total request budget
 * is the caller-supplied @p timeout_s. */
#define M5_PING_TIMEOUT_MS 500
#define M5_SETUP_TIMEOUT_MS 5000

/* RX scratch — sized for a few streaming chunks at a time.  Heap-allocated
 * once on first use, never freed. */
#define M5_RX_BUF_BYTES 4096

/* TX scratch — request frames stay well under 1 KB.  Stack-allocated. */
#define M5_TX_BUF_BYTES 1024

/* ---------------------------------------------------------------------- */
/*  Module state                                                          */
/* ---------------------------------------------------------------------- */

static char s_setup_work_id[32];   /* "llm.NNNN" or empty if not set up */
static uint32_t s_request_counter; /* monotonic; wraps after 4 B requests */
static char *s_rx_buf;             /* lazy heap_caps_malloc(SPIRAM) */
static size_t s_rx_len;            /* bytes already in s_rx_buf */

/* ---------------------------------------------------------------------- */
/*  Internal helpers                                                      */
/* ---------------------------------------------------------------------- */

static void make_request_id(char *buf, size_t buf_cap, const char *prefix) {
   snprintf(buf, buf_cap, "%s%" PRIu32, prefix, ++s_request_counter);
}

static esp_err_t ensure_uart(void) {
   esp_err_t err = tab5_port_c_uart_init();
   if (err != ESP_OK) {
      ESP_LOGE(TAG, "Port C UART init failed: %s", esp_err_to_name(err));
   }
   return err;
}

static esp_err_t ensure_rx_buf(void) {
   if (s_rx_buf != NULL) return ESP_OK;
   s_rx_buf = heap_caps_malloc(M5_RX_BUF_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (s_rx_buf == NULL) {
      ESP_LOGE(TAG, "rx_buf alloc (PSRAM, %d B) failed", M5_RX_BUF_BYTES);
      return ESP_ERR_NO_MEM;
   }
   s_rx_len = 0;
   return ESP_OK;
}

/* Send one already-built request and read up to one newline-terminated
 * response frame within @p timeout_ms.  Returns the frame length on
 * success (excluding the trailing \n).  The frame is left in s_rx_buf
 * with NUL termination. */
static int send_and_recv_one_frame(const char *tx, int tx_len, uint32_t timeout_ms) {
   tab5_port_c_flush();
   if (tab5_port_c_send(tx, (size_t)tx_len) != tx_len) {
      ESP_LOGE(TAG, "Port C send truncated");
      return -1;
   }

   if (ensure_rx_buf() != ESP_OK) return -1;
   s_rx_len = 0;

   int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
   while (esp_timer_get_time() < deadline && s_rx_len < M5_RX_BUF_BYTES - 1) {
      uint32_t remaining = (uint32_t)((deadline - esp_timer_get_time()) / 1000);
      if (remaining > 100) remaining = 100;
      int n = tab5_port_c_recv(s_rx_buf + s_rx_len, M5_RX_BUF_BYTES - 1 - s_rx_len, remaining);
      if (n > 0) {
         s_rx_len += (size_t)n;
         /* Look for the first \n — that's our frame boundary. */
         char *nl = memchr(s_rx_buf, '\n', s_rx_len);
         if (nl != NULL) {
            int frame_len = (int)(nl - s_rx_buf);
            *nl = '\0'; /* convenient for cJSON */
            return frame_len;
         }
      }
   }
   return -1;
}

/* Streaming receive — accumulates `delta` strings into output until
 * `finish == true` or @p deadline_us is hit.  Returns ESP_OK / ESP_ERR_TIMEOUT
 * / ESP_ERR_INVALID_RESPONSE.  output_used reflects how many bytes were
 * written (excluding NUL). */
static esp_err_t stream_collect(const char *expected_request_id, char *output, size_t output_cap, int64_t deadline_us,
                                size_t *output_used) {
   *output_used = 0;
   if (output_cap > 0) output[0] = '\0';

   if (ensure_rx_buf() != ESP_OK) return ESP_ERR_NO_MEM;
   s_rx_len = 0;

   while (esp_timer_get_time() < deadline_us) {
      /* Refill rx_buf until we have at least one \n. */
      char *nl = memchr(s_rx_buf, '\n', s_rx_len);
      while (nl == NULL && s_rx_len < M5_RX_BUF_BYTES - 1 && esp_timer_get_time() < deadline_us) {
         uint32_t budget_ms = (uint32_t)((deadline_us - esp_timer_get_time()) / 1000);
         if (budget_ms > 100) budget_ms = 100;
         int n = tab5_port_c_recv(s_rx_buf + s_rx_len, M5_RX_BUF_BYTES - 1 - s_rx_len, budget_ms);
         if (n > 0) {
            s_rx_len += (size_t)n;
            nl = memchr(s_rx_buf, '\n', s_rx_len);
         }
      }
      if (nl == NULL) {
         ESP_LOGW(TAG, "stream_collect: timeout waiting for next frame");
         return ESP_ERR_TIMEOUT;
      }

      /* Pop one frame from the front of rx_buf. */
      size_t frame_len = (size_t)(nl - s_rx_buf);
      m5_stackflow_response_t resp = {0};
      esp_err_t pe = m5_stackflow_parse_response(s_rx_buf, frame_len, &resp);
      /* Slide remaining bytes down. */
      size_t consumed = frame_len + 1; /* include the \n */
      if (consumed < s_rx_len) {
         memmove(s_rx_buf, s_rx_buf + consumed, s_rx_len - consumed);
      }
      s_rx_len -= consumed;

      if (pe != ESP_OK) {
         ESP_LOGW(TAG, "parse_response failed (%s) — discarding frame", esp_err_to_name(pe));
         continue;
      }

      if (!m5_stackflow_response_matches(&resp, expected_request_id)) {
         /* Stale frame from a prior request — drop and keep reading. */
         m5_stackflow_response_free(&resp);
         continue;
      }

      if (resp.error_code != 0) {
         ESP_LOGE(TAG, "K144 returned error %d (%s)", resp.error_code, resp.error_message ? resp.error_message : "");
         m5_stackflow_response_free(&resp);
         return ESP_ERR_INVALID_RESPONSE;
      }

      m5_stackflow_stream_chunk_t chunk = {0};
      esp_err_t ee = m5_stackflow_extract_stream_chunk(&resp, &chunk);
      if (ee != ESP_OK) {
         /* Non-stream frame — could be an early ack with data:"None". */
         m5_stackflow_response_free(&resp);
         continue;
      }

      if (chunk.delta != NULL && output_cap > 0) {
         size_t room = output_cap - 1 - *output_used;
         if (room > 0) {
            size_t copy = strlen(chunk.delta);
            if (copy > room) copy = room;
            memcpy(output + *output_used, chunk.delta, copy);
            *output_used += copy;
            output[*output_used] = '\0';
         }
      }

      bool fin = chunk.finish;
      m5_stackflow_response_free(&resp);
      if (fin) return ESP_OK;
   }

   return ESP_ERR_TIMEOUT;
}

/* Issue llm.setup, capture the returned work_id. */
static esp_err_t do_llm_setup(void) {
   if (s_setup_work_id[0] != '\0') return ESP_OK;

   /* Wire shape from M5Module-LLM v1.7.0 source (api_llm.cpp:setup):
    *   data.input  is an ARRAY in v1.1+ (was string in v1.0)
    *   data.enkws  is required (false unless using KWS unit)
    * Sending input as a string trips "task full" / silent rejection on
    * later infer (Phase 3.5 dig). */
   cJSON *data = cJSON_CreateObject();
   if (data == NULL) return ESP_ERR_NO_MEM;
   cJSON_AddStringToObject(data, "model", M5_LLM_MODEL);
   cJSON_AddStringToObject(data, "response_format", M5_LLM_RESPONSE_FORMAT);
   cJSON *input_arr = cJSON_CreateArray();
   cJSON_AddItemToArray(input_arr, cJSON_CreateString(M5_LLM_INPUT_FORMAT));
   cJSON_AddItemToObject(data, "input", input_arr);
   cJSON_AddBoolToObject(data, "enoutput", true);
   cJSON_AddBoolToObject(data, "enkws", false);
   cJSON_AddNumberToObject(data, "max_token_len", M5_LLM_MAX_TOKENS);
   cJSON_AddStringToObject(data, "prompt", M5_LLM_PROMPT_PREFIX);

   char request_id[32];
   make_request_id(request_id, sizeof(request_id), "setup-");

   const m5_stackflow_request_t req = {
       .request_id = request_id,
       .work_id = "llm",
       .action = "setup",
       .object = "llm.setup",
       .data_json = data,
   };

   char tx[M5_TX_BUF_BYTES];
   int tx_len = m5_stackflow_build_request(&req, tx, sizeof(tx));
   cJSON_Delete(data);
   if (tx_len < 0) {
      ESP_LOGE(TAG, "build_request(setup) failed");
      return ESP_ERR_NO_MEM;
   }

   int frame_len = send_and_recv_one_frame(tx, tx_len, M5_SETUP_TIMEOUT_MS);
   if (frame_len < 0) {
      ESP_LOGE(TAG, "llm.setup: no response within %d ms", M5_SETUP_TIMEOUT_MS);
      return ESP_ERR_TIMEOUT;
   }

   m5_stackflow_response_t resp = {0};
   esp_err_t pe = m5_stackflow_parse_response(s_rx_buf, (size_t)frame_len, &resp);
   if (pe != ESP_OK) return ESP_ERR_INVALID_RESPONSE;

   if (!m5_stackflow_response_matches(&resp, request_id) || resp.error_code != 0 || resp.work_id == NULL) {
      ESP_LOGE(TAG, "llm.setup ack mismatch or err=%d msg='%s'", resp.error_code,
               resp.error_message ? resp.error_message : "");
      m5_stackflow_response_free(&resp);
      return ESP_ERR_INVALID_RESPONSE;
   }

   strncpy(s_setup_work_id, resp.work_id, sizeof(s_setup_work_id) - 1);
   s_setup_work_id[sizeof(s_setup_work_id) - 1] = '\0';
   ESP_LOGI(TAG, "llm.setup ok — work_id=%s model=%s", s_setup_work_id, M5_LLM_MODEL);

   m5_stackflow_response_free(&resp);
   return ESP_OK;
}

/* ---------------------------------------------------------------------- */
/*  Public API                                                            */
/* ---------------------------------------------------------------------- */

esp_err_t voice_m5_llm_probe(void) {
   esp_err_t err = ensure_uart();
   if (err != ESP_OK) return err;

   char request_id[32];
   make_request_id(request_id, sizeof(request_id), "ping-");
   const m5_stackflow_request_t req = {
       .request_id = request_id,
       .work_id = "sys",
       .action = "ping",
   };

   char tx[256];
   int tx_len = m5_stackflow_build_request(&req, tx, sizeof(tx));
   if (tx_len < 0) return ESP_ERR_NO_MEM;

   int frame_len = send_and_recv_one_frame(tx, tx_len, M5_PING_TIMEOUT_MS);
   if (frame_len < 0) return ESP_ERR_TIMEOUT;

   m5_stackflow_response_t resp = {0};
   esp_err_t pe = m5_stackflow_parse_response(s_rx_buf, (size_t)frame_len, &resp);
   if (pe != ESP_OK) return ESP_ERR_INVALID_RESPONSE;

   bool match = m5_stackflow_response_matches(&resp, request_id);
   bool ok = match && resp.error_code == 0;
   m5_stackflow_response_free(&resp);
   return ok ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

esp_err_t voice_m5_llm_infer(const char *prompt, char *output, size_t output_cap, uint32_t timeout_s) {
   if (prompt == NULL || prompt[0] == '\0' || output == NULL || output_cap == 0) {
      return ESP_ERR_INVALID_ARG;
   }
   output[0] = '\0';

   esp_err_t err = ensure_uart();
   if (err != ESP_OK) return err;

   int64_t deadline_us = esp_timer_get_time() + (int64_t)timeout_s * 1000 * 1000;

   err = do_llm_setup();
   if (err != ESP_OK) return err;
   if (esp_timer_get_time() >= deadline_us) return ESP_ERR_TIMEOUT;

   /* Wire shape from M5Module-LLM v1.7.0 (api_llm.cpp:inference): the
    * inference request's `data` is a stream-frame OBJECT
    *   { "delta": <prompt>, "index": 0, "finish": true }
    * NOT a bare string.  Sending a string returns error -25
    * "Stream data index error" silently — Phase 3.5 dig surfaced this. */
   char request_id[32];
   make_request_id(request_id, sizeof(request_id), "infer-");
   cJSON *data = cJSON_CreateObject();
   if (data == NULL) return ESP_ERR_NO_MEM;
   cJSON_AddStringToObject(data, "delta", prompt);
   cJSON_AddNumberToObject(data, "index", 0);
   cJSON_AddBoolToObject(data, "finish", true);
   const m5_stackflow_request_t req = {
       .request_id = request_id,
       .work_id = s_setup_work_id,
       .action = "inference",
       .object = M5_LLM_RESPONSE_FORMAT,
       .data_json = data,
   };

   char tx[M5_TX_BUF_BYTES];
   int tx_len = m5_stackflow_build_request(&req, tx, sizeof(tx));
   cJSON_Delete(data);
   if (tx_len < 0) return ESP_ERR_NO_MEM;

   tab5_port_c_flush();
   if (tab5_port_c_send(tx, (size_t)tx_len) != tx_len) {
      return ESP_FAIL;
   }

   size_t used = 0;
   esp_err_t se = stream_collect(request_id, output, output_cap, deadline_us, &used);
   ESP_LOGI(TAG, "inference: %s, %u bytes written%s", esp_err_to_name(se), (unsigned)used,
            (used + 1 >= output_cap) ? " (truncated)" : "");
   return se;
}

void voice_m5_llm_release(void) {
   if (s_setup_work_id[0] == '\0') return;
   if (!tab5_port_c_uart_is_initialized()) {
      s_setup_work_id[0] = '\0';
      return;
   }

   char request_id[32];
   make_request_id(request_id, sizeof(request_id), "exit-");
   const m5_stackflow_request_t req = {
       .request_id = request_id,
       .work_id = s_setup_work_id,
       .action = "exit",
   };

   char tx[M5_TX_BUF_BYTES];
   int tx_len = m5_stackflow_build_request(&req, tx, sizeof(tx));
   if (tx_len > 0) {
      (void)send_and_recv_one_frame(tx, tx_len, M5_PING_TIMEOUT_MS);
   }
   ESP_LOGI(TAG, "released work_id=%s", s_setup_work_id);
   s_setup_work_id[0] = '\0';
}

bool voice_m5_llm_is_ready(void) { return s_setup_work_id[0] != '\0'; }
