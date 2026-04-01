# Changelog

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
