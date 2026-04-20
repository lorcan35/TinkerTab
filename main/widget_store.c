/**
 * widget_store.c — bounded in-PSRAM widget cache with priority queue semantics.
 *
 * Design constraints (see LEARNINGS entry "Widget Platform"):
 *   - Bounded at WIDGET_STORE_MAX; no runtime malloc/free per widget.
 *   - All storage in MALLOC_CAP_SPIRAM. Internal SRAM untouched.
 *   - LIVE priority = user-supplied priority × age boost (recently-updated wins ties).
 *   - GC evicts inactive records once older than GC_IDLE_MS to avoid unbounded growth.
 */

#include "widget.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "widget_store";

#define WIDGET_STORE_MAX 32
#define GC_IDLE_MS       (5 * 60 * 1000)    /* evict inactive widgets 5 min old */

/* PSRAM-backed store. One static pointer; allocated once at init. */
static widget_t *s_widgets = NULL;
static int       s_capacity = 0;

/* ── Helpers ─────────────────────────────────────────────────── */

static uint32_t now_ms(void)
{
    /* lv_tick_get is monotonic ms since boot; matches existing call sites
     * in ui_home orb-debounce. Avoids pulling esp_timer here. */
    extern uint32_t lv_tick_get(void);
    return lv_tick_get();
}

static void slot_clear(widget_t *w)
{
    memset(w, 0, sizeof(*w));
    w->active = false;
    w->type = WIDGET_TYPE_NONE;
    w->tone = WIDGET_TONE_CALM;
}

/* v4·D audit P1: priority-weighted eviction.
 *
 * The previous policy picked the "oldest" (smallest updated_at_ms)
 * active widget regardless of priority -- a running Pomodoro at
 * priority 80 would lose its slot to a stale info card at priority
 * 20 just because it had updated earlier.  Score each active slot
 * with (age_ms / (priority + 1)) so high-priority or recently-touched
 * slots resist eviction; lowest-score wins. */
static widget_t *slot_for_new(void)
{
    widget_t *best_inactive = NULL;
    widget_t *best_active   = NULL;
    uint32_t best_active_score = 0;
    uint32_t now = lv_tick_get();

    for (int i = 0; i < s_capacity; i++) {
        widget_t *w = &s_widgets[i];
        if (!w->active) {
            if (!best_inactive
                || w->updated_at_ms < best_inactive->updated_at_ms) {
                best_inactive = w;
            }
            continue;
        }
        /* Score: older + lower-priority ⇒ higher score ⇒ more evictable. */
        uint32_t age_ms = (now >= w->updated_at_ms)
                         ? (now - w->updated_at_ms) : 0;
        uint32_t pri    = w->priority ? w->priority : 50;
        uint32_t score  = age_ms / (pri + 1);
        if (!best_active || score > best_active_score) {
            best_active = w;
            best_active_score = score;
        }
    }

    if (best_inactive) return best_inactive;
    ESP_LOGW(TAG,
             "store full -- evicting id=%s (priority=%u, score=%lu)",
             best_active ? best_active->card_id : "?",
             best_active ? best_active->priority : 0,
             (unsigned long)best_active_score);
    return best_active;
}

/* ── Public API ─────────────────────────────────────────────── */

void widget_store_init(void)
{
    if (s_widgets) return;

    size_t bytes = sizeof(widget_t) * WIDGET_STORE_MAX;
    s_widgets = heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_widgets) {
        ESP_LOGE(TAG, "PSRAM alloc failed (%u bytes) — widget store disabled", (unsigned)bytes);
        s_capacity = 0;
        return;
    }
    s_capacity = WIDGET_STORE_MAX;
    for (int i = 0; i < s_capacity; i++) slot_clear(&s_widgets[i]);
    ESP_LOGI(TAG, "widget store initialized (cap=%d, bytes=%u)",
             s_capacity, (unsigned)bytes);
}

widget_t *widget_store_find(const char *card_id)
{
    if (!s_widgets || !card_id || !card_id[0]) return NULL;
    for (int i = 0; i < s_capacity; i++) {
        widget_t *w = &s_widgets[i];
        if (w->active && strncmp(w->card_id, card_id, WIDGET_ID_LEN) == 0) {
            return w;
        }
    }
    return NULL;
}

widget_t *widget_store_upsert(const widget_t *in)
{
    if (!s_widgets || !in || !in->card_id[0]) return NULL;

    widget_t *slot = widget_store_find(in->card_id);
    if (!slot) {
        slot = slot_for_new();
        if (!slot) return NULL;
        slot_clear(slot);
        slot->created_at_ms = now_ms();
    }

    /* Full overwrite — upsert treats the input as the new ground truth. */
    memcpy(slot->card_id,     in->card_id,     WIDGET_ID_LEN);
    memcpy(slot->skill_id,    in->skill_id,    WIDGET_SKILL_ID_LEN);
    memcpy(slot->title,       in->title,       WIDGET_TITLE_LEN);
    memcpy(slot->body,        in->body,        WIDGET_BODY_LEN);
    memcpy(slot->icon,        in->icon,        WIDGET_ICON_LEN);
    memcpy(slot->action_label, in->action_label, WIDGET_ACTION_LBL_LEN);
    memcpy(slot->action_event, in->action_event, WIDGET_ACTION_EVT_LEN);
    slot->type          = in->type ? in->type : WIDGET_TYPE_LIVE;
    slot->tone          = in->tone;
    slot->progress      = in->progress;
    slot->priority      = in->priority ? in->priority : 50;
    slot->expires_at_ms = in->expires_at_ms;
    slot->updated_at_ms = now_ms();
    slot->active        = true;

    /* v4·D Phase 4c: copy widget_list item payload.  memcpy the whole
     * items array (unused rows stay zero) + items_count so the render
     * path can iterate safely.  Non-list widgets carry items_count=0. */
    memcpy(slot->items, in->items, sizeof(slot->items));
    slot->items_count = in->items_count;

    /* v4·D Phase 4f: copy widget_chart payload.  Same zero-safe
     * semantics -- non-chart widgets carry chart_count=0. */
    memcpy(slot->chart_values, in->chart_values, sizeof(slot->chart_values));
    slot->chart_max   = in->chart_max;
    slot->chart_count = in->chart_count;

    /* v4·D Phase 4g: copy widget_media + widget_prompt payloads. */
    memcpy(slot->media_url, in->media_url, sizeof(slot->media_url));
    memcpy(slot->media_alt, in->media_alt, sizeof(slot->media_alt));
    memcpy(slot->choices,       in->choices,       sizeof(slot->choices));
    slot->choices_count = in->choices_count;
    return slot;
}

widget_t *widget_store_update(const char *card_id,
                              const char *body,
                              widget_tone_t tone,
                              float progress,
                              const char *action_label,
                              const char *action_event)
{
    widget_t *w = widget_store_find(card_id);
    if (!w) return NULL;
    if (body && body[0]) {
        strncpy(w->body, body, WIDGET_BODY_LEN - 1);
        w->body[WIDGET_BODY_LEN - 1] = '\0';
    }
    /* Tone 0 = CALM (default); we interpret the caller's 0 as "don't change"
     * only when progress is negative (see protocol §17.3 partial semantics).
     * Callers emitting a real CALM intentionally should pass progress >= 0. */
    if (progress >= 0.0f) {
        w->progress = progress;
        w->tone     = tone;          /* Accept any tone on real update */
    } else if (tone != WIDGET_TONE_CALM) {
        w->tone = tone;
    }
    if (action_label && action_label[0]) {
        strncpy(w->action_label, action_label, WIDGET_ACTION_LBL_LEN - 1);
        w->action_label[WIDGET_ACTION_LBL_LEN - 1] = '\0';
    }
    if (action_event && action_event[0]) {
        strncpy(w->action_event, action_event, WIDGET_ACTION_EVT_LEN - 1);
        w->action_event[WIDGET_ACTION_EVT_LEN - 1] = '\0';
    }
    w->updated_at_ms = now_ms();
    return w;
}

void widget_store_dismiss(const char *card_id)
{
    widget_t *w = widget_store_find(card_id);
    if (!w) return;
    w->active = false;
    w->updated_at_ms = now_ms();
}

widget_t *widget_store_live_active(void)
{
    if (!s_widgets) return NULL;
    widget_t *winner = NULL;
    for (int i = 0; i < s_capacity; i++) {
        widget_t *w = &s_widgets[i];
        /* v4·D Phase 4c: list widgets also compete for the live slot
         * via the same priority queue.  LIVE and LIST are both "now"
         * surfaces; CARD is for chat inline, MEDIA/PROMPT handled
         * elsewhere.  This lets web_search etc emit a LIST and take
         * the slot without needing a separate render path. */
        if (!w->active) continue;
        if (w->type != WIDGET_TYPE_LIVE
            && w->type != WIDGET_TYPE_LIST
            && w->type != WIDGET_TYPE_CHART
            && w->type != WIDGET_TYPE_MEDIA
            && w->type != WIDGET_TYPE_PROMPT) continue;
        if (!winner) { winner = w; continue; }
        if (w->priority > winner->priority) { winner = w; continue; }
        if (w->priority == winner->priority &&
            w->updated_at_ms > winner->updated_at_ms) {
            winner = w;
        }
    }
    return winner;
}

int widget_store_live_count(void)
{
    if (!s_widgets) return 0;
    int n = 0;
    for (int i = 0; i < s_capacity; i++) {
        if (s_widgets[i].active &&
            (s_widgets[i].type == WIDGET_TYPE_LIVE ||
             s_widgets[i].type == WIDGET_TYPE_LIST ||
             s_widgets[i].type == WIDGET_TYPE_CHART ||
             s_widgets[i].type == WIDGET_TYPE_MEDIA ||
             s_widgets[i].type == WIDGET_TYPE_PROMPT)) n++;
    }
    return n;
}

void widget_store_gc(uint32_t now)
{
    if (!s_widgets) return;
    for (int i = 0; i < s_capacity; i++) {
        widget_t *w = &s_widgets[i];
        if (!w->active) {
            /* Evict inactive records after GC_IDLE_MS of quiet. */
            if (w->card_id[0] && now - w->updated_at_ms > GC_IDLE_MS) {
                slot_clear(w);
            }
        } else if (w->expires_at_ms && now > w->expires_at_ms) {
            w->active = false;
            w->updated_at_ms = now;
        }
    }
}

widget_tone_t widget_tone_from_str(const char *s)
{
    if (!s) return WIDGET_TONE_CALM;
    if (!strcmp(s, "calm"))        return WIDGET_TONE_CALM;
    if (!strcmp(s, "active"))      return WIDGET_TONE_ACTIVE;
    if (!strcmp(s, "approaching")) return WIDGET_TONE_APPROACHING;
    if (!strcmp(s, "urgent"))      return WIDGET_TONE_URGENT;
    if (!strcmp(s, "done"))        return WIDGET_TONE_DONE;
    if (!strcmp(s, "info"))        return WIDGET_TONE_INFO;
    if (!strcmp(s, "success"))     return WIDGET_TONE_SUCCESS;
    if (!strcmp(s, "alert"))       return WIDGET_TONE_ALERT;
    return WIDGET_TONE_CALM;
}

widget_type_t widget_type_from_str(const char *s)
{
    if (!s) return WIDGET_TYPE_NONE;
    if (!strcmp(s, "live"))   return WIDGET_TYPE_LIVE;
    if (!strcmp(s, "card"))   return WIDGET_TYPE_CARD;
    if (!strcmp(s, "list"))   return WIDGET_TYPE_LIST;
    if (!strcmp(s, "chart"))  return WIDGET_TYPE_CHART;
    if (!strcmp(s, "media"))  return WIDGET_TYPE_MEDIA;
    if (!strcmp(s, "prompt")) return WIDGET_TYPE_PROMPT;
    return WIDGET_TYPE_NONE;
}
