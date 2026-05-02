/*
 * voice_widget_ws.h — WS widget-verb dispatch extracted from voice.c.
 *
 * 2026-05-03 SOLID audit (SRP-2 / P1, see docs/AUDIT-solid-2026-05-03.md):
 * voice.c's `handle_text_message()` is a 970-LOC if/else-if chain across
 * 25 WS verb classes.  The widget block (~270 LOC) is the cleanest first
 * extract — every widget handler writes through the already-stable
 * `widget_store_*` surface and ends with the same `ui_home_update_status`
 * async-call tail.  Pulling it into its own translation unit also
 * deletes six redundant `extern widget_store_upsert / widget_store_update
 * / widget_tone_from_str / ui_home_update_status` clusters that were
 * scattered through voice.c — pure DIP smell, since `widget.h` + `ui_home.h`
 * already expose the right contract.
 *
 * After this extract, `voice.c` is ~270 LOC lighter and `widget.h` is no
 * longer extern-declared at all from voice.c.
 *
 * Verbs owned by this module (all dispatch into widget_store + the home
 * status refresh):
 *   widget_card           — chat-bubble card with optional action button
 *   widget_live           — single live-slot upsert (orb tone, body, progress)
 *   widget_live_update    — partial in-place update of an existing live slot
 *   widget_list           — ranked list with up to 5 {text,value} rows
 *   widget_chart          — mini bar chart with up to 12 floats
 *   widget_media          — image + caption in the live slot
 *   widget_prompt         — multi-choice prompt (up to 3 buttons)
 *   widget_dismiss        — explicit dismiss by card_id
 *   widget_live_dismiss   — alias of widget_dismiss for compat
 *
 * Not in this module: `audio_clip` (chat audio playback), `card` (the
 * legacy rich-media card from non-widget LLM turns), and the binary
 * media/file frames.  Those stay in voice.c since they don't touch
 * widget_store.
 */

#pragma once

#include <stdbool.h>

/* Forward decl — keep this header self-sufficient without dragging in
 * cJSON.h on every consumer just for the prototype.  voice.c already
 * includes cJSON.h before calling us. */
struct cJSON;

/* Returns true if `type_str` matched a widget WS verb and was handled.
 * Returns false if not a widget verb — caller should fall through to
 * its remaining else-if chain (audio_clip, stt_*, llm_*, dictation_*,
 * tool_*, receipt, config_update, etc.).
 *
 * `type_str` MUST be non-NULL.  `root` MUST be the parsed cJSON object
 * for the WS frame; the caller retains ownership and is responsible for
 * `cJSON_Delete(root)` after dispatch returns. */
bool voice_widget_ws_dispatch(const char *type_str, struct cJSON *root);
