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

/* ── Public API ──────────────────────────────────────────────────────── */

bool ui_mode_sheet_visible(void)
{
    return s_overlay != NULL && !lv_obj_has_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
}

void ui_mode_sheet_hide(void)
{
    if (s_overlay) {
        lv_obj_del(s_overlay);
    }
    s_overlay = NULL;
    s_sheet   = NULL;
    s_composite_head = NULL;
    s_composite_sub  = NULL;
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

    ESP_LOGI(TAG, "Opening dial sheet (int=%d voi=%d aut=%d)",
             s_int_tier, s_voi_tier, s_aut_tier);

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

    /* Composite preview — lives below the three dials */
    lv_obj_t *comp = lv_obj_create(s_sheet);
    lv_obj_remove_style_all(comp);
    lv_obj_set_pos(comp, SIDE_PAD, y + 20);
    lv_obj_set_size(comp, MS_W - 2 * SIDE_PAD, 140);
    lv_obj_set_style_bg_color(comp, lv_color_hex(TH_CARD_ELEVATED), 0);
    lv_obj_set_style_bg_opa(comp, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(comp, 20, 0);
    lv_obj_set_style_border_width(comp, 1, 0);
    lv_obj_set_style_border_color(comp, lv_color_hex(0x1E1E2A), 0);
    lv_obj_clear_flag(comp, LV_OBJ_FLAG_SCROLLABLE);

    /* amber accent bar on top-left */
    lv_obj_t *accent = lv_obj_create(comp);
    lv_obj_remove_style_all(accent);
    lv_obj_set_size(accent, 140, 3);
    lv_obj_set_pos(accent, 0, 0);
    lv_obj_set_style_bg_color(accent, lv_color_hex(TH_AMBER), 0);
    lv_obj_set_style_bg_opa(accent, LV_OPA_COVER, 0);

    lv_obj_t *comp_k = lv_label_create(comp);
    lv_label_set_text(comp_k, "\xe2\x80\xa2 RESOLVES TO");
    lv_obj_set_style_text_font(comp_k, FONT_SMALL, 0);
    lv_obj_set_style_text_color(comp_k, lv_color_hex(TH_AMBER), 0);
    lv_obj_set_style_text_letter_space(comp_k, 4, 0);
    lv_obj_set_pos(comp_k, 24, 22);

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
    if (!s_composite_head || !s_composite_sub) return;

    char model_out[64] = {0};
    uint8_t resolved = tab5_mode_resolve(s_int_tier, s_voi_tier, s_aut_tier,
                                          model_out, sizeof(model_out));
    const char *mode_name[] = { "Local", "Hybrid", "Full Cloud", "TinkerClaw" };
    const char *mode_sub[]  = {
        "ON-DEVICE \xe2\x80\xa2 FREE",
        "STUDIO VOICE \xe2\x80\xa2 LOCAL BRAIN \xe2\x80\xa2 ~$0.02",
        "SONNET 4 \xe2\x80\xa2 STUDIO \xe2\x80\xa2 ~$0.04",
        "AGENT GATEWAY \xe2\x80\xa2 MULTI-STEP",
    };
    if (resolved > 3) resolved = 0;
    lv_label_set_text(s_composite_head, mode_name[resolved]);
    lv_label_set_text(s_composite_sub, mode_sub[resolved]);
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

static void seg_click_cb(lv_event_t *e)
{
    uintptr_t pack = (uintptr_t)lv_event_get_user_data(e);
    int row = (int)((pack >> 4) & 0x0F);
    int col = (int)(pack & 0x0F);

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
