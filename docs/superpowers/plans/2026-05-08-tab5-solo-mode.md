# Tab5 Solo Mode (vmode=5 SOLO_DIRECT) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a Dragon-independent voice mode (`vmode=5`) that makes Tab5 talk directly to OpenRouter for STT, LLM streaming, TTS, and embeddings — with QR-scan API key entry, on-device sessions/RAG, and a mode pill that taps to cycle K144 (`vmode=4`) ↔ SOLO (`vmode=5`).

**Architecture:** Six new ESP-IDF modules under `main/` (`voice_solo` orchestrator, `openrouter_client` + `openrouter_sse`, `solo_session_store`, `solo_rag`, `qr_decoder` + vendored quirc) plus targeted edits to `voice.h`, `voice_modes.{c,h}`, `voice.c`, `ui_settings.c`, `ui_camera.{c,h}`, `settings.{c,h}`, `debug_server_settings.c`. The solo state machine reuses existing `IDLE/LISTENING/PROCESSING/SPEAKING` states across three serialized OpenRouter HTTPS calls. All large buffers in PSRAM per `CLAUDE.md` rules. Test-loop is the existing Python E2E harness at `tests/e2e/runner.py` driving the debug HTTP server, plus synthetic test endpoints in a new `debug_server_solo.c` module for deterministic unit-style tests of pure-logic modules (SSE parser, RAG cosine).

**Tech Stack:** ESP-IDF 5.5.2 · LVGL 9.2.2 · `esp_http_client` over TLS via the bundled cert store (covers Cloudflare/DigiCert chain that openrouter.ai serves) · `cJSON` (PSRAM-backed via existing allocator hooks) · `esp_codec_dev` (existing playback/mic) · vendored quirc 1.2 (ISC) · existing `media_cache.c` chunked-HTTP idiom · `tests/e2e/runner.py` Python harness for verification

**Spec source of truth:** [`docs/superpowers/specs/2026-05-08-tab5-solo-mode-design.md`](../specs/2026-05-08-tab5-solo-mode-design.md)

**Working branch:** Create `feat/solo-mode` from current `docs/dragon-ip-refresh` (the spec's commit lives there). One PR per task, squash-merge to `main`.

**Issue:** Open `gh issue create -t "Tab5 Solo Mode (vmode=5)" -b "Phase 1 of docs/superpowers/specs/2026-05-08-tab5-solo-mode-design.md"` before Task 1; reference its number in every commit (`refs #N`).

---

## Pre-flight (one-time, before Task 1)

- [ ] **Branch + issue**

```bash
cd ~/projects/TinkerTab
. /home/rebelforce/esp/esp-idf/export.sh
git fetch origin
git checkout docs/dragon-ip-refresh
git pull origin docs/dragon-ip-refresh
ISSUE=$(gh issue create --title "Tab5 Solo Mode (vmode=5 SOLO_DIRECT)" \
  --body "Phase 1 of docs/superpowers/specs/2026-05-08-tab5-solo-mode-design.md.\nTracking issue for the 19 implementation tasks in docs/superpowers/plans/2026-05-08-tab5-solo-mode.md." \
  --label enhancement | tail -1 | sed 's|.*/||')
echo "ISSUE=$ISSUE" > /tmp/solo_mode_issue
git checkout -b feat/solo-mode-${ISSUE}
```

Expected: new branch `feat/solo-mode-<N>`, issue link printed.

---

## File structure (locked at start, deviations require plan update)

**New (Phase 1):**

| Path | Responsibility | LOC | Owner of |
|------|----------------|-----|----------|
| `main/voice_solo.{c,h}` | Solo mode orchestrator: state machine, STT→LLM→TTS chain, mic-buffer ingest, chat/session/RAG wiring | ≤600 | `voice_solo_send_text`, `voice_solo_send_audio`, `voice_solo_cancel`, mode init/teardown |
| `main/openrouter_client.{c,h}` | HTTP wrapper for 4 verbs: STT (multipart), LLM (SSE chat-completions), TTS (chunked WAV), embeddings (JSON). Handles auth, base URL, error mapping. | ≤400 | `openrouter_stt`, `openrouter_chat_stream`, `openrouter_tts`, `openrouter_embed`, `openrouter_cancel_inflight` |
| `main/openrouter_sse.{c,h}` | Line-buffered SSE frame parser: extracts `data: {...}` payloads, terminates on `[DONE]`, ignores comment lines | ≤80 | `openrouter_sse_init`, `openrouter_sse_feed` |
| `main/solo_session_store.{c,h}` | SD-backed session persistence: per-session JSON file at `/sdcard/sessions/<id>.json`, max 50 turns, list/load/append/rotate | ≤250 | `solo_session_open`, `solo_session_append`, `solo_session_load_recent`, `solo_session_close` |
| `main/solo_rag.{c,h}` | Flat float32 vector store at `/sdcard/rag.bin` + brute-force cosine top-K | ≤300 | `solo_rag_remember`, `solo_rag_recall`, `solo_rag_count` |
| `main/qr_decoder.{c,h}` | quirc wrapper: grayscale frame → QR payload string | ≤150 | `qr_decoder_init`, `qr_decoder_decode_frame`, `qr_decoder_free` |
| `main/quirc/{quirc.h, quirc.c, decode.c, identify.c, version_db.c}` | Vendored ISC quirc 1.2 (no upstream changes, see `https://github.com/dlbeer/quirc`) | ~25KB | (third-party) |
| `main/debug_server_solo.c` | Synthetic test endpoints used by E2E + plan tasks (`POST /solo/sse_test`, `POST /solo/rag_test`, `POST /solo/llm_test`) | ≤200 | endpoint registration, calls into the modules above |
| `tests/e2e/scenarios/story_solo.py` | E2E story scenario `story_solo` exercising vmode=5 end-to-end | ≤180 | Python steps |

**Modified:**

| Path | Change |
|------|--------|
| `main/voice.h` | Add `#define VMODE_SOLO_DIRECT 5`; bump comment "0..4" → "0..5" on `vmode` NVS doc |
| `main/voice_modes.{c,h}` | Add `VOICE_MODES_ROUTE_SOLO_*` enum values; route_text dispatcher: vmode==5 → `voice_solo_send_text` |
| `main/voice.c` | Mode-pill cycle wraps K144↔SOLO when current is one of those; mic-stop hook: when vmode==5, hand the captured PCM buffer to `voice_solo_send_audio` instead of WS |
| `main/ui_settings.c` | "Cloud Direct" section with API key field (masked, "Scan QR" button) + 4 model fields + voice |
| `main/ui_camera.{c,h}` | Add a QR scan submode that decodes camera frames and calls back with payload |
| `main/settings.{c,h}` | Add NVS getters/setters for `or_key`, `or_mdl_llm`, `or_mdl_stt`, `or_mdl_tts`, `or_mdl_emb`, `or_voice` |
| `main/debug_server_settings.c` | Add new keys to GET response + accept them on POST |
| `main/debug_server.c` + `main/debug_server.h` | Register `tab5_debug_solo_register(httpd_handle_t)` in the boot path |
| `main/CMakeLists.txt` | List new sources |
| `main/idf_component.yml` | (no change — quirc is vendored, not a managed component) |
| `tests/e2e/runner.py` | Register `story_solo` scenario |
| `tests/e2e/audit_tinkerclaw.py` | Add a "Section 7: solo mode" smoke block |

---

## Tech reminders for the engineer

1. **PSRAM rule:** every buffer >4 KB allocated with `heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)`, freed with `heap_caps_free`. Never `malloc`/`free`. Static BSS arrays >3 KB push Tab5 over a boot SRAM threshold (see `LEARNINGS.md`).
2. **No `lv_async_call` direct:** always wrap as `tab5_lv_async_call(cb, arg)` (`ui_core.h`).
3. **No `vTaskDelete(NULL)`:** use `vTaskSuspend(NULL)` for self-terminating tasks (TLSP cleanup crash on P4).
4. **HTTP cert bundle** is initialized in `service_network.c` — `openrouter_client.c` only needs to set `.cert_pem = NULL` and `.crt_bundle_attach = esp_crt_bundle_attach` on its `esp_http_client_config_t`.
5. **clang-format** runs in CI (`.github/workflows/lint.yml`) with `--diff origin/main`. Run `git-clang-format --binary clang-format-18 --diff origin/main main/*.c main/*.h` before every push; CI gates on a clean diff.
6. **Issue refs in every commit message:** `feat: <description> (refs #N)`.

---

## Tasks

### Task 1: Add NVS keys for OpenRouter

**Files:**
- Modify: `main/settings.h` (add 6 getter/setter prototypes)
- Modify: `main/settings.c` (add 6 typed wrappers)
- Modify: `main/debug_server_settings.c` (surface keys on `GET /settings` and accept them on `POST /settings`)

- [ ] **Step 1: Write the E2E test (failing)**

Create file `tests/e2e/scenarios/test_solo_nvs.py`:

```python
"""Solo-mode NVS keys round-trip via /settings."""
from tests.e2e.driver import Tab5Driver

def test_solo_nvs_roundtrip(tab5: Tab5Driver) -> None:
    tab5.post_settings({
        "or_key": "sk-or-v1-testkey",
        "or_mdl_llm": "~anthropic/claude-haiku-latest",
        "or_mdl_stt": "whisper-1",
        "or_mdl_tts": "tts-1",
        "or_mdl_emb": "text-embedding-3-small",
        "or_voice": "alloy",
    })
    s = tab5.get_settings()
    assert s["or_key"] == "sk-or-v1-testkey"
    assert s["or_mdl_llm"] == "~anthropic/claude-haiku-latest"
    assert s["or_mdl_stt"] == "whisper-1"
    assert s["or_mdl_tts"] == "tts-1"
    assert s["or_mdl_emb"] == "text-embedding-3-small"
    assert s["or_voice"] == "alloy"
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cd ~/projects/TinkerTab
TAB5_TOKEN=$(grep '^TAB5_TOKEN=' .env | cut -d= -f2) \
TAB5_URL=http://192.168.70.128:8080 \
python3 -m pytest tests/e2e/scenarios/test_solo_nvs.py -v
```

Expected: KeyError on `s["or_key"]` — the key isn't in /settings yet.

- [ ] **Step 3: Add prototypes in `main/settings.h`**

Insert after the `dragon_tok` setter (line ~57 in current head, or wherever `tab5_settings_set_dragon_tok` ends):

```c
/* ── OpenRouter (solo mode, vmode=5) ──────────────────────────────────── */

esp_err_t tab5_settings_get_or_key(char *buf, size_t len);
esp_err_t tab5_settings_set_or_key(const char *key);

esp_err_t tab5_settings_get_or_mdl_llm(char *buf, size_t len);
esp_err_t tab5_settings_set_or_mdl_llm(const char *model);

esp_err_t tab5_settings_get_or_mdl_stt(char *buf, size_t len);
esp_err_t tab5_settings_set_or_mdl_stt(const char *model);

esp_err_t tab5_settings_get_or_mdl_tts(char *buf, size_t len);
esp_err_t tab5_settings_set_or_mdl_tts(const char *model);

esp_err_t tab5_settings_get_or_mdl_emb(char *buf, size_t len);
esp_err_t tab5_settings_set_or_mdl_emb(const char *model);

esp_err_t tab5_settings_get_or_voice(char *buf, size_t len);
esp_err_t tab5_settings_set_or_voice(const char *voice);
```

- [ ] **Step 4: Add implementations in `main/settings.c`**

Append at end of file (above any closing `#ifdef __cplusplus`):

```c
/* ── OpenRouter (solo mode, vmode=5) ───────────────────────────────────
 *
 * Defaults match docs/superpowers/specs/2026-05-08-tab5-solo-mode-design.md.
 * Empty key disables solo mode at runtime; voice_solo_send_text returns
 * ESP_ERR_INVALID_STATE so the UI can prompt for a QR scan. */

esp_err_t tab5_settings_get_or_key(char *buf, size_t len) {
    return get_str("or_key", buf, len, "");
}
esp_err_t tab5_settings_set_or_key(const char *key) {
    return set_str("or_key", key);
}

esp_err_t tab5_settings_get_or_mdl_llm(char *buf, size_t len) {
    return get_str("or_mdl_llm", buf, len, "~anthropic/claude-haiku-latest");
}
esp_err_t tab5_settings_set_or_mdl_llm(const char *m) {
    return set_str("or_mdl_llm", m);
}

esp_err_t tab5_settings_get_or_mdl_stt(char *buf, size_t len) {
    return get_str("or_mdl_stt", buf, len, "whisper-1");
}
esp_err_t tab5_settings_set_or_mdl_stt(const char *m) {
    return set_str("or_mdl_stt", m);
}

esp_err_t tab5_settings_get_or_mdl_tts(char *buf, size_t len) {
    return get_str("or_mdl_tts", buf, len, "tts-1");
}
esp_err_t tab5_settings_set_or_mdl_tts(const char *m) {
    return set_str("or_mdl_tts", m);
}

esp_err_t tab5_settings_get_or_mdl_emb(char *buf, size_t len) {
    return get_str("or_mdl_emb", buf, len, "text-embedding-3-small");
}
esp_err_t tab5_settings_set_or_mdl_emb(const char *m) {
    return set_str("or_mdl_emb", m);
}

esp_err_t tab5_settings_get_or_voice(char *buf, size_t len) {
    return get_str("or_voice", buf, len, "alloy");
}
esp_err_t tab5_settings_set_or_voice(const char *v) {
    return set_str("or_voice", v);
}
```

(`get_str` and `set_str` are existing static helpers in `settings.c` — verify by `grep -n "^static.*get_str\|^static.*set_str" main/settings.c`. If they're named `settings_get_str` / `settings_set_str`, adapt the calls.)

- [ ] **Step 5: Surface keys in `debug_server_settings.c`**

Find the function that builds the `GET /settings` JSON response (search `cJSON_CreateObject` near `dragon_tok`) and add 6 lines:

```c
    char or_key[96] = {0}; tab5_settings_get_or_key(or_key, sizeof or_key);
    cJSON_AddStringToObject(root, "or_key", or_key);
    char or_llm[96] = {0}; tab5_settings_get_or_mdl_llm(or_llm, sizeof or_llm);
    cJSON_AddStringToObject(root, "or_mdl_llm", or_llm);
    char or_stt[64] = {0}; tab5_settings_get_or_mdl_stt(or_stt, sizeof or_stt);
    cJSON_AddStringToObject(root, "or_mdl_stt", or_stt);
    char or_tts[64] = {0}; tab5_settings_get_or_mdl_tts(or_tts, sizeof or_tts);
    cJSON_AddStringToObject(root, "or_mdl_tts", or_tts);
    char or_emb[64] = {0}; tab5_settings_get_or_mdl_emb(or_emb, sizeof or_emb);
    cJSON_AddStringToObject(root, "or_mdl_emb", or_emb);
    char or_voice[32] = {0}; tab5_settings_get_or_voice(or_voice, sizeof or_voice);
    cJSON_AddStringToObject(root, "or_voice", or_voice);
```

In the POST handler (search `cJSON_GetObjectItem(root, "dragon_tok")`), add 6 sibling blocks:

```c
    cJSON *or_key = cJSON_GetObjectItem(root, "or_key");
    if (cJSON_IsString(or_key)) tab5_settings_set_or_key(or_key->valuestring);
    cJSON *or_llm = cJSON_GetObjectItem(root, "or_mdl_llm");
    if (cJSON_IsString(or_llm)) tab5_settings_set_or_mdl_llm(or_llm->valuestring);
    cJSON *or_stt = cJSON_GetObjectItem(root, "or_mdl_stt");
    if (cJSON_IsString(or_stt)) tab5_settings_set_or_mdl_stt(or_stt->valuestring);
    cJSON *or_tts = cJSON_GetObjectItem(root, "or_mdl_tts");
    if (cJSON_IsString(or_tts)) tab5_settings_set_or_mdl_tts(or_tts->valuestring);
    cJSON *or_emb = cJSON_GetObjectItem(root, "or_mdl_emb");
    if (cJSON_IsString(or_emb)) tab5_settings_set_or_mdl_emb(or_emb->valuestring);
    cJSON *or_voice = cJSON_GetObjectItem(root, "or_voice");
    if (cJSON_IsString(or_voice)) tab5_settings_set_or_voice(or_voice->valuestring);
```

Add `#include "settings.h"` at the top if not already present.

- [ ] **Step 6: Build + flash**

```bash
cd ~/projects/TinkerTab
. /home/rebelforce/esp/esp-idf/export.sh
idf.py build
idf.py -p /dev/ttyACM0 flash
```

Expected: `Generated /home/rebelforce/projects/TinkerTab/build/tinkertab.bin`, then `Hash of data verified.` then `Hard resetting via RTS pin...`. Wait ~10s for the device to come up, then check serial: `I (xxx) settings: NVS namespace 'settings' opened`.

- [ ] **Step 7: Run test to verify it passes**

```bash
TAB5_TOKEN=$(grep '^TAB5_TOKEN=' .env | cut -d= -f2) \
TAB5_URL=http://192.168.70.128:8080 \
python3 -m pytest tests/e2e/scenarios/test_solo_nvs.py -v
```

Expected: PASS.

- [ ] **Step 8: clang-format**

```bash
git fetch origin main:refs/remotes/origin/main
git-clang-format --binary clang-format-18 --diff origin/main main/settings.c main/settings.h main/debug_server_settings.c
```

Expected: empty output. If there's output, run again without `--diff` to autofix.

- [ ] **Step 9: Commit**

```bash
ISSUE=$(grep -oE '[0-9]+' /tmp/solo_mode_issue)
git add main/settings.h main/settings.c main/debug_server_settings.c \
        tests/e2e/scenarios/test_solo_nvs.py
git commit -m "feat(settings): add OpenRouter NVS keys for solo mode (refs #${ISSUE})"
git push -u origin HEAD
```

---

### Task 2: Add `VMODE_SOLO_DIRECT` constant + voice_modes route enum

**Files:**
- Modify: `main/voice.h` (add `#define VMODE_SOLO_DIRECT 5`)
- Modify: `main/voice_modes.h` (add `VOICE_MODES_ROUTE_SOLO_*` enum values)
- Modify: `main/voice_modes.c` (handle vmode==5 in `voice_modes_route_text` — stub to a `not_implemented` route for now)

- [ ] **Step 1: Add `VMODE_SOLO_DIRECT` in `main/voice.h`**

After the existing `#define VMODE_LOCAL_ONBOARD 4` line (around line 122):

```c
#define VMODE_SOLO_DIRECT 5   /* OpenRouter direct, no Dragon — see voice_solo.{c,h} */
```

Update the doc comment block above (currently says "Existing 0..3"): change to "Existing 0..5". Update the `POST /settings` example to mention 5.

- [ ] **Step 2: Add route enum entries in `main/voice_modes.h`**

After `VOICE_MODES_ROUTE_DRAGON_PATH` line in the `voice_modes_route_kind_t` enum:

```c
   /* vmode=5 SOLO_DIRECT — caller should send to voice_solo_send_text. */
   VOICE_MODES_ROUTE_SOLO_OK,
   /* vmode=5 selected but OpenRouter API key empty — caller surfaces
    * "Scan QR to set API key" toast and refuses. */
   VOICE_MODES_ROUTE_SOLO_NO_KEY,
   /* Routed to voice_solo but voice_solo returned an error — caller
    * surfaces toast (failure detail in `err`). */
   VOICE_MODES_ROUTE_SOLO_FAILED,
```

- [ ] **Step 3: Stub the SOLO branch in `main/voice_modes.c`**

In `voice_modes_route_text`, just before the final `return (voice_modes_route_result_t){.kind = VOICE_MODES_ROUTE_DRAGON_PATH}`, insert:

```c
    if (vmode == VMODE_SOLO_DIRECT) {
        char or_key[96] = {0};
        tab5_settings_get_or_key(or_key, sizeof or_key);
        if (or_key[0] == '\0') {
            return (voice_modes_route_result_t){
                .kind = VOICE_MODES_ROUTE_SOLO_NO_KEY,
                .err = ESP_ERR_INVALID_STATE,
            };
        }
        /* Task 12 wires this in.  For now we still fall through to the
         * Dragon path so we never silently drop a turn. */
        ESP_LOGW("voice_modes", "SOLO route stub — falling through to Dragon");
    }
```

Add `#include "settings.h"` at the top if missing.

- [ ] **Step 4: Build**

```bash
idf.py build
```

Expected: clean build. No `unused variable` warnings.

- [ ] **Step 5: Verify on device**

```bash
idf.py -p /dev/ttyACM0 flash
TAB5_TOKEN=$(grep '^TAB5_TOKEN=' .env | cut -d= -f2)
curl -sS -H "Authorization: Bearer $TAB5_TOKEN" -X POST 'http://192.168.70.128:8080/mode?m=5'
curl -sS -H "Authorization: Bearer $TAB5_TOKEN" 'http://192.168.70.128:8080/voice' | python3 -m json.tool
```

Expected: `/mode?m=5` returns `{"ok":true}`-ish (mode persisted), `/voice` shows `mode_id: 5`. NVS read-back via `/settings | jq '.voice_mode'` returns 5.

- [ ] **Step 6: clang-format + commit**

```bash
git-clang-format --binary clang-format-18 --diff origin/main main/voice.h main/voice_modes.h main/voice_modes.c
ISSUE=$(grep -oE '[0-9]+' /tmp/solo_mode_issue)
git add main/voice.h main/voice_modes.h main/voice_modes.c
git commit -m "feat(voice): add VMODE_SOLO_DIRECT=5 and route enum (refs #${ISSUE})"
git push
```

---

### Task 3: `voice_solo` skeleton (header, types, init/teardown, stub send)

**Files:**
- Create: `main/voice_solo.h`
- Create: `main/voice_solo.c`
- Modify: `main/CMakeLists.txt` (add new source)

- [ ] **Step 1: Create `main/voice_solo.h`**

```c
/**
 * voice_solo — Tab5 solo (Dragon-independent) voice orchestrator.
 *
 * Runs the vmode=5 SOLO_DIRECT pipeline: mic → OpenRouter STT →
 * OpenRouter chat-completions (SSE) → OpenRouter TTS → speaker.
 *
 * The state machine reuses voice.h's IDLE/LISTENING/PROCESSING/SPEAKING
 * (set via voice_set_state) so the existing UI orb + chat overlay
 * "just work" without an alternate state enum.
 *
 * Public surface is intentionally tiny — voice.c calls these from the
 * mic-stop hook (audio path) and voice_modes.c calls voice_solo_send_text
 * (text path).  Cancellation is global: voice_solo_cancel aborts any
 * in-flight HTTP via openrouter_cancel_inflight.
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** One-time init; safe to call multiple times.  Allocates PSRAM ring
 *  buffers + opens the SD session store.  Returns ESP_OK or first
 *  failing init step. */
esp_err_t voice_solo_init(void);

/** Send a text turn.  Synchronous from caller's perspective up to LLM
 *  TTFT; the SSE callback streams reply chunks into the chat UI; TTS
 *  + playback happens on a worker.  Caller is voice_modes_route_text. */
esp_err_t voice_solo_send_text(const char *text);

/** Hand a captured mic buffer (PCM-16LE @ 16 kHz mono) for STT → LLM
 *  → TTS.  Caller (voice.c mic-stop hook) loses ownership; voice_solo
 *  frees with heap_caps_free when done. */
esp_err_t voice_solo_send_audio(int16_t *pcm, size_t samples);

/** Abort any in-flight HTTP + flush playback ring.  Safe from any
 *  task; idempotent. */
void voice_solo_cancel(void);

/** True iff a turn is currently in PROCESSING or SPEAKING. */
bool voice_solo_busy(void);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: Create `main/voice_solo.c` (stub)**

```c
#include "voice_solo.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "voice.h"

static const char *TAG = "voice_solo";

static bool s_initialized = false;
static volatile bool s_busy = false;

esp_err_t voice_solo_init(void) {
    if (s_initialized) return ESP_OK;
    /* Task 14 fills this in (open session store, alloc ring). */
    s_initialized = true;
    ESP_LOGI(TAG, "voice_solo_init OK (skeleton)");
    return ESP_OK;
}

esp_err_t voice_solo_send_text(const char *text) {
    if (!text || !*text) return ESP_ERR_INVALID_ARG;
    /* Task 8 wires the LLM call in.  For now: log + flip a state so we
     * can verify the path is reached during integration tests. */
    ESP_LOGW(TAG, "voice_solo_send_text stub — text='%s'", text);
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t voice_solo_send_audio(int16_t *pcm, size_t samples) {
    if (!pcm) return ESP_ERR_INVALID_ARG;
    /* Task 11 wires this in. */
    heap_caps_free(pcm);
    ESP_LOGW(TAG, "voice_solo_send_audio stub — samples=%u", (unsigned) samples);
    return ESP_ERR_NOT_SUPPORTED;
}

void voice_solo_cancel(void) {
    /* Task 11 cancels in-flight HTTP via openrouter_cancel_inflight. */
    s_busy = false;
}

bool voice_solo_busy(void) {
    return s_busy;
}
```

- [ ] **Step 3: Register source in `main/CMakeLists.txt`**

Find the `idf_component_register(SRCS ...)` block (top of file). Add `"voice_solo.c"` to the SRCS list (alphabetical-ish, near `"voice_modes.c"`).

- [ ] **Step 4: Build**

```bash
idf.py build
```

Expected: `voice_solo.c.obj` link success.

- [ ] **Step 5: clang-format + commit**

```bash
git-clang-format --binary clang-format-18 --diff origin/main main/voice_solo.c main/voice_solo.h main/CMakeLists.txt
ISSUE=$(grep -oE '[0-9]+' /tmp/solo_mode_issue)
git add main/voice_solo.h main/voice_solo.c main/CMakeLists.txt
git commit -m "feat(voice_solo): skeleton orchestrator (refs #${ISSUE})"
git push
```

---

### Task 4: Wire `voice_modes` SOLO route to `voice_solo_send_text`

**Files:**
- Modify: `main/voice_modes.c` (replace stub from Task 2 with real call)

- [ ] **Step 1: Replace the stub in `voice_modes.c`**

Find the `if (vmode == VMODE_SOLO_DIRECT) { ... }` block from Task 2 and replace with:

```c
    if (vmode == VMODE_SOLO_DIRECT) {
        char or_key[96] = {0};
        tab5_settings_get_or_key(or_key, sizeof or_key);
        if (or_key[0] == '\0') {
            return (voice_modes_route_result_t){
                .kind = VOICE_MODES_ROUTE_SOLO_NO_KEY,
                .err = ESP_ERR_INVALID_STATE,
            };
        }
        esp_err_t err = voice_solo_send_text(text);
        return (voice_modes_route_result_t){
            .kind = (err == ESP_OK) ? VOICE_MODES_ROUTE_SOLO_OK
                                    : VOICE_MODES_ROUTE_SOLO_FAILED,
            .err = err,
        };
    }
```

Add `#include "voice_solo.h"` at the top.

- [ ] **Step 2: Update `voice.c` to handle the new route kinds**

In `voice_send_text` (search `voice_modes_route_text`), find the `switch (result.kind)` block and add cases:

```c
        case VOICE_MODES_ROUTE_SOLO_OK:
            /* voice_solo_send_text already drove the state machine +
             * streamed chat updates; nothing to do here. */
            return ESP_OK;
        case VOICE_MODES_ROUTE_SOLO_NO_KEY:
            ui_chat_show_toast("Scan QR to set OpenRouter key");
            return ESP_ERR_INVALID_STATE;
        case VOICE_MODES_ROUTE_SOLO_FAILED:
            ui_chat_show_toast("Solo turn failed — see logs");
            return result.err;
```

(`ui_chat_show_toast` is the existing toast helper; verify with `grep -n "ui_chat_show_toast\b" main/ui_chat.h`. If the function is `ui_show_toast` or similar, adapt.)

- [ ] **Step 3: Build + flash**

```bash
idf.py build && idf.py -p /dev/ttyACM0 flash
```

- [ ] **Step 4: Manual smoke**

```bash
TAB5_TOKEN=$(grep '^TAB5_TOKEN=' .env | cut -d= -f2)
# Set vmode=5 with empty key — expect NO_KEY toast path
curl -sS -H "Authorization: Bearer $TAB5_TOKEN" -X POST 'http://192.168.70.128:8080/settings' -d '{"or_key": ""}'
curl -sS -H "Authorization: Bearer $TAB5_TOKEN" -X POST 'http://192.168.70.128:8080/mode?m=5'
curl -sS -H "Authorization: Bearer $TAB5_TOKEN" -X POST 'http://192.168.70.128:8080/chat' -d '{"text":"hi"}'
# Watch /events for the NO_KEY toast obs event
curl -sS -H "Authorization: Bearer $TAB5_TOKEN" 'http://192.168.70.128:8080/events?since=0' | tail -10
```

Expected: serial log shows `voice_solo_send_text stub — text='hi'` was NOT reached, route returned NO_KEY (because key empty); /events shows `error.solo` or `voice.toast` entry. No reboot.

- [ ] **Step 5: clang-format + commit**

```bash
git-clang-format --binary clang-format-18 --diff origin/main main/voice_modes.c main/voice.c
ISSUE=$(grep -oE '[0-9]+' /tmp/solo_mode_issue)
git add main/voice_modes.c main/voice.c
git commit -m "feat(voice_modes): route vmode=5 to voice_solo_send_text (refs #${ISSUE})"
git push
```

---

### Task 5: Mode-pill cycling K144 ↔ SOLO in `voice.c`

**Files:**
- Modify: `main/voice.c` (cycle logic)

- [ ] **Step 1: Locate the mode-pill cycle function**

```bash
grep -n "voice_cycle_mode\|mode_pill_cycle\|next_mode\|tab5_voice_mode_pill" main/voice.c main/ui_home.c main/voice_modes.c
```

The function is in `voice.c` (or close). Read the body to understand the current cycle (it walks 0→1→2→3→4→0 today).

- [ ] **Step 2: Modify cycle to handle k144↔solo wrap**

Replace the cycle body so when current is `VMODE_LOCAL_ONBOARD` next is `VMODE_SOLO_DIRECT`, and when current is `VMODE_SOLO_DIRECT` next wraps back to `VMODE_LOCAL_ONBOARD`. Other modes still cycle 0→1→2→3→4 as before.

```c
voice_mode_t voice_modes_pill_next(voice_mode_t current) {
    switch (current) {
        case VMODE_LOCAL_ONBOARD:  return VMODE_SOLO_DIRECT;
        case VMODE_SOLO_DIRECT:    return VMODE_LOCAL_ONBOARD;
        default:                   return (current + 1) % 5;  /* 0→1→2→3→4→0 */
    }
}
```

If the existing function lives in `voice.c` and is already public, edit it in place; otherwise add `voice_modes_pill_next` to `voice_modes.{c,h}` and have `voice.c` call it.

- [ ] **Step 3: Update mode pill UI label rendering**

In `ui_home.c` find where the pill label is computed (search `mode_pill\|s_mode_pill\|mode_label`). Add a "SOLO" string for `VMODE_SOLO_DIRECT`:

```c
    static const char *kModeLabels[] = {
        "LOCAL", "HYBRID", "CLOUD", "TINKERCLAW", "K144", "SOLO",
    };
    const char *label = (vmode <= VMODE_SOLO_DIRECT) ? kModeLabels[vmode] : "?";
```

Adjust to whatever shape the existing array has. The colour token convention from `widget_icons` (calm=emerald) maps "SOLO" to the same family as "CLOUD" (cyan-ish). Use `ui_theme.h` colour tokens.

- [ ] **Step 4: Build + flash + manual cycle test**

```bash
idf.py build && idf.py -p /dev/ttyACM0 flash
TAB5_TOKEN=$(grep '^TAB5_TOKEN=' .env | cut -d= -f2)
# Force vmode=4
curl -sS -H "Authorization: Bearer $TAB5_TOKEN" -X POST 'http://192.168.70.128:8080/mode?m=4'
# Tap the mode pill (coordinates from reference_tab5_touchmap)
curl -sS -H "Authorization: Bearer $TAB5_TOKEN" -X POST 'http://192.168.70.128:8080/touch' -d '{"x":620,"y":120,"action":"tap"}'
# Verify mode is now 5
curl -sS -H "Authorization: Bearer $TAB5_TOKEN" 'http://192.168.70.128:8080/voice' | python3 -m json.tool
# Tap again — should wrap back to 4
curl -sS -H "Authorization: Bearer $TAB5_TOKEN" -X POST 'http://192.168.70.128:8080/touch' -d '{"x":620,"y":120,"action":"tap"}'
curl -sS -H "Authorization: Bearer $TAB5_TOKEN" 'http://192.168.70.128:8080/voice' | python3 -m json.tool
```

Expected: first tap reports mode_id=5, second reports mode_id=4. (Adjust pill coords by reading the actual layout if 620,120 misses — `reference_tab5_touchmap.md` has the canonical pixel map.)

- [ ] **Step 5: clang-format + commit**

```bash
git-clang-format --binary clang-format-18 --diff origin/main main/voice.c main/voice_modes.c main/voice_modes.h main/ui_home.c
ISSUE=$(grep -oE '[0-9]+' /tmp/solo_mode_issue)
git add main/voice.c main/voice_modes.c main/voice_modes.h main/ui_home.c
git commit -m "feat(voice): mode pill cycles K144 ↔ SOLO (refs #${ISSUE})"
git push
```

---

### Task 6: `openrouter_sse` parser (pure logic + synthetic test endpoint)

**Files:**
- Create: `main/openrouter_sse.h`
- Create: `main/openrouter_sse.c`
- Create: `main/debug_server_solo.c` (test endpoints — first endpoint goes here)
- Modify: `main/debug_server.h` (declare `tab5_debug_solo_register`)
- Modify: `main/debug_server.c` (call `tab5_debug_solo_register` on startup)
- Modify: `main/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

Add to `tests/e2e/scenarios/test_solo_sse.py`:

```python
"""SSE parser unit tests via /solo/sse_test."""
import json
import requests

URL = "http://192.168.70.128:8080"
import os; TOKEN = os.environ["TAB5_TOKEN"]
H = {"Authorization": f"Bearer {TOKEN}"}

def _parse(payload: bytes) -> dict:
    r = requests.post(f"{URL}/solo/sse_test", headers=H, data=payload, timeout=4)
    r.raise_for_status()
    return r.json()

def test_single_delta() -> None:
    out = _parse(
        b'data: {"choices":[{"delta":{"content":"hello"}}]}\n\n'
        b'data: [DONE]\n\n'
    )
    assert out["deltas"] == ["hello"]
    assert out["done"] is True

def test_split_chunk() -> None:
    # Boundary in the middle of "world" — parser must buffer.
    out = _parse(
        b'data: {"choices":[{"delta":{"content":"hello "}}]}\n\ndata: {"choi'
        b'ces":[{"delta":{"content":"world"}}]}\n\ndata: [DONE]\n\n'
    )
    assert out["deltas"] == ["hello ", "world"]
    assert out["done"] is True

def test_comment_lines_ignored() -> None:
    out = _parse(
        b': openrouter keepalive\n\n'
        b'data: {"choices":[{"delta":{"content":"x"}}]}\n\n'
        b'data: [DONE]\n\n'
    )
    assert out["deltas"] == ["x"]
```

- [ ] **Step 2: Run the test (will fail — endpoint missing)**

```bash
TAB5_TOKEN=$(grep '^TAB5_TOKEN=' .env | cut -d= -f2) \
TAB5_URL=http://192.168.70.128:8080 \
python3 -m pytest tests/e2e/scenarios/test_solo_sse.py -v
```

Expected: 404 from `/solo/sse_test`.

- [ ] **Step 3: Create `main/openrouter_sse.h`**

```c
/**
 * openrouter_sse — line-buffered SSE chunk parser.
 *
 * Feeds bytes via openrouter_sse_feed; calls back per `data: <json>` line
 * with the JSON body (string lifetime: callback only).  Returns true
 * when the stream has hit `data: [DONE]`.  Comment lines (starting `:`)
 * are dropped.  Multiple events per feed call are fine; chunks split
 * across feed calls are buffered.
 *
 * Allocates a 4 KB line buffer in PSRAM via heap_caps_malloc on init;
 * caller must call openrouter_sse_free.
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*openrouter_sse_data_cb_t)(const char *json, size_t len, void *ctx);

typedef struct openrouter_sse_state openrouter_sse_state_t;

esp_err_t openrouter_sse_init(openrouter_sse_state_t **out,
                              openrouter_sse_data_cb_t cb, void *ctx);

/** Feed bytes; returns true if [DONE] sentinel observed. */
bool openrouter_sse_feed(openrouter_sse_state_t *s, const char *buf, size_t len);

void openrouter_sse_free(openrouter_sse_state_t *s);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 4: Create `main/openrouter_sse.c`**

```c
#include "openrouter_sse.h"

#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"

#define LINE_BUF_BYTES 4096

struct openrouter_sse_state {
    char *line;
    size_t line_len;
    bool done;
    openrouter_sse_data_cb_t cb;
    void *ctx;
};

static const char *TAG = "or_sse";

esp_err_t openrouter_sse_init(openrouter_sse_state_t **out,
                              openrouter_sse_data_cb_t cb, void *ctx) {
    if (!out || !cb) return ESP_ERR_INVALID_ARG;
    openrouter_sse_state_t *s = heap_caps_calloc(
        1, sizeof(*s), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s) return ESP_ERR_NO_MEM;
    s->line = heap_caps_malloc(LINE_BUF_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s->line) { heap_caps_free(s); return ESP_ERR_NO_MEM; }
    s->cb = cb;
    s->ctx = ctx;
    *out = s;
    return ESP_OK;
}

static void emit_line(openrouter_sse_state_t *s) {
    /* s->line is NOT null-terminated; emit using length. */
    if (s->line_len == 0) return;
    /* Trim trailing \r if present. */
    size_t n = s->line_len;
    if (s->line[n - 1] == '\r') n--;
    if (n == 0) return;
    /* Comment line. */
    if (s->line[0] == ':') return;
    /* `data:` lines we care about. */
    static const char kPrefix[] = "data:";
    if (n < sizeof(kPrefix) - 1 ||
        memcmp(s->line, kPrefix, sizeof(kPrefix) - 1) != 0) {
        return;
    }
    const char *p = s->line + sizeof(kPrefix) - 1;
    size_t pn = n - (sizeof(kPrefix) - 1);
    /* Optional single space after colon. */
    if (pn > 0 && *p == ' ') { p++; pn--; }
    /* Done sentinel. */
    if (pn == 6 && memcmp(p, "[DONE]", 6) == 0) {
        s->done = true;
        return;
    }
    s->cb(p, pn, s->ctx);
}

bool openrouter_sse_feed(openrouter_sse_state_t *s, const char *buf, size_t len) {
    if (!s || !buf) return s ? s->done : false;
    for (size_t i = 0; i < len; i++) {
        char c = buf[i];
        if (c == '\n') {
            emit_line(s);
            s->line_len = 0;
        } else if (s->line_len + 1 < LINE_BUF_BYTES) {
            s->line[s->line_len++] = c;
        } else {
            ESP_LOGW(TAG, "SSE line overflow — dropping");
            s->line_len = 0;
        }
    }
    return s->done;
}

void openrouter_sse_free(openrouter_sse_state_t *s) {
    if (!s) return;
    heap_caps_free(s->line);
    heap_caps_free(s);
}
```

- [ ] **Step 5: Create `main/debug_server_solo.c` with the synthetic SSE endpoint**

```c
/**
 * debug_server_solo — synthetic test endpoints for solo-mode unit tests.
 *
 * /solo/sse_test  — POST raw SSE bytes, get back parsed deltas + done flag
 * /solo/rag_test  — added in Task 15
 * /solo/llm_test  — added in Task 8
 *
 * These are intentionally permanent (not gated behind a build flag) — the
 * E2E harness uses them on every CI run.
 */
#include "debug_server.h"
#include "debug_server_internal.h"
#include "openrouter_sse.h"

#include <string.h>
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"

static const char *TAG = "dbg_solo";

typedef struct {
    cJSON *deltas;
} sse_test_ctx_t;

static void sse_test_cb(const char *json, size_t len, void *ctx) {
    sse_test_ctx_t *t = ctx;
    /* Parse JSON, pull out choices[0].delta.content. */
    char *copy = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    memcpy(copy, json, len); copy[len] = '\0';
    cJSON *root = cJSON_Parse(copy);
    if (root) {
        cJSON *choices = cJSON_GetObjectItem(root, "choices");
        if (cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0) {
            cJSON *c0 = cJSON_GetArrayItem(choices, 0);
            cJSON *delta = cJSON_GetObjectItem(c0, "delta");
            cJSON *content = cJSON_GetObjectItem(delta, "content");
            if (cJSON_IsString(content)) {
                cJSON_AddItemToArray(t->deltas,
                    cJSON_CreateString(content->valuestring));
            }
        }
        cJSON_Delete(root);
    }
    heap_caps_free(copy);
}

static esp_err_t sse_test_handler(httpd_req_t *req) {
    if (tab5_debug_check_auth(req) != ESP_OK) return ESP_FAIL;

    size_t total = req->content_len;
    if (total == 0 || total > 64 * 1024) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad len");
    }
    char *buf = heap_caps_malloc(total + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return ESP_ERR_NO_MEM;
    size_t got = 0;
    while (got < total) {
        int n = httpd_req_recv(req, buf + got, total - got);
        if (n <= 0) { heap_caps_free(buf); return ESP_FAIL; }
        got += n;
    }

    sse_test_ctx_t tctx = { .deltas = cJSON_CreateArray() };
    openrouter_sse_state_t *s = NULL;
    openrouter_sse_init(&s, sse_test_cb, &tctx);
    bool done = openrouter_sse_feed(s, buf, total);
    openrouter_sse_free(s);
    heap_caps_free(buf);

    cJSON *out = cJSON_CreateObject();
    cJSON_AddItemToObject(out, "deltas", tctx.deltas);
    cJSON_AddBoolToObject(out, "done", done);
    return tab5_debug_send_json(req, out);
}

esp_err_t tab5_debug_solo_register(httpd_handle_t srv) {
    static const httpd_uri_t kSseTest = {
        .uri = "/solo/sse_test", .method = HTTP_POST,
        .handler = sse_test_handler, .user_ctx = NULL,
    };
    httpd_register_uri_handler(srv, &kSseTest);
    ESP_LOGI(TAG, "debug_solo registered (sse_test)");
    return ESP_OK;
}
```

(Verify the helper names: `tab5_debug_check_auth` and `tab5_debug_send_json` — `grep -n "tab5_debug_check_auth\|tab5_debug_send_json\|send_json_resp" main/debug_server*.c main/debug_server_internal.h`. If the project name is `send_json_resp`, swap accordingly.)

- [ ] **Step 6: Wire registration**

In `main/debug_server.h` add prototype:

```c
esp_err_t tab5_debug_solo_register(httpd_handle_t srv);
```

In `main/debug_server.c` near the other `tab5_debug_*_register` calls (search the boot path's family registrations):

```c
    tab5_debug_solo_register(server);
```

In `main/CMakeLists.txt` add `"openrouter_sse.c"` and `"debug_server_solo.c"` to SRCS.

- [ ] **Step 7: Build + flash + run test**

```bash
idf.py build && idf.py -p /dev/ttyACM0 flash
TAB5_TOKEN=$(grep '^TAB5_TOKEN=' .env | cut -d= -f2) \
TAB5_URL=http://192.168.70.128:8080 \
python3 -m pytest tests/e2e/scenarios/test_solo_sse.py -v
```

Expected: all 3 tests PASS.

- [ ] **Step 8: clang-format + commit**

```bash
git-clang-format --binary clang-format-18 --diff origin/main \
    main/openrouter_sse.c main/openrouter_sse.h \
    main/debug_server_solo.c main/debug_server.c main/debug_server.h main/CMakeLists.txt
ISSUE=$(grep -oE '[0-9]+' /tmp/solo_mode_issue)
git add main/openrouter_sse.h main/openrouter_sse.c \
        main/debug_server_solo.c main/debug_server.h main/debug_server.c main/CMakeLists.txt \
        tests/e2e/scenarios/test_solo_sse.py
git commit -m "feat(openrouter): SSE parser + /solo/sse_test endpoint (refs #${ISSUE})"
git push
```

---

### Task 7: `openrouter_client` header + auth header helper

**Files:**
- Create: `main/openrouter_client.h` (full public API)
- Create: `main/openrouter_client.c` (auth helper + stubs for 4 verbs)
- Modify: `main/CMakeLists.txt`

- [ ] **Step 1: Create `main/openrouter_client.h`**

```c
/**
 * openrouter_client — HTTP wrapper for OpenRouter's audio + chat APIs.
 *
 * All four verbs share a base URL (https://openrouter.ai/api/v1) and an
 * Authorization: Bearer <or_key> header.  TLS via the bundled cert
 * store (DigiCert/Cloudflare chain — already attached in
 * service_network.c).  PSRAM is used for response bodies; internal
 * SRAM impact is ~0.
 *
 * Cancellation: openrouter_cancel_inflight aborts whatever HTTP call
 * is currently in progress (single-flight model — the solo state
 * machine never overlaps two calls).
 */
#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Caller-supplied callbacks for streaming verbs. */
typedef void (*openrouter_chat_delta_cb_t)(const char *delta, size_t len, void *ctx);
typedef void (*openrouter_tts_chunk_cb_t)(const uint8_t *pcm, size_t len, void *ctx);

/** STT — multipart upload of PCM-16LE @ 16 kHz mono.  Writes UTF-8
 *  transcript into out_text (caller-owned, NUL-terminated). */
esp_err_t openrouter_stt(const int16_t *pcm, size_t samples,
                         char *out_text, size_t out_cap);

/** Streaming chat completion.  messages_json is a JSON-encoded array of
 *  {role, content} (caller builds it).  delta_cb fires per token. */
esp_err_t openrouter_chat_stream(const char *messages_json,
                                 openrouter_chat_delta_cb_t cb, void *ctx);

/** TTS.  Streams WAV bytes; first 44 bytes are the RIFF header (skipped
 *  by the callback) and the rest is PCM-16LE @ 24 kHz mono.  cb fires
 *  per HTTP chunk. */
esp_err_t openrouter_tts(const char *text,
                         openrouter_tts_chunk_cb_t cb, void *ctx);

/** Embeddings.  Returns a malloc'd float vector (PSRAM); caller frees
 *  with heap_caps_free.  *out_dim is set to the dimensionality. */
esp_err_t openrouter_embed(const char *text, float **out_vec, size_t *out_dim);

/** Abort any in-flight HTTP. */
void openrouter_cancel_inflight(void);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: Create `main/openrouter_client.c` with shared auth helper + stubs**

```c
#include "openrouter_client.h"

#include <string.h>
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "settings.h"

static const char *TAG = "or_client";

#define OR_BASE_URL "https://openrouter.ai/api/v1"
#define OR_REFERER  "https://tab5.tinkerclaw.local"

static esp_http_client_handle_t s_inflight = NULL;  /* single-flight */

esp_err_t openrouter_apply_auth(esp_http_client_handle_t c) {
    char key[96] = {0};
    esp_err_t err = tab5_settings_get_or_key(key, sizeof key);
    if (err != ESP_OK || key[0] == '\0') return ESP_ERR_INVALID_STATE;
    char hdr[128];
    snprintf(hdr, sizeof hdr, "Bearer %s", key);
    esp_http_client_set_header(c, "Authorization", hdr);
    esp_http_client_set_header(c, "HTTP-Referer", OR_REFERER);
    esp_http_client_set_header(c, "X-Title", "TinkerTab Solo");
    return ESP_OK;
}

void openrouter_cancel_inflight(void) {
    if (s_inflight) {
        ESP_LOGW(TAG, "cancel_inflight — closing in-flight HTTP");
        esp_http_client_close(s_inflight);
    }
}

/* Stubs — Tasks 8/9/10/11 fill these in. */

esp_err_t openrouter_stt(const int16_t *pcm, size_t samples,
                         char *out_text, size_t out_cap) {
    (void)pcm; (void)samples; (void)out_text; (void)out_cap;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t openrouter_chat_stream(const char *messages_json,
                                 openrouter_chat_delta_cb_t cb, void *ctx) {
    (void)messages_json; (void)cb; (void)ctx;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t openrouter_tts(const char *text,
                         openrouter_tts_chunk_cb_t cb, void *ctx) {
    (void)text; (void)cb; (void)ctx;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t openrouter_embed(const char *text, float **out_vec, size_t *out_dim) {
    (void)text; (void)out_vec; (void)out_dim;
    return ESP_ERR_NOT_SUPPORTED;
}

/* Internal helper used by the four verbs (visibility note — keep
 * non-static so the verbs in this same TU can call it). */
extern esp_http_client_handle_t openrouter_open_post(const char *path);

esp_http_client_handle_t openrouter_open_post(const char *path) {
    char url[160];
    snprintf(url, sizeof url, "%s%s", OR_BASE_URL, path);
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 60000,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return NULL;
    if (openrouter_apply_auth(c) != ESP_OK) {
        esp_http_client_cleanup(c);
        return NULL;
    }
    s_inflight = c;
    return c;
}
```

- [ ] **Step 3: Register source**

`main/CMakeLists.txt`: add `"openrouter_client.c"` to SRCS.

- [ ] **Step 4: Build**

```bash
idf.py build
```

Expected: clean build, `openrouter_client.c.obj` linked.

- [ ] **Step 5: clang-format + commit**

```bash
git-clang-format --binary clang-format-18 --diff origin/main main/openrouter_client.c main/openrouter_client.h
ISSUE=$(grep -oE '[0-9]+' /tmp/solo_mode_issue)
git add main/openrouter_client.h main/openrouter_client.c main/CMakeLists.txt
git commit -m "feat(openrouter): client header + auth helper (refs #${ISSUE})"
git push
```

---

### Task 8: Implement `openrouter_chat_stream` (LLM SSE)

**Files:**
- Modify: `main/openrouter_client.c`
- Modify: `main/debug_server_solo.c` (add `/solo/llm_test` endpoint)

- [ ] **Step 1: Write the failing test**

Add `tests/e2e/scenarios/test_solo_llm.py`:

```python
"""LLM streaming smoke via /solo/llm_test."""
import os, requests
URL = "http://192.168.70.128:8080"
H = {"Authorization": f"Bearer {os.environ['TAB5_TOKEN']}"}

def test_chat_completion_simple():
    r = requests.post(f"{URL}/solo/llm_test", headers=H, timeout=30,
                      json={"prompt": "say the word 'pong' once"})
    r.raise_for_status()
    out = r.json()
    assert out["ok"] is True
    assert "pong" in out["reply"].lower()
    assert out["delta_count"] >= 1
```

- [ ] **Step 2: Verify it fails**

```bash
TAB5_TOKEN=$(grep '^TAB5_TOKEN=' .env | cut -d= -f2) \
python3 -m pytest tests/e2e/scenarios/test_solo_llm.py -v
```

Expected: 404.

- [ ] **Step 3: Implement `openrouter_chat_stream` in `main/openrouter_client.c`**

Replace the stub:

```c
typedef struct {
    openrouter_chat_delta_cb_t cb;
    void *cb_ctx;
    openrouter_sse_state_t *sse;
} chat_stream_ctx_t;

static void chat_sse_cb(const char *json, size_t len, void *ctx) {
    chat_stream_ctx_t *c = ctx;
    /* Extract choices[0].delta.content. */
    char *copy = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!copy) return;
    memcpy(copy, json, len); copy[len] = '\0';
    cJSON *root = cJSON_Parse(copy);
    if (root) {
        cJSON *choices = cJSON_GetObjectItem(root, "choices");
        if (cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0) {
            cJSON *c0 = cJSON_GetArrayItem(choices, 0);
            cJSON *delta = cJSON_GetObjectItem(c0, "delta");
            cJSON *content = cJSON_GetObjectItem(delta, "content");
            if (cJSON_IsString(content) && content->valuestring) {
                c->cb(content->valuestring, strlen(content->valuestring), c->cb_ctx);
            }
        }
        cJSON_Delete(root);
    }
    heap_caps_free(copy);
}

esp_err_t openrouter_chat_stream(const char *messages_json,
                                 openrouter_chat_delta_cb_t cb, void *ctx) {
    if (!messages_json || !cb) return ESP_ERR_INVALID_ARG;

    char model[96] = {0};
    tab5_settings_get_or_mdl_llm(model, sizeof model);

    /* Build request body: {"model": "...", "messages": [...], "stream": true} */
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "model", model);
    cJSON_AddItemToObject(body, "messages", cJSON_Parse(messages_json));
    cJSON_AddBoolToObject(body, "stream", true);
    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!body_str) return ESP_ERR_NO_MEM;

    esp_http_client_handle_t c = openrouter_open_post("/chat/completions");
    if (!c) { free(body_str); return ESP_ERR_INVALID_STATE; }
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_header(c, "Accept", "text/event-stream");

    chat_stream_ctx_t cctx = { .cb = cb, .cb_ctx = ctx };
    openrouter_sse_init(&cctx.sse, chat_sse_cb, &cctx);

    esp_err_t err = esp_http_client_open(c, strlen(body_str));
    if (err != ESP_OK) goto cleanup;
    int wrote = esp_http_client_write(c, body_str, strlen(body_str));
    if (wrote < 0) { err = ESP_FAIL; goto cleanup; }
    int hlen = esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    if (status != 200) {
        ESP_LOGE(TAG, "chat status=%d hlen=%d", status, hlen);
        err = ESP_FAIL; goto cleanup;
    }
    char rbuf[1024];
    while (1) {
        int n = esp_http_client_read(c, rbuf, sizeof rbuf);
        if (n <= 0) break;
        bool done = openrouter_sse_feed(cctx.sse, rbuf, n);
        if (done) break;
    }

cleanup:
    openrouter_sse_free(cctx.sse);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    s_inflight = NULL;
    free(body_str);
    return err;
}
```

Add includes: `#include "openrouter_sse.h"`, `#include "cJSON.h"`, `#include <stdlib.h>`.

- [ ] **Step 4: Add `/solo/llm_test` endpoint in `debug_server_solo.c`**

Append:

```c
typedef struct { char *reply; size_t cap; size_t len; size_t deltas; } llm_test_ctx_t;
static void llm_test_delta(const char *d, size_t n, void *vctx) {
    llm_test_ctx_t *c = vctx;
    if (c->len + n + 1 < c->cap) {
        memcpy(c->reply + c->len, d, n);
        c->len += n;
        c->reply[c->len] = '\0';
    }
    c->deltas++;
}

static esp_err_t llm_test_handler(httpd_req_t *req) {
    if (tab5_debug_check_auth(req) != ESP_OK) return ESP_FAIL;
    char body[512] = {0};
    int n = httpd_req_recv(req, body, sizeof body - 1);
    if (n <= 0) return ESP_FAIL;
    cJSON *root = cJSON_Parse(body);
    cJSON *prompt = root ? cJSON_GetObjectItem(root, "prompt") : NULL;
    if (!cJSON_IsString(prompt)) {
        if (root) cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad prompt");
    }

    /* Build a minimal messages array. */
    char msgs[640];
    snprintf(msgs, sizeof msgs,
        "[{\"role\":\"user\",\"content\":%s}]",
        cJSON_PrintUnformatted(cJSON_CreateString(prompt->valuestring)));
    cJSON_Delete(root);

    llm_test_ctx_t tctx = {
        .reply = heap_caps_calloc(1, 4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
        .cap = 4096,
    };
    esp_err_t err = openrouter_chat_stream(msgs, llm_test_delta, &tctx);

    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", err == ESP_OK);
    cJSON_AddStringToObject(out, "reply", tctx.reply ? tctx.reply : "");
    cJSON_AddNumberToObject(out, "delta_count", tctx.deltas);
    heap_caps_free(tctx.reply);
    return tab5_debug_send_json(req, out);
}
```

Register in `tab5_debug_solo_register`:

```c
    static const httpd_uri_t kLlmTest = {
        .uri = "/solo/llm_test", .method = HTTP_POST,
        .handler = llm_test_handler, .user_ctx = NULL,
    };
    httpd_register_uri_handler(srv, &kLlmTest);
```

Add `#include "openrouter_client.h"` at the top.

- [ ] **Step 5: Build + flash + provision a real API key**

```bash
idf.py build && idf.py -p /dev/ttyACM0 flash
TAB5_TOKEN=$(grep '^TAB5_TOKEN=' .env | cut -d= -f2)
# Replace with a real key (test account preferred):
curl -sS -H "Authorization: Bearer $TAB5_TOKEN" -X POST http://192.168.70.128:8080/settings \
  -d "{\"or_key\":\"${OPENROUTER_KEY}\"}"
```

- [ ] **Step 6: Run the test**

```bash
TAB5_TOKEN=$(grep '^TAB5_TOKEN=' .env | cut -d= -f2) \
python3 -m pytest tests/e2e/scenarios/test_solo_llm.py -v
```

Expected: PASS, reply contains "pong".

- [ ] **Step 7: clang-format + commit**

```bash
git-clang-format --binary clang-format-18 --diff origin/main main/openrouter_client.c main/debug_server_solo.c
ISSUE=$(grep -oE '[0-9]+' /tmp/solo_mode_issue)
git add main/openrouter_client.c main/debug_server_solo.c tests/e2e/scenarios/test_solo_llm.py
git commit -m "feat(openrouter): chat completions streaming + /solo/llm_test (refs #${ISSUE})"
git push
```

---

### Task 9: Wire LLM into `voice_solo_send_text`

**Files:**
- Modify: `main/voice_solo.c` (replace stub)

- [ ] **Step 1: Replace the stub `voice_solo_send_text`**

```c
#include "openrouter_client.h"
#include "openrouter_sse.h"
#include "voice.h"
#include "ui_chat.h"
#include "debug_obs.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef struct {
    char *acc;
    size_t acc_cap;
    size_t acc_len;
} solo_chat_ctx_t;

static void solo_chat_delta(const char *d, size_t n, void *vctx) {
    solo_chat_ctx_t *c = vctx;
    if (c->acc_len + n + 1 < c->acc_cap) {
        memcpy(c->acc + c->acc_len, d, n);
        c->acc_len += n;
        c->acc[c->acc_len] = '\0';
    }
    /* Push delta into chat UI as it arrives. */
    ui_chat_append_assistant_delta(d, n);
}

static char *solo_build_messages_json(const char *user_text) {
    /* Single-shot: just the user turn (history wiring lands in Task 14). */
    cJSON *arr = cJSON_CreateArray();
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", "user");
    cJSON_AddStringToObject(m, "content", user_text);
    cJSON_AddItemToArray(arr, m);
    char *s = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return s;
}

static void solo_send_text_task(void *arg) {
    char *text = arg;
    s_busy = true;
    voice_set_state(VOICE_STATE_PROCESSING, "solo");
    tab5_debug_obs_event("solo.llm_start", "");

    char *msgs = solo_build_messages_json(text);
    solo_chat_ctx_t cctx = {
        .acc = heap_caps_calloc(1, 8192, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
        .acc_cap = 8192,
    };
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = openrouter_chat_stream(msgs, solo_chat_delta, &cctx);
    int64_t dt = (esp_timer_get_time() - t0) / 1000;
    char detail[48]; snprintf(detail, sizeof detail, "ms=%lld", dt);
    tab5_debug_obs_event(err == ESP_OK ? "solo.llm_done" : "solo.error", detail);
    free(msgs);

    if (err == ESP_OK) {
        ui_chat_finalize_assistant(cctx.acc);
    } else {
        ui_chat_show_toast("Solo LLM failed");
    }
    heap_caps_free(cctx.acc);
    free(text);
    voice_set_state(VOICE_STATE_READY, "solo");
    s_busy = false;
    vTaskSuspend(NULL); /* CLAUDE.md rule: no vTaskDelete(NULL) on P4
                          (TLSP cleanup crash, see issue #20). */
}

esp_err_t voice_solo_send_text(const char *text) {
    if (!text || !*text) return ESP_ERR_INVALID_ARG;
    if (s_busy) return ESP_ERR_INVALID_STATE;
    char *copy = strdup(text);
    if (!copy) return ESP_ERR_NO_MEM;
    /* 8 KB stack — chat_stream needs 4 KB SSE buffer + cJSON. */
    BaseType_t r = xTaskCreate(
        solo_send_text_task, "voice_solo_text", 8192, copy, 5, NULL);
    if (r != pdPASS) { free(copy); return ESP_ERR_NO_MEM; }
    return ESP_OK;
}
```

`ui_chat_append_assistant_delta` and `ui_chat_finalize_assistant` are existing helpers used by the WS streaming path — verify with `grep -n "ui_chat_append_assistant_delta\|ui_chat_finalize_assistant" main/ui_chat.h main/ui_chat.c`. If the existing names are different (likely `ui_chat_stream_append` / `ui_chat_stream_done`), adapt.

Add the includes if missing: `#include "esp_timer.h"`, `#include <string.h>`, `#include "freertos/FreeRTOS.h"`, `#include "freertos/task.h"`.

Note on `vTaskDelete(NULL)` — `CLAUDE.md` warns this can crash on P4 cleanup. Use `vTaskSuspend(NULL)` and accept the leaked task slot (`task_worker.c` is the canonical "shared FreeRTOS job queue" — better long-term route is to enqueue to that instead of spawning ad-hoc tasks; `grep -n "tab5_worker_post\|tab5_worker_enqueue" main/task_worker.h` to see the API). For this plan, post via `tab5_worker_post` and drop the per-task creation if the API exists; otherwise `vTaskSuspend(NULL)`.

- [ ] **Step 2: Build + flash**

```bash
idf.py build && idf.py -p /dev/ttyACM0 flash
```

- [ ] **Step 3: Manual verification**

```bash
TAB5_TOKEN=$(grep '^TAB5_TOKEN=' .env | cut -d= -f2)
curl -sS -H "Authorization: Bearer $TAB5_TOKEN" -X POST 'http://192.168.70.128:8080/mode?m=5'
curl -sS -H "Authorization: Bearer $TAB5_TOKEN" -X POST http://192.168.70.128:8080/chat \
  -d '{"text":"give me a 5-word weather metaphor"}'
sleep 8
curl -sS -H "Authorization: Bearer $TAB5_TOKEN" 'http://192.168.70.128:8080/events?since=0' | python3 -m json.tool | tail -30
curl -sS -H "Authorization: Bearer $TAB5_TOKEN" 'http://192.168.70.128:8080/voice' | python3 -m json.tool
```

Expected: `solo.llm_start` and `solo.llm_done` events; `last_llm_text` populated; chat overlay on Tab5 screen shows the assistant reply text.

- [ ] **Step 4: clang-format + commit**

```bash
git-clang-format --binary clang-format-18 --diff origin/main main/voice_solo.c
ISSUE=$(grep -oE '[0-9]+' /tmp/solo_mode_issue)
git add main/voice_solo.c
git commit -m "feat(voice_solo): wire LLM streaming into send_text (refs #${ISSUE})"
git push
```

---

### Task 10: Implement `openrouter_stt` (multipart upload)

**Files:**
- Modify: `main/openrouter_client.c`

- [ ] **Step 1: Implement multipart helper + STT verb**

Replace the `openrouter_stt` stub:

```c
/* Build a 44-byte WAV RIFF header + return pointer.  Caller writes the
 * PCM right after.  16-bit, mono, 16 kHz. */
static void wav16_header(uint8_t *hdr, size_t pcm_bytes) {
    const uint32_t sample_rate = 16000;
    const uint16_t num_ch = 1;
    const uint16_t bits = 16;
    const uint32_t byte_rate = sample_rate * num_ch * bits / 8;
    memcpy(hdr,      "RIFF", 4);
    uint32_t riff_size = 36 + (uint32_t)pcm_bytes;
    memcpy(hdr + 4,  &riff_size, 4);
    memcpy(hdr + 8,  "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    uint32_t fmt_size = 16; memcpy(hdr + 16, &fmt_size, 4);
    uint16_t fmt_pcm = 1;   memcpy(hdr + 20, &fmt_pcm, 2);
    memcpy(hdr + 22, &num_ch, 2);
    memcpy(hdr + 24, &sample_rate, 4);
    memcpy(hdr + 28, &byte_rate, 4);
    uint16_t block_align = num_ch * bits / 8;
    memcpy(hdr + 32, &block_align, 2);
    memcpy(hdr + 34, &bits, 2);
    memcpy(hdr + 36, "data", 4);
    uint32_t data_size = (uint32_t)pcm_bytes;
    memcpy(hdr + 40, &data_size, 4);
}

esp_err_t openrouter_stt(const int16_t *pcm, size_t samples,
                         char *out_text, size_t out_cap) {
    if (!pcm || !out_text || out_cap < 16) return ESP_ERR_INVALID_ARG;
    if (samples == 0) return ESP_ERR_INVALID_ARG;

    char model[64] = {0};
    tab5_settings_get_or_mdl_stt(model, sizeof model);

    static const char kBoundary[] = "----tab5-solo-boundary";
    /* multipart parts:
     *   --boundary\r\nContent-Disposition: form-data; name="file"; filename="audio.wav"\r\n
     *   Content-Type: audio/wav\r\n\r\n
     *   <44-byte WAV header><PCM bytes>\r\n
     *   --boundary\r\nContent-Disposition: form-data; name="model"\r\n\r\n<model>\r\n
     *   --boundary--\r\n
     */
    size_t pcm_bytes = samples * sizeof(int16_t);
    size_t wav_bytes = 44 + pcm_bytes;

    char part_a[256];
    int part_a_n = snprintf(part_a, sizeof part_a,
        "--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n", kBoundary);

    char part_b[128];
    int part_b_n = snprintf(part_b, sizeof part_b,
        "\r\n--%s\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n%s\r\n--%s--\r\n",
        kBoundary, model, kBoundary);

    size_t total = part_a_n + wav_bytes + part_b_n;

    esp_http_client_handle_t c = openrouter_open_post("/audio/transcriptions");
    if (!c) return ESP_ERR_INVALID_STATE;
    char ctype[96];
    snprintf(ctype, sizeof ctype, "multipart/form-data; boundary=%s", kBoundary);
    esp_http_client_set_header(c, "Content-Type", ctype);

    esp_err_t err = esp_http_client_open(c, total);
    if (err != ESP_OK) goto cleanup;
    esp_http_client_write(c, part_a, part_a_n);
    uint8_t hdr[44]; wav16_header(hdr, pcm_bytes);
    esp_http_client_write(c, (const char *)hdr, sizeof hdr);
    /* PCM in 4 KB chunks to bound TX buffer copies. */
    const char *pcm_b = (const char *)pcm;
    size_t off = 0;
    while (off < pcm_bytes) {
        size_t n = pcm_bytes - off; if (n > 4096) n = 4096;
        int wrote = esp_http_client_write(c, pcm_b + off, n);
        if (wrote < 0) { err = ESP_FAIL; goto cleanup; }
        off += wrote;
    }
    esp_http_client_write(c, part_b, part_b_n);

    esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    if (status != 200) { err = ESP_FAIL; goto cleanup; }

    /* Response is JSON: {"text":"..."}.  Read it whole (≤16KB). */
    char rbuf[16384] = {0};
    int total_read = 0;
    while (total_read < (int)sizeof rbuf - 1) {
        int n = esp_http_client_read(c, rbuf + total_read, sizeof rbuf - 1 - total_read);
        if (n <= 0) break;
        total_read += n;
    }
    cJSON *root = cJSON_Parse(rbuf);
    if (root) {
        cJSON *t = cJSON_GetObjectItem(root, "text");
        if (cJSON_IsString(t)) {
            strncpy(out_text, t->valuestring, out_cap - 1);
            out_text[out_cap - 1] = '\0';
        } else { err = ESP_FAIL; }
        cJSON_Delete(root);
    } else { err = ESP_FAIL; }

cleanup:
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    s_inflight = NULL;
    return err;
}
```

- [ ] **Step 2: Build**

```bash
idf.py build
```

- [ ] **Step 3: Smoke test via solo-mode voice turn (post-Task 11)**

(STT is exercised end-to-end in Task 11 via the audio path. Mark this task complete only after Task 11 verifies a real mic turn.)

- [ ] **Step 4: clang-format + commit**

```bash
git-clang-format --binary clang-format-18 --diff origin/main main/openrouter_client.c
ISSUE=$(grep -oE '[0-9]+' /tmp/solo_mode_issue)
git add main/openrouter_client.c
git commit -m "feat(openrouter): STT multipart upload (refs #${ISSUE})"
git push
```

---

### Task 11: Implement `openrouter_tts` (chunked WAV) + wire audio path

**Files:**
- Modify: `main/openrouter_client.c` (TTS verb)
- Modify: `main/voice_solo.c` (mic-buffer ingest path)
- Modify: `main/voice.c` (mic-stop hook routes to voice_solo when vmode==5)

- [ ] **Step 1: Implement `openrouter_tts`**

Replace the stub:

```c
esp_err_t openrouter_tts(const char *text,
                         openrouter_tts_chunk_cb_t cb, void *ctx) {
    if (!text || !cb) return ESP_ERR_INVALID_ARG;
    char model[64] = {0}; tab5_settings_get_or_mdl_tts(model, sizeof model);
    char voice[32] = {0}; tab5_settings_get_or_voice(voice, sizeof voice);

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "model", model);
    cJSON_AddStringToObject(body, "voice", voice);
    cJSON_AddStringToObject(body, "input", text);
    cJSON_AddStringToObject(body, "response_format", "wav");
    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    esp_http_client_handle_t c = openrouter_open_post("/audio/speech");
    if (!c) { free(body_str); return ESP_ERR_INVALID_STATE; }
    esp_http_client_set_header(c, "Content-Type", "application/json");

    esp_err_t err = esp_http_client_open(c, strlen(body_str));
    if (err != ESP_OK) { free(body_str); goto cleanup; }
    esp_http_client_write(c, body_str, strlen(body_str));
    esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    if (status != 200) { err = ESP_FAIL; goto cleanup; }

    /* Skip RIFF header (44 bytes) — playback ring expects raw PCM. */
    uint8_t header[44];
    int hr = esp_http_client_read(c, (char *)header, sizeof header);
    if (hr < 44) { err = ESP_FAIL; goto cleanup; }

    /* Stream PCM in 4 KB chunks. */
    uint8_t buf[4096];
    while (1) {
        int n = esp_http_client_read(c, (char *)buf, sizeof buf);
        if (n <= 0) break;
        cb(buf, n, ctx);
    }

cleanup:
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    s_inflight = NULL;
    free(body_str);
    return err;
}
```

- [ ] **Step 2: Implement `voice_solo_send_audio` in `voice_solo.c`**

```c
typedef struct {
    /* nothing yet — chunk goes straight to the playback ring. */
    int dummy;
} solo_tts_ctx_t;

static void solo_tts_chunk(const uint8_t *pcm, size_t len, void *vctx) {
    (void)vctx;
    /* tab5_audio_play_raw is the existing playback API (voice.c uses it
     * for Dragon TTS).  It expects PCM-16LE; OpenRouter TTS-1 returns
     * 24 kHz mono — playback drain task upsamples 24k→48k at 1:2 (the
     * existing 1:3 path is for 16k input).  See LEARNINGS for the
     * upsample buffer capacity gotcha that bit us in #260. */
    voice_solo_play_24k(pcm, len);
}

static void solo_send_audio_task(void *arg) {
    int16_t *pcm = arg; /* see voice_solo_send_audio for samples count
                           passed via globals (one-flight). */
    extern size_t s_pending_pcm_samples;
    size_t samples = s_pending_pcm_samples;

    s_busy = true;
    voice_set_state(VOICE_STATE_PROCESSING, "solo");
    tab5_debug_obs_event("solo.stt_start", "");

    char transcript[1024] = {0};
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = openrouter_stt(pcm, samples, transcript, sizeof transcript);
    int64_t dt = (esp_timer_get_time() - t0) / 1000;
    char detail[48]; snprintf(detail, sizeof detail, "ms=%lld", dt);
    tab5_debug_obs_event(err == ESP_OK ? "solo.stt_done" : "solo.error", detail);
    heap_caps_free(pcm);
    if (err != ESP_OK || transcript[0] == '\0') {
        ui_chat_show_toast("STT failed");
        goto out;
    }
    ui_chat_append_user_text(transcript);

    /* Now run the LLM + TTS leg.  We can synchronously drive
     * openrouter_chat_stream + openrouter_tts here (we own the task). */
    char *msgs = solo_build_messages_json(transcript);
    solo_chat_ctx_t cctx = {
        .acc = heap_caps_calloc(1, 8192, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
        .acc_cap = 8192,
    };
    err = openrouter_chat_stream(msgs, solo_chat_delta, &cctx);
    free(msgs);
    if (err != ESP_OK) {
        tab5_debug_obs_event("solo.error", "llm");
        ui_chat_show_toast("LLM failed");
        goto cleanup_chat;
    }
    ui_chat_finalize_assistant(cctx.acc);

    voice_set_state(VOICE_STATE_SPEAKING, "solo");
    tab5_debug_obs_event("solo.tts_start", "");
    t0 = esp_timer_get_time();
    err = openrouter_tts(cctx.acc, solo_tts_chunk, NULL);
    dt = (esp_timer_get_time() - t0) / 1000;
    snprintf(detail, sizeof detail, "ms=%lld", dt);
    tab5_debug_obs_event(err == ESP_OK ? "solo.tts_done" : "solo.error", detail);

cleanup_chat:
    heap_caps_free(cctx.acc);
out:
    voice_set_state(VOICE_STATE_READY, "solo");
    s_busy = false;
    vTaskSuspend(NULL);
}

static size_t s_pending_pcm_samples = 0;

esp_err_t voice_solo_send_audio(int16_t *pcm, size_t samples) {
    if (!pcm) return ESP_ERR_INVALID_ARG;
    if (s_busy) { heap_caps_free(pcm); return ESP_ERR_INVALID_STATE; }
    s_pending_pcm_samples = samples;
    BaseType_t r = xTaskCreate(
        solo_send_audio_task, "voice_solo_audio", 12288, pcm, 5, NULL);
    if (r != pdPASS) { heap_caps_free(pcm); return ESP_ERR_NO_MEM; }
    return ESP_OK;
}
```

`voice_solo_play_24k` is a thin shim around the existing playback ring. The
existing `tab5_audio_play_raw` (in `voice.c`, search the term to find it)
expects PCM-16LE @ 16 kHz mono and the drain task upsamples 1:3 to 48 kHz.
OpenRouter TTS-1 returns 24 kHz mono — so the shim must downsample 24→16 kHz
(simple 3:2 decimation: take 2 samples out of every 3, with a 1-tap average
to avoid aliasing) before feeding the existing ring. Add to `voice.c`
(non-static, declared in `voice.h`):

```c
/* Downsample 24 kHz mono int16 → 16 kHz and feed the playback ring.
 * Simple 3:2 averaging: out[2k]   = (in[3k]   + in[3k+1]) / 2
 *                       out[2k+1] = (in[3k+1] + in[3k+2]) / 2
 * Carry leftover (≤2 samples) across calls via a static buffer. */
void voice_solo_play_24k(const uint8_t *pcm, size_t bytes) {
    static int16_t carry[2];
    static size_t carry_n = 0;
    if (!pcm || bytes == 0) return;
    const int16_t *in = (const int16_t *)pcm;
    size_t in_samples = bytes / sizeof(int16_t);

    /* Build a contiguous span of [carry_n + in_samples] samples in PSRAM. */
    size_t span = carry_n + in_samples;
    int16_t *buf = heap_caps_malloc(span * sizeof(int16_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return;
    if (carry_n) memcpy(buf, carry, carry_n * sizeof(int16_t));
    memcpy(buf + carry_n, in, in_samples * sizeof(int16_t));

    /* Process triples; output ⌊span/3⌋ * 2 samples. */
    size_t triples = span / 3;
    int16_t *out = heap_caps_malloc(triples * 2 * sizeof(int16_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out) { heap_caps_free(buf); return; }
    for (size_t k = 0; k < triples; k++) {
        out[2 * k]     = (buf[3 * k]     + buf[3 * k + 1]) / 2;
        out[2 * k + 1] = (buf[3 * k + 1] + buf[3 * k + 2]) / 2;
    }
    /* Stash leftover. */
    carry_n = span - triples * 3;
    if (carry_n) memcpy(carry, buf + triples * 3, carry_n * sizeof(int16_t));

    tab5_audio_play_raw(out, triples * 2 * sizeof(int16_t));
    heap_caps_free(buf);
    heap_caps_free(out);
}
```

Declare in `voice.h`:

```c
void voice_solo_play_24k(const uint8_t *pcm, size_t bytes);
```

- [ ] **Step 3: Wire mic-stop hook in `voice.c`**

In the function that handles "mic stop" (search `voice_stop_listening\|mic_stop_callback`), find where it currently sends the captured PCM frames over WS. Add a branch:

```c
    if (voice_get_vmode() == VMODE_SOLO_DIRECT) {
        /* Hand the buffer to voice_solo; it owns it now. */
        int16_t *handoff = heap_caps_malloc(s_mic_capture_samples * sizeof(int16_t),
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (handoff) {
            memcpy(handoff, s_mic_capture_pcm,
                   s_mic_capture_samples * sizeof(int16_t));
            voice_solo_send_audio(handoff, s_mic_capture_samples);
        }
        return;
    }
    /* …existing Dragon-WS path unchanged below… */
```

`s_mic_capture_pcm`/`s_mic_capture_samples` are placeholders — use whatever `voice.c` calls its mic-capture buffer. `voice_get_vmode()` is the existing accessor (or `voice_get_mode()` — verify with `grep -n "voice_get_vmode\|voice_get_mode" main/voice.h`).

- [ ] **Step 4: Build + flash + run a real mic turn**

```bash
idf.py build && idf.py -p /dev/ttyACM0 flash
TAB5_TOKEN=$(grep '^TAB5_TOKEN=' .env | cut -d= -f2)
curl -sS -H "Authorization: Bearer $TAB5_TOKEN" -X POST 'http://192.168.70.128:8080/mode?m=5'
# Tap mic (orb at 360,1100), wait 3s, release
curl -sS -H "Authorization: Bearer $TAB5_TOKEN" -X POST http://192.168.70.128:8080/touch \
  -d '{"x":360,"y":1100,"action":"long_press","duration_ms":3000}'
sleep 12
curl -sS -H "Authorization: Bearer $TAB5_TOKEN" 'http://192.168.70.128:8080/events?since=0' \
  | python3 -c 'import json,sys; [print(e) for e in json.load(sys.stdin)["events"] if e["kind"].startswith("solo")]'
```

Expected: `solo.stt_start` → `solo.stt_done` → `solo.llm_done` → `solo.tts_start` → `solo.tts_done`. Audio plays on speaker. No reboot.

- [ ] **Step 5: clang-format + commit**

```bash
git-clang-format --binary clang-format-18 --diff origin/main main/openrouter_client.c main/voice_solo.c main/voice.c
ISSUE=$(grep -oE '[0-9]+' /tmp/solo_mode_issue)
git add main/openrouter_client.c main/voice_solo.c main/voice.c
git commit -m "feat(voice_solo): full mic→STT→LLM→TTS chain (refs #${ISSUE})"
git push
```

---

### Task 12: `solo_session_store` — SD-backed session persistence

**Files:**
- Create: `main/solo_session_store.h`
- Create: `main/solo_session_store.c`
- Modify: `main/voice_solo.c` (open/append/load on every turn)
- Modify: `main/CMakeLists.txt`

- [ ] **Step 1: Create `main/solo_session_store.h`**

```c
/**
 * solo_session_store — SD-backed solo-mode session persistence.
 *
 * One JSON file per session at /sdcard/sessions/<id>.json.  Format:
 *   {"id":"...","created":1730000000,"turns":[
 *      {"role":"user","content":"...","ts":...},
 *      {"role":"assistant","content":"...","ts":...},
 *      ...max 50 turns
 *   ]}
 *
 * On overflow, oldest turn is dropped (FIFO).  Sessions older than 30
 * days are not auto-pruned in v1 — manual cleanup via /sdcard.
 */
#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SOLO_SESSION_MAX_TURNS 50

esp_err_t solo_session_init(void);  /* mkdir /sdcard/sessions */

/** Open or create the active session.  ID is auto-generated from
 *  esp_random; persisted in NVS as "solo_sid" to survive reboot. */
esp_err_t solo_session_open(char *out_id, size_t cap);

/** Append a turn.  Called twice per voice turn (user, then assistant). */
esp_err_t solo_session_append(const char *role, const char *content);

/** Load up to N most-recent turns into out_json (caller-owned). */
esp_err_t solo_session_load_recent(int n, char *out_json, size_t cap);

/** Force a new session (e.g., user taps "New Chat"). */
esp_err_t solo_session_rotate(void);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: Implement `main/solo_session_store.c`**

```c
#include "solo_session_store.h"

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "cJSON.h"
#include "nvs_flash.h"
#include "nvs.h"

#define SESSIONS_DIR "/sdcard/sessions"
#define NVS_NAMESPACE "settings"
#define NVS_KEY "solo_sid"

static const char *TAG = "solo_sess";
static char s_sid[33] = {0};

esp_err_t solo_session_init(void) {
    struct stat st;
    if (stat(SESSIONS_DIR, &st) != 0) {
        if (mkdir(SESSIONS_DIR, 0775) != 0) {
            ESP_LOGE(TAG, "mkdir %s failed", SESSIONS_DIR);
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

static void gen_sid(char *out, size_t cap) {
    uint32_t a = esp_random(), b = esp_random();
    snprintf(out, cap, "%08x%08x", a, b);
}

static esp_err_t nvs_get_sid(char *out, size_t cap) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    size_t len = cap;
    err = nvs_get_str(h, NVS_KEY, out, &len);
    nvs_close(h);
    return err;
}

static esp_err_t nvs_set_sid(const char *sid) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, NVS_KEY, sid);
    if (err == ESP_OK) nvs_commit(h);
    nvs_close(h);
    return err;
}

static void session_path(char *buf, size_t cap, const char *sid) {
    snprintf(buf, cap, "%s/%s.json", SESSIONS_DIR, sid);
}

esp_err_t solo_session_open(char *out_id, size_t cap) {
    if (s_sid[0] == '\0') {
        char buf[33] = {0};
        if (nvs_get_sid(buf, sizeof buf) != ESP_OK || buf[0] == '\0') {
            gen_sid(buf, sizeof buf);
            nvs_set_sid(buf);
        }
        strncpy(s_sid, buf, sizeof s_sid - 1);
    }
    if (out_id) strncpy(out_id, s_sid, cap - 1);

    /* Create file if it doesn't exist. */
    char path[96]; session_path(path, sizeof path, s_sid);
    struct stat st;
    if (stat(path, &st) == 0) return ESP_OK;
    FILE *f = fopen(path, "w");
    if (!f) return ESP_FAIL;
    fprintf(f, "{\"id\":\"%s\",\"created\":%lld,\"turns\":[]}",
            s_sid, (long long)time(NULL));
    fclose(f);
    return ESP_OK;
}

static cJSON *load_session(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, n, f); buf[n] = '\0'; fclose(f);
    cJSON *root = cJSON_Parse(buf);
    heap_caps_free(buf);
    return root;
}

static esp_err_t save_session(const char *path, cJSON *root) {
    char *s = cJSON_PrintUnformatted(root);
    if (!s) return ESP_ERR_NO_MEM;
    FILE *f = fopen(path, "w");
    if (!f) { free(s); return ESP_FAIL; }
    fwrite(s, 1, strlen(s), f);
    fclose(f);
    free(s);
    return ESP_OK;
}

esp_err_t solo_session_append(const char *role, const char *content) {
    if (s_sid[0] == '\0') {
        esp_err_t e = solo_session_open(NULL, 0);
        if (e != ESP_OK) return e;
    }
    char path[96]; session_path(path, sizeof path, s_sid);
    cJSON *root = load_session(path);
    if (!root) return ESP_FAIL;
    cJSON *turns = cJSON_GetObjectItem(root, "turns");
    cJSON *t = cJSON_CreateObject();
    cJSON_AddStringToObject(t, "role", role);
    cJSON_AddStringToObject(t, "content", content);
    cJSON_AddNumberToObject(t, "ts", (double)time(NULL));
    cJSON_AddItemToArray(turns, t);
    while (cJSON_GetArraySize(turns) > SOLO_SESSION_MAX_TURNS) {
        cJSON_DeleteItemFromArray(turns, 0);
    }
    esp_err_t err = save_session(path, root);
    cJSON_Delete(root);
    return err;
}

esp_err_t solo_session_load_recent(int n, char *out_json, size_t cap) {
    if (s_sid[0] == '\0') {
        esp_err_t e = solo_session_open(NULL, 0);
        if (e != ESP_OK) return e;
    }
    char path[96]; session_path(path, sizeof path, s_sid);
    cJSON *root = load_session(path);
    if (!root) { strncpy(out_json, "[]", cap); return ESP_FAIL; }
    cJSON *turns = cJSON_GetObjectItem(root, "turns");
    int total = cJSON_GetArraySize(turns);
    int start = total > n ? total - n : 0;
    cJSON *out = cJSON_CreateArray();
    for (int i = start; i < total; i++) {
        cJSON *t = cJSON_GetArrayItem(turns, i);
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "role", cJSON_GetObjectItem(t, "role")->valuestring);
        cJSON_AddStringToObject(m, "content", cJSON_GetObjectItem(t, "content")->valuestring);
        cJSON_AddItemToArray(out, m);
    }
    char *s = cJSON_PrintUnformatted(out);
    strncpy(out_json, s ? s : "[]", cap - 1);
    out_json[cap - 1] = '\0';
    free(s);
    cJSON_Delete(out);
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t solo_session_rotate(void) {
    s_sid[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, NVS_KEY);
        nvs_commit(h);
        nvs_close(h);
    }
    return solo_session_open(NULL, 0);
}
```

- [ ] **Step 3: Hook into `voice_solo`**

In `voice_solo_init` add `solo_session_init();` and `solo_session_open(NULL, 0);`.

Replace `solo_build_messages_json` to load history:

```c
static char *solo_build_messages_json(const char *user_text) {
    char history[16384] = {0};
    solo_session_load_recent(20, history, sizeof history);
    cJSON *arr = cJSON_Parse(history);
    if (!arr) arr = cJSON_CreateArray();
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", "user");
    cJSON_AddStringToObject(m, "content", user_text);
    cJSON_AddItemToArray(arr, m);
    char *s = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return s;
}
```

In both `solo_send_text_task` and `solo_send_audio_task`, after success append:

```c
    solo_session_append("user", text /* or transcript */);
    solo_session_append("assistant", cctx.acc);
```

Add `#include "solo_session_store.h"` at top of `voice_solo.c`.

- [ ] **Step 4: Build + flash + verify across reboot**

```bash
idf.py build && idf.py -p /dev/ttyACM0 flash
# Send 2 turns
TAB5_TOKEN=$(grep '^TAB5_TOKEN=' .env | cut -d= -f2)
curl -sS -H "Authorization: Bearer $TAB5_TOKEN" -X POST 'http://192.168.70.128:8080/mode?m=5'
curl -sS -H "Authorization: Bearer $TAB5_TOKEN" -X POST http://192.168.70.128:8080/chat \
  -d '{"text":"my name is Emile"}'; sleep 8
curl -sS -H "Authorization: Bearer $TAB5_TOKEN" -X POST http://192.168.70.128:8080/chat \
  -d '{"text":"what is my name"}'; sleep 8
# Reboot
curl -sS -H "Authorization: Bearer $TAB5_TOKEN" -X POST http://192.168.70.128:8080/reboot
sleep 30
# After reboot, ask again — should still know
curl -sS -H "Authorization: Bearer $TAB5_TOKEN" -X POST http://192.168.70.128:8080/chat \
  -d '{"text":"do you remember my name"}'; sleep 8
curl -sS -H "Authorization: Bearer $TAB5_TOKEN" 'http://192.168.70.128:8080/voice' | python3 -m json.tool | grep last_llm
```

Expected: post-reboot reply contains "Emile".

- [ ] **Step 5: clang-format + commit**

```bash
git-clang-format --binary clang-format-18 --diff origin/main main/solo_session_store.c main/solo_session_store.h main/voice_solo.c
ISSUE=$(grep -oE '[0-9]+' /tmp/solo_mode_issue)
git add main/solo_session_store.h main/solo_session_store.c main/voice_solo.c main/CMakeLists.txt
git commit -m "feat(solo): SD-backed session persistence (refs #${ISSUE})"
git push
```

---

### Task 13: Implement `openrouter_embed`

**Files:**
- Modify: `main/openrouter_client.c`

- [ ] **Step 1: Implement the embed verb**

Replace the stub:

```c
esp_err_t openrouter_embed(const char *text, float **out_vec, size_t *out_dim) {
    if (!text || !out_vec || !out_dim) return ESP_ERR_INVALID_ARG;
    *out_vec = NULL; *out_dim = 0;

    char model[64] = {0}; tab5_settings_get_or_mdl_emb(model, sizeof model);
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "model", model);
    cJSON_AddStringToObject(body, "input", text);
    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    esp_http_client_handle_t c = openrouter_open_post("/embeddings");
    if (!c) { free(body_str); return ESP_ERR_INVALID_STATE; }
    esp_http_client_set_header(c, "Content-Type", "application/json");

    esp_err_t err = esp_http_client_open(c, strlen(body_str));
    if (err != ESP_OK) { free(body_str); goto cleanup; }
    esp_http_client_write(c, body_str, strlen(body_str));
    esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    if (status != 200) { err = ESP_FAIL; goto cleanup; }

    /* Response body, in PSRAM (≤32KB for 1536-dim float). */
    size_t cap = 32 * 1024;
    char *rbuf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rbuf) { err = ESP_ERR_NO_MEM; goto cleanup; }
    int total_read = 0;
    while (total_read < (int)cap - 1) {
        int n = esp_http_client_read(c, rbuf + total_read, cap - 1 - total_read);
        if (n <= 0) break;
        total_read += n;
    }
    rbuf[total_read] = '\0';

    cJSON *root = cJSON_Parse(rbuf);
    heap_caps_free(rbuf);
    if (!root) { err = ESP_FAIL; goto cleanup; }
    cJSON *data = cJSON_GetObjectItem(root, "data");
    if (cJSON_IsArray(data) && cJSON_GetArraySize(data) > 0) {
        cJSON *embedding = cJSON_GetObjectItem(cJSON_GetArrayItem(data, 0), "embedding");
        if (cJSON_IsArray(embedding)) {
            int dim = cJSON_GetArraySize(embedding);
            float *v = heap_caps_malloc(dim * sizeof(float),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (v) {
                for (int i = 0; i < dim; i++) {
                    cJSON *e = cJSON_GetArrayItem(embedding, i);
                    v[i] = (float)e->valuedouble;
                }
                *out_vec = v;
                *out_dim = dim;
            } else err = ESP_ERR_NO_MEM;
        }
    }
    cJSON_Delete(root);

cleanup:
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    s_inflight = NULL;
    free(body_str);
    return err;
}
```

- [ ] **Step 2: Build + smoke test (smoke happens via Task 14's RAG remember endpoint)**

```bash
idf.py build && idf.py -p /dev/ttyACM0 flash
```

- [ ] **Step 3: clang-format + commit**

```bash
git-clang-format --binary clang-format-18 --diff origin/main main/openrouter_client.c
ISSUE=$(grep -oE '[0-9]+' /tmp/solo_mode_issue)
git add main/openrouter_client.c
git commit -m "feat(openrouter): embeddings (refs #${ISSUE})"
git push
```

---

### Task 14: `solo_rag` — flat-file vector store + cosine search + `/solo/rag_test`

**Files:**
- Create: `main/solo_rag.h`
- Create: `main/solo_rag.c`
- Modify: `main/debug_server_solo.c` (add `/solo/rag_test`)
- Modify: `main/voice_solo.c` (init in `voice_solo_init`)
- Modify: `main/CMakeLists.txt`

- [ ] **Step 1: Write the failing test**

Add `tests/e2e/scenarios/test_solo_rag.py`:

```python
"""RAG remember/recall round-trip via /solo/rag_test."""
import os, requests
URL = "http://192.168.70.128:8080"
H = {"Authorization": f"Bearer {os.environ['TAB5_TOKEN']}"}

def test_remember_then_recall():
    r = requests.post(f"{URL}/solo/rag_test", headers=H, timeout=15,
                      json={"action":"remember", "text":"my favorite colour is teal"})
    r.raise_for_status()
    assert r.json()["fact_id"] >= 1

    r = requests.post(f"{URL}/solo/rag_test", headers=H, timeout=15,
                      json={"action":"recall", "query":"what colour do I like"})
    out = r.json()
    assert len(out["hits"]) >= 1
    assert "teal" in out["hits"][0]["text"].lower()
```

- [ ] **Step 2: Create `main/solo_rag.h`**

```c
/**
 * solo_rag — Tab5 on-device RAG store.
 *
 * Flat float32 records at /sdcard/rag.bin.  Each record:
 *   uint32_t magic      = 'RAGv'
 *   uint32_t fact_id    monotonic, NVS-persisted in "rag_next_id"
 *   uint32_t ts_unix
 *   uint16_t vec_dim    fixed across the file (default 1536)
 *   uint16_t text_len
 *   char     text[text_len]
 *   float    vec[vec_dim]
 *
 * Brute-force cosine over every record (linear scan); ≤10k records is
 * the soft ceiling at SD-1.5 Mbps for sub-5s recall.
 */
#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t solo_rag_init(void);

/** Embed `text` via openrouter_embed and append. */
esp_err_t solo_rag_remember(const char *text, uint32_t *out_fact_id);

typedef struct {
    uint32_t fact_id;
    uint32_t ts;
    float    score;
    char     text[256];
} solo_rag_hit_t;

/** Embed `query` and return top-K cosine hits in `hits`/`*n_hits`. */
esp_err_t solo_rag_recall(const char *query, int k,
                          solo_rag_hit_t *hits, int *n_hits);

/** Number of records currently stored. */
int solo_rag_count(void);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 3: Implement `main/solo_rag.c`**

```c
#include "solo_rag.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "openrouter_client.h"

#define RAG_PATH "/sdcard/rag.bin"
#define RAG_MAGIC 0x76474152u  /* 'RAGv' little-endian */
#define NVS_NS "settings"
#define NVS_NEXT_ID "rag_next_id"

static const char *TAG = "solo_rag";

esp_err_t solo_rag_init(void) {
    /* Touch the file (creates if absent). */
    FILE *f = fopen(RAG_PATH, "ab");
    if (!f) return ESP_FAIL;
    fclose(f);
    return ESP_OK;
}

static uint32_t next_fact_id(void) {
    nvs_handle_t h;
    uint32_t id = 1;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_get_u32(h, NVS_NEXT_ID, &id);
        nvs_set_u32(h, NVS_NEXT_ID, id + 1);
        nvs_commit(h);
        nvs_close(h);
    }
    return id;
}

esp_err_t solo_rag_remember(const char *text, uint32_t *out_fact_id) {
    if (!text) return ESP_ERR_INVALID_ARG;
    float *vec = NULL; size_t dim = 0;
    esp_err_t err = openrouter_embed(text, &vec, &dim);
    if (err != ESP_OK || !vec) return err == ESP_OK ? ESP_FAIL : err;

    FILE *f = fopen(RAG_PATH, "ab");
    if (!f) { heap_caps_free(vec); return ESP_FAIL; }

    uint32_t magic = RAG_MAGIC;
    uint32_t fact_id = next_fact_id();
    uint32_t ts = (uint32_t)time(NULL);
    uint16_t vec_dim = (uint16_t)dim;
    uint16_t text_len = (uint16_t)strlen(text);
    if (text_len > 4096) text_len = 4096;

    fwrite(&magic, sizeof magic, 1, f);
    fwrite(&fact_id, sizeof fact_id, 1, f);
    fwrite(&ts, sizeof ts, 1, f);
    fwrite(&vec_dim, sizeof vec_dim, 1, f);
    fwrite(&text_len, sizeof text_len, 1, f);
    fwrite(text, 1, text_len, f);
    fwrite(vec, sizeof(float), dim, f);
    fclose(f);
    heap_caps_free(vec);

    if (out_fact_id) *out_fact_id = fact_id;
    return ESP_OK;
}

static float cosine(const float *a, const float *b, size_t n) {
    float dot = 0, na = 0, nb = 0;
    for (size_t i = 0; i < n; i++) {
        dot += a[i] * b[i]; na += a[i]*a[i]; nb += b[i]*b[i];
    }
    if (na <= 0 || nb <= 0) return 0;
    return dot / (sqrtf(na) * sqrtf(nb));
}

esp_err_t solo_rag_recall(const char *query, int k,
                          solo_rag_hit_t *hits, int *n_hits) {
    if (!query || !hits || !n_hits || k <= 0) return ESP_ERR_INVALID_ARG;
    *n_hits = 0;

    float *qvec = NULL; size_t qdim = 0;
    esp_err_t err = openrouter_embed(query, &qvec, &qdim);
    if (err != ESP_OK || !qvec) return err == ESP_OK ? ESP_FAIL : err;

    FILE *f = fopen(RAG_PATH, "rb");
    if (!f) { heap_caps_free(qvec); return ESP_FAIL; }

    /* Top-K min-heap-ish; with k≤8 a linear scan is fine. */
    int got = 0;
    while (1) {
        uint32_t magic, fact_id, ts;
        uint16_t vd, tl;
        if (fread(&magic, sizeof magic, 1, f) != 1) break;
        if (magic != RAG_MAGIC) break;
        fread(&fact_id, sizeof fact_id, 1, f);
        fread(&ts, sizeof ts, 1, f);
        fread(&vd, sizeof vd, 1, f);
        fread(&tl, sizeof tl, 1, f);
        char text_buf[4096]; if (tl >= sizeof text_buf) tl = sizeof text_buf - 1;
        fread(text_buf, 1, tl, f); text_buf[tl] = '\0';
        float *vec = heap_caps_malloc(vd * sizeof(float),
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        fread(vec, sizeof(float), vd, f);

        if (vd != qdim) { heap_caps_free(vec); continue; }
        float score = cosine(qvec, vec, vd);
        heap_caps_free(vec);

        /* Insert if lower than worst current hit. */
        int insert_at = -1;
        if (got < k) insert_at = got;
        else {
            int worst = 0;
            for (int i = 1; i < k; i++)
                if (hits[i].score < hits[worst].score) worst = i;
            if (score > hits[worst].score) insert_at = worst;
        }
        if (insert_at >= 0) {
            hits[insert_at].fact_id = fact_id;
            hits[insert_at].ts = ts;
            hits[insert_at].score = score;
            strncpy(hits[insert_at].text, text_buf, sizeof(hits[insert_at].text) - 1);
            hits[insert_at].text[sizeof(hits[insert_at].text) - 1] = '\0';
            if (got < k) got++;
        }
    }
    fclose(f);
    heap_caps_free(qvec);

    /* Sort hits by score descending. */
    for (int i = 0; i < got - 1; i++)
        for (int j = i + 1; j < got; j++)
            if (hits[j].score > hits[i].score) {
                solo_rag_hit_t tmp = hits[i]; hits[i] = hits[j]; hits[j] = tmp;
            }

    *n_hits = got;
    return ESP_OK;
}

int solo_rag_count(void) {
    /* Cheap count: scan magic markers. */
    FILE *f = fopen(RAG_PATH, "rb");
    if (!f) return 0;
    int n = 0;
    while (1) {
        uint32_t magic;
        if (fread(&magic, sizeof magic, 1, f) != 1) break;
        if (magic != RAG_MAGIC) break;
        uint32_t fact_id, ts; uint16_t vd, tl;
        fread(&fact_id, sizeof fact_id, 1, f);
        fread(&ts, sizeof ts, 1, f);
        fread(&vd, sizeof vd, 1, f);
        fread(&tl, sizeof tl, 1, f);
        fseek(f, tl + vd * sizeof(float), SEEK_CUR);
        n++;
    }
    fclose(f);
    return n;
}
```

- [ ] **Step 4: Add `/solo/rag_test` in `debug_server_solo.c`**

```c
static esp_err_t rag_test_handler(httpd_req_t *req) {
    if (tab5_debug_check_auth(req) != ESP_OK) return ESP_FAIL;
    char body[1024] = {0};
    int n = httpd_req_recv(req, body, sizeof body - 1);
    if (n <= 0) return ESP_FAIL;
    cJSON *root = cJSON_Parse(body);
    if (!root) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    cJSON *action = cJSON_GetObjectItem(root, "action");
    cJSON *out = cJSON_CreateObject();
    if (cJSON_IsString(action) && strcmp(action->valuestring, "remember") == 0) {
        cJSON *t = cJSON_GetObjectItem(root, "text");
        uint32_t fid = 0;
        esp_err_t err = solo_rag_remember(cJSON_IsString(t) ? t->valuestring : "", &fid);
        cJSON_AddBoolToObject(out, "ok", err == ESP_OK);
        cJSON_AddNumberToObject(out, "fact_id", fid);
    } else if (cJSON_IsString(action) && strcmp(action->valuestring, "recall") == 0) {
        cJSON *q = cJSON_GetObjectItem(root, "query");
        solo_rag_hit_t hits[5] = {0};
        int got = 0;
        solo_rag_recall(cJSON_IsString(q) ? q->valuestring : "", 5, hits, &got);
        cJSON *arr = cJSON_CreateArray();
        for (int i = 0; i < got; i++) {
            cJSON *h = cJSON_CreateObject();
            cJSON_AddNumberToObject(h, "fact_id", hits[i].fact_id);
            cJSON_AddNumberToObject(h, "score", hits[i].score);
            cJSON_AddStringToObject(h, "text", hits[i].text);
            cJSON_AddItemToArray(arr, h);
        }
        cJSON_AddItemToObject(out, "hits", arr);
    } else {
        cJSON_AddBoolToObject(out, "ok", false);
        cJSON_AddStringToObject(out, "error", "unknown action");
    }
    cJSON_Delete(root);
    return tab5_debug_send_json(req, out);
}
```

Register:

```c
    static const httpd_uri_t kRagTest = {
        .uri = "/solo/rag_test", .method = HTTP_POST,
        .handler = rag_test_handler, .user_ctx = NULL,
    };
    httpd_register_uri_handler(srv, &kRagTest);
```

Add `#include "solo_rag.h"` at the top.

- [ ] **Step 5: Init in `voice_solo_init`**

In `voice_solo.c`'s `voice_solo_init` add `solo_rag_init();`.

- [ ] **Step 6: Build + flash + run test**

```bash
idf.py build && idf.py -p /dev/ttyACM0 flash
TAB5_TOKEN=$(grep '^TAB5_TOKEN=' .env | cut -d= -f2) \
python3 -m pytest tests/e2e/scenarios/test_solo_rag.py -v
```

Expected: PASS — "teal" in top hit text.

- [ ] **Step 7: clang-format + commit**

```bash
git-clang-format --binary clang-format-18 --diff origin/main main/solo_rag.c main/solo_rag.h main/debug_server_solo.c main/voice_solo.c
ISSUE=$(grep -oE '[0-9]+' /tmp/solo_mode_issue)
git add main/solo_rag.h main/solo_rag.c main/debug_server_solo.c main/voice_solo.c \
        main/CMakeLists.txt tests/e2e/scenarios/test_solo_rag.py
git commit -m "feat(solo): RAG store + cosine recall + /solo/rag_test (refs #${ISSUE})"
git push
```

---

### Task 15: Vendor quirc + create `qr_decoder` wrapper

**Files:**
- Create: `main/quirc/quirc.h`, `main/quirc/quirc.c`, `main/quirc/decode.c`, `main/quirc/identify.c`, `main/quirc/version_db.c` (downloaded verbatim)
- Create: `main/quirc/LICENSE` (the ISC license text)
- Create: `main/qr_decoder.h`
- Create: `main/qr_decoder.c`
- Modify: `main/CMakeLists.txt`

- [ ] **Step 1: Vendor quirc 1.2 from upstream**

```bash
cd ~/projects/TinkerTab/main
mkdir -p quirc
cd quirc
QUIRC_REL=v1.2
for f in lib/quirc.h lib/quirc_internal.h lib/quirc.c lib/decode.c lib/identify.c lib/version_db.c LICENSE; do
    base=$(basename "$f")
    curl -fsSL "https://raw.githubusercontent.com/dlbeer/quirc/${QUIRC_REL}/$f" -o "$base"
done
ls -1
```

Expected: 7 files present (quirc.h, quirc_internal.h, quirc.c, decode.c, identify.c, version_db.c, LICENSE).

- [ ] **Step 2: Create `main/qr_decoder.h`**

```c
/**
 * qr_decoder — quirc wrapper for Tab5.
 *
 * Owns the quirc state machine; caller feeds 8-bit grayscale frames at
 * 1280×720 (or smaller) and receives decoded payload strings.  Frame
 * working buffer (~900 KB) is allocated in PSRAM lazily on first
 * decode; freed on _free.
 */
#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct qr_decoder qr_decoder_t;

esp_err_t qr_decoder_init(qr_decoder_t **out, int width, int height);

/** Returns ESP_OK if a payload was decoded into out_buf (NUL-term).
 *  ESP_ERR_NOT_FOUND if the frame had no detectable code. */
esp_err_t qr_decoder_decode_frame(qr_decoder_t *d, const uint8_t *gray,
                                   char *out_buf, size_t out_cap);

void qr_decoder_free(qr_decoder_t *d);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 3: Create `main/qr_decoder.c`**

```c
#include "qr_decoder.h"

#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "quirc/quirc.h"

static const char *TAG = "qr_decoder";

struct qr_decoder { struct quirc *q; };

esp_err_t qr_decoder_init(qr_decoder_t **out, int width, int height) {
    if (!out) return ESP_ERR_INVALID_ARG;
    qr_decoder_t *d = heap_caps_calloc(1, sizeof(*d),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!d) return ESP_ERR_NO_MEM;
    d->q = quirc_new();
    if (!d->q) { heap_caps_free(d); return ESP_ERR_NO_MEM; }
    if (quirc_resize(d->q, width, height) < 0) {
        quirc_destroy(d->q); heap_caps_free(d); return ESP_ERR_NO_MEM;
    }
    *out = d;
    return ESP_OK;
}

esp_err_t qr_decoder_decode_frame(qr_decoder_t *d, const uint8_t *gray,
                                   char *out_buf, size_t out_cap) {
    if (!d || !gray || !out_buf || out_cap < 2) return ESP_ERR_INVALID_ARG;
    int w, h;
    uint8_t *fb = quirc_begin(d->q, &w, &h);
    memcpy(fb, gray, (size_t)w * h);
    quirc_end(d->q);
    int n = quirc_count(d->q);
    if (n <= 0) return ESP_ERR_NOT_FOUND;
    for (int i = 0; i < n; i++) {
        struct quirc_code code; struct quirc_data data;
        quirc_extract(d->q, i, &code);
        quirc_decode_error_t err = quirc_decode(&code, &data);
        if (err == QUIRC_SUCCESS) {
            size_t copy = data.payload_len < out_cap - 1
                ? (size_t)data.payload_len : out_cap - 1;
            memcpy(out_buf, data.payload, copy);
            out_buf[copy] = '\0';
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

void qr_decoder_free(qr_decoder_t *d) {
    if (!d) return;
    quirc_destroy(d->q);
    heap_caps_free(d);
}
```

- [ ] **Step 4: Add quirc + qr_decoder to `main/CMakeLists.txt`**

In SRCS list, add:

```cmake
        "quirc/quirc.c"
        "quirc/decode.c"
        "quirc/identify.c"
        "quirc/version_db.c"
        "qr_decoder.c"
```

If the existing block doesn't have an `INCLUDE_DIRS`, also add:

```cmake
                       "quirc"
```

(quirc has its own internal `quirc_internal.h` that lives in the same dir, so adding the dir to INCLUDE_DIRS makes the quirc TU discover its sibling.)

- [ ] **Step 5: Build**

```bash
idf.py fullclean
idf.py build
```

Expected: clean build. quirc has zero `printf`/stdlib surprises on ESP-IDF; if a missing-symbol error appears, check `quirc.c`'s `#include` lines (it expects `<stdio.h>` etc. — all present in IDF's newlib).

- [ ] **Step 6: clang-format + commit (skip clang-format on quirc files — vendored)**

```bash
git-clang-format --binary clang-format-18 --diff origin/main main/qr_decoder.c main/qr_decoder.h
ISSUE=$(grep -oE '[0-9]+' /tmp/solo_mode_issue)
git add main/quirc/ main/qr_decoder.h main/qr_decoder.c main/CMakeLists.txt
git commit -m "feat(solo): vendor quirc 1.2 + qr_decoder wrapper (refs #${ISSUE})"
git push
```

---

### Task 16: `ui_camera` QR scan submode

**Files:**
- Modify: `main/ui_camera.h` (declare `ui_camera_open_qr_scan`)
- Modify: `main/ui_camera.c` (continuous-decode loop, payload callback)

- [ ] **Step 1: Add prototype in `main/ui_camera.h`**

```c
/** Open camera in QR-scan mode.  Calls cb with the payload string when
 *  a valid QR is decoded; cb may close the camera. */
typedef void (*ui_camera_qr_cb_t)(const char *payload, void *ctx);
void ui_camera_open_qr_scan(ui_camera_qr_cb_t cb, void *ctx);
```

- [ ] **Step 2: Implement the QR scan loop in `main/ui_camera.c`**

Find the main capture/render loop (search `lv_canvas\|cam_render\|sccb`). Add a per-frame post-process branch:

```c
static qr_decoder_t *s_qr = NULL;
static ui_camera_qr_cb_t s_qr_cb = NULL;
static void *s_qr_ctx = NULL;

void ui_camera_open_qr_scan(ui_camera_qr_cb_t cb, void *ctx) {
    s_qr_cb = cb; s_qr_ctx = ctx;
    ui_camera_create(); /* or whatever creates the screen */
    if (!s_qr) qr_decoder_init(&s_qr, 1280, 720);
}

/* Inside the per-frame callback (RGB565 → grayscale → quirc): */
static void process_frame_for_qr(const uint16_t *rgb565, int w, int h) {
    if (!s_qr || !s_qr_cb) return;
    static uint8_t *gray = NULL;
    if (!gray) {
        gray = heap_caps_malloc(w * h, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!gray) return;
    for (int i = 0; i < w * h; i++) {
        uint16_t p = rgb565[i];
        uint8_t r = (p >> 11) & 0x1F;
        uint8_t g = (p >> 5)  & 0x3F;
        uint8_t b = p & 0x1F;
        /* Approx Y' = 0.3R + 0.59G + 0.11B in 6-bit space. */
        gray[i] = (r * 38 + g * 75 + b * 15) >> 4;
    }
    char payload[2048] = {0};
    if (qr_decoder_decode_frame(s_qr, gray, payload, sizeof payload) == ESP_OK) {
        s_qr_cb(payload, s_qr_ctx);
        s_qr_cb = NULL; /* one-shot */
    }
}
```

Hook `process_frame_for_qr` into the existing per-frame callback path.

- [ ] **Step 3: Build + flash**

```bash
idf.py build && idf.py -p /dev/ttyACM0 flash
```

- [ ] **Step 4: Manual smoke**

Print a QR with known payload (`{"or_key":"sk-or-v1-test"}`) and run:

```bash
TAB5_TOKEN=$(grep '^TAB5_TOKEN=' .env | cut -d= -f2)
curl -sS -H "Authorization: Bearer $TAB5_TOKEN" -X POST 'http://192.168.70.128:8080/navigate?screen=camera'
# Then on Tab5, tap the (still-stub) "Scan QR" button — Task 17 wires it up.
```

(End-to-end QR scan is verified after Task 17.)

- [ ] **Step 5: clang-format + commit**

```bash
git-clang-format --binary clang-format-18 --diff origin/main main/ui_camera.c main/ui_camera.h
ISSUE=$(grep -oE '[0-9]+' /tmp/solo_mode_issue)
git add main/ui_camera.h main/ui_camera.c
git commit -m "feat(ui_camera): QR scan submode (refs #${ISSUE})"
git push
```

---

### Task 17: `ui_settings` Cloud Direct section + Scan QR button

**Files:**
- Modify: `main/ui_settings.c`

- [ ] **Step 1: Add the Cloud Direct section**

Find the existing LLM model picker (around `~888-950` per CLAUDE.md). Add a new section below the existing voice-mode section. The pattern follows the existing settings section layout — read the surrounding code to match style.

```c
/* ── Cloud Direct (vmode=5) ──────────────────────────────────────────── */

static lv_obj_t *s_or_key_label;
static lv_obj_t *s_or_key_value; /* shows masked first/last 4 chars */
static lv_obj_t *s_or_models[4];
static lv_obj_t *s_or_voice;

static void on_qr_scanned(const char *payload, void *ctx) {
    (void)ctx;
    cJSON *root = cJSON_Parse(payload);
    if (!root) {
        ui_chat_show_toast("Invalid QR payload");
        return;
    }
    cJSON *k = cJSON_GetObjectItem(root, "or_key");
    if (cJSON_IsString(k)) tab5_settings_set_or_key(k->valuestring);
    cJSON *m = cJSON_GetObjectItem(root, "models");
    if (m) {
        cJSON *llm = cJSON_GetObjectItem(m, "llm");
        if (cJSON_IsString(llm)) tab5_settings_set_or_mdl_llm(llm->valuestring);
        cJSON *stt = cJSON_GetObjectItem(m, "stt");
        if (cJSON_IsString(stt)) tab5_settings_set_or_mdl_stt(stt->valuestring);
        cJSON *tts = cJSON_GetObjectItem(m, "tts");
        if (cJSON_IsString(tts)) tab5_settings_set_or_mdl_tts(tts->valuestring);
        cJSON *emb = cJSON_GetObjectItem(m, "emb");
        if (cJSON_IsString(emb)) tab5_settings_set_or_mdl_emb(emb->valuestring);
    }
    cJSON *voice = cJSON_GetObjectItem(root, "voice");
    if (cJSON_IsString(voice)) tab5_settings_set_or_voice(voice->valuestring);
    cJSON_Delete(root);
    ui_chat_show_toast("Solo mode configured");
    /* Camera close happens via the QR cb's one-shot guard. */
    ui_settings_refresh_or_section();
}

static void on_scan_qr_btn(lv_event_t *e) {
    (void)e;
    ui_camera_open_qr_scan(on_qr_scanned, NULL);
}

static void build_cloud_direct_section(lv_obj_t *parent) {
    lv_obj_t *header = lv_label_create(parent);
    lv_label_set_text(header, "Cloud Direct (vmode 5)");
    lv_obj_set_style_text_font(header, FONT_HEADING, 0);

    /* API key row — masked. */
    char key[96] = {0}; tab5_settings_get_or_key(key, sizeof key);
    s_or_key_label = lv_label_create(parent);
    lv_label_set_text(s_or_key_label, "OpenRouter Key");
    s_or_key_value = lv_label_create(parent);
    if (key[0] == '\0') {
        lv_label_set_text(s_or_key_value, "(not set)");
    } else {
        char masked[24];
        size_t n = strlen(key);
        snprintf(masked, sizeof masked, "%.4s...%s",
                 key, n > 4 ? key + n - 4 : "");
        lv_label_set_text(s_or_key_value, masked);
    }

    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_t *btn_lbl = lv_label_create(btn);
    lv_label_set_text(btn_lbl, "Scan QR");
    lv_obj_add_event_cb(btn, on_scan_qr_btn, LV_EVENT_CLICKED, NULL);

    /* Model fields (read-only labels for v1; QR is the input). */
    static const char *kModelLabels[] = {
        "LLM", "STT", "TTS", "Embeddings",
    };
    for (int i = 0; i < 4; i++) {
        char buf[96] = {0};
        switch (i) {
            case 0: tab5_settings_get_or_mdl_llm(buf, sizeof buf); break;
            case 1: tab5_settings_get_or_mdl_stt(buf, sizeof buf); break;
            case 2: tab5_settings_get_or_mdl_tts(buf, sizeof buf); break;
            case 3: tab5_settings_get_or_mdl_emb(buf, sizeof buf); break;
        }
        s_or_models[i] = lv_label_create(parent);
        char line[160];
        snprintf(line, sizeof line, "%s: %s", kModelLabels[i], buf);
        lv_label_set_text(s_or_models[i], line);
    }

    char voice[32] = {0}; tab5_settings_get_or_voice(voice, sizeof voice);
    s_or_voice = lv_label_create(parent);
    char line[64]; snprintf(line, sizeof line, "Voice: %s", voice);
    lv_label_set_text(s_or_voice, line);
}

void ui_settings_refresh_or_section(void) {
    /* Re-read NVS and refresh the labels (called after QR scan). */
    char key[96] = {0}; tab5_settings_get_or_key(key, sizeof key);
    if (s_or_key_value) {
        if (key[0] == '\0') lv_label_set_text(s_or_key_value, "(not set)");
        else {
            char masked[24]; size_t n = strlen(key);
            snprintf(masked, sizeof masked, "%.4s...%s",
                     key, n > 4 ? key + n - 4 : "");
            lv_label_set_text(s_or_key_value, masked);
        }
    }
    /* Models + voice — analogous re-fetch. */
}
```

Call `build_cloud_direct_section(parent)` from the settings-screen creation function (in the same place where the existing voice-mode section is built).

- [ ] **Step 2: Build + flash + manual scan test**

```bash
idf.py build && idf.py -p /dev/ttyACM0 flash
TAB5_TOKEN=$(grep '^TAB5_TOKEN=' .env | cut -d= -f2)
# Generate a QR via:  https://www.qr-code-generator.com/  with payload:
#   {"or_key":"sk-or-v1-YOUR-KEY","models":{"llm":"~anthropic/claude-haiku-latest","stt":"whisper-1","tts":"tts-1","emb":"text-embedding-3-small"},"voice":"alloy"}
# Print or display on phone, then on Tab5:
#   open Settings > Cloud Direct > tap "Scan QR" > point camera at QR
# Expect a toast "Solo mode configured" and key field shows sk-o...XXXX.
curl -sS -H "Authorization: Bearer $TAB5_TOKEN" 'http://192.168.70.128:8080/settings' | python3 -m json.tool | grep or_
```

Expected: NVS keys populated.

- [ ] **Step 3: clang-format + commit**

```bash
git-clang-format --binary clang-format-18 --diff origin/main main/ui_settings.c
ISSUE=$(grep -oE '[0-9]+' /tmp/solo_mode_issue)
git add main/ui_settings.c
git commit -m "feat(ui_settings): Cloud Direct section + Scan QR button (refs #${ISSUE})"
git push
```

---

### Task 18: E2E scenario `story_solo`

**Files:**
- Create: `tests/e2e/scenarios/story_solo.py`
- Modify: `tests/e2e/runner.py` (register scenario)

- [ ] **Step 1: Create `tests/e2e/scenarios/story_solo.py`**

```python
"""End-to-end story exercising vmode=5 SOLO_DIRECT.

Pre-conditions:
  - or_key is set on the Tab5 (export OPENROUTER_KEY in env, scenario
    will provision it via /settings before starting).
  - Tab5 is on a network with internet access.
"""
from __future__ import annotations

import os
import time

from tests.e2e.driver import Tab5Driver
from tests.e2e.scenarios.runner_base import scenario  # adapt to actual import

OR_KEY = os.environ.get("OPENROUTER_KEY", "")


@scenario("story_solo")
def run_story_solo(tab5: Tab5Driver) -> None:
    if not OR_KEY:
        tab5.skip("OPENROUTER_KEY not in env")
        return

    tab5.step("provision-key", lambda: tab5.post_settings({"or_key": OR_KEY}))
    tab5.step("set-mode-5", lambda: tab5.mode(5))
    tab5.step("await-ready", lambda: tab5.await_voice_state("READY", 10))

    # Text turn
    tab5.step("text-turn", lambda: tab5.chat("respond with the word 'pong' once"))
    tab5.step("await-llm-done", lambda: tab5.await_event("solo.llm_done", timeout_s=30))
    tab5.step("verify-text", lambda: assert_voice_state_contains(tab5, "pong"))

    # Voice turn (long-press mic, wait, release implicit on duration)
    tab5.step("press-mic", lambda: tab5.long_press(360, 1100, 3000))
    tab5.step("await-stt-done", lambda: tab5.await_event("solo.stt_done", timeout_s=15))
    tab5.step("await-tts-done", lambda: tab5.await_event("solo.tts_done", timeout_s=30))

    # RAG round-trip via solo turns
    tab5.step("remember", lambda: tab5.chat("remember that my dog is named Rex"))
    tab5.step("await-llm-done-2", lambda: tab5.await_event("solo.llm_done", timeout_s=30))
    tab5.step("recall", lambda: tab5.chat("what is my dog's name"))
    tab5.step("await-llm-done-3", lambda: tab5.await_event("solo.llm_done", timeout_s=30))
    tab5.step("verify-recall", lambda: assert_voice_state_contains(tab5, "rex"))

    # Mode pill cycle
    tab5.step("pill-tap", lambda: tab5.tap(620, 120))   # adjust pill coords if needed
    tab5.step("verify-mode-4", lambda: assert_mode(tab5, 4))
    tab5.step("pill-tap-back", lambda: tab5.tap(620, 120))
    tab5.step("verify-mode-5", lambda: assert_mode(tab5, 5))

    # Heap stable check
    tab5.step("heap-check", lambda: assert_heap_healthy(tab5, internal_min_kb=80))


def assert_voice_state_contains(tab5: Tab5Driver, needle: str) -> None:
    vs = tab5.voice_state()
    text = (vs.get("last_llm_text") or "").lower()
    assert needle.lower() in text, f"{needle!r} not in {text!r}"


def assert_mode(tab5: Tab5Driver, expected: int) -> None:
    vs = tab5.voice_state()
    assert vs.get("mode_id") == expected, f"mode {vs.get('mode_id')} != {expected}"


def assert_heap_healthy(tab5: Tab5Driver, internal_min_kb: int) -> None:
    h = tab5.heap()
    largest_kb = h.get("internal_largest", 0) // 1024
    assert largest_kb >= internal_min_kb, \
        f"internal_largest {largest_kb}KB < {internal_min_kb}KB"
```

Adapt the imports + decorator to whatever the existing runner expects (look at `tests/e2e/scenarios/story_smoke.py` and `runner.py` for the register pattern). The scenarios use `tab5.driver` shorthands defined in CLAUDE.md "Driver primitives".

- [ ] **Step 2: Register scenario in `tests/e2e/runner.py`**

Find the scenario list (search `story_full\|SCENARIOS`) and add:

```python
from tests.e2e.scenarios.story_solo import run_story_solo  # noqa
```

…and register it the same way `story_full` is.

- [ ] **Step 3: Run it**

```bash
cd ~/projects/TinkerTab
export TAB5_TOKEN=$(grep '^TAB5_TOKEN=' .env | cut -d= -f2)
export OPENROUTER_KEY=...    # real key
python3 tests/e2e/runner.py story_solo
```

Expected: every step passes; report at `tests/e2e/runs/<run-id>/report.md` shows green checks.

- [ ] **Step 4: Commit**

```bash
ISSUE=$(grep -oE '[0-9]+' /tmp/solo_mode_issue)
git add tests/e2e/scenarios/story_solo.py tests/e2e/runner.py
git commit -m "test(e2e): story_solo scenario for vmode=5 (refs #${ISSUE})"
git push
```

---

### Task 19: `audit_tinkerclaw.py` solo-mode assertions

**Files:**
- Modify: `tests/e2e/audit_tinkerclaw.py`

- [ ] **Step 1: Add Section 7 — Solo mode**

After Section 6 in `audit_tinkerclaw.py`, append:

```python
    section("7. Solo mode (vmode=5) — text + heap-stability")
    or_key = os.environ.get("OPENROUTER_KEY", "")
    if not or_key:
        print("  SKIP: OPENROUTER_KEY not set")
    else:
        try:
            S.post(f"{TAB5_URL}/settings", json={"or_key": or_key}, timeout=4)
            set_mode(5)
            time.sleep(2)
            h0 = health_snapshot("pre-solo-chat")
            t0 = time.monotonic()
            chat("solo audit: respond with 5 words")
            states = []
            deadline = time.monotonic() + 60
            while time.monotonic() < deadline:
                try:
                    vs = voice_state()
                    states.append((round(time.monotonic() - t0, 1), vs.get("state_name")))
                    if vs.get("state_name") == "READY" and len(states) > 3:
                        break
                except Exception:
                    pass
                time.sleep(1)
            h1 = health_snapshot("post-solo-chat")
            results["sections"].append({"solo_chat": {"timeline": states, "pre": h0, "post": h1}})
            print(f"  state timeline: {states}")
        except Exception as e:
            results["sections"].append({"solo_chat": {"error": str(e)}})
            print(f"  solo chat failed: {e}")
```

- [ ] **Step 2: Run the full audit**

```bash
cd ~/projects/TinkerTab
export TAB5_TOKEN=$(grep '^TAB5_TOKEN=' .env | cut -d= -f2)
export OPENROUTER_KEY=...
python3 tests/e2e/audit_tinkerclaw.py
```

Expected: Section 7 runs and reports a clean timeline ending in READY. JSON report at `/tmp/tc-audit/<ts>/audit.json` includes `solo_chat` block.

- [ ] **Step 3: Commit + push + open PR**

```bash
ISSUE=$(grep -oE '[0-9]+' /tmp/solo_mode_issue)
git add tests/e2e/audit_tinkerclaw.py
git commit -m "test(audit): add solo-mode section to audit_tinkerclaw (refs #${ISSUE})"
git push

# Open the PR for the whole branch (all 19 commits land as one squash):
gh pr create \
  --title "Tab5 Solo Mode (vmode=5 SOLO_DIRECT) — Phase 1 (refs #${ISSUE})" \
  --body "$(cat <<EOF
## Summary
Implements Phase 1 of \`docs/superpowers/specs/2026-05-08-tab5-solo-mode-design.md\`:
Dragon-independent voice mode that talks directly to OpenRouter for STT,
LLM streaming, TTS, and embeddings, with QR-scan API key entry, on-device
sessions/RAG, and a mode pill that taps to cycle K144 ↔ SOLO.

19 commits — one per task in \`docs/superpowers/plans/2026-05-08-tab5-solo-mode.md\`.

## Test plan
- [ ] \`idf.py build\` clean
- [ ] \`tests/e2e/runner.py story_solo\` green end-to-end
- [ ] \`audit_tinkerclaw.py\` Section 7 reports clean state timeline + heap stable
- [ ] Manual: Settings → Cloud Direct → Scan QR provisions key, masked label updates
- [ ] Manual: 5 sequential solo turns, internal heap delta < 200 KB
- [ ] Manual: WiFi pulled mid-stream → recovers within 30 s, no reboot
- [ ] Manual: Mode pill K144 → SOLO → K144 round-trip
- [ ] Manual: Sessions persist across reboot (resume from /sdcard/sessions/)
- [ ] Manual: \`remember\` → \`recall\` round-trip via solo turns

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```

---

## Acceptance criteria (from spec, mapped to plan tasks)

| Spec acceptance | Verifying task |
|-----------------|----------------|
| `idf.py build` clean with new modules | Tasks 3, 6, 7, 12, 14, 15 (every C-file add) |
| QR scan completes < 5s, NVS updated | Task 17 manual + Task 18 indirectly |
| First-turn solo voice: STT < 3s, LLM TTFT < 2s, TTS < 1s post-DONE | Task 11 obs events expose latencies; Task 18 timing assertions |
| 5 sequential turns, total memory delta < 200KB | Task 18 `assert_heap_healthy` |
| WiFi pulled mid-stream → recovers, no reboot | Manual test in Task 19 PR checklist |
| Mode pill K144 → SOLO → K144 round-trips | Task 5 manual + Task 18 |
| Sessions persist across reboot | Task 12 |
| `remember` → `recall` round-trip | Task 14 + Task 18 |
| Existing modes 0–4 untouched | Existing `story_full` regression run before merge (no new code paths in WS path) |
| No new clang-format violations | Each task ends with `git-clang-format --diff origin/main` |

## Risks logged inline (each was lifted from the spec's risks section)

- **TTS audio format:** Task 11 hardcodes `response_format=wav`. If OpenRouter returns MP3 for some providers, the chunk callback will see RIFF-less bytes and the playback ring will play noise. Verify on-hardware in Task 11 by checking the first 4 response bytes ("RIFF"); if missing, log + abort + surface toast. MP3 decoder (helix-mp3, ~50 KB) is a Phase 2 add-on if needed.
- **STT streaming:** v1 is post-stop transcribe (Task 10 uploads the full mic buffer). UX is "stop and wait 1-3s for transcript". OpenRouter shipping streaming STT is a future backfill, no plan change.
- **Cert bundle coverage:** Task 7 uses `esp_crt_bundle_attach`. CI build flag `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y` must remain on (it's already on per `sdkconfig.defaults`). If a TLS handshake fails after flash, run `openssl s_client -connect openrouter.ai:443 -showcerts` and inspect the chain root — must be one of the bundled CAs.
- **Quirc license:** ISC, vendored under `main/quirc/LICENSE` (Task 15 Step 1 fetches it).
- **Mode pill discoverability:** Task 5 adds the SOLO label; first-encounter hint toast deferred to Phase 2.
- **Cost guard:** Existing `cap_mils`/`spent_mils` NVS keys are not wired to the solo path in Phase 1 — solo turns are uncapped. Phase 1.5 candidate.

---

## Out-of-Phase-1 (deferred, file follow-up issues post-merge)

- `ui_onboarding.c` "Use Dragon at home, or cloud direct?" question — Phase 1.5
- `connection_mode` toggle (`conn_mode` NVS key) — Phase 2
- Drop Dragon prompts entirely for fresh standalone-SKU devices — Phase 3
- Multi-provider direct (Anthropic / OpenAI native paths) — Phase 3
- On-device skill scaffolding for solo (timesense Pomodoro on Tab5) — Phase 3
- On-device embedding model (Sherpa-onnx MiniLM) so RAG works without internet — Phase 3
- Cost guard wiring (`cap_mils`/`spent_mils` debits per OpenRouter response header `x-ratelimit-remaining-credits`) — Phase 1.5
- Most-recent-1000 vectors in PSRAM hot cache for sub-second recall — Phase 2
