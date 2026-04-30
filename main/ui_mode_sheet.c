/*
 * ui_mode_sheet.c — v4·D Sovereign Halo triple-dial mode picker.
 * See header for contract. Approx 400 LOC, all v4·D styling primitives
 * already in use elsewhere (rounded rect + 1 px border + 2-stop radial).
 */
#include "ui_mode_sheet.h"

#include "settings.h"
#include "voice.h"
#include "ui_theme.h"
#include "config.h"

#include "esp_log.h"
#include "lvgl.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "ui_mode_sheet";

/* ── Layout ──────────────────────────────────────────────────────────── */
#define MS_W        720
#define MS_H        1280
#define SIDE_PAD    40
#define SEG_H       56
#define ROW_H       144      /* header label + segments */
#define ROW_GAP     14

/* ── State ───────────────────────────────────────────────────────────── */
static lv_obj_t *s_overlay     = NULL;  /* scrim container on layer_top */
static lv_obj_t *s_sheet       = NULL;  /* visible sheet inside overlay */
static lv_obj_t *s_seg_btn[3][3] = {{0}};  /* [dial][segment] for redraw */
static lv_obj_t *s_composite_head = NULL;
static lv_obj_t *s_composite_sub  = NULL;
static lv_obj_t *s_composite_card = NULL;  /* container — reborder on agent */
static lv_obj_t *s_composite_accent = NULL; /* amber/violet bar top-left */
static lv_obj_t *s_composite_kicker = NULL; /* "RESOLVES TO" / "AGENT MODE" */

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

/* ── Forward decls ───────────────────────────────────────────────────── */
static void refresh_segments(void);
static void refresh_composite(void);
static void persist_and_notify_dragon(void);
static void seg_click_cb(lv_event_t *e);
static void done_click_cb(lv_event_t *e);
static void scrim_click_cb(lv_event_t *e);
static void show_agent_consent(uint8_t prev_aut_tier);
/* Wave 10 H5 Presets: forward decl — body below seg_click_cb. */
void preset_click_cb(lv_event_t *e);
static void hide_agent_consent(bool commit);
static void consent_confirm_cb(lv_event_t *e);
static void consent_cancel_cb(lv_event_t *e);

/* ── Public API ──────────────────────────────────────────────────────── */

bool ui_mode_sheet_visible(void)
{
    return s_overlay != NULL && !lv_obj_has_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
}

void ui_mode_sheet_hide(void)
{
    /* If an Agent consent modal is still up when the sheet gets hidden
     * (e.g. user backs out via nav), treat that as Cancel — do NOT commit. */
    if (s_consent_overlay) {
        lv_obj_del(s_consent_overlay);
        s_consent_overlay = NULL;
        s_aut_tier = s_pre_consent_aut;
    }
    if (s_overlay) {
        lv_obj_del(s_overlay);
    }
    s_overlay = NULL;
    s_sheet   = NULL;
    s_composite_head = NULL;
    s_composite_sub  = NULL;
    s_composite_card = NULL;
    s_composite_accent = NULL;
    s_composite_kicker = NULL;
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) s_seg_btn[r][c] = NULL;
    }
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
    lv_label_set_text(kicker, "\xe2\x80\xa2 MODE DIALS");
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

    /* Build three dial rows.
     * Each row: kicker label (12 px above segments) + segmented control. */
    struct {
        const char *kicker;
        const char *labels[3];
        int count;
    } rows[3] = {
        { "\xe2\x80\xa2 INTELLIGENCE",
          { "Fast", "Balanced", "Smart" }, 3 },
        { "\xe2\x80\xa2 VOICE",
          { "Local", "Neutral", "Studio" }, 3 },
        { "\xe2\x80\xa2 AUTONOMY",
          { "Ask", "Agent", NULL }, 2 },
    };

    int y = 170;
    for (int r = 0; r < 3; r++) {
        lv_obj_t *rk = lv_label_create(s_sheet);
        lv_label_set_text(rk, rows[r].kicker);
        lv_obj_set_style_text_font(rk, FONT_SMALL, 0);
        lv_obj_set_style_text_color(rk, lv_color_hex(TH_AMBER), 0);
        lv_obj_set_style_text_letter_space(rk, 4, 0);
        lv_obj_set_pos(rk, SIDE_PAD, y);

        /* Segment track — elevated card + padded row of buttons */
        lv_obj_t *track = lv_obj_create(s_sheet);
        lv_obj_remove_style_all(track);
        lv_obj_set_pos(track, SIDE_PAD, y + 30);
        lv_obj_set_size(track, MS_W - 2 * SIDE_PAD, SEG_H);
        lv_obj_set_style_bg_color(track, lv_color_hex(TH_CARD_ELEVATED), 0);
        lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(track, SEG_H / 2, 0);
        lv_obj_set_style_border_width(track, 1, 0);
        lv_obj_set_style_border_color(track, lv_color_hex(0x1E1E2A), 0);
        lv_obj_clear_flag(track, LV_OBJ_FLAG_SCROLLABLE);

        const int track_pad  = 4;
        const int seg_count  = rows[r].count;
        const int track_w    = (MS_W - 2 * SIDE_PAD) - 2 * track_pad;
        const int seg_w      = track_w / seg_count;

        for (int c = 0; c < seg_count; c++) {
            lv_obj_t *seg = lv_obj_create(track);
            lv_obj_remove_style_all(seg);
            lv_obj_set_pos(seg, track_pad + c * seg_w, track_pad);
            lv_obj_set_size(seg, seg_w, SEG_H - 2 * track_pad);
            lv_obj_set_style_radius(seg, (SEG_H - 2 * track_pad) / 2, 0);
            lv_obj_clear_flag(seg, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(seg, LV_OBJ_FLAG_CLICKABLE);
            /* user_data packs dial row (high nibble) + segment idx (low nibble) */
            uintptr_t pack = (uintptr_t)((r << 4) | c);
            lv_obj_add_event_cb(seg, seg_click_cb, LV_EVENT_CLICKED, (void*)pack);

            lv_obj_t *lbl = lv_label_create(seg);
            lv_label_set_text(lbl, rows[r].labels[c]);
            lv_obj_set_style_text_font(lbl, FONT_BODY, 0);
            lv_obj_center(lbl);

            s_seg_btn[r][c] = seg;
        }
        y += ROW_H + ROW_GAP;
    }

    /* Wave 10 H5: Presets row — four chips matching the legacy voice_mode
     * values so users can one-tap a recipe instead of spinning each dial.
     * Presets set (int/voi/aut) to the same mapping the reverse-derive
     * uses at line ~115 above:
     *   Local  -> (0,0,0)   Hybrid -> (1,2,0)
     *   Cloud  -> (2,2,0)   Agent  -> keep int/voi, aut=1 (triggers consent)
     * Tapping Agent routes through show_agent_consent just like the
     * dial segment does at line 460 — no silent mode-3 switch. */
    {
        lv_obj_t *rk = lv_label_create(s_sheet);
        lv_label_set_text(rk, "\xe2\x80\xa2 PRESETS");
        lv_obj_set_style_text_font(rk, FONT_SMALL, 0);
        lv_obj_set_style_text_color(rk, lv_color_hex(TH_AMBER), 0);
        lv_obj_set_style_text_letter_space(rk, 4, 0);
        lv_obj_set_pos(rk, SIDE_PAD, y);

        lv_obj_t *row = lv_obj_create(s_sheet);
        lv_obj_remove_style_all(row);
        lv_obj_set_pos(row, SIDE_PAD, y + 30);
        lv_obj_set_size(row, MS_W - 2 * SIDE_PAD, SEG_H);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        /* TT #328 Wave 9 follow-up — Onboard added as a 5th preset.
         * vmode=4 (K144) doesn't fit the int/voi/aut dial taxonomy
         * (it's an entirely different runtime — on-device chip, no
         * Dragon involvement); preset_click_cb's case 4 takes a
         * direct-write path that bypasses tab5_mode_resolve.  Closes
         * the "K144 unreachable from mode-sheet" half of audit P0 #10. */
        const int gap = 8;
        const int chip_count = 5;
        const int chip_w = ((MS_W - 2 * SIDE_PAD) - gap * (chip_count - 1)) / chip_count;
        const char *labels[5] = {"Local", "Hybrid", "Cloud", "Agent", "Onboard"};
        extern void preset_click_cb(lv_event_t *e);
        for (int c = 0; c < chip_count; c++) {
            lv_obj_t *chip = lv_obj_create(row);
            lv_obj_remove_style_all(chip);
            lv_obj_set_pos(chip, c * (chip_w + gap), 0);
            lv_obj_set_size(chip, chip_w, SEG_H);
            lv_obj_set_style_bg_color(chip, lv_color_hex(TH_CARD_ELEVATED), 0);
            lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(chip, SEG_H / 2, 0);
            lv_obj_set_style_border_width(chip, 1, 0);
            lv_obj_set_style_border_color(chip, lv_color_hex(0x1E1E2A), 0);
            lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(chip, preset_click_cb, LV_EVENT_CLICKED,
                                (void *)(uintptr_t)c);
            lv_obj_t *lbl = lv_label_create(chip);
            lv_label_set_text(lbl, labels[c]);
            lv_obj_set_style_text_font(lbl, FONT_BODY, 0);
            lv_obj_center(lbl);
        }
        y += ROW_H + ROW_GAP;
    }

    /* Composite preview — lives below the three dials. Border / accent /
     * kicker / text colours all swap to violet when aut_tier == 1 to
     * signal the TinkerClaw memory-bypass (Phase 2c informed-consent). */
    s_composite_card = lv_obj_create(s_sheet);
    lv_obj_remove_style_all(s_composite_card);
    lv_obj_set_pos(s_composite_card, SIDE_PAD, y + 20);
    lv_obj_set_size(s_composite_card, MS_W - 2 * SIDE_PAD, 140);
    lv_obj_set_style_bg_color(s_composite_card, lv_color_hex(TH_CARD_ELEVATED), 0);
    lv_obj_set_style_bg_opa(s_composite_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_composite_card, 20, 0);
    lv_obj_set_style_border_width(s_composite_card, 1, 0);
    lv_obj_set_style_border_color(s_composite_card, lv_color_hex(0x1E1E2A), 0);
    lv_obj_clear_flag(s_composite_card, LV_OBJ_FLAG_SCROLLABLE);

    s_composite_accent = lv_obj_create(s_composite_card);
    lv_obj_remove_style_all(s_composite_accent);
    lv_obj_set_size(s_composite_accent, 140, 3);
    lv_obj_set_pos(s_composite_accent, 0, 0);
    lv_obj_set_style_bg_color(s_composite_accent, lv_color_hex(TH_AMBER), 0);
    lv_obj_set_style_bg_opa(s_composite_accent, LV_OPA_COVER, 0);

    s_composite_kicker = lv_label_create(s_composite_card);
    lv_label_set_text(s_composite_kicker, "\xe2\x80\xa2 RESOLVES TO");
    lv_obj_set_style_text_font(s_composite_kicker, FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_composite_kicker, lv_color_hex(TH_AMBER), 0);
    lv_obj_set_style_text_letter_space(s_composite_kicker, 4, 0);
    lv_obj_set_pos(s_composite_kicker, 24, 22);
    lv_obj_t *comp = s_composite_card; /* alias for remaining label placement */

    s_composite_head = lv_label_create(comp);
    lv_obj_set_style_text_font(s_composite_head, FONT_HEADING, 0);
    lv_obj_set_style_text_color(s_composite_head, lv_color_hex(TH_TEXT_PRIMARY), 0);
    lv_obj_set_pos(s_composite_head, 24, 48);

    s_composite_sub = lv_label_create(comp);
    lv_obj_set_style_text_font(s_composite_sub, FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_composite_sub, lv_color_hex(TH_TEXT_DIM), 0);
    lv_obj_set_style_text_letter_space(s_composite_sub, 3, 0);
    lv_obj_set_pos(s_composite_sub, 24, 86);

    /* Initial styling + composite text */
    refresh_segments();
    refresh_composite();

    /* Force full-screen invalidate — same pattern as ui_home create
     * (PARTIAL render needs this to paint every strip on first show). */
    lv_obj_invalidate(lv_layer_top());
    lv_refr_now(lv_display_get_default());
}

/* ── Internals ───────────────────────────────────────────────────────── */

static void refresh_segments(void)
{
    /* Dial 0 = int_tier, 1 = voi_tier, 2 = aut_tier */
    uint8_t sel[3] = { s_int_tier, s_voi_tier, s_aut_tier };
    int counts[3]  = { 3, 3, 2 };

    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < counts[r]; c++) {
            lv_obj_t *seg = s_seg_btn[r][c];
            if (!seg) continue;
            bool on = (sel[r] == c);
            if (on) {
                lv_obj_set_style_bg_color(seg, lv_color_hex(TH_AMBER), 0);
                lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
                /* Update label colour */
                lv_obj_t *lbl = lv_obj_get_child(seg, 0);
                if (lbl) {
                    lv_obj_set_style_text_color(lbl, lv_color_hex(TH_BG), 0);
                }
            } else {
                lv_obj_set_style_bg_opa(seg, LV_OPA_TRANSP, 0);
                lv_obj_t *lbl = lv_obj_get_child(seg, 0);
                if (lbl) {
                    lv_obj_set_style_text_color(lbl, lv_color_hex(TH_TEXT_SECONDARY), 0);
                }
            }
        }
    }
}

static void refresh_composite(void)
{
    if (!s_composite_head || !s_composite_sub || !s_composite_card) return;

    char model_out[64] = {0};
    uint8_t resolved = tab5_mode_resolve(s_int_tier, s_voi_tier, s_aut_tier,
                                          model_out, sizeof(model_out));
    const char *mode_name[] = { "Local", "Hybrid", "Full Cloud", "Agent \xe2\x80\xa2 TinkerClaw" };
    if (resolved > 3) resolved = 0;
    lv_label_set_text(s_composite_head, mode_name[resolved]);

    /* Sub-label is built live so it reflects the actual LLM the user picked
     * (gemini-3-flash-preview, gpt-4o-mini, etc) instead of hardcoding
     * Sonnet.  Shortens vendor/model to the tail after "/" and upper-cases
     * it to match the kicker typography. */
    char sub_buf[96] = {0};
    char short_model[48] = {0};
    {
        char lm[64] = {0};
        tab5_settings_get_llm_model(lm, sizeof(lm));
        if (lm[0]) {
            const char *slash = strchr(lm, '/');
            const char *tail  = slash ? slash + 1 : lm;
            snprintf(short_model, sizeof(short_model), "%.47s", tail);
            for (int i = 0; short_model[i]; i++) {
                if (short_model[i] >= 'a' && short_model[i] <= 'z')
                    short_model[i] -= 32;
            }
        }
    }
    /* TT #328 Wave 5 (audit Hybrid story) — captions now stamp three
     * decisive variables per mode: latency, privacy, cost.  Pre-Wave-5
     * the composite read "STUDIO VOICE · LOCAL BRAIN · ~$0.02" — true,
     * but the user couldn't see WHY they'd pick Hybrid over Local
     * (60 s vs. 4-8 s is the load-bearing reason).  Now every mode
     * surfaces its sweet-spot in the same shape so cross-comparison
     * is a glance, not an analysis. */
    switch (resolved) {
        case 0:
           snprintf(sub_buf, sizeof(sub_buf), "~60S \xe2\x80\xa2 100%% PRIVATE \xe2\x80\xa2 FREE");
           break;
        case 1:
           snprintf(sub_buf, sizeof(sub_buf), "4-8S \xe2\x80\xa2 PRIVATE BRAIN \xe2\x80\xa2 ~$0.02");
           break;
        case 2:
           snprintf(sub_buf, sizeof(sub_buf), "3-6S \xe2\x80\xa2 %s \xe2\x80\xa2 ~$0.04",
                    short_model[0] ? short_model : "CLOUD");
           break;
        case 3:
           snprintf(sub_buf, sizeof(sub_buf), "AGENT TOOLS \xe2\x80\xa2 MEMORY BYPASSED");
           break;
    }
    lv_label_set_text(s_composite_sub, sub_buf);

    /* v4·D Sovereign Halo Phase 2c: when aut_tier == 1 (Agent),
     * recolor the composite card to violet to flag the memory-bypass
     * boundary.  The user has already tapped Agent, so this isn't a
     * revert-confirm modal -- just a tonally distinct "this mode runs
     * differently" signal matching the Sovereign system-d-modes.html M5
     * warning sheet concept. */
    const bool agent = (s_aut_tier >= 1);
    uint32_t accent_col = agent ? 0xA78BFA : TH_AMBER;
    uint32_t border_col = agent ? 0xA78BFA : 0x1E1E2A;
    const char *kicker  = agent ? "\xe2\x80\xa2 AGENT MODE" : "\xe2\x80\xa2 RESOLVES TO";

    lv_obj_set_style_bg_color(s_composite_accent, lv_color_hex(accent_col), 0);
    lv_obj_set_style_border_color(s_composite_card, lv_color_hex(border_col), 0);
    lv_obj_set_style_border_opa(s_composite_card, agent ? 255 : 255, 0);
    if (s_composite_kicker) {
        lv_label_set_text(s_composite_kicker, kicker);
        lv_obj_set_style_text_color(s_composite_kicker, lv_color_hex(accent_col), 0);
    }
}

static void persist_and_notify_dragon(void)
{
    /* Persist the three tiers + the derived voice_mode + (optional) llm_model. */
    tab5_settings_set_int_tier(s_int_tier);
    tab5_settings_set_voi_tier(s_voi_tier);
    tab5_settings_set_aut_tier(s_aut_tier);

    char model_out[64] = {0};
    uint8_t new_mode = tab5_mode_resolve(s_int_tier, s_voi_tier, s_aut_tier,
                                          model_out, sizeof(model_out));
    tab5_settings_set_voice_mode(new_mode);
    if (model_out[0]) {
        tab5_settings_set_llm_model(model_out);
    }

    /* Fire config_update to Dragon so it swaps backends on the next turn.
     * Re-read llm_model from NVS to send what's actually stored (either
     * the newly-written cloud model or the pre-existing one). */
    char model_to_send[64] = {0};
    tab5_settings_get_llm_model(model_to_send, sizeof(model_to_send));
    voice_send_config_update(new_mode, model_to_send);

    ESP_LOGI(TAG, "Tier change resolved -> voice_mode=%d model=%s",
             new_mode, model_to_send);
}

/* Wave 10 H5 Presets: one-tap recipes that set all three dials to the
 * tier combination matching the legacy voice_mode (0=Local, 1=Hybrid,
 * 2=Cloud, 3=Agent). Agent preset routes through the same consent modal
 * that the AUTONOMY dial does so there's no silent bypass path. */
void preset_click_cb(lv_event_t *e)
{
    int preset = (int)(uintptr_t)lv_event_get_user_data(e);
    uint8_t prev_aut = s_aut_tier;
    switch (preset) {
        case 0: /* Local   */ s_int_tier = 0; s_voi_tier = 0; s_aut_tier = 0; break;
        case 1: /* Hybrid  */ s_int_tier = 1; s_voi_tier = 2; s_aut_tier = 0; break;
        case 2: /* Cloud   */ s_int_tier = 2; s_voi_tier = 2; s_aut_tier = 0; break;
        case 3: /* Agent   */
            /* Keep intelligence + voice tiers as-is — agent mode is only
             * about autonomy/memory-bypass. If already on Agent, no-op.
             * Otherwise gate behind the consent modal. */
            if (s_aut_tier == 1) return;
            s_aut_tier = 1;
            refresh_segments();
            refresh_composite();
            show_agent_consent(prev_aut);
            return;  /* do NOT persist yet — modal commits or reverts */
        case 4:      /* Onboard — TT #328 Wave 9 follow-up.  Direct vmode=4
                      * write that bypasses tab5_mode_resolve (which only
                      * maps to 0..3).  K144 is its own runtime — the dial
                      * taxonomy doesn't apply, so the dials' visual state
                      * stays at whatever the user last picked.  Closes the
                      * "K144 unreachable from sheet" half of audit P0 #10. */
           tab5_settings_set_voice_mode(VOICE_MODE_ONBOARD);
           char model[64] = {0};
           tab5_settings_get_llm_model(model, sizeof(model));
           voice_send_config_update(VOICE_MODE_ONBOARD, model);
           ESP_LOGI(TAG, "Onboard preset -> voice_mode=%d (K144)", VOICE_MODE_ONBOARD);
           ui_mode_sheet_hide();
           return;
        default: return;
    }
    refresh_segments();
    refresh_composite();
    persist_and_notify_dragon();
    /* TT #328 Wave 10 follow-up — dismiss the sheet on every preset tap
     * so the user immediately sees the change reflected on home + their
     * next navigation isn't shadowed by a sheet that's still up.  Pre-
     * fix only the Onboard preset (case 4) auto-dismissed; the other
     * four required a manual "Done" tap. */
    ui_mode_sheet_hide();
}

static void seg_click_cb(lv_event_t *e)
{
    uintptr_t pack = (uintptr_t)lv_event_get_user_data(e);
    int row = (int)((pack >> 4) & 0x0F);
    int col = (int)(pack & 0x0F);

    /* Phase 2c gate: tapping AUTONOMY→Agent while not already on Agent
     * must trigger the consent modal before we commit the mode switch.
     * Memory bypass is the most sensitive boundary in the system and
     * deserves an explicit acknowledge+back path, not a silent recolor. */
    if (row == 2 && col == 1 && s_aut_tier != 1) {
        uint8_t prev = s_aut_tier;
        s_aut_tier = 1;
        refresh_segments();
        refresh_composite();
        show_agent_consent(prev);
        return;  /* do NOT persist yet — modal commits or reverts */
    }

    switch (row) {
        case 0: s_int_tier = (uint8_t)col; break;
        case 1: s_voi_tier = (uint8_t)col; break;
        case 2: s_aut_tier = (uint8_t)col; break;
        default: return;
    }

    refresh_segments();
    refresh_composite();
    persist_and_notify_dragon();
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
    /* Generic callback mode (E3) — invoke caller's decision handler and
     * reset the callback slots. Does NOT touch s_aut_tier / persist. */
    if (s_consent_confirm_cb || s_consent_cancel_cb) {
        void (*cb)(void *) = commit ? s_consent_confirm_cb : s_consent_cancel_cb;
        void  *ctx         = s_consent_cb_ctx;
        s_consent_confirm_cb = NULL;
        s_consent_cancel_cb  = NULL;
        s_consent_cb_ctx     = NULL;
        if (cb) cb(ctx);
        return;
    }
    /* Legacy mode-sheet flow: commit persists tiers, cancel reverts. */
    if (commit) {
        persist_and_notify_dragon();
    } else {
        /* Revert to the pre-modal autonomy tier + rebuild segment UI. */
        s_aut_tier = s_pre_consent_aut;
        refresh_segments();
        refresh_composite();
    }
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
