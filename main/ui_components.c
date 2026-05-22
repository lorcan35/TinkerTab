/**
 * @file ui_components.c
 * @brief Shared UI atoms — Polish Wave P1 (TT #648).
 *
 * Implementations are intentionally short.  Each atom is one LVGL
 * widget tree built from `ui_theme.h` tokens + `ui_feedback.h`
 * press-state helpers.  No persistent state, no allocator calls,
 * no PSRAM use — atoms are pure widget factories.
 */

#include "ui_components.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "config.h"
#include "lvgl.h"
#include "ui_core.h"
#include "ui_feedback.h"
#include "ui_theme.h"

/* ── Layout constants — shared by every atom ───────────────────── */
#define UI_TOPBAR_H 48
#define UI_LIST_ROW_H 64
#define UI_PILL_H 36
#define UI_ACTION_BTN_H 44
#define UI_CHIP_H 28

/* ── Tone → color resolver ──────────────────────────────────────── */
static uint32_t tone_to_accent(ui_tone_t tone) {
   switch (tone) {
      case UI_TONE_ACCENT:
         return TH_AMBER;
      case UI_TONE_SUCCESS:
         return 0x22C55E; /* TT #328 success-green */
      case UI_TONE_DANGER:
         return 0xEF4444;
      case UI_TONE_INFO:
         return 0x3B82F6;
      case UI_TONE_DIM:
         return TH_TEXT_DIM;
      case UI_TONE_NEUTRAL:
      default:
         return TH_TEXT_BODY;
   }
}

/* ─────────────────────────────────────────────────────────────────
 *  Top-bar
 * ───────────────────────────────────────────────────────────────── */

lv_obj_t *ui_topbar(lv_obj_t *parent, const char *title, ui_topbar_cb_t back_cb, lv_obj_t **out_right_slot) {
   lv_obj_t *bar = lv_obj_create(parent);
   if (!bar) return NULL;
   lv_obj_remove_style_all(bar);
   lv_obj_set_size(bar, lv_obj_get_width(parent), UI_TOPBAR_H);
   lv_obj_set_pos(bar, 0, 0);
   lv_obj_set_style_bg_color(bar, lv_color_hex(TH_BG), 0);
   lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
   lv_obj_set_style_pad_all(bar, 0, 0);
   lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

   /* Back arrow (only when caller supplies a handler — explicit is
    * better than implicit; no caller wants a phantom back button). */
   if (back_cb) {
      lv_obj_t *back = lv_obj_create(bar);
      lv_obj_remove_style_all(back);
      lv_obj_set_size(back, 60, UI_TOPBAR_H);
      lv_obj_set_pos(back, 0, 0);
      lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, 0);
      lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_clear_flag(back, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);
      ui_fb_button(back);
      lv_obj_t *icon = lv_label_create(back);
      lv_label_set_text(icon, LV_SYMBOL_LEFT);
      lv_obj_set_style_text_color(icon, lv_color_hex(TH_TEXT_PRIMARY), 0);
      lv_obj_set_style_text_font(icon, FONT_HEADING, 0);
      lv_obj_center(icon);
   }

   /* Title — centered horizontally in the bar.  FONT_HEADING for
    * scannability.  Caller passes empty string to suppress. */
   if (title && title[0]) {
      lv_obj_t *lbl = lv_label_create(bar);
      lv_label_set_text(lbl, title);
      lv_obj_set_style_text_font(lbl, FONT_HEADING, 0);
      lv_obj_set_style_text_color(lbl, lv_color_hex(TH_TEXT_PRIMARY), 0);
      lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
   }

   /* Right-slot — caller stashes pointer; can lv_obj_set_parent
    * children to it later.  Useful for action buttons or live status
    * chips (privacy lock, k144 health). */
   if (out_right_slot) {
      lv_obj_t *slot = lv_obj_create(bar);
      lv_obj_remove_style_all(slot);
      lv_obj_set_size(slot, 120, UI_TOPBAR_H);
      lv_obj_align(slot, LV_ALIGN_RIGHT_MID, 0, 0);
      lv_obj_set_style_bg_opa(slot, LV_OPA_TRANSP, 0);
      lv_obj_set_style_pad_all(slot, 0, 0);
      lv_obj_clear_flag(slot, LV_OBJ_FLAG_SCROLLABLE);
      *out_right_slot = slot;
   }

   return bar;
}

/* ─────────────────────────────────────────────────────────────────
 *  Card
 * ───────────────────────────────────────────────────────────────── */

lv_obj_t *ui_card(lv_obj_t *parent, int x, int y, int w, int h) {
   lv_obj_t *card = lv_obj_create(parent);
   if (!card) return NULL;
   lv_obj_remove_style_all(card);
   lv_obj_set_pos(card, x, y);
   lv_obj_set_size(card, w, h);
   lv_obj_set_style_bg_color(card, lv_color_hex(TH_CARD), 0);
   lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
   lv_obj_set_style_radius(card, 16, 0);
   lv_obj_set_style_border_width(card, 1, 0);
   lv_obj_set_style_border_color(card, lv_color_hex(0x1E2030), 0);
   lv_obj_set_style_border_opa(card, LV_OPA_COVER, 0);
   lv_obj_set_style_pad_all(card, 14, 0);
   lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
   return card;
}

/* ─────────────────────────────────────────────────────────────────
 *  List row
 * ───────────────────────────────────────────────────────────────── */

lv_obj_t *ui_list_row(lv_obj_t *parent, int y, const char *label, const char *secondary, ui_tone_t tone) {
   int parent_w = lv_obj_get_width(parent);
   if (parent_w <= 0) parent_w = 680;
   lv_obj_t *row = lv_obj_create(parent);
   if (!row) return NULL;
   lv_obj_remove_style_all(row);
   lv_obj_set_size(row, parent_w, UI_LIST_ROW_H);
   lv_obj_set_pos(row, 0, y);
   lv_obj_set_style_bg_color(row, lv_color_hex(TH_CARD), 0);
   lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
   lv_obj_set_style_radius(row, 14, 0);
   lv_obj_set_style_pad_all(row, 14, 0);
   lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

   /* Accent bar on the left edge for selected / focused rows. */
   if (tone == UI_TONE_ACCENT) {
      lv_obj_t *bar = lv_obj_create(row);
      lv_obj_remove_style_all(bar);
      lv_obj_set_size(bar, 3, UI_LIST_ROW_H - 8);
      lv_obj_align(bar, LV_ALIGN_LEFT_MID, -10, 0);
      lv_obj_set_style_bg_color(bar, lv_color_hex(TH_AMBER), 0);
      lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
      lv_obj_set_style_radius(bar, 2, 0);
   }

   if (label && label[0]) {
      lv_obj_t *lbl = lv_label_create(row);
      lv_label_set_text(lbl, label);
      lv_obj_set_style_text_font(lbl, FONT_BODY, 0);
      lv_obj_set_style_text_color(lbl, lv_color_hex(TH_TEXT_PRIMARY), 0);
      lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);
   }

   if (secondary && secondary[0]) {
      lv_obj_t *sec = lv_label_create(row);
      lv_label_set_text(sec, secondary);
      lv_obj_set_style_text_font(sec, FONT_SECONDARY, 0);
      lv_obj_set_style_text_color(sec, lv_color_hex(TH_TEXT_DIM), 0);
      lv_obj_align(sec, LV_ALIGN_RIGHT_MID, 0, 0);
   }

   return row;
}

/* ─────────────────────────────────────────────────────────────────
 *  Filter pills — selected-index state lives on the container via
 *  user_data; each pill's tap handler updates it and repaints siblings.
 * ───────────────────────────────────────────────────────────────── */

typedef struct {
   int n;
   int selected;
   ui_filter_cb_t cb;
} filter_state_t;

static void filter_pill_cb(lv_event_t *e) {
   lv_obj_t *pill = lv_event_get_target(e);
   lv_obj_t *container = lv_obj_get_parent(pill);
   filter_state_t *state = (filter_state_t *)lv_obj_get_user_data(container);
   if (!state) return;
   /* Index = position of pill among siblings. */
   int idx = -1;
   int n = lv_obj_get_child_count(container);
   for (int i = 0; i < n; i++) {
      if (lv_obj_get_child(container, i) == pill) {
         idx = i;
         break;
      }
   }
   if (idx < 0 || idx == state->selected) return;
   /* Repaint old + new pills. */
   for (int i = 0; i < n; i++) {
      lv_obj_t *p = lv_obj_get_child(container, i);
      bool sel = (i == idx);
      lv_obj_set_style_bg_color(p, lv_color_hex(sel ? TH_AMBER : TH_CARD), 0);
      lv_obj_set_style_bg_opa(p, sel ? LV_OPA_COVER : LV_OPA_COVER, 0);
      lv_obj_set_style_border_color(p, lv_color_hex(sel ? TH_AMBER : 0x1E2030), 0);
      /* Set text color on the first child label. */
      if (lv_obj_get_child_count(p) > 0) {
         lv_obj_t *lbl = lv_obj_get_child(p, 0);
         lv_obj_set_style_text_color(lbl, lv_color_hex(sel ? 0x111119 : TH_TEXT_BODY), 0);
      }
   }
   state->selected = idx;
   if (state->cb) state->cb(idx);
}

lv_obj_t *ui_filter_pills(lv_obj_t *parent, int y, int w, const char *const *labels, int n_labels, int selected,
                          ui_filter_cb_t cb) {
   if (n_labels <= 0) return NULL;
   lv_obj_t *container = lv_obj_create(parent);
   if (!container) return NULL;
   lv_obj_remove_style_all(container);
   lv_obj_set_size(container, w, UI_PILL_H + 8);
   lv_obj_set_pos(container, 0, y);
   lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
   lv_obj_set_style_pad_all(container, 0, 0);
   lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);

   /* Persist state on the container via user_data.  static_alloc so
    * we don't malloc per pill row — each filter row owns one. */
   static filter_state_t s_states[4]; /* up to 4 filter rows in app */
   static int s_states_n = 0;
   filter_state_t *st = NULL;
   if (s_states_n < (int)(sizeof(s_states) / sizeof(s_states[0]))) {
      st = &s_states[s_states_n++];
      st->n = n_labels;
      st->selected = selected;
      st->cb = cb;
      lv_obj_set_user_data(container, st);
   }

   int gap = 8;
   int total_gap = gap * (n_labels - 1);
   int pill_w = (w - total_gap) / n_labels;
   for (int i = 0; i < n_labels; i++) {
      lv_obj_t *p = lv_obj_create(container);
      lv_obj_remove_style_all(p);
      lv_obj_set_size(p, pill_w, UI_PILL_H);
      lv_obj_set_pos(p, i * (pill_w + gap), 4);
      bool sel = (i == selected);
      lv_obj_set_style_bg_color(p, lv_color_hex(sel ? TH_AMBER : TH_CARD), 0);
      lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
      lv_obj_set_style_radius(p, UI_PILL_H / 2, 0);
      lv_obj_set_style_border_width(p, 1, 0);
      lv_obj_set_style_border_color(p, lv_color_hex(sel ? TH_AMBER : 0x1E2030), 0);
      lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_add_event_cb(p, filter_pill_cb, LV_EVENT_CLICKED, NULL);
      ui_fb_button(p);
      lv_obj_t *lbl = lv_label_create(p);
      lv_label_set_text(lbl, labels[i]);
      lv_obj_set_style_text_font(lbl, FONT_BODY, 0);
      lv_obj_set_style_text_color(lbl, lv_color_hex(sel ? 0x111119 : TH_TEXT_BODY), 0);
      lv_obj_center(lbl);
   }
   return container;
}

/* ─────────────────────────────────────────────────────────────────
 *  Action button
 * ───────────────────────────────────────────────────────────────── */

lv_obj_t *ui_action_btn(lv_obj_t *parent, const char *icon, const char *label, ui_tone_t tone, ui_topbar_cb_t cb,
                        void *user_data) {
   lv_obj_t *btn = lv_obj_create(parent);
   if (!btn) return NULL;
   lv_obj_remove_style_all(btn);
   int w = (icon && label) ? 140 : 56;
   lv_obj_set_size(btn, w, UI_ACTION_BTN_H);
   uint32_t accent = tone_to_accent(tone);
   lv_obj_set_style_bg_color(btn, lv_color_hex(TH_CARD), 0);
   lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
   lv_obj_set_style_radius(btn, UI_ACTION_BTN_H / 2, 0);
   lv_obj_set_style_border_width(btn, 1, 0);
   lv_obj_set_style_border_color(btn, lv_color_hex(accent), 0);
   lv_obj_set_style_border_opa(btn, LV_OPA_COVER, 0);
   lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
   lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
   if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
   ui_fb_button(btn);

   /* Compose label string: "icon  label" when both supplied. */
   char text[64];
   if (icon && label)
      snprintf(text, sizeof(text), "%s  %s", icon, label);
   else if (icon)
      snprintf(text, sizeof(text), "%s", icon);
   else if (label)
      snprintf(text, sizeof(text), "%s", label);
   else
      text[0] = '\0';
   lv_obj_t *lbl = lv_label_create(btn);
   lv_label_set_text(lbl, text);
   lv_obj_set_style_text_font(lbl, FONT_BODY, 0);
   lv_obj_set_style_text_color(lbl, lv_color_hex(accent), 0);
   lv_obj_center(lbl);
   return btn;
}

/* ─────────────────────────────────────────────────────────────────
 *  Chip
 * ───────────────────────────────────────────────────────────────── */

lv_obj_t *ui_chip(lv_obj_t *parent, int x, int y, const char *label, ui_tone_t tone) {
   if (!label) label = "";
   uint32_t accent = tone_to_accent(tone);
   /* Estimate chip width from label length (FONT_SECONDARY is ~10 px/char). */
   int label_len = strlen(label);
   int chip_w = label_len * 10 + 24;
   if (chip_w < 60) chip_w = 60;

   lv_obj_t *chip = lv_obj_create(parent);
   if (!chip) return NULL;
   lv_obj_remove_style_all(chip);
   lv_obj_set_size(chip, chip_w, UI_CHIP_H);
   lv_obj_set_pos(chip, x, y);
   lv_obj_set_style_bg_color(chip, lv_color_hex(TH_CARD), 0);
   lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
   lv_obj_set_style_radius(chip, UI_CHIP_H / 2, 0);
   lv_obj_set_style_border_width(chip, 1, 0);
   lv_obj_set_style_border_color(chip, lv_color_hex(accent), 0);
   lv_obj_set_style_border_opa(chip, LV_OPA_COVER, 0);
   lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
   lv_obj_clear_flag(chip, LV_OBJ_FLAG_CLICKABLE);

   lv_obj_t *lbl = lv_label_create(chip);
   lv_label_set_text(lbl, label);
   lv_obj_set_style_text_font(lbl, FONT_SECONDARY, 0);
   lv_obj_set_style_text_color(lbl, lv_color_hex(accent), 0);
   lv_obj_set_style_text_letter_space(lbl, 2, 0);
   lv_obj_center(lbl);
   return chip;
}

/* ─────────────────────────────────────────────────────────────────
 *  Empty state
 * ───────────────────────────────────────────────────────────────── */

lv_obj_t *ui_empty_state(lv_obj_t *parent, const char *icon_text, const char *title, const char *hint) {
   lv_obj_t *box = lv_obj_create(parent);
   if (!box) return NULL;
   lv_obj_remove_style_all(box);
   lv_obj_set_size(box, lv_obj_get_width(parent), 240);
   lv_obj_center(box);
   lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
   lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
   lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);

   int y = 0;
   if (icon_text && icon_text[0]) {
      lv_obj_t *icon = lv_label_create(box);
      lv_label_set_text(icon, icon_text);
      lv_obj_set_style_text_font(icon, FONT_TITLE, 0);
      lv_obj_set_style_text_color(icon, lv_color_hex(TH_TEXT_DIM), 0);
      lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, y);
      y += 56;
   }
   if (title && title[0]) {
      lv_obj_t *lbl = lv_label_create(box);
      lv_label_set_text(lbl, title);
      lv_obj_set_style_text_font(lbl, FONT_HEADING, 0);
      lv_obj_set_style_text_color(lbl, lv_color_hex(TH_TEXT_BODY), 0);
      lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, y);
      y += 36;
   }
   if (hint && hint[0]) {
      lv_obj_t *lbl = lv_label_create(box);
      lv_label_set_text(lbl, hint);
      lv_obj_set_style_text_font(lbl, FONT_SECONDARY, 0);
      lv_obj_set_style_text_color(lbl, lv_color_hex(TH_TEXT_DIM), 0);
      lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
      lv_obj_set_width(lbl, lv_obj_get_width(parent) - 80);
      lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, y);
   }
   return box;
}

/* ─────────────────────────────────────────────────────────────────
 *  Time formatter
 * ───────────────────────────────────────────────────────────────── */

void ui_fmt_time(char *buf, size_t cap, int64_t ts, ui_time_style_t style) {
   if (!buf || cap < 8) return;
   time_t now = time(NULL);
   time_t when = (time_t)ts;
   struct tm tm_now = {0}, tm_when = {0};
   localtime_r(&now, &tm_now);
   localtime_r(&when, &tm_when);

   if (style == UI_TIME_SHORT) {
      snprintf(buf, cap, "%02d:%02d", tm_when.tm_hour, tm_when.tm_min);
      return;
   }
   if (style == UI_TIME_ABSOLUTE) {
      static const char *mons[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
      int m = tm_when.tm_mon;
      if (m < 0 || m > 11) m = 0;
      snprintf(buf, cap, "%s %d %02d:%02d", mons[m], tm_when.tm_mday, tm_when.tm_hour, tm_when.tm_min);
      return;
   }

   /* RELATIVE: today / yesterday / day-of-week (≤7 days) / absolute. */
   long diff_days = ((long)now - (long)when) / 86400;
   if (now <= 0 || when <= 0 || diff_days < 0) {
      /* Clock not set or future ts — fall back to absolute. */
      ui_fmt_time(buf, cap, ts, UI_TIME_ABSOLUTE);
      return;
   }
   if (tm_now.tm_year == tm_when.tm_year && tm_now.tm_yday == tm_when.tm_yday) {
      snprintf(buf, cap, "Today %02d:%02d", tm_when.tm_hour, tm_when.tm_min);
   } else if (diff_days == 1) {
      snprintf(buf, cap, "Yest %02d:%02d", tm_when.tm_hour, tm_when.tm_min);
   } else if (diff_days < 7) {
      static const char *dows[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
      int d = tm_when.tm_wday;
      if (d < 0 || d > 6) d = 0;
      snprintf(buf, cap, "%s %02d:%02d", dows[d], tm_when.tm_hour, tm_when.tm_min);
   } else {
      ui_fmt_time(buf, cap, ts, UI_TIME_ABSOLUTE);
   }
}

/* ─────────────────────────────────────────────────────────────────
 *  Byte / count formatters
 * ───────────────────────────────────────────────────────────────── */

void ui_fmt_bytes(char *buf, size_t cap, uint64_t n) {
   if (!buf || cap < 8) return;
   if (n < 1024) {
      snprintf(buf, cap, "%llu B", (unsigned long long)n);
   } else if (n < 1024 * 1024) {
      snprintf(buf, cap, "%.1f KB", (double)n / 1024.0);
   } else if (n < 1024ULL * 1024 * 1024) {
      snprintf(buf, cap, "%.1f MB", (double)n / (1024.0 * 1024.0));
   } else {
      snprintf(buf, cap, "%.1f GB", (double)n / (1024.0 * 1024.0 * 1024.0));
   }
}

void ui_fmt_count(char *buf, size_t cap, uint64_t n) {
   if (!buf || cap < 8) return;
   if (n < 1000) {
      snprintf(buf, cap, "%llu", (unsigned long long)n);
   } else if (n < 1000 * 1000) {
      snprintf(buf, cap, "%.1fK", (double)n / 1000.0);
   } else {
      snprintf(buf, cap, "%.1fM", (double)n / 1000000.0);
   }
}

/* ─────────────────────────────────────────────────────────────────
 *  Overlay fade-in
 * ───────────────────────────────────────────────────────────────── */

static void fade_anim_cb(void *obj, int32_t v) {
   if (obj) lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, LV_PART_MAIN);
}

void ui_overlay_fade_in(lv_obj_t *obj, uint32_t duration_ms) {
   if (!obj) return;
   if (duration_ms == 0) duration_ms = 250;
   /* Snap to opaque-zero before animating so the widget appears at 0
    * and animates up.  Idempotent — calling on a fully-visible widget
    * just blinks it in 250 ms. */
   lv_obj_set_style_opa(obj, LV_OPA_TRANSP, LV_PART_MAIN);
   lv_anim_t a;
   lv_anim_init(&a);
   lv_anim_set_var(&a, obj);
   lv_anim_set_exec_cb(&a, fade_anim_cb);
   lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
   lv_anim_set_duration(&a, duration_ms);
   lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
   lv_anim_start(&a);
}

/* ─────────────────────────────────────────────────────────────────
 *  Skeleton row — static dim-gray placeholder bars
 * ───────────────────────────────────────────────────────────────── */

lv_obj_t *ui_skeleton_row(lv_obj_t *parent, int y, int w, int lines) {
   if (lines < 1) lines = 1;
   if (lines > 4) lines = 4;
   /* Row container.  Card-shaped to match real list rows so the
    * placeholder reads as "row coming" rather than "broken UI". */
   const int line_h = 14;
   const int line_gap = 8;
   const int pad = 14;
   const int h = pad * 2 + lines * line_h + (lines - 1) * line_gap;
   lv_obj_t *box = lv_obj_create(parent);
   if (!box) return NULL;
   lv_obj_remove_style_all(box);
   lv_obj_set_pos(box, 0, y);
   lv_obj_set_size(box, w, h);
   lv_obj_set_style_bg_color(box, lv_color_hex(TH_CARD), 0);
   lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
   lv_obj_set_style_radius(box, 14, 0);
   lv_obj_set_style_border_width(box, 1, 0);
   lv_obj_set_style_border_color(box, lv_color_hex(0x1E2030), 0);
   lv_obj_set_style_pad_all(box, pad, 0);
   lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
   for (int i = 0; i < lines; i++) {
      lv_obj_t *bar = lv_obj_create(box);
      lv_obj_remove_style_all(bar);
      /* First bar full width, rest taper for natural variation. */
      int bar_w = (i == 0) ? (w - pad * 2) : ((w - pad * 2) * (75 - i * 12) / 100);
      if (bar_w < 80) bar_w = 80;
      lv_obj_set_size(bar, bar_w, line_h);
      lv_obj_set_pos(bar, 0, i * (line_h + line_gap));
      lv_obj_set_style_bg_color(bar, lv_color_hex(0x222230), 0);
      lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
      lv_obj_set_style_radius(bar, 4, 0);
      lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
   }
   return box;
}

/* ─────────────────────────────────────────────────────────────────
 *  Error chip
 * ───────────────────────────────────────────────────────────────── */

lv_obj_t *ui_error_chip(lv_obj_t *parent, int x, int y, int w, const char *message, ui_topbar_cb_t retry_cb) {
   if (!message) message = "Error";
   lv_obj_t *chip = lv_obj_create(parent);
   if (!chip) return NULL;
   lv_obj_remove_style_all(chip);
   const int chip_h = 64;
   lv_obj_set_pos(chip, x, y);
   lv_obj_set_size(chip, w, chip_h);
   lv_obj_set_style_bg_color(chip, lv_color_hex(0x2A1A1A), 0);
   lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
   lv_obj_set_style_radius(chip, 14, 0);
   lv_obj_set_style_border_width(chip, 1, 0);
   lv_obj_set_style_border_color(chip, lv_color_hex(0xEF4444), 0);
   lv_obj_set_style_border_opa(chip, LV_OPA_COVER, 0);
   lv_obj_set_style_pad_left(chip, 16, 0);
   lv_obj_set_style_pad_right(chip, 12, 0);
   lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
   lv_obj_clear_flag(chip, LV_OBJ_FLAG_CLICKABLE);

   /* Danger icon — single warning glyph. */
   lv_obj_t *icon = lv_label_create(chip);
   lv_label_set_text(icon, LV_SYMBOL_WARNING);
   lv_obj_set_style_text_color(icon, lv_color_hex(0xEF4444), 0);
   lv_obj_set_style_text_font(icon, FONT_HEADING, 0);
   lv_obj_align(icon, LV_ALIGN_LEFT_MID, 0, 0);

   /* Message label — body weight, primary text color (red would be
    * shouting on red border; primary reads as info). */
   lv_obj_t *lbl = lv_label_create(chip);
   lv_label_set_text(lbl, message);
   lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
   lv_obj_set_style_text_font(lbl, FONT_BODY, 0);
   lv_obj_set_style_text_color(lbl, lv_color_hex(TH_TEXT_PRIMARY), 0);
   lv_obj_set_width(lbl, w - 48 - (retry_cb ? 100 : 0));
   lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 36, 0);

   /* Optional inline retry pill — fires retry_cb on tap. */
   if (retry_cb) {
      lv_obj_t *retry = lv_obj_create(chip);
      lv_obj_remove_style_all(retry);
      lv_obj_set_size(retry, 84, 36);
      lv_obj_align(retry, LV_ALIGN_RIGHT_MID, 0, 0);
      lv_obj_set_style_bg_color(retry, lv_color_hex(0xEF4444), 0);
      lv_obj_set_style_bg_opa(retry, LV_OPA_COVER, 0);
      lv_obj_set_style_radius(retry, 18, 0);
      lv_obj_add_flag(retry, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_clear_flag(retry, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_add_event_cb(retry, retry_cb, LV_EVENT_CLICKED, NULL);
      ui_fb_button(retry);
      lv_obj_t *rlbl = lv_label_create(retry);
      lv_label_set_text(rlbl, "RETRY");
      lv_obj_set_style_text_font(rlbl, FONT_BODY, 0);
      lv_obj_set_style_text_color(rlbl, lv_color_hex(0xFFFFFF), 0);
      lv_obj_center(rlbl);
   }

   return chip;
}
