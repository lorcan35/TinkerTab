/**
 * ui_chrome — implementation.  See header for rationale.
 */
#include "ui_chrome.h"

#include "config.h" /* DPI_SCALE / TOUCH_MIN */
#include "esp_log.h"
#include "ui_core.h"
#include "ui_feedback.h"
#include "ui_home.h"
#include "ui_theme.h"

static const char *TAG = "ui_chrome";

/* Bottom-right floating circle — out of the way of the chat keyboard
 * trigger (which lives bottom-left) and the chat input bar (centre). */
#define HOME_BTN_SZ DPI_SCALE(54)
#define HOME_BTN_MARGIN DPI_SCALE(20)

static lv_obj_t *s_home_btn = NULL;
static bool s_inited = false;

static void home_btn_click_cb(lv_event_t *e) {
   (void)e;
   /* Universal tap-debounce — the home button is reachable from every
    * non-home screen, so a slammed double-tap during a navigation
    * animation could otherwise stack two screen-loads. */
   if (!ui_tap_gate("chrome:home", 300)) return;
   ESP_LOGI(TAG, "persistent home -> ui_home_get_screen");
   lv_obj_t *home = ui_home_get_screen();
   if (home) lv_screen_load(home);
   /* Hide ourselves — home doesn't want the persistent button (the
    * mode chip, say-pill, and 4-dot menu chip already cover navigation
    * intent on home). */
   ui_chrome_set_home_visible(false);
}

void ui_chrome_init(void) {
   if (s_inited) return;

   s_home_btn = lv_obj_create(lv_layer_top());
   if (!s_home_btn) {
      ESP_LOGE(TAG, "ui_chrome: home button alloc failed");
      return;
   }
   lv_obj_remove_style_all(s_home_btn);
   lv_obj_set_size(s_home_btn, HOME_BTN_SZ, HOME_BTN_SZ);
   lv_obj_align(s_home_btn, LV_ALIGN_BOTTOM_RIGHT, -HOME_BTN_MARGIN, -HOME_BTN_MARGIN);
   lv_obj_set_style_radius(s_home_btn, LV_RADIUS_CIRCLE, 0);
   lv_obj_set_style_bg_color(s_home_btn, lv_color_hex(TH_CARD_ELEVATED), 0);
   lv_obj_set_style_bg_opa(s_home_btn, LV_OPA_COVER, 0);
   lv_obj_set_style_border_width(s_home_btn, 1, 0);
   lv_obj_set_style_border_color(s_home_btn, lv_color_hex(TH_AMBER), 0);
   lv_obj_clear_flag(s_home_btn, LV_OBJ_FLAG_SCROLLABLE);
   lv_obj_add_flag(s_home_btn, LV_OBJ_FLAG_CLICKABLE);
   lv_obj_add_event_cb(s_home_btn, home_btn_click_cb, LV_EVENT_CLICKED, NULL);
   ui_fb_button(s_home_btn);

   /* House-glyph (LV_SYMBOL_HOME) centred. */
   lv_obj_t *icon = lv_label_create(s_home_btn);
   lv_label_set_text(icon, LV_SYMBOL_HOME);
   lv_obj_set_style_text_font(icon, FONT_HEADING, 0);
   lv_obj_set_style_text_color(icon, lv_color_hex(TH_AMBER), 0);
   lv_obj_center(icon);

   /* Hidden by default; screens that want the escape hatch toggle on. */
   lv_obj_add_flag(s_home_btn, LV_OBJ_FLAG_HIDDEN);
   s_inited = true;
   ESP_LOGI(TAG, "ui_chrome ready (home button hidden until screen requests it)");
}

void ui_chrome_set_home_visible(bool visible) {
   if (!s_inited || !s_home_btn) return;
   if (visible) {
      lv_obj_clear_flag(s_home_btn, LV_OBJ_FLAG_HIDDEN);
      lv_obj_move_foreground(s_home_btn);
   } else {
      lv_obj_add_flag(s_home_btn, LV_OBJ_FLAG_HIDDEN);
   }
}
