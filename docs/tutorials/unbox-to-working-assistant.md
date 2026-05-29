---
audience: tinkerer
type: tutorial
prerequisites: An M5Stack Tab5 + USB-C cable, a Dragon (or any Linux box) for the brain, and a computer to flash from
last-verified: 2026-05-29
est-time: 60 min
---
# From unboxing to a working voice assistant

By the end you will have built the whole stack from scratch: flashed firmware
onto a brand-new [Tab5](../../GLOSSARY.md), brought up a
[Dragon](../../GLOSSARY.md) to be its brain, connected the two, and said
**"Hey Tinker"** to hold your first spoken conversation. This is a
learning-by-doing walkthrough; every milestone has a result you can see or hear
so you know it worked.

This is the **front door** of the docs. It is the one page that walks the full
journey across both halves of the stack. It does not go deep on any single
step — instead, each milestone hands you off to a focused guide if you want more
detail or hit trouble. Read the whole page once, then follow it with both
devices in front of you.

## How the stack fits, in one breath

The product is two devices talking over **one WebSocket**:

- **The Tab5 is the face.** It owns the screen, the microphone, and the speaker.
  It holds no intelligence — it captures your voice and plays back answers.
- **The Dragon is the brain.** Speech-to-text, the LLM, and text-to-speech all
  run there. It is a Radxa Dragon Q6A, but for this tutorial any Linux box with
  Python 3.12+ works.

You need both. The journey is three milestones — **flash the face**, **wake the
brain**, **introduce them** — and then you talk. For the full mental model, read
[How the stack fits together](../explanation/how-the-stack-fits-together.md);
you do not need it to finish this page.

## Before you start

- An **M5Stack Tab5** (ESP32-P4) and a **USB-C cable**.
- A computer with **ESP-IDF v5.5.2** installed — the exact version the firmware
  is pinned to. If you do not have it:
  ```bash
  mkdir -p ~/esp && cd ~/esp
  git clone -b v5.5.2 --recursive https://github.com/espressif/esp-idf.git
  cd esp-idf && ./install.sh esp32p4
  ```
- A **Dragon** (or any Linux box) with **Python 3.12+** and network access. For
  cloud voice modes you also want an **OpenRouter API key**; the default Local
  mode needs no key.
- Your **Wi-Fi SSID and password**. Both devices must be on the **same network**.

---

## Milestone 1 — Flash the face

You will turn a bare Tab5 into a booted device showing its home screen.

1. **Clone the firmware and enter it.**
   ```bash
   git clone https://github.com/lorcan35/TinkerTab.git
   cd TinkerTab
   ```
   _You should see:_ a `TinkerTab/` directory containing `main/`, `docs/`, and a
   top-level `CMakeLists.txt`.

2. **Tell the Tab5 your Wi-Fi (and a placeholder Dragon address).** Open
   `sdkconfig.defaults` and set:
   ```
   CONFIG_TAB5_WIFI_SSID="YourNetwork"
   CONFIG_TAB5_WIFI_PASS="YourPassword"
   CONFIG_TAB5_DRAGON_HOST="192.168.1.91"   # update in Milestone 3 once you know it
   CONFIG_TAB5_DRAGON_PORT=3502
   ```
   The Dragon address can stay a placeholder for now — you will set the real one
   in Milestone 3, on-device, without reflashing.
   _You should see:_ the four values updated in the file.

3. **Activate ESP-IDF in this terminal** (do this in every new shell):
   ```bash
   . ~/esp/esp-idf/export.sh
   ```
   _You should see:_ `Done! You can now compile ESP-IDF projects.`

4. **Build the firmware.** The first build downloads components and takes
   several minutes.
   ```bash
   idf.py set-target esp32p4
   idf.py build
   ```
   _You should see:_ `Project build complete.` and a `build/tinkertab.bin`.

5. **Flash it over USB.** Plug the Tab5 in, then flash (use the port your board
   enumerated as — check with `ls /dev/ttyACM* /dev/ttyUSB*`):
   ```bash
   idf.py -p /dev/ttyACM0 flash
   ```
   _You should see:_ `Hash of data verified.` and `Leaving... Hard resetting`.

6. **If the screen stays dark (ROM download mode), kick a watchdog reset.**
   ESP32-P4 boards commonly need this after a flash:
   ```bash
   python -m esptool --chip esp32p4 -p /dev/ttyACM0 \
     --before no_reset --after watchdog_reset read_mac
   ```
   _You should see:_ the MAC address printed and the Tab5 boot. Skip this if the
   screen already lit up.

**Milestone 1 result:** the Tab5 boots past the splash to the **home screen**
with the ambient orb breathing gently, and the Wi-Fi indicator lights up once it
joins your network. The connection status will still read *disconnected* — the
Dragon is not up yet. That is expected.

> Stuck on the build or flash? The full loop, recovery, and troubleshooting live
> in [How to flash the firmware](../how-to/flash-firmware.md).

---

## Milestone 2 — Wake the brain

You will get the Dragon voice server live on **port 3502**, answering a health
check over the network. This half of the journey lives in the
[**TinkerBox**](https://github.com/lorcan35/TinkerBox) repo — the brain is its
own project with its own front door.

1. **Follow TinkerBox's getting-started tutorial.** It walks you from a clone to
   the `dragon_voice` service running on port 3502:
   [TinkerBox — Get your Dragon running](https://github.com/lorcan35/TinkerBox/blob/main/docs/tutorials/get-dragon-running.md).

2. **Note the Dragon's IP address** on your network — you will point the Tab5 at
   it next. From the Dragon itself, `hostname -I` prints it; from your
   workstation you can find it on the same LAN with:
   ```bash
   ping radxa-dragon-q6a
   # or scan the subnet for the voice WS + dashboard + gateway ports
   nmap -p 22,3502,18789 --open <subnet>/24
   ```

3. **Confirm the brain answers** over the network from your workstation:
   ```bash
   curl -s http://<dragon-ip>:3502/health
   # → expect a JSON health response, not a connection refused
   ```

**Milestone 2 result:** `curl` to the Dragon's `:3502/health` returns a healthy
JSON response from another machine on the same network. The brain is awake and
listening.

> Putting the Dragon into always-on production (systemd, tunnels, OTA) is a
> separate task — see
> [TinkerBox — Deploy on a Dragon](https://github.com/lorcan35/TinkerBox/blob/main/docs/how-to/deploy-on-a-dragon.md).

---

## Milestone 3 — Introduce them

You will point the Tab5 at the Dragon and watch the WebSocket come up.

1. **On the Tab5, open Settings → Network.** (First-boot onboarding may have
   already walked you through Wi-Fi.)

2. **Set the Dragon host and port.** Enter the Dragon's IP from Milestone 2 and
   port **3502**. The Tab5 saves these to [NVS](../../GLOSSARY.md) and reconnects
   on its own.

3. **Watch the link come up.** The Tab5 holds exactly one persistent
   [WebSocket](../../GLOSSARY.md) to the Dragon at `ws://<dragon-ip>:3502/ws/voice`,
   registers the device, and starts a session.

**Milestone 3 result:** the home screen's status reads **connected**. You can
prove it from your workstation over the Tab5's
[debug server](../reference/debug-server.md) (the bearer token prints on the
serial boot log):

```bash
export TOKEN="<32-hex-token-from-serial-boot-log>"
curl -s -H "Authorization: Bearer $TOKEN" http://<tab5-ip>:8080/voice \
     | python3 -m json.tool
# → expect "connected": true and a state_name of READY (or IDLE)
```

> The three ways to set the address (on-device, at build time, over the debug
> server) plus connection troubleshooting are in
> [How to connect a Tab5 to a Dragon](../how-to/connect-to-dragon.md).

---

## Milestone 4 — Talk to it

Both halves are live and linked. Now hold a conversation.

1. **Start listening.** With the K144/[TinkerON](../../GLOSSARY.md) module the
   wakeword is always on — just say **"Hey Tinker."** Without it, **tap the orb**
   on the home screen.
   _You should see:_ the orb shift to its **LISTENING** state — it leans and
   ripples, and the prompt below it changes to show it is hearing you.

2. **Ask a question with a clear answer**, e.g. _"What time is it?"_ or
   _"What's the capital of France?"_ — then stop talking.
   _You should see and hear:_ the orb move to **PROCESSING** while the Dragon
   transcribes your speech and runs the LLM, then to **SPEAKING** as the answer
   plays through the speaker.

3. **Ask a follow-up** that depends on the first answer, e.g.
   _"And how many people live there?"_
   _You should see:_ the answer reference the previous turn — the Dragon keeps a
   session, so the conversation has memory.

**Milestone 4 result:** a spoken answer comes out of the Tab5's speaker. A
successful turn looks like this on the serial log (115200 baud):

```
I (xxxx) voice: state IDLE -> LISTENING
I (xxxx) voice: state LISTENING -> PROCESSING
I (xxxx) voice: state PROCESSING -> SPEAKING
I (xxxx) voice: state SPEAKING -> READY
```

---

## What you built

You now have a complete, self-hosted voice assistant: the Tab5 captures your
speech and streams it over one WebSocket to the Dragon, which runs
speech-to-text, the LLM, and text-to-speech, and streams the answer back to play
through the speaker. Both halves run on your own hardware — no cloud required,
though [cloud and hybrid modes](../how-to/switch-voice-modes.md) are a setting
away when you want faster or smarter models.

## Next

- [Your first voice conversation](your-first-voice-conversation.md) — the
  day-to-day rhythm: follow-ups, cancelling, typing, and dictation.
- [Switch voice modes](../how-to/switch-voice-modes.md) — trade privacy, speed,
  and cost across the six tiers (local, hybrid, cloud, onboard, solo).
- [Enable the TinkerON wakeword](../how-to/enable-tinkeron-wakeword.md) — go
  fully hands-free with "Hey Tinker."
- [The voice pipeline](../explanation/the-voice-pipeline.md) — what actually
  happens between your words and the answer.
- The brain's own docs live in
  [**TinkerBox**](https://github.com/lorcan35/TinkerBox) — start at its README.
