# TinkerTab Development Guidelines

## Issue-Driven Development Workflow

Every bug fix and feature MUST follow this workflow:

1. **Create GitHub issue first** — describe the bug/problem, symptoms, affected files
2. **Fix it** — make minimal, targeted changes
3. **Commit referencing the issue** — e.g. `fix: voice crash on WS send (closes #1)`
4. **Push immediately** — so we always have a working state to revert to
5. **Close the issue** — with root cause, culprit, why it happened, and how it was fixed

### Issue Format
```
## Bug / Problem
What the user sees. Symptoms, frequency, impact.

## Root Cause
Technical explanation of WHY this happens.

## Culprit
Exact file(s) and line(s) responsible.

## Fix
What was changed and why this fixes it.

## Resolved
Commit hash + PR if applicable.
```

### Commit Messages
- Reference issues: `fix: description (closes #N)` or `feat: description (refs #N)`
- Keep commits atomic — one fix per commit, push after each
- Never batch multiple unrelated fixes into one commit

## Build & Flash
```bash
source ~/esp/esp-idf-v5.4.3/export.sh
idf.py build
idf.py -p /dev/ttyACM0 flash
```

## Debug Server
- Screenshots: `curl -s -o screen.bmp http://192.168.1.90:8080/screenshot`
- Device info: `curl -s http://192.168.1.90:8080/info | python3 -m json.tool`
- Touch inject: `curl -s -X POST http://192.168.1.90:8080/touch -d '{"x":360,"y":640,"action":"tap"}'`

## Key Technical Notes
- ESP-IDF WS transport masks frames in-place — NEVER pass string literals to `esp_transport_ws_send_raw()`, always copy to mutable buffer first
- LVGL objects must not be accessed from background tasks after screen destroy — use `volatile bool s_destroying` guards
- All FreeRTOS tasks doing network + LVGL callbacks need minimum 8KB stack on ESP32-P4
- Screenshot handler must copy framebuffer under LVGL lock, then stream without lock
- `lv_screen_load_anim()` with auto_delete=false when returning to existing home screen

## Dragon (192.168.1.89)
```bash
sshpass -p 'rock' ssh rock@192.168.1.89
echo rock | sudo -S systemctl status tinkerclaw-voice
echo rock | sudo -S journalctl -u tinkerclaw-voice --no-pager -n 50
```
