# TinkerTab

TinkerOS firmware for the M5Stack Tab5 (ESP32-P4). Thin client — connects to a Dragon Q6A server over WebSocket for STT, LLM, and TTS.

## Quick Start

```bash
# Clone
git clone https://github.com/lorcan35/TinkerTab.git
cd TinkerTab

# Install ESP-IDF v5.5.2
curl -L -o esp-idf.zip https://github.com/espressif/esp-idf/releases/download/v5.5.2/esp-idf-v5.5.2.zip
unzip -q esp-idf.zip -d ~/
rm esp-idf.zip
. ~/esp-idf-v5.5.2/install.sh && . ~/esp-idf-v5.5.2/export.sh

# Build
idf.py set-target esp32p4
idf.py build

# Flash
idf.py -p /dev/ttyACM0 flash

# Monitor
idf.py -p /dev/ttyACM0 monitor
```

If the board enters ROM download mode after flashing (common on ESP32-P4), recover with:
```bash
python -m esptool --chip esp32p4 -p /dev/ttyACM0 --before no_reset --after watchdog_reset read_mac
```

## Architecture

```
Tab5 (ESP32-P4)  <--WebSocket-->  Dragon Q6A (TinkerBox)
  LVGL UI                            STT (Moonshine ONNX)
  mic + speaker                       LLM (Llama on NPU)
  camera + SD                         TTS (Piper)
  sensors (IMU, RTC, battery)          sessions + memory
```

## Repo Structure

```
main/           — firmware source (.c, .h)
components/     — ESP-IDF components
sim/            — SDL2 desktop simulator (Linux)
docs/           — protocol spec, build plan
CLAUDE.md       — developer guide
LEARNINGS.md    — hard-won lessons (read before debugging)
```

## Documentation

| File | Purpose |
|------|---------|
| [CLAUDE.md](CLAUDE.md) | Developer guide, workflow, build commands |
| [LEARNINGS.md](LEARNINGS.md) | Bug database: symptom, root cause, fix per issue |
| TinkerBox/docs/protocol.md | WebSocket protocol contract |

## Build Requirements

- ESP-IDF v5.5.2 (pinned in `dependencies.lock`)
- Target: ESP32-P4
- Python 3.11+
- ~4GB disk for toolchain + build

## Contributing

1. Open an issue before starting work
2. Branch from `main`
3. Commit with issue reference: `fix: description (closes #N)`
4. Open a PR — CI runs build + format checks

## License

MIT
