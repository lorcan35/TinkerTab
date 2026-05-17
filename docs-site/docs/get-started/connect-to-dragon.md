---
title: Connect to Dragon
sidebar_label: Connect to Dragon
---

# Connect to Dragon

Tab5 is the face; Dragon is the brain. Without a Dragon, the orb pulses but the LLM has no host. Here's how Tab5 finds Dragon.

## The discovery order

When Tab5 boots, the WebSocket client tries up to three hosts in order:

```
1. NVS-stored host:port  (Settings → Network)
2. mDNS lookup           (tinkerclaw.local on the LAN)
3. Public ngrok fallback (wss://tinkerclaw-voice.ngrok.dev:443)
```

The first one that responds wins. The session takes ~300 ms to establish on the LAN, ~1.5 s via ngrok.

## Easy path — same LAN, same subnet

If your Dragon is on the same Wi-Fi network as your Tab5, **you don't have to do anything**. mDNS picks it up automatically. Open the Settings overlay and you'll see a green "Dragon connected" pill.

## Manual host:port

If mDNS isn't working (some routers block multicast, some VLANs split traffic), set the host explicitly:

1. Tap the gear icon (top-right of home screen) to open Settings
2. Scroll to **Network**
3. Tap **Dragon host** → enter `192.168.1.91` (or your Dragon's IP)
4. Tap **Dragon port** → confirm `3502` (the default)
5. Tap **Reconnect**

The pill at the top should flip from grey to green within ~2 seconds.

## Remote / hybrid path — ngrok

If your Tab5 is at a friend's house but your Dragon is at home, you can still connect via the public ngrok tunnel. The Dragon needs to have `tinkerclaw-ngrok.service` running. From the Tab5:

1. Settings → Network
2. **Connection mode** → **Auto** (default — tries LAN first, then ngrok)

The ngrok connection adds ~50 ms latency vs LAN; usable but not snappy. Voice latency is dominated by the LLM anyway, so it's fine for ambient use.

## Verifying the connection

From any other machine on the LAN, you can ping Dragon's voice server:

```bash
curl -s http://192.168.1.91:3502/health | python3 -m json.tool
# → {"status": "ok", "uptime_s": 12345, "active_sessions": 1}
```

From the Tab5 itself, the **Voice state pill** at the top of the home screen tells you everything:

- **Grey** — not connected (Wi-Fi down, or Dragon unreachable)
- **Yellow** — connecting / handshake in progress
- **Green** — ready
- **Pulsing green** — listening / processing / speaking
- **Red** — error (tap for details)

## What if Dragon is just *down*?

Tab5 watches the connection every 5 seconds. If Dragon disappears (process crashed, power outage, Wi-Fi hiccup), the pill goes grey and Tab5 tries to reconnect with exponential backoff (10 s → 20 s → 40 s → 60 s max).

If you're in **Local** mode and the K144 LLM module is stacked on top, Tab5 transparently routes your next text turn through the K144 instead. You see a small *"Using onboard LLM"* toast. When Dragon comes back, the next turn returns to Dragon and shows *"Dragon reconnected"*.
