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

   /* Build sys.uartsetup request.  K144 expects:
    *   data = {baud, data_bits:8, stop_bits:1, parity:"n"} */
   cJSON *data = cJSON_CreateObject();
   if (data == NULL) return ESP_ERR_NO_MEM;
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
      return ESP_ERR_NO_MEM;
   }

   /* Send at OLD baud, wait for ACK at OLD baud. */
   int frame_len = send_and_recv_one_frame(tx, tx_len, 2000);
   if (frame_len < 0) {
      ESP_LOGE(TAG, "uartsetup: no ACK at old baud %lu", (unsigned long)old_baud);
      return ESP_ERR_TIMEOUT;
   }

   m5_stackflow_response_t resp = {0};
   if (m5_stackflow_parse_response(s_rx_buf, (size_t)frame_len, &resp) != ESP_OK) {
      return ESP_ERR_INVALID_RESPONSE;
   }
   bool ack_ok = m5_stackflow_response_matches(&resp, request_id) && resp.error_code == 0;
   m5_stackflow_response_free(&resp);
   if (!ack_ok) {
      ESP_LOGE(TAG, "uartsetup: K144 NACK'd baud-switch request");
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
         return ESP_ERR_INVALID_RESPONSE;
      }
      vTaskDelay(pdMS_TO_TICKS(200));
   }
   ESP_LOGE(TAG,
            "K144 unreachable at both %lu and %lu — likely stranded at new baud; "
            "use voice_m5_llm_recover_baud to retry, or power-cycle the K144",
            (unsigned long)new_baud, (unsigned long)old_baud);
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

   int64_t deadline_us = esp_timer_get_time() + (int64_t)timeout_s * 1000 * 1000;
   err = do_tts_setup();
   if (err != ESP_OK) return err;
   if (esp_timer_get_time() >= deadline_us) return ESP_ERR_TIMEOUT;

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
   if (tx_len < 0) return ESP_ERR_NO_MEM;
   tab5_port_c_flush();
   if (tab5_port_c_send(tx, (size_t)tx_len) != tx_len) return ESP_FAIL;

   /* Drain frames; concatenate base64 strings until parse-success-but-no-more
    * arrives.  K144's `tts.base64.wav` (non-stream) emits a single
    * response per inference with the entire base64 in `data` as a string.
    * Future enhancement: tts.base64.wav.stream for token-by-token. */
   if (ensure_rx_buf() != ESP_OK) return ESP_ERR_NO_MEM;
   s_rx_len = 0;

   /* Allocate a PSRAM scratch for accumulated base64 (~256 KB max).  The
    * K144's TTS for short utterances stays under 50 KB base64; alloc a
    * generous cap to handle longer prompts. */
   const size_t b64_cap = 256 * 1024;
   char *b64 = heap_caps_malloc(b64_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (b64 == NULL) return ESP_ERR_NO_MEM;
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
      return ESP_ERR_INVALID_RESPONSE;
   }
   *out_samples = pcm_bytes / sizeof(int16_t);
   ESP_LOGI(TAG, "tts: %u samples @ 16 kHz mono (%u ms speech)", (unsigned)*out_samples,
            (unsigned)((*out_samples * 1000) / 16000));
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
static esp_err_t chain_setup_unit(const char *work_id_seed, const char *object, cJSON *data_obj, char *out_work_id,
                                  size_t out_cap, uint32_t timeout_ms) {
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
   while (esp_timer_get_time() < deadline_us) {
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

   ESP_LOGE(TAG, "chain setup(%s): timeout waiting for ACK", object);
   return ESP_ERR_TIMEOUT;
}

esp_err_t voice_m5_llm_chain_setup(voice_m5_chain_handle_t **out_handle) {
   if (out_handle == NULL) return ESP_ERR_INVALID_ARG;
   *out_handle = NULL;

   esp_err_t err = ensure_uart();
   if (err != ESP_OK) return err;

   voice_m5_chain_handle_t *h = heap_caps_calloc(1, sizeof(*h), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (h == NULL) return ESP_ERR_NO_MEM;

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
      err = chain_setup_unit("audio", "audio.setup", d, h->audio_id, sizeof(h->audio_id), M5_SETUP_TIMEOUT_MS);
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
      err = chain_setup_unit("asr", "asr.setup", d, h->asr_id, sizeof(h->asr_id), M5_SETUP_TIMEOUT_MS);
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
      err = chain_setup_unit("llm", "llm.setup", d, h->llm_id, sizeof(h->llm_id), 15000);
      if (err != ESP_OK) goto fail;
   }

   /* 4) tts.setup — subscribe to LLM text, base64 wav out. */
   {
      cJSON *d = cJSON_CreateObject();
      cJSON_AddStringToObject(d, "model", M5_TTS_MODEL);
      cJSON_AddStringToObject(d, "response_format", "tts.base64.wav");
      cJSON *inp = cJSON_CreateArray();
      cJSON_AddItemToArray(inp, cJSON_CreateString(h->llm_id));
      cJSON_AddItemToObject(d, "input", inp);
      cJSON_AddBoolToObject(d, "enoutput", true);
      cJSON_AddBoolToObject(d, "enkws", false);
      err = chain_setup_unit("tts", "tts.setup", d, h->tts_id, sizeof(h->tts_id), M5_SETUP_TIMEOUT_MS);
      if (err != ESP_OK) goto fail;
   }

   ESP_LOGI(TAG, "chain ready: audio=%s asr=%s llm=%s tts=%s — speak at the K144", h->audio_id, h->asr_id, h->llm_id,
            h->tts_id);
   *out_handle = h;
   return ESP_OK;

fail:
   /* Best-effort tear-down of whatever DID come up. */
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
   /* Reverse order of bring-up so each unit's downstream subscriber is
    * killed before the publisher disappears. */
   chain_exit_unit(handle->tts_id);
   chain_exit_unit(handle->llm_id);
   chain_exit_unit(handle->asr_id);
   chain_exit_unit(handle->audio_id);
   ESP_LOGI(TAG, "chain torn down");
   heap_caps_free(handle);
}

esp_err_t voice_m5_llm_chain_run(voice_m5_chain_handle_t *handle, voice_m5_chain_text_cb text_cb,
                                 voice_m5_chain_audio_cb audio_cb, void *user, volatile bool *stop_flag,
                                 uint32_t timeout_s) {
   if (handle == NULL) return ESP_ERR_INVALID_ARG;
   if (handle->asr_id[0] == '\0' || handle->llm_id[0] == '\0' || handle->tts_id[0] == '\0') {
      return ESP_ERR_INVALID_STATE;
   }
   esp_err_t err = ensure_uart();
   if (err != ESP_OK) return err;
   if (ensure_rx_buf() != ESP_OK) return ESP_ERR_NO_MEM;

   const int64_t deadline_us = (timeout_s == 0) ? INT64_MAX : esp_timer_get_time() + (int64_t)timeout_s * 1000 * 1000;

   /* PCM scratch — TTS chunks decode to ~16 KB each (1 sec of 16 kHz mono).
    * Allocate generous PSRAM headroom for longer chunks. */
   const size_t pcm_cap_samples = 64 * 1024; /* 4 sec @ 16 kHz */
   int16_t *pcm = heap_caps_malloc(pcm_cap_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (pcm == NULL) return ESP_ERR_NO_MEM;

   while (!(stop_flag != NULL && *stop_flag) && esp_timer_get_time() < deadline_us) {
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
      if (nl == NULL) continue;

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
      if (resp.work_id == NULL || resp.data == NULL) {
         m5_stackflow_response_free(&resp);
         continue;
      }

      const bool is_asr = (strcmp(resp.work_id, handle->asr_id) == 0);
      const bool is_llm = (strcmp(resp.work_id, handle->llm_id) == 0);
      const bool is_tts = (strcmp(resp.work_id, handle->tts_id) == 0);
      if (!is_asr && !is_llm && !is_tts) {
         m5_stackflow_response_free(&resp);
         continue;
      }

      if (is_tts) {
         /* TTS: data is either bare base64 string OR {delta,index,finish}. */
         const char *b64 = NULL;
         if (cJSON_IsString(resp.data)) {
            b64 = resp.data->valuestring;
         } else if (cJSON_IsObject(resp.data)) {
            const cJSON *d = cJSON_GetObjectItemCaseSensitive(resp.data, "delta");
            if (cJSON_IsString(d)) b64 = d->valuestring;
         }
         if (b64 != NULL && audio_cb != NULL) {
            size_t pcm_bytes = 0;
            int dec = mbedtls_base64_decode((unsigned char *)pcm, pcm_cap_samples * sizeof(int16_t), &pcm_bytes,
                                            (const unsigned char *)b64, strlen(b64));
            if (dec == 0 && pcm_bytes > 0) {
               audio_cb(pcm, pcm_bytes / sizeof(int16_t), user);
            } else if (dec != 0) {
               ESP_LOGW(TAG, "chain: tts base64 decode failed -0x%04x (b64_len=%u)", -dec, (unsigned)strlen(b64));
            }
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
   }

   heap_caps_free(pcm);
   if (stop_flag != NULL && *stop_flag) return ESP_OK;
   return ESP_ERR_TIMEOUT;
}

esp_err_t voice_m5_llm_recover_baud(uint32_t candidate_baud) {
   esp_err_t uart_err = ensure_uart();
   if (uart_err != ESP_OK) return uart_err;
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
      return ESP_ERR_INVALID_RESPONSE;
   }
   ESP_LOGI(TAG, "Recovery: K144 found at %lu — reuniting both ends at 115200", (unsigned long)candidate_baud);
   /* Now do a clean uartsetup back to 115200. */
   esp_err_t ret = voice_m5_llm_set_baud(115200);
   if (ret != ESP_OK) {
      ESP_LOGW(TAG, "Recovery: graceful switch back to 115200 failed (%s)", esp_err_to_name(ret));
   }
   return ret;
}
