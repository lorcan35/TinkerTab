# Tab5 Solo Mode — Dragon-Independent Voice Assistant

> Spec for a new voice mode that lets Tab5 operate without Dragon, using direct cloud calls (OpenRouter) plus the existing K144 onboard LLM module.

**Status:** spec / pre-implementation
**Owner:** Emile
**Created:** 2026-05-08
**Companion repo:** TinkerBox (no changes required for v1)

---

## Context

Today every Tab5 voice mode requires Dragon for some part of the pipeline:

| Mode | LLM | STT | TTS | Dragon required? |
|------|-----|-----|-----|------------------|
| `vmode=0 LOCAL` | Dragon Ollama | Dragon Moonshine | Dragon Piper | yes (everything) |
| `vmode=1 HYBRID` | Dragon Ollama | Dragon proxy → OpenRouter | Dragon proxy → OpenRouter | yes |
| `vmode=2 CLOUD` | Dragon proxy → OpenRouter | Dragon proxy → OpenRouter | Dragon proxy → OpenRouter | yes |
| `vmode=3 TINKERCLAW` | Dragon → TinkerClaw gateway | Dragon Moonshine | Dragon Piper | yes |
| `vmode=4 LOCAL_ONBOARD` | Tab5 K144 module | (text-only or K144 ASR) | K144 TTS | **no** |

`vmode=4` is the only Dragon-less path today. It's good for offline/private but the K144 model is small (qwen2.5-0.5B), latency is ~5s/turn, and the hardware is currently in a probe-fail loop in the field. There's no on-device path to use proper cloud models without Dragon as a proxy.

**Three drivers for adding a Dragon-less cloud path:**

- **A — Travel.** Tab5 away from home WiFi (hotel, mobile hotspot). Dragon is not reachable.
- **C — Privacy / simplicity.** User wants Tab5 + cloud only, with no Dragon-side message store.
- **D — Standalone product SKU.** Tab5 sold without Dragon. Has to be self-sufficient long-term.

**The OpenRouter audio APIs (shipped 2026-05-07)** make this radically simpler than it would have been a month ago:

- `POST /api/v1/audio/transcriptions` — STT
- `POST /api/v1/chat/completions` (SSE) — streaming LLM
- `POST /api/v1/audio/speech` — TTS
- `POST /api/v1/embeddings` — embeddings (for RAG)

All four under one host, one TLS endpoint, one API key. `~latest` model aliases mean Tab5 doesn't need to update model strings as new versions ship. Free response caching covers retry-on-disconnect cost.

**Outcome:** a new voice mode (`vmode=5 SOLO_DIRECT`) where Tab5 talks directly to OpenRouter — no Dragon, no K144 — plus a mode pill that lets the user toggle between `vmode=4` (K144 onboard) and `vmode=5` (cloud direct) per turn. Eventually a `connection_mode` toggle hides Dragon-mediated modes for users on the standalone SKU.

---

## Decisions (locked from clarifying round)

- **Routing default:** user picks per turn via mode pill (no auto/smart-routing in v1)
- **Provider for v1:** OpenRouter only — single host, single key
- **Local state:** sessions, chat, notes, **and RAG facts** all live on Tab5; embeddings outsourced to OpenRouter (no on-device embedding model)
- **API-key provisioning:** QR code scan via Tab5 camera (re-uses existing camera path)
- **Mode pill:** tap to cycle (K144 → SOLO → K144 …); state persists in NVS `vmode`
- **Phasing:** Approach D — ship `vmode=5` first (Phase 1), then add `connection_mode` abstraction (Phase 2), defer onboarding overhaul (Phase 3)

## Out of scope for v1

- Multi-provider direct (Anthropic / OpenAI native paths) — single OpenRouter only
- On-device embedding model (Sherpa-onnx / MiniLM) — embeddings via OpenRouter
- Smart auto-routing (short → K144, complex → cloud) — explicit per-turn pill toggle only
- On-device skill execution (timesense, web search, etc.) — Solo mode is chat-only for v1
- Cross-device session sync — sessions stay on Tab5 SD
- Dragon mode auto-detection at boot — deferred to Phase 2

---

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│ Tab5 (vmode=5 SOLO_DIRECT)                              │
│                                                         │
│  ui_chat / ui_voice                                     │
│         │                                               │
│         ▼                                               │
│  voice_solo.{c,h}      ◄── mode pill (k144↔solo)        │
│  state machine          ──► (vmode=4 K144 path stays)   │
│         │                                               │
│         ▼                                               │
│  openrouter_client.{c,h}                                │
│  ├─ STT  : POST /api/v1/audio/transcriptions  (multipart)│
│  ├─ LLM  : POST /api/v1/chat/completions      (SSE)     │
│  ├─ TTS  : POST /api/v1/audio/speech          (chunked) │
│  └─ EMB  : POST /api/v1/embeddings            (batch)   │
│         │                                               │
│         ▼                                               │
│  TLS → openrouter.ai (cert bundle already attached)     │
│                                                         │
│  solo_session_store.{c,h}  →  /sdcard/sessions/<id>.json│
│  solo_rag.{c,h}            →  /sdcard/rag.bin           │
│  notes (existing module)    →  /sdcard/notes.json       │
└─────────────────────────────────────────────────────────┘
```

Existing voice modes 0–4 are not changed. `vmode=5` is added as the 6th tier.

---

## Phase 1 — `vmode=5 SOLO_DIRECT` (≈2 weeks)

### New modules

| File | Purpose | Approx LOC |
|------|---------|-----------|
| `main/voice_solo.{c,h}` | Solo mode orchestrator: state machine, STT → LLM → TTS chain, chat history wiring | 600 |
| `main/openrouter_client.{c,h}` | HTTP wrapper: 4 verbs (STT / LLM-stream / TTS / embeddings), SSE parser, auth header injection, error handling | 400 |
| `main/openrouter_sse.{c,h}` | Tiny line-buffered SSE frame parser: `data: {...}` extraction, `[DONE]` terminator, comment lines | 80 |
| `main/solo_session_store.{c,h}` | SD-backed session persistence: per-session JSON file, max 50 turns/session (matches Dragon), session list, resume | 250 |
| `main/solo_rag.{c,h}` | Flat float32 vector store + brute-force cosine search; record format `{fact_id u32, ts u32, vec_dim u16, text_len u16, text bytes, vec float32[vec_dim]}` | 300 |
| `main/qr_decoder.{c,h}` | quirc wrapper for camera frame → QR payload string | 150 |

### Modified modules

| File | Change |
|------|--------|
| `main/voice.h` | Add `#define VMODE_SOLO_DIRECT 5`; bump `vmode` NVS range comment to 0–5 |
| `main/voice_modes.{c,h}` | Add `VOICE_MODES_ROUTE_SOLO` route kind; route_text dispatcher checks `vmode==5` and calls `voice_solo_send_text()`; mic-stop hook routes mic buffer to `voice_solo_send_audio()` instead of WS when in solo |
| `main/voice.c` | Mode pill cycling: when current is k144, advance to solo; when solo, advance back to k144. Existing dragon-mode pill behaviour unchanged outside solo/k144 |
| `main/ui_settings.c` | Add "Cloud Direct" section: API key field (masked, "Scan QR" button), 4 model fields (LLM, STT, TTS, Embeddings) with sensible `~latest` defaults |
| `main/ui_camera.c` | New "QR scan" mode: continuous capture into a quirc decoder, exits with payload string when a valid QR is found |
| `main/ui_onboarding.c` | Optional Phase 1.5: extra step asking "Use Dragon at home, or cloud direct?" — writes connection_mode hint to NVS (used by Phase 2) |
| `main/settings.{c,h}` | New NVS keys (all in `"settings"` namespace) — see schema below |
| `main/CMakeLists.txt` + `main/idf_component.yml` | Add quirc dependency (or vendor `quirc.{c,h}` directly — ~25 KB code) |

### NVS schema additions

| Key | Type | Default | Notes |
|-----|------|---------|-------|
| `or_key` | str | `""` | OpenRouter API key (sk-or-v1-...). 64 chars typical. |
| `or_mdl_llm` | str | `~anthropic/claude-haiku-latest` | Streaming chat model |
| `or_mdl_stt` | str | `whisper-1` | Transcription model |
| `or_mdl_tts` | str | `tts-1` | Speech model. Voice param `alloy` or per-user via NVS later. |
| `or_mdl_emb` | str | `text-embedding-3-small` | 1536-dim embeddings |
| `or_voice` | str | `alloy` | TTS voice preset |

### QR-code provisioning flow

QR payload (JSON, fits comfortably in QR v25 ≤ 1853 chars):

```json
{
  "or_key": "sk-or-v1-abcdef...",
  "models": {
    "llm": "~anthropic/claude-haiku-latest",
    "stt": "whisper-1",
    "tts": "tts-1",
    "emb": "text-embedding-3-small"
  },
  "voice": "alloy"
}
```

Flow:

1. User taps "Scan QR" in `ui_settings.c` → opens camera in QR-scan mode
2. `ui_camera.c` runs continuous JPEG decode → quirc → JSON parse
3. On valid payload: persist to NVS, dismiss camera, return to settings
4. Companion HTML page (any QR generator works — single static `<input>` form) lets the user paste the key and download/print a QR

**Why QR over keyboard entry:** OpenRouter keys are 64+ chars and case-sensitive — typing on the on-screen keyboard is error-prone and slow.

### Voice path for `vmode=5`

State machine extension (existing voice states reused):

```
IDLE  ── mic press ──────►  LISTENING
LISTENING  ── stop / 5s silence ──►  PROCESSING
PROCESSING:
  1) POST /audio/transcriptions  with PCM-16LE @ 16kHz mono (multipart)
     ─► transcript text
  2) Build chat-completions request with last N turns + transcript
  3) POST /chat/completions stream:true
     ─► SSE delta tokens, accumulate into reply, render to chat UI
  4) On [DONE]: post reply to /audio/speech (response_format=wav)
     ─► chunked WAV stream → playback ring → speaker
  5) Append (user_msg, assistant_msg) to /sdcard/sessions/<id>.json
SPEAKING ── audio drain done ──►  IDLE
```

Streaming details:

- **STT:** the entire mic buffer is uploaded after stop (multipart form, field `file=audio.wav`, `model=whisper-1`). Streaming STT is OpenRouter-side TBD; v1 is post-stop transcribe.
- **LLM:** SSE; `openrouter_sse_parse_chunk()` is called every chunk, dispatches `delta.content` strings to a callback that writes into chat. Reply is assembled into a single string before going to TTS.
- **TTS:** request `response_format=wav` (raw PCM-16LE 24kHz mono per OpenAI TTS-1 spec; Tab5 downsamples 24k→16k inline OR upsamples 24k→48k for the speaker — confirm during implementation). Audio chunks are pushed to the existing `playback_drain_task` ring as they arrive.
- **Cancellation:** mic-cancel mid-PROCESSING aborts in-flight HTTP via `esp_http_client_close()`.

### RAG path

- "remember X" or auto-extracted facts (e.g., LLM tool-call `remember`) → text → POST `/v1/embeddings`
- Vector returned (1536-dim float32 default, or 768 for `text-embedding-3-small` low-dim mode)
- Append record to `/sdcard/rag.bin`:

```c
struct rag_record_v1 {
    uint32_t magic;        // 'RAGv'
    uint32_t fact_id;      // monotonic counter, NVS-persisted
    uint32_t ts_unix;      // timestamp
    uint16_t vec_dim;
    uint16_t text_len;
    char     text[text_len];
    float    vec[vec_dim];
};
```

- "recall X" or pre-LLM-context auto-recall → embed query → mmap-style scan over `rag.bin` → top-K cosine
- For ≤10k facts × 1536 floats = ~60MB scan. SD read at 1.5 Mbps SDIO ≈ 5s. Acceptable for "what did I say about X" UX; not real-time.
- **Optimization (out-of-scope for v1):** keep most-recent-1000 vectors in PSRAM for sub-second hot recall.

### Existing utilities to reuse

- `media_cache.c:12-84` — chunked HTTP read pattern (copy idiom for SSE; mediating chunks across reads is the trick)
- `voice.c:~950-1050` — mic capture, downsample 3:1, PSRAM buffering (no change needed; just route the buffer to voice_solo)
- `voice.c:~1300-1400` — `playback_drain_task` → ES8388 DAC; raw PCM int16 at 16kHz, upsamples 1:3 internally (TTS audio decoded from response feeds this directly)
- `voice_codec.c:108-110` — OPUS decoder (only relevant if OpenRouter supports OPUS TTS output; otherwise WAV path is simpler)
- `ui_camera.{c,h}` — existing camera capture, frame buffer in PSRAM (extend with QR-scan loop)
- `voice_onboard.{c,h}` — K144 path stays untouched; mode pill simply toggles `vmode` between 4 and 5
- `settings.{c,h}` — typed getters/setters; NVS commit per setter call already handles wear-leveling
- `chat_msg_store.{c,h}` — in-memory chat buffer; extend to flush to SD on session boundaries
- `ui_settings.c:~888-950` — existing LLM model picker pattern; replicated for solo mode's 4 model fields

### Binary size budget

| Component | Estimate |
|-----------|---------|
| voice_solo.c | 6 KB |
| openrouter_client.c + sse | 5 KB |
| solo_session_store.c | 3 KB |
| solo_rag.c | 4 KB |
| qr_decoder.c (quirc) | 25 KB |
| TLS bundle additions (none — already includes Cloudflare/AWS roots) | 0 KB |
| **Total firmware impact** | **~45 KB** (well below the 1 MB OTA-slot headroom) |

### Heap / PSRAM impact

| Use | Size | Heap |
|-----|------|------|
| HTTP response buffer | 64 KB | PSRAM |
| Chat history cache (per session) | 256 KB | PSRAM |
| QR decoder working frame | 1280×720 grayscale = 900 KB | PSRAM (during QR scan only) |
| RAG in-memory cache (Phase 2) | ≤1 MB | PSRAM |
| **Internal SRAM impact** | **~0** — all big buffers in PSRAM (per CLAUDE.md rule) |

### Failure modes + observability

- WiFi loss mid-stream → render a "reconnecting…" toast, retry once, then fall back to error toast with hint to switch to K144
- 401 invalid key → "API key invalid — re-scan QR or check settings"
- 429 rate limit → exponential backoff, surface "rate-limited, slow down" if persistent
- Stuck PROCESSING > 60s → cancel HTTP, surface error
- New obs events:
  - `solo.stt_done` (latency_ms)
  - `solo.llm_done` (latency_ms, tokens_in, tokens_out)
  - `solo.tts_done` (audio_ms, bytes)
  - `solo.error` (stage = stt|llm|tts|embed, reason)

### Verification

```bash
cd /home/rebelforce/projects/TinkerTab
. /home/rebelforce/esp/esp-idf/export.sh
idf.py build && idf.py -p /dev/ttyACM0 flash

# Provision via QR (recommended) — print QR from companion HTML page, scan with Tab5
# OR provision via debug-server fallback:
TOKEN=05eed3b13bf62d92cfd8ac424438b9f2
curl -sS -H "Authorization: Bearer $TOKEN" -X POST http://192.168.70.128:8080/settings \
  -d '{"or_key":"sk-or-v1-...", "or_mdl_llm":"~anthropic/claude-haiku-latest", "or_mdl_stt":"whisper-1", "or_mdl_tts":"tts-1", "or_mdl_emb":"text-embedding-3-small"}'

# Switch to solo mode
curl -sS -H "Authorization: Bearer $TOKEN" -X POST 'http://192.168.70.128:8080/mode?m=5'

# Send a chat turn
curl -sS -H "Authorization: Bearer $TOKEN" -X POST http://192.168.70.128:8080/chat \
  -d '{"text":"give me a 5-word weather metaphor"}'

# Watch the obs ring
curl -sS -H "Authorization: Bearer $TOKEN" 'http://192.168.70.128:8080/events?since=0' | python3 -m json.tool

# Voice turn (mic press via touch injection)
curl -sS -H "Authorization: Bearer $TOKEN" -X POST http://192.168.70.128:8080/touch \
  -d '{"x":360,"y":1100,"action":"long_press","duration_ms":3000}'
# Then verify audio came back: tail /screen + /voice while it processes

# E2E story (NEW)
TAB5_URL=http://192.168.70.128:8080 TAB5_TOKEN=$TOKEN \
  /usr/bin/python3 tests/e2e/runner.py story_solo

# Heap stability under 5min of solo chat
curl -sS -H "Authorization: Bearer $TOKEN" http://192.168.70.128:8080/heap | python3 -m json.tool
# Expect: internal_largest stable >100KB, PSRAM stable, no chat_evictions
```

Acceptance criteria:

- [ ] `idf.py build` clean with new modules
- [ ] QR scan completes < 5s with a valid payload, NVS updated
- [ ] First-turn solo voice: STT < 3s, LLM TTFT < 2s, TTS playback starts < 1s after [DONE]
- [ ] 5 sequential turns in solo, total memory delta < 200KB (no leak)
- [ ] WiFi pulled mid-stream → recovers cleanly within 30s, no reboot
- [ ] Mode pill cycle K144 → SOLO → K144 round-trips correctly without crash
- [ ] Sessions persist across reboot (resume from `/sdcard/sessions/<id>.json`)
- [ ] `remember` then `recall` round-trip (fact stored, recalled by semantic similarity)
- [ ] Existing modes 0–4 untouched: smoke test passes for each
- [ ] No new clang-format violations on the diff

---

## Phase 2 — `connection_mode` abstraction (≈1 week)

After Phase 1 stabilizes:

- Add NVS key `conn_mode` ∈ `{"dragon", "solo"}`, default `"dragon"` (preserves existing behaviour)
- `ui_settings.c`: top-level toggle near the top
- When `conn_mode == "solo"`:
  - Voice-mode picker shows only K144 (4) and SOLO (5)
  - Dragon-host / Dragon-port fields hidden
  - LLM model picker for Dragon modes hidden
  - Mode pill cycles k144 ↔ solo only
- When `conn_mode == "dragon"`:
  - Today's behaviour: all 5 modes (0-4) selectable
- Onboarding wizard: optional question "do you have a Dragon server?" — writes `conn_mode` accordingly

This is mostly a UI refactor + a few `if (conn_mode == ...)` gates. No new modules.

---

## Phase 3 — defer

- Drop Dragon prompts from onboarding entirely for fresh standalone-SKU devices
- Add multi-provider direct (Anthropic + OpenAI) for users who want to bypass OpenRouter
- On-device skill scaffolding (timesense Pomodoro on Tab5 without Dragon)
- On-device embedding model (Sherpa-onnx MiniLM) so RAG works without internet

---

## Risks / open questions

- **TTS audio format:** OpenRouter's `/audio/speech` returns whatever the provider underneath returns. OpenAI TTS-1 returns WAV @ 24kHz mono; ElevenLabs returns MP3. Phase 1 implementation should request `response_format=wav` and verify; if the underlying provider doesn't honour the param, we may need an MP3 decoder (helix-mp3, ~50KB) or downselect to WAV-only providers.
- **STT streaming:** v1 is post-stop transcribe (full mic buffer up at once). If OpenRouter ships streaming transcription later, we can backfill — but the current UX of "stop and wait 1-3s for transcript" is acceptable.
- **Cert bundle coverage:** ESP-IDF cert bundle reaches `*.ngrok.dev` (verified). `openrouter.ai` uses Cloudflare → DigiCert, also bundled by default. Verify on hardware as a build-time test.
- **Quirc license:** quirc is ISC, embedded-friendly. Vendor it under `main/quirc/` rather than ESP-IDF component manager dep, to keep build stable.
- **Mode pill discoverability:** if Dragon-mediated users don't expect a "K144 ↔ Cloud" pill, add a one-time hint toast on first encounter. Ties into existing onboarding hint pattern.
- **Cost guard:** reuse existing `cap_mils` / `spent_mils` NVS pair for solo mode token-cost tracking; OpenRouter response headers include `x-ratelimit-remaining-credits` and per-call cost metadata.

---

## Files to modify (Phase 1 master list)

**New:**
- `main/voice_solo.{c,h}`
- `main/openrouter_client.{c,h}`
- `main/openrouter_sse.{c,h}`
- `main/solo_session_store.{c,h}`
- `main/solo_rag.{c,h}`
- `main/qr_decoder.{c,h}` (vendored quirc under `main/quirc/`)
- `tests/e2e/scenarios/story_solo.py` (new E2E scenario)

**Modified:**
- `main/voice.h` — add VMODE_SOLO_DIRECT
- `main/voice_modes.{c,h}` — solo route + dispatch
- `main/voice.c` — mode pill cycling, mic-stop dispatch
- `main/ui_settings.c` — Cloud Direct section
- `main/ui_camera.{c,h}` — QR scan mode
- `main/ui_onboarding.c` — optional connection-mode hint
- `main/settings.{c,h}` — new NVS keys
- `main/debug_server_settings.c` — surface the new NVS keys via /settings
- `main/CMakeLists.txt` — list new sources
- `tests/e2e/runner.py` — register `story_solo`
- `tests/e2e/audit_tinkerclaw.py` — add solo-mode assertions

**Read-only references (utilities to study + reuse):**
- `main/media_cache.c` — chunked HTTP idioms
- `main/voice_codec.c` — OPUS encode/decode
- `main/voice_onboard.{c,h}` — K144 path (untouched)
- `main/chat_msg_store.{c,h}` — chat buffer pattern
- `main/voice.c:~1300-1400` — playback drain task
