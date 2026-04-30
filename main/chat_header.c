/**
 * Chat Header — v4·C Ambient composite widget.
 *
 * Spec §4.1: 720×96 root, Fraunces italic 32 title, mode chip + "+" at right,
 * 140×2 amber accent bar painted at y=96 in the parent.
 */
#include "chat_header.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "esp_log.h"
#include "ui_core.h"     /* TT #328 Wave 5: ui_tap_gate */
#include "ui_feedback.h" /* TT #328 Wave 10: ui_fb_* visual pressed states */
#include "ui_theme.h"
#include "widget_mode_dot.h" /* TT #328 Wave 6 */

static const char *TAG = "chat_hdr";

/* Pixel spec — raw px (720×1280 framebuffer, no DPI_SCALE since the spec
 * fixes these numbers).
 * TT #328 Wave 5: HDR_TOUCH bumped 44 → TOUCH_MIN (DPI_SCALE(44) = 60 px
 * at 218 DPI).  Pre-Wave-5 the chat header back / chevron / plus all sat
 * at the absolute minimum 44 px (= 6.5 mm), below the canonical
 * 7 mm / TOUCH_MIN that the rest of the firmware uses.  Audit P0 #6. */
#define HDR_H          96
#define HDR_SIDE_PAD   40
#define ACCENT_W       140
#define ACCENT_H       2
#define HDR_TOUCH TOUCH_MIN

static const uint32_t s_mode_tint[VOICE_MODE_COUNT] = {
    TH_MODE_LOCAL, TH_MODE_HYBRID, TH_MODE_CLOUD, TH_MODE_CLAW, TH_MODE_ONBOARD,
};
static const char *s_mode_short[VOICE_MODE_COUNT] = {"Local", "Hybrid", "Cloud", "Claw", "Onboard"};

struct chat_header {
    lv_obj_t *root;       /* 96-h bar */
    lv_obj_t *back;
    lv_obj_t *title;
    lv_obj_t *chev;
    lv_obj_t *chip;
    lv_obj_t *chip_dot;
    lv_obj_t *chip_name;
    lv_obj_t *chip_sub;
    lv_obj_t *plus;
    lv_obj_t *accent;     /* 140×2 bar below root */

    chat_header_evt_cb_t back_cb, chev_cb, plus_cb, mlp_cb;
    void *back_ud, *chev_ud, *plus_ud, *mlp_ud;
};

/* ── Event trampolines ─────────────────────────────────────────── */
/* TT #328 Wave 5 — universal tap-debounce.  Repeat-tap on any of these
 * stacked overlapping create/dismiss → SDIO TX copy_buff exhaustion
 * before the gate. */
static void ev_back(lv_event_t *e)
{
   if (!ui_tap_gate("chat:back", 300)) return;
   chat_header_t *h = lv_event_get_user_data(e);
   if (h && h->back_cb) h->back_cb(h->back_ud);
}
static void ev_chev(lv_event_t *e)
{
   if (!ui_tap_gate("chat:chev", 300)) return;
   chat_header_t *h = lv_event_get_user_data(e);
   if (h && h->chev_cb) h->chev_cb(h->chev_ud);
}
static void ev_plus(lv_event_t *e)
{
   if (!ui_tap_gate("chat:new", 300)) return;
   chat_header_t *h = lv_event_get_user_data(e);
   if (h && h->plus_cb) h->plus_cb(h->plus_ud);
}
static void ev_chip_lp(lv_event_t *e)
{
    chat_header_t *h = lv_event_get_user_data(e);
    if (h && h->mlp_cb) h->mlp_cb(h->mlp_ud);
}

/* ── Create ────────────────────────────────────────────────────── */
chat_header_t *chat_header_create(lv_obj_t *parent, const char *title)
{
    chat_header_t *h = calloc(1, sizeof(*h));
    if (!h) { ESP_LOGE(TAG, "OOM"); return NULL; }

    /* Root: absolute-positioned 96-h strip at top of parent, no flex
     * (we place children manually to match spec pixel coords). */
    h->root = lv_obj_create(parent);
    lv_obj_remove_style_all(h->root);
    lv_obj_set_size(h->root, 720, HDR_H);
    lv_obj_set_pos(h->root, 0, 0);
    lv_obj_set_style_bg_color(h->root, lv_color_hex(TH_BG), 0);
    lv_obj_set_style_bg_opa(h->root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(h->root, LV_OBJ_FLAG_SCROLLABLE);

    /* Back button — label wrapped in a clickable container for 44px touch. */
    h->back = lv_obj_create(h->root);
    lv_obj_remove_style_all(h->back);
    lv_obj_set_size(h->back, HDR_TOUCH, HDR_TOUCH);
    lv_obj_set_pos(h->back, HDR_SIDE_PAD - 10, (HDR_H - HDR_TOUCH) / 2);
    lv_obj_set_style_bg_opa(h->back, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(h->back, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(h->back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(h->back, ev_back, LV_EVENT_CLICKED, h);
    ui_fb_icon(h->back); /* TT #328 Wave 10 */
    lv_obj_t *back_lbl = lv_label_create(h->back);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(back_lbl, FONT_HEADING, 0);
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(TH_TEXT_BODY), 0);
    lv_obj_center(back_lbl);

    /* Title — Fraunces italic 32, left-aligned after back button. */
    h->title = lv_label_create(h->root);
    lv_label_set_text(h->title, title && *title ? title : "Chat");
    lv_obj_set_style_text_font(h->title, FONT_CHAT_TITLE, 0);
    lv_obj_set_style_text_color(h->title, lv_color_hex(TH_TEXT_PRIMARY), 0);
    lv_obj_set_pos(h->title, HDR_SIDE_PAD + 44, (HDR_H - 40) / 2);

    /* Chevron — right of title, independent 44×44 tap area. */
    h->chev = lv_obj_create(h->root);
    lv_obj_remove_style_all(h->chev);
    lv_obj_set_size(h->chev, HDR_TOUCH, HDR_TOUCH);
    /* y-center; x is set after title measures below, placeholder for now. */
    lv_obj_set_pos(h->chev, HDR_SIDE_PAD + 44 + 120, (HDR_H - HDR_TOUCH) / 2);
    lv_obj_set_style_bg_opa(h->chev, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(h->chev, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(h->chev, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(h->chev, ev_chev, LV_EVENT_CLICKED, h);
    ui_fb_icon(h->chev); /* TT #328 Wave 10 */
    lv_obj_t *chev_lbl = lv_label_create(h->chev);
    lv_label_set_text(chev_lbl, LV_SYMBOL_DOWN);
    lv_obj_set_style_text_font(chev_lbl, FONT_SECONDARY, 0);
    lv_obj_set_style_text_color(chev_lbl, lv_color_hex(TH_TEXT_DIM), 0);
    lv_obj_center(chev_lbl);

    /* "+" new chat button — elevated circular 44×44, amber glyph. */
    h->plus = lv_obj_create(h->root);
    lv_obj_remove_style_all(h->plus);
    lv_obj_set_size(h->plus, HDR_TOUCH, HDR_TOUCH);
    lv_obj_set_pos(h->plus, 720 - HDR_SIDE_PAD - HDR_TOUCH,
                   (HDR_H - HDR_TOUCH) / 2);
    lv_obj_set_style_bg_color(h->plus, lv_color_hex(TH_CARD_ELEVATED), 0);
    lv_obj_set_style_bg_opa(h->plus, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(h->plus, 22, 0);
    lv_obj_set_style_border_width(h->plus, 1, 0);
    lv_obj_set_style_border_color(h->plus, lv_color_hex(0x1E1E2A), 0);
    lv_obj_clear_flag(h->plus, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(h->plus, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(h->plus, ev_plus, LV_EVENT_CLICKED, h);
    ui_fb_button(h->plus); /* TT #328 Wave 10 */
    lv_obj_t *plus_lbl = lv_label_create(h->plus);
    lv_label_set_text(plus_lbl, "+");
    lv_obj_set_style_text_font(plus_lbl, FONT_HEADING, 0);
    lv_obj_set_style_text_color(plus_lbl, lv_color_hex(TH_AMBER), 0);
    lv_obj_center(plus_lbl);

    /* Mode chip — placed to the left of the "+" button.  Width is
     * content-driven; we use LV_SIZE_CONTENT then align manually. */
    h->chip = lv_obj_create(h->root);
    lv_obj_remove_style_all(h->chip);
    lv_obj_set_size(h->chip, LV_SIZE_CONTENT, HDR_TOUCH);
    lv_obj_set_style_bg_color(h->chip, lv_color_hex(TH_CARD_ELEVATED), 0);
    lv_obj_set_style_bg_opa(h->chip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(h->chip, 22, 0);
    lv_obj_set_style_border_width(h->chip, 1, 0);
    lv_obj_set_style_border_color(h->chip, lv_color_hex(0x1E1E2A), 0);
    lv_obj_set_style_pad_hor(h->chip, 18, 0);
    lv_obj_set_style_pad_ver(h->chip, 0, 0);
    lv_obj_set_style_pad_column(h->chip, 10, 0);
    lv_obj_set_flex_flow(h->chip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(h->chip, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(h->chip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(h->chip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(h->chip, 6);
    lv_obj_add_event_cb(h->chip, ev_chip_lp, LV_EVENT_LONG_PRESSED, h);

    /* TT #328 Wave 6 — shared mode-dot widget. */
    h->chip_dot = widget_mode_dot_create(h->chip, 8, 0 /* vmode=0 default;
                                                          set_mode below */);

    h->chip_name = lv_label_create(h->chip);
    lv_label_set_text(h->chip_name, "Local");
    lv_obj_set_style_text_font(h->chip_name, FONT_HEADING, 0);
    lv_obj_set_style_text_color(h->chip_name, lv_color_hex(TH_TEXT_PRIMARY), 0);

    h->chip_sub = lv_label_create(h->chip);
    lv_label_set_text(h->chip_sub, "");
    lv_obj_set_style_text_font(h->chip_sub, FONT_CHAT_MONO, 0);
    lv_obj_set_style_text_color(h->chip_sub, lv_color_hex(TH_TEXT_DIM), 0);

    /* Measure then align chip before the "+" button. */
    lv_obj_update_layout(h->chip);
    int chip_w = lv_obj_get_width(h->chip);
    if (chip_w < HDR_TOUCH * 2) chip_w = HDR_TOUCH * 2;
    int chip_x = 720 - HDR_SIDE_PAD - HDR_TOUCH - 12 - chip_w;
    if (chip_x < HDR_SIDE_PAD + 44 + 140) chip_x = HDR_SIDE_PAD + 44 + 140;
    lv_obj_set_pos(h->chip, chip_x, (HDR_H - HDR_TOUCH) / 2);

    /* 140×2 amber accent bar under the header, parent-relative y=96. */
    h->accent = lv_obj_create(parent);
    lv_obj_remove_style_all(h->accent);
    lv_obj_set_size(h->accent, ACCENT_W, ACCENT_H);
    lv_obj_set_pos(h->accent, HDR_SIDE_PAD, HDR_H);
    lv_obj_set_style_bg_color(h->accent, lv_color_hex(TH_AMBER), 0);
    lv_obj_set_style_bg_opa(h->accent, LV_OPA_COVER, 0);

    return h;
}

void chat_header_destroy(chat_header_t *h)
{
    if (!h) return;
    if (h->root)   lv_obj_del(h->root);
    if (h->accent) lv_obj_del(h->accent);
    free(h);
}

void chat_header_set_title(chat_header_t *h, const char *title)
{
    if (h && h->title) lv_label_set_text(h->title, title ? title : "");
}

void chat_header_set_mode(chat_header_t *h, uint8_t m, const char *llm)
{
    if (!h) return;
    if (m >= VOICE_MODE_COUNT) m = 0;
    /* TT #328 Wave 6 — recolor through the shared widget so any future
     * change to mode-dot rendering (opacity, gradient) flows everywhere. */
    if (h->chip_dot) widget_mode_dot_set_mode(h->chip_dot, m);
    if (h->chip_name) lv_label_set_text(h->chip_name, s_mode_short[m]);
    if (h->chip_sub) {
        /* v4·D connectivity polish: TinkerClaw mode talks to the openclaw
         * gateway via Dragon, so the NVS llm_model (which still holds the
         * Tab5-preferred Cloud model like "google/gemini-3-flash-preview")
         * is NOT what's actually serving the turn.  Show AGENT instead of
         * the stale Cloud model name.  For modes 0/1/2 we keep showing the
         * underlying LLM id since it IS the model in use. */
        char buf[32] = {0};
        if (m == 3) {
            snprintf(buf, sizeof(buf), "AGENT");
        } else {
            const char *nick = llm ? llm : "";
            const char *slash = strchr(nick, '/');
            if (slash) nick = slash + 1;
            size_t n = strlen(nick);
            if (n >= sizeof(buf)) n = sizeof(buf) - 1;
            memcpy(buf, nick, n);
            char *col = strchr(buf, ':');
            if (col) *col = 0;
            for (char *p = buf; *p; p++) {
                if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
            }
        }
        lv_label_set_text(h->chip_sub, buf);
    }
    chat_header_set_accent_color(h, s_mode_tint[m]);
    /* Recentre chip after label width change. */
    if (h->chip) {
        lv_obj_update_layout(h->chip);
        int chip_w = lv_obj_get_width(h->chip);
        int chip_x = 720 - HDR_SIDE_PAD - HDR_TOUCH - 12 - chip_w;
        if (chip_x < HDR_SIDE_PAD + 44 + 140) chip_x = HDR_SIDE_PAD + 44 + 140;
        lv_obj_set_pos(h->chip, chip_x, (HDR_H - HDR_TOUCH) / 2);
    }
}

void chat_header_set_accent_color(chat_header_t *h, uint32_t hex)
{
    if (h && h->accent) lv_obj_set_style_bg_color(h->accent, lv_color_hex(hex), 0);
}

void chat_header_set_chevron_open(chat_header_t *h, bool open)
{
    if (!h || !h->chev) return;
    lv_obj_t *lbl = lv_obj_get_child(h->chev, 0);
    if (lbl) lv_label_set_text(lbl, open ? LV_SYMBOL_UP : LV_SYMBOL_DOWN);
}

void chat_header_on_back(chat_header_t *h, chat_header_evt_cb_t cb, void *ud)
{ if (h) { h->back_cb = cb; h->back_ud = ud; } }

void chat_header_on_chevron(chat_header_t *h, chat_header_evt_cb_t cb, void *ud)
{ if (h) { h->chev_cb = cb; h->chev_ud = ud; } }

void chat_header_on_plus(chat_header_t *h, chat_header_evt_cb_t cb, void *ud)
{ if (h) { h->plus_cb = cb; h->plus_ud = ud; } }

void chat_header_on_mode_long_press(chat_header_t *h, chat_header_evt_cb_t cb, void *ud)
{ if (h) { h->mlp_cb = cb; h->mlp_ud = ud; } }
