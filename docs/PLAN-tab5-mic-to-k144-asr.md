# PLAN — Route Tab5 mic to K144 ASR for wakeword

Closes umbrella issue [#608](https://github.com/lorcan35/TinkerTab/issues/608).

## Problem
Wakeword today runs on the K144's onboard mic, which is physically buried inside the M5-Bus stack between Tab5 and the Mate carrier. It hears ambient TV / room noise better than user voice from across the room. False fires + missed wakes both stem from this mic position.

Tab5's ES7210 quad-mic array sits on the front face of the device, much closer to the user. We already stream this mic cleanly to Dragon during voice turns. **The wakeword path should use Tab5's mic, not K144's.**

## Architecture decision: Path A (UART PCM injection) — BLOCKED

ADB-probed K144 2026-05-18. The asr subscription side works — `asr.setup` with `input: ["sys.pcm"]` is accepted and produces an `asr.utf-8.stream` work_id, exactly as the wakeword listener already uses. **But there is no JSON-RPC verb to publish PCM onto the `sys.pcm` bus from outside.**

Probe-confirmed negatives:

| Attempt | K144 response |
|---|---|
| `action: inference` on `asr.NNNN`, `object: audio.pcm/asr.pcm/sys.pcm/audio.raw/audio.cap.0_0`, base64 PCM | `-4 "inference data push false"` for every shape |
| `action: sys.push` on `work_id: sys.pcm` | `-3 "action match false"` |

`sys.push` is an internal C++ function (`zmq_bus_publisher_push`) called from inside the audio unit's hardware-capture loop — it isn't registered as a callable RPC action.  The `audio.cap` action group on `llm_audio` is `cap_param / cap_config / cap_stop / cap_stop_all` only — no `cap_raw` symmetric to the documented `play_raw` for speaker.

**Conclusion:** without modifying K144's `llm_audio` binary or deploying a custom audio-source daemon to the K144 filesystem (binary deploy via ADB), there is no public StackFlow API to publish host PCM onto the `sys.pcm` bus. Path A as originally designed cannot be implemented purely from Tab5.

### Path B (Dragon-side wake) — now the active path

Original plan deferred this, but Path A's K144 protocol blocker makes B the only realistic implementation route without K144 firmware modification:

- Tab5 streams 16 kHz mono PCM to Dragon continuously when idle (existing voice WS, new "always-listening" mode flag)
- Dragon runs a lightweight wake detector on the stream — options:
  - Whisper.cpp `tiny.en` in streaming mode (already on disk, used by dictation path)
  - openWakeWord with a fine-tuned "hey tinker" model (custom training needed, several days)
  - Moonshine streaming partials matched against `hey tinker` substring (already configured, just route to a new wake-detector instead of full STT path)
- Dragon sends a `wake` WS frame to Tab5 when fired; Tab5 then opens a real voice turn (mic stays open, just transitions from continuous-tap-to-Dragon mode → in-turn mode)

Trade-offs accepted: Dragon becomes a hard dependency for wake (was acceptable for everything else already in vmode=0). Continuous WiFi audio is local LAN only, ~256 kbps, no battery concern (Tab5 is wired). Privacy: same trust boundary as today's voice turns — user owns Dragon.

Could still revisit Path A in a follow-up by writing + deploying a custom `llm_audio_external` daemon to the K144 that accepts PCM via a /tmp/pipe and publishes to `sys.pcm`. That's a multi-day effort (cross-compile ARM64 binary, ADB push to K144 `/opt/m5stack/bin/`, register as a StackFlow unit, wire up). Worth doing if Path B latency proves unworkable, otherwise leave for later.

### Why not Path C (physical fix)
Repositioning the K144 doesn't unlock Tab5's better mic. The whole device topology is fixed by the M5-Bus stacking spec.

## Implementation phases — REVISED to Path B

### Phase 1 — Manual probe (DONE 2026-05-18)
Probed K144 via ADB.  Findings:
- `asr.setup` with `input: [sys.pcm]` is accepted and allocates a work_id ✅
- `asr.utf-8.stream` deltas flow back over the existing TCP/UART bridge ✅
- No JSON-RPC verb exists to publish PCM onto `sys.pcm` — Path A blocked, see "Architecture decision" section above ❌

Conclusion: pivot to Path B.

### Phase 2 — Tab5 always-listening mic mode (3-4 hours)
New Tab5-side flag: `voice_set_wake_streaming(true/false)`. When on, the existing mic capture path runs in IDLE state too, not just during voice turns. PCM frames go to Dragon over the existing voice WS as `{"type":"wake_audio"}` framed binary (new type, parallel to the existing untagged-PCM-during-turn).

- Reuse existing `voice.c` mic task — just open earlier (on wake-streaming arm) and keep running through state transitions
- No new audio capture code needed
- Lifecycle: pauseable when SPEAKING (so Tab5 doesn't hear its own TTS — barge-in stays as a separate feature)

### Phase 3 — Dragon-side wake detector (4-5 hours)
New `dragon_voice/wake/` module on Dragon. Subscribes to a new WS frame type:
- Tab5 → Dragon: `wake_audio` binary frame (16 kHz mono int16)
- Dragon runs a sliding-window detector on the stream
- v1 detector: feed audio into the existing `whisper_cpp small.en` backend (already on disk) with a short window (~1.5 s), match output text against `hey tinker` substring (case-insensitive, with the fuzzy phonetic matcher logic ported from Tab5's `voice_wakeword.c`)
- v2 (follow-up): openWakeWord with a custom-trained "hey tinker" model

On match: Dragon sends `{"type": "wake"}` WS frame back to Tab5. Tab5 transitions to LISTENING + pauses wake-streaming for the duration of the turn.

### Phase 4 — Lifecycle + state machine (2 hours)
- New Tab5 voice substate or NVS key: `wake_streaming_armed` (default true when Dragon connected + vmode=0/1/2/3)
- When armed: mic capture loop runs continuously, ships frames to Dragon
- On `wake` frame from Dragon: transition to LISTENING (existing path), stop wake-streaming
- On voice turn end (state → READY): re-arm wake-streaming
- K144 wakeword path stays as a fallback when Dragon is offline (current behavior unchanged in failover scenarios)

### Phase 5 — Live verify (1 hour)
- Flash to Tab5 192.168.1.90
- Say "Hey Tinker" from 1 m, 2 m, 3 m positions → confirm wake fires
- Background TV/podcast playing → confirm wake does NOT spuriously fire
- Mid-TTS barge-in: speak "Hey Tinker" while Tinker is replying → confirm cancel + new turn opens
- Network drop: Dragon disconnect → confirm fallback to K144 wakeword path (no regression)

## Code anchors (revised for Path B)

| Phase | File | What |
|-------|------|------|
| 2 | `main/voice.c` | extend mic capture lifecycle, new `voice_set_wake_streaming()` API + WAKE_AUDIO binary magic tag for the WS frame |
| 2 | `main/voice_ws_proto.{c,h}` | new `WAK0` 4-byte magic for `wake_audio` frames (parallel to `VID0` / `AUD0`) |
| 3 | `dragon_voice/wake/__init__.py` (Dragon) | new module — sliding-window wake detector consuming `wake_audio` WS frames |
| 3 | `dragon_voice/server.py` (Dragon) | wire wake module into the per-connection pipeline |
| 4 | `main/voice.c` | state machine — wake-streaming pauses during LISTENING/SPEAKING |
| 4 | `main/settings.{c,h}` | new NVS key `wake_stream` (default 1 when Dragon connected) |

## Risks + unknowns

1. **Continuous WiFi audio bandwidth**: 256 kbps continuous. Tab5 wired, local LAN, fine.
2. **Dragon-side detector accuracy**: whisper-tiny.en in sliding-window mode is a v1 detector — not purpose-built for wake detection. False-fire rate vs miss rate need measurement. Fallback option: train openWakeWord "hey tinker" model.
3. **Wake latency**: 1.5 s sliding window + WiFi roundtrip + Dragon CPU = expect ~1-2 s wake latency. Compare to current K144 wake (~500 ms). Acceptable if accuracy is dramatically better; not acceptable if comparable.
4. **K144 wakeword fallback path**: must stay working when Dragon is offline. Don't remove the K144 code — gate it behind a `wake_source` setting (auto / k144 / dragon).

## Rollback plan
If any phase fails: revert to current K144-mic-onboard wakeword path. The new mic-tap module is additive; old code stays in place until Phase 4 cuts it over via config. Worst case: comment out one function call and rebuild.

## ASR model alternatives (research summary, 2026-05-18)

A research pass surveyed what other ASR models can run on K144 (AX630C, ~1 GB user RAM, 3.2 TOPS NPU). The short version: **stay on sherpa-ncnn zipformer-20M for wake**. The relevant findings:

- **No streaming alternative.** `llm-asr` zipformer-20M is the only true streaming ASR M5Stack ships for K144. Everything else (whisper-tiny/base/small, sensevoice-small) is batch / file-mode only.
- **whisper-base / whisper-small** would be candidates for a non-streaming dictation upgrade (the parallel use case to wake) — base hits ~660 ms encoding + ~50 ms/token decoding; small uses 1.1 GB CMM and pushes the 1 GB user-RAM ceiling alongside Qwen2.5-0.5B.
- **sensevoice-small** via sherpa-onnx v1.12.20 AXERA backend offers VAD-chunked pseudo-streaming (10-second windows) for higher-accuracy multilingual ASR. Not true streaming but could be a Tier-3 quality upgrade for the dictation path.
- **openWakeWord / Porcupine / Snowboy** — no public K144 ports. openWakeWord ONNX could theoretically run through Pulsar2 conversion to AX630C `.axmodel`, but that's a multi-day port effort, not a drop-in. Porcupine is closed-source so it's out.
- **No drop-in custom-wake-phrase upgrade.** If "hey tinker" recognition stays unreliable after this plan ships, the realistic next step is training an openWakeWord model + porting via Pulsar2.

This confirms the plan: **the lever to pull is input quality (Tab5 mic), not the ASR model**. The mic-input fix is mic-side, orthogonal to the model. We can layer a whisper-base dictation path on top of this work later without re-architecting the wake side.

Sources: M5Stack apt repo (`repo.llm.m5stack.com`), `docs.m5stack.com/en/stackflow/models/whisper-*`, `github.com/ml-inory/whisper.axera`, `huggingface.co/M5Stack/SenseVoiceSmall-axmodel`, `huggingface.co/AXERA-TECH/Whisper`, sherpa-onnx v1.12.20 AXERA build.
