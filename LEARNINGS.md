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

### I2S TX/RX Clock Mismatch on Shared Bus
- **Date:** 2026-03-21
- **Symptom:** Audio playback was distorted when mic capture was active simultaneously.
- **Root Cause:** TX was configured as STD 2-slot mode (BCLK=1.536MHz) and RX as TDM 4-slot mode (BCLK=3.072MHz). Both share the same I2S bus (I2S_NUM_1), which means shared BCLK. The conflicting clock rates corrupted both streams (commit d547338, issue #25).
- **Fix:** Configured both TX and RX to use TDM 4-slot mode so BCLK is consistently 3.072MHz.
- **Prevention:** When TX and RX share an I2S bus, they MUST use the same slot mode and BCLK rate. Always configure both sides identically.

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
- **Date:** 2026-03-21
- **Symptom:** N/A (design constraint)
- **Root Cause:** Tab5 hardware routes both ES8388 (DAC/speaker) and ES7210 (ADC/mic) to the same I2S bus (I2S_NUM_1). This is a PCB-level constraint, not a software choice.
- **Fix:** Both TX and RX must use TDM 4-slot mode for consistent BCLK. Cannot use separate buses.
- **Prevention:** Accept this constraint. All I2S configuration changes must consider both the playback and capture sides simultaneously.

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

### No Hardcoded Voice Response Timeout
- **Date:** 2026-03-29
- **Symptom:** Tab5 disconnected from Dragon voice server after 120s while LLM was still processing
- **Root Cause:** Hardcoded VOICE_RESPONSE_TIMEOUT_MS forced disconnection regardless of whether Dragon was still working. ARM64 LLM inference is slow (~0.24 tok/s).
- **Fix:** Removed timeout entirely. Tab5 stays in PROCESSING state as long as WebSocket is alive. JSON heartbeat every 15s prevents TCP idle timeout. Only disconnects on WS error or Dragon error message.
- **Prevention:** Never use fixed timeouts for LLM inference. Processing time varies by model, prompt length, and hardware. Use connection health checks instead.
