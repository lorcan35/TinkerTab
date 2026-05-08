/*
 * debug_server_settings.c — /settings (GET + POST) and /nvs/erase.
 *
 * Wave 23b follow-up (#332): seventh per-family extract.  Three
 * handlers + their factory-reset confirm guard moved verbatim from
 * debug_server.c.  Only call-site changes are the standard
 * `check_auth` → `tab5_debug_check_auth`, `send_json_resp` →
 * `tab5_debug_send_json_resp`, and the auth-token-prefix compare
 * which now goes through `tab5_debug_check_factory_reset_confirm`
 * (added to debug_server_internal.h alongside this extract so the
 * family doesn't need access to the file-static `s_auth_token`).
 */

#include "debug_server_settings.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "debug_obs.h"
#include "debug_server_internal.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mode_manager.h" /* tab5_mode_resolve */
#include "nvs_flash.h"
#include "settings.h" /* tab5_settings_*, tab5_budget_* */
#include "ui_core.h"  /* ui_core_apply_auto_rotation (extern-declared at call site) */
#include "voice.h"    /* voice_is_connected, voice_get_state, voice_reapply_connection_mode */

static const char *TAG = "debug_settings";

/* GET /settings — dump all NVS settings as JSON */
static esp_err_t settings_get_handler(httpd_req_t *req) {
   if (!tab5_debug_check_auth(req)) return ESP_OK;

   /* #148: full NVS key coverage — was exposing only 10 of ~25 keys.
    * wifi_pass + auth_tok are intentionally omitted (secrets).  Runtime
    * state (voice_connected, voice_state) stays for backward compat. */
   cJSON *root = cJSON_CreateObject();
   char buf[96];

   /* Network */
   tab5_settings_get_wifi_ssid(buf, sizeof(buf));
   cJSON_AddStringToObject(root, "wifi_ssid", buf);
   tab5_settings_get_dragon_host(buf, sizeof(buf));
   cJSON_AddStringToObject(root, "dragon_host", buf);
   cJSON_AddNumberToObject(root, "dragon_port", tab5_settings_get_dragon_port());
   cJSON_AddNumberToObject(root, "conn_mode", tab5_settings_get_connection_mode());
   /* TT #328 Wave 8 — surface dragon_api_token presence (NOT the
    * value, since /settings is bearer-auth on the Tab5 debug server
    * itself but the token is sensitive Dragon-side credential).  The
    * caller can verify the token is set without exposing it.  POST
    * setters accept a "dragon_api_token" key to write. */
   {
      /* 96-byte buffer accommodates 64-char tokens + null + headroom
       * for future-format tokens (UUIDs, JWT prefixes).  The 64-char
       * cap on the writer is the meaningful limit, not the reader. */
      char tok[96] = {0};
      tab5_settings_get_dragon_api_token(tok, sizeof(tok));
      cJSON_AddBoolToObject(root, "dragon_api_token_set", tok[0] != '\0');
      cJSON_AddNumberToObject(root, "dragon_api_token_len", (double)strlen(tok));
   }
   /* TT #328 Wave 11 — starred-skills list.  Tool names are public
    * via /api/v1/tools so exposing the list is fine + lets the
    * harness verify the toggle round-trip via /settings GET. */
   {
      char stars[256] = {0};
      tab5_settings_get_starred_skills(stars, sizeof(stars));
      cJSON_AddStringToObject(root, "starred_skills", stars);
   }

   /* TT #370 — OpenRouter (vmode=5 SOLO_DIRECT).  The key is
    * surfaced as `or_key_set` + `or_key_len` (same shape as
    * dragon_api_token above) so the harness + UI can verify it's
    * provisioned without exposing the secret.  Model + voice fields
    * are non-sensitive and surfaced directly. */
   {
      char or_key[96] = {0};
      tab5_settings_get_or_key(or_key, sizeof(or_key));
      cJSON_AddBoolToObject(root, "or_key_set", or_key[0] != '\0');
      cJSON_AddNumberToObject(root, "or_key_len", (double)strlen(or_key));

      char or_buf[96] = {0};
      tab5_settings_get_or_mdl_llm(or_buf, sizeof(or_buf));
      cJSON_AddStringToObject(root, "or_mdl_llm", or_buf);
      tab5_settings_get_or_mdl_stt(or_buf, sizeof(or_buf));
      cJSON_AddStringToObject(root, "or_mdl_stt", or_buf);
      tab5_settings_get_or_mdl_tts(or_buf, sizeof(or_buf));
      cJSON_AddStringToObject(root, "or_mdl_tts", or_buf);
      tab5_settings_get_or_mdl_emb(or_buf, sizeof(or_buf));
      cJSON_AddStringToObject(root, "or_mdl_emb", or_buf);

      char or_voice[32] = {0};
      tab5_settings_get_or_voice(or_voice, sizeof(or_voice));
      cJSON_AddStringToObject(root, "or_voice", or_voice);
   }

   /* Hardware */
   cJSON_AddNumberToObject(root, "brightness", tab5_settings_get_brightness());
   cJSON_AddNumberToObject(root, "volume", tab5_settings_get_volume());
   cJSON_AddNumberToObject(root, "mic_mute", tab5_settings_get_mic_mute());

   /* Identity */
   if (tab5_settings_get_device_id(buf, sizeof(buf)) == ESP_OK) {
      cJSON_AddStringToObject(root, "device_id", buf);
   }
   if (tab5_settings_get_hardware_id(buf, sizeof(buf)) == ESP_OK) {
      cJSON_AddStringToObject(root, "hardware_id", buf);
   }
   if (tab5_settings_get_session_id(buf, sizeof(buf)) == ESP_OK) {
      cJSON_AddStringToObject(root, "session_id", buf);
   }

   /* Voice / AI */
   cJSON_AddNumberToObject(root, "voice_mode", tab5_settings_get_voice_mode());
   char model[64];
   tab5_settings_get_llm_model(model, sizeof(model));
   cJSON_AddStringToObject(root, "llm_model", model);
   cJSON_AddNumberToObject(root, "int_tier", tab5_settings_get_int_tier());
   cJSON_AddNumberToObject(root, "voi_tier", tab5_settings_get_voi_tier());
   cJSON_AddNumberToObject(root, "aut_tier", tab5_settings_get_aut_tier());
   cJSON_AddBoolToObject(root, "onboarded", tab5_settings_is_onboarded());

   /* Quiet hours */
   cJSON_AddNumberToObject(root, "quiet_on", tab5_settings_get_quiet_on());
   cJSON_AddNumberToObject(root, "quiet_start", tab5_settings_get_quiet_start());
   cJSON_AddNumberToObject(root, "quiet_end", tab5_settings_get_quiet_end());

   /* Audit U2 (#206): auto-rotate persisted preference. */
   cJSON_AddNumberToObject(root, "auto_rot", tab5_settings_get_auto_rotate());
   /* #260: camera rotation persisted preference. */
   cJSON_AddNumberToObject(root, "cam_rot", tab5_settings_get_cam_rotation());

   /* Budget */
   cJSON_AddNumberToObject(root, "spent_mils", (double)tab5_budget_get_today_mils());
   cJSON_AddNumberToObject(root, "cap_mils", (double)tab5_budget_get_cap_mils());

   /* Runtime state (handy for "dump everything once" callers) */
   cJSON_AddBoolToObject(root, "voice_connected", voice_is_connected());
   cJSON_AddNumberToObject(root, "voice_state", voice_get_state());

   char *json = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   httpd_resp_set_type(req, "application/json");
   httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
   httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Authorization");
   esp_err_t ret = httpd_resp_sendstr(req, json);
   free(json);
   return ret;
}

/* POST /settings — update NVS keys via JSON body.
 * Accepts any combination of: wifi_ssid, wifi_pass, dragon_host, dragon_port,
 * conn_mode, brightness, volume, mic_mute, quiet_on, quiet_start, quiet_end,
 * voice_mode, llm_model, session_id, int_tier, voi_tier, aut_tier, cap_mils,
 * reset_spent (bool).
 *
 * Returns: {"updated":[...], "skipped":[...], "resolved_voice_mode":N?, ...}
 * Note: does NOT reconnect voice WS automatically — call /voice/reconnect after.
 */
static esp_err_t settings_set_handler(httpd_req_t *req) {
   if (!tab5_debug_check_auth(req)) return ESP_OK;

   /* #148: body cap bumped 512 → 2048 to fit a full key dump.
    * Heap-allocated via PSRAM so the httpd task stack doesn't balloon. */
   const size_t MAX_BODY = 2048;
   int total = req->content_len;
   if (total <= 0 || total > (int)MAX_BODY) {
      httpd_resp_set_type(req, "application/json");
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "{\"error\":\"body 1..2048 bytes required\"}");
      return ESP_OK;
   }

   char *body = heap_caps_malloc(total + 1, MALLOC_CAP_SPIRAM);
   if (!body) {
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "{\"error\":\"oom\"}");
      return ESP_OK;
   }
   int received = 0;
   while (received < total) {
      int r = httpd_req_recv(req, body + received, total - received);
      if (r <= 0) {
         heap_caps_free(body);
         httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "{\"error\":\"recv failed\"}");
         return ESP_OK;
      }
      received += r;
   }
   body[received] = '\0';

   cJSON *req_json = cJSON_Parse(body);
   heap_caps_free(body);
   if (!req_json) {
      httpd_resp_set_type(req, "application/json");
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "{\"error\":\"invalid JSON\"}");
      return ESP_OK;
   }

   cJSON *resp = cJSON_CreateObject();
   cJSON *updated = cJSON_CreateArray();

   /* ── Network ────────────────────────────────────────────────── */
   cJSON *ssid = cJSON_GetObjectItem(req_json, "wifi_ssid");
   if (cJSON_IsString(ssid) && ssid->valuestring && strlen(ssid->valuestring) > 0) {
      if (tab5_settings_set_wifi_ssid(ssid->valuestring) == ESP_OK) {
         cJSON_AddItemToArray(updated, cJSON_CreateString("wifi_ssid"));
      }
   }
   cJSON *pass = cJSON_GetObjectItem(req_json, "wifi_pass");
   if (cJSON_IsString(pass) && pass->valuestring) {
      if (tab5_settings_set_wifi_pass(pass->valuestring) == ESP_OK) {
         cJSON_AddItemToArray(updated, cJSON_CreateString("wifi_pass"));
      }
   }
   cJSON *host = cJSON_GetObjectItem(req_json, "dragon_host");
   if (cJSON_IsString(host) && host->valuestring && strlen(host->valuestring) > 0) {
      if (tab5_settings_set_dragon_host(host->valuestring) == ESP_OK) {
         cJSON_AddItemToArray(updated, cJSON_CreateString("dragon_host"));
      }
   }

   cJSON *port = cJSON_GetObjectItem(req_json, "dragon_port");
   if (cJSON_IsNumber(port)) {
      int p = (int)port->valuedouble;
      if (p > 0 && p < 65536) {
         if (tab5_settings_set_dragon_port((uint16_t)p) == ESP_OK) {
            cJSON_AddItemToArray(updated, cJSON_CreateString("dragon_port"));
         }
      }
   }

   /* TT #328 Wave 8 — Dragon REST API bearer token.  Empty string is
    * valid (clears the token).  Length cap 64 is conservative for
    * common secret formats (UUIDs, base64, hex). */
   cJSON *tok = cJSON_GetObjectItem(req_json, "dragon_api_token");
   if (cJSON_IsString(tok)) {
      if (strlen(tok->valuestring) <= 64 && tab5_settings_set_dragon_api_token(tok->valuestring) == ESP_OK) {
         cJSON_AddItemToArray(updated, cJSON_CreateString("dragon_api_token"));
      }
   }

   /* TT #328 Wave 11 — starred-skills list (comma-separated tool
    * names).  Lets the test harness seed/clear the list; users
    * normally toggle individual entries via tap on a skill card.
    * Empty string clears.  Cap 240 chars (the NVS string write
    * path is fine up to ~4 KB but 240 keeps the JSON tidy). */
   cJSON *stars_in = cJSON_GetObjectItem(req_json, "starred_skills");
   if (cJSON_IsString(stars_in)) {
      if (strlen(stars_in->valuestring) <= 240 && tab5_settings_set_starred_skills(stars_in->valuestring) == ESP_OK) {
         cJSON_AddItemToArray(updated, cJSON_CreateString("starred_skills"));
      }
   }

   /* TT #370 — OpenRouter (vmode=5 SOLO_DIRECT).  Six string keys.
    * `or_key` is the only one with a tight length cap (80 chars
    * accommodates the 73-char sk-or-v1- format + headroom).  Models
    * accept 95 chars (handles the longest published OpenRouter model
    * IDs with `~latest` aliases).  `or_voice` is a short preset name. */
   {
      cJSON *or_key = cJSON_GetObjectItem(req_json, "or_key");
      if (cJSON_IsString(or_key) && strlen(or_key->valuestring) <= 80 &&
          tab5_settings_set_or_key(or_key->valuestring) == ESP_OK) {
         cJSON_AddItemToArray(updated, cJSON_CreateString("or_key"));
      }
      cJSON *or_llm = cJSON_GetObjectItem(req_json, "or_mdl_llm");
      if (cJSON_IsString(or_llm) && strlen(or_llm->valuestring) <= 95 &&
          tab5_settings_set_or_mdl_llm(or_llm->valuestring) == ESP_OK) {
         cJSON_AddItemToArray(updated, cJSON_CreateString("or_mdl_llm"));
      }
      cJSON *or_stt = cJSON_GetObjectItem(req_json, "or_mdl_stt");
      if (cJSON_IsString(or_stt) && strlen(or_stt->valuestring) <= 95 &&
          tab5_settings_set_or_mdl_stt(or_stt->valuestring) == ESP_OK) {
         cJSON_AddItemToArray(updated, cJSON_CreateString("or_mdl_stt"));
      }
      cJSON *or_tts = cJSON_GetObjectItem(req_json, "or_mdl_tts");
      if (cJSON_IsString(or_tts) && strlen(or_tts->valuestring) <= 95 &&
          tab5_settings_set_or_mdl_tts(or_tts->valuestring) == ESP_OK) {
         cJSON_AddItemToArray(updated, cJSON_CreateString("or_mdl_tts"));
      }
      cJSON *or_emb = cJSON_GetObjectItem(req_json, "or_mdl_emb");
      if (cJSON_IsString(or_emb) && strlen(or_emb->valuestring) <= 95 &&
          tab5_settings_set_or_mdl_emb(or_emb->valuestring) == ESP_OK) {
         cJSON_AddItemToArray(updated, cJSON_CreateString("or_mdl_emb"));
      }
      cJSON *or_voice_in = cJSON_GetObjectItem(req_json, "or_voice");
      if (cJSON_IsString(or_voice_in) && strlen(or_voice_in->valuestring) <= 31 &&
          tab5_settings_set_or_voice(or_voice_in->valuestring) == ESP_OK) {
         cJSON_AddItemToArray(updated, cJSON_CreateString("or_voice"));
      }
   }

   /* ── Hardware ───────────────────────────────────────────────── */
   cJSON *br = cJSON_GetObjectItem(req_json, "brightness");
   if (cJSON_IsNumber(br)) {
      int v = (int)br->valuedouble;
      if (v >= 0 && v <= 100 && tab5_settings_set_brightness((uint8_t)v) == ESP_OK) {
         cJSON_AddItemToArray(updated, cJSON_CreateString("brightness"));
      }
   }
   cJSON *vol = cJSON_GetObjectItem(req_json, "volume");
   if (cJSON_IsNumber(vol)) {
      int v = (int)vol->valuedouble;
      if (v >= 0 && v <= 100 && tab5_settings_set_volume((uint8_t)v) == ESP_OK) {
         cJSON_AddItemToArray(updated, cJSON_CreateString("volume"));
      }
   }
   cJSON *mic = cJSON_GetObjectItem(req_json, "mic_mute");
   if (cJSON_IsNumber(mic) || cJSON_IsBool(mic)) {
      int v = cJSON_IsBool(mic) ? cJSON_IsTrue(mic) : (int)mic->valuedouble;
      if (tab5_settings_set_mic_mute(v ? 1 : 0) == ESP_OK) {
         cJSON_AddItemToArray(updated, cJSON_CreateString("mic_mute"));
      }
   }

   /* ── Quiet hours ────────────────────────────────────────────── */
   cJSON *qon = cJSON_GetObjectItem(req_json, "quiet_on");
   if (cJSON_IsNumber(qon) || cJSON_IsBool(qon)) {
      int v = cJSON_IsBool(qon) ? cJSON_IsTrue(qon) : (int)qon->valuedouble;
      if (tab5_settings_set_quiet_on(v ? 1 : 0) == ESP_OK) {
         cJSON_AddItemToArray(updated, cJSON_CreateString("quiet_on"));
      }
   }
   cJSON *qs = cJSON_GetObjectItem(req_json, "quiet_start");
   if (cJSON_IsNumber(qs)) {
      int v = (int)qs->valuedouble;
      if (v >= 0 && v <= 23 && tab5_settings_set_quiet_start((uint8_t)v) == ESP_OK) {
         cJSON_AddItemToArray(updated, cJSON_CreateString("quiet_start"));
      }
   }
   cJSON *qe = cJSON_GetObjectItem(req_json, "quiet_end");
   if (cJSON_IsNumber(qe)) {
      int v = (int)qe->valuedouble;
      if (v >= 0 && v <= 23 && tab5_settings_set_quiet_end((uint8_t)v) == ESP_OK) {
         cJSON_AddItemToArray(updated, cJSON_CreateString("quiet_end"));
      }
   }

   /* Audit U2 (#206): auto-rotate POST setter for testability — also
    * triggers ui_core_apply_auto_rotation so the new value takes
    * effect immediately. */
   cJSON *ar = cJSON_GetObjectItem(req_json, "auto_rot");
   if (cJSON_IsNumber(ar) || cJSON_IsBool(ar)) {
      int v = cJSON_IsBool(ar) ? cJSON_IsTrue(ar) : (int)ar->valuedouble;
      if (tab5_settings_set_auto_rotate(v ? 1 : 0) == ESP_OK) {
         extern void ui_core_apply_auto_rotation(bool enabled);
         ui_core_apply_auto_rotation(v != 0);
         cJSON_AddItemToArray(updated, cJSON_CreateString("auto_rot"));
      }
   }

   /* #260: camera rotation POST setter for testability. */
   cJSON *cr = cJSON_GetObjectItem(req_json, "cam_rot");
   if (cJSON_IsNumber(cr)) {
      int v = (int)cr->valuedouble;
      if (v >= 0 && v <= 3 && tab5_settings_set_cam_rotation((uint8_t)v) == ESP_OK) {
         cJSON_AddItemToArray(updated, cJSON_CreateString("cam_rot"));
      }
   }

   /* ── Voice / AI ─────────────────────────────────────────────── */
   cJSON *vm = cJSON_GetObjectItem(req_json, "voice_mode");
   if (cJSON_IsNumber(vm)) {
      int v = (int)vm->valuedouble;
      /* TT #317 Phase 5: vmode=4 added for VMODE_LOCAL_ONBOARD (K144). */
      if (v >= 0 && v <= 4 && tab5_settings_set_voice_mode((uint8_t)v) == ESP_OK) {
         cJSON_AddItemToArray(updated, cJSON_CreateString("voice_mode"));
      }
   }
   cJSON *lm = cJSON_GetObjectItem(req_json, "llm_model");
   if (cJSON_IsString(lm) && lm->valuestring) {
      if (tab5_settings_set_llm_model(lm->valuestring) == ESP_OK) {
         cJSON_AddItemToArray(updated, cJSON_CreateString("llm_model"));
      }
   }
   cJSON *sid = cJSON_GetObjectItem(req_json, "session_id");
   if (cJSON_IsString(sid) && sid->valuestring) {
      if (tab5_settings_set_session_id(sid->valuestring) == ESP_OK) {
         cJSON_AddItemToArray(updated, cJSON_CreateString("session_id"));
      }
   }

   cJSON *cmode = cJSON_GetObjectItem(req_json, "conn_mode");
   if (cJSON_IsNumber(cmode)) {
      int m = (int)cmode->valuedouble;
      if (m >= 0 && m <= 2) {
         if (tab5_settings_set_connection_mode((uint8_t)m) == ESP_OK) {
            cJSON_AddItemToArray(updated, cJSON_CreateString("conn_mode"));
            /* U21 (#206): re-target the live WS URI so a remote
             * change to "Internet Only"/"Local Only" lands without
             * waiting for the user to power-cycle. */
            extern void voice_reapply_connection_mode(void);
            voice_reapply_connection_mode();
         }
      }
   }

   /* v4·D Sovereign Halo mode dials — accept tiers and auto-resolve into
    * voice_mode + llm_model. Calling /voice/reconnect after a tier
    * change is optional but recommended -- Dragon picks up the new mode
    * on the next config_update that follows. */
   bool any_tier = false;
   cJSON *it = cJSON_GetObjectItem(req_json, "int_tier");
   if (cJSON_IsNumber(it)) {
      int t = (int)it->valuedouble;
      if (t >= 0 && t <= 2 && tab5_settings_set_int_tier((uint8_t)t) == ESP_OK) {
         cJSON_AddItemToArray(updated, cJSON_CreateString("int_tier"));
         any_tier = true;
      }
   }
   cJSON *vt = cJSON_GetObjectItem(req_json, "voi_tier");
   if (cJSON_IsNumber(vt)) {
      int t = (int)vt->valuedouble;
      if (t >= 0 && t <= 2 && tab5_settings_set_voi_tier((uint8_t)t) == ESP_OK) {
         cJSON_AddItemToArray(updated, cJSON_CreateString("voi_tier"));
         any_tier = true;
      }
   }
   cJSON *at = cJSON_GetObjectItem(req_json, "aut_tier");
   if (cJSON_IsNumber(at)) {
      int t = (int)at->valuedouble;
      if (t >= 0 && t <= 1 && tab5_settings_set_aut_tier((uint8_t)t) == ESP_OK) {
         cJSON_AddItemToArray(updated, cJSON_CreateString("aut_tier"));
         any_tier = true;
      }
   }

   /* Phase 3e budget cap edit + spent reset (dev/debug knob) */
   cJSON *cap = cJSON_GetObjectItem(req_json, "cap_mils");
   if (cJSON_IsNumber(cap)) {
      uint32_t v = (uint32_t)cap->valuedouble;
      if (tab5_budget_set_cap_mils(v) == ESP_OK) {
         cJSON_AddItemToArray(updated, cJSON_CreateString("cap_mils"));
      }
   }
   cJSON *reset = cJSON_GetObjectItem(req_json, "reset_spent");
   if (cJSON_IsBool(reset) && cJSON_IsTrue(reset)) {
      /* #148: actually zero today's spend via the new helper. */
      if (tab5_budget_reset_spent() == ESP_OK) {
         cJSON_AddItemToArray(updated, cJSON_CreateString("reset_spent"));
      }
   }
   if (any_tier) {
      /* Resolve the new tier triple and persist the derived voice_mode +
       * llm_model. Leaves llm_model untouched when resolver doesn't pick
       * a specific cloud model (ie Local, Hybrid, TinkerClaw modes). */
      char model_out[64] = {0};
      uint8_t new_mode = tab5_mode_resolve(tab5_settings_get_int_tier(), tab5_settings_get_voi_tier(),
                                           tab5_settings_get_aut_tier(), model_out, sizeof(model_out));
      tab5_settings_set_voice_mode(new_mode);
      cJSON_AddItemToArray(updated, cJSON_CreateString("voice_mode"));
      cJSON_AddNumberToObject(resp, "resolved_voice_mode", new_mode);
      if (model_out[0]) {
         tab5_settings_set_llm_model(model_out);
         cJSON_AddItemToArray(updated, cJSON_CreateString("llm_model"));
         cJSON_AddStringToObject(resp, "resolved_llm_model", model_out);
      }
   }

   cJSON_AddItemToObject(resp, "updated", updated);
   char *out = cJSON_PrintUnformatted(resp);
   cJSON_Delete(resp);
   cJSON_Delete(req_json);

   httpd_resp_set_type(req, "application/json");
   httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
   httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Authorization");
   esp_err_t ret = httpd_resp_sendstr(req, out);
   free(out);
   return ret;
}

/* ── POST /nvs/erase?confirm=<token> ────────────────────────────── */
/* Factory reset path.  Requires ?confirm=<first 8 chars of auth_tok>
 * as a guard against accidental triggering.  After erase, reboots. */
static esp_err_t nvs_erase_handler(httpd_req_t *req) {
   if (!tab5_debug_check_auth(req)) return ESP_OK;
   char q[64] = {0}, confirm[16] = {0};
   httpd_req_get_url_query_str(req, q, sizeof(q));
   httpd_query_key_value(q, "confirm", confirm, sizeof(confirm));

   if (!tab5_debug_check_factory_reset_confirm(confirm)) {
      cJSON *r = cJSON_CreateObject();
      cJSON_AddStringToObject(r, "error", "factory reset requires ?confirm=<first-8-chars-of-auth-token>");
      return tab5_debug_send_json_resp(req, r);
   }
   ESP_LOGW(TAG, "NVS erase requested via debug — rebooting in 500 ms");
   tab5_debug_obs_event("nvs", "erase");
   cJSON *r = cJSON_CreateObject();
   cJSON_AddBoolToObject(r, "ok", true);
   cJSON_AddStringToObject(r, "note", "erasing + rebooting");
   tab5_debug_send_json_resp(req, r);
   vTaskDelay(pdMS_TO_TICKS(300));
   nvs_flash_erase();
   vTaskDelay(pdMS_TO_TICKS(200));
   esp_restart();
   return ESP_OK; /* unreachable */
}

void debug_server_settings_register(httpd_handle_t server) {
   const httpd_uri_t uri_settings_get = {.uri = "/settings", .method = HTTP_GET, .handler = settings_get_handler};
   const httpd_uri_t uri_settings_set = {.uri = "/settings", .method = HTTP_POST, .handler = settings_set_handler};
   const httpd_uri_t uri_nvs_erase = {.uri = "/nvs/erase", .method = HTTP_POST, .handler = nvs_erase_handler};
   httpd_register_uri_handler(server, &uri_settings_get);
   httpd_register_uri_handler(server, &uri_settings_set);
   httpd_register_uri_handler(server, &uri_nvs_erase);
   ESP_LOGI(TAG, "Settings endpoint family registered (3 URIs)");
}
