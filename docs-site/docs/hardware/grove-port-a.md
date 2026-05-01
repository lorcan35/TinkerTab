---
title: Grove Port A (sensors)
sidebar_label: Grove Port A (sensors)
---

# Grove Port A (sensors)

Tab5's side-mounted **HY2.0-4P** Grove connector is an I2C bus you can use to plug in passive sensors. The pin map:

| Pin | Signal | GPIO |
|-----|--------|------|
| 1 | GND | — |
| 2 | 5 V | gated by `EXT5V_EN` (IO-Expander 1, P4) |
| 3 | SCL | GPIO 53 |
| 4 | SDA | GPIO 54 |

## What works

Any **passive I2C peripheral** with a documented register map. We're particularly fond of:

| Sensor | Address | What it gives you |
|--------|---------|-------------------|
| Grove BME280 | 0x76 | Temp + humidity + barometric pressure |
| Grove SHT31 | 0x44 | Temp + humidity |
| Grove SGP30 | 0x58 | TVOC + eCO2 (air quality) |
| Grove VL53L0X | 0x29 | Time-of-flight distance |
| Grove BH1750 | 0x23 | Ambient light |
| Grove ENS160 | 0x53 | Air quality (newer alternative to SGP30) |

## What doesn't work

- **Active modules** that are themselves microcontrollers (ESP32-CAM, Arduino-based modules) — they don't speak I2C-slave and would need custom firmware on both ends. See [the FAQ](/docs/glossary#esp32-cam) for why.
- **3.3 V-only sensors that don't tolerate 5 V VCC** — the EXT5V rail is 5 V. Most sensor breakouts have a level-shift / regulator on board, but bare chips don't.
- **High-current peripherals** (>500 mA on the 5 V rail) — `EXT5V_EN` switches a 500 mA budget; over that and you'll trip the IO-expander's protection.

## EXT5V_EN power gating

The 5 V rail to Port A and the rest of the expansion connectors is **gated**. By default it's off; the firmware's sensor service flips it on at boot once it's confirmed Port A has something on it.

The reason for the gate: a hot-plugged sensor at boot can momentarily collapse the 5 V rail and reset the ESP32-P4. Gating defers the rail rise until the system is stable.

Refcounted helpers (Phase 2 of [Grove plan](https://github.com/lorcan35/TinkerTab/blob/main/docs/PLAN-grove.md)) `tab5_ext5v_acquire()` / `tab5_ext5v_release()` keep the rail on as long as at least one driver wants it.

## How sensor data reaches Dragon

```
sensor → Tab5 I2C poll → service_port_a → WS sensor_data event → Dragon
                                                                    │
                                                                    ↓
                                                      MessageStore + LLM context
```

When the LLM sees a question that could benefit from environmental context (*"is it humid in here?"*, *"how warm is it?"*), the sensor reading is injected into the system prompt for that turn. No tool call needed — it's ambient context.

## Adding a new sensor driver

1. Create `bsp/tab5/sensors/<name>.{c,h}` with `init() / read() / shutdown()`
2. Register in `main/service_port_a.c`'s sensor table with vendor ID + I2C address
3. Add a `case` in `service_port_a_emit_sensor_data()` to populate the WS event
4. Test in the simulator if possible, then on hardware

The plan doc is the source-of-truth for the architecture: [`docs/PLAN-grove.md`](https://github.com/lorcan35/TinkerTab/blob/main/docs/PLAN-grove.md).

## Module13.2 stack — different beast

The connector on the *top* of the Tab5 (where the K144 stacks) is the **Module13.2** stack. That's a higher-pin-count connector with UART, SPI, additional power rails. Grove Port A is just the side I2C connector. They don't share infrastructure.
