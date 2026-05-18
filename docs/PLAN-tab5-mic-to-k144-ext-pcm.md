# PLAN — Path A: custom K144 ext_pcm StackFlow unit

Closes [#131] / refs [#615] umbrella. Supersedes the `Path A blocked` claim in the prior plan doc — research on 2026-05-18 found the unblock.

## Goal
Tab5 mic feeds K144's sherpa-ncnn ASR for wakeword **without going through Dragon**. Purely-onboard wake using the better mic. Adds a fourth `wake_src` value `ext_pcm` to the picker shipped in #618.

## The unblock (research confirmed against M5Stack StackFlow MIT source)

K144's `main_audio/src/main.cpp:43-148`:
```cpp
std::string sys_pcm_cap_channel = "ipc:///tmp/llm/pcm.cap.socket";
// ...
pub_ctx_ = std::make_unique<pzmq>(sys_pcm_cap_channel, ZMQ_PUB);
```

K144's `main_asr/src/main.cpp:1051`:
```cpp
audio_url_ = unit_call("audio", "cap", input);   // asks audio for the URL
subscriber(audio_url_, ...);                      // subscribes to whatever URL
```

`"sys.pcm"` is **a label, not a path**. ASR calls audio's `cap` RPC, audio returns the configurable `sys_pcm_cap_channel`. The URL is yours to override (already accepted in `audio.setup`'s `data` field — verified live tonight). Bind your own ZMQ PUB to that URL → ASR consumes your PCM, K144 hw mic stays unused.

## Architecture

```
Tab5 mic → existing wake_stream task
        → split: WS frames to Dragon (existing #616 path, wake_src=dragon)
                 UART frames to K144  (NEW, wake_src=ext_pcm)

K144's llm_sys (UART bridge) → routes JSON inference frames to → ext_pcm unit's per-instance ZMQ inference URL
ext_pcm unit → publishes to sys.pcm ZMQ URL → sherpa-ncnn ASR consumes
```

## Implementation phases

### Phase 0 — Toolchain setup (1-2 hours)
- Install AXera SDK / M5Stack StackFlow cross-compile env. Source clone done: `~/projects/StackFlow`. SConstruct expects `aarch64-linux-gnu-g++` + the M5Stack `static_lib_v0.1.3` (auto-download).
- Verify a known-good unit (e.g. `main_audio`) builds cleanly before adding ours.
- Verify the resulting binary runs on K144 via ADB push.

### Phase 1 — ext_pcm unit (2-3 hours)
- Copy `main_skel/` → `main_ext_pcm/` as the template.
- Implement 7-RPC skeleton (`setup, work, pause, exit, link, unlink, taskinfo`).
- `setup`: parse config, allocate `pzmq pub("ipc:///tmp/llm/pcm.cap.socket", ZMQ_PUB)`. Also override `sys_pcm_cap_channel` via the existing audio unit if it has already bound the default URL — race-aware.
- `work` / `inference`: receive incoming PCM payload (base64 decode in `data` field), publish to the bound PUB socket.
- Lifecycle: refuse setup if `audio.setup` is currently running with the default URL (we'd conflict).
- Build, verify the binary, ADB push to `/opt/m5stack/bin/llm_ext_pcm`, add to `/opt/m5stack/scripts/` startup or symlink into the StackFlow daemon's launch list.

### Phase 2 — Tab5-side PCM-over-UART (1-2 hours)
- Modify `voice_wake_stream.c`: when `wake_src=ext_pcm`, route the PCM chunks to K144 over UART (existing `voice_m5_llm` UART path) as JSON inference frames targeting the new unit's work_id.
- Bandwidth: 16 kHz × 16-bit × 1.33 base64 = ~85 KB/s sustained. UART runs at 1.5 Mbps (~187 KB/s capacity). Fits with margin.
- Frame batching: 30 fps × ~1.5 KB/frame, JSON envelope per frame. May need to compress the JSON wrapper or use binary framing (UART JSON is the existing protocol — keep it).

### Phase 3 — Wire wake_src=ext_pcm (30 min)
- Add `ext_pcm` as a valid value in `/tinkeron/wake_src` endpoint (debug_server_tinkeron.c).
- Add a `tab5_settings_wake_src_is("ext_pcm")` branch in `voice_onboard.c`'s lifecycle:
  - On READY: instead of `voice_wakeword_start` (K144 onboard mic) OR `voice_wake_stream_arm` (Dragon path), call a new `voice_ext_pcm_start()` that:
    - Calls K144's ext_pcm.setup
    - Calls K144's asr.setup with input=sys.pcm
    - Starts the UART PCM sender
  - Lifecycle cleanup on disconnect / source change.

### Phase 4 — Live verify (1 hour)
- Flash Tab5 + ADB deploy ext_pcm binary
- Flip wake_src to ext_pcm
- Confirm Tab5 mic audio reaches sherpa-ncnn (look for asr.utf-8.stream deltas matching our PCM)
- Compare wake latency vs Path B (should be much faster — sherpa-ncnn streaming at NPU speed, no whisper.cpp on Dragon CPU)
- Verify fall-through: if K144 daemon dies / disconnects, fall back to wake_src=dragon

## Risks

1. **Race: audio.setup vs ext_pcm.setup binding the same URL.** Solution: ext_pcm.setup checks if the URL is already bound and either (a) refuses + asks caller to call audio.exit first, OR (b) writes a different URL via `sys_pcm_cap_channel` override and asks ASR to subscribe to ours.
2. **K144 daemon stability with non-vendor binary deployed.** Vendor firmware risk. Mitigation: keep the binary install non-destructive (don't replace any existing file, just add new `llm_ext_pcm`), test ADB rollback by deleting our binary + restarting StackFlow.
3. **Cross-compile toolchain compatibility.** AXera ships specific gcc version + libstdc++ vintage. Mismatch = runtime crash on K144. Mitigation: build inside the M5Stack-provided Docker image or VM if there is one.
4. **UART bandwidth saturation.** If JSON overhead is heavier than expected, batch more samples per frame.

## Test plan
- Bench-test ext_pcm binary on K144 alone: bind PUB, manually publish PCM bytes, run `asr.setup` against it, observe transcripts.
- Tab5 end-to-end: say "Hey Tinker" → expect wake fire on K144 via Tab5's audio.
- Latency measurement: time from spoken "hey tinker" to wake.fire event. Target < 800ms (vs Path B's ~1.5-2s on Dragon CPU).
- Stability: 30 min continuous streaming, watch for K144 daemon restarts.

## Estimated total: 5-8 hours focused, fresh session
Cleanest done as a single focused effort with the cross-compile env set up before kickoff.
