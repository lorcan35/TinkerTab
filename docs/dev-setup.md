# Tab5 Firmware Dev Environment

> Get from "I cloned TinkerTab" to "I can build + flash + iterate on
> the firmware."  Estimated time: 15-20 minutes the first time.
>
> If you also need Dragon-side dev setup, that's in
> [TinkerBox `docs/dev-setup.md`](https://github.com/lorcan35/TinkerBox/blob/main/docs/dev-setup.md).

## Prerequisites

| Requirement | Why |
|-------------|-----|
| **Linux or macOS workstation** | ESP-IDF works best on Linux; macOS is supported but trickier |
| **USB-C cable that supports data** (not just charging) | To flash + monitor Tab5 |
| **M5Stack Tab5 hardware** | The thing you're building firmware for |
| **Python 3.10+** | ESP-IDF tooling + the e2e harness |
| **Git + GitHub CLI (`gh`)** | Branching, PRs |

If you don't have hardware, you can still validate that the
firmware *builds* — useful for review work.

## 1. Clone

```bash
git clone https://github.com/lorcan35/TinkerTab.git ~/projects/TinkerTab
cd ~/projects/TinkerTab
```

## 2. Install ESP-IDF v5.5.2

The firmware is **pinned** to ESP-IDF v5.5.2 (per `dependencies.lock`).  Don't use a different version.

```bash
mkdir -p ~/esp
cd ~/esp
git clone -b v5.5.2 --recursive https://github.com/espressif/esp-idf.git

cd ~/esp/esp-idf
./install.sh esp32p4
```

The installer downloads ~2 GB of toolchains.  Takes 5-10 minutes.

Source the env script in every shell that builds firmware:

```bash
source ~/esp/esp-idf/export.sh
idf.py --version
# → ESP-IDF v5.5.2
```

> **Tip:** Add an alias to your shell rc:
>
> ```bash
> # in ~/.bashrc or ~/.zshrc
> alias idfenv='. ~/esp/esp-idf/export.sh'
> ```

## 3. Set the target

```bash
cd ~/projects/TinkerTab
idf.py set-target esp32p4
```

This generates `sdkconfig` from `sdkconfig.defaults`.  Re-run after
every branch change that touches sdkconfig defaults.

## 4. Configure WiFi + Dragon connection

Two ways:

### Option A — interactive menuconfig

```bash
idf.py menuconfig
```

Navigate to:
- **TinkerClaw → WiFi credentials** — set SSID + password
- **TinkerClaw → Dragon host/port** — defaults to 192.168.1.91:3502
- **TinkerClaw → Dragon API token** — paste your `DRAGON_API_TOKEN` value

Save (S) and exit (Q).

### Option B — non-interactive `sdkconfig.local`

Create `sdkconfig.local` (gitignored) at repo root:

```text
CONFIG_TAB5_WIFI_SSID="MyHomeWifi"
CONFIG_TAB5_WIFI_PASS="hunter2"
CONFIG_TAB5_DRAGON_HOST="192.168.1.91"
CONFIG_TAB5_DRAGON_PORT=3502
CONFIG_TAB5_DRAGON_TOKEN="40e6ba82b22a40a5094482e695dc6919997611b5e5b9eb03c5e5a005be97b1c7"
```

ESP-IDF reads `sdkconfig.local` on top of `sdkconfig.defaults`.

## 5. Build

```bash
idf.py build
```

First build takes 5-10 minutes (compiling LVGL, esp-hosted, esp-video, esp-codec-dev, etc.).  Subsequent incrementals are 30-90 seconds.

Build outputs land in `build/`.  The flashable artefact is
`build/tinkertab.bin`.

If you change `sdkconfig.defaults` or pull new components:

```bash
idf.py fullclean build
```

## 6. USB permissions on Linux

Plug Tab5 into a USB-C port.  It enumerates as `/dev/ttyACM0`.

If you get `Permission denied: '/dev/ttyACM0'`:

```bash
sudo usermod -a -G dialout $USER
# Log out and back in for the group change to take effect.
```

On macOS the device is `/dev/tty.usbmodem*` — adjust `-p` flags accordingly.

## 7. Flash

```bash
idf.py -p /dev/ttyACM0 flash
```

If the board enters ROM-download mode after flashing instead of
booting the new app (common on ESP32-P4 — USB-JTAG doesn't wire RTS
to EN), trigger a watchdog reset:

```bash
python -m esptool --chip esp32p4 -p /dev/ttyACM0 \
    --before no_reset --after watchdog_reset read_mac
```

This is annoying enough that we recommend a wrapper script:

```bash
# ~/bin/flashtab5
#!/bin/bash
set -e
. ~/esp/esp-idf/export.sh
cd ~/projects/TinkerTab
idf.py -p /dev/ttyACM0 flash
python -m esptool --chip esp32p4 -p /dev/ttyACM0 \
    --before no_reset --after watchdog_reset read_mac
```

## 8. Monitor serial output

Two options:

### Option A — `idf.py monitor`

```bash
idf.py monitor
# Ctrl+] to exit
```

### Option B — Python serial reader

```bash
python3 -c "
import serial, time
s = serial.Serial('/dev/ttyACM0', 115200, timeout=5)
time.sleep(0.3)
s.write(b'\r')
while True:
    if s.in_waiting:
        print(s.read(s.in_waiting).decode('utf-8', errors='replace'),
              end='', flush=True)
"
```

(Useful when you don't want `idf.py`'s extra output / its panic-handler
features.)

## 9. First-boot Tab5 verification

After flash, on a phone or laptop on the same WiFi:

```bash
curl -s http://<tab5-ip>:8080/info | python3 -m json.tool
```

Should show `voice_connected: true` if Dragon is reachable.

The auth token for the debug server is on the serial log (look for `auth token:`) — masked but the first/last 4 chars confirm which token is active.  Full token recoverable via `esptool read_flash 0x9000 0x6000` if needed (see [`SECURITY.md`](../SECURITY.md)).

## 10. E2E harness

Once flashed + reachable:

```bash
export TAB5_URL=http://<tab5-ip>:8080
export TAB5_TOKEN=<auth_tok-from-NVS>

# Smoke run (~2 min)
python3 tests/e2e/runner.py story_smoke

# Full feature suite (~2 min)
python3 tests/e2e/runner.py story_full

# 10-minute stress
python3 tests/e2e/runner.py story_stress

# All three with a clean reboot
python3 tests/e2e/runner.py all --reboot
```

Reports + per-step screenshots land in `tests/e2e/runs/<scenario>-<ts>/`.  See [`tests/e2e/README.md`](../tests/e2e/README.md) for details.

## 11. Iteration loops

### Edit + flash + monitor

```bash
. ~/esp/esp-idf/export.sh
idf.py build && idf.py -p /dev/ttyACM0 flash
python -m esptool --chip esp32p4 -p /dev/ttyACM0 \
    --before no_reset --after watchdog_reset read_mac
sleep 22  # give it boot time
curl -s http://<tab5-ip>:8080/info | python3 -m json.tool
```

### Test a UI change without flashing

The desktop SDL2 simulator in `sim/` (work-in-progress) lets you run
the LVGL UI on a workstation.  Status: Phase 2 priority per
TinkerClaw vision; not ready for daily use yet.

For now: flash to real hardware.

### Cross-stack change (touches Dragon too)

See [`CONTRIBUTING.md`](../CONTRIBUTING.md) "Cross-stack changes" — start in TinkerBox first.

## Common pitfalls

### "Cannot find ESP-IDF environment"

You forgot to `source ~/esp/esp-idf/export.sh`.

### `idf.py build` fails after `git checkout`

```bash
idf.py set-target esp32p4
idf.py fullclean
idf.py build
```

### "Failed to connect to ESP32-P4: No serial data received"

Tab5 is in a weird state.  Try:
1. Unplug + replug USB-C (5 second pause)
2. `python -m esptool --chip esp32p4 -p /dev/ttyACM0 --before default_reset chip_id` — should succeed; if not, the cable might be charge-only

### Build error "component not found: esp_video"

Dependency cache stale:

```bash
idf.py fullclean
rm -rf managed_components
idf.py build
```

### Flash fills up

The 16 MB partition table has dual OTA slots (3 MB each) + NVS + spiffs.  If you hit a full partition during flash:

```bash
idf.py -p /dev/ttyACM0 erase-flash flash
```

This wipes everything (including saved WiFi creds) and starts fresh.

### Tab5 won't connect to Dragon after flash

Check `dragon_host` in NVS:

```bash
curl -s -H "Authorization: Bearer $TAB5_TOKEN" http://<tab5-ip>:8080/settings | python3 -c "import sys,json; print(json.load(sys.stdin).get('dragon_host'))"
```

If wrong:

```bash
curl -X POST -H "Authorization: Bearer $TAB5_TOKEN" \
    http://<tab5-ip>:8080/settings -d '{"dragon_host":"192.168.1.91"}'
curl -X POST -H "Authorization: Bearer $TAB5_TOKEN" \
    http://<tab5-ip>:8080/reboot
```

### "Stale `auth_tok` in memory" — recovery

If the auth token in your local notes is stale (e.g., after an NVS erase):

```bash
. ~/esp/esp-idf/export.sh
python -m esptool --chip esp32p4 -p /dev/ttyACM0 read_flash 0x9000 0x6000 /tmp/nvs_dump.bin
python3 <<'PY'
import re
data = open('/tmp/nvs_dump.bin','rb').read()
idx = data.find(b'auth_tok')
m = re.search(rb'[0-9a-f]{32}', data[idx:idx+200])
print('TOKEN:', m.group().decode() if m else 'not found')
PY
```

Cross-check with the masked serial output (`05ee****b9f2` → token starts `05ee` and ends `b9f2`).  See [`SECURITY.md`](../SECURITY.md).

## Where to next

- **Add a new screen / overlay:** look at `ui_camera.c` or `ui_settings.c` for the LVGL pattern
- **Add a debug-server endpoint:** [`main/debug_server.c`](../main/debug_server.c) — every `httpd_register_uri_handler` is one
- **Wire a new obs event:** call `tab5_debug_obs_event(kind, detail)` at your site, update the events table in CLAUDE.md
- **Add a NVS setting:** [`main/settings.{c,h}`](../main/settings.h) + the NVS Settings table in CLAUDE.md
- **Add a hardware peripheral:** [`bsp/tab5/bsp_config.h`](../bsp/tab5/bsp_config.h) + [`docs/HARDWARE.md`](HARDWARE.md)
- **Write a skill that emits widgets:** [`docs/WIDGETS.md`](WIDGETS.md) + [`docs/PLAN-widget-platform.md`](PLAN-widget-platform.md)
- **Read the war stories:** [`../LEARNINGS.md`](../LEARNINGS.md)

Welcome aboard. 🐉
