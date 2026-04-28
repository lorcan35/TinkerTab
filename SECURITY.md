# Security — Tab5 Firmware

> Threat model, attack-surface map, auth-token lifecycle, and
> firmware-integrity status for the Tab5 ESP32-P4 firmware.
>
> The companion Dragon-side doc is
> [TinkerBox `SECURITY.md`](https://github.com/lorcan35/TinkerBox/blob/main/SECURITY.md);
> read both for the full picture.

---

## TL;DR honest assessment

The Tab5 firmware is **enthusiast-grade**, not appliance-grade.
Specifically:

- **No Secure Boot** — anyone with USB-JTAG access can flash arbitrary firmware
- **No Flash Encryption** — NVS contents (incl. WiFi password, Dragon address, debug bearer token) are recoverable via `esptool read_flash`
- **OTA SHA256 verification** exists (PR #SEC07) but the firmware itself isn't signed — protects against MitM-during-download, not against a compromised Dragon serving a malicious bundle
- **Plain `ws://` on LAN** — Tab5 ↔ Dragon traffic is unencrypted on your home network; encrypted only when using the ngrok fallback path

This is fine for "voice assistant in my house" and *not* fine if your
threat model includes hostile WiFi neighbours, untrusted physical
access to the device, or supply-chain firmware tampering.

---

## 1. Attack surfaces

```
┌──────────────────────────────────────────────────────────────┐
│ PHYSICAL ACCESS                                              │
│   USB-C → ESP32-P4 USB-JTAG                                  │
│     • Unrestricted: read_flash, write_flash, debug halt      │
│     • Recovery: erase NVS via debug server endpoint          │
│   I2C / SPI / SDIO test points (board-level)                 │
└──────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────┐
│ NETWORK (LAN-trusted by default)                             │
│   :8080 — debug HTTP server                                  │
│     • /info + /selftest: NO AUTH                             │
│     • everything else: bearer-token gated (auth_tok)         │
│     • Token is constant-time-compared                        │
│   :3502 (outbound) — voice WS to Dragon                      │
│     • Sends DRAGON_API_TOKEN as Authorization header on      │
│       WS upgrade                                             │
│     • Plain ws:// on LAN (no TLS)                            │
│     • wss:// only when conn_m=1 (force ngrok)                │
└──────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────┐
│ HARDWARE INPUTS                                              │
│   Mic (4-channel TDM) — only captured when voice_mode != 0   │
│     and the user explicitly initiates listening              │
│   Camera (SC202CS) — only initialised when entering camera   │
│     screen or video call                                     │
│   Touch (GT911) — capacitive; passive                        │
│   IMU (BMI270) — passive (auto-rotate, currently no-op)      │
└──────────────────────────────────────────────────────────────┘
```

**Implications for hostile-LAN scenarios:**

| Threat | Pre-condition | Mitigation |
|--------|--------------|------------|
| Anyone on WiFi reads Tab5 ↔ Dragon traffic | Plain WS on LAN, attacker on same network | Use `conn_m=1` to force ngrok WSS; or run Dragon on a VLAN |
| Anyone on WiFi takes over Tab5 via debug API | They need `auth_tok` (32-hex-char NVS value) | Token never logged unmasked; recoverable only via USB read_flash. LAN-only — not exposed via ngrok. |
| Anyone on WiFi reads Tab5 mic | Out of scope — Tab5 only sends mic frames *to Dragon over WS* and only when listening | Mic is software-controlled; can't be activated remotely without going through `voice.c`'s state machine + the explicit user-tap-the-orb flow |

---

## 2. Auth tokens

### `auth_tok` (debug HTTP server, port 8080)

- **Purpose:** Gates Tab5 firmware's debug HTTP server on port 8080.
- **Storage:** NVS partition (offset `0x9000`, size `0x6000`), namespace `"settings"`, key `"auth_tok"`.  32 hex chars (=128 bits of entropy).
- **Generation:** Auto-generated on first boot via `esp_random()` (CSPRNG-grade) if missing.  Persists across reboots; only NVS erase rotates it.
- **Public endpoints (NO auth):**
  - `GET /info` — uptime, heap, voice state, WiFi info, reset reason. No secrets.
  - `GET /selftest` — 8-check health probe. No secrets.
- **Logging discipline:** The token is *masked* in serial logs (e.g., `05ee****b9f2`).  The unmasked value is recoverable two ways:
  1. NVS dump via `esptool read_flash 0x9000 0x6000` (requires physical USB).
  2. Direct query to a deployed Tab5 if you already have the token (chicken-and-egg — useful for token rotation, not as an exfiltration vector).
- **If leaked:** Anyone on the LAN can drive the device remotely: tap, type, navigate, swap voice modes, take photos, reboot, even read NVS settings via `GET /settings` (which is bearer-gated).  *Not* a path to Dragon — `auth_tok` is Tab5-local; Dragon's access uses a different token.
- **Rotation:** Force NVS erase via `POST /nvs/erase` (bearer-gated, dangerous).  Tab5 reboots, generates a fresh token, prints the new masked value to serial log.

### `DRAGON_API_TOKEN` (sent to Dragon)

- **Purpose:** Tab5 sends this as the `Authorization: Bearer …` header on the WebSocket upgrade request to Dragon's `/ws/voice`.  Same token gates `/api/*` REST calls when Tab5 hits Dragon's media-upload endpoint, etc.
- **Storage:** Compiled in via `CONFIG_TAB5_DRAGON_TOKEN` (`sdkconfig.local`) — currently *not* in NVS.  This is mostly a deployment convenience; rotating means re-flashing.
- **If leaked:** Whoever has it can speak to Dragon as if they were Tab5.  Same impact as the Dragon-side `DRAGON_API_TOKEN` leak in [TinkerBox's SECURITY.md](https://github.com/lorcan35/TinkerBox/blob/main/SECURITY.md#dragon_api_token).

### `OPENROUTER_API_KEY` (Dragon-side; not on Tab5)

Tab5 **never** sees this.  Cloud-mode requests are made by Dragon, not Tab5.  Tab5 only knows there's a "Cloud mode" and that Dragon is doing something.

---

## 3. Network exposure

By design, Tab5 listens on exactly one TCP port:

| Port | Service | Bind | Auth | Exposure |
|------|---------|------|------|----------|
| 8080 | Debug HTTP server | `0.0.0.0` (LAN) | bearer-token (`auth_tok`); 2 endpoints public | LAN-only; not bridged to ngrok |

Tab5 makes outbound connections to:
- Dragon WebSocket on `:3502` (LAN or ngrok depending on `conn_m`)
- Dragon REST `/api/media/upload` for camera photos
- Dragon REST `/api/ota/check` and `/api/ota/firmware.bin` for OTA updates
- Dragon REST `/api/v1/notes` for offline-recorded notes

That's the entire network surface.  No mDNS browse, no UPnP, no
public DNS over the device.

If you want to *eliminate* the LAN-debug surface (e.g., for a
factory-mode build), set `CONFIG_TAB5_DEBUG_SERVER_DISABLED=y` in
`sdkconfig.local` (currently not implemented but a 1-hour change —
TODO).

---

## 4. Data inventory

### Persisted on Tab5

| Data | Storage | Retention | Cloud-exposed? |
|------|---------|-----------|----------------|
| WiFi SSID + password | NVS `wifi_ssid` / `wifi_pass` | Permanent | No |
| Dragon host + port + token | NVS `dragon_host` / `dragon_port` / Kconfig | Permanent | No |
| Debug bearer token | NVS `auth_tok` | Permanent | No |
| Voice mode preference + tier dials | NVS `vmode`, `int_tier`, `voi_tier`, `aut_tier` | Permanent | No |
| Daily LLM spend (Cloud-mode) | NVS `spent_mils` + `spent_day` | Resets at midnight | No |
| Daily spend cap | NVS `cap_mils` | Permanent | No |
| Camera rotation pref | NVS `cam_rot` | Permanent | No |
| Quiet hours config | NVS `quiet_on/start/end` | Permanent | No |
| Onboarding completion | NVS `onboard` | Permanent (until NVS erase) | No |
| Captured photos | `/sdcard/IMG_NNNN.jpg` | Until SD removed/cleaned | Yes when uploaded (Dragon stores 24h, then OpenRouter sees them in Cloud-mode vision turns) |
| Recorded videos | `/sdcard/VID_NNNN.MJP` | Until SD removed/cleaned | Yes if explicitly uploaded |
| Offline-queued audio notes | `/sdcard/REC/...` | Until uploaded + dragon-side cleaned | Same as photos when STT'd |
| Conversation history (cached) | In-memory (chat overlay) | Cleared on reboot | (lives on Dragon) |

### Sent over the network

When Tab5 is in **Local mode**, traffic to Dragon contains:
- Mic PCM frames (ephemeral; Dragon doesn't persist raw audio)
- Text input from chat
- Photo bytes when sharing to chat
- Telemetry: device_id, hardware_id (MAC), firmware version, capabilities, session_id

When Tab5 is in **Hybrid / Cloud mode**, the same traffic flows to Dragon, which then forwards audio + transcripts + photos to OpenRouter.

When Tab5 makes a **video call**, the camera + mic are streaming continuously to Dragon while the call is active.  Dragon broadcasts to other call participants but doesn't persist the video stream.

---

## 5. Firmware integrity

### Secure Boot — Disabled

ESP-IDF supports **Secure Boot V2**: the bootloader and app are
signed with an RSA-3072 or ECDSA-P256 key burned into eFuse.  We
don't currently enable this.

Implication: anyone with USB-JTAG can `idf.py flash` a malicious
firmware, including one that exfiltrates `auth_tok` over WiFi.

To enable: see [ESP-IDF Secure Boot V2 docs](https://docs.espressif.com/projects/esp-idf/en/v5.5.2/esp32p4/security/secure-boot-v2.html).
Roughly 4 hours engineering + a key-management story we don't currently have.

### Flash Encryption — Disabled

ESP-IDF supports **Flash Encryption**: the flash contents are
encrypted at rest with an AES key burned into eFuse.

Implication: NVS contents (`auth_tok`, `wifi_pass`, etc.) are
recoverable in plaintext via `esptool read_flash`.

To enable: ESP-IDF docs same as above.  Note that Flash Encryption +
Secure Boot are commonly enabled together.

### OTA Signature — Partial (SHA256 only)

What's implemented:
- Dragon serves `/api/ota/firmware.bin` with a paired `version.json` containing the SHA256.
- Tab5 downloads, hashes, compares; mismatch → `ESP_ERR_INVALID_CRC`, abort.
- ESP-IDF's `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` — new firmware boots in `PENDING_VERIFY`; if it crashes before `tab5_ota_mark_valid()`, bootloader auto-reverts to the previous slot.

What's *not* implemented:
- The firmware bundle isn't signed.  A compromised Dragon could serve any binary + matching hash and Tab5 would accept it.
- Bootloader signature verification (which is what Secure Boot V2 provides).

To harden: enable Secure Boot V2 + sign the OTA bundle end-to-end with the same key.

---

## 6. Physical access threats

If an attacker has Tab5 in their hands:

| Threat | Achievable via | Mitigation |
|--------|---------------|------------|
| Read all NVS (incl. WiFi password, Dragon address) | `esptool read_flash 0x9000 0x6000` | Flash Encryption (currently disabled) |
| Flash arbitrary firmware | `idf.py flash` | Secure Boot V2 (currently disabled) |
| Halt + inspect running firmware via JTAG | USB-JTAG | Not practical to mitigate without physical security |
| Physically eavesdrop on the mic | None — chip is hardwired | Don't put Tab5 in adversarial physical environments |

---

## 7. Known limitations / non-goals

| Limitation | Tracked? | Workaround |
|-----------|----------|------------|
| Plain `ws://` on LAN | Open issue | `conn_m=1` to force ngrok WSS |
| `auth_tok` not rotatable without NVS erase | Not tracked | Manual NVS erase + reboot |
| OTA bundle not signed | Not tracked | Enable Secure Boot V2 (~4h) |
| `CONFIG_TAB5_DRAGON_TOKEN` baked into sdkconfig (not NVS) | Not tracked | Move to NVS via Settings UI in a future PR |
| OPUS encoder gated off pending [#264](https://github.com/lorcan35/TinkerTab/issues/264) | Tracked | Stays PCM until SILK NSQ crash on ESP32-P4 root-caused |
| Camera/mic activation has no hardware indicator | By design | Camera/mic require explicit user action via touch + UI state |
| Wake-word retired in [#162](https://github.com/lorcan35/TinkerTab/pull/162) | Documented | Always-on listening was retired; user-initiated PTT only |
| Single thread on the debug HTTP server | By design | Use polling, not long-poll (LEARNINGS #91) |

---

## 8. Reporting a vulnerability

This is a personal project; no formal SLA.  If you find something:

- File a private issue on GitHub or email the project owner.
- Don't post PoCs publicly until a patch ships, especially anything ngrok-exposed (which would imply a Dragon-side issue rather than Tab5 — still cross-link to the TinkerBox repo).
- Acknowledge time: best-effort, typically within a week.

If you find an issue affecting the auth-token derivation (`esp_random()` weakness on this hardware variant, etc.) please coordinate disclosure carefully — it would affect every Tab5 in the wild.

---

## 9. What we got right (focus auditing here)

- `auth_tok` uses `esp_random()` (CSPRNG, not deterministic seeded PRNG).  128 bits of entropy.
- Bearer comparison uses constant-time string compare in `debug_server.c: check_auth`.
- Token never logged unmasked — even in panic dumps (the masking is done in `tab5_settings_get_auth_token` before any `ESP_LOGI`).
- `/touch` endpoint is bearer-gated, so a hostile-LAN attacker can't drive the UI without the token.
- `POST /input/text` (the typing primitive) is scoped to the chat input only post-#300, so a stray harness call can't write into Settings/WiFi-password/Dragon-host fields.
- Mic capture requires the persistent mic task to be triggered; the task only wakes on user-initiated `voice_start_listening()` / `voice_start_dictation()` / `voice_video_start_call()` — there's no remote "start listening" path that bypasses the orb tap.
- LVGL `lv_async_call` thread-safety wrapper (`tab5_lv_async_call`, PR #257/#259) — every cross-thread UI write goes through the mutex, eliminating a class of UAF bugs that could've been exploitable.

---

## 10. Auditor's checklist (for hackers)

If you're doing a firmware security review:

1. **Read [TinkerBox `docs/protocol.md`](https://github.com/lorcan35/TinkerBox/blob/main/docs/protocol.md)** to understand what Tab5 sends and receives.
2. **Audit [`main/debug_server.c`](main/debug_server.c)** — every `httpd_register_uri_handler` line is an attack surface.  Verify each handler calls `check_auth(req)` (or is intentionally public).
3. **Trace `auth_tok` from generation to comparison:**
   - Generation: `tab5_settings_init()` in [`main/settings.c`](main/settings.c)
   - Storage: NVS via `tab5_settings_set_auth_token`
   - Read: `tab5_settings_get_auth_token`
   - Comparison: `check_auth` in `debug_server.c`
4. **Walk the WS RX loop** in [`main/voice.c`](main/voice.c) — every `cJSON` parse from Dragon is a potential parser-confusion target.
5. **Check the OTA verification path** in [`main/ota.c`](main/ota.c) — confirm the SHA256 check is mandatory, not just a warning.
6. **Pull NVS via esptool** to confirm `auth_tok` extraction is as advertised — if you can't extract it, that's a bug in this doc and we'd like to know.
7. **Read [`LEARNINGS.md`](LEARNINGS.md)** — every entry is a class of bug we hit.  Pattern-match for similar issues in code we haven't audited yet.

---

## See also

- [TinkerBox `SECURITY.md`](https://github.com/lorcan35/TinkerBox/blob/main/SECURITY.md) — Dragon-side counterpart
- [TinkerBox `docs/ARCHITECTURE.md`](https://github.com/lorcan35/TinkerBox/blob/main/docs/ARCHITECTURE.md) — system overview with trust boundaries
- [TinkerBox `docs/protocol.md`](https://github.com/lorcan35/TinkerBox/blob/main/docs/protocol.md) — wire format
- [`docs/HARDWARE.md`](docs/HARDWARE.md) — physical-layer reference
- [`LEARNINGS.md`](LEARNINGS.md) — war stories
