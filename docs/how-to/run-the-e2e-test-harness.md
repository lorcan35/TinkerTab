---
audience: developer
type: how-to
prerequisites: A flashed [Tab5](../../GLOSSARY.md) on Wi-Fi with the [debug server](../reference/debug-server.md) reachable, Python 3, the bearer token
last-verified: 2026-05-29
est-time: 20 min
---
# How to run the e2e test harness

Use this when you want to drive a [Tab5](../../GLOSSARY.md) through long
user-story flows automatically — for regression testing after a change, or to
reproduce a bug the harness has historically caught. The harness is a
Python-driven scenario runner in [`tests/e2e/`](../../tests/e2e/) that exercises
the firmware end-to-end through the [debug server](../reference/debug-server.md)
HTTP API (`/touch`, `/navigate`, `/chat`, `/screen`, `/events`,
`/screenshot.jpg`).

Assumes the firmware is flashed, the Tab5 is on Wi-Fi, you know its IP, and you
have the bearer token (printed to the serial boot log).

## Steps

1. **Point the harness at the device and authenticate.**
   ```bash
   cd ~/projects/TinkerTab
   export TAB5_URL=http://192.168.1.90:8080      # your Tab5 IP
   export TAB5_TOKEN=<bearer-token-from-NVS>
   ```
2. **Run a single scenario.**
   ```bash
   python3 tests/e2e/runner.py story_smoke
   ```
3. **Or run the full suite with a clean reboot first.**
   ```bash
   python3 tests/e2e/runner.py all --reboot
   ```

The scenarios:

| Name | Duration | Coverage |
|---|---|---|
| `story_smoke` | ~5 min | Boot → mode set → home → text chat → camera → settings round-trip. |
| `story_full` | ~10 min | All voice modes + Local text turn + photo capture + REC start/stop + Cloud text turn. |
| `story_stress` | ~20 min | 6 cycles of (mode × screen × chat) with heap-watchdog assertions. |
| `story_onboard` | ~30 s | vmode=4 K144 chain lifecycle. Skips if `/m5` reports `failover_state != READY`. |

## Verify it worked

Each run lands in `tests/e2e/runs/<scenario>-<timestamp>/` (gitignored):

- `report.json` — machine-readable pass/fail per step + events captured.
- `report.md` — human-readable table with inline screenshots.
- `NN_<step>.jpg` — a screenshot per step.

Open `report.md` and confirm the per-step pass count. The last full validation
baseline was **smoke 14/14, full 24/24, stress 76/77** (a single LLM-timeout
flake from Q6A latency, not a regression).

## Adding a scenario

Add a `def story_my_thing(r: Runner) -> None:` to `runner.py` and register it in
`SCENARIOS`. Inside, call `r.step("name", lambda t: …)` — each step gets a
timestamp, a screenshot, and the events that fired during it. Use `r.soft_step`
for diagnostic reads where failure should not count as a hard fail. Failed
assertions do **not** abort the run (you want to know how far the firmware
gets). The `Tab5Driver` in [`tests/e2e/driver.py`](../../tests/e2e/driver.py)
wraps the debug API: `tap`, `long_press`, `swipe`, `navigate`, `screen`, `chat`,
`mode`, `await_event`, `await_voice_state`, `await_screen`, `screenshot`,
`heap`, `metrics`.

## Troubleshooting

- **`Connection refused` / no response** → Wrong `TAB5_URL` or the Tab5 is not
  on Wi-Fi. Confirm with `curl http://<ip>:8080/info` (no auth needed).
- **Every authed step 401s** → Wrong or stale `TAB5_TOKEN`. The token changes
  only if NVS is wiped; re-read it from the serial boot log.
- **`story_onboard` skips immediately** → Expected when the K144 chain is not
  READY (`/m5` reports `failover_state != READY`). Recover the chain first (see
  [Recover a stuck device](recover-a-stuck-device.md)).
- **`await_event` misses events that clearly fired** → A historic cursor-stealing
  bug (a diagnostic snapshot advanced `_last_event_ms`); the fix uses
  `events(peek=True)`. If you see it again in a new scenario, prefer
  `await_voice_state` (which polls `/voice` current state too) over raw
  `await_event`.
- **A step times out waiting for an LLM reply** → Local mode (`vmode=0`) is slow
  on the Q6A (~60–90 s/turn). Use a faster mode for the run, or raise the
  await timeout.

## See also

- [Debug server reference](../reference/debug-server.md) — the API the harness
  drives.
- [Observability events reference](../reference/observability-events.md) — the
  event kinds `await_event` polls.
- [The voice pipeline](../explanation/the-voice-pipeline.md) — what the voice
  scenarios are exercising.
