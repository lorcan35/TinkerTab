# TinkerTab documentation

> **Part of the Tinker stack** — four repos, each documented on its own:
> [**TinkerTab**](https://github.com/lorcan35/TinkerTab) (Tab5 device firmware) ·
> [**TinkerBox**](https://github.com/lorcan35/TinkerBox) (Dragon inference server — "the brain") ·
> [**PingOS**](https://github.com/lorcan35/PingOS) (website-to-API automation gateway) ·
> [**TinkerClaw**](https://github.com/lorcan35/TinkerClaw) (agent sidecar).
> New here? Each repo's README is its own front door.

This is the documentation map for TinkerTab — the ESP32-P4 firmware for the
M5Stack [Tab5](../GLOSSARY.md). The docs follow [Diátaxis](https://diataxis.fr):
four content types, each with one job, never mixed. Find your audience below, or
browse by type.

## I want to…

- **Use it** → [Get started](tutorials/getting-started.md)
- **Build / modify it** → [Set up a dev environment](how-to/dev-setup.md)
- **Integrate with it** → [Reference index](reference/README.md)
- **Run it in production** → [Run a Tab5 in production](how-to/deploy.md)
- **Understand it** → [How the stack fits together](explanation/how-the-stack-fits-together.md)

## By audience

- **Tinkerer / end-user** → [Get started](tutorials/getting-started.md),
  [Your first voice conversation](tutorials/your-first-voice-conversation.md),
  then [Switch voice modes](how-to/switch-voice-modes.md),
  [Use the camera](how-to/use-the-camera.md),
  [Dictate and take notes](how-to/dictate-and-take-notes.md),
  [Enable the TinkerON wakeword](how-to/enable-tinkeron-wakeword.md).
- **Developer / contributor** →
  [Set up a dev environment](how-to/dev-setup.md),
  [Flash the firmware](how-to/flash-firmware.md),
  [Run the e2e test harness](how-to/run-the-e2e-test-harness.md);
  [TinkerTab architecture](explanation/architecture.md),
  [The voice pipeline](explanation/the-voice-pipeline.md),
  [LVGL on ESP32-P4](explanation/lvgl-on-esp32p4.md);
  [Firmware file map](reference/firmware-file-map.md).
- **API / protocol integrator** →
  [WebSocket protocol](reference/websocket-protocol.md),
  [Debug server](reference/debug-server.md),
  [Voice modes](reference/voice-modes.md),
  [NVS settings](reference/nvs-settings.md),
  [Observability events](reference/observability-events.md),
  [Hardware](reference/hardware.md).
- **Operator** →
  [Connect a Tab5 to a Dragon](how-to/connect-to-dragon.md),
  [Run a Tab5 in production](how-to/deploy.md),
  [Recover a stuck device](how-to/recover-a-stuck-device.md);
  [The TinkerON / K144 chain](explanation/the-tinkeron-k144-chain.md).

## By Diátaxis type

### Tutorials — learning by doing (audience: tinkerer)

| Page | What you get |
|---|---|
| [From unboxing to a working voice assistant](tutorials/unbox-to-working-assistant.md) | **Start here.** The full end-to-end journey across both halves of the stack: flash the Tab5, bring up the Dragon, link them, and hold your first conversation. |
| [Get started](tutorials/getting-started.md) | Unbox → flash → first boot → "Hey Tinker" → first voice turn. |
| [Your first voice conversation](tutorials/your-first-voice-conversation.md) | Hold a real multi-turn conversation: ask, follow up, cancel, type, dictate. |

### How-to guides — task-oriented (audience: tinkerer / developer / operator)

| Page | When to use it | Audience |
|---|---|---|
| [Choose your voice mode](how-to/choose-your-voice-mode.md) | Decide *which* of the six tiers fits — privacy vs latency vs cost vs offline. | tinkerer |
| [Switch voice modes](how-to/switch-voice-modes.md) | Change the privacy/speed/cost trade-off across the six tiers. | tinkerer |
| [Use the camera](how-to/use-the-camera.md) | Take a photo, record video, send an image into chat. | tinkerer |
| [Dictate and take notes](how-to/dictate-and-take-notes.md) | Capture a longer thought hands-free → title + summary in Notes. | tinkerer |
| [Enable the TinkerON wakeword](how-to/enable-tinkeron-wakeword.md) | Go fully hands-free with "Hey Tinker" on the K144 module. | tinkerer |
| [Set up a dev environment](how-to/dev-setup.md) | Stand up the ESP-IDF toolchain to build and modify the firmware. | developer |
| [Flash the firmware](how-to/flash-firmware.md) | Build and load the firmware, with recovery + troubleshooting. | developer |
| [Run the e2e test harness](how-to/run-the-e2e-test-harness.md) | Drive a Tab5 through long user-story flows for regression testing. | developer |
| [Connect a Tab5 to a Dragon](how-to/connect-to-dragon.md) | Point a device at a Dragon — first-time, new network, or repair. | operator |
| [Run a Tab5 in production](how-to/deploy.md) | Operate one or more devices day-to-day (OTA, monitoring). | operator |
| [Recover a stuck device](how-to/recover-a-stuck-device.md) | The cheapest-first recovery ladder for a misbehaving device. | operator |

### Reference — look up facts (audience: integrator / developer)

| Page | Covers |
|---|---|
| [Reference index](reference/README.md) | The reference section's own map. |
| [WebSocket protocol](reference/websocket-protocol.md) | The Tab5 client side of the Dragon WebSocket — every frame and magic tag. |
| [Debug server](reference/debug-server.md) | The `:8080` HTTP control API — endpoints, bearer-token auth, the post-Wave-23b handler module map. |
| [Voice modes](reference/voice-modes.md) | The six tiers, on-the-wire behavior, routing codes, failover guards. |
| [NVS settings](reference/nvs-settings.md) | Every persistent settings key in the `"settings"` namespace. |
| [Observability events](reference/observability-events.md) | The `/events` ring — every event kind and detail format. |
| [Hardware](reference/hardware.md) | The Tab5 SoC, peripherals, audio pipeline, M5-Bus, and memory rules. |
| [Firmware file map](reference/firmware-file-map.md) | Where everything lives under `main/` — find the right home for a change. |

### Explanation — understand why (audience: developer)

| Page | Helps you understand |
|---|---|
| [The Tinker stack](explanation/the-tinker-stack.md) | The contributor's whole-system map: the four repos, who owns what, how data flows, and where each repo's docs live. |
| [How the stack fits together](explanation/how-the-stack-fits-together.md) | What the Tab5, Dragon, and K144/TinkerON each do, and the six voice modes. |
| [TinkerTab architecture](explanation/architecture.md) | How the firmware is layered internally and why the boundaries fall where they do. |
| [The voice pipeline](explanation/the-voice-pipeline.md) | How a turn becomes an answer — STT → LLM → TTS, the state machine, and the audio path. |
| [LVGL on ESP32-P4](explanation/lvgl-on-esp32p4.md) | Why the UI is hide/show, why `lv_async_call` is banned, and the render-budget footguns. |
| [The TinkerON / K144 chain](explanation/the-tinkeron-k144-chain.md) | How the on-device chain works, talks StackFlow, and self-heals. |

## Standards + shared assets

- [`STYLE.md`](../STYLE.md) — the cross-repo documentation writing standard.
- [`GLOSSARY.md`](../GLOSSARY.md) — canonical cross-stack terms.
- [`ROADMAP.md`](ROADMAP.md) — the documentation program waves.
- [`_templates/`](_templates/) — the four Diátaxis page templates. Start every
  new page from the matching template.

## Internal working docs

- [`internal/`](internal/) — plans, audits, and retrospectives (the engineering
  history). **Not audience documentation** — see
  [`internal/README.md`](internal/README.md) for the old→new map.

## Other in-repo docs (to be slotted into Diátaxis in later waves)

These predate or supplement the Diátaxis pages and remain in `docs/` for now:

- [`HARDWARE.md`](HARDWARE.md) · [`hardware-mods.md`](hardware-mods.md) ·
  [`VOICE_PIPELINE.md`](VOICE_PIPELINE.md) · [`WIDGETS.md`](WIDGETS.md) ·
  [`dev-setup.md`](dev-setup.md) · [`CHANGELOG.md`](CHANGELOG.md) ·
  [`STABILITY-INVESTIGATION.md`](STABILITY-INVESTIGATION.md) ·
  [`adr/`](adr/) (architecture decision records).
