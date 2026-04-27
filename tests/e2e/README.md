# Tab5 e2e User-Story Harness

Python-driven scenario runner that exercises Tab5 firmware end-to-end via the
debug HTTP server (`/touch`, `/navigate`, `/chat`, `/screen`, `/events`,
`/screenshot.jpg`, etc.).  Built for issues #293/#294.

## Quick start

```bash
cd ~/projects/TinkerTab
export TAB5_URL=http://192.168.1.90:8080
export TAB5_TOKEN=<bearer-token-from-NVS>

# Single scenario
python3 tests/e2e/runner.py story_smoke

# All three (about 35 minutes total)
python3 tests/e2e/runner.py all --reboot
```

Each run lands in `tests/e2e/runs/<scenario>-<timestamp>/`:
- `report.json` — machine-readable pass/fail per step + events captured
- `report.md`   — human-readable summary table with screenshots inline
- `NN_<step>.jpg` — per-step screenshot

## Scenarios

| Name | Duration | Coverage |
|------|----------|----------|
| `story_smoke`  | ~5 min  | Boot → mode set → home → text chat → camera screen → settings round-trip |
| `story_full`   | ~10 min | All four voice modes + Local text turn + photo capture + REC start/stop + Cloud text turn |
| `story_stress` | ~20 min | 6 cycles of (mode rotation × screen rotation × text chat) with heap watchdog assertions |

## Adding a scenario

Add a `def story_my_thing(r: Runner) -> None:` to `runner.py` and register it
in `SCENARIOS`.  Inside, call `r.step("name", lambda t: …)` — each step gets
a timestamp, a screenshot, and the events that fired during it.  Failed
assertions don't abort the run (we want to know how far the firmware gets).

`r.soft_step` is for diagnostic reads where failure shouldn't be counted hard.

## Driver primitives

The `Tab5Driver` class wraps the debug server.  Common building blocks:
- `tab5.tap(x, y)` / `long_press(x, y, ms)` / `swipe(x1,y1,x2,y2)`
- `tab5.navigate("camera")` → returns `{"navigated":"camera"}`
- `tab5.screen()` → `{"current":"home","overlays":{...}}`
- `tab5.chat("text")` → text turn through Dragon
- `tab5.mode(0|1|2|3, model="…")` → swap voice mode
- `tab5.await_event("chat.llm_done", timeout_s=120)`
- `tab5.await_voice_state("READY", 30)`
- `tab5.await_screen("camera", 5)`
- `tab5.screenshot("/path/to/file.jpg")`
- `tab5.heap()` / `tab5.metrics()` / `tab5.voice_state()`

See `driver.py` for the full surface.
