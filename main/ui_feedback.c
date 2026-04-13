/**
 * ui_feedback.c — Unified touch feedback for TinkerOS
 *
 * Adds smooth pressed-state visual feedback to LVGL objects.
 * Uses static styles (shared across all objects of same type) to
 * minimize memory — no per-object allocation.
 */

#include "ui_feedback.h"

/* ── Transition: 100ms ease-out for snappy feel ──────────────── */
static const lv_style_prop_t _tr_props_bg[] = {
    LV_STYLE_BG_COLOR, LV_STYLE_BG_OPA, LV_STYLE_BORDER_COLOR, 0
};
static lv_style_transition_dsc_t _tr_bg;
static bool _tr_inited = false;

static void _ensure_tr(void)
{
    if (_tr_inited) return;
    lv_style_transition_dsc_init(&_tr_bg, _tr_props_bg, lv_anim_path_ease_out, 100, 0, NULL);
    _tr_inited = true;
}

/* ── Shared styles (one per feedback type) ───────────────────── */

/* Button pressed: darken by shifting to darker shade */
static lv_style_t _st_btn_press;
static bool _st_btn_ready = false;

/* Card pressed: lighten border + shift bg */
static lv_style_t _st_card_press;
static bool _st_card_ready = false;

/* Icon pressed: dim opacity */
static lv_style_t _st_icon_press;
static bool _st_icon_ready = false;

/* Nav pressed: brighten */
static lv_style_t _st_nav_press;
static bool _st_nav_ready = false;

/* ── Public API ──────────────────────────────────────────────── */

void ui_fb_button(lv_obj_t *obj)
{
    _ensure_tr();
    if (!_st_btn_ready) {
        lv_style_init(&_st_btn_press);
        lv_style_set_bg_opa(&_st_btn_press, LV_OPA_80);      /* Darken: 80% opacity */
        lv_style_set_transition(&_st_btn_press, &_tr_bg);
        _st_btn_ready = true;
    }
    lv_obj_add_style(obj, &_st_btn_press, LV_STATE_PRESSED);
}

void ui_fb_button_colored(lv_obj_t *obj, uint32_t pressed_hex)
{
    _ensure_tr();
    /* Per-object style for custom color — use inline style instead */
    lv_obj_set_style_bg_color(obj, lv_color_hex(pressed_hex),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_transition(obj, &_tr_bg,
                                LV_PART_MAIN | LV_STATE_PRESSED);
}

void ui_fb_card(lv_obj_t *obj)
{
    _ensure_tr();
    if (!_st_card_ready) {
        lv_style_init(&_st_card_press);
        lv_style_set_bg_color(&_st_card_press, lv_color_hex(0x252540));  /* Slight lighten */
        lv_style_set_border_color(&_st_card_press, lv_color_hex(0x555555)); /* Brighter border */
        lv_style_set_transition(&_st_card_press, &_tr_bg);
        _st_card_ready = true;
    }
    lv_obj_add_style(obj, &_st_card_press, LV_STATE_PRESSED);
}

void ui_fb_icon(lv_obj_t *obj)
{
    _ensure_tr();
    if (!_st_icon_ready) {
        lv_style_init(&_st_icon_press);
        lv_style_set_opa(&_st_icon_press, LV_OPA_60);         /* Dim to 60% */
        lv_style_set_transition(&_st_icon_press, &_tr_bg);
        _st_icon_ready = true;
    }
    lv_obj_add_style(obj, &_st_icon_press, LV_STATE_PRESSED);
}

void ui_fb_nav(lv_obj_t *obj)
{
    _ensure_tr();
    if (!_st_nav_ready) {
        lv_style_init(&_st_nav_press);
        lv_style_set_text_color(&_st_nav_press, lv_color_hex(0xFFFFFF)); /* Bright white */
        lv_style_set_opa(&_st_nav_press, LV_OPA_COVER);
        lv_style_set_transition(&_st_nav_press, &_tr_bg);
        _st_nav_ready = true;
    }
    lv_obj_add_style(obj, &_st_nav_press, LV_STATE_PRESSED);
}

void ui_fb_custom(lv_obj_t *obj, uint32_t pressed_bg_hex)
{
    ui_fb_button_colored(obj, pressed_bg_hex);
}
