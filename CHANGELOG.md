# Changelog

## v0.8.0 (April 15, 2026) — Stability + Polish Sprint

### Security
- Bearer token auth on debug server (20 protected, 2 public endpoints)
- WiFi credentials removed from source (NVS auto-seed pattern)
- SSH credentials redacted from documentation
- OTA SHA256 firmware verification

### Stability (Critical)
- Generation counter on all voice→LVGL async calls (prevents use-after-free)
- I2S channel disable before mic task deletion (prevents ISR crash)
- Mode switch swapping guard + audio buffer clear
- Per-connection deep copy config (no shared mutable state)
- SQLite WAL corruption auto-recovery on startup
- WS mutex bounded timeouts (no more portMAX_DELAY)
- PSRAM heap fragmentation watchdog (reboot after 3min sustained)

### UI/UX
- Orb ring color per voice mode (green/yellow/blue/rose)
- Keyboard text input visible while typing (was hidden behind keyboard)
- Home screen layout rebalanced (orb moved up, labels bigger)
- Chat bubbles: 500px max width, readable timestamps
- Model label: "Local AI" instead of "qwen3:1.7b"
- Quick action pre-fill instead of auto-send
- Suggestion cards tappable and taller
- Settings: "USB Powered" / "119 GB available" / version prominent
- Notes: compact buttons, proper touch targets
- Voice overlay auto-hide scales with response length
- OTA confirmation dialog + progress indicator
- Swipe-back restricted to left edge
- Connection dropdown: "Automatic" / "Home Network" / "Internet Only"
- Mode rejection toast shows Dragon error
- Dictation countdown "Stopping in 2/1..."
- Volume slider preview tone
- 50-message "Older messages archived" notification

### Architecture
- WS reconnect exponential backoff 2s→60s with jitter
- Keepalive degraded state (3 missed pongs before disconnect)
- Session resume after Dragon crash
- Notes sync retry on reconnect
- Navigate API 500ms debounce + re-entry guard
- Touch input blocked during Settings creation

### Pipeline
- SSE truncation detection (no [DONE] marker)
- TinkerClaw tool-call timeout 30→90s
- Context window overflow protection (token estimation + trim)
- Piper subprocess tracking + kill on cancel
- Mode-aware clause flush (20 chars local, 60 chars cloud)
- SSE parser 5-error circuit breaker
- Dictation buffer capped at 9.6MB
- Voice/text mutual exclusion
- OpenRouter 429 retry with Retry-After

### Dragon Server
- Message retention purge (30-day configurable)
- Dead WS connection detection (30s timeout)
- Ollama keep_alive=30s (prevents OOM on model swap)
- Ollama shutdown 5s timeout with force-close
- Memory monitor (RSS every 5min, GC at 2GB)
- FD count monitoring
- Dedicated inference ThreadPoolExecutor
- eMMC WAL: synchronous=NORMAL + passive checkpoint
- systemd service ordering files
- Stale session force-close on device reconnect

### Hardware
- NVS slider debounce (500ms, saves flash wear)
- NVS write counter for wear monitoring
- Internal heap selftest threshold 50KB→30KB
- INA226 I2C address corrected (0x41, not 0x40)

## v1.0.0 — 2026-04-01

### Fixed
- BMI270 IMU init: add 10ms settle delay + 5x retry loop after soft reset to handle transient `ESP_ERR_INVALID_STATE` on chip-ID read (`main/imu.c`)

### Changed
- `CLAUDE.md`: updated ESP-IDF to v5.5.2, added `idf.py set-target esp32p4` to build commands, added ROM download mode watchdog reset recovery, added Recovery & Rollback section, removed SIM-FIRST mandatory workflow
- `LEARNINGS.md`: added 4 entries (managed_components hash error, sdkconfig esp32p4 target, ROM download mode boot, BMI270 readiness race)

### Added
- GitHub Actions CI: `build.yml` (ESP-IDF v5.5.2, esp32p4, clean build)
- GitHub Actions CI: `clang-format.yml` (format check on PRs)
- `.clang-format` (ESP-IDF-compatible Google style)
- `CHANGELOG.md`
- `docs/STASH_INDEX.md` (index of preserved stashes)

### Removed
- `stitch-designs/` from git tracking (local copy preserved, added to `.gitignore`)
- `dependencies.lock` from `.gitignore` (should be tracked)
