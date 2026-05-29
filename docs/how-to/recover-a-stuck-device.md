---
audience: operator
type: how-to
prerequisites: A [Tab5](../../GLOSSARY.md) you can reach over the [debug server](../reference/debug-server.md), serial, or USB
last-verified: 2026-05-29
est-time: 15 min
---
# How to recover a stuck device

Use this when a [Tab5](../../GLOSSARY.md) is misbehaving — frozen UI, voice
stuck, wakeword dead, or fully unresponsive. Recovery has layers; try them
cheapest-first. Assumes you can reach the device over the network
([debug server](../reference/debug-server.md), port 8080), serial (115200 baud),
or USB.

## Steps

Work down this ladder. Stop at the first one that fixes it.

### 1. Force a voice/WebSocket reconnect

If voice is stuck (no reply, "connecting" forever) but the UI responds:

```bash
export TOKEN="abcdef1234567890abcdef1234567890"
curl -s -H "Authorization: Bearer $TOKEN" -X POST http://<ip>:8080/voice/reconnect
# then re-read state
curl -s -H "Authorization: Bearer $TOKEN" http://<ip>:8080/voice | python3 -m json.tool
```

### 2. Cancel / clear a wedged voice turn

A turn stuck in PROCESSING (e.g. after a transient `stt_empty`) can be snapped
back:

```bash
curl -s -H "Authorization: Bearer $TOKEN" -X POST http://<ip>:8080/voice/cancel
curl -s -H "Authorization: Bearer $TOKEN" -X POST http://<ip>:8080/voice/clear
```

### 3. Recover the K144 / TinkerON chain

If wakeword or Onboard (mode 4) is dead, reset the
[StackFlow](../../GLOSSARY.md) daemon on the K144 (no power-cycle needed,
~10 s):

```bash
curl -s -H "Authorization: Bearer $TOKEN" -X POST http://<ip>:8080/m5/reset
# poll until failover_state_name is "ready"
curl -s -H "Authorization: Bearer $TOKEN" http://<ip>:8080/m5 | python3 -m json.tool
```

If `/m5/reset` does not recover it, the daemon may need a full restart. From a
dev host over Axera ADB (plug into the **K144's top USB-C**):

```bash
sudo adb shell systemctl restart llm-sys
# then POST /m5/reset again
```

### 4. Reboot the Tab5

If the UI itself is unresponsive but the network stack is alive:

```bash
curl -s -H "Authorization: Bearer $TOKEN" -X POST http://<ip>:8080/reboot
```

There is also a top-right reboot button on the home screen: tap shows a "Hold
to reboot" toast, long-press reboots. The fragmentation watchdog will also
auto-reboot on its own if internal SRAM's largest free block stays below
threshold for ~3 minutes.

### 5. OTA rollback (a bad firmware booted)

If a new image boots but fails self-test, the bootloader auto-reverts on the
next reboot (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`). Force it:

```bash
curl -sS -H "Authorization: Bearer $TOKEN" -X POST http://<ip>:8080/ota/rollback
```

### 6. Reflash a known-good image (device bricked)

When nothing on-device responds, serial-flash over USB. See
[Flash the firmware](flash-firmware.md) for the full loop:

```bash
. ~/esp/esp-idf/export.sh
cd ~/projects/TinkerTab
idf.py -p /dev/ttyACM0 flash
# if it stays in ROM download mode, kick a watchdog reset:
python -m esptool --chip esp32p4 -p /dev/ttyACM0 \
  --before no_reset --after watchdog_reset read_mac
```

### 7. Git rollback (a code regression)

Every feature lands as one squash-merge commit on `main`, so each is
revert-able. Undo the last landed feature and reflash:

```bash
cd ~/projects/TinkerTab
git log --oneline -5
git revert <sha>
idf.py build && idf.py -p /dev/ttyACM0 flash
```

## Verify it worked

After any layer, confirm the device is healthy:

```bash
# No auth needed
curl -s http://<ip>:8080/info | python3 -m json.tool       # heap, mode, auth_required
curl -s http://<ip>:8080/selftest | python3 -m json.tool   # health check
# Then a voice round-trip
curl -s -H "Authorization: Bearer $TOKEN" -X POST http://<ip>:8080/chat \
     -d '{"text":"reply with PONG"}'
```

## Troubleshooting

- **Cannot find the device on the network** → It is DHCP-assigned. Scan with
  `nmap -p 8080 --open <subnet>/24`; the Tab5 is the host whose `GET /info`
  returns `"auth_required": true`.
- **`/reboot` returns but the device does not come back** → It may be in ROM
  download mode after a flash. Kick a watchdog reset (layer 6) over serial.
- **K144 reset "rejected — probe already in flight"** → A reset is already
  cycling. Wait and re-poll `GET /m5`; do not spam `/m5/reset`.
- **Repeated fragmentation-watchdog reboots** → A leak or sustained low
  largest-free-block. Pull `/heap/history` and check for a screen that
  create/destroys overlays instead of hide/show. This is a code bug, not an ops
  fix.
- **ES7210 mic codec drift (RMS stuck low after hours)** → Physically unplug the
  USB-C for ~10 s to reset the codec.

## See also

- [Run a Tab5 in production](deploy.md) — the OTA + monitoring workflow.
- [Enable the TinkerON wakeword](enable-tinkeron-wakeword.md) — K144 chain setup.
- [Debug server reference](../reference/debug-server.md) — every recovery
  endpoint.
- [LVGL on ESP32-P4](../explanation/lvgl-on-esp32p4.md) — why the fragmentation
  watchdog exists.
