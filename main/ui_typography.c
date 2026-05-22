/**
 * @file ui_typography.c
 * @brief Implementation — see ui_typography.h.
 */

#include "ui_typography.h"

#include "esp_log.h"
#include "settings.h"

static const char *TAG = "ui_typo";

/* Lookup table: 10 roles × 4 tiers → font pointer.
 *
 * Picks below clamp at the nearest available compiled Montserrat
 * size from sdkconfig (12/14/16/18/20/24/28/36/48).  Where a tier's
 * mathematical scale lands between two compiled sizes, we round to
 * the nearer one biased toward the larger so legibility wins.
 *
 * Per-role notes:
 *   CLOCK 48px is already the largest compiled Montserrat — tiers 2/3
 *   stay at 48 since there's no larger.  Tier 0 drops to 36 (next
 *   available below 48).
 *   SMALL 14px tier 0 drops to 12 (smallest sensible body size).
 *   TITLE 28px upscale jumps to 36 (no 32 compiled). */
static const lv_font_t *s_table[FONT_ROLE_COUNT][4] = {
    [FONT_ROLE_TITLE] = {&lv_font_montserrat_24, &lv_font_montserrat_28, &lv_font_montserrat_36,
                         &lv_font_montserrat_36},
    [FONT_ROLE_HEADING] = {&lv_font_montserrat_20, &lv_font_montserrat_24, &lv_font_montserrat_28,
                           &lv_font_montserrat_28},
    [FONT_ROLE_BODY] = {&lv_font_montserrat_18, &lv_font_montserrat_20, &lv_font_montserrat_24, &lv_font_montserrat_28},
    [FONT_ROLE_SECONDARY] = {&lv_font_montserrat_16, &lv_font_montserrat_18, &lv_font_montserrat_20,
                             &lv_font_montserrat_24},
    [FONT_ROLE_CAPTION] = {&lv_font_montserrat_14, &lv_font_montserrat_16, &lv_font_montserrat_18,
                           &lv_font_montserrat_20},
    [FONT_ROLE_SMALL] = {&lv_font_montserrat_12, &lv_font_montserrat_14, &lv_font_montserrat_16,
                         &lv_font_montserrat_18},
    [FONT_ROLE_CLOCK] = {&lv_font_montserrat_36, &lv_font_montserrat_48, &lv_font_montserrat_48,
                         &lv_font_montserrat_48},
    [FONT_ROLE_DATE] = {&lv_font_montserrat_20, &lv_font_montserrat_24, &lv_font_montserrat_28, &lv_font_montserrat_28},
    [FONT_ROLE_KEY] = {&lv_font_montserrat_18, &lv_font_montserrat_20, &lv_font_montserrat_24, &lv_font_montserrat_28},
    [FONT_ROLE_NAV] = {&lv_font_montserrat_16, &lv_font_montserrat_18, &lv_font_montserrat_20, &lv_font_montserrat_24},
};

/* Cached pointers for the session — one per role, picked at boot from
 * the active font_scale tier.  Faster than indexing the table on
 * every FONT_* macro expansion (~1500 hits during a full UI render). */
static const lv_font_t *s_cache[FONT_ROLE_COUNT] = {0};
static bool s_initialized = false;

void tab5_typography_init(void) {
   uint8_t tier = tab5_settings_get_font_scale();
   if (tier > 3) tier = 1;
   for (int r = 0; r < FONT_ROLE_COUNT; r++) {
      s_cache[r] = s_table[r][tier];
   }
   s_initialized = true;
   ESP_LOGI(TAG, "typography initialized — tier %u", (unsigned)tier);
}

const lv_font_t *tab5_typography_get(font_role_t role) {
   if (role < 0 || role >= FONT_ROLE_COUNT) {
      return &lv_font_montserrat_20;
   }
   /* Defensive: if a UI path runs before tab5_typography_init (boot
    * order regression), fall back to the default-tier pointer. */
   if (!s_initialized) {
      return s_table[role][1];
   }
   return s_cache[role];
}
