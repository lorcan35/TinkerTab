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
#define MS_W 720
#define MS_H 1280
#define SIDE_PAD 40

/* ── State ───────────────────────────────────────────────────────────── */
static lv_obj_t *s_overlay     = NULL;  /* scrim container on layer_top */
static lv_obj_t *s_sheet = NULL;        /* visible sheet inside overlay */

/* Agent consent modal — own scrim, shown on top of the sheet. */
static lv_obj_t *s_consent_overlay = NULL;
/* Generic callback mode (audit E3): when non-NULL, hide_agent_consent
 * invokes these instead of the tier-revert / persist logic. Used by the
 * Settings TinkerClaw row so the same UI can drive a different commit
 * path (settings already has its own voice_tab_switch flow). */
static void (*s_consent_confirm_cb)(void *) = NULL;
static void (*s_consent_cancel_cb)(void *)  = NULL;
static void  *s_consent_cb_ctx              = NULL;

/* TT #724 (3.2): mode-row picker state. */
static uint8_t s_sel_vmode = 0;       /* row currently shown expanded */
static lv_obj_t *s_rows_root = NULL;  /* container holding the 6 mode rows */
static lv_obj_t *s_smart_root = NULL; /* Smartness segment container */

/* TT #724 (3.3): Fast/Balanced/Smart -> a concrete model. Setting int_tier
 * alone changes nothing (routing keys off vmode + llm_model, never int_tier
 * — validated). Live for Cloud + Solo only; other modes are fixed-brain.
 * Model IDs match s_cloud_models[] in ui_settings.c. */
static const char *const s_smart_cloud[3] = {
    "~anthropic/claude-haiku-latest", /* Fast     */
    "anthropic/claude-sonnet-4.6",    /* Balanced */
    "anthropic/claude-opus-4.7",      /* Smart    */
};

/* TT #724 (3.4): Advanced drawer state. */
static lv_obj_t *s_adv_root = NULL; /* drawer container below the rows */
static bool s_adv_open = false;

/* Exact-model list for the Advanced drawer (relocated from ui_settings.c
 * CLOUD LLM block; Task 5 removes the Settings copy). model_id is what we
 * send to Dragon as llm_model. */
typedef struct {
   const char *label;
   const char *model_id;
} adv_model_t;
static const adv_model_t s_adv_models[] = {
    {"Opus 4.7", "anthropic/claude-opus-4.7"},
    {"Sonnet", "anthropic/claude-sonnet-4.6"},
    {"Haiku", "~anthropic/claude-haiku-latest"},
    {"GPT-5.5", "openai/gpt-5.5"},
    {"GPT-5.4m", "openai/gpt-5.4-mini"},
    {"Gemini Pro", "~google/gemini-pro-latest"},
    {"Gemini 3.1", "google/gemini-3.1-flash-lite"},
    {"Grok 4.3", "x-ai/grok-4.3"},
};
#define ADV_MODEL_COUNT (sizeof(s_adv_models) / sizeof(s_adv_models[0]))

/* Daily-cap presets in mils (1 mil = 1/1000 cent): OFF / $1 / $5 / $10. */
static const uint32_t s_adv_caps[4] = {0, 100000, 500000, 1000000};
static const char *const s_adv_cap_lbl[4] = {"Off", "$1", "$5", "$10"};

/* ── Forward decls ───────────────────────────────────────────────────── */
static void commit_mode(uint8_t vmode);
static void rebuild_rows(void);
static void build_smartness(void);
static void smart_click_cb(lv_event_t *e);
static void build_advanced(void);
static void row_click_cb(lv_event_t *e);
static void done_click_cb(lv_event_t *e);
static void scrim_click_cb(lv_event_t *e);
static void show_agent_consent(void);
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
    s_smart_root = NULL;
    s_adv_root = NULL;
}

void ui_mode_sheet_show(void)
{
    if (ui_mode_sheet_visible()) return;

    /* TT #724 (3.3): the picker no longer uses the int/voi/aut dials — routing
     * keys off the persisted vmode, and tab5_mode_resolve stays only for the
     * debug /mode-from-tiers path. No tier resync needed here. */

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

    /* TT #724 (3.3): Smartness segment, above the mode rows. */
    s_smart_root = lv_obj_create(s_sheet);
    lv_obj_remove_style_all(s_smart_root);
    lv_obj_set_pos(s_smart_root, 0, 116);
    lv_obj_set_size(s_smart_root, MS_W, 56);
    lv_obj_clear_flag(s_smart_root, LV_OBJ_FLAG_SCROLLABLE);
    build_smartness();

    s_rows_root = lv_obj_create(s_sheet);
    lv_obj_remove_style_all(s_rows_root);
    lv_obj_set_pos(s_rows_root, 0, 182);
    lv_obj_set_size(s_rows_root, MS_W, 352);
    lv_obj_clear_flag(s_rows_root, LV_OBJ_FLAG_SCROLLABLE);
    rebuild_rows();

    /* TT #724 (3.4): Advanced drawer, collapsed by default, below the rows. */
    s_adv_open = false;
    s_adv_root = lv_obj_create(s_sheet);
    lv_obj_remove_style_all(s_adv_root);
    lv_obj_set_pos(s_adv_root, 0, 542);
    lv_obj_set_size(s_adv_root, MS_W, 480);
    lv_obj_clear_flag(s_adv_root, LV_OBJ_FLAG_SCROLLABLE);
    build_advanced();

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
   rebuild_rows();    /* expand the newly-selected row */
   build_smartness(); /* fixed-brain modes grey the knob; Cloud/Solo enable it */
   commit_mode(vmode);
}

/* In-sheet TinkerAgent consent decisions (routed via ui_agent_consent_show). */
static void agent_consent_confirm_cb(void *ctx) {
   (void)ctx;
   s_sel_vmode = VOICE_MODE_TINKERCLAW;
   rebuild_rows();
   build_smartness(); /* TinkerAgent is fixed-brain -> grey the knob */
   commit_mode(VOICE_MODE_TINKERCLAW);
}

static void agent_consent_cancel_cb(void *ctx) {
   (void)ctx;
   /* Keep the prior selection; nothing was committed. */
}

/* TT #724 (3.3): Smartness tap — sets the *real* model for the only two modes
 * where Tab5 picks it (Cloud -> llm_model, Solo -> or_mdl_llm), persists the
 * tier for the segment's on-state, and notifies Dragon. No-op for fixed-brain
 * modes (the segment is greyed + non-clickable there). */
static void smart_click_cb(lv_event_t *e) {
   uint8_t tier = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
   if (tier > 2 || s_mode_meta[s_sel_vmode].fixed_brain) return;
   tab5_settings_set_int_tier(tier);
   const char *model = s_smart_cloud[tier];
   if (s_sel_vmode == VOICE_MODE_SOLO)
      tab5_settings_set_or_mdl_llm(model);
   else
      tab5_settings_set_llm_model(model); /* Cloud */
   voice_send_config_update((int)s_sel_vmode, (char *)model);
   build_smartness(); /* re-render the on-state */
   ESP_LOGI(TAG, "smartness tier=%u -> model=%s (vmode=%u)", tier, model, s_sel_vmode);
}

/* TT #724 (3.3): (re)build the Smartness segment for the current selection.
 * Greyed + non-clickable for fixed-brain modes; the on-state reflects the
 * persisted int_tier. Lives in its own container so it can be rebuilt cheaply
 * when the selected mode changes. */
static void build_smartness(void) {
   if (!s_smart_root) return;
   lv_obj_clean(s_smart_root);

   lv_obj_t *lab = lv_label_create(s_smart_root);
   lv_label_set_text(lab, "SMARTNESS");
   lv_obj_set_style_text_font(lab, FONT_SMALL, 0);
   lv_obj_set_style_text_color(lab, lv_color_hex(TH_AMBER), 0);
   lv_obj_set_style_text_letter_space(lab, 2, 0);
   lv_obj_set_pos(lab, SIDE_PAD, 0);

   bool fixed = s_mode_meta[s_sel_vmode].fixed_brain;
   /* TT #724: reflect the ACTUAL model, not int_tier (which drifts). Map the
    * mode's model field to a tier; -1 (custom / off-catalog) highlights none. */
   int tier = -1;
   {
      char m[64] = {0};
      if (s_sel_vmode == VOICE_MODE_SOLO)
         tab5_settings_get_or_mdl_llm(m, sizeof(m));
      else
         tab5_settings_get_llm_model(m, sizeof(m));
      for (int i = 0; i < 3; i++)
         if (strcmp(m, s_smart_cloud[i]) == 0) {
            tier = i;
            break;
         }
   }
   const char *names[3] = {"Fast", "Balanced", "Smart"};
   int seg_w = (MS_W - 2 * SIDE_PAD - 2 * 6) / 3;
   for (int i = 0; i < 3; i++) {
      bool on = (i == tier && !fixed);
      lv_obj_t *s = lv_obj_create(s_smart_root);
      lv_obj_remove_style_all(s);
      lv_obj_set_pos(s, SIDE_PAD + i * (seg_w + 6), 22);
      lv_obj_set_size(s, seg_w, 30);
      lv_obj_set_style_radius(s, 10, 0);
      lv_obj_set_style_bg_color(s, lv_color_hex(on ? TH_AMBER : 0x15151F), 0);
      lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
      lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
      if (!fixed) {
         lv_obj_add_flag(s, LV_OBJ_FLAG_CLICKABLE);
         lv_obj_add_event_cb(s, smart_click_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
      } else {
         lv_obj_set_style_opa(s, LV_OPA_40, 0);
      }
      lv_obj_t *t = lv_label_create(s);
      lv_label_set_text(t, names[i]);
      lv_obj_set_style_text_font(t, FONT_SMALL, 0);
      lv_obj_set_style_text_color(t, lv_color_hex(on ? TH_BG : TH_TEXT_PRIMARY), 0);
      lv_obj_center(t);
   }
}

/* ── Advanced drawer (3.4) ───────────────────────────────────────────── */

/* Pill chip used by every drawer control. */
static lv_obj_t *adv_chip(lv_obj_t *parent, int x, int y, int w, int h, const char *text, bool sel, lv_event_cb_t cb,
                          void *ud) {
   lv_obj_t *c = lv_obj_create(parent);
   lv_obj_remove_style_all(c);
   lv_obj_set_pos(c, x, y);
   lv_obj_set_size(c, w, h);
   lv_obj_set_style_radius(c, h / 2, 0);
   lv_obj_set_style_bg_color(c, lv_color_hex(sel ? TH_AMBER : 0x15151F), 0);
   lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
   lv_obj_set_style_border_width(c, 1, 0);
   lv_obj_set_style_border_color(c, lv_color_hex(sel ? TH_AMBER : 0x20202C), 0);
   lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
   lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
   if (cb) lv_obj_add_event_cb(c, cb, LV_EVENT_CLICKED, ud);
   lv_obj_t *l = lv_label_create(c);
   lv_label_set_text(l, text);
   lv_obj_set_style_text_font(l, FONT_SMALL, 0);
   lv_obj_set_style_text_color(l, lv_color_hex(sel ? TH_BG : TH_TEXT_PRIMARY), 0);
   lv_obj_center(l);
   return c;
}

static void adv_section_label(lv_obj_t *parent, int y, const char *text) {
   lv_obj_t *l = lv_label_create(parent);
   lv_label_set_text(l, text);
   lv_obj_set_style_text_font(l, FONT_SMALL, 0);
   lv_obj_set_style_text_color(l, lv_color_hex(TH_AMBER), 0);
   lv_obj_set_style_text_letter_space(l, 2, 0);
   lv_obj_set_pos(l, SIDE_PAD, y);
}

static void adv_toggle_cb(lv_event_t *e) {
   (void)e;
   s_adv_open = !s_adv_open;
   build_advanced();
}

static void adv_engine_cb(lv_event_t *e) {
   uint8_t idx = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
   if (idx >= LLM_ENG_COUNT) return;
   tab5_settings_set_llm_engine(idx);
   tab5_debug_obs_event("eng.llm", "picker");
   build_advanced();
}

static void adv_model_cb(lv_event_t *e) {
   uint8_t idx = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
   if (idx >= ADV_MODEL_COUNT) return;
   const char *model = s_adv_models[idx].model_id;
   /* Solo reads or_mdl_llm; every other cloud mode reads llm_model. Mirror
    * smart_click_cb so an exact-model override actually takes effect (TT #724). */
   if (s_sel_vmode == VOICE_MODE_SOLO)
      tab5_settings_set_or_mdl_llm(model);
   else
      tab5_settings_set_llm_model(model);
   voice_send_config_update((int)s_sel_vmode, (char *)model);
   build_advanced();
}

static void adv_privacy_cb(lv_event_t *e) {
   (void)e;
   tab5_settings_set_privacy_lock(!tab5_settings_get_privacy_lock());
   build_advanced();
}

static void adv_cap_cb(lv_event_t *e) {
   uint8_t idx = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
   if (idx >= 4) return;
   tab5_budget_set_cap_mils(s_adv_caps[idx]);
   build_advanced();
}

/* (re)build the Advanced drawer. Collapsed = a single toggle row; expanded =
 * engine pins + exact-model row + privacy lock + daily cap, relocated from
 * Settings. K144 as a hardware label is acceptable on this advanced surface. */
static void build_advanced(void) {
   if (!s_adv_root) return;
   lv_obj_clean(s_adv_root);

   lv_obj_t *tog = lv_obj_create(s_adv_root);
   lv_obj_remove_style_all(tog);
   lv_obj_set_pos(tog, SIDE_PAD, 0);
   lv_obj_set_size(tog, MS_W - 2 * SIDE_PAD, 38);
   lv_obj_clear_flag(tog, LV_OBJ_FLAG_SCROLLABLE);
   lv_obj_add_flag(tog, LV_OBJ_FLAG_CLICKABLE);
   lv_obj_add_event_cb(tog, adv_toggle_cb, LV_EVENT_CLICKED, NULL);
   lv_obj_t *tl = lv_label_create(tog);
   lv_label_set_text(tl, s_adv_open ? "Advanced  -"
                                    : "Advanced  +   model \xe2\x80\xa2 engine \xe2\x80\xa2 privacy \xe2\x80\xa2 cap");
   lv_obj_set_style_text_font(tl, FONT_SMALL, 0);
   lv_obj_set_style_text_color(tl, lv_color_hex(0x8A8A98), 0);
   lv_obj_set_pos(tl, 0, 8);
   if (!s_adv_open) return;

   int y = 50;
   const int avail_w = MS_W - 2 * SIDE_PAD;

   /* Engine pins (AUTO follows vmode; K144 / OpenRouter explicitly override). */
   adv_section_label(s_adv_root, y, "ENGINE");
   y += 22;
   {
      uint8_t cur = tab5_settings_get_llm_engine();
      int gap = 6;
      int w = (avail_w - 2 * gap) / 3;
      const char *names[3] = {"Auto", "K144", "OpenRouter"};
      for (int i = 0; i < 3; i++)
         adv_chip(s_adv_root, SIDE_PAD + i * (w + gap), y, w, 34, names[i], cur == i, adv_engine_cb,
                  (void *)(uintptr_t)i);
   }
   y += 46;

   /* Exact model — overrides the Smartness default. Horizontally scrollable. */
   adv_section_label(s_adv_root, y, "EXACT MODEL");
   y += 22;
   {
      lv_obj_t *scroll = lv_obj_create(s_adv_root);
      lv_obj_remove_style_all(scroll);
      lv_obj_set_pos(scroll, SIDE_PAD, y);
      lv_obj_set_size(scroll, avail_w, 40);
      lv_obj_set_scroll_dir(scroll, LV_DIR_HOR);
      lv_obj_set_scrollbar_mode(scroll, LV_SCROLLBAR_MODE_OFF);
      char cur_model[64] = {0};
      if (s_sel_vmode == VOICE_MODE_SOLO)
         tab5_settings_get_or_mdl_llm(cur_model, sizeof(cur_model));
      else
         tab5_settings_get_llm_model(cur_model, sizeof(cur_model));
      int cx = 0;
      for (uint32_t i = 0; i < ADV_MODEL_COUNT; i++) {
         bool sel = (strcmp(cur_model, s_adv_models[i].model_id) == 0);
         adv_chip(scroll, cx, 0, 116, 34, s_adv_models[i].label, sel, adv_model_cb, (void *)(uintptr_t)i);
         cx += 116 + 8;
      }
   }
   y += 50;

   /* Privacy lock (on-device only). */
   adv_section_label(s_adv_root, y, "ON-DEVICE LOCK");
   y += 22;
   {
      bool on = tab5_settings_get_privacy_lock();
      adv_chip(s_adv_root, SIDE_PAD, y, 220, 34, on ? "Locked: on-device" : "Off: cloud allowed", on, adv_privacy_cb,
               NULL);
   }
   y += 46;

   /* Daily spend cap. */
   adv_section_label(s_adv_root, y, "DAILY CAP");
   y += 22;
   {
      uint32_t cur = tab5_budget_get_cap_mils();
      int gap = 6;
      int w = (avail_w - 3 * gap) / 4;
      for (int i = 0; i < 4; i++)
         adv_chip(s_adv_root, SIDE_PAD + i * (w + gap), y, w, 34, s_adv_cap_lbl[i], cur == s_adv_caps[i], adv_cap_cb,
                  (void *)(uintptr_t)i);
   }
}

bool ui_mode_sheet_is_modified(void) {
   if (tab5_settings_get_llm_engine() != LLM_ENG_AUTO) return true;
   if (tab5_settings_get_privacy_lock()) return true;
   /* TT #724 (#4): a custom (off-tier) model on a model-picking mode is also a
    * deviation from the plain Smartness default. */
   uint8_t vm = tab5_settings_get_voice_mode();
   if (vm == VOICE_MODE_CLOUD || vm == VOICE_MODE_SOLO) {
      char m[64] = {0};
      if (vm == VOICE_MODE_SOLO)
         tab5_settings_get_or_mdl_llm(m, sizeof(m));
      else
         tab5_settings_get_llm_model(m, sizeof(m));
      if (m[0]) {
         for (int i = 0; i < 3; i++)
            if (strcmp(m, s_smart_cloud[i]) == 0) return false;
         return true; /* model set but not a tier default => custom */
      }
   }
   return false;
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
    show_agent_consent();
}

static void show_agent_consent(void) {
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
