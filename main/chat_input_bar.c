/**
 * Chat Input Bar — v4·C Ambient say-pill.
 *
 * 108 × (720-80) pill at bottom with a 84 amber orb-ball, ghost hint,
 * 56 px keyboard affordance on the right.  Byte-identical to the home
 * say-pill (ui_home.c) so the two surfaces read as one product.
 */
#include "chat_input_bar.h"
#include "ui_theme.h"
#include "config.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "input_bar";

/* Pixel spec (raw 720×1280 px) — match ui_home.c. */
#define SW              720
#define PILL_SIDE_PAD   40
#define PILL_BOT_PAD    40
#define PILL_H          108
#define PILL_R          54
#define PILL_W          (SW - 2 * PILL_SIDE_PAD)
#define PILL_BALL_SZ    84
#define PILL_BALL_PAD   12
#define PILL_KB_SZ      56
#define PILL_TEXT_X     (PILL_BALL_PAD + PILL_BALL_SZ + 20)

struct chat_input_bar {
    lv_obj_t *pill;          /* 108×... rounded pill */
    lv_obj_t *ball;           /* 84×84 amber disc */
    lv_obj_t *ball_mark;      /* dot glyph inside the ball */
    lv_obj_t *ghost;          /* placeholder text */
    lv_obj_t *cursor;          /* blinking caret */
    lv_obj_t *kb_btn;          /* 56×56 keyboard affordance */
    lv_obj_t *textarea;        /* hidden textarea used by ui_keyboard_show */
    lv_obj_t *partial;         /* optional STT partial line above the pill */
    lv_timer_t *cursor_blink;

    int voice_state;
    lv_anim_t breath_anim;
    bool breathing;

    /* #190: create-time Y of the pill, saved so keyboard-raise can restore
     * it after the keyboard hides. */
    int default_pill_y;

    chat_input_evt_cb_t   ball_cb;     void *ball_ud;
    chat_input_evt_cb_t   kb_cb;       void *kb_ud;
    chat_input_submit_cb_t submit_cb;   void *submit_ud;
    chat_input_evt_cb_t   pill_cb;     void *pill_ud;
};

/* ── Orb-ball gradient per voice_state ─────────────────────────── */

static void ball_paint(chat_input_bar_t *b, int state)
{
    if (!b || !b->ball) return;
    uint32_t top, bot;
    switch (state) {
        case 0: /* idle amber */
            top = 0xFBBF24; bot = TH_AMBER;       break;
        case 1: /* listening amber-hot */
            top = 0xFFD45A; bot = TH_AMBER;       break;
        case 2: /* processing — base amber, breathing opa */
            top = 0xFBBF24; bot = TH_AMBER;       break;
        case 3: /* speaking amber-dark */
            top = TH_AMBER; bot = TH_AMBER_DARK;  break;
        case 4: /* done — calm */
            top = 0xFBBF24; bot = TH_AMBER;       break;
        default:
            top = 0xFBBF24; bot = TH_AMBER;       break;
    }
    lv_obj_set_style_bg_color(b->ball, lv_color_hex(top), 0);
    lv_obj_set_style_bg_grad_color(b->ball, lv_color_hex(bot), 0);
    lv_obj_set_style_bg_grad_dir(b->ball, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(b->ball, LV_OPA_COVER, 0);
}

static void breath_anim_cb(void *var, int32_t v)
{
    chat_input_bar_t *b = (chat_input_bar_t *)var;
    if (!b || !b->ball) return;
    /* v ranges 150..255 → opa for a gentle pulse. */
    lv_obj_set_style_bg_opa(b->ball, (lv_opa_t)v, 0);
}

static void start_breathing(chat_input_bar_t *b)
{
    if (!b || b->breathing) return;
    /* v4·D TC polish: dial way down.  Was 150->255 (58%->100%) over
     * 900ms, which read as an aggressive flash especially during
     * multi-second TC turns where the ball stayed breathing for
     * 30+ s.  Now 210->255 (82%->100%) over 2000 ms -- the ball
     * still clearly pulses, but as a slow glow instead of a strobe. */
    lv_anim_init(&b->breath_anim);
    lv_anim_set_var(&b->breath_anim, b);
    lv_anim_set_exec_cb(&b->breath_anim, breath_anim_cb);
    lv_anim_set_values(&b->breath_anim, 210, 255);
    lv_anim_set_time(&b->breath_anim, 2000);
    lv_anim_set_playback_time(&b->breath_anim, 2000);
    lv_anim_set_repeat_count(&b->breath_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&b->breath_anim);
    b->breathing = true;
}

static void stop_breathing(chat_input_bar_t *b)
{
    if (!b || !b->breathing) return;
    lv_anim_delete(b, breath_anim_cb);
    b->breathing = false;
    if (b->ball) lv_obj_set_style_bg_opa(b->ball, LV_OPA_COVER, 0);
}

/* ── Ghost + cursor refresh based on textarea contents ─────────── */

static void refresh_ghost(chat_input_bar_t *b)
{
    if (!b) return;
    const char *t = b->textarea ? lv_textarea_get_text(b->textarea) : "";
    bool empty = !t || !*t;
    if (b->ghost) {
        if (empty) lv_obj_clear_flag(b->ghost, LV_OBJ_FLAG_HIDDEN);
        else       lv_obj_add_flag(b->ghost, LV_OBJ_FLAG_HIDDEN);
    }
    if (b->cursor) {
        if (empty) lv_obj_clear_flag(b->cursor, LV_OBJ_FLAG_HIDDEN);
        else       lv_obj_add_flag(b->cursor, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ── Event trampolines ─────────────────────────────────────────── */
static void ev_ball_tap(lv_event_t *e)
{
    chat_input_bar_t *b = lv_event_get_user_data(e);
    if (b && b->ball_cb) b->ball_cb(b->ball_ud);
}
static void ev_kb_tap(lv_event_t *e)
{
    chat_input_bar_t *b = lv_event_get_user_data(e);
    if (b && b->kb_cb) b->kb_cb(b->kb_ud);
}
static void ev_pill_tap(lv_event_t *e)
{
    chat_input_bar_t *b = lv_event_get_user_data(e);
    if (b && b->pill_cb) b->pill_cb(b->pill_ud);
}
static void ev_ta_ready(lv_event_t *e)
{
    chat_input_bar_t *b = lv_event_get_user_data(e);
    if (!b) return;
    const char *t = b->textarea ? lv_textarea_get_text(b->textarea) : "";
    if (b->submit_cb && t && *t) b->submit_cb(t, b->submit_ud);
}
static void ev_ta_value(lv_event_t *e)
{
    chat_input_bar_t *b = lv_event_get_user_data(e);
    refresh_ghost(b);
}

/* ── Cursor blink timer ────────────────────────────────────────── */

static void cursor_blink_cb(lv_timer_t *t)
{
    chat_input_bar_t *b = lv_timer_get_user_data(t);
    if (!b || !b->cursor) return;
    if (lv_obj_has_flag(b->cursor, LV_OBJ_FLAG_HIDDEN)) return;
    static bool tick = false;
    tick = !tick;
    lv_obj_set_style_bg_opa(b->cursor, tick ? LV_OPA_COVER : LV_OPA_30, 0);
}

/* ── Create ────────────────────────────────────────────────────── */

chat_input_bar_t *chat_input_bar_create(lv_obj_t *parent, int parent_h)
{
    chat_input_bar_t *b = calloc(1, sizeof(*b));
    if (!b) { ESP_LOGE(TAG, "OOM"); return NULL; }
    int pill_y = parent_h - PILL_BOT_PAD - PILL_H;
    b->default_pill_y = pill_y;

    /* Pill container — TH_CARD fill, 1 px border, radius 54. */
    b->pill = lv_obj_create(parent);
    lv_obj_remove_style_all(b->pill);
    lv_obj_set_size(b->pill, PILL_W, PILL_H);
    lv_obj_set_pos(b->pill, PILL_SIDE_PAD, pill_y);
    lv_obj_set_style_bg_color(b->pill, lv_color_hex(TH_CARD), 0);
    lv_obj_set_style_bg_opa(b->pill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b->pill, PILL_R, 0);
    lv_obj_set_style_border_width(b->pill, 1, 0);
    lv_obj_set_style_border_color(b->pill, lv_color_hex(0x1E1E2A), 0);
    lv_obj_clear_flag(b->pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b->pill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b->pill, ev_pill_tap, LV_EVENT_CLICKED, b);

    /* Amber ball — 84×84 radial (vertical gradient is the LVGL 9
     * equivalent that ui_home.c ships with).  Click steals from the
     * pill so ball-tap is distinct from pill-tap. */
    b->ball = lv_obj_create(b->pill);
    lv_obj_remove_style_all(b->ball);
    lv_obj_set_size(b->ball, PILL_BALL_SZ, PILL_BALL_SZ);
    lv_obj_set_pos(b->ball, PILL_BALL_PAD, PILL_BALL_PAD);
    lv_obj_set_style_radius(b->ball, LV_RADIUS_CIRCLE, 0);
    ball_paint(b, 0);
    lv_obj_add_flag(b->ball, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(b->ball, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b->ball, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(b->ball, ev_ball_tap, LV_EVENT_CLICKED, b);

    b->ball_mark = lv_label_create(b->ball);
    lv_label_set_text(b->ball_mark, "\xe2\x80\xa2");   /* U+2022 bullet */
    lv_obj_set_style_text_font(b->ball_mark, FONT_CLOCK, 0);
    lv_obj_set_style_text_color(b->ball_mark, lv_color_hex(TH_BG), 0);
    lv_obj_align(b->ball_mark, LV_ALIGN_CENTER, 0, -6);

    /* Hidden textarea — sits inside the pill but is invisible; shown
     * only when ui_keyboard attaches to it. The visible affordance is
     * the ghost label + blinking cursor. */
    b->textarea = lv_textarea_create(b->pill);
    lv_obj_remove_style_all(b->textarea);
    lv_obj_set_size(b->textarea, PILL_W - PILL_TEXT_X - PILL_KB_SZ - 20, 48);
    lv_obj_set_pos(b->textarea, PILL_TEXT_X, (PILL_H - 48) / 2);
    lv_textarea_set_one_line(b->textarea, true);
    lv_textarea_set_placeholder_text(b->textarea, "");
    lv_obj_set_style_bg_opa(b->textarea, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(b->textarea, 0, 0);
    lv_obj_set_style_text_font(b->textarea, FONT_BODY, 0);
    lv_obj_set_style_text_color(b->textarea, lv_color_hex(TH_TEXT_PRIMARY), 0);
    lv_obj_set_style_pad_all(b->textarea, 0, 0);
    lv_obj_add_event_cb(b->textarea, ev_ta_ready, LV_EVENT_READY, b);
    lv_obj_add_event_cb(b->textarea, ev_ta_value, LV_EVENT_VALUE_CHANGED, b);

    /* Blinking cursor (only when textarea is empty — otherwise the
     * textarea's own caret handles it). */
    b->cursor = lv_obj_create(b->pill);
    lv_obj_remove_style_all(b->cursor);
    lv_obj_set_size(b->cursor, 2, 24);
    lv_obj_set_pos(b->cursor, PILL_TEXT_X, (PILL_H - 24) / 2);
    lv_obj_set_style_bg_color(b->cursor, lv_color_hex(TH_AMBER), 0);
    lv_obj_set_style_bg_opa(b->cursor, LV_OPA_COVER, 0);

    /* Ghost text to the right of the cursor. */
    b->ghost = lv_label_create(b->pill);
    lv_label_set_text(b->ghost, "Hold to speak \xe2\x80\xa2 or type");
    lv_obj_set_style_text_font(b->ghost, FONT_BODY, 0);
    lv_obj_set_style_text_color(b->ghost, lv_color_hex(TH_TEXT_DIM), 0);
    lv_obj_set_pos(b->ghost, PILL_TEXT_X + 10, (PILL_H - 24) / 2);

    /* Keyboard affordance — 56×56 elevated card at the right. */
    b->kb_btn = lv_obj_create(b->pill);
    lv_obj_remove_style_all(b->kb_btn);
    lv_obj_set_size(b->kb_btn, PILL_KB_SZ, PILL_KB_SZ);
    lv_obj_set_pos(b->kb_btn, PILL_W - PILL_KB_SZ - 12, (PILL_H - PILL_KB_SZ) / 2);
    lv_obj_set_style_bg_color(b->kb_btn, lv_color_hex(TH_CARD_ELEVATED), 0);
    lv_obj_set_style_bg_opa(b->kb_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b->kb_btn, 18, 0);
    lv_obj_set_style_border_width(b->kb_btn, 1, 0);
    lv_obj_set_style_border_color(b->kb_btn, lv_color_hex(0x1E1E2A), 0);
    lv_obj_add_flag(b->kb_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(b->kb_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(b->kb_btn, ev_kb_tap, LV_EVENT_CLICKED, b);
    lv_obj_t *kb_lbl = lv_label_create(b->kb_btn);
    lv_label_set_text(kb_lbl, LV_SYMBOL_KEYBOARD);
    lv_obj_set_style_text_font(kb_lbl, FONT_BODY, 0);
    lv_obj_set_style_text_color(kb_lbl, lv_color_hex(TH_TEXT_BODY), 0);
    lv_obj_center(kb_lbl);

    /* Partial-caption label above the pill, hidden by default. */
    b->partial = lv_label_create(parent);
    lv_label_set_text(b->partial, "");
    lv_obj_set_style_text_font(b->partial, FONT_CHAT_MONO, 0);
    lv_obj_set_style_text_color(b->partial, lv_color_hex(TH_AMBER), 0);
    lv_obj_set_pos(b->partial, PILL_SIDE_PAD + 16, pill_y - 24);
    lv_obj_add_flag(b->partial, LV_OBJ_FLAG_HIDDEN);

    /* Kick off cursor blink. */
    b->cursor_blink = lv_timer_create(cursor_blink_cb, 500, b);

    refresh_ghost(b);
    return b;
}

void chat_input_bar_destroy(chat_input_bar_t *b)
{
    if (!b) return;
    stop_breathing(b);
    if (b->cursor_blink) { lv_timer_delete(b->cursor_blink); b->cursor_blink = NULL; }
    if (b->pill)    lv_obj_del(b->pill);
    if (b->partial) lv_obj_del(b->partial);
    free(b);
}

void chat_input_bar_set_ghost(chat_input_bar_t *b, const char *hint)
{
    if (!b || !b->ghost) return;
    lv_label_set_text(b->ghost, hint ? hint : "");
}

void chat_input_bar_set_voice_state(chat_input_bar_t *b, int state)
{
    if (!b) return;
    b->voice_state = state;
    ball_paint(b, state);
    if (state == 2) start_breathing(b);
    else            stop_breathing(b);
    /* When listening, the ghost hint shifts to reassure the user. */
    if (b->ghost) {
        switch (state) {
            case 1: lv_label_set_text(b->ghost, "Listening..."); break;
            case 2: lv_label_set_text(b->ghost, "Thinking...");  break;
            case 3: lv_label_set_text(b->ghost, "Speaking...");  break;
            default:
                lv_label_set_text(b->ghost, "Hold to speak \xe2\x80\xa2 or type");
                break;
        }
    }
}

void chat_input_bar_show_partial(chat_input_bar_t *b, const char *partial)
{
    if (!b || !b->partial) return;
    if (!partial || !*partial) {
        lv_obj_add_flag(b->partial, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_label_set_text(b->partial, partial);
    lv_obj_clear_flag(b->partial, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t *chat_input_bar_get_textarea(chat_input_bar_t *b)
{ return b ? b->textarea : NULL; }

/* #190: move the pill (and its partial-caption label) to a given Y.
 * The partial sits 24 px above the pill (see create path) — keep that
 * offset so the STT caption doesn't collide with the pill when raised. */
void chat_input_bar_set_pill_y(chat_input_bar_t *b, int y)
{
    if (!b || !b->pill) return;
    lv_obj_set_y(b->pill, y);
    if (b->partial) lv_obj_set_y(b->partial, y - 24);
}

void chat_input_bar_restore_pill_y(chat_input_bar_t *b)
{
    if (!b) return;
    chat_input_bar_set_pill_y(b, b->default_pill_y);
}

const char *chat_input_bar_get_text(chat_input_bar_t *b)
{
    if (!b || !b->textarea) return "";
    return lv_textarea_get_text(b->textarea);
}

void chat_input_bar_clear(chat_input_bar_t *b)
{
    if (b && b->textarea) {
        lv_textarea_set_text(b->textarea, "");
        refresh_ghost(b);
    }
}

void chat_input_bar_on_ball_tap(chat_input_bar_t *b, chat_input_evt_cb_t cb, void *ud)
{ if (b) { b->ball_cb = cb; b->ball_ud = ud; } }

void chat_input_bar_on_keyboard(chat_input_bar_t *b, chat_input_evt_cb_t cb, void *ud)
{ if (b) { b->kb_cb = cb; b->kb_ud = ud; } }

void chat_input_bar_on_text_submit(chat_input_bar_t *b, chat_input_submit_cb_t cb, void *ud)
{ if (b) { b->submit_cb = cb; b->submit_ud = ud; } }

void chat_input_bar_on_pill_tap(chat_input_bar_t *b, chat_input_evt_cb_t cb, void *ud)
{ if (b) { b->pill_cb = cb; b->pill_ud = ud; } }
