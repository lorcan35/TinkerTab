/**
 * TinkerTab Design System — Style initialization.
 *
 * Styles are static globals. Initialized once at boot via ui_theme_init().
 * Following LVGL best practice: shared static styles prevent per-object allocation.
 */

#include "ui_theme.h"
#include "esp_log.h"

static const char *TAG = "ui_theme";

/* ── Mode arrays ───────────────────────────────────────────── */
const char *th_mode_names[4] = {"Local", "Hybrid", "Cloud", "TinkerClaw"};
const uint32_t th_mode_colors[4] = {
    TH_MODE_LOCAL, TH_MODE_HYBRID, TH_MODE_CLOUD, TH_MODE_CLAW
};

/* ── Shared styles ─────────────────────────────────────────── */
lv_style_t s_style_card;
lv_style_t s_style_card_elevated;

static bool s_inited = false;

void ui_theme_init(void)
{
    if (s_inited) return;
    s_inited = true;

    /* Card surface — visible bg on dark background */
    lv_style_init(&s_style_card);
    lv_style_set_bg_color(&s_style_card, lv_color_hex(TH_CARD));
    lv_style_set_bg_opa(&s_style_card, LV_OPA_COVER);
    lv_style_set_radius(&s_style_card, TH_INFO_RADIUS);
    lv_style_set_pad_all(&s_style_card, TH_CARD_PAD);
    lv_style_set_border_width(&s_style_card, 1);
    lv_style_set_border_color(&s_style_card, lv_color_hex(TH_CARD_BORDER));

    /* Elevated card — agent card, modals */
    lv_style_init(&s_style_card_elevated);
    lv_style_set_bg_color(&s_style_card_elevated, lv_color_hex(TH_CARD_ELEVATED));
    lv_style_set_bg_opa(&s_style_card_elevated, LV_OPA_COVER);
    lv_style_set_radius(&s_style_card_elevated, TH_CARD_RADIUS);
    lv_style_set_pad_all(&s_style_card_elevated, TH_CARD_PAD);
    lv_style_set_border_width(&s_style_card_elevated, 1);
    lv_style_set_border_color(&s_style_card_elevated, lv_color_hex(TH_CARD_BORDER));

    ESP_LOGI(TAG, "Design system ready (2 styles, %d colors)", 16);
}
