# Conversational STT/TTS Pipeline Research for TinkerClaw
**Date: 2026-03-27**

Architecture: Tab5 (ESP32-P4) ↔ WiFi ↔ Dragon (ARM mini-PC, 8GB RAM, Linux) running Ollama

Goal: User speaks → Tab5 captures → Dragon does STT → LLM → TTS → audio back to Tab5 → plays response. Target: <1s total latency.

---

## Table of Contents
1. [Local STT Options](#1-local-stt-options)
2. [Local TTS Options](#2-local-tts-options)
3. [Audio Streaming Architecture](#3-audio-streaming-architecture)
4. [Wake Word Detection](#4-wake-word-detection)
5. [End-to-End Conversational Pipeline](#5-end-to-end-conversational-pipeline)
6. [Existing Frameworks & Implementations](#6-existing-frameworks--implementations)
7. [Recommended Architecture](#7-recommended-architecture)
8. [Latency Budget](#8-latency-budget)

---

## 1. Local STT Options

### Summary Comparison

| Model | Size | RAM | Speed (ARM CPU) | Accuracy (WER) | Streaming | Best For |
|-------|------|-----|-----------------|-----------------|-----------|----------|
| **Moonshine Tiny** | 27M params | <100MB | ~200ms for 10s audio | ~Whisper Small level | Yes (v2) | **RECOMMENDED** — best speed/accuracy on ARM |
| **Moonshine Base** | ~100M params | ~200MB | ~400ms for 10s audio | Better than Whisper Small | Yes (v2) | Best accuracy without GPU |
| Whisper.cpp tiny | 39M | ~200MB | ~1.5s for <30s audio | Higher WER | No (batch) | Fallback option |
| Whisper.cpp small | 244M | ~1GB | ~5-8s for 30s audio | Good | No (batch) | Quality when latency doesn't matter |
| Whisper.cpp base | 74M | ~500MB | ~3s for 30s audio | Moderate | No (batch) | Balanced option |
| Faster-Whisper small | 244M | ~1GB | ~2-3s for 30s audio | Good | No | CPU faster than whisper.cpp |
| Vosk small-en | ~50MB model | ~300MB | Real-time streaming | Lower than Whisper | Yes | Ultra-lightweight, real-time |
| Distil-Whisper large-v3 | 756M | ~2-3GB | Too slow on ARM CPU | Near large-v3 | No | GPU-only |

### Detailed Analysis

#### Moonshine (RECOMMENDED for Dragon)
- **Developer**: Useful Sensors (Pete Warden)
- **Key advantage**: Variable-length encoder — processes only actual audio length, no 30s zero-padding like Whisper
- **ARM target**: Designed specifically for edge/embedded. Targets 8MB RAM for short utterances
- **Speed**: 5x faster than Whisper on 10-second clips, 1.7x faster overall
- **Accuracy**: Matches or exceeds Whisper Small despite much smaller model
- **Moonshine v2** (2026): Adds streaming/ergodic encoder with sliding-window attention for low-latency streaming inference. Sub-200ms latency on Raspberry Pi
- **ONNX runtime**: Portable C++ core, runs via OnnxRuntime for cross-platform ARM performance
- **Language**: English only (sufficient for conversational assistant)
- **Integration**: Works with sherpa-onnx ecosystem, which OVOS also supports

#### Whisper.cpp
- **ARM support**: Compiles to ARM64-native binaries with NEON intrinsics
- **Pi 5 performance**: 3-5x real-time on base model. Tiny model: ~1.5s for <30s audio
- **RAM**: Tiny model fits in 200MB, small in ~1GB. Quantized variants fit in 2GB
- **Limitation**: Batch processing only — must wait for complete utterance. 30s fixed window wastes compute on short phrases
- **Good for**: Fallback when Moonshine accuracy insufficient

#### Faster-Whisper (CTranslate2)
- **Speed**: ~5x faster than whisper.cpp on CPU via INT8 quantization
- **Problem on ARM**: CTranslate2 has limited ARM optimization. Best on x86 with AVX/SSE
- **RAM**: Similar to whisper.cpp but with quantization benefits
- **Verdict**: Not ideal for ARM — whisper.cpp with NEON is often faster on aarch64

#### Vosk
- **Ultra-lightweight**: 50MB models, 300MB RAM total
- **Real-time streaming**: Processes audio as it arrives, zero-latency API
- **Accuracy**: Lower than Whisper/Moonshine but acceptable for command-style input
- **Best use case**: If Dragon had less RAM, or as wake-word-to-command fallback
- **20+ languages** supported

#### Sherpa-ONNX (Worth Considering)
- **All-in-one**: STT + TTS + VAD + speaker diarization in one framework
- **ARM optimized**: Builds for aarch64 and armv7l, supports Cortex-A7+
- **Models**: Supports Moonshine, Whisper, Zipformer, Paraformer models via ONNX
- **WebSocket server**: Built-in streaming WebSocket server for STT
- **Active**: Latest release March 2026, Python 3.12-3.14 ARM64 wheels
- **Verdict**: Could be the single runtime for both STT and TTS on Dragon

### STT Recommendation

**Primary: Moonshine v2 via sherpa-onnx** — Sub-200ms streaming STT on ARM, best speed/accuracy ratio, designed for exactly this use case.

**Fallback: Vosk** — If Moonshine has issues, Vosk gives real-time streaming with lower accuracy but battle-tested stability.

---

## 2. Local TTS Options

### Summary Comparison

| Model | Size | RAM | Speed (ARM CPU) | Quality | Streaming | Best For |
|-------|------|-----|-----------------|---------|-----------|----------|
| **Piper** | 15-60MB | ~500MB | Real-time+ (20-30ms/sentence) | Good (medium voices) | Yes (sentence) | **RECOMMENDED** — proven fast on ARM |
| Kokoro-82M | ~80MB (ONNX) | ~500MB | Sub-real-time on Pi 4 | Excellent (arena #1) | Partial | Best quality if speed OK |
| Kokoro-82M (ONNX quantized) | ~80MB | ~500MB | Slightly < real-time on ARM | Excellent | Partial | Quality priority |
| XTTS-v2 | ~1.8GB | ~4GB+ | Too slow on ARM CPU | Excellent, voice cloning | Yes (<200ms GPU) | GPU-only |
| F5-TTS | ~600MB | ~2GB+ | Sub-7s (GPU), slow CPU | Good, zero-shot cloning | No | GPU-only |
| Fish Speech | ~1-2GB | ~4GB+ | 150ms latency (GPU) | Good, multilingual | Yes | GPU-only |
| Bark | ~5GB | ~6GB+ | Very slow, even on GPU | Most natural | No | Not practical |
| StyleTTS 2 | ~200MB | ~1GB | Moderate | State-of-art quality | No | Research, GPU preferred |
| Edge TTS | Cloud API | N/A | ~100ms (network) | Very good | Yes | If internet available |

### Detailed Analysis

#### Piper TTS (RECOMMENDED for Dragon)
- **Speed**: Generates speech in 20-30ms per sentence on CPU. Real-time+ on Raspberry Pi 4, much faster on Pi 5 class hardware
- **RAM**: ~500MB total, ONNX runtime
- **Quality tiers**: x_low (16kHz), low, medium (22.05kHz), high — medium is the sweet spot
- **Voices**: 100+ voices across many languages
- **Architecture**: VITS-based, ONNX inference, optimized for ARM (aarch64 + armv7l builds)
- **Integration**: Native Wyoming protocol support, works with sherpa-onnx
- **Streaming**: Can stream sentence-by-sentence (generate while LLM still producing text)
- **Proven**: Default TTS for Home Assistant voice pipeline, battle-tested on Pi hardware

#### Kokoro-82M
- **Quality**: #1 on TTS Arena leaderboard (Jan 2026), beating XTTS and MetaVoice despite being 82M params
- **Speed on GPU**: <0.3s across all text lengths, 210x real-time on RTX 4090
- **Speed on ARM CPU**: Sub-real-time on Pi 4 (2GB) — audio takes longer to generate than to play
- **Pi 5 / Dragon**: Likely near-real-time or slightly above on Dragon's ARM CPU (better than Pi 4)
- **ONNX**: Available via kokoro-onnx package, ~80MB quantized model
- **License**: Apache 2.0
- **Verdict**: Test on Dragon — if it hits real-time, use it. If not, Piper is the safe choice

#### XTTS-v2 (Coqui)
- **Voice cloning**: 6-second reference audio for any voice
- **Streaming**: <200ms latency but requires GPU
- **17 languages**
- **Problem**: 1.8GB model, needs GPU for acceptable latency. Coqui company shut down (community maintained)
- **Verdict**: Not viable for ARM CPU-only Dragon

#### F5-TTS
- **Zero-shot voice cloning** with flow matching
- **RTF 0.15** — but on GPU only
- **Verdict**: Too heavy for ARM CPU

#### Bark (Suno)
- **Most natural sounding**, includes laughter, breathing, music
- **Very slow**: Even on GPU, multi-second generation times
- **Huge model**: ~5GB
- **Verdict**: Not practical for conversational use

#### Edge TTS
- **Microsoft's free API** — not local but extremely fast (~100ms)
- **Quality**: Very good, many voices
- **Use case**: Fallback when internet available, or for testing pipeline before local TTS is tuned
- **Limitation**: Requires internet, may have rate limits

### TTS Recommendation

**Primary: Piper TTS (medium quality voices)** — Proven real-time on ARM, 20-30ms generation, native Wyoming support, great voice selection.

**Upgrade path: Kokoro-82M ONNX** — Test on Dragon hardware. If it hits ≥1x real-time, switch to Kokoro for significantly better voice quality. The 82M parameter model via ONNX may be fast enough on Dragon's ARM if it's stronger than Pi 4.

**Cloud fallback: Edge TTS** — For when internet is available and you want the best quality with zero local compute.

---

## 3. Audio Streaming Architecture

### Protocol Comparison

| Protocol | Latency | Reliability | Complexity | Best For |
|----------|---------|-------------|------------|----------|
| **WebSocket** | ~5-20ms | TCP-reliable | Medium | **RECOMMENDED** — bidirectional, proven |
| HTTP chunked | ~50-100ms | TCP-reliable | Low | Simple but higher latency |
| UDP raw | ~1-5ms | Unreliable | High | Ultra-low latency, needs FEC |
| MQTT + UDP | ~10ms | Hybrid | High | XiaoZhi approach, complex |

### Recommended: WebSocket with OPUS Encoding

Based on XiaoZhi's proven architecture and ESP32 voice assistant projects:

#### Capture Path (Tab5 → Dragon)
```
I2S Mic (INMP441/SPH0645) → ESP32-P4 I2S DMA → PCM 16-bit 16kHz
→ VAD check (on ESP32-P4, see §4)
→ OPUS encode (16kHz, ~16kbps)
→ WebSocket binary frame → Dragon server
```

#### Playback Path (Dragon → Tab5)
```
Dragon TTS output → PCM 16-bit 24kHz
→ OPUS encode (24kHz)
→ WebSocket binary frame → Tab5
→ OPUS decode → I2S DMA → DAC/Speaker
```

#### Why OPUS?
- Compresses 16kHz 16-bit PCM (256 kbps) down to ~16-32 kbps
- Designed for real-time speech, handles packet loss gracefully
- ESP32-P4 has enough CPU for OPUS encode/decode
- XiaoZhi already implements this on ESP32-P4

#### Why WebSocket over raw UDP?
- TCP reliability — no lost audio packets
- Bidirectional on single connection — control messages (JSON) + audio (binary) on same socket
- Firewall-friendly
- Built-in framing — no custom packet protocol needed
- Latency overhead (~5-15ms) is negligible vs. STT/LLM/TTS processing time

#### Audio Formats
- **STT input**: PCM 16-bit, 16kHz, mono (standard for all STT models)
- **TTS output**: PCM 16-bit, 22.05kHz or 24kHz (Piper outputs 22.05kHz medium, Kokoro 24kHz)
- **Wire format**: OPUS encoded in both directions to save WiFi bandwidth
- **Buffer size**: 20ms frames (320 samples at 16kHz) — standard for OPUS

#### ESP-IDF I2S Implementation
```c
// Mic capture (I2S RX)
i2s_chan_config_t rx_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
i2s_std_config_t rx_std_cfg = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
    .gpio_cfg = { .mclk = I2S_GPIO_UNUSED, .bclk = GPIO_BCLK, .ws = GPIO_WS, .dout = I2S_GPIO_UNUSED, .din = GPIO_DIN }
};

// Speaker playback (I2S TX)
i2s_chan_config_t tx_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
i2s_std_config_t tx_std_cfg = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(24000),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
    .gpio_cfg = { .mclk = I2S_GPIO_UNUSED, .bclk = GPIO_BCLK_OUT, .ws = GPIO_WS_OUT, .dout = GPIO_DOUT, .din = I2S_GPIO_UNUSED }
};
```

**Important ESP32-P4 note**: DPI framebuffer in PSRAM needs `esp_cache_msync()` or DMA sees stale data. Same applies to I2S DMA buffers if they live in PSRAM.

---

## 4. Wake Word Detection

### Options Comparison

| Engine | Runs On | Custom Words | Latency | RAM | Accuracy |
|--------|---------|-------------|---------|-----|----------|
| **ESP-SR WakeNet9** | ESP32-P4 (on-device) | Limited prebuilt | <100ms | ~1MB | Good |
| **microWakeWord** | ESP32-S3/P4 | Trainable | <100ms | <1MB | Good |
| Porcupine | ESP32 or server | Easy custom training | <100ms | <1MB | Very good |
| OpenWakeWord | Server (Dragon) | Custom trainable | ~200ms | ~100MB | Good |
| Mycroft Precise | Server | Custom trainable | ~200ms | ~50MB | Moderate |

### Recommendation: Two-Tier Approach

#### Tier 1: On-Device VAD + Wake Word (Tab5 ESP32-P4)
- **ESP-SR WakeNet9**: Runs natively on ESP32-P4, Espressif's own library
- **ESP-SR VADNet**: New VAD model released Feb 2025, runs on P4
- **How it works**: P4 continuously listens using I2S mic. VADNet detects speech. WakeNet9 checks for wake phrase
- **Power**: Only streams to Dragon after wake word detected — saves WiFi bandwidth and Dragon CPU
- **Custom "Hey Glyph"**: ESP-SR supports custom wake words but requires Espressif's training service. Alternative: use Porcupine for easy custom word training on ESP32

#### Tier 2: Server-Side Verification (Dragon)
- **OpenWakeWord**: Run on Dragon as backup verification to reduce false positives
- **When to use**: If ESP-SR false-positive rate is too high, add Dragon-side verification

### Custom "Hey Glyph" Wake Word

**Option A — Porcupine (Easiest)**:
- Train custom wake word by typing the phrase on Picovoice console
- Generates model file that runs on ESP32-P4
- Free tier: 3 custom wake words
- Limitation: Proprietary, requires license for production

**Option B — ESP-SR Custom Training**:
- Contact Espressif for custom WakeNet model training
- Or use the open-source training pipeline with collected audio samples
- More work but fully open-source

**Option C — microWakeWord**:
- ESPHome's on-device wake word engine
- Trainable with synthetic data
- Runs on ESP32-S3 and likely P4
- Community-driven, good Home Assistant integration

### VAD Strategy

**Run VAD on ESP32-P4** using ESP-SR VADNet:
- Saves bandwidth: Only stream when speech detected
- Saves Dragon CPU: No processing silence
- ESP32-P4 has dual 400MHz cores — plenty for VAD + wake word
- VAD output triggers: start streaming to Dragon (even before wake word confirmed, to reduce latency)

---

## 5. End-to-End Conversational Pipeline

### Optimal Sub-1-Second Pipeline

The key insight: **overlap everything**. Don't wait for one step to finish before starting the next.

```
Timeline (target ~800ms end-to-end):

User speaks: "What's the weather?"
├── [0ms]    VAD detects speech on ESP32-P4
├── [50ms]   Start streaming OPUS audio to Dragon via WebSocket
├── [100ms]  Dragon begins streaming STT (Moonshine v2)
│            ↳ Partial transcripts available as user speaks
├── [~600ms] User stops speaking → VAD detects silence
├── [~650ms] Final STT result: "What's the weather?"
├── [~700ms] Send to Ollama (streaming mode)
├── [~900ms] First LLM token arrives
├── [~950ms] First sentence complete → send to TTS
├── [~980ms] Piper generates first audio chunk (20-30ms)
├── [~1000ms] First audio playing on Tab5 speaker
│            ↳ While LLM still generating remaining tokens
│            ↳ While TTS processing next sentences
└── [~2-3s]  Full response complete
```

### Pipeline Architecture

```
┌─────────────────────────────────────────────────────────────┐
│  Tab5 (ESP32-P4)                                            │
│  ┌──────────┐  ┌──────────┐  ┌──────────────┐              │
│  │ I2S Mic  │→│  VAD/WW   │→│ OPUS Encode  │──WebSocket──→ │
│  └──────────┘  └──────────┘  └──────────────┘              │
│  ┌──────────┐  ┌──────────────┐                             │
│  │ I2S Spkr │←│ OPUS Decode  │←──────WebSocket────────────  │
│  └──────────┘  └──────────────┘                             │
└─────────────────────────────────────────────────────────────┘
                        │ WiFi │
┌─────────────────────────────────────────────────────────────┐
│  Dragon (ARM mini-PC)                                       │
│                                                             │
│  WebSocket Server (Python asyncio)                          │
│  ┌──────────────────────────────────────────────┐           │
│  │ Audio In → OPUS Decode → Moonshine STT       │           │
│  │              (streaming, partial results)     │           │
│  │                      ↓                        │           │
│  │           Ollama API (streaming)              │           │
│  │              /api/generate                    │           │
│  │                      ↓                        │           │
│  │           Piper TTS (sentence-by-sentence)    │           │
│  │                      ↓                        │           │
│  │           OPUS Encode → WebSocket Out         │           │
│  └──────────────────────────────────────────────┘           │
│                                                             │
│  Ollama (separate process)                                  │
│  ├── Model: Gemma 3 4B Q4 (~3GB RAM)                       │
│  └── Or: Phi-3.5 mini 3.8B Q4 (~2.5GB RAM)                │
└─────────────────────────────────────────────────────────────┘
```

### Key Techniques for Low Latency

1. **Streaming STT**: Moonshine v2 processes audio in real-time as user speaks, not after silence
2. **Streaming LLM**: Use Ollama `/api/generate` with `stream: true` — process tokens as they arrive
3. **Sentence-Level TTS**: Buffer LLM output until sentence boundary (`.`, `!`, `?`), send sentence to Piper immediately
4. **Streaming TTS playback**: Start playing first TTS chunk while subsequent sentences are still being generated
5. **Parallel processing**: STT, LLM, and TTS run as separate async tasks, connected by queues
6. **Pre-warm models**: Keep Moonshine, Piper, and Ollama loaded in RAM permanently

### Ollama Integration

```python
import aiohttp

async def stream_llm(prompt: str):
    async with aiohttp.ClientSession() as session:
        async with session.post('http://localhost:11434/api/generate', json={
            'model': 'gemma3:4b-q4_K_M',
            'prompt': prompt,
            'stream': True
        }) as resp:
            sentence_buffer = ""
            async for line in resp.content:
                data = json.loads(line)
                token = data.get('response', '')
                sentence_buffer += token
                # Flush on sentence boundary
                if any(sentence_buffer.rstrip().endswith(p) for p in '.!?'):
                    yield sentence_buffer.strip()
                    sentence_buffer = ""
            if sentence_buffer.strip():
                yield sentence_buffer.strip()
```

### RAM Budget on Dragon (8GB)

| Component | RAM | Notes |
|-----------|-----|-------|
| Linux OS + services | ~1GB | Base system |
| Ollama + Gemma3 4B Q4 | ~3.5GB | Loaded permanently |
| Moonshine STT (ONNX) | ~200MB | Via sherpa-onnx |
| Piper TTS | ~500MB | ONNX runtime + voice model |
| Python server process | ~200MB | WebSocket + pipeline logic |
| OPUS codec buffers | ~50MB | Encode/decode |
| **Total** | **~5.5GB** | **Leaves ~2.5GB headroom** |

---

## 6. Existing Frameworks & Implementations

### Wyoming Protocol (Home Assistant)
- **What**: Standard protocol for linking STT/TTS/Wake Word engines
- **Components**: Wyoming Whisper (STT), Wyoming Piper (TTS), Wyoming OpenWakeWord
- **Relevance**: Well-tested protocol, but tied to Home Assistant ecosystem
- **Use for us**: Can adopt the protocol concept (JSON + binary audio over TCP) without Home Assistant dependency
- **ESP32 satellite**: Wyoming-satellite project runs on Pi, streams audio from ESP32 devices

### XiaoZhi ESP32 (Most Relevant)
- **Exactly our architecture**: ESP32 (including P4) ↔ WiFi ↔ Server for STT/LLM/TTS
- **Proven on ESP32-P4**: 70+ board configs, P4 explicitly supported
- **Audio pipeline**: OPUS 16kHz up / 24kHz down, WebSocket or MQTT+UDP
- **Features**: Wake word (ESP-SR), AEC, VAD, noise suppression, MCP protocol
- **Open source**: Active development, large Chinese community
- **Verdict**: **Study this codebase carefully** — it solves many of the same problems. Could fork/adapt rather than build from scratch

### OpenVoiceOS (OVOS)
- **Full voice assistant OS**: STT + Intent + TTS + Skills
- **Plugin architecture**: Supports Vosk, Whisper, Piper, Moonshine via plugins
- **Recent**: Added ONNX-based STT (sherpa-onnx integration) in Feb 2026
- **Heavy**: Full desktop OS approach, more than we need
- **Use for us**: Study their plugin architecture, but don't adopt whole framework

### Willow (ESP32-S3)
- **Architecture**: ESP32-S3 → WiFi → Willow Inference Server (WIS) → STT/TTS
- **Performance**: 500ms end-of-speech to action
- **Wake word**: On-device via ESP32-S3 DSP, 50-80mW always-listening
- **Limitations**: S3-only (not P4), tied to their inference server
- **Use for us**: Reference for wake word implementation and latency targets

### Faster-Local-Voice-AI
- **Architecture**: Vosk STT + Ollama (Gemma3:1b) + Piper TTS
- **Target**: 8GB laptop, no GPU, sub-second latency
- **WebSocket**: Client/server model, JACK/PipeWire for audio
- **Verdict**: Good reference implementation, but not ESP32-aware. Adapt the server-side pipeline

### ESP-BOX Voice Examples
- **Espressif's reference**: ESP-BOX-3 with S3, voice assistant examples
- **ESP-SR integration**: WakeNet + MultiNet on-device
- **Not P4-specific**: But ESP-SR supports P4, so code is adaptable

---

## 7. Recommended Architecture

### Phase 1: Quick Prototype (Get Audio Flowing)
1. **Tab5 firmware**: I2S mic capture → raw PCM 16kHz → WebSocket to Dragon (no OPUS yet)
2. **Dragon server**: Python WebSocket server → save audio → batch Whisper.cpp tiny → Ollama → Piper → WAV back to Tab5
3. **Tab5 playback**: Receive WAV → I2S speaker
4. **Wake word**: None yet — use push-to-talk button on Tab5

**Expected latency**: 3-5 seconds (acceptable for prototype)

### Phase 2: Optimize Pipeline
1. **Add OPUS**: Encode/decode on both ends to reduce bandwidth
2. **Switch STT**: Moonshine v2 via sherpa-onnx (streaming)
3. **Streaming LLM**: Ollama streaming API
4. **Sentence-level TTS**: Stream Piper output sentence by sentence
5. **VAD on Tab5**: ESP-SR VADNet to detect speech boundaries

**Expected latency**: 1-2 seconds

### Phase 3: Production Quality
1. **Wake word**: ESP-SR WakeNet9 or Porcupine on Tab5 for "Hey Glyph"
2. **AEC**: Acoustic Echo Cancellation on Tab5 (ESP-SR AFE) so user can interrupt
3. **Barge-in**: Allow user to interrupt TTS playback
4. **Kokoro TTS**: If Dragon can run it at real-time, upgrade from Piper
5. **Conversation memory**: Context window management in Ollama
6. **Noise suppression**: ESP-SR AFE on Tab5

**Expected latency**: <1 second for first audio

### Component Stack (Final)

| Layer | Component | Runs On |
|-------|-----------|---------|
| Wake Word | ESP-SR WakeNet9 / Porcupine | Tab5 (ESP32-P4) |
| VAD | ESP-SR VADNet | Tab5 |
| Audio Capture | I2S → OPUS encode | Tab5 |
| Audio Transport | WebSocket (binary frames) | WiFi |
| STT | Moonshine v2 (sherpa-onnx) | Dragon |
| LLM | Ollama (Gemma3 4B Q4) | Dragon |
| TTS | Piper (medium) or Kokoro-82M | Dragon |
| Audio Playback | OPUS decode → I2S | Tab5 |

---

## 8. Latency Budget

Target: First audio response within 800ms of user finishing speech.

| Stage | Time | Cumulative | Notes |
|-------|------|-----------|-------|
| VAD detects end-of-speech | 200ms | 200ms | Conservative silence threshold |
| Final STT result | 50ms | 250ms | Moonshine v2 streaming already processed most audio |
| Network (Tab5→Dragon→Tab5) | 10ms | 260ms | Local WiFi round trip |
| LLM first token | 300ms | 560ms | Gemma3 4B Q4 on ARM, time-to-first-token |
| LLM first sentence | 200ms | 760ms | ~50 tokens at ~250 tok/s |
| TTS first sentence | 30ms | 790ms | Piper on ARM CPU |
| OPUS encode + transmit | 20ms | 810ms | Encode + WiFi |
| OPUS decode + I2S start | 10ms | 820ms | Tab5 side |
| **Total to first audio** | | **~820ms** | Achievable with streaming |

### Bottleneck Analysis
- **LLM inference** is the biggest bottleneck at ~500ms to first sentence
- Smaller models (Gemma3 1B) reduce this to ~200ms but lower quality
- Everything else combined is ~320ms
- **Streaming STT** saves ~500ms vs. batch (processes while user speaks)

---

## Key References & Repos

- **XiaoZhi ESP32**: https://github.com/78/xiaozhi-esp32 — Most relevant reference implementation
- **Sherpa-ONNX**: https://github.com/k2-fsa/sherpa-onnx — Unified STT/TTS/VAD runtime
- **Moonshine**: https://github.com/moonshine-ai/moonshine — Streaming STT for edge
- **Piper TTS**: https://github.com/rhasspy/piper — Fast local TTS
- **Kokoro-82M**: https://huggingface.co/hexgrad/Kokoro-82M — High-quality TTS
- **ESP-SR**: https://github.com/espressif/esp-sr — Wake word + VAD for ESP32-P4
- **Faster-Local-Voice-AI**: https://github.com/m15-ai/Faster-Local-Voice-AI — 8GB reference pipeline
- **Wyoming Protocol**: https://www.home-assistant.io/integrations/wyoming/
- **OpenVoiceOS**: https://github.com/openVoiceOS — Full voice assistant framework
- **Willow**: https://heywillow.io/ — ESP32-S3 voice assistant
- **ollama-STT-TTS**: https://github.com/sancliffe/ollama-STT-TTS — Simple Whisper+Ollama+Piper
- **Kokoro-FastAPI**: https://github.com/remsky/Kokoro-FastAPI — Dockerized Kokoro with ONNX CPU support
