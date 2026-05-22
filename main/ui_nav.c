/**
 * @file ui_nav.c
 * @brief Implementation — see ui_nav.h.
 *
 * Wraps debug_server_nav's `async_navigate` dispatcher with a voice-
 * cancel-then-navigate gate and a unified tap debounce.  The actual
 * screen-swap logic (overlay hide order, target → ui_*_create dispatch)
 * lives unchanged in debug_server_nav.c — we just trigger it.
 *
 * TT #623.
 */

#include "ui_nav.h"

#include <stdio.h>
#include <string.h>

#include "debug_obs.h"
#include "debug_server.h" /* tab5_debug_set_nav_target */
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "settings.h" /* TT #658 — gesture-hint NVS guard */
#include "ui_core.h"  /* ui_tap_gate */
#include "ui_home.h"  /* TT #658 — ui_home_show_toast for the hint */
#include "voice.h"    /* voice_get_state, voice_cancel, voice_state_t */

static const char *TAG = "ui_nav";

/* Forward decl — implemented in debug_server_nav.c.  Sets the cached
 * nav-target name (so /screen returns it) and schedules `async_navigate`
 * on the LVGL thread. */
extern void tab5_debug_trigger_async_navigate(const char *target_name);

static const char *NAV_NAMES[NAV_COUNT] = {
    [NAV_HOME] = "home",         [NAV_CHAT] = "chat",         [NAV_NOTES] = "notes",   [NAV_FILES] = "files",
    [NAV_CAMERA] = "camera",     [NAV_SETTINGS] = "settings", [NAV_AGENTS] = "agents", [NAV_SKILLS] = "skills",
    [NAV_SESSIONS] = "sessions", [NAV_MEMORY] = "memory",     [NAV_FOCUS] = "focus",   [NAV_WIFI] = "wifi",
};

const char *tab5_nav_name(tab5_nav_target_t target) {
   if (target < 0 || target >= NAV_COUNT) return "?";
   return NAV_NAMES[target];
}

tab5_nav_target_t tab5_nav_target_from_name(const char *name) {
   if (!name || !name[0]) return NAV_COUNT;
   for (int i = 0; i < NAV_COUNT; i++) {
      if (strcmp(NAV_NAMES[i], name) == 0) return (tab5_nav_target_t)i;
   }
   return NAV_COUNT;
}

esp_err_t tab5_nav_to_name(const char *name, tab5_nav_flags_t flags) {
   tab5_nav_target_t t = tab5_nav_target_from_name(name);
   if (t == NAV_COUNT) return ESP_ERR_NOT_FOUND;
   return tab5_nav_to(t, flags);
}

esp_err_t tab5_nav_to(tab5_nav_target_t target, tab5_nav_flags_t flags) {
   if (target < 0 || target >= NAV_COUNT) return ESP_ERR_INVALID_ARG;
   const char *name = NAV_NAMES[target];

   /* Step 1: per-target tap debounce.  Catches double-taps + rapid
    * back-button mashing.  300 ms matches the existing UI debounce. */
   if (!(flags & NAV_FLAGS_FORCE)) {
      char gate_key[24];
      snprintf(gate_key, sizeof(gate_key), "nav:%s", name);
      if (!ui_tap_gate(gate_key, 300)) {
         ESP_LOGD(TAG, "%s: debounced", name);
         return ESP_ERR_INVALID_STATE;
      }
   }

   /* Step 2: voice-aware cancel.  If a voice turn is mid-flight, stop
    * the mic + speaker + Dragon side cleanly before swapping screens.
    * The ~200 ms settle gives the I2S RX disable+re-enable cycle inside
    * voice_cancel() time to complete before the screen tears down.
    * Without this, the audit found possible LVGL heap corruption when
    * tts playback writes land mid-screen-transition (Journey 5). */
   if (!(flags & NAV_FLAGS_NO_VOICE_CANCEL)) {
      voice_state_t vs = voice_get_state();
      if (vs != VOICE_STATE_IDLE && vs != VOICE_STATE_READY) {
         ESP_LOGI(TAG, "%s: voice in state %d — cancelling first", name, (int)vs);
         tab5_debug_obs_event("nav.voice_cancel", name);
         voice_cancel();
         vTaskDelay(pdMS_TO_TICKS(200));
      }
   }

   /* Step 3: emit obs event so the e2e harness can wait on it. */
   tab5_debug_obs_event("nav.go", name);

   /* Polish P6 (TT #658): single-shot gesture hint.  On first
    * overlay nav from home, surface a toast telling the user about
    * the swipe-right-to-go-back gesture.  Gated on NVS `gest_hint`
    * so it never fires twice.  Skipped on nav-to-home (would be
    * unhelpful — user is going home, not landing on a new overlay). */
   if (target != NAV_HOME && !tab5_settings_get_gesture_hint_seen()) {
      tab5_settings_set_gesture_hint_seen(true);
      /* Fire the toast — ui_home_show_toast already async-routes to
       * the LVGL thread, so calling from any context is safe. */
      ui_home_show_toast("Tip: swipe right to go back");
      tab5_debug_obs_event("gest.hint", "fired");
   }

   /* Step 4: hand off to the LVGL-thread dispatcher.  This sets the
    * cached nav-target name (so /screen returns the new value) and
    * schedules `async_navigate` to do the actual hide-overlays-then-
    * swap-screen work. */
   tab5_debug_trigger_async_navigate(name);
   return ESP_OK;
}
