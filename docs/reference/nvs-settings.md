---
audience: integrator
type: reference
prerequisites: none
last-verified: 2026-05-29
---
# NVS settings keys

Authoritative, lookup-oriented reference for the Tab5's persistent settings. No
tutorials here — to read or write these keys from a workstation, see the
[debug server reference](debug-server.md) (`GET`/`POST /settings`).

Every runtime setting on the [Tab5](../../GLOSSARY.md) lives in the
[NVS](../../GLOSSARY.md) (Non-Volatile Storage) flash key-value store. All keys
below live in the **`"settings"` namespace**. The ESP-IDF NVS API caps key
length at **15 characters** — that is why the keys are abbreviated (`vmode`,
`llm_mdl`, `dragon_tok`). The API is in
[`main/settings.h`](../../main/settings.h). Defaults shown as
`NAME (config.h)` are compiled in from
[`main/config.h`](../../main/config.h) (set via `sdkconfig.defaults` /
`menuconfig`) and only apply until the user overrides them at runtime.

## Reading and writing keys

Read every setting as JSON, or write one, over the debug server:

```bash
export TOKEN="abcdef1234567890abcdef1234567890"   # from the serial boot log

# Read all settings
curl -s -H "Authorization: Bearer $TOKEN" http://<ip>:8080/settings \
     | python3 -m json.tool

# Write one (or more) keys
curl -s -H "Authorization: Bearer $TOKEN" -X POST http://<ip>:8080/settings \
     -d '{"brightness":60,"vmode":1}'

# Wipe the whole namespace back to compiled defaults
curl -s -H "Authorization: Bearer $TOKEN" -X POST http://<ip>:8080/nvs/erase
```

A `nvs` observability event (`detail: "erase"`) fires on `POST /nvs/erase`; see
the debug server's [observability events](debug-server.md) section.

## Network + identity

| Key | Type | Default | Range | Description |
|---|---|---|---|---|
| `wifi_ssid` | str | `TAB5_WIFI_SSID` (config.h) | — | Wi-Fi network SSID. |
| `wifi_pass` | str | `TAB5_WIFI_PASS` (config.h) | — | Wi-Fi network password. |
| `dragon_host` | str | `TAB5_DRAGON_HOST` (config.h) | — | [Dragon](../../GLOSSARY.md) server hostname/IP. |
| `dragon_port` | u16 | `TAB5_DRAGON_PORT` (config.h) | 1–65535 | Dragon server port. |
| `device_id` | str | MAC-derived (12 hex chars) | — | Unique device identifier, auto-generated on first boot. |
| `session_id` | str | `""` (empty) | — | Dragon conversation session ID for resume. |
| `conn_m` | u8 | 0 | 0–2 | Connection mode: 0=auto (ngrok first then LAN), 1=local only, 2=remote only. Tab5-internal — never crosses the wire (see `voice_ws_start_client`). |
| `auth_tok` | str | auto-generated (32 hex chars) | — | Debug server bearer auth token, generated on first boot. See the [debug server reference](debug-server.md). |
| `dragon_tok` | str | `""` | — | Dragon REST API bearer token for outbound HTTP to gated endpoints (`/api/v1/tools`, `/api/v1/agent_log`, `/api/v1/sessions`, `/api/v1/memory`). Empty by default; provisioned via `POST /settings`. |

## Display + audio

| Key | Type | Default | Range | Description |
|---|---|---|---|---|
| `brightness` | u8 | 80 | 0–100 | Display brightness percentage. |
| `volume` | u8 | 70 | 0–100 | Speaker volume percentage. |
| `mic_mute` | u8 | 0 | 0–1 | Master mic mute — `voice_start_listening` refuses with a toast when set. |
| `cam_rot` | u8 | 0 | 0–3 | Camera frame rotation in 90° steps applied in software after capture (0=none, 1=90° CW, 2=180°, 3=270° CW). Settings dropdown. |

## Voice mode + LLM

| Key | Type | Default | Range | Description |
|---|---|---|---|---|
| `vmode` | u8 | 0 | 0–4 | [Voice mode](../../GLOSSARY.md): 0=local, 1=hybrid, 2=cloud, 3=TinkerClaw, 4=onboard (K144). Mode 5 (Solo Direct) is set via the same key and is Tab5-side-only. |
| `llm_mdl` | str | `anthropic/claude-3.5-haiku` | — | LLM model identifier for cloud mode. |

The on-the-wire `config_update` always carries a `voice_mode` in `0..3`; modes 4
(Onboard) and 5 (Solo Direct) are Tab5-side-only and downconvert to 0 over the
WebSocket. See the [voice-modes table](#voice-mode-vmode-values) below and the
[`config_update` glossary entry](../../GLOSSARY.md).

## Solo Direct mode (`or_*`)

These keys drive [voice mode 5 (Solo Direct)](../../GLOSSARY.md), where the Tab5
talks straight to OpenRouter with no Dragon. The model keys default to a
`~latest` alias that the alias resolver fills in.

| Key | Type | Default | Range | Description |
|---|---|---|---|---|
| `or_key` | str | `""` | — | OpenRouter API key. Empty disables Solo mode — `voice_modes_route_text` returns `SOLO_NO_KEY` and the UI prompts a QR scan. Provisioned via QR scan or `POST /settings`. |
| `or_mdl_llm` | str | `~latest` alias | — | OpenRouter LLM model for Solo mode. |
| `or_mdl_stt` | str | `~latest` alias | — | OpenRouter STT model (audio in). |
| `or_mdl_tts` | str | `~latest` alias | — | OpenRouter TTS model (audio out). |
| `or_mdl_emb` | str | `~latest` alias | — | OpenRouter embedding model for Solo-mode RAG. |
| `or_voice` | str | sensible default | — | OpenRouter TTS voice id. |
| `or_mdl_audio` | str | `~latest` alias | — | Multimodal audio chat model — one `/chat/completions` call replaces the STT → LLM → TTS chain in Solo mode. |

## Spend + intelligence dials

| Key | Type | Default | Range | Description |
|---|---|---|---|---|
| `int_tier` | u8 | 0 | 0–2 | Intelligence dial: 0=fast, 1=balanced, 2=smart. |
| `voi_tier` | u8 | 0 | 0–2 | Voice dial: 0=local Piper, 1=neutral, 2=studio OpenRouter. |
| `aut_tier` | u8 | 0 | 0–1 | Autonomy dial: 0=ask first, 1=agent mode. |
| `spent_mils` | u32 | 0 | 0–UINT32_MAX | Today's cumulative LLM spend in mils (1/1000 ¢). Resets when `spent_day` rolls to a new day. Wear-bounded to ~one commit per LLM turn. |
| `spent_day` | u32 | 0 | 0–UINT32_MAX | Days-since-epoch of the last `spent_mils` write — the dayroll guard. |
| `cap_mils` | u32 | 100000 | 0–UINT32_MAX | Per-day spend cap in mils (default $1.00/day). Exceeding it triggers `cap_downgrade` in `voice_send_config_update_ex`. |

## Onboarding + skills

| Key | Type | Default | Range | Description |
|---|---|---|---|---|
| `onboard` | u8 | 0 | 0–1 | Onboarding-flow completion marker (set once the user clears the welcome screens). |
| `star_skills` | str | `""` | — | Comma-separated list of starred (pinned) skill/tool names. Read by `ui_skills.c` to sort starred tools first + apply amber tint + "PINNED" caption. Toggled by tap or `POST /settings`. |

## Quiet hours

| Key | Type | Default | Range | Description |
|---|---|---|---|---|
| `quiet_on` | u8 | 0 | 0–1 | Quiet-hours master switch. |
| `quiet_start` | u8 | 22 | 0–23 | Quiet-hours start hour (local clock, 24h). |
| `quiet_end` | u8 | 7 | 0–23 | Quiet-hours end hour; can wrap past midnight. |

## Per-channel notification toggles

All `ch_*_on` keys default **off** — the user opts in per platform via
Settings → CHANNELS. `ui_notification.c` silently drops an incoming
[`channel_message`](../../GLOSSARY.md) whose `channel` field maps to a disabled
toggle (with a `ui.notif.channel_off` obs event). Unknown channels fail open.

| Key | Type | Default | Range | Description |
|---|---|---|---|---|
| `ch_tg_on` | u8 | 0 | 0–1 | Telegram notification toggle. |
| `ch_wa_on` | u8 | 0 | 0–1 | WhatsApp notification toggle. |
| `ch_dc_on` | u8 | 0 | 0–1 | Discord notification toggle. |
| `ch_sl_on` | u8 | 0 | 0–1 | Slack notification toggle. |
| `ch_sg_on` | u8 | 0 | 0–1 | Signal notification toggle. |
| `ch_im_on` | u8 | 0 | 0–1 | iMessage notification toggle. |
| `ch_ma_on` | u8 | 0 | 0–1 | Matrix notification toggle. |
| `ch_em_on` | u8 | 0 | 0–1 | Email notification toggle. |

## Voice mode (`vmode`) values

The `vmode` key encodes the six-tier voice mode. Values 4 and 5 are
Tab5-side-only; the wire only ever sees `0..3`.

| `vmode` | Mode | STT | LLM | TTS |
|---|---|---|---|---|
| 0 | Local | Moonshine | Dragon-local (NPU / llama-server / Ollama) | Piper |
| 1 | Hybrid | OpenRouter `gpt-audio-mini` | Dragon-local | OpenRouter `gpt-audio-mini` |
| 2 | Cloud | OpenRouter `gpt-audio-mini` | User-selected (`llm_mdl`) | OpenRouter `gpt-audio-mini` |
| 3 | TinkerClaw | Moonshine (or OpenRouter) | TinkerClaw gateway | Piper (or OpenRouter) |
| 4 | Onboard (K144) | K144 sherpa-ncnn ASR (chain) or text-only | K144 stacked LLM (qwen2.5-0.5B over UART) | K144 `single_speaker_english_fast` |
| 5 | Solo Direct | OpenRouter (`or_mdl_stt`) | OpenRouter (`or_mdl_llm`) | OpenRouter (`or_mdl_tts`, `or_voice`) |

## Notes

- **Key length.** Keys are capped at 15 characters by the NVS API — do not add a
  longer key.
- **String vs scalar.** `str` keys hold UTF-8 byte strings; `u8`/`u16`/`u32`
  are unsigned integers of that width.
- **`config.h` defaults** apply only until the user (or a `POST /settings`) sets
  a runtime value. After that, the NVS value wins across reboots.
- **Token security.** `auth_tok` and `dragon_tok` are bearer secrets — treat a
  `GET /settings` dump (which includes them) as sensitive.

## Examples

Switch to Hybrid mode and dim the screen in one write:

```bash
curl -s -H "Authorization: Bearer $TOKEN" -X POST http://<ip>:8080/settings \
     -d '{"vmode":1,"brightness":50}'
# → {"ok":true}
```

Provision Solo Direct mode without the QR flow:

```bash
curl -s -H "Authorization: Bearer $TOKEN" -X POST http://<ip>:8080/settings \
     -d '{"vmode":5,"or_key":"sk-or-v1-...","or_mdl_llm":"openai/gpt-4o-mini"}'
```

Opt in to Telegram notifications:

```bash
curl -s -H "Authorization: Bearer $TOKEN" -X POST http://<ip>:8080/settings \
     -d '{"ch_tg_on":1}'
```
