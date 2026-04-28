# Tab5 Hardware Modding Guide

> Want to add a sensor, an external display, a button, an
> environmental probe?  This doc covers what's safe to wire to,
> what's already in use, and what you absolutely should not touch.
>
> **Read [`HARDWARE.md`](HARDWARE.md) first** — it's the
> authoritative pinout.  This doc is the *modder's* companion: how
> to extend safely, where the spare capacity lives, and how to
> integrate a mod into the firmware.

---

## TL;DR safety rules

1. **Don't ground out the I2C bus.**  System I2C (SDA=31, SCL=32) is
   shared with display reset, touch, IO expanders, IMU, RTC, audio
   codecs, battery monitor.  A short here knocks the whole device
   off the air.
2. **Don't pull >100 mA off the 3V3 rail without a regulator.**  The
   Tab5's 3V3 supply is sized for the on-board ICs.  For anything
   power-hungry (radios, motors, heaters) use a separate boost from
   the 5 V USB rail.
3. **Don't reuse a GPIO that's already pinned in `bsp/tab5/bsp_config.h`.**
   The compiler won't catch this — you'll get random failures (mic
   stops working, screen tears, WiFi drops).  See "GPIO map" below.
4. **Don't connect anything to the MIPI-CSI / MIPI-DSI lanes.**  These
   are differential high-speed pairs designed for specific peripherals
   (camera, display).  Treat them as a forbidden zone.
5. **Re-flash with `idf.py erase-flash flash` after pin-table changes.**
   Stale NVS pin caches cause baffling boot failures.

If you ignore exactly one of these, ignore #5.  Fix in software is
much faster than fix in hardware.

---

## What's free, what's not

The ESP32-P4 has 55 GPIOs.  Tab5 uses ~30 of them for on-board
peripherals.  The rest are accessible via the **Grove connector**
on the side of the device + the **HY2.0-4P expansion port** on the
back.

### Grove connector (recommended path for sensors)

Tab5 ships with a standard 4-pin Grove connector exposing:

| Pin | Signal | Notes |
|-----|--------|-------|
| 1 | 5 V | USB rail, fused |
| 2 | GND | Common ground |
| 3 | GPIO (default I2C SDA) | Routable to either I2C or GPIO modes |
| 4 | GPIO (default I2C SCL) | Routable to either I2C or GPIO modes |

Default config exposes the Grove pins as a *secondary* I2C bus
(separate from the system I2C at SDA=31, SCL=32).  This is the
cleanest path for adding I2C sensors — you can hot-plug an air
quality sensor, a soil-moisture probe, a BME280 weather sensor, etc.,
without affecting the on-board peripherals.

To use the Grove I2C bus:
```c
#include "driver/i2c_master.h"

i2c_master_bus_handle_t mod_bus;
i2c_master_bus_config_t cfg = {
    .clk_source = I2C_CLK_SRC_DEFAULT,
    .i2c_port = 1,                // I2C_NUM_1 — separate from system bus
    .scl_io_num = 18,             // Grove default; verify with your board
    .sda_io_num = 17,             // Grove default; verify with your board
    .glitch_ignore_cnt = 7,
    .flags.enable_internal_pullup = true,
};
i2c_new_master_bus(&cfg, &mod_bus);
```

Verify the actual GPIO numbers on your board revision against the
schematic — early units shipped Grove on different pins than the
production run.

### HY2.0-4P expansion port (advanced / power users)

The 4-pin expansion header on the back of Tab5 brings out:

| Pin | Signal |
|-----|--------|
| 1 | 5 V |
| 2 | GND |
| 3 | GPIO (free) |
| 4 | GPIO (free) |

Use this for non-I2C peripherals — a button, an LED, a one-wire
sensor.  Software-configure the GPIO direction + pull in your
firmware code; nothing on this header is auto-claimed by the BSP.

---

## GPIO map — what's already in use

Don't reuse any of these.  Pulled from
[`bsp/tab5/bsp_config.h`](../bsp/tab5/bsp_config.h):

| GPIO | Use |
|------|-----|
| 8-13, 15 | WiFi (ESP32-C6 hosted via SDIO) |
| 22 | LCD backlight PWM |
| 23 | GT911 touch interrupt |
| 26-30 | Audio I2S (MCLK, BCLK, WS, DOUT, DIN) |
| 31, 32 | System I2C (SDA, SCL) — shared by every on-board IC |
| 36 | Camera external clock (24 MHz) |
| 39-44 | SD card SDMMC |

Plus the IO expander outputs (PI4IOE5V6416 chips at 0x43 + 0x44)
which control LCD reset, speaker enable, camera reset, WiFi
power, USB-host 5V, charging enable.  These don't consume ESP32
GPIOs but you can extend them via I2C if you need more digital
outputs.

### Free ESP32-P4 GPIOs (verified against bsp_config.h)

GPIOs **0, 1, 2, 3, 4, 6, 7, 14, 16, 17, 18, 19, 20, 21, 24, 25,
33, 34, 35, 37, 38, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54** are
not pinned in the BSP (as of 2026-04 firmware).

These are accessible via the Grove + expansion ports above.  Some
are exposed; others require board-level test points.  Check the
M5Stack Tab5 schematic before assuming a pin is physically
reachable — being unused in firmware ≠ being broken out to a
header.

---

## Recipe 1 — Add an I2C sensor (BME280 weather)

Common case.  You want temperature/humidity/pressure on the home
screen as a widget.

### Hardware

- BME280 breakout (I2C address 0x76 or 0x77)
- 4-wire Grove cable

Plug into Grove.  No solder, no schematic — done.

### Firmware

1. **Add a driver component** (or write a minimal one).  ESP-IDF
   has community BME280 drivers; pick one with permissive licensing.

2. **Initialize on a separate I2C bus** so it doesn't fight the
   system I2C:
   ```c
   // In main/main.c after BSP init
   i2c_master_bus_handle_t mod_bus = init_grove_i2c();
   bme280_handle_t bme = bme280_init(mod_bus, 0x76);
   ```

3. **Read periodically from a FreeRTOS task** — don't poll on the
   LVGL thread:
   ```c
   static void weather_task(void *arg) {
       while (1) {
           bme280_data_t d;
           if (bme280_read(bme, &d) == ESP_OK) {
               // Update UI via tab5_lv_async_call
               lv_label_set_text_fmt(s_weather_label,
                   "%.1f°C  %.0f%%  %.0fhPa",
                   d.temperature, d.humidity, d.pressure);
           }
           vTaskDelay(pdMS_TO_TICKS(60000));  // every minute
       }
   }
   ```

4. **Or skip the firmware-side custom UI entirely and emit a widget**
   via the protocol.  Add a tool on Dragon that reads from a sensor
   the user has registered with their device, then have the LLM
   call it.  This is the cleaner long-term path — see
   [TinkerBox `docs/SKILL_AUTHORING.md`](https://github.com/lorcan35/TinkerBox/blob/main/docs/SKILL_AUTHORING.md).

### Surfacing the value

Three options, in order of effort:

1. **Quick & dirty:** put a label on the home screen, update it
   from your task.
2. **Proper widget:** build a `widget_live` emission via the surface
   manager (see [`docs/PLAN-widget-platform.md`](PLAN-widget-platform.md)).
3. **Voice-accessible:** make Dragon aware via a `weather_local`
   tool that returns the latest reading.  The LLM can then answer
   "what's the temperature in here?"

Option 3 is the highest leverage — your sensor becomes part of the
AI's knowledge.

---

## Recipe 2 — Add a hardware button (panic / wake / mute)

### Hardware

- Momentary push button
- 10kΩ pull-up resistor (or use ESP32 internal pull)
- Wires to expansion port

Wire button between GPIO and GND; the firmware enables the internal
pull-up.

### Firmware

```c
#include "driver/gpio.h"

#define MOD_BTN_GPIO 19   // pick any free GPIO

static void mod_btn_task(void *arg) {
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << MOD_BTN_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,  // we'll poll for simplicity
    };
    gpio_config(&cfg);

    bool last = true;
    while (1) {
        bool pressed = !gpio_get_level(MOD_BTN_GPIO);  // active-low
        if (pressed && !last) {
            // Falling edge — debounce 50 ms then act
            vTaskDelay(pdMS_TO_TICKS(50));
            if (!gpio_get_level(MOD_BTN_GPIO)) {
                ESP_LOGI(TAG, "mod button pressed");
                // Trigger something — e.g., toggle mic mute
                tab5_settings_set_mic_mute(!tab5_settings_get_mic_mute());
            }
        }
        last = pressed;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
```

Spawn the task in `main()` after BSP init.

### Use cases

- **Panic button** — long-press to clear current voice/chat session, dismiss all overlays, force-reconnect WS.
- **Push-to-talk pedal** — saves your hands when typing/cooking.
- **Wake from sleep** (when sleep lands) — the touch screen is too sensitive for accidental wakes; a hardware button is more deliberate.
- **Privacy mute** — physical mute that the AI can't override.

---

## Recipe 3 — Add an external display / status LED

### Hardware

- WS2812B (NeoPixel) or APA102 strip
- Power: 5 V from expansion port if 1-2 LEDs; separate 5V regulator if more
- Data: any free GPIO

For NeoPixels you need precise timing — ESP32-P4's RMT peripheral
handles this.  Use the `led_strip` ESP-IDF component.

### Firmware

```c
#include "led_strip.h"

#define LED_GPIO    20
#define LED_COUNT   8

static led_strip_handle_t s_strip;

void mod_leds_init(void) {
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = LED_GPIO,
        .max_leds = LED_COUNT,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_WS2812,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .resolution_hz = 10 * 1000 * 1000,
    };
    led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
    led_strip_clear(s_strip);
}

// Call from voice state-change callback:
void mod_leds_voice_state(voice_state_t st) {
    switch (st) {
    case VOICE_STATE_LISTENING:
        led_strip_set_pixel(s_strip, 0, 255, 100, 0);  // orange
        break;
    case VOICE_STATE_PROCESSING:
        led_strip_set_pixel(s_strip, 0, 0, 100, 255);  // blue
        break;
    case VOICE_STATE_SPEAKING:
        led_strip_set_pixel(s_strip, 0, 100, 255, 100); // green
        break;
    default:
        led_strip_clear(s_strip);
        break;
    }
    led_strip_refresh(s_strip);
}
```

Hook into voice state via `voice.c`'s state-change callback.

---

## Recipe 4 — Add an extra IO expander (more digital I/O)

If you need more digital outputs than the 55 GPIOs allow (relays,
LEDs, motor controllers, etc.), add a third PI4IOE5V6416 (or
similar) on the system I2C bus.

### Hardware

- PI4IOE5V6416 IC (or PCA9555, MCP23017 — same idea)
- I2C address: pick one not in use.  System I2C addresses already
  taken: 0x10 (ES8388), 0x32 (RX8130 RTC), 0x36 (camera SCCB), 0x40
  (ES7210), 0x41 (INA226), 0x43 + 0x44 (existing IO expanders),
  0x68 (BMI270 IMU).  0x20-0x27 are the natural range for a third
  PI4IOE.
- 0.1 µF decoupling cap, pull-ups on SDA/SCL (already present
  on-board for system I2C).

### Firmware

Use the existing `pi4ioe.{c,h}` driver in `main/`.  Initialize a
new instance pointing at the new address; the rest is the same API
as the on-board expanders.

### Watch out for

- **Don't put high-current loads on the expander outputs.**  These
  drive ~25 mA each, max.  For relays/motors, drive a transistor
  from the expander pin.
- **System I2C bandwidth is finite.**  If the new expander gets
  polled at high frequency it'll contend with the touch controller
  (also on system I2C).  Touch latency will degrade visibly.

---

## Recipe 5 — Add an external sensor over UART

### Hardware

- Sensor with UART output (GPS, fingerprint reader, RFID reader, etc.)
- 4 wires: 5 V, GND, TX → ESP32-P4 RX, RX → ESP32-P4 TX
- Logic-level converter if the sensor uses 5 V signalling

### Firmware

Use ESP-IDF's UART driver.  ESP32-P4 has multiple UARTs; UART0 is
the default boot console (don't repurpose), UART1 + UART2 are
typically free.

```c
#include "driver/uart.h"

#define MOD_UART_NUM   UART_NUM_1
#define MOD_UART_TX    21
#define MOD_UART_RX    20
#define MOD_UART_BAUD  9600

void mod_uart_init(void) {
    uart_config_t cfg = {
        .baud_rate = MOD_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };
    uart_driver_install(MOD_UART_NUM, 1024, 0, 0, NULL, 0);
    uart_param_config(MOD_UART_NUM, &cfg);
    uart_set_pin(MOD_UART_NUM, MOD_UART_TX, MOD_UART_RX,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}

// Read in a task
static void mod_uart_task(void *arg) {
    uint8_t buf[256];
    while (1) {
        int n = uart_read_bytes(MOD_UART_NUM, buf, sizeof(buf), pdMS_TO_TICKS(1000));
        if (n > 0) {
            // Parse + act
            ESP_LOGI(TAG, "got %d bytes from sensor", n);
        }
    }
}
```

---

## Things that will brick / damage your device

| Don't | Why | What happens |
|-------|-----|--------------|
| Wire 5V to a 3.3V GPIO | ESP32-P4 GPIOs are NOT 5V tolerant | Latch-up; permanent damage |
| Short any I2C line to ground | Loads the system bus into stuck state | Touch dies, audio codecs lose config, IMU stops, RTC drifts; recoverable on reboot but annoying |
| Pull >500 mA from 3V3 | On-board regulator overheats | Brownout, random reboots |
| Pull >2 A from 5V (USB) | Tab5's USB power management has limits | Tab5 disconnects from USB host |
| Reuse GPIO 8-13, 15 | WiFi SDIO bus | WiFi dies; you'll think it's a software bug for hours |
| Reuse GPIO 39-44 | SD card SDMMC | SD card stops mounting |
| Reuse GPIO 26-30 | Audio I2S | Mic/speaker stops; codec init fails |
| Drive GPIO 36 from external source | Camera's 24 MHz clock | Camera initialization fails |
| Tap into MIPI-CSI / MIPI-DSI lanes | High-speed differential pairs | Display tears or camera misframes; possibly latch the SoC |
| Hot-plug 5 V to a powered-on Tab5 | Inrush current spike | Random reboot; rarely permanent damage |

If you've done one of these, factory-reset NVS (`POST /nvs/erase`)
and re-flash before drawing conclusions about whether the mod
"worked" — half the time the symptoms are software state, not
hardware damage.

---

## Distributing your mod

If you've built something useful, a few options for sharing:

1. **Custom firmware fork** — fork TinkerTab on GitHub, add your
   mod to a branch, `idf.py build` produces a `.bin` your friends can
   OTA into their Tab5.  Best for one-off / personal mods.
2. **Component contribution** — package the mod as an ESP-IDF
   component under `components/your-mod/` with proper
   `CMakeLists.txt` + `idf_component.yml`.  Submit a PR if it's
   broadly useful.
3. **Skill on Dragon side** — instead of firmware code, write a
   Python skill on Dragon that exposes your sensor data via a tool.
   No firmware change needed; works for any user with the same
   hardware.  Highest reuse, lowest install friction.  See
   [TinkerBox `docs/adding-a-tool.md`](https://github.com/lorcan35/TinkerBox/blob/main/docs/adding-a-tool.md).

---

## When something goes wrong

1. **Disconnect the mod.** Verify Tab5 boots normally without it.
2. **Read the serial log** at 115200 8N1 on `/dev/ttyACM0`.  Look
   for `ESP_LOG` errors at boot.  Common patterns:
   - `gpio: gpio_set_direction(...) GPIO already in use` → you
     reused a pinned GPIO
   - `i2c.master: I2C transaction unexpected NACK` → device not
     responding on the address you expected
   - `WatchDog Timer Group …` → your task is blocking too long
3. **`idf.py erase-flash flash` + reboot** → wipes NVS, restores
   defaults.
4. **Compare against the unmodded HEAD** — `git stash` your mod,
   build + flash, verify clean state.

If hardware damage is suspected (latch-up, regulator failure,
weird thermal hot-spots), unplug and contact M5Stack support if
you bought directly.  Self-installed mods are obviously not
covered by warranty but the support team is usually friendly
about diagnostics.

---

## See also

- [`HARDWARE.md`](HARDWARE.md) — authoritative pinout
- [`docs/dev-setup.md`](dev-setup.md) — how to build + flash
- [`CONTRIBUTING.md`](../CONTRIBUTING.md) — if your mod becomes a PR
- [`LEARNINGS.md`](../LEARNINGS.md) — read before you debug
- [TinkerBox `docs/adding-a-tool.md`](https://github.com/lorcan35/TinkerBox/blob/main/docs/adding-a-tool.md) — Dragon-side skill / tool patterns
- M5Stack Tab5 product page — for official schematic + revision history
