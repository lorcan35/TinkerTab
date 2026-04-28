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
#include "tool_log.h"
#include "esp_log.h"
#include <stdio.h>
#include <time.h>

static const char *TAG = "ui_focus";

#define SW        720
#define SH        1280
#define SIDE_PAD  52
#define ORB_CORNER_SZ  120
#define ORB_CORNER_X   (SW - ORB_CORNER_SZ - 56)
#define ORB_CORNER_Y   64

static lv_obj_t *s_overlay   = NULL;
static lv_obj_t *s_back_btn  = NULL;
static lv_obj_t *s_narrative = NULL;     /* U8 (#206): refreshed on each show */
static lv_obj_t *s_tasks     = NULL;
/* Phase-follow-up to U8: the EARLIER TODAY header + feed are now
 * rebuilt on each show from tool_log instead of hardcoded demo
 * strings.  Hide the entire section when tool_log has nothing past
 * the newest event. */
static lv_obj_t *s_history_header = NULL;
static lv_obj_t *s_history_feed = NULL;
static bool      s_visible   = false;

static void render_live_focus(void);

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
    /* Match ui_home_go_home / ui_home_nav_settings: if the currently
     * displayed lv_screen is camera / files (separate lv_screen
     * objects), overlays parented to home won't render.  Make sure
     * home is the active screen first. */
    lv_obj_t *home = ui_home_get_screen();
    if (home && lv_screen_active() != home) {
        lv_screen_load(home);
    }

    if (s_overlay) {
        lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_overlay);
        s_visible = true;
        render_live_focus();
        return;
    }

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
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_GESTURE_BUBBLE);

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
    /* Pre-fix said "HEARTBEAT  •  2 MIN LIVE" hardcoded — looked like
     * filler.  Keep just "HEARTBEAT" since the narrative line already
     * conveys whether anything is live. */
    lv_label_set_text(h, "HEARTBEAT");
    lv_obj_set_style_text_font(h, FONT_CAPTION, 0);
    lv_obj_set_style_text_color(h, lv_color_hex(TH_AMBER), 0);
    lv_obj_set_style_text_letter_space(h, 3, 0);

    s_narrative = lv_label_create(sec1);
    lv_label_set_long_mode(s_narrative, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_narrative, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_narrative, lv_color_hex(TH_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_line_space(s_narrative, 4, 0);
    /* Reserve the top-right corner for the orb (140 px) so the narrative
     * can't collide with it. Spec shot-01 right pane. */
    lv_obj_set_width(s_narrative, SW - 2 * SIDE_PAD - 160);

    /* Task stream below the narrative — populated by render_live_focus. */
    s_tasks = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(s_tasks);
    lv_obj_set_size(s_tasks, SW - 2 * SIDE_PAD, LV_SIZE_CONTENT);
    lv_obj_set_pos(s_tasks, SIDE_PAD, 480);
    lv_obj_set_flex_flow(s_tasks, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_tasks, 2, 0);
    lv_obj_clear_flag(s_tasks, LV_OBJ_FLAG_SCROLLABLE);

    /* Section 2 — EARLIER TODAY (live tool_log entries past the
     * newest, mirroring the agents overlay's history list).  Pre-fix
     * this section had two hardcoded demo strings ("AirPods vs Sony
     * XM5" / "TinkerClaw roadmap -- Phase 2 outline") that always
     * appeared regardless of what the user actually did.  Looked like
     * filler.  Now the rows come from the same tool_log ring the
     * Agents overlay uses; if there's no history we hide the section
     * entirely instead of fabricating activity. */
    s_history_header = lv_label_create(s_overlay);
    lv_label_set_text(s_history_header, "EARLIER TODAY");
    lv_obj_set_style_text_font(s_history_header, FONT_CAPTION, 0);
    lv_obj_set_style_text_color(s_history_header, lv_color_hex(TH_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_letter_space(s_history_header, 4, 0);
    lv_obj_set_pos(s_history_header, SIDE_PAD, 680);
    lv_obj_add_flag(s_history_header, LV_OBJ_FLAG_HIDDEN); /* show iff history exists */

    s_history_feed = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(s_history_feed);
    lv_obj_set_size(s_history_feed, SW - 2 * SIDE_PAD, LV_SIZE_CONTENT);
    lv_obj_set_pos(s_history_feed, SIDE_PAD, 720);
    lv_obj_set_flex_flow(s_history_feed, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(s_history_feed, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_history_feed, LV_OBJ_FLAG_HIDDEN); /* render_live_focus reveals iff have_history */

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
    render_live_focus();
    ESP_LOGI(TAG, "focus overlay shown");
}

/* U8 (#206): rebuild narrative + task rows from the live tool_log
 * snapshot.  Idempotent on re-show; preserves the hide/show pattern by
 * recycling s_narrative (text update) and s_tasks (clean + rebuild). */
static void render_live_focus(void)
{
    if (!s_overlay || !s_narrative || !s_tasks) return;

    int total = tool_log_count();
    int running = 0, done = 0;
    for (int i = 0; i < total; i++) {
        tool_log_event_t e;
        if (tool_log_get(i, &e)) {
            if (e.status == TOOL_LOG_RUNNING) running++;
            else                              done++;
        }
    }

    if (total == 0) {
        lv_label_set_text(s_narrative,
            "Nothing running. Talk to TinkerClaw and your live "
            "activity will land here.");
    } else {
        tool_log_event_t newest;
        tool_log_get(0, &newest);
        char narr[200];
        if (running > 0) {
            snprintf(narr, sizeof(narr),
                "Running %s now. %d completed in this session.",
                newest.detail[0] ? newest.detail : newest.name, done);
        } else {
            snprintf(narr, sizeof(narr),
                "Last action: %s. %d completed in this session.",
                newest.detail[0] ? newest.detail : newest.name, done);
        }
        lv_label_set_text(s_narrative, narr);
    }

    /* Wipe + repopulate the task rows. */
    lv_obj_clean(s_tasks);
    int n_show = total > 6 ? 6 : total;
    for (int i = 0; i < n_show; i++) {
        tool_log_event_t e;
        if (!tool_log_get(i, &e)) continue;
        char meta[48];
        if (e.status == TOOL_LOG_DONE) {
            snprintf(meta, sizeof(meta), "DONE  \xe2\x80\xa2  %u MS",
                     (unsigned)e.exec_ms);
        } else {
            snprintf(meta, sizeof(meta), "RUNNING");
        }
        uint32_t color = (e.status == TOOL_LOG_DONE) ? TH_STATUS_GREEN : TH_AMBER;
        build_task_row(s_tasks,
                       e.detail[0] ? e.detail : e.name,
                       meta, color);
    }

    /* EARLIER TODAY: tool_log entries past the first n_show, shown as
     * a compact "time / kind / detail" feed.  When tool_log has at
     * most n_show entries (i.e. nothing older to show), hide the
     * entire section instead of fabricating demo rows. */
    bool have_history = (total > n_show);
    if (s_history_header) {
       if (have_history)
          lv_obj_remove_flag(s_history_header, LV_OBJ_FLAG_HIDDEN);
       else
          lv_obj_add_flag(s_history_header, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_history_feed) {
       lv_obj_clean(s_history_feed);
       if (have_history) {
          lv_obj_remove_flag(s_history_feed, LV_OBJ_FLAG_HIDDEN);
          for (int i = n_show; i < total && i < n_show + 4; i++) {
             tool_log_event_t e;
             if (!tool_log_get(i, &e)) continue;
             struct tm tm_l;
             localtime_r(&e.started_at, &tm_l);
             char tbuf[8];
             snprintf(tbuf, sizeof(tbuf), "%02d:%02d", tm_l.tm_hour, tm_l.tm_min);
             build_feed_row(s_history_feed, tbuf, "TOOL", TH_AMBER, e.detail[0] ? e.detail : e.name);
          }
       } else {
          lv_obj_add_flag(s_history_feed, LV_OBJ_FLAG_HIDDEN);
       }
    }
}

void ui_focus_hide(void)
{
    if (!s_visible) return;
    /* Hide instead of destroy — destroy/recreate churn fragments the LV
     * pool and eventually lv_obj_allocate_spec_attr returns NULL mid-
     * create, crashing ui_task. */
    if (s_overlay) lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    s_visible = false;
    ESP_LOGI(TAG, "focus overlay hidden");
}

bool ui_focus_is_visible(void)
{
    return s_visible;
}
