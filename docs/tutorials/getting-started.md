---
audience: tinkerer
type: tutorial
prerequisites: A M5Stack Tab5, a USB-C cable, and a running Dragon server (see [TinkerBox](https://github.com/lorcan35/TinkerBox))
last-verified: 2026-05-29
est-time: 30 min
---
# Get started — from a fresh Tab5 to your first voice turn

By the end you will have flashed the firmware onto a brand-new
[Tab5](../../GLOSSARY.md), booted it, said "Hey Tinker," and heard it answer a
question out loud. This is a learning-by-doing walkthrough; every step has a
visible result so you know it worked.

The Tab5 is the **face** of the stack — it owns the screen, microphone, and
speaker, but the thinking happens on the [Dragon](../../GLOSSARY.md) server. You
need both running. If you only want the build details, jump to
[How to flash the firmware](../how-to/flash-firmware.md); to understand how the
pieces relate, read
[How the stack fits together](../explanation/how-the-stack-fits-together.md).

## Before you start

- An **M5Stack Tab5** (ESP32-P4) and a **USB-C cable**.
- A computer with **ESP-IDF v5.5.2** installed (this is the exact version the
  firmware is pinned to). If you do not have it yet:
  ```bash
  mkdir -p ~/esp && cd ~/esp
  git clone -b v5.5.2 --recursive https://github.com/espressif/esp-idf.git
  cd esp-idf && ./install.sh esp32p4
  ```
- A **Dragon server** reachable on your Wi-Fi, running
  [TinkerBox](https://github.com/lorcan35/TinkerBox). Note its IP address — you
  will point the Tab5 at it.
- Your **Wi-Fi SSID and password**.

## Steps

1. **Clone the firmware and enter it.**
   ```bash
   git clone https://github.com/lorcan35/TinkerTab.git
   cd TinkerTab
   ```
   _You should see:_ a `TinkerTab/` directory containing `main/`, `docs/`, and a
   top-level `CMakeLists.txt`.

2. **Tell the Tab5 your Wi-Fi and Dragon address.** Open `sdkconfig.defaults`
   and set:
   ```
   CONFIG_TAB5_WIFI_SSID="YourNetwork"
   CONFIG_TAB5_WIFI_PASS="YourPassword"
   CONFIG_TAB5_DRAGON_HOST="192.168.1.91"   # your Dragon's IP
   CONFIG_TAB5_DRAGON_PORT=3502
   ```
   _You should see:_ the four values updated in the file. (You can also change
   these later on-device in **Settings**.)

3. **Activate ESP-IDF in this terminal.**
   ```bash
   . ~/esp/esp-idf/export.sh
   ```
   _You should see:_ `Done! You can now compile ESP-IDF projects.`

4. **Build the firmware.**
   ```bash
   idf.py set-target esp32p4
   idf.py build
   ```
   _You should see:_ `Project build complete.` and a
   `build/tinkertab.bin` produced. The first build downloads components and
   takes several minutes.

5. **Flash it over USB.** Plug the Tab5 in, then:
   ```bash
   idf.py -p /dev/ttyACM0 flash
   ```
   (Use the port your board enumerated as — `ls /dev/ttyACM*` or
   `/dev/ttyUSB*`.)
   _You should see:_ `Hash of data verified.` and `Leaving... Hard resetting`.

6. **If the screen stays dark (ROM download mode), kick a watchdog reset.**
   ESP32-P4 boards sometimes need this after a flash:
   ```bash
   python -m esptool --chip esp32p4 -p /dev/ttyACM0 \
     --before no_reset --after watchdog_reset read_mac
   ```
   _You should see:_ the MAC address printed and the Tab5 boot. If the screen
   was already on, skip this step. (More fixes:
   [How to flash the firmware](../how-to/flash-firmware.md#troubleshooting).)

7. **Watch it boot.** The splash screen appears, then the home screen with the
   ambient orb. Within a few seconds the Wi-Fi indicator lights up and the Tab5
   connects to the Dragon over its [WebSocket](../../GLOSSARY.md).
   _You should see:_ the home screen with the orb breathing gently and a
   connected status.

8. **Say "Hey Tinker."** With the K144/[TinkerON](../../GLOSSARY.md) module the
   wakeword is always on; without it, tap the orb to start listening. Then ask a
   question, e.g. "Hey Tinker — what time is it?"
   _You should see:_ the orb shift to its LISTENING state, then PROCESSING,
   then SPEAKING as the Dragon's answer plays through the speaker.

## What you built

You now have a working Tab5 voice assistant: it captures your speech, streams it
to the Dragon for speech-to-text + LLM + text-to-speech, and plays the answer
back — all on your own hardware. A successful first turn looks like this on the
serial log (115200 baud):

```
I (xxxx) voice: state IDLE -> LISTENING
I (xxxx) voice: state LISTENING -> PROCESSING
I (xxxx) voice: state PROCESSING -> SPEAKING
I (xxxx) voice: state SPEAKING -> READY
```

## Next

- [How to flash the firmware](../how-to/flash-firmware.md) — the build/flash
  loop in detail, including recovery.
- [How the stack fits together](../explanation/how-the-stack-fits-together.md) —
  what the Tab5, Dragon, and K144/TinkerON each do.
- [Debug server reference](../reference/debug-server.md) — drive the Tab5
  remotely over HTTP for testing.
