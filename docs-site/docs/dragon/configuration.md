---
title: Configuration
sidebar_label: Configuration
---

# Configuration

Dragon's behavior is controlled by `dragon_voice/config.yaml`. Validated on startup; bad keys log a clear error and abort.

![Settings overlay](/img/settings.jpg)

## Where it lives

```
~/projects/TinkerBox/dragon_voice/config.yaml      # source of truth
/home/radxa/dragon_voice/config.yaml               # deployed copy on Dragon
```

The deploy `scp` overwrites the deployed copy — so if you've made local edits on the Dragon, they'll get clobbered. Best practice: edit in the repo, scp, restart.

## Top-level structure

```yaml
voice:
  port: 3502
  ws_keepalive_s: 5

llm:
  backend: "ollama"           # or router / openrouter / lmstudio / npu_genie / tinkerclaw / dual
  ollama_model: "ministral-3:3b"
  ollama_url: "http://localhost:11434"
  openrouter_api_key: "${OPENROUTER_API_KEY}"
  openrouter_model: "anthropic/claude-3.5-haiku"
  fleet: []                   # populated when backend: "router"

stt:
  backend: "moonshine"
  moonshine_model: "tiny"

tts:
  backend: "piper"
  piper_voice: "en_US-amy-medium"
  piper_rate: 22050

memory:
  enabled: true
  embed_model: "nomic-embed-text"
  recall_threshold: 0.7
  facts_per_context: 8
  documents_per_context: 4

tools:
  enabled: true
  searxng_url: "http://localhost:8888"
  max_calls_per_turn: 3

scheduler:
  enabled: true
  runaway_cap_per_device: 100

mcp:
  enabled: false
  servers: []

api:
  bearer_token: "${DRAGON_API_TOKEN}"

ngrok:
  enabled: false
```

## Environment-variable substitution

`${VAR_NAME}` in any field is substituted at load time from the process environment. Systemd's `EnvironmentFile=/home/radxa/.env` populates these without the secret hitting `config.yaml` (which is in git).

`.env` is **not** overwritten by `scp -r dragon_voice/` deploys — secrets survive code pushes.

## Per-device + per-session overrides

You can also override config keys per-device or per-session via the REST API:

```bash
# Per-session: this conversation uses claude-sonnet
curl -X PUT -H "Authorization: Bearer $TOKEN" \
     -d '{"value":"anthropic/claude-sonnet-4.6","scope":"session","scope_id":"<session_id>"}' \
     http://192.168.1.91:3502/api/v1/config/llm.openrouter_model

# Per-device: this Tab5 always uses Cloud mode
curl -X PUT -H "Authorization: Bearer $TOKEN" \
     -d '{"value":2,"scope":"device","scope_id":"<device_id>"}' \
     http://192.168.1.91:3502/api/v1/config/voice_mode
```

Resolution order: session > device > global. More specific wins.

## Hot reload

Most config changes require a `systemctl restart tinkerclaw-voice`. The few exceptions:

- LLM backend swap (via WS `config_update` from Tab5) — happens live, no restart
- `recall_threshold` — re-read on next memory query
- `runaway_cap_per_device` — re-read on next notification schedule

When in doubt, restart.

## Validation

`python3 -c 'from dragon_voice.config import load_config; load_config()'` validates the file and exits cleanly on success, prints the validation error and exits non-zero on failure.

CI runs this check on every PR that touches `config.yaml`.

## Common gotchas

- **`ollama_url` must be `localhost`** if Ollama is on the same Dragon. Don't put the Dragon's LAN IP here — Ollama only listens on `127.0.0.1` by default for security.
- **`openrouter_api_key` must start with `sk-or-v1-`**. If it starts with `sk-` (OpenAI raw key), you'll get cryptic 401s; OpenRouter wraps OpenAI but uses its own keys.
- **`piper_voice` must match a downloaded ONNX file** under `~/.local/share/piper/`. List with `ls ~/.local/share/piper/`.
- **`fleet[]` is only used when `backend: "router"`.** With other backends, populating `fleet` does nothing (silently). Don't get confused.
