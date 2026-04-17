/*
 * ui_focus.c — v5 FOCUS state overlay
 *
 * Corner orb (120 px) + narrative heartbeat + task stream + 'earlier today'
 * feed + 'say something' prompt.  Dismisses on back tap or swipe-down.
 *
 * Stability posture:
 *   - Creates a single lv_obj root; destroys on hide (consistent with
 *     ui_agents / ui_memory / ui_chat).
 *   - any_overlay_visible() on home already no-ops re-entrant swipe-ups,
 *     so double-swipes can't stack two focus overlays.
 *   - Demo content is static; no async allocations during the life of
 *     the overlay.
 */

#include "ui_focus.h"
#include "ui_theme.h"
#include "ui_home.h"
#include "ui_core.h"
#include "config.h"
#include "esp_log.h"

static const char *TAG = "ui_focus";

#define SW        720
#define SH        1280
#define SIDE_PAD  52
#define ORB_CORNER_SZ  120
#define ORB_CORNER_X   (SW - ORB_CORNER_SZ - 56)
#define ORB_CORNER_Y   64

static lv_obj_t *s_overlay  = NULL;
static lv_obj_t *s_back_btn = NULL;
static bool      s_visible  = false;

static void back_click_cb(lv_event_t *e)
{
    (void)e;
    ui_focus_hide();
}

static void overlay_gesture_cb(lv_event_t *e)
{
    (void)e;
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if (dir == LV_DIR_BOTTOM) {
        ui_focus_hide();
    }
}

/* A task row — coloured dot, label, right-aligned meta. */
static void build_task_row(lv_obj_t *parent, const char *label,
                           const char *meta, uint32_t dot_color)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_set_style_pad_ver(row, 4, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *dot = lv_obj_create(row);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 10, 10);
    lv_obj_set_style_radius(dot, 5, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(dot_color), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_font(lbl, FONT_BODY, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(TH_TEXT_PRIMARY), 0);
    lv_obj_set_flex_grow(lbl, 1);

    lv_obj_t *m = lv_label_create(row);
    lv_label_set_text(m, meta);
    lv_obj_set_style_text_font(m, FONT_CAPTION, 0);
    lv_obj_set_style_text_color(m, lv_color_hex(0x55555D), 0);
    lv_obj_set_style_text_letter_space(m, 1, 0);
}

static void build_feed_row(lv_obj_t *parent, const char *time_str,
                           const char *tag_str, uint32_t tag_color,
                           const char *title)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 14, 0);
    lv_obj_set_style_pad_ver(row, 10, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(0x1A1A24), 0);  /* hairline */
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(row);
    lv_label_set_text(t, time_str);
    lv_obj_set_style_text_font(t, FONT_CAPTION, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(0x55555D), 0);
    lv_obj_set_style_text_letter_space(t, 1, 0);
    lv_obj_set_width(t, 72);

    lv_obj_t *tag = lv_label_create(row);
    lv_label_set_text(tag, tag_str);
    lv_obj_set_style_text_font(tag, FONT_CAPTION, 0);
    lv_obj_set_style_text_color(tag, lv_color_hex(tag_color), 0);
    lv_obj_set_style_text_letter_space(tag, 2, 0);
    lv_obj_set_width(tag, 70);

    lv_obj_t *title_lbl = lv_label_create(row);
    lv_label_set_text(title_lbl, title);
    lv_obj_set_style_text_font(title_lbl, FONT_BODY, 0);
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(TH_TEXT_BODY), 0);
    lv_obj_set_flex_grow(title_lbl, 1);
    lv_label_set_long_mode(title_lbl, LV_LABEL_LONG_DOT);
}

void ui_focus_show(void)
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

    /* Orb in corner — solid amber fill (2-stop gradient to match home) */
    lv_obj_t *orb = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(orb);
    lv_obj_set_size(orb, ORB_CORNER_SZ, ORB_CORNER_SZ);
    lv_obj_set_pos(orb, ORB_CORNER_X, ORB_CORNER_Y);
    lv_obj_set_style_radius(orb, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(orb, lv_color_hex(0xFFC75A), 0);
    lv_obj_set_style_bg_grad_color(orb, lv_color_hex(0xB9650A), 0);
    lv_obj_set_style_bg_grad_dir(orb, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(orb, LV_OPA_COVER, 0);

    /* Back (top-left, tappable) */
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

    /* Section 1 — HEARTBEAT header + narrative */
    lv_obj_t *sec1 = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(sec1);
    lv_obj_set_size(sec1, SW - 2 * SIDE_PAD - ORB_CORNER_SZ - 40, LV_SIZE_CONTENT);
    lv_obj_set_pos(sec1, SIDE_PAD, 220);
    lv_obj_set_flex_flow(sec1, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(sec1, 14, 0);
    lv_obj_clear_flag(sec1, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *h = lv_label_create(sec1);
    lv_label_set_text(h, "HEARTBEAT  •  2 MIN LIVE");
    lv_obj_set_style_text_font(h, FONT_CAPTION, 0);
    lv_obj_set_style_text_color(h, lv_color_hex(TH_AMBER), 0);
    lv_obj_set_style_text_letter_space(h, 3, 0);

    lv_obj_t *narrative = lv_label_create(sec1);
    lv_label_set_long_mode(narrative, LV_LABEL_LONG_WRAP);
    lv_label_set_text(narrative,
        "I scanned your inbox -- three replies were waiting. "
        "Drafting one to Aisha now; the digest is queued.");
    lv_obj_set_style_text_font(narrative, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(narrative, lv_color_hex(TH_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_line_space(narrative, 4, 0);
    /* Reserve the top-right corner for the orb (140 px) so the narrative
     * can't collide with it. Spec shot-01 right pane. */
    lv_obj_set_width(narrative, SW - 2 * SIDE_PAD - 160);

    /* Task stream below the narrative */
    lv_obj_t *tasks = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(tasks);
    lv_obj_set_size(tasks, SW - 2 * SIDE_PAD, LV_SIZE_CONTENT);
    lv_obj_set_pos(tasks, SIDE_PAD, 480);
    lv_obj_set_flex_flow(tasks, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(tasks, 2, 0);
    lv_obj_clear_flag(tasks, LV_OBJ_FLAG_SCROLLABLE);

    build_task_row(tasks, "Inbox scanned",            "3 FOUND  •  234MS",   TH_STATUS_GREEN);
    build_task_row(tasks, "Drafting reply to Aisha",  "70%  •  1.2KB",       TH_AMBER);
    build_task_row(tasks, "News digest",              "QUEUED  •  ETA 60S",  0x2D2D35);

    /* Section 2 — EARLIER TODAY */
    lv_obj_t *sec2h = lv_label_create(s_overlay);
    lv_label_set_text(sec2h, "EARLIER TODAY");
    lv_obj_set_style_text_font(sec2h, FONT_CAPTION, 0);
    lv_obj_set_style_text_color(sec2h, lv_color_hex(TH_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_letter_space(sec2h, 4, 0);
    lv_obj_set_pos(sec2h, SIDE_PAD, 680);

    lv_obj_t *feed = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(feed);
    lv_obj_set_size(feed, SW - 2 * SIDE_PAD, LV_SIZE_CONTENT);
    lv_obj_set_pos(feed, SIDE_PAD, 720);
    lv_obj_set_flex_flow(feed, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(feed, LV_OBJ_FLAG_SCROLLABLE);

    build_feed_row(feed, "11:42", "NOTE",  TH_AMBER,  "TinkerClaw roadmap -- Phase 2 outline");
    build_feed_row(feed, "10:15", "CHAT",  0x9EB8FF,  "AirPods vs Sony XM5 -- Sonnet");

    /* Ask prompt at bottom */
    lv_obj_t *ask_cursor = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(ask_cursor);
    lv_obj_set_size(ask_cursor, 2, 28);
    lv_obj_set_pos(ask_cursor, SIDE_PAD, SH - 110);
    lv_obj_set_style_bg_color(ask_cursor, lv_color_hex(TH_AMBER), 0);
    lv_obj_set_style_bg_opa(ask_cursor, LV_OPA_COVER, 0);

    lv_obj_t *ask_lbl = lv_label_create(s_overlay);
    lv_label_set_text(ask_lbl, "say something...");
    lv_obj_set_style_text_font(ask_lbl, FONT_BODY, 0);
    lv_obj_set_style_text_color(ask_lbl, lv_color_hex(0x5A5A63), 0);
    lv_obj_set_pos(ask_lbl, SIDE_PAD + 14, SH - 114);

    s_visible = true;
    ESP_LOGI(TAG, "focus overlay shown");
}

void ui_focus_hide(void)
{
    if (!s_visible) return;
    if (s_overlay) { lv_obj_del(s_overlay); s_overlay = NULL; }
    s_back_btn = NULL;
    s_visible = false;
    ESP_LOGI(TAG, "focus overlay hidden");
}

bool ui_focus_is_visible(void)
{
    return s_visible;
}
