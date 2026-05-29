# PLAN — Route Tab5 mic to K144 ASR for wakeword

Closes umbrella issue [#608](https://github.com/lorcan35/TinkerTab/issues/608).

## Problem
Wakeword today runs on the K144's onboard mic, which is physically buried inside the M5-Bus stack between Tab5 and the Mate carrier. It hears ambient TV / room noise better than user voice from across the room. False fires + missed wakes both stem from this mic position.

Tab5's ES7210 quad-mic array sits on the front face of the device, much closer to the user. We already stream this mic cleanly to Dragon during voice turns. **The wakeword path should use Tab5's mic, not K144's.**

## Architecture decision: Path A (UART PCM injection)

Confirmed feasible via K144 ADB probe 2026-05-18:

1. K144's `llm_asr` binary publishes config schema with `"input_type": ["sys.pcm", "sys.cap.0_0"]`. `sys.pcm` is **raw host-injected PCM**, separate from `sys.cap.0_0` (K144's hardware mic chained via `audio.setup`).
2. Each asr work_id exposes per-instance ZMQ sockets: `inference_url` (push PCM frames in) + `output_url` (read transcripts out).
3. The existing UART bridge `llm_sys` already converts UART JSON ↔ ZMQ — no firmware-side changes needed on K144. Same mechanism we use today for setup messages.
4. UART runs at 1.5 Mbps (verified Phase 6a). 16 kHz × 16-bit mono = 256 kbps audio bandwidth — fits with ~6× headroom for JSON+base64 framing overhead.

### Why not Path B (Dragon-side wake)
Considered. Rejected for this iteration:
- Requires Tab5 to stream audio continuously to Dragon over WiFi → battery + privacy concern (continuous over-air audio when device is idle)
- Adds Dragon as a hard dependency for the wakeword path. Current K144 setup keeps wake fully on-device — works even when Dragon is offline.
- Dragon-side wake model would need to be picked + benchmarked (more research).

Could revisit Path B as a Tier-3 follow-up if Tab5→K144 latency proves too high.

### Why not Path C (physical fix)
Repositioning the K144 doesn't unlock Tab5's better mic. The whole device topology is fixed by the M5-Bus stacking spec.

## Implementation phases

### Phase 1 — Manual probe (1 hour, no code)
Validate Path A works end-to-end with hand-crafted JSON before writing C code.

- ADB into K144, manually setup an ASR work_id with `input_type=sys.pcm`
- Hand-stream a known WAV (e.g. "hey tinker" recorded on a phone) via ZMQ PUSH to the inference_url
- Confirm asr.utf-8.stream output frames return with the expected transcript

Exit criteria: known-good audio in → known-good transcript out.

### Phase 2 — Tab5 mic-tap task (4-6 hours)
New module `main/voice_wakeword_mic_tap.{c,h}` — a low-priority background task that captures Tab5 mic audio continuously while the wakeword listener is armed, and ships it over UART to K144.

- Reuse the existing `mic.c` API (ES7210 quad-mic, TDM slot 0 extraction)
- Downsample 48 kHz → 16 kHz (3:1 same as voice path)
- Frame at 20-50 ms cadence (320-800 samples per frame at 16 kHz)
- JSON-encode with `{action: "inference", work_id: "asr.NNNN", object: "audio.raw", data: "<base64>"}` per frame
- Write to UART through the existing `bsp/tab5/uart_port_c` mutex-guarded sender

Bandwidth budget: 30 fps × ~1.5 KB/frame (incl. base64 1.33× overhead + JSON wrap) = 45 KB/s. UART at 1.5 Mbps = 187 KB/s capacity. ~25% utilization. Headroom for control traffic.

Concerns:
- **Mic contention with the voice WS path** during real voice turns. Two options:
  - Stop the K144 tap when voice state enters LISTENING (single mic consumer at a time, simple lifecycle)
  - Tee the PCM stream to both consumers (cleaner UX — wake fires during TTS still work, barge-in stays smooth)
- Go with **stop-on-LISTENING for v1**, tee in a follow-up. Less risk.

### Phase 3 — K144 setup wiring change (2 hours)
Modify `main/voice_m5_llm.c::voice_m5_llm_wakeword_setup`:

- Drop the `audio.setup` chain step
- Setup ASR directly with `"input_type": "sys.pcm"`
- Cache the returned `asr.work_id` so the mic-tap task knows where to push
- Teardown: stop mic tap → ASR exit (no audio unit to release)

Existing wakeword task drains transcripts unchanged — the asr output_url protocol is identical.

### Phase 4 — Lifecycle coordination (2 hours)
- Mic-tap task starts when wakeword arms (warmup READY)
- Mic-tap task stops when wakeword stops OR voice enters LISTENING
- On LISTENING → READY transition, mic-tap resumes
- Wake-during-SPEAKING (barge-in #598) still works because the mic-tap was paused before TTS started, so K144 doesn't hear its own response

### Phase 5 — Live verify (1 hour)
- Flash to Tab5 192.168.1.90
- Say "Hey Tinker" from 1 m, 2 m, 3 m positions → confirm fire_count increments cleanly
- Background TV/podcast playing → confirm fire_count does NOT spuriously increment
- Mid-TTS barge-in: speak "Hey Tinker" while Tinker is replying → confirm cancel + new turn opens

## Code anchors

| Phase | File | What |
|-------|------|------|
| 2 | `main/voice_wakeword_mic_tap.{c,h}` | new module, background mic+UART task |
| 2 | `main/voice_wakeword.c` | call into mic_tap start/stop in lifecycle hooks |
| 2 | `main/voice.c` | hook mic-tap pause/resume on voice state changes |
| 3 | `main/voice_m5_llm.c` | swap audio.setup chain → sys.pcm input |
| 3 | `main/voice_m5_llm.h` | API exposes inference_url / work_id for mic-tap |

## Risks + unknowns

1. **UART contention during a voice turn**: voice WS path sends PCM directly to Dragon over WiFi — it doesn't touch K144 UART. So no contention. But control messages (setup/teardown) might compete. Mutex on uart_port_c already exists (TT #327 Wave 1).
2. **PCM framing overhead**: base64 vs binary. Probe Phase 1 will reveal whether the asr inference accepts binary `data` field. If yes, drop the 1.33× base64 multiplier.
3. **Latency budget**: each PCM frame = JSON parse + ZMQ hop + ASR feature extract. Need to measure end-to-end wake latency vs current. Target: ≤ 500 ms from spoken "tinker" to wake fire.
4. **K144 daemon crash on sustained PCM stream**: unknown reliability. The KWS unit was rejected by vendor firmware (parse_config fails); ASR might have similar surprises with sys.pcm input. Phase 1 probe gates this.

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
