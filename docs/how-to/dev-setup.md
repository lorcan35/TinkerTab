---
audience: developer
type: how-to
prerequisites: [ESP-IDF v5.5.2](https://docs.espressif.com/projects/esp-idf/en/v5.5.2/esp32p4/get-started/index.html), git, a Tab5 on USB
last-verified: 2026-05-29
est-time: 20 min
---
# How to set up a development environment

Use this when you want to build, modify, and flash the TinkerTab firmware
yourself. Assumes you have git and a Tab5 to flash.

## Steps

1. **Install ESP-IDF v5.5.2** — the exact version pinned by `dependencies.lock`:
   ```bash
   mkdir -p ~/esp && cd ~/esp
   git clone -b v5.5.2 --recursive https://github.com/espressif/esp-idf.git
   cd esp-idf && ./install.sh esp32p4
   ```
2. **Clone the firmware:**
   ```bash
   git clone https://github.com/lorcan35/TinkerTab.git
   cd TinkerTab
   ```
3. **Build and flash** — see [Flash the firmware](flash-firmware.md) for the
   full build/flash/recover loop.

## Verify it worked

```bash
. ~/esp/esp-idf/export.sh
idf.py --version    # → expect v5.5.2
```

## Troubleshooting

- **`idf.py` not found** → you did not source `export.sh` in this shell.
- **Component download / lock mismatch** → confirm IDF v5.5.2; a mismatched
  version will not match `dependencies.lock`.

> A deeper developer guide (the full toolchain, simulator, and contribution
> workflow) lands in **Wave 1**. For now, the legacy
> [`../dev-setup.md`](../dev-setup.md) carries additional detail, and
> [Flash the firmware](flash-firmware.md) covers the build loop. See
> [`../ROADMAP.md`](../ROADMAP.md).
