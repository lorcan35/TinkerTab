/**
 * TinkerTab — Settings Screen (Material Dark Design)
 *
 * Fullscreen overlay on home screen for M5Stack Tab5 (720x1280 portrait).
 * LVGL v9.2.2, manual Y positioning, max 55 objects (128KB pool).
 *
 * Sections: Display, Network, Voice Mode, Storage, Battery, About
 * Each section has a unique accent color. Spacing used instead of dividers.
 */

#include "ui_settings.h"

#include <stdio.h>
#include <string.h>

#include "audio.h"
#include "battery.h"
#include "bluetooth.h"
#include "config.h"
#include "debug_obs.h" /* TT #642 — eng.llm obs event */
#include "display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "imu.h"
#include "ota.h"
#include "sdcard.h"
#include "settings.h"
#include "tab5_rtc.h"
#include "task_worker.h"   /* TT #328 Wave 14 — async K144 hwinfo fetch */
#include "ui_components.h" /* TT #652 — ui_overlay_fade_in */
#include "ui_core.h"
#include "ui_feedback.h" /* TT #328 Wave 10: ui_fb_* */
#include "ui_home.h"
#include "ui_keyboard.h"
#include "ui_nav.h" /* TT #623 — tab5_nav_to */
#include "voice.h"
#include "wifi.h"

static const char *TAG = "ui_settings";

/* TT #723: canonical mode names live in ui_theme.c. ui_settings.c mirrors the
 * TH_* color tokens locally rather than including ui_theme.h, so declare just
 * the one symbol we need here. */
extern const char *th_mode_names[];

static inline void feed_wdt(void) {
    esp_task_wdt_reset();
    /* Yield to let LWIP TCP/IP task process packets on Core 0.
     * Settings creates 55 LVGL objects in one lv_async_call callback,
     * monopolizing Core 0. LWIP needs Core 0 cycles to deliver HTTP
     * requests to the debug server (running on Core 1).
     * 20ms per yield × 7 sections = 140ms total overhead — acceptable. */
    vTaskDelay(pdMS_TO_TICKS(20));
}

/* ── v5 Zero Interface constants (mirrors ui_theme.h TH_*) ──────────── */
#define BG_COLOR     0x08080E  /* TH_BG */
#define CARD_COLOR   0x111119  /* TH_CARD */
#define HAIR_COLOR   0x1C1C28  /* TH_HAIRLINE */
#define AMBER        0xF59E0B
#define TEXT_PRIMARY 0xE8E8EF
#define TEXT_SUBTLE  0x555555
#define TEXT_DIM     0x888888
#define SIDE_PAD     20
#define RIGHT_X      380
#define ROW_H        44
#define HDR_H        28
#define TOPBAR_H     48
#define OVERLAY_H    1280
#define NAV_BAR_H    120
#define USABLE_H     (OVERLAY_H - NAV_BAR_H)  /* Full screen — nav bar on lv_layer_top() */
#define CONTENT_W    680

/* v5: single amber accent across all sections. Voice-tab colors stay
 * (they encode mode choice, not section identity). */
#define ACC_DISPLAY  AMBER
#define ACC_NETWORK  AMBER
#define ACC_VOICE    AMBER
#define ACC_STORAGE  AMBER
#define ACC_BATTERY  AMBER
#define ACC_ABOUT    AMBER

/* Voice tab colors — TT #328 Wave 1: HYBRID + ONBOARD literals had drifted
 * from canonical ui_theme.h tokens (TH_MODE_HYBRID = 0xEAB308 was 0xF59E0B
 * amber here; TH_MODE_ONBOARD = 0x8E5BFF was 0x8B5CF6 here).  Settings
 * radio preview disagreed with dots/chips everywhere else (Sessions
 * drawer, Home pill, mode_to_tag).  Realigned; mirror any future change
 * in ui_theme.h here too. */
#define TAB_LOCAL    0x22C55E
#define TAB_HYBRID 0xEAB308
#define TAB_CLOUD    0x3B82F6
#define TAB_TINKERCLAW 0xF43F5E
#define TAB_ONBOARD 0x8E5BFF /* P5b: violet — distinct from existing four */

/* TT #328 Wave 14 — K144 hardware gauge.  Small label below the chip
 * showing NPU temperature + load + StackFlow daemon version.
 * Wave 15 extends this with a model inventory line summarising the
 * sys.lsmode registry ("11 MODELS · 1 LLM · 2 ASR · 3 TTS · 2 KWS · 3 vision").
 * Populated via a worker job (UART round-trips ~150 ms each; can't
 * run on the LVGL thread).  Both labels rebuild every Settings show. */
#include "esp_heap_caps.h" /* heap_caps_calloc for the modelist scratch */
#include "voice_m5_llm.h"  /* voice_m5_hwinfo_t + sys_hwinfo + sys_version + sys_lsmode */
#include "voice_onboard.h"  /* TT #586: arm wakeword + reset failover */
#include "voice_wakeword.h" /* TT #586: is_active for switch state */
static lv_obj_t *s_k144_gauge_lbl = NULL;
static lv_obj_t *s_k144_models_lbl = NULL;
/* Wave 16 — chip widget tracked at file-scope so ui_settings_update()
 * can re-render its text + color when the K144 failover state
 * transitions across hide/show cycles.  Pre-Wave-16 the chip was a
 * local var inside the radio-row creation loop; it captured the
 * state at first build and never updated, so a Tab5 navigated away
 * during UNAVAILABLE and back after recovery still showed the stale
 * red chip until the user rebooted. */
static lv_obj_t *s_k144_chip_lbl = NULL;
static int s_k144_last_chip_fs = -1; /* sentinel: force refresh on first call */

typedef struct {
   voice_m5_hwinfo_t hw;
   char version[16];
   bool hw_ok;
   /* Wave 15 — capability counts for the inventory line.  Heap-allocated
    * via calloc + freed in the async render so the full ~1.8 KB
    * voice_m5_modelist_t doesn't ride along — only the summary. */
   int n_total;
   int n_llm;
   int n_asr;
   int n_tts;
   int n_kws;
   int n_vision;
   bool models_ok;
} k144_gauge_payload_t;

static void k144_gauge_async_render(void *arg) {
   k144_gauge_payload_t *p = (k144_gauge_payload_t *)arg;
   if (s_k144_gauge_lbl != NULL && p != NULL) {
      char buf[64];
      if (p->hw_ok && p->hw.valid) {
         double temp_c = (double)p->hw.temperature_milli_c / 1000.0;
         /* Compact one-line gauge:  "NPU 39.4°C · load 0 · v1.3" */
         snprintf(buf, sizeof(buf),
                  "NPU %.1f\xc2\xb0"
                  "C \xc2\xb7 load %d \xc2\xb7 %s",
                  temp_c, (int)p->hw.cpu_loadavg, p->version[0] ? p->version : "—");
      } else {
         snprintf(buf, sizeof(buf), "—");
      }
      lv_label_set_text(s_k144_gauge_lbl, buf);
   }
   if (s_k144_models_lbl != NULL && p != NULL) {
      char buf[96];
      if (p->models_ok && p->n_total > 0) {
         /* Compact inventory: "11 MODELS · 1 LLM · 2 ASR · 3 TTS · 2 KWS · 3 vision".
          * Categories with zero entries are elided so the line stays scannable. */
         int n = 0;
         n += snprintf(buf + n, sizeof(buf) - n, "%d MODEL%s", p->n_total, p->n_total == 1 ? "" : "S");
         if (p->n_llm > 0) n += snprintf(buf + n, sizeof(buf) - n, " \xc2\xb7 %d LLM", p->n_llm);
         if (p->n_asr > 0) n += snprintf(buf + n, sizeof(buf) - n, " \xc2\xb7 %d ASR", p->n_asr);
         if (p->n_tts > 0) n += snprintf(buf + n, sizeof(buf) - n, " \xc2\xb7 %d TTS", p->n_tts);
         if (p->n_kws > 0) n += snprintf(buf + n, sizeof(buf) - n, " \xc2\xb7 %d KWS", p->n_kws);
         if (p->n_vision > 0) n += snprintf(buf + n, sizeof(buf) - n, " \xc2\xb7 %d vision", p->n_vision);
      } else {
         snprintf(buf, sizeof(buf), "—");
      }
      lv_label_set_text(s_k144_models_lbl, buf);
   }
   if (p != NULL) free(p);
}

static void k144_gauge_fetch_job(void *arg); /* fwd — defined just below */

/* TT #328 Wave 16 — re-render the K144 health chip from the current
 * voice_onboard_failover_state().  Idempotent + cheap (early-exits
 * when state is unchanged from last call).  Called from the chip
 * creation site at first Settings build AND from ui_settings_update()
 * every 2 s so a state transition while Settings is hidden becomes
 * visible immediately on next show. */
static void refresh_k144_chip(void) {
   if (s_k144_chip_lbl == NULL) return;
   extern int voice_onboard_failover_state(void);
   int fs = voice_onboard_failover_state();
   if (fs == s_k144_last_chip_fs) return; /* no-op when state hasn't moved */
   s_k144_last_chip_fs = fs;

   const char *chip_glyph;
   const char *chip_text;
   uint32_t chip_col;
   switch (fs) {
      case 2:
         chip_glyph = "\xe2\x97\x8f"; /* ● filled */
         chip_text = " READY";
         chip_col = 0x10B981;
         break;
      case 1:
         chip_glyph = "\xe2\x97\x8b"; /* ○ open */
         chip_text = " WARMING";
         chip_col = AMBER;
         break;
      case 3:
         chip_glyph = "\xe2\x9c\x97"; /* ✗ */
         chip_text = " UNAVAILABLE";
         chip_col = 0xE5484D;
         break;
      default:
         chip_glyph = "\xe2\x97\x8b";
         chip_text = " UNKNOWN";
         chip_col = 0x55555D;
         break;
   }
   char chip_buf[40];
   if (fs == 3 || fs == 0) {
      snprintf(chip_buf, sizeof(chip_buf), "%s%s \xc2\xb7 TAP", chip_glyph, chip_text);
   } else {
      snprintf(chip_buf, sizeof(chip_buf), "%s%s", chip_glyph, chip_text);
   }
   lv_label_set_text(s_k144_chip_lbl, chip_buf);
   lv_obj_set_style_text_color(s_k144_chip_lbl, lv_color_hex(chip_col), 0);

   /* If we just transitioned to READY, re-fire the worker fetch so
    * the gauge + inventory line populate (worker bails when state
    * isn't READY, so a transition is exactly when we want to retry). */
   if (fs == 2) {
      (void)tab5_worker_enqueue(k144_gauge_fetch_job, NULL, "k144_gauge");
   } else {
      /* Reset gauge + inventory to placeholder when leaving READY so
       * stale numbers don't mislead the user during a recovery cycle. */
      if (s_k144_gauge_lbl != NULL) lv_label_set_text(s_k144_gauge_lbl, "—");
      if (s_k144_models_lbl != NULL) lv_label_set_text(s_k144_models_lbl, "—");
   }
}

static void k144_gauge_fetch_job(void *arg) {
   (void)arg;
   k144_gauge_payload_t *p = calloc(1, sizeof(*p));
   if (p == NULL) return;
   esp_err_t he = voice_m5_llm_sys_hwinfo(&p->hw);
   (void)voice_m5_llm_sys_version(p->version, sizeof(p->version));
   p->hw_ok = (he == ESP_OK);
   /* Wave 15 — fetch the model registry too.  PSRAM allocation because
    * voice_m5_modelist_t is ~1.8 KB and we don't want it on the worker
    * stack.  Counts are summarised into the payload + the full list
    * freed before the LVGL async-render callback. */
   voice_m5_modelist_t *ml = heap_caps_calloc(1, sizeof(*ml), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (ml != NULL) {
      esp_err_t le = voice_m5_llm_sys_lsmode(ml);
      if (le == ESP_OK && ml->valid) {
         p->n_total = ml->n;
         for (int i = 0; i < ml->n; i++) {
            const char *cap = ml->models[i].primary_cap;
            if (strcmp(cap, "text_generation") == 0)
               p->n_llm++;
            else if (strcmp(cap, "Automatic_Speech_Recognition") == 0)
               p->n_asr++;
            else if (strcmp(cap, "tts") == 0)
               p->n_tts++;
            else if (strcmp(cap, "Keyword_spotting") == 0)
               p->n_kws++;
            else if (strcmp(cap, "Pose") == 0 || strcmp(cap, "Segmentation") == 0 || strcmp(cap, "Detection") == 0)
               p->n_vision++;
         }
         p->models_ok = true;
      }
      heap_caps_free(ml);
   }
   tab5_lv_async_call(k144_gauge_async_render, p);
}

/* ── Screen-lifetime state ──────────────────────────────────────────── */
static lv_obj_t *s_screen          = NULL;
static lv_obj_t *s_scroll          = NULL;

/* Live-updated labels */
static lv_obj_t *s_lbl_wifi       = NULL;
static lv_obj_t *s_lbl_bat_status = NULL;   /* primary: "XX% • Charging" or "USB Powered" */
static lv_obj_t *s_lbl_bat_volt   = NULL;  /* debug: small dim voltage */
static lv_obj_t *s_bar_bat_level  = NULL;
static lv_obj_t *s_lbl_bat_pct    = NULL;
static lv_obj_t *s_lbl_sd_info    = NULL;
static lv_obj_t *s_lbl_heap       = NULL;
static lv_obj_t *s_lbl_orient     = NULL;

/* NTP sync */
static lv_obj_t *s_ntp_spinner    = NULL;
static lv_obj_t *s_ntp_btn_label  = NULL;

/* Dragon host text input */
static lv_obj_t *s_dragon_ta      = NULL;

/* OTA */
static lv_obj_t *s_ota_btn_label  = NULL;
static lv_obj_t *s_ota_apply_btn  = NULL;
static char      s_ota_url[256]   = {0};
static char      s_ota_sha256[65] = {0};

/* Sliders */
static lv_obj_t *s_slider_bright  = NULL;
static lv_obj_t *s_slider_volume  = NULL;
static lv_obj_t *s_slider_cap     = NULL;  /* v4·D Phase 4e daily budget cap */

/* Slider value labels */
static lv_obj_t *s_lbl_bright_val = NULL;
static lv_obj_t *s_lbl_vol_val    = NULL;
static lv_obj_t *s_lbl_cap_val    = NULL;  /* v4·D Phase 4e cap readout ($X.XX / OFF) */
static lv_timer_t *s_cap_save_timer = NULL;

/* Auto-rotate switch */
static lv_obj_t *s_sw_autorot     = NULL;

/* Guard flag for background tasks during destroy */
static volatile bool s_destroying = false;
/* Guard flag to prevent double-creation from overlapping navigate calls */
static volatile bool s_creating = false;
static lv_timer_t *s_refresh_timer = NULL;

/* Two-pass creation: Phase 2 deferred via lv_timer_create */
static volatile bool s_phase2_pending = false;
/* U13 (#206): explicit "phase 2 finished cleanly" sentinel.  Replaces
 * the previous `!s_lbl_heap` heuristic on the re-show path, which was
 * fragile for two reasons: (1) phase 2 could fail BEFORE creating
 * s_lbl_heap, leaving s_phase2_done correct but the heuristic still
 * reading "needs Phase 2"; (2) phase 2 could fail AFTER creating
 * s_lbl_heap, leaving the heuristic falsely reporting "Phase 2 done".
 * The flag is set only at the end of phase2_timer_cb on success, and
 * cleared on every Phase-1 entry + destroy. */
static volatile bool s_phase2_done    = false;
static lv_timer_t   *s_phase2_timer  = NULL;
static int            s_phase2_y     = 0;       /* Y position carried from Phase 1 */

/* NVS write debounce timers — prevent flash wear from slider drag (US-HW17) */
static lv_timer_t *s_bright_save_timer = NULL;
static lv_timer_t *s_vol_save_timer    = NULL;

/* Voice tab system */
/* v5: five flat radio rows (no tabs).  Indexed by voice_mode.
 * Row 4 = Onboard (K144 stacked LLM via Mate carrier) — added in TT #317 P5b. */
/* Back-compat pointers kept = NULL so legacy references compile. */
static lv_obj_t *s_tab_local      = NULL;
static lv_obj_t *s_tab_hybrid     = NULL;
static lv_obj_t *s_tab_cloud      = NULL;
static lv_obj_t *s_tab_tinkerclaw = NULL;
static lv_obj_t *s_local_card     = NULL;
static lv_obj_t *s_hybrid_card    = NULL;
static lv_obj_t *s_cloud_card     = NULL;
static lv_obj_t *s_local_content[4]  = {NULL};
static lv_obj_t *s_hybrid_content[4] = {NULL};
static lv_obj_t *s_cloud_content[4]  = {NULL};
static lv_obj_t *s_tinkerclaw_card   = NULL;
static lv_obj_t *s_tinkerclaw_content[4] = {NULL};

/* Cap Wave 4 (TT #642) — LLM engine override chips + STT/TTS status
 * labels.  Three chips (AUTO / K144 / OPENROUTER) directly under the
 * Cloud LLM picker, plus two read-only status lines below.  The chip
 * row composes with the coarse VOICE MODE picker — engine=AUTO means
 * "follow whatever vmode dispatches"; an explicit chip wins. */
static lv_obj_t *s_eng_chip[LLM_ENG_COUNT] = {NULL};
static lv_obj_t *s_eng_stt_lbl = NULL;
static lv_obj_t *s_eng_tts_lbl = NULL;

/* ══════════════════════════════════════════════════════════════════════
 *  Material Dark Helper Functions
 * ══════════════════════════════════════════════════════════════════════ */

/** Section header with accent-colored label.
 *  Returns the Y position after the header (y + HDR_H).
 *
 *  Wave 15 W15-C06: NULL-guard on `lv_label_create`.  When internal
 *  heap is exhausted (observed live: `/heap` shows internal 6 KB free,
 *  largest 5 KB) LVGL returns NULL from the create call; the old code
 *  then passed NULL to `lv_label_set_text` → dereference → panic in
 *  `lv_obj_get_ext_draw_size (obj=0x0)` → hard reboot.  Now we skip
 *  the styling silently and return the same Y offset so the rest of
 *  the screen can still render.  The user sees a missing label but
 *  the device stays alive — infinitely better than a panic-reboot
 *  mid-navigation. */
static int mk_section(lv_obj_t *parent, const char *text, lv_color_t accent, int y)
{
    lv_obj_t *lbl = lv_label_create(parent);
    if (!lbl) {
        ESP_LOGW("ui_settings", "mk_section: lv_label_create failed (text=%s y=%d)", text, y);
        return y + HDR_H;
    }
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, accent, 0);
    lv_obj_set_style_text_font(lbl, FONT_CAPTION, 0);
    lv_obj_set_style_text_letter_space(lbl, 3, 0);
    lv_obj_set_pos(lbl, SIDE_PAD, y);

    return y + HDR_H;
}

/** Cap Wave 5 (TT #644) — card-background helper.  Creates a rounded
 *  filled rect under the just-laid-out section and sends it to back
 *  via lv_obj_move_background so it sits behind the section's content
 *  without re-parenting.  Lets us turn the flat row-stack into airy
 *  visually-grouped cards with one helper call per section.
 *
 *  Pass the y_top BEFORE the section header label and y_bottom AFTER
 *  the last row.  The card extends 8 px above/below for breathing
 *  room (header sits on the card top edge, content has padding). */
static void mk_card_bg(lv_obj_t *parent, int y_top, int y_bottom) {
   if (y_bottom <= y_top) return;
   lv_obj_t *card = lv_obj_create(parent);
   if (!card) return;
   lv_obj_remove_style_all(card);
   lv_obj_set_pos(card, SIDE_PAD - 12, y_top + HDR_H - 4);
   lv_obj_set_size(card, CONTENT_W + 24, (y_bottom - y_top) - HDR_H + 12);
   lv_obj_set_style_bg_color(card, lv_color_hex(0x121620), 0);
   lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
   lv_obj_set_style_radius(card, 16, 0);
   lv_obj_set_style_border_width(card, 1, 0);
   lv_obj_set_style_border_color(card, lv_color_hex(0x1E2030), 0);
   lv_obj_set_style_border_opa(card, LV_OPA_COVER, 0);
   lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
   lv_obj_clear_flag(card, LV_OBJ_FLAG_CLICKABLE);
   /* Send to back so content (labels, switches, chips) renders on top. */
   lv_obj_move_background(card);
}

/** Row with label (single line, vertically centered in ROW_H). */
static void mk_row_label(lv_obj_t *parent, const char *label, int y)
{
    lv_obj_t *lbl = lv_label_create(parent);
    if (!lbl) {
        ESP_LOGW("ui_settings", "mk_row_label: lv_label_create failed (label=%s y=%d)", label, y);
        return;
    }
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, lv_color_hex(TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(lbl, FONT_BODY, 0);
    lv_obj_set_pos(lbl, SIDE_PAD, y + (ROW_H - 20) / 2);
}

/** Right-aligned value text. Returns the label object (or NULL on OOM).
 *  W15-C06 NULL guard. */
static lv_obj_t *mk_row_value(lv_obj_t *parent, const char *text, lv_color_t color, int y)
{
    lv_obj_t *lbl = lv_label_create(parent);
    if (!lbl) {
        ESP_LOGW("ui_settings", "mk_row_value: lv_label_create failed (text=%s y=%d)", text, y);
        return NULL;
    }
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, color, 0);
    lv_obj_set_style_text_font(lbl, FONT_BODY, 0);
    lv_obj_set_pos(lbl, RIGHT_X, y + (ROW_H - 20) / 2);
    return lbl;
}

/* ══════════════════════════════════════════════════════════════════════
 *  Refresh timer
 * ══════════════════════════════════════════════════════════════════════ */

static void settings_refresh_cb(lv_timer_t *t) {
    (void)t;
    feed_wdt();
    if (s_destroying) return;
    if (s_screen && lv_obj_has_flag(s_screen, LV_OBJ_FLAG_HIDDEN)) return;
    ui_settings_update();
}

/* ── Forward declarations ───────────────────────────────────────────── */
static void ntp_sync_task(void *arg);
void ui_settings_hide(void);  /* needed by cb_back_btn before definition */

/* ══════════════════════════════════════════════════════════════════════
 *  Event Callbacks
 * ══════════════════════════════════════════════════════════════════════ */

static void cb_back_btn(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_GESTURE) {
        lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
        if (dir != LV_DIR_RIGHT) return;
    } else if (!ui_tap_gate("settings:back", 300)) {
       /* TT #328 Wave 5 — only debounce CLICKED; gesture handler above
        * already exits early on non-RIGHT dirs and is throttled by LVGL
        * gesture state, so debouncing it would also drop legitimate
        * RIGHT swipes. */
       return;
    }
    /* Hide instead of destroy — destroy+recreate of 55 objects causes
     * internal SRAM heap fragmentation from LVGL alloc/free churn.
     * The overlay stays allocated (hidden) and is re-shown on next open.
     * ~11KB from LVGL expand pool (PSRAM), zero internal SRAM impact. */
    ui_settings_hide();
    /* TT #623 — centralised nav: voice-cancel-then-nav + debounce + obs. */
    tab5_nav_to(NAV_HOME, NAV_FLAGS_NONE);
}

/* ── NVS debounce timer callbacks (US-HW17) ───────────────────────────
 * Slider drag fires VALUE_CHANGED 10-30x/sec.  Hardware updates are
 * instant, but NVS writes need flash sector erases.  We defer the NVS
 * commit until 500ms after the last drag event (one-shot timer).       */

static void brightness_save_cb(lv_timer_t *t)
{
    int val = (int)(intptr_t)lv_timer_get_user_data(t);
    tab5_settings_set_brightness((uint8_t)val);
    ESP_LOGI(TAG, "Brightness %d%% saved to NVS (debounced)", val);
    s_bright_save_timer = NULL;
}

static void volume_save_cb(lv_timer_t *t)
{
    int val = (int)(intptr_t)lv_timer_get_user_data(t);
    tab5_settings_set_volume((uint8_t)val);
    ESP_LOGI(TAG, "Volume %d%% saved to NVS (debounced)", val);
    s_vol_save_timer = NULL;
}

/* v4·D Phase 4e: daily budget cap slider.
 * Slider range 0-50, each step = 20 cents = 20,000 mils.
 *   0  -> cap=0 (OFF, no auto-downgrade)
 *   5  -> $1.00
 *   50 -> $10.00
 * Debounced 500ms like brightness/volume. */
static void cap_save_cb(lv_timer_t *t)
{
    uint32_t mils = (uint32_t)(intptr_t)lv_timer_get_user_data(t);
    tab5_budget_set_cap_mils(mils);
    ESP_LOGI(TAG, "Cap %lu mils saved to NVS (debounced)", (unsigned long)mils);
    s_cap_save_timer = NULL;
}

static void cb_brightness(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int val = lv_slider_get_value(slider);
    tab5_display_set_brightness(val);                       /* instant HW */
    if (s_lbl_bright_val) lv_label_set_text_fmt(s_lbl_bright_val, "%d%%", val);

    /* Restart 500ms debounce for NVS write */
    if (s_bright_save_timer) lv_timer_delete(s_bright_save_timer);
    s_bright_save_timer = lv_timer_create(brightness_save_cb, 500,
                                          (void *)(intptr_t)val);
    lv_timer_set_repeat_count(s_bright_save_timer, 1);
}

static void cb_cap(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int step = lv_slider_get_value(slider);     /* 0..50 */
    uint32_t cents = (uint32_t)step * 20;       /* 20¢ per step */
    uint32_t mils  = cents * 1000;              /* 1 cent = 1000 mils */
    if (s_lbl_cap_val) {
        if (mils == 0) {
            lv_label_set_text(s_lbl_cap_val, "OFF");
        } else {
            int dollars = cents / 100;
            int remc    = cents % 100;
            lv_label_set_text_fmt(s_lbl_cap_val, "$%d.%02d", dollars, remc);
        }
    }
    if (s_cap_save_timer) lv_timer_delete(s_cap_save_timer);
    s_cap_save_timer = lv_timer_create(cap_save_cb, 500,
                                       (void *)(intptr_t)mils);
    lv_timer_set_repeat_count(s_cap_save_timer, 1);
}

static void cb_volume(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int val = lv_slider_get_value(slider);
    tab5_audio_set_volume((uint8_t)val);                    /* instant HW */
    if (s_lbl_vol_val) lv_label_set_text_fmt(s_lbl_vol_val, "%d%%", val);

    /* Restart 500ms debounce for NVS write */
    if (s_vol_save_timer) lv_timer_delete(s_vol_save_timer);
    s_vol_save_timer = lv_timer_create(volume_save_cb, 500,
                                       (void *)(intptr_t)val);
    lv_timer_set_repeat_count(s_vol_save_timer, 1);
}

/* Play a short preview tone on slider release so user hears the new volume (US-PR12) */
static void cb_volume_released(lv_event_t *e)
{
    (void)e;
    tab5_audio_test_tone(440, 200);  /* 440Hz A4 for 200ms */
}

/* #260: camera rotation dropdown — persists to NVS.  ui_camera reads
 * the value at screen-create time, so the new setting applies the next
 * time the user opens the camera (or right away if they're not on it). */
static void cb_cam_rotation(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    uint16_t sel = lv_dropdown_get_selected(dd);
    if (sel > 3) sel = 0;
    ESP_LOGI(TAG, "Camera rotation -> %u (%u deg)",
             (unsigned)sel, (unsigned)(sel * 90));
    tab5_settings_set_cam_rotation((uint8_t)sel);
}

/* Polish P10 / P11 (TT #666): a11y + theme persistence callbacks.
 * Infrastructure-only — applying the value across every TH_* token /
 * FONT_* size / animation site is a future per-screen sweep. */
static void cb_font_scale(lv_event_t *e) {
   lv_obj_t *dd = lv_event_get_target(e);
   uint16_t sel = lv_dropdown_get_selected(dd);
   if (sel > 3) sel = 1;
   ESP_LOGI(TAG, "Font scale tier -> %u", (unsigned)sel);
   tab5_settings_set_font_scale((uint8_t)sel);
   /* P10 follow-up (TT #670): apply-on-restart model — every widget
    * already has its font baked at create-time, so a live tier change
    * isn't visible until the next boot.  Toast prompts a restart. */
   extern void ui_home_show_toast(const char *msg);
   ui_home_show_toast("Text size — restart to apply");
}

static void cb_reduce_motion(lv_event_t *e) {
   lv_obj_t *sw = lv_event_get_target(e);
   bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
   ESP_LOGI(TAG, "Reduce motion %s", on ? "enabled" : "disabled");
   tab5_settings_set_reduce_motion(on);
}

static void cb_theme(lv_event_t *e) {
   lv_obj_t *dd = lv_event_get_target(e);
   uint16_t sel = lv_dropdown_get_selected(dd);
   if (sel > 2) sel = 0;
   ESP_LOGI(TAG, "Theme -> %u", (unsigned)sel);
   tab5_settings_set_theme((uint8_t)sel);
}

/* Vision V2-A.1 (TT #674): always-on vision master toggle.  Persists
 * to NVS via vision_service_set_enabled (which also fires the obs
 * event for harness use). */
static void cb_vision_on(lv_event_t *e) {
   lv_obj_t *sw = lv_event_get_target(e);
   bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
   ESP_LOGI(TAG, "Always-on vision %s", on ? "enabled" : "disabled");
   extern esp_err_t vision_service_set_enabled(bool enabled);
   vision_service_set_enabled(on);
}

/* Vision V2-A.1 (TT #674): inference rate (1 / 2 / 3 Hz). */
static void cb_vision_rate(lv_event_t *e) {
   lv_obj_t *dd = lv_event_get_target(e);
   uint16_t sel = lv_dropdown_get_selected(dd);
   uint8_t hz = (uint8_t)(sel + 1);
   if (hz < 1) hz = 1;
   if (hz > 3) hz = 3;
   ESP_LOGI(TAG, "Vision rate -> %u Hz", (unsigned)hz);
   tab5_settings_set_vision_rate(hz);
}

/* Vision V2-A.3 (TT #680): auto-dim when away. */
static void cb_away_dim(lv_event_t *e) {
   lv_obj_t *sw = lv_event_get_target(e);
   bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
   ESP_LOGI(TAG, "Auto-dim when away %s", on ? "enabled" : "disabled");
   tab5_settings_set_away_dim(on);
}

static void cb_autorotate(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    ESP_LOGI(TAG, "Auto-rotate %s", on ? "enabled" : "disabled");

    /* Audit U2 (#206): persist the toggle to NVS + immediately apply
     * the current IMU-detected orientation.  ui_core's poll timer
     * picks up the new setting and starts/stops applying rotation. */
    tab5_settings_set_auto_rotate(on ? 1 : 0);
    extern void ui_core_apply_auto_rotation(bool enabled);
    ui_core_apply_auto_rotation(on);

    if (s_lbl_orient) {
        if (on) {
            tab5_orientation_t o = tab5_imu_get_orientation();
            const char *names[] = {"Portrait", "Landscape", "Portrait Inv", "Landscape Inv"};
            lv_label_set_text(s_lbl_orient, names[o]);
        } else {
            lv_label_set_text(s_lbl_orient, "Off");
        }
    }
}

/* ── Cap Wave 4 (TT #642): LLM engine chip styling + handlers ──────── */

/* True when the engine chip should accept taps; AUTO is always
 * enabled; K144 needs failover_state==READY; OPENROUTER needs or_key. */
static bool _eng_chip_enabled(int idx) {
   if (idx == LLM_ENG_AUTO) return true;
   if (idx == LLM_ENG_K144) return voice_onboard_failover_state() == 2 /* M5_FAIL_READY */;
   if (idx == LLM_ENG_OPENROUTER) {
      char or_key[96] = {0};
      tab5_settings_get_or_key(or_key, sizeof(or_key));
      return or_key[0] != '\0';
   }
   return false;
}

static void _eng_chip_style(lv_obj_t *chip, bool selected, bool enabled) {
   if (!chip) return;
   if (selected) {
      lv_obj_set_style_border_width(chip, 2, 0);
      lv_obj_set_style_border_color(chip, lv_color_hex(AMBER), 0);
      lv_obj_set_style_border_opa(chip, LV_OPA_COVER, 0);
      lv_obj_set_style_bg_color(chip, lv_color_hex(AMBER), 0);
      lv_obj_set_style_bg_opa(chip, 18, 0);
   } else {
      lv_obj_set_style_border_width(chip, 1, 0);
      lv_obj_set_style_border_color(chip, lv_color_hex(0x1E1E2A), 0);
      lv_obj_set_style_border_opa(chip, LV_OPA_COVER, 0);
      lv_obj_set_style_bg_opa(chip, LV_OPA_TRANSP, 0);
   }
   /* Grey text + reduced opacity when unreachable; reads as "disabled". */
   lv_obj_set_style_opa(chip, enabled ? LV_OPA_COVER : LV_OPA_40, 0);
}

/* Map current vmode to a human-readable STT/TTS engine string.  Called
 * from ui_settings_update on the 2 s refresh tick so the labels stay
 * accurate as the user toggles vmode + the override above. */
static const char *_stt_engine_label(uint8_t vmode) {
   switch (vmode) {
      case 1: /* HYBRID */
      case 2: /* CLOUD */
         return "OpenRouter gpt-audio-mini";
      case 4: /* LOCAL_ONBOARD */
         return "K144 sherpa-ncnn";
      case 5: /* SOLO_DIRECT */
         return "OpenRouter (Solo)";
      case 0: /* LOCAL */
      case 3: /* TINKERCLAW */
      default:
         return "Moonshine (Dragon)";
   }
}

static const char *_tts_engine_label(uint8_t vmode, uint8_t llm_eng) {
   /* LLM engine override doesn't move TTS today (Wave 4b parking); the
    * read-out is determined by vmode only. */
   (void)llm_eng;
   switch (vmode) {
      case 1: /* HYBRID */
      case 2: /* CLOUD */
         return "OpenRouter gpt-audio-mini";
      case 4: /* LOCAL_ONBOARD */
         return "K144 single_speaker_english_fast";
      case 5: /* SOLO_DIRECT */
         return "OpenRouter (Solo)";
      case 0: /* LOCAL */
      case 3: /* TINKERCLAW */
      default:
         return "Piper (Dragon)";
   }
}

/* TT #724 (3.2): the read-only mode row opens the picker — the single mode
 * authority. Settings no longer hosts a second radio. */
static void cb_open_mode_picker(lv_event_t *e) {
   (void)e;
   extern void ui_mode_sheet_show(void);
   ui_mode_sheet_show();
}

static void cb_mic_mute(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    tab5_settings_set_mic_mute(on ? 1 : 0);
    ESP_LOGI(TAG, "Mic mute: %d", on);
    extern void ui_home_refresh_sys_label(void);
    ui_home_refresh_sys_label();
}

/* TT #586 — TinkerON always-on listener toggle.  ON calls
 * voice_onboard_arm_wakeword (re-uses the canonical event handler
 * from voice_onboard.c so toasts + orb ripple stay consistent).
 * OFF calls voice_wakeword_stop. */
static void cb_tinkeron_armed(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    ESP_LOGI(TAG, "TinkerON listener: %s", on ? "ARM" : "STOP");
    if (on) {
        esp_err_t err = voice_onboard_arm_wakeword();
        if (err == ESP_ERR_INVALID_STATE) {
            ui_home_show_toast("TinkerON unavailable — try Reset");
            /* Revert switch — listener didn't actually arm. */
            lv_obj_remove_state(sw, LV_STATE_CHECKED);
        } else if (err != ESP_OK) {
            ESP_LOGW(TAG, "arm_wakeword failed: %s", esp_err_to_name(err));
        }
    } else {
        voice_wakeword_stop();
    }
}

/* TT #586 — Reset TinkerON button.  Same path as POST /m5/reset:
 * sys.reset to the StackFlow daemon + auto re-warmup + re-arm. */
static void cb_tinkeron_reset(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "TinkerON reset requested");
    esp_err_t err = voice_onboard_reset_failover();
    if (err == ESP_OK) {
        ui_home_show_toast("TinkerON resetting…");
    } else if (err == ESP_ERR_INVALID_STATE) {
        ui_home_show_toast("TinkerON already resetting");
    } else {
        ESP_LOGW(TAG, "reset_failover failed: %s", esp_err_to_name(err));
    }
}

static void cb_quiet_on(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    tab5_settings_set_quiet_on(on ? 1 : 0);
    ESP_LOGI(TAG, "Quiet hours: %d", on);
    extern void ui_home_refresh_sys_label(void);
    ui_home_refresh_sys_label();
}

/* Cap Wave 5 (TT #644): Privacy lock master switch.  Flips the NVS
 * value + refreshes the home top-bar indicator. */
static lv_obj_t *s_lbl_privacy_status = NULL;

static void cb_privacy_lock(lv_event_t *e) {
   lv_obj_t *sw = lv_event_get_target(e);
   bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
   tab5_settings_set_privacy_lock(on);
   ESP_LOGI(TAG, "Privacy lock: %d", on);
   tab5_debug_obs_event("privacy.lock", on ? "on" : "off");
   if (s_lbl_privacy_status) {
      lv_label_set_text(s_lbl_privacy_status, on ? "ON · On-device only" : "OFF · Cloud allowed");
      lv_obj_set_style_text_color(s_lbl_privacy_status, lv_color_hex(on ? 0xF59E0B : TEXT_DIM), 0);
   }
   /* Nudge the home pill so the new state is reflected without a 5 s
    * polling delay. */
   extern void ui_home_refresh_sys_label(void);
   ui_home_refresh_sys_label();
}

/* W7-E.5: per-channel notification toggles.  Each callback persists to
 * NVS via the matching tab5_settings_set_ch_*_on; the notification
 * router (ui_notification.c) reads the value at incoming-message time,
 * so toggling takes effect instantly without re-render. */
#define CHANNEL_TOGGLE_CB(short_name, fn_name)          \
   static void cb_ch_##fn_name##_on(lv_event_t *e) {    \
      lv_obj_t *sw = lv_event_get_target(e);            \
      bool on = lv_obj_has_state(sw, LV_STATE_CHECKED); \
      tab5_settings_set_ch_##fn_name##_on(on ? 1 : 0);  \
      ESP_LOGI(TAG, "Channel " short_name ": %d", on);  \
   }

CHANNEL_TOGGLE_CB("tg", tg)
CHANNEL_TOGGLE_CB("wa", wa)
CHANNEL_TOGGLE_CB("dc", dc)
CHANNEL_TOGGLE_CB("sl", sl)
CHANNEL_TOGGLE_CB("sg", sg)
CHANNEL_TOGGLE_CB("im", im)
CHANNEL_TOGGLE_CB("ma", ma)
CHANNEL_TOGGLE_CB("em", em)

/* Audit U3 (#206): start/end-hour sliders.  Both fire on
 * LV_EVENT_VALUE_CHANGED while dragging, so we update the label live
 * AND persist to NVS on every step.  ui_home re-reads on next sys-label
 * refresh, so no extra notification needed. */
static lv_obj_t *s_lbl_quiet_start_val = NULL;
static lv_obj_t *s_lbl_quiet_end_val   = NULL;

static void cb_quiet_start(lv_event_t *e)
{
    lv_obj_t *s = lv_event_get_target(e);
    int v = lv_slider_get_value(s);
    if (v < 0 || v > 23) return;
    tab5_settings_set_quiet_start((uint8_t)v);
    if (s_lbl_quiet_start_val) {
        lv_label_set_text_fmt(s_lbl_quiet_start_val, "%02d:00", v);
    }
    extern void ui_home_refresh_sys_label(void);
    ui_home_refresh_sys_label();
}

static void cb_quiet_end(lv_event_t *e)
{
    lv_obj_t *s = lv_event_get_target(e);
    int v = lv_slider_get_value(s);
    if (v < 0 || v > 23) return;
    tab5_settings_set_quiet_end((uint8_t)v);
    if (s_lbl_quiet_end_val) {
        lv_label_set_text_fmt(s_lbl_quiet_end_val, "%02d:00", v);
    }
    extern void ui_home_refresh_sys_label(void);
    ui_home_refresh_sys_label();
}

/* ── WiFi picker ────────────────────────────────────────────────────── */

static void cb_wifi_setup(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Launching WiFi setup screen");
    extern lv_obj_t *ui_wifi_create(void);
    ui_wifi_create();
}

/* ── Connection mode ────────────────────────────────────────────────── */

static void cb_conn_mode(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    uint32_t sel = lv_dropdown_get_selected(dd);
    tab5_settings_set_connection_mode((uint8_t)sel);
    ESP_LOGI(TAG, "Connection mode: %d (%s)",
             (int)sel, sel == 0 ? "auto" : sel == 1 ? "local" : "remote");
    /* U21 (#206): re-target the live WS URI so the new mode (esp.
     * "Internet Only") takes effect immediately.  Without this, the
     * existing WS stays on whatever URI was picked at boot and the
     * dropdown change is purely cosmetic until next power-cycle. */
    voice_reapply_connection_mode();
}

/* ── Dragon host ────────────────────────────────────────────────────── */

static lv_obj_t *s_dragon_status_lbl = NULL;  /* reconnect feedback label (US-PR09) */

static void cb_dragon_host_done(lv_event_t *e)
{
    (void)e;
    if (!s_dragon_ta) return;
    const char *txt = lv_textarea_get_text(s_dragon_ta);
    if (txt && txt[0]) {
        tab5_settings_set_dragon_host(txt);
        ESP_LOGI(TAG, "Dragon host saved: %s", txt);
        if (voice_is_connected()) {
            ESP_LOGI(TAG, "Disconnecting voice for host change -> watchdog will reconnect");
            voice_disconnect();
        }
        /* US-PR09: Show reconnect feedback below Dragon host field */
        if (s_dragon_status_lbl) {
            char msg[96];
            snprintf(msg, sizeof(msg), "Reconnecting to %s...", txt);
            lv_label_set_text(s_dragon_status_lbl, msg);
            lv_obj_set_style_text_color(s_dragon_status_lbl, lv_color_hex(ACC_NETWORK), 0);
            lv_obj_clear_flag(s_dragon_status_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    }
    ui_keyboard_hide();
}

static void cb_dragon_host_click(lv_event_t *e)
{
    (void)e;
    if (s_dragon_ta) {
        ui_keyboard_show(s_dragon_ta);
    }
}

/* ── Keyboard layout callback — shrink scroll area above keyboard ── */
static void settings_keyboard_layout_cb(bool visible, int kb_height)
{
    if (!s_scroll) return;

    if (visible) {
        /* Shrink scroll container so it ends above keyboard */
        lv_obj_set_height(s_scroll, USABLE_H - TOPBAR_H - kb_height);

        /* Scroll to make dragon_ta visible if it exists */
        if (s_dragon_ta) {
            lv_obj_scroll_to_view(s_dragon_ta, LV_ANIM_ON);
        }
    } else {
        /* Restore full height */
        lv_obj_set_height(s_scroll, OVERLAY_H - TOPBAR_H);
    }
}

/* ── NTP sync ───────────────────────────────────────────────────────── */

static void cb_ntp_sync(lv_event_t *e)
{
    (void)e;
    if (s_ntp_spinner) return;
    if (s_ntp_btn_label) {
        lv_label_set_text(s_ntp_btn_label, "Syncing...");
    }
    xTaskCreate(ntp_sync_task, "ntp_sync", 4096, NULL, 5, NULL);
}

static void ntp_sync_task(void *arg)
{
    (void)arg;
    esp_err_t ret = tab5_rtc_sync_from_ntp();

    if (s_destroying) { vTaskSuspend(NULL); return; }

    tab5_ui_lock();
    if (s_destroying || !s_ntp_btn_label) { tab5_ui_unlock(); vTaskSuspend(NULL); return; }
    if (s_ntp_btn_label) {
        if (ret == ESP_OK) {
            lv_label_set_text(s_ntp_btn_label, "Synced!");
            ESP_LOGI(TAG, "NTP sync OK");
        } else {
            lv_label_set_text(s_ntp_btn_label, "Failed");
            ESP_LOGW(TAG, "NTP sync failed: %s", esp_err_to_name(ret));
        }
    }
    s_ntp_spinner = NULL;
    tab5_ui_unlock();
    vTaskSuspend(NULL);
}

/* ── OTA ────────────────────────────────────────────────────────────── */

/* OTA progress update via lv_async_call (thread-safe LVGL access) */
typedef struct {
    int percent;
    char phase[16];
} ota_progress_msg_t;

static void ota_progress_async_cb(void *arg)
{
    ota_progress_msg_t *msg = (ota_progress_msg_t *)arg;
    if (!msg) return;
    if (s_destroying || !s_ota_btn_label) { free(msg); return; }

    if (strcmp(msg->phase, "download") == 0) {
        lv_label_set_text_fmt(s_ota_btn_label, "Downloading... %d%%", msg->percent);
    } else if (strcmp(msg->phase, "verify") == 0) {
        lv_label_set_text(s_ota_btn_label, "Verifying...");
    } else if (strcmp(msg->phase, "reboot") == 0) {
        lv_label_set_text(s_ota_btn_label, "Rebooting...");
    }
    free(msg);
}

static void ota_progress_cb(int percent, const char *phase)
{
    if (s_destroying) return;
    ota_progress_msg_t *msg = malloc(sizeof(ota_progress_msg_t));
    if (!msg) return;
    msg->percent = percent;
    strncpy(msg->phase, phase, sizeof(msg->phase) - 1);
    msg->phase[sizeof(msg->phase) - 1] = '\0';
    tab5_lv_async_call(ota_progress_async_cb, msg);
}

static void ota_apply_task(void *arg)
{
    ESP_LOGI(TAG, "OTA apply: scheduling %s for next boot", s_ota_url);
    /* Wave 10 #77 extended-use fix: schedule and reboot. The fresh boot
     * applies the stored OTA before any heavy subsystems have claimed
     * DMA-capable internal SRAM. If we stay here and call tab5_ota_apply
     * directly, a user applying after a 30+ min session hits the
     * "esp_dma_capable_malloc: Not enough heap memory" path and the
     * watchdog reboots without the firmware actually landing. */
    esp_err_t err = tab5_ota_schedule(s_ota_url,
                                      s_ota_sha256[0] ? s_ota_sha256 : NULL);
    /* tab5_ota_schedule esp_restarts on success; if we're here, failed. */
    ESP_LOGE(TAG, "OTA schedule failed: %s", esp_err_to_name(err));
    if (s_destroying) { vTaskSuspend(NULL); return; }
    tab5_ui_lock();
    if (s_ota_btn_label) lv_label_set_text(s_ota_btn_label, "Update failed!");
    if (s_ota_apply_btn) lv_obj_clear_flag(s_ota_apply_btn, LV_OBJ_FLAG_HIDDEN);
    tab5_ui_unlock();
    vTaskSuspend(NULL);
}

/* OTA confirmation dialog callbacks (US-PR19) */
static void cb_ota_cancel(lv_event_t *e)
{
    lv_obj_t *mbox = lv_event_get_user_data(e);
    if (mbox) lv_msgbox_close(mbox);
}

static void cb_ota_confirm(lv_event_t *e)
{
    lv_obj_t *mbox = lv_event_get_user_data(e);
    if (mbox) lv_msgbox_close(mbox);

    /* Proceed with actual OTA apply */
    if (!s_ota_url[0]) return;
    if (s_ota_btn_label) lv_label_set_text(s_ota_btn_label, "Updating...");
    if (s_ota_apply_btn) lv_obj_add_flag(s_ota_apply_btn, LV_OBJ_FLAG_HIDDEN);
    xTaskCreate(ota_apply_task, "ota_apply", 8192, NULL, 5, NULL);
}

static void cb_ota_apply(lv_event_t *e)
{
    (void)e;
    if (!s_ota_url[0]) return;

    /* Show confirmation dialog instead of applying immediately (US-PR19) */
    lv_obj_t *mbox = lv_msgbox_create(NULL);
    lv_msgbox_add_title(mbox, "Update Firmware?");
    lv_msgbox_add_text(mbox, "Device will restart after update.\nDo not unplug during update.");
    lv_obj_t *btn_cancel = lv_msgbox_add_footer_button(mbox, "Cancel");
    lv_obj_t *btn_update = lv_msgbox_add_footer_button(mbox, "Update");
    lv_obj_add_event_cb(btn_cancel, cb_ota_cancel, LV_EVENT_CLICKED, mbox);
    lv_obj_add_event_cb(btn_update, cb_ota_confirm, LV_EVENT_CLICKED, mbox);

    /* Style the dialog for Material Dark theme */
    lv_obj_set_style_bg_color(mbox, lv_color_hex(CARD_COLOR), 0);
    lv_obj_set_style_radius(mbox, 16, 0);
    lv_obj_set_style_border_width(mbox, 1, 0);
    lv_obj_set_style_border_color(mbox, lv_color_hex(0x1A1A24), 0);
    lv_obj_set_width(mbox, 400);

    /* Style title */
    lv_obj_t *title = lv_msgbox_get_title(mbox);
    if (title) {
        lv_obj_set_style_text_color(title, lv_color_hex(TEXT_PRIMARY), 0);
        lv_obj_set_style_text_font(title, FONT_BODY, 0);
    }

    /* Style content area */
    lv_obj_t *content = lv_msgbox_get_content(mbox);
    if (content) {
        lv_obj_set_style_text_color(content, lv_color_hex(TEXT_DIM), 0);
        lv_obj_set_style_text_font(content, FONT_CAPTION, 0);
    }

    /* Style header */
    lv_obj_t *header = lv_msgbox_get_header(mbox);
    if (header) {
        lv_obj_set_style_bg_color(header, lv_color_hex(CARD_COLOR), 0);
    }

    /* Style footer */
    lv_obj_t *footer = lv_msgbox_get_footer(mbox);
    if (footer) {
        lv_obj_set_style_bg_color(footer, lv_color_hex(CARD_COLOR), 0);
    }
}

static void ota_check_task(void *arg)
{
    tab5_ota_info_t info;
    esp_err_t err = tab5_ota_check(&info);

    if (s_destroying) { vTaskSuspend(NULL); return; }

    tab5_ui_lock();
    if (s_destroying || !s_ota_btn_label) { tab5_ui_unlock(); vTaskSuspend(NULL); return; }

    if (err != ESP_OK) {
        lv_label_set_text(s_ota_btn_label, "Check failed");
        if (s_ota_apply_btn) lv_obj_add_flag(s_ota_apply_btn, LV_OBJ_FLAG_HIDDEN);
    } else if (info.available) {
        char buf[64];
        snprintf(buf, sizeof(buf), LV_SYMBOL_OK " v%s available!", info.version);
        lv_label_set_text(s_ota_btn_label, buf);
        snprintf(s_ota_url, sizeof(s_ota_url), "%s", info.url);
        strncpy(s_ota_sha256, info.sha256, sizeof(s_ota_sha256) - 1);
        s_ota_sha256[sizeof(s_ota_sha256) - 1] = '\0';
        if (s_ota_apply_btn) lv_obj_clear_flag(s_ota_apply_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(s_ota_btn_label, LV_SYMBOL_OK " Up to date");
        if (s_ota_apply_btn) lv_obj_add_flag(s_ota_apply_btn, LV_OBJ_FLAG_HIDDEN);
    }
    tab5_ui_unlock();
    vTaskSuspend(NULL);
}

static void cb_ota_check(lv_event_t *e)
{
    (void)e;
    if (s_ota_btn_label) {
        lv_label_set_text(s_ota_btn_label, "Checking...");
    }
    if (s_ota_apply_btn) lv_obj_add_flag(s_ota_apply_btn, LV_OBJ_FLAG_HIDDEN);
    xTaskCreate(ota_check_task, "ota_check", 8192, NULL, 5, NULL);
}

/* Wave 4: force-show onboarding carousel (audit G follow-up).
 * Lets the user review the intro after first-boot without NVS erase. */
void cb_replay_intro(lv_event_t *e)
{
    (void)e;
    extern void ui_settings_hide(void);
    extern void ui_onboarding_force_show(void);
    /* Hide Settings first so the onboarding card isn't stacked
     * underneath it visually. */
    ui_settings_hide();
    ui_onboarding_force_show();
}

/* ══════════════════════════════════════════════════════════════════════
 *  Inline style helpers
 * ══════════════════════════════════════════════════════════════════════ */

/** Create a Material Dark slider: 200px wide, 4px track, 16px accent knob.
 *
 *  Wave 15 W15-C06: NULL-guard on `lv_slider_create` — returns NULL
 *  silently so caller sees a missing slider but the device doesn't
 *  panic on the next `lv_obj_set_pos(NULL, ...)`. */
static lv_obj_t *mk_slider(lv_obj_t *parent, lv_color_t accent, int x, int y,
                            int min, int max, int val, lv_event_cb_t cb)
{
    lv_obj_t *s = lv_slider_create(parent);
    if (!s) {
        ESP_LOGW("ui_settings", "mk_slider: lv_slider_create failed (x=%d y=%d)", x, y);
        return NULL;
    }
    lv_obj_set_pos(s, x, y + (ROW_H - 4) / 2 - 2);
    lv_obj_set_size(s, 200, 4);
    lv_slider_set_range(s, min, max);
    lv_slider_set_value(s, val, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s, lv_color_hex(CARD_COLOR), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s, accent, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s, accent, LV_PART_KNOB);
    lv_obj_set_style_radius(s, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(s, 2, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s, 8, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s, 6, LV_PART_KNOB);
    lv_obj_add_event_cb(s, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return s;
}

/** Create a Material Dark toggle switch: 44x24px.  W15-C06 NULL-guard. */
static lv_obj_t *mk_switch(lv_obj_t *parent, lv_color_t accent, int x, int y,
                            bool checked, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *sw = lv_switch_create(parent);
    if (!sw) {
        ESP_LOGW("ui_settings", "mk_switch: lv_switch_create failed");
        return NULL;
    }
    lv_obj_set_pos(sw, x, y + (ROW_H - 24) / 2);
    lv_obj_set_size(sw, 44, 24);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x1A1A24), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, accent, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, lv_color_hex(TEXT_PRIMARY), LV_PART_KNOB);
    lv_obj_set_style_radius(sw, 12, LV_PART_MAIN);
    lv_obj_set_style_radius(sw, 12, LV_PART_INDICATOR);
    lv_obj_set_style_radius(sw, 10, LV_PART_KNOB);
    if (checked) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, user_data);
    return sw;
}

/** Create a pill-shaped button with centered text.  W15-C06 NULL guards. */
static lv_obj_t *mk_pill_btn(lv_obj_t *parent, const char *text, lv_color_t bg,
                              lv_color_t text_color, int x, int y, int w, int h,
                              int radius, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    if (!btn) {
        ESP_LOGW("ui_settings", "mk_pill_btn: lv_button_create failed (text=%s)", text);
        return NULL;
    }
    lv_obj_remove_style_all(btn);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, radius, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(btn);
    if (lbl) {
        lv_label_set_text(lbl, text);
        lv_obj_set_style_text_color(lbl, text_color, 0);
        lv_obj_set_style_text_font(lbl, FONT_CAPTION, 0);
        lv_obj_center(lbl);
    }

    return btn;
}

/* ══════════════════════════════════════════════════════════════════════
 *  Public API
 * ══════════════════════════════════════════════════════════════════════ */

/* ── Phase 2 timer callback: create remaining sections ──────────────── */
static void phase2_timer_cb(lv_timer_t *t)
{
    (void)t;
    s_phase2_timer = NULL;

    /* Guard: screen destroyed/hidden before Phase 2 fires */
    if (!s_phase2_pending || s_destroying || !s_scroll) {
        ESP_LOGW(TAG, "Phase 2 skipped (screen gone)");
        s_phase2_pending = false;
        s_creating = false;
        return;
    }

    ESP_LOGI(TAG, "Phase 2: creating Voice + Storage + Battery + About...");
    int y = s_phase2_y;

    /* ════════════════════════════════════════════════════════════════
     *  SECTION: STORAGE (amber #F59E0B)
     * ════════════════════════════════════════════════════════════════ */
    feed_wdt();
    ESP_LOGI(TAG, "Section: Storage");
    lv_color_t acc_storage = lv_color_hex(ACC_STORAGE);

    int storage_section_top = y;
    y = mk_section(s_scroll, "STORAGE", acc_storage, y);

    /* SD info shown directly under section header */
    s_lbl_sd_info = lv_label_create(s_scroll);
    lv_label_set_text(s_lbl_sd_info, "SD: Checking...");
    lv_obj_set_style_text_color(s_lbl_sd_info, lv_color_hex(TEXT_DIM), 0);
    lv_obj_set_style_text_font(s_lbl_sd_info, FONT_BODY, 0);
    lv_obj_set_pos(s_lbl_sd_info, SIDE_PAD, y + (ROW_H - 18) / 2);
    y += ROW_H + 16;
    mk_card_bg(s_scroll, storage_section_top, y);
    y += 24;

    /* ════════════════════════════════════════════════════════════════
     *  SECTION: BATTERY (red #EF4444)
     * ════════════════════════════════════════════════════════════════ */
    feed_wdt();
    ESP_LOGI(TAG, "Section: Battery");
    lv_color_t acc_battery = lv_color_hex(ACC_BATTERY);

    int battery_section_top = y;
    y = mk_section(s_scroll, "BATTERY", acc_battery, y);

    /* Primary battery status: "XX% . Charging" or "USB Powered" */
    s_lbl_bat_status = lv_label_create(s_scroll);
    lv_label_set_text(s_lbl_bat_status, "Checking...");
    lv_obj_set_style_text_color(s_lbl_bat_status, lv_color_hex(TEXT_DIM), 0);
    lv_obj_set_style_text_font(s_lbl_bat_status, FONT_BODY, 0);
    lv_obj_set_pos(s_lbl_bat_status, SIDE_PAD, y + (ROW_H - 18) / 2);
    y += ROW_H + 4;

    /* Level with bar + percentage */
    s_bar_bat_level = lv_bar_create(s_scroll);
    lv_obj_set_pos(s_bar_bat_level, SIDE_PAD, y + (ROW_H - 12) / 2);
    lv_obj_set_size(s_bar_bat_level, 400, 12);
    lv_bar_set_range(s_bar_bat_level, 0, 100);
    lv_bar_set_value(s_bar_bat_level, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_bar_bat_level, lv_color_hex(CARD_COLOR), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar_bat_level, acc_battery, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bar_bat_level, 6, LV_PART_MAIN);
    lv_obj_set_style_radius(s_bar_bat_level, 6, LV_PART_INDICATOR);
    s_lbl_bat_pct = lv_label_create(s_scroll);
    lv_label_set_text(s_lbl_bat_pct, "0%");
    lv_obj_set_style_text_color(s_lbl_bat_pct, lv_color_hex(TEXT_DIM), 0);
    lv_obj_set_style_text_font(s_lbl_bat_pct, FONT_BODY, 0);
    lv_obj_set_pos(s_lbl_bat_pct, SIDE_PAD + 415, y + (ROW_H - 18) / 2);
    y += ROW_H + 2;

    /* Debug voltage label -- small and dim for developers */
    s_lbl_bat_volt = lv_label_create(s_scroll);
    lv_label_set_text(s_lbl_bat_volt, "");
    lv_obj_set_style_text_color(s_lbl_bat_volt, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_font(s_lbl_bat_volt, FONT_SMALL, 0);
    lv_obj_set_pos(s_lbl_bat_volt, SIDE_PAD, y);
    y += 20 + 16;
    mk_card_bg(s_scroll, battery_section_top, y);
    y += 24;

    /* ════════════════════════════════════════════════════════════════
     *  SECTION: ABOUT (violet #8B5CF6)
     * ════════════════════════════════════════════════════════════════ */
    feed_wdt();
    ESP_LOGI(TAG, "Section: About");
    lv_color_t acc_about = lv_color_hex(ACC_ABOUT);

    int about_section_top = y;
    y = mk_section(s_scroll, "ABOUT", acc_about, y);

    /* Device info -- Line 1: product name + version (prominent) */
    {
        char ver_str[64];
        snprintf(ver_str, sizeof(ver_str), "TinkerTab v%s", TAB5_FIRMWARE_VER);
        lv_obj_t *ver_lbl = lv_label_create(s_scroll);
        lv_label_set_text(ver_lbl, ver_str);
        lv_obj_set_style_text_color(ver_lbl, lv_color_hex(TEXT_PRIMARY), 0);
        lv_obj_set_style_text_font(ver_lbl, FONT_HEADING, 0);
        lv_obj_set_pos(ver_lbl, SIDE_PAD, y + 2);
    }
    y += 28;
    /* Device info -- Line 2: hardware details (smaller, gray) */
    {
        char hw_str[96];
        snprintf(hw_str, sizeof(hw_str),
                 "M5Stack Tab5 \xc2\xb7 ESP32-P4 \xc2\xb7 %s", tab5_ota_current_partition());
        lv_obj_t *hw_lbl = lv_label_create(s_scroll);
        lv_label_set_text(hw_lbl, hw_str);
        lv_obj_set_style_text_color(hw_lbl, lv_color_hex(TEXT_DIM), 0);
        lv_obj_set_style_text_font(hw_lbl, FONT_SMALL, 0);
        lv_obj_set_pos(hw_lbl, SIDE_PAD, y);
    }
    y += 24;

    /* OTA Update -- full-width Check button */
    {
        lv_obj_t *ota_btn = mk_pill_btn(s_scroll, LV_SYMBOL_DOWNLOAD " Check Update",
                                        acc_about, lv_color_hex(TEXT_PRIMARY),
                                        SIDE_PAD, y + 3, CONTENT_W, 42, 8, cb_ota_check);
        s_ota_btn_label = lv_obj_get_child(ota_btn, 0);
    }
    y += ROW_H + 8;
    {
        s_ota_apply_btn = mk_pill_btn(s_scroll, LV_SYMBOL_OK " Apply Update",
                                      lv_color_hex(TAB_LOCAL), lv_color_hex(TEXT_PRIMARY),
                                      SIDE_PAD, y + 3, CONTENT_W, 42, 8, cb_ota_apply);
        lv_obj_add_flag(s_ota_apply_btn, LV_OBJ_FLAG_HIDDEN);
    }
    y += ROW_H + 8;

    /* "Show intro again" row — re-triggers the onboarding carousel
     * without requiring an NVS erase.  Useful after a factory-reset-
     * like setup or when demoing the product.
     *
     * U17 (#206): was rendered in dim grey on dim grey ("buried"
     * per the audit).  Promoted to the section-accent color (amber)
     * so users can find it without scanning every row in About. */
    {
        extern void cb_replay_intro(lv_event_t *e);
        lv_obj_t *intro_btn = mk_pill_btn(s_scroll,
                                          LV_SYMBOL_REFRESH " Show intro again",
                                          acc_about,
                                          lv_color_hex(TEXT_PRIMARY),
                                          SIDE_PAD, y + 3, CONTENT_W, 42, 8,
                                          cb_replay_intro);
        (void)intro_btn;
    }
    y += ROW_H + 8;

    /* Free Heap + PSRAM (debug info -- small and dim) */
    s_lbl_heap = lv_label_create(s_scroll);
    lv_label_set_text(s_lbl_heap, "Heap: -- KB  |  PSRAM: -- MB");
    lv_obj_set_style_text_color(s_lbl_heap, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_font(s_lbl_heap, FONT_SMALL, 0);
    lv_obj_set_pos(s_lbl_heap, SIDE_PAD, y + (ROW_H - 14) / 2);
    y += ROW_H;
    mk_card_bg(s_scroll, about_section_top, y);
    y += 24;

    /* ── Bottom padding ──────────────────────────────────────────────── */
    y += 40;

    s_phase2_pending = false;
    s_phase2_done    = true;     /* U13 (#206): authoritative sentinel */
    ESP_LOGI(TAG, "Phase 2 complete (%lu objects total)", (unsigned long)lv_obj_get_child_count(s_scroll));

    /* Trigger immediate data refresh for the newly created labels */
    ui_settings_update();

    /* Mark creation complete — touch is already enabled from Phase 1 */
    s_creating = false;
}

lv_obj_t *ui_settings_create(void)
{
    if (s_creating) {
        ESP_LOGW(TAG, "Settings creation already in progress — skipping");
        return s_screen;
    }

    /* Wave 15 W15-C06: refuse to start a fresh creation pass when
     * internal heap is so low that a child widget alloc is likely to
     * return NULL.  Settings renders ~55 LVGL objects (big box of
     * labels, sliders, dropdowns, switches); every one of those that
     * returns NULL must then not be dereferenced.  Guarding every
     * caller is impractical, so we bail once upfront.
     *
     * Threshold chosen from live observation: /heap showed 6 KB free
     * when the screen crashed.  8 KB gives a small cushion without
     * being so conservative that the screen refuses to open in any
     * real-world condition.  Returning NULL from here makes the
     * navigation tap appear to "do nothing" instead of rebooting
     * the device mid-use. */
    if (!s_screen) {
        size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (internal_free < 8 * 1024) {
            ESP_LOGW(TAG, "Settings: deferring create — internal heap too low (%u bytes free)",
                     (unsigned)internal_free);
            extern void ui_home_show_toast(const char *text);
            tab5_lv_async_call((lv_async_cb_t)ui_home_show_toast,
                          (void *)"Low memory — reboot Tab5 to open Settings");
            return NULL;
        }
    }

    if (s_screen) {
        /* Overlay already exists — just unhide and resume refresh */
        ESP_LOGI(TAG, "Settings screen resumed");
        s_destroying = false;
        lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_screen, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_move_foreground(s_screen);
        /* Recreate refresh timer (deleted on hide to prevent race conditions) */
        if (!s_refresh_timer) s_refresh_timer = lv_timer_create(settings_refresh_cb, 2000, NULL);
        else lv_timer_resume(s_refresh_timer);
        ui_settings_update();
        ui_keyboard_set_layout_cb(settings_keyboard_layout_cb);

        /* U13 (#206): re-schedule Phase 2 if it didn't complete cleanly.
         * Use the explicit s_phase2_done sentinel rather than the old
         * !s_lbl_heap heuristic, which was both false-positive (heap
         * label exists but other rows didn't make it) and false-negative
         * (phase failed before reaching the heap label). */
        if (!s_phase2_done && s_scroll && !s_phase2_pending) {
            ESP_LOGI(TAG, "Phase 2 incomplete — re-scheduling");
            s_creating = true;
            s_phase2_pending = true;
            s_phase2_timer = lv_timer_create(phase2_timer_cb, 50, NULL);
            lv_timer_set_repeat_count(s_phase2_timer, 1);
        }
        return s_screen;
    }
    ESP_LOGI(TAG, "Creating settings screen (two-pass)...");
    s_creating = true;
    s_destroying = false;

    /* ═══════════════════════════════════════════════════════════════════
     *  PHASE 1: Container + Display + Network (~15 objects, <100ms)
     *  User sees the screen immediately after this returns.
     * ═══════════════════════════════════════════════════════════════════ */

    /* ── Fullscreen overlay on home screen ───────────────────────────── */
    lv_obj_t *home = ui_home_get_screen();
    s_screen = lv_obj_create(home);
    if (!s_screen) {
        ESP_LOGE(TAG, "OOM: failed to create settings screen");
        s_creating = false;
        return NULL;
    }
    lv_obj_remove_style_all(s_screen);
    lv_obj_set_size(s_screen, 720, OVERLAY_H);
    lv_obj_set_pos(s_screen, 0, 0);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(BG_COLOR), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(s_screen);

    /* Swipe-right to go back */
    lv_obj_add_event_cb(s_screen, cb_back_btn, LV_EVENT_GESTURE, NULL);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_GESTURE_BUBBLE);

    /* ── Top bar (v5: ghost HOME back + amber right-aligned title) ──── */
    lv_obj_t *bar = lv_obj_create(s_screen);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 720, TOPBAR_H);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(BG_COLOR), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(0x1A1A24), 0); /* TH_HAIRLINE */
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    /* v5 spec shot-09: Settings title LEFT, '← HOME' caption RIGHT. */
    lv_obj_t *title = lv_label_create(bar);
    lv_label_set_text(title, "Settings");
    lv_obj_set_style_text_color(title, lv_color_hex(0xF59E0B), 0); /* TH_AMBER */
    lv_obj_set_style_text_font(title, FONT_TITLE, 0);              /* 28 px */
    lv_obj_set_style_text_letter_space(title, -1, 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 24, 0);

    /* TT #328 Wave 10 — back btn was 140×TOPBAR_H (48 = below TOUCH_MIN
     * 60).  Topbar height itself stays at 48 (full layout depends on
     * it); set_ext_click_area grows ONLY the hit area to TOUCH_MIN
     * without disturbing visual layout. */
    lv_obj_t *back_btn = lv_button_create(bar);
    lv_obj_set_size(back_btn, 140, TOPBAR_H);
    lv_obj_set_ext_click_area(back_btn, (TOUCH_MIN - TOPBAR_H + 1) / 2);
    lv_obj_align(back_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(back_btn, 0, 0);
    lv_obj_set_style_border_width(back_btn, 0, 0);
    lv_obj_add_event_cb(back_btn, cb_back_btn, LV_EVENT_CLICKED, NULL);
    ui_fb_icon(back_btn); /* TT #328 Wave 10 */

    lv_obj_t *arrow = lv_label_create(back_btn);
    lv_label_set_text(arrow, LV_SYMBOL_LEFT "  HOME");
    lv_obj_set_style_text_color(arrow, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_font(arrow, FONT_CAPTION, 0);
    lv_obj_set_style_text_letter_space(arrow, 3, 0);
    lv_obj_center(arrow);

    /* ── Scrollable content area ─────────────────────────────────────── */
    s_scroll = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_scroll);
    lv_obj_set_size(s_scroll, 720, USABLE_H - TOPBAR_H);
    lv_obj_set_pos(s_scroll, 0, TOPBAR_H);
    lv_obj_set_style_bg_opa(s_scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_scroll, 0, 0);
    lv_obj_add_flag(s_scroll, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_scroll, LV_SCROLLBAR_MODE_AUTO);

    int y = 12;

    /* ════════════════════════════════════════════════════════════════
     *  SECTION: VOICE MODE  (v5 spec shot-09: first-class picker at top)
     * ════════════════════════════════════════════════════════════════ */
    feed_wdt();
    ESP_LOGI(TAG, "Phase 1 — Section: Voice Mode");
    lv_color_t acc_voice = lv_color_hex(ACC_VOICE);

    /* Cap Wave 5 (TT #644): track VOICE MODE + CLOUD LLM block's top y
     * so we can paint a single airy card behind the combined picker
     * region (header → mode rows → cloud chips). */
    int s_voice_section_top = y;
    y = mk_section(s_scroll, "VOICE MODE", acc_voice, y);

    /* TT #724 (3.2/3.5): the picker is the single mode authority now. Settings
     * shows a read-only current-mode row that opens it; the old radio plus the
     * ENGINES + CLOUD LLM pickers moved into the picker (Advanced drawer). */
    {
       extern const uint32_t th_mode_colors[];
       uint8_t vm = tab5_settings_get_voice_mode();
       if (vm >= VOICE_MODE_COUNT) vm = 0;

       lv_obj_t *row = lv_obj_create(s_scroll);
       lv_obj_remove_style_all(row);
       lv_obj_set_pos(row, SIDE_PAD, y);
       lv_obj_set_size(row, CONTENT_W, 64);
       lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
       lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
       lv_obj_add_event_cb(row, cb_open_mode_picker, LV_EVENT_CLICKED, NULL);

       lv_obj_t *dot = lv_obj_create(row);
       lv_obj_remove_style_all(dot);
       lv_obj_set_size(dot, 10, 10);
       lv_obj_set_pos(dot, 18, (64 - 10) / 2);
       lv_obj_set_style_bg_color(dot, lv_color_hex(th_mode_colors[vm]), 0);
       lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
       lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);

       lv_obj_t *nm = lv_label_create(row);
       lv_label_set_text(nm, th_mode_names[vm]);
       lv_obj_set_style_text_font(nm, FONT_BODY, 0);
       lv_obj_set_style_text_color(nm, lv_color_hex(TEXT_PRIMARY), 0);
       lv_obj_set_pos(nm, 44, 12);

       lv_obj_t *dc = lv_label_create(row);
       lv_label_set_text(dc, "Tap to choose mode \xe2\x80\xa2 smartness \xe2\x80\xa2 advanced");
       lv_obj_set_style_text_font(dc, FONT_SMALL, 0);
       lv_obj_set_style_text_color(dc, lv_color_hex(TEXT_DIM), 0);
       lv_obj_set_style_text_letter_space(dc, 2, 0);
       lv_obj_set_pos(dc, 44, 38);

       lv_obj_t *chev = lv_label_create(row);
       lv_label_set_text(chev, ">");
       lv_obj_set_style_text_font(chev, FONT_BODY, 0);
       lv_obj_set_style_text_color(chev, lv_color_hex(TEXT_DIM), 0);
       lv_obj_align(chev, LV_ALIGN_RIGHT_MID, -18, 0);

       y += 64 + 12;
    }
    s_local_card = s_hybrid_card = s_cloud_card = s_tinkerclaw_card = NULL;
    mk_card_bg(s_scroll, s_voice_section_top, y);
    y += 24;

    /* ── Cap Wave 5 (TT #644): PRIVACY section ─────────────────────
     *
     * Master "on-device only" lock.  When ON, voice_modes_route_text
     * refuses Dragon/OpenRouter dispatch and forces K144 routing for
     * every text turn.  Composes with the LLM engine knob above. */
    int privacy_section_top = y;
    {
       lv_obj_t *cap = lv_label_create(s_scroll);
       lv_label_set_text(cap, "PRIVACY");
       lv_obj_set_pos(cap, SIDE_PAD, y);
       lv_obj_set_style_text_color(cap, lv_color_hex(AMBER), 0);
       lv_obj_set_style_text_font(cap, FONT_SECONDARY, 0);
       lv_obj_set_style_text_letter_space(cap, 4, 0);
       y += 26;

       bool priv_on = tab5_settings_get_privacy_lock();
       mk_row_label(s_scroll, "On-device lock", y);
       mk_switch(s_scroll, acc_voice, 660, y, priv_on, cb_privacy_lock, NULL);
       y += ROW_H + 4;

       /* Status line under the toggle row — "ON · On-device only" or
        * "OFF · Cloud allowed" — same pattern as the K144 health chip. */
       s_lbl_privacy_status = lv_label_create(s_scroll);
       if (s_lbl_privacy_status) {
          lv_obj_set_pos(s_lbl_privacy_status, SIDE_PAD, y);
          lv_obj_set_style_text_font(s_lbl_privacy_status, FONT_SECONDARY, 0);
          lv_obj_set_style_text_color(s_lbl_privacy_status, lv_color_hex(priv_on ? 0xF59E0B : TEXT_DIM), 0);
          lv_label_set_text(s_lbl_privacy_status, priv_on ? "ON · On-device only" : "OFF · Cloud allowed");
       }
       y += 28 + 16;
    }
    mk_card_bg(s_scroll, privacy_section_top, y);
    y += 24;

    /* AUDIO + QUIET HOURS rows (spec groups them under the VOICE MODE
     * section visually — single amber caption, rows straight below). */
    int audio_section_top = y;
    y = mk_section(s_scroll, "AUDIO", acc_voice, y);
    mk_row_label(s_scroll, "Mic mute", y);
    mk_switch(s_scroll, acc_voice, 660, y, tab5_settings_get_mic_mute() != 0,
              cb_mic_mute, NULL);
    y += ROW_H + 16;
    mk_card_bg(s_scroll, audio_section_top, y);
    y += 24;

    int quiet_section_top = y;
    y = mk_section(s_scroll, "QUIET HOURS", acc_voice, y);
    mk_row_label(s_scroll, "Enable", y);
    mk_switch(s_scroll, acc_voice, 660, y, tab5_settings_get_quiet_on() != 0,
              cb_quiet_on, NULL);
    y += ROW_H + 8;

    /* Audit U3 (#206): start + end hour sliders that were missing pre-fix.
     * Range 0-23, default values from NVS, label shows current value.
     * Editable regardless of master switch state — preferences are
     * decoupled from the on/off, matching CLAUDE.md NVS table semantics. */
    mk_row_label(s_scroll, "  Start", y);
    {
        lv_obj_t *sl = mk_slider(s_scroll, acc_voice, RIGHT_X, y,
                                 0, 23, tab5_settings_get_quiet_start(),
                                 cb_quiet_start);
        (void)sl;
        s_lbl_quiet_start_val = lv_label_create(s_scroll);
        if (s_lbl_quiet_start_val) {
            lv_obj_set_pos(s_lbl_quiet_start_val, RIGHT_X + 208, y + (ROW_H - 14) / 2);
            lv_obj_set_style_text_color(s_lbl_quiet_start_val, lv_color_hex(0xF59E0B), 0);
            lv_obj_set_style_text_font(s_lbl_quiet_start_val, FONT_SECONDARY, 0);
            lv_label_set_text_fmt(s_lbl_quiet_start_val, "%02d:00",
                                  tab5_settings_get_quiet_start());
        }
    }
    y += ROW_H + 4;
    mk_row_label(s_scroll, "  End", y);
    {
        lv_obj_t *sl = mk_slider(s_scroll, acc_voice, RIGHT_X, y,
                                 0, 23, tab5_settings_get_quiet_end(),
                                 cb_quiet_end);
        (void)sl;
        s_lbl_quiet_end_val = lv_label_create(s_scroll);
        if (s_lbl_quiet_end_val) {
            lv_obj_set_pos(s_lbl_quiet_end_val, RIGHT_X + 208, y + (ROW_H - 14) / 2);
            lv_obj_set_style_text_color(s_lbl_quiet_end_val, lv_color_hex(0xF59E0B), 0);
            lv_obj_set_style_text_font(s_lbl_quiet_end_val, FONT_SECONDARY, 0);
            lv_label_set_text_fmt(s_lbl_quiet_end_val, "%02d:00",
                                  tab5_settings_get_quiet_end());
        }
    }
    y += ROW_H + 16;
    mk_card_bg(s_scroll, quiet_section_top, y);
    y += 24;

    /* v4·D Phase 4e: daily budget cap editor.  Slider range 0-50, each
     * step = 20¢, so 0 = OFF, 50 = $10.00.  Maps to NVS cap_mils field
     * consumed by the auto-downgrade path in voice.c. */
    int budget_section_top = y;
    y = mk_section(s_scroll, "BUDGET", acc_voice, y);
    mk_row_label(s_scroll, "Daily cap", y);
    {
        uint32_t cap_mils = tab5_budget_get_cap_mils();
        uint32_t cents    = cap_mils / 1000;
        int step          = (int)((cents + 10) / 20);   /* round to nearest 20¢ */
        if (step > 50) step = 50;
        s_slider_cap = mk_slider(s_scroll, acc_voice, RIGHT_X, y,
                                 0, 50, step, cb_cap);
        s_lbl_cap_val = lv_label_create(s_scroll);
        lv_obj_set_pos(s_lbl_cap_val, RIGHT_X + 208, y + (ROW_H - 14) / 2);
        lv_obj_set_style_text_color(s_lbl_cap_val, lv_color_hex(0xF59E0B), 0);
        lv_obj_set_style_text_font(s_lbl_cap_val, FONT_SECONDARY, 0);
        if (cap_mils == 0) {
            lv_label_set_text(s_lbl_cap_val, "OFF");
        } else {
            int dollars = (int)(cents / 100);
            int remc    = (int)(cents % 100);
            lv_label_set_text_fmt(s_lbl_cap_val, "$%d.%02d", dollars, remc);
        }
    }
    y += ROW_H + 20;
    mk_card_bg(s_scroll, budget_section_top, y);
    y += 24;

    /* ════════════════════════════════════════════════════════════════
     *  SECTION: TINKERON (TT #586 — always-on listener controls)
     * ════════════════════════════════════════════════════════════════ */
    feed_wdt();
    ESP_LOGI(TAG, "Phase 1 — Section: TinkerON");
    int tinkeron_section_top = y;
    y = mk_section(s_scroll, "TINKERON", acc_voice, y);

    /* Status row: temp + version + chain hwinfo cache (best-effort —
     * stays blank if K144 is UNAVAILABLE). */
    {
        voice_m5_hwinfo_t hw = {0};
        char ver[16] = {0};
        bool hwok = (voice_m5_llm_sys_hwinfo(&hw) == ESP_OK);
        (void)voice_m5_llm_sys_version(ver, sizeof(ver));
        char status[80];
        if (hwok && hw.valid) {
            snprintf(status, sizeof(status), "%s  ·  %ld.%ld°C  ·  load %ld",
                     ver[0] ? ver : "v?", (long)(hw.temperature_milli_c / 1000),
                     (long)((hw.temperature_milli_c / 100) % 10), (long)hw.cpu_loadavg);
        } else {
            snprintf(status, sizeof(status), "UNAVAILABLE — try Reset");
        }
        mk_row_label(s_scroll, status, y);
        y += ROW_H + 8;
    }

    /* Always-on listener switch. */
    mk_row_label(s_scroll, "Always-on listener", y);
    mk_switch(s_scroll, acc_voice, 660, y,
              voice_wakeword_is_active(), cb_tinkeron_armed, NULL);
    y += ROW_H + 8;

    /* Reset TinkerON pill button. */
    mk_row_label(s_scroll, "Reset module", y);
    mk_pill_btn(s_scroll, "RESET", acc_voice, lv_color_hex(0xFFFFFF),
                RIGHT_X - 130, y + 4, 130, ROW_H - 8, 18, cb_tinkeron_reset);
    y += ROW_H + 20;
    mk_card_bg(s_scroll, tinkeron_section_top, y);
    y += 24;

    /* ════════════════════════════════════════════════════════════════
     *  SECTION: CHANNELS (W7-E.5 — per-platform notification opt-in)
     * ════════════════════════════════════════════════════════════════ */
    feed_wdt();
    ESP_LOGI(TAG, "Phase 1 — Section: Channels");
    lv_color_t acc_channels = lv_color_hex(ACC_VOICE); /* shares amber w/ Voice */

    int channels_section_top = y;
    y = mk_section(s_scroll, "CHANNELS", acc_channels, y);

    struct {
       const char *label;
       uint8_t (*getter)(void);
       lv_event_cb_t cb;
    } ch_rows[] = {
        {"Telegram", tab5_settings_get_ch_tg_on, cb_ch_tg_on}, {"WhatsApp", tab5_settings_get_ch_wa_on, cb_ch_wa_on},
        {"Discord", tab5_settings_get_ch_dc_on, cb_ch_dc_on},  {"Slack", tab5_settings_get_ch_sl_on, cb_ch_sl_on},
        {"Signal", tab5_settings_get_ch_sg_on, cb_ch_sg_on},   {"iMessage", tab5_settings_get_ch_im_on, cb_ch_im_on},
        {"Matrix", tab5_settings_get_ch_ma_on, cb_ch_ma_on},   {"Email", tab5_settings_get_ch_em_on, cb_ch_em_on},
    };
    for (size_t i = 0; i < sizeof(ch_rows) / sizeof(ch_rows[0]); i++) {
       mk_row_label(s_scroll, ch_rows[i].label, y);
       mk_switch(s_scroll, acc_channels, 660, y, ch_rows[i].getter() != 0, ch_rows[i].cb, NULL);
       y += ROW_H + 8;
    }
    y += 12;
    mk_card_bg(s_scroll, channels_section_top, y);
    y += 24;

    /* ════════════════════════════════════════════════════════════════
     *  SECTION: DISPLAY (amber #F5A623)
     * ════════════════════════════════════════════════════════════════ */
    feed_wdt();
    ESP_LOGI(TAG, "Phase 1 — Section: Display");
    lv_color_t acc_display = lv_color_hex(ACC_DISPLAY);

    int display_section_top = y;
    y = mk_section(s_scroll, "DISPLAY", acc_display, y);

    /* Brightness */
    mk_row_label(s_scroll, "Brightness", y);
    s_slider_bright = mk_slider(s_scroll, acc_display, RIGHT_X, y,
                                0, 100, tab5_settings_get_brightness(), cb_brightness);
    /* Wave 15 W15-C06: NULL guard — in rapid nav cycling the first
     * Settings create after boot has been observed to return NULL
     * from `lv_label_create` despite 50 KB+ LVGL pool free.  Root
     * cause under investigation; the immediate fix is defence in
     * depth — skip styling if create fails so we don't NULL-deref
     * and panic the whole device. */
    s_lbl_bright_val = lv_label_create(s_scroll);
    if (s_lbl_bright_val) {
        lv_obj_set_pos(s_lbl_bright_val, RIGHT_X + 208, y + (ROW_H - 14) / 2);
        lv_label_set_text_fmt(s_lbl_bright_val, "%d%%", tab5_settings_get_brightness());
        lv_obj_set_style_text_color(s_lbl_bright_val, lv_color_hex(0xF59E0B), 0);
        lv_obj_set_style_text_font(s_lbl_bright_val, FONT_SECONDARY, 0);
    } else {
        ESP_LOGW(TAG, "s_lbl_bright_val: lv_label_create returned NULL — skipping style");
    }
    y += ROW_H + 4;

    /* Volume */
    mk_row_label(s_scroll, "Volume", y);
    s_slider_volume = mk_slider(s_scroll, acc_display, RIGHT_X, y,
                                0, 100, tab5_settings_get_volume(), cb_volume);
    /* W15-C06: mk_slider now NULL-checks internally — still guard
     * the caller-side deref (add_event_cb) against NULL. */
    if (s_slider_volume) {
        lv_obj_add_event_cb(s_slider_volume, cb_volume_released, LV_EVENT_RELEASED, NULL);
    }
    s_lbl_vol_val = lv_label_create(s_scroll);
    if (s_lbl_vol_val) {
        lv_obj_set_pos(s_lbl_vol_val, RIGHT_X + 208, y + (ROW_H - 14) / 2);
        lv_label_set_text_fmt(s_lbl_vol_val, "%d%%", tab5_settings_get_volume());
        lv_obj_set_style_text_color(s_lbl_vol_val, lv_color_hex(0xF59E0B), 0);
        lv_obj_set_style_text_font(s_lbl_vol_val, FONT_SECONDARY, 0);
    } else {
        ESP_LOGW(TAG, "s_lbl_vol_val: lv_label_create returned NULL — skipping style");
    }
    y += ROW_H + 4;

    /* Auto-rotate */
    mk_row_label(s_scroll, "Auto-rotate", y);
    s_lbl_orient = mk_row_value(s_scroll, "Off", lv_color_hex(TEXT_DIM), y);
    /* Audit U2 (#206): switch initial state mirrors NVS so the toggle
     * survives reboot. */
    s_sw_autorot = mk_switch(s_scroll, acc_display, 660, y,
                             tab5_settings_get_auto_rotate() != 0,
                             cb_autorotate, NULL);
    y += ROW_H + 4;

    /* #260: Camera rotation dropdown.  Sensor is fixed-orientation
     * landscape; this rotates the captured frame in software. */
    mk_row_label(s_scroll, "Camera rotation", y);
    {
        lv_obj_t *dd = lv_dropdown_create(s_scroll);
        lv_dropdown_set_options(dd, "0\xc2\xb0\n90\xc2\xb0 CW\n180\xc2\xb0\n270\xc2\xb0 CW");
        uint8_t cur = tab5_settings_get_cam_rotation();
        if (cur > 3) cur = 0;
        lv_dropdown_set_selected(dd, cur);
        lv_obj_set_pos(dd, 340, y);
        lv_obj_set_size(dd, 340, 36);
        lv_obj_set_style_bg_color(dd, lv_color_hex(CARD_COLOR), 0);
        lv_obj_set_style_bg_opa(dd, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(dd, lv_color_hex(TEXT_PRIMARY), 0);
        lv_obj_set_style_text_font(dd, FONT_SECONDARY, 0);
        lv_obj_set_style_border_width(dd, 1, 0);
        lv_obj_set_style_border_color(dd, lv_color_hex(0x1A1A24), 0);
        lv_obj_set_style_radius(dd, 6, 0);
        lv_obj_set_style_bg_color(dd, lv_color_hex(CARD_COLOR), LV_PART_ITEMS);
        lv_obj_set_style_text_color(dd, lv_color_hex(TEXT_PRIMARY), LV_PART_ITEMS);
        lv_obj_add_event_cb(dd, cb_cam_rotation, LV_EVENT_VALUE_CHANGED, NULL);
    }
    y += ROW_H + 4;

    /* Polish P10 (TT #666): font-scale dropdown.  Persists tier 0..3
     * (0.85 / 1.0 / 1.15 / 1.3).  Application of the multiplier to
     * every FONT_* token is a future per-screen sweep. */
    mk_row_label(s_scroll, "Text size", y);
    {
       lv_obj_t *dd = lv_dropdown_create(s_scroll);
       lv_dropdown_set_options(dd, "Small\nDefault\nLarge\nExtra large");
       uint8_t cur = tab5_settings_get_font_scale();
       if (cur > 3) cur = 1;
       lv_dropdown_set_selected(dd, cur);
       lv_obj_set_pos(dd, 340, y);
       lv_obj_set_size(dd, 340, 36);
       lv_obj_set_style_bg_color(dd, lv_color_hex(CARD_COLOR), 0);
       lv_obj_set_style_bg_opa(dd, LV_OPA_COVER, 0);
       lv_obj_set_style_text_color(dd, lv_color_hex(TEXT_PRIMARY), 0);
       lv_obj_set_style_text_font(dd, FONT_SECONDARY, 0);
       lv_obj_set_style_border_width(dd, 1, 0);
       lv_obj_set_style_border_color(dd, lv_color_hex(0x1A1A24), 0);
       lv_obj_set_style_radius(dd, 6, 0);
       lv_obj_set_style_bg_color(dd, lv_color_hex(CARD_COLOR), LV_PART_ITEMS);
       lv_obj_set_style_text_color(dd, lv_color_hex(TEXT_PRIMARY), LV_PART_ITEMS);
       lv_obj_add_event_cb(dd, cb_font_scale, LV_EVENT_VALUE_CHANGED, NULL);
    }
    y += ROW_H + 4;

    /* Polish P10 (TT #666): reduce-motion switch.  Persists boolean.
     * Application to decorative animation sites is a future sweep. */
    mk_row_label(s_scroll, "Reduce motion", y);
    mk_switch(s_scroll, acc_display, 660, y, tab5_settings_get_reduce_motion(), cb_reduce_motion, NULL);
    y += ROW_H + 4;

    /* Polish P11 (TT #666): theme picker dropdown.  0=dark, 1=light,
     * 2=auto-by-time (7am-7pm light, else dark).  Dispatching TH_*
     * tokens through a light palette is a future per-screen sweep. */
    mk_row_label(s_scroll, "Theme", y);
    {
       lv_obj_t *dd = lv_dropdown_create(s_scroll);
       lv_dropdown_set_options(dd, "Dark\nLight\nAuto (time of day)");
       uint8_t cur = tab5_settings_get_theme();
       if (cur > 2) cur = 0;
       lv_dropdown_set_selected(dd, cur);
       lv_obj_set_pos(dd, 340, y);
       lv_obj_set_size(dd, 340, 36);
       lv_obj_set_style_bg_color(dd, lv_color_hex(CARD_COLOR), 0);
       lv_obj_set_style_bg_opa(dd, LV_OPA_COVER, 0);
       lv_obj_set_style_text_color(dd, lv_color_hex(TEXT_PRIMARY), 0);
       lv_obj_set_style_text_font(dd, FONT_SECONDARY, 0);
       lv_obj_set_style_border_width(dd, 1, 0);
       lv_obj_set_style_border_color(dd, lv_color_hex(0x1A1A24), 0);
       lv_obj_set_style_radius(dd, 6, 0);
       lv_obj_set_style_bg_color(dd, lv_color_hex(CARD_COLOR), LV_PART_ITEMS);
       lv_obj_set_style_text_color(dd, lv_color_hex(TEXT_PRIMARY), LV_PART_ITEMS);
       lv_obj_add_event_cb(dd, cb_theme, LV_EVENT_VALUE_CHANGED, NULL);
    }
    y += ROW_H + 4;

    /* Vision V2-A.1 (TT #674): Always-on vision master toggle.
     * Privacy-by-default — default OFF.  Background YOLO service
     * captures + classifies the camera feed when this is on. */
    mk_row_label(s_scroll, "Always-on vision", y);
    mk_switch(s_scroll, acc_display, 660, y, tab5_settings_get_vision_on(), cb_vision_on, NULL);
    y += ROW_H + 4;

    /* Vision V2-A.1 (TT #674): inference rate (1 / 2 / 3 Hz). */
    mk_row_label(s_scroll, "Vision rate", y);
    {
       lv_obj_t *dd = lv_dropdown_create(s_scroll);
       lv_dropdown_set_options(dd, "1 Hz\n2 Hz\n3 Hz");
       uint8_t cur = tab5_settings_get_vision_rate();
       if (cur < 1) cur = 2;
       if (cur > 3) cur = 3;
       lv_dropdown_set_selected(dd, cur - 1);
       lv_obj_set_pos(dd, 340, y);
       lv_obj_set_size(dd, 340, 36);
       lv_obj_set_style_bg_color(dd, lv_color_hex(CARD_COLOR), 0);
       lv_obj_set_style_bg_opa(dd, LV_OPA_COVER, 0);
       lv_obj_set_style_text_color(dd, lv_color_hex(TEXT_PRIMARY), 0);
       lv_obj_set_style_text_font(dd, FONT_SECONDARY, 0);
       lv_obj_set_style_border_width(dd, 1, 0);
       lv_obj_set_style_border_color(dd, lv_color_hex(0x1A1A24), 0);
       lv_obj_set_style_radius(dd, 6, 0);
       lv_obj_set_style_bg_color(dd, lv_color_hex(CARD_COLOR), LV_PART_ITEMS);
       lv_obj_set_style_text_color(dd, lv_color_hex(TEXT_PRIMARY), LV_PART_ITEMS);
       lv_obj_add_event_cb(dd, cb_vision_rate, LV_EVENT_VALUE_CHANGED, NULL);
    }
    y += ROW_H + 4;

    /* Vision V2-A.3 (TT #680): auto-dim screen when user walks away
     * (person LEAVE confirmed) and restore on return (person ENTER). */
    mk_row_label(s_scroll, "Auto-dim when away", y);
    mk_switch(s_scroll, acc_display, 660, y, tab5_settings_get_away_dim(), cb_away_dim, NULL);
    y += ROW_H + 16;
    mk_card_bg(s_scroll, display_section_top, y);
    y += 24;

    /* ════════════════════════════════════════════════════════════════
     *  SECTION: NETWORK (cyan #00E5FF)
     * ════════════════════════════════════════════════════════════════ */
    feed_wdt();
    ESP_LOGI(TAG, "Phase 1 — Section: Network");
    lv_color_t acc_network = lv_color_hex(ACC_NETWORK);

    int network_section_top = y;
    y = mk_section(s_scroll, "NETWORK", acc_network, y);

    /* WiFi status */
    mk_row_label(s_scroll, "WiFi", y);
    s_lbl_wifi = mk_row_value(s_scroll, "Checking...", lv_color_hex(TEXT_DIM), y);
    y += ROW_H + 4;

    /* WiFi Setup */
    {
        lv_obj_t *btn = mk_pill_btn(s_scroll, LV_SYMBOL_WIFI " Configure",
                                    acc_network, lv_color_hex(0x08080E),
                                    RIGHT_X, y + 3, 180, 38, 8, cb_wifi_setup);
        (void)btn;
    }
    /* NTP Sync */
    {
        lv_obj_t *ntp_btn = mk_pill_btn(s_scroll, "Sync NTP",
                                        acc_network, lv_color_hex(0x08080E),
                                        SIDE_PAD, y + 3, 140, 38, 8, cb_ntp_sync);
        s_ntp_btn_label = lv_obj_get_child(ntp_btn, 0);
    }
    y += ROW_H + 4;

    feed_wdt(); /* mid-section yield — Network section */

    /* Dragon host input (placeholder text serves as label) */
    s_dragon_ta = lv_textarea_create(s_scroll);
    lv_obj_set_pos(s_dragon_ta, SIDE_PAD, y + 4);
    lv_obj_set_size(s_dragon_ta, CONTENT_W, 36);
    lv_textarea_set_one_line(s_dragon_ta, true);
    lv_textarea_set_max_length(s_dragon_ta, 63);
    lv_obj_set_style_bg_color(s_dragon_ta, lv_color_hex(CARD_COLOR), 0);
    lv_obj_set_style_bg_opa(s_dragon_ta, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_dragon_ta, lv_color_hex(TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(s_dragon_ta, FONT_CAPTION, 0);
    lv_obj_set_style_border_width(s_dragon_ta, 1, 0);
    lv_obj_set_style_border_color(s_dragon_ta, lv_color_hex(0x1A1A24), 0);
    lv_obj_set_style_radius(s_dragon_ta, 6, 0);
    lv_obj_set_style_pad_left(s_dragon_ta, 8, 0);
    lv_textarea_set_placeholder_text(s_dragon_ta, "Dragon Host (e.g. 192.168.1.89)");
    {
        char dhost[64];
        tab5_settings_get_dragon_host(dhost, sizeof(dhost));
        if (dhost[0]) {
            lv_textarea_set_text(s_dragon_ta, dhost);
        }
    }
    lv_obj_add_flag(s_dragon_ta, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_event_cb(s_dragon_ta, cb_dragon_host_click, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_dragon_ta, cb_dragon_host_done, LV_EVENT_DEFOCUSED, NULL);
    lv_obj_add_event_cb(s_dragon_ta, cb_dragon_host_done, LV_EVENT_READY, NULL);
    y += ROW_H + 4;

    /* US-PR09: Reconnect status label (hidden until host change) */
    s_dragon_status_lbl = lv_label_create(s_scroll);
    lv_label_set_text(s_dragon_status_lbl, "");
    lv_obj_set_pos(s_dragon_status_lbl, SIDE_PAD, y);
    lv_obj_set_style_text_color(s_dragon_status_lbl, lv_color_hex(ACC_NETWORK), 0);
    lv_obj_set_style_text_font(s_dragon_status_lbl, FONT_SMALL, 0);
    lv_obj_add_flag(s_dragon_status_lbl, LV_OBJ_FLAG_HIDDEN);
    y += 4;

    /* Connection mode: Auto / Local / Remote */
    {
        lv_obj_t *conn_lbl = lv_label_create(s_scroll);
        lv_label_set_text(conn_lbl, "Connection");
        lv_obj_set_pos(conn_lbl, SIDE_PAD, y + 8);
        lv_obj_set_style_text_color(conn_lbl, lv_color_hex(TEXT_DIM), 0);
        lv_obj_set_style_text_font(conn_lbl, FONT_CAPTION, 0);

        static lv_obj_t *s_conn_dd = NULL;
        s_conn_dd = lv_dropdown_create(s_scroll);
        lv_dropdown_set_options(s_conn_dd, "Automatic\nHome Network\nInternet Only");
        lv_dropdown_set_selected(s_conn_dd, tab5_settings_get_connection_mode());
        lv_obj_set_pos(s_conn_dd, 340, y);
        lv_obj_set_size(s_conn_dd, 340, 36);
        lv_obj_set_style_bg_color(s_conn_dd, lv_color_hex(CARD_COLOR), 0);
        lv_obj_set_style_bg_opa(s_conn_dd, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(s_conn_dd, lv_color_hex(TEXT_PRIMARY), 0);
        lv_obj_set_style_text_font(s_conn_dd, FONT_SECONDARY, 0);
        lv_obj_set_style_border_width(s_conn_dd, 1, 0);
        lv_obj_set_style_border_color(s_conn_dd, lv_color_hex(0x1A1A24), 0);
        lv_obj_set_style_radius(s_conn_dd, 6, 0);
        /* Dropdown list styling */
        lv_obj_set_style_bg_color(s_conn_dd, lv_color_hex(CARD_COLOR), LV_PART_ITEMS);
        lv_obj_set_style_text_color(s_conn_dd, lv_color_hex(TEXT_PRIMARY), LV_PART_ITEMS);
        lv_obj_add_event_cb(s_conn_dd, cb_conn_mode, LV_EVENT_VALUE_CHANGED, NULL);
    }
    y += ROW_H + 16;
    mk_card_bg(s_scroll, network_section_top, y);
    y += 24;

    /* ═══════════════════════════════════════════════════════════════════
     *  END PHASE 1 — Screen is visible now with Display + Network.
     *  Schedule Phase 2 to create Voice + Storage + Battery + About
     *  after a 50ms delay so LVGL renders Phase 1 first.
     * ═══════════════════════════════════════════════════════════════════ */
    ESP_LOGI(TAG, "Phase 1 complete — scheduling Phase 2 in 50ms");

    s_phase2_y = y;
    s_phase2_pending = true;
    s_phase2_done    = false;   /* U13 (#206): clear before scheduling */
    s_phase2_timer = lv_timer_create(phase2_timer_cb, 50, NULL);
    lv_timer_set_repeat_count(s_phase2_timer, 1);

    /* Start refresh timer + initial data update for Phase 1 labels */
    s_refresh_timer = lv_timer_create(settings_refresh_cb, 2000, NULL);
    lv_timer_t *init_timer = lv_timer_create(settings_refresh_cb, 200, NULL);
    lv_timer_set_repeat_count(init_timer, 1);

    ui_keyboard_set_layout_cb(settings_keyboard_layout_cb);

    ESP_LOGI(TAG, "Phase 1 visible — user sees Display + Network immediately");
    /* Polish P3 (TT #652): 250 ms fade-in for the whole Settings
     * screen instead of a hard cut from home. */
    ui_overlay_fade_in(s_screen, 250);
    return s_screen;
}

/* ══════════════════════════════════════════════════════════════════════
 *  Update live values
 * ══════════════════════════════════════════════════════════════════════ */

void ui_settings_update(void)
{
    if (!s_screen) return;

    /* TT #328 Wave 16 — re-render the K144 health chip + kick off
     * the worker fetch when state has transitioned.  Cheap when no
     * change (early-exit on s_k144_last_chip_fs match). */
    refresh_k144_chip();

    /* Cap Wave 4 (TT #642) — refresh ENGINES section: chip greying
     * (in case K144 came back / or_key was set externally) + STT/TTS
     * status labels (in case vmode changed via /mode or modesheet). */
    {
       uint8_t cur_eng = tab5_settings_get_llm_engine();
       for (int i = 0; i < LLM_ENG_COUNT; i++) {
          if (s_eng_chip[i]) {
             _eng_chip_style(s_eng_chip[i], i == cur_eng, _eng_chip_enabled(i));
          }
       }
       uint8_t vm = tab5_settings_get_voice_mode();
       if (s_eng_stt_lbl) {
          lv_label_set_text(s_eng_stt_lbl, _stt_engine_label(vm));
       }
       if (s_eng_tts_lbl) {
          lv_label_set_text(s_eng_tts_lbl, _tts_engine_label(vm, cur_eng));
       }
    }

    /* WiFi status */
    if (s_lbl_wifi) {
        extern bool tab5_wifi_connected(void);
        if (tab5_wifi_connected()) {
            lv_label_set_text(s_lbl_wifi, "Connected");
            lv_obj_set_style_text_color(s_lbl_wifi, lv_color_hex(TAB_LOCAL), 0);
        } else {
            lv_label_set_text(s_lbl_wifi, "Not connected");
            lv_obj_set_style_text_color(s_lbl_wifi, lv_color_hex(ACC_BATTERY), 0);
        }
    }

    /* US-PR09: Hide reconnect label once voice is back up */
    if (s_dragon_status_lbl && !lv_obj_has_flag(s_dragon_status_lbl, LV_OBJ_FLAG_HIDDEN)) {
        if (voice_is_connected()) {
            lv_label_set_text(s_dragon_status_lbl, "Connected");
            lv_obj_set_style_text_color(s_dragon_status_lbl, lv_color_hex(TAB_LOCAL), 0);
        }
    }

    /* SD card info */
    if (s_lbl_sd_info) {
        if (tab5_sdcard_mounted()) {
            uint64_t total = tab5_sdcard_total_bytes();
            uint64_t free_b = tab5_sdcard_free_bytes();
            uint64_t used_b = (total > free_b) ? (total - free_b) : 0;
            int total_gb = (int)((total + 536870912ULL) / 1073741824ULL);  /* round */
            int free_gb  = (int)((free_b + 536870912ULL) / 1073741824ULL);
            int used_gb  = (int)((used_b + 536870912ULL) / 1073741824ULL);
            char sd_buf[64];
            if (used_gb == 0) {
                snprintf(sd_buf, sizeof(sd_buf), "SD Card: %d GB available", free_gb);
            } else {
                snprintf(sd_buf, sizeof(sd_buf), "SD Card: %d / %d GB used", used_gb, total_gb);
            }
            lv_label_set_text(s_lbl_sd_info, sd_buf);
            lv_obj_set_style_text_color(s_lbl_sd_info, lv_color_hex(TAB_LOCAL), 0);
        } else {
            lv_label_set_text(s_lbl_sd_info, "SD Card: Not mounted");
            lv_obj_set_style_text_color(s_lbl_sd_info, lv_color_hex(TEXT_DIM), 0);
        }
    }

    /* Battery */
    {
        tab5_battery_info_t bi;
        if (tab5_battery_read(&bi) == ESP_OK) {
            bool no_battery = (bi.voltage < 2.0f && bi.percent == 0);

            /* Primary status label */
            if (s_lbl_bat_status) {
                char sbuf[48];
                if (no_battery) {
                    snprintf(sbuf, sizeof(sbuf), "USB Powered");
                    lv_obj_set_style_text_color(s_lbl_bat_status,
                        lv_color_hex(TAB_LOCAL), 0);
                } else {
                    snprintf(sbuf, sizeof(sbuf), "%d%% \xc2\xb7 %s", bi.percent,
                             bi.charging ? "Charging" : "Discharging");
                    lv_obj_set_style_text_color(s_lbl_bat_status,
                        bi.charging ? lv_color_hex(TAB_LOCAL) : lv_color_hex(TEXT_DIM), 0);
                }
                lv_label_set_text(s_lbl_bat_status, sbuf);
            }
            /* Debug voltage (small dim text) */
            if (s_lbl_bat_volt) {
                char vbuf[32];
                snprintf(vbuf, sizeof(vbuf), "%.2fV", bi.voltage);
                lv_label_set_text(s_lbl_bat_volt, vbuf);
            }
            /* Bar + percentage */
            if (s_bar_bat_level) {
                int bar_pct = no_battery ? 0 : bi.percent;
                lv_bar_set_value(s_bar_bat_level, bar_pct, LV_ANIM_ON);
                lv_color_t bar_col = bar_pct > 20 ? lv_color_hex(TAB_LOCAL) :
                                     bar_pct > 10 ? lv_color_hex(TAB_HYBRID) :
                                     lv_color_hex(ACC_BATTERY);
                lv_obj_set_style_bg_color(s_bar_bat_level, bar_col, LV_PART_INDICATOR);
                /* Hide bar when USB powered (no battery) */
                if (no_battery) {
                    lv_obj_add_flag(s_bar_bat_level, LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_clear_flag(s_bar_bat_level, LV_OBJ_FLAG_HIDDEN);
                }
            }
            if (s_lbl_bat_pct) {
                if (no_battery) {
                    lv_label_set_text(s_lbl_bat_pct, "");
                } else {
                    char pbuf[8];
                    snprintf(pbuf, sizeof(pbuf), "%d%%", bi.percent);
                    lv_label_set_text(s_lbl_bat_pct, pbuf);
                }
            }
        }
    }

    /* Heap + PSRAM (combined label) */
    if (s_lbl_heap) {
        uint32_t heap_kb = (uint32_t)(esp_get_free_heap_size() / 1024);
        uint32_t psram = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        char hbuf[64];
        if (psram > 1048576) {
            snprintf(hbuf, sizeof(hbuf), "Heap: %lu KB  |  PSRAM: %.1f MB",
                     (unsigned long)heap_kb, psram / 1048576.0f);
        } else {
            snprintf(hbuf, sizeof(hbuf), "Heap: %lu KB  |  PSRAM: %lu KB",
                     (unsigned long)heap_kb, (unsigned long)(psram / 1024));
        }
        lv_label_set_text(s_lbl_heap, hbuf);
    }
}

/* ══════════════════════════════════════════════════════════════════════
 *  Destroy / Hide / Get
 * ══════════════════════════════════════════════════════════════════════ */

void ui_settings_destroy(void)
{
    if (!s_screen) return;

    ui_keyboard_set_layout_cb(NULL);
    s_destroying = true;
    s_creating = false;
    s_phase2_pending = false;  /* Cancel Phase 2 if it hasn't fired yet */
    s_phase2_done    = false;  /* U13 (#206): clear sentinel on destroy */

    if (s_phase2_timer) { lv_timer_delete(s_phase2_timer); s_phase2_timer = NULL; }
    if (s_refresh_timer) { lv_timer_delete(s_refresh_timer); s_refresh_timer = NULL; }

    /* Flush pending NVS debounce timers — save current value before destroy */
    if (s_bright_save_timer) {
        brightness_save_cb(s_bright_save_timer);   /* commit now */
        lv_timer_delete(s_bright_save_timer);
        s_bright_save_timer = NULL;
    }
    if (s_vol_save_timer) {
        volume_save_cb(s_vol_save_timer);           /* commit now */
        lv_timer_delete(s_vol_save_timer);
        s_vol_save_timer = NULL;
    }

    lv_obj_delete(s_screen);

    s_screen        = NULL;
    s_scroll        = NULL;
    /* Cap Wave 4 (TT #642) — engine chip + status label pointers
     * become dangling after lv_obj_clean; null them so the refresh
     * tick + post-clean state stays safe. */
    memset(s_eng_chip, 0, sizeof(s_eng_chip));
    s_eng_stt_lbl = NULL;
    s_eng_tts_lbl = NULL;
    /* Cap Wave 5 (TT #644) — privacy status label. */
    s_lbl_privacy_status = NULL;

    s_lbl_wifi      = NULL;
    s_lbl_bat_status = NULL;
    s_lbl_bat_volt  = NULL;
    s_bar_bat_level = NULL;
    s_lbl_bat_pct   = NULL;
    s_lbl_sd_info   = NULL;
    s_lbl_heap      = NULL;
    s_lbl_orient    = NULL;
    s_lbl_quiet_start_val = NULL;
    s_lbl_quiet_end_val   = NULL;
    s_ntp_spinner   = NULL;
    s_ntp_btn_label = NULL;
    s_slider_bright = NULL;
    s_slider_volume = NULL;
    s_lbl_bright_val = NULL;
    s_lbl_vol_val    = NULL;
    s_sw_autorot    = NULL;
    s_tab_local     = NULL;
    s_tab_hybrid    = NULL;
    s_tab_cloud     = NULL;
    s_tab_tinkerclaw = NULL;
    s_local_card    = NULL;
    s_hybrid_card   = NULL;
    s_cloud_card    = NULL;
    s_tinkerclaw_card = NULL;
    memset(s_local_content,  0, sizeof(s_local_content));
    memset(s_hybrid_content, 0, sizeof(s_hybrid_content));
    memset(s_cloud_content,  0, sizeof(s_cloud_content));
    memset(s_tinkerclaw_content, 0, sizeof(s_tinkerclaw_content));
    s_dragon_ta     = NULL;
    s_dragon_status_lbl = NULL;
    s_ota_btn_label = NULL;
    s_ota_apply_btn = NULL;

    ESP_LOGI(TAG, "Settings screen destroyed");
}

lv_obj_t *ui_settings_get_screen(void) { return s_screen; }

void ui_settings_hide(void)
{
    s_creating = false;
    s_phase2_pending = false;  /* Cancel Phase 2 if it hasn't fired yet */
    ui_keyboard_set_layout_cb(NULL);

    if (s_phase2_timer) { lv_timer_delete(s_phase2_timer); s_phase2_timer = NULL; }

    /* Hide instead of destroy — rapid open/close cycles exhaust LVGL pool.
     * PAUSE refresh timer to prevent it updating hidden objects during
     * other overlay creation (Settings timer + Notes creation = WDT). */
    if (s_refresh_timer) { lv_timer_delete(s_refresh_timer); s_refresh_timer = NULL; }

    /* Flush pending NVS debounce timers before hiding */
    if (s_bright_save_timer) {
        brightness_save_cb(s_bright_save_timer);
        lv_timer_delete(s_bright_save_timer);
        s_bright_save_timer = NULL;
    }
    if (s_vol_save_timer) {
        volume_save_cb(s_vol_save_timer);
        lv_timer_delete(s_vol_save_timer);
        s_vol_save_timer = NULL;
    }

    if (s_screen) {
        lv_obj_add_flag(s_screen, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_CLICKABLE);
    }
    s_destroying = false;
}

bool ui_settings_is_visible(void)
{
    return s_screen && !lv_obj_has_flag(s_screen, LV_OBJ_FLAG_HIDDEN);
}
