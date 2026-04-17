/*
 * ui_memory.c — v5 Memory search overlay (minimal)
 *
 * Typography-forward search surface. Top: big "Memory" title + stats line.
 * Below the title: query pill with amber cursor + placeholder text.
 * Below the query: hit rows, each one a tag + when + excerpt.
 */

#include "ui_memory.h"
#include "ui_theme.h"
#include "ui_home.h"
#include "ui_core.h"
#include "config.h"
#include "esp_log.h"

static const char *TAG = "ui_memory";

#define SW        720
#define SH        1280
#define SIDE_PAD  52

static lv_obj_t *s_overlay  = NULL;
static lv_obj_t *s_back_btn = NULL;
static bool      s_visible  = false;

static void back_click_cb(lv_event_t *e)
{
    (void)e;
    ui_memory_hide();
}

static void overlay_gesture_cb(lv_event_t *e)
{
    (void)e;
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    /* Unified with agents/focus/sessions/notes/settings: swipe RIGHT or
     * BOTTOM closes. Memory used to be LEFT which broke muscle-memory. */
    if (dir == LV_DIR_RIGHT || dir == LV_DIR_BOTTOM) {
        ui_memory_hide();
    }
}

static void build_hit(lv_obj_t *parent, int y,
                      const char *tag, uint32_t tag_color,
                      const char *when, const char *excerpt)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, SW - 2 * SIDE_PAD, LV_SIZE_CONTENT);
    lv_obj_set_pos(c, SIDE_PAD, y);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 6, 0);
    lv_obj_set_style_pad_bottom(c, 18, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(0x1A1A24), 0);
    lv_obj_set_style_border_side(c, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);

    /* tag + when row */
    lv_obj_t *row = lv_obj_create(c);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_column(row, 12, 0);

    lv_obj_t *tag_lbl = lv_label_create(row);
    lv_label_set_text(tag_lbl, tag);
    lv_obj_set_style_text_font(tag_lbl, FONT_CAPTION, 0);
    lv_obj_set_style_text_color(tag_lbl, lv_color_hex(tag_color), 0);
    lv_obj_set_style_text_letter_space(tag_lbl, 3, 0);

    lv_obj_t *sp = lv_obj_create(row);
    lv_obj_remove_style_all(sp);
    lv_obj_set_flex_grow(sp, 1);
    lv_obj_set_height(sp, 1);

    lv_obj_t *time_lbl = lv_label_create(row);
    lv_label_set_text(time_lbl, when);
    lv_obj_set_style_text_font(time_lbl, FONT_CAPTION, 0);
    lv_obj_set_style_text_color(time_lbl, lv_color_hex(0x55555D), 0);
    lv_obj_set_style_text_letter_space(time_lbl, 2, 0);

    /* excerpt */
    lv_obj_t *ex = lv_label_create(c);
    lv_label_set_long_mode(ex, LV_LABEL_LONG_WRAP);
    lv_label_set_text(ex, excerpt);
    lv_obj_set_width(ex, SW - 2 * SIDE_PAD);
    lv_obj_set_style_text_font(ex, FONT_SMALL, 0);
    lv_obj_set_style_text_color(ex, lv_color_hex(TH_TEXT_BODY), 0);
    lv_obj_set_style_text_line_space(ex, 4, 0);
}

void ui_memory_show(void)
{
    if (s_visible && s_overlay) {
        lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_overlay);
        return;
    }

    lv_obj_t *parent = ui_home_get_screen();
    if (!parent) parent = lv_screen_active();
    if (!parent) return;

    s_overlay = lv_obj_create(parent);
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, SW, SH);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(TH_BG), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(s_overlay, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_dir(s_overlay, LV_DIR_VER);
    lv_obj_add_event_cb(s_overlay, overlay_gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_GESTURE_BUBBLE);

    /* Back affordance top-left */
    s_back_btn = lv_button_create(s_overlay);
    lv_obj_set_size(s_back_btn, 120, 60);
    lv_obj_set_pos(s_back_btn, 24, 30);
    lv_obj_set_style_bg_opa(s_back_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(s_back_btn, 0, 0);
    lv_obj_set_style_border_width(s_back_btn, 0, 0);
    lv_obj_add_event_cb(s_back_btn, back_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(s_back_btn);
    lv_label_set_text(bl, LV_SYMBOL_LEFT "  HOME");
    lv_obj_set_style_text_font(bl, FONT_CAPTION, 0);
    lv_obj_set_style_text_color(bl, lv_color_hex(TH_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_letter_space(bl, 3, 0);
    lv_obj_center(bl);

    /* Header */
    lv_obj_t *head = lv_label_create(s_overlay);
    lv_label_set_text(head, "Memory");
    lv_obj_set_style_text_font(head, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(head, lv_color_hex(TH_AMBER), 0);
    lv_obj_set_pos(head, SIDE_PAD, 110);

    lv_obj_t *stats = lv_label_create(s_overlay);
    lv_label_set_text(stats, "12,340  •  3.4 MB  •  QWEN3-EMBEDDING");
    lv_obj_set_style_text_font(stats, FONT_CAPTION, 0);
    lv_obj_set_style_text_color(stats, lv_color_hex(TH_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_letter_space(stats, 3, 0);
    lv_obj_set_pos(stats, SIDE_PAD, 190);

    /* Query pill */
    lv_obj_t *pill = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(pill);
    lv_obj_set_size(pill, SW - 2 * SIDE_PAD, 72);
    lv_obj_set_pos(pill, SIDE_PAD, 230);
    lv_obj_set_style_bg_color(pill, lv_color_hex(TH_CARD), 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pill, 16, 0);
    lv_obj_set_style_border_width(pill, 1, 0);
    lv_obj_set_style_border_color(pill, lv_color_hex(TH_CARD_BORDER), 0);
    lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cur = lv_obj_create(pill);
    lv_obj_remove_style_all(cur);
    lv_obj_set_size(cur, 2, 36);
    lv_obj_set_pos(cur, 18, 18);
    lv_obj_set_style_bg_color(cur, lv_color_hex(TH_AMBER), 0);
    lv_obj_set_style_bg_opa(cur, LV_OPA_COVER, 0);

    lv_obj_t *q = lv_label_create(pill);
    lv_label_set_text(q, "find anything you said to me...");
    lv_obj_set_style_text_font(q, FONT_BODY, 0);
    lv_obj_set_style_text_color(q, lv_color_hex(TH_TEXT_DIM), 0);
    lv_obj_set_pos(q, 34, 26);

    /* Demo hits — static so the surface is demonstrable. */
    build_hit(s_overlay, 340,
              "CHAT  •  SONNET", TH_AMBER, "TODAY  •  10:15",
              "\"Compare AirPods Pro 2 vs Sony XM5 -- which for calls?\" "
              "-- XM5 won on call quality, AirPods tighter latency.");
    build_hit(s_overlay, 500,
              "NOTE  •  VOICE", TH_AMBER, "APR 13  •  22:04",
              "Weekend spending: headphones case replacement, new monitor "
              "stand, groceries.");
    build_hit(s_overlay, 640,
              "CHAT  •  LOCAL", TH_AMBER, "APR 9  •  08:30",
              "\"my airpods are dropping audio mid-call\" -- yes, hold setup "
              "button 15 s in the case to reset.");
    build_hit(s_overlay, 800,
              "CHAT  •  HYBRID", TH_AMBER, "MAR 28  •  13:12",
              "Asked about spatial audio with Ableton on macOS. Answer: no, "
              "only Music / TV apps.");

    s_visible = true;
    ESP_LOGI(TAG, "memory overlay shown");
}

void ui_memory_hide(void)
{
    if (!s_visible) return;
    if (s_overlay) { lv_obj_del(s_overlay); s_overlay = NULL; }
    s_back_btn = NULL;
    s_visible = false;
    ESP_LOGI(TAG, "memory overlay hidden");
}

bool ui_memory_is_visible(void)
{
    return s_visible;
}
