/*
 * ui_memory.c — v5 Memory search overlay.
 *
 * Typography-forward surface showing the live contents of Dragon's
 * fact store (POST /api/v1/memory).  On show, spawns a FreeRTOS task
 * that GETs /api/v1/memory?limit=6, parses cJSON, and re-builds the
 * hit rows on the LVGL thread via lv_async_call.
 *
 * v4·D 2026-04-20: wired to Dragon REST -- previously rendered four
 * hardcoded mock rows.
 */

#include "ui_memory.h"
#include "ui_keyboard.h"
#include "ui_theme.h"
#include "ui_home.h"
#include "ui_core.h"
#include "config.h"
#include "settings.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "ui_memory";

#define SW        720
#define SH        1280
#define SIDE_PAD  52
#define MAX_HITS  6

static lv_obj_t *s_overlay    = NULL;
static lv_obj_t *s_back_btn   = NULL;
static lv_obj_t *s_stats_lbl  = NULL;
static lv_obj_t *s_hits_root  = NULL;   /* container that holds all hit rows */
static lv_obj_t *s_loading    = NULL;   /* "Loading..." label while fetching */
/* U6 (#206): live query plumbing.  s_pill_ta holds the typed query;
 * s_pill_ghost is the placeholder shown only when the query is empty;
 * s_query is the source-of-truth for the next /api/v1/memory call
 * (read by fetch_task).  Empty s_query → list-all (the v4·D default). */
static lv_obj_t *s_pill_ta    = NULL;
static lv_obj_t *s_pill_ghost = NULL;
static char      s_query[128] = "";
static bool      s_visible    = false;
static volatile bool s_fetch_inflight = false;

typedef struct {
    char id[24];
    char content[192];
    char source[16];
    double created_at;   /* unix seconds */
} mem_hit_t;

typedef struct {
    mem_hit_t items[MAX_HITS];
    int       count;
    int       total;
    bool      ok;
} mem_fetch_result_t;

/* Shared buffer the fetch task hands off to the LVGL async callback. */
static mem_fetch_result_t s_last_fetch;

/* U6 (#206) — forward decls for the query-pill callbacks (defined below). */
static void pill_clicked_cb(lv_event_t *e);
static void pill_ta_ready_cb(lv_event_t *e);
static void pill_ta_changed_cb(lv_event_t *e);

/* ── UI helpers ─────────────────────────────────────────────────── */

static void back_click_cb(lv_event_t *e)
{
    (void)e;
    ui_memory_hide();
}

static void overlay_gesture_cb(lv_event_t *e)
{
    (void)e;
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if (dir == LV_DIR_RIGHT || dir == LV_DIR_BOTTOM) {
        ui_memory_hide();
    }
}

static void format_when(char *out, size_t n, double created_at)
{
    time_t now   = time(NULL);
    time_t t     = (time_t)created_at;
    time_t delta = now - t;
    if (delta < 0) delta = 0;
    if (delta < 3600) {
        int m = (int)(delta / 60);
        snprintf(out, n, "%d MIN AGO", m);
        return;
    }
    if (delta < 86400) {
        int h = (int)(delta / 3600);
        snprintf(out, n, "%d H AGO", h);
        return;
    }
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(out, n, "%b %d  \xe2\x80\xa2  %H:%M", &tm);
}

static void build_hit(lv_obj_t *parent,
                      const char *tag, uint32_t tag_color,
                      const char *when, const char *excerpt)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, SW - 2 * SIDE_PAD, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 6, 0);
    lv_obj_set_style_pad_bottom(c, 18, 0);
    lv_obj_set_style_pad_top(c, 12, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(0x1A1A24), 0);
    lv_obj_set_style_border_side(c, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);

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

    lv_obj_t *ex = lv_label_create(c);
    lv_label_set_long_mode(ex, LV_LABEL_LONG_WRAP);
    lv_label_set_text(ex, excerpt);
    lv_obj_set_width(ex, SW - 2 * SIDE_PAD);
    lv_obj_set_style_text_font(ex, FONT_SMALL, 0);
    lv_obj_set_style_text_color(ex, lv_color_hex(TH_TEXT_BODY), 0);
    lv_obj_set_style_text_line_space(ex, 4, 0);
}

/* ── Render callback (LVGL thread) ──────────────────────────────── */

static void render_hits_cb(void *arg)
{
    (void)arg;
    /* #171: the fetch_task on Core 1 queues this callback via
     * lv_async_call after the HTTP response arrives.  Between queuing
     * and execution the user may have hidden the overlay, so there's
     * no visual reason to rebuild the list — and a future refactor
     * that migrates this overlay to a destroy/create lifecycle would
     * see s_hits_root / s_stats_lbl dangling.  Cheap guard: just bail
     * if we're not currently visible. */
    if (!s_visible || !s_hits_root) return;
    lv_obj_clean(s_hits_root);

    if (s_loading) {
        lv_obj_add_flag(s_loading, LV_OBJ_FLAG_HIDDEN);
    }

    if (!s_last_fetch.ok) {
        lv_obj_t *err = lv_label_create(s_hits_root);
        lv_label_set_long_mode(err, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(err, SW - 2 * SIDE_PAD);
        lv_label_set_text(err, "Dragon unreachable. Check Settings \xe2\x80\xa2 Network.");
        lv_obj_set_style_text_font(err, FONT_BODY, 0);
        lv_obj_set_style_text_color(err, lv_color_hex(0xEF4444), 0);
        return;
    }

    if (s_stats_lbl) {
        char sbuf[64];
        snprintf(sbuf, sizeof(sbuf),
                 "%d FACTS  \xe2\x80\xa2  QWEN3-EMBEDDING", s_last_fetch.total);
        lv_label_set_text(s_stats_lbl, sbuf);
    }

    if (s_last_fetch.count == 0) {
        lv_obj_t *empty = lv_label_create(s_hits_root);
        lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(empty, SW - 2 * SIDE_PAD);
        lv_label_set_text(empty,
            "Nothing yet. Say \"remember that...\" to start the memory.");
        lv_obj_set_style_text_font(empty, FONT_BODY, 0);
        lv_obj_set_style_text_color(empty, lv_color_hex(TH_TEXT_DIM), 0);
        return;
    }

    for (int i = 0; i < s_last_fetch.count; i++) {
        char when[32];
        char tag[32];
        const mem_hit_t *h = &s_last_fetch.items[i];
        format_when(when, sizeof(when), h->created_at);
        /* Uppercase the source as a kicker. */
        snprintf(tag, sizeof(tag), "%.15s", h->source[0] ? h->source : "memory");
        for (int k = 0; tag[k]; k++) {
            if (tag[k] >= 'a' && tag[k] <= 'z') tag[k] -= 32;
        }
        build_hit(s_hits_root, tag, TH_AMBER, when, h->content);
    }
}

/* ── Fetch task (background FreeRTOS) ───────────────────────────── */

/* U6 (#206): minimal query escape — just enough so a typed query like
 * "tea time" or "alex's birthday" survives the URL.  Allowed alnum,
 * dash, underscore, dot; everything else gets %HH-encoded.  Output
 * truncated to fit dst (always NUL-terminated). */
static void url_encode_q(char *dst, size_t dstn, const char *src)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t w = 0;
    if (dstn == 0) return;
    while (src && *src && w + 4 < dstn) {
        unsigned char c = (unsigned char)*src++;
        bool safe = (c >= '0' && c <= '9') ||
                    (c >= 'A' && c <= 'Z') ||
                    (c >= 'a' && c <= 'z') ||
                    c == '-' || c == '_' || c == '.';
        if (safe) {
            dst[w++] = (char)c;
        } else {
            dst[w++] = '%';
            dst[w++] = hex[(c >> 4) & 0xF];
            dst[w++] = hex[c & 0xF];
        }
    }
    dst[w] = 0;
}

static void fetch_task(void *pv)
{
    (void)pv;
    char host[64] = {0};
    tab5_settings_get_dragon_host(host, sizeof(host));
    if (!host[0]) snprintf(host, sizeof(host), "192.168.1.91");
    uint16_t port = tab5_settings_get_dragon_port();
    if (port == 0) port = 3502;

    char url[256];
    /* Snapshot the query so a re-tap during this fetch doesn't change
     * the in-flight URL halfway through. */
    char q_snapshot[128];
    snprintf(q_snapshot, sizeof(q_snapshot), "%s", s_query);
    if (q_snapshot[0]) {
        char qenc[160];
        url_encode_q(qenc, sizeof(qenc), q_snapshot);
        snprintf(url, sizeof(url),
                 "http://%s:%u/api/v1/memory?q=%s&limit=%d",
                 host, port, qenc, MAX_HITS);
    } else {
        snprintf(url, sizeof(url),
                 "http://%s:%u/api/v1/memory?limit=%d", host, port, MAX_HITS);
    }

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 4000,
        .buffer_size = 4096,
        .buffer_size_tx = 512,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    s_last_fetch.count = 0;
    s_last_fetch.total = 0;
    s_last_fetch.ok    = false;
    if (!cli) goto done;

    esp_err_t err = esp_http_client_open(cli, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "http_client_open failed: %s", esp_err_to_name(err));
        goto cleanup;
    }
    int content_len = esp_http_client_fetch_headers(cli);
    if (content_len < 0) content_len = 8192;
    char *body = malloc(content_len + 1);
    if (!body) goto cleanup;
    int got = 0;
    while (got < content_len) {
        int r = esp_http_client_read(cli, body + got, content_len - got);
        if (r <= 0) break;
        got += r;
    }
    body[got] = '\0';

    int status = esp_http_client_get_status_code(cli);
    ESP_LOGI(TAG, "GET %s -> %d (%d bytes)", url, status, got);
    if (status != 200) {
        free(body);
        goto cleanup;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        ESP_LOGW(TAG, "cJSON parse failed");
        goto cleanup;
    }
    cJSON *items = cJSON_GetObjectItem(root, "items");
    cJSON *total = cJSON_GetObjectItem(root, "count");
    if (cJSON_IsNumber(total)) s_last_fetch.total = total->valueint;
    if (cJSON_IsArray(items)) {
        int arr_n = cJSON_GetArraySize(items);
        int n = arr_n > MAX_HITS ? MAX_HITS : arr_n;
        for (int i = 0; i < n; i++) {
            cJSON *it = cJSON_GetArrayItem(items, i);
            if (!cJSON_IsObject(it)) continue;
            mem_hit_t *h = &s_last_fetch.items[s_last_fetch.count];
            const char *id  = cJSON_GetStringValue(cJSON_GetObjectItem(it, "id"));
            const char *ct  = cJSON_GetStringValue(cJSON_GetObjectItem(it, "content"));
            const char *sr  = cJSON_GetStringValue(cJSON_GetObjectItem(it, "source"));
            cJSON *cat      = cJSON_GetObjectItem(it, "created_at");
            if (id) strncpy(h->id,      id, sizeof(h->id)      - 1);
            if (ct) strncpy(h->content, ct, sizeof(h->content) - 1);
            if (sr) strncpy(h->source,  sr, sizeof(h->source)  - 1);
            h->created_at = cJSON_IsNumber(cat) ? cat->valuedouble : 0.0;
            s_last_fetch.count++;
        }
    }
    /* If the server reports 0 total but items is empty that's still OK -- the
     * "Nothing yet" empty-state will render. */
    if (s_last_fetch.total == 0 && s_last_fetch.count > 0) {
        s_last_fetch.total = s_last_fetch.count;
    }
    s_last_fetch.ok = true;
    cJSON_Delete(root);

cleanup:
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
done:
    /* Hop to LVGL thread to rebuild the UI. */
    lv_async_call(render_hits_cb, NULL);
    s_fetch_inflight = false;
    vTaskSuspend(NULL)  /* wave 13 C4: P4 TLSP crash on delete — suspend instead */;
}

static void kick_fetch(void)
{
    if (s_fetch_inflight) return;
    s_fetch_inflight = true;
    /* Allocate the task's TCB + stack from PSRAM (via WithCaps) so that
     * a fragmented internal-SRAM heap doesn't block the memory overlay
     * from ever fetching facts.  Without this, /navigate?screen=memory
     * after several overlay cycles would log "failed to spawn fetch
     * task" and the UI would sit stuck on "Loading facts..." forever.
     * Fallback to regular xTaskCreatePinnedToCore if WithCaps fails
     * (belt + braces). */
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(
        fetch_task, "mem_fetch", 6144, NULL, 3, NULL, 1,
        MALLOC_CAP_SPIRAM);
    if (ok != pdPASS) {
        ok = xTaskCreatePinnedToCore(
            fetch_task, "mem_fetch", 6144, NULL, 3, NULL, 1);
    }
    if (ok != pdPASS) {
        ESP_LOGW(TAG, "failed to spawn fetch task");
        s_fetch_inflight = false;
    }
}

/* ── U6 (#206): query pill click + textarea callbacks ──────────── */

static void pill_clicked_cb(lv_event_t *e)
{
    (void)e;
    if (s_pill_ta) ui_keyboard_show(s_pill_ta);
}

static void pill_ta_changed_cb(lv_event_t *e)
{
    (void)e;
    if (!s_pill_ta || !s_pill_ghost) return;
    const char *txt = lv_textarea_get_text(s_pill_ta);
    if (txt && txt[0]) lv_obj_add_flag(s_pill_ghost, LV_OBJ_FLAG_HIDDEN);
    else               lv_obj_remove_flag(s_pill_ghost, LV_OBJ_FLAG_HIDDEN);
}

static void pill_ta_ready_cb(lv_event_t *e)
{
    (void)e;
    if (!s_pill_ta) return;
    const char *txt = lv_textarea_get_text(s_pill_ta);
    snprintf(s_query, sizeof(s_query), "%s", txt ? txt : "");
    /* Hide keyboard on submit so the user sees the hits land. */
    ui_keyboard_hide();
    if (s_loading) {
        lv_label_set_text(s_loading, s_query[0] ? "Searching..." : "Loading facts...");
        lv_obj_remove_flag(s_loading, LV_OBJ_FLAG_HIDDEN);
    }
    kick_fetch();
}

/* ── Public API ──────────────────────────────────────────────────── */

void ui_memory_show(void)
{
    lv_obj_t *home = ui_home_get_screen();
    if (home && lv_screen_active() != home) {
        lv_screen_load(home);
    }

    if (s_overlay) {
        lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_overlay);
        s_visible = true;
        /* Kick a fresh fetch so the surface is always current. */
        kick_fetch();
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

    /* Back affordance */
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

    s_stats_lbl = lv_label_create(s_overlay);
    lv_label_set_text(s_stats_lbl, "LOADING \xe2\x80\xa2 QWEN3-EMBEDDING");
    lv_obj_set_style_text_font(s_stats_lbl, FONT_CAPTION, 0);
    lv_obj_set_style_text_color(s_stats_lbl, lv_color_hex(TH_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_letter_space(s_stats_lbl, 3, 0);
    lv_obj_set_pos(s_stats_lbl, SIDE_PAD, 190);

    /* Query pill — U6 (#206): real textarea wired to ui_keyboard.
     *   tap pill → ui_keyboard_show(textarea) → user types
     *   Done/Enter → LV_EVENT_READY → snapshot text into s_query +
     *   kick a fresh /api/v1/memory?q=... fetch.
     * Empty textarea shows the ghost "find anything..." placeholder. */
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
    lv_obj_add_flag(pill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(pill, pill_clicked_cb, LV_EVENT_CLICKED, NULL);

    s_pill_ta = lv_textarea_create(pill);
    lv_obj_remove_style_all(s_pill_ta);
    lv_obj_set_size(s_pill_ta, SW - 2 * SIDE_PAD - 36, 48);
    lv_obj_set_pos(s_pill_ta, 18, 12);
    lv_textarea_set_one_line(s_pill_ta, true);
    lv_textarea_set_text(s_pill_ta, s_query);
    lv_obj_set_style_bg_opa(s_pill_ta, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_pill_ta, 0, 0);
    lv_obj_set_style_text_font(s_pill_ta, FONT_BODY, 0);
    lv_obj_set_style_text_color(s_pill_ta, lv_color_hex(TH_TEXT_PRIMARY), 0);
    lv_obj_set_style_pad_all(s_pill_ta, 0, 0);
    lv_obj_add_event_cb(s_pill_ta, pill_ta_ready_cb,   LV_EVENT_READY,         NULL);
    lv_obj_add_event_cb(s_pill_ta, pill_ta_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_pill_ghost = lv_label_create(pill);
    lv_label_set_text(s_pill_ghost, "find anything you said to me...");
    lv_obj_set_style_text_font(s_pill_ghost, FONT_BODY, 0);
    lv_obj_set_style_text_color(s_pill_ghost, lv_color_hex(TH_TEXT_DIM), 0);
    lv_obj_set_pos(s_pill_ghost, 18, 26);
    if (s_query[0])
        lv_obj_add_flag(s_pill_ghost, LV_OBJ_FLAG_HIDDEN);

    /* Hits container -- filled on async */
    s_hits_root = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(s_hits_root);
    lv_obj_set_size(s_hits_root, SW, LV_SIZE_CONTENT);
    lv_obj_set_pos(s_hits_root, 0, 330);
    lv_obj_set_flex_flow(s_hits_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_hits_root, 0, 0);
    lv_obj_clear_flag(s_hits_root, LV_OBJ_FLAG_SCROLLABLE);

    /* Transient loading state */
    s_loading = lv_label_create(s_hits_root);
    lv_label_set_text(s_loading, "Loading facts...");
    lv_obj_set_style_text_font(s_loading, FONT_BODY, 0);
    lv_obj_set_style_text_color(s_loading, lv_color_hex(TH_TEXT_DIM), 0);

    s_visible = true;
    kick_fetch();
    ESP_LOGI(TAG, "memory overlay shown (fetching from Dragon)");
}

void ui_memory_hide(void)
{
    if (!s_visible) return;
    /* U6 (#206): if the keyboard is up for our pill, dismiss it so it
     * doesn't linger over Home or whatever the next screen is. */
    if (ui_keyboard_is_visible()) ui_keyboard_hide();
    if (s_overlay) lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    s_visible = false;
    ESP_LOGI(TAG, "memory overlay hidden");
}

bool ui_memory_is_visible(void)
{
    return s_visible;
}
