/**
 * Voice five-tier mode dispatcher (Tab5) — implementation.
 *
 * Wave 23 SOLID-audit closure for TT #331 (extract A2).  Pre-extract
 * the routing decision was inline at voice.c:voice_send_text in the
 * five-tier branch ladder; the config_update senders lived alongside
 * voice_send_text.
 */
#include "voice_modes.h"

#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "settings.h"
#include "ui_home.h" /* ui_home_show_toast for chain-busy refusal */
#include "voice.h"
#include "voice_m5_llm.h"  /* M5_FAILOVER_GRACE_MS (the WS-down grace window) */
#include "voice_onboard.h" /* K144 chain + send_text + failover state */
#include "voice_ws_proto.h"

static const char *TAG = "tab5_voice_modes";

/* TT #331 Wave 23 SRP-A2: ownership of s_voice_mode lives here.
 * voice.c calls voice_modes_set_internal from the listening / dictation /
 * call lifecycle entry points; readers go through voice_get_mode(). */
static voice_mode_t s_voice_mode = VOICE_MODE_ASK;

voice_mode_t voice_get_mode(void) { return s_voice_mode; }

void voice_modes_set_internal(voice_mode_t mode) { s_voice_mode = mode; }

esp_err_t voice_send_config_update_ex(int voice_mode, const char *llm_model, const char *reason) {
   if (!g_voice_ws || !esp_websocket_client_is_connected(g_voice_ws)) return ESP_ERR_INVALID_STATE;

   cJSON *msg = cJSON_CreateObject();
   cJSON_AddStringToObject(msg, "type", "config_update");
   cJSON_AddNumberToObject(msg, "voice_mode", voice_mode);
   cJSON_AddBoolToObject(msg, "cloud_mode", voice_mode >= 1);
   if (voice_mode == 2 && llm_model && llm_model[0]) {
      cJSON_AddStringToObject(msg, "llm_model", llm_model);
   }
   if (reason && reason[0]) {
      cJSON_AddStringToObject(msg, "reason", reason);
   }
   char *json = cJSON_PrintUnformatted(msg);
   cJSON_Delete(msg);
   if (!json) return ESP_ERR_NO_MEM;

   ESP_LOGI(TAG, "Sending config_update: voice_mode=%d llm_model=%s reason=%s", voice_mode,
            (llm_model && llm_model[0]) ? llm_model : "(local)", (reason && reason[0]) ? reason : "(none)");
   esp_err_t ret = voice_ws_send_text(json);
   cJSON_free(json);
   return ret;
}

esp_err_t voice_send_config_update(int voice_mode, const char *llm_model) {
   return voice_send_config_update_ex(voice_mode, llm_model, NULL);
}

void voice_modes_route_text(const char *text, voice_modes_route_result_t *out) {
   if (!out) return;
   out->kind = VOICE_MODES_ROUTE_DRAGON_PATH;
   out->err = ESP_OK;

   /* TT #317 Phase 5: VMODE_LOCAL_ONBOARD always routes to K144 regardless
    * of Dragon WS state.  voice_onboard.c owns the actual chain transport. */
   if (tab5_settings_get_voice_mode() == VMODE_LOCAL_ONBOARD) {
      if (voice_onboard_chain_active()) {
         ESP_LOGI(TAG, "VMODE_LOCAL_ONBOARD: chain active, refusing text turn");
         out->kind = VOICE_MODES_ROUTE_K144_CHAIN_BUSY;
         out->err = ESP_ERR_INVALID_STATE;
         return;
      }
      esp_err_t fe = voice_onboard_send_text(text);
      if (fe == ESP_OK) {
         ESP_LOGI(TAG, "VMODE_LOCAL_ONBOARD — routed to K144");
         out->kind = VOICE_MODES_ROUTE_K144_OK;
      } else {
         ESP_LOGW(TAG, "VMODE_LOCAL_ONBOARD but K144 unavailable (%s) — refusing send", esp_err_to_name(fe));
         out->kind = VOICE_MODES_ROUTE_K144_FAILED;
         out->err = fe;
      }
      return;
   }

   /* TT #317 Phase 4: WS unreachable in Local mode → try K144 failover after
    * the M5_FAILOVER_GRACE_MS grace window. */
   uint32_t down_ms = 0;
   if (s_ws_last_alive_us) {
      down_ms = (uint32_t)((esp_timer_get_time() - s_ws_last_alive_us) / 1000);
   }
   if (down_ms >= M5_FAILOVER_GRACE_MS && tab5_settings_get_voice_mode() == VMODE_LOCAL) {
      esp_err_t fe = voice_onboard_send_text(text);
      if (fe == ESP_OK) {
         ESP_LOGI(TAG, "Local-mode failover engaged — routed to K144 (down=%ums)", (unsigned)down_ms);
         out->kind = VOICE_MODES_ROUTE_K144_OK;
         return;
      }
      /* K144 also unreachable — fall through to DRAGON_PATH so the
       * existing voice_ws_send_text error pathway can surface the
       * connectivity error to the user uniformly. */
      ESP_LOGW(TAG, "Local-mode failover attempted but K144 errored: %s", esp_err_to_name(fe));
   }

   /* TT #370 — vmode=5 SOLO_DIRECT.  Tab5 ↔ OpenRouter direct.  Real
    * dispatch to voice_solo_send_text lands in a follow-up commit;
    * for now we gate on or_key + fall through to DRAGON_PATH so a
    * misconfigured device still has a working error pathway via the
    * existing WS error UX.  The NO_KEY return short-circuits before
    * any send so the caller can prompt for QR scan. */
   if (tab5_settings_get_voice_mode() == VMODE_SOLO_DIRECT) {
      char or_key[96] = {0};
      tab5_settings_get_or_key(or_key, sizeof or_key);
      if (or_key[0] == '\0') {
         ESP_LOGW(TAG, "VMODE_SOLO_DIRECT but or_key empty — refusing send");
         out->kind = VOICE_MODES_ROUTE_SOLO_NO_KEY;
         out->err = ESP_ERR_INVALID_STATE;
         return;
      }
      ESP_LOGW(TAG, "VMODE_SOLO_DIRECT: voice_solo not wired yet, falling through to Dragon");
      /* fall through */
   }

   /* Default — Dragon WS path. */
   out->kind = VOICE_MODES_ROUTE_DRAGON_PATH;
}
