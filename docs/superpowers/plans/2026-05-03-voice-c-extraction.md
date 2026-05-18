# voice.c Extraction Implementation Plan (Wave 23 · TT #331)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Drop `main/voice.c` from 3,846 → ~2,470 LOC by extracting WS frame routing into `voice_ws_proto.{c,h}` and the five-tier voice-mode dispatch into `voice_modes.{c,h}`, with zero observable behavior change verified on physical Tab5 hardware via the e2e harness.

**Architecture:** Two sibling C modules in `main/` (same shape as the prior `voice_billing` / `voice_widget_ws` / `voice_onboard` extracts). `voice_ws_proto.c` owns the WS event callback + JSON RX dispatcher + binary-magic dispatcher + send wrappers + REGISTER frame builder. `voice_modes.c` owns `voice_send_config_update*` + `voice_get_mode` + the five-tier text-routing decision (pure-K144 vs Local-mode-failover vs Dragon path). voice.c keeps the state machine, mic capture task, playback drain task, listening/dictation/call lifecycles, reconnect watchdog, and the public API surface.

**Tech Stack:** ESP-IDF v5.5.2, C11, esp_websocket_client, cJSON, FreeRTOS, LVGL 9.x. No new dependencies.

**Verification discipline:** No host-side C unit test framework exists. The two test loops are:
1. **Build is the type-checker** — `idf.py build` fails on any missed call site or stale forward decl.
2. **E2E user stories are the behavior spec** — `python3 tests/e2e/runner.py <story>` against a flashed Tab5 must match the pre-extract baseline pass count + heap profile.

**Branching strategy:** One branch per PR (`refactor/voice-ws-proto-extract` then `refactor/voice-modes-extract`). Each PR is a SINGLE squash-merge to `main` per the workflow in `CLAUDE.md` ("Workflow" section). Inside the branch we do many small commits; squash on merge.

---

## Pre-flight context for the implementer

### Environment

```bash
# IDF must be sourced once per shell:
. /home/rebelforce/esp/esp-idf/export.sh   # IDF v5.5.2

# Tab5 reachability:
export TAB5_URL=http://192.168.1.90:8080
export TAB5_TOKEN=05eed3b13bf62d92cfd8ac424438b9f2   # from CLAUDE.md
ping -c 2 192.168.1.90                                # MUST respond
curl -s "$TAB5_URL/info" | python3 -m json.tool       # device discovery
```

### Tab5 hardware

- M5Stack Tab5 (ESP32-P4), wired via USB to `/dev/ttyACM0`, on LAN at 192.168.1.90.
- Dragon at 192.168.1.91 must be reachable for any voice-path test (otherwise the WS sits in CONNECTING). Verify with `ssh radxa@192.168.1.91 sudo systemctl is-active tinkerclaw-voice` → `active`.
- The Tab5 may need a watchdog reset after flashing if it sticks in ROM download mode:
  ```bash
  python -m esptool --chip esp32p4 -p /dev/ttyACM0 \
      --before no_reset --after watchdog_reset read_mac
  ```

### Files this plan touches

**Created:**
- `main/voice_ws_proto.h` — public API for the WS proto module
- `main/voice_ws_proto.c` — implementation
- `main/voice_modes.h` — public API for the modes module
- `main/voice_modes.c` — implementation

**Modified:**
- `main/voice.c` — symbol removal + new `#include`s + call-site updates
- `main/voice.h` — minor: a few statics promoted to public for cross-module access
- `main/CMakeLists.txt` (line 35) — add `voice_ws_proto.c` `voice_modes.c` to the `SRCS` list
- `CLAUDE.md` "Key Files → Dragon voice link" subsection — add the two new modules
- `~/.claude/projects/-home-rebelforce/memory/project_solid_audit_wave23.md` — append PR-merge entries
- `docs/AUDIT-architecture-2026-05-01.md` (or whichever audit doc tracks #331) — flip A1 from "open" to "shipped via PR #N + #N+1"

### Source map — what moves where

The grep + section-header pass produced this canonical map. Line numbers are AS-OF the current `main` HEAD (`95d732f`); they will drift as commits land but the symbol names are stable.

| voice.c lines | Symbol | Destination |
|---------------|--------|-------------|
| L525-543 | `voice_ws_send_text` (static) | voice_ws_proto.c (becomes non-static, declared in .h) |
| L545-572 | `voice_ws_send_binary` (static) | voice_ws_proto.c (becomes non-static, declared in .h) |
| L574-577 | `voice_ws_send_binary_public` | voice_ws_proto.c (already public; .h decl moves to voice_ws_proto.h via re-export, voice.h keeps the declaration too for source compat with existing callers) |
| L587-672 | `voice_ws_send_register` (static) | voice_ws_proto.c (becomes non-static, declared in .h) |
| L876-879 | `voice_debug_inject_text` | voice_ws_proto.c (it's the debug entry to handle_text_message) |
| L881-1479 | `handle_text_message` (static) | voice_ws_proto.c (becomes non-static, declared in .h as `voice_ws_proto_handle_text`) |
| L1486-1499 | `upsample_16k_to_48k` (static helper) | voice_ws_proto.c (stays static — only used by handle_binary_message) |
| L1500-1582 | `handle_binary_message` (static) | voice_ws_proto.c (becomes non-static, declared in .h as `voice_ws_proto_handle_binary`) |
| L1586-1940 | `voice_ws_event_handler` (static) | voice_ws_proto.c (becomes non-static, declared in .h as `voice_ws_proto_event_handler`) |
| L1944-1948 | `voice_build_local_uri` (static) | voice_ws_proto.c (becomes non-static, declared in .h as `voice_ws_proto_build_local_uri`) |
| L3019-3022 | `voice_get_mode` | voice_modes.c (already public; voice.h decl stays) |
| L3478-3516 | `voice_send_widget_action` | voice_ws_proto.c (it builds a JSON frame and calls voice_ws_send_text — pure WS proto concern) |
| L3518-3543 | `voice_send_config_update_ex` | voice_modes.c |
| L3545-3548 | `voice_send_config_update` | voice_modes.c |
| L3373-3471 (subset) | The five-tier mode-routing branches inside `voice_send_text`: `VMODE_LOCAL_ONBOARD` always-onboard branch + `VMODE_LOCAL` failover-grace branch | voice_modes.c (extracted as a pure helper `voice_modes_route_text(const char *text, voice_modes_route_result_t *out)` returning an enum: ROUTE_K144_CHAIN_BUSY / ROUTE_K144_OK / ROUTE_K144_FAILED / ROUTE_DRAGON_PATH; voice.c's `voice_send_text` just dispatches on the result) |

### Statics that need promotion (cross-module access)

These are currently `static` in voice.c but the extracted code reads/writes them. Two options: (a) promote to non-static + add to `voice.h` (preferred when the symbol is logically a voice-module-wide datum), or (b) add a getter/setter in voice.h.

| Symbol | Read by | Write by | Approach |
|--------|---------|----------|----------|
| `s_ws` (WS handle) | voice_ws_proto.c (sends + event handler) | voice.c (connect/disconnect) + voice_ws_proto.c (event handler) | Promote to non-static, declare `extern esp_websocket_client_handle_t volatile g_voice_ws;` in voice.h. Rename for clarity in the same commit. |
| `s_state_mutex` | voice_ws_proto.c (RX guard) | voice.c (init) | Promote to non-static, declare `extern SemaphoreHandle_t g_voice_state_mutex;` in voice.h. |
| `s_session_gen` | voice_ws_proto.c (async-call guard) + voice_modes.c | voice.c (set on disconnect) | Add `voice_get_session_gen()` getter in voice.h (read-only access is enough). |
| `s_voice_mode` | voice_modes.c (route decision; reads its own static directly) | voice.c (start_listening / call_audio_start / stop_listening) | Move the static itself into voice_modes.c. Add `voice_modes_set_internal(voice_mode_t)` setter in voice_modes.h for voice.c's lifecycle calls. The existing public `voice_get_mode()` moves into voice_modes.c too and becomes a one-line `return s_voice_mode;` (its declaration in voice.h stays — public API contract is unchanged). |
| `s_send_lock`, `s_register_lock`, etc. | voice_ws_proto.c (every send) | voice.c (init only) | Promote to non-static. |
| `s_dragon_host`, `s_dragon_port`, `s_session_id`, `s_device_id` | voice_ws_proto.c (REGISTER frame) | voice.c (connect) | Promote to non-static, declare extern. |
| `s_last_ws_alive_ms` (failover gate) | voice_modes.c | voice.c (event handler stamps it) | Add `voice_get_last_ws_alive_ms()` getter in voice.h. |
| `s_voice_mode_locked` etc. | voice_modes.c | voice.c | Promote per the same pattern. |

> The implementer should grep for `static\s+\([a-zA-Z_]\+\s\+\)\+s_` in voice.c BEFORE moving any function, to build the exact list of statics each extracted function touches. Update the table above if anything was missed — the audit was pattern-matched, not line-by-line.

### Story coverage matrix

Map each extracted concern to the story that exercises it on hardware. Run these BEFORE any extract to capture a green baseline.

| Concern | Story to run | Story duration |
|---------|--------------|----------------|
| WS event handler (CONNECTED → REGISTER → READY transition) | `story_smoke` | ~5 min |
| JSON dispatcher: `stt`, `stt_partial`, `llm`, `llm_done`, `tts_*` | `story_smoke` (one Local turn) + `story_full` (4 modes) | ~15 min combined |
| JSON dispatcher: `media`, `card`, `audio_clip`, `text_update` (rich media) | `story_full` (Cloud turn renders rich media) | (covered above) |
| JSON dispatcher: `widget_*` (live, card, list, chart, prompt, dismiss) | No dedicated story today — use `story_wave10_ui_skills` (1738) which exercises widget surfaces | ~5 min |
| Binary dispatcher: untagged PCM (TTS audio) | `story_smoke` (Local turn produces PCM TTS) | (covered above) |
| Binary dispatcher: VID0 (downlink JPEG video) | `story_full` does a brief video pane probe; full call exercises this | (covered above) |
| Binary dispatcher: AUD0 (in-call mic uplink + downlink) | Manual: `POST /call/start` + observe `/video` stats | ~30 sec |
| Send wrappers (text + binary) | Implicit in every story — any text turn fires `voice_ws_send_text` | (covered above) |
| REGISTER frame on reconnect | `story_stress` does a forced reconnect cycle | ~20 min |
| Five-tier mode dispatch (config_update) | `story_full` rotates through all 4 modes, each fires a config_update | (covered above) |
| Five-tier text routing: VMODE_LOCAL_ONBOARD always-K144 | `story_onboard` (K144 must be warm — skip otherwise) | ~30 sec |
| Five-tier text routing: VMODE_LOCAL → K144 failover | `story_wave7_onboard_polish` (line 1253) | ~3 min |
| Reconnect after WS drop | `story_stress` | (covered above) |

**Minimum baseline run before any extract:** `story_smoke` + `story_full` + `story_onboard`. About 16 min total. Save the report directory paths for diff comparison after the extract.

---

## Phase 0 — Pre-flight baseline

### Task 0.1: Confirm working tree + Dragon are healthy

**Files:** none (read-only checks)

- [ ] **Step 1: Check git working tree is clean**

```bash
cd ~/projects/TinkerTab
git status --short
```

Expected: empty output OR only untracked files (`docs-site/`, `docs/AUDIT-solid-2026-05-03.md`). Any modified-tracked files → stop and reconcile with the user before proceeding.

- [ ] **Step 2: Confirm we're on main**

```bash
git branch --show-current
```

Expected: `main`. If not → `git switch main`.

- [ ] **Step 3: Pull latest main**

```bash
git fetch origin && git merge --ff-only origin/main
```

Expected: "Already up to date" or fast-forward. If a non-FF is reported, stop — the user has local main commits that need investigation.

- [ ] **Step 4: Confirm Tab5 is reachable**

```bash
ping -c 2 192.168.1.90
curl -sS -H "Authorization: Bearer $TAB5_TOKEN" http://192.168.1.90:8080/voice | python3 -m json.tool
```

Expected: ping responds + `/voice` returns a JSON blob with `"connected": true` (or "false" with state=`CONNECTING` if Dragon is mid-restart).

- [ ] **Step 5: Confirm Dragon is up**

```bash
sshpass -p 'thedragon' ssh -o StrictHostKeyChecking=no radxa@192.168.1.91 \
    sudo systemctl is-active tinkerclaw-voice
```

Expected: `active`. If `inactive` or `failed` → restart with `sudo systemctl restart tinkerclaw-voice` and wait 60s for Moonshine/Piper/Ollama to load, then re-check.

### Task 0.2: Capture green E2E baseline on the current `main` HEAD

**Files:** none (produces baseline reports for diff)

- [ ] **Step 1: Run `story_smoke`**

```bash
cd ~/projects/TinkerTab
python3 tests/e2e/runner.py story_smoke 2>&1 | tee /tmp/baseline-smoke.log
```

Expected: trailing line says `PASS: N/N steps`. Note the run dir from the log (`tests/e2e/runs/story_smoke-<ts>/`). Save the path:

```bash
BASELINE_SMOKE=$(ls -td tests/e2e/runs/story_smoke-* | head -1)
echo "BASELINE_SMOKE=$BASELINE_SMOKE"
```

- [ ] **Step 2: Run `story_full`**

```bash
python3 tests/e2e/runner.py story_full 2>&1 | tee /tmp/baseline-full.log
BASELINE_FULL=$(ls -td tests/e2e/runs/story_full-* | head -1)
echo "BASELINE_FULL=$BASELINE_FULL"
```

Expected: `PASS: N/N steps`. Document N — this is the bar that must hold post-extract.

- [ ] **Step 3: Run `story_onboard` only if K144 is warm**

```bash
curl -sS -H "Authorization: Bearer $TAB5_TOKEN" http://192.168.1.90:8080/m5 \
    | python3 -c "import json,sys; d=json.load(sys.stdin); print(d.get('failover_state_name'))"
```

If output is `ready` → run it. Otherwise skip (the story self-skips):

```bash
python3 tests/e2e/runner.py story_onboard 2>&1 | tee /tmp/baseline-onboard.log
BASELINE_ONBOARD=$(ls -td tests/e2e/runs/story_onboard-* | head -1)
```

- [ ] **Step 4: Capture heap floor**

```bash
curl -sS -H "Authorization: Bearer $TAB5_TOKEN" http://192.168.1.90:8080/heap \
    | python3 -m json.tool > /tmp/baseline-heap.json
cat /tmp/baseline-heap.json
```

Expected: a JSON blob with `internal_free`, `internal_largest_free_block`, `psram_free`. Anchor numbers — extracts must not introduce a >5% regression in `internal_largest_free_block` after a fresh boot + smoke run.

- [ ] **Step 5: Record baseline expectations in a scratch file**

```bash
cat > /tmp/voice-extract-baseline.md << 'EOF'
# Pre-extract baseline for #331 (capture date: $(date -u +%Y-%m-%dT%H:%M:%SZ))

- story_smoke: <NN/NN> steps
- story_full:  <NN/NN> steps
- story_onboard: <NN/NN> steps (or SKIPPED — K144 not warm)
- internal_free: <bytes>
- internal_largest_free_block: <bytes>
EOF
${EDITOR:-vi} /tmp/voice-extract-baseline.md
```

Acceptance: baseline file exists with concrete numbers. This is the contract every subsequent extract must honor.

---

## Phase 1 — PR #1: Extract `voice_ws_proto.{c,h}`

### Task 1.1: Create branch

**Files:** none (git only)

- [ ] **Step 1: Branch off main**

```bash
git switch -c refactor/voice-ws-proto-extract
```

### Task 1.2: Open the tracking sub-issue

**Files:** none (GitHub only)

- [ ] **Step 1: Open issue**

```bash
gh issue create --title "refactor(voice): extract voice_ws_proto.{c,h} (refs #331)" \
    --body "$(cat <<'EOF'
## Scope

Extract the WS frame routing layer out of `main/voice.c` into a new sibling module `main/voice_ws_proto.{c,h}`.  Same shape as the prior `voice_billing` / `voice_widget_ws` / `voice_onboard` extracts (see PRs #349 / #351 / #327).

## What moves

- `voice_ws_send_text`, `voice_ws_send_binary`, `voice_ws_send_binary_public` — send wrappers
- `voice_ws_send_register` — REGISTER frame builder/sender
- `voice_debug_inject_text` — debug entry to the JSON dispatcher
- `handle_text_message` (599 LOC JSON RX dispatcher) — exposed as `voice_ws_proto_handle_text`
- `handle_binary_message` (binary magic VID0/AUD0/untagged-PCM dispatcher) — exposed as `voice_ws_proto_handle_binary`
- `voice_ws_event_handler` (esp_websocket_client callback) — exposed as `voice_ws_proto_event_handler`
- `upsample_16k_to_48k` (helper used only by binary dispatcher — stays static in the new module)
- `voice_build_local_uri` (URI helper) — exposed as `voice_ws_proto_build_local_uri`
- `voice_send_widget_action` (builds JSON + calls send_text — pure WS-proto concern)

## What stays in voice.c

- State machine (`voice_set_state` etc.)
- Mic capture task + playback drain task
- Listening / dictation / call lifecycles
- Reconnect watchdog
- Public API surface (voice_init, voice_connect*, voice_start_listening, etc.)

## Verification

- Build is clean (`idf.py build`)
- E2E baseline holds: story_smoke + story_full + story_onboard match pre-extract pass count
- Heap floor (internal_largest_free_block after smoke) within 5% of baseline
- No regression in CLAUDE.md "story coverage matrix" (see plan doc)

## Plan doc

`docs/superpowers/plans/2026-05-03-voice-c-extraction.md` — Phase 1 (Tasks 1.1 – 1.10)

Refs #331.
EOF
)"
```

Expected: prints the new issue URL. Note the issue number (call it `$WSPROTO_ISSUE`).

### Task 1.3: Carve the new header (no symbol moves yet)

**Files:**
- Create: `main/voice_ws_proto.h`

- [ ] **Step 1: Write the header skeleton**

```c
/**
 * Voice WebSocket protocol layer (Tab5 ↔ Dragon).
 *
 * Owns the on-the-wire dispatch:
 *   - JSON RX (text frames) → handle_text routes to typed handlers in voice.c
 *   - Binary RX (mag-tagged VID0/AUD0 + untagged PCM) → handle_binary
 *   - WS event callback (CONNECTED / DISCONNECTED / DATA / ERROR)
 *   - TX wrappers around esp_websocket_client_send_{text,bin}
 *   - REGISTER frame builder
 *
 * Wave 23 SOLID-audit closure for TT #331 (extract A1).  Pre-extract
 * this all lived inline in voice.c at L525-1948; voice.c is left with
 * the state machine + mic capture + listening/dictation/call lifecycles.
 */
#pragma once

#include "esp_err.h"
#include "esp_websocket_client.h"
#include "esp_event.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* TX wrappers (formerly static helpers in voice.c) */
esp_err_t voice_ws_send_text(const char *msg);
esp_err_t voice_ws_send_binary(const void *data, size_t len);
esp_err_t voice_ws_send_register(void);

/* RX dispatchers — invoked by voice_ws_proto_event_handler. */
void voice_ws_proto_handle_text(const char *data, int len);
void voice_ws_proto_handle_binary(const char *data, int len);

/* esp_websocket_client event callback — register with
 * esp_websocket_register_events from voice_ws_start_client in voice.c. */
void voice_ws_proto_event_handler(void *arg, esp_event_base_t base,
                                  int32_t event_id, void *event_data);

/* URI helper (LAN target builder).  out_cap must be at least 64. */
void voice_ws_proto_build_local_uri(char *out, size_t out_cap,
                                    const char *dragon_host, uint16_t dragon_port);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: Verify it compiles standalone**

```bash
. /home/rebelforce/esp/esp-idf/export.sh
cd ~/projects/TinkerTab
idf.py build 2>&1 | tail -20
```

Expected: build still passes (the .h is unused so far). If it fails, the issue is in the existing tree, not the change — investigate before continuing.

- [ ] **Step 3: Commit**

```bash
git add main/voice_ws_proto.h
git commit -m "refactor(voice): carve voice_ws_proto.h skeleton (refs #$WSPROTO_ISSUE, refs #331)"
```

### Task 1.4: Move TX send wrappers (`voice_ws_send_text`, `_send_binary`, register)

**Files:**
- Create: `main/voice_ws_proto.c` (initial creation)
- Modify: `main/voice.c` — delete L525-577 + L587-672 (send wrappers + register)
- Modify: `main/CMakeLists.txt` line 35 — add `voice_ws_proto.c` to SRCS

- [ ] **Step 1: Create voice_ws_proto.c with the moved TX wrappers**

Copy verbatim out of voice.c:
- `voice_ws_send_text` body (L525-543, current line numbers — re-grep before moving)
- `voice_ws_send_binary` body (L545-572)
- `voice_ws_send_register` body (L587-672)
- All statics they touch: `s_send_lock`, `s_register_lock`, etc. (grep for `static\s.*\bs_send_lock\b` etc. in voice.c)

Each function loses its `static` qualifier (now declared in the header). The `s_*` locks they use stay as file-statics IN voice_ws_proto.c — they're internal to the WS-proto layer. The WS handle `s_ws` and connection-context statics (`s_dragon_host`, `s_session_id`, `s_device_id`) remain in voice.c but are referenced via `extern` from voice_ws_proto.c (next step adds the externs to voice.h).

Top-of-file includes for voice_ws_proto.c:

```c
#include "voice_ws_proto.h"
#include "voice.h"             /* extern g_voice_ws + s_state_mutex etc. */
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "cJSON.h"
#include "settings.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "tab5_voice_ws";
```

- [ ] **Step 2: Promote `s_ws` and shared statics to voice.h externs**

In voice.c, find the line `static esp_websocket_client_handle_t volatile s_ws = NULL;` (around L199). Rename to `g_voice_ws` and remove `static`. In voice.h, add (in a new section near the bottom labeled `// Cross-module accessors (voice_ws_proto.c)`):

```c
/* Promoted from static in voice.c so voice_ws_proto.c can send / inspect.
 * volatile because the WS event task (Core 1) writes it on connect/disconnect
 * while the LVGL thread (Core 0) reads it for liveness checks. */
extern esp_websocket_client_handle_t volatile g_voice_ws;

/* Shared connection context — voice.c's voice_connect populates these,
 * voice_ws_send_register reads them when stamping the REGISTER frame. */
extern char g_voice_dragon_host[];
extern uint16_t g_voice_dragon_port;
extern char g_voice_session_id[];   /* persisted via NVS */
extern char g_voice_device_id[];

/* Read-only accessor for the session generation counter (voice_ws_proto.c
 * uses it to guard async lv_async_call dispatches against stale RX). */
uint32_t voice_get_session_gen(void);
```

In voice.c, rename `s_ws` → `g_voice_ws`, `s_dragon_host` → `g_voice_dragon_host`, etc. (use `replace_all` style).

- [ ] **Step 3: Add voice_ws_proto.c to CMakeLists.txt**

In `main/CMakeLists.txt` at line 35, change:

```cmake
"voice.c" "voice_codec.c" "voice_video.c" "voice_widget_ws.c" "voice_billing.c" "mode_manager.c"
```

to:

```cmake
"voice.c" "voice_ws_proto.c" "voice_codec.c" "voice_video.c" "voice_widget_ws.c" "voice_billing.c" "mode_manager.c"
```

- [ ] **Step 4: Build**

```bash
idf.py build 2>&1 | tail -30
```

Expected: clean build. If linker errors about undefined refs to `voice_ws_send_text` etc., the call sites in voice.c (mic_capture_task at L2089/L2095/L2173/L2201, voice_start_listening at L2902, voice_start_dictation at L3000, voice_clear_history at L3112, voice_stop_listening at L3180, voice_cancel at L3221, voice_send_text at L3469, voice_send_widget_action at L3510, voice_send_config_update_ex at L3540) need an `#include "voice_ws_proto.h"` at the top of voice.c. Add it next to the other voice_* includes.

- [ ] **Step 5: Flash to Tab5**

```bash
idf.py -p /dev/ttyACM0 flash 2>&1 | tail -20
```

Expected: `Hash of data verified.` and `Leaving...` lines. If it sticks in ROM mode, run the watchdog reset (see "Pre-flight" section).

- [ ] **Step 6: Wait for boot and confirm WS connects**

```bash
sleep 25
curl -sS -H "Authorization: Bearer $TAB5_TOKEN" http://192.168.1.90:8080/voice \
    | python3 -m json.tool
```

Expected: `"connected": true`, `"state_name": "READY"` (or `"LISTENING"` if mid-test). If state is stuck in `CONNECTING` for >30s after boot, check serial output for stack/linker issues — a missed promotion of a static is the most common cause.

- [ ] **Step 7: Smoke E2E**

```bash
python3 tests/e2e/runner.py story_smoke 2>&1 | tail -5
```

Expected: same pass count as baseline. Diff the report:

```bash
NEW_SMOKE=$(ls -td tests/e2e/runs/story_smoke-* | head -1)
diff <(jq '.steps[].name' $BASELINE_SMOKE/report.json) \
     <(jq '.steps[].name' $NEW_SMOKE/report.json)
```

Expected: empty diff (same step list). Pass counts in `report.json`'s `summary` field must match.

- [ ] **Step 8: Commit**

```bash
git add main/voice.c main/voice.h main/voice_ws_proto.c main/CMakeLists.txt
git commit -m "refactor(voice): move WS send wrappers + REGISTER to voice_ws_proto (refs #$WSPROTO_ISSUE)"
```

### Task 1.5: Move `voice_send_widget_action`

**Files:**
- Modify: `main/voice_ws_proto.c` — append the function
- Modify: `main/voice.c` — delete L3478-3516
- Modify: `main/voice.h` — no change (declaration already there as public API)

- [ ] **Step 1: Move the function verbatim**

Copy `voice_send_widget_action` (L3478-3516 in current main) from voice.c into voice_ws_proto.c. It uses cJSON to build the JSON payload then calls `voice_ws_send_text` — both are local to the new module so no external symbol resolution needed.

- [ ] **Step 2: Build**

```bash
idf.py build 2>&1 | tail -10
```

Expected: clean.

- [ ] **Step 3: Flash + smoke**

```bash
idf.py -p /dev/ttyACM0 flash 2>&1 | tail -5
sleep 25
python3 tests/e2e/runner.py story_smoke 2>&1 | tail -3
python3 tests/e2e/runner.py story_wave10_ui_skills 2>&1 | tail -3
```

Expected: both stories pass at baseline counts. story_wave10 exercises widget_action emission.

- [ ] **Step 4: Commit**

```bash
git add main/voice.c main/voice_ws_proto.c
git commit -m "refactor(voice): move voice_send_widget_action to voice_ws_proto (refs #$WSPROTO_ISSUE)"
```

### Task 1.6: Move `handle_text_message` (the 599-LOC giant)

**Files:**
- Modify: `main/voice_ws_proto.c` — append the function as `voice_ws_proto_handle_text`
- Modify: `main/voice.c` — delete L876-1479 (`voice_debug_inject_text` + `handle_text_message` body)

- [ ] **Step 1: Identify cross-module symbols the function touches**

Before moving, grep the function body for symbols defined elsewhere in voice.c:

```bash
sed -n '881,1479p' main/voice.c \
    | grep -oE '\bs_[a-z_][a-zA-Z0-9_]*\b' | sort -u
```

Expected output: a list like `s_dictation_text`, `s_dictation_text_len`, `s_dictation_partial_acc`, `s_llm_text`, `s_stt_text`, `s_last_transcript`, `s_state_mutex`, `s_voice_mode`, `s_session_gen`, `s_ws`, `s_link_health`, `s_vision_capable`, `s_vision_model`, `s_per_frame_mils`, `s_dictation_title`, `s_dictation_summary`, `s_queued_text_pending`, `s_queued_text_buf`, `s_call_audio_muted`, `s_last_ws_alive_ms`, etc. Each one is either:

  - **Already promoted in Task 1.4** (e.g. `s_ws` → `g_voice_ws`)
  - **A buffer that should stay private to voice.c** — wrap with a getter/setter in voice.h (e.g., `voice_set_stt_text(const char*)`, `voice_set_llm_text_partial(const char*)` already exists at L3361)
  - **A flag that voice_ws_proto only needs read access to** — add a getter in voice.h

For each unique symbol, make the call: promote-with-extern OR getter/setter. Update voice.h accordingly.

- [ ] **Step 2: Move the function and `voice_debug_inject_text`**

Cut L876-1479 from voice.c. Paste into voice_ws_proto.c. Rename `static void handle_text_message` to `void voice_ws_proto_handle_text`. Keep `voice_debug_inject_text` public (its name doesn't change — it's already in voice.h).

Inside the moved body, replace direct `s_*` reads with the getters/extern names from Step 1. Rename `s_ws` references to `g_voice_ws`.

- [ ] **Step 3: Update the WS event callback's call site**

The remaining static `voice_ws_event_handler` in voice.c (still there until Task 1.8) currently calls `handle_text_message(...)`. Update to `voice_ws_proto_handle_text(...)`. (After Task 1.8 the event handler itself moves out, so this becomes an internal call.)

- [ ] **Step 4: Build**

```bash
idf.py build 2>&1 | tail -30
```

Expected: clean. If you missed a static-promotion, the linker will tell you exactly which symbol — go back to Step 1, add it, rebuild.

- [ ] **Step 5: Flash**

```bash
idf.py -p /dev/ttyACM0 flash 2>&1 | tail -5
sleep 25
```

- [ ] **Step 6: Smoke + full E2E**

```bash
python3 tests/e2e/runner.py story_smoke 2>&1 | tail -3
python3 tests/e2e/runner.py story_full 2>&1 | tail -3
```

Expected: both at baseline pass count. **This is the high-risk extract — if anything regresses, this is where it shows.** Particularly watch for:

- Rich-media bubbles missing (broken `media`/`card`/`audio_clip` JSON branches)
- LLM streaming bubbles not updating (broken `llm` partial-token branch)
- Tool-call indicators missing (broken `tool_call`/`tool_result` branches)
- TTS playback stuck silent (broken `tts_start`/`tts_end` state transitions)

If a regression appears, `git diff HEAD~1 -- main/voice_ws_proto.c` against the original L881-1479 in main — every JSON-type branch must be byte-equivalent except for the renamed `s_*` → getter/extern references.

- [ ] **Step 7: Commit**

```bash
git add main/voice.c main/voice.h main/voice_ws_proto.c
git commit -m "refactor(voice): move handle_text_message + voice_debug_inject_text (refs #$WSPROTO_ISSUE)"
```

### Task 1.7: Move `handle_binary_message` + `upsample_16k_to_48k`

**Files:**
- Modify: `main/voice_ws_proto.c` — append both functions
- Modify: `main/voice.c` — delete L1486-1582

- [ ] **Step 1: Move both functions**

`upsample_16k_to_48k` (L1486-1499) is a tiny static helper used only by `handle_binary_message` — keep it `static` inside voice_ws_proto.c. `handle_binary_message` (L1500-1582) becomes `void voice_ws_proto_handle_binary(const char *data, int len)`.

The function dispatches on the binary-frame magic prefix:
- `"VID0"` → `voice_video_handle_downlink_frame(payload, len-8)` (already external)
- `"AUD0"` → `voice_handle_call_audio_pcm(payload, len-8)` — currently a small static near the top of voice.c (around L800-820 — re-grep). For now, expose it as `void voice_handle_call_audio_pcm(const char *pcm, int bytes)` in voice.h (promote to non-static + add decl). It stays in voice.c (it touches the playback ring buffer).
- Untagged → existing `playback_buf_write` path. `playback_buf_write` is also a static in voice.c — promote to non-static OR (cleaner) add a `voice_playback_write_pcm(const int16_t*, size_t)` wrapper in voice.h.

- [ ] **Step 2: Update WS event handler call site (still in voice.c)**

In `voice_ws_event_handler` (still at L1586+), change `handle_binary_message(...)` → `voice_ws_proto_handle_binary(...)`.

- [ ] **Step 3: Build**

```bash
idf.py build 2>&1 | tail -20
```

Expected: clean.

- [ ] **Step 4: Flash + smoke + brief call test**

```bash
idf.py -p /dev/ttyACM0 flash 2>&1 | tail -5
sleep 25
python3 tests/e2e/runner.py story_smoke 2>&1 | tail -3

# Quick call test — verifies AUD0 path
curl -sS -H "Authorization: Bearer $TAB5_TOKEN" -X POST http://192.168.1.90:8080/call/start
sleep 8
curl -sS -H "Authorization: Bearer $TAB5_TOKEN" http://192.168.1.90:8080/video | python3 -m json.tool
curl -sS -H "Authorization: Bearer $TAB5_TOKEN" -X POST http://192.168.1.90:8080/call/end
```

Expected: smoke passes; `/video` shows non-zero `frames_sent` after the 8s call; voice state returns to READY after end.

- [ ] **Step 5: Commit**

```bash
git add main/voice.c main/voice.h main/voice_ws_proto.c
git commit -m "refactor(voice): move handle_binary_message + upsample helper (refs #$WSPROTO_ISSUE)"
```

### Task 1.8: Move `voice_ws_event_handler`

**Files:**
- Modify: `main/voice_ws_proto.c` — append as `voice_ws_proto_event_handler`
- Modify: `main/voice.c` — delete L1586-1940
- Modify: `main/voice.c` — update `voice_ws_start_client` (L2607+) to register `voice_ws_proto_event_handler` instead of `voice_ws_event_handler`

- [ ] **Step 1: Move the function**

Cut L1586-1940 from voice.c. Paste into voice_ws_proto.c. Rename `static void voice_ws_event_handler` to `void voice_ws_proto_event_handler`. The body calls `voice_ws_proto_handle_text` and `voice_ws_proto_handle_binary` (already in the same module — no externs needed) and `voice_ws_send_register` (already public via the header).

Watch for these voice.c statics it touches and ensure each is reachable:
- `g_voice_ws` (already extern)
- `s_state_mutex` — promote OR add a setter `voice_set_state(VOICE_STATE_*, detail)` (already exists, public — use it)
- `s_voice_mode` — read via `voice_get_mode()` (public)
- `s_link_health` (link probe stats) — needs promotion. Add `extern voice_link_health_t g_voice_link_health;` to voice.h.
- `s_last_ws_alive_ms` — add `voice_set_last_ws_alive_ms(uint32_t ms)` setter in voice.h.
- `s_consec_lan_fail`, `s_consec_auth_fail` — connection-state counters. Promote to voice.h externs or wrap with setters.
- `voice_async_toast`, `voice_async_error_banner_dyn`, `voice_async_refresh_badge` — these are statics in voice.c (around L749-866). They're UI helpers. Two options:
  - (a) Promote to non-static + add to voice.h as `voice_show_toast(...)`, `voice_show_error_banner(...)` etc.
  - (b) Move them to voice_ws_proto.c (they're only called from event_handler + handle_text_message — both now in voice_ws_proto.c).
  - **Decision: (b)** — move them. Cleaner. They're UI-callback helpers, but the only callers are the WS-proto layer.

- [ ] **Step 2: Update `voice_ws_start_client` registration call**

Find this in voice.c (around L2700):

```c
esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY,
                              voice_ws_event_handler, NULL);
```

Change to:

```c
esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY,
                              voice_ws_proto_event_handler, NULL);
```

- [ ] **Step 3: Build**

```bash
idf.py build 2>&1 | tail -30
```

Expected: clean. Linker errors here mean a static UI helper or a connection-state counter wasn't promoted — go back to Step 1.

- [ ] **Step 4: Flash + run the FULL E2E suite**

```bash
idf.py -p /dev/ttyACM0 flash 2>&1 | tail -5
sleep 25
python3 tests/e2e/runner.py story_smoke 2>&1 | tail -3
python3 tests/e2e/runner.py story_full  2>&1 | tail -3
```

Expected: both pass at baseline counts. The event handler IS the boot-time WS bring-up — if the boot READY transition is missing, the WS stayed in CONNECTING and the smoke story would fail at step 1 (await_voice_state READY).

- [ ] **Step 5: Commit**

```bash
git add main/voice.c main/voice.h main/voice_ws_proto.c
git commit -m "refactor(voice): move WS event handler + UI-callback helpers (refs #$WSPROTO_ISSUE)"
```

### Task 1.9: Move `voice_build_local_uri`

**Files:**
- Modify: `main/voice_ws_proto.c` — append as `voice_ws_proto_build_local_uri`
- Modify: `main/voice.c` — delete L1944-1948
- Modify: `main/voice.c` — update the single call site (in `voice_ws_start_client` around L2660) to use the new name

- [ ] **Step 1: Move + rename**

Cut L1944-1948 from voice.c, paste into voice_ws_proto.c. Rename `static void voice_build_local_uri` → `void voice_ws_proto_build_local_uri`.

- [ ] **Step 2: Update the call site**

In `voice_ws_start_client` find `voice_build_local_uri(uri, sizeof(uri), dragon_host, dragon_port);` → change to `voice_ws_proto_build_local_uri(...)`.

- [ ] **Step 3: Build**

```bash
idf.py build 2>&1 | tail -10
```

Expected: clean.

- [ ] **Step 4: Flash + smoke**

```bash
idf.py -p /dev/ttyACM0 flash 2>&1 | tail -5
sleep 25
python3 tests/e2e/runner.py story_smoke 2>&1 | tail -3
```

- [ ] **Step 5: Commit**

```bash
git add main/voice.c main/voice_ws_proto.c
git commit -m "refactor(voice): move voice_build_local_uri to voice_ws_proto (refs #$WSPROTO_ISSUE)"
```

### Task 1.10: Final regression sweep + clang-format

**Files:** all touched in Phase 1.

- [ ] **Step 1: Run clang-format on the diff**

```bash
git fetch origin main:refs/remotes/origin/main
git-clang-format --binary clang-format-18 --diff origin/main \
    main/voice.c main/voice.h main/voice_ws_proto.c main/voice_ws_proto.h
```

Expected: empty output. If any files print, autofix:

```bash
git-clang-format --binary clang-format-18 origin/main main/voice.c main/voice.h main/voice_ws_proto.c main/voice_ws_proto.h
git add -u
git commit -m "chore(format): clang-format voice_ws_proto extract (refs #$WSPROTO_ISSUE)"
```

- [ ] **Step 2: Run the full E2E gauntlet**

```bash
python3 tests/e2e/runner.py story_smoke    2>&1 | tail -3
python3 tests/e2e/runner.py story_full     2>&1 | tail -3
[ "$(curl -sS -H "Authorization: Bearer $TAB5_TOKEN" http://192.168.1.90:8080/m5 \
        | python3 -c 'import json,sys; print(json.load(sys.stdin).get("failover_state_name"))')" = "ready" ] && \
    python3 tests/e2e/runner.py story_onboard  2>&1 | tail -3
python3 tests/e2e/runner.py story_wave10_ui_skills 2>&1 | tail -3
```

Expected: all match baseline pass counts.

- [ ] **Step 3: Heap floor check**

```bash
curl -sS -H "Authorization: Bearer $TAB5_TOKEN" http://192.168.1.90:8080/heap | python3 -m json.tool > /tmp/post-pr1-heap.json
python3 -c "
import json
b = json.load(open('/tmp/baseline-heap.json'))
n = json.load(open('/tmp/post-pr1-heap.json'))
for k in ('internal_free', 'internal_largest_free_block', 'psram_free'):
    delta_pct = (n[k] - b[k]) / b[k] * 100
    print(f'{k}: baseline={b[k]} now={n[k]} delta={delta_pct:+.1f}%')
"
```

Expected: each delta within ±5%. A regression >5% on `internal_largest_free_block` means we leaked an allocation site or duplicated buffer ownership — investigate via `git diff origin/main -- main/voice.c main/voice_ws_proto.c | grep -E 'malloc|heap_caps_alloc'`.

- [ ] **Step 4: Push branch + open PR**

```bash
git push -u origin refactor/voice-ws-proto-extract
gh pr create --title "refactor(voice): extract voice_ws_proto.{c,h} (closes #$WSPROTO_ISSUE, refs #331)" \
    --body "$(cat <<'EOF'
## Summary

- Extract WS frame routing out of `main/voice.c` into a new sibling module `main/voice_ws_proto.{c,h}`.
- voice.c drops by ~1230 LOC (3,846 → ~2,616).
- Same shape as the prior `voice_billing` / `voice_widget_ws` / `voice_onboard` extracts.

## Symbols moved

- TX wrappers: `voice_ws_send_text`, `voice_ws_send_binary`, `voice_ws_send_binary_public`, `voice_ws_send_register`
- RX dispatchers: `voice_ws_proto_handle_text` (formerly `handle_text_message`), `voice_ws_proto_handle_binary` (formerly `handle_binary_message`)
- Event callback: `voice_ws_proto_event_handler` (formerly `voice_ws_event_handler`)
- Helpers: `upsample_16k_to_48k` (private), `voice_ws_proto_build_local_uri`
- UI callback helpers: `voice_async_toast`, `voice_async_error_banner_dyn`, `voice_async_refresh_badge`
- `voice_send_widget_action`, `voice_debug_inject_text`

## Verification (on physical Tab5 192.168.1.90)

- `idf.py build` clean
- story_smoke:    NN/NN steps (matches baseline)
- story_full:     NN/NN steps (matches baseline)
- story_onboard:  NN/NN steps (matches baseline; or SKIPPED if K144 cold)
- story_wave10_ui_skills: NN/NN steps (matches baseline)
- Heap floor delta within ±5% of baseline
- AUD0/VID0 call path verified via /call/start + /video stats

Closes #$WSPROTO_ISSUE.
Refs #331.
EOF
)"
```

**Acceptance:** PR is green on CI (`git-clang-format` diff is empty), every regression bullet is filled in with concrete numbers, and the linked issue auto-closes on squash-merge.

---

## Phase 2 — PR #2: Extract `voice_modes.{c,h}`

This phase only starts after PR #1 is merged to main and the local working tree is rebased onto the new main. The PR-1 extraction promoted enough statics that PR-2's extracts compose cleanly.

### Task 2.1: Branch + tracking issue

**Files:** none (git + gh).

- [ ] **Step 1: Branch**

```bash
git switch main && git pull --ff-only
git switch -c refactor/voice-modes-extract
```

- [ ] **Step 2: Open issue**

```bash
gh issue create --title "refactor(voice): extract voice_modes.{c,h} (refs #331)" \
    --body "$(cat <<'EOF'
## Scope

Extract the five-tier voice-mode dispatcher out of `main/voice.c` into a new sibling module `main/voice_modes.{c,h}`.  Closes the second extract from #331.  Builds on PR #$WSPROTO_ISSUE (voice_ws_proto extract) which must land first.

## What moves

- `voice_send_config_update_ex` + `voice_send_config_update` — config_update JSON sender (with optional reason for cap_downgrade)
- `voice_get_mode` — public mode getter
- The five-tier text-routing decision currently inline in `voice_send_text` — extracted as a pure helper `voice_modes_route_text(const char *text, voice_modes_route_result_t *out)` returning ROUTE_K144_CHAIN_BUSY / ROUTE_K144_OK / ROUTE_K144_FAILED / ROUTE_DRAGON_PATH

## What stays in voice.c

- `voice_send_text` itself (the public API) — becomes a thin dispatcher on the result of voice_modes_route_text
- `voice_start_listening` / `voice_start_dictation` (they set s_voice_mode but the lifecycle is a voice.c concern)
- The internal `s_voice_mode` static is moved into voice_modes.c with new getter/setter

## Verification

- Build clean
- E2E baseline holds: story_full (4-mode rotation) + story_onboard (vmode=4) + story_wave7_onboard_polish (Local→K144 failover)

## Plan doc

`docs/superpowers/plans/2026-05-03-voice-c-extraction.md` — Phase 2 (Tasks 2.1 – 2.6)

Refs #331.
EOF
)"
```

Note the new issue number as `$MODES_ISSUE`.

### Task 2.2: Carve the new header

**Files:**
- Create: `main/voice_modes.h`

- [ ] **Step 1: Write the header**

```c
/**
 * Voice five-tier mode dispatcher (Tab5).
 *
 * Owns the {LOCAL, HYBRID, CLOUD, TINKERCLAW, LOCAL_ONBOARD} routing
 * decision for outbound text turns + the config_update JSON frame
 * that announces a mode/model change to Dragon.
 *
 * Wave 23 SOLID-audit closure for TT #331 (extract A2).  Pre-extract
 * the routing decision was inline at voice.c:3373-3471 inside
 * voice_send_text.  Pulling it out lets voice.c keep the public API
 * (voice_send_text) thin while the mode-specific branches stay
 * testable + extendable in voice_modes.c.
 */
#pragma once

#include "esp_err.h"
#include "voice.h"   /* voice_mode_t */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    /* VMODE_LOCAL_ONBOARD active + chain currently running — caller must
     * surface a "Stop onboard chat first" toast and refuse the send. */
    VOICE_MODES_ROUTE_K144_CHAIN_BUSY = 0,
    /* Routed text to K144 successfully (vmode=4 OR Local-mode failover hit). */
    VOICE_MODES_ROUTE_K144_OK,
    /* Routed text to K144 but K144 returned an error — caller should
     * surface a toast (failure detail in `err`). */
    VOICE_MODES_ROUTE_K144_FAILED,
    /* Default — caller should send the text via the Dragon WS path. */
    VOICE_MODES_ROUTE_DRAGON_PATH,
} voice_modes_route_kind_t;

typedef struct {
    voice_modes_route_kind_t kind;
    esp_err_t err;       /* set when kind == K144_FAILED */
} voice_modes_route_result_t;

/* Pure routing decision.  Called by voice_send_text BEFORE building any
 * Dragon-side JSON frame — caller dispatches based on result.kind.
 *
 *   - vmode=4 (LOCAL_ONBOARD) → always K144 (CHAIN_BUSY if chain active,
 *     OK on send success, FAILED on send error)
 *   - vmode=0 (LOCAL) AND Dragon WS down for >= M5_FAILOVER_GRACE_MS AND
 *     K144 warm → K144 failover (OK or FAILED)
 *   - Otherwise → DRAGON_PATH
 */
void voice_modes_route_text(const char *text, voice_modes_route_result_t *out);

/* config_update frame senders (formerly voice_send_config_update*). */
esp_err_t voice_send_config_update(int voice_mode, const char *llm_model);
esp_err_t voice_send_config_update_ex(int voice_mode, const char *llm_model,
                                      const char *reason);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: Build to verify the header parses**

```bash
. /home/rebelforce/esp/esp-idf/export.sh
idf.py build 2>&1 | tail -10
```

Expected: clean.

- [ ] **Step 3: Commit**

```bash
git add main/voice_modes.h
git commit -m "refactor(voice): carve voice_modes.h skeleton (refs #$MODES_ISSUE, refs #331)"
```

### Task 2.3: Move `voice_send_config_update*`

**Files:**
- Create: `main/voice_modes.c`
- Modify: `main/voice.c` — delete L3518-3548 (config_update_ex + config_update)
- Modify: `main/CMakeLists.txt` line 35 — add `voice_modes.c`

- [ ] **Step 1: Create voice_modes.c with the moved senders**

Top-of-file:

```c
#include "voice_modes.h"
#include "voice.h"
#include "voice_ws_proto.h"     /* voice_ws_send_text */
#include "voice_onboard.h"      /* failover state + chain helpers */
#include "voice_m5_llm.h"       /* M5_FAILOVER_GRACE_MS + voice_m5_llm_send_text */
#include "settings.h"           /* tab5_settings_get_voice_mode + VMODE_* */
#include "ui_home.h"            /* ui_home_show_toast */
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include <string.h>

static const char *TAG = "tab5_voice_modes";
```

Body: paste `voice_send_config_update_ex` (L3518-3543 in main pre-Phase-1) and `voice_send_config_update` (L3545-3548) verbatim. The `voice_ws_send_text` call already resolves via the new include.

- [ ] **Step 2: Update CMakeLists.txt**

Append `voice_modes.c` to the `SRCS` list:

```cmake
"voice.c" "voice_ws_proto.c" "voice_modes.c" "voice_codec.c" ...
```

- [ ] **Step 3: Build + flash + smoke**

```bash
idf.py build 2>&1 | tail -15
idf.py -p /dev/ttyACM0 flash 2>&1 | tail -5
sleep 25
python3 tests/e2e/runner.py story_smoke 2>&1 | tail -3
```

Expected: clean build + smoke matches baseline. config_update fires on every boot (during REGISTER → first mode-set).

- [ ] **Step 4: Commit**

```bash
git add main/voice.c main/voice_modes.c main/CMakeLists.txt
git commit -m "refactor(voice): move config_update senders to voice_modes (refs #$MODES_ISSUE)"
```

### Task 2.4: Move `voice_get_mode` + extract `s_voice_mode` ownership

**Files:**
- Modify: `main/voice_modes.c` — add `s_voice_mode` static + `voice_get_mode` + accessors
- Modify: `main/voice.c` — delete `s_voice_mode` static + delete `voice_get_mode` (L3019-3022)
- Modify: `main/voice_modes.h` — declare the internal accessors

- [ ] **Step 1: Add accessors to voice_modes.h**

Append before the closing `#ifdef __cplusplus`:

```c
/* Internal mode setter — voice.c calls this from voice_start_listening
 * (sets ASK), voice_start_dictation (sets DICTATE), voice_call_audio_start
 * (sets CALL), voice_call_audio_stop (sets ASK).  Keeps s_voice_mode
 * ownership inside voice_modes.c. */
void voice_modes_set_internal(voice_mode_t mode);
```

- [ ] **Step 2: In voice_modes.c**

Add the static at the top:

```c
static voice_mode_t s_voice_mode = VOICE_MODE_ASK;
```

Append at the bottom:

```c
voice_mode_t voice_get_mode(void) {
    return s_voice_mode;
}

void voice_modes_set_internal(voice_mode_t mode) {
    s_voice_mode = mode;
}
```

- [ ] **Step 3: In voice.c**

Delete the `static voice_mode_t s_voice_mode = VOICE_MODE_ASK;` line (around L324). Delete the `voice_get_mode` body (L3019-3022). Add `#include "voice_modes.h"` at the top (alongside the other voice_* includes).

Find every `s_voice_mode = ...` assignment in voice.c (grep result earlier showed L2899, L2975, L3052, L3099, L3176) and rewrite as `voice_modes_set_internal(...)`. Find every `s_voice_mode` read (mic_capture_task: L1992, L2007, L2073, L2105, L2111, L2120, L2143, L2198) and rewrite as `voice_get_mode()`.

- [ ] **Step 4: Build**

```bash
idf.py build 2>&1 | tail -20
```

Expected: clean. Linker errors here mean a `s_voice_mode` reference was missed — re-grep `git diff main/voice.c | grep s_voice_mode`.

- [ ] **Step 5: Flash + run mode-rotation E2E**

```bash
idf.py -p /dev/ttyACM0 flash 2>&1 | tail -5
sleep 25
python3 tests/e2e/runner.py story_full 2>&1 | tail -3
```

Expected: story_full passes — it rotates through all 4 voice modes. If any mode-switch silently fails to take effect, this is where it shows up.

- [ ] **Step 6: Commit**

```bash
git add main/voice.c main/voice_modes.c main/voice_modes.h
git commit -m "refactor(voice): move s_voice_mode ownership + voice_get_mode (refs #$MODES_ISSUE)"
```

### Task 2.5: Extract the five-tier text-routing decision

**Files:**
- Modify: `main/voice_modes.c` — add `voice_modes_route_text` body
- Modify: `main/voice.c` — gut the K144-routing branches in `voice_send_text` (L3373-L3441 approximately) and replace with a single `voice_modes_route_text(text, &result)` call + dispatch

- [ ] **Step 1: Implement voice_modes_route_text**

In voice_modes.c append:

```c
void voice_modes_route_text(const char *text, voice_modes_route_result_t *out)
{
    if (!out) return;
    out->kind = VOICE_MODES_ROUTE_DRAGON_PATH;
    out->err  = ESP_OK;

    /* TT #317 Phase 5: VMODE_LOCAL_ONBOARD always routes to K144 regardless
     * of Dragon WS state.  voice_onboard.c owns the actual chain transport. */
    if (tab5_settings_get_voice_mode() == VMODE_LOCAL_ONBOARD) {
        if (voice_onboard_chain_active()) {
            ESP_LOGI(TAG, "VMODE_LOCAL_ONBOARD: chain active, refusing text turn");
            out->kind = VOICE_MODES_ROUTE_K144_CHAIN_BUSY;
            out->err  = ESP_ERR_INVALID_STATE;
            return;
        }
        esp_err_t fe = voice_onboard_send_text(text);
        if (fe == ESP_OK) {
            ESP_LOGI(TAG, "VMODE_LOCAL_ONBOARD — routed to K144");
            out->kind = VOICE_MODES_ROUTE_K144_OK;
        } else {
            ESP_LOGW(TAG, "VMODE_LOCAL_ONBOARD but K144 unavailable (%s) — refusing send",
                     esp_err_to_name(fe));
            out->kind = VOICE_MODES_ROUTE_K144_FAILED;
            out->err  = fe;
        }
        return;
    }

    /* TT #317 Phase 4: WS unreachable in Local mode → try K144 failover after
     * the M5_FAILOVER_GRACE_MS grace window. */
    uint32_t down_ms = 0;
    uint32_t last_alive = voice_get_last_ws_alive_ms();
    if (last_alive) {
        down_ms = (uint32_t)(esp_timer_get_time() / 1000) - last_alive;
    }
    if (down_ms >= M5_FAILOVER_GRACE_MS &&
        tab5_settings_get_voice_mode() == VMODE_LOCAL) {
        esp_err_t fe = voice_onboard_send_text(text);
        if (fe == ESP_OK) {
            ESP_LOGI(TAG, "Local-mode failover engaged — routed to K144 (down=%ums)", down_ms);
            out->kind = VOICE_MODES_ROUTE_K144_OK;
            return;
        }
        /* K144 also unreachable — fall through to DRAGON_PATH so the
         * existing voice_ws_send_text error pathway can surface the
         * connectivity error to the user uniformly. */
        ESP_LOGW(TAG, "Local-mode failover attempted but K144 errored: %s",
                 esp_err_to_name(fe));
    }

    /* Default — Dragon WS path. */
    out->kind = VOICE_MODES_ROUTE_DRAGON_PATH;
}
```

This requires `voice_get_last_ws_alive_ms()` in voice.h — add it as a getter in Phase 1 Task 1.8 (already in the static-promotion list above; if not added then, do it here).

- [ ] **Step 2: Replace the inline branches in voice_send_text**

In voice.c at the top of `voice_send_text` (L3369), replace the chunk from L3373 to wherever the `// TT #317 Phase 4` failover branch ends (approximately L3441 — re-read the exact range before editing) with:

```c
voice_modes_route_result_t route;
voice_modes_route_text(text, &route);
switch (route.kind) {
case VOICE_MODES_ROUTE_K144_CHAIN_BUSY:
    if (s_state_cb) {
        ui_home_show_toast("Stop onboard chat first to send text");
    }
    return route.err;
case VOICE_MODES_ROUTE_K144_OK:
    return ESP_OK;
case VOICE_MODES_ROUTE_K144_FAILED:
    /* Toast already surfaced by the K144 path — fall through to a
     * Dragon-side error return for the caller. */
    return route.err;
case VOICE_MODES_ROUTE_DRAGON_PATH:
    /* Fall through to the existing Dragon-WS send below. */
    break;
}
```

Everything below (the existing `voice_ws_send_text(json)` + queue-busy branch) stays put.

- [ ] **Step 3: Build**

```bash
idf.py build 2>&1 | tail -20
```

Expected: clean.

- [ ] **Step 4: Flash + run all three K144-relevant stories**

```bash
idf.py -p /dev/ttyACM0 flash 2>&1 | tail -5
sleep 25
python3 tests/e2e/runner.py story_smoke 2>&1 | tail -3
python3 tests/e2e/runner.py story_wave7_onboard_polish 2>&1 | tail -3
[ "$(curl -sS -H "Authorization: Bearer $TAB5_TOKEN" http://192.168.1.90:8080/m5 \
        | python3 -c 'import json,sys; print(json.load(sys.stdin).get("failover_state_name"))')" = "ready" ] && \
    python3 tests/e2e/runner.py story_onboard 2>&1 | tail -3
```

Expected:
- story_smoke matches baseline (Dragon path)
- story_wave7 matches baseline (Local→K144 failover)
- story_onboard matches baseline if K144 is warm (vmode=4 always-K144 path)

The three together cover every `voice_modes_route_text` branch.

- [ ] **Step 5: Commit**

```bash
git add main/voice.c main/voice_modes.c main/voice_modes.h
git commit -m "refactor(voice): extract five-tier text-routing decision (refs #$MODES_ISSUE)"
```

### Task 2.6: Final regression sweep + clang-format

- [ ] **Step 1: clang-format diff**

```bash
git fetch origin main:refs/remotes/origin/main
git-clang-format --binary clang-format-18 --diff origin/main \
    main/voice.c main/voice.h main/voice_modes.c main/voice_modes.h
```

Expected: empty. If not, autofix + re-commit.

- [ ] **Step 2: Full E2E gauntlet**

```bash
python3 tests/e2e/runner.py story_smoke    2>&1 | tail -3
python3 tests/e2e/runner.py story_full     2>&1 | tail -3
python3 tests/e2e/runner.py story_wave7_onboard_polish  2>&1 | tail -3
[ "$(curl -sS -H "Authorization: Bearer $TAB5_TOKEN" http://192.168.1.90:8080/m5 \
        | python3 -c 'import json,sys; print(json.load(sys.stdin).get("failover_state_name"))')" = "ready" ] && \
    python3 tests/e2e/runner.py story_onboard  2>&1 | tail -3
```

Expected: all match baseline pass counts.

- [ ] **Step 3: Heap floor check**

```bash
curl -sS -H "Authorization: Bearer $TAB5_TOKEN" http://192.168.1.90:8080/heap | python3 -m json.tool > /tmp/post-pr2-heap.json
python3 -c "
import json
b = json.load(open('/tmp/baseline-heap.json'))
n = json.load(open('/tmp/post-pr2-heap.json'))
for k in ('internal_free', 'internal_largest_free_block', 'psram_free'):
    print(f'{k}: baseline={b[k]} now={n[k]} delta={(n[k]-b[k])/b[k]*100:+.1f}%')
"
```

Expected: each delta within ±5%.

- [ ] **Step 4: Push + open PR**

```bash
git push -u origin refactor/voice-modes-extract
gh pr create --title "refactor(voice): extract voice_modes.{c,h} (closes #$MODES_ISSUE, refs #331)" \
    --body "$(cat <<'EOF'
## Summary

- Second of two extracts that close TT #331 (the first was voice_ws_proto in PR #$WSPROTO_ISSUE).
- voice.c lands at ~2,470 LOC owning state machine + mic + playback + reconnect + listening/dictation/call.
- Five-tier mode-routing decision is now a pure helper testable in isolation.

## Symbols moved

- `voice_send_config_update`, `voice_send_config_update_ex`
- `voice_get_mode` + `s_voice_mode` ownership
- New: `voice_modes_route_text` (extracted from inline branches in `voice_send_text`)

## Verification (on physical Tab5 192.168.1.90)

- `idf.py build` clean
- story_smoke:                 NN/NN steps (matches baseline)
- story_full:                  NN/NN steps (matches baseline) — 4-mode rotation green
- story_wave7_onboard_polish:  NN/NN steps (matches baseline) — Local→K144 failover green
- story_onboard:               NN/NN steps (matches baseline) — vmode=4 always-K144 green
- Heap floor delta within ±5% of baseline

Closes #$MODES_ISSUE.
Closes #331.
EOF
)"
```

**Acceptance:** PR is green, both issues auto-close on squash-merge.

---

## Phase 3 — Wrap-up

### Task 3.1: Update `CLAUDE.md` Key Files section

**Files:**
- Modify: `CLAUDE.md` — "Key Files → Dragon voice link" subsection (around the `voice.c` entry).

- [ ] **Step 1: Edit the description block**

Find:

```markdown
main/voice.{c,h}          — Voice WS client (port 3502), mic capture, TTS playback,
                             dictation, reconnect watchdog, three-tier mode, tool events,
                             rich-media handlers, VOICE_MODE_CALL (AUD0-tagged mic).
                             Tab5 talks to Dragon only via this path.
```

Replace with:

```markdown
main/voice.{c,h}          — Voice public API + state machine + mic capture task +
                             playback drain task + listening/dictation/call lifecycles +
                             reconnect watchdog. Wave 23 thinned voice.c from ~3,846 LOC to
                             ~2,470 LOC by extracting voice_ws_proto (RX/TX dispatch +
                             event handler) and voice_modes (five-tier routing).
main/voice_ws_proto.{c,h} — WS frame routing layer: JSON RX dispatcher + binary magic
                             VID0/AUD0/untagged-PCM dispatcher + esp_websocket_client
                             event callback + send wrappers + REGISTER frame builder.
                             Wave 23 SRP closure for TT #331-A.
main/voice_modes.{c,h}    — Five-tier voice-mode dispatcher: config_update sender +
                             voice_modes_route_text decision (LOCAL / HYBRID / CLOUD /
                             TINKERCLAW / LOCAL_ONBOARD).  Wave 23 SRP closure for
                             TT #331-B.
```

- [ ] **Step 2: Commit on a docs branch**

```bash
git switch -c docs/wave-23-voice-c-keyfiles
git add CLAUDE.md
git commit -m "docs(claude): refresh voice.{c,h} Key Files block after #331 extracts"
git push -u origin docs/wave-23-voice-c-keyfiles
gh pr create --title "docs(claude): refresh voice.{c,h} Key Files after #331 extracts" \
    --body "Updates the Key Files section to reflect Wave 23 voice.c thinning (PR #$WSPROTO_ISSUE + PR #$MODES_ISSUE, closes #331)."
```

### Task 3.2: Update memory + audit tracker

**Files:**
- Modify: `~/.claude/projects/-home-rebelforce/memory/project_solid_audit_wave23.md` — append PR-merge entries
- Modify: `docs/AUDIT-architecture-2026-05-01.md` — flip A1 from "open" to "shipped"

- [ ] **Step 1: Append to memory**

In `~/.claude/projects/-home-rebelforce/memory/project_solid_audit_wave23.md`, find the "Closures" or "Status" section and append:

```markdown
- 2026-05-XX — TT #331 closed via PR #$WSPROTO_ISSUE (voice_ws_proto extract: WS event handler + JSON RX + binary RX + send wrappers + REGISTER, ~1230 LOC out of voice.c) + PR #$MODES_ISSUE (voice_modes extract: config_update senders + voice_modes_route_text + s_voice_mode ownership, ~150 LOC out + behavioral split).  voice.c lands at ~2,470 LOC.  E2E full + onboard + wave7 hold at baseline pass counts on physical Tab5 (192.168.1.90); heap floor delta within ±5%.
```

- [ ] **Step 2: Flip A1 in the audit doc**

In `docs/AUDIT-architecture-2026-05-01.md`, find finding A1 (`voice.c god-file`) and change its status line to:

```markdown
**Status:** SHIPPED — closed by PR #$WSPROTO_ISSUE + PR #$MODES_ISSUE (Wave 23, 2026-05-XX).  voice.c thinned 3,846 → ~2,470 LOC.
```

- [ ] **Step 3: Commit on the same docs branch**

```bash
git add docs/AUDIT-architecture-2026-05-01.md
git commit -m "docs(audit): mark A1 (voice.c god-file) shipped via #$WSPROTO_ISSUE + #$MODES_ISSUE"
git push
```

(Memory file is outside the repo — no git step.)

### Task 3.3: Run the long-form stress story

**Files:** none (verification only)

- [ ] **Step 1: 20-minute stress sweep**

```bash
python3 tests/e2e/runner.py story_stress 2>&1 | tail -10
```

Expected: `PASS: 76/77` or better (the historic single LLM-timeout flake noted in CLAUDE.md is acceptable). Heap watchdog assertions inside the story must all hold — any "internal SRAM largest free block dropped below 30 KB" failure means the extract introduced fragmentation and we need to investigate before signing off.

- [ ] **Step 2: Document the result**

Append to the wrap-up issue or comment on #331:

```bash
gh issue comment 331 --body "Wave 23 extract complete. story_stress: NN/77 steps over ~20 min on physical Tab5 (192.168.1.90).  Heap floor stable.  Refs PRs #$WSPROTO_ISSUE + #$MODES_ISSUE."
```

---

## Files to modify summary

| Path | Phase | Change |
|------|-------|--------|
| `main/voice_ws_proto.h` | 1.3 | Create |
| `main/voice_ws_proto.c` | 1.4 – 1.9 | Create + grow per task |
| `main/voice_modes.h` | 2.2 | Create |
| `main/voice_modes.c` | 2.3 – 2.5 | Create + grow per task |
| `main/voice.c` | 1.4 – 2.5 | Symbol removal + getter wiring + new includes (see source map) |
| `main/voice.h` | 1.4, 1.7, 1.8 | New extern declarations + getters/setters for promoted statics |
| `main/CMakeLists.txt` | 1.4, 2.3 | Append `voice_ws_proto.c` then `voice_modes.c` to SRCS |
| `CLAUDE.md` | 3.1 | Refresh Key Files block |
| `docs/AUDIT-architecture-2026-05-01.md` | 3.2 | Mark A1 shipped |
| `~/.claude/projects/-home-rebelforce/memory/project_solid_audit_wave23.md` | 3.2 | Append closure entry |

## Risks + things to watch

- **Static-promotion churn.** The biggest risk is missing a `static s_*` symbol that the extracted code reads. The grep step at the start of each task catches most of these but some macros expand to `s_*` references that grep misses — the linker error is the fallback.
- **WS event handler is on the WS task (Core 1).** Anything that the moved event handler used to reach via voice.c statics now needs an explicit thread-safety guarantee. Use `g_voice_state_mutex` for shared mutable state (it was already taken by the original code). Sanity-check by re-reading the WS task's locking pattern around `voice_set_state` — the move shouldn't add any new lock dependencies.
- **K144 path requires hardware that may be cold.** `story_onboard` self-skips if K144 isn't ready. If K144 IS warm but a regression breaks the chain, we'll see `voice_modes_route_text` returning `K144_FAILED` for every send — symptom is the `/voice` endpoint shows `last_llm_text` empty after a vmode=4 send.
- **Heap regression.** The most likely fragmentation source is the moved cJSON parsing in `handle_text_message` allocating from a different file's static lock context. The `±5% on internal_largest_free_block` gate after each PR catches this; if breached, look for double-allocation or a missing `cJSON_Delete` in the moved branches.
- **CI clang-format.** The Task 1.10 / 2.6 clang-format step is non-negotiable — CI will reject the PR otherwise. Run it BEFORE pushing.
