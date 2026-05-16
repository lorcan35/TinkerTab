---
title: Troubleshooting
sidebar_label: Troubleshooting
---

# Troubleshooting

:::note Curated from project sources
This page is curated from the project's `CLAUDE.md` runbook + `LEARNINGS.md` + `docs/*` trees. The internal versions are the source of truth; this page reflects them as of 2026-05-01.
:::

Sorted by symptom. If your problem isn't here, file an issue in the [TinkerTab](https://github.com/lorcan35/TinkerTab/issues) (firmware) or [TinkerBox](https://github.com/lorcan35/TinkerBox/issues) (server) tracker.

## Tab5 boot loop right after a flash

**Most common cause**: a new BSS-static allocation pushed the FreeRTOS timer task over its internal SRAM stack canary. MCAUSE 0x1b in the panic frame.

Fix:

1. Identify the offending static. `grep -nE 'static [a-zA-Z_]+_t s_[a-zA-Z_]+' main/*.c` is a good starting heuristic.
2. Convert to PSRAM-lazy: `static T *s_x = NULL;` + lazy `heap_caps_calloc(... MALLOC_CAP_SPIRAM ...)` on first use.

See [stability guide rule 1](/docs/firmware-reference/stability-guide#rule-1--bss-static-caches-18-kb-push-the-boot-timer-task-over-its-sram-canary).

## Tab5 reboots randomly after ~3 minutes of use

Internal SRAM fragmentation from overlay create/destroy. The fragmentation watchdog in `heap_watchdog.c` triggers a controlled reboot when the largest free internal block stays under 30 KB for 3 minutes.

Fix: convert overlay create/destroy to hide/show. See [LVGL conventions](/docs/firmware-reference/lvgl-conventions#3-hideshow-overlays-dont-destroycreate).

## Voice WS won't connect

**Check 1**: is Dragon up?

```bash
curl http://192.168.1.91:3502/health
```

**Check 2**: is mDNS working?

```bash
avahi-resolve -n tinkerclaw.local
```

If mDNS fails, set the IP manually in Settings → Network → Dragon host.

**Check 3**: ngrok fallback. If the LAN host is unreachable, Tab5 should try `wss://tinkerclaw-voice.ngrok.dev:443`. Verify by checking Tab5's voice state pill at the top of the home screen — yellow = connecting, green = ready.

## LLM seems to hang for >5 minutes

Local mode model is too big for available RAM. Check `ollama ps` on Dragon.

```bash
ssh radxa@192.168.1.91 'ollama ps'
```

If a 4B-class model is being evicted, switch to `ministral-3:3b` (the current default, ~2.8 GB).

Or move that model to the cloud-tier in fleet config.

## Tool calls "succeed" but reply is empty

Some FC-trained models emit a tool call and stop. The empty-reply guard in `dragon_voice/tools/response_wrap.py` should synthesize a one-line natural-language ack from the tool result.

If you're seeing empty replies anyway:

1. Check the tool returns an `"ack"` field in its result. Without it, the wrap has nothing to synthesize.
2. Add a per-tool wrap function in `response_wrap.py` if the default isn't doing the right thing.

## Memory recall feels weak

Lower the threshold in `config.yaml`:

```yaml
memory:
  recall_threshold: 0.6   # default 0.7
```

A lower threshold is more permissive — more facts get injected.

## Cloud mode 401s

```
ERROR openrouter_llm.py: 401 Unauthorized
```

Verify `OPENROUTER_API_KEY` in `~/.env` on Dragon. Note: it must start with `sk-or-v1-` (OpenRouter), not `sk-` (raw OpenAI).

```bash
ssh radxa@192.168.1.91 'cat .env | grep OPENROUTER'
ssh radxa@192.168.1.91 'sudo systemctl restart tinkerclaw-voice'
```

## Dashboard shows "Disconnected"

The dashboard runs on a separate systemd unit (port 3500). It proxies API calls to the voice server (port 3502).

```bash
sudo systemctl status tinkerclaw-dashboard
sudo journalctl -u tinkerclaw-dashboard -n 50
```

If the dashboard process is dead, `systemctl restart tinkerclaw-dashboard`. Voice sessions stay up — they're on a different process.

## Camera viewfinder is blank

Most likely the SCCB I2C address is wrong. The SC202CS uses **0x36**, not 0x30.

`CONFIG_CAMERA_SC202CS=y` MUST be set in `sdkconfig.defaults`. Verify:

```bash
grep CAMERA_SC202CS build/config/sdkconfig.h
```

If it's `=n` or missing, edit `sdkconfig.defaults` and `idf.py fullclean build`.

## K144 stuck in UNAVAILABLE

Tap the K144 health chip in Settings to trigger a software reset (`POST /m5/reset`). Live timing on hardware: ~9.6 s to recover.

If software reset doesn't help, power-cycle the K144 by unplugging its top USB-C for 30 s.

If it's *still* unavailable, the AX630C might be in a wedged state. Use ADB:

```bash
sudo adb kill-server && sudo adb start-server
sudo adb shell
# Inside the K144 shell:
systemctl restart llm-llm
```

## Audio is 1.5× speed (very specific old bug)

If you're running pre-2026 firmware, you might hit the upsample buffer halving bug. Update to current main; the fix is in voice.c (UPSAMPLE_BUF_CAPACITY 8192 → 16384).

## Speaker buzzes at boot

IO Expander 1 P1 (`SPK_EN`) wasn't initialized LOW. Update to current firmware — the fix is in service_audio.c init.

## Camera reboots Tab5

Hot-plugging at boot can collapse the 5 V rail. Use `EXT5V_EN` gating:

1. Boot Tab5 first
2. *Then* plug in the Grove sensor

Or wait for the firmware to flip `EXT5V_EN` HIGH after stable boot — usually within a few seconds.

## Dragon high RAM

Multiple LLM models resident in Ollama. Check + evict:

```bash
ssh radxa@192.168.1.91 'ollama ps'
ssh radxa@192.168.1.91 'ollama stop <model-name>'
```

Or set `keep_alive_s: 60` in your fleet config so Ollama auto-evicts after 60 s of idle.

## Last resort

Three layers of recovery:

1. **Tab5 reset pinhole** — clean reboot, retains user data
2. **Tab5 OTA rollback** — `curl -X POST http://<tab5-ip>:8080/ota/rollback -H "Authorization: Bearer $TOKEN"`
3. **Re-flash via USB** — `idf.py -p /dev/ttyACM0 flash`

For Dragon: `sudo systemctl restart tinkerclaw-voice tinkerclaw-dashboard`. The whole state lives in `~/.tinkerclaw/`; the services are stateless.
