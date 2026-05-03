/*
 * debug_server_chat.c — chat surface debug HTTP family.
 *
 * Wave 23b follow-up (#332): sixteenth per-family extract.  All 5
 * chat-overlay debug endpoints + a local url_pct_decode_inplace
 * helper move from debug_server.c into a new sibling module:
 *
 *   POST /chat              — send text to Dragon via voice WS
 *                             (also kicks ui_chat_push_message so the
 *                              user-bubble shows even if Dragon is down)
 *   GET  /chat/messages?n=  — last N rows from chat_msg_store
 *   POST /chat/llm_done?text= — feed text through md_strip_tool_markers
 *                               + ui_chat_push_message (verifies the
 *                               #78/#160 defensive scrub without
 *                               needing Dragon to emit a tool call)
 *   POST /chat/partial?text= — set the live STT-partial caption above
 *                              the chat input pill
 *   POST /chat/audio_clip?url=&label=
 *                            — inject an audio_clip row (U5 #206)
 *
 * Same convention as the prior 15 per-family extracts:
 *   check_auth(req)            → tab5_debug_check_auth(req)
 *   send_json_resp(req, root)  → tab5_debug_send_json_resp(req, root)
 *
 * Note on duplication: url_pct_decode_inplace is also referenced by
 * tool_log_push_handler in debug_server.c.  We keep a local copy here
 * (24 lines) to avoid an artificial cross-TU dependency on what is
 * essentially a 5-line URL-decode primitive.  If a future module
 * needs it too, lift to debug_server_internal.h.
 */
#include "debug_server_chat.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "chat_msg_store.h" /* chat_store_count / _get + chat_msg_t */
#include "debug_server_internal.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "ui_chat.h" /* ui_chat_push_message / show_partial / push_audio_clip */
#include "voice.h"   /* voice_send_text / voice_is_connected */

static const char *TAG = "debug_chat";

#define check_auth(req) tab5_debug_check_auth(req)
#define send_json_resp(req, root) tab5_debug_send_json_resp(req, root)

/* Local copy — see file header for the duplication rationale. */
static void url_pct_decode_inplace(char *s) {
   char *r = s, *w = s;
   while (*r) {
      if (*r == '%' && r[1] && r[2]) {
         char hi = r[1], lo = r[2];
         int hv = (hi >= '0' && hi <= '9')   ? hi - '0'
                  : (hi >= 'a' && hi <= 'f') ? hi - 'a' + 10
                  : (hi >= 'A' && hi <= 'F') ? hi - 'A' + 10
                                             : -1;
         int lv = (lo >= '0' && lo <= '9')   ? lo - '0'
                  : (lo >= 'a' && lo <= 'f') ? lo - 'a' + 10
                  : (lo >= 'A' && lo <= 'F') ? lo - 'A' + 10
                                             : -1;
         if (hv >= 0 && lv >= 0) {
            *w++ = (char)((hv << 4) | lv);
            r += 3;
            continue;
         }
      }
      if (*r == '+') {
         *w++ = ' ';
         r++;
         continue;
      }
      *w++ = *r++;
   }
   *w = 0;
}

/* ── POST /chat — send text to Dragon via voice WS ───────────────────── */

static esp_err_t chat_handler(httpd_req_t *req) {
   if (!check_auth(req)) return ESP_OK;

   /* POST /chat — send text to Dragon via voice WS.
    * #148: was capped at a 256-byte stack buffer which silently truncated
    * anything longer than ~200 bytes of JSON after the `{"text":"..."}`
    * overhead.  Now accepts up to 4 KB via PSRAM-backed body buffer with
    * the text itself up to 1 KB (voice.c accepts up to 2 KB). */
   const size_t MAX_BODY = 4096;
   const size_t MAX_TEXT = 1024;
   char text_buf[MAX_TEXT + 1];
   text_buf[0] = '\0';

   /* First try query string — cheap for short text, no body needed. */
   char query[256] = {0};
   if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
      httpd_query_key_value(query, "text", text_buf, sizeof(text_buf));
   }

   /* If no query text, parse JSON body from heap. */
   if (text_buf[0] == '\0') {
      int total = req->content_len;
      if (total > 0) {
         if (total > (int)MAX_BODY) total = MAX_BODY;
         char *body = heap_caps_malloc(total + 1, MALLOC_CAP_SPIRAM);
         if (!body) {
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, "{\"error\":\"oom\"}");
            return ESP_OK;
         }
         int got = 0;
         while (got < total) {
            int r = httpd_req_recv(req, body + got, total - got);
            if (r <= 0) break;
            got += r;
         }
         body[got] = '\0';
         cJSON *root = cJSON_Parse(body);
         heap_caps_free(body);
         if (root) {
            cJSON *t = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(t) && t->valuestring) {
               snprintf(text_buf, sizeof(text_buf), "%s", t->valuestring);
            }
            cJSON_Delete(root);
         }
      }
   }

   if (text_buf[0] == '\0') {
      httpd_resp_set_type(req, "application/json");
      httpd_resp_sendstr(req, "{\"error\":\"use ?text=hello or POST {\\\"text\\\":\\\"hello\\\"}\"}");
      return ESP_OK;
   }

   ESP_LOGI(TAG, "Debug chat: %.80s%s (len=%u)", text_buf, strlen(text_buf) > 80 ? "..." : "",
            (unsigned)strlen(text_buf));

   /* TT #317 P5: don't short-circuit on !voice_is_connected — let
    * voice_send_text decide.  When WS is down, the K144 failover (Local
    * mode) or VMODE_LOCAL_ONBOARD path may still complete the turn. */
   ui_chat_push_message("user", text_buf);
   esp_err_t send_err = voice_send_text(text_buf);
   bool sent_ok = (send_err == ESP_OK);

   cJSON *root = cJSON_CreateObject();
   cJSON_AddStringToObject(root, "text", text_buf);
   cJSON_AddNumberToObject(root, "text_len", (double)strlen(text_buf));
   cJSON_AddBoolToObject(root, "sent", sent_ok);
   cJSON_AddBoolToObject(root, "voice_connected", voice_is_connected());
   if (!sent_ok) {
      cJSON_AddStringToObject(root, "send_status", esp_err_to_name(send_err));
   }
   char *json = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   httpd_resp_set_type(req, "application/json");
   httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
   httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Authorization");
   esp_err_t ret = httpd_resp_sendstr(req, json);
   free(json);
   return ret;
}

/* ── GET /chat/messages?n=50 ────────────────────────────────────────── */

static esp_err_t chat_messages_handler(httpd_req_t *req) {
   if (!check_auth(req)) return ESP_OK;
   int n = 50;
   char q[32] = {0}, v[8] = {0};
   if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
       httpd_query_key_value(q, "n", v, sizeof(v)) == ESP_OK) {
      int x = atoi(v);
      if (x > 0 && x <= 500) n = x;
   }
   int total = chat_store_count();
   if (n > total) n = total;

   cJSON *root = cJSON_CreateObject();
   cJSON *arr = cJSON_AddArrayToObject(root, "messages");
   cJSON_AddNumberToObject(root, "total", total);
   cJSON_AddNumberToObject(root, "returned", n);

   /* Return the last `n` messages, oldest first. */
   for (int i = total - n; i < total; i++) {
      const chat_msg_t *m = chat_store_get(i);
      if (!m) continue;
      cJSON *o = cJSON_CreateObject();
      cJSON_AddStringToObject(o, "role", m->is_user ? "user" : "assistant");
      const char *types[] = {"text", "image", "card", "audio", "system"};
      cJSON_AddStringToObject(o, "type", (int)m->type < (int)(sizeof(types) / sizeof(types[0])) ? types[m->type] : "?");
      cJSON_AddStringToObject(o, "text", m->text);
      if (m->media_url[0]) cJSON_AddStringToObject(o, "media_url", m->media_url);
      if (m->subtitle[0]) cJSON_AddStringToObject(o, "subtitle", m->subtitle);
      cJSON_AddNumberToObject(o, "timestamp", (double)m->timestamp);
      if (m->receipt_mils > 0) {
         cJSON *rcpt = cJSON_AddObjectToObject(o, "receipt");
         cJSON_AddNumberToObject(rcpt, "mils", m->receipt_mils);
         cJSON_AddNumberToObject(rcpt, "prompt_tok", m->receipt_ptok);
         cJSON_AddNumberToObject(rcpt, "compl_tok", m->receipt_ctok);
         cJSON_AddStringToObject(rcpt, "model", m->receipt_model_short);
         cJSON_AddBoolToObject(rcpt, "retried", m->receipt_retried);
      }
      cJSON_AddItemToArray(arr, o);
   }
   return send_json_resp(req, root);
}

/* ── POST /chat/llm_done?text=<> — verify Tab5-side md_strip_tool_markers ─ */

extern void md_strip_tool_markers(const char *in, char *out, size_t out_cap);

static esp_err_t chat_llm_done_handler(httpd_req_t *req) {
   if (!check_auth(req)) return ESP_OK;
   char q[1024] = {0}, text[800] = {0};
   httpd_req_get_url_query_str(req, q, sizeof(q));
   httpd_query_key_value(q, "text", text, sizeof(text));
   url_pct_decode_inplace(text);
   cJSON *root = cJSON_CreateObject();
   if (!text[0]) {
      cJSON_AddStringToObject(root, "error", "need ?text=<llm response>");
      return send_json_resp(req, root);
   }
   char clean[800];
   md_strip_tool_markers(text, clean, sizeof(clean));
   if (clean[0]) {
      ui_chat_push_message("assistant", clean);
   }
   cJSON_AddBoolToObject(root, "ok", true);
   cJSON_AddStringToObject(root, "raw", text);
   cJSON_AddStringToObject(root, "after_strip", clean);
   return send_json_resp(req, root);
}

/* ── POST /chat/partial?text=<> ─────────────────────────────────────── */

static esp_err_t chat_partial_handler(httpd_req_t *req) {
   if (!check_auth(req)) return ESP_OK;
   char q[256] = {0}, text[160] = {0};
   httpd_req_get_url_query_str(req, q, sizeof(q));
   httpd_query_key_value(q, "text", text, sizeof(text));
   url_pct_decode_inplace(text);
   ui_chat_show_partial(text[0] ? text : NULL);
   cJSON *root = cJSON_CreateObject();
   cJSON_AddBoolToObject(root, "ok", true);
   cJSON_AddStringToObject(root, "text", text);
   return send_json_resp(req, root);
}

/* ── POST /chat/audio_clip?url=<>&label=<> ─────────────────────────── */

static esp_err_t chat_audio_clip_handler(httpd_req_t *req) {
   if (!check_auth(req)) return ESP_OK;
   char q[512] = {0}, url[256] = {0}, label[96] = {0};
   httpd_req_get_url_query_str(req, q, sizeof(q));
   httpd_query_key_value(q, "url", url, sizeof(url));
   httpd_query_key_value(q, "label", label, sizeof(label));
   url_pct_decode_inplace(url);
   url_pct_decode_inplace(label);

   cJSON *root = cJSON_CreateObject();
   if (!url[0]) {
      cJSON_AddStringToObject(root, "error", "need ?url=<dragon-relative or absolute>");
      return send_json_resp(req, root);
   }
   ui_chat_push_audio_clip(url, 0.0f, label[0] ? label : "test clip");
   cJSON_AddBoolToObject(root, "ok", true);
   cJSON_AddStringToObject(root, "url", url);
   cJSON_AddStringToObject(root, "label", label[0] ? label : "test clip");
   return send_json_resp(req, root);
}

/* ── POST /voice/text — alias of /chat (#149 PR β) ──────────────────── */
/* α already fixed /chat to heap-allocate — /voice/text is an explicit
 * alias so the REST surface reads cleaner.  Forward wholesale. */
static esp_err_t voice_text_handler(httpd_req_t *req) { return chat_handler(req); }

void debug_server_chat_register(httpd_handle_t server) {
   if (!server) return;

   static const httpd_uri_t uri_chat = {.uri = "/chat", .method = HTTP_POST, .handler = chat_handler};
   static const httpd_uri_t uri_chat_msgs = {
       .uri = "/chat/messages", .method = HTTP_GET, .handler = chat_messages_handler};
   static const httpd_uri_t uri_chat_audio = {
       .uri = "/chat/audio_clip", .method = HTTP_POST, .handler = chat_audio_clip_handler};
   static const httpd_uri_t uri_chat_partial = {
       .uri = "/chat/partial", .method = HTTP_POST, .handler = chat_partial_handler};
   static const httpd_uri_t uri_chat_llm_done = {
       .uri = "/chat/llm_done", .method = HTTP_POST, .handler = chat_llm_done_handler};
   static const httpd_uri_t uri_voice_text = {.uri = "/voice/text", .method = HTTP_POST, .handler = voice_text_handler};

   httpd_register_uri_handler(server, &uri_chat);
   httpd_register_uri_handler(server, &uri_chat_msgs);
   httpd_register_uri_handler(server, &uri_chat_audio);
   httpd_register_uri_handler(server, &uri_chat_partial);
   httpd_register_uri_handler(server, &uri_chat_llm_done);
   httpd_register_uri_handler(server, &uri_voice_text);
}
