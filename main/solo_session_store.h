/**
 * solo_session_store — SD-backed solo-mode session persistence.
 *
 * One JSON file per session at /sdcard/sessions/<id>.json.  Format:
 *   {"id":"...","created":<unix>,"turns":[
 *      {"role":"user","content":"...","ts":<unix>},
 *      {"role":"assistant","content":"...","ts":<unix>},
 *      ...max SOLO_SESSION_MAX_TURNS turns
 *   ]}
 *
 * On overflow, oldest turn is dropped (FIFO).  Sessions older than 30
 * days are NOT auto-pruned in v1 — manual cleanup via /sdcard.
 *
 * The active session id is persisted in NVS as "solo_sid" so it
 * survives reboots; solo_session_open re-uses or creates one.
 *
 * TT #370 — see docs/superpowers/specs/2026-05-08-tab5-solo-mode-design.md
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SOLO_SESSION_MAX_TURNS 50

esp_err_t solo_session_init(void); /* mkdir /sdcard/sessions */

/** Open or create the active session.  ID auto-generated from
 *  esp_random; persisted in NVS.  Pass NULL/0 if you don't need
 *  the id back. */
esp_err_t solo_session_open(char *out_id, size_t cap);

/** Append a turn.  Called twice per turn (user, then assistant). */
esp_err_t solo_session_append(const char *role, const char *content);

/** Load up to N most-recent turns into out_json (caller-owned).
 *  Output is a JSON array `[{"role":...,"content":...},...]` ready
 *  to drop into an OpenRouter chat-completions messages field.  On
 *  empty/missing session writes "[]". */
esp_err_t solo_session_load_recent(int n, char *out_json, size_t cap);

/** Force a new session (e.g., user taps "New Chat"). */
esp_err_t solo_session_rotate(void);

#ifdef __cplusplus
}
#endif
