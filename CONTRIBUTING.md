# Contributing to TinkerTab

> Tab5 firmware contributions.  Same workflow rules as TinkerBox
> (the Dragon-side server) — those are documented in
> [TinkerBox's CONTRIBUTING.md](https://github.com/lorcan35/TinkerBox/blob/main/CONTRIBUTING.md).
> This file covers the firmware-specific bits.

## TL;DR

- **Issue first** — `gh issue create` before opening a PR.
- **Branch from `main`**: `feat/<slug>` / `fix/<slug>` / `chore/<slug>` / `docs/<slug>` / `investigate/<slug>`.
- **Conventional-commit prefix** + reference the issue (`refs #N` or `closes #N`).
- **Squash-merge.** Delete the branch after.
- **Build + flash + verify** before pushing — CI doesn't run on hardware.
- **Cross-stack changes** start in TinkerBox if they extend the protocol; Tab5 firmware can ignore unknown fields per protocol's forward-compat design.

## First time? Read these in order

1. [`README.md`](README.md) and [TinkerBox `WELCOME.md`](https://github.com/lorcan35/TinkerBox/blob/main/WELCOME.md)
2. [`docs/dev-setup.md`](docs/dev-setup.md) — get ESP-IDF v5.5.2 installed + your USB-C connected Tab5 building + flashing
3. [`docs/HARDWARE.md`](docs/HARDWARE.md) — what's on the board
4. [TinkerBox `docs/ARCHITECTURE.md`](https://github.com/lorcan35/TinkerBox/blob/main/docs/ARCHITECTURE.md) — how the firmware fits into the bigger picture
5. [`LEARNINGS.md`](LEARNINGS.md) — search before you debug

## Workflow

Identical to TinkerBox.  See
[TinkerBox CONTRIBUTING.md sections 1-6](https://github.com/lorcan35/TinkerBox/blob/main/CONTRIBUTING.md#1-open-an-issue-first).

The bug-report issue format from CLAUDE.md (Bug / Root Cause /
Culprit / Fix / Resolved) applies here too.

## Firmware-specific gotchas

### Always run `idf.py set-target esp32p4` after a branch change

ESP-IDF caches the target; switching branches that touch
`sdkconfig.defaults` requires a fresh setup.

### `idf.py fullclean build` after sdkconfig changes

Incremental builds cache stale config.  When in doubt, fullclean.

### Watchdog reset, not hard reset, after flash

ESP32-P4's USB-JTAG doesn't wire RTS to EN.  After a normal
`idf.py flash`, the board sometimes lands in ROM-download mode
instead of booting the new app:

```bash
python -m esptool --chip esp32p4 -p /dev/ttyACM0 \
    --before no_reset --after watchdog_reset read_mac
```

### Always use `tab5_lv_async_call`, not `lv_async_call`

The LVGL primitive is **not thread-safe** (does `lv_malloc` +
`lv_timer_create` against unprotected TLSF — caused the long-residual
stability class closed in PRs #257/#259).  We have a wrapper in
[`main/ui_core.{c,h}`](main/ui_core.h) that takes the LVGL recursive
mutex first.

If you find a new `lv_async_call(` call site outside `tab5_lv_async_call`'s
implementation, it's a bug.  PR will be rejected.

### LVGL config goes in `sdkconfig.defaults`, NOT `lv_conf.h`

The ESP-IDF LVGL component sets `CONFIG_LV_CONF_SKIP=1` so
`lv_conf.h` is **completely ignored**.  Any change to it has zero
effect.  Always verify with `grep "SETTING" build/config/sdkconfig.h`
after building.

### NVS keys are 15 chars max

Key names longer than 15 characters silently truncate or fail.
We've hit this — added it to LEARNINGS.

### LFN is disabled, FATFS does 8.3 short names

`CONFIG_FATFS_LFN_NONE=1`.  Files on SD must fit 8 chars + 3-char
extension.  This is why video recordings use `.MJP`, not `.mjpeg`.
See LEARNINGS for the full story.

### Test on real hardware

CI doesn't run on Tab5.  Before opening a PR with firmware changes:
1. `idf.py build` — must pass cleanly (no warnings treated as errors)
2. `idf.py -p /dev/ttyACM0 flash` — must succeed
3. Boot Tab5, watch the serial log for crashes / panics
4. Ideally: run the e2e harness ([`tests/e2e/runner.py`](tests/e2e/README.md))

### E2E harness for full-flow regression

```bash
export TAB5_TOKEN=<auth_tok-from-NVS>
python3 tests/e2e/runner.py story_smoke    # ~2 min nav + voice
python3 tests/e2e/runner.py story_full     # ~2 min all modes + camera + REC
python3 tests/e2e/runner.py story_stress   # ~10 min mode rotation × cycles
python3 tests/e2e/runner.py all --reboot   # all three with clean reboot
```

Reports + screenshots in `tests/e2e/runs/<scenario>-<ts>/`.  See
[`tests/e2e/README.md`](tests/e2e/README.md) for details.

### LEARNINGS.md is mandatory for non-obvious fixes

Every bug fix with a non-obvious root cause adds a dated entry:

```markdown
### NN. [Short Title]
- **Date:** YYYY-MM-DD
- **Symptom:** What was observed
- **Root Cause:** Why it happened
- **Fix:** What was done
- **Prevention:** How to avoid it in the future
```

Skip only if the fix is genuinely one-line and self-explaining.

## Specific contribution recipes

| I want to… | Read this |
|------------|-----------|
| Add a new debug-server endpoint | Look at the pattern in [`main/debug_server.c`](main/debug_server.c) and add a row to the CLAUDE.md "Debug Server" table |
| Add a new screen (LVGL) | Existing examples: `ui_camera.c`, `ui_settings.c`, `ui_files.c`, `ui_chat.c`.  Use the hide/show pattern, not destroy/recreate (avoids fragmentation) |
| Add a new NVS setting | Add getter/setter in [`main/settings.{c,h}`](main/settings.h) + table row in CLAUDE.md "NVS Settings Keys" |
| Wire a new obs event | Call `tab5_debug_obs_event(kind, detail)` at the relevant site.  Update the table in CLAUDE.md "Observability events" |
| Add a hardware peripheral | Update [`bsp/tab5/bsp_config.h`](bsp/tab5/bsp_config.h) with pins + I2C address; document in [`docs/HARDWARE.md`](docs/HARDWARE.md) |
| Wire something to the chat overlay | See `chat_msg_view.c` for renderer entry points; rich-media types need both Tab5 and Dragon coordination |
| Add a new voice mode | This is a cross-stack change — start with TinkerBox's `pipeline.py` swap_backends and the protocol.md reference for voice_mode field; Tab5 follows |

## Cross-stack changes

When a feature touches both TinkerBox (Dragon, Python) and TinkerTab
(Tab5, C):

1. Open an issue in **whichever repo holds the harder side** of the change.
2. Cross-link the other repo's issue.
3. PR in TinkerBox first if Dragon needs a new field — Tab5 firmware can ignore unknown fields per the protocol's forward-compat design.
4. PR in TinkerTab next, depending on the merged TinkerBox change.
5. Update [TinkerBox `docs/protocol.md`](https://github.com/lorcan35/TinkerBox/blob/main/docs/protocol.md) **in the same PR as the protocol-changing side**, never as a follow-up.

Recent example: PR #186 (TinkerBox) added `fleet_summary` to
`session_start.config`.  TinkerTab firmware ignored the new field
gracefully; a future Tab5 firmware PR can read it to dynamically
light up capability chips.

## License

By contributing, you agree your contributions are licensed under the
same license as the project (see [`LICENSE`](LICENSE)).
