# Plan — M5Stack LLM Module integration on Tab5

**Status:** parked, not started.
**Owner:** unassigned.
**Tracking issue:** TBD (filed alongside this doc).
**Last updated:** 2026-04-28.
**Related:** TinkerTab #67 (widget platform), TinkerBox `docs/ARCHITECTURE.md` (router design).

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

### Physical connection on Tab5

K144 ships with a **carrier board** (Module13.2 LLM Mate) exposing USB-C + RJ45 + a 4-pin TX/RX/GND/5V header. On Tab5 we'd connect via **Port C** (4-pin UART side connector).

| Tab5 Port C pin | K144 |
|---|---|
| GND | GND |
| 5V (gated by EXT5V_EN) | 5V in |
| TX (Tab5) | RX (module) |
| RX (Tab5) | TX (module) |

We don't currently initialize Port C in firmware. **First firmware step:** stand up a UART driver on Port C, do a loopback test, then a "send `sys.reset`, expect ACK" test before plugging the actual module.

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

**Files:** `bsp/tab5/uart_port_c.{c,h}` (new), `bsp/tab5/bsp_config.h` (TX/RX pins TBD per Phase 0 bench), `main/service_uart_port_c.{c,h}`.

**Acceptance:**
- Boot logs "Port C UART ready (TX=N, RX=M, 115200 8N1)".
- `tab5_port_c_send(buf, len)` and `tab5_port_c_recv(buf, len, timeout)` APIs work.
- A loopback test (TX shorted to RX) round-trips.

### Phase 2 — StackFlow JSON marshalling (1 day)

**Files:** `main/m5_stackflow.{c,h}` (new) — request builder + response parser. Reuses cJSON which we already include.

**Acceptance:**
- `m5_stackflow_send_request(...)` builds a JSON object and sends over Port C.
- `m5_stackflow_parse_response(...)` validates `request_id` matching and unmarshals payloads.
- Test against bench-captured frames from Phase 0 — at least one round-trip per unit (`sys`, `llm`, `whisper`, `melotts`).

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

1. **TTFT on AX630C** — no published figure. Phase 0 will measure it directly.
2. **Module's wire-protocol completeness** — the M5Module-LLM Arduino library is the de facto spec but might not cover every edge case (timeouts, error responses, partial-token streaming). Phase 0 documents gaps.
3. **Whether VLM (`internvl2.5-1B`) and LLM can co-reside in 3 GB NPU budget** — undocumented. If we want vision-on-device, this is the blocking question.
4. **Whether M5 still ships a working AX630C model toolchain** as of 2026 — needed for custom models. If Axera deprecated it, we're stuck with the pre-converted models in their package repo.
5. **Power draw under sustained load + thermal throttling** — 500-800 mA peak per spec. Tab5's 30 Wh battery → ~30-36 hr if module is plugged in continuously. Episodic use is fine. Continuous voice for hours is not.
6. **Whether Port C exposes UART pins without rework** — research says yes (4-pin connector on the side), but our firmware never tested it.

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
