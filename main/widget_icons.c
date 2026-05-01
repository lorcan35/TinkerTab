/**
 * @file widget_icons.c
 * @brief Implementation — see widget_icons.h for API contract.
 *
 * Each icon is a static array of `step_t` records describing
 * primitive shapes (line / polyline / rect / circle / arc) in the
 * 24×24 SVG viewBox space from the brainstorm reference.  At render
 * time we walk the array, scale coords ×2 to the 48 px target, and
 * issue lv_draw_* calls into a single `lv_canvas` widget's pixel
 * buffer.
 *
 * Why canvas rather than primitive children?  One widget per icon
 * keeps the BSS pool churn bounded — same lesson as TT #247 / Wave
 * 17 (each new lv_obj allocation goes through TLSF; multiplying by
 * 5 strokes per icon = 80 widgets across the 16 set, all churning
 * on icon-change).  The canvas pixel buffer is PSRAM-backed; a
 * 48×48 RGB565 buffer is 4608 bytes, a fixed cost per active icon
 * slot regardless of how many strokes the icon has.
 *
 * Coords are encoded in 24×24 source space; the renderer scales
 * everything to a fixed 48 px output.  Visual fidelity is "close
 * enough at 48 px" — some SVG cubic beziers from the source are
 * approximated as polylines or arc segments.
 */
#include "widget_icons.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "ui_theme.h"

static const char *TAG = "widget_icons";

#define ICONS_VBOX 24               /* source SVG viewBox */
#define ICONS_OUT WIDGET_ICONS_SIZE /* render target px (48) */
#define SCALE(v) ((int32_t)((v) * ICONS_OUT / ICONS_VBOX))

/* ── Path-step opcodes ────────────────────────────────────────────
 *
 * Tiny DSL — coords are in 24×24 viewBox space, compiled in flash.
 * Total icon table size: 16 icons × ~5 ops × 7 bytes ≈ 560 B.
 */
typedef enum {
   OP_END = 0,
   OP_LINE,   /* p[0..3] = x1, y1, x2, y2 */
   OP_POLY3,  /* p[0..5] = x1,y1, x2,y2, x3,y3 (3-point polyline) */
   OP_RECT,   /* p[0..3] = x, y, w, h (stroked, no fill) */
   OP_CIRCLE, /* p[0..2] = cx, cy, r (full 360° arc) */
   OP_ARC,    /* p[0..4] = cx, cy, r, start_deg, end_deg */
} op_t;

typedef struct {
   uint8_t op;
   int8_t p[6];
} step_t;

/* ── Icon path tables ───────────────────────────────────────────── */

static const step_t icon_clock[] = {
    {OP_CIRCLE, {12, 12, 10}},
    {OP_LINE, {12, 6, 12, 12}},
    {OP_LINE, {12, 12, 16, 14}},
    {OP_END, {0}},
};

static const step_t icon_briefcase[] = {
    {OP_RECT, {3, 7, 18, 13}}, {OP_LINE, {9, 7, 9, 5}}, {OP_LINE, {9, 5, 15, 5}},
    {OP_LINE, {15, 5, 15, 7}}, {OP_END, {0}},
};

static const step_t icon_laundry[] = {
    {OP_RECT, {7, 3, 10, 3}},
    {OP_RECT, {5, 6, 14, 15}},
    {OP_LINE, {10, 11, 10, 15}},
    {OP_LINE, {14, 11, 14, 15}},
    {OP_END, {0}},
};

static const step_t icon_coffee[] = {
    {OP_RECT, {5, 8, 10, 10}}, {OP_ARC, {17, 12, 2, 270, 90}}, /* handle right */
    {OP_LINE, {8, 4, 7, 6}},   {OP_LINE, {11, 4, 10, 6}},      {OP_END, {0}},
};

static const step_t icon_book[] = {
    {OP_RECT, {5, 4, 13, 16}},
    {OP_LINE, {9, 4, 9, 20}},
    {OP_LINE, {5, 10, 9, 10}},
    {OP_LINE, {5, 15, 9, 15}},
    {OP_END, {0}},
};

static const step_t icon_car[] = {
    {OP_LINE, {5, 12, 7, 6}},   {OP_LINE, {7, 6, 17, 6}},    {OP_LINE, {17, 6, 19, 12}},
    {OP_LINE, {4, 12, 20, 12}}, {OP_LINE, {5, 12, 5, 17}},   {OP_LINE, {19, 12, 19, 17}},
    {OP_LINE, {6, 16, 8, 16}},  {OP_LINE, {16, 16, 18, 16}}, {OP_END, {0}},
};

static const step_t icon_pot[] = {
    {OP_RECT, {5, 10, 14, 9}}, {OP_ARC, {21, 13, 2, 270, 90}}, /* side handle */
    {OP_LINE, {7, 5, 6, 8}},   {OP_LINE, {10, 5, 9, 8}},       {OP_LINE, {13, 5, 12, 8}}, {OP_END, {0}},
};

static const step_t icon_person[] = {
    {OP_CIRCLE, {12, 7, 3}},
    {OP_LINE, {12, 11, 6, 18}},
    {OP_LINE, {12, 11, 18, 18}},
    {OP_END, {0}},
};

/* Droplet — triangle + arc bottom approximates the teardrop. */
static const step_t icon_droplet[] = {
    {OP_LINE, {12, 4, 6, 13}},
    {OP_LINE, {18, 13, 12, 4}},
    {OP_ARC, {12, 15, 6, 0, 180}},
    {OP_END, {0}},
};

static const step_t icon_check[] = {
    {OP_POLY3, {4, 12, 9, 17, 20, 6}},
    {OP_END, {0}},
};

static const step_t icon_alert[] = {
    {OP_CIRCLE, {12, 12, 9}},
    {OP_LINE, {12, 8, 12, 12}},
    {OP_CIRCLE, {12, 16, 1}},
    {OP_END, {0}},
};

static const step_t icon_sun[] = {
    {OP_CIRCLE, {12, 12, 3}},    {OP_LINE, {12, 3, 12, 5}},
    {OP_LINE, {12, 19, 12, 21}}, {OP_LINE, {3, 12, 5, 12}},
    {OP_LINE, {19, 12, 21, 12}}, {OP_LINE, {6, 6, 7, 7}},
    {OP_LINE, {17, 17, 18, 18}}, {OP_LINE, {6, 18, 7, 17}},
    {OP_LINE, {17, 7, 18, 6}},   {OP_END, {0}},
};

/* Moon — outer arc, with a smaller offset arc creating crescent.
 * (Both rendered as full circles; without fill the inner arc carves
 * the crescent visually.) */
static const step_t icon_moon[] = {
    {OP_CIRCLE, {12, 11, 6}},
    {OP_CIRCLE, {15, 12, 5}},
    {OP_END, {0}},
};

static const step_t icon_cloud[] = {
    {OP_ARC, {12, 12, 6, 180, 360}},
    {OP_LINE, {6, 12, 6, 17}},
    {OP_LINE, {18, 12, 18, 17}},
    {OP_LINE, {4, 18, 20, 18}},
    {OP_END, {0}},
};

static const step_t icon_calendar[] = {
    {OP_RECT, {4, 4, 16, 16}},
    {OP_LINE, {4, 9, 20, 9}},
    {OP_LINE, {9, 4, 9, 9}},
    {OP_LINE, {15, 4, 15, 9}},
    {OP_END, {0}},
};

static const step_t icon_star[] = {
    {OP_LINE, {12, 3, 15, 9}},
    {OP_LINE, {15, 9, 22, 10}},
    {OP_LINE, {22, 10, 17, 15}},
    {OP_LINE, {17, 15, 18, 22}},
    {OP_LINE, {18, 22, 12, 19}},
    {OP_LINE, {12, 19, 6, 22}},
    {OP_LINE, {6, 22, 7, 15}},
    {OP_LINE, {7, 15, 2, 10}},
    {OP_LINE, {2, 10, 9, 9}},
    {OP_LINE, {9, 9, 12, 3}},
    {OP_END, {0}},
};

/* ── Lookup table ───────────────────────────────────────────────── */

typedef struct {
   const char *name;
   const step_t *path;
} icon_entry_t;

static const icon_entry_t s_icons[WIDGET_ICON__COUNT] = {
    [WIDGET_ICON_NONE] = {NULL, NULL},
    [WIDGET_ICON_CLOCK] = {"clock", icon_clock},
    [WIDGET_ICON_BRIEFCASE] = {"briefcase", icon_briefcase},
    [WIDGET_ICON_LAUNDRY] = {"laundry", icon_laundry},
    [WIDGET_ICON_COFFEE] = {"coffee", icon_coffee},
    [WIDGET_ICON_BOOK] = {"book", icon_book},
    [WIDGET_ICON_CAR] = {"car", icon_car},
    [WIDGET_ICON_POT] = {"pot", icon_pot},
    [WIDGET_ICON_PERSON] = {"person", icon_person},
    [WIDGET_ICON_DROPLET] = {"droplet", icon_droplet},
    [WIDGET_ICON_CHECK] = {"check", icon_check},
    [WIDGET_ICON_ALERT] = {"alert", icon_alert},
    [WIDGET_ICON_SUN] = {"sun", icon_sun},
    [WIDGET_ICON_MOON] = {"moon", icon_moon},
    [WIDGET_ICON_CLOUD] = {"cloud", icon_cloud},
    [WIDGET_ICON_CALENDAR] = {"calendar", icon_calendar},
    [WIDGET_ICON_STAR] = {"star", icon_star},
};

widget_icon_id_t widget_icons_lookup(const char *name) {
   if (name == NULL || name[0] == '\0') return WIDGET_ICON_NONE;
   for (int i = 1; i < WIDGET_ICON__COUNT; i++) {
      if (s_icons[i].name && strcmp(s_icons[i].name, name) == 0) {
         return (widget_icon_id_t)i;
      }
   }
   return WIDGET_ICON_NONE;
}

/* ── Canvas-based renderer ──────────────────────────────────────── */

/* Per-canvas user_data so we can free the buffer when the canvas
 * gets deleted as part of an `lv_obj_clean(container)`. */
typedef struct {
   void *buf;
} canvas_ud_t;

static void canvas_delete_cb(lv_event_t *e) {
   lv_obj_t *canvas = lv_event_get_target(e);
   canvas_ud_t *ud = (canvas_ud_t *)lv_obj_get_user_data(canvas);
   if (ud) {
      if (ud->buf) heap_caps_free(ud->buf);
      heap_caps_free(ud);
      lv_obj_set_user_data(canvas, NULL);
   }
}

static void render_step(lv_layer_t *layer, const step_t *st, lv_color_t color, int stroke_w) {
   switch (st->op) {
      case OP_LINE: {
         lv_draw_line_dsc_t d;
         lv_draw_line_dsc_init(&d);
         d.color = color;
         d.width = stroke_w;
         d.round_start = 1;
         d.round_end = 1;
         d.p1.x = SCALE(st->p[0]);
         d.p1.y = SCALE(st->p[1]);
         d.p2.x = SCALE(st->p[2]);
         d.p2.y = SCALE(st->p[3]);
         lv_draw_line(layer, &d);
         break;
      }
      case OP_POLY3: {
         /* Render as two connected line segments — preserves the
          * rounded-cap join at the inner vertex. */
         lv_draw_line_dsc_t d;
         lv_draw_line_dsc_init(&d);
         d.color = color;
         d.width = stroke_w;
         d.round_start = 1;
         d.round_end = 1;
         d.p1.x = SCALE(st->p[0]);
         d.p1.y = SCALE(st->p[1]);
         d.p2.x = SCALE(st->p[2]);
         d.p2.y = SCALE(st->p[3]);
         lv_draw_line(layer, &d);
         d.p1.x = SCALE(st->p[2]);
         d.p1.y = SCALE(st->p[3]);
         d.p2.x = SCALE(st->p[4]);
         d.p2.y = SCALE(st->p[5]);
         lv_draw_line(layer, &d);
         break;
      }
      case OP_RECT: {
         /* Stroked rect = 4 lines.  No `lv_draw_rect_dsc.border_*`
          * usable here because it draws around an area, not a
          * stroked outline of one. */
         int x1 = SCALE(st->p[0]);
         int y1 = SCALE(st->p[1]);
         int x2 = SCALE(st->p[0] + st->p[2]);
         int y2 = SCALE(st->p[1] + st->p[3]);
         lv_draw_line_dsc_t d;
         lv_draw_line_dsc_init(&d);
         d.color = color;
         d.width = stroke_w;
         d.round_start = 1;
         d.round_end = 1;
         /* top */
         d.p1.x = x1;
         d.p1.y = y1;
         d.p2.x = x2;
         d.p2.y = y1;
         lv_draw_line(layer, &d);
         /* right */
         d.p1.x = x2;
         d.p1.y = y1;
         d.p2.x = x2;
         d.p2.y = y2;
         lv_draw_line(layer, &d);
         /* bottom */
         d.p1.x = x1;
         d.p1.y = y2;
         d.p2.x = x2;
         d.p2.y = y2;
         lv_draw_line(layer, &d);
         /* left */
         d.p1.x = x1;
         d.p1.y = y1;
         d.p2.x = x1;
         d.p2.y = y2;
         lv_draw_line(layer, &d);
         break;
      }
      case OP_CIRCLE: {
         lv_draw_arc_dsc_t d;
         lv_draw_arc_dsc_init(&d);
         d.color = color;
         d.width = stroke_w;
         d.rounded = 0;
         d.center.x = SCALE(st->p[0]);
         d.center.y = SCALE(st->p[1]);
         d.radius = SCALE(st->p[2]);
         d.start_angle = 0;
         d.end_angle = 360;
         lv_draw_arc(layer, &d);
         break;
      }
      case OP_ARC: {
         lv_draw_arc_dsc_t d;
         lv_draw_arc_dsc_init(&d);
         d.color = color;
         d.width = stroke_w;
         d.rounded = 1;
         d.center.x = SCALE(st->p[0]);
         d.center.y = SCALE(st->p[1]);
         d.radius = SCALE(st->p[2]);
         d.start_angle = st->p[3] < 0 ? 0 : (uint16_t)st->p[3];
         d.end_angle = (uint16_t)st->p[4];
         /* Wrap negative end_angle convention (>=360) by normalising
          * caller-side encoding. */
         lv_draw_arc(layer, &d);
         break;
      }
      default:
         break;
   }
}

void widget_icons_render(lv_obj_t *container, widget_icon_id_t id, lv_color_t color, int stroke_w) {
   if (container == NULL) return;
   if (id <= WIDGET_ICON_NONE || id >= WIDGET_ICON__COUNT) return;
   const step_t *path = s_icons[id].path;
   if (path == NULL) return;
   if (stroke_w <= 0) stroke_w = WIDGET_ICONS_STROKE;

   /* Allocate a 48×48 ARGB8888 canvas buffer in PSRAM.  Need a real
    * alpha channel so the icon-on-transparent compositing works
    * cleanly over whatever the parent's background is — the home
    * card has a transparent body that shows through to the screen
    * gradient, so any opaque background here would punch a 48×48
    * box of wrong color into the layout.
    *
    * 48×48×4 bytes = 9 KB per active icon slot, PSRAM-backed.
    * Buffer is freed automatically on container clean via
    * `canvas_delete_cb` (LV_EVENT_DELETE handler). */
   const size_t buf_sz = LV_CANVAS_BUF_SIZE(ICONS_OUT, ICONS_OUT, 32, LV_DRAW_BUF_STRIDE_ALIGN);
   void *buf = heap_caps_malloc(buf_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (buf == NULL) {
      ESP_LOGW(TAG, "icon canvas alloc failed (id=%d, %u B)", id, (unsigned)buf_sz);
      return;
   }

   lv_obj_t *canvas = lv_canvas_create(container);
   if (canvas == NULL) {
      heap_caps_free(buf);
      return;
   }
   lv_canvas_set_buffer(canvas, buf, ICONS_OUT, ICONS_OUT, LV_COLOR_FORMAT_ARGB8888);
   /* Transparent-fill so only the strokes show. */
   lv_canvas_fill_bg(canvas, lv_color_hex(0x000000), LV_OPA_TRANSP);
   lv_obj_set_size(canvas, ICONS_OUT, ICONS_OUT);
   lv_obj_set_pos(canvas, 0, 0);
   lv_obj_clear_flag(canvas, LV_OBJ_FLAG_SCROLLABLE);

   /* Track the buffer via user_data so cleanup is automatic when
    * the parent container is `lv_obj_clean`'d (cascade-deletes
    * the canvas, which fires our DELETE event handler). */
   canvas_ud_t *ud = heap_caps_calloc(1, sizeof(*ud), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
   if (ud != NULL) {
      ud->buf = buf;
      lv_obj_set_user_data(canvas, ud);
      lv_obj_add_event_cb(canvas, canvas_delete_cb, LV_EVENT_DELETE, NULL);
   }

   /* Walk the path, draw each step into the canvas's pixel buffer. */
   lv_layer_t layer;
   lv_canvas_init_layer(canvas, &layer);
   for (const step_t *st = path; st->op != OP_END; st++) {
      render_step(&layer, st, color, stroke_w);
   }
   lv_canvas_finish_layer(canvas, &layer);
}

void widget_icons_render_named(lv_obj_t *container, const char *name, lv_color_t color, int stroke_w) {
   widget_icons_render(container, widget_icons_lookup(name), color, stroke_w);
}

lv_color_t widget_icons_color_for_tone(widget_tone_t tone) {
   switch (tone) {
      case WIDGET_TONE_CALM:
         return lv_color_hex(TH_STATUS_GREEN);
      case WIDGET_TONE_ACTIVE:
         return lv_color_hex(TH_AMBER);
      case WIDGET_TONE_APPROACHING:
         return lv_color_hex(0xF59E0B);
      case WIDGET_TONE_URGENT:
         return lv_color_hex(0xE5484D);
      case WIDGET_TONE_DONE:
         return lv_color_hex(TH_STATUS_GREEN);
      case WIDGET_TONE_INFO:
         return lv_color_hex(0x94A3B8);
      case WIDGET_TONE_SUCCESS:
         return lv_color_hex(TH_STATUS_GREEN);
      case WIDGET_TONE_ALERT:
         return lv_color_hex(0xE5484D);
      default:
         return lv_color_hex(0xC0C0C8);
   }
}
