# TinkerTab E2E Test Harness

Touch-based regression tests driven through the debug server. Tests simulate user
interactions (tap, long-press, swipe) and verify outcomes via screenshots and
state endpoints — not against internal implementation.

## Setup

```bash
pip install -r test/e2e/requirements.txt
```

Environment variables (defaults in parens):

- `TAB5_HOST` (192.168.70.128) — device IP
- `TAB5_PORT` (8080) — debug server port
- `TAB5_TOKEN` — bearer token; if unset, pulled from serial via `token` command
- `TAB5_SERIAL` (/dev/ttyACM0) — serial device for token / state inspection
- `DRAGON_HOST` (192.168.70.242) — Dragon IP (for cross-device tests)

## Run

```bash
cd test/e2e
pytest                    # all tests
pytest -m baseline        # just the sanity tests against current firmware
pytest -m ui              # UI tests (requires UI overhaul features)
pytest -k orb             # tests mentioning "orb"
pytest -s                 # show print output
```

Artifacts (screenshots, state dumps) land in `test/e2e/artifacts/<test-name>/`.

## Writing tests

```python
def test_tap_orb_opens_voice(tab5, home_screen):
    tab5.tap(ORB_CX, ORB_CY)
    tab5.wait_for_voice_state("LISTENING", timeout_s=5)
    shot = tab5.screenshot()
    assert_amber_present_near(shot, ORB_CX, ORB_CY, radius=120)
```

See `lib/tab5.py` for the `Tab5Client` surface and `lib/touch.py` for coordinate constants.

## Reference

`E2E_TEST_SPECIFICATION.md` holds 150 hand-written tests across 30 user stories.
This harness implements them incrementally as phases land.
