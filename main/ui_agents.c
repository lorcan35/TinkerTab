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
#include "tool_log.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "ui_agents";

#define SW        720
#define SH        1280
#define SIDE_PAD  52

static lv_obj_t *s_overlay       = NULL;
static lv_obj_t *s_back_btn      = NULL;
static lv_obj_t *s_count_lbl     = NULL;   /* U7 (#206): refreshed on each show */
static lv_obj_t *s_entry_root    = NULL;   /* container for the live activity entry */
static bool      s_visible       = false;

static void render_live_content(void);

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
        /* U7 (#206): live content was built from a snapshot of
         * tool_log on first show — refresh on every re-show so new
         * tool activity appears. */
        render_live_content();
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

    render_live_content();
    s_visible = true;
    ESP_LOGI(TAG, "agents overlay shown");
}

/* U7+U8 (#206): rebuild the count label + entry container from the
 * current tool_log ring snapshot.  Idempotent — called on every show
 * (first or re-show).  Hide/show pattern is preserved (we don't
 * destroy s_overlay), so the LV pool isn't churned. */
static void render_live_content(void)
{
    if (!s_overlay) return;

    int total = tool_log_count();
    int running = 0, done = 0;
    for (int i = 0; i < total; i++) {
        tool_log_event_t e;
        if (tool_log_get(i, &e)) {
            if (e.status == TOOL_LOG_RUNNING) running++;
            else                              done++;
        }
    }

    /* Count label — create-once, update text in place on re-renders. */
    if (!s_count_lbl) {
        s_count_lbl = lv_label_create(s_overlay);
        lv_obj_set_style_text_font(s_count_lbl, FONT_CAPTION, 0);
        lv_obj_set_style_text_color(s_count_lbl,
            lv_color_hex(TH_TEXT_SECONDARY), 0);
        lv_obj_set_style_text_letter_space(s_count_lbl, 3, 0);
        lv_obj_set_pos(s_count_lbl, SIDE_PAD, 190);
    }
    char count_buf[40];
    snprintf(count_buf, sizeof(count_buf), "%d LIVE  \xe2\x80\xa2  %d DONE",
             running, done);
    lv_label_set_text(s_count_lbl, count_buf);

    /* Entry container — wipe + rebuild children on each render.  The
     * container itself persists across re-shows so the LV pool isn't
     * thrashed; only the cheap label/dot widgets are recycled. */
    if (!s_entry_root) {
        s_entry_root = lv_obj_create(s_overlay);
        lv_obj_remove_style_all(s_entry_root);
        lv_obj_set_size(s_entry_root, SW, LV_SIZE_CONTENT);
        lv_obj_set_pos(s_entry_root, 0, 250);
        lv_obj_clear_flag(s_entry_root, LV_OBJ_FLAG_SCROLLABLE);
    } else {
        lv_obj_clean(s_entry_root);
    }

    if (total == 0) {
        lv_obj_t *empty = lv_label_create(s_entry_root);
        lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
        lv_label_set_text(empty,
            "No tool activity yet.\n\n"
            "Talk to TinkerClaw -- when it searches the web, recalls a "
            "memory, or runs any other tool, it'll show up here.");
        lv_obj_set_width(empty, SW - 2 * SIDE_PAD);
        lv_obj_set_style_text_font(empty, FONT_BODY, 0);
        lv_obj_set_style_text_color(empty, lv_color_hex(TH_TEXT_DIM), 0);
        lv_obj_set_style_text_line_space(empty, 4, 0);
        lv_obj_set_pos(empty, SIDE_PAD, 0);
        return;
    }

    int n_show = total > 6 ? 6 : total;
    const char *task_lines[6] = {0};
    static char  task_storage[6][96];

    tool_log_event_t newest;
    tool_log_get(0, &newest);

    char narrative[160];
    if (running > 0) {
        snprintf(narrative, sizeof(narrative),
            "Running %s now. %d completed in this session.",
            newest.detail[0] ? newest.detail : newest.name, done);
    } else {
        snprintf(narrative, sizeof(narrative),
            "Last action: %s. %d completed in this session.",
            newest.detail[0] ? newest.detail : newest.name, done);
    }

    for (int i = 0; i < n_show; i++) {
        tool_log_event_t e;
        if (!tool_log_get(i, &e)) continue;
        if (e.status == TOOL_LOG_DONE) {
            snprintf(task_storage[i], sizeof(task_storage[0]),
                     "%s  \xe2\x80\xa2  done  \xe2\x80\xa2  %u ms",
                     e.name, (unsigned)e.exec_ms);
        } else {
            snprintf(task_storage[i], sizeof(task_storage[0]),
                     "%s  \xe2\x80\xa2  running",
                     e.name);
        }
        task_lines[i] = task_storage[i];
    }

    char ts_label[24] = "JUST NOW";
    time_t now = time(NULL);
    long secs = (long)(now - newest.started_at);
    if      (secs < 60)    snprintf(ts_label, sizeof(ts_label), "JUST NOW");
    else if (secs < 3600)  snprintf(ts_label, sizeof(ts_label), "%ld MIN AGO", secs / 60);
    else if (secs < 86400) snprintf(ts_label, sizeof(ts_label), "%ld H AGO",   secs / 3600);
    else                   snprintf(ts_label, sizeof(ts_label), "%ld D AGO",   secs / 86400);

    build_agent_entry(s_entry_root, 0,
                      "AGENT ACTIVITY", ts_label,
                      running > 0 ? TH_MODE_CLAW : TH_STATUS_GREEN,
                      narrative, task_lines, n_show);
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
