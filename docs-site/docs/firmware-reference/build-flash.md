---
title: Build & flash
sidebar_label: Build & flash
---

# Build & flash

:::note Curated from project sources
This page is curated from the project's `CLAUDE.md` runbook + `LEARNINGS.md` + `docs/*` trees. The internal versions are the source of truth; this page reflects them as of 2026-05-01.
:::

Get a TinkerTab binary on a Tab5. ESP-IDF 5.5.2 is the pinned toolchain — anything older or newer may not build cleanly.

## One-time setup

```bash
# Install ESP-IDF v5.5.2 (matches dependencies.lock)
mkdir -p ~/esp && cd ~/esp
git clone -b v5.5.2 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32p4
echo "alias get_idf='. ~/esp/esp-idf/export.sh'" >> ~/.bashrc

# Clone TinkerTab
cd ~/projects
git clone https://github.com/lorcan35/TinkerTab.git
cd TinkerTab
```

## Build

```bash
get_idf                        # source ESP-IDF env
idf.py set-target esp32p4
idf.py build
```

First build takes ~5-10 minutes. Subsequent builds are incremental — usually under a minute.

If you change `sdkconfig.defaults`, run `idf.py fullclean build` — incremental builds cache stale config.

## Flash

```bash
idf.py -p /dev/ttyACM0 flash
```

The Tab5's USB-C port enumerates as `/dev/ttyACM0` on Linux. If your machine sees it as `/dev/ttyUSB0`, use that.

If the chip enters ROM-download mode after flashing (the screen stays black), trigger a watchdog reset to boot the flashed app:

```bash
python -m esptool --chip esp32p4 -p /dev/ttyACM0 \
    --before no_reset --after watchdog_reset read_mac
```

## Monitor serial output

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

`idf.py monitor` works too but uses port 0 (UART0 = G37/G38, M5-Bus 13/14). The Python helper above uses the USB-CDC port which doesn't conflict with anything.

## OTA flow (for live deployments)

The firmware ships with two OTA partitions (3 MB + 3 MB) and `esp_https_ota` rollback. Deploy a new binary by serving it from Dragon:

```bash
# Build new firmware locally
idf.py build

# Compute hash
SHA=$(sha256sum build/tinkertab.bin | cut -d' ' -f1)

# Push to Dragon
scp build/tinkertab.bin radxa@192.168.1.91:/home/radxa/ota/

# Update version manifest
echo "{\"version\":\"0.7.1\",\"sha256\":\"$SHA\"}" \
  | ssh radxa@192.168.1.91 'cat > /home/radxa/ota/version.json'

# Tab5 checks hourly OR user taps "Check Update" in Settings
```

Tab5 verifies the SHA256 after download and aborts if it doesn't match. New firmware boots in PENDING_VERIFY; if it crashes before calling `tab5_ota_mark_valid()`, the bootloader reverts on next boot.

## Force OTA rollback (recovery)

If a flashed image is bad but Tab5 still boots:

```bash
curl -sS -H "Authorization: Bearer $TAB5_DEBUG_TOKEN" \
     -X POST http://<tab5-ip>:8080/ota/rollback
```

Forces a reboot into the inactive slot.

## CI gates

Pre-push, run:

```bash
git fetch origin main:refs/remotes/origin/main
git-clang-format --binary clang-format-18 --diff origin/main \
    main/*.c main/*.h
# Empty diff → clean. Any output → fix:
git-clang-format --binary clang-format-18 origin/main
git add -u && git commit --amend --no-edit && git push -f
```

CI runs the same diff against your changed C/H lines. Pre-existing format violations elsewhere in `main/` are intentionally ignored, but **your diff must be clean**.
