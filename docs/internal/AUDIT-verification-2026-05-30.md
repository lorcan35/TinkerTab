# Audit Verification Report — Feature-Set Audit 2026-05-30

> Adversarial code-level fact-check of [`AUDIT-feature-set-2026-05-30.md`](AUDIT-feature-set-2026-05-30.md).
> Method: 15 independent verifiers each opened the cited `file:line` and graded every code-grounded claim CONFIRMED / OVERSTATED / FALSE / UNVERIFIABLE against the real source. This report aggregates their verdicts.
> Verifier date: 2026-05-30.

---

## 1. Bottom Line — Is the audit safe to act on?

**Yes — with three named corrections.** The audit is unusually well-grounded: of ~135 distinct code-grounded claims checked across 15 areas, the overwhelming majority (~90%) verified TRUE against the cited source, with no hallucinated files and only a small number of imprecise line numbers (the audit itself warns of a post-snapshot refactor, drift row 325). Every load-bearing finding in the Executive Summary — **OTA auto-rollback not compiled in, the dead server-side spend cap, privacy-lock no-op on voice, the default `wake_src=dragon` mic-leak with no Dragon WAK0 handler, the `user_image`/`user_media` frame mismatch, the camera capture use-after-requeue race, and the committed/over-scoped security posture** — is CONFIRMED at the code level. The NOW-roadmap rests on solid findings.

**Three corrections the owner must apply before acting:**

1. **OPUS uplink encoder is NOT gated-off (FALSE).** The feature-map and §8 say the uplink encoder is gated-off / "crashes (#264)." The code says `VOICE_CODEC_OPUS_UPLINK_ENABLED 1` (enabled; #264 was *fixed* by a 16→32 KB stack bump). This is a stale finding, not a gap. The *downlink* encoder being an unwired stub is separately CONFIRMED.
2. **The debug token is NOT printed in full to UART (OVERSTATED, recurring).** Two places say "token logged/printed to UART every boot." The code masks it to `first4****last4 (32 chars, masked)`. The "anyone with serial capture owns /touch…" consequence does not follow from boot logs. (The committed token in `CLAUDE.md:454` and hardcoded ngrok host ARE real — that half of the secrets finding stands.)
3. **A registered `forget_fact` tool exists (OVERSTATED).** The drift/§8 "dedup/forget/retention missing" overstates: a confirm-gated per-fact `forget_fact` tool ships and is registered. Only *automatic/unified* forget + retention + dedup are genuinely absent.

A handful of additional line-number imprecisions and count-undercounts (TTS "3 reachable" should be 4; settings "11 sections" is actually 12; several cites land on a comment or adjacent function) do not change any conclusion. None of the NOW-roadmap criticals are weakened.

---

## 2. Tallies

Counts are at the level of distinct checkable claims across all 15 verifier tables (a claim asserted in multiple areas is counted once at its strongest grade).

| Verdict | Count | Notes |
|---|---:|---|
| **CONFIRMED** | ~118 | Includes the audit's own self-corrections (client cap fires → single-tier; Solo bypasses; `auth.py` fails closed) which all verified TRUE. |
| **OVERSTATED** | 11 | Mostly stale rationale, wrong line numbers landing on adjacent code, or count undercounts. One recurring (debug-token-to-UART) appears in two cells. |
| **FALSE** | 1 | OPUS uplink encoder "gated-off" — code says ENABLED. |
| **UNVERIFIABLE** | 4 | Require a live device/gateway: mid-session llama-server recovery (out of OTA scope), ext_pcm re-verify on USB transport, vmode=3 M2.7-live/M2.5-404, passwordless-sudo (doc-grounded only). |

Confidence: the audit's factual base is **trustworthy**. No finding was fabricated; the failures are staleness (1) and over-precision/over-claim (11), all locally correctable.

---

## 3. Every OVERSTATED / FALSE Claim — with the precise correction

**This is the section to act on.** Down-weight or re-scope these specific findings; everything else in the audit verified TRUE.

| # | Claim (as audit states it) | Audit cite | Verdict | Precise correction (from source) |
|---|---|---|---|---|
| 1 | OPUS **uplink** encoder is "gated-off … SILK NSQ crash #264; decoder ready" | feature-map L45, L113; §8 L185, L304 | **FALSE** | `voice_codec.h:50` `#define VOICE_CODEC_OPUS_UPLINK_ENABLED 1` — **ENABLED**. Header comment ":25-50" documents the #264 fix (SILK NSQ needs ~24 KB stack → `MIC_TASK_STACK_SIZE` bumped 16→32 KB) and the gate flipped 0→1. The audit treats a *fixed* item as still-broken. The separate claim that the **downlink** encoder (`audio_codec.py:79 OpusDownlinkEncoder`) is a never-instantiated stub is CONFIRMED — keep that; drop "uplink crashes." Also L113 "OPUS uplink decode gated-off" is stale: Dragon's `OpusUplinkDecoder` is live-instantiated (`pipeline.py:400`). |
| 2 | "Debug token printed to UART on every boot" / "token logged to UART" | feature-map L91; §9.1 L215 | **OVERSTATED** | `debug_server.c:132` logs `"... auth token: %.*s****%.*s (%u chars, masked)"` — only first-4 + `****` + last-4. It is the **only** token-log site (L126 logs no value). The §9.1 consequence "anyone with serial capture owns /touch…" does **not** follow from boot logs. Correct to: "token is masked in UART logs (first/last 4 only)." (Note: `CLAUDE.md` itself still falsely claims "Printed to serial log on every boot: …<token>" — a doc bug, but the *code* masks it.) |
| 3 | "Memory dedup/**forget**/retention missing" | drift L123/L203 (§8 lists "unified forget") | **OVERSTATED** (drift row) | A per-fact `ForgetFactTool` (`forget_fact`) exists (`tools/memory_tools.py:87/103`) and **is registered** (`agentic_init.py:104,111`, confirm-gated) + `delete_fact` (`memory.py:284`) + `DELETE /api/v1/memory/{id}`. Correct the bare "forget missing" to "**unified/automatic** forget + retention + dedup missing" (the §8 wording "unified `forget`" is already accurate). |
| 4 | Spoken-turn LLM hop "chosen by Dragon's last `config_update`" cited at `voice_ws_proto.c:640` | §5.1 L59 | **OVERSTATED** (cite) | Mechanism is correct; the cited line is the STT-complete handler (`voice_ws_proto.c:640` = `voice_set_state(PROCESSING, s_stt_text)`), **not** config_update. The real send is `voice_ws_proto.c:826` (`voice_send_config_update(...)` on connect); Dragon applies it via `_handle_config_update` (`server.py:1329`). Substance stands; fix the line cite. |
| 5 | TTS "**3 reachable** on voice path" / partial(3 reachable) | feature-map L108; §8 L187; drift L310 | **OVERSTATED** (undercount) | The local whitelist (`config_swap.py:124-126`) admits piper/kokoro/neutts_air, **and** cloud/hybrid forces `openrouter` (`config_swap.py:138`) → **4** voice-path-reachable backends, not 3. Core claim (supertonic/kitten/edge_tts silently downgrade, dashboard offers all 7) holds; the number is off by one. |
| 6 | Settings "11 sections" (vs doc "55 obj/7 sections") | feature-map L61; drift L295 | **OVERSTATED** (undercount) | Code renders **12** distinct section cards (storage, battery, about, voice, privacy, audio, quiet, budget, tinkeron, channels, display, network — `ui_settings.c:1296-2118`). Direction (doc "7/55" is stale) is right; the audit's "11" is itself an undercount. |
| 7 | Receipts "emit `model='llm'`" for tinkerclaw/dual | drift L311; dragon-llm L430 | **OVERSTATED / STALE** | `tinkerclaw_llm.py` genuinely lacks `get_last_usage` (CONFIRMED). But the live receipt code was fixed (Wave 21b #204): `voice_path_receipt.py:139-140` **skips** the receipt entirely when `not isinstance(llm, SupportsUsage)` — it does **not** emit `model="llm"`. The audit quotes the `base.py:249-252` docstring's description of the *old* bug as current behavior. The exception-fallback (`voice_path_receipt.py:173`) emits `llm.name` = `"TinkerClaw (...)"`, still not `"llm"`. |
| 8 | protocol.md bare `start` vs "`turn_id` **required** end-to-end" | drift L302 (`start_handler.py:69`, `voice.c:1788`) | **OVERSTATED** | Tab5 *does* send turn_id (`voice.c:1788`) and protocol.md *is* silent on it (undocumented — that half CONFIRMED). But "required" is wrong: `start_handler.py:69` `turn_id = cmd.get("turn_id") or "-"` — Dragon **defaults gracefully**. It is threaded-but-optional, not required. |
| 9 | "Dictation auto-stop 5s / cap … `voice.c:193`" (4-hr cap) | drift L293 (`voice_dictation.h:58`, `voice.c:193`) | **OVERSTATED** (cite) | Claim is true: cap = 14400 s/720000 frames (real value at `voice.c:209`), 5 s silence still wired (`voice.c:193 = DICTATION_AUTO_STOP_FRAMES 250`). But `voice.c:193` is the *silence* constant, not the 4-hr cap; the cap lives at `voice.c:209`. Fix the line cite. |
| 10 | Notes search "case-insensitive substring … `ui_notes.c:325`" | feature-map L50 | **OVERSTATED** (cite) | Claim true: `strcasestr` at `ui_notes.c:2571` over the ≤30 local ring. But cited **line 325 is `fetch_full_transcript_from_dragon()`**, an unrelated function — stale post-refactor (the audit's own drift row 325 warns of this). Fix the line cite. |
| 11 | `user_image` emission "`voice.c:2864`" | journey #4; drift §16 | **OVERSTATED** (cite) | The frame mismatch (Tab5 `user_image` vs Dragon `user_media`, dropped at `server.py:864`) is fully CONFIRMED — grep finds zero `user_image` in all Dragon Python. But `voice.c:2864` is a **comment** in the flow docblock; the real emission is `voice.c:2989`/`:2996`. Fix the line cite. |
| 12 | Camera claims under `camera.c:*` (implied `main/camera.c`) | feature-map + drift (298/341/328-338) | **OVERSTATED** (path) | All line numbers match **`bsp/tab5/camera.c`** exactly (no `main/camera.c` exists). The claims (no-op resolution, BMP-as-.jpg, requeue-before-consume race, SC202CS/0x36 vs stale SC2336/0x30 header) are CONFIRMED; only the implied `main/` path (Key Files list) is wrong. |

### Bonus — a doc-vs-code drift the audit could *add* (in its favor)
Dragon's own docs (`docs/flows/vision-turn.md:99`, `GLOSSARY.md:62`, `ARCHITECTURE.md:198-199`) **falsely** claim `user_image` and `user_media` were "unified into the same handler by PR #186." The code shows no such unification (zero `user_image` in Dragon Python). This *strengthens* the audit's "photo→ask not-possible" verdict — worth citing as an extra drift item.

---

## 4. Confidence by Area

| Area | Held up? | Notes |
|---|---|---|
| **OTA + rollback + recovery + deploy** | ✅ Fully | Every claim CONFIRMED. `mark_valid` unconditional (main.c:652) + rollback flag unset (sdkconfig:598) + absent `/ota/rollback` + `:8080` (not `:3500`) + LAN-only + no hourly timer + manual-scp deploy all verified. One nuance: `mark_valid`'s body is PENDING_VERIFY-guarded but that strengthens (never true → permanent no-op), not weakens. |
| **Spend cap + cost_mils + billing** | ✅ Fully | All CONFIRMED, including precise lines (`text_path_receipt.py:136`, `pipeline.py:1351`, `spend_tracker.py:137`, `voice_billing.c:74-112`). The audit's self-corrections (client cap fires → single-tier; Solo bypasses) verified TRUE. |
| **Voice FSM + reconnect + zombie-wifi + 401** | ✅ Fully | 14/14 CONFIRMED with exact line numbers — 7-state FSM, jittered 1→30 s backoff, ngrok ≥4-fail fallback, three-tier zombie escalation, 401-lockout-after-3, orb gate blocks vmode 4/5, dead `s_last_activity_us`. Strong cluster. |
| **Memory + inject + FTS5 + RAG** | ✅ Strong (1 OVERSTATED) | Core thesis (recall out of fast set + native `inject_memory=False` at the precise `conversation.py:958` override + phantom `tasks_*` extending to async tier) fully CONFIRMED. Only "forget missing" overstated (#3). |
| **Multimodal photo→ask** | ✅ Strong (2 cite nits) | Frame mismatch + drop + dead vision stack CONFIRMED by grep (zero `user_image` in Dragon). Two line cites land on comments (#11) but no factual error. |
| **Camera race + BMP + resolution + YOLO** | ✅ Strong (path nit) | Use-after-requeue race, BMP-as-.jpg, no-op resolution, SC202CS/0x36 all CONFIRMED. Path is `bsp/tab5/`, not `main/` (#12). Minor: SEG model is yolo11**s**-seg, not 11n. |
| **Wakeword + WAK0 + wake_src + barge-in** | ✅ Strong | Default `dragon` (settings.c:620), no Dragon WAK0 branch (zero hits anywhere in TinkerBox), barge-in dead-code, no Settings picker all CONFIRMED. Matcher token set is a real subset. One UNVERIFIABLE (ext_pcm re-verify on USB — live). |
| **Privacy-lock + voice routing + Solo** | ✅ Strong (1 cite) | route_text typed-only, privacy no-op on voice, Solo POSTs to Dragon + bypasses cap, no Dragon-side honor (zero `privacy` in Dragon Python) all CONFIRMED. One cite imprecision (#4). |
| **Scheduler + reminders + recurrence + snooze** | ✅ Fully | All CONFIRMED — `schedule_reminder` excluded from fast set, fires to chat-bubble not now-card, recurrence inert, snooze unwired (no `register_action`), bare "at 6pm" fails to parse (empirically run), `list_pending` vs doc `list_due`. |
| **Dictation FSM + note_id + notes ring + SD** | ✅ Strong (2 cite) | `note_id` FSM-field-never-written (distinct from the populated `note_entry_t.note_id`), 30-note silent ring, ≤30 local substring search, 2 s WAV reopen, silent-drop, two racing 45 s/60 s timers all CONFIRMED. Two stale line cites (#9, #10). |
| **Security (secrets, HTTP, gateway, auth)** | ✅ Strong (1 OVERSTATED) | Committed token (CLAUDE.md:454), `skip_cert_common_name_check` (ota.c:186), `ALLOW_HTTP`, `ws://`, `operator.admin` self-grant (gateway.py:400), unauth `/info`+`/selftest`, memory mirror, trust-policy RCE, fail-closed `auth.py`, `ci-bearer-token`, device_id-trust all CONFIRMED. Only debug-token-to-UART overstated (#2). One UNVERIFIABLE (passwordless-sudo — doc-grounded). |
| **Dragon LLM router/fleet/backends/TTS** | ✅ Strong (2 OVERSTATED/stale) | Dormant `fleet: []` w/ unreleased models, native-tools-on-Ollama no-op, one-shot probe, 13-tool fast set, async-drop-on-disconnect, `lmstudio_model:"default"` all CONFIRMED. `model="llm"` receipt is stale (#7); TTS count off by one (#5). |
| **Drift table — firmware rows** | ✅ Strong | All CONFIRMED except OPUS-adjacent (#1) lives in feature-map not this area; two cite nits (settings "11" #6, dictation `voice.c:193` #9). |
| **Drift table — Dragon rows** | ✅ Strong | All CONFIRMED including heartbeat 60 vs doc 600, FTS5-dead, native-Local memory-off, chunks-no-vec, phantom Tasks, nested integrations routes, 72 endpoints (> stale "62+"). turn_id "required" overstated (#8); receipt `model="llm"` stale (#7); recurrence "never read" imprecise (read-into-dataclass but never acted on — inert stands). |

---

## 5. NOW-Roadmap Items Resting on OVERSTATED/FALSE Claims — re-scope guidance

**Good news: none of the NOW criticals collapse.** The roadmap items map cleanly onto CONFIRMED findings. Re-scope notes:

- **NOW — `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` + gate `mark_valid`** → rests on fully-CONFIRMED facts. **Act as written.**
- **NOW — Enforce privacy-lock on voice path** → CONFIRMED (no-op on voice; no Dragon-side honor). The cite for the mechanism is off-by-a-function (#4) but the fix target is correct. **Act as written.**
- **NOW — Stop the dead WAK0 idle-mic stream + Settings picker** → CONFIRMED (no Dragon WAK0 handler anywhere). The "re-verify ext_pcm on USB transport first" caveat is itself the one UNVERIFIABLE item — keep that verification step. **Act as written.**
- **NOW — Scrub secrets; stop printing the debug token to UART** → **RE-SCOPE.** The committed token (CLAUDE.md:454), hardcoded ngrok host, and leaked keys are real and must be scrubbed/rotated. But "stop printing the debug token to UART" is **already done in code** (it is masked, #2). The residual action is fixing the *doc* (`CLAUDE.md` Debug Server section still claims the full token is printed) — not a code change. Drop the firmware sub-task; keep the secret-scrub + rotation.
- **NOW — Persist priced `api_usage` + Solo client-side cap** → CONFIRMED dead server-side cap + uncapped Solo. **Act as written.**
- **NEXT — Resolve video-calling half-state (delete cheapest)** → the RX-auto-modal vector + capture-race amplifier are CONFIRMED. Note the "delete" rationale should **not** cite "OPUS uplink crashes" as supporting dead-weight (that's FALSE, #1) — the uplink encoder is live. The video-calling deletion case stands on the `#if 0` nav tile + live RX modal alone.
- **NEXT — "delete dead OPUS / all TTS is raw PCM"** (§8 framing) → **RE-SCOPE.** Only the *downlink* OPUS encoder is dead (CONFIRMED unwired stub). The uplink encoder + downlink decoder are live (#1). "All TTS is raw PCM" is true (downlink encoder never instantiated) but the reason is the downlink stub, not a broken uplink. Don't "revive" the uplink — it's already enabled.
- **NEXT — Re-enable memory on Local path (add `recall` + relevance floor)** → CONFIRMED. The adjacent "unified forget" work should note a per-fact `forget_fact` already exists (#3) — scope the new work to *retention/dedup/auto-forget*, not basic deletion.
- **LATER — Data export + unified `forget` + retention** → **RE-SCOPE** per #3: per-fact deletion + tool already ship; the genuine gaps are export, *unified/joint* delete across the gateway mirror, and retention/purge.

No NOW item is invalidated. Two NEXT/LATER items (OPUS revival framing, unified-forget framing) need the scope trims above so effort isn't spent re-building things that already work.

---

## 6. One-line verdict

The audit is **safe to act on**. Its headline safety/privacy/security findings are all code-confirmed; correct the three named items (OPUS uplink is enabled not crashed; the debug token is masked in UART not printed; a `forget_fact` tool already exists) and tidy ~8 stale line-number/count cites, and the document is a reliable basis for the prioritized roadmap.
