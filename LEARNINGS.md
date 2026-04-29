# TinkerClaw Learnings

Institutional knowledge from building TinkerClaw: **TinkerTab** (ESP32-P4 Tab5 firmware) and **TinkerBox** (Dragon ARM64 server).

Every entry here was learned the hard way. Read this before touching the codebase.

**How to use this document:**
- Search by symptom when debugging a new issue
- Read the relevant category before starting work in that area
- Add new entries at the bottom of the appropriate category using the template below

**Entry template:**
```
### [Short Title]
- **Date:** YYYY-MM-DD
- **Symptom:** What was observed
- **Root Cause:** Why it happened
- **Fix:** What was done
- **Prevention:** How to avoid it in the future
```

---

## Hardware Gotchas

### Camera is SC202CS at 0x36, NOT SC2336 at 0x30
- **Date:** 2026-03-10
- **Symptom:** Camera init failed silently. SCCB probe at address 0x30 returned no ACK.
- **Root Cause:** M5Stack documentation claims the camera is SC2336 at SCCB address 0x30. The actual silicon is SC202CS (SC2356) at address 0x36. The docs are wrong.
- **Fix:** Changed SCCB address to 0x36 and camera model to SC202CS in driver config (commit 541d1bb, a5fbf83).
- **Prevention:** Never trust vendor docs for I2C/SCCB addresses. Always probe the bus with `i2cdetect` or equivalent scan first. Read the actual chip markings.

### SD Card Uses SDMMC SLOT 0 with LDO Channel 4
- **Date:** 2026-03-10
- **Symptom:** SD card mount failed. Code was using SDMMC SLOT 1 (the ESP-IDF default).
- **Root Cause:** Tab5 hardware routes SD card to SDMMC SLOT 0, not the default SLOT 1. Additionally, the SD card VCC is powered by on-chip LDO channel 4, which must be enabled before mount.
- **Fix:** Changed to SLOT 0 and added LDO channel 4 power-on in SD init (commit 541d1bb).
- **Prevention:** Check the Tab5 schematic for SDMMC slot assignment and power rail. Don't assume ESP-IDF defaults match the hardware.

### ESP-Hosted SDIO WiFi Boot Crash
- **Date:** 2026-03-12
- **Symptom:** Board crashed immediately on boot after enabling ESP-Hosted WiFi (C6 coprocessor via SDIO).
- **Root Cause:** The ESP32-C6 WiFi coprocessor must be powered and released from reset BEFORE the P4 initializes the SDIO bus. Without proper power sequencing via the IO expander, the SDIO init reads garbage and panics. Also requires IDF v5.4.3+, C6 slave target config, active-low reset polarity, and Tab5-specific pin mapping.
- **Fix:** Added IO expander power sequencing: enable C6 power, delay, deassert reset, then init SDIO (commits 13df413, 0785795).
- **Prevention:** Any coprocessor on a shared bus needs explicit power sequencing. Document the boot order. Test WiFi init in isolation before integrating.

### ESP32-P4 PSRAM Cache Coherency (DPI Framebuffer)
- **Date:** 2026-03-07
- **Symptom:** Display showed stale or corrupted frames. CPU writes to the framebuffer in PSRAM were not visible on screen.
- **Root Cause:** DPI DMA reads PSRAM directly, bypassing the L2 cache. After the CPU writes pixels, the cache holds the new data but PSRAM still has the old data. DMA reads the old data.
- **Fix:** Call `esp_cache_msync()` after every CPU write to the framebuffer to flush the L2 cache to PSRAM.
- **Prevention:** Any time CPU writes to a buffer that DMA reads from PSRAM, you MUST call `esp_cache_msync()`. This is an ESP32-P4-specific requirement. Add it to every framebuffer write path.

### INA226 Battery Monitor Reads 0%
- **Date:** 2026-03-27
- **Symptom:** Battery SOC always reports 0%. I2C communication to INA226 succeeds but values are zero.
- **Root Cause:** Shunt resistor value or calibration register configuration does not match the actual hardware. The INA226 calibration register must be programmed based on the specific shunt resistance on the Tab5 board.
- **Fix:** Known open issue. Needs shunt resistance measurement from the PCB and corresponding calibration register programming.
- **Prevention:** When integrating power monitors, always verify the shunt resistor value against the schematic and calculate calibration register from datasheet formula.

### K144 must sit on the Module13.2 LLM Mate carrier — direct stack on Tab5 causes power hangs
- **Date:** 2026-04-29
- **Symptom:** With the K144 stacked DIRECTLY on Tab5's M5-Bus rear (no carrier between them) and both Tab5 + K144 plugged into separate USB-Cs, the K144's NPU model load consistently hung — `llm_llm` at 0% CPU, load avg 3.5+ from `[npu_task_launch]` D-state kthreads, no `load_mode success` log even after 18+ minutes of waiting.  Phase 3.5 documented this as "K144 cold-start hang".  Inserting the Module13.2 LLM Mate carrier between Tab5 and K144 (Tab5 → Mate → K144) and powering the K144's USB-C on the Mate, the model loaded in ~4 minutes and inference returned in 2 seconds.
- **Root Cause:** When K144 is stacked directly on Tab5, both devices share the M5-Bus 5V rail.  K144's NPU peaks at 500-800 mA during model load.  Tab5's USB-C provides 5V to the M5-Bus through `EXT5V_EN` (a single switch), but the K144's own top USB-C ALSO feeds 5V into the same rail.  The two power sources fight, causing brownouts / supply-noise on the NPU rail mid-load.  AX630C NPU silicon doesn't recover gracefully from supply transients — it goes into a wedged state visible only as D-state kthreads.  The Mate carrier sits between Tab5 and K144 with its own USB-C feeding power directly to the K144's FPC connector, isolating the rails.
- **Fix:** **Always use the Module13.2 LLM Mate carrier when stacking the K144.**  The carrier:
  - Supplies clean dedicated 5V to the K144 via its own USB-C
  - Passes M5-Bus UART (GPIO 6/7 on Tab5) through to the K144's FPC, so Tab5's `uart_port_c` driver works unchanged
  - Adds a CH340N debug serial (handy for K144-side log capture)
  - Acts as a passive heatsink for the K144's metal frame
- **Prevention:** Document the carrier-required topology in `docs/PLAN-m5-llm-module.md` — direct K144-on-Tab5 stacking is NOT a supported config.  When verifying K144 setup against Tab5, the bench rig must be:  Tab5 base → Mate carrier (M5-Bus) → K144 (FPC) → top USB-C powering K144.

### K144 has a working microphone — push-PCM is NOT needed; chain runs autonomously on K144
- **Date:** 2026-04-29 (corrected from earlier "subscription-only" finding)
- **Earlier wrong claim:** Documented Phase 6b as "deferred indefinitely" because I assumed K144 had no mic and the only path was Tab5-pushes-PCM-to-K144 (which the StackFlow protocol doesn't expose).
- **What I actually missed:** The K144 hardware has a **working microphone** wired through the AX630C audio codec.  Live capture from `/opt/usr/bin/tinycap -D 0 -d 0 -r 48000 -c 2 -b 16` shows:
  ```
  sec 0: rms=  42  peak= 665   (silence)
  sec 1: rms= 357  peak=2984   ████████  (user speaking)
  sec 2: rms= 422  peak=2983   ████████
  sec 3: rms= 276  peak=2211   █████
  sec 4: rms= 363  peak=3108   ███████
  ```
  RMS jumped 8× when audio was present.  The mic is NOT a dead channel — it's wired, working, and picks up speech clearly.
- **My initial mistake:** I recorded for 2 seconds in silence, got RMS=42, and concluded "no mic."  RMS=42 in a quiet room is normal ambient noise floor for a working mic — a truly disconnected channel reads RMS≈0-5 (just ADC quantization).  Should have asked the user to speak BEFORE concluding.
- **What this means for the architecture:**  The K144 doesn't need Tab5 to send it audio.  The K144 captures sound itself, runs the full ASR → LLM → TTS chain locally, and only needs to send the synthesized TTS audio back to Tab5 over UART for playback.  This matches the official M5 KWS_VAD_Whisper_LLM_TTS example exactly — host devices just kick off the pipeline and drain output.
- **Correct architecture (Phase 6b reopened with this approach):**
  ```
  User speaks at the stack
        ↓
  K144 mic (sys.pcm topic, published by llm-audio)
        ↓
  K144 ASR (sherpa-ncnn streaming) subscribes → text
        ↓
  K144 LLM (qwen2.5-0.5B) subscribes to ASR → reply text
        ↓
  K144 TTS (single_speaker_english_fast) subscribes to LLM → audio bytes
        ↓
  TTS output frames sent back to caller via the StackFlow gateway (UART)
        ↓
  Tab5 drains audio frames from UART, plays through speaker
  ```
  No push-PCM, no custom binaries, no protocol gymnastics.  The K144's StackFlow framework was designed for exactly this.
- **Prevention:** When the AX630C / K144 / similar M5 module exposes ALSA capture devices, **always test capture WITH audible input** (have the user speak, clap, play music) before assuming the channel is disconnected.  Pure silence in a quiet room reads RMS≈40-60 from any working mic — that's not "no signal," that's "ambient noise floor."  A truly dead channel reads RMS≈0-5.

### K144 ASR via push-PCM-over-UART — alternative documented for completeness
- **Date:** 2026-04-29
- **Context:** If you ever WANT Tab5's mic specifically (not K144's) to feed ASR — for example to use Tab5's superior beamforming or ECM mic — you'd need to bridge audio over UART because the K144's StackFlow architecture is **subscription-based** (the `audio` unit owns the `ipc:///tmp/llm/pcm.cap.socket` ZMQ publish channel, no push action exists in `register_rpc_action`).
- **Engineering paths to enable Tab5-mic→K144-ASR:**
  1. **Custom K144 binary** — replace `llm_audio` with one that adds a `push_pcm` RPC action.  Native compile on K144 (gcc + libzmq.so.5 present).  ~200 LOC C++.  Real engineering, ~8 hours.
  2. **`audio.cap_channel` override** — point K144's audio module at a named pipe that a custom helper feeds from UART.  More invasive than #1.
  3. **I2S over M5-Bus** — wire Tab5's I2S through the Mate carrier to K144 audio codec input.  Hardware refactor.
- **For TT #317 we use Option 0 instead:** K144's onboard mic (which works) handles voice input.  Tab5 only needs to drain TTS output frames, not push audio in.

### K144 TTS returns headerless 16 kHz mono PCM (not RIFF WAV) despite "tts.base64.wav" name
- **Date:** 2026-04-29
- **Symptom:** Setting `response_format = "tts.base64.wav"` on the K144 TTS unit and base64-decoding the response yielded ~32 KB of bytes that didn't validate as a WAV file (`file` reported "data", not "RIFF").
- **Root Cause:** The `wav` suffix in `tts.base64.wav` is a misnomer in the K144's StackFlow API.  The K144 emits **headerless 16-bit signed-LE mono PCM at 16 kHz** — the format the model produces internally, NOT a RIFF-wrapped WAV file.  Confirmed by passing the bytes through `ffmpeg -f s16le -ar 16000 -ac 1` which validated as proper PCM.
- **Fix:** Tab5 firmware (`voice_m5_llm_tts`) treats the decoded base64 as raw int16 PCM samples and upsamples 1:3 (16 kHz → 48 kHz) before handing to `tab5_audio_play_raw`.  Documented in `voice_m5_llm.h` API contract.
- **Prevention:** When a K144 `response_format` mentions `wav` but you don't see a RIFF magic, treat it as raw PCM at the documented sample rate.  Don't try to skip a WAV header that isn't there.

### K144 voice_m5_llm rx buffer must be sized for full TTS response (~384 KB PSRAM)
- **Date:** 2026-04-29
- **Symptom:** Phase 6c TTS calls timed out silently with `tts: no audio data received` even though K144 logged the message and synthesized audio.
- **Root Cause:** `voice_m5_llm.c`'s `M5_RX_BUF_BYTES` was originally 4 KB (sized for short LLM streaming chunks).  TTS responses are SINGLE frames carrying the entire base64-encoded audio (~30 KB+ for 1 sec of speech).  When a frame exceeded 4 KB, the rx buffer filled before any newline appeared, the inner `recv` loop exited on `s_rx_len >= cap` not on `nl != NULL`, and the outer loop broke out before parsing.
- **Fix:** Bumped `M5_RX_BUF_BYTES` to 384 KB (PSRAM-allocated, lazy first-use), enough for ~8 sec of 16 kHz mono PCM base64-encoded.
- **Prevention:** When adding K144 actions that return large base64 payloads (TTS, VLM, future video frames), size the rx buffer for the worst-case single-frame response, not the smallest streaming chunk.  All transports here are PSRAM-backed; over-sizing is cheap.

### K144 inference data shape is stream-frame OBJECT, not a string
- **Date:** 2026-04-29
- **Symptom:** Phase 3 / 4 inference attempts returned `error.code: -25 "Stream data index error"` from the K144 even when setup succeeded and the model was confirmed loaded.
- **Root Cause:** The K144's StackFlow daemon expects the `data` field of an `action: inference` request to be a stream-frame OBJECT mirroring the response shape:
  ```json
  "data": {"delta": "<prompt>", "index": 0, "finish": true}
  ```
  NOT a bare string `"data": "<prompt>"`.  The string form is what the M5Module-LLM v1.0 Arduino library used, but v1.1+ deprecated it.  Confirmed by reading `M5Module-LLM/src/api/api_llm.cpp` v1.7.0 source.
- **Fix:** Patched `voice_m5_llm.c` to construct the inference request data as a cJSON object with `delta` / `index` / `finish`.  Also patched setup `data.input` from string `"llm.utf-8"` to array `["llm.utf-8"]` (same v1.1+ schema migration).  After both fixes, `m5infer Tell me a joke` returned `"Why did the tomato turn red? Because it saw the salad dressing!"` in 2.0 seconds.
- **Prevention:** Always cross-reference protocol shapes against the latest `M5Module-LLM` Arduino source (treat M5's docs as approximate, the C++ source is authoritative).  Stale `error.code: -25` should now be a clear "request shape mismatch" tell — log the response and check delta/index/finish presence.

### K144 cold-start model load can hang for 5+ min — root cause: NPU stack
- **Date:** 2026-04-29 (initial), updated 2026-04-29 (Phase 3.5 root-cause)
- **Symptom:** Phase 3 of the K144 LLM Module integration verified setup-ack + error-path but couldn't complete a clean inference round-trip.  After Tab5 sends `llm.setup`, the K144 returns the early ack with `work_id=llm.NNNN` instantly (Tab5 sees `error.code=0`, work_id assigned), but the ACTUAL model load (mapping the qwen2.5-0.5B-prefill-20e checkpoint into NPU memory) runs asynchronously after the ack and **frequently never completes**.  Multiple observations during Phase 3.5:
  - Cold-start after K144 ADB reboot: load hung 18+ min, no completion.
  - After `systemctl restart llm-llm`: load hung 4+ min.
  - Earlier (Phase 0 era): load completed in ~4m42s.  Inconsistent.
- **Root cause (found via Phase 3.5):** the K144's `llm_llm` userspace process is **NOT actually doing model load work** during the hang — `top -bn1` shows only ~3 sec total CPU consumed since process start, with `State: S (sleeping)` and 0% CPU.  Yet `loadavg = 3.5+`.  The discrepancy: load average counts processes in **uninterruptible sleep (D state)**, and on the K144 those are kernel kthreads `[npu_task_launch]` (PIDs hop around, two of them when the model load is in progress).  **The NPU hardware/driver itself is unresponsive** — `llm_llm` submitted a model-load syscall, the kernel's NPU driver kicked off `npu_task_launch` workers, and those workers are stuck waiting on the NPU which never returns.  This is **K144 firmware/hardware**, not Tab5.
- **Investigated:** ADB-shell into the K144 (`32c9:2003 axera ax620e-adb` via top USB-C) → `journalctl -u llm-llm --boot --no-pager` shows the loading sequence stops at `LLM init start` and produces no further entries until either `LLM init ok` + `setup: load_mode success` (when complete) or never.  `/tmp/llm/` IPC sockets accumulate (5010/5012/5013/5015 etc.) across stale setup attempts; clean state has only `5556.sock`.  The K144's cold-load state appears to be sensitive to: (a) accumulated stale work_ids from prior sessions, (b) thermal load (the unit has been running > 2 hours when the slowest load occurred), (c) possibly NPU memory fragmentation we can't observe.
- **Fix (today):** None landed for the cold-start itself — that's a K144-firmware concern, not Tab5.  Phase 3's code is structurally correct: setup ack parsed, work_id cached, error_code surfaced, streaming chunk extraction tested via Phase 2's `m5lscmd` parse path.  Phase 4 must handle `voice_m5_llm_infer` returning `ESP_ERR_TIMEOUT` gracefully — the failover should NOT engage if the K144 isn't ready.
- **Operational workarounds:**
  - **Full power cycle is required** when the NPU is hung — `sudo adb shell reboot` (OS reboot only) does NOT always reset the NPU hardware state.  Phase 3.5 saw the NPU stay hung across an OS reboot.  Unplug BOTH USB-Cs (Tab5 bottom + K144 top), wait 5+ seconds, replug.  This drops VBUS to the AX630C and resets the NPU silicon.
  - **Diagnostic — confirm NPU hang vs slow load:** ADB into K144 → `top -bn1 -p $(pgrep -f llm_llm | head -1)`.  If `%CPU=0.0` for 30+ sec while `loadavg > 2`, the NPU is hung (D-state kthreads).  If `%CPU > 50` and RES is growing, it's actually loading.
  - **Watch for `[npu_task_launch]` kthreads** in `ps -ef` — when present + accumulating CPU time, model load is genuinely in progress.  When PIDs are present but `top` shows zero activity, NPU is waiting on hardware that's not responding.
  - **First inference after a clean K144 power-up** typically takes ~5 min cold-start.  Phase 4 must do a background warm-up at Tab5 boot AND degrade gracefully when the K144 is hung (which is non-rare on this unit).
- **Prevention:** Document the cold-start cost in `voice_m5_llm.h` API contract.  Don't surface K144 LLM as "available" until a successful inference has completed (i.e. `voice_m5_llm_is_ready()` should require a positive infer round-trip, not just successful setup).  Phase 4 wiring must keep the user-facing assistant fully functional via Dragon-cloud paths during the cold-start window.

### Adding the ESP-IDF UART driver pulls in esp_ringbuf, IRAM-hungry, → boot panic
- **Date:** 2026-04-28
- **Symptom:** Initial K144 verification attempt: added a `/m5/probe` HTTP endpoint to `main/debug_server.c` that drove `UART_NUM_1` on GPIO 6/7.  Two attempts (full + minimal probe), both boot-panicked **before app_main** with `assert failed: vApplicationGetTimerTaskMemory port_common.c:97 (pxStackBufferTemp != NULL)`.  Moving `uart_port_c.c` to its own component in `bsp/tab5/` did NOT help — same panic.  WiFi never came up, display dark.  Tab5 recovered cleanly via `idf.py -p /dev/ttyACM0 flash` of the `main` firmware (~15 s).
- **Root Cause:** Pulling in `driver/uart.h` causes the linker to include `esp_ringbuf` (UART driver allocates internal ring buffers via xRingbufferCreate).  `esp_ringbuf`'s functions default to **IRAM-resident** — adds 5,634 bytes of `.text` to DIRAM.  Tab5's existing `main/` firmware was right at the edge of internal-RAM heap availability for early-boot allocations: `port_common.c:91` does `pvPortMalloc(configTIMER_TASK_STACK_DEPTH)` for the timer task stack at `vTaskStartScheduler`, which on ESP-IDF lands at `heap_caps_malloc(size, MALLOC_CAP_DEFAULT|MALLOC_CAP_8BIT)`.  With `configTIMER_TASK_STACK_DEPTH = 4096` (16 KB on RISC-V) and the now-shifted heap regions, no contiguous internal-RAM block was large enough — pvPortMalloc returned NULL, assert fired before app_main.
- **Fix:** Two-line sdkconfig change in `sdkconfig.defaults` — `CONFIG_RINGBUF_PLACE_FUNCTIONS_INTO_FLASH=y` (moves ~2 KB of esp_ringbuf out of IRAM) **+** `CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH=2048` (halves the timer task stack from 16 KB to 8 KB, matching stock FreeRTOS — already the precedent for the IDLE task at `sdkconfig.defaults:146`).  Either alone is insufficient; both together let Tab5 boot cleanly with the UART driver linked.  Verified via `m5ping` serial command — round-trip JSON to/from stacked K144 over GPIO 6/7 (`error.code: 0` MODULE_LLM_OK).
- **Prevention:** When pulling new ESP-IDF driver components into the firmware (uart, twai, mcpwm, anything that uses `esp_ringbuf` internally), **always add `CONFIG_RINGBUF_PLACE_FUNCTIONS_INTO_FLASH=y` to sdkconfig.defaults preemptively** — IRAM is the scarcer Tab5 resource.  And: timer-task stack depth shouldn't exceed 2048 stack words on this platform until someone diagnoses why the heap regions get split so unevenly at boot.  Diagnostic recipe — `idf.py size-files` between failing and clean builds, look at the DIRAM `.text` column for new IRAM additions; `cat /home/rebelforce/esp/esp-idf/components/freertos/port_common.c` for the assert source.

### M5-Bus Rear UART Aliases Side Port C (Same Wires)
- **Date:** 2026-04-28
- **Symptom:** While planning K144 LLM Module integration, ambiguity over whether stacked-on-rear vs side-Port-C connector required different firmware paths. The `bsp/tab5/uart_port_c.{c,h}` filename suggested side-only.
- **Root Cause:** Tab5 wires the same ESP32-P4 UART (GPIO 6 TX / GPIO 7 RX) in parallel to BOTH the side 4-pin Port C header AND M5-Bus rear pins 15/16 (labeled `PC_TX` / `PC_RX` in the schematic).  No physical multiplexing — the pins are the same copper.
- **Fix:** Documented the M5-Bus rear pinout in `docs/HARDWARE.md` and locked GPIO 6/7 on UART_NUM_1 as the canonical "Port C UART" regardless of which connector the module is plugged into.  K144's M5Module-LLM Arduino library uses M5Unified's `port_c_rxd` / `port_c_txd` pin names, confirming the K144 expects this same UART position via stacking.
- **Prevention:** When a Tab5 schematic shows `PC_*` (Port C) signals on a M5-Bus pin, treat them as identical to side Port C connector wires.  UART0 (G37/G38) on M5-Bus 13/14 is a separate UART — avoid for new modules because it conflicts with `idf.py monitor` boot console.

### EXT5V Rail Gates ALL Expansion 5V Together
- **Date:** 2026-04-28
- **Symptom:** Verifying power path for K144 stacked on M5-Bus rear: how do we power the module from Tab5 instead of relying on its top USB-C cable?
- **Root Cause:** A single IO-Expander pin (E1.P4 = `EXT5V_EN`) gates Tab5's external 5V rail to *every* expansion connector at once: HY2.0-4P (Port A / Grove), side Port C 5V, AND M5-Bus rear pin 28.  Default state at boot is OFF.  Driving it for the K144 also energises whatever's plugged into Grove and vice versa.
- **Fix:** Phase 1 of the K144 integration plan flips P4 high in the UART service init, with a coordination note in `docs/PLAN-grove.md` so both plans don't double-init the gate.  Bench rigs sidestep by leaving the K144's top USB-C plugged in for independent power during development.
- **Prevention:** Treat `EXT5V_EN` as a shared resource — wrap it behind a refcounted `tab5_ext5v_acquire()` / `release()` helper before adding the second user.  Any addon (Grove sensor, K144, side-port module) that needs 5V from Tab5 must go through that helper.  Don't toggle E1.P4 directly.

### IDF v5.5.x MIPI-DSI Is Broken
- **Date:** 2026-03-15
- **Symptom:** Display stopped working after upgrading to IDF v5.5.x. DSI init fails or produces no output.
- **Root Cause:** Regression in IDF v5.5.x MIPI-DSI driver for ESP32-P4.
- **Fix:** Pinned to IDF v5.4.3 (v5.4.2 lacks PSRAM XIP + TCM stack fixes needed by ESP-Hosted). Do not upgrade to v5.5.x.
- **Prevention:** Stick with IDF v5.4.3 until Espressif confirms the DSI fix in a later release. Test display output on any IDF upgrade before merging.

---

## ESP-IDF Pitfalls

### FreeRTOS TLSP Crash on vTaskDelete
- **Date:** 2026-03-18
- **Symptom:** Guru Meditation crash (LoadProhibited) when a task calls `vTaskDelete(NULL)` to delete itself.
- **Root Cause:** Thread Local Storage Pointer (TLSP) cleanup during `vTaskDelete` has a race condition on ESP32-P4. The task's TCB is freed while TLSP destructors still reference it (issue #18).
- **Fix:** Replaced `vTaskDelete(NULL)` with `vTaskSuspend(NULL)` everywhere. Suspended tasks are cleaned up by the idle task instead (commit ddcc2ab).
- **Prevention:** Never use `vTaskDelete(NULL)` on ESP32-P4. Always use `vTaskSuspend(NULL)` for self-deletion. Add a comment explaining why.

### SDIO Task Stack Overflow
- **Date:** 2026-03-14
- **Symptom:** Panic: Stack canary watchpoint triggered (SDIO task). Board reboots during WiFi operations.
- **Root Cause:** Default stack size of 5KB for SDIO tasks is insufficient. ESP-Hosted SDIO on P4 has deeper call chains that overflow at 5K (issue #15).
- **Fix:** Increased SDIO task stack to 8KB minimum (commits 0e51a53, 1e1f650).
- **Prevention:** Set SDIO task stacks to 8K+ in sdkconfig. Monitor high water mark with `uxTaskGetStackHighWaterMark()` during development.

### ESP-IDF WS Transport Fragments Control Frames
- **Date:** 2026-03-29
- **Symptom:** Dragon aiohttp server logs "Received fragmented control frame" and disconnects
- **Root Cause:** `esp_transport_ws_send_raw()` with `WS_TRANSPORT_OPCODES_PING` sends the ping as a fragmented frame. WebSocket spec forbids fragmenting control frames. aiohttp strictly enforces this.
- **Fix:** Use JSON text heartbeat `{"type":"ping"}` instead of WS ping opcode for keep-alive
- **Prevention:** Never use WS control frame opcodes with `esp_transport_ws_send_raw()` for keep-alive. Use application-level heartbeats.

### Flash String Literals Crash WebSocket Send
- **Date:** 2026-03-19
- **Symptom:** Crash in `esp_transport_ws_send_raw()` when sending a string literal like `"hello"`.
- **Root Cause:** String literals are stored in read-only flash on ESP32. The WebSocket transport may attempt to modify the send buffer in place. Writing to flash = instant crash (commit 5800076).
- **Fix:** Copy any string literal to a heap buffer before passing to `esp_transport_ws_send_raw()`.
- **Prevention:** Never pass string literals directly to transport send functions. Always `strdup()` or `malloc+memcpy` first. This applies to any ESP-IDF API that might modify its input buffer.

### LVGL Is NOT Thread-Safe
- **Date:** 2026-03-27
- **Symptom:** Guru Meditation crash when WiFi scan callback updated the UI (SSID list).
- **Root Cause:** LVGL is single-threaded by design. Any `lv_obj_*` call from a non-LVGL task corrupts internal state (commit 728110e).
- **Fix:** Wrapped all UI updates from non-LVGL tasks in `lv_lock()` / `lv_unlock()`.
- **Prevention:** Every single LVGL call outside the main LVGL task MUST be wrapped in `lv_lock()`/`lv_unlock()`. No exceptions. Grep for `lv_` calls and verify they have the lock.

### LVGL Shadow Rendering Crashes P4
- **Date:** 2026-03-22
- **Symptom:** Guru Meditation when rendering a widget with `LV_OBJ_FLAG_SHADOW` enabled.
- **Root Cause:** Shadow rendering exceeds the P4's draw buffer capacity. The shadow algorithm allocates temporary buffers that overflow available memory (commit 7df64b3).
- **Fix:** Disabled all shadows globally. Removed any `LV_OBJ_FLAG_SHADOW` usage.
- **Prevention:** Do not use shadows on ESP32-P4. If visual depth is needed, use border/outline styling instead.

### esp_codec_dev Uses 8-Bit I2C Addresses
- **Date:** 2026-03-15
- **Symptom:** ES7210 and ES8388 codec init succeeded (no error returned) but codecs produced no audio. I2C writes went to wrong addresses.
- **Root Cause:** The `esp_codec_dev` library expects 8-bit I2C addresses (left-shifted by 1), not the standard 7-bit form. ES7210 is `0x80` not `0x40`. ES8388 is `0x20` not `0x10` (commit 34efa77).
- **Fix:** Changed all codec I2C addresses to 8-bit form.
- **Prevention:** When using `esp_codec_dev`, always use 8-bit addresses. Add a comment next to each address: `0x80 /* 7-bit: 0x40 */`.

### Static BSS Buffers Exhaust Internal RAM
- **Date:** 2026-03-29
- **Symptom:** Boot crash with `assert failed: app_startup.c:86`. Board never reaches `app_main()`.
- **Root Cause:** Two large static arrays (48KB playback buffer + 12KB upsample buffer = 60KB) were declared as global/static BSS. These consume internal SRAM, which is limited (~512KB shared with FreeRTOS). Combined with other static allocations, internal RAM was exhausted before the scheduler started.
- **Fix:** Changed to `heap_caps_malloc(MALLOC_CAP_SPIRAM)` for both buffers, allocating from the 32MB PSRAM instead.
- **Prevention:** Any buffer larger than 4KB should use `heap_caps_malloc(MALLOC_CAP_SPIRAM)`. Never declare large static arrays. Grep for large static buffers periodically.

---

## Audio Pipeline Lessons

### I2S TDM MONO Only Captures Slot 0
- **Date:** 2026-03-21
- **Symptom:** 4-channel mic array (ES7210) only captured from one microphone. The other three channels were silent. Audio had 12kHz aliased noise (5750Hz zero-crossings).
- **Root Cause:** `I2S_SLOT_MODE_MONO` only captures TDM slot 0 into the DMA buffer. The other 3 slots are discarded by hardware. `voice.c` was reading every 4th sample expecting interleaved 4-channel data, but only got slot 0 data, creating aliasing artifacts (commit d547338, issue #25).
- **Fix:** Changed to `I2S_SLOT_MODE_STEREO` which captures all TDM slots interleaved in the DMA buffer.
- **Prevention:** For multi-channel TDM capture, always use `I2S_SLOT_MODE_STEREO`, never `MONO`. The naming is misleading -- `STEREO` really means "capture all configured slots."

### ES8388 Custom Register Init NEVER Produced Audio
- **Date:** 2026-03-30
- **Symptom:** No audio output from speaker despite weeks of debugging. ES8388 was initialized, I2S data was being written, speaker amp was enabled. Zero sound.
- **Root Cause:** Our custom ES8388 register init had FIVE differences from the working esp_codec_dev library init:
  1. DACCONTROL1=0x1E (DSP/PCM mode) — library uses 0x18 (standard I2S mode)
  2. CONTROL1=0x36 — library uses 0x12
  3. CONTROL2=0x72 — library uses 0x50
  4. Missing DACCONTROL24/25 (LOUT/ROUT output volume registers, should be 0x1E = 0dB)
  5. Missing internal DLL disable (regs 0x35=0xA0, 0x37=0xD0, 0x39=0xD0)
  Additionally, our TDM-for-TX approach was wrong: M5Stack uses STD Philips for TX + TDM for RX on the same I2S port. ESP32-P4 supports mixed modes on the same port.
- **Fix:** Replaced entire custom register init with `es8388_codec_new()` from `esp_codec_dev` library. Changed I2S TX from TDM to STD Philips mode (matching M5Stack BSP). Removed TDM frame expansion. All playback now goes through `esp_codec_dev_write()`.
- **Prevention:** NEVER write custom codec register sequences. Use the esp_codec_dev library for ES8388/ES7210 initialization — it has a tested, proven register sequence. The 6 hours spent debugging custom registers could have been avoided by using the library from day one.

### ES7210 Returns All Zeros (6 Register Bugs)
- **Date:** 2026-03-19
- **Symptom:** ES7210 ADC output was all zeros. I2C communication worked fine.
- **Root Cause:** The hand-rolled ES7210 init sequence had six register-level bugs: wrong clock source, ADC not enabled, analog power not configured, and three others (commit 5b0688b, issue #22).
- **Fix:** Threw away the custom init and replaced it entirely with the `esp_codec_dev` library, which has a tested ES7210 driver (commit 58b3f15).
- **Prevention:** Use `esp_codec_dev` for all codec initialization. Do not write custom register sequences unless the codec is not supported by the library.

### ES8388 Mixer Routing for Stereo Output
- **Date:** 2026-03-25
- **Symptom:** Speaker output was silent or mono-only. One channel missing.
- **Root Cause:** ES8388 DACCONTROL17 mixer register controls L/R DAC output routing. Incorrect configuration muted one channel or routed both to the same output (commit 6fa0407, issue #30).
- **Fix:** Set DACCONTROL17 to correct mixer routing for stereo L/R output.
- **Prevention:** When configuring ES8388, always explicitly set DACCONTROL17 mixer register. Refer to ES8388 datasheet Table 26 for routing options.

### TTS 16kHz vs I2S 48kHz (Chipmunk Effect)
- **Date:** 2026-03-29
- **Symptom:** TTS playback sounded like a chipmunk -- 3x too fast and too high pitched.
- **Root Cause:** Dragon voice server sends 16kHz PCM. Tab5 I2S TX runs at 48kHz (hardware rate). Without resampling, the 16kHz samples are played at 48kHz, tripling the speed (commit 710473a).
- **Fix:** Added linear interpolation upsampler: 16kHz to 48kHz (3x) before writing to the I2S playback buffer.
- **Prevention:** Always check sample rates at both ends of an audio pipeline. If source and sink rates differ, add a resampler. Document the expected rates in comments.

### Audio GPIO Pin Mapping Was Wrong in Docs
- **Date:** 2026-03-14
- **Symptom:** No audio I2S clocks or data on scope. Pins were toggling but not the right ones.
- **Root Cause:** M5Stack documentation had incorrect GPIO assignments for audio I2S. The actual PCB traces are: MCLK=30, BCK=27, WS=29, DOUT=26, DIN=28 (commit 4830647, issue #16).
- **Fix:** Traced the actual PCB connections and updated pin definitions.
- **Prevention:** Never trust vendor pin maps for audio. Verify with a scope or continuity tester. Document confirmed pin assignments in a hardware header with a "VERIFIED" comment.

---

## Dragon/ARM64 Quirks

### Piper TTS Model File Permissions
- **Date:** 2026-03-29
- **Symptom:** `PermissionError` when Piper TTS tried to load voice model files.
- **Root Cause:** Models were cached under `/home/rock/.cache/` but the voice service systemd unit had `User=radxa`. Different user, no read access.
- **Fix:** Copied model caches to `/home/radxa/.cache/` and set `User=radxa` consistently in the service file.
- **Prevention:** Always verify that the systemd `User=` matches the home directory where cached files live. After any user migration, check all service files.

### rsync Not Available on Dragon
- **Date:** 2026-03-10
- **Symptom:** `rsync: command not found` when deploying to Dragon.
- **Root Cause:** Radxa Zero 3W minimal image does not include rsync.
- **Fix:** Used `cp -r` locally or `scp` for remote copies.
- **Prevention:** Don't assume rsync exists on embedded ARM boards. Use `scp` or `cp -r` in deployment scripts. Or install rsync explicitly in the provisioning step.

### Moonshine V2 STT Replaces whisper.cpp on ARM64
- **Date:** 2026-03-25
- **Symptom:** whisper.cpp STT was too slow on ARM64 Dragon board. Transcription latency was unacceptable.
- **Root Cause:** whisper.cpp is not well-optimized for ARM64 without GPU acceleration.
- **Fix:** Replaced with Moonshine V2 using ONNX runtime, which runs significantly faster on ARM64 (commit 0f3f7b8, issue #31).
- **Prevention:** Benchmark STT models on the actual target hardware before committing to one. Prefer ONNX-based models on ARM64.

### Ollama Inference Latency on ARM64
- **Date:** 2026-03-20
- **Symptom:** Full STT-to-LLM-to-TTS voice loop takes ~20 seconds with gemma3:4b.
- **Root Cause:** Ollama running on ARM64 CPU without GPU. LLM inference is the bottleneck.
- **Fix:** Accepted the latency as a tradeoff. Set generous timeouts in the Tab5 client. Added loading/thinking indicators in the UI.
- **Prevention:** Account for 15-25 second LLM latency in all timeout and UX designs. Consider smaller models (gemma3:1b) if latency is critical.

---

## Deployment Issues

### systemd User= Field Mismatch
- **Date:** 2026-03-29
- **Symptom:** Service failed to start. Journal showed permission errors accessing working directory.
- **Root Cause:** Service files were copied from a machine where the primary user was `rock`. On the current Dragon, the user is `radxa`. The `User=` and `WorkingDirectory=` fields pointed to a non-existent user/path.
- **Fix:** Updated `User=radxa` and `WorkingDirectory=/home/radxa/...` in all service files.
- **Prevention:** After copying service files between machines, always verify `User=`, `WorkingDirectory=`, and `ExecStart=` paths. Add a deployment checklist.

### sudo tee Heredoc Fails Over SSH
- **Date:** 2026-03-15
- **Symptom:** Heredoc piped to `sudo tee` over SSH produced empty or corrupted files.
- **Root Cause:** Nested heredocs combined with sudo and SSH quoting interact badly. The shell expansion and redirection across the SSH boundary drops content silently.
- **Fix:** Write the file locally, `scp` it to the target as a temp file, then `sudo cp` to the final destination.
- **Prevention:** Never use `sudo tee` with heredocs over SSH. Always scp-then-cp for privileged file writes on remote hosts.

### mDNS Service Discovery Setup
- **Date:** 2026-03-12
- **Symptom:** Tab5 could not discover Dragon on the network. mDNS queries returned nothing.
- **Root Cause:** `avahi-publish-service` on Dragon was not advertising the correct service type or port.
- **Fix:** Configured `avahi-publish-service` to advertise `_tinkerclaw._tcp` with correct ports: dragon_server on 3501, voice on 3502.
- **Prevention:** After changing any server port, update the avahi-publish command. Test discovery from Tab5 with `mdns_query_ptr()` before assuming it works.

---

## Architecture Decisions

### Full-Duplex Shared I2S Bus (ES8388 + ES7210)
- **Date:** 2026-03-21 (updated 2026-03-30)
- **Symptom:** N/A (design constraint)
- **Root Cause:** Tab5 hardware routes both ES8388 (DAC/speaker) and ES7210 (ADC/mic) to the same I2S bus (I2S_NUM_1). This is a PCB-level constraint, not a software choice.
- **Fix:** TX uses STD Philips mode (ES8388 expects standard I2S), RX uses TDM 4-slot (ES7210 quad-mic). ESP32-P4 supports mixed STD/TDM modes on the same I2S port — confirmed working by M5Stack BSP and our own testing. ES8388 initialized via esp_codec_dev library. All playback through esp_codec_dev_write().
- **Prevention:** Accept this constraint. Match M5Stack BSP exactly: STD TX + TDM RX. Never use custom ES8388 register init — always use esp_codec_dev library.

### 48kHz to 16kHz Downsample for STT
- **Date:** 2026-03-21
- **Symptom:** N/A (design decision)
- **Root Cause:** Hardware mic runs at 48kHz. All STT models (Moonshine, Whisper) expect 16kHz input.
- **Fix:** Simple 3:1 decimation (take every 3rd sample). Works because 48000/16000 = 3 exactly. No anti-alias filter needed for speech bandwidth.
- **Prevention:** If the hardware sample rate ever changes from 48kHz, revisit the decimation ratio. If STT model input rate changes from 16kHz, recalculate.

### Separate Ports for Dragon Server and Voice Pipeline
- **Date:** 2026-03-12
- **Symptom:** N/A (design decision)
- **Root Cause:** Dragon CDP browser control and voice pipeline have different reliability and restart requirements.
- **Fix:** dragon_server on port 3501, voice_service on port 3502, dashboard on port 3500. Each is an independent systemd service.
- **Prevention:** Keep services on separate ports. Independent processes allow independent restarts. Dashboard aggregates status from both.

### PSRAM for All Large Buffers
- **Date:** 2026-03-29
- **Symptom:** N/A (design rule established after BSS exhaustion crash)
- **Root Cause:** ESP32-P4 has 32MB PSRAM but only ~512KB internal SRAM shared with FreeRTOS heap, stacks, and BSS.
- **Fix:** Established rule: any buffer >4KB must use `heap_caps_malloc(MALLOC_CAP_SPIRAM)`.
- **Prevention:** Grep for large static arrays (`static.*\[.*\]`) periodically. Code review should reject any static buffer over 4KB. Document the 4KB threshold in CLAUDE.md.

### Desktop SDL2 Simulator — Build Architecture
- **Date:** 2026-03-30
- **Symptom:** N/A (design decision)
- **Root Cause:** Testing UI required flashing hardware every iteration (~2 minutes per cycle). Needed a faster dev loop.
- **Fix:** Built SDL2 desktop simulator in `sim/`. All `ui_*.c` files compile for both x86_64/ARM64 Linux and ESP32-P4 without modification. `TINKEROS_SIMULATOR` define controls platform behavior via `main/ui_port.h`. Hardware stubs in `sim/stubs.c` return safe defaults. `make && ./tinkeros_sim` gives a 720x1280 interactive window. Mouse = touch.
- **Prevention:** Add any new ui_*.c to `sim/CMakeLists.txt` so it compiles in both environments.

### ui_port.h — The ONLY Platform Include for UI Files
- **Date:** 2026-03-30
- **Symptom:** N/A (Phase 1 refactor)
- **Root Cause:** `ui_*.c` files directly included `esp_log.h`, `esp_heap_caps.h`, `esp_task_wdt.h`, `freertos/*.h` — none of which exist in the simulator environment.
- **Fix:** Created `main/ui_port.h`. On simulator: maps to printf/SDL_Delay/tracked malloc. On firmware: maps to ESP_LOGx/heap_caps_malloc/esp_task_wdt_reset/vTaskDelay. All `ui_*.c` files include ONLY this header for platform primitives.
- **Prevention:** Never include `esp_*.h` or `freertos/*.h` directly in `ui_*.c` files. Use `UI_LOGI`, `UI_MALLOC_PSRAM`, `UI_FREE`, `UI_WDT_RESET`, `UI_DELAY_MS`, `UI_TIME_MS`.

### No Hardcoded Voice Response Timeout
- **Date:** 2026-03-29
- **Symptom:** Tab5 disconnected from Dragon voice server after 120s while LLM was still processing
- **Root Cause:** Hardcoded VOICE_RESPONSE_TIMEOUT_MS forced disconnection regardless of whether Dragon was still working. ARM64 LLM inference is slow (~0.24 tok/s).
- **Fix:** Removed timeout entirely. Tab5 stays in PROCESSING state as long as WebSocket is alive. JSON heartbeat every 15s prevents TCP idle timeout. Only disconnects on WS error or Dragon error message.
- **Prevention:** Never use fixed timeouts for LLM inference. Processing time varies by model, prompt length, and hardware. Use connection health checks instead.

### lv_screen_load() crashes from tileview touch events
- **Date:** 2026-03-30
- **Symptom:** Device hard faults (Instruction access fault + WDT) when any app icon is tapped on the tileview apps page. Camera, WiFi, Files, Settings, Chat — ALL crash.
- **Root Cause:** Creating a new screen via lv_screen_load() during a tileview touch event triggers a hard fault. The same ui_chat_create() code works perfectly when called from the HTTP /open endpoint (different task context) but crashes when called from the LVGL touch callback.
- **Workaround:** Use lv_layer_top() overlay instead of lv_screen_load(). The /open?screen=chat HTTP endpoint works reliably.
- **Fix:** TBD — needs investigation in desktop simulator. Likely a tileview + screen load conflict in LVGL v9.2.2.
- **Prevention:** Never call lv_screen_load() from a tileview touch event. Use overlays or deferred HTTP calls instead.

### sdkconfig changes require rm -rf build
- **Date:** 2026-03-30
- **Symptom:** Changed CONFIG_ESP_TASK_WDT_TIMEOUT_S from 5 to 60 in sdkconfig, but build still used 5s.
- **Root Cause:** ESP-IDF caches config in build/config/sdkconfig.h. Incremental builds don't regenerate from sdkconfig.
- **Fix:** Always `idf.py fullclean build` after sdkconfig changes, then `idf.py set-target esp32p4` to be safe.
- **Prevention:** When changing sdkconfig, always clean build.

### sdkconfig CONFIG_IDF_TARGET must be esp32p4 for Tab5
- **Date:** 2026-03-31
- **Symptom:** Full clean build failed with `fatal error: esp_lcd_mipi_dsi.h: No such file directory` in debug_server.c and display.c.
- **Root Cause:** sdkconfig had `CONFIG_IDF_TARGET="esp32"` (plain ESP32, no DSI support). Tab5 uses ESP32-P4 with MIPI DSI display panel. The DSI include paths are only exposed when the target supports MIPI DSI.
- **Fix:** Run `idf.py set-target esp32p4` in a clean worktree. This updates sdkconfig and regenerates the build.
- **Prevention:** Always verify `CONFIG_IDF_TARGET` matches the actual hardware (esp32p4 for Tab5). After any `idf.py fullclean`, re-run `idf.py set-target esp32p4` to be safe.

### Managed_components esp_hosted Hash Error on Clean Build
- **Date:** 2026-03-31
- **Symptom:** `idf.py build` fails: `Hash file does not exist` for `managed_components/espressif__esp_hosted`.
- **Root Cause:** ESP-IDF component manager tracks managed_components in a local hash file. When the managed_components directory is checked into git (as it is here) alongside a `dependencies.lock` that points to a local path, the manager gets confused. The tracked `managed_components/espressif__esp_hosted` directory interferes with the component manager's expected layout.
- **Fix:** Move the broken tracked managed_components directory aside in a disposable worktree (not the primary checkout): `mv managed_components/espressif__esp_hosted managed_components/espressif__esp_hosted.bak`. The component manager then rebuilds its state cleanly. This is non-destructive and reversible.
- **Prevention:** Do not check managed_components directories into git. Add them to .gitignore. The `dependencies.lock` file is sufficient for reproducible builds.

### ESP32-P4 Flashes to ROM Download Mode Instead of Booting App
- **Date:** 2026-03-31
- **Symptom:** After flashing, board shows `rst:0x17 (CHIP_USB_UART_RESET), boot:0x204 (DOWNLOAD(USB/UART0/SPI)) waiting for download` instead of booting the app. Ctrl+C / Ctrl+R over serial did not recover.
- **Root Cause:** ESP32-P4 reliably enters ROM download mode after esptool finishes flashing when using USB-Serial/JTAG. This is a known P4 boot behavior — the ROM bootloader, not the flashed app, starts after USB reset events.
- **Fix:** Trigger a watchdog reset from the ROM stub while the chip is in this state:
  ```
  python -m esptool --chip esp32p4 -p /dev/ttyACM0 --before no_reset --after watchdog_reset read_mac
  ```
  This forces the ROM watchdog to fire, which causes a clean app boot from SPI flash.
- **Prevention:** If flashing succeeds but the board boots to ROM mode, always try the watchdog reset command before assuming the flash failed.

### BMI270 Post-Reset Readiness Race Causes IMU Init Failure
- **Date:** 2026-03-31
- **Symptom:** IMU init fails with `ESP_ERR_INVALID_STATE`. I2C bus scan shows `0x68` (BMI270 present). Soft reset succeeds. But chip-ID read fails immediately after reset.
- **Root Cause:** BMI270 needs a short settle time after soft reset before it can respond to register reads. The first chip-ID read happens too early, and `i2c_master_bus_add_device()` returns `ESP_ERR_INVALID_STATE` for what is actually a transient not-ready condition.
- **Fix:** After BMI270 soft reset, wait 10ms then retry chip-ID read up to 5 times with 5ms spacing between attempts. Only fail if all retries are exhausted. This is in `main/imu.c`.
- **Prevention:** Any sensor that requires a settling period after reset needs a retry loop. Check the datasheet and add appropriate delay+retry logic rather than assuming the device is immediately ready.

---

## Session Bugs (2026-04-06)

### #41 — tts_end Dropped by DMA Contention (Keepalive vs Activity Timer)
- **Date:** 2026-04-06
- **Symptom:** `tts_end` message was never received by the client; voice playback appeared to hang indefinitely after TTS audio finished.
- **Root Cause:** The keepalive timer and the response timeout timer were the same timer. Every keepalive ping reset the response timeout, so the timeout could never fire. The `tts_end` frame was dropped by DMA contention, and the timeout that should have recovered from this never triggered.
- **Fix:** Separated the keepalive timer from the activity/response timeout timer so they operate independently. Keepalive pings no longer reset the response timeout.
- **Prevention:** Never multiplex unrelated timeout semantics onto a single timer. Keepalive (connection liveness) and response timeout (application-level deadline) are separate concerns and must use separate timers.

### #42 — Persistent Keyboard Across Page Transitions
- **Date:** 2026-04-06
- **Symptom:** Once the Notes screen's "Type Note" textarea opened the on-screen keyboard, the keyboard persisted across all screens — navigating to Home, Voice, Settings, etc. still showed the keyboard overlay.
- **Root Cause:** No code dismissed the keyboard when leaving a screen. The LVGL keyboard remained attached and visible because neither the tileview scroll handler nor the nav tab click handler called `ui_keyboard_hide()`.
- **Fix:** Added `ui_keyboard_hide()` calls in both the tileview scroll event handler and the navigation tab click handler, ensuring the keyboard is dismissed on any page transition.
- **Prevention:** Any screen that can open a keyboard must ensure the keyboard is dismissed on exit. Add `ui_keyboard_hide()` to all navigation/transition code paths.

### #43 — Notes month==0 Array Underflow Crash
- **Date:** 2026-04-06
- **Symptom:** Crash (array underflow) when displaying a note's date. Device rebooted.
- **Root Cause:** `month_names[n->month - 1]` was used to look up the month string, but `n->month` could be 0 (uninitialized or default). `month_names[0 - 1]` = `month_names[-1]`, which is an out-of-bounds access on an embedded system with no memory protection — instant crash.
- **Fix:** Added bounds check: clamp `n->month` to the valid range [1, 12] before indexing into `month_names[]`.
- **Prevention:** Always validate array indices derived from external or stored data before use. Treat any value read from NVS, SD card, or network as untrusted input.

### #44 — Static IP Unreliable (Race with DHCP)
- **Date:** 2026-04-06
- **Symptom:** Static IP configuration was intermittently ignored. Device sometimes got a DHCP address instead of the configured static IP.
- **Root Cause:** The static IP was being set in the WiFi event handler, after `esp_wifi_connect()` had already triggered DHCP. The DHCP client could complete before the static IP was applied, creating a race condition.
- **Fix:** Set the static IP configuration (via `esp_netif_set_ip_info()` or equivalent) before calling `esp_wifi_connect()`, ensuring DHCP is never started.
- **Prevention:** Always configure static IP before initiating WiFi connection, not in the connected event handler. The event handler is too late — DHCP may have already won the race.

### #45 — Voice Test Wait Too Short (5s for Moonshine)
- **Date:** 2026-04-06
- **Symptom:** Voice connectivity test reported failure even though Dragon was healthy and responding.
- **Root Cause:** The test had a 5-second timeout, but the Dragon Moonshine STT model takes 7-10 seconds to load on first invocation (cold start). The test timed out before Dragon could respond.
- **Fix:** Increased the voice test timeout from 5 seconds to 15 seconds to accommodate model cold-start loading.
- **Prevention:** Timeouts for first-contact tests must account for cold-start latency of the backend service. Always measure worst-case (cold boot) timing, not steady-state.

### #46 — Cloud Mode Auto-Restore Race Condition
- **Date:** 2026-04-06
- **Symptom:** Switching to cloud mode caused Dragon's voice pipeline to crash or produce garbled output. The pipeline attempted a hot-swap during active session startup.
- **Root Cause:** On WebSocket connect, the client automatically sent a `config_update` message to restore the last-known cloud config. This arrived while Dragon was still processing `session_start`, causing a pipeline hot-swap race — the TTS/STT backends were being swapped out while actively being used.
- **Fix:** Removed the automatic config restore on connect. Config updates are now only sent explicitly by user action, not automatically on reconnect.
- **Prevention:** Never auto-send config mutations during connection setup. Connection handshake should be read-only (register, resume session). Config changes require explicit user intent.

### #47 — WAV Player Hardcoded 16kHz Sample Rate
- **Date:** 2026-04-06
- **Symptom:** WAV files recorded at sample rates other than 16kHz played back at wrong speed — too fast or too slow depending on the actual sample rate.
- **Root Cause:** The WAV player skipped the WAV header entirely and assumed all audio was 16kHz 16-bit mono PCM. It did not parse the header to read the actual sample rate.
- **Fix:** Modified the WAV player to read and parse the WAV header, extracting the actual sample rate, bit depth, and channel count. Playback now uses the correct sample rate from the file.
- **Prevention:** Never hardcode audio format parameters. Always parse the container header (WAV, FLAC, etc.) to determine the actual format. Add assertions if the format is outside supported ranges.

### #48 — Chat Textarea No Keyboard on Tap
- **Date:** 2026-04-06
- **Symptom:** Tapping the chat text input area did nothing — no on-screen keyboard appeared. Users could not type messages.
- **Root Cause:** The textarea widget had no click event callback registered to trigger the on-screen keyboard. LVGL textareas do not automatically show a keyboard on focus — the application must explicitly handle this.
- **Fix:** Added `cb_textarea_click` callback to the chat textarea that calls the keyboard show function when the textarea is tapped/focused.
- **Prevention:** Every textarea in the UI that expects user text input must have a click/focus handler that shows the on-screen keyboard. Add this to the UI component checklist.

### #49 — Camera Gallery Button No Handler
- **Date:** 2026-04-06
- **Symptom:** The gallery button on the camera screen appeared clickable (visual feedback on press) but tapping it did nothing.
- **Root Cause:** The button was created with visual styling but no event callback was registered. The button had no `LV_EVENT_CLICKED` handler.
- **Fix:** Added an event callback that opens the file browser / gallery view when the gallery button is clicked.
- **Prevention:** Every UI button must have an event callback registered at creation time. Add a lint/review step that checks all `lv_obj_add_event_cb` calls match all clickable widgets.

### #50 — Camera Capture Counter Resets on Reboot
- **Date:** 2026-04-06
- **Symptom:** After reboot, new camera captures overwrote existing files on the SD card. Files named `IMG_0000.jpg`, `IMG_0001.jpg`, etc. were lost.
- **Root Cause:** The capture filename counter always started at 0 on boot. It did not scan the SD card for existing files to determine the next available number.
- **Fix:** On boot, scan the capture directory for existing files and set the counter to max(existing) + 1 before starting new captures.
- **Prevention:** Any auto-incrementing filename counter must persist across reboots. Either store the counter in NVS or scan existing files on startup. Never assume the storage is empty.

### #51 — lv_async_call Cast Warning (Function Pointer Type Mismatch)
- **Date:** 2026-04-06
- **Symptom:** Compiler warning: incompatible function pointer type passed to `lv_async_call()`.
- **Root Cause:** Functions with different signatures (e.g., `void fn(specific_type*)`) were being cast to `void (*)(void*)` when passed to `lv_async_call()`. While this often works in practice on most ABIs, it is undefined behavior in C and triggers compiler warnings.
- **Fix:** Created proper wrapper functions with the correct `void (*)(void*)` signature that cast the argument internally before calling the real function.
- **Prevention:** Never cast function pointers to a different type. Always write a thin wrapper with the correct signature. Treat all `-Wincompatible-pointer-types` warnings as errors.

### #52 — Year Comparison Always-False (uint8_t > 2000)
- **Date:** 2026-04-06
- **Symptom:** Date validation logic never triggered, allowing invalid dates through. Year was always treated as "valid" regardless of RTC state.
- **Root Cause:** The RTC year was stored as `uint8_t` (0-255). The comparison `if (year > 2000)` can never be true for a `uint8_t` because max value is 255. The condition was dead code.
- **Fix:** Changed the comparison to use the RTC year value directly (e.g., `year > 24` for years after 2024, where the RTC stores year as offset from 2000), or cast to a wider type before adding the century offset.
- **Prevention:** Enable `-Wtype-limits` compiler warning to catch comparisons that are always true or always false due to type range. Review all comparisons involving `uint8_t` / `uint16_t` for range issues.

### ESP-SR AFE + Wake Word Integration (Phase 2)

- **Date:** 2026-04-06
- **Symptom:** ESP-SR AFE crashes on first `feed()` call with "Load access fault" on Core 1.
- **Root Cause:** Multiple issues compounded:
  1. **Feed chunk size mismatch:** AFE expects exactly `get_feed_chunksize()` samples per `feed()` call (512 for "MR", 1024 for "MMR"). Feeding 960 samples (320 samples x 3 channels from 20ms mic frame) caused buffer overrun inside the AFE library.
  2. **PSRAM stack + stack-allocated arrays:** Task stacks in PSRAM have cache coherency issues with large local arrays. Stack-allocated `int16_t afe_tmp[960]` on a PSRAM stack caused Store access faults.
  3. **2-mic mode ("MMR") exhausts SRAM:** With only 44KB SRAM free after AFE init, the HIGH_PERF 2-mic mode couldn't allocate working buffers. Switching to "MR" (1-mic + 1-ref, LOW_COST) leaves 68KB free and runs stable.
  4. **30-second mic task timeout:** The `MAX_RECORD_FRAMES_ASK` limit (1500 frames = 30s) killed the mic capture task even in always-listening mode, starving the AFE feed.
- **Fix:**
  1. Query `get_feed_chunksize()` at init, accumulate samples in a ring-style buffer, only call `feed()` when exactly `chunksize` samples are ready.
  2. Allocate `afe_tmp` and `afe_buf` on heap (PSRAM), not stack. Use `xTaskCreatePinnedToCoreWithCaps(MALLOC_CAP_SPIRAM)` for task stacks.
  3. Use "MR" format (1 mic + 1 ref) with `AFE_MODE_LOW_COST`.
  4. Skip 30s duration limit when `s_afe_enabled` is true.
- **Prevention:** Always call `get_feed_chunksize()` before `feed()`. Never use stack-allocated arrays >256 bytes on PSRAM-stack tasks. Monitor SRAM free after AFE init — below 40KB is danger zone.

- **Date:** 2026-04-06
- **Symptom:** WakeNet9 "hilexin" model never triggers `wake=1` despite VAD detecting speech.
- **Root Cause:** "hilexin" (嗨乐鑫) is trained on native Mandarin Chinese pronunciation. English speakers saying "Hi Lexin" don't match the acoustic pattern. TTS-generated speech also doesn't trigger it (by design — prevents TV/speaker false triggers).
- **Fix:** Switched to "hiesp" model. Still didn't trigger — likely a TDM slot mapping issue. The AEC reference channel may be on slot 1 (near-zero RMS when speaker silent), not slot 2. If the mic signal is fed as the reference, AEC cancels the actual voice.
- **Prevention:** Verify TDM slot mapping before integrating AEC. Play a known tone through the speaker and log per-channel RMS — the channel that shows the tone is the reference. Slots 0 and 2 both showed high RMS (both are mics), slot 1 was near-zero (likely the AEC ref or unused).
- **Status:** AFE pipeline works (feed/fetch stable, VAD detects speech, no crashes). Wake word detection needs slot mapping verification and possibly custom "Hey Tinker" model from Espressif.

---

## Session Bugs (2026-04-10)

### Speaker Buzzing at Boot
- **Date:** 2026-04-10
- **Symptom:** Speaker amp produces audible buzz/noise during boot before any audio plays.
- **Root Cause:** IO expander PI4IOE1 register OUT_SET was 0b01110110 which sets P1 (SPK_EN) HIGH at init. Speaker amp was enabled from the moment the IO expander initialized, amplifying noise on the undriven I2S bus.
- **Fix:** Changed OUT_SET to 0b01110100 (P1 LOW). Speaker amp now off by default, only enabled when audio playback starts.
- **Prevention:** Always default power-amplifier enable pins to OFF in IO expander init. Amps should only be enabled when there is a valid audio signal to play.

### Settings Screen WDT Crash (Final Fix)
- **Date:** 2026-04-10
- **Symptom:** Navigating to Settings triggers 5s watchdog timeout and reboot.
- **Root Cause:** LVGL flex layout on s_scroll container with 100+ widgets causes O(n²) layout recalculation on every child addition. On DPI display with PSRAM cache, each recalc takes seconds, cumulative time exceeds any WDT timeout.
- **Fix:** Complete rewrite — eliminated flex layout, use manual Y positioning with labels placed directly on s_scroll. No row containers. Creation time: 28ms (down from 2+ minutes). Settings is now a fullscreen overlay on the home screen (not a separate lv_screen) because screen transitions also crash the draw pipeline.
- **Prevention:** Never use LVGL flex layout when adding many children dynamically. Use manual positioning with a running Y offset. For screens with 50+ widgets, always benchmark creation time and ensure it stays under 100ms.

### LVGL Screen Transition Crash
- **Date:** 2026-04-10
- **Symptom:** lv_screen_load() from settings screen to home screen causes Guru Meditation (Store/Load access fault) in lv_draw_add_task.
- **Root Cause:** LVGL's 64KB internal memory pool is too small for the DPI 720x1280 display. Screen transitions require draw buffers for both screens simultaneously, exhausting the pool. Attempted fixes: increased pool to 128KB (binary too large), enabled pool expand (helps but fragments), switched to system malloc (breaks FreeRTOS idle task).
- **Fix:** Make settings a fullscreen overlay on the home screen (child of home, not separate screen). No screen transitions needed — just hide/show.
- **Prevention:** Avoid lv_screen_load() on high-resolution displays with limited memory pools. Use overlays (children of the existing screen) instead of separate screens wherever possible.

### LVGL Memory Pool Configuration
- **Date:** 2026-04-10
- **Symptom:** N/A (configuration note)
- **Root Cause:** CONFIG_LV_MEM_SIZE_KILOBYTES=96 with CONFIG_LV_MEM_POOL_EXPAND_SIZE_KILOBYTES=1024. The 96KB base pool fits in internal SRAM BSS. The 1024KB expand feature lets LVGL request additional blocks from system heap (PSRAM) when the initial pool is exhausted. Critical for settings screen which creates ~50 objects.
- **Fix:** Set both values in sdkconfig. The expand feature is the safety net that prevents out-of-memory crashes without requiring an oversized initial pool.
- **Prevention:** Always enable LV_MEM_POOL_EXPAND on memory-constrained devices with complex UIs. Monitor LVGL memory usage with lv_mem_monitor() during development to verify the expand pool is not being hit excessively.

### WiFi WPA2/WPA3 Mixed Mode Failure
- **Date:** 2026-04-10
- **Symptom:** ESP-Hosted C6 WiFi co-processor fails to authenticate with WPA2/WPA3 mixed mode routers (auth=7). Gets reason=2 (AUTH_EXPIRE) and reason=205 (CONNECTION_FAIL) indefinitely. Scan finds AP at -30 RSSI, credentials verified correct.
- **Root Cause:** ESP-Hosted v1.4.0 C6 firmware has a bug with WPA3 SAE transition on mixed-mode APs.
- **Fix:** Changed router to WPA2-PSK only. Also fixed auth threshold from WPA2_PSK to WPA_PSK for broader compatibility.
- **Prevention:** If WiFi authentication fails on a mixed WPA2/WPA3 AP, try WPA2-only first to isolate. Set auth threshold to WPA_PSK (not WPA2_PSK) for maximum compatibility. Track ESP-Hosted firmware updates for WPA3 SAE transition fixes.

### Voice WebSocket Transport Leaks
- **Date:** 2026-04-10
- **Symptom:** Memory leak on every voice_connect() call. Heap free decreased monotonically over reconnect cycles.
- **Root Cause:** TCP transport handle leaked on every voice_connect() call (created TCP+WS wrapper, then immediately destroyed WS but not TCP). SSL transport also leaked on ngrok connect failure.
- **Fix:** Removed orphaned TCP creation and added proper SSL cleanup paths. Every transport allocation now has a matching destroy on all error paths.
- **Prevention:** For every esp_transport_*_init() call, ensure a matching esp_transport_destroy() exists on ALL code paths (success, failure, early return). Audit transport lifecycle with heap tracing if leaks are suspected.

### WiFi Event Handler vTaskDelay
- **Date:** 2026-04-10
- **Symptom:** WiFi disconnect retry was unreliable. Cascading connection failures after first disconnect. ESP-Hosted SDIO transport errors in logs.
- **Root Cause:** Calling vTaskDelay(1000) inside the WiFi disconnect event handler blocks the entire ESP-IDF system event loop for 1 second per retry. This starves ESP-Hosted SDIO transport, causing cascading connection failures.
- **Fix:** Removed all vTaskDelay from event handlers. Retry logic moved to a separate task or timer callback that does not block the event loop.
- **Prevention:** NEVER call vTaskDelay() or any blocking function inside ESP-IDF event handlers. Event handlers run on the system event task — blocking them starves all other event processing. Use xTimerStart() or xTaskNotifyGive() to defer slow work.

- **Date:** 2026-04-06
- **Symptom:** Dragon Q6A keeps going offline for 10+ minutes, unreachable via network.
- **Root Cause:** Dragon uses WiFi (wlan0 at 192.168.1.89), not ethernet. WiFi power save was enabled (`Power save: on`), causing periodic disconnects. Additionally, PingOS services (node/Fastify) competed for port 3500 with our dashboard.
- **Fix:** `iw wlan0 set power_save off` + NetworkManager dispatcher script at `/etc/NetworkManager/dispatcher.d/99-wifi-powersave.sh`. Disabled PingOS services (`systemctl disable pingos-dashboard pingos-gateway pingos-chromium pingos-xvfb`). Also disabled EEE on ethernet (`ethtool --set-eee enp1s0 eee off`) as belt-and-suspenders.
- **Prevention:** Always check `iw wlan0 get power_save` on ARM SBCs using WiFi. Set power save off immediately after any reboot. The dispatcher script should handle this automatically but verify after updates.

### SD Card and WiFi SDIO Coexistence
- **Date:** 2026-04-06
- **Symptom:** SD card and WiFi SDIO were documented as conflicting (can't use simultaneously).
- **Root Cause:** The comment in sdcard.c was incorrect. Tab5 uses SDMMC Slot 0 for SD card and Slot 1 for WiFi SDIO — different slots on different GPIO banks. They coexist fine.
- **Fix:** Updated sdcard.c comment to reflect verified coexistence. Confirmed: 122GB SD card mounts and operates normally while WiFi is active.
- **Prevention:** Don't trust hardware conflict comments without testing. The ESP32-P4 SDMMC controller supports 2 independent slots.

### Settings Screen WDT Crash (f_getfree on Large SD Card)
- **Date:** 2026-04-07
- **Symptom:** Opening the Settings screen triggered a watchdog timer (WDT) reset. Device rebooted every time Settings was accessed. Only occurred with the 128GB SD card inserted.
- **Root Cause:** `f_getfree()` on a 128GB FAT32 SD card blocks the calling thread for ~30 seconds while it scans the FAT. This was called inline during settings UI creation on the LVGL thread. The LVGL thread is monitored by the task WDT (5s default), so 30s of blocking triggered a WDT reset.
- **Fix:** Cache the `f_getfree()` result at boot (called once during SD init, before LVGL starts). Settings UI reads the cached value instead of calling `f_getfree()` directly. Additionally, feed `esp_task_wdt_reset()` between settings section creation to prevent WDT trips during the (still moderately slow) settings UI build.
- **Prevention:** Never call `f_getfree()` or any blocking filesystem operation from the LVGL thread. Cache slow filesystem queries at boot. Feed the WDT explicitly in any long-running UI creation sequence.

### Tool Parser Stray > After </args>
- **Date:** 2026-04-07
- **Symptom:** Tool calls from qwen3:1.7b were not parsed. The LLM generated valid tool XML but with a stray `>` character after the closing `</args>` tag.
- **Root Cause:** qwen3:1.7b (and other small quantized models) sometimes produce slightly malformed XML: `<tool>datetime</tool><args>{"query":"time"}</args>>` — note the extra `>` at the end. The strict regex parser required exact XML format and rejected these.
- **Fix:** Made the tool parser tolerant with a fallback regex pattern. Primary pattern matches strict XML. If it fails, fallback pattern strips trailing `>`, handles missing `</args>` closing tag, and extracts tool name + args JSON more loosely. Tested across 12 tool-calling scenarios.
- **Prevention:** When using small local LLMs for structured output (XML, JSON), always implement tolerant parsing with fallback patterns. Don't assume clean formatting from models under 4B parameters.

### Response Timeout Local vs Cloud Mode
- **Date:** 2026-04-07
- **Symptom:** Tab5 auto-cancelled voice requests during local-mode tool-calling chains. The LLM was still processing but Tab5 timed out and sent `cancel`.
- **Root Cause:** The 35-second response timeout was designed for cloud mode (fast inference). In local mode with qwen3:1.7b at 7.1 tok/s, a tool-calling chain (LLM generates tool call → tool executes → LLM re-queries with result) can take 2-4 minutes total. The 35s timeout fired mid-chain.
- **Fix:** Made the response timeout mode-aware: 5 minutes for local mode (voice_mode=0), 35 seconds for cloud mode (voice_mode=2). Hybrid mode (voice_mode=1) uses 35s since the LLM is still local but STT/TTS are cloud-fast.
- **Prevention:** Response timeouts must account for the slowest component in the pipeline. Local LLM inference with tool-calling chains is 10-50x slower than cloud. Always parameterize timeouts by mode.

### Dragon Q6A 15-Minute Reboot Cycle

- **Date:** 2026-04-07
- **Symptom:** Dragon reboots every ~15 minutes. Boot log shows exact 14-19 minute sessions. Previous boot journal empty (hard power cut, no graceful shutdown). Happens on both WiFi and ethernet.
- **Root Cause:** Power/thermal overload from bloated services. The Dragon was running GDM3 (gnome desktop, 175MB), nanobot/TinkerClaw agent (116MB, 56% CPU), snapd, ollama, rustdesk, fwupd, and chromium on every boot — totaling ~900MB RAM and heavy CPU. The QCS6490 SoC hit thermal limits after ~15 minutes under sustained load and hard-reset.
- **Fix:** Masked/disabled all non-essential services: gdm3, snapd, ollama, snap.rustdesk.rustdesk-server, tinkerclaw-nanobot, fwupd, systemd-sysupdate-reboot.timer. Created tinkerclaw-strip.service to enforce this on every boot. Memory dropped from 900MB to 558MB. Only tinkerclaw-voice, tinkerclaw-dashboard, tinkerclaw-ngrok, and dragon_server.py remain.
- **Prevention:** Never enable desktop environment (GDM) on a headless server. Monitor memory usage after adding services. The QCS6490 with 12GB RAM seems generous but the SoC power budget is tight — keep total RAM usage under 1GB and CPU under 50% sustained.

### Device Always Offline in Dragon Dashboard

- **Date:** 2026-04-07
- **Symptom:** Tab5 device shows `is_online=0` in Dragon DB despite being connected. Dashboard shows all devices offline. Dragon reports `active_connections: 0` even though Tab5's `/info` endpoint says `dragon_connected: true`.
- **Root Cause:** Voice WS only connected on-demand (when user tapped mic). Device registration only happens on voice WS connect. The `dragon_connected` field in Tab5's /info came from `dragon_link.c` (CDP health check on port 3501), not the voice WS (port 3502). So Tab5 said "connected" to Dragon's streaming server but was never registered on the voice server.
- **Fix:** Auto-connect voice WS at boot (`main.c` → `deferred_overlay_init_cb`). Uses `ui_voice_set_boot_connect(true)` to suppress overlay popup during boot. Device registers immediately, Dragon marks `is_online=1`. Session created on boot, persists across screen transitions.
- **Prevention:** "Connected" must mean registered on the voice server (port 3502), not just reachable via CDP health check. Always distinguish between the streaming connection (3501) and voice connection (3502).

### Single-Tap Voice Had Connect Delay

- **Date:** 2026-04-07
- **Symptom:** Tapping the mic orb from Home screen took 5-15 seconds before "Listening..." appeared. User had to wait for WS connect → register → session_start → READY → auto-start.
- **Root Cause:** Voice WS only connected on-demand. Each mic tap triggered: mode_switch(VOICE) → stop_streaming → settle(200ms) → TCP connect → WS upgrade → register → session_start (6s pipeline init) → READY → start_listening.
- **Fix:** Boot auto-connect means voice WS is READY before user ever taps. Mic tap from READY goes straight to `voice_start_listening()` — instant response. Mode-aware: if streaming is active, still calls `mode_switch(VOICE)` to stop MJPEG before recording.
- **Prevention:** Persistent connections eliminate connect-time latency. The voice WS is lightweight (~1KB/s keepalive) and doesn't conflict with other modes until actual recording starts.

### Camera SC202CS — Black Preview (Multiple Root Causes)

- **Date:** 2026-04-07
- **Symptom:** Camera screen showed black viewfinder. `cam` serial command returned "CSI transaction queue full" or "Capture timeout".
- **Root Cause:** FIVE stacked issues: (1) CSI queue_items=1 caused queue overflow on overlapping preview captures. (2) Lane count wrong: code said 2-lane but SC202CS is 1-lane. (3) Sensor init had only ~10 stub registers — needs 129 from Espressif's verified table. (4) MIPI bitrate 400 Mbps vs correct 576 Mbps. (5) Missing `esp_cam_ctlr_start()` call. Even after fixing all 5, frames still didn't arrive because the raw `esp_cam_ctlr_csi` API doesn't handle ISP pipeline setup (RAW→RGB conversion).
- **Fix:** Complete rewrite using M5Stack's `esp_video` + `esp_cam_sensor` V4L2 stack. Sensor auto-detected, ISP handles RAW8→RGB565, V4L2 MMAP double-buffering. Also required `CONFIG_CAMERA_SC202CS=y` in sdkconfig (sensor code guarded by Kconfig!). Exposure tuned via SCCB registers post-init.
- **Prevention:** Never use raw `esp_cam_ctlr_csi` for MIPI cameras — use `esp_video` V4L2 framework. Always check Kconfig guards in component CMakeLists.txt. SC202CS is 1-lane only, not 2-lane.

### LVGL Deadlock from HTTP Handler

- **Date:** 2026-04-07
- **Symptom:** Tab5 froze when calling `/navigate?screen=settings`. Debug server HTTP handler called `tab5_ui_lock()` → `ui_settings_create()` directly, but LVGL task was waiting for the HTTP response → deadlock.
- **Root Cause:** HTTP handler runs on the httpd task. `tab5_ui_lock()` acquires the LVGL mutex. But LVGL timer task may be blocked waiting for httpd to finish (if LVGL callback triggered an HTTP request). Classic mutex inversion deadlock.
- **Fix:** Use `lv_async_call()` to schedule UI operations on the LVGL timer thread instead of calling them directly from HTTP context. The HTTP handler returns immediately, the LVGL thread executes the navigation on its next tick.
- **Prevention:** NEVER call `tab5_ui_lock()` + LVGL create/load functions from HTTP handlers. Always use `lv_async_call()` for deferred execution.

### Chat Overlay Blocking Navigation After Close

- **Date:** 2026-04-07
- **Symptom:** After closing Chat overlay (F1: hide instead of delete), tapping nav bar had no effect. Tab5 appeared frozen on Chat page.
- **Root Cause:** Hidden overlay on `lv_layer_top()` still intercepted all touch events. `LV_OBJ_FLAG_CLICKABLE` was set even when hidden, so the invisible overlay ate every tap.
- **Fix:** Clear `LV_OBJ_FLAG_CLICKABLE` when hiding, re-add when showing. Hidden overlay becomes transparent to touch events.
- **Prevention:** When hiding LVGL overlays (instead of deleting), always clear CLICKABLE flag. Hidden ≠ non-interactive in LVGL.

### Three-Tier Mode — NVS vs ACK Race Condition

- **Date:** 2026-04-07
- **Symptom:** After switching to Full Cloud (mode=2), NVS sometimes showed mode=1 (hybrid). The `/mode` debug endpoint saved mode=2 locally but the Dragon ACK arrived with a different mode value.
- **Root Cause:** The `/mode` endpoint uses `voice_send_cloud_mode(bool)` as a bridge — both hybrid (1) and cloud (2) send `cloud_mode=true`. Dragon's backward compat maps `cloud_mode=true` to voice_mode=2, but the ACK timing vs NVS write created a race.
- **Fix:** Document the limitation. For proper three-tier, Tab5 should send `voice_mode` integer directly (not boolean bridge). The current implementation works for Local↔Hybrid switching; Full Cloud model selection needs the integer protocol.
- **Prevention:** Don't use boolean bridges for multi-valued enums. Send the actual value.

### camera.c Format String Errors (V4L2 __u32 Types)

- **Date:** 2026-04-07
- **Symptom:** Compiler warnings: format specifier `%d` / `%x` expects `int`, but argument has type `__u32` (unsigned 32-bit). On some platforms this could produce garbled log output or undefined behavior if `__u32` has a different size than `int`.
- **Root Cause:** V4L2 structures (`struct v4l2_format`, `struct v4l2_capability`, etc.) use `__u32` for pixel format, width, height, and capability fields. These were printed with `%d` and `%x` format specifiers, which expect `int` and `unsigned int` respectively. On ESP32-P4 (ILP32), `__u32` is `unsigned long` (4 bytes) — same size as `int` but different type, triggering warnings and potential UB under strict aliasing.
- **Fix:** Cast `__u32` values to `(unsigned long)` and use `%lu` / `%lx` format specifiers in all `ESP_LOGI` / `ESP_LOGE` calls in camera.c.
- **Prevention:** Always cast V4L2 `__u32` fields before passing to printf-family functions. Use `(unsigned long)` + `%lu` / `%lx` as the standard pattern. Enable `-Wformat` warnings and treat them as errors.

### Three-Tier Boolean Bridge Replaced with Full config_update

- **Date:** 2026-04-07
- **Symptom:** ui_settings.c used `voice_send_cloud_mode(bool)` to switch voice modes. This only sent `cloud_mode=true/false` — a boolean that collapsed three modes (Local/Hybrid/Cloud) into two states. Dragon had to guess whether `cloud_mode=true` meant Hybrid or Full Cloud. Model selection was not transmitted at all.
- **Root Cause:** The original cloud mode was a simple on/off toggle. When three-tier mode was added (Local=0, Hybrid=1, Full Cloud=2), the settings UI still used the boolean bridge function instead of sending the integer `voice_mode` + string `llm_model` directly.
- **Fix:** Created `voice_send_config_update(int voice_mode, const char *llm_model)` which sends the full `{"type":"config_update","voice_mode":0|1|2,"llm_model":"..."}` message. Updated ui_settings.c to call this new function with the actual integer mode and selected model string. Dragon receives the exact mode and model — no guessing.
- **Prevention:** When extending a protocol from binary (on/off) to multi-valued, replace the old boolean API entirely. Don't add a new integer on top of the old boolean — it creates ambiguity. The protocol message should carry the exact value the server needs.

---

## UI/UX Redesign (2026-04-12)

### LVGL Object Count Limit
- **Date:** 2026-04-12
- **Symptom:** Memory exhaustion crash when screens had too many LVGL objects.
- **Root Cause:** On ESP32-P4 with 128KB+64KB LVGL memory pool and DPI PSRAM framebuffer, maximum ~55 LVGL objects per screen before memory exhaustion crashes. Decorative objects (divider lines, subtitle labels, accent lines) consume object slots that push past this limit.
- **Fix:** Eliminated all decorative objects — use spacing and color for visual hierarchy instead of divider lines, subtitle labels, and accent lines.
- **Prevention:** Keep LVGL object count per screen under 55. Never add decorative-only objects. Use padding, margins, and color contrast for hierarchy. Count objects with `lv_obj_get_child_count()` during development.

### Settings as Overlay Not Screen
- **Date:** 2026-04-12
- **Symptom:** `lv_screen_load()` crashes when transitioning between screens with many objects (draw buffer exhaustion).
- **Root Cause:** LVGL screen transitions require draw buffers for both screens simultaneously, exceeding the 128KB+64KB memory pool on the DPI 720x1280 display.
- **Fix:** Settings is a fullscreen overlay (child of home screen) with hide/show, not a separate `lv_screen`. No screen transitions needed.
- **Prevention:** Use overlays (children of the existing screen) instead of separate screens for complex UIs. Reserve `lv_screen_load()` for screens with low object counts.

### Notes Uses Separate Screen
- **Date:** 2026-04-12
- **Symptom:** N/A (architecture note)
- **Root Cause:** Unlike Chat/Settings which are overlays on the home screen, Notes uses `lv_screen_load()` because it was built as a standalone 1812-line module. This works because Notes has fewer total objects than Settings/Chat.
- **Fix:** No change — converting Notes to an overlay would require a major refactor of the entire 1812-line module.
- **Prevention:** New screens should default to overlay architecture. Only use `lv_screen_load()` if the screen is simple (low object count) and self-contained.

### Material Dark Design Language
- **Date:** 2026-04-12
- **Symptom:** N/A (design decision)
- **Root Cause:** Needed a consistent design language across all screens that looks polished on the 720x1280 DPI display.
- **Fix:** Adopted Material Dark across all screens: background #0A0A0F, cards #1A1A2E with #333 borders, colored uppercase section headers, amber accent (#F5A623), pill-shaped tabs, 12px radius on cards, no divider objects.
- **Prevention:** All new UI elements must follow this palette. No divider line objects — use spacing. No shadow objects (P4 shadow rendering crashes). Cards use 12px radius + #333 border.

### Tileview Height Must Exclude Nav Bar
- **Date:** 2026-04-12
- **Symptom:** Tileview covered the nav bar, making bottom navigation inaccessible.
- **Root Cause:** Tileview was created at 720x1280 (full screen) but the nav bar (120px) and page dots (36px) sit at the bottom. The tileview overlapped them.
- **Fix:** Set tileview height to 720x1124 (1280 - 120 nav - 36 dots). Pages that use separate screens (Notes, Camera, Files) can use the full 1280 height since they have their own back navigation.

---

## Critical LVGL + DPI Rendering Lessons (2026-04-13)

### ⚠️ CONFIG_LV_CONF_SKIP=1 — lv_conf.h IS COMPLETELY IGNORED
- **Date:** 2026-04-13
- **Symptom:** Every change made to `main/lv_conf.h` (circle cache, assert handler, custom allocator, memory pool size) had ZERO effect. Builds compiled fine but the runtime behavior didn't change. Spent hours debugging why "fixes" didn't work.
- **Root Cause:** ESP-IDF's LVGL component sets `CONFIG_LV_CONF_SKIP=1` via Kconfig. This makes `lv_conf_internal.h` skip `#include "lv_conf.h"` entirely. ALL LVGL configuration comes from Kconfig (`sdkconfig`/`sdkconfig.defaults`), NOT from `lv_conf.h`. The `lv_conf.h` file in `main/` is a dead file — it compiles into `main` component but LVGL never reads it.
- **Fix:** Put ALL LVGL configuration in `sdkconfig.defaults` using `CONFIG_LV_*` Kconfig keys. Do NOT edit `lv_conf.h` — it has no effect.
- **Prevention:** **BEFORE making any LVGL config change, verify it appears in `build/config/sdkconfig.h` after building.** If `grep "YOUR_SETTING" build/config/sdkconfig.h` returns nothing, the setting is NOT active. Setting `CONFIG_LV_CONF_SKIP=n` breaks the build (LVGL component can't find `lv_conf.h` from `main` component without adding REQUIRES dependency).

### LVGL Memory Pool Exhaustion — THE Root Cause of Note Tap Crashes
- **Date:** 2026-04-13
- **Symptom:** Tapping a note card to open the edit overlay crashes the device. Two crash modes: (1) `task_wdt` timeout — ui_task stuck in `while(1)` inside `LV_ASSERT_MALLOC` at `lv_label_set_text:166` or `draw_letter:427`. (2) `Store access fault` — NULL pointer propagation after failed `lv_malloc`.
- **Root Cause:** LVGL's builtin allocator had a **64KB fixed pool** (`CONFIG_LV_MEM_SIZE_KILOBYTES=64`) with **0KB expand** (`CONFIG_LV_MEM_POOL_EXPAND_SIZE_KILOBYTES=0`). Notes screen with 7 cards + topbar + search + buttons consumed ~50KB. Opening the edit overlay (textarea + labels + buttons) needed ~15KB more → pool exhausted → `lv_malloc` returns NULL → `LV_ASSERT_MALLOC` fires `while(1)` → WDT kills device after 60s.
- **Fix:** `CONFIG_LV_MEM_SIZE_KILOBYTES=96` + `CONFIG_LV_MEM_POOL_EXPAND_SIZE_KILOBYTES=1024` = 1120KB total. The 96KB base pool fits in internal SRAM BSS. The 1024KB expand auto-allocates from system heap (PSRAM) when base pool is full.
- **Prevention:** Monitor LVGL memory usage: `lv_mem_monitor_t mon; lv_mem_monitor(&mon); ESP_LOGI("LVGL", "used=%d%% frag=%d%%", mon.used_pct, mon.frag_pct);`. If `used_pct > 80%`, increase pool or simplify UI. NEVER go above 96KB base pool — 128KB causes linker error on ESP32-P4 (exceeds internal SRAM BSS capacity). Use expand pool for overflow.
- **Why it worked before:** The edit overlay feature was added but never tested with physical taps on the device. API navigation created the Notes list (which fits in 64KB) but never triggered `cb_note_tap` which creates the edit overlay.

### LVGL Circle Cache — Set via Kconfig, NOT lv_conf.h
- **Date:** 2026-04-13
- **Symptom:** Device reboots with `Store access fault` in `circ_calc_aa4` → `lv_draw_sw_mask_radius_init` when too many rounded-corner objects are on screen.
- **Root Cause:** `CONFIG_LV_DRAW_SW_CIRCLE_CACHE_SIZE` defaults to **4**. Notes (7 cards) + dialogs + overlays need 20+ simultaneous radius masks. Overflow triggers `lv_malloc_zeroed` which fails (64KB pool exhausted) → writes to NULL → crash.
- **Fix:** `CONFIG_LV_DRAW_SW_CIRCLE_CACHE_SIZE=32` in `sdkconfig.defaults`. Costs ~4KB but handles any realistic UI combination.
- **Prevention:** Count max simultaneous rounded objects across all visible screens + overlays. Cache must be >= that count. Current peak: ~20 (Notes list + edit overlay + keyboard).

### Notes WDT Crash During Card Creation
- **Date:** 2026-04-13
- **Symptom:** Device reboots with `Task watchdog got triggered` on `ui_task` when opening Notes with 7+ notes. No panic/fault, just WDT timeout.
- **Root Cause:** Notes `ui_notes_create()` creates ~50 LVGL objects synchronously (topbar + 2 buttons + search + divider + scrollable list + 7 note cards with children). On ESP32-P4, creating and rendering all these objects takes long enough to exceed the 60s WDT timeout. The UI task holds the LVGL mutex the entire time, preventing the WDT from being fed.
- **Fix:** Added `esp_task_wdt_reset()` calls between major UI sections (after topbar, after buttons, every 3 note cards, before/after refresh_list).
- **Prevention:** Any UI creation that builds >20 LVGL objects must feed the WDT periodically. Settings already does this. Pattern: `static inline void feed_wdt(void) { esp_task_wdt_reset(); }` then call every ~10 objects or between sections.

### NEVER Change Notes Overlay Creation Pattern
- **Date:** 2026-04-13
- **Symptom:** Notes screen showed only the topbar (title, back button, Voice/Type buttons) but the main content area was transparent — home screen content (orb, Ask Tinker, Camera/Files buttons) bled through below the topbar.
- **Root Cause:** The original Notes creation code uses this EXACT pattern that works:
  ```c
  s_screen = lv_obj_create(ui_home_get_screen());
  lv_obj_remove_style_all(s_screen);      // ← CRITICAL: must keep this
  lv_obj_set_size(s_screen, SW, SH);
  lv_obj_set_pos(s_screen, 0, 0);
  lv_obj_set_style_bg_color(s_screen, ...);
  lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
  lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_move_foreground(s_screen);
  ```
  Multiple changes were attempted that ALL broke rendering:
  1. **Removing `lv_obj_remove_style_all()`** → Background didn't cover full screen
  2. **Changing parent to `lv_layer_top()`** → DPI framebuffer doesn't render layers correctly
  3. **Changing to separate `lv_screen` with `lv_screen_load()`** → Background didn't repaint over DPI framebuffer
  4. **Using `LV_DISPLAY_RENDER_MODE_DIRECT`** → Severe horizontal tearing artifacts
  5. **Adding `lv_obj_invalidate()` or `lv_refr_now()`** → Either no effect or caused internal heap exhaustion
  6. **Hiding the tileview** → Home content still showed through from status bar, nav bar, page dots
  
  The original code with `lv_obj_remove_style_all()` + `PARTIAL` render mode + overlay on `ui_home_get_screen()` is the ONLY combination that renders correctly on ESP32-P4 DPI.
- **Fix:** Reverted ALL changes back to the known-good pattern (commit ae7d4ad).
- **Prevention:** **DO NOT modify the Notes overlay creation code.** If you need to change how Notes renders, test EVERY change by navigating to Notes and taking a screenshot. The DPI framebuffer on ESP32-P4 has subtle rendering behavior where LVGL partial updates don't cover the full screen background unless the exact combination of `remove_style_all` + overlay-on-screen + `move_foreground` is used.

### PARTIAL Render Mode Is Required for DPI Display
- **Date:** 2026-04-13
- **Symptom:** Switching to `LV_DISPLAY_RENDER_MODE_DIRECT` (using DPI framebuffer directly as LVGL draw buffer) caused severe horizontal line tearing on all screens.
- **Root Cause:** DPI peripheral reads the framebuffer via DMA continuously. In DIRECT mode, LVGL writes directly to the same memory that DPI is reading → tearing when write and read happen on same scanline. PARTIAL mode uses separate draw buffers and copies finished renders to the framebuffer, which minimizes the DMA read/write conflict window.
- **Fix:** Keep `LV_DISPLAY_RENDER_MODE_PARTIAL` with two 144KB draw buffers in PSRAM.
- **Prevention:** NEVER switch to DIRECT or FULL render mode on ESP32-P4 DPI. The M5Stack Tab5 hardware requires PARTIAL mode with the copy-to-framebuffer flush callback. If you need tear-free rendering, the correct approach is double-buffered DPI with vsync (not implemented yet).

### lv_refr_now() Causes Internal Heap Exhaustion
- **Date:** 2026-04-13
- **Symptom:** After adding `lv_refr_now(NULL)` to force a synchronous screen refresh in `dismiss_all_overlays()`, the voice task failed to create with "task create failed" error.
- **Root Cause:** `lv_refr_now()` forces a synchronous full-screen re-render. On 720x1280 DPI, this temporarily allocates large draw buffers from internal SRAM (not PSRAM) for the render pipeline. This exhausts the ~292KB internal heap, leaving insufficient memory for FreeRTOS task stack allocation (voice task needs 8KB internal RAM).
- **Fix:** Removed `lv_refr_now()` — never force synchronous full-screen refresh.
- **Prevention:** NEVER call `lv_refr_now()` on ESP32-P4 with DPI display. Let LVGL handle refresh timing naturally through `lv_timer_handler()`. If you need to ensure a redraw, use `lv_obj_invalidate()` and let the next timer cycle handle it.

### ui_chat_destroy() in Navigation Paths Causes Crashes
- **Date:** 2026-04-13
- **Symptom:** Navigating from Chat → Notes crashed with Store access fault in `lv_draw_sw_fill` or `lv_draw_sw_mask_radius_init`.
- **Root Cause:** `dismiss_all_overlays()` called `ui_chat_destroy()` which calls `lv_obj_del(s_overlay)`, freeing the Chat overlay's LVGL objects synchronously. When Notes then creates its 7 rounded cards, LVGL's draw pipeline encounters stale references to the just-freed Chat objects.
- **Fix:** The original code uses `ui_chat_destroy()` in `dismiss_all_overlays()` and it works with the circle cache fix (LV_DRAW_SW_CIRCLE_CACHE_SIZE=16). The crash was actually the circle cache exhaustion, not the destroy. With sufficient cache, destroy is safe because LVGL processes the object tree cleanup before the next render.
- **Prevention:** The circle cache fix is the real solution. If crashes return in `lv_draw_sw_mask_radius_init`, increase `LV_DRAW_SW_CIRCLE_CACHE_SIZE` further. Don't change `dismiss_all_overlays()` to use hide-instead-of-destroy unless you verify Notes still renders correctly afterward.

### Nav Debounce Prevents Animation Race Conditions
- **Date:** 2026-04-13
- **Symptom:** Rapidly tapping nav bar buttons caused LVGL animation callbacks to fire on deleted objects, or multiple overlays to be created simultaneously.
- **Root Cause:** Each nav tap calls `dismiss_all_overlays()` + `lv_async_call(create_screen)`. If tapped faster than the 150ms fade-out animation, the old animation callbacks reference objects that were already deleted by the next tap's dismiss.
- **Fix:** 300ms debounce on `nav_click_cb()`:
  ```c
  static uint32_t s_last_nav_ms = 0;
  uint32_t now = lv_tick_get();
  if (now - s_last_nav_ms < 300) return;
  s_last_nav_ms = now;
  ```
- **Prevention:** All navigation entry points (nav bar taps, debug server /navigate, screen callbacks) should debounce. 300ms is the right threshold — long enough to prevent double-taps, short enough to feel responsive.

### Voice Overlay Must Hide Instantly (No Fade Animation)
- **Date:** 2026-04-13
- **Symptom:** Voice overlay fade-out animation (150ms) raced with navigation dismiss, causing `fade_overlay_cb` and `fade_done_hide_cb` to access the overlay after it was hidden/deleted. Also: dangling `s_auto_hide` timer caused use-after-free crash on next voice session (see dedicated entry above).
- **Root Cause:** `ui_voice_hide()` started a 150ms fade-out animation. If navigation happened during the fade window, callbacks fired on stale state. Combined with a dangling timer pointer from local-static `s_auto_hide` with `auto_delete=true`.
- **Fix:** `ui_voice_hide()` now hides instantly (no animation): cancel any in-flight fade via `lv_anim_delete()`, set HIDDEN flag, clear CLICKABLE, reset opacity. Removed `fade_done_hide_cb` entirely. Moved `s_auto_hide` to file scope, cleaned up in `stop_all_anims()`.
- **Prevention:** Overlays that can be dismissed externally (by navigation) should NEVER use async animations for hide. Show animations (fade-in) are fine. Hide must be synchronous and immediate. NEVER use `lv_timer_set_auto_delete(true)` when storing the timer pointer — it creates dangling pointers.

---

## Touch Feedback System (2026-04-13)

### Pressed State Feedback Required on ALL Interactive Elements
- **Date:** 2026-04-13
- **Symptom:** User reported "I don't know where I'm touching" — tapping buttons, cards, nav items produced no visual response. Only the keyboard keys and voice mic button had pressed-state styles.
- **Root Cause:** 30+ interactive elements across Home, Chat, Notes, Settings, Camera screens had no `LV_STATE_PRESSED` style defined. LVGL's default theme provides minimal pressed feedback (a subtle gray overlay) that's invisible on the dark OLED display.
- **Fix:** Created `ui_feedback.h/c` module with shared static styles (one per feedback type, ~200 bytes total):
  - `ui_fb_button(obj)` — darken to 80% opacity, 100ms ease-out transition
  - `ui_fb_button_colored(obj, hex)` — explicit pressed color
  - `ui_fb_card(obj)` — lighten border + bg shift to #252540
  - `ui_fb_icon(obj)` — dim to 60% opacity
  - `ui_fb_nav(obj)` — brighten text to white
  Applied to all buttons, cards, nav items across all screens.
- **Prevention:** Every new `lv_obj_add_event_cb(..., LV_EVENT_CLICKED, ...)` MUST be followed by a `ui_fb_*()` call. Add it as part of the button creation pattern. No interactive element should exist without pressed feedback.

### No Transition Animations Defined Anywhere
- **Date:** 2026-04-13
- **Symptom:** Pressed states (where they existed) snapped instantly with no smooth animation.
- **Root Cause:** Zero `lv_style_transition_dsc_t` instances in the entire codebase before ui_feedback.c was added. LVGL requires explicit transition descriptors for smooth state changes.
- **Fix:** ui_feedback.c defines one shared transition: 100ms ease-out for `bg_color`, `bg_opa`, `border_color`. Applied via shared static styles.
- **Prevention:** All new pressed-state styles should use `ui_fb_*()` functions which include the transition. Don't set `LV_STATE_PRESSED` styles inline without also setting a transition.

---

## Dragon Server Lessons (2026-04-13)

### Mode-Aware System Prompts and Token Limits
- **Date:** 2026-04-13
- **Symptom:** Local mode (qwen3:1.7b at 7 tok/s) used the same system prompt and max_tokens as cloud mode (Claude 3.5 Haiku). Small local model was being asked to give detailed responses it couldn't deliver, wasting tokens and time.
- **Root Cause:** Single system prompt "Reply in 1-2 sentences maximum" applied to all modes. No token limit differentiation.
- **Fix:** Three mode-specific prompts in `config.py`: Local (concise, 128 tokens), Hybrid (medium, 256 tokens), Cloud (rich, 512 tokens). Applied during `config_update` handler in `server.py`. Session's system_prompt updated in DB so conversation engine uses it.
- **Prevention:** Any new voice mode or model tier should define its own prompt+token config. The prompt must match the model's capability — small models need tight constraints, large models can be given more freedom.

### Pipeline Init Should Default to Local Backends
- **Date:** 2026-04-13
- **Symptom:** On Tab5 reconnect, Dragon initialized the pipeline with whatever backends were set from the PREVIOUS connection (e.g., OpenRouter STT+TTS+LLM for mode 2). Tab5 then immediately sent `config_update voice_mode=1` which swapped the LLM back to Ollama — wasting time initializing cloud backends that get immediately replaced.
- **Root Cause:** `self._config` retained the previous connection's backend settings. New pipeline used those stale settings.
- **Fix:** Reset `self._config` to local defaults (moonshine/piper/ollama) before pipeline init in `_handle_register()`.
- **Prevention:** Pipeline init should always use local defaults. Tab5 sends its actual mode in `config_update` immediately after registration.

### SearXNG vs DuckDuckGo Web Search
- **Date:** 2026-04-13
- **Symptom:** DuckDuckGo search returned 3 results max with no engine diversity. Wanted better search coverage.
- **Root Cause:** DuckDuckGo client library (`ddgs`) is single-engine with low result count.
- **Fix:** Installed SearXNG from git on Dragon (not pip — the pip `searxng` package is an MCP server, NOT the search engine). SearXNG aggregates Google+Bing+DDG → 44 results per query. Runs on port 8888 as systemd service. `web_search.py` tries SearXNG first, falls back to DDG.
- **Prevention:** The pip package `searxng` (PyPI) is NOT the SearXNG search engine. Install from https://github.com/searxng/searxng.git with `pip install -e .` from the cloned repo. Requires `msgspec` installed first.

### Compact Tool Format for Small Local Models
- **Date:** 2026-04-13
- **Symptom:** qwen3:1.7b struggled with the full tool description format — too many tokens in the system prompt consumed context window and confused the model.
- **Root Cause:** Full tool format listed all 10 tools with detailed descriptions, multiple examples, and verbose instructions. This used ~500 tokens of the model's 2048 context window.
- **Fix:** `registry.py` now has `format_for_llm(compact=True)` for local models: shows only 5 priority tools (web_search, datetime, remember, recall, calculator) with minimal one-line descriptions and single examples. ~150 tokens.
- **Prevention:** When adding new tools, also add them to the priority list in `_format_compact()` if they're commonly used. Keep compact format under 200 tokens total.

### SCP Overwrites Live Config — Always Restore API Keys After Deploy
- **Date:** 2026-04-13
- **Symptom:** After `scp -r dragon_voice/ radxa@dragon:/home/radxa/`, Dragon voice service started with empty OpenRouter API key and empty SearXNG URL. Cloud mode rejected with "No API key configured".
- **Root Cause:** `scp -r` copies the REPO version of `config.yaml` which has empty/placeholder values. The LIVE `config.yaml` has the real API key and SearXNG URL.
- **Fix:** After every deploy, restore the API key and SearXNG URL on the live config:
  ```bash
  sed -i 's|openrouter_api_key: ""|openrouter_api_key: "sk-or-v1-..."|' /home/radxa/dragon_voice/config.yaml
  sed -i 's|searxng_url: ""|searxng_url: "http://localhost:8888"|' /home/radxa/dragon_voice/config.yaml
  ```
- **Prevention:** Use environment variables for secrets instead of config.yaml. Or create a `config.local.yaml` override that's in `.gitignore` and loaded after the base config. NEVER commit API keys to the repo.

### Clear __pycache__ After Deploy
- **Date:** 2026-04-13
- **Symptom:** After deploying new Python files to Dragon, the old `.pyc` bytecode was still used. New `WebSearchTool.__init__(searxng_url)` parameter caused "takes no arguments" error because the cached `.pyc` had the old class without `__init__`.
- **Root Cause:** Python caches compiled bytecode in `__pycache__/` directories. `scp` copies new `.py` files but the existing `.pyc` files have newer timestamps, so Python uses the stale cached version.
- **Fix:** `find /home/radxa/dragon_voice -name '__pycache__' -type d -exec rm -rf {} +` after every deploy, before restarting the service.
- **Prevention:** Add pycache cleanup to the deploy script. Or add `PYTHONDONTWRITEBYTECODE=1` to the systemd service environment.
- **Prevention:** Any scrollable container on the home screen must account for the nav bar (120px) and page dots (36px). Full-height = 1124px, not 1280px. Separate screens (loaded via `lv_screen_load()`) can use full 1280px.

### CONFIG_LV_USE_ASSERT_MALLOC=n Prevents while(1) WDT Hang
- **Date:** 2026-04-13
- **Symptom:** When LVGL `lv_malloc` returned NULL (pool exhausted), the device hung for 60 seconds then rebooted via WDT. No useful crash backtrace — just `task_wdt` on `ui_task`.
- **Root Cause:** `LV_ASSERT_MALLOC` default implementation is `while(1){}` — an infinite loop that blocks the LVGL task until the watchdog kills it. This produces no backtrace, no fault address, no useful debug info.
- **Fix:** Set `CONFIG_LV_USE_ASSERT_MALLOC=n` and `CONFIG_LV_USE_ASSERT_NULL=n` in `sdkconfig.defaults`. With asserts disabled, NULL from `lv_malloc` propagates to the caller, which dereferences it and produces a `Store access fault` with a full backtrace pointing to the exact line.
- **Prevention:** Always disable LVGL assert macros on production firmware. A crash with a backtrace is infinitely more useful than a silent `while(1)` hang. Fix the OOM root cause (increase pool, reduce objects) rather than relying on asserts.

### Voice Overlay Dangling Timer Pointer — Use-After-Free Crash
- **Date:** 2026-04-13
- **Symptom:** Device crashes when closing voice overlay and navigating to another screen within 0.3s. Cascade failure: once crashed, all subsequent tests fail (device rebooting).
- **Root Cause:** THREE compounding bugs in `ui_voice.c`:
  1. **Dangling `s_auto_hide` timer (UAF):** `s_auto_hide` was a `static` local variable inside the READY state handler, created with `lv_timer_set_auto_delete(true)` and `repeat_count=1`. After firing, LVGL auto-freed the timer struct, but `s_auto_hide` still held the old pointer. Next READY entry: `lv_timer_delete(dangling_pointer)` → use-after-free → heap corruption → crash.
  2. **Fade animation race:** `ui_voice_hide()` started a 150ms fade-out animation with `fade_done_hide_cb`. If navigation changed screen state during the 150ms window, the callback accessed stale LVGL objects.
  3. **Event callback stacking:** `show_state_speaking()` called `lv_obj_add_event_cb(orb, orb_speak_click_cb)` without removing first. Re-entering SPEAKING state stacked duplicate handlers → double-fire on tap.
- **Fix:**
  1. Moved `s_auto_hide` to file scope, cleaned up in `stop_all_anims()`, removed `auto_delete=true`.
  2. Made `ui_voice_hide()` instant: no animation, just HIDDEN flag + clear CLICKABLE + reset opacity.
  3. Added `lv_obj_remove_event_cb()` before `lv_obj_add_event_cb()` for orb click handlers.
- **Prevention:** NEVER use `lv_timer_set_auto_delete(true)` with a stored pointer — LVGL frees the memory but your pointer becomes dangling. NEVER use local static for timer pointers — use file-scope statics cleaned up in a central teardown function. NEVER add LVGL event callbacks without removing first to prevent stacking. Overlays dismissed by external navigation must hide instantly (no async animations).

### Chat Hide-Not-Destroy in dismiss_all_overlays Prevents Timer Linked List Corruption
- **Date:** 2026-04-13
- **Symptom:** Navigating away from Chat (e.g., Chat -> Notes) caused sporadic crashes in `lv_timer_handler` or `lv_draw_sw_fill` on the next LVGL tick.
- **Root Cause:** `ui_chat_destroy()` deletes the Chat overlay and all its children synchronously. If any child had an active `lv_timer` (e.g., status bar blink timer, message animation), the timer's linked list node was freed but the timer handler still iterated through it on the next tick — classic use-after-free on a linked list.
- **Fix:** Changed `dismiss_all_overlays()` to call `ui_chat_hide()` (sets HIDDEN flag, clears CLICKABLE) instead of `ui_chat_destroy()`. The Chat overlay stays allocated but invisible and non-interactive. Its timers remain valid. Destroy only happens when Chat is explicitly replaced (e.g., New Chat button).
- **Prevention:** Overlays with active timers or animations should be hidden, not destroyed, during navigation. Only destroy when the overlay is being permanently replaced. If you must destroy, cancel all timers first with `lv_timer_del()`.

---

## ngrok WebSocket Stability (2026-04-14)

### WS Connection Drops Every 30s Through ngrok Tunnel
- **Date:** 2026-04-14
- **Symptom:** WebSocket connection drops every 30s through ngrok tunnel. Tab5 reconnects but the cycle repeats indefinitely.
- **Root Cause:** Three compounding issues: (1) `vTaskSuspend` leaked tasks on reconnect — suspended tasks accumulated and were never cleaned up. (2) Protocol-level `ws.ping()` frames were not counted as ngrok activity — ngrok's idle timeout only monitors data frames, not WebSocket control frames. (3) aiohttp `heartbeat` parameter was too aggressive for ngrok latency, causing premature timeout detection.
- **Fix:** (1) `vTaskDelete` is now safe with TLSP callbacks disabled (`CONFIG_FREERTOS_TLSP_DELETION_CALLBACKS=n`). (2) Dragon sends JSON `{"type":"pong"}` data frames every 15s instead of relying on protocol-level pings. (3) Set `heartbeat=None` in aiohttp WebSocket handler to disable aiohttp's own ping/pong mechanism.
- **Prevention:** Always send DATA frames for keepalive through proxies/tunnels — never rely on WebSocket protocol pings alone. Proxies like ngrok, Cloudflare Tunnel, and nginx may not forward or count control frames as activity.

### Connection Mode Toggle (Auto/Local/Remote)
- **Date:** 2026-04-14
- **Symptom:** N/A (feature addition)
- **Root Cause:** Users needed control over whether Tab5 connects locally or through ngrok, with an automatic fallback option.
- **Fix:** Added Connection Mode setting with three options: Auto (local first, ngrok fallback), Local only, Remote only. Stored in NVS key `conn_m`. Exposed in Settings dropdown under the Network section.
- **Prevention:** When adding new NVS settings, always add a corresponding UI control in the Settings screen and document the NVS key name and valid values.

---

## Stability Sprint Fixes (2026-04-15)

### Overlay Height 1280 Must NOT Be Reduced (lv_color_mix32 Crash)
- **Date:** 2026-04-15
- **Symptom:** Reducing overlay height below 1280px (e.g., to 1160px to "avoid" the nav bar area) caused a crash in `lv_color_mix32` during LVGL rendering.
- **Root Cause:** The LVGL partial render mode calculates draw areas based on the object's full bounding box. When an overlay is shorter than the display, the draw buffer math can produce out-of-bounds writes in `lv_color_mix32` during alpha blending at the boundary. The 720x1280 display expects full-height overlays.
- **Fix:** Keep all overlays at full 1280px height. The nav bar on `lv_layer_top` renders above them. Never try to make overlays shorter to "leave room" for the nav bar.
- **Prevention:** Overlays on the 720x1280 display must always be exactly 1280px tall. Use `lv_layer_top` for elements that need to float above overlays (like the nav bar).

### Settings Must Use Hide/Show Not Create/Destroy (Fragmentation)
- **Date:** 2026-04-15
- **Symptom:** After opening/closing Settings 10-15 times, device rebooted. Internal SRAM largest free block dropped below 20KB while total free was still 80KB+.
- **Root Cause:** Each Settings create/destroy cycle allocated and freed ~55 LVGL objects from internal SRAM. The freed memory became fragmented — many small free chunks but no contiguous block large enough for the next Settings creation.
- **Fix:** Settings overlay is created once and hidden/shown via `LV_OBJ_FLAG_HIDDEN`. Values are refreshed on show. Destroy only when the overlay is permanently replaced.
- **Prevention:** Any LVGL overlay with many objects (>20) must use hide/show, not create/destroy. Monitor `heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)` during development. If it drops significantly after open/close cycles, you have a fragmentation problem.

### Nav Bar Must Be on lv_layer_top Not Inside Tileview
- **Date:** 2026-04-15
- **Symptom:** Nav bar was hidden when fullscreen overlays (Settings, Chat, Voice) were shown. Users could not navigate away from an overlay without a back button or swipe gesture.
- **Root Cause:** The nav bar was a child of the home screen tileview. Fullscreen overlays covered the entire tileview including the nav bar.
- **Fix:** Moved nav bar to `lv_layer_top()` which renders above all screens and overlays. Nav bar is always visible and tappable regardless of what screen or overlay is active.
- **Prevention:** Any UI element that must remain accessible across all screens belongs on `lv_layer_top()`, not inside a specific screen's object tree. The nav bar, status bar, and system notifications are candidates for `lv_layer_top`.

### Done Key Should Dispatch LV_EVENT_READY Not Insert Newline
- **Date:** 2026-04-15
- **Symptom:** Pressing Done/Enter on the keyboard inserted a literal newline character into single-line text fields (WiFi password, Dragon host, etc.) instead of submitting the form.
- **Root Cause:** The keyboard's Done key was configured to send `LV_KEY_ENTER` which LVGL's textarea interprets as "insert newline" for multi-line fields. Single-line fields needed a different behavior.
- **Fix:** Done key now dispatches `LV_EVENT_READY` on the focused textarea. Event handlers on each text field catch `LV_EVENT_READY` to close the keyboard and process the input (save to NVS, connect WiFi, etc.).
- **Prevention:** For single-line input fields (settings, passwords, hostnames), always register an `LV_EVENT_READY` handler. Use `LV_KEY_ENTER` only for multi-line text areas where newlines are expected.

### Internal SRAM Fragmentation: Free != Usable (Largest Block Matters)
- **Date:** 2026-04-15
- **Symptom:** `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)` reported 90KB free, but a 32KB allocation failed with NULL. Device appeared to have plenty of memory but could not allocate.
- **Root Cause:** Heap fragmentation from repeated create/destroy cycles. The 90KB of free memory was split across dozens of non-contiguous small blocks (largest was 18KB). No single block was large enough for the 32KB allocation.
- **Fix:** (1) Switched overlays to hide/show pattern to stop fragmenting. (2) Added fragmentation watchdog that monitors `heap_caps_get_largest_free_block()` and reboots if it stays below 30KB for 3 minutes. (3) Added `/selftest` endpoint that reports both free size and largest block for remote monitoring.
- **Prevention:** Always check `heap_caps_get_largest_free_block()`, not just `heap_caps_get_free_size()`. The largest block is the true measure of allocation capacity. Log both values in health checks.

---

## Rich Media Chat (2026-04-15)

### TJPGD Requires LV_USE_FS_MEMFS for In-Memory JPEG Decode
- **Date:** 2026-04-15
- **Symptom:** Image bubbles appeared in chat but were empty — no image content rendered. TJPGD returned no errors but produced zero pixels. The JPEG data was downloaded correctly (verified by dumping bytes).
- **Root Cause:** LVGL's TJPGD decoder uses the filesystem abstraction to read image data. When the JPEG is in a RAM buffer (not a file on disk), TJPGD needs `LV_USE_FS_MEMFS` enabled to treat a memory pointer as a "file." Without it, TJPGD cannot open the image data source and silently produces an empty decode.
- **Fix:** Added `CONFIG_LV_USE_FS_MEMFS=y` to `sdkconfig.defaults`. TJPGD now reads JPEG data directly from the PSRAM cache buffer.
- **Prevention:** Any LVGL image decoder that reads from RAM buffers (not SD card or flash) requires `LV_USE_FS_MEMFS=y`. This is not obvious from the TJPGD documentation — it's a dependency through LVGL's file abstraction layer. Always verify decode output is non-empty after enabling a new decoder.

### JPEG SOF Marker Parsing Needed for lv_image_dsc_t Dimensions
- **Date:** 2026-04-15
- **Symptom:** Downloaded JPEG images were invisible in chat — the bubble was allocated but nothing appeared on screen. No crash, no error, just a zero-height invisible image.
- **Root Cause:** `lv_image_dsc_t` requires `header.w` and `header.h` to be set before the image is displayed. Setting width=0 and height=0 (or leaving them uninitialized) causes LVGL to render a zero-area image — technically valid but invisible. The JPEG file contains dimensions in the SOF (Start of Frame) marker but LVGL does not auto-extract them before display.
- **Fix:** Added a JPEG SOF parser that scans the downloaded JPEG data for the SOF0 marker (0xFF 0xC0) and extracts the image height and width from the marker payload. These values are set on `lv_image_dsc_t.header.w` and `.header.h` before calling `lv_image_set_src()`.
- **Prevention:** When displaying JPEG images via `lv_image_dsc_t`, always parse the SOF marker to extract dimensions. Never assume width/height can be left at zero or "auto-detected" — LVGL image descriptors require explicit dimensions. The SOF0 marker is at a variable offset (after APP0/APP1 markers), so the parser must scan forward through the JPEG header.

### lv_image_dsc_t Must Be Persistent (Not Stack-Local)
- **Date:** 2026-04-15
- **Symptom:** Image appeared briefly then corrupted or caused a crash on the next LVGL refresh cycle. Sometimes showed garbage pixels, sometimes triggered a Store access fault in the draw pipeline.
- **Root Cause:** The `lv_image_dsc_t` struct was declared as a local variable inside the async callback function. After the callback returned, the struct went out of scope. LVGL's image widget holds a pointer to the descriptor and reads from it on every redraw — accessing freed stack memory.
- **Fix:** Allocate `lv_image_dsc_t` from PSRAM (as part of the media cache slot) so it persists for the lifetime of the image widget. The descriptor lives alongside the JPEG data in the pre-allocated cache slot.
- **Prevention:** `lv_image_dsc_t` (and any struct passed to `lv_image_set_src()`) must outlive the `lv_image` widget that references it. Never use stack-local or function-scoped descriptors. Allocate from heap or embed in a long-lived struct. Same rule applies to `lv_font_dsc_t` and similar LVGL descriptor types.

### Pre-Allocated PSRAM Slots Prevent Image Alloc/Free Fragmentation
- **Date:** 2026-04-15
- **Symptom:** After displaying 20+ images across multiple chat sessions, PSRAM allocations for new images started failing even though total free PSRAM was adequate.
- **Root Cause:** Each image download allocated a ~500KB PSRAM buffer, and freeing it after the image was no longer visible left fragmented holes. Over time, PSRAM fragmentation mirrored the internal SRAM fragmentation problem (see "Internal SRAM Fragmentation" entry) — total free was fine but largest contiguous block was too small.
- **Fix:** Pre-allocate 5 fixed PSRAM slots (581KB each, ~2.9MB total) at boot in `media_cache_init()`. Images are written into these slots using LRU eviction. No runtime malloc/free for image data — the 5 slots are reused forever.
- **Prevention:** For large, repeatedly allocated/freed buffers on ESP32-P4 (images, audio, video frames), always pre-allocate a fixed pool at boot. Use LRU or ring-buffer eviction within the pool. Runtime `heap_caps_malloc`/`heap_caps_free` of large PSRAM blocks will fragment the heap within hours of continuous use.

---

## Widget Platform (April 2026) — Architecture Decisions

### Why typed widget vocabulary and not a layout engine
- **Date:** 2026-04-18
- **Context:** Designing v1 of the skill-surface platform. Alternative options were (a) Python-only with Tab5 rendering opinionated native screens, (b) YAML-declared layouts, (c) full L3 app manifest with a JSON layout engine.
- **Decision:** Typed widget vocabulary (6 primitives: live, card, list, chart, media, prompt). Skills emit state with fixed slots; device renders opinionatedly.
- **Why:** (a) fails portability — skills tied to one device. (b)/(c) fail because every skill author has to understand every device's screen size, touch capability, and color depth; novice authors ship broken layouts. See `docs/WIDGETS.md` §1 non-goals.
- **Prevention:** Don't revisit this decision unless adding a 2nd renderer surfaces a widget type the 6 can't cover. Escape hatch is `widget_media` (JPEG pre-rendered on the brain).

### Why live slot reuses `s_poem_label` instead of new LVGL objects
- **Date:** 2026-04-18
- **Context:** The home screen already has a bottom text slot (`s_poem_label`) that switches between "last note", "offline", "standing by", and edge states. The widget platform's live slot needs to render a title + body on top of the orb.
- **Decision:** Extend `s_poem_label`'s state machine to include a widget-live source, highest priority. Create zero new LVGL objects per widget update.
- **Why:** Creating new LVGL objects per widget tick was the root cause of the LV pool fragmentation crashes from the v5 UI pass (see "Internal SRAM Fragmentation" and "lv_malloc NULL deref in circ_calc_aa4"). Reusing existing objects with text updates is O(1) memory.
- **Prevention:** Never create new LVGL objects in the widget live render path. If a new visual element is absolutely required, pre-allocate it in `ui_home.c` init and toggle visibility.

### Widget action rate-limiting prevents SDIO TX flooding
- **Date:** 2026-04-18
- **Context:** Earlier stress tests (rapid nav + rapid orb taps) exhausted the ESP-Hosted SDIO TX `copy_buff` pool and panicked. Patched in `components/espressif__esp_hosted/host/drivers/transport/transport_drv.c` to drop-not-crash.
- **Decision:** The widget platform's Tab5→Dragon `widget_action` emitter rate-limits at 4/sec. Rapid button-mashing collapses to 4 events per second regardless of tap count.
- **Why:** Even with the copy_buff patch, flooding would drop legitimate LWIP traffic. The rate-limit matches user intent (nobody legitimately taps an action 10 times in a second) while preserving SDIO headroom.
- **Prevention:** Any new Tab5-initiated WS message type should include a rate limit. Consider 4/sec default; raise only if a specific use case demands it.

### Skills vs native screens — the product split
- **Date:** 2026-04-18
- **Context:** The widget platform's success depends on choosing what becomes a "skill" (Python on Dragon) vs what stays as a "native screen" (C firmware on Tab5).
- **Decision:**
  - **Widget-worthy:** anything with *state* — timers, reminders, notifications, polls, ambient updates, completion cards, agent activity, list pickers.
  - **Native-only:** hardware-specific features — camera viewfinder, keyboard layout, voice overlay orb (the voice pipeline itself), touch calibration, OTA UI.
  - **Gray area:** home screen, settings. These are native but *hold widget slots* — home has the live slot; settings may eventually show a widget per skill for config.
- **Prevention:** When adding a new feature, first ask "does it have state that changes over time based on external data?" If yes → widget. If no → native screen.

### Capability downgrade belongs on the brain, not the device
- **Date:** 2026-04-18
- **Context:** If a device doesn't support `widget_chart`, something has to fall back. Options: device ignores, device synthesizes a text fallback, brain synthesizes and emits a `widget_card` instead.
- **Decision:** Brain synthesizes, emits `widget_card` with text body ("avg 42, trending up"). Device never sees a widget type it can't render.
- **Why:** Pushing the fallback to the brain keeps device renderers simple ("render exactly the 6 widgets"). Tab5 has enough on its plate. Also: brain has better context to synthesize meaningful text summaries than device code would.
- **Prevention:** New widget types must be accompanied by a brain-side fallback to an existing supported type before they're added to the protocol.


---

## Audit Wave 6 — Stability + D5/D6 (April 2026)

### DMA pool exhaustion is a third reboot class, distinct from SRAM/PSRAM frag
- **Date:** 2026-04-20
- **Symptom:** Tab5 silently dropped off LAN 2-5 min into any session. LVGL loop kept running (`ui_task alive, lvgl_fps=29` in serial), device still logged on `/info` with `wifi_connected=True`, but ARP from the router went incomplete and every WS reconnect hit `ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT errno=119`. Users saw "Dragon unreachable" until they reflashed.
- **Root Cause:** WiFi TX/RX descriptor ring is allocated from the DMA-capable internal SRAM pool. When the pool drops below ~15KB free, the WiFi driver can no longer allocate its Rx slot. The radio stays associated (driver flag `wifi_connected=True`) but no packets actually flow. `heap_watchdog.c` logged the "DMA pool critically low" warning at the 16KB soft threshold but had NO reboot path — only SRAM and PSRAM fragmentation triggered `esp_restart()`. So the device sat stuck alive-but-WiFi-dead until the user intervened.
- **Fix:** Added a third reboot class to `heap_watchdog.c`. When `MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL` free size drops below `HEAP_WD_DMA_CRITICAL_BYTES (16 KB)` for 2 consecutive 60 s samples (2 min), esp_restart() — with the same voice-active grace window that the SRAM path uses. Threshold was initially 8KB (too conservative) per on-device soak showing WiFi starts dropping packets at ~15KB free, before exhaustion.
- **Prevention:** Monitor DMA pool alongside PSRAM and SRAM. Any shared-pool subsystem (WiFi driver, SPI flash, I2S) needs a reboot safety net, not just a soft warning. When adding a new watchdog class, test both the trigger AND the recovery on hardware — thresholds chosen from `menuconfig` defaults rarely match real device wear.
- **Known limitation:** This is a RECOVERY fix, not a PREVENTION fix. The underlying DMA drain still needs root-cause investigation — audit P0 candidates include per-TTS-chunk `heap_caps_malloc` churn (`voice.c:1303-1312`) and `widget_store_gc` call frequency. A 3-minute reboot cycle is better than indefinite unreachability, but a stable device should never hit the threshold.

### Empty text_update must pop the last bubble, not be silently dropped
- **Date:** 2026-04-20
- **Symptom:** Dragon rendered a pure-code-block response as a JPEG media event + sent `text_update` with an empty string to clear the raw-markdown chat bubble. Tab5 dropped the empty update silently; the raw markdown stayed visible ABOVE the decoded JPEG.
- **Root Cause:** `ui_chat_update_last_message()` had `if (!text || !*text) return;` at both the entry and inside the async callback. This was a defensive guard against bad WS input, but it collapsed the legitimate "clear the bubble" signal into a no-op. Dragon's protocol assumes Tab5 will interpret empty text as "remove".
- **Fix:** Accept empty strings. In the async callback, if the text is empty, call `chat_store_pop_last()` to remove the now-stale AI text bubble. The accompanying media event then renders alone.
- **Prevention:** When designing "replace last" protocol messages, define what empty payload means explicitly (remove, clear, or retain). Document it in `docs/protocol.md`. Don't let input validators accidentally strip semantic content.

### Dragon must send text_update BEFORE media events, not after
- **Date:** 2026-04-20
- **Symptom:** Related to the bubble-pop fix above. Even after Tab5 accepted empty updates, the wrong bubble was being removed because media events arrived first, making them the "last" bubble.
- **Root Cause:** Event order is load-bearing. Tab5's `chat_store_pop_last` removes the tail of the store. If the media event appends MSG_IMAGE before text_update fires, the pop removes the image, not the obsolete text bubble.
- **Fix:** Reorder in `server.py` both cloud and TC paths: emit `text_update` FIRST, then media events. Verified via `tests/audit/test_d5_d6_ws.py` (TinkerBox) which asserts `text_update.index < media.index` in the outbound event stream.
- **Prevention:** When designing multi-message protocol replacements, document the required order in `docs/protocol.md` and back it up with an assertion test that runs against a live server. The Python-side test is in the TinkerBox repo; Tab5 clients that consume this protocol should mirror the contract.

### Objective WS-level audit tests are better than device screenshots for proving Dragon correctness
- **Date:** 2026-04-20
- **Symptom:** Visual verification on Tab5 kept failing — not because Dragon was wrong, but because Tab5's WiFi kept dying (see DMA entry above). Trying to prove audit fixes with device screenshots produced flaky results dependent on device stability.
- **Root Cause:** Tab5's DMA + WiFi issues are orthogonal to Dragon-side audit fixes. Using Tab5 as the test harness conflates two failure modes.
- **Fix:** Added `tests/audit/test_d5_d6_ws.py` in TinkerBox — connects directly to Dragon's `/ws/voice` WebSocket with a synthetic device registration, exercises the same code paths Tab5 does, and asserts the protocol invariants (`text_update` before `media`, no `<tool>` markup in `llm` stream). Bypasses Tab5 entirely.
- **Prevention:** For audit/regression tests that verify server-side behavior, prefer a WS-level probe over a device-level visual. Reserve device screenshots for end-to-end UX verification once protocol correctness is established. Keep the probe in the TinkerBox repo (not in a separate test infrastructure) so any Dragon change can trigger it.

### vTaskDelete(NULL) on P4 corrupts TLSP cleanup
- **Date:** 2026-04-20 (wave 13 C4)
- **Symptom:** Several helper tasks (in `voice.c`, `debug_server.c`, `ui_home.c`, `ui_memory.c`, `chat_msg_view.c`) called `vTaskDelete(NULL)` as their exit path. On ESP32-P4 with PSRAM XIP this occasionally tripped `esp_ptr_executable()` inside FreeRTOS's TLSP deletion callbacks — because `.text` at `0x4800xxxx` isn't recognized as executable — causing a LoadStoreError on task teardown.
- **Root Cause:** Known issue #20 — `CONFIG_FREERTOS_TLSP_DELETION_CALLBACKS=n` in `sdkconfig.defaults` masks the *default* TLSP handler, but any code path that calls `vTaskDelete(NULL)` from inside the task being deleted is still fragile on P4 because the kernel has to unwind the TLSP slot list with the task stack already being torn down.
- **Fix:** Swept all 12 sites to `vTaskSuspend(NULL)` with an inline comment. The task stays parked; FreeRTOS reclaims stack via the IDLE task's cleanup path without running the in-task deletion code. Memory overhead is ~10 TCBs that never run again — negligible.
- **Prevention:** `vTaskDelete(NULL)` is banned on ESP32-P4 with PSRAM XIP. Use `vTaskSuspend(NULL)`. Grep should catch any new additions — if one sneaks in, the crash is a LoadStore at `xPortRaisePrivilege` which is easy to misattribute.

### Brownout detector saves coredumps from USB supply dips
- **Date:** 2026-04-20 (wave 13 C5)
- **Symptom:** During sustained WiFi TX bursts on USB power the Tab5 would sometimes hang with the LVGL thread stuck and no reboot — observable as a frozen screen that never came back without a physical replug.
- **Root Cause:** The USB supply dips during WiFi TX peaks are enough to corrupt PSRAM + internal SRAM mid-instruction, but not enough to power-cycle the P4. The CPU runs through the dip with scrambled state; the main loop looks alive but pointers are torched.
- **Fix:** Enabled `CONFIG_ESP_BROWNOUT_DET=y` with `CONFIG_ESP_BROWNOUT_DET_LVL_SEL_0=y` (the most tolerant threshold — ESP32-P4 is noisier than S3/C-series on this rail). A dip now triggers a clean reboot, and wave-11's coredump path captures it for post-mortem.
- **Prevention:** Always enable the brownout detector on any board that can run on cheap USB power. The cost is measured in tens of nanoamps.

### voice.c public APIs read s_state without the state mutex
- **Date:** 2026-04-20 (wave 13 H1)
- **Symptom:** Concurrency audit: `voice_start_listening`, `voice_cancel`, `voice_send_text`, and the async_connect_task poll all read `s_state` directly without `s_state_mutex`. No visible bug surfaced in hand-testing, but the race is real — WS RX callback runs on Core 1's WS task, every caller runs on Core 0 LVGL, and the P4 has per-core caches that can tear-read a `voice_state_t`.
- **Root Cause:** The original code assumed aligned enum reads are atomic, which is true on single-core ARM but not on dual-core P4 when two tasks pin to different cores and the enum lives in DRAM with no explicit barrier.
- **Fix:** All public API state checks now go through `voice_get_state()` which takes `s_state_mutex`. The async_connect_task polling loop uses the same accessor. `voice_start_listening` and `voice_send_text` snapshot state once at entry into a local `cur_state` so the whole function reasons about the same tick.
- **Prevention:** Every multi-task-visible field should have one read path (through a mutex-taking accessor) and one write path (through `voice_set_state` which already locks). No direct reads outside the mutex. Treat the static field as if it were volatile-fetched-under-lock.

### NVS mutex used portMAX_DELAY, starving the heap watchdog
- **Date:** 2026-04-20 (wave 13 H10)
- **Symptom:** Under a corrupted-NVS recovery scenario, an NVS write on Core 0 took ~800 ms. Core 1's heap watchdog tried to read the `s_nvs_write_count` counter (which is `uint32_t` and protected by the same mutex on writes), waited on `portMAX_DELAY`, and tripped the heap-watchdog's own 5 s WDT. Device rebooted without a clean coredump.
- **Root Cause:** `NVS_LOCK()` macro used unbounded wait. `set_u32` was missing the mutex entirely (concurrent `set_u8` + `set_u32` on the same handle is UB per ESP-IDF docs).
- **Fix:** New `nvs_try_lock(timeout_ms)` with 2000 ms bound (2.5x the worst observed write). All getters fall back to the default value on timeout and log a warning; all setters return `ESP_ERR_TIMEOUT`. `set_u32` now locks. Callers were not updated — an old caller that ignored the return value of `set_u32` still works; a new caller that checks the return now gets the real error.
- **Prevention:** Every mutex take in an embedded system should have a bounded timeout. `portMAX_DELAY` is a debugging convenience, not production code. If the caller can't handle a timeout, the bound should still exist but with a log + abort (fail-fast beats silent deadlock).

### SD-card note writes needed fsync, not just fflush
- **Date:** 2026-04-20 (wave 13 H8)
- **Symptom:** Power-loss testing: a yank during a note save left `notes.json.tmp` readable but with stale/partial content, which `rename()` then promoted to `notes.json`. Silent data corruption.
- **Root Cause:** `fflush()` pushes libc's buffer into the kernel; the ESP-IDF FATFS VFS still holds dirty sectors in its own cache until an `f_sync()` lands. The atomic-rename pattern doesn't help if the "new" file is itself half-written.
- **Fix:** Added `fsync(fileno(f))` between `fflush` and `fclose`. ESP-IDF VFS routes `fsync()` to FATFS's `f_sync()` which pushes dirty sectors to the SD card before we even try the rename.
- **Prevention:** Atomic-write-via-rename is only atomic if you `fsync` the temp file before renaming. The pattern is: write → fflush → fsync → fclose → rename. Skip any of those steps and the atomicity is theatrical.

---

## Multimodal + Camera

### MJPEG file extension forced to .MJP because LFN is disabled
- **Date:** 2026-04-27
- **Symptom:** First end-to-end test of video recording (#291) — REC button tapped, `cb_record_btn` fired, but `fopen("/sdcard/VID_0000.mjpeg", "wb")` returned NULL.  Toast read "Open file failed" and obs ring showed `rec: fopen ... failed`.
- **Root Cause:** `CONFIG_FATFS_LFN_NONE=1` in sdkconfig.defaults — FATFS only allows 8.3 short filenames.  `.mjpeg` is 5 characters, which doesn't fit the 3-char extension limit.  Other code in the firmware uses 3-char extensions (`.jpg`, `.bmp`, `.wav`, `.txt`) and never tripped this — recording was the first time anything tried `.mjpeg`.
- **Fix:** Renamed extension to `.MJP` (3 chars).  ffmpeg + VLC sniff the magic bytes and play these natively regardless of extension; `.MJP` is just a cosmetic affordance.  Documented in `ui_camera.c` so the next person doesn't try `.mjpeg` again.
- **Prevention:**
  1. **8.3-only is one `grep CONFIG_FATFS_LFN sdkconfig.h` away** — when a new file extension is being introduced, check the LFN config first.
  2. **Enabling LFN costs RAM** (`LFN_BUF_SIZE` per open file) so the tradeoff is real.  Stay 8.3 unless a feature genuinely requires it.

### ESP32-P4 has only one HW JPEG encoder engine (`jpeg_new_encoder_engine` fails on second alloc)
- **Date:** 2026-04-27
- **Symptom:** `ui_camera.c` recording feature initially tried `jpeg_new_encoder_engine()` to get its own encoder.  Always failed with `E (xxx) jpeg.encoder: jpeg_new_encoder_engine(93): no memory for jpeg encoder rxlink` because `voice_video.c` (the call-streaming module) had already allocated the only one at boot.
- **Root Cause:** The ESP32-P4 SoC has a single hardware JPEG engine (named "rxlink" in IDF).  `jpeg_new_encoder_engine()` claims it; a second call fails.  IDF doesn't expose this as a documented limit but the error message + Espressif HW docs confirm it.
- **Fix:** Exposed `voice_video_encode_rgb565()` as a public function in `voice_video.{c,h}`.  The recording timer in `ui_camera.c` calls it instead of allocating its own encoder.  The mutex inside `voice_video.c` serialises concurrent uplink-stream + record-to-SD encodes.
- **Prevention:**
  1. **For singleton hardware resources, expose a shared accessor + mutex from the module that owns it.**  Don't let multiple modules try to claim the same HW.
  2. **Check `idf.py -c menuconfig | grep -i jpeg`** at design time — the encoder is a single resource, the decoder ring is also limited.

### Camera recording needs DMA-aligned buffers (`heap_caps_malloc` isn't enough)
- **Date:** 2026-04-27
- **Symptom:** First version of the `.MJP` recorder allocated the JPEG output buffer with `heap_caps_malloc(96 KB, MALLOC_CAP_SPIRAM)`.  Encoder returned `ESP_ERR_INVALID_ARG: bit stream is not aligned`.
- **Root Cause:** The ESP32-P4 JPEG engine reads/writes via DMA and requires its buffers to be aligned to (at minimum) the SoC's cache-line + DMA descriptor size.  Plain PSRAM allocations don't guarantee this alignment.  IDF provides `jpeg_alloc_encoder_mem(size, &cfg, &actual)` which understands the engine's alignment requirements and returns a properly-positioned buffer.
- **Fix:** Replaced both the JPEG output buffer and the rotation scratch buffer with `jpeg_alloc_encoder_mem()` calls.  The lazy-allocated buffers persist for the lifetime of the camera screen and free in `ui_camera_destroy()`.
- **Prevention:**
  1. **For any HW peripheral with DMA, use the IDF-provided allocator helpers** — `jpeg_alloc_encoder_mem`, `dma_alloc_aligned`, etc.  Plain `heap_caps_malloc(MALLOC_CAP_DMA)` only ensures DMA-capable memory; it doesn't guarantee the alignment a specific peripheral requires.
  2. **Free symmetrically** — the IDF docs aren't always clear which `free` to use.  For `jpeg_alloc_encoder_mem`-returned buffers, plain `free()` is correct (verified via `voice_video.c` precedent).

---

## Debug + Test Harness

### `obs_event_t.kind` 16-char buffer truncated `camera.record_start` to `camera.record_s`
- **Date:** 2026-04-27
- **Symptom:** E2E harness's `await_event("camera.record_start", timeout_s=8)` failed every time, even though /events showed an event named `camera.record_s` was firing on REC tap.
- **Root Cause:** `obs_event_t.kind` was a `char[16]` buffer in `debug_obs.c`.  PR #294 added new event names like `camera.record_start` (19 chars) and `camera.record_stop` (18 chars) — both silently truncated by the `snprintf` write into the fixed buffer.
- **Fix:** Bumped buffer to `char[32]` in PR #295 (firmware fix included alongside the harness commit).  Existing 16-char names like `voice.state` still fit; new names have 50% headroom.
- **Prevention:**
  1. **Whenever you add a new event-name string, check it fits the buffer** — better, use a compile-time `_Static_assert(sizeof("camera.record_start") <= sizeof(((obs_event_t*)0)->kind))` in `debug_obs.c`.  TBD as a follow-up; not blocking.
  2. **Truncation in observability layers is silent by definition** — the event still gets written, just with the wrong name.  Test harnesses doing exact-string matching will fail without a clear error.

### ESP-IDF httpd is single-task — long-poll handlers freeze the entire debug server
- **Date:** 2026-04-27
- **Symptom:** Experimental `?block_until=&timeout_ms=` long-poll variant of `GET /events` worked correctly when the matching event was already in the ring (21 ms first-call latency) but blocked all other requests for the wait duration.  After a multi-minute test run, Tab5 hit a PANIC reset.
- **Root Cause:** ESP-IDF's `esp_http_server` uses one `httpd_task` to service all connections via `select()`.  When a handler `vTaskDelay`s, every other in-flight or pending request waits.  My implementation also re-allocated the cJSON events array on every 200 ms iteration — across ~minute-long waits, the per-iteration alloc/free churn accumulated heap fragmentation that eventually pushed the device over.
- **Fix:** Reverted the long-poll feature.  `GET /events?since=N` stays instant-return only.  Note left in `debug_server.c` so the next person who reaches for "but long-poll would be nice" sees the prior art and doesn't try the same approach.  Harness in `tests/e2e/driver.py` uses 250 ms polling instead.
- **Prevention:**
  1. **Single-thread http servers cannot host long-poll handlers** — a `vTaskDelay` inside the request handler is equivalent to taking the whole server offline for the wait duration.  If you need push-style notification, either (a) use a different transport (a dedicated WebSocket on a separate task), (b) spawn a per-request background task via `httpd_queue_work`, or (c) just tell the client to poll faster.
  2. **Per-iteration heap allocation in tight loops on ESP32 is suspicious** — even if balanced (alloc + free), the TLSF heap fragments.  Allocate-once + reuse, or batch the work outside the loop.

### TTS audio sounded sped-up (~1.5×) — half-buffer split clamped upsample output to 67 % of every chunk
- **Date:** 2026-04-28
- **Symptom:** User report: "the audio goes at 2× speed or something".  TTS replies arrived at the speaker compressed and rushed; intelligible but missing audio chunks.  Affected every Local-mode and Cloud-mode TTS turn.
- **Root Cause:** `voice.c::handle_binary_message` decodes the WS frame into the *low half* of `s_upsample_buf` (size `UPSAMPLE_BUF_CAPACITY/2 = 4096` samples) and upsamples 16 kHz → 48 kHz into the *high half*.  The guard `if (max_out > dec_cap) max_out = dec_cap` clamped the upsample output at 4096 samples — but Dragon ships TTS in 4096-byte chunks (`server.py:_handle_text_body` + `pipeline._synthesize_and_send`), which is **2048 int16 samples** that upsample 1:3 to **6144 samples**.  Every chunk got clamped 6144 → 4096, losing 33 % of its content.  At 48 kHz playback rate, the speaker plays the truncated content in the same wall-clock interval the un-truncated content would have used, so 128 ms-of-source landed on the speaker as ≈ 85 ms-of-output.  Cumulatively the message played in ≈ 67 % of its real duration — "sped up" to a listener's ear.
- **Fix:** Bumped `UPSAMPLE_BUF_CAPACITY` from 8192 to 16384 samples (16 KB → 32 KB PSRAM).  `dec_cap` is now 8192, so 6144-sample upsamples fit in the high half with comfortable headroom.  Pre-existing `if (max_out > dec_cap)` guard now only kicks in for chunks > 2730 input samples — well above Dragon's 2048-sample cap.
- **Why it took this long to surface:** The `ESP_LOGW(TAG, "handle_binary: upsample would exceed half-buf — truncating")` warning fired ~20 times per TTS reply, but voice journals are noisy under load and "truncating" reads as a graceful-degradation note, not a content-loss bug.  Nobody on the original commit noticed the lost samples were audible until a user described the symptom in subjective terms.
- **Prevention:**
  1. **When splitting a single buffer for two stages with different size requirements, size the halves to the stage's actual output ratio** — a 1:3 upsampler needs the output half to be 3× the input half, not equal.  Or just use two separately-allocated buffers so the math is local.
  2. **Treat "truncating" warnings as content-loss bugs, not graceful degradation.**  At minimum the log should include the actual numbers (`6144 → 4096 samples lost`) so the magnitude is obvious in a journal scrape.
  3. **Audio bugs that degrade playback duration are subjectively reported in fuzzy terms** ("sounds sped up", "sounds slow", "choppy") that don't immediately point at sample-rate / sample-count math.  When debugging audio, check the chain end-to-end with sample-counts at every transform — if input duration ≠ output duration, content was lost or stretched.

### Diagnostic event-snapshots advance the cursor that subsequent waiters need
- **Date:** 2026-04-27
- **Symptom:** Harness's `await_event("camera.capture", timeout_s=10)` failed after a `tap(360,1100)` even though `/screenshot` clearly showed "Photo saved!" toast and direct `/events?since=0` confirmed `camera.capture` was in the ring.
- **Root Cause:** `Tab5Driver.events()` advances `_last_event_ms` to the latest event's timestamp on every call.  The scenario runner's per-step diagnostic block called `events(since_ms=events_before)` to capture what fired during the step — and inadvertently advanced the cursor *past* the events it captured.  The next step's `await_event` started polling from a cursor that was already in the future relative to the events it was looking for.
- **Fix:** Added `peek=True` parameter to `events()`.  When `peek=True`, the cursor doesn't advance.  The scenario runner's diagnostic snapshot uses `peek=True`; explicit `await_event` still advances the cursor.
- **Prevention:**
  1. **Stateful side effects in "read" methods are easy to write and hard to debug.**  Whenever a read advances a cursor, give callers an opt-out for the side effect.
  2. **Diagnostic plumbing should never compete with primary control plumbing.**  Capturing "what happened" for a report is observation; consuming events for routing is action.  Separate the two.

### K144 chain setup ACK reads must skip non-matching frames — not bail on the first one
- **Date:** 2026-04-29
- **Symptom:** Phase 6b first attempt at `voice_m5_llm_chain_setup`: `audio.setup` and `asr.setup` succeeded cleanly, but `llm.setup` failed with `err=0 msg=''`.  Confusing because error_code 0 means the K144 ACK'd OK, yet the chain bailed.
- **Root Cause:** Once `audio.setup` and `asr.setup` are live, the K144's ASR unit immediately starts publishing stream frames on the UART carrying its own `work_id` (e.g. `asr.1012`) the moment it sees published audio on `sys.pcm`.  My `chain_setup_unit` helper read **the first frame** that arrived after sending `llm.setup`, parsed it, and checked: `request_id matches? error_code 0? work_id non-NULL?`  The frame was a valid ASR stream chunk — `error_code` was 0 (it's a stream frame, not a NACK), `work_id` was `asr.1012` (the ASR unit's id), and `request_id` did NOT match the LLM-setup ID we'd just sent — so the helper logged the (uselessly partial) error and bailed.  The actual `llm.setup` ACK arrived a beat later but my helper had already given up.
- **Fix:** Rewrote `chain_setup_unit` to keep reading frames until either (a) one matches our `request_id` (success path) or (b) the timeout elapses.  Frames with mismatched `request_id` are silently dropped — they're stream-side noise from already-running upstream units, not relevant to the setup ACK we're waiting for.
- **Prevention:**
  1. **In a publish/subscribe protocol where multiple actors emit on the same wire, every `recv` loop is implicitly multiplexed — you cannot assume the next frame answers your last request.**  Always filter by `request_id` and keep reading until you see *yours*.  This is the same lesson as `stream_collect` (which already did this for inference) but easier to forget on a one-shot setup call.
  2. **Logging "err=0 msg='' work_id=null" is the symptom of the multiplex-blindness above, not a K144 bug.**  When you see error_code 0 but the helper bailed, the helper is probably reading a stream frame from a different unit.  Log the *full* response (request_id, work_id, object) at error time so this is obvious next time.

### K144 single_speaker_english_fast TTS in stream-from-LLM mode crashes with Eigen assertion
- **Date:** 2026-04-29
- **Symptom:** With `tts.setup` chained `input=[llm.NNNN]` and `response_format=tts.base64.wav`, the K144's `llm-tts` service crashes mid-utterance with `Reshaped.h:125: Assertion 'reshapeRows * reshapeCols == xpr.rows() * xpr.cols()' failed.` and exits with status 6/ABRT.  systemd auto-restarts the service, but the chain's tts subscription is then orphaned — the LLM keeps publishing reply text but no TTS audio is ever emitted on the UART.
- **Root Cause:** SummerTTS (the engine behind K144's `single_speaker_english_fast` model) does an Eigen tensor reshape that assumes a particular input shape.  When fed token-by-token streaming from K144's LLM unit (`qwen2.5-0.5B-prefill-20e`), some token pattern (probably an empty / whitespace-only chunk on the LLM's `finish=true` boundary) violates the reshape pre-condition.  Reproducible across K144 service restarts; not present when using `voice_m5_llm_tts` for one-shot synthesis (which sends the whole text as a single string, not token-by-token).
- **Fix (current):**  None on the K144 side — that's vendor SummerTTS code we don't ship.  Workaround on Tab5 side that works today: keep `voice_m5_llm_tts` (the one-shot synth path used by m5tts REPL + the failover flow) and **don't** rely on chained `tts.setup`.  The chain pipeline gets ASR + LLM streaming (which is fine), and TTS is invoked separately on each LLM `finish=true` to synthesise the assembled reply.
- **Prevention:**
  1. **When wiring vendor pub/sub pipelines, test each subscriber under realistic upstream traffic before committing to it as a code path.**  K144's TTS unit can stream from LLM in the documented config, but `single_speaker_english_fast` chokes on the actual LLM output shape.  We discovered this only by running the chain with a live mic — passing `m5tts hello world` as a one-shot worked fine and hid the bug.
  2. **Vendor crash + auto-restart breaks subscription chains silently.**  When systemd restarts a K144 service, its work_id changes (e.g. tts.1043 → tts.1044), but other units in the chain still publish to the old subscriber URL.  No error logs on the publisher side.  Detection: the subscriber's logs show an exit + restart but the chain caller sees zero TTS frames.  Always check K144's `journalctl -u llm-tts --no-pager -n 30` after a quiet TTS path before assuming the chain is wired wrong.

### K144 voice-assistant chain runs autonomously — Tab5 only drains output frames
- **Date:** 2026-04-29
- **Symptom:** Phase 6b first sketch had Tab5 running an inference loop: capture mic, push PCM over UART, wait for ASR, send to LLM, get reply, request TTS, play back.  This was the natural extrapolation from the existing `voice_m5_llm_infer` API but didn't match what the K144 actually exposes.
- **Root Cause:** I was modeling the K144 as a stateless function (text in → text out, audio in → audio out) when it's actually a publish/subscribe runtime.  The K144's `audio` unit publishes `sys.pcm` from its onboard mic; `asr` subscribes and publishes text; `llm` subscribes to ASR text and publishes reply text; `tts` subscribes to LLM text and publishes audio bytes.  Once the four units are wired up via `setup` calls with chained `input` arrays, the entire pipeline runs **autonomously inside the K144** — no host involvement after setup.  The TCP spike at `/tmp/m5spike/chain_probe.py` proved this: `[ASR] delta='ah'` → `[LLM] delta='Ah, I understand. How may I assist you today?'` → `[TTS] AUDIO (4352 samples)` all flowed back over a passive socket while the spike was just calling `recv`.
- **Fix:** Implemented `voice_m5_llm_chain_setup` / `_chain_run` / `_chain_teardown` in `voice_m5_llm.{c,h}` that:
  1. Issue four `setup` calls with chained `input` references (`audio` → `asr` ← `sys.pcm`, `llm` ← `asr.NNNN`, `tts` ← `llm.NNNN`).
  2. Drain UART output frames in a passive loop, dispatching ASR/LLM text to a `text_cb` and base64-decoded TTS PCM to an `audio_cb`.
  3. Tear down each unit in reverse order via `exit`.
- **Live verification:**  Built + flashed firmware, sent `m5chain 35` over the REPL.  User said "ah".  Logs:
  ```
  [ASR]  ah
  [ASR]  ah. (fin)
  [LLM] Ah, I understand. How may I assist you today?
  [TTS] 4352 samples (~272 ms)         ← played through Tab5 speaker
  ```
  Full closed loop: K144 mic → ASR → LLM → TTS → UART → Tab5 speaker (upsampled 1:3 to 48 kHz).
- **Prevention:**
  1. **Before designing a host-driven control loop for an external module, find out if the module already has its own runtime that just needs wiring up.**  M5Stack's StackFlow protocol is explicitly designed for inter-unit chaining via `input` arrays — read M5Module-LLM's example sketches (KWS_VAD_Whisper_LLM_TTS) before assuming the host has to mediate every byte.
  2. **Pub/sub modules don't need request/response framing for streamed output.**  The chain emits frames spontaneously; consumers need a passive `recv` loop with a stop signal, not an inference-driven request/wait pattern.
  3. **K144 work_id state is per-service-process and persists across host disconnects.**  After clean teardown via `exit` everything is freed, but if a chain setup fails partway through the K144 keeps the partial state.  Restarting the K144 services via adb (`systemctl restart llm-llm llm-asr llm-tts llm-audio`) clears it.  In code we tear down in reverse order on partial failure to minimise this.
