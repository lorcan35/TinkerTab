---
audience: operator
type: how-to
prerequisites: A flashed [Tab5](../../GLOSSARY.md) and a reachable [Dragon](../../GLOSSARY.md) server running [TinkerBox](https://github.com/lorcan35/TinkerBox)
last-verified: 2026-05-29
est-time: 10 min
---
# How to connect a Tab5 to a Dragon

Use this when you need to point a [Tab5](../../GLOSSARY.md) at a
[Dragon](../../GLOSSARY.md) server — first-time setup, moving to a new network,
or repairing a connection that dropped. Assumes the firmware is already flashed
(see [Flash the firmware](flash-firmware.md)) and a Dragon running
[TinkerBox](https://github.com/lorcan35/TinkerBox) is reachable on your network
(its voice [WebSocket](../../GLOSSARY.md) listens on **port 3502**).

The Tab5 holds exactly one persistent WebSocket to the Dragon at
`ws://<host>:3502/ws/voice`. It tries the configured host on the LAN first and
falls back to the ngrok tunnel if `conn_m` allows it.

## Steps

You can set the Wi-Fi + Dragon address three ways. Pick one.

### Option A — on-device (no computer)

1. From the home screen, open **Settings → Network**.
2. Run the **Wi-Fi** flow: scan, pick your SSID, enter the password. (First-boot
   onboarding runs this automatically.)
3. Set the **Dragon host** to the Dragon's IP (or hostname) and the **port** to
   `3502`.
4. The Tab5 saves these to [NVS](../../GLOSSARY.md) (`wifi_ssid`, `wifi_pass`,
   `dragon_host`, `dragon_port`) and reconnects.

### Option B — at build time

Set the compiled defaults in `sdkconfig.defaults` before flashing:

```
CONFIG_TAB5_WIFI_SSID="YourNetwork"
CONFIG_TAB5_WIFI_PASS="YourPassword"
CONFIG_TAB5_DRAGON_HOST="192.168.1.91"   # your Dragon's IP
CONFIG_TAB5_DRAGON_PORT=3502
```

These are only the fallback — any value the user sets on-device (Option A) or
over the wire (Option C) wins and persists across reboots.

### Option C — over the debug server

Write the keys remotely with the [debug server](../reference/debug-server.md)
(needs the bearer token from the serial boot log):

```bash
export TOKEN="abcdef1234567890abcdef1234567890"
curl -s -H "Authorization: Bearer $TOKEN" -X POST http://<tab5-ip>:8080/settings \
     -d '{"dragon_host":"192.168.1.91","dragon_port":3502}'
# → {"ok":true}
```

## Verify it worked

1. **Find the Dragon first** if you do not know its IP. From a workstation on
   the same LAN:
   ```bash
   ping radxa-dragon-q6a
   # or scan the subnet for the voice WS + dashboard + gateway ports
   nmap -p 22,3502,18789 --open <subnet>/24
   ```
2. **Confirm the Tab5 connected** by reading its voice state over the debug
   server:
   ```bash
   curl -s -H "Authorization: Bearer $TOKEN" http://<tab5-ip>:8080/voice \
        | python3 -m json.tool
   # → expect "connected": true and a state_name of READY (or IDLE)
   ```
3. **Send a test turn** end-to-end:
   ```bash
   curl -s -H "Authorization: Bearer $TOKEN" -X POST http://<tab5-ip>:8080/chat \
        -d '{"text":"reply with PONG"}'
   # then re-read /voice for last_llm_text
   ```

## Troubleshooting

- **`/voice` shows `connected: false`** → The Dragon is unreachable. Confirm the
  host/port (`GET /settings`), that the Dragon's voice service is up
  (`systemctl status tinkerclaw-voice` on the Dragon), and that both are on the
  same network.
- **Connected, then drops repeatedly** → The reconnect watchdog re-dials with
  exponential backoff (10 s → 20 s → 40 s → 60 s). Persistent drops usually mean
  Wi-Fi signal or a Dragon restart. Force an immediate retry with
  `POST /voice/reconnect`.
- **LAN works at home but not remotely** → The connection mode key `conn_m`
  controls fallback: `0` = auto (LAN then ngrok), `1` = local only, `2` = remote
  only. For an off-LAN device, leave it at `0` so the ngrok tunnel
  (`wss://tinkerclaw-voice.ngrok.dev`) is available.
- **Wi-Fi never associates after a reflash** → Re-check the SSID/password and
  verify the ESP32-C6 SDIO co-processor is intact (Wi-Fi on the Tab5 runs
  through a hosted C6 — see the [hardware reference](../reference/hardware.md)).
- **Connected but every turn errors** → The link is fine but the pipeline is
  failing on the Dragon side. Check `last_stt_text` / `last_llm_text` in
  `/voice` and the Dragon logs (`journalctl -u tinkerclaw-voice`).

## See also

- [Switch voice modes](switch-voice-modes.md) · [Run a Tab5 in production](deploy.md)
- [Debug server reference](../reference/debug-server.md) ·
  [NVS settings reference](../reference/nvs-settings.md) (`dragon_host`,
  `dragon_port`, `conn_m`)
- [How the stack fits together](../explanation/how-the-stack-fits-together.md)
