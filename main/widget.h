/**
 * TinkerTab — Widget Platform v1 data model
 *
 * Typed skill-surface primitives. See docs/WIDGETS.md for the full spec.
 *
 * v1 scope: the LIVE widget only (home hero-slot). card/list/chart/media/prompt
 * come in later phases per docs/PLAN-widget-platform.md.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* ── Widget taxonomy ───────────────────────────────────────────── */

typedef enum {
    WIDGET_TYPE_NONE = 0,
    WIDGET_TYPE_LIVE,
    WIDGET_TYPE_CARD,
    WIDGET_TYPE_LIST,
    WIDGET_TYPE_CHART,
    WIDGET_TYPE_MEDIA,
    WIDGET_TYPE_PROMPT,
} widget_type_t;

/* Tone drives color + breathing on LIVE; info/success/alert for CARD. */
typedef enum {
    WIDGET_TONE_CALM = 0,    /* default when unknown — emerald, slow breath */
    WIDGET_TONE_ACTIVE,      /* amber, 3 s cycle                         */
    WIDGET_TONE_APPROACHING, /* amber-hot, 2 s pulse                     */
    WIDGET_TONE_URGENT,      /* rose, 1 s rapid                          */
    WIDGET_TONE_DONE,        /* emerald settled, static                  */
    WIDGET_TONE_INFO,        /* neutral card                             */
    WIDGET_TONE_SUCCESS,     /* emerald card                             */
    WIDGET_TONE_ALERT,       /* red card                                 */
} widget_tone_t;

/* ── Widget instance (single record in the store) ───────────────── */

#define WIDGET_ID_LEN        32
#define WIDGET_SKILL_ID_LEN  32
#define WIDGET_TITLE_LEN     64
#define WIDGET_BODY_LEN      256
#define WIDGET_ICON_LEN      16
#define WIDGET_ACTION_LBL_LEN 16
#define WIDGET_ACTION_EVT_LEN 48

/* v4·D Phase 4c widget_list items.  Used when widget.type==LIST.
 * Max 5 items (matches system-d-sovereign.html M widget_list mockup);
 * skills shipping longer lists are expected to either truncate or
 * offer a "+N more" affordance when they get the tap-action.         */
#define WIDGET_LIST_MAX_ITEMS  5
#define WIDGET_LIST_ITEM_TEXT_LEN   80
#define WIDGET_LIST_ITEM_VALUE_LEN  16

typedef struct {
    char text [WIDGET_LIST_ITEM_TEXT_LEN];
    char value[WIDGET_LIST_ITEM_VALUE_LEN]; /* optional right-side value */
} widget_list_item_t;

typedef struct {
    /* Identity */
    char          card_id[WIDGET_ID_LEN];
    char          skill_id[WIDGET_SKILL_ID_LEN];
    widget_type_t type;

    /* Presentation */
    char          title[WIDGET_TITLE_LEN];
    char          body[WIDGET_BODY_LEN];
    char          icon[WIDGET_ICON_LEN];
    widget_tone_t tone;
    float         progress;           /* 0..1 where meaningful, else 0     */

    /* Action (optional; empty label = none) */
    char          action_label[WIDGET_ACTION_LBL_LEN];
    char          action_event[WIDGET_ACTION_EVT_LEN];

    /* Priority + lifecycle */
    uint8_t       priority;           /* 0..100; default 50                */
    uint32_t      expires_at_ms;      /* 0 = never                         */
    uint32_t      created_at_ms;
    uint32_t      updated_at_ms;

    /* Flags */
    bool          active;             /* set false on dismiss; GC on age   */

    /* v4·D Phase 4c: list-type payload (only used when type==LIST).
     * items_count == 0 on non-list widgets. */
    widget_list_item_t items[WIDGET_LIST_MAX_ITEMS];
    uint8_t       items_count;
} widget_t;

/* ── Public API ────────────────────────────────────────────────── */

/** Initialize the widget store. Call once at boot. */
void widget_store_init(void);

/** Upsert a widget by card_id. Returns pointer to the stored record or NULL
 *  if the store is full of active records. Caller supplies the input; store
 *  copies into its internal slot. */
widget_t *widget_store_upsert(const widget_t *in);

/** Merge partial update into existing record by card_id. Only non-empty
 *  strings and non-zero numeric fields overwrite. Returns the record or
 *  NULL if not found. */
widget_t *widget_store_update(const char *card_id,
                              const char *body,        /* NULL = unchanged  */
                              widget_tone_t tone,      /* WIDGET_TONE_CALM=0 means unchanged here */
                              float progress,          /* <0 = unchanged    */
                              const char *action_label,
                              const char *action_event);

/** Mark a widget inactive by card_id. Store keeps the slot until GC. */
void widget_store_dismiss(const char *card_id);

/** Find by id. Returns NULL if not found. */
widget_t *widget_store_find(const char *card_id);

/** Return highest-priority active LIVE widget, or NULL if none. Priority
 *  tiebreak: most recently updated wins. */
widget_t *widget_store_live_active(void);

/** Count active LIVE widgets. Used by the home counter pill. */
int widget_store_live_count(void);

/** Evict expired + inactive records. Call periodically (e.g. from a timer). */
void widget_store_gc(uint32_t now_ms);

/** Convert a tone string like "calm" to the enum. Returns CALM on unknown. */
widget_tone_t widget_tone_from_str(const char *s);

/** Convert a type string like "live" to the enum. Returns NONE on unknown. */
widget_type_t widget_type_from_str(const char *s);
