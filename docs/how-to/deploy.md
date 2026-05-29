---
audience: operator
type: how-to
prerequisites: A flashed Tab5 (see [Flash the firmware](flash-firmware.md)) and a reachable Dragon server
last-verified: 2026-05-29
est-time: 15 min
---
# How to run a Tab5 in production

Use this when you are operating one or more Tab5 devices day-to-day — keeping
them updated and recovering them when something goes wrong. Assumes the firmware
is already flashed and a [Dragon](../../GLOSSARY.md) server is reachable.

## Steps

1. **Confirm the device is healthy** over the
   [debug server](../reference/debug-server.md) (no auth needed for `/info`):
   ```bash
   curl -s http://<tab5-ip>:8080/info | python3 -m json.tool
   ```
2. **Ship a firmware update over OTA.** Copy the binary to the Dragon's OTA dir
   and publish a `version.json` that includes the SHA256 (required — it protects
   against MitM firmware swaps over unencrypted LAN HTTP):
   ```bash
   scp build/tinkertab.bin radxa@<dragon-ip>:/home/radxa/ota/
   SHA=$(sha256sum build/tinkertab.bin | cut -d' ' -f1)
   echo "{\"version\":\"0.7.1\",\"sha256\":\"$SHA\"}" \
     | ssh radxa@<dragon-ip> 'cat > /home/radxa/ota/version.json'
   ```
   The Tab5 checks hourly, or an operator can tap **Check Update** in Settings.
3. **Trigger / verify an OTA from the debug server:**
   ```bash
   curl -s -H "Authorization: Bearer $TOKEN" http://<tab5-ip>:8080/ota/check | python3 -m json.tool
   curl -s -H "Authorization: Bearer $TOKEN" -X POST http://<tab5-ip>:8080/ota/apply
   ```

## Verify it worked

After an OTA, the new image boots in `PENDING_VERIFY`; if it self-tests cleanly
it is marked valid, otherwise the bootloader auto-rolls-back on the next reboot.
Confirm the running version via `/info`.

## Troubleshooting

- **Device unreachable but powered** → the DMA pool may be exhausted; the heap
  watchdog reboots it automatically. As a last resort, reflash a known-good
  image over USB (see [Flash the firmware](flash-firmware.md)).
- **Bad firmware deployed** → because every feature lands as one squash commit,
  `git revert <sha>` then rebuild + OTA. The new image will also auto-roll-back
  if it fails its self-test.

> A full fleet-operations guide (multi-device monitoring, rollout strategy)
> lands in **Wave 1**. The project [`CLAUDE.md`](../../CLAUDE.md) "Recovery &
> Rollback" and "OTA Firmware Updates" sections carry the current detail. See
> [`../ROADMAP.md`](../ROADMAP.md).
