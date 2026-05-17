---
title: NVS keys
sidebar_label: NVS keys
---

# NVS keys

:::note Curated from project sources
This page is curated from the project's `CLAUDE.md` runbook + `LEARNINGS.md` + `docs/*` trees. The internal versions are the source of truth; this page reflects them as of 2026-05-01.
:::

All settings live in the **`"settings"`** NVS namespace. Max key length is 15 chars (NVS-imposed limit). Read/write via `tab5_settings_get_X()` / `tab5_settings_set_X()` helpers in [`main/settings.{c,h}`](https://github.com/lorcan35/TinkerTab/blob/main/main/settings.h).

## Connectivity

| Key | Type | Default | Range | Description |
|-----|------|---------|-------|-------------|
| `wifi_ssid` | str | `TAB5_WIFI_SSID` (config.h) | — | WiFi network SSID |
| `wifi_pass` | str | `TAB5_WIFI_PASS` (config.h) | — | WiFi network password |
| `dragon_host` | str | `TAB5_DRAGON_HOST` (config.h) | — | Dragon server hostname/IP |
| `dragon_port` | u16 | `TAB5_DRAGON_PORT` (config.h) | 1–65535 | Dragon server port |
| `dragon_tok` | str | `""` | — | Dragon REST API bearer token. Empty by default; provisioned via `POST /settings`. |
| `conn_m` | u8 | 0 | 0–2 | Connection mode: 0=auto (ngrok first then LAN), 1=local only, 2=remote only |
| `device_id` | str | MAC-derived (12 hex chars) | — | Unique device identifier, auto-generated on first boot |
| `session_id` | str | `""` | — | Dragon conversation session ID for resume |

## Display + audio

| Key | Type | Default | Range | Description |
|-----|------|---------|-------|-------------|
| `brightness` | u8 | 80 | 0–100 | Display brightness percentage |
| `volume` | u8 | 70 | 0–100 | Speaker volume percentage |
| `mic_mute` | u8 | 0 | 0–1 | Master mic mute. `voice_start_listening` refuses with a toast if set. |
| `cam_rot` | u8 | 0 | 0–3 | Camera rotation in 90° steps (0=none, 1=90°CW, 2=180°, 3=270°CW) |

## Voice + LLM

| Key | Type | Default | Range | Description |
|-----|------|---------|-------|-------------|
| `vmode` | u8 | 0 | 0–4 | Voice mode: 0=local, 1=hybrid, 2=cloud, 3=TinkerClaw, 4=onboard (K144) |
| `llm_mdl` | str | `anthropic/claude-3.5-haiku` | — | LLM model identifier for cloud mode |

## Quiet hours

| Key | Type | Default | Range | Description |
|-----|------|---------|-------|-------------|
| `quiet_on` | u8 | 0 | 0–1 | Quiet hours master switch |
| `quiet_start` | u8 | 22 | 0–23 | Quiet-hours start hour (local clock, 24h) |
| `quiet_end` | u8 | 7 | 0–23 | Quiet-hours end hour; can wrap past midnight |

## Sovereign-Halo dials (v4·D)

| Key | Type | Default | Range | Description |
|-----|------|---------|-------|-------------|
| `int_tier` | u8 | 0 | 0–2 | Intelligence dial (0=fast, 1=balanced, 2=smart) |
| `voi_tier` | u8 | 0 | 0–2 | Voice dial (0=local Piper, 1=neutral, 2=studio OpenRouter) |
| `aut_tier` | u8 | 0 | 0–1 | Autonomy dial (0=ask first, 1=agent mode) |

## Spend caps

| Key | Type | Default | Range | Description |
|-----|------|---------|-------|-------------|
| `spent_mils` | u32 | 0 | 0–UINT32_MAX | Today's cumulative LLM spend, in mils (1/1000 ¢). Resets when `spent_day` rolls. |
| `spent_day` | u32 | 0 | 0–UINT32_MAX | Days-since-epoch of the last `spent_mils` write — the dayroll guard. |
| `cap_mils` | u32 | 100000 | 0–UINT32_MAX | Per-day spend cap in mils (default $1.00/day). Exceeding it triggers cap_downgrade. |

## Lifecycle + auth

| Key | Type | Default | Range | Description |
|-----|------|---------|-------|-------------|
| `auth_tok` | str | auto-generated (32 hex chars) | — | Debug server bearer auth token, generated on first boot |
| `onboard` | u8 | 0 | 0–1 | Onboarding-flow completion marker |
| `star_skills` | str | `""` | — | Comma-separated list of starred skill names. Toggled by tap on a skill card. |

## Read all keys

```bash
curl -s -H "Authorization: Bearer $TOKEN" \
     http://<tab5-ip>:8080/settings | python3 -m json.tool
```

Returns every key in the `settings` namespace with its current value.

## Write a single key

```bash
curl -X POST -H "Authorization: Bearer $TOKEN" \
     -d '{"key":"vmode","value":2}' \
     http://<tab5-ip>:8080/settings
```

Type-checks the value against the key's expected type. Returns `4xx` on type mismatch.

## Erase all settings

Last-resort recovery:

```bash
curl -X POST -H "Authorization: Bearer $TOKEN" \
     http://<tab5-ip>:8080/nvs/erase
```

This wipes the entire `settings` namespace. The device will boot into the onboarding flow on next start. Auth token survives because it's regenerated on first boot.
