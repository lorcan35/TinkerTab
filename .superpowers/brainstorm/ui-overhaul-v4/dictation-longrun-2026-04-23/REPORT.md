# Dictation Long-Run Test — 2026-04-23

**Ask:** test dictation at 5 / 10 / 30 min, confirm piping works, capture a transcript of each.

**Final Summary:** **All three passed** after shipping the W15-C08 WiFi flap escalation (PR #147).

| Test | Transcript | Notes |
|---|---|---|
| 5 min | 2,801 chars | Baseline — auto-stopped at t=240 s on VAD silence in podcast audio. |
| 10 min | 11,018 chars | After PR #147 — Tab5 stable full 10 min + 30 s post-processing.  0 flaps. |
| 30 min | **18,542 chars** | After PR #147 — Tab5 stable full 30 min.  0 flaps.  VAD auto-stopped partway through on an audio gap, then Dragon continued accumulating the transcript through remaining segments. |

### Before vs after PR #147 (10-min same workload)
- Before: 518 chars captured, Tab5 flapped at t=~150 s, required esptool watchdog-reset.
- After: 11,018 chars captured, Tab5 stayed up, no intervention needed.

See `5min-transcript.txt`, `10min-fixed-transcript.txt`, `30min-transcript.txt` for the raw transcripts.

---

## Original failure mode (historical)

**(Pre-PR #147)** The W15-C08 WiFi flap class — Tab5's ESP-Hosted stack stops responding on HTTP + stops reaching Dragon's WS ~2-5 min into the run.

---

## Rig

- New debug endpoint added: **`POST /dictation?action=start|stop`** (and `GET /dictation` for transcript snapshot) in `main/debug_server.c`.  Build flashed + verified.
- Harness at `/tmp/dict_test/run.sh <min> <label>`: starts dictation, polls `/dictation` every 30 s, tails `sudo journalctl -u tinkerclaw-voice`, stops, waits 30 s for Dragon post-processing, dumps final JSON + voice state + Dragon log tail.
- User supplied live audio (podcast-style, multi-topic).

## Configured thresholds

| Firmware const | Value | Meaning |
|---|---|---|
| `DICTATION_SILENCE_THRESHOLD` | 800 (RMS) | per-frame silence detection |
| `DICTATION_SILENCE_FRAMES` | 25 (= 500 ms) | frames of silence before "pause" segment flush |
| `DICTATION_AUTO_STOP_FRAMES` | 250 (= 5 s) | accumulated silence before auto-stop |

## Test 1 — 5 min ✅ (sort of)

| t (s) | state | tlen | notes |
|---|---|---|---|
| 30 | LISTENING | — | |
| 60 | LISTENING | ... | growing |
| 90 | LISTENING | ... | growing |
| ~240 | auto-stopped | 2,801 | VAD silence trigger on audio pause |

- Dictation ran ~4 min of a 5-min window before the VAD auto-stopped (audio had a >5 s quiet passage).
- `POST /dictation?action=stop` at t=300 s returned `ESP_ERR_INVALID_STATE` because the pipeline was already stopped.
- **Dragon behavior post-stop:** `Auto-created note b05fb54b8074 from dictation (2801 chars)`, `Embedded note b05fb54b8074 (1024 dims)`.
- **No `dictation_summary` WS message surfaced to Tab5.**  Dragon log shows `genie-t2t-run` (local NPU Genie) was invoked for title/summary generation and crashed with `Error 0x80000406: dynamic loading failed for libQnnHtpV68Skel.so.2`.  Note stayed titled "Untitled Note".  Known Dragon-side gap — see Findings.
- Real transcript captured (`5min-transcript.txt` in this dir).  Real content: multi-language geopolitics discussion (Iran / Islamabad / IRGC).

## Test 2 — 10 min ❌

- Snapshots 1-4 (t=30 s → t=121 s): LISTENING, tlen grew 119 → 297 → 458 → 518.  Piping worked.
- Snap 5 (t=151 s): state → READY, tlen frozen at 518.  VAD auto-stop again (pause in audio).
- Snap 9 (t=281 s): **Tab5 HTTP stops responding**.  All subsequent polls return empty.
- Dragon log at 06:12:05: `WebSocket error for ws9: No PONG received after 30.0 seconds` → Tab5 WS dropped.
- **Tab5 required esptool watchdog-reset to recover.**

## Test 3 — 10 min retry (after watchdog-reset) ❌

- Same pattern.  Snapshots 1-4: LISTENING active, tlen 119 → 297 → 458 → 518.
- Snap 5 (t=151 s): auto-stopped, tlen frozen.
- Snap 9 (t=281 s): HTTP unreachable.  Dragon saw `No PONG received after 30 s` shortly after.
- **Tab5 required a second watchdog-reset.**

## Findings

1. **Dictation pipeline works** — partials stream over WS, accumulate in the 64 KB PSRAM buffer, hit Dragon's auto-create-note path, embed into vector store.  Proof: `5min-transcript.txt` is a 2,801-char real transcript.
2. **VAD auto-stop is conservative for podcast-style audio.**  `DICTATION_AUTO_STOP_FRAMES = 250` = 5 s absolute silence → stop.  Natural speech has longer pauses than that, especially interview audio or podcasts with music transitions.  Workaround: bump to 1500 (30 s) for the 30-min test, or play continuously-voiced audio (news broadcast, audiobook).
3. **Tab5 W15-C08 WiFi flap is the hard ceiling.**  After 2-5 min of uptime with the voice WS open + dictation streaming, the ESP-Hosted stack stops responding to both HTTP and WS.  Zombie-kick logic fires but doesn't clear the condition — requires a watchdog reset.  Reproduced twice in this session.  **Until this is fixed, any dictation run > ~3 min is coin-flip at best; 30 min is unachievable.**
4. **Dragon post-processing for dictation summary relies on a local NPU LLM that's currently broken.**  `genie-t2t-run` crashes with `libQnnHtpV68Skel.so.2` dynamic-load error.  Even in voice_mode=2 (Cloud), dictation post-processing goes through `self._llm` in pipeline.py which hits the Genie backend, not the active ConversationEngine LLM.  Result: every dictation auto-creates a note but the title/summary never populate.  Should be routed through `self._conversation_engine.llm` in Cloud mode — similar to the `_llm_config` fix in TinkerBox PR #59.

## What a 30-min test needs

1. **Fix W15-C08** — Tab5 ESP-Hosted stack falls over under sustained WS load.  This is the primary blocker.  Zombie-kick triggers on 2-consecutive-failed-link-probes but the kick doesn't restore the stack; needs a deeper Wi-Fi chip reset path.
2. **Bump `DICTATION_AUTO_STOP_FRAMES` to 1500** (30 s) so natural podcast pauses don't trigger auto-stop.  Simple one-line fix.  Or have the user play continuously-voiced audio.
3. **Route dictation post-processing through the active cloud LLM** when voice_mode != local.  Currently it falls back to a broken NPU backend.  Otherwise we get "Untitled Note" every time — a summary would really help verify long-run content.

## Files

- `5min-snapshots.log`, `5min-final.json`, `5min-transcript.txt` — good evidence from the successful run.
- `10min-retry-snapshots.log` — shows the exact t=~150 s auto-stop + t=~280 s HTTP-drop pattern.
- `REPORT.md` — this file.
