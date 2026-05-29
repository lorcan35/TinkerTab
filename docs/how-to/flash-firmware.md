---
audience: developer
type: how-to
prerequisites: [ESP-IDF v5.5.2 installed](https://docs.espressif.com/projects/esp-idf/en/v5.5.2/esp32p4/get-started/index.html), a checked-out TinkerTab repo, a Tab5 on USB
last-verified: 2026-05-29
est-time: 15 min
---
# How to flash the firmware

Use this when you need to build the TinkerTab firmware and load it onto a
[Tab5](../../GLOSSARY.md). Assumes you already have **ESP-IDF v5.5.2** installed
(the exact version the firmware is pinned to via `dependencies.lock`) and the
repo cloned. If this is your very first boot, start with the
[getting-started tutorial](../tutorials/getting-started.md) instead.

## Steps

1. **Source ESP-IDF in your shell.** You must do this in every new terminal:
   ```bash
   . ~/esp/esp-idf/export.sh
   ```

2. **Set the target.** Required after a clean build or a target change:
   ```bash
   cd ~/projects/TinkerTab
   idf.py set-target esp32p4
   ```

3. **Build.**
   ```bash
   idf.py build
   ```
   For a clean build after `sdkconfig` changes, pulling new components, or a
   target change, use `idf.py fullclean build` instead — incremental builds
   cache stale configuration.

4. **Flash over USB.** Adjust the port to match your board (`ls /dev/ttyACM*`):
   ```bash
   idf.py -p /dev/ttyACM0 flash
   ```

5. **Recover if the board enters ROM download mode.** ESP32-P4 boards commonly
   stay in ROM download mode after a flash instead of booting the app. Kick a
   watchdog reset to boot the flashed firmware:
   ```bash
   python -m esptool --chip esp32p4 -p /dev/ttyACM0 \
     --before no_reset --after watchdog_reset read_mac
   ```

## Verify it worked

Monitor the serial output at 115200 baud and confirm the firmware boots:

```bash
python3 -c "
import serial, time
s = serial.Serial('/dev/ttyACM0', 115200, timeout=5)
time.sleep(0.3)
s.write(b'\r')
while True:
    if s.in_waiting:
        print(s.read(s.in_waiting).decode('utf-8', errors='replace'), end='', flush=True)
"
```

Expect boot log lines and, once Wi-Fi is up, the
[debug server](../reference/debug-server.md) announcing its port and auth token:

```
I (xxxx) debug_srv: Debug server auth token: <32-hex-chars>
```

You can also confirm the device is alive over the network (no auth needed):

```bash
curl -s http://<device-ip>:8080/info | python3 -m json.tool
# → expect JSON with "auth_required": true and the current mode
```

## Troubleshooting

- **Board does not boot after flash; serial shows ROM messages** → The Tab5 is
  stuck in ROM download mode. Run the watchdog-reset `esptool` command from
  step 5.
- **`sdkconfig.defaults` changes have no effect** → Incremental builds cache
  stale config. Run `idf.py fullclean build`.
- **Wrong / no serial port** → List candidates with `ls /dev/ttyACM* /dev/ttyUSB*`
  and pass the right one to `-p`.
- **Build fails on a component download** → Confirm you sourced ESP-IDF
  **v5.5.2** (`idf.py --version`); a mismatched IDF version will not match
  `dependencies.lock`.
- **Wi-Fi never connects after boot** → Check the SSID/password in
  `sdkconfig.defaults` (or set them on-device in **Settings**), and verify the
  ESP32-C6 SDIO co-processor pins are intact (see the project README's
  Troubleshooting section).
