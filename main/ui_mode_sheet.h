/*
 * ui_mode_sheet.h — v4·D Sovereign Halo triple-dial mode picker.
 *
 * Long-press home mode chip -> this fullscreen overlay opens.  User turns
 * three dials (Intelligence / Voice / Autonomy); taps resolve live into
 * voice_mode + llm_model via tab5_mode_resolve() and fire
 * voice_send_config_update() so Dragon stays in sync.
 *
 * Implementation uses lv_layer_top() — sits above home and any other
 * screens.  Tap "Done" or the backdrop scrim to dismiss.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Show the mode sheet. Idempotent: calling while already visible is a no-op. */
void ui_mode_sheet_show(void);

/* Hide (and destroy) the sheet. Idempotent. */
void ui_mode_sheet_hide(void);

/* Returns true if the sheet is currently visible. */
bool ui_mode_sheet_visible(void);

#ifdef __cplusplus
}
#endif
