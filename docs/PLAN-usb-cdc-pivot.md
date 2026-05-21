# PLAN — Tab5↔K144 USB transport pivot (#620)

> Status: ACTIVE — branch `feat/usb-cdc-pivot`, target merge to `main` 2026-05-20.

## Why

M5-Bus UART at 1.5 Mbps is the source of every TinkerON wedge.  ~4 % byte
corruption from clock drift accumulates; K144 daemons hang; recovery
commands flow over the same broken wire.  Five watchdog hardening commits
this week made detection better but cannot fix a transport whose recovery
verbs (`sys.reset`, `sys.reboot`) depend on the transport itself.

Solution: pivot to USB.  Tab5's USB-A → K144's top USB-C carries control
(CDC-ACM), audio (UAC1), and optionally Ethernet (RNDIS) on one physical
link, alongside the existing ADB gadget.

## Live recon (2026-05-20)

K144 kernel config (probed via ADB this session):

```
CONFIG_USB_GADGET=y
CONFIG_USB_F_ACM=y              ← CDC-ACM serial
CONFIG_USB_F_UAC1=y             ← USB Audio Class 1
CONFIG_USB_F_UVC=y              ← USB Video Class (unused — K144 has no camera)
CONFIG_USB_F_RNDIS=y            ← Ethernet-over-USB
CONFIG_USB_F_MASS_STORAGE=y     ← file transfer
CONFIG_USB_CONFIGFS=y           ← composite framework
CONFIG_USB_CONFIGFS_ACM=y
CONFIG_USB_CONFIGFS_F_UAC1=y
CONFIG_USB_CONFIGFS_F_UVC=y
CONFIG_USB_CONFIGFS_RNDIS=y
```

UDC = `8000000.dwc3` (Synopsys DesignWare USB3, HS-capable).
Kernel 4.19.125 SMP PREEMPT.

Current gadget composition (single ADB):

```
/etc/configfs/usb_gadget/usb_adb/
├── idVendor   = 0x32c9
├── idProduct  = 0x2003
├── functions/ffs.adb           ← functionfs-backed ADB
└── configs/c.1/<sym>ffs.adb
```

Bound by `/soc/scripts/usb-adb.sh start` invoked from `/etc/rc.local`.
1 Hz auto-rebind watchdog at `/usr/local/m5stack/bin/ax_usb_adb_event.sh`
keeps the UDC populated across unplug events.

Tab5 side: IDF v5.5.2 ships `examples/peripherals/usb/host/cdc/cdc_acm_host`.
Project has zero existing USB host code.  USB-A 5 V on Tab5 gated by
IO-Expander 2 P1 (`docs/HARDWARE.md:138`).

## Architecture target

```
┌─────────────────────────────────────────────────────────────┐
│   K144 (AX630C / Linux 4.19)                                │
│                                                              │
│   /sys/kernel/config/usb_gadget/tinker/                     │
│     functions/ffs.adb                  ─→ adbd              │
│     functions/acm.GS0                  ─→ /dev/ttyGS0       │
│                                            (StackFlow JSON) │
│     functions/uac1.usb0                ─→ /dev/snd/pcmC?D?c │
│                                            /dev/snd/pcmC?D?p│
│     functions/rndis.usb0               ─→ usb0 netif (opt)  │
│                          │                                   │
│                          ▼ all bound to one UDC              │
└──────────────────────────┼──────────────────────────────────┘
                           │ USB HS (480 Mbps)
                           │
┌──────────────────────────▼──────────────────────────────────┐
│   Tab5 USB-A host (ESP32-P4 USB-OTG HS)                     │
│                                                              │
│   cdc_acm_host         ─→  voice_usb_cdc shim              │
│                            └─→ replaces tab5_port_c_send/   │
│                                recv/lock for K144 traffic    │
│                                                              │
│   uac_host (W5)        ─→  voice_usb_audio module           │
│                            └─→ replaces voice_ext_pcm_stream │
│                                                              │
│   rndis_host (W8, opt) ─→  LwIP netif → TCP :10001          │
└─────────────────────────────────────────────────────────────┘
```

## Waves

### W1 — K144 composite gadget (this commit)

Replace `usb-adb.sh` with one that builds an ADB + CDC-ACM + UAC1
composite under a new gadget name `tinker` (preserving ADB so we never
lose recovery access).  Mount functionfs for ADB, leave ACM/UAC1
kernel-backed.

**Verify:**
- `lsusb` from host shows multi-interface device
- `adb devices` still lists axera-ax620e
- New `/dev/ttyACM*` appears on the host
- `cat /sys/kernel/config/usb_gadget/tinker/UDC` = `8000000.dwc3`
- `arecord -l` on K144 shows `usb0` capture device

**Rollback:** original `usb-adb.sh.orig` kept; revert by `mv` + `reboot`.

### W2 — Tab5 USB host bring-up

- sdkconfig: enable USB host, OTG HS as host, USB-A 5 V at boot
- Add `espressif/usb_host_cdc_acm` managed component
- New file `main/voice_usb_cdc.{c,h}` — wraps `cdc_acm_host` in the
  `tab5_port_c_*` API shape (init, send, recv, lock, unlock, flush,
  set_baud as no-op, is_initialized)
- USB device-event task to handle connect/disconnect notifications
- New BSP helper `bsp/tab5/usb_host_5v.{c,h}` for the IO-Exp 2 P1 pin

### W3 — Transport abstraction

- New `tab5_xport_*` API in `main/voice_xport.{c,h}` — dispatches to
  UART or USB-CDC based on NVS `xport` key
- `voice_m5_llm.c` + `voice_ext_pcm_stream.c` use the new API
- NVS migration: default `xport=uart` for safety, user flips to
  `xport=usb_cdc` after live verify

### W4 — Full JSON migration

All StackFlow request/response traffic flows over CDC-ACM:
- `sys.ping`, `sys.hwinfo`, `sys.reset`, `sys.reboot`, `sys.lsmode`,
  `sys.version`
- `asr.setup` / `asr.inference` / `asr.exit`
- `llm.setup` / `llm.inference` / `llm.exit`
- `tts.setup` / `tts.inference` / `tts.exit` (request side; TTS audio
  reply switches to UAC1 playback in W6)
- ext_pcm pump payload (W5 replaces this with UAC capture)

### W5 — UAC1 mic streaming

- Add `espressif/usb_host_uac` managed component (vet first)
- `main/voice_usb_audio.c` opens UAC1 capture, streams Tab5 mic frames
  as raw 16 kHz mono PCM directly to K144's `/dev/snd/pcmC?D?c`
- `voice_ext_pcm_stream.c` becomes a no-op when `xport=usb_cdc`
- K144 `llm-asr` reconfigured to read from USB ALSA device instead of
  the StackFlow audio unit (config under `/etc/llm/audio.yaml`)

### W6 — UAC1 speaker streaming

- TTS reply path: K144 writes to UAC1 playback endpoint
- Tab5 USB host reads PCM stream, feeds `tab5_audio_play_raw`
- Replaces the per-utterance ADPCM-over-UART TTS reply path

### W7 — Live Hey Tinker verify

- Cold-boot, wake phrase, single voice turn, no UART involvement
- Run 1-hour soak — confirm no daemon wedge, no transport recovery
  events

### W8 — RNDIS Ethernet (bonus)

- K144 exposes `usb0` Ethernet device, static IP `192.168.7.1/24`
- Tab5 brings up matching `192.168.7.2/24`, hits K144 TCP :10001 for
  Wave 14 hwinfo cache without going through the USB serial path

### W9 — UART teardown

- Delete `voice_ext_pcm_stream.c` (UAC1 replaces it)
- Delete UART branch of `tab5_xport_*` if no fallback need confirmed
- Keep `bsp/tab5/uart_port_c.{c,h}` for Grove / other Port C add-ons

## Risk register

| Risk | Mitigation |
|---|---|
| Composite gadget binding fails — typo in configfs script | Keep `usb-adb.sh.orig` for one-line rollback |
| ESP-IDF `usb_host_uac` doesn't exist / is buggy | W5 is the first place we'd hit this — fall back to ALSA over CDC-ACM control if blocked |
| K144 boot script race — ADB event watchdog re-binds before our composite is ready | Disable the watchdog while we boot, re-enable after `UDC` is set |
| Tab5 USB-A 5 V doesn't auto-enable | Add IO-Exp pin assertion at USB host init |
| K144 `llm-asr` doesn't accept ALSA input mode | W5 — config audit before code |

## Code anchors

- `bsp/tab5/uart_port_c.{c,h}` — current UART API surface (shim target)
- `main/voice_m5_llm.c` — StackFlow JSON marshaller
- `main/voice_ext_pcm_stream.c` — current audio pump
- `main/voice_onboard.c` — chain lifecycle + watchdog
- `docs/PLAN-m5-llm-module.md` — original K144 integration plan
- `docs/HARDWARE.md` — Tab5 pinout

## Acceptance

Per issue #620.  PR squashes to one commit on `main` per wave.

## Status snapshot — 2026-05-20

**Committed + working:**
- W1: K144 composite gadget — ADB + CDC-ACM + UAC1 all enumerate ✓
- W2: Tab5 USB host stack — opens K144 CDC-ACM endpoint ✓
- W3: voice_xport abstraction + NVS xport switch ✓
- W4: K144 sys_config.json template (points llm_sys at /dev/ttyGS0) ✓
- K144 watchdog (ax_usb_tinker_event.sh) ✓

**Live-verified end-to-end (dev box ↔ K144 path):**
```
$ printf '{"request_id":"diag-1","work_id":"sys","action":"ping"}\n' > /dev/ttyACM1
$ cat /dev/ttyACM1
{"created":...,"data":"None","error":{"code":0,"message":""},
 "object":"None","request_id":"diag-1","work_id":"sys"}
```

**Known bug — Tab5 USB CDC RX direction silent:**
- TX from Tab5 → K144 works (`tx 56 bytes: {...}` logged)
- RX from K144 → Tab5 returns zero bytes (handle_rx callback never fires)
- DTR=1 + RTS=1 + line_coding_set(1500000) all confirmed via control transfers
- usb_cdc.connected = true, K144 enumerates cleanly

Likely cause: bulk-IN endpoint isn't being polled by `cdc_acm_host` driver
in our configuration, OR K144's f_acm gadget needs an additional control
signal we're missing.

**Production rollback applied 2026-05-20:**
- K144 `sys_config.json` removed → llm_sys back on /dev/ttyS1 (UART path live)
- Tab5 NVS `xport` stays at 0 (uart) — UART chain functional
- All USB code paths remain compiled but inactive
- To resume USB pivot: fix Tab5 RX bug, re-install K144 sys_config.json via
  `./scripts/k144/install.sh`, set NVS `xport=1`

---

## TT #621 — functionfs escape hatch (W5–W9, 2026-05-21)

### Why the rollback came back

The Tab5 cdc_acm RX bug noted above turned out to be a **K144-side** problem,
not Tab5's: K144's `f_acm.ko` silently drops bulk-OUT bytes when Tab5 is
the host (TT #621).  The `acm.usb0` function never delivered Tab5's writes
to `/dev/ttyGS0`'s tty buffer.  Dev-box probes against the same K144
gadget worked fine.

We tried every known software fix (CLEAR_FEATURE on both endpoints,
SET_INTERFACE, DTR transition, line coding, latest cdc_acm_host v2.4,
latest K144 firmware) before concluding the fix had to bypass `f_acm`
entirely.

### Architecture

Replace the broken `f_acm` bridge with a **functionfs** function on K144
that a userspace daemon owns end-to-end.  Same shape as ADB:

```
K144 gadget composition  (scripts/k144/usb-tinker-ffs.sh)
   ├─ ffs.adb           subclass 0x42  (adbd, untouched)
   ├─ ffs.control       subclass 0x44  (tinker-ffs-control → TCP 10001)
   ├─ ffs.video         subclass 0x43  (tinker-ffs-video   → TCP 10001)
   ├─ acm.usb0          (kept for fallback, unused by us)
   └─ uac1.usb0         (kept for future audio routing)
```

Each Tinker function: a small C daemon (`scripts/k144/tinker-ffs-*.c`)
mounts functionfs, writes USB descriptors to ep0, then relays bytes
between the bulk endpoints and a TCP socket on the K144 loopback to
StackFlow's `llm-sys` daemon (port 10001).  No tty layer, no CDC setup
requests, no f_acm involvement.

Tab5 side: a single USB host client (`main/voice_usb_ffs.{c,h}`) claims
**both** Tinker interfaces by discovering them via their unique
subclasses (not interface number — the kernel reshuffles those as the
gadget composition changes).  One client + two interface claims keeps
internal SRAM tracking flat; the dual-client variant we tried first
exhausted the SRAM (1 KB largest free) before recovering.

Per channel, on Tab5:
- recursive mutex for serialized sends
- PSRAM stream buffer for RX from the bulk-IN completion callback
- **persistent OUT transfer + done semaphore** (pre-allocated at claim
  time, reused per send) — fixes the panic_abort that happened when we
  freed in-flight transfers on send timeout
- zero-copy `borrow/commit` API on the video channel so `voice_yolo`
  encodes base64 directly into the USB DMA buffer (halves PSRAM
  bandwidth churn during YOLO bursts)

Tab5 modules consuming the channels:
- `voice_xport` (control channel — control plane: hwinfo, llm.setup,
  asr.setup, ext_pcm ingest, TTS playback)
- `voice_yolo` (video channel — yolo11n inference)
- `voice_ext_pcm_stream` (control channel via `voice_xport_*` — Tab5
  mic → K144 ASR; was UART, now USB)

### Verified live 2026-05-21

```
xport=usb_ffs  failover_state=ready  hwinfo.valid=true
ext_pcm: frames_pumped climbing ~10 fps, wakeword_active=true,
         asr_id=asr.1001, RMS varying with audible mic
yolo: POST /yolo/infer (cat_320.jpg) → cat (0.79) at (107,15) 227×154
      one round-trip ~330 ms (USB HS + NPU + USB HS)
```

Cold boot picks usb_ffs automatically (default NVS `xport` value flipped
from 0 to 2 in W9); `voice_xport_init` still falls back to UART if USB
doesn't enumerate within the boot window, so the legacy path stays as
recovery.

### Files added / changed

| File | Purpose |
|------|---------|
| `scripts/k144/usb-tinker-ffs.sh` | Composite gadget composer (adb+control+video+acm+uac1) |
| `scripts/k144/tinker-ffs-control.c` | K144 bridge daemon for ffs.control ↔ TCP 10001 |
| `scripts/k144/tinker-ffs-video.c` | K144 bridge daemon for ffs.video ↔ TCP 10001 |
| `main/voice_usb_ffs.{c,h}` | Tab5 single-client driver, claims both ffs interfaces |
| `main/voice_yolo.{c,h}` | yolo11n setup + inference via ffs.video |
| `main/voice_xport.{c,h}` | Adds `VOICE_XPORT_USB_FFS = 2` |
| `main/voice_ext_pcm_stream.c` | Pump now uses `voice_xport_*` (UART or USB) |
| `main/voice_m5_llm.{c,h}` | Adds `_wakeword_set_paused()` so yolo can quiet the recv loop |
| `main/debug_server_m5.c` | Adds `POST /yolo/infer`, extends `/m5/xport` to accept `usb_ffs` |
| `main/settings.c` | Default xport flipped 0 → 2 |
| `main/main.c` | Boot-time picker: voice_usb_ffs (xport=2) OR voice_usb_cdc (xport=1) |

### Operational notes

- K144 daemons SIGTERM cleanly (their `g_running` flag) but a SIGKILL
  while UDC-bound takes the whole gadget down — use `mv` to atomic-swap
  the binary then SIGTERM, or just `reboot` the K144 and let rc.local
  respawn from the new binary.
- Subclass IDs are gadget-side identifiers: ADB uses 0x42 (we can't
  change), we picked 0x43 for video and 0x44 for control to keep them
  unique.  Tab5 walks `bNumInterfaces` and matches by subclass.
- Each channel's OUT transfer is sized for its workload — control 8 KB
  (ext_pcm pump frame is 4.4 KB; sys.* < 1 KB), video 48 KB (yolo
  base64 JPEG ~ 40 KB).  Both are PSRAM-backed via
  `CONFIG_USB_HOST_DWC_DMA_CAP_MEMORY_IN_PSRAM=y`.

### Known follow-ups

- Sustained YOLO bursts (3+ calls in <1s) cause Wi-Fi DMA contention →
  WS reconnect (Tab5 recovers, no panic).  Either rate-limit client-side
  or task-pin yolo to Core 1 isolated from Wi-Fi.
- `cdc_acm_host` (xport=1) variant remains compiled but is now
  deprecated by the functionfs path.  Can be removed once a few weeks
  of telemetry confirm no xport=1 users.
