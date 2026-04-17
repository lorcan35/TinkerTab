/*
 * ui_sessions.c — v5 Conversations browser (chat sessions list)
 *
 * Pattern cloned from ui_agents.c: fullscreen overlay on the home screen,
 * hand-curated demo data rendered as flat row list (no card surfaces,
 * hairline dividers).
 */

#include "ui_sessions.h"
#include "ui_theme.h"
#include "ui_home.h"
#include "ui_core.h"
#include "config.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "ui_sessions";

#define SW        720
#define SH        1280
#define SIDE_PAD  52

static lv_obj_t *s_overlay  = NULL;
static lv_obj_t *s_back_btn = NULL;
static bool      s_visible  = false;

typedef struct {
    const char *time_top;    /* "10:15" or "YEST" */
    const char *time_bot;    /* optional second line e.g. "19:04" */
    const char *subject;     /* amber, the user's question */
    const char *preview;     /* dim, last AI reply */
    int         msg_count;
    const char *mode_tag;    /* "CLAW" / "SONNET" / "LOCAL" / "HYBRID" */
    uint32_t    mode_color;  /* TH_MODE_* */
} session_row_t;

static const session_row_t s_sessions[] = {
    { "10:15", NULL,
      "AirPods vs XM5",
      "Both excellent, but the XM5 wins for calls -- its dual-beamforming mic reads vocals about 3dB cleaner in wind.",
      8, "SONNET", TH_MODE_CLOUD },
    { "YEST", "19:04",
      "Debug LVGL crash",
      "Circle cache overflow -- try CACHE_SIZE=32 in sdkconfig.defaults and a full rebuild.",
      14, "CLAW", TH_MODE_CLAW },
    { "YEST", "11:20",
      "Lunch ideas (no rice)",
      "How about a protein bowl with roasted chickpeas, tahini, pickled onions, and a soft egg on top?",
      3, "LOCAL", TH_MODE_LOCAL },
    { "APR 14", "16:02",
      "Meeting prep -- OKR review",
      "Here's what I'd bring up: Q2 carry-overs, the hiring freeze, and a proposal to split the ingest workstream.",
      22, "CLAW", TH_MODE_CLAW },
    { "APR 14", "08:11",
      "Morning digest",
      "3 items: Ollama 0.6, LocalAI hot-swap, and a WWDC rumor worth a second glance.",
      4, "HYBRID", TH_MODE_HYBRID },
};
#define N_SESSIONS (int)(sizeof(s_sessions)/sizeof(s_sessions[0]))

static void back_click_cb(lv_event_t *e)
{
    (void)e;
    ui_sessions_hide();
}

static void overlay_gesture_cb(lv_event_t *e)
{
    (void)e;
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if (dir == LV_DIR_RIGHT || dir == LV_DIR_BOTTOM) {
        ui_sessions_hide();
    }
}

static int build_session_row(lv_obj_t *parent, int y, const session_row_t *r)
{
    const int row_w = SW - 2 * SIDE_PAD;

    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, row_w, LV_SIZE_CONTENT);
    lv_obj_set_pos(c, SIDE_PAD, y);
    lv_obj_set_style_pad_top(c, 16, 0);
    lv_obj_set_style_pad_bottom(c, 18, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(0x1C1C28), 0);  /* TH_HAIRLINE */
    lv_obj_set_style_border_side(c, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);

    /* Left column: time (stack top+bot if present) */
    lv_obj_t *tt = lv_label_create(c);
    lv_label_set_text(tt, r->time_top);
    lv_obj_set_style_text_font(tt, FONT_CAPTION, 0);
    lv_obj_set_style_text_color(tt, lv_color_hex(TH_AMBER), 0);
    lv_obj_set_style_text_letter_space(tt, 2, 0);
    lv_obj_set_pos(tt, 0, 0);

    if (r->time_bot) {
        lv_obj_t *tb = lv_label_create(c);
        lv_label_set_text(tb, r->time_bot);
        lv_obj_set_style_text_font(tb, FONT_CAPTION, 0);
        lv_obj_set_style_text_color(tb, lv_color_hex(0x5C5C68), 0);
        lv_obj_set_style_text_letter_space(tb, 2, 0);
        lv_obj_set_pos(tb, 0, 22);
    }

    /* Subject (amber, FONT_HEADING) */
    lv_obj_t *sub = lv_label_create(c);
    lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
    lv_label_set_text(sub, r->subject);
    lv_obj_set_width(sub, row_w - 140 - 110);
    lv_obj_set_style_text_font(sub, FONT_SECONDARY, 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(TH_TEXT_BODY), 0);
    lv_obj_set_pos(sub, 120, 0);

    /* Preview (dim, 2-line wrap) */
    lv_obj_t *pv = lv_label_create(c);
    lv_label_set_long_mode(pv, LV_LABEL_LONG_DOT);
    lv_label_set_text(pv, r->preview);
    lv_obj_set_width(pv, row_w - 140 - 110);
    lv_obj_set_style_text_font(pv, FONT_SMALL, 0);
    lv_obj_set_style_text_color(pv, lv_color_hex(TH_TEXT_DIM), 0);
    lv_obj_set_style_text_line_space(pv, 3, 0);
    lv_obj_set_pos(pv, 120, 28);

    /* Right meta: msg count + mode tag */
    char msgbuf[16];
    snprintf(msgbuf, sizeof(msgbuf), "%d MSG", r->msg_count);
    lv_obj_t *mc = lv_label_create(c);
    lv_label_set_text(mc, msgbuf);
    lv_obj_set_style_text_font(mc, FONT_CAPTION, 0);
    lv_obj_set_style_text_color(mc, lv_color_hex(0x5C5C68), 0);
    lv_obj_set_style_text_letter_space(mc, 2, 0);
    lv_obj_align(mc, LV_ALIGN_TOP_RIGHT, 0, 0);

    lv_obj_t *mt = lv_label_create(c);
    lv_label_set_text(mt, r->mode_tag);
    lv_obj_set_style_text_font(mt, FONT_CAPTION, 0);
    lv_obj_set_style_text_color(mt, lv_color_hex(r->mode_color), 0);
    lv_obj_set_style_text_letter_space(mt, 3, 0);
    lv_obj_align(mt, LV_ALIGN_TOP_RIGHT, 0, 22);

    /* Return the content height so caller can advance y cleanly. */
    return 72;
}

void ui_sessions_show(void)
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

    /* Back hit — top-left (taps). Swipe-right also closes. */
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
    lv_label_set_text(head, "Conversations");
    lv_obj_set_style_text_font(head, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(head, lv_color_hex(TH_AMBER), 0);
    lv_obj_set_pos(head, SIDE_PAD, 110);

    char count_buf[40];
    snprintf(count_buf, sizeof(count_buf), "%d  \xe2\x80\xa2  %s",
             N_SESSIONS, "THIS WEEK");
    lv_obj_t *count = lv_label_create(s_overlay);
    lv_label_set_text(count, count_buf);
    lv_obj_set_style_text_font(count, FONT_CAPTION, 0);
    lv_obj_set_style_text_color(count, lv_color_hex(TH_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_letter_space(count, 3, 0);
    lv_obj_set_pos(count, SIDE_PAD, 190);

    int y = 250;
    for (int i = 0; i < N_SESSIONS; i++) {
        int h = build_session_row(s_overlay, y, &s_sessions[i]);
        y += h + 14;
    }

    s_visible = true;
    ESP_LOGI(TAG, "sessions overlay shown (%d rows)", N_SESSIONS);
}

void ui_sessions_hide(void)
{
    if (!s_visible) return;
    if (s_overlay) { lv_obj_del(s_overlay); s_overlay = NULL; }
    s_back_btn = NULL;
    s_visible = false;
    ESP_LOGI(TAG, "sessions overlay hidden");
}

bool ui_sessions_is_visible(void)
{
    return s_visible;
}
