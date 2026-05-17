---
title: Buttons & resets
sidebar_label: Buttons & resets
---

# Buttons & resets

## Power button (top edge)

| Action | Duration | Effect |
|--------|----------|--------|
| Press + release | &lt;1 s | Wake from low-power / sleep |
| Long-press | 3 s | Power on (from off) |
| Long-press | 8 s | Hard power-off (force; bypass clean shutdown) |

The 8-second hard power-off is the "stuck" recovery. Use it sparingly — on rare occasions a hard cut while the SD card is being written to can corrupt the FAT32 filesystem (`fsck.fat` recovers it).

## Volume rocker (side)

Two-button pair: + / -. Each press adjusts `volume` (NVS) by 5 percentage points (0–100). Long-press for fast scrub.

The rocker also wakes the device from low-power.

## Reset pinhole (rear)

Use a thin pointed object (paperclip). A short press triggers a clean watchdog reset — equivalent to `esp_restart()` from firmware. The display flashes white briefly, then boot resumes from `ota_0` or `ota_1` per the partition table's selected slot.

This is the *first* recovery to try if Tab5 is unresponsive but not bricked.

## OTA rollback (no physical buttons)

Tab5 runs two OTA partitions. If a flashed image boots but fails self-test (never calls `esp_ota_mark_app_valid_cancel_rollback()`), the bootloader auto-rollbacks on the next reboot.

To force a rollback from a workstation:

```bash
curl -X POST -H "Authorization: Bearer $TOKEN" \
     http://<tab5-ip>:8080/ota/rollback
```

The Tab5 reboots into the inactive slot. See [build & flash](/docs/firmware-reference/build-flash) for OTA partition mechanics.

## What if even hard power-off doesn't help?

Three layers of recovery, ordered by aggressiveness:

1. **Reset pinhole** — clean reboot, retains user data
2. **Hard power-off** (8 s power button) — forces a power cycle
3. **Disconnect USB-C, wait 60 s, reconnect** — forces the LiPo charging IC to fully discharge any residual rail voltage, useful if a soft brick is masking a charge-controller issue
4. **Re-flash via USB** — `idf.py -p /dev/ttyACM0 flash` from a workstation. Wipes and re-installs the active OTA slot.

In ~3 years of use across our test fleet, level 4 has been needed exactly once — after a flash interrupt mid-write. Levels 1-3 cover virtually all field-recoverable issues.

## The orange "mic active" LED

Not a button, but worth knowing: there's a small orange LED next to the speaker grille that lights when the microphone is *active*. Off when muted or idle. Useful for confirming the mic isn't accidentally listening when you think it isn't.
