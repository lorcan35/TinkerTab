# K144 Recovery + Control Plan (2026-05-01)

> **Source:** Live ADB probe of K144 v1.3 StackFlow daemon on
> 2026-05-01 (commit-companion to TT #328 retro).  Probe verified
> that `sys.reset`, `sys.reboot`, `sys.hwinfo`, `sys.lsmode`, and
> `sys.version` are all real, working verbs on the AX630C side.
> The prior assumption ("K144 has no software reset path; needs
> PCB rework") is **wrong** — software-only recovery is feasible.
>
> **Tracking:** TT #317 (master K144 issue).  Each wave references
> #317 in its commit subject.

**Goal:** Close the K144 device-management gaps surfaced in the
control-surface audit — specifically (1) the sticky `UNAVAILABLE`
state that requires a Tab5 reboot to escape, (2) the observability
blackout on K144 thermal/memory/load, and (3) the model-swap-
impossibility — using software-only changes through the existing
UART transport.  No PCB rework required.

**Architecture:** Three sequential waves, each wave one PR
shippable on its own.  Wave 13 is the user's original ask
("better control like rebooting"); Waves 14-15 build on it for
operational visibility and feature reach.

**Tech stack:** ESP-IDF v5.5.2 + LVGL 9.x + StackFlow JSON-over-
UART.  Reference [CLAUDE.md](../CLAUDE.md) for build commands,
ADB access, and the existing K144 file map.

---

## Wave overview

| Wave | Theme | Status | Closes |
|---|---|---|---|
| 13 | K144 is recoverable — `sys.reset` plumbing + `voice_onboard_reset_failover()` + tap-to-recover on the Settings health chip + auto-retry-from-UNAVAILABLE timer | **SHIPPED** `4352e9e` | Audit gap #1 (sticky UNAVAILABLE), gap #4 (mid-session unrecoverability) |
| 14 | K144 is observable — `sys.hwinfo` + `sys.version` verbs + enriched `GET /m5` (temp/mem/cpu/version) + `POST /m5/refresh` + Settings UI thermal+load+version gauge.  Two-tier caching: 30 s success TTL + 5 s attempt rate-limit. | **SHIPPED** `fcb5d1e` | Audit gap #2 (observability blind), gap #5 (no proactive thermal awareness) |
| 15 | K144 model registry surfaced — `sys.lsmode` + `GET /m5/models` (PSRAM-cached, ?force=1 bypass) + Settings UI inventory line ("11 MODELS · 1 LLM · 2 ASR · 3 TTS · 2 KWS · 3 vision").  Picker chip-row deferred until M5 ships a 2nd LLM. | **SHIPPED** `bb2b284` | Audit gap #3 — data path landed; UI picker semantic deferred |
| 16+ | KWS revival on K144 — open-vocab sherpa-onnx-kws-zipformer-gigaspeech.  Resurrects TT #162 retired wake-word feature.  Cost dropped from "vendor dependency" to "1 wave" per LEARNINGS. | Candidate (deferred per `feedback_no_wake_word.md`) | TT #162 |

**Out of scope (parked):**
- KWS revival via K144 sherpa-onnx-kws — the gigaspeech model is open-vocabulary and can detect "Hey Tinker" without retraining.  Bigger surface (touches voice mode semantics + mic routing).  File as Wave 16+ candidate after Wave 15 lands.
- Per-K144 hardware power-gate / GPIO reset line — out of scope for firmware; Module13.2 LLM Mate carrier rework if ever needed.
- StackFlow concurrent multi-unit chains beyond the existing audio→asr→llm→tts chain.

---

# Wave 13 — K144 is recoverable

**Goal:** A sticky `M5_FAIL_UNAVAILABLE` state is no longer fatal.  Tab5
exposes a software path to (a) auto-retry probing every 60 s up to 3
attempts, (b) explicit user "tap to recover" via the K144 health chip
on Settings, and (c) send `sys.reset` to the StackFlow daemon to clear
hung NPU/work_id state without a full reboot.

**PR title:** `feat(wave13): K144 is recoverable — sys.reset + auto-retry + tap-to-recover (refs #317 #328)`

## Files modified

| File | Change |
|---|---|
| `main/m5_stackflow.{c,h}` | New `m5_stackflow_send_sys_action(action, resp_buf, resp_len)` helper that builds + sends the request and reads the synchronous ack.  Supports any `sys.*` verb that returns one frame (`reset`, `reboot`, `ping`, `version`).  Streaming variants (`sys.lsmode`) deferred to Wave 14. |
| `main/voice_m5_llm.{c,h}` | New public `voice_m5_llm_sys_reset(void)` and `voice_m5_llm_sys_ping(void)` that take the UART mutex, call the stackflow helper, parse the ack, return ESP_OK on success.  Kept thin — no business logic. |
| `main/voice_onboard.{c,h}` | New `voice_onboard_reset_failover(void)` — thread-safe, idempotent.  Steps: (1) flip state to `M5_FAIL_PROBING`, (2) send `sys.reset` over UART, (3) wait 4 s for daemon restart, (4) re-trigger warmup probe via existing `voice_onboard_start_warmup()`, (5) on success → READY, on failure → UNAVAILABLE again with retry counter incremented.  Caller-agnostic — works from auto-retry timer, debug endpoint, or UI tap. |
| `main/voice_onboard.c` | New auto-retry timer.  When state transitions to UNAVAILABLE, schedule a one-shot 60 s timer that calls `voice_onboard_reset_failover()`.  Cap retries at 3 (NVS-persisted counter), then go sticky with a "K144 needs power-cycle" toast.  Counter resets on Tab5 reboot. |
| `main/ui_settings.c` | The K144 health chip on the Onboard row (Wave 7 surface) becomes tappable.  Tap fires `voice_onboard_reset_failover()` directly + shows toast "Re-probing K144…".  Currently only displays state — Wave 13 adds the action. |
| `main/debug_server.c` | New `POST /m5/reset` endpoint (bearer-auth) that calls `voice_onboard_reset_failover()` and returns ack JSON.  Lets the e2e harness verify the round-trip without UI. |
| `tests/e2e/runner.py` | `story_wave13_k144_recoverable` scenario — 14 steps covering force-UNAVAILABLE → call `/m5/reset` → verify state cycles back to PROBING then READY.  Uses the existing `/m5` GET to observe state transitions. |

## Implementation order (TDD-flavored, but we don't have a unit-test framework on Tab5 firmware, so each step ends in `idf.py build` + a hardware verify)

- [ ] **Step 1: m5_stackflow_send_sys_action helper.** Pure protocol — build `{"request_id","work_id":"sys","action":<verb>}` newline-terminated, write to UART under transport mutex, read until newline or 3 s timeout, return the response body.  Build clean.
- [ ] **Step 2: voice_m5_llm_sys_reset / voice_m5_llm_sys_ping wrappers.**  Take the K144 module mutex, call the stackflow helper, parse the response's `error.code` field, return ESP_OK on code==0.  Build clean.
- [ ] **Step 3: voice_onboard_reset_failover() function.**  State machine logic only — no UI, no ADB, no fancy retry math.  Single sequential flow: PROBING → reset → wait 4s → re-warmup → READY/UNAVAILABLE.  Build clean.
- [ ] **Step 4: POST /m5/reset debug endpoint.**  Thin wrapper around step 3.  Builds + flash + verify with curl: `curl -H "Authorization: Bearer $TOK" -X POST http://192.168.1.90:8080/m5/reset` returns JSON ack within ~10 s.
- [ ] **Step 5: Auto-retry timer.**  In voice_onboard, when set_failover_state(UNAVAILABLE) is called, kick off a `xTimerCreate` 60 s one-shot that calls reset_failover.  Counter capped at 3, persisted via static int (resets on Tab5 reboot).  Verify: force UNAVAILABLE via stale STT, watch journal for retry attempts at 60 s / 120 s / 180 s, then sticky.
- [ ] **Step 6: Tap-to-recover on Settings K144 health chip.**  ui_settings.c — add LV_OBJ_FLAG_CLICKABLE + event cb that calls reset_failover + shows toast.  Verify: tap the chip while UNAVAILABLE, see toast "Re-probing K144…", chip transitions to "Probing…" then "Ready" if K144 is healthy.
- [ ] **Step 7: e2e story_wave13.**  See file table.  Run on hardware.  All steps pass.
- [ ] **Step 8: Commit + clang-format.**  Single squashed commit on main referencing #317 #328.

## Verification

```bash
# Build + flash
. /home/rebelforce/esp/esp-idf/export.sh
cd /home/rebelforce/projects/TinkerTab
idf.py build && idf.py -p /dev/ttyACM0 flash

# Sanity: K144 reachable
curl -s -H "Authorization: Bearer $TAB5_TOKEN" http://192.168.1.90:8080/m5 | jq

# Force a recovery cycle (works regardless of K144 state)
curl -s -H "Authorization: Bearer $TAB5_TOKEN" -X POST http://192.168.1.90:8080/m5/reset

# Watch the obs ring fire m5.reset events (added in step 3)
curl -s -H "Authorization: Bearer $TAB5_TOKEN" "http://192.168.1.90:8080/events?since=0" \
  | python3 -c "import sys,json; r=json.load(sys.stdin); [print(e) for e in r.get('items',[]) if 'm5' in e.get('kind','')]"

# e2e
export TAB5_TOKEN=05eed3b13bf62d92cfd8ac424438b9f2
python3 tests/e2e/runner.py story_wave13
```

Acceptance: 14/14 e2e steps pass, the auto-retry attempts visible in `journalctl` over the 3-min window, manual tap on the Settings chip recovers UNAVAILABLE in under 10 s.

---

# Wave 14 — K144 is observable (deferred)

**Theme:** `sys.hwinfo` + `sys.version` plumbing → enriched `GET /m5` → small thermal + memory gauge in Settings.

Closes audit gaps #2 (no AX630C temp / NPU memory / queue depth visibility) and #5 (no proactive thermal awareness — silent NPU throttling).

Files: extends Wave 13's `m5_stackflow_send_sys_action` to handle multi-line responses, adds `voice_m5_llm_sys_hwinfo()` returning a typed struct, ui_settings adds a 60 px tall gauge widget below the health chip.  Auto-refresh every 30 s while Settings is visible.

Plan body deferred until Wave 13 ships.

---

# Wave 15 — K144 model picker (deferred)

**Theme:** `sys.lsmode` plumbing → fetch K144 model registry on Settings show → user-selectable Onboard model dropdown.

Closes audit gap #3 (model locked to hardcoded qwen2.5-0.5B).  Surfaces the 11 K144-installed models the audit found unused (notably the Chinese-language ASR + TTS, alternate English TTS, and 3 YOLO vision models for future camera-frame surface).

Plan body deferred until Wave 14 ships.

---

## Out of scope follow-ups (file as separate issues if warranted)

- **Wave 16+ candidate: KWS revival on K144.**  Sherpa-onnx-kws-zipformer-gigaspeech is open-vocabulary — passes runtime keyword list, no custom training needed for "Hey Tinker."  Resurrects the feature TT #162 retired.  Touches voice mode semantics (new "wake mode" or background KWS in vmode 4).
- **Out of scope: hardware power-gate.**  Module13.2 LLM Mate carrier doesn't expose a per-K144 reset GPIO.  PCB rework on the Mate carrier — out of firmware scope.  The software `sys.reset` and `sys.reboot` verbs make this unnecessary for normal operation.
- **Out of scope: K144 firmware OTA.**  K144 packages are installed via `apt` on the AX630C Linux side; Tab5 has no orchestration path.  Treat as ADB-from-dev-host for now.

---

## Provenance — verified verbs as of 2026-05-01

```bash
# Probed live via:  sudo adb forward tcp:10001 tcp:10001
sys.ping       → ack (data:"None")
sys.hwinfo     → {cpu_loadavg, eth_info, mem, temperature: 41350}  # milli-°C
sys.lsmode     → 11 models: tts(2 EN, 1 ZH, 1 melotts ZH), llm(qwen2.5-0.5B),
                 asr(ncnn-EN, ncnn-ZH), kws(gigaspeech-EN, wenetspeech-ZH),
                 yolo(pose, seg, detect)
sys.reset      → "llm server restarting ..."  (soft StackFlow daemon restart)
sys.reboot     → "rebooting ..."              (full Linux reboot — heavy)
sys.version    → "v1.3"

# Negative results — not real verbs:
sys.list, sys.status, sys.cpuload, sys.diskinfo, sys.uptime, sys.log → "action match false"
```

Wave 13 uses `sys.reset` (not `sys.reboot`) because the daemon restart is sufficient to clear hung work_ids + NPU memory and recovers in ~4 s vs ~30 s for a full Linux reboot.  `sys.reboot` is reserved for an "extreme recovery" fallback in Wave 14+.
