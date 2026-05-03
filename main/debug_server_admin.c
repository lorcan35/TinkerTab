/*
 * debug_server_admin.c — system-control / storage / widget-injection
 * debug HTTP family.
 *
 * Wave 23b follow-up (#332): fifteenth per-family extract.  Owns:
 *
 *   POST /reboot     — esp_restart after a 500 ms grace
 *   GET  /sdcard     — mount state + total/free MB + root + /sdcard/rec
 *                      directory listings
 *   POST /widget     — test-harness hook that injects a widget_live (or
 *                      dismisses one) directly into the widget store,
 *                      bypassing Dragon.  Body matches the widget_live
 *                      schema (TinkerBox docs/protocol.md §17.2).
 *
 * Same convention as the prior 14 per-family extracts:
 *   check_auth(req)  → tab5_debug_check_auth(req)
 */
#include "debug_server_admin.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "debug_server_internal.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdcard.h"
#include "ui_core.h" /* tab5_lv_async_call */
#include "widget.h"

static const char *TAG = "debug_admin";

#define check_auth(req) tab5_debug_check_auth(req)

/* ── POST /reboot ────────────────────────────────────────────────────── */

static esp_err_t reboot_handler(httpd_req_t *req) {
   if (!check_auth(req)) return ESP_OK;

   httpd_resp_set_type(req, "application/json");
   httpd_resp_sendstr(req, "{\"rebooting\":true}");
   ESP_LOGW(TAG, "Reboot requested via debug server");
   vTaskDelay(pdMS_TO_TICKS(500));
   esp_restart();
   return ESP_OK; /* unreachable */
}

/* ── GET /sdcard ─────────────────────────────────────────────────────── */

static void list_dir_to_json(cJSON *arr, const char *path) {
   DIR *d = opendir(path);
   if (!d) return;

   struct dirent *entry;
   while ((entry = readdir(d)) != NULL) {
      char full[320];
      snprintf(full, sizeof(full), "%s/%s", path, entry->d_name);

      struct stat st;
      stat(full, &st);

      cJSON *item = cJSON_CreateObject();
      cJSON_AddStringToObject(item, "name", entry->d_name);
      cJSON_AddStringToObject(item, "path", full);
      cJSON_AddBoolToObject(item, "dir", entry->d_type == DT_DIR);
      if (entry->d_type != DT_DIR) {
         cJSON_AddNumberToObject(item, "size", (double)st.st_size);
      }
      cJSON_AddItemToArray(arr, item);
   }
   closedir(d);
}

static esp_err_t sdcard_handler(httpd_req_t *req) {
   if (!check_auth(req)) return ESP_OK;

   cJSON *root = cJSON_CreateObject();
   cJSON_AddBoolToObject(root, "mounted", tab5_sdcard_mounted());

   if (tab5_sdcard_mounted()) {
      cJSON_AddNumberToObject(root, "total_mb", (double)(tab5_sdcard_total_bytes() / (1024 * 1024)));
      cJSON_AddNumberToObject(root, "free_mb", (double)(tab5_sdcard_free_bytes() / (1024 * 1024)));

      /* List /sdcard root */
      cJSON *files = cJSON_AddArrayToObject(root, "files");
      list_dir_to_json(files, "/sdcard");

      /* List /sdcard/rec if it exists */
      struct stat st;
      if (stat("/sdcard/rec", &st) == 0) {
         cJSON *recs = cJSON_AddArrayToObject(root, "recordings");
         list_dir_to_json(recs, "/sdcard/rec");
      }
   }

   char *json = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   if (!json) {
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON alloc");
      return ESP_FAIL;
   }

   httpd_resp_set_type(req, "application/json");
   httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
   httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Authorization");
   esp_err_t ret = httpd_resp_sendstr(req, json);
   free(json);
   return ret;
}

/* ── POST /widget ────────────────────────────────────────────────────── */
/* Testing-only hook that injects a widget_live into the store without
 * requiring Dragon. Body is JSON matching widget_live schema (see
 * TinkerBox docs/protocol.md §17.2). Used by the Widget Platform test
 * harness before the Dragon Tab5Surface is wired. */

static void async_widget_refresh(void *arg) {
   (void)arg;
   extern void ui_home_update_status(void);
   ui_home_update_status();
}

static esp_err_t widget_handler(httpd_req_t *req) {
   if (!check_auth(req)) return ESP_OK;

   /* Read body */
   int len = req->content_len;
   if (len <= 0 || len > 2048) {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body 1..2048 bytes");
      return ESP_FAIL;
   }
   char *buf = malloc(len + 1);
   if (!buf) {
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
      return ESP_FAIL;
   }
   int got = 0;
   while (got < len) {
      int r = httpd_req_recv(req, buf + got, len - got);
      if (r <= 0) {
         free(buf);
         httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "read");
         return ESP_FAIL;
      }
      got += r;
   }
   buf[len] = '\0';

   cJSON *root = cJSON_Parse(buf);
   free(buf);
   if (!root) {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
      return ESP_FAIL;
   }

   const char *action = cJSON_GetStringValue(cJSON_GetObjectItem(root, "action"));
   if (action && !strcmp(action, "dismiss")) {
      const char *cid = cJSON_GetStringValue(cJSON_GetObjectItem(root, "card_id"));
      if (cid) widget_store_dismiss(cid);
      cJSON_Delete(root);
      tab5_lv_async_call(async_widget_refresh, NULL);
      httpd_resp_sendstr(req, "{\"ok\":true}");
      return ESP_OK;
   }

   /* Build widget_t from JSON */
   widget_t w = {0};
   const char *sid = cJSON_GetStringValue(cJSON_GetObjectItem(root, "skill_id"));
   const char *cid = cJSON_GetStringValue(cJSON_GetObjectItem(root, "card_id"));
   const char *ttl = cJSON_GetStringValue(cJSON_GetObjectItem(root, "title"));
   const char *bdy = cJSON_GetStringValue(cJSON_GetObjectItem(root, "body"));
   const char *icn = cJSON_GetStringValue(cJSON_GetObjectItem(root, "icon"));
   const char *tn = cJSON_GetStringValue(cJSON_GetObjectItem(root, "tone"));
   cJSON *prog = cJSON_GetObjectItem(root, "progress");
   cJSON *pri = cJSON_GetObjectItem(root, "priority");
   cJSON *act = cJSON_GetObjectItem(root, "action");

   if (!cid) cid = "debug_1";
   if (!sid) sid = "debug";
   strncpy(w.card_id, cid, WIDGET_ID_LEN - 1);
   strncpy(w.skill_id, sid, WIDGET_SKILL_ID_LEN - 1);
   if (ttl) strncpy(w.title, ttl, WIDGET_TITLE_LEN - 1);
   if (bdy) strncpy(w.body, bdy, WIDGET_BODY_LEN - 1);
   if (icn) strncpy(w.icon, icn, WIDGET_ICON_LEN - 1);
   /* v4·D Phase 4c: optional "type" + "items" fields let the debug
    * endpoint inject widget_list payloads for visual testing. */
   const char *tstr = cJSON_GetStringValue(cJSON_GetObjectItem(root, "type"));
   w.type = tstr ? widget_type_from_str(tstr) : WIDGET_TYPE_LIVE;
   if (w.type == WIDGET_TYPE_NONE) w.type = WIDGET_TYPE_LIVE;
   w.tone = widget_tone_from_str(tn);
   w.progress = cJSON_IsNumber(prog) ? (float)prog->valuedouble : 0.0f;
   w.priority = cJSON_IsNumber(pri) ? (uint8_t)pri->valueint : 50;
   if (cJSON_IsObject(act)) {
      const char *al = cJSON_GetStringValue(cJSON_GetObjectItem(act, "label"));
      const char *ae = cJSON_GetStringValue(cJSON_GetObjectItem(act, "event"));
      if (al) strncpy(w.action_label, al, WIDGET_ACTION_LBL_LEN - 1);
      if (ae) strncpy(w.action_event, ae, WIDGET_ACTION_EVT_LEN - 1);
   }
   cJSON *items = cJSON_GetObjectItem(root, "items");
   if (cJSON_IsArray(items)) {
      int cnt = cJSON_GetArraySize(items);
      if (cnt > WIDGET_LIST_MAX_ITEMS) cnt = WIDGET_LIST_MAX_ITEMS;
      for (int i = 0; i < cnt; i++) {
         cJSON *it = cJSON_GetArrayItem(items, i);
         if (!cJSON_IsObject(it)) continue;
         const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(it, "text"));
         const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(it, "value"));
         if (t) strncpy(w.items[i].text, t, WIDGET_LIST_ITEM_TEXT_LEN - 1);
         if (v) strncpy(w.items[i].value, v, WIDGET_LIST_ITEM_VALUE_LEN - 1);
      }
      w.items_count = (uint8_t)cnt;
   }
   /* v4·D Phase 4f: optional "values" + "max" fields for widget_chart. */
   cJSON *vals = cJSON_GetObjectItem(root, "values");
   if (cJSON_IsArray(vals)) {
      int cnt = cJSON_GetArraySize(vals);
      if (cnt > WIDGET_CHART_MAX_POINTS) cnt = WIDGET_CHART_MAX_POINTS;
      for (int i = 0; i < cnt; i++) {
         cJSON *v = cJSON_GetArrayItem(vals, i);
         w.chart_values[i] = cJSON_IsNumber(v) ? (float)v->valuedouble : 0.0f;
      }
      w.chart_count = (uint8_t)cnt;
   }
   cJSON *mx = cJSON_GetObjectItem(root, "max");
   w.chart_max = cJSON_IsNumber(mx) ? (float)mx->valuedouble : 0.0f;
   /* v4·D Phase 4g: media + prompt fields for /widget debug injection. */
   const char *media_url = cJSON_GetStringValue(cJSON_GetObjectItem(root, "url"));
   const char *media_alt = cJSON_GetStringValue(cJSON_GetObjectItem(root, "alt"));
   if (media_url) strncpy(w.media_url, media_url, WIDGET_MEDIA_URL_LEN - 1);
   if (media_alt) strncpy(w.media_alt, media_alt, WIDGET_MEDIA_ALT_LEN - 1);
   cJSON *choices = cJSON_GetObjectItem(root, "choices");
   if (cJSON_IsArray(choices)) {
      int cnt = cJSON_GetArraySize(choices);
      if (cnt > WIDGET_PROMPT_MAX_CHOICES) cnt = WIDGET_PROMPT_MAX_CHOICES;
      for (int i = 0; i < cnt; i++) {
         cJSON *it = cJSON_GetArrayItem(choices, i);
         if (!cJSON_IsObject(it)) continue;
         const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(it, "text"));
         const char *ev = cJSON_GetStringValue(cJSON_GetObjectItem(it, "event"));
         if (t) strncpy(w.choices[i].text, t, WIDGET_PROMPT_CHOICE_LEN - 1);
         if (ev) strncpy(w.choices[i].event, ev, WIDGET_PROMPT_EVENT_LEN - 1);
      }
      w.choices_count = (uint8_t)cnt;
   }
   widget_store_upsert(&w);
   cJSON_Delete(root);
   tab5_lv_async_call(async_widget_refresh, NULL);

   httpd_resp_set_type(req, "application/json");
   httpd_resp_sendstr(req, "{\"ok\":true}");
   return ESP_OK;
}

void debug_server_admin_register(httpd_handle_t server) {
   if (!server) return;

   static const httpd_uri_t uri_reboot = {.uri = "/reboot", .method = HTTP_POST, .handler = reboot_handler};
   static const httpd_uri_t uri_sdcard = {.uri = "/sdcard", .method = HTTP_GET, .handler = sdcard_handler};
   static const httpd_uri_t uri_widget = {.uri = "/widget", .method = HTTP_POST, .handler = widget_handler};

   httpd_register_uri_handler(server, &uri_reboot);
   httpd_register_uri_handler(server, &uri_sdcard);
   httpd_register_uri_handler(server, &uri_widget);
}
