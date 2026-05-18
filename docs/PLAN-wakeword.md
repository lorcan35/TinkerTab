# Plan — TinkerON always-on wakeword (K144 ASR) + on-device dictation

**Status:** **LIVE end-to-end on Tab5 192.168.1.90 (2026-05-18, commit `b614a23`).** Said "Hey Tinker — what time is it?" → Tab5 mic opened → Dragon STT → LLM → Kokoro TTS spoke the time back, hands-free.  No orb tap needed.  Branch `feat/wakeword`; PR [#576](https://github.com/lorcan35/TinkerTab/pull/576) open against `main`.

**Brand:** The K144 LLM Module Kit is **branded "TinkerON"** in all user-facing surfaces.  K144 / AX630C / sherpa-ncnn stay as hardware identifiers in technical text + log messages + code symbols.

**Owner:** unassigned.
**Tracking issue:** TT [#575](https://github.com/lorcan35/TinkerTab/issues/575).
**Last updated:** 2026-05-18.
**Related:** [`docs/PLAN-m5-llm-module.md`](PLAN-m5-llm-module.md) (Phase 6b autonomous chain — same K144 audio + asr units, different downstream consumer), [`docs/PLAN-k144-chain-hardening.md`](PLAN-k144-chain-hardening.md) (UART mutex + obs ring patterns this reuses).

## Working state (2026-05-18)

Five fixes since the 2026-05-17 "wake-fired-but-toast-only" status, all on commit `b614a23`:

1. **WAKE → real voice turn.**  `voice_onboard.c::wakeword_event_handler` dispatches `voice_start_listening()` on the LVGL thread (the orb-tap pipeline) instead of toast-only.  Tab5 mic captures the user's question, ships to Dragon, runs STT → LLM → TTS round-trip per the active `voice_mode` routing (Local / Hybrid / Cloud / TinkerClaw — same logic as the orb tap, no special case).  K144's dictation-buffer auto-capture is cancelled in the same handler via `voice_wakeword_force_dictation_stop()` so we don't mirror the question into a save-note.

2. **ASR T → Th phonetic alt.**  K144's sherpa-ncnn streaming zipformer consistently transcribes "tinker" → "thinker".  `voice_wakeword.c` auto-derives an alt phrase (swap every "tinker" → "thinker" in the configured wake_phrase) and matches against EITHER variant.  Default wake phrase changed from `"tinker"` to `"hey tinker"` — fewer false positives than bare substring, and "hey tinker" matched as "hey thinker" via the alt path on the live test.

3. **K144 NPU slot fix.**  Boot warmup loads `qwen2.5-0.5B-prefill-20e` (`llm.NNNN`) onto NPU; subsequent `asr.setup` for wakeword fails with `err=-21 'task full'`.  Fix: `voice_m5_llm_release()` runs after warmup-infer success, before `voice_wakeword_start()`, freeing the slot.  Same release added to the auto-recovery reset path.  Net cost: ~3 s warmup on the next vmode=4 turn (LLM re-loads); wakeword actually starts.

4. **sys.reset coordination fix.**  `sys.reset` kills K144's audio + asr units mid-stream, leaving Tab5's wakeword task draining a dead UART.  The subsequent post-warmup `voice_wakeword_start()` then returned `INVALID_STATE` silently because the prior task was still alive.  Fix: `voice_wakeword_stop()` runs BEFORE `sys.reset` in `onboard_reset_failover_job`, AND defensively in `onboard_warmup_job`.

5. **Hardware topology requirement (operational).**  BOTH the Mate carrier's USB-C AND the K144's top USB-C must be powered independently.  Mate-only or K144-only leads to NPU brownouts under sustained ASR load → `err=-9 'unit call false'` and module wedging needing a power-cycle.  This is now in the runbook + memory under the TinkerON brand entry.

---

## What this enables

Always-on wake-word + on-device long-form dictation that does NOT require Dragon, an OpenRouter key, or any wake-model retraining.  The user speaks the configured wake phrase (default `"tinker"`); Tab5 captures every subsequent partial transcript into a PSRAM buffer until an end phrase, a silent endpointer gap, or a 4-hour cap fires.  The accumulated text becomes a note via the same path the Dictate chip uses today.

Revives the wake-word product story that was retired in TT #162 (TDM-AEC blocker + custom WakeNet procurement cost) by sidestepping both problems — K144 ships a known-good streaming Zipformer ASR (`sherpa-ncnn-streaming-zipformer-20M-2023-02-17`) that already powers vmode=4, and Tab5 reuses the existing chain hardening.

---

## Architecture

```
K144 (Mate-stacked, vmode-independent)
   audio.setup                 → mic frames continuously published to sys.pcm
        ↓
   asr.setup input=[sys.pcm]   → sherpa-ncnn streaming zipformer
                                   emits asr.utf-8.stream {delta, finish}
                                   over UART (no llm, no tts in this chain)
        ↓
Tab5 voice_wakeword_task drains the partials and runs a 2-state machine:

   IDLE ── partial contains wake phrase (case-insensitive substring on
            ~96-byte sliding window of recent segment text) ──→ WAKE callback

   LISTENING ── every delta appended to 32 KB PSRAM dictation buffer
                exit on any of:
                  • end-phrase matched ("save note")
                  • 3 silent finish-segments since last delta
                  • hard 4-hour timeout (matches TT #573 dictation cap)
                  • external voice_wakeword_force_dictation_stop()
                                          ↓
                                  DICTATION_FINAL callback with accumulated text

UI bridge runs on tab5_lv_async_call from the wakeword task:
   WAKE             → ui_home_show_toast("Tinker listening — speak then say 'save note'")
                       + ui_orb_ripple_for_tool("wakeword")
   DICTATION_FINAL  → ui_home_show_toast("Saved: <first 40 chars>…")
                       + ui_orb_ripple_for_tool("wakeword")
   DICTATION_PARTIAL → no-op (avoid toast spam; live transcript can layer
                                in via orb caption / voice overlay later)
```

The same K144 audio + asr units are also used by `voice_m5_llm.c` Phase 6b autonomous chain (mic → ASR → LLM → TTS) when `vmode=4` is active.  Because that chain pulls full ownership of the UART transaction (the Phase-6b chain_setup posts asr+llm+tts in one breath), the wakeword listener tears its own chain down before vmode=4 work runs and re-arms on every successful warmup READY event.  The transitions are coordinated in `main/voice_onboard.c` — see "Lifecycle" below.

---

## Public API — `voice_wakeword.h`

```c
typedef enum {
    VOICE_WAKEWORD_EVENT_WAKE,              /**< wake phrase matched */
    VOICE_WAKEWORD_EVENT_DICTATION_PARTIAL, /**< new transcript chunk during LISTENING */
    VOICE_WAKEWORD_EVENT_DICTATION_FINAL,   /**< end-of-utterance with full text */
    VOICE_WAKEWORD_EVENT_TRANSCRIPT,        /**< background ASR partial (debug-only) */
} voice_wakeword_event_t;

typedef void (*voice_wakeword_cb_t)(voice_wakeword_event_t event, const char *text, void *user);

typedef struct {
    const char *wake_phrase;          /* default "tinker" */
    const char *end_phrase;           /* default "save note" — empty disables phrase stop */
    size_t      dictation_buf_bytes;  /* default 32 KB */
    uint8_t     silence_segments_to_stop; /* default 3 */
    uint32_t    dictation_timeout_s;  /* default 14400 (4 h) — matches #573 */
    bool        emit_background_transcripts; /* debug; off by default */
} voice_wakeword_config_t;

esp_err_t voice_wakeword_start(const voice_wakeword_config_t *cfg,
                               voice_wakeword_cb_t cb, void *user);
void      voice_wakeword_stop(void);
bool      voice_wakeword_is_active(void);
void      voice_wakeword_force_dictation_stop(void);
```

Idempotency: `voice_wakeword_start` returns `ESP_ERR_INVALID_STATE` if already running.  `voice_wakeword_stop` is safe to call when not running.  All callbacks fire from the wakeword background task — the caller is responsible for bridging to LVGL via `tab5_lv_async_call`.

---

## UI bridge

The wakeword task posts every event onto the LVGL thread via `tab5_lv_async_call`:

- `WAKE` — green toast ("Tinker listening — speak then say 'save note'") + `ui_orb_ripple_for_tool("wakeword")` so the orb pulses with the same skill-comet look used by tool calls in vmode=3.
- `DICTATION_FINAL` — toast carries the first 40 chars of the accumulated transcript + same orb ripple.  Drops the text into the same notes path as Dictate (`/api/notes` sync with bearer auth from the W13 fix).
- `DICTATION_PARTIAL` — intentionally no-op on the home surface (toast spam).  Future iteration can layer a live caption under the orb or in the voice overlay.

The orb ripple is the only visible behavior added across the four surfaces (home / chat / notes / voice) — the existing IDLE orb sphere/halo are unchanged.

---

## Lifecycle (where the listener gets armed)

Bridged from `main/voice_onboard.c`:

1. **Boot warmup READY** — after the existing K144 cold-start probe + warm infer succeeds, the onboard module starts the wakeword task with the default config.  Until READY the task is silent; this matches the "no Tab5-side feature regresses if the module is absent" modularity rule from `PLAN-m5-llm-module.md`.
2. **Auto-recovery (Wave 13 reset path)** — every successful `voice_onboard_reset_failover()` re-arms the wakeword task on the way out (after the post-reset warmup probe goes READY).
3. **vmode=4 chain takeover** — when the Phase 6b autonomous chain starts, the wakeword listener tears down so the K144 UART is owned by one consumer at a time.  When the chain stops, the wakeword task does NOT auto-restart — the user explicitly returned to non-Onboard mode, so the listener will re-arm on the next warmup or reset.
4. **K144 UNAVAILABLE** — listener stays down.  No retry loop here; the existing `voice_onboard.c` 60 s reset auto-retry covers the recovery path and the listener will hook in on the next successful warmup probe (item 1).

All UART transactions inside the wakeword task take/release the recursive mutex added in TT #327 Wave 1 — no special-casing needed beyond that.

---

## KWS dead-end (vendor follow-up needed)

K144 ships a fully-installed `sherpa-onnx-kws-zipformer-gigaspeech-3.3M-2024-01-01` model + a running `llm-kws` systemd service + an officially-listed entry in `sys.lsmode` (visible via the Wave 15 model registry endpoint).  Despite all of that, every `kws.setup` body shape I tried returned `parse_config` rejection errors from the StackFlow daemon — no body shape was accepted.  Bodies attempted included:

- `{"model":"sherpa-onnx-kws-zipformer-gigaspeech-3.3M-2024-01-01"}` (matches the lsmode `mode` field)
- `{"model":..., "kws":["tinker"]}`
- `{"model":..., "wake_word":"tinker"}`
- `{"model":..., "keywords":["tinker"]}`
- `{"model":..., "input":["sys.pcm"]}` (mirrors asr.setup shape)
- Variations with/without `kv_cache_id`, `enkws`, threshold fields from the M5Module-LLM headers

All returned `parse_config false` shapes.  Logged for M5 vendor follow-up; the ASR-based path supersedes because it gives us a strict superset (open-vocabulary phrase matching at runtime + full transcript for the dictation buffer) on a unit we already trust.  If/when M5 publishes a working `kws.setup` body the wakeword module can switch to it without touching the public API — the state machine just consumes a different stream object name.

---

## Mic limitation — Tab5-mic-stream is wave 2

K144's onboard mic faces **backward** when stacked through the Module13.2 LLM Mate carrier — toward the rear of the Tab5, away from where a seated user typically is.  That's fine for one-shot ("speak at the K144 stack") and for the live-verified wake on 2026-05-17 (user was leaning toward the device), but is unlikely to give reliable positional pickup for hands-free use across a desk or while looking at the screen.

**Wave 2 (deferred):** push Tab5's existing ES7210 4-mic TDM array PCM to K144's `asr.setup` via a new stream object name.  StackFlow doesn't expose a Tab5→K144 PCM push path today (see `LEARNINGS.md` "K144 ASR via push-PCM-over-UART — alternative documented for completeness" from Phase 6b notes), so wave 2 requires either:

- A custom K144 binary (~200 LOC C++) replacing `llm-audio` to accept UART-pushed PCM, OR
- I2S-over-M5-Bus hardware refactor

Neither is needed today for the wake-word product story to land — the live verification on 2026-05-17 proved the K144 mic + ASR is good enough for "speak the wake phrase, say a note" with a user near the device.  Wave 2 only matters once the user-research data says positional pickup is the limit.

---

## PRs + commits

- **Issue:** TT [#575](https://github.com/lorcan35/TinkerTab/issues/575) — "Always-on ASR wakeword + on-device dictation via K144"
- **PR:** [#576](https://github.com/lorcan35/TinkerTab/pull/576) — `feat/wakeword` branch
- Commits on branch:
  - `83f82e3` feat(voice): always-on K144 ASR — wakeword + on-device dictation (refs #575)
  - `e5426cc` feat(wakeword): wire UI feedback — toast + orb pulse on WAKE / FINAL (refs #575)

---

## Code anchors

- **K144 chain setup/run/teardown** for the wakeword (audio + asr only — no llm, no tts):
  - `main/voice_m5_llm.c` — `voice_m5_llm_wakeword_setup` / `_run` / `_teardown` (audio.setup + asr.setup stages, `asr.utf-8.stream` parse, NULL-safe teardown)
  - `main/voice_m5_llm.h` — public function declarations for the wakeword-only chain
- **High-level state machine + dictation buffer + force-stop API:**
  - `main/voice_wakeword.c` (~312 LOC) + `main/voice_wakeword.h` (~114 LOC)
- **Lifecycle wiring (warmup READY hook + Wave 13 reset hook):** `main/voice_onboard.c`
- **UI bridge (toast + orb ripple via `tab5_lv_async_call`):** `main/voice_onboard.c`'s wakeword callback
- **Build wiring:** `main/CMakeLists.txt` adds `voice_wakeword.c`

---

## Observability

The wakeword task emits these obs events through the standard `tab5_debug_obs_event(kind, detail)` ring (visible at `GET /events`):

| Kind | Detail | Notes |
|------|--------|-------|
| `wakeword.start` | wake phrase string | task started, chain up |
| `wakeword.task` | `start` / `stop` | drain task lifecycle |
| `wakeword.fire` | matched phrase fragment | WAKE callback path |
| `wakeword.dictation` | `start` / `final` / `forced` | LISTENING transitions |
| `wakeword.partial` | partial text (truncated) | only when `emit_background_transcripts` is on |

These slot into the existing `m5.warmup` / `m5.chain` / `m5.reset` / `error.k144` namespace from PLAN-k144-chain-hardening + PLAN-k144-recovery.

---

## Honest unknowns

1. **Hardware retest of the UI bridge.**  The wake path is live-verified (obs ring caught `wakeword.fire` at 282 s post-boot on 2026-05-17 when the user spoke "tinker").  The toast + orb-ripple commit (`e5426cc`) landed after that verification — needs a fresh hardware run to confirm both surfaces light up.
2. **End-phrase miss recovery.**  If the K144 transcribes "save note" as e.g. "safe note" or breaks the phrase across two finish segments, the listener falls back to the silence-segments cap and the 4-hour timeout.  Once we have enough hardware data on segment shape, the matcher might need to consume `delta` + `finish` boundary metadata, not just the concatenated text.
3. **Power impact of always-on ASR.**  K144's `audio.setup` + `asr.setup` together draw 500-800 mA peak during active inference per the K144 spec — order-of-magnitude the same as the existing autonomous chain, just sustained instead of on-demand.  Bench measurement is wave-2-or-later.
4. **Phrase-match false positives in conversation.**  "tinker bell", "tinkering with", etc. all trip the substring matcher.  Two mitigation paths exist (whole-word boundary check + per-segment finalisation gate); both are deferred until field data shows whether it matters.

---

## Out of scope (for this wave)

- Tab5-mic-stream push to K144 (wave 2; see "Mic limitation" above)
- Multiple wake phrases (today the matcher takes a single substring)
- Confidence thresholds / endpointing tuning (K144 ASR doesn't surface a confidence field on `asr.utf-8.stream`)
- Wake-word in vmode=3 / vmode=5 (those modes have their own STT paths — Dragon + OpenRouter respectively)
- Visual transcript display during LISTENING (DICTATION_PARTIAL is a no-op today; future iteration could caption under the orb)
