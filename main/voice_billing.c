/*
 * voice_billing.c — receipt + budget + cap-downgrade implementation.
 *
 * 2026-05-03 SOLID audit SRP-3 extract from voice.c.  Verbatim move
 * of the receipt-verb branch + the deferred-attach machinery; only
 * call-site changes are header includes replacing the prior extern
 * decls inside voice.c.
 */

#include "voice_billing.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chat_msg_store.h" /* chat_store_attach_receipt_ex */
#include "esp_log.h"
#include "settings.h" /* tab5_settings_*, tab5_budget_* */
#include "ui_chat.h"  /* ui_chat_refresh_receipts, ui_chat_refresh_spend */
#include "ui_core.h"  /* tab5_lv_async_call (#258) */
#include "ui_home.h"  /* ui_home_update_status, ui_home_show_toast */
#include "voice.h"    /* voice_send_config_update_ex */

static const char *TAG = "voice_billing";

/* ── Deferred receipt attach ─────────────────────────────────────────── */
/* Hops from voice WS rx task onto LVGL thread so it queues AFTER
 * ui_chat_push_message("assistant", ...) from llm_done. */
typedef struct {
   uint32_t mils;
   uint16_t ptok;
   uint16_t ctok;
   bool retried;
   char model_short[16];
} receipt_attach_async_t;

static void receipt_attach_async_cb(void *arg) {
   receipt_attach_async_t *r = (receipt_attach_async_t *)arg;
   if (!r) return;
   chat_store_attach_receipt_ex(r->mils, r->ptok, r->ctok, r->model_short, r->retried);
   ui_chat_refresh_receipts();
   free(r);
}

void voice_billing_defer_receipt_attach_async(uint32_t mils, uint16_t ptok, uint16_t ctok, const char *model_short,
                                              bool retried) {
   receipt_attach_async_t *r = calloc(1, sizeof(*r));
   if (!r) {
      /* Wave 14 W14-M08: log the drop so "my cost tracking is off"
       * bug reports have a trail. */
      ESP_LOGW(TAG, "voice_billing_defer_receipt_attach OOM — dropped receipt (mils=%lu)", (unsigned long)mils);
      return;
   }
   r->mils = mils;
   r->ptok = ptok;
   r->ctok = ctok;
   r->retried = retried;
   if (model_short) {
      snprintf(r->model_short, sizeof(r->model_short), "%s", model_short);
   }
   tab5_lv_async_call(receipt_attach_async_cb, r);
}

/* ── /receipt WS verb handler ─────────────────────────────────────── */

void voice_billing_record_receipt(const char *model, int pt, int ct, int m, bool retried) {
   ESP_LOGI(TAG, "Receipt: model=%s tok=%d+%d cost=%d mils ($%d.%05d)", model ? model : "?", pt, ct, m, m / 100000,
            m % 100000);

   /* Phase 3c: accumulate into the daily spend counter. NVS write is
    * serialised by the settings mutex; this runs in the voice WS rx
    * task so the LVGL thread won't block. */
   if (m > 0) {
      tab5_budget_accumulate((uint32_t)m);
      uint32_t spent = tab5_budget_get_today_mils();
      uint32_t cap = tab5_budget_get_cap_mils();
      ESP_LOGI(TAG, "Budget: today=%lu mils / cap=%lu mils", (unsigned long)spent, (unsigned long)cap);

      /* Nudge home to redraw its live-line spend readout. */
      tab5_lv_async_call((lv_async_cb_t)ui_home_update_status, NULL);
      /* TT #328 Wave 1 — also refresh the chat-header spend badge
       * so a turn that flipped the chip from dim → amber → red
       * shows up the moment the receipt lands. */
      ui_chat_refresh_spend();

      /* Phase 3e auto-downgrade: once spent >= cap AND we're in a
       * cloud-cost-bearing mode (1=Hybrid or 2=Full Cloud), flip
       * back to Local (voice_mode=0) + notify Dragon + surface a
       * toast so the user knows why their next turn is free.
       * Agent (mode 3) goes via TinkerClaw which has its own
       * billing -- we don't auto-change it here. */
      if (cap > 0 && spent >= cap) {
         uint8_t cur = tab5_settings_get_voice_mode();
         if (cur == 1 || cur == 2) {
            ESP_LOGW(TAG, "Budget cap reached -- auto-downgrading %d -> 0 (Local)", cur);
            tab5_settings_set_voice_mode(0);
            /* Clear llm_model override so Dragon reverts to local.
             * Leaving the NVS llm_model alone keeps the user's
             * cloud preference for when they raise the cap. */
            char lm[64] = {0};
            tab5_settings_get_llm_model(lm, sizeof(lm));
            /* G7-F: tag this downgrade so Dragon speaks a short TTS
             * alert -- the user hears the switch even with the screen
             * off. */
            voice_send_config_update_ex(0, lm, "cap_downgrade");
            /* Also reset the three Sovereign tiers so the mode
             * sheet visually reflects the downgrade. */
            tab5_settings_set_int_tier(0);
            tab5_settings_set_voi_tier(0);
            tab5_lv_async_call((lv_async_cb_t)ui_home_show_toast, (void *)"Budget cap hit — Local mode");
         }
      }
   }

   /* Phase 3d + 4a: attach the receipt to the most-recent assistant
    * bubble in the chat store so chat_msg_view can render a per-turn
    * stamp.  Attach regardless of cost_mils so LOCAL turns (Ollama
    * qwen3, cost=0) also get a "qwen3 · FREE" stamp -- engine-used
    * transparency on every bubble, not just billable ones.
    *
    * v4·D audit P1 fix: always stamp the bubble, even when model is
    * missing -- a blank model still tells the user the turn finished
    * and the cost was zero.  Previously the null/empty-model path
    * silently swallowed the receipt and the user never saw any
    * stamp on a local turn that Dragon forgot to name. */
   char short_model[16] = {0};
   const char *mp = (model && model[0]) ? model : "local";
   const char *slash = strchr(mp, '/');
   const char *tail = slash ? slash + 1 : mp;
   const char *hyphen = strchr(tail, '-'); /* skip vendor prefix "claude-" */
   const char *start = hyphen ? hyphen + 1 : tail;
   snprintf(short_model, sizeof(short_model), "%s", start);
   /* Defer the attach onto the LVGL thread.  ui_chat_push_message
    * ("assistant", ...) from llm_done enqueues via lv_async_call a
    * few ms before this handler runs; if we attach synchronously
    * here (on the voice WS rx task) the assistant bubble is not
    * yet in chat_store, attach returns -1, and the stamp is
    * silently dropped.  Queuing via lv_async_call after the push
    * preserves FIFO order — push lands first, attach lands
    * second — and both run on the LVGL thread. */
   voice_billing_defer_receipt_attach_async((uint32_t)m, (uint16_t)pt, (uint16_t)ct, short_model, retried);
}
