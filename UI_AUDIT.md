# TinkerTab UI Audit — Glyph OS (ESP32-P4, 720x1280, LVGL v9)

**Date:** 2026-03-27
**Firmware:** TinkerTab v1.0.0
**Files audited:** 9 ui_*.c + 9 ui_*.h + main.c + config.h

---

## Section 1: Existing UI Audit

### 1.1 ui_core.c — LVGL Engine Layer

| Aspect | Detail |
|--------|--------|
| **Purpose** | Initializes LVGL, display driver (DPI framebuffer flush with `esp_cache_msync`), touch input (GT911), tick timer (2ms), and dedicated UI task on core 0 |
| **Functional** | Display flush (partial render, double-buffered PSRAM), touch read, recursive mutex for thread safety, dark theme (amber primary / cyan secondary) |
| **Public API** | `tab5_ui_init()`, `tab5_ui_tick()`, `tab5_ui_get_display()`, `tab5_ui_lock()`, `tab5_ui_unlock()` |
| **Status** | **Fully functional** — production-grade core layer |

### 1.2 ui_splash.c — Boot Splash Screen

| Aspect | Detail |
|--------|--------|
| **Purpose** | Amber-on-black boot splash with "G L Y P H" wordmark, system check items, progress bar |
| **Widgets** | Large title label (Montserrat 48), subtitle "TinkerTab OS", accent line, up to 8 system check labels (mint check marks), progress bar (amber), status text, version label |
| **Functional** | `ui_splash_set_status()` adds check items and updates status. `ui_splash_set_progress()` animates bar. Called from main.c boot sequence (Hardware OK -> WiFi -> Dragon -> Ready) |
| **Status** | **Fully functional** — clean boot experience |

### 1.3 ui_home.c — Main Home Screen (Tileview, 4 Pages)

| Aspect | Detail |
|--------|--------|
| **Purpose** | Primary launcher with vertical snap-scroll tileview |
| **Architecture** | 4-page tileview + floating status bar + bottom nav bar + page dots |

#### Page 0 — Home / Lock Screen
| Element | Status |
|---------|--------|
| Large clock (Montserrat 48) | **Functional** — reads RTC, updates every 1s |
| Date label | **Functional** — static text (NOT dynamically formatted from RTC) |
| Glyph AI orb (140px inner + 180px ring) | **Functional** — breathing border opacity animation |
| Orb eye icon | Visual only |
| Greeting label ("Glyph AI" / "Dragon Connected") | **Functional** — updates based on Dragon state |
| Swipe-up hint arrow | Visual only |

**Issues on Page 0:**
- Date label is hardcoded "Wednesday, March 26" — NOT derived from RTC. Stale after first boot.
- Orb is not clickable (no tap-to-activate voice from home).

#### Page 1 — Apps Grid
| Element | Status |
|---------|--------|
| "Apps" title | Visual |
| 8 app icons (2 rows x 4 cols, rounded squares) | **Visual only — NONE are clickable** |
| Icons: WiFi, Dragon, Camera, Audio, Files, Battery, AI Chat, Settings | Rendered with LV_SYMBOL + colored backgrounds |

**Critical issue:** `make_app_icon()` never calls `lv_obj_add_flag(icon, LV_OBJ_FLAG_CLICKABLE)` and never adds event callbacks. The entire app grid is **non-interactive decoration**. Tapping any icon does nothing. There is no navigation from apps page to the actual Camera, Files, Audio, or Settings screens.

#### Page 2 — Dragon Remote
| Element | Status |
|---------|--------|
| Viewport placeholder (688x700 dark card) | **Visual placeholder** — shows play icon + "Dragon Remote" text. No MJPEG frames rendered in LVGL (streaming uses direct DPI framebuffer bypass) |
| Status card | **Functional** — shows "Streaming"/"Disconnected" + FPS from dragon_link |

**Issue:** When Dragon is streaming, MJPEG goes to the DPI framebuffer directly, but the LVGL tileview still renders on top. `tab5_display_set_jpeg_enabled()` is gated to page 2, but there's no mechanism to blend or avoid overdraw conflicts.

#### Page 3 — Settings (Inline)
| Element | Status |
|---------|--------|
| "Settings" title | Visual |
| Group 1: WiFi status, Dragon Link status | **Functional** — live text updates from `ui_home_update_status()` |
| Group 2: Memory (live), Display (static "720x1280"), Battery (static "---") | **Partial** — Memory updates, Battery label never populated |
| Group 3: Audio/Camera/IMU (all static chip names) | Static |
| About line | Static |

**Issues:** Battery row on Settings page 3 shows "---" forever — `update_timer_cb` never fills it. This is a different screen from the full `ui_settings.c` which IS wired. The inline settings page duplicates info but is read-only with no tap actions.

#### Status Bar (top of screen, overlays tileview)
| Element | Status |
|---------|--------|
| Time (left) | **Functional** — updates from RTC |
| WiFi icon (right) | **Functional** — white when connected, dim when off |
| Battery % (right) | **Functional** — reads `tab5_battery_percent()` |

**Issues:**
- No Dragon connection indicator in status bar
- No WiFi signal strength (just on/off icon)
- No battery icon/bar (just text %)
- Semi-transparent background (OPA_80) — adequate but no blur

#### Bottom Nav Bar
| Element | Status |
|---------|--------|
| 4 icons: Home, Apps, Dragon, Settings | **Functional** — tap navigates tileview. Active icon highlighted white |
| Page dots | **Functional** — updates on scroll |

#### Update Timer
- 1-second `lv_timer` calls `ui_home_update_status()` updating time, battery, WiFi, Dragon state, memory

### 1.4 ui_settings.c — Full Settings Screen (Standalone)

| Aspect | Detail |
|--------|--------|
| **Purpose** | Standalone settings screen with scrollable sections (Display, Network, Storage, Battery, About) |
| **Architecture** | Separate LVGL screen with back button -> destroys self |

| Section | Widgets | Status |
|---------|---------|--------|
| Display > Brightness | Slider 0-100, wired to `tab5_display_set_brightness()` | **Functional** |
| Display > Auto-rotate | Switch + orientation label from IMU | **Functional** (reads IMU) but does NOT actually rotate the display |
| Network > WiFi | Status label | **Functional** (connected/not connected) |
| Network > Bluetooth | Static "ESP-Hosted pending" | **Stub** |
| Network > Sync Time | Button triggers NTP sync in background task | **Functional** |
| Storage > SD Card | Free/total GB or "Not mounted" | **Functional** |
| Battery > Voltage | Live from INA226 | **Functional** |
| Battery > Status | Charging/Discharging | **Functional** |
| Battery > Level | Bar + percentage | **Functional** with color coding |
| About > Device/SoC/Firmware | Static labels | Visual |
| About > Free Heap / PSRAM | Live values | **Functional** |

**Issues:**
- Brightness slider defaults to 80 every time — does NOT read current brightness. Value is lost on screen re-entry.
- Auto-rotate switch logs state but never calls any display rotation API.
- **No way to reach this screen** — there is no click handler on the Settings icon in the app grid (Page 1) or the Settings nav icon that opens this screen. It's orphaned code.

### 1.5 ui_voice.c — Voice Interaction Overlay

| Aspect | Detail |
|--------|--------|
| **Purpose** | Full-screen voice overlay with animated orb, mic button, wave bars |
| **Architecture** | Floating mic button on `lv_layer_top()` + full-screen overlay shown during voice states |

| Element | Status |
|---------|--------|
| Floating mic button (bottom-left, 72px) | **Functional** — clickable, toggles voice overlay |
| Mic button state indicator dot | **Functional** — changes color per state |
| Full-screen overlay (95% opacity) | **Functional** — fade in/out animations |
| Animated orb (listening: cyan breathe, processing: purple pulse, speaking: green) | **Functional** — multi-layer glow, size changes |
| Wave bars (5 bars, speaking state) | **Functional** — animated height changes |
| Status text ("Listening...", "Thinking...", transcript) | **Functional** |
| Thinking dots animation | **Functional** |
| Cancel/close button | **Functional** |
| Auto-hide after speaking done (1.5s delay) | **Functional** |

**Issues:**
- Wired to `voice.h` state callbacks, but actual STT/TTS pipeline requires Dragon connection. No graceful fallback if Dragon is offline.
- No visual indication of microphone input level during listening.

### 1.6 ui_keyboard.c — On-Screen Keyboard Overlay

| Aspect | Detail |
|--------|--------|
| **Purpose** | Custom QWERTY/123 keyboard on `lv_layer_top()` with slide-up animation |
| **Architecture** | Full custom keys (not lv_keyboard), floating trigger button bottom-right |

| Element | Status |
|---------|--------|
| Floating trigger button (keyboard icon, bottom-right) | **Functional** — toggles keyboard |
| QWERTY letter layout (3 rows) | **Functional** — types into target textarea |
| Shift / Caps Lock (double-tap) | **Functional** |
| Backspace | **Functional** |
| Enter | **Functional** |
| Space bar | **Functional** |
| 123/ABC layer toggle | **Functional** — numbers + symbols layer |
| Slide-up/down animation (300ms/250ms) | **Functional** |

**Issues:**
- No textarea exists on any current screen to receive input. Keyboard overlay is built but has no target.
- No swipe-to-type, autocomplete, or emoji support.

### 1.7 ui_camera.c — Camera Viewfinder Screen

| Aspect | Detail |
|--------|--------|
| **Purpose** | Full camera screen with live preview canvas, capture to SD, resolution picker |

| Element | Status |
|---------|--------|
| Viewfinder area (720x960) with LVGL canvas | **Functional** — 10fps RGB565 preview from SC202CS |
| Capture button (shutter style, 80px circle) | **Functional** — captures frame, saves JPEG to SD |
| Resolution dropdown (QVGA/VGA/HD/Full) | **Functional** — changes camera res, reallocates canvas |
| "No SD" warning label | **Functional** — shown when SD unmounted |
| "No Camera" placeholder | **Functional** — graceful degradation |
| Gallery button | **Partial** — shows photo count text, but tap does nothing (no gallery view) |
| Local toast notifications | **Functional** — "Photo saved!", "Capture failed", auto-dismiss 2s |

**Issues:**
- **No way to reach this screen** — no click handler wired from the app grid Camera icon.
- Gallery button has no click handler — tapping it does nothing.
- No back button to return to home.
- No photo review after capture.
- Canvas centered but aspect ratio mismatch possible at non-VGA resolutions.

### 1.8 ui_audio.c — Audio Player Overlay

| Aspect | Detail |
|--------|--------|
| **Purpose** | Bottom-half overlay for WAV playback |

| Element | Status |
|---------|--------|
| Semi-transparent fullscreen backdrop | **Functional** |
| Bottom panel (640px, rounded top corners) | **Functional** |
| Close button (top-right, red circle) | **Functional** — stops playback, destroys overlay |
| Audio icon + filename | **Functional** |
| Play/Pause button (100px circle) | **Functional** — 3-state: play/pause/resume |
| Status label (Ready/Playing/Paused/Finished/Stopped) | **Functional** |
| Volume slider | **Functional** — wired to `tab5_audio_set_volume()` |
| Volume percentage label | **Visual** — set once at creation, NOT updated when slider moves |
| Background playback task | **Functional** — reads WAV in chunks from SD, 16-bit PCM only |

**Issues:**
- Only reachable by tapping .wav files in the file browser (which itself is unreachable from the app grid).
- No seek/progress bar — no way to see playback position or skip.
- Volume % label is static — does not update when slider is dragged.
- Only supports 16-bit PCM WAV. No MP3/OGG/FLAC.

### 1.9 ui_files.c — File Browser Screen

| Aspect | Detail |
|--------|--------|
| **Purpose** | SD card file browser with folder navigation |

| Element | Status |
|---------|--------|
| Top bar with back button + path label | **Functional** — back navigates up, exits at root |
| Scrollable file list | **Functional** — sorted (dirs first, then alpha) |
| File type icons (folder, image, audio, text, generic) | **Functional** |
| File size display | **Functional** |
| Tap folder -> navigate | **Functional** |
| Tap .wav -> audio player overlay | **Functional** |
| Tap .jpg/.png -> image preview | **Functional** — fullscreen overlay with close |
| Long-press -> file info popup | **Functional** |
| "No SD card" panel with retry button | **Functional** |
| Bottom storage bar (free/total GB) | **Functional** |

**Issues:**
- **No way to reach this screen** from the app grid — Files icon is not clickable.
- No file operations (delete, rename, copy).
- No file creation / text editor.
- Max 256 entries per directory.

---

## Section 2: Missing UI Elements

### 2.1 App Grid Click Handlers (P0 / S / Deps: none)

**THE most critical gap.** The 8 app icons on Page 1 are purely decorative. Each needs a click event that navigates to its corresponding screen:

| Icon | Target | Exists? |
|------|--------|---------|
| WiFi | WiFi config screen | **Screen missing** |
| Dragon | Tileview page 2 | Page exists, just needs handler |
| Camera | `ui_camera_create()` | Screen exists, needs handler |
| Audio | Audio player (needs a file picker or recent list) | Overlay exists, needs entry point |
| Files | `ui_files_create()` | Screen exists, needs handler |
| Battery | Battery detail or scroll to settings battery section | Could reuse settings |
| AI Chat | Text chat with AI | **Screen missing** |
| Settings | `ui_settings_create()` | Screen exists, needs handler |

**Priority:** P0
**Effort:** S (< 1 day) — just add `lv_obj_add_flag(icon, LV_OBJ_FLAG_CLICKABLE)` + event callbacks
**Dependencies:** WiFi config screen and AI Chat screen need to be built first

### 2.2 Screen Navigation / Back Stack (P0 / M / Deps: none)

Currently each screen (camera, files, settings) loads itself via `lv_screen_load()` and destroys itself to "go back." There is no navigation stack — if screen A opens screen B opens screen C, going back from C goes to... nothing.

**Need:**
- Simple nav stack (push/pop screen, max 4-5 deep)
- Universal back gesture (swipe right from left edge, or hardware back if available)
- All secondary screens must destroy cleanly and return to home

**Priority:** P0
**Effort:** M (1-3 days)
**Dependencies:** None

### 2.3 WiFi Configuration Screen (P0 / M / Deps: keyboard)

Currently WiFi SSID/password are compile-time `#define` in config.h. No runtime WiFi setup.

**Need:**
- SSID scan list (pull from ESP-Hosted C6 scan results)
- Tap SSID -> password entry using on-screen keyboard
- Save to NVS for persistence across reboots
- Show signal strength (RSSI bars)
- Current connection status + IP address display
- Forget/disconnect option

**Priority:** P0
**Effort:** M (1-3 days)
**Dependencies:** `ui_keyboard.c` (exists), NVS persistence (need to add)

### 2.4 Toast / Notification System (P1 / S / Deps: none)

`ui_camera.c` has a local toast implementation. This should be extracted into a global system usable by any screen.

**Need:**
- `ui_toast_show(text, duration_ms, color)` on `lv_layer_top()`
- Auto-dismiss with fade animation
- Queue multiple toasts (show sequentially, not overlap)

**Priority:** P1
**Effort:** S (< 1 day)
**Dependencies:** None

### 2.5 Date Formatting in Home Clock (P0 / S / Deps: none)

The date label on Page 0 is hardcoded `"Wednesday, March 26"`. Must derive from RTC.

**Need:**
- Day-of-week lookup table (RTC provides weekday 0-6)
- Month name lookup
- Format: "Wednesday, March 26" from `tab5_rtc_time_t`

**Priority:** P0
**Effort:** S (< 1 day, trivial)
**Dependencies:** None (RTC driver exists)

### 2.6 Brightness Persistence (P1 / S / Deps: none)

Brightness slider in `ui_settings.c` always starts at 80. Value not saved.

**Need:**
- Read current brightness from NVS on settings screen open
- Save to NVS on slider change (debounced)
- Apply saved brightness on boot

**Priority:** P1
**Effort:** S (< 1 day)
**Dependencies:** NVS (exists in firmware)

### 2.7 AI Chat Screen (P1 / L / Deps: keyboard, Dragon)

The "AI Chat" app icon exists but has no corresponding screen.

**Need:**
- Chat bubble UI (user messages right-aligned, AI left-aligned)
- Text input area with keyboard integration
- Send via Dragon link WebSocket to AI backend
- Scrollable message history
- Voice input button (integrate with voice overlay)

**Priority:** P1
**Effort:** L (3-7 days)
**Dependencies:** `ui_keyboard.c` (exists), Dragon link (exists), AI backend on Dragon

### 2.8 Lock Screen (P2 / M / Deps: none)

Page 0 looks like a lock screen but has no lock functionality.

**Need:**
- Screen lock after timeout (configurable)
- Swipe-to-unlock gesture
- Optional PIN/pattern (P3, much harder on embedded)
- Display-off / backlight-off after longer timeout

**Priority:** P2
**Effort:** M (1-3 days)
**Dependencies:** Backlight control (exists), sleep mode

### 2.9 Power Menu (P1 / S / Deps: none)

No way to shut down, restart, or sleep from the UI. Only serial `reboot` command.

**Need:**
- Long-press trigger (physical button or gesture)
- Modal overlay with: Restart, Power Off, Sleep
- Confirmation dialog for power off
- `esp_restart()` / `esp_deep_sleep_start()`

**Priority:** P1
**Effort:** S (< 1 day)
**Dependencies:** None

### 2.10 OTA Update UI (P2 / L / Deps: WiFi)

No over-the-air update mechanism.

**Need:**
- Check for update button in settings
- Download progress bar
- "Updating, don't power off" screen
- Rollback on failure
- ESP-IDF OTA partition scheme

**Priority:** P2
**Effort:** L (3-7 days) — OTA backend + partition table + UI
**Dependencies:** WiFi (exists), server infrastructure

### 2.11 Dragon Connection Status in Status Bar (P1 / S / Deps: none)

Status bar shows time + WiFi + battery. Dragon link state is only visible on Page 2/3.

**Need:**
- Small Dragon icon or indicator dot in status bar
- Color: cyan when streaming, dim when disconnected, amber when connecting

**Priority:** P1
**Effort:** S (< 1 day)
**Dependencies:** None (dragon_link API exists)

### 2.12 Battery Icon in Status Bar (P2 / S / Deps: none)

Status bar shows "85%" text but no battery icon. Standard mobile UX expects an icon.

**Need:**
- Battery outline icon with fill level
- Charging bolt overlay when charging
- Color: green >20%, yellow 10-20%, red <10%

**Priority:** P2
**Effort:** S (< 1 day) — LVGL symbol + dynamic text
**Dependencies:** None

### 2.13 First-Run / Onboarding Wizard (P2 / M / Deps: WiFi screen, keyboard)

No first-run experience. Device boots to home with hardcoded WiFi.

**Need:**
- Welcome screen with Glyph branding
- WiFi setup step
- Dragon discovery step
- Optional name/persona setup
- "Setup complete" transition to home
- Flag in NVS to skip on subsequent boots

**Priority:** P2
**Effort:** M (1-3 days)
**Dependencies:** WiFi config screen (2.3), NVS

### 2.14 Error / Crash Recovery Screen (P2 / S / Deps: none)

If a subsystem fails at boot, the splash shows "Hardware OK" regardless. No error state handling in UI.

**Need:**
- If critical init fails (display, touch): show error on serial
- If non-critical fails (WiFi, camera): show degraded-mode banner on home screen
- Watchdog recovery: boot into safe mode with minimal UI
- Error log viewer in settings

**Priority:** P2
**Effort:** S (< 1 day)
**Dependencies:** None

### 2.15 Gallery / Photo Viewer (P2 / M / Deps: files, camera)

Camera screen has a "Gallery" button that does nothing. File browser can show image preview but it's basic.

**Need:**
- Grid view of captured photos (thumbnails)
- Full-screen photo viewer with swipe navigation
- Delete photo option
- Share via Dragon link

**Priority:** P2
**Effort:** M (1-3 days)
**Dependencies:** SD card (exists), camera (exists)

### 2.16 Volume Controls (Global) (P1 / S / Deps: none)

Volume is only adjustable inside the audio player overlay. No global volume control.

**Need:**
- Volume slider in settings
- Quick-volume overlay (triggered by hardware button or status bar tap)
- Persist volume in NVS

**Priority:** P1
**Effort:** S (< 1 day)
**Dependencies:** Audio driver (exists)

### 2.17 Browser Controls (P3 / XL / Deps: Dragon)

No web browser on the device. Dragon streaming is the remote-browser approach.

**Need (if standalone browsing desired):**
- Address bar + keyboard integration
- Back/forward/reload buttons
- Bookmarks
- This would require a web rendering engine — likely not feasible on ESP32-P4

**If Dragon-based browser remote:**
- URL bar that sends to Dragon
- Bookmark list synced from Dragon
- Browser-specific touch gestures forwarded via WebSocket

**Priority:** P3
**Effort:** XL (> 1 week) for standalone, L (3-7 days) for Dragon remote controls
**Dependencies:** Dragon link (exists)

### 2.18 Auto-Rotate Implementation (P2 / M / Deps: IMU)

Settings has an auto-rotate switch that reads IMU orientation but never rotates the display.

**Need:**
- `lv_display_set_rotation()` call when IMU orientation changes
- Coordinate transform for touch input
- Only rotate when switch is enabled
- Debounce orientation changes (avoid flicker)

**Priority:** P2
**Effort:** M (1-3 days)
**Dependencies:** IMU driver (exists), display driver modifications

### 2.19 Bluetooth UI (P3 / L / Deps: ESP-Hosted BLE support)

Settings shows "ESP-Hosted pending" for Bluetooth. No BLE UI.

**Need:**
- BLE scan results list
- Pair/unpair controls
- Connected devices list
- Blocked until ESP-Hosted adds BLE pass-through support

**Priority:** P3
**Effort:** L (3-7 days) once BLE driver exists
**Dependencies:** ESP-Hosted BLE support (NOT yet available)

### 2.20 Audio Player Seek Bar (P2 / S / Deps: none)

Audio player has play/pause and volume but no progress/seek.

**Need:**
- Progress bar showing current position / total duration
- Tap-to-seek on progress bar
- Elapsed / remaining time labels

**Priority:** P2
**Effort:** S (< 1 day)
**Dependencies:** None (file size + byte position already tracked in playback task)

---

## Section 3: Priority Summary

### P0 — Essential for Basic Usability
| # | Item | Effort |
|---|------|--------|
| 2.1 | App grid click handlers | S |
| 2.2 | Navigation back stack | M |
| 2.3 | WiFi configuration screen | M |
| 2.5 | Dynamic date on home page | S |

### P1 — Needed for Demo
| # | Item | Effort |
|---|------|--------|
| 2.4 | Global toast system | S |
| 2.6 | Brightness persistence | S |
| 2.7 | AI Chat screen | L |
| 2.9 | Power menu | S |
| 2.11 | Dragon indicator in status bar | S |
| 2.16 | Global volume controls | S |

### P2 — Polish
| # | Item | Effort |
|---|------|--------|
| 2.8 | Lock screen | M |
| 2.10 | OTA update UI | L |
| 2.12 | Battery icon in status bar | S |
| 2.13 | First-run onboarding | M |
| 2.14 | Error recovery screen | S |
| 2.15 | Photo gallery | M |
| 2.18 | Auto-rotate implementation | M |
| 2.20 | Audio seek bar | S |

### P3 — Nice-to-Have
| # | Item | Effort |
|---|------|--------|
| 2.17 | Browser controls | XL |
| 2.19 | Bluetooth UI | L |

---

## Section 4: Critical Finding Summary

1. **The app grid is completely non-interactive.** This is the single biggest UX gap — 8 beautiful icons that do nothing when tapped. Five of the eight target screens already exist in the codebase but are unreachable.

2. **Three complete screens are orphaned:** `ui_settings.c`, `ui_camera.c`, and `ui_files.c` are fully built and functional but have zero entry points from the UI. They can only be invoked programmatically.

3. **The keyboard and voice overlays work but have no consumer.** The keyboard has no textarea to type into. Voice overlay works but only if the Dragon STT/TTS pipeline is online.

4. **Home page date is hardcoded.** Trivial fix but embarrassing in a demo.

5. **No WiFi runtime configuration.** SSID and password are baked into the firmware at compile time. First-time setup or network change requires reflashing.
