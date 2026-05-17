---
title: Power & charging
sidebar_label: Power & charging
---

# Power & charging

## Battery

- **Capacity** — 1500 mAh LiPo
- **Cell** — single 3.7 V nominal
- **Charge IC** — manages 5 V USB-C → 4.2 V cell with thermal/overcurrent protection
- **Charge LED** — rear-mounted, amber while charging, green at 100%
- **Runtime** — ~3 hours typical mixed use; ~6 hours light idle; ~1 hour heavy (camera streaming + voice + bright display)

## Charging

- **Input** — USB-C, 5 V at 2 A minimum
- **Charge speed** — to 80% in ~45 minutes; to 100% in ~75 minutes
- **Pass-through** — yes, you can use the device while charging
- **Wireless / magnetic** — no, USB-C wired only

:::warning 5 V only
Use a 5 V supply. Higher voltages (some USB-PD adapters output 9 V / 12 V / 15 V / 20 V) will damage the device. The Tab5 doesn't negotiate USB-PD upward — it expects 5 V.
:::

## Battery health

The Tab5 doesn't expose a battery percentage on the main UI yet (planned, see roadmap). For now you can read the raw battery state via the debug HTTP server:

```bash
curl -s -H "Authorization: Bearer $TOKEN" http://<tab5-ip>:8080/info | jq .battery
# → {"voltage_mv": 3940, "charging": false, "soc_pct": 67}
```

## Power-saving modes

Tab5 currently does:

- **Display dim** during configured "quiet hours" (NVS keys `quiet_on`, `quiet_start`, `quiet_end`)
- **Voice WS keepalive** — pings Dragon every 5 seconds to keep the connection warm
- **Heap watchdog** — passive monitor, doesn't affect power

Not yet implemented:

- True deep-sleep with mic-AON wake — the ESP32-P4 has the silicon for it but TinkerOS hasn't wired it
- Display-off when not interacted with — currently just dims via the quiet-hours mechanism

## When the battery is dead

The Tab5 will refuse to boot below ~3.4 V cell voltage. Plug in USB-C, give it 5 minutes, then long-press power to boot.

If it *appears* dead (no LED, no boot), but power is connected:

1. Try a different USB-C cable. (Cheap cables drop voltage under load.)
2. Try a different 5 V adapter.
3. Reset pinhole.
4. Hard power-off, then power-on.

## Battery replacement

The 1500 mAh cell is replaceable but requires opening the case (4 screws on the rear). Use only the same form-factor LiPo (130 × 60 × 4 mm) with built-in protection circuit. M5Stack sells genuine replacements.
