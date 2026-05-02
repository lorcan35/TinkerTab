/*
 * voice_billing.h — receipt + budget + cap-downgrade module.
 *
 * 2026-05-03 SOLID audit (SRP-3 / P1, see docs/AUDIT-solid-2026-05-03.md):
 * voice.c's `handle_text_message()` had 110 LOC of receipt-handling logic
 * inside the WS verb dispatch — token accounting, NVS budget accumulate,
 * UI refresh hops, and the cap-triggered auto-downgrade rule, all
 * tangled with the WS frame parser.  Extra ~40 LOC of
 * `voice_defer_receipt_attach` machinery + last_receipt_* statics
 * compounded the SRP violation.
 *
 * Billing policy (cap-triggered mode downgrade, "Hybrid+Cloud → Local;
 * Agent unchanged") is a different axis of change than "Dragon sent
 * me a receipt frame" — extending to "warn at 80% before downgrade"
 * shouldn't force an edit deep inside the WS dispatcher.  Pulling
 * the chain into voice_billing.{c,h} gives policy + receipt rendering
 * a single home.
 *
 * Endpoints owned by this module:
 *   * `voice_billing_record_receipt` — entry point called from voice.c's
 *     receipt-verb branch.  Owns NVS write, UI refresh, cap-policy.
 *   * `voice_billing_defer_receipt_attach_async` — async LVGL-thread
 *     hop for the chat-bubble receipt stamp.  Stays public because
 *     other receipt-emitting paths (TinkerClaw bypass, possible
 *     future skill-emitted receipts) may need to call it directly.
 *
 * The `s_last_receipt_*` statics in the pre-extract voice.c were
 * write-only — never read from anywhere — so they are NOT preserved
 * here.  If a future caller needs the cached receipt, add a public
 * `voice_billing_get_last_receipt(...)` accessor.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Forward decl — keep this header self-sufficient without dragging in
 * cJSON.h on every consumer just for the prototype. */
struct cJSON;

/* Process a "receipt" WS frame from Dragon.  Owns the full chain:
 *   - log the receipt to ESP_LOGI
 *   - if cost_mils > 0: accumulate into the daily NVS budget
 *   - nudge home + chat UI to re-render their spend chips
 *   - if spent >= cap AND voice_mode is Hybrid (1) or Cloud (2):
 *     auto-downgrade to Local (0), notify Dragon via
 *     voice_send_config_update_ex(0, llm, "cap_downgrade"), reset the
 *     int_tier + voi_tier dials, surface a "Budget cap hit" toast
 *   - regardless of cost: defer-attach a per-bubble receipt stamp via
 *     voice_billing_defer_receipt_attach_async() so chat_msg_view can
 *     render "claude-haiku · $0.0042" (or "local · FREE") below the
 *     last assistant bubble.
 *
 * Caller (voice.c WS dispatcher) parses the JSON fields out of `root`
 * and hands them in typed.  This keeps cJSON dependency on the WS
 * parser side and lets the billing module stay testable without a
 * cJSON mock.
 */
void voice_billing_record_receipt(const char *model_id, int prompt_tok, int compl_tok, int cost_mils, bool retried);

/* Async LVGL-thread hop for the per-bubble receipt stamp.  Pre-extract
 * this lived in voice.c as `voice_defer_receipt_attach`; same shape,
 * same semantics — just in its proper home now.
 *
 * Dragon's `llm_done` path enqueues `ui_chat_push_message("assistant",
 * ...)` via lv_async_call ~ms before the WS rx task receives the
 * receipt.  If this attached synchronously on the rx task, the
 * assistant bubble wouldn't yet be in chat_store and the stamp would
 * silently drop.  Queuing via lv_async_call after the push preserves
 * FIFO order — push lands first, attach lands second — both on the
 * LVGL thread. */
void voice_billing_defer_receipt_attach_async(uint32_t mils, uint16_t prompt_tok, uint16_t compl_tok,
                                              const char *model_short, bool retried);
