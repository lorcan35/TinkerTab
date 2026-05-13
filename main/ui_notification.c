/*
 * ui_notification.c — see header.
 *
 * Wave 7-E.1 toast path + W7-E.2 now-card router + W7-E.3 dedupe + snooze rings.
 */
#include "ui_notification.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "debug_obs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "settings.h"
#include "ui_audio_cues.h"
#include "ui_core.h" /* tab5_lv_async_call */
#include "ui_home.h"
#include "voice.h" /* W7-E.4: voice_send_channel_reply */

static const char *TAG = "ui_notification";

/* ── W7-E.3: dedupe ring ───────────────────────────────────────────── */

/* 32 entries keyed on message_id.  PSRAM-cheap (32 × 64 B = 2 KB in BSS).
 * Older entries get overwritten as new arrivals push past the head. */
#define DEDUPE_RING_SIZE 32
static char s_dedupe_ring[DEDUPE_RING_SIZE][CHANNEL_MSG_ID_MAX];
static int s_dedupe_head = 0;

/* Returns true if `id` is new (was not in the ring; added).  Returns
 * false if it was already present (caller should skip).  Empty id always
 * passes through (no dedupe — older Dragon builds may omit message_id). */
static bool dedupe_check_and_add(const char *id) {
   if (!id || !id[0]) return true; /* no id → always allow */
   for (int i = 0; i < DEDUPE_RING_SIZE; i++) {
      if (s_dedupe_ring[i][0] && strncmp(s_dedupe_ring[i], id, CHANNEL_MSG_ID_MAX) == 0) {
         return false;
      }
   }
   /* snprintf is bounds-safe and dodges the GCC strncpy-truncation warning
    * since we explicitly cap output length including NUL. */
   snprintf(s_dedupe_ring[s_dedupe_head], CHANNEL_MSG_ID_MAX, "%s", id);
   s_dedupe_head = (s_dedupe_head + 1) % DEDUPE_RING_SIZE;
   return true;
}

/* ── W7-E.3: snooze ring ───────────────────────────────────────────── */

#define SNOOZE_RING_SIZE 8
#define SNOOZE_DELAY_MS (15 * 60 * 1000) /* 15 minutes per PLAN §3.2 */
#define SNOOZE_TICK_MS (60 * 1000)       /* walker fires every 60 s */

static channel_message_t s_snooze_ring[SNOOZE_RING_SIZE];
static uint64_t s_snooze_fire_at[SNOOZE_RING_SIZE]; /* esp_timer_get_time() µs */
static int s_snooze_count = 0;
static lv_timer_t *s_snooze_timer = NULL;

/* W7-E.2 → W7-E.3 handoff: remember the most recent now-card message so
 * the SNOOZE button can recover it without ui_home needing to carry the
 * full channel_message_t through its public API. */
static channel_message_t s_last_now_msg;
static bool s_last_now_msg_valid = false;

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

/* W7-E.2: routing rule per PLAN §3.1 — high-priority OR starred sender
 * OR needs_reply lands on the now-card; everything else stays on the
 * toast surface from W7-E.1. */
static bool route_to_now_card(const channel_message_t *msg) {
   if (msg->sender_starred) return true;
   if (msg->needs_reply) return true;
   if (msg->priority[0] && strcasecmp(msg->priority, "high") == 0) return true;
   return false;
}

/* Owned copy queued for the LVGL thread.  Freed in the async callback. */
static void notif_show_async_cb(void *arg) {
   channel_message_t *msg = (channel_message_t *)arg;
   if (!msg) return;

   /* W7-E.5: per-channel opt-in gate.  Settings → CHANNELS controls
    * which platforms surface on this device.  Fail-open on unknown
    * channel names so future Dragon additions still get through. */
   if (msg->channel[0] && !tab5_settings_get_channel_enabled(msg->channel)) {
      char detail[32];
      snprintf(detail, sizeof(detail), "%.16s", msg->channel);
      tab5_debug_obs_event("ui.notif.channel_off", detail);
      free(msg);
      return;
   }

   /* W7-E.3: dedupe before any surface side-effect.  Same message_id
    * arriving twice = silent drop with obs trail. */
   if (!dedupe_check_and_add(msg->message_id)) {
      char detail[48];
      snprintf(detail, sizeof(detail), "drop %.32s", msg->message_id);
      tab5_debug_obs_event("ui.notif.dedupe", detail);
      free(msg);
      return;
   }

   const char *ch = msg->channel[0] ? msg->channel : "?";
   const char *sender = msg->sender[0] ? msg->sender : "Someone";
   const char *preview = msg->preview[0] ? msg->preview : "(no preview)";
   bool to_now_card = route_to_now_card(msg);

   bool quiet = quiet_now();

   if (to_now_card) {
      /* Richer surface: kicker + multi-line preview + 3-button row.
       * High-priority gets through visually even in quiet hours per
       * PLAN §4.3 — user opted into the priority signal. */
      ui_home_show_channel_now(ch, sender, preview);
      /* Cache for the SNOOZE button — see ui_notification_snooze_current. */
      memcpy(&s_last_now_msg, msg, sizeof(s_last_now_msg));
      s_last_now_msg_valid = true;
   } else {
      /* Toast: compose "[ch] sender · preview".  Bounded with %.Ns
       * specifiers so compiler can prove no truncation. */
      char text[200];
      snprintf(text, sizeof(text), "[%.15s] %.40s · %.120s", ch, sender, preview);
      /* W7-E.6: dim variant during quiet hours — visible but not
       * attention-grabbing per PLAN §4.3. */
      ui_home_show_toast_ex(text, quiet ? UI_TOAST_INCOMING_QUIET : UI_TOAST_INCOMING);
   }

   /* Audio cue: HIGH for now-card, LOW for toast.  Respect quiet hours
    * per spec §4.3 — visible surface still fires, audio is suppressed. */
   if (!quiet) {
      ui_audio_cue_play(to_now_card ? UI_CUE_INCOMING_HIGH : UI_CUE_INCOMING_LOW);
   }

   char detail[64];
   snprintf(detail, sizeof(detail), "%.6s/%.16s pri=%.6s surf=%s%s", ch, sender, msg->priority[0] ? msg->priority : "?",
            to_now_card ? "now" : "toast", quiet ? " quiet" : "");
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

/* ── W7-E.3: snooze walker + public API ────────────────────────────── */

static uint64_t snooze_now_us(void) { return (uint64_t)esp_timer_get_time(); }

/* LVGL-thread periodic timer.  Walks the ring; any entry whose fire_at
 * has passed gets removed + re-shown via the normal routing path. */
static void snooze_walk_cb(lv_timer_t *t) {
   (void)t;
   /* W7-E.6: during quiet hours, leave the ring untouched so re-fires
    * don't wake the user.  Next 60 s tick after quiet ends will process
    * the accumulated backlog naturally — entries with `fire_at` already
    * in the past stay flagged ready. */
   if (quiet_now()) {
      if (s_snooze_count > 0) {
         tab5_debug_obs_event("ui.notif.snooze", "defer_quiet");
      }
      return;
   }
   uint64_t now_us = snooze_now_us();
   /* Walk in reverse so we can remove without re-indexing. */
   for (int i = s_snooze_count - 1; i >= 0; i--) {
      if (s_snooze_fire_at[i] > now_us) continue;

      /* Snapshot the message so we can fire it after we shrink the ring
       * — ui_notification_show could recursively populate s_snooze_ring
       * if the re-fire ends up snoozed again. */
      channel_message_t fire = s_snooze_ring[i];
      if (i < s_snooze_count - 1) {
         memmove(&s_snooze_ring[i], &s_snooze_ring[i + 1], sizeof(s_snooze_ring[0]) * (s_snooze_count - i - 1));
         memmove(&s_snooze_fire_at[i], &s_snooze_fire_at[i + 1], sizeof(uint64_t) * (s_snooze_count - i - 1));
      }
      s_snooze_count--;

      /* Clear the dedupe slot for this id so the refire isn't silently
       * dropped.  Simpler than tracking a "snooze bypass" flag through
       * the async hop. */
      if (fire.message_id[0]) {
         for (int k = 0; k < DEDUPE_RING_SIZE; k++) {
            if (strncmp(s_dedupe_ring[k], fire.message_id, CHANNEL_MSG_ID_MAX) == 0) {
               s_dedupe_ring[k][0] = '\0';
               break;
            }
         }
      }

      tab5_debug_obs_event("ui.notif.snooze", "refire");
      ui_notification_show(&fire);
   }
}

void ui_notification_init(void) {
   if (s_snooze_timer) return;
   s_snooze_timer = lv_timer_create(snooze_walk_cb, SNOOZE_TICK_MS, NULL);
   ESP_LOGI(TAG, "snooze walker armed (tick=%u ms, delay=%u ms)", (unsigned)SNOOZE_TICK_MS, (unsigned)SNOOZE_DELAY_MS);
}

void ui_notification_reply_current(const char *text) {
   if (!s_last_now_msg_valid) {
      ESP_LOGW(TAG, "reply requested but no now-card message in cache");
      tab5_debug_obs_event("ui.notif.reply", "no_cache");
      return;
   }

   /* W7-E.4b: when caller supplies explicit text, use the legacy direct-
    * send path (debug surfaces + future shortcut paths still use this).
    * When text is NULL/empty, arm the reply context so the next mic-orb
    * dictation routes to voice_send_channel_reply via the STT-complete
    * branch in voice_ws_proto.c — that's the real spec §3.3 flow. */
   if (text && text[0]) {
      esp_err_t r = voice_send_channel_reply(s_last_now_msg.channel, s_last_now_msg.thread_id, text);
      char detail[64];
      if (r == ESP_OK) {
         snprintf(detail, sizeof(detail), "sent ch=%.10s thread=%.20s", s_last_now_msg.channel,
                  s_last_now_msg.thread_id);
         tab5_debug_obs_event("ui.notif.reply", detail);
         char toast[96];
         snprintf(toast, sizeof(toast), "Reply queued to %.40s", s_last_now_msg.sender);
         ui_home_show_toast_ex(toast, UI_TOAST_INFO);
      } else {
         snprintf(detail, sizeof(detail), "send_fail err=%d", (int)r);
         tab5_debug_obs_event("ui.notif.reply", detail);
         ui_home_show_toast_ex("Reply not sent — Dragon offline", UI_TOAST_WARN);
      }
      return;
   }

   /* W7-E.4b: voice-dictated reply path.  Arm + prompt. */
   voice_arm_channel_reply(s_last_now_msg.channel, s_last_now_msg.thread_id, s_last_now_msg.sender);

   /* TT #481 (W7-E.4c follow-up): if the voice overlay is already up
    * (e.g. user was mid-conversation when a now-card arrived), the
    * chip-paint inside ui_voice_show won't re-run on next listening
    * start because s_visible is already true.  Repaint explicitly so
    * the chip appears on either path: overlay-already-up OR fresh-open. */
   extern void ui_voice_refresh_reply_chip(void);
   ui_voice_refresh_reply_chip();

   char detail[64];
   snprintf(detail, sizeof(detail), "armed ch=%.8s sender=%.20s", s_last_now_msg.channel, s_last_now_msg.sender);
   tab5_debug_obs_event("ui.notif.reply", detail);

   char toast[96];
   snprintf(toast, sizeof(toast), "Hold the orb to reply to %.30s", s_last_now_msg.sender);
   ui_home_show_toast_ex(toast, UI_TOAST_INFO);
}

void ui_notification_snooze_current(void) {
   if (!s_last_now_msg_valid) {
      ESP_LOGW(TAG, "snooze requested but no now-card message in cache");
      return;
   }

   /* Overflow: drop oldest. */
   if (s_snooze_count >= SNOOZE_RING_SIZE) {
      memmove(&s_snooze_ring[0], &s_snooze_ring[1], sizeof(s_snooze_ring[0]) * (SNOOZE_RING_SIZE - 1));
      memmove(&s_snooze_fire_at[0], &s_snooze_fire_at[1], sizeof(uint64_t) * (SNOOZE_RING_SIZE - 1));
      s_snooze_count = SNOOZE_RING_SIZE - 1;
      tab5_debug_obs_event("ui.notif.snooze", "overflow");
   }

   s_snooze_ring[s_snooze_count] = s_last_now_msg;
   s_snooze_fire_at[s_snooze_count] = snooze_now_us() + (uint64_t)SNOOZE_DELAY_MS * 1000ULL;
   s_snooze_count++;

   char detail[48];
   snprintf(detail, sizeof(detail), "added %.8s/%.20s in=%us", s_last_now_msg.channel, s_last_now_msg.sender,
            (unsigned)(SNOOZE_DELAY_MS / 1000));
   tab5_debug_obs_event("ui.notif.snooze", detail);

   ui_home_show_toast_ex("Snoozed for 15 minutes", UI_TOAST_INFO);
}
