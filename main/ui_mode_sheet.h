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

/* Show the Agent-mode consent modal standalone — for callers outside the
 * mode sheet (e.g. the Settings TinkerClaw row). On Switch: on_confirm is
 * invoked. On Keep Ask / scrim-dismiss: on_cancel is invoked (may be NULL
 * if the caller has nothing to revert). Audit E3 (2026-04-20). */
void ui_agent_consent_show(void (*on_confirm)(void *ctx),
                           void (*on_cancel)(void *ctx),
                           void *ctx);

#ifdef __cplusplus
}
#endif
