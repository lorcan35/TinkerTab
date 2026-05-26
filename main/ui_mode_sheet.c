/*
 * ui_mode_sheet.c — v4·D Sovereign Halo triple-dial mode picker.
 * See header for contract. Approx 400 LOC, all v4·D styling primitives
 * already in use elsewhere (rounded rect + 1 px border + 2-stop radial).
 */
#include "ui_mode_sheet.h"

#include <stdio.h>
#include <string.h>

#include "config.h"
#include "debug_obs.h" /* TT #625 Wave A.2: mode.cancel_for_switch */
#include "esp_log.h"
#include "lvgl.h"
#include "settings.h"
#include "ui_audio_cues.h" /* W8: mode-switch chirp on preset / dial commit */
#include "ui_core.h"       /* W8: tab5_ui_try_lock / tab5_ui_unlock for or_key gate toast */
#include "ui_home.h"       /* W8: ui_home_show_toast */
#include "ui_theme.h"
#include "voice.h"
#include "voice_onboard.h"   /* TT #724 (3.5) — voice_onboard_failover_state */
#include "widget_mode_dot.h" /* TT #724 (3.2) — mode dot on each picker row */

static const char *TAG = "ui_mode_sheet";

/* TT #724 (3.2): per-mode display + availability metadata. Names come from
 * th_mode_names (single source). `requires` drives the 3.5 dim/refuse gate. */
typedef enum { REQ_NONE = 0, REQ_ADDON, REQ_OR_KEY } mode_req_t;
typedef struct {
   const char *reason;  /* one-line why-you'd-pick-it (selected row) */
   const char *leaves;  /* what leaves the device + speed + cost (selected row) */
   const char *oneline; /* compact meta for unselected rows */
   mode_req_t req;
   bool fixed_brain; /* true => Smartness has no range (grey it) */
} mode_meta_t;

/* Indexed by vmode (0..5). Order matches th_mode_names / VOICE_MODE_*. */
static const mode_meta_t s_mode_meta[VOICE_MODE_COUNT] = {
    /* 0 Local       */ {"Private brain, on the Dragon box", "Nothing leaves \xe2\x80\xa2 free \xe2\x80\xa2 ~60s",
                         "Nothing leaves \xe2\x80\xa2 free \xe2\x80\xa2 ~60s", REQ_NONE, true},
    /* 1 Hybrid      */
    {"Private brain, fast voice", "leaves: your voice (STT) \xe2\x80\xa2 ~5s \xe2\x80\xa2 ~$0.02",
     "Private brain \xe2\x80\xa2 fast voice \xe2\x80\xa2 ~$0.02", REQ_NONE, true},
    /* 2 Cloud       */
    {"Smartest, everything cloud", "leaves: voice + text \xe2\x80\xa2 ~5s \xe2\x80\xa2 needs key",
     "Voice+text \xe2\x80\xa2 smartest \xe2\x80\xa2 needs key", REQ_NONE, false},
    /* 3 TinkerAgent */
    {"Tools + memory, agentic", "leaves: voice + text + tools \xe2\x80\xa2 via gateway",
     "Tools + memory \xe2\x80\xa2 agentic", REQ_NONE, true},
    /* 4 TinkerON    */
    {"Works offline, on-device addon", "Nothing leaves \xe2\x80\xa2 works offline",
     "Nothing leaves \xe2\x80\xa2 works offline", REQ_ADDON, true},
    /* 5 Solo        */
    {"Cloud quality, no Dragon needed", "leaves: voice + text \xe2\x80\xa2 direct to OpenRouter",
     "No Dragon \xe2\x80\xa2 voice+text leave", REQ_OR_KEY, false},
};

/* TT #724 (3.5/3.2): can this mode run right now? */
static bool mode_is_available(uint8_t vmode) {
   if (vmode >= VOICE_MODE_COUNT) return false;
   switch (s_mode_meta[vmode].req) {
      case REQ_ADDON:
         return voice_onboard_failover_state() == 2 /* M5_FAIL_READY */;
      case REQ_OR_KEY: {
         char k[128] = {0};
         tab5_settings_get_or_key(k, sizeof k);
         return k[0] != '\0';
      }
      default:
         return true;
   }
}

/* ── Layout ──────────────────────────────────────────────────────────── */
#define MS_W        720
#define MS_H        1280
#define SIDE_PAD    40
#define SEG_H       56
#define ROW_H       144      /* header label + segments */
#define ROW_GAP     14

/* ── State ───────────────────────────────────────────────────────────── */
static lv_obj_t *s_overlay     = NULL;  /* scrim container on layer_top */
static lv_obj_t *s_sheet = NULL;        /* visible sheet inside overlay */

/* Phase 2c Agent consent modal — own scrim, shown on top of the sheet. */
static lv_obj_t *s_consent_overlay = NULL;
static uint8_t   s_pre_consent_aut = 0;  /* tier to revert to on Cancel */
/* Generic callback mode (audit E3): when non-NULL, hide_agent_consent
 * invokes these instead of the tier-revert / persist logic. Used by the
 * Settings TinkerClaw row so the same UI can drive a different commit
 * path (settings already has its own voice_tab_switch flow). */
static void (*s_consent_confirm_cb)(void *) = NULL;
static void (*s_consent_cancel_cb)(void *)  = NULL;
static void  *s_consent_cb_ctx              = NULL;

static uint8_t s_int_tier = 0;
static uint8_t s_voi_tier = 0;
static uint8_t s_aut_tier = 0;

/* TT #724 (3.2): mode-row picker state. */
static uint8_t s_sel_vmode = 0;      /* row currently shown expanded */
static lv_obj_t *s_rows_root = NULL; /* container holding the 6 mode rows */

/* ── Forward decls ───────────────────────────────────────────────────── */
static void commit_mode(uint8_t vmode);
static void rebuild_rows(void);
static void row_click_cb(lv_event_t *e);
static void done_click_cb(lv_event_t *e);
static void scrim_click_cb(lv_event_t *e);
static void show_agent_consent(uint8_t prev_aut_tier);
static void hide_agent_consent(bool commit);
static void consent_confirm_cb(lv_event_t *e);
static void consent_cancel_cb(lv_event_t *e);
static void agent_consent_confirm_cb(void *ctx);
static void agent_consent_cancel_cb(void *ctx);

/* ── Public API ──────────────────────────────────────────────────────── */

bool ui_mode_sheet_visible(void)
{
    return s_overlay != NULL && !lv_obj_has_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
}

void ui_mode_sheet_hide(void)
{
   /* If an Agent consent modal is still up when the sheet gets hidden
    * (e.g. user backs out via nav), treat that as Cancel — drop the
    * pending callbacks so nothing commits. */
   if (s_consent_overlay) {
      lv_obj_del(s_consent_overlay);
      s_consent_overlay = NULL;
      s_consent_confirm_cb = NULL;
      s_consent_cancel_cb = NULL;
      s_consent_cb_ctx = NULL;
   }
    if (s_overlay) {
        lv_obj_del(s_overlay);
    }
    s_overlay = NULL;
    s_sheet = NULL;
    s_rows_root = NULL;
}

void ui_mode_sheet_show(void)
{
    if (ui_mode_sheet_visible()) return;

    /* Pick up current tier values -- so the segmented buttons draw with the
     * correct on-state for whatever the user last persisted. */
    s_int_tier = tab5_settings_get_int_tier();
    s_voi_tier = tab5_settings_get_voi_tier();
    s_aut_tier = tab5_settings_get_aut_tier();

    /* If voice_mode was set via a path that bypassed the dial sheet
     * (debug /mode, settings radio rows, orb long-press cycle), the
     * tiers can drift out of sync with the live mode.  Reverse-derive
     * the tiers from the current voice_mode so the dials open showing
     * what the device is actually running on. */
    uint8_t resolved = tab5_mode_resolve(s_int_tier, s_voi_tier, s_aut_tier,
                                         NULL, 0);
    uint8_t live_mode = tab5_settings_get_voice_mode();
    if (resolved != live_mode) {
        switch (live_mode) {
            case 3: /* TinkerClaw / Agent */
                s_aut_tier = 1;
                /* leave int/voi alone -- agent wins */
                break;
            case 2: /* Full Cloud */
                s_int_tier = 2; s_voi_tier = 2; s_aut_tier = 0;
                break;
            case 1: /* Hybrid */
                s_int_tier = 1; s_voi_tier = 2; s_aut_tier = 0;
                break;
            case 0: /* Local */
            default:
                s_int_tier = 0; s_voi_tier = 0; s_aut_tier = 0;
                break;
        }
        ESP_LOGI(TAG, "Dial sheet tiers resynced to live mode %d -> int=%d voi=%d aut=%d",
                 live_mode, s_int_tier, s_voi_tier, s_aut_tier);
    } else {
        ESP_LOGI(TAG, "Opening dial sheet (int=%d voi=%d aut=%d)",
                 s_int_tier, s_voi_tier, s_aut_tier);
    }

    /* Overlay scrim — fills the screen, dim semi-transparent, tappable
     * to dismiss.  lv_layer_top() keeps it above home + any other screen. */
    s_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_size(s_overlay, MS_W, MS_H);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_70, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_overlay, scrim_click_cb, LV_EVENT_CLICKED, NULL);

    /* Sheet — amber-accented container. Held near the top so the user
     * can still see a glimpse of the orb (and the sheet isn't a wall). */
    const int sheet_y = 60;
    const int sheet_h = 1100;
    s_sheet = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(s_sheet);
    lv_obj_set_pos(s_sheet, 0, sheet_y);
    lv_obj_set_size(s_sheet, MS_W, sheet_h);
    lv_obj_set_style_bg_color(s_sheet, lv_color_hex(TH_CARD), 0);
    lv_obj_set_style_bg_opa(s_sheet, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_sheet, 32, 0);
    lv_obj_set_style_border_width(s_sheet, 1, 0);
    lv_obj_set_style_border_color(s_sheet, lv_color_hex(0x1E1E2A), 0);
    lv_obj_clear_flag(s_sheet, LV_OBJ_FLAG_SCROLLABLE);
    /* Stop tap propagation — taps inside the sheet shouldn't dismiss. */
    lv_obj_add_flag(s_sheet, LV_OBJ_FLAG_CLICKABLE);

    /* Grip pill at the top centre */
    lv_obj_t *grip = lv_obj_create(s_sheet);
    lv_obj_remove_style_all(grip);
    lv_obj_set_size(grip, 56, 5);
    lv_obj_set_pos(grip, (MS_W - 56) / 2, 22);
    lv_obj_set_style_bg_color(grip, lv_color_hex(0x1E1E2A), 0);
    lv_obj_set_style_bg_opa(grip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(grip, 3, 0);

    /* Kicker + headline */
    lv_obj_t *kicker = lv_label_create(s_sheet);
    lv_label_set_text(kicker, "\xe2\x80\xa2 MODE");
    lv_obj_set_style_text_font(kicker, FONT_SMALL, 0);
    lv_obj_set_style_text_color(kicker, lv_color_hex(TH_AMBER), 0);
    lv_obj_set_style_text_letter_space(kicker, 4, 0);
    lv_obj_set_pos(kicker, SIDE_PAD, 56);

    lv_obj_t *head = lv_label_create(s_sheet);
    lv_label_set_text(head, "How should she think?");
    lv_obj_set_style_text_font(head, FONT_TITLE, 0);
    lv_obj_set_style_text_color(head, lv_color_hex(TH_TEXT_PRIMARY), 0);
    lv_obj_set_pos(head, SIDE_PAD, 82);

    /* Done button — top right, dismisses the sheet */
    lv_obj_t *done = lv_obj_create(s_sheet);
    lv_obj_remove_style_all(done);
    lv_obj_set_size(done, 92, 44);
    lv_obj_set_pos(done, MS_W - SIDE_PAD - 92, 64);
    lv_obj_set_style_bg_color(done, lv_color_hex(TH_AMBER), 0);
    lv_obj_set_style_bg_opa(done, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(done, 22, 0);
    lv_obj_clear_flag(done, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(done, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(done, done_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *done_lbl = lv_label_create(done);
    lv_label_set_text(done_lbl, "Done");
    lv_obj_set_style_text_font(done_lbl, FONT_BODY, 0);
    lv_obj_set_style_text_color(done_lbl, lv_color_hex(TH_BG), 0);
    lv_obj_center(done_lbl);

    /* TT #724 (3.2/3.3): mode-row picker body. Replaces the int/voi/aut
     * dials + preset chips + composite card. Each row writes vmode directly
     * via commit_mode(); the selected row expands to show why-you'd-pick-it.
     * Routing keys off vmode, so the resolver core is untouched. */
    s_sel_vmode = tab5_settings_get_voice_mode();
    if (s_sel_vmode >= VOICE_MODE_COUNT) s_sel_vmode = 0;
    s_rows_root = lv_obj_create(s_sheet);
    lv_obj_remove_style_all(s_rows_root);
    lv_obj_set_pos(s_rows_root, 0, 150);
    lv_obj_set_size(s_rows_root, MS_W, 470);
    lv_obj_clear_flag(s_rows_root, LV_OBJ_FLAG_SCROLLABLE);
    rebuild_rows();

    /* Force full-screen invalidate — same pattern as ui_home create
     * (PARTIAL render needs this to paint every strip on first show). */
    lv_obj_invalidate(lv_layer_top());
    lv_refr_now(lv_display_get_default());
}

/* ── Internals ───────────────────────────────────────────────────────── */

/* TT #724 (3.3): write the chosen mode + notify Dragon. Mirrors the proven
 * preset-4/5 calls but for all six modes. Does NOT touch tab5_mode_resolve /
 * the dial tiers — routing keys off the persisted vmode. */
static void commit_mode(uint8_t vmode) {
   if (vmode >= VOICE_MODE_COUNT) return;

   /* TT #625 Wave A.2 (R6) — a vmode change mid-turn used to orphan the
    * in-flight Dragon turn. Cancel cleanly first; the config_update then
    * applies to the NEXT turn. */
   voice_state_t vs = voice_get_state();
   if (vs != VOICE_STATE_IDLE && vs != VOICE_STATE_READY) {
      ESP_LOGI(TAG, "vmode change while voice in state %d — cancelling first", (int)vs);
      tab5_debug_obs_event("mode.cancel_for_switch", "");
      voice_cancel();
      ui_home_show_toast("Switched mode — current turn stopped");
   }

   tab5_settings_set_voice_mode(vmode);
   char model[64] = {0};
   tab5_settings_get_llm_model(model, sizeof(model));
   voice_send_config_update((int)vmode, model);
   ui_audio_cue_play(UI_CUE_MODE_SWITCH);
   extern void ui_agents_on_mode_change(void);
   ui_agents_on_mode_change();
   ESP_LOGI(TAG, "picker: mode -> %u (%s)", vmode, th_mode_names[vmode]);
}

/* TT #724 (3.2/3.3): rebuild the six mode rows. The selected row expands to
 * show reason + what-leaves; the rest show a single dim meta line. Unavailable
 * modes dim and refuse selection (3.5). */
static void rebuild_rows(void) {
   if (!s_rows_root) return;
   lv_obj_clean(s_rows_root);
   int y = 0;
   for (uint8_t m = 0; m < VOICE_MODE_COUNT; m++) {
      bool avail = mode_is_available(m);
      bool sel = (m == s_sel_vmode);
      int rh = sel ? 74 : 46;

      lv_obj_t *row = lv_obj_create(s_rows_root);
      lv_obj_remove_style_all(row);
      lv_obj_set_pos(row, SIDE_PAD, y);
      lv_obj_set_size(row, MS_W - 2 * SIDE_PAD, rh);
      lv_obj_set_style_bg_color(row, lv_color_hex(sel ? 0x1A1509 : 0x13131C), 0);
      lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
      lv_obj_set_style_radius(row, 12, 0);
      lv_obj_set_style_border_width(row, 1, 0);
      lv_obj_set_style_border_color(row, lv_color_hex(sel ? TH_AMBER : 0x20202C), 0);
      lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_event_cb(row, row_click_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)m);
      if (!avail) lv_obj_set_style_opa(row, LV_OPA_40, 0);

      /* mode dot (shared widget, colored by mode) */
      lv_obj_t *dot = widget_mode_dot_create(row, 8, m);
      if (dot) lv_obj_set_pos(dot, 12, sel ? 14 : 18);

      lv_obj_t *nm = lv_label_create(row);
      lv_label_set_text(nm, th_mode_names[m]);
      lv_obj_set_style_text_font(nm, FONT_BODY, 0);
      lv_obj_set_style_text_color(nm, lv_color_hex(TH_TEXT_PRIMARY), 0);
      lv_obj_set_pos(nm, 30, sel ? 10 : 13);

      if (m == 1) { /* Hybrid "recommended" tag */
         lv_obj_t *rec = lv_label_create(row);
         lv_label_set_text(rec, "RECOMMENDED");
         lv_obj_set_style_text_font(rec, FONT_SMALL, 0);
         lv_obj_set_style_text_color(rec, lv_color_hex(0x7A7A88), 0);
         lv_obj_align(rec, LV_ALIGN_TOP_RIGHT, -14, sel ? 14 : 16);
      }
      if (m == 4 && !avail) { /* TinkerON addon hint */
         lv_obj_t *w = lv_label_create(row);
         lv_label_set_text(w, "attach addon");
         lv_obj_set_style_text_font(w, FONT_SMALL, 0);
         lv_obj_set_style_text_color(w, lv_color_hex(TH_AMBER), 0);
         lv_obj_align(w, LV_ALIGN_TOP_RIGHT, -14, sel ? 14 : 16);
      }

      if (sel) {
         lv_obj_t *reason = lv_label_create(row);
         lv_label_set_text(reason, s_mode_meta[m].reason);
         lv_obj_set_style_text_font(reason, FONT_SMALL, 0);
         lv_obj_set_style_text_color(reason, lv_color_hex(0xC2C2CC), 0);
         lv_obj_set_pos(reason, 30, 34);
         lv_obj_t *meta = lv_label_create(row);
         lv_label_set_text(meta, s_mode_meta[m].leaves);
         lv_obj_set_style_text_font(meta, FONT_SMALL, 0);
         lv_obj_set_style_text_color(meta, lv_color_hex(0x6A6A78), 0);
         lv_obj_set_pos(meta, 30, 53);
      } else if (!(m == 1 || (m == 4 && !avail))) {
         /* one-line meta; skipped for rows that already carry a right-side
          * annotation (Hybrid RECOMMENDED / TinkerON attach-addon) so they
          * don't collide on the narrow unselected row. */
         lv_obj_t *one = lv_label_create(row);
         lv_label_set_text(one, s_mode_meta[m].oneline);
         lv_obj_set_style_text_font(one, FONT_SMALL, 0);
         lv_obj_set_style_text_color(one, lv_color_hex(0x6A6A78), 0);
         lv_obj_align(one, LV_ALIGN_RIGHT_MID, -14, 0);
      }
      y += rh + 8;
   }
}

static void row_click_cb(lv_event_t *e) {
   uint8_t vmode = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
   if (vmode >= VOICE_MODE_COUNT) return;
   if (!mode_is_available(vmode)) {
      if (tab5_ui_try_lock(150)) {
         ui_home_show_toast(s_mode_meta[vmode].req == REQ_ADDON ? "TinkerON not ready \xe2\x80\x94 check the module"
                                                                : "Add an OpenRouter key in Settings first");
         tab5_ui_unlock();
      }
      return;
   }
   /* TinkerAgent (3) bypasses on-device memory — preserve the consent modal
    * when switching INTO agent from another mode (memory-bypass boundary). */
   if (vmode == VOICE_MODE_TINKERCLAW && tab5_settings_get_voice_mode() != VOICE_MODE_TINKERCLAW) {
      ui_agent_consent_show(agent_consent_confirm_cb, agent_consent_cancel_cb, NULL);
      return;
   }
   s_sel_vmode = vmode;
   rebuild_rows(); /* expand the newly-selected row */
   commit_mode(vmode);
}

/* In-sheet TinkerAgent consent decisions (routed via ui_agent_consent_show). */
static void agent_consent_confirm_cb(void *ctx) {
   (void)ctx;
   s_sel_vmode = VOICE_MODE_TINKERCLAW;
   rebuild_rows();
   commit_mode(VOICE_MODE_TINKERCLAW);
}

static void agent_consent_cancel_cb(void *ctx) {
   (void)ctx;
   /* Keep the prior selection; nothing was committed. */
}

/* ── Agent consent modal ─────────────────────────────────────────────── */

static void consent_confirm_cb(lv_event_t *e)
{
    (void)e;
    hide_agent_consent(true);
}

static void consent_cancel_cb(lv_event_t *e)
{
    (void)e;
    hide_agent_consent(false);
}

static void consent_scrim_cb(lv_event_t *e)
{
    /* Tapping the scrim behaves like Cancel — safe default. */
    if (lv_event_get_target(e) == s_consent_overlay) {
        hide_agent_consent(false);
    }
}

static void hide_agent_consent(bool commit)
{
    if (s_consent_overlay) {
        lv_obj_del(s_consent_overlay);
        s_consent_overlay = NULL;
    }
    /* Invoke the caller's decision handler and reset the callback slots.
     * Both the in-sheet TinkerAgent flow (TT #724) and the Settings
     * TinkerClaw row (audit E3) route through this generic path. */
    void (*cb)(void *) = commit ? s_consent_confirm_cb : s_consent_cancel_cb;
    void *ctx = s_consent_cb_ctx;
    s_consent_confirm_cb = NULL;
    s_consent_cancel_cb = NULL;
    s_consent_cb_ctx = NULL;
    if (cb) cb(ctx);
}

void ui_agent_consent_show(void (*on_confirm)(void *ctx),
                           void (*on_cancel)(void *ctx),
                           void *ctx)
{
    /* Guard: if callback-mode modal is already up, chain-cancel it first
     * so we don't leak state. */
    if (s_consent_overlay) {
        hide_agent_consent(false);
    }
    s_consent_confirm_cb = on_confirm;
    s_consent_cancel_cb  = on_cancel;
    s_consent_cb_ctx     = ctx;
    s_pre_consent_aut    = s_aut_tier;  /* irrelevant in cb-mode, but safe */
    show_agent_consent(s_aut_tier);
}

static void show_agent_consent(uint8_t prev_aut_tier)
{
    s_pre_consent_aut = prev_aut_tier;

    /* Scrim over the whole screen (on top layer so it covers the sheet
     * plus any transient chrome). */
    s_consent_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_consent_overlay);
    lv_obj_set_size(s_consent_overlay, MS_W, MS_H);
    lv_obj_set_pos(s_consent_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_consent_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_consent_overlay, 200, 0);
    lv_obj_clear_flag(s_consent_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_consent_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_consent_overlay, consent_scrim_cb, LV_EVENT_CLICKED, NULL);

    /* Card — centered, tall enough for 4 bullets + 2 buttons. */
    lv_obj_t *card = lv_obj_create(s_consent_overlay);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 640, 780);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x13131F), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 24, 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0xA78BFA), 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    /* Violet accent bar top. */
    lv_obj_t *bar = lv_obj_create(card);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 140, 4);
    lv_obj_set_pos(bar, 36, 32);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0xA78BFA), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bar, 2, 0);

    /* Kicker */
    lv_obj_t *kicker = lv_label_create(card);
    lv_label_set_text(kicker, "\xe2\x80\xa2 AGENT MODE");
    lv_obj_set_style_text_font(kicker, FONT_SMALL, 0);
    lv_obj_set_style_text_color(kicker, lv_color_hex(0xA78BFA), 0);
    lv_obj_set_style_text_letter_space(kicker, 4, 0);
    lv_obj_set_pos(kicker, 36, 52);

    /* Title */
    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "Switch to Agent?");
    lv_obj_set_style_text_font(title, FONT_TITLE, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(TH_TEXT_PRIMARY), 0);
    lv_obj_set_pos(title, 36, 80);

    /* Subtitle */
    lv_obj_t *sub = lv_label_create(card);
    lv_label_set_text(sub, "This changes how she thinks about you.");
    lv_obj_set_style_text_font(sub, FONT_BODY, 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(TH_TEXT_DIM), 0);
    lv_obj_set_pos(sub, 36, 128);

    /* Bullets — 4 items, each a row with a violet dot + text label. */
    const char *bullets[4] = {
        "Your on-device memory is NOT injected.\nAgent runs from the gateway's own context.",
        "Tools drive the turn - search, calendar,\ninbox, etc. - not your recall of facts.",
        "All routed through the TinkerClaw gateway.\nLatency is higher; responses can run 30-60s.",
        "Billing flows through the gateway tier,\nnot your daily cap here.",
    };
    int y = 180;
    for (int i = 0; i < 4; i++) {
        lv_obj_t *dot = lv_obj_create(card);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 8, 8);
        lv_obj_set_pos(dot, 36, y + 8);
        lv_obj_set_style_bg_color(dot, lv_color_hex(0xA78BFA), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(dot, 4, 0);

        lv_obj_t *txt = lv_label_create(card);
        lv_label_set_text(txt, bullets[i]);
        lv_obj_set_style_text_font(txt, FONT_BODY, 0);
        lv_obj_set_style_text_color(txt, lv_color_hex(TH_TEXT_PRIMARY), 0);
        lv_obj_set_style_text_line_space(txt, 4, 0);
        lv_obj_set_width(txt, 540);
        lv_obj_set_pos(txt, 60, y);
        y += 100;
    }

    /* Primary button: Switch to Agent (violet fill). */
    lv_obj_t *confirm = lv_obj_create(card);
    lv_obj_remove_style_all(confirm);
    lv_obj_set_size(confirm, 568, 64);
    lv_obj_set_pos(confirm, 36, 620);
    lv_obj_set_style_bg_color(confirm, lv_color_hex(0xA78BFA), 0);
    lv_obj_set_style_bg_opa(confirm, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(confirm, 32, 0);
    lv_obj_set_style_border_width(confirm, 0, 0);
    lv_obj_clear_flag(confirm, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(confirm, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(confirm, consent_confirm_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *confirm_lbl = lv_label_create(confirm);
    lv_label_set_text(confirm_lbl, "Switch to Agent");
    lv_obj_set_style_text_font(confirm_lbl, FONT_HEADING, 0);
    lv_obj_set_style_text_color(confirm_lbl, lv_color_hex(0x08080E), 0);
    lv_obj_center(confirm_lbl);

    /* Secondary button: Keep Ask mode (ghost / outlined). */
    lv_obj_t *cancel = lv_obj_create(card);
    lv_obj_remove_style_all(cancel);
    lv_obj_set_size(cancel, 568, 64);
    lv_obj_set_pos(cancel, 36, 694);
    lv_obj_set_style_bg_opa(cancel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cancel, 1, 0);
    lv_obj_set_style_border_color(cancel, lv_color_hex(0x2A2A3A), 0);
    lv_obj_set_style_radius(cancel, 32, 0);
    lv_obj_clear_flag(cancel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(cancel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(cancel, consent_cancel_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *cancel_lbl = lv_label_create(cancel);
    lv_label_set_text(cancel_lbl, "Keep Ask mode");
    lv_obj_set_style_text_font(cancel_lbl, FONT_BODY, 0);
    lv_obj_set_style_text_color(cancel_lbl, lv_color_hex(TH_TEXT_DIM), 0);
    lv_obj_center(cancel_lbl);
}

static void done_click_cb(lv_event_t *e)
{
    (void)e;
    ui_mode_sheet_hide();
}

static void scrim_click_cb(lv_event_t *e)
{
    /* Only dismiss if the click landed on the scrim itself, not bubbled up
     * from the sheet.  LVGL doesn't give us a currentTarget-style test
     * directly, but lv_event_get_target tells us where the click originated. */
    if (lv_event_get_target(e) == s_overlay) {
        ui_mode_sheet_hide();
    }
}
