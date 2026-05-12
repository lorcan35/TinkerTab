/*
 * ui_notification.c — see header.
 *
 * Wave 7-E.1 (toast-path slice).
 */
#include "ui_notification.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "debug_obs.h"
#include "esp_log.h"
#include "lvgl.h"
#include "settings.h"
#include "ui_audio_cues.h"
#include "ui_core.h" /* tab5_lv_async_call */
#include "ui_home.h"

static const char *TAG = "ui_notification";

/* True if quiet-hours is currently active per NVS + local clock.  Mirrors
 * `tab5_settings_quiet_active(hour_local)` which expects an [0..23] hour. */
static bool quiet_now(void) {
   if (!tab5_settings_get_quiet_on()) return false;
   time_t now = time(NULL);
   if (now <= 0) return false; /* clock not yet synced — be permissive */
   struct tm lt;
   localtime_r(&now, &lt);
   return tab5_settings_quiet_active(lt.tm_hour);
}

/* Owned copy queued for the LVGL thread.  Freed in the async callback. */
static void notif_show_async_cb(void *arg) {
   channel_message_t *msg = (channel_message_t *)arg;
   if (!msg) return;

   /* Compose toast text: "[ch] sender · preview".  Bounded with %.Ns
    * specifiers so compiler can prove no truncation can OOB the 200 B
    * buffer.  Visually we want the line short anyway — long previews
    * read better in the now-card surface (W7-E.2). */
   char text[200];
   const char *ch = msg->channel[0] ? msg->channel : "?";
   const char *sender = msg->sender[0] ? msg->sender : "Someone";
   const char *preview = msg->preview[0] ? msg->preview : "(no preview)";
   snprintf(text, sizeof(text), "[%.15s] %.40s · %.120s", ch, sender, preview);

   ui_home_show_toast_ex(text, UI_TOAST_INCOMING);

   /* Audio cue: low ping for the toast surface.  Respect quiet hours
    * per spec §4.3 — visible toast still fires, audio is suppressed. */
   if (!quiet_now()) {
      ui_audio_cue_play(UI_CUE_INCOMING_LOW);
   }

   char detail[48];
   snprintf(detail, sizeof(detail), "%.8s/%.20s pri=%.6s", ch, sender, msg->priority[0] ? msg->priority : "?");
   tab5_debug_obs_event("ui.notif", detail);

   free(msg);
}

void ui_notification_show(const channel_message_t *msg) {
   if (!msg) return;

   /* Allocate an owned copy for the async hop.  malloc (not heap_caps)
    * because the payload is small (~400 B) and the LVGL thread reads it
    * during the same tick. */
   channel_message_t *owned = (channel_message_t *)malloc(sizeof(*owned));
   if (!owned) {
      ESP_LOGW(TAG, "OOM — dropping channel_message from %s/%s", msg->channel, msg->sender);
      return;
   }
   memcpy(owned, msg, sizeof(*owned));

   /* Defensive null-termination — every field is char[N] but the caller
    * might have set them via memcpy without the trailing NUL. */
   owned->channel[CHANNEL_NAME_MAX - 1] = '\0';
   owned->message_id[CHANNEL_MSG_ID_MAX - 1] = '\0';
   owned->thread_id[CHANNEL_THREAD_ID_MAX - 1] = '\0';
   owned->sender[CHANNEL_SENDER_MAX - 1] = '\0';
   owned->preview[CHANNEL_PREVIEW_MAX - 1] = '\0';
   owned->priority[CHANNEL_PRIORITY_MAX - 1] = '\0';

   lv_result_t r = tab5_lv_async_call(notif_show_async_cb, owned);
   if (r != LV_RESULT_OK) {
      ESP_LOGW(TAG, "lv_async_call failed — dropping channel_message");
      free(owned);
   }
}
