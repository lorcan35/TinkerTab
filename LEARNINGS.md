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

- **Date:** 2026-04-06
- **Symptom:** Dragon Q6A keeps going offline for 10+ minutes, unreachable via network.
- **Root Cause:** Dragon uses WiFi (wlan0 at 192.168.1.89), not ethernet. WiFi power save was enabled (`Power save: on`), causing periodic disconnects. Additionally, PingOS services (node/Fastify) competed for port 3500 with our dashboard.
- **Fix:** `iw wlan0 set power_save off` + NetworkManager dispatcher script at `/etc/NetworkManager/dispatcher.d/99-wifi-powersave.sh`. Disabled PingOS services (`systemctl disable pingos-dashboard pingos-gateway pingos-chromium pingos-xvfb`). Also disabled EEE on ethernet (`ethtool --set-eee enp1s0 eee off`) as belt-and-suspenders.
- **Prevention:** Always check `iw wlan0 get power_save` on ARM SBCs using WiFi. Set power save off immediately after any reboot. The dispatcher script should handle this automatically but verify after updates.

- **Date:** 2026-04-06
- **Symptom:** SD card and WiFi SDIO were documented as conflicting (can't use simultaneously).
- **Root Cause:** The comment in sdcard.c was incorrect. Tab5 uses SDMMC Slot 0 for SD card and Slot 1 for WiFi SDIO — different slots on different GPIO banks. They coexist fine.
- **Fix:** Updated sdcard.c comment to reflect verified coexistence. Confirmed: 122GB SD card mounts and operates normally while WiFi is active.
- **Prevention:** Don't trust hardware conflict comments without testing. The ESP32-P4 SDMMC controller supports 2 independent slots.
