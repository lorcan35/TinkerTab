/*
 * voice_widget_ws.c — WS widget-verb dispatch (extracted from voice.c).
 *
 * 2026-05-03 SOLID audit SRP-2: nine widget verbs moved verbatim from
 * voice.c's `handle_text_message()` into a single dispatch function.
 *
 * Behavior is bit-for-bit identical to the pre-extract code paths;
 * only call-site changes:
 *   * The six redundant `extern widget_store_upsert / widget_store_update
 *     / widget_tone_from_str / ui_home_update_status` clusters that
 *     used to sit inside voice.c are replaced by the single header
 *     includes here.
 *   * Each handler still calls `tab5_lv_async_call` to bounce the home
 *     status refresh onto the LVGL thread (LVGL 9.x lv_async_call is
 *     not thread-safe — see CLAUDE.md "lv_async_call not thread-safe").
 *   * Each handler still calls `ui_chat_push_card_action` for
 *     widget_card the same way voice.c did.
 *
 * Coverage proven via the cross-modal e2e harness — Wave 12 +
 * Phase 4c/4f/4g shipping covers every verb in this dispatch.
 */

#include "voice_widget_ws.h"

#include <stdint.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "ui_chat.h" /* ui_chat_push_card_action */
#include "ui_core.h" /* tab5_lv_async_call */
#include "ui_home.h" /* ui_home_update_status */
#include "widget.h"  /* widget_t, widget_store_*, widget_tone_from_str, WIDGET_* sizes */

static const char *TAG = "widget_ws";

bool voice_widget_ws_dispatch(const char *type_str, struct cJSON *root) {
   /* Fast reject so the cJSON cast below stays cheap when called for
    * non-widget verbs (the common case at runtime — chat + STT + LLM
    * frames dominate). */
   if (!type_str || strncmp(type_str, "widget_", 7) != 0) {
      return false;
   }
   cJSON *r = (cJSON *)root;

   if (strcmp(type_str, "widget_card") == 0) {
      /* Audit B2 (2026-04-20): widget_card from Tab5Surface.card()
       * is a different shape than the legacy "card" message used by
       * rich-media turns — title + body + tone + optional image_url.
       * Route it to the same chat bubble renderer so skills can push
       * context cards into the conversation.
       *
       * Phase 2 (#70): also read card_id + action.{label,event} so the
       * chat renderer can draw a tappable button that round-trips a
       * widget_action back to the skill. */
      const char *title = cJSON_GetStringValue(cJSON_GetObjectItem(r, "title"));
      const char *body = cJSON_GetStringValue(cJSON_GetObjectItem(r, "body"));
      const char *img = cJSON_GetStringValue(cJSON_GetObjectItem(r, "image_url"));
      const char *card_id = cJSON_GetStringValue(cJSON_GetObjectItem(r, "card_id"));
      const char *action_label = NULL;
      const char *action_event = NULL;
      cJSON *action = cJSON_GetObjectItem(r, "action");
      if (cJSON_IsObject(action)) {
         action_label = cJSON_GetStringValue(cJSON_GetObjectItem(action, "label"));
         action_event = cJSON_GetStringValue(cJSON_GetObjectItem(action, "event"));
      }
      if (title) {
         ESP_LOGI(TAG, "widget_card: %s%s", title, (action_label && action_label[0]) ? " [+action]" : "");
         /* chat_push_card_action takes (title, subtitle, image_url,
          * description, card_id, action_label, action_event).
          * Map body → description for read-order; subtitle stays NULL.
          * Action fields gated behind all-three-present in the helper. */
         ui_chat_push_card_action(title, NULL, img, body, card_id, action_label, action_event);
      }
      return true;
   }

   if (strcmp(type_str, "widget_live") == 0 || strcmp(type_str, "widget_live_update") == 0) {
      const char *cid = cJSON_GetStringValue(cJSON_GetObjectItem(r, "card_id"));
      if (!cid) {
         ESP_LOGW(TAG, "widget_live missing card_id");
         return true;
      }
      if (strcmp(type_str, "widget_live_update") == 0) {
         const char *body = cJSON_GetStringValue(cJSON_GetObjectItem(r, "body"));
         const char *tone_s = cJSON_GetStringValue(cJSON_GetObjectItem(r, "tone"));
         cJSON *prog_j = cJSON_GetObjectItem(r, "progress");
         float progress = cJSON_IsNumber(prog_j) ? (float)prog_j->valuedouble : -1.0f;
         cJSON *act = cJSON_GetObjectItem(r, "action");
         const char *al = act ? cJSON_GetStringValue(cJSON_GetObjectItem(act, "label")) : NULL;
         const char *ae = act ? cJSON_GetStringValue(cJSON_GetObjectItem(act, "event")) : NULL;
         widget_store_update(cid, body, tone_s ? widget_tone_from_str(tone_s) : WIDGET_TONE_CALM, progress, al, ae);
         tab5_lv_async_call((lv_async_cb_t)ui_home_update_status, NULL);
      } else {
         widget_t w = {0};
         strncpy(w.card_id, cid, WIDGET_ID_LEN - 1);
         const char *sid = cJSON_GetStringValue(cJSON_GetObjectItem(r, "skill_id"));
         if (sid) strncpy(w.skill_id, sid, WIDGET_SKILL_ID_LEN - 1);
         const char *ttl = cJSON_GetStringValue(cJSON_GetObjectItem(r, "title"));
         if (ttl) strncpy(w.title, ttl, WIDGET_TITLE_LEN - 1);
         const char *bdy = cJSON_GetStringValue(cJSON_GetObjectItem(r, "body"));
         if (bdy) strncpy(w.body, bdy, WIDGET_BODY_LEN - 1);
         const char *icn = cJSON_GetStringValue(cJSON_GetObjectItem(r, "icon"));
         if (icn) strncpy(w.icon, icn, WIDGET_ICON_LEN - 1);
         const char *tone_s = cJSON_GetStringValue(cJSON_GetObjectItem(r, "tone"));
         w.tone = widget_tone_from_str(tone_s);
         cJSON *prog_j = cJSON_GetObjectItem(r, "progress");
         w.progress = cJSON_IsNumber(prog_j) ? (float)prog_j->valuedouble : 0.0f;
         cJSON *pri = cJSON_GetObjectItem(r, "priority");
         w.priority = cJSON_IsNumber(pri) ? (uint8_t)pri->valueint : 50;
         cJSON *act = cJSON_GetObjectItem(r, "action");
         if (cJSON_IsObject(act)) {
            const char *al = cJSON_GetStringValue(cJSON_GetObjectItem(act, "label"));
            const char *ae = cJSON_GetStringValue(cJSON_GetObjectItem(act, "event"));
            if (al) strncpy(w.action_label, al, WIDGET_ACTION_LBL_LEN - 1);
            if (ae) strncpy(w.action_event, ae, WIDGET_ACTION_EVT_LEN - 1);
         }
         w.type = WIDGET_TYPE_LIVE;
         widget_store_upsert(&w);
         tab5_lv_async_call((lv_async_cb_t)ui_home_update_status, NULL);
         ESP_LOGI(TAG, "widget_live upsert: %s/%s tone=%s", w.skill_id, cid, tone_s ? tone_s : "calm");
      }
      return true;
   }

   if (strcmp(type_str, "widget_list") == 0) {
      /* v4·D Phase 4c: ranked list widget.  Same upsert shape as
       * widget_live but with an "items" array carrying up to 5 rows
       * of {text, value} displayed stacked on the home live slot.
       * Skills like web_search, memory recall, daily agenda, etc.
       * should emit this instead of cramming rows into a body blob. */
      const char *cid = cJSON_GetStringValue(cJSON_GetObjectItem(r, "card_id"));
      if (!cid) {
         ESP_LOGW(TAG, "widget_list missing card_id");
         return true;
      }
      widget_t w = {0};
      strncpy(w.card_id, cid, WIDGET_ID_LEN - 1);
      const char *sid = cJSON_GetStringValue(cJSON_GetObjectItem(r, "skill_id"));
      if (sid) strncpy(w.skill_id, sid, WIDGET_SKILL_ID_LEN - 1);
      const char *ttl = cJSON_GetStringValue(cJSON_GetObjectItem(r, "title"));
      if (ttl) strncpy(w.title, ttl, WIDGET_TITLE_LEN - 1);
      const char *bdy = cJSON_GetStringValue(cJSON_GetObjectItem(r, "body"));
      if (bdy) strncpy(w.body, bdy, WIDGET_BODY_LEN - 1);
      const char *tone_s = cJSON_GetStringValue(cJSON_GetObjectItem(r, "tone"));
      w.tone = widget_tone_from_str(tone_s);
      cJSON *pri = cJSON_GetObjectItem(r, "priority");
      w.priority = cJSON_IsNumber(pri) ? (uint8_t)pri->valueint : 50;
      cJSON *items = cJSON_GetObjectItem(r, "items");
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
      w.type = WIDGET_TYPE_LIST;
      widget_store_upsert(&w);
      tab5_lv_async_call((lv_async_cb_t)ui_home_update_status, NULL);
      ESP_LOGI(TAG, "widget_list upsert: %s/%s items=%u", w.skill_id, cid, w.items_count);
      return true;
   }

   if (strcmp(type_str, "widget_chart") == 0) {
      /* v4·D Phase 4f: mini bar chart widget.  Same upsert shape as
       * widget_list; "values" array carries up to 12 floats, optional
       * "max" bound for normalization, optional "body" for a summary
       * line below the bars. */
      const char *cid = cJSON_GetStringValue(cJSON_GetObjectItem(r, "card_id"));
      if (!cid) {
         ESP_LOGW(TAG, "widget_chart missing card_id");
         return true;
      }
      widget_t w = {0};
      strncpy(w.card_id, cid, WIDGET_ID_LEN - 1);
      const char *sid = cJSON_GetStringValue(cJSON_GetObjectItem(r, "skill_id"));
      if (sid) strncpy(w.skill_id, sid, WIDGET_SKILL_ID_LEN - 1);
      const char *ttl = cJSON_GetStringValue(cJSON_GetObjectItem(r, "title"));
      if (ttl) strncpy(w.title, ttl, WIDGET_TITLE_LEN - 1);
      const char *bdy = cJSON_GetStringValue(cJSON_GetObjectItem(r, "body"));
      if (bdy) strncpy(w.body, bdy, WIDGET_BODY_LEN - 1);
      const char *tone_s = cJSON_GetStringValue(cJSON_GetObjectItem(r, "tone"));
      w.tone = widget_tone_from_str(tone_s);
      cJSON *pri = cJSON_GetObjectItem(r, "priority");
      w.priority = cJSON_IsNumber(pri) ? (uint8_t)pri->valueint : 50;
      cJSON *mx = cJSON_GetObjectItem(r, "max");
      w.chart_max = cJSON_IsNumber(mx) ? (float)mx->valuedouble : 0.0f;
      cJSON *vals = cJSON_GetObjectItem(r, "values");
      if (cJSON_IsArray(vals)) {
         int cnt = cJSON_GetArraySize(vals);
         if (cnt > WIDGET_CHART_MAX_POINTS) cnt = WIDGET_CHART_MAX_POINTS;
         for (int i = 0; i < cnt; i++) {
            cJSON *v = cJSON_GetArrayItem(vals, i);
            w.chart_values[i] = cJSON_IsNumber(v) ? (float)v->valuedouble : 0.0f;
         }
         w.chart_count = (uint8_t)cnt;
      }
      w.type = WIDGET_TYPE_CHART;
      widget_store_upsert(&w);
      tab5_lv_async_call((lv_async_cb_t)ui_home_update_status, NULL);
      ESP_LOGI(TAG, "widget_chart upsert: %s/%s pts=%u max=%.2f", w.skill_id, cid, w.chart_count, w.chart_max);
      return true;
   }

   if (strcmp(type_str, "widget_media") == 0) {
      /* v4·D Phase 4g: media widget (image + caption in the live slot). */
      const char *cid = cJSON_GetStringValue(cJSON_GetObjectItem(r, "card_id"));
      if (!cid) {
         ESP_LOGW(TAG, "widget_media missing card_id");
         return true;
      }
      widget_t w = {0};
      strncpy(w.card_id, cid, WIDGET_ID_LEN - 1);
      const char *sid = cJSON_GetStringValue(cJSON_GetObjectItem(r, "skill_id"));
      if (sid) strncpy(w.skill_id, sid, WIDGET_SKILL_ID_LEN - 1);
      const char *ttl = cJSON_GetStringValue(cJSON_GetObjectItem(r, "title"));
      if (ttl) strncpy(w.title, ttl, WIDGET_TITLE_LEN - 1);
      const char *bdy = cJSON_GetStringValue(cJSON_GetObjectItem(r, "body"));
      if (bdy) strncpy(w.body, bdy, WIDGET_BODY_LEN - 1);
      const char *url = cJSON_GetStringValue(cJSON_GetObjectItem(r, "url"));
      if (url) strncpy(w.media_url, url, WIDGET_MEDIA_URL_LEN - 1);
      const char *alt = cJSON_GetStringValue(cJSON_GetObjectItem(r, "alt"));
      if (alt) strncpy(w.media_alt, alt, WIDGET_MEDIA_ALT_LEN - 1);
      const char *tone_s = cJSON_GetStringValue(cJSON_GetObjectItem(r, "tone"));
      w.tone = widget_tone_from_str(tone_s);
      cJSON *pri = cJSON_GetObjectItem(r, "priority");
      w.priority = cJSON_IsNumber(pri) ? (uint8_t)pri->valueint : 50;
      w.type = WIDGET_TYPE_MEDIA;
      widget_store_upsert(&w);
      tab5_lv_async_call((lv_async_cb_t)ui_home_update_status, NULL);
      ESP_LOGI(TAG, "widget_media upsert: %s/%s alt=%s", w.skill_id, cid, w.media_alt);
      return true;
   }

   if (strcmp(type_str, "widget_prompt") == 0) {
      /* v4·D Phase 4g: multi-choice prompt widget.  Up to 3 choices;
       * Tab5 renders each as a button.  Tap fires widget_action. */
      const char *cid = cJSON_GetStringValue(cJSON_GetObjectItem(r, "card_id"));
      if (!cid) {
         ESP_LOGW(TAG, "widget_prompt missing card_id");
         return true;
      }
      widget_t w = {0};
      strncpy(w.card_id, cid, WIDGET_ID_LEN - 1);
      const char *sid = cJSON_GetStringValue(cJSON_GetObjectItem(r, "skill_id"));
      if (sid) strncpy(w.skill_id, sid, WIDGET_SKILL_ID_LEN - 1);
      const char *ttl = cJSON_GetStringValue(cJSON_GetObjectItem(r, "title"));
      if (ttl) strncpy(w.title, ttl, WIDGET_TITLE_LEN - 1);
      const char *bdy = cJSON_GetStringValue(cJSON_GetObjectItem(r, "body"));
      if (bdy) strncpy(w.body, bdy, WIDGET_BODY_LEN - 1);
      const char *tone_s = cJSON_GetStringValue(cJSON_GetObjectItem(r, "tone"));
      w.tone = widget_tone_from_str(tone_s);
      cJSON *pri = cJSON_GetObjectItem(r, "priority");
      w.priority = cJSON_IsNumber(pri) ? (uint8_t)pri->valueint : 60;
      cJSON *choices = cJSON_GetObjectItem(r, "choices");
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
      w.type = WIDGET_TYPE_PROMPT;
      widget_store_upsert(&w);
      tab5_lv_async_call((lv_async_cb_t)ui_home_update_status, NULL);
      ESP_LOGI(TAG, "widget_prompt upsert: %s/%s choices=%u", w.skill_id, cid, w.choices_count);
      return true;
   }

   if (strcmp(type_str, "widget_live_dismiss") == 0 || strcmp(type_str, "widget_dismiss") == 0) {
      const char *cid = cJSON_GetStringValue(cJSON_GetObjectItem(r, "card_id"));
      if (cid) {
         widget_store_dismiss(cid);
         tab5_lv_async_call((lv_async_cb_t)ui_home_update_status, NULL);
         ESP_LOGI(TAG, "widget_live_dismiss: %s", cid);
      }
      return true;
   }

   /* type_str started with "widget_" but didn't match a known verb.
    * Returning false lets the caller log the unknown verb in its
    * existing fallthrough branch. */
   return false;
}
