# Plan — Grove sensor support on Tab5

**Status:** parked, not started.
**Owner:** unassigned.
**Tracking issue:** TBD (filed alongside this doc).
**Last updated:** 2026-04-28.

---

## What this enables

Plug a 4-pin Grove sensor (BME280, SHT3x, MPU6050, soil-moisture, light, anything I2C) into Tab5's **Port A** connector and surface its readings to Dragon, where they become available as conversational context for the LLM ("how warm is it in here?", "remind me to drink water if humidity drops below 30%"). Foundation for any future hardware-extension story — temperature dashboards, environmental triggers for skills, custom bench sensors.

This is **additive infrastructure**, not a feature. Done well it's invisible: a new sensor is one new file and one driver registry entry.

## Why this isn't trivial

Tab5 ships the connector physically but our firmware doesn't initialize the bus, doesn't know the EXT5V pin, doesn't have a sensor-emit WS protocol. Three layers — pinout discovery, driver wrapper, protocol — all of which need to land before the first sensor reading reaches Dragon.

---

## Hardware reality

### Port A (HY2.0-4P, Grove-compatible)

| Pin | Wire | Tab5 connection | Notes |
|---|---|---|---|
| 1 | yellow | **GPIO 53** (SCL) | Community-confirmed via Arduino's `M5.getPin(port_a_scl)`. **Not in our `bsp_config.h` yet.** |
| 2 | white | **GPIO 54** (SDA) | Same — community-confirmed, not in our pin map. |
| 3 | red | **5 V (gated)** | The 5 V rail is gated by `EXT5V_EN` on one of the IO expanders (0x43 or 0x44). Without flipping that pin, Port A is dark. |
| 4 | black | GND | — |

### EXT5V_EN — the unknown pin

Both `bsp/tab5/io_expander.{c,h}` and `bsp/tab5/bsp_config.h` document the IO expanders' P-pins **partially**:

- **0x43:** P0 = LCD reset, P1 = speaker amp, P2 = touch reset, P3 = camera reset, P4 = ??? (likely EXT5V), P5-P15 = ???
- **0x44:** P0 = WiFi C6 power, P1 = USB 5V, P2 = battery charging, P3-P15 = ???

The EXT5V pin is **mentioned in M5's docs as existing** but the exact P-number isn't recorded in our firmware. **First task:** read both expanders' GPIO direction registers + states, log them, then probe Port A for any device while toggling each unknown pin. Whichever pin makes a sensor appear at I2C addr 0x77 is EXT5V.

### Voltage levels

- VCC on the connector: **5 V** (when EXT5V_EN is high).
- Logic on SCL/SDA: **3.3 V** (Tab5 GPIOs are 3.3 V LVCMOS; pulled up to 3.3 V).

Most Grove I2C breakouts ship with an onboard 3.3 V LDO and tolerate either 3.3 V or 5 V VCC. **Verify per sensor.** If a board only runs at 3.3 V, you'll need a separate 3.3 V supply (USB-C dev board) or to mod the breakout.

### Existing I2C0 bus is full-but-not-claimed

Our system bus (`SDA=31, SCL=32`) has 8 devices on it but **plenty of free 7-bit addresses** (0x08-0x77 range, minus the 8 in use). Port A is a **second physical bus on different GPIOs** — we'll initialize it as I2C Port 1, separate from I2C Port 0.

> Why not just put new sensors on the system bus? Two reasons: (a) the IC list there is closely tied to internal hardware (camera, codec, IMU, RTC, expanders) and we shouldn't risk a cable-induced glitch knocking out the audio codec. (b) Port A is where M5 ships sensors; respecting that contract means a Grove cable Just Works.

---

## Code architecture — where this slots in

### Service-registry pattern

We have a 4-step lifecycle (see `main/service_registry.{c,h}` and `main/service_audio.c` as the cleanest example):

```c
esp_err_t  XXX_service_init(void);   // probe + alloc, no side effects
esp_err_t  XXX_service_start(void);  // begin sampling
esp_err_t  XXX_service_stop(void);   // pause cleanly
// + register in service_registry.c
```

Boot sequence in `main.c`:
1. `tab5_services_register_all()` — populate registry metadata (line 278).
2. `tab5_services_init_all()` — STORAGE → DISPLAY → AUDIO → NETWORK → DRAGON in order (line 317).
3. `tab5_services_start(...)` — start each in dependency order (lines 368-384).

A new sensor is a new service. Three file edits per sensor.

### WS protocol extension

Today the Tab5 → Dragon WS messages are `register`, `start/stop` (audio), `text`, `cancel`, `ping`, `config_update`, `user_image`, `widget_action`. None of these carry sensor telemetry.

**New message type to define and ship:**

```json
{
  "type": "sensor_data",
  "kind": "bme280",
  "ts": 1714329600000,
  "values": {
    "temp_c": 23.4,
    "humidity_pct": 45.2,
    "pressure_hpa": 1013.7
  }
}
```

Cadence: 1 Hz default, configurable per sensor via the `register` capability advertisement.

**Capability advertisement** at register time extends the existing `capabilities` object:

```json
{
  "type": "register",
  "device_id": "...",
  "capabilities": {
    "audio_codec": ["pcm","opus"],
    "sensors": [
      {"kind":"bme280","fields":["temp_c","humidity_pct","pressure_hpa"]}
    ]
  }
}
```

Dragon-side: a small handler in `dragon_voice/server.py` that logs to the session events table or surfaces in the dashboard. Future LLM context-builder can pull recent sensor readings as input ("user is in a 23°C room, 45% humidity") so skills can react.

---

## Phased plan — six small PRs

### Phase 1 — Port A I2C bring-up (no sensor yet)

**Files:** `bsp/tab5/i2c_port_a.{c,h}` (new), `bsp/tab5/bsp_config.h` (define GPIO 53/54 as `TAB5_PORT_A_SCL/SDA`), `main/service_port_a.{c,h}` (new), `main/service_registry.c` (add enum), `main/CMakeLists.txt`.

**Acceptance:**
- Boot logs "Port A bus ready (SDA=54, SCL=53, 100 kHz)" — chosen 100 kHz for cable robustness.
- A `tab5_get_i2c_port_a_bus()` accessor returns a valid `i2c_master_bus_handle_t`.
- `i2c_master_probe(port_a_bus, 0x77, 100)` returns `ESP_ERR_NOT_FOUND` (nothing plugged in yet).

**Risk:** Low. Pure infrastructure, no sensor dependency.

### Phase 2 — EXT5V pin discovery

**Files:** debug helper + `bsp/tab5/io_expander.{c,h}` patch.

**Acceptance:**
- Identify which IO-expander P-pin enables Port A's 5 V rail.
- Document in `bsp_config.h` as `TAB5_IOEXP_EXT5V_PIN`.
- Add `tab5_io_expander_set_ext5v(bool)` API; default-on at boot.
- After flip: `i2c_master_probe(port_a_bus, 0x77, 100)` finds a BME280 (when one is plugged in).

**Risk:** Medium. Probing the wrong expander pin could disable WiFi or speakers temporarily. Test in a controlled environment.

### Phase 3 — BME280 worked example

**Files:** `main/service_bme280.{c,h}` (new), `main/CMakeLists.txt`, `idf_component.yml` (add `espressif/bme280`).

**Acceptance:**
- BME280 plugged into Port A is detected at boot.
- Service samples temp/humidity/pressure at 1 Hz.
- Readings logged to serial: `bme280: 23.4°C  45.2% RH  1013.7 hPa`.

**Risk:** Low. Driver is upstream-supported and small.

### Phase 4 — `sensor_data` WS protocol

**Files:** `main/voice.c` (new emit function), `dragon_voice/server.py` + `dragon_voice/db.py` (handler + storage), `docs/protocol.md` (TinkerBox).

**Acceptance:**
- Tab5 emits `sensor_data` frames at 1 Hz when sensor is present.
- Dragon logs them to `events` table with `event_type = "sensor_data"`.
- Dashboard "Events" tab can filter to sensor-data frames and show a chart.

**Risk:** Low-medium. Two-repo change; careful with test coverage on both sides.

### Phase 5 — Capability advertisement

**Files:** `main/voice.c` (extend `register` frame), `dragon_voice/server.py` (parse).

**Acceptance:**
- Tab5's register frame includes `capabilities.sensors` array.
- Dragon stores per-device sensor list in the `devices` table.
- LLM context-builder can query "what sensors does this device have?".

**Risk:** Low.

### Phase 6 — LLM context injection

**Files:** `dragon_voice/conversation.py` (extend system prompt with sensor context).

**Acceptance:**
- Most-recent sensor reading appears in the LLM's system prompt when the user asks about environmental state.
- LLM responses naturally reference the data ("It's 23°C in your room").

**Risk:** Low (just prompt engineering + a DB query).

---

## Multi-sensor support

Phase 1-6 ship a single sensor. For multiple sensors, two paths:

**Option A — Same Port A bus, different addresses.** BME280 at 0x77, MPU6050 at 0x68 (collision with on-board IMU — don't use), SHT3x at 0x44 (collision with IO expander 2 — don't use). Address conflicts are real on Tab5; check carefully.

**Option B — I2C multiplexer (PCA9548A) on Port A.** 1× 8-channel mux; each channel becomes a virtual sub-bus. Standard ESP-IDF pattern. Adds ~$2-5 hardware, ~50 LOC mux driver, near-zero protocol impact.

**Recommended:** Option B once we have ≥3 sensors; Option A for the first 2.

---

## Honest unknowns

1. **EXT5V_EN pin number** — Phase 2's whole job. Could be P4 on 0x43, could be elsewhere. No way to know without bench probe.
2. **Whether Port A's GPIO 53/54 work without level-shifting** — most M5 hosts run Port A at 3.3 V logic with a 5 V rail. Should be fine. Verify with scope before plugging fragile sensors.
3. **Multiplexer bus contention if both audio + sensor poll at high rate** — we share the system bus with the codec (different bus, but mux bring-up could expose latencies). Bench under load.

## Out of scope

- Grove devices using **UART/analog/digital** variants of the connector. Tab5's Port A is I2C-only.
- 1-Wire / SPI sensors. Different connector, different bus.
- Hot-swap detection. We assume sensors are present at boot; replug requires reboot.
- Alerting / threshold rules on Tab5. The LLM can do that via prompt; no firmware-side rule engine.

---

## References

- M5 Tab5 product page: https://docs.m5stack.com/en/core/Tab5
- Arduino Forum — I2C on Tab5 (GPIO 53/54): https://forum.arduino.cc/t/i2c-on-the-m5stack-tab5/1403830
- Espressif BME280 driver: https://components.espressif.com/components/espressif/bme280
- Seeed Wiki — Grove System: https://wiki.seeedstudio.com/Grove_System/
- HY2.0-4P SMD spec: https://docs.m5stack.com/en/accessory/converter/hy2.0_4p_smd

## Code anchors

- I2C bus init pattern: `bsp/tab5/audio.c:60-140` (system bus + ES8388 codec dev)
- Service-registry pattern: `main/service_registry.{c,h}` + `main/service_audio.c:16-50`
- IO expander API: `main/io_expander.h` + `bsp/tab5/io_expander.c`
- Boot sequence: `main/main.c:278, 317, 368-384`
- WS register frame emit: `main/voice.c` (search for `"type":"register"` or `voice_send_register`)
- Pinout reference: `bsp/tab5/bsp_config.h` (the canonical pin map; missing GPIO 53/54 today)
