---
title: Install Dragon
sidebar_label: Install Dragon
---

# Install Dragon

Dragon Q6A is the brain. This page covers installing the [TinkerBox](https://github.com/lorcan35-/TinkerBox) server on a fresh Linux box.

## Requirements

- **OS** — Ubuntu 22.04 / 24.04 or Debian 12, AMD64 or ARM64
- **RAM** — 12 GB+ (8 GB minimum but you'll want headroom for the LLM)
- **Storage** — 30 GB+ (Ollama models alone are ~10 GB; conversation history accumulates)
- **Network** — Wired ethernet preferred; static IP recommended
- **Optional** — Qualcomm QCS6490 NPU (Dragon Q6A) for ~8 tok/s NPU inference; Nvidia GPU (works with LM Studio backend on the LAN tier)

## Quick path

```bash
# 1. Clone
git clone https://github.com/lorcan35/TinkerBox.git
cd TinkerBox

# 2. System deps
sudo apt install -y python3.12 python3.12-venv python3-pip \
                    fonts-dejavu-core ffmpeg \
                    avahi-daemon \
                    sqlite3

# 3. Python deps
pip install --break-system-packages -r requirements.txt

# 4. Ollama (for local LLM + embeddings)
curl -fsSL https://ollama.com/install.sh | sh
ollama pull ministral-3:3b
ollama pull nomic-embed-text       # for memory + RAG embeddings

# 5. (Optional) SearXNG for web search
docker run -d --name searxng -p 8888:8080 \
       -v ./searxng:/etc/searxng \
       searxng/searxng

# 6. Initial config
cp dragon_voice/config.example.yaml dragon_voice/config.yaml
# edit dragon_voice/config.yaml — see configuration page

# 7. Schema
sqlite3 ~/.tinkerclaw/dragon.db < schema.sql

# 8. Test run
python3 -m dragon_voice
# → starts on port 3502, watch the logs for green "session_start" messages
```

## Production install (systemd)

```bash
# 9. Copy systemd units
sudo cp systemd/tinkerclaw-voice.service     /etc/systemd/system/
sudo cp systemd/tinkerclaw-dashboard.service /etc/systemd/system/
sudo cp systemd/tinkerclaw-mdns.service      /etc/systemd/system/

# 10. Environment file (API keys, etc.)
sudo install -m 600 -o $USER /dev/stdin /home/$USER/.env <<EOF
OPENROUTER_API_KEY=sk-or-v1-...
DRAGON_API_TOKEN=$(openssl rand -hex 32)
TINKERCLAW_TOKEN=$(openssl rand -hex 32)
EOF

# 11. Enable + start
sudo systemctl daemon-reload
sudo systemctl enable --now tinkerclaw-voice tinkerclaw-dashboard tinkerclaw-mdns
sudo systemctl status tinkerclaw-voice
```

## Verify

```bash
# Voice server up
curl -s http://localhost:3502/health | jq

# Dashboard up
curl -sI http://localhost:3500/ | head -1

# mDNS advertising
avahi-browse -a -t -r | grep tinkerclaw

# Local LLM responds
curl -s http://localhost:11434/api/tags | jq '.models[].name'
```

If all four checks pass, point your Tab5 at this Dragon (Settings → Network → Dragon host) and tap the orb.

## Optional: ngrok for remote access

```bash
# Install ngrok per https://ngrok.com/download
# Then deploy our 3-tunnel config:
sudo cp systemd/tinkerclaw-ngrok.service /etc/systemd/system/
sudo systemctl enable --now tinkerclaw-ngrok
```

This gives you `tinkerclaw-voice.ngrok.dev`, `tinkerclaw-dashboard.ngrok.dev`, `tinkerclaw-gateway.ngrok.dev` as public HTTPS endpoints. Tab5's auto-mode will use these if LAN connectivity drops.

## Optional: Qualcomm NPU (for Dragon Q6A only)

See [`docs/npu-setup.md`](https://github.com/lorcan35/TinkerBox/blob/main/docs/npu-setup.md) for QAIRT SDK install + Llama 3.2 1B → Genie format conversion. ~8 tok/s vs ~0.24 tok/s on ARM64 CPU — a 30× speedup.

## Next

- [Configuration](/docs/dragon/configuration) — `config.yaml` reference
- [LLM backends](/docs/dragon/llm-backends) — pick which model runs
- [Voice modes config](/docs/dragon/voice-modes-config) — STT/TTS/LLM tuples per tier
- [Logs & monitoring](/docs/dragon/logs-monitoring) — keeping it healthy
