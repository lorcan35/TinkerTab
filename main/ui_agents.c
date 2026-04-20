/*
 * ui_agents.c — v5 Agents overlay (minimal)
 *
 * Header + two agent entries rendered as typographic cards, separated by
 * hairlines. No live data yet — the entries are static so the surface is
 * navigable. Back-button swipe + tap both dismiss.
 */

#include "ui_agents.h"
#include "ui_theme.h"
#include "ui_home.h"
#include "ui_core.h"
#include "config.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "ui_agents";

#define SW        720
#define SH        1280
#define SIDE_PAD  52

static lv_obj_t *s_overlay       = NULL;
static lv_obj_t *s_back_btn      = NULL;
static bool      s_visible       = false;

/* ── Helpers ─────────────────────────────────────────────────── */

static void back_click_cb(lv_event_t *e)
{
    (void)e;
    ui_agents_hide();
}

static void overlay_gesture_cb(lv_event_t *e)
{
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if (dir == LV_DIR_RIGHT || dir == LV_DIR_BOTTOM) {
        ui_agents_hide();
    }
}

/* One agent "entry" — tight row block: label line, narrative, task list. */
static void build_agent_entry(lv_obj_t *parent, int y,
                              const char *label, const char *ts,
                              uint32_t dot_color, const char *narrative,
                              const char *tasks[], const int n_tasks)
{
    /* Container */
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, SW - 2 * SIDE_PAD, LV_SIZE_CONTENT);
    lv_obj_set_pos(c, SIDE_PAD, y);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 10, 0);
    lv_obj_set_style_pad_bottom(c, 24, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(0x1A1A24), 0);  /* hairline */
    lv_obj_set_style_border_side(c, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);

    /* Head row: colored dot + label + timestamp */
    lv_obj_t *head = lv_obj_create(c);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, 10, 0);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *dot = lv_obj_create(head);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 8, 8);
    lv_obj_set_style_radius(dot, 4, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(dot_color), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);

    lv_obj_t *name = lv_label_create(head);
    lv_label_set_text(name, label);
    lv_obj_set_style_text_font(name, FONT_CAPTION, 0);
    lv_obj_set_style_text_color(name, lv_color_hex(TH_AMBER), 0);
    lv_obj_set_style_text_letter_space(name, 3, 0);

    lv_obj_t *sp = lv_obj_create(head);
    lv_obj_remove_style_all(sp);
    lv_obj_set_flex_grow(sp, 1);
    lv_obj_set_height(sp, 1);

    lv_obj_t *time = lv_label_create(head);
    lv_label_set_text(time, ts);
    lv_obj_set_style_text_font(time, FONT_CAPTION, 0);
    lv_obj_set_style_text_color(time, lv_color_hex(0x55555D), 0);
    lv_obj_set_style_text_letter_space(time, 2, 0);

    /* Narrative — the headline line */
    lv_obj_t *line = lv_label_create(c);
    lv_label_set_long_mode(line, LV_LABEL_LONG_WRAP);
    lv_label_set_text(line, narrative);
    lv_obj_set_width(line, SW - 2 * SIDE_PAD);
    lv_obj_set_style_text_font(line, FONT_SECONDARY, 0);
    lv_obj_set_style_text_color(line, lv_color_hex(TH_TEXT_BODY), 0);
    lv_obj_set_style_text_line_space(line, 4, 0);

    /* Task stream — amber bullets for done, hairline for queued */
    for (int i = 0; i < n_tasks; i++) {
        lv_obj_t *row = lv_obj_create(c);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 14, 0);
        lv_obj_set_style_pad_left(row, 4, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *tdot = lv_obj_create(row);
        lv_obj_remove_style_all(tdot);
        lv_obj_set_size(tdot, 6, 6);
        lv_obj_set_style_radius(tdot, 3, 0);
        lv_obj_set_style_bg_color(tdot, lv_color_hex(i == 0 ? TH_STATUS_GREEN
                                                   : i == 1 ? TH_AMBER
                                                   : 0x2D2D35), 0);
        lv_obj_set_style_bg_opa(tdot, LV_OPA_COVER, 0);

        lv_obj_t *tlbl = lv_label_create(row);
        lv_label_set_text(tlbl, tasks[i]);
        lv_obj_set_style_text_font(tlbl, FONT_SMALL, 0);
        lv_obj_set_style_text_color(tlbl, lv_color_hex(TH_TEXT_BODY), 0);
    }
}

/* ── Build + show ────────────────────────────────────────────── */

void ui_agents_show(void)
{
    /* Match the go-home fix: if a secondary lv_screen (camera/files) is
     * currently active, load home first so our overlay renders. */
    lv_obj_t *home = ui_home_get_screen();
    if (home && lv_screen_active() != home) {
        lv_screen_load(home);
    }

    /* Re-use hidden overlay (hide/show pattern). */
    if (s_overlay) {
        lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_overlay);
        s_visible = true;
        return;
    }

    /* Fullscreen overlay — parent is home screen, same pattern as chat/settings */
    lv_obj_t *parent = home;
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
    /* Stop gesture bubble so home's screen_gesture_cb doesn't reopen an
     * overlay we just closed via swipe. */
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_GESTURE_BUBBLE);

    /* Back hit — top-left (taps) */
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

    /* Header — "Agents" + count */
    lv_obj_t *head = lv_label_create(s_overlay);
    lv_label_set_text(head, "Agents");
    lv_obj_set_style_text_font(head, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(head, lv_color_hex(TH_AMBER), 0);
    lv_obj_set_pos(head, SIDE_PAD, 110);

    lv_obj_t *count = lv_label_create(s_overlay);
    lv_label_set_text(count, "1 LIVE  •  1 DONE");
    lv_obj_set_style_text_font(count, FONT_CAPTION, 0);
    lv_obj_set_style_text_color(count, lv_color_hex(TH_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_letter_space(count, 3, 0);
    lv_obj_set_pos(count, SIDE_PAD, 190);

    /* Two hand-curated entries so the surface is demonstrable. */
    const char *heartbeat_tasks[] = {
        "Inbox scanned  •  3 found  •  234 ms",
        "Drafting reply to Aisha  •  70%  •  1.2 KB",
        "News digest  •  queued  •  ETA 60 s",
    };
    build_agent_entry(s_overlay, 250,
                      "HEARTBEAT",
                      "2 MIN LIVE",
                      TH_MODE_CLAW,
                      "I scanned your inbox -- three replies were waiting. "
                      "Drafting one to Aisha now; the digest is queued.",
                      heartbeat_tasks, 3);

    const char *browser_tasks[] = {
        "Fetched Hacker News  •  240 ms",
        "Summarised 4 articles  •  2.1 s",
    };
    build_agent_entry(s_overlay, 560,
                      "BROWSER",
                      "YESTERDAY",
                      TH_STATUS_GREEN,
                      "Pulled Hacker News and saved four articles for you.",
                      browser_tasks, 2);

    s_visible = true;
    ESP_LOGI(TAG, "agents overlay shown");
}

void ui_agents_hide(void)
{
    if (!s_visible) return;
    /* Hide instead of destroy (see ui_focus_hide). */
    if (s_overlay) lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    s_visible = false;
    ESP_LOGI(TAG, "agents overlay hidden");
}

bool ui_agents_is_visible(void)
{
    return s_visible;
}
