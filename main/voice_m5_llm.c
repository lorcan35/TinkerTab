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
#include "mbedtls/base64.h"
#include "uart_port_c.h"

static const char *TAG = "voice_m5_llm";

/* All public entry points serialise on the Port C UART.  Long-lived callers
 * (chain_run) take/release per outer-loop iteration so a stop_flag flip can
 * land between frames.  See docs/AUDIT-k144-chain-2026-04-29.md item #1.
 * Recursive mutex permits an audio_cb invoked from chain_run to itself
 * call back into voice_m5_llm_* if a future feature needs that. */
#define M5_LOCK_OR_RETURN(timeout_ms)                                        \
   do {                                                                      \
      esp_err_t _le = tab5_port_c_lock(timeout_ms);                          \
      if (_le != ESP_OK) {                                                   \
         ESP_LOGW(TAG, "uart busy (timeout %u ms)", (unsigned)(timeout_ms)); \
         return _le;                                                         \
      }                                                                      \
   } while (0)

#define M5_UNLOCK() tab5_port_c_unlock()

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

/* RX scratch — sized for a single TTS response frame (base64 of ~10 sec
 * of 16 kHz mono PCM ≈ 425 KB; we cap at 8 sec so 340 KB covers it).
 * Heap-allocated in PSRAM once on first use, never freed.  P6c bumped
 * from 4 KB → 384 KB after observing TTS responses larger than the old
 * cap caused the read loop to spin until the buffer filled with no '\n'. */
#define M5_RX_BUF_BYTES (384 * 1024)

/* TX scratch — request frames stay well under 1 KB.  Stack-allocated. */
#define M5_TX_BUF_BYTES 1024

/* ---------------------------------------------------------------------- */
/*  Module state                                                          */
/* ---------------------------------------------------------------------- */

static char s_setup_work_id[32];   /* "llm.NNNN" or empty if not set up */
static char s_tts_work_id[32];     /* P6c — "tts.NNNN" or empty */
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
         /* P5: -4 "inference data push false" or -25 "Stream data index error"
          * mean our cached work_id is stale (e.g. K144 service was
          * restarted).  Clear it so the next infer triggers a fresh setup. */
         if (resp.error_code == -4 || resp.error_code == -25) {
            ESP_LOGW(TAG, "Clearing stale work_id=%s — next call will re-setup", s_setup_work_id);
            s_setup_work_id[0] = '\0';
         }
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
   M5_LOCK_OR_RETURN(2000);

   char request_id[32];
   make_request_id(request_id, sizeof(request_id), "ping-");
   const m5_stackflow_request_t req = {
       .request_id = request_id,
       .work_id = "sys",
       .action = "ping",
   };

   char tx[256];
   int tx_len = m5_stackflow_build_request(&req, tx, sizeof(tx));
   if (tx_len < 0) {
      M5_UNLOCK();
      return ESP_ERR_NO_MEM;
   }

   int frame_len = send_and_recv_one_frame(tx, tx_len, M5_PING_TIMEOUT_MS);
   if (frame_len < 0) {
      M5_UNLOCK();
      return ESP_ERR_TIMEOUT;
   }

   m5_stackflow_response_t resp = {0};
   esp_err_t pe = m5_stackflow_parse_response(s_rx_buf, (size_t)frame_len, &resp);
   if (pe != ESP_OK) {
      M5_UNLOCK();
      return ESP_ERR_INVALID_RESPONSE;
   }

   bool match = m5_stackflow_response_matches(&resp, request_id);
   bool ok = match && resp.error_code == 0;
   m5_stackflow_response_free(&resp);
   M5_UNLOCK();
   return ok ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

esp_err_t voice_m5_llm_infer(const char *prompt, char *output, size_t output_cap, uint32_t timeout_s) {
   if (prompt == NULL || prompt[0] == '\0' || output == NULL || output_cap == 0) {
      return ESP_ERR_INVALID_ARG;
   }
   output[0] = '\0';

   esp_err_t err = ensure_uart();
   if (err != ESP_OK) return err;
   M5_LOCK_OR_RETURN(timeout_s * 1000);

   int64_t deadline_us = esp_timer_get_time() + (int64_t)timeout_s * 1000 * 1000;

   err = do_llm_setup();
   if (err != ESP_OK) {
      M5_UNLOCK();
      return err;
   }
   if (esp_timer_get_time() >= deadline_us) {
      M5_UNLOCK();
      return ESP_ERR_TIMEOUT;
   }

   /* Wire shape from M5Module-LLM v1.7.0 (api_llm.cpp:inference): the
    * inference request's `data` is a stream-frame OBJECT
    *   { "delta": <prompt>, "index": 0, "finish": true }
    * NOT a bare string.  Sending a string returns error -25
    * "Stream data index error" silently — Phase 3.5 dig surfaced this. */
   char request_id[32];
   make_request_id(request_id, sizeof(request_id), "infer-");
   cJSON *data = cJSON_CreateObject();
   if (data == NULL) {
      M5_UNLOCK();
      return ESP_ERR_NO_MEM;
   }
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
   if (tx_len < 0) {
      M5_UNLOCK();
      return ESP_ERR_NO_MEM;
   }

   tab5_port_c_flush();
   if (tab5_port_c_send(tx, (size_t)tx_len) != tx_len) {
      M5_UNLOCK();
      return ESP_FAIL;
   }

   size_t used = 0;
   esp_err_t se = stream_collect(request_id, output, output_cap, deadline_us, &used);
   ESP_LOGI(TAG, "inference: %s, %u bytes written%s", esp_err_to_name(se), (unsigned)used,
            (used + 1 >= output_cap) ? " (truncated)" : "");
   M5_UNLOCK();
   return se;
}

void voice_m5_llm_release(void) {
   if (s_setup_work_id[0] == '\0') return;
   if (!tab5_port_c_uart_is_initialized()) {
      s_setup_work_id[0] = '\0';
      return;
   }
   if (tab5_port_c_lock(2000) != ESP_OK) {
      ESP_LOGW(TAG, "release: uart busy, skipping exit (work_id=%s left stale)", s_setup_work_id);
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
   tab5_port_c_unlock();
}

bool voice_m5_llm_is_ready(void) { return s_setup_work_id[0] != '\0'; }

esp_err_t voice_m5_llm_sys_reset(void) {
   esp_err_t err = ensure_uart();
   if (err != ESP_OK) return err;
   /* Wave 13 — soft restart of the StackFlow daemon on K144.  Verified
    * live against v1.3 (2026-05-01 ADB probe): the daemon emits one
    * synchronous ack with `{"data":"None", "error":{"code":0, "message":
    * "llm server restarting ..."}}` before going silent for ~4 s while
    * it reconnects MQTT internally.  Caller (voice_onboard layer) is
    * responsible for the post-reset wait + re-warmup re-trigger. */
   M5_LOCK_OR_RETURN(2000);

   char request_id[32];
   make_request_id(request_id, sizeof(request_id), "rst-");
   const m5_stackflow_request_t req = {
       .request_id = request_id,
       .work_id = "sys",
       .action = "reset",
   };

   char tx[256];
   int tx_len = m5_stackflow_build_request(&req, tx, sizeof(tx));
   if (tx_len < 0) {
      M5_UNLOCK();
      return ESP_ERR_NO_MEM;
   }

   /* Generous 1500 ms — daemon answers within a few hundred ms typical
    * but a loaded NPU on first sys.reset can be slower.  Fail fast
    * past that — caller will treat as ESP_ERR_TIMEOUT and proceed. */
   int frame_len = send_and_recv_one_frame(tx, tx_len, 1500);
   if (frame_len < 0) {
      M5_UNLOCK();
      ESP_LOGW(TAG, "sys.reset: no ack frame (timeout)");
      return ESP_ERR_TIMEOUT;
   }

   m5_stackflow_response_t resp = {0};
   esp_err_t pe = m5_stackflow_parse_response(s_rx_buf, (size_t)frame_len, &resp);
   if (pe != ESP_OK) {
      M5_UNLOCK();
      return ESP_ERR_INVALID_RESPONSE;
   }

   bool match = m5_stackflow_response_matches(&resp, request_id);
   bool ok = match && resp.error_code == 0;
   if (ok) {
      ESP_LOGI(TAG, "sys.reset acked: %s", resp.error_message ? resp.error_message : "(no msg)");
      /* All K144-side work_ids are invalidated by the daemon restart.
       * Clear our cache so the next infer call re-issues llm.setup. */
      s_setup_work_id[0] = '\0';
   } else {
      ESP_LOGW(TAG, "sys.reset returned error_code=%d msg=%s", resp.error_code,
               resp.error_message ? resp.error_message : "(none)");
   }
   m5_stackflow_response_free(&resp);
   M5_UNLOCK();
   return ok ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

esp_err_t voice_m5_llm_sys_hwinfo(voice_m5_hwinfo_t *out) {
   if (out == NULL) return ESP_ERR_INVALID_ARG;
   memset(out, 0, sizeof(*out));

   esp_err_t err = ensure_uart();
   if (err != ESP_OK) return err;
   M5_LOCK_OR_RETURN(2000);

   char request_id[32];
   make_request_id(request_id, sizeof(request_id), "hwi-");
   const m5_stackflow_request_t req = {
       .request_id = request_id,
       .work_id = "sys",
       .action = "hwinfo",
   };

   char tx[256];
   int tx_len = m5_stackflow_build_request(&req, tx, sizeof(tx));
   if (tx_len < 0) {
      M5_UNLOCK();
      return ESP_ERR_NO_MEM;
   }

   int frame_len = send_and_recv_one_frame(tx, tx_len, 1500);
   if (frame_len < 0) {
      M5_UNLOCK();
      return ESP_ERR_TIMEOUT;
   }

   m5_stackflow_response_t resp = {0};
   esp_err_t pe = m5_stackflow_parse_response(s_rx_buf, (size_t)frame_len, &resp);
   if (pe != ESP_OK) {
      M5_UNLOCK();
      return ESP_ERR_INVALID_RESPONSE;
   }

   esp_err_t result = ESP_ERR_INVALID_RESPONSE;
   if (m5_stackflow_response_matches(&resp, request_id) && resp.error_code == 0 && resp.data != NULL &&
       cJSON_IsObject(resp.data)) {
      cJSON *t = cJSON_GetObjectItemCaseSensitive(resp.data, "temperature");
      cJSON *cpu = cJSON_GetObjectItemCaseSensitive(resp.data, "cpu_loadavg");
      cJSON *mem = cJSON_GetObjectItemCaseSensitive(resp.data, "mem");
      if (cJSON_IsNumber(t)) out->temperature_milli_c = (int32_t)t->valuedouble;
      if (cJSON_IsNumber(cpu)) out->cpu_loadavg = (int32_t)cpu->valuedouble;
      if (cJSON_IsNumber(mem)) out->mem = (int32_t)mem->valuedouble;
      out->valid = 1;
      result = ESP_OK;
   }
   m5_stackflow_response_free(&resp);
   M5_UNLOCK();
   return result;
}

esp_err_t voice_m5_llm_sys_version(char *buf, size_t buf_cap) {
   if (buf == NULL || buf_cap == 0) return ESP_ERR_INVALID_ARG;
   buf[0] = '\0';

   esp_err_t err = ensure_uart();
   if (err != ESP_OK) return err;
   M5_LOCK_OR_RETURN(2000);

   char request_id[32];
   make_request_id(request_id, sizeof(request_id), "ver-");
   const m5_stackflow_request_t req = {
       .request_id = request_id,
       .work_id = "sys",
       .action = "version",
   };

   char tx[256];
   int tx_len = m5_stackflow_build_request(&req, tx, sizeof(tx));
   if (tx_len < 0) {
      M5_UNLOCK();
      return ESP_ERR_NO_MEM;
   }

   int frame_len = send_and_recv_one_frame(tx, tx_len, 1500);
   if (frame_len < 0) {
      M5_UNLOCK();
      return ESP_ERR_TIMEOUT;
   }

   m5_stackflow_response_t resp = {0};
   esp_err_t pe = m5_stackflow_parse_response(s_rx_buf, (size_t)frame_len, &resp);
   if (pe != ESP_OK) {
      M5_UNLOCK();
      return ESP_ERR_INVALID_RESPONSE;
   }

   esp_err_t result = ESP_ERR_INVALID_RESPONSE;
   if (m5_stackflow_response_matches(&resp, request_id) && resp.error_code == 0 && resp.data != NULL &&
       cJSON_IsString(resp.data)) {
      strncpy(buf, resp.data->valuestring, buf_cap - 1);
      buf[buf_cap - 1] = '\0';
      result = ESP_OK;
   }
   m5_stackflow_response_free(&resp);
   M5_UNLOCK();
   return result;
}

esp_err_t voice_m5_llm_sys_lsmode(voice_m5_modelist_t *out) {
   if (out == NULL) return ESP_ERR_INVALID_ARG;
   memset(out, 0, sizeof(*out));

   esp_err_t err = ensure_uart();
   if (err != ESP_OK) return err;
   M5_LOCK_OR_RETURN(3000);

   char request_id[32];
   make_request_id(request_id, sizeof(request_id), "lsm-");
   const m5_stackflow_request_t req = {
       .request_id = request_id,
       .work_id = "sys",
       .action = "lsmode",
   };

   char tx[256];
   int tx_len = m5_stackflow_build_request(&req, tx, sizeof(tx));
   if (tx_len < 0) {
      M5_UNLOCK();
      return ESP_ERR_NO_MEM;
   }

   /* sys.lsmode response is ~3 KB on K144 v1.3 (11 entries × ~250 B
    * each).  Daemon enumerates units from disk so 3 s allows the slow
    * path; warm path returns in ~50-100 ms. */
   int frame_len = send_and_recv_one_frame(tx, tx_len, 3000);
   if (frame_len < 0) {
      M5_UNLOCK();
      return ESP_ERR_TIMEOUT;
   }

   m5_stackflow_response_t resp = {0};
   esp_err_t pe = m5_stackflow_parse_response(s_rx_buf, (size_t)frame_len, &resp);
   if (pe != ESP_OK) {
      M5_UNLOCK();
      return ESP_ERR_INVALID_RESPONSE;
   }

   esp_err_t result = ESP_ERR_INVALID_RESPONSE;
   if (m5_stackflow_response_matches(&resp, request_id) && resp.error_code == 0 && resp.data != NULL &&
       cJSON_IsArray(resp.data)) {
      int count = cJSON_GetArraySize(resp.data);
      if (count > VOICE_M5_MODELLIST_MAX) count = VOICE_M5_MODELLIST_MAX;
      for (int i = 0; i < count; i++) {
         cJSON *entry = cJSON_GetArrayItem(resp.data, i);
         if (!cJSON_IsObject(entry)) continue;
         voice_m5_model_t *m = &out->models[out->n];
         cJSON *mode = cJSON_GetObjectItemCaseSensitive(entry, "mode");
         if (cJSON_IsString(mode)) {
            strncpy(m->mode, mode->valuestring, sizeof(m->mode) - 1);
         }
         cJSON *caps = cJSON_GetObjectItemCaseSensitive(entry, "capabilities");
         if (cJSON_IsArray(caps)) {
            cJSON *primary = cJSON_GetArrayItem(caps, 0);
            if (cJSON_IsString(primary)) {
               strncpy(m->primary_cap, primary->valuestring, sizeof(m->primary_cap) - 1);
            }
            cJSON *secondary = cJSON_GetArrayItem(caps, 1);
            if (cJSON_IsString(secondary)) {
               /* Only store as language if it's a recognised language
                * tag — otherwise leave empty.  K144 uses "English",
                * "Chinese" today; "chat" appears as secondary on the
                * LLM unit which is NOT a language. */
               const char *s = secondary->valuestring;
               if (strcmp(s, "English") == 0 || strcmp(s, "Chinese") == 0) {
                  strncpy(m->language, s, sizeof(m->language) - 1);
               }
            }
         }
         out->n++;
      }
      out->valid = true;
      result = ESP_OK;
   }
   m5_stackflow_response_free(&resp);
   M5_UNLOCK();
   return result;
}

/* ---------------------------------------------------------------------- */
/*  Phase 6a — adaptive baud negotiation                                  */
/* ---------------------------------------------------------------------- */

/* Helper: send sys.ping at current baud, return true on a clean ACK. */
static bool ping_ok(void) {
   char request_id[32];
   make_request_id(request_id, sizeof(request_id), "baudping-");
   const m5_stackflow_request_t req = {
       .request_id = request_id,
       .work_id = "sys",
       .action = "ping",
   };
   char tx[256];
   int tx_len = m5_stackflow_build_request(&req, tx, sizeof(tx));
   if (tx_len < 0) return false;
   int frame_len = send_and_recv_one_frame(tx, tx_len, M5_PING_TIMEOUT_MS);
   if (frame_len < 0) return false;
   m5_stackflow_response_t resp = {0};
   if (m5_stackflow_parse_response(s_rx_buf, (size_t)frame_len, &resp) != ESP_OK) {
      return false;
   }
   bool ok = m5_stackflow_response_matches(&resp, request_id) && resp.error_code == 0;
   m5_stackflow_response_free(&resp);
   return ok;
}

esp_err_t voice_m5_llm_set_baud(uint32_t new_baud) {
   esp_err_t uart_err = ensure_uart();
   if (uart_err != ESP_OK) return uart_err;

   const uint32_t old_baud = tab5_port_c_uart_get_baud();
   if (old_baud == new_baud) return ESP_OK;
   ESP_LOGI(TAG, "Negotiating UART baud %lu -> %lu", (unsigned long)old_baud, (unsigned long)new_baud);

   /* Hold the lock for the FULL negotiation including verify-loop — any
    * concurrent caller would clobber both ends mid-switch. */
   M5_LOCK_OR_RETURN(5000);

   /* Build sys.uartsetup request.  K144 expects:
    *   data = {baud, data_bits:8, stop_bits:1, parity:"n"} */
   cJSON *data = cJSON_CreateObject();
   if (data == NULL) {
      M5_UNLOCK();
      return ESP_ERR_NO_MEM;
   }
   cJSON_AddNumberToObject(data, "baud", (double)new_baud);
   cJSON_AddNumberToObject(data, "data_bits", 8);
   cJSON_AddNumberToObject(data, "stop_bits", 1);
   cJSON_AddStringToObject(data, "parity", "n");

   char request_id[32];
   make_request_id(request_id, sizeof(request_id), "uartsetup-");
   const m5_stackflow_request_t req = {
       .request_id = request_id,
       .work_id = "sys",
       .action = "uartsetup",
       .object = "sys.uartsetup",
       .data_json = data,
   };

   char tx[M5_TX_BUF_BYTES];
   int tx_len = m5_stackflow_build_request(&req, tx, sizeof(tx));
   cJSON_Delete(data);
   if (tx_len < 0) {
      ESP_LOGE(TAG, "build_request(uartsetup) failed");
      M5_UNLOCK();
      return ESP_ERR_NO_MEM;
   }

   /* Send at OLD baud, wait for ACK at OLD baud. */
   int frame_len = send_and_recv_one_frame(tx, tx_len, 2000);
   if (frame_len < 0) {
      ESP_LOGE(TAG, "uartsetup: no ACK at old baud %lu", (unsigned long)old_baud);
      M5_UNLOCK();
      return ESP_ERR_TIMEOUT;
   }

   m5_stackflow_response_t resp = {0};
   if (m5_stackflow_parse_response(s_rx_buf, (size_t)frame_len, &resp) != ESP_OK) {
      M5_UNLOCK();
      return ESP_ERR_INVALID_RESPONSE;
   }
   bool ack_ok = m5_stackflow_response_matches(&resp, request_id) && resp.error_code == 0;
   m5_stackflow_response_free(&resp);
   if (!ack_ok) {
      ESP_LOGE(TAG, "uartsetup: K144 NACK'd baud-switch request");
      M5_UNLOCK();
      return ESP_ERR_INVALID_RESPONSE;
   }

   /* K144 has accepted the new baud and is switching its UART NOW.
    * Give it a tiny window (StackFlow's serial_com module has TX-FIFO
    * drain + reconfig to do) before we follow. */
   vTaskDelay(pdMS_TO_TICKS(50));
   esp_err_t bset = tab5_port_c_uart_set_baud(new_baud);
   if (bset != ESP_OK) {
      /* We can't switch — try to revert K144 by sending another uartsetup
       * at the new baud (which we can't reach because we couldn't
       * switch).  Best we can do is bail; user power-cycle resets K144. */
      M5_UNLOCK();
      return bset;
   }

   /* Verify with up to 5 pings at the new baud.  K144's UART reconfig +
    * StackFlow's serial_com module restart can take >100 ms.  Each ping
    * has its own 500 ms timeout via M5_PING_TIMEOUT_MS; total budget
    * here is ~2.5 s for the new baud to come up. */
   vTaskDelay(pdMS_TO_TICKS(200));
   for (int i = 0; i < 5; i++) {
      if (ping_ok()) {
         ESP_LOGI(TAG, "Baud switch OK — Tab5 + K144 now at %lu (verify took %d tries)", (unsigned long)new_baud,
                  i + 1);
         M5_UNLOCK();
         return ESP_OK;
      }
      vTaskDelay(pdMS_TO_TICKS(200));
   }

   ESP_LOGW(TAG, "Post-switch ping failed at %lu — K144 may be stranded; trying revert", (unsigned long)new_baud);
   /* Tab5 reverts.  If K144 also auto-reverted on its own ack-timeout
    * watchdog, the next ping at old_baud succeeds.  If K144 stayed at
    * new_baud (observed in Phase 6a bench), neither baud will ping —
    * we surface that loudly so the bench harness flags it. */
   tab5_port_c_uart_set_baud(old_baud);
   vTaskDelay(pdMS_TO_TICKS(200));
   for (int i = 0; i < 3; i++) {
      if (ping_ok()) {
         ESP_LOGI(TAG, "Reverted to %lu — comms restored", (unsigned long)old_baud);
         M5_UNLOCK();
         return ESP_ERR_INVALID_RESPONSE;
      }
      vTaskDelay(pdMS_TO_TICKS(200));
   }
   ESP_LOGE(TAG,
            "K144 unreachable at both %lu and %lu — likely stranded at new baud; "
            "use voice_m5_llm_recover_baud to retry, or power-cycle the K144",
            (unsigned long)new_baud, (unsigned long)old_baud);
   M5_UNLOCK();
   return ESP_ERR_INVALID_RESPONSE;
}

uint32_t voice_m5_llm_get_baud(void) { return tab5_port_c_uart_get_baud(); }

/* ---------------------------------------------------------------------- */
/*  Phase 6c — TTS                                                        */
/* ---------------------------------------------------------------------- */

#define M5_TTS_MODEL "single_speaker_english_fast"

/* TTS setup: lazy, reuses work_id across calls (like LLM setup). */
static esp_err_t do_tts_setup(void) {
   if (s_tts_work_id[0] != '\0') return ESP_OK;

   cJSON *data = cJSON_CreateObject();
   if (data == NULL) return ESP_ERR_NO_MEM;
   cJSON_AddStringToObject(data, "model", M5_TTS_MODEL);
   /* tts.base64.wav → K144 returns headerless 16 kHz mono int16 PCM
    * base64-encoded in `data` (despite the .wav suffix — confirmed via
    * Phase 6c TCP spike). */
   cJSON_AddStringToObject(data, "response_format", "tts.base64.wav");
   cJSON *input_arr = cJSON_CreateArray();
   cJSON_AddItemToArray(input_arr, cJSON_CreateString("tts.utf-8"));
   cJSON_AddItemToObject(data, "input", input_arr);
   cJSON_AddBoolToObject(data, "enoutput", true);
   cJSON_AddBoolToObject(data, "enkws", false);

   char request_id[32];
   make_request_id(request_id, sizeof(request_id), "tts-setup-");
   const m5_stackflow_request_t req = {
       .request_id = request_id,
       .work_id = "tts",
       .action = "setup",
       .object = "tts.setup",
       .data_json = data,
   };

   char tx[M5_TX_BUF_BYTES];
   int tx_len = m5_stackflow_build_request(&req, tx, sizeof(tx));
   cJSON_Delete(data);
   if (tx_len < 0) return ESP_ERR_NO_MEM;

   int frame_len = send_and_recv_one_frame(tx, tx_len, M5_SETUP_TIMEOUT_MS);
   if (frame_len < 0) return ESP_ERR_TIMEOUT;

   m5_stackflow_response_t resp = {0};
   if (m5_stackflow_parse_response(s_rx_buf, (size_t)frame_len, &resp) != ESP_OK) {
      return ESP_ERR_INVALID_RESPONSE;
   }
   if (!m5_stackflow_response_matches(&resp, request_id) || resp.error_code != 0 || resp.work_id == NULL) {
      ESP_LOGE(TAG, "tts.setup ack mismatch or err=%d msg='%s'", resp.error_code,
               resp.error_message ? resp.error_message : "");
      m5_stackflow_response_free(&resp);
      return ESP_ERR_INVALID_RESPONSE;
   }
   strncpy(s_tts_work_id, resp.work_id, sizeof(s_tts_work_id) - 1);
   s_tts_work_id[sizeof(s_tts_work_id) - 1] = '\0';
   ESP_LOGI(TAG, "tts.setup ok — work_id=%s model=%s", s_tts_work_id, M5_TTS_MODEL);
   m5_stackflow_response_free(&resp);
   return ESP_OK;
}

esp_err_t voice_m5_llm_tts(const char *text, int16_t *pcm_out, size_t max_samples, size_t *out_samples,
                           uint32_t timeout_s) {
   if (text == NULL || text[0] == '\0' || pcm_out == NULL || out_samples == NULL || max_samples == 0) {
      return ESP_ERR_INVALID_ARG;
   }
   *out_samples = 0;

   esp_err_t err = ensure_uart();
   if (err != ESP_OK) return err;
   M5_LOCK_OR_RETURN(timeout_s * 1000);

   int64_t deadline_us = esp_timer_get_time() + (int64_t)timeout_s * 1000 * 1000;
   err = do_tts_setup();
   if (err != ESP_OK) {
      M5_UNLOCK();
      return err;
   }
   if (esp_timer_get_time() >= deadline_us) {
      M5_UNLOCK();
      return ESP_ERR_TIMEOUT;
   }

   /* Send inference: tts.utf-8 with text as bare string (per K144 docs). */
   char request_id[32];
   make_request_id(request_id, sizeof(request_id), "tts-infer-");
   const m5_stackflow_request_t req = {
       .request_id = request_id,
       .work_id = s_tts_work_id,
       .action = "inference",
       .object = "tts.utf-8",
       .data_string = text,
   };
   char tx[M5_TX_BUF_BYTES];
   int tx_len = m5_stackflow_build_request(&req, tx, sizeof(tx));
   if (tx_len < 0) {
      M5_UNLOCK();
      return ESP_ERR_NO_MEM;
   }
   tab5_port_c_flush();
   if (tab5_port_c_send(tx, (size_t)tx_len) != tx_len) {
      M5_UNLOCK();
      return ESP_FAIL;
   }

   /* Drain frames; concatenate base64 strings until parse-success-but-no-more
    * arrives.  K144's `tts.base64.wav` (non-stream) emits a single
    * response per inference with the entire base64 in `data` as a string.
    * Future enhancement: tts.base64.wav.stream for token-by-token. */
   if (ensure_rx_buf() != ESP_OK) {
      M5_UNLOCK();
      return ESP_ERR_NO_MEM;
   }
   s_rx_len = 0;

   /* Allocate a PSRAM scratch for accumulated base64 (~256 KB max).  The
    * K144's TTS for short utterances stays under 50 KB base64; alloc a
    * generous cap to handle longer prompts. */
   const size_t b64_cap = 256 * 1024;
   char *b64 = heap_caps_malloc(b64_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (b64 == NULL) {
      M5_UNLOCK();
      return ESP_ERR_NO_MEM;
   }
   size_t b64_len = 0;

   while (esp_timer_get_time() < deadline_us) {
      char *nl = memchr(s_rx_buf, '\n', s_rx_len);
      while (nl == NULL && s_rx_len < M5_RX_BUF_BYTES - 1 && esp_timer_get_time() < deadline_us) {
         uint32_t budget_ms = (uint32_t)((deadline_us - esp_timer_get_time()) / 1000);
         if (budget_ms > 200) budget_ms = 200;
         int n = tab5_port_c_recv(s_rx_buf + s_rx_len, M5_RX_BUF_BYTES - 1 - s_rx_len, budget_ms);
         if (n > 0) {
            s_rx_len += (size_t)n;
            nl = memchr(s_rx_buf, '\n', s_rx_len);
         }
      }
      if (nl == NULL) break;

      size_t frame_len = (size_t)(nl - s_rx_buf);
      m5_stackflow_response_t resp = {0};
      esp_err_t pe = m5_stackflow_parse_response(s_rx_buf, frame_len, &resp);
      size_t consumed = frame_len + 1;
      if (consumed < s_rx_len) {
         memmove(s_rx_buf, s_rx_buf + consumed, s_rx_len - consumed);
      }
      s_rx_len -= consumed;

      if (pe != ESP_OK) {
         m5_stackflow_response_free(&resp);
         continue;
      }
      if (!m5_stackflow_response_matches(&resp, request_id)) {
         m5_stackflow_response_free(&resp);
         continue;
      }
      if (resp.error_code != 0) {
         ESP_LOGE(TAG, "K144 tts error %d: %s", resp.error_code, resp.error_message ? resp.error_message : "");
         if (resp.error_code == -4 || resp.error_code == -25) {
            s_tts_work_id[0] = '\0';
         }
         m5_stackflow_response_free(&resp);
         heap_caps_free(b64);
         M5_UNLOCK();
         return ESP_ERR_INVALID_RESPONSE;
      }
      /* Audio comes in `data` — could be a string (whole base64) or a
       * stream-frame {delta, index, finish}.  Handle both. */
      if (resp.data != NULL) {
         const char *delta_str = NULL;
         bool finished = false;
         if (cJSON_IsString(resp.data)) {
            delta_str = resp.data->valuestring;
            finished = true; /* non-stream → single shot */
         } else if (cJSON_IsObject(resp.data)) {
            const cJSON *d = cJSON_GetObjectItemCaseSensitive(resp.data, "delta");
            if (cJSON_IsString(d)) delta_str = d->valuestring;
            const cJSON *fin = cJSON_GetObjectItemCaseSensitive(resp.data, "finish");
            finished = cJSON_IsTrue(fin);
         }
         if (delta_str != NULL) {
            size_t add = strlen(delta_str);
            if (b64_len + add < b64_cap) {
               memcpy(b64 + b64_len, delta_str, add);
               b64_len += add;
            }
         }
         m5_stackflow_response_free(&resp);
         if (finished) break;
      } else {
         m5_stackflow_response_free(&resp);
      }
   }

   if (b64_len == 0) {
      heap_caps_free(b64);
      ESP_LOGW(TAG, "tts: no audio data received");
      M5_UNLOCK();
      return ESP_ERR_TIMEOUT;
   }

   /* Decode base64 → raw PCM (bytes).  pcm_out capacity in samples,
    * 2 bytes per sample. */
   size_t pcm_bytes = 0;
   const size_t pcm_cap_bytes = max_samples * sizeof(int16_t);
   int dec =
       mbedtls_base64_decode((unsigned char *)pcm_out, pcm_cap_bytes, &pcm_bytes, (const unsigned char *)b64, b64_len);
   heap_caps_free(b64);
   if (dec != 0) {
      ESP_LOGE(TAG, "base64 decode failed: -0x%04x (b64_len=%u)", -dec, (unsigned)b64_len);
      M5_UNLOCK();
      return ESP_ERR_INVALID_RESPONSE;
   }
   *out_samples = pcm_bytes / sizeof(int16_t);
   ESP_LOGI(TAG, "tts: %u samples @ 16 kHz mono (%u ms speech)", (unsigned)*out_samples,
            (unsigned)((*out_samples * 1000) / 16000));
   M5_UNLOCK();
   return ESP_OK;
}

/* ---------------------------------------------------------------------- */
/*  Phase 6b — Autonomous K144 voice-assistant chain                      */
/* ---------------------------------------------------------------------- */

#define M5_CHAIN_ASR_MODEL "sherpa-ncnn-streaming-zipformer-20M-2023-02-17"

struct voice_m5_chain_handle {
   char audio_id[32]; /* "audio" or "audio.NNNN" depending on K144 ver */
   char asr_id[32];   /* "asr.NNNN" */
   char llm_id[32];   /* "llm.NNNN" */
   char tts_id[32];   /* "tts.NNNN" */
};

/* Generic setup helper — sends one `setup` and returns the assigned work_id
 * via @p out_work_id (32-byte buffer).  data_obj is consumed (deleted).
 *
 * NB: once a unit upstream is set up (e.g. ASR after audio.setup), the K144
 * may already be emitting stream frames on the UART carrying that unit's
 * work_id while we're still waiting for the NEXT setup's ACK.  We therefore
 * skip non-matching frames within the timeout instead of bailing on the
 * first one. */
/* Audit #13: stop_flag short-circuits the inner refill loop so a user
 * tap-to-stop during a 15-sec NPU cold-start bails fast (within ~100 ms)
 * instead of blocking on the deadline.  NULL stop_flag is allowed for
 * callers that don't have one. */
static esp_err_t chain_setup_unit(const char *work_id_seed, const char *object, cJSON *data_obj, char *out_work_id,
                                  size_t out_cap, uint32_t timeout_ms, volatile bool *stop_flag) {
   char request_id[32];
   make_request_id(request_id, sizeof(request_id), "chain-setup-");
   const m5_stackflow_request_t req = {
       .request_id = request_id,
       .work_id = work_id_seed,
       .action = "setup",
       .object = object,
       .data_json = data_obj,
   };
   char tx[M5_TX_BUF_BYTES];
   int tx_len = m5_stackflow_build_request(&req, tx, sizeof(tx));
   cJSON_Delete(data_obj);
   if (tx_len < 0) return ESP_ERR_NO_MEM;

   tab5_port_c_flush();
   if (tab5_port_c_send(tx, (size_t)tx_len) != tx_len) {
      ESP_LOGE(TAG, "chain setup(%s): port_c send truncated", object);
      return ESP_FAIL;
   }
   if (ensure_rx_buf() != ESP_OK) return ESP_ERR_NO_MEM;
   s_rx_len = 0;

   const int64_t deadline_us = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
   while (esp_timer_get_time() < deadline_us && !(stop_flag != NULL && *stop_flag)) {
      char *nl = memchr(s_rx_buf, '\n', s_rx_len);
      while (nl == NULL && s_rx_len < M5_RX_BUF_BYTES - 1 && esp_timer_get_time() < deadline_us &&
             !(stop_flag != NULL && *stop_flag)) {
         uint32_t budget_ms = (uint32_t)((deadline_us - esp_timer_get_time()) / 1000);
         if (budget_ms > 100) budget_ms = 100;
         int n = tab5_port_c_recv(s_rx_buf + s_rx_len, M5_RX_BUF_BYTES - 1 - s_rx_len, budget_ms);
         if (n > 0) {
            s_rx_len += (size_t)n;
            nl = memchr(s_rx_buf, '\n', s_rx_len);
         }
      }
      if (nl == NULL) break;

      size_t frame_len = (size_t)(nl - s_rx_buf);
      m5_stackflow_response_t resp = {0};
      esp_err_t pe = m5_stackflow_parse_response(s_rx_buf, frame_len, &resp);
      size_t consumed = frame_len + 1;
      if (consumed < s_rx_len) {
         memmove(s_rx_buf, s_rx_buf + consumed, s_rx_len - consumed);
      }
      s_rx_len -= consumed;

      if (pe != ESP_OK) {
         m5_stackflow_response_free(&resp);
         continue;
      }
      if (!m5_stackflow_response_matches(&resp, request_id)) {
         /* Stream frame from a previously-set-up unit — ignore. */
         m5_stackflow_response_free(&resp);
         continue;
      }
      if (resp.error_code != 0 || resp.work_id == NULL) {
         ESP_LOGE(TAG, "chain setup(%s) err=%d msg='%s' work_id=%s", object, resp.error_code,
                  resp.error_message ? resp.error_message : "", resp.work_id ? resp.work_id : "(null)");
         m5_stackflow_response_free(&resp);
         return ESP_ERR_INVALID_RESPONSE;
      }
      strncpy(out_work_id, resp.work_id, out_cap - 1);
      out_work_id[out_cap - 1] = '\0';
      ESP_LOGI(TAG, "chain setup(%s) -> %s", object, out_work_id);
      m5_stackflow_response_free(&resp);
      return ESP_OK;
   }

   if (stop_flag != NULL && *stop_flag) {
      ESP_LOGI(TAG, "chain setup(%s): user stop during ACK wait", object);
      return ESP_ERR_INVALID_STATE;
   }
   ESP_LOGE(TAG, "chain setup(%s): timeout waiting for ACK", object);
   return ESP_ERR_TIMEOUT;
}

esp_err_t voice_m5_llm_chain_setup(voice_m5_chain_handle_t **out_handle, volatile bool *stop_flag) {
   if (out_handle == NULL) return ESP_ERR_INVALID_ARG;
   *out_handle = NULL;

   esp_err_t err = ensure_uart();
   if (err != ESP_OK) return err;

   /* Hold the lock for all 4 sequential setups — ~5-15 sec on cold NPU
    * load.  Concurrent text infer or probe would corrupt the rx buffer. */
   M5_LOCK_OR_RETURN(60000);

   voice_m5_chain_handle_t *h = heap_caps_calloc(1, sizeof(*h), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (h == NULL) {
      M5_UNLOCK();
      return ESP_ERR_NO_MEM;
   }

   /* 1) audio.setup — onboard mic + speaker, capcard=0/capdev=0,
    *    playcard=0/playdev=1.  Verified via TCP spike. */
   {
      cJSON *d = cJSON_CreateObject();
      cJSON_AddNumberToObject(d, "capcard", 0);
      cJSON_AddNumberToObject(d, "capdevice", 0);
      cJSON_AddNumberToObject(d, "capVolume", 0.5);
      cJSON_AddNumberToObject(d, "playcard", 0);
      cJSON_AddNumberToObject(d, "playdevice", 1);
      cJSON_AddNumberToObject(d, "playVolume", 0.15);
      err =
          chain_setup_unit("audio", "audio.setup", d, h->audio_id, sizeof(h->audio_id), M5_SETUP_TIMEOUT_MS, stop_flag);
      if (err != ESP_OK) goto fail;
   }

   /* 2) asr.setup — subscribe to sys.pcm, stream UTF-8 out. */
   {
      cJSON *d = cJSON_CreateObject();
      cJSON_AddStringToObject(d, "model", M5_CHAIN_ASR_MODEL);
      cJSON_AddStringToObject(d, "response_format", "asr.utf-8.stream");
      cJSON *inp = cJSON_CreateArray();
      cJSON_AddItemToArray(inp, cJSON_CreateString("sys.pcm"));
      cJSON_AddItemToObject(d, "input", inp);
      cJSON_AddBoolToObject(d, "enoutput", true);
      err = chain_setup_unit("asr", "asr.setup", d, h->asr_id, sizeof(h->asr_id), M5_SETUP_TIMEOUT_MS, stop_flag);
      if (err != ESP_OK) goto fail;
   }

   /* 3) llm.setup — subscribe to ASR text, stream UTF-8 out. */
   {
      cJSON *d = cJSON_CreateObject();
      cJSON_AddStringToObject(d, "model", M5_LLM_MODEL);
      cJSON_AddStringToObject(d, "response_format", M5_LLM_RESPONSE_FORMAT);
      cJSON *inp = cJSON_CreateArray();
      cJSON_AddItemToArray(inp, cJSON_CreateString(h->asr_id));
      cJSON_AddItemToObject(d, "input", inp);
      cJSON_AddBoolToObject(d, "enoutput", true);
      cJSON_AddBoolToObject(d, "enkws", false);
      cJSON_AddNumberToObject(d, "max_token_len", 127);
      cJSON_AddStringToObject(d, "prompt", "You are a helpful, concise assistant. Reply in one sentence.");
      /* LLM cold-start NPU model load can take 5-10s — give it more headroom. */
      err = chain_setup_unit("llm", "llm.setup", d, h->llm_id, sizeof(h->llm_id), 15000, stop_flag);
      if (err != ESP_OK) goto fail;
   }

   /* 4) tts.setup — INTENTIONALLY OMITTED.  K144's
    * `single_speaker_english_fast` engine crashes mid-utterance with an
    * Eigen reshape assertion when consuming token-by-token from the LLM
    * unit (LEARNINGS: "K144 single_speaker_english_fast TTS in stream-
    * from-LLM mode crashes").  systemd auto-restarts the service but
    * the chain's tts subscription gets orphaned.
    *
    * Workaround: tts_id stays empty.  voice.c chain_text_callback now
    * calls voice_m5_llm_tts (the one-shot synth path) on each LLM
    * `finish=true` instead.  The chain's recv loop's `is_tts` strcmp
    * never matches an empty work_id (early-skip in the dispatch). */

   ESP_LOGI(TAG, "chain ready: audio=%s asr=%s llm=%s (TTS via per-utterance synth) — speak at the K144", h->audio_id,
            h->asr_id, h->llm_id);
   *out_handle = h;
   M5_UNLOCK();
   return ESP_OK;

fail:
   /* Best-effort tear-down of whatever DID come up.  Recursive mutex
    * lets teardown re-take the lock cleanly. */
   M5_UNLOCK();
   voice_m5_llm_chain_teardown(h);
   return err;
}

/* Helper: one-shot exit by work_id (no response wait — fire-and-forget). */
static void chain_exit_unit(const char *work_id) {
   if (work_id == NULL || work_id[0] == '\0') return;
   char request_id[32];
   make_request_id(request_id, sizeof(request_id), "chain-exit-");
   const m5_stackflow_request_t req = {
       .request_id = request_id,
       .work_id = work_id,
       .action = "exit",
   };
   char tx[M5_TX_BUF_BYTES];
   int tx_len = m5_stackflow_build_request(&req, tx, sizeof(tx));
   if (tx_len > 0) {
      (void)send_and_recv_one_frame(tx, tx_len, 500);
   }
}

void voice_m5_llm_chain_teardown(voice_m5_chain_handle_t *handle) {
   if (handle == NULL) return;
   /* Hold the lock for the four sequential exits.  If a concurrent caller
    * holds the lock we wait — teardown is best-effort, so a skipped exit
    * leaves K144-side state stale (better than corrupting the live
    * caller's transaction). */
   if (tab5_port_c_lock(5000) != ESP_OK) {
      ESP_LOGW(TAG, "teardown: uart busy, skipping unit exits (work_ids will leak on K144 until reset)");
      heap_caps_free(handle);
      return;
   }
   /* Reverse order of bring-up so each unit's downstream subscriber is
    * killed before the publisher disappears. */
   chain_exit_unit(handle->tts_id);
   chain_exit_unit(handle->llm_id);
   chain_exit_unit(handle->asr_id);
   chain_exit_unit(handle->audio_id);
   /* Drain any in-flight publisher frames so the next chain run starts
    * clean.  Audit #11. */
   tab5_port_c_flush();
   s_rx_len = 0;
   ESP_LOGI(TAG, "chain torn down");
   tab5_port_c_unlock();
   heap_caps_free(handle);
}

esp_err_t voice_m5_llm_chain_run(voice_m5_chain_handle_t *handle, voice_m5_chain_text_cb text_cb,
                                 voice_m5_chain_audio_cb audio_cb, void *user, volatile bool *stop_flag,
                                 uint32_t timeout_s) {
   if (handle == NULL) return ESP_ERR_INVALID_ARG;
   /* tts_id is allowed to be empty (Wave 3a Eigen workaround — chain
    * handles ASR + LLM only; per-utterance TTS happens in the caller's
    * text_cb on LLM finish=true). */
   if (handle->asr_id[0] == '\0' || handle->llm_id[0] == '\0') {
      return ESP_ERR_INVALID_STATE;
   }
   esp_err_t err = ensure_uart();
   if (err != ESP_OK) return err;
   if (ensure_rx_buf() != ESP_OK) return ESP_ERR_NO_MEM;

   /* Discard any stale bytes left over from a prior session — without
    * this, the first iteration can parse garbage as a frame.  Audit #11. */
   if (tab5_port_c_lock(2000) == ESP_OK) {
      tab5_port_c_flush();
      s_rx_len = 0;
      tab5_port_c_unlock();
   }

   const int64_t deadline_us = (timeout_s == 0) ? INT64_MAX : esp_timer_get_time() + (int64_t)timeout_s * 1000 * 1000;

   /* PCM scratch — TTS chunks decode to ~16 KB each (1 sec of 16 kHz mono).
    * Allocate generous PSRAM headroom for longer chunks. */
   const size_t pcm_cap_samples = 128 * 1024; /* 8 sec @ 16 kHz */
   int16_t *pcm = heap_caps_malloc(pcm_cap_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (pcm == NULL) return ESP_ERR_NO_MEM;

   /* TTS base64 accumulator — K144's `tts.base64.wav` chunked stream emits
    * partial b64 deltas (each is a sub-string of ONE big base64 payload),
    * not standalone-decodable chunks.  We accumulate across frames keyed
    * on the LLM utterance, and decode + play the whole payload at the
    * `finish=true` frame.  256 KB cap covers ~10 sec of speech.  Same
    * pattern as the existing voice_m5_llm_tts() implementation. */
   const size_t b64_cap = 256 * 1024;
   char *b64_acc = heap_caps_malloc(b64_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (b64_acc == NULL) {
      heap_caps_free(pcm);
      return ESP_ERR_NO_MEM;
   }
   size_t b64_len = 0;

   while (!(stop_flag != NULL && *stop_flag) && esp_timer_get_time() < deadline_us) {
      /* Take/release per iteration — concurrent text turn or probe lands
       * between frames.  Holding across the callbacks is intentional for
       * Wave 1 (caller blocks at most one iteration's work; audio_cb
       * blocking is a separate audit item to address later). */
      if (tab5_port_c_lock(2000) != ESP_OK) {
         vTaskDelay(pdMS_TO_TICKS(10));
         continue;
      }
      /* Refill rx_buf until we have at least one \n. */
      char *nl = memchr(s_rx_buf, '\n', s_rx_len);
      while (nl == NULL && s_rx_len < M5_RX_BUF_BYTES - 1 && !(stop_flag != NULL && *stop_flag) &&
             esp_timer_get_time() < deadline_us) {
         /* Short slice so we can re-check stop_flag and the deadline often. */
         int n = tab5_port_c_recv(s_rx_buf + s_rx_len, M5_RX_BUF_BYTES - 1 - s_rx_len, 100);
         if (n > 0) {
            s_rx_len += (size_t)n;
            nl = memchr(s_rx_buf, '\n', s_rx_len);
         }
      }
      if (nl == NULL) {
         tab5_port_c_unlock();
         continue;
      }

      size_t frame_len = (size_t)(nl - s_rx_buf);
      m5_stackflow_response_t resp = {0};
      esp_err_t pe = m5_stackflow_parse_response(s_rx_buf, frame_len, &resp);
      size_t consumed = frame_len + 1;
      if (consumed < s_rx_len) {
         memmove(s_rx_buf, s_rx_buf + consumed, s_rx_len - consumed);
      }
      s_rx_len -= consumed;
      if (pe != ESP_OK) {
         m5_stackflow_response_free(&resp);
         tab5_port_c_unlock();
         continue;
      }
      if (resp.work_id == NULL || resp.data == NULL) {
         m5_stackflow_response_free(&resp);
         tab5_port_c_unlock();
         continue;
      }

      const bool is_asr = (strcmp(resp.work_id, handle->asr_id) == 0);
      const bool is_llm = (strcmp(resp.work_id, handle->llm_id) == 0);
      const bool is_tts = (strcmp(resp.work_id, handle->tts_id) == 0);
      if (!is_asr && !is_llm && !is_tts) {
         m5_stackflow_response_free(&resp);
         tab5_port_c_unlock();
         continue;
      }

      if (is_tts) {
         /* TTS: data is either {delta, index, finish} (chunked stream)
          * or a bare base64 string (single-shot).  Accumulate b64
          * across delta frames; decode + play on `finish=true` (or on
          * a single bare-string frame which is implicitly final). */
         const char *delta = NULL;
         bool finish = false;
         if (cJSON_IsString(resp.data)) {
            delta = resp.data->valuestring;
            finish = true;
         } else if (cJSON_IsObject(resp.data)) {
            const cJSON *d = cJSON_GetObjectItemCaseSensitive(resp.data, "delta");
            if (cJSON_IsString(d)) delta = d->valuestring;
            const cJSON *fin = cJSON_GetObjectItemCaseSensitive(resp.data, "finish");
            finish = cJSON_IsTrue(fin);
         }
         if (delta != NULL) {
            const size_t add = strlen(delta);
            if (b64_len + add < b64_cap) {
               memcpy(b64_acc + b64_len, delta, add);
               b64_len += add;
            } else {
               ESP_LOGW(TAG, "chain: TTS b64 acc overflow (have=%u add=%u cap=%u) — dropping frame", (unsigned)b64_len,
                        (unsigned)add, (unsigned)b64_cap);
            }
         }
         if (finish && b64_len > 0 && audio_cb != NULL) {
            size_t pcm_bytes = 0;
            int dec = mbedtls_base64_decode((unsigned char *)pcm, pcm_cap_samples * sizeof(int16_t), &pcm_bytes,
                                            (const unsigned char *)b64_acc, b64_len);
            if (dec == 0 && pcm_bytes > 0) {
               audio_cb(pcm, pcm_bytes / sizeof(int16_t), user);
            } else if (dec != 0) {
               ESP_LOGW(TAG, "chain: tts base64 decode failed -0x%04x (b64_len=%u)", -dec, (unsigned)b64_len);
            }
            b64_len = 0; /* ready for next utterance */
         }
      } else {
         /* ASR or LLM: data is {delta,...,finish} or bare string. */
         const char *delta = NULL;
         bool finish = false;
         if (cJSON_IsString(resp.data)) {
            delta = resp.data->valuestring;
            finish = true;
         } else if (cJSON_IsObject(resp.data)) {
            const cJSON *d = cJSON_GetObjectItemCaseSensitive(resp.data, "delta");
            if (cJSON_IsString(d)) delta = d->valuestring;
            const cJSON *fin = cJSON_GetObjectItemCaseSensitive(resp.data, "finish");
            finish = cJSON_IsTrue(fin);
         }
         if (delta != NULL && delta[0] != '\0' && text_cb != NULL) {
            text_cb(delta, is_llm, finish, user);
         }
      }
      m5_stackflow_response_free(&resp);
      tab5_port_c_unlock();
   }

   heap_caps_free(pcm);
   heap_caps_free(b64_acc);
   if (stop_flag != NULL && *stop_flag) return ESP_OK;
   return ESP_ERR_TIMEOUT;
}

esp_err_t voice_m5_llm_recover_baud(uint32_t candidate_baud) {
   esp_err_t uart_err = ensure_uart();
   if (uart_err != ESP_OK) return uart_err;
   M5_LOCK_OR_RETURN(5000);

   const uint32_t before = tab5_port_c_uart_get_baud();
   ESP_LOGI(TAG, "Recovery: probing K144 at %lu (Tab5 was at %lu)", (unsigned long)candidate_baud,
            (unsigned long)before);
   tab5_port_c_uart_set_baud(candidate_baud);
   vTaskDelay(pdMS_TO_TICKS(100));
   bool found = false;
   for (int i = 0; i < 3; i++) {
      if (ping_ok()) {
         found = true;
         break;
      }
      vTaskDelay(pdMS_TO_TICKS(150));
   }
   if (!found) {
      ESP_LOGW(TAG, "Recovery: K144 not at %lu either", (unsigned long)candidate_baud);
      tab5_port_c_uart_set_baud(before);
      M5_UNLOCK();
      return ESP_ERR_INVALID_RESPONSE;
   }
   ESP_LOGI(TAG, "Recovery: K144 found at %lu — reuniting both ends at 115200", (unsigned long)candidate_baud);
   M5_UNLOCK();
   /* Now do a clean uartsetup back to 115200.  voice_m5_llm_set_baud
    * takes the lock itself; recursive mutex makes this safe even if we
    * re-enter from already-holding state, but we released above for
    * clarity. */
   esp_err_t ret = voice_m5_llm_set_baud(115200);
   if (ret != ESP_OK) {
      ESP_LOGW(TAG, "Recovery: graceful switch back to 115200 failed (%s)", esp_err_to_name(ret));
   }
   return ret;
}
