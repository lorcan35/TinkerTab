# TinkerTab documentation

> **Part of the Tinker stack** — four repos, each documented on its own:
> [**TinkerTab**](https://github.com/lorcan35/TinkerTab) (Tab5 device firmware) ·
> [**TinkerBox**](https://github.com/lorcan35/TinkerBox) (Dragon inference server — "the brain") ·
> [**PingOS**](https://github.com/lorcan35/PingOS) (portable Tab5 OS) ·
> [**TinkerClaw**](https://github.com/lorcan35/TinkerClaw) (agent sidecar).
> New here? Each repo's README is its own front door.

This is the documentation map for TinkerTab — the ESP32-P4 firmware for the
M5Stack [Tab5](../GLOSSARY.md). The docs follow [Diátaxis](https://diataxis.fr):
four content types, each with one job, never mixed. Find your audience below, or
browse by type.

## I want to…

- **Use it** → [Get started](tutorials/getting-started.md)
- **Build / modify it** → [Flash the firmware](how-to/flash-firmware.md)
- **Integrate with it** → [Debug server reference](reference/debug-server.md)
- **Understand it** → [How the stack fits together](explanation/how-the-stack-fits-together.md)

## By Diátaxis type

### Tutorials — learning by doing (audience: tinkerer)

| Page | What you get |
|---|---|
| [Get started](tutorials/getting-started.md) | Unbox → flash → first boot → "Hey Tinker" → first voice turn. |

### How-to guides — task-oriented (audience: developer / operator / integrator)

| Page | When to use it |
|---|---|
| [Flash the firmware](how-to/flash-firmware.md) | Build and load the firmware onto a Tab5, with recovery + troubleshooting. |
| _dev-setup_ (planned, Wave 1) | Stand up a full development environment. |
| _deploy_ (planned, Wave 1) | Run it in production / fleet operation. |

### Reference — look up facts (audience: integrator / developer)

| Page | Covers |
|---|---|
| [Debug server](reference/debug-server.md) | The `:8080` HTTP control API — endpoints, bearer-token auth, the post-Wave-23b handler module map. |

### Explanation — understand why (audience: developer)

| Page | Helps you understand |
|---|---|
| [How the stack fits together](explanation/how-the-stack-fits-together.md) | What the Tab5, Dragon, and K144/TinkerON each do, and the six voice modes. |

> **Wave 1** fills out the four-audience content: hardware build/mods, voice/UI
> usage, troubleshooting, and architecture. The pages above are the validated
> seed pages — one per Diátaxis type — that prove the templates end-to-end.

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

These predate the Diátaxis restructure and remain in `docs/` for now:

- [`HARDWARE.md`](HARDWARE.md) · [`hardware-mods.md`](hardware-mods.md) ·
  [`VOICE_PIPELINE.md`](VOICE_PIPELINE.md) · [`WIDGETS.md`](WIDGETS.md) ·
  [`dev-setup.md`](dev-setup.md) · [`CHANGELOG.md`](CHANGELOG.md) ·
  [`STABILITY-INVESTIGATION.md`](STABILITY-INVESTIGATION.md) ·
  [`adr/`](adr/) (architecture decision records).
