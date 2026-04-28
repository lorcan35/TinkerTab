# Plan — M5Stack LLM Module integration on Tab5

**Status:** Phase 0–2 complete (Phase 2 verified live via `m5ping` + `m5lscmd` serial commands going through `main/m5_stackflow.{c,h}` marshalling, 2026-04-28).  Phases 3-4 not started.
**Owner:** unassigned.
**Tracking issue:** TT #317.
**Last updated:** 2026-04-28.
**Related:** TinkerTab #67 (widget platform), TinkerBox `docs/ARCHITECTURE.md` (router design).

**Hardware path locked (2026-04-28):** K144 stays **stacked on Tab5's M5-Bus rear connector** for both bench AND production. Both USB-Cs (Tab5's bottom + K144's top) plugged in for power/debug while Phase 1-4 are written; once stable, only Tab5's USB-C is needed (K144 draws 5V from M5-Bus). Side Port C 4-pin header path is discarded but the same UART (G6/G7) is shared between rear M5-Bus pins 15/16 and the side connector, so the firmware is identical regardless.

---

## Phase 0 results — live bench on the K144 (2026-04-28)

**Module reachable, protocol confirmed, real numbers captured.**

Setup:
- Tab5 dev host (DGX) ↔ K144's top USB-C (M140 module port) via Axera ADB
- `adb forward tcp:10001 tcp:10001` to reach the on-module `llm_sys` listener
- Newline-delimited JSON over TCP (same shape as planned over UART)

What's installed on this K144:
- Ubuntu 22.04 LTS, kernel 4.19.125 aarch64
- All StackFlow services running: `llm-llm`, `llm-asr`, `llm-tts`, `llm-melotts`, `llm-kws`, `llm-audio`, `llm-camera`, `llm-skel`, `llm-sys`
- LLM model: `qwen2.5-0.5B-prefill-20e` (M5 package v0.2)
- ASR: sherpa-ncnn streaming Zipformer (en + zh-CN)
- TTS: single-speaker English fast + MeloTTS Chinese
- KWS: sherpa-onnx Zipformer wake-word (en + zh-CN)

**Real measurements (3 multi-sentence prompts, 60s wallclock):**

| Prompt | TTFT | tok/s during stream | Output tokens |
|---|---|---|---|
| 3-sentence robot story | **632 ms** | 14.5 | ~75 |
| 2-sentence photosynthesis | **557 ms** | 13.7 | ~58 |
| 3 sleep benefits | **611 ms** | 16.7 | ~28 |
| **Average** | **~600 ms** | **~15 tok/s** | — |

Refines the estimates this doc previously had ("TTFT inferred <1s", "12.88 tok/s from M5 spec"):
- TTFT confirmed ~600 ms — **better than the < 1 s upper bound**
- tok/s ~15 — slightly above M5's published 12.88 (different model/quant)
- End-to-end short reply: **1-2 s** (e.g. "capital of France?" → 1.25 s)
- End-to-end multi-sentence: **~5 s** (75 tok @ 14.5 tok/s + 600 ms TTFT)

**Quality spot-check:** coherent replies, correct facts (Paris, oxygen+glucose, sleep-benefit triplet), reasonable creativity ("Max the painter robot"). Slightly verbose despite a "be concise" system prompt. Acceptable for a fallback / onboard mode; clearly worse than ministral-3B for instruction following.

**Wire-protocol gotchas captured:**
- Setup returns **non-streaming** ack with `data:"None"` first; the `work_id` (e.g. `llm.1000`) is the new resource ID for subsequent inference calls.
- Inference streaming chunks use shape: `{"object":"llm.utf-8.stream", "data":{"delta":"...", "index":N, "finish":bool}, "request_id":..., "work_id":...}`.
- Both `data: object` and `data: string` accepted on the inference REQUEST (the response always has `data: object`).
- No length-prefix framing; newline-delimited JSON, single TCP socket survives multiple inference calls.
- TTFT measured from request send → first `delta` chunk is **consistently ~600 ms** (cold or warm — no measurable cold-start penalty between consecutive prompts).

**Implications for the firmware integration:**
- Wire shape exactly matches what `m5_stackflow.{c,h}` will need (Phase 2). Plan unchanged.
- Latency math holds — Option A (failover sidecar) is feasible UX-wise with sub-2s short-reply turnaround.
- The module's local TCP listener on port 10001 means an alternative integration shape would be: Tab5 SSH-tunnels into the K144 via the carrier's USB-Ethernet bridge and talks JSON-over-TCP. **More complex than UART**, defer unless UART proves problematic.
- The `m5stack-LLM` hostname + ADB interface means we have a fully-bench-able Linux box with apt + dpkg for installing extra models / debugging.

---

---

## What this enables

Plug an **M5Stack Module LLM Kit (K144, $79.90)** into Tab5 and have a Local-mode voice path that runs **entirely on-device** — no Dragon, no cloud, no network. Speaker → ASR (Whisper-tiny on the module) → LLM (Qwen2.5-0.5B / Llama-3.2-1B on the module) → TTS (MeloTTS on the module) → speaker. End-to-end ~3-8 seconds for a short reply, smaller models than Dragon's ministral-3B but **5-10× faster**.

Strategic value: a **truly offline** TinkerClaw. Today our "Local" mode still needs a Dragon box on the LAN. With the LLM Module, Tab5 + the module is the entire system. Suitcase TinkerClaw, off-grid TinkerClaw, demo-without-network TinkerClaw.

## SKU clarification — K144 vs K143

| Product | SKU | Form factor | Use here |
|---|---|---|---|
| **Module LLM Kit** | **K144** ($79.90) | Stackable M5-Bus + USB-C carrier | **THIS** — sidecar that plugs into Tab5 |
| LLM630 Compute Kit | K143 (price TBD) | Standalone Linux box w/ GbE, MIPI DSI | Not this — standalone replacement for Dragon |

The user previously mentioned "Module 631" — that's a conflation. The two real products are K144 (sidecar, supersedes the EOL'd M140 module) and K143 (standalone). For our integration: **K144**.

## Hardware specs

| | |
|---|---|
| **SoC** | Axera AX630C (dual ARM Cortex-A53 @ 1.2 GHz + 3.2 TOPS NPU @ INT8 / 12.8 TOPS @ INT4) |
| **Memory** | 4 GB LPDDR4 (split 1 GB system / 3 GB NPU-reserved) |
| **Storage** | 32 GB eMMC 5.1 |
| **Power** | ~1.5 W average; ~500-800 mA peak under inference load |
| **OS** | Debian-flavour Linux + M5's **StackFlow** inference framework |
| **Models shipped** | Qwen2.5-0.5B/1.5B, Llama-3.2-1B, InternVL2.5-1B (vision), DeepSeek-R1-distill-Qwen-1.5B + Whisper-tiny/base + MeloTTS + YOLO11 |
| **Throughput** | ~12.88 tok/s for Qwen3-0.6B INT8 (M5's published benchmark) |
| **Context** | ~2.5 K tokens (`max_token_len=2559, kv_cache=1024, prefill_token_num=128`) |
| **Cost** | $79.90 (Kit including the carrier) |

### Wire interface — UART @ 115200 8N1

The module is a Linux box that speaks **JSON-over-UART** to the host. Each interaction is a JSON object:

```json
{
  "request_id": "1",
  "work_id": "llm.1234",
  "action": "inference",
  "object": "llm.utf-8.stream",
  "data": "what is the capital of France?"
}
```

**Functional units** (separate Linux services on the module):
`kws`, `vad`, `asr`, `whisper`, `llm`, `vlm`, `tts`, `melotts`, `camera`, `yolo`.

**Lifecycle per unit:** `sys.reset` → `setup(unit, model, params)` returns a `work_id` → `inference(work_id, payload)` (streaming) → `exit`.

**Authoritative source for the JSON schema:** the M5Module-LLM Arduino library at https://github.com/m5stack/M5Module-LLM (v1.7.0 / 2025-09-05, MIT). M5's docs don't publish the full grammar in one place — the C++ source is the de-facto spec.

### Physical connection on Tab5 — stacked M5-Bus rear

The K144 ships with the **Module13.2 LLM Mate** carrier (USB-C + RJ45 + a side 4-pin header) and a 30-pin M5-Bus connector on the rear. We stack it on Tab5's M5-Bus rear pinheaders. The K144's UART solder pads ship configured for the M5Stack standard "Port C" position (M5-Bus pins 15/16) — the same position M5Unified exposes via `m5::pin_name_t::port_c_rxd / port_c_txd` in the official Arduino TextAssistant example.

**Tab5 M5-Bus rear pinout — UART block** (full table now lives in [`docs/HARDWARE.md`](HARDWARE.md#m5-bus-rear-connector)):

| M5-Bus pin | Tab5 signal | ESP32-P4 GPIO | K144 side | Notes |
|---|---|---|---|---|
| 15 | PC_RX | **GPIO 7** | K144 TX out | Reads K144 → Tab5 |
| 16 | PC_TX | **GPIO 6** | K144 RX in | Drives Tab5 → K144 |
| 5 | GND | — | GND | |
| 28 | 5V (EXT_5V_BUS) | — (gated by EXT5V_EN on E1.P4) | 5V in | Tab5 powers the module via M5-Bus when EXT5V is on |

**Why this is the same UART as side-connector "Port C":** Tab5 routes the same ESP32-P4 UART (GPIO 6 TX / GPIO 7 RX) to both the M5-Bus rear pins 15/16 *and* the side 4-pin Port C header — they're physically wired in parallel. So the firmware filename `bsp/tab5/uart_port_c.{c,h}` from the original plan is still correct; it just means "the Port-C UART (GPIO 6/7), exposed at whatever physical connector you plug into."

**Bonus:** UART0 (RXD0=GPIO 38, TXD0=GPIO 37) is *also* exposed on M5-Bus pins 13/14. Don't use it for the K144 — it's the ESP32-P4's primary serial console and gets fought over by `idf.py monitor`.

**First firmware step:** stand up a UART driver on GPIO 6/7 (115200 8N1). Loopback first (jumper TX→RX with the K144 unstacked), then `sys.reset`/`sys.ping` against the live K144.

### Power note — EXT5V must be ON before the module boots

The M5-Bus 5V rail is *gated* by `EXT5V_EN` on IO-Expander 1 pin P4. Today this gate is also used by Port A (HY2.0-4P) and the side expansion header, so toggling it has multi-rail consequences (see `docs/PLAN-grove.md`). Default state at boot today: **OFF**. The module won't power up via M5-Bus unless we explicitly drive EXT5V_EN high during Phase 1 service init. **Until that's wired**, the second USB-C (the K144's top USB-C) will keep the module powered for bench testing — which is why we keep both USB-Cs plugged in during development.

---

## Non-negotiable: modular addon

**Tab5 must never depend on the LLM Module being present.** This is the architectural constraint that shapes every design choice below.

Translated into rules:

1. **Boot path is module-agnostic.** Tab5 boots identically whether the module is plugged in, unplugged mid-flight, or never present. No "waiting for module" loops. No error dialogs. No log spam past a single "module not detected" debug line.
2. **No Tab5-side feature regresses if the module is absent.** Local mode still works via Dragon. Cloud still works. Voice overlay still functions. The user must not lose anything by not having the module.
3. **Capability detection is the gate.** On boot, probe with a short `sys.ping` (≤500 ms timeout). Present → light up addon paths. Absent → silent, no UI artifacts.
4. **UI surfaces gate on detection.** Mode-sheet entry "Onboard" (if/when shipped) appears **grayed out with a subtitle "module not detected"** when absent — never just missing. Predictable UX.
5. **Hot-unplug is graceful.** If the module disappears mid-session, in-flight inference fails-soft (toast + reconnect to Dragon path); no Tab5 crash, no LVGL screen-load fault.
6. **No firmware build-time coupling.** The module-driver code lives behind a runtime check, not `#ifdef TAB5_HAS_M5_LLM`. Same firmware binary works with or without the module.

This **rules out Option C** (replace Dragon entirely) and **constrains Option B** (capability-gated, never default).

**Option A is already shape-correct** — failover sidecar that's silent when Dragon is up, so Tab5 with no module behaves identically to Tab5 with module + Dragon-up.

---

## Three integration strategies

The decision between these is a **product call**, not an engineering call. Each has different scope, latency, and complexity.

### Option A — Sidecar over UART, transparent (lowest scope)

The module is a "fallback LLM" that voice.c can route to **only when Dragon is unreachable**. Dragon stays the orchestrator. No new voice mode.

**Shape:**
- New `voice_m5_llm.{c,h}` — pure UART transport + StackFlow JSON marshalling. ~200 LOC.
- Probe the module on boot; if alive, register as fallback.
- voice.c's WS-disconnect handler tries the module before showing "Dragon unreachable".
- Module returns LLM responses; voice.c surfaces them as if from Dragon.

**Pros:**
- Zero refactor to voice.c — module is a hidden failover.
- Easy to ship; fast to validate.
- Doesn't change product surface (no new mode toggle).

**Cons:**
- Module sits idle 99% of the time (when Dragon is up).
- Can't use the module's superior latency (3-8 s) when Dragon is reachable.
- ASR + TTS still go to Dragon; we only fall back the LLM step.

**Effort:** ~3 days.

### Option B — New `voice_mode = LOCAL_ONBOARD` (the obvious shape)

Adds a fourth tier alongside Local / Hybrid / Cloud / TinkerClaw. Selected explicitly by the user via the mode sheet.

**Shape:**
- New `voice_local_llm.{c,h}` implementing the full Local-onboard pipeline (ASR + LLM + TTS all on the module).
- Refactor voice.c to abstract the LLM backend behind a `llm_backend_t` interface — today voice.c is hardcoded to a single Dragon WS connection.
- New mode-sheet entry "Onboard" with copy explaining the tradeoff (smaller models, faster, fully offline).
- Capability detection at boot — if module isn't plugged in, the mode is grayed out in the sheet.

**Pros:**
- Cleanest separation; user-controlled which backend runs.
- Can reuse the module's KWS/VAD/ASR/TTS units, removing CPU pressure from ESP32-P4.
- Future-proof: adding more backends (Qualcomm AI Hub, Hailo, etc.) becomes a one-line addition.

**Cons:**
- voice.c refactor is the riskiest part — that file is ~3000 LOC of WebSocket + audio pipeline; introducing a backend abstraction touches every state transition.
- New mode-sheet UX work.
- Quality cliff: Qwen2.5-0.5B vs ministral-3B (Local mode today) — instruction-following will be noticeably worse. UX should communicate this honestly.

**Effort:** ~5 days.

### Option C — Replace Dragon entirely (most ambitious)

Module does ASR (Whisper) + LLM + TTS (MeloTTS) all locally. Dragon becomes optional. Conversation state lives on Tab5 (NVS / SD card).

**Shape:**
- All of Option B, plus:
- Re-implement Dragon's `pipeline.py` (multi-turn context, tool-calling, memory) in C on Tab5.
- Conversation history persisted to SD.
- Skill platform translates from Dragon-side Python skills to module-StackFlow units (or skills disable in Onboard mode).

**Pros:**
- Suitcase TinkerClaw — works anywhere with no infrastructure.
- Cool factor: a complete voice assistant on a $200 device.

**Cons:**
- Multi-week effort.
- Loses the entire skill ecosystem (Dragon-only Python tools).
- Conversation memory + RAG can't replicate Dragon's MemoryService / DocumentService without a vector store on Tab5 — possible but heavy.

**Effort:** Multi-week (3+).

### Recommendation

**Ship A first** as a probe — it validates the wire protocol, the carrier-board behavior, the StackFlow schema, the latency math, all without product-surface risk. ~3 days. If the bench results look good (token throughput as advertised, JSON parsing stable, no thermal issues), **escalate to B**. **Skip C** unless we explicitly want offline-only TinkerClaw as a product story.

---

## Phased plan

Same structure as PLAN-grove.md but with bigger phases.

### Phase 0 — Buy K144 + bench standalone (1 day)

**No firmware work.**
- Order from M5 store ($79.90 + shipping).
- Wire K144 to a dev board (XIAO ESP32-S3 or similar) with the official Arduino library.
- Run the `TextAssistant.ino` example.
- Measure: TTFT, tok/s for `qwen2.5-0.5b`, JSON shapes for setup/inference/exit, error responses.
- Document any deviations from the published spec in this file.

### Phase 1 — Port C UART bring-up (1 day)

**Files:** `bsp/tab5/uart_port_c.{c,h}` (new), `bsp/tab5/bsp_config.h` (add `TAB5_PORT_C_UART_TX = 6`, `TAB5_PORT_C_UART_RX = 7`, `TAB5_PORT_C_UART_NUM = UART_NUM_1`), `main/service_uart_port_c.{c,h}`.

**Pins locked (verified against M5Module-LLM TextAssistant example + Tab5 schematic, 2026-04-28):**
- TX = **GPIO 6** (PC_TX, M5-Bus rear pin 16, also wired to side Port C header)
- RX = **GPIO 7** (PC_RX, M5-Bus rear pin 15, also wired to side Port C header)
- UART peripheral: **UART_NUM_1** (UART0 reserved for boot console; UART2 free if we ever need a second link)
- Framing: 115200 8N1, no flow control

**Power coupling — must drive EXT5V_EN before talking to the module:**
- Today the side Port A + Port C 5V + M5-Bus 5V are all gated by IO-Expander 1, P4 (`EXT5V_EN`). Boot default is OFF.
- During development we leave the K144's top USB-C plugged in (powers the module independently), so the firmware doesn't strictly need to flip EXT5V_EN to bench Phase 1.
- **Production path:** Phase 1 service init flips P4 high once per boot. This is the same gate Grove will need (see `docs/PLAN-grove.md`), so coordinate the two PRs to avoid double-init.

**Acceptance:**
- Boot logs `"Port C UART ready (TX=6, RX=7, 115200 8N1)"` after the Phase 1 service starts (and only when the addon-detect probe in Phase 3 succeeds — Phase 1 itself is allowed to fire unconditionally for bring-up debugging).
- `tab5_port_c_send(buf, len)` and `tab5_port_c_recv(buf, len, timeout_ms)` APIs work.
- Loopback test (TX shorted to RX, K144 *unstacked*) round-trips a 64-byte payload in <50 ms.
- With the K144 stacked + powered (top USB-C OR EXT5V on), `sys.ping` returns `MODULE_LLM_OK` shape (`{"created":..., "request_id":..., "error":{"code":0,...}, "object":"None", "data":"None"}`).
- Negative test: with the K144 disconnected, `sys.ping` times out cleanly after 500 ms — Tab5 never blocks the LVGL task.

**Memory ceiling resolved 2026-04-28** (root-cause + fix in LEARNINGS.md "Adding the ESP-IDF UART driver pulls in esp_ringbuf, IRAM-hungry, → boot panic").  Three earlier attempts boot-panicked at `vApplicationGetTimerTaskMemory port_common.c:97` — including one with `uart_port_c.c` properly placed in its own `bsp/tab5/` component (so the "different component" hypothesis was wrong).  The real cause: pulling in `driver/uart.h` brings `esp_ringbuf`'s 5.6 KB of `.text` into IRAM, shifting heap region layout enough that `pvPortMalloc(16 KB)` for the timer task stack returns NULL.  Fix is two `sdkconfig.defaults` lines: `CONFIG_RINGBUF_PLACE_FUNCTIONS_INTO_FLASH=y` + `CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH=2048`.  Verified live: `m5ping` serial command rounds JSON to/from the stacked K144 cleanly (`error.code: 0` MODULE_LLM_OK).

### Phase 2 — StackFlow JSON marshalling — DONE 2026-04-28

**Files landed:** `main/m5_stackflow.{c,h}` (new) — pure marshalling layer, transport-agnostic (no UART dep, takes caller buffers).  Built around five small functions following ISP / SRP:

- `m5_stackflow_build_request(req, buf, buf_cap)` → newline-terminated JSON
- `m5_stackflow_parse_response(json, len, &resp)` → owned cJSON tree + typed view
- `m5_stackflow_response_free(&resp)`
- `m5_stackflow_response_matches(&resp, expected_request_id)`
- `m5_stackflow_response_is_stream(&resp)` + `m5_stackflow_extract_stream_chunk(&resp, &chunk)` — open-closed for future units (`asr.utf-8.stream`, `kws.utf-8.stream`, etc.) via a `*.stream` suffix check, no core changes needed when M5 ships new objects.

The `voice_m5_llm.c` sidecar (Phase 3) and any future Tab5↔K144 transport layer (TCP, ZMQ) sit on top of this marshalling without modification — pure functions, no transport coupling.

**Acceptance — all met:**
- `m5_stackflow_build_request` accepts text-or-object `data` and an optional `object` field; returns -1 on missing required fields or buffer-too-small (verified via Tab5 unit test path through the `m5ping` serial command).
- `m5_stackflow_parse_response` validates `request_id` matching, extracts `error.code` + `error.message`, and produces a borrowed cJSON pointer for `data`.
- Live round-trip via `m5ping` (action=ping, expected `err=0`) AND `m5lscmd` (action=lscmd, intentional invalid action → `err=-3 "action match false"`).  Both commands match request_id and surface error fields cleanly through the new layer.

```text
[ping]  tx=52 rx=119  match=yes err=0  ()                    work=sys object=None
[lscmd] tx=54 rx=139  match=yes err=-3 ("action match false") work=sys object=None
```

The error-path test is the more valuable one — it confirms `error_code` / `error_message` propagate through the parser, which Phase 3's `voice_m5_llm_infer` will lean on for setup-failure / timeout handling.

### Phase 3 — `voice_m5_llm.c` sidecar service (1 day)

**Files:** `main/voice_m5_llm.{c,h}` (new), `main/service_registry.c` (add).

**Acceptance:**
- On boot, attempt `sys.reset` to the module; success means module is alive.
- Service exposes a synchronous `voice_m5_llm_infer(prompt, output, output_cap, timeout_s)` API.
- Bench: `voice_m5_llm_infer("Hello, how are you?", buf, 256, 30)` returns a sensible response in ≤8 s.

### Phase 4 — Failover wiring (Option A) (half day)

**Files:** `main/voice.c` (WS-disconnect handler).

**Acceptance:**
- When Dragon WS is unreachable for 30 s and voice_mode is Local, voice.c routes the next text turn through `voice_m5_llm_infer`.
- A toast informs the user "Using onboard LLM (Dragon unreachable)".
- Reconnect to Dragon switches back automatically.

### Phase 5 (optional, decided later) — voice_mode = LOCAL_ONBOARD (Option B) (~5 days)

**Files:** voice.c refactor + new mode-sheet entry + capability gating.

**Out of scope of this initial plan.** File a follow-up issue when we get there.

---

## Latency comparison (estimated)

| Backend | TTFT | tok/s | 100-token reply |
|---|---|---|---|
| Cloud (OpenRouter Haiku-3.5) | 1-2 s | 30+ | **3-6 s** |
| **M5 Module LLM (Qwen2.5-0.5B)** | <1 s* | ~12.88 | **3-8 s** |
| Dragon Q6A ministral-3B (Local) | ~30 s | ~1.5 | ~65 s |

\* M5 hasn't published TTFT figures — inferred from `prefill_token_num=128` + AX630C clock. **Phase 0 bench will replace these estimates with measurements.**

**Quality cliff:** M5 ships 0.5B-1.5B models. Dragon's ministral is 3B. Cloud is 8B-70B. Expect noticeably worse instruction-following from M5 vs current Local mode, but **5-10× faster** end-to-end.

---

## Honest unknowns

1. ~~**TTFT on AX630C**~~ — **resolved Phase 0 (2026-04-28):** measured ~600 ms cold or warm, ~15 tok/s during stream.
2. **Module's wire-protocol completeness** — the M5Module-LLM Arduino library is the de facto spec but might not cover every edge case (timeouts, error responses, partial-token streaming). Phase 0 captured the major shapes; gaps documented inline above.
3. **Whether VLM (`internvl2.5-1B`) and LLM can co-reside in 3 GB NPU budget** — undocumented. If we want vision-on-device, this is the blocking question.
4. **Whether M5 still ships a working AX630C model toolchain** as of 2026 — needed for custom models. If Axera deprecated it, we're stuck with the pre-converted models in their package repo.
5. **Power draw under sustained load + thermal throttling** — 500-800 mA peak per spec. Tab5's 30 Wh battery → ~30-36 hr if module is plugged in continuously. Episodic use is fine. Continuous voice for hours is not.
6. ~~**Whether Port C exposes UART pins without rework**~~ — **resolved 2026-04-28:** Tab5 wires the same UART (GPIO 6 TX / GPIO 7 RX) to *both* the side Port C 4-pin header and M5-Bus rear pins 15/16 (`PC_TX` / `PC_RX`). The K144's stock solder-pad config matches M5Unified's `port_c_*` pins, so no PCB rework is needed for the stacked path. UART0 (G37/G38) is also exposed on M5-Bus 13/14 but should be avoided — collides with `idf.py monitor`.
7. **EXT5V_EN coupling with Port A** — `IO-Expander 1, P4` gates Tab5's external 5V rail to *all* expansion connectors at once: HY2.0-4P (Grove), side Port C 5V, and M5-Bus rear 5V. Driving it for the K144 also energises whatever's plugged into Grove, and vice-versa. Coordination required between this plan and `docs/PLAN-grove.md`. For dev we sidestep by leaving the K144's top USB-C plugged in.

## Out of scope of this plan

- Vision (`vlm` / `yolo` units on the module). Future work.
- Multi-module setups (two K144s for redundancy). Why would we?
- Custom model conversion. We use M5's pre-quantized models from their package repo.
- Cloud fallback for the module (e.g. when its eMMC fills up). It's local-only by design.
- Dragon-side awareness of the module. This is a Tab5-firmware-only feature.

---

## References (verified live 2026-04-28)

- M5 Module LLM Kit K144 — store: https://shop.m5stack.com/products/m5stack-llm-large-language-model-module-kit-ax630c
- M5Module-LLM Arduino library (de-facto protocol spec): https://github.com/m5stack/M5Module-LLM
- M5 Docs Module LLM main page: https://docs.m5stack.com/en/module/Module-LLM
- StackFlow software / package list: https://docs.m5stack.com/en/stackflow/module_llm/software
- AX630C Qwen3-0.6B benchmark, 12.88 tok/s: https://docs.m5stack.com/en/guide/ai_accelerator/llm-8850/m5_llm_8850_qwen3_0.6b
- Hackster.io launch coverage: https://www.hackster.io/news/m5stack-adds-large-language-model-support-to-its-offerings-with-the-3-2-tops-llm-module-f0a4e061f0de
- M5 Tab5 product page (Port C reference): https://docs.m5stack.com/en/core/Tab5
- TextAssistant.ino canonical Arduino example: https://github.com/m5stack/M5Module-LLM/blob/main/examples/TextAssistant/TextAssistant.ino

## Code anchors

- voice.c WS connect path: `main/voice.c` (search `voice_ws_start_client`)
- Mode-sheet UI: `main/ui_mode_sheet.c`
- Capability negotiation pattern (audio codec): `main/voice_codec.h:1-41` + `main/voice.c` config_update handler
- Service-registry pattern: `main/service_registry.{c,h}` + `main/service_audio.c:16-50`
- Boot sequence: `main/main.c:278, 317, 368-384`
- IO expander API (for EXT5V_EN gate on Port C 5V rail — same gate as Port A): `main/io_expander.h`
- TinkerBox router (where a future Onboard backend would slot in if we ever extend the router to the firmware side): `dragon_voice/llm/router.py`

## Companion plan

This document is the LLM-Module half of an external-hardware push. The companion is **`docs/PLAN-grove.md`** which covers Port A I2C sensor support. The two plans share infrastructure work (EXT5V_EN pin discovery on the IO expander) — whichever ships first should land that part for both.
