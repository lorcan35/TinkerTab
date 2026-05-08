/* debug_server_solo — synthetic test endpoints for vmode=5 modules.
 *
 * /solo/sse_test  POST raw SSE bytes → JSON {deltas:[...], done:bool}
 *
 * Subsequent endpoints (/solo/llm_test, /solo/rag_test) land alongside
 * as the solo modules grow.  These are intentionally permanent — the
 * E2E harness uses them for deterministic unit-style coverage of the
 * pure-logic modules (SSE parser, RAG cosine) without needing real
 * hardware audio + real OpenRouter calls.
 *
 * TT #370 — see openrouter_sse.h.
 */

#include <string.h>

#include "cJSON.h"
#include "debug_server.h"
#include "debug_server_internal.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "openrouter_client.h"
#include "openrouter_sse.h"

static const char *TAG = "debug_solo";

typedef struct {
   cJSON *deltas;
} sse_test_ctx_t;

static void sse_test_cb(const char *json, size_t len, void *vctx) {
   sse_test_ctx_t *t = vctx;
   /* Parse JSON, pull choices[0].delta.content out into the array. */
   char *copy = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (!copy) return;
   memcpy(copy, json, len);
   copy[len] = '\0';
   cJSON *root = cJSON_Parse(copy);
   if (root) {
      cJSON *choices = cJSON_GetObjectItem(root, "choices");
      if (cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0) {
         cJSON *c0 = cJSON_GetArrayItem(choices, 0);
         cJSON *delta = cJSON_GetObjectItem(c0, "delta");
         cJSON *content = delta ? cJSON_GetObjectItem(delta, "content") : NULL;
         if (cJSON_IsString(content) && content->valuestring) {
            cJSON_AddItemToArray(t->deltas, cJSON_CreateString(content->valuestring));
         }
      }
      cJSON_Delete(root);
   }
   heap_caps_free(copy);
}

static esp_err_t sse_test_handler(httpd_req_t *req) {
   if (!tab5_debug_check_auth(req)) return ESP_FAIL;

   size_t total = req->content_len;
   if (total == 0 || total > 64 * 1024) {
      return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad len");
   }
   char *buf = heap_caps_malloc(total + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (!buf) return ESP_ERR_NO_MEM;
   size_t got = 0;
   while (got < total) {
      int n = httpd_req_recv(req, buf + got, total - got);
      if (n <= 0) {
         heap_caps_free(buf);
         return ESP_FAIL;
      }
      got += (size_t)n;
   }

   sse_test_ctx_t tctx = {.deltas = cJSON_CreateArray()};
   openrouter_sse_state_t *s = NULL;
   esp_err_t ie = openrouter_sse_init(&s, sse_test_cb, &tctx);
   bool done = false;
   if (ie == ESP_OK) {
      done = openrouter_sse_feed(s, buf, total);
      openrouter_sse_free(s);
   }
   heap_caps_free(buf);

   cJSON *out = cJSON_CreateObject();
   cJSON_AddItemToObject(out, "deltas", tctx.deltas);
   cJSON_AddBoolToObject(out, "done", done);
   return tab5_debug_send_json_resp(req, out);
}

typedef struct {
   char *reply;
   size_t cap;
   size_t len;
   size_t deltas;
} llm_test_acc_t;

static void llm_test_delta_cb(const char *d, size_t n, void *vctx) {
   llm_test_acc_t *acc = vctx;
   if (acc->len + n + 1 < acc->cap) {
      memcpy(acc->reply + acc->len, d, n);
      acc->len += n;
      acc->reply[acc->len] = '\0';
   }
   acc->deltas++;
}

static esp_err_t llm_test_handler(httpd_req_t *req) {
   if (!tab5_debug_check_auth(req)) return ESP_FAIL;

   char body[512] = {0};
   size_t total = req->content_len < sizeof(body) - 1 ? req->content_len : sizeof(body) - 1;
   if (total == 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty");
   size_t got = 0;
   while (got < total) {
      int n = httpd_req_recv(req, body + got, total - got);
      if (n <= 0) return ESP_FAIL;
      got += (size_t)n;
   }
   body[got] = '\0';

   cJSON *root = cJSON_Parse(body);
   cJSON *prompt = root ? cJSON_GetObjectItem(root, "prompt") : NULL;
   if (!cJSON_IsString(prompt)) {
      if (root) cJSON_Delete(root);
      return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad prompt");
   }

   /* Build messages array using cJSON for safe escaping. */
   cJSON *msgs = cJSON_CreateArray();
   cJSON *m = cJSON_CreateObject();
   cJSON_AddStringToObject(m, "role", "user");
   cJSON_AddStringToObject(m, "content", prompt->valuestring);
   cJSON_AddItemToArray(msgs, m);
   char *msgs_json = cJSON_PrintUnformatted(msgs);
   cJSON_Delete(msgs);
   cJSON_Delete(root);

   llm_test_acc_t acc = {0};
   acc.cap = 4096;
   acc.reply = heap_caps_calloc(1, acc.cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   esp_err_t err = openrouter_chat_stream(msgs_json, llm_test_delta_cb, &acc);
   free(msgs_json);

   cJSON *out = cJSON_CreateObject();
   cJSON_AddBoolToObject(out, "ok", err == ESP_OK);
   cJSON_AddStringToObject(out, "reply", acc.reply ? acc.reply : "");
   cJSON_AddNumberToObject(out, "delta_count", (double)acc.deltas);
   if (err != ESP_OK) {
      cJSON_AddStringToObject(out, "error", esp_err_to_name(err));
   }
   heap_caps_free(acc.reply);
   return tab5_debug_send_json_resp(req, out);
}

void debug_server_solo_register(httpd_handle_t server) {
   if (!server) return;
   static const httpd_uri_t uri_sse = {.uri = "/solo/sse_test", .method = HTTP_POST, .handler = sse_test_handler};
   esp_err_t e = httpd_register_uri_handler(server, &uri_sse);
   if (e == ESP_OK) {
      ESP_LOGI(TAG, "registered /solo/sse_test");
   } else {
      ESP_LOGE(TAG, "register /solo/sse_test failed: %s", esp_err_to_name(e));
   }

   static const httpd_uri_t uri_llm = {.uri = "/solo/llm_test", .method = HTTP_POST, .handler = llm_test_handler};
   httpd_register_uri_handler(server, &uri_llm);
   ESP_LOGI(TAG, "registered /solo/llm_test");
}
