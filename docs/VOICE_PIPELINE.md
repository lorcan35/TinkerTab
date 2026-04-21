# Voice Pipeline Documentation

Conversational AI voice pipeline for TinkerClaw: **STT -> LLM -> TTS**.

The Tab5 (ESP32-P4) captures microphone audio and streams it over WiFi to the Dragon (ARM mini-PC), which runs speech-to-text, a large language model, and text-to-speech, then streams the spoken response back to the Tab5 speaker.

---

## Architecture Overview

```
┌──────────────────────────────────────────────────────┐
│  Tab5 (ESP32-P4)                                     │
│                                                      │
│  ES7210 Mic ─► I2S RX ─► PCM 16kHz mono             │
│                              │                       │
│                        Push-to-Talk                   │
│                              │                       │
│                    WebSocket binary frames ──────────►│──┐
│                                                      │  │
│  ES8388 Codec ◄─ I2S TX ◄─ PCM audio ◄──────────────│◄─┤
│       │                                              │  │
│  NS4150B Spkr                                        │  │
└──────────────────────────────────────────────────────┘  │
                                                          │
                         WiFi LAN                         │
                                                          │
┌──────────────────────────────────────────────────────┐  │
│  Dragon (ARM mini-PC, 12GB RAM, Linux)               │  │
│                                                      │  │
│  ┌─────────────────────────────────────────────────┐ │  │
│  │  dragon_voice server (port 3502)                │◄├──┘
│  │                                                 │ │
│  │  Audio In ──► STT (Moonshine/Whisper/Vosk)      │ │
│  │                       │                         │ │
│  │               LLM (Ollama/OpenRouter/LM Studio) │ │
│  │                       │                         │ │
│  │               TTS (Piper/Kokoro/Edge TTS)       │ │
│  │                       │                         │ │
│  │  Audio Out ◄── PCM response                     │ │
│  └─────────────────────────────────────────────────┘ │
│                                                      │
│  Ollama (separate process, loaded model in RAM)      │
└──────────────────────────────────────────────────────┘
```

---

## Component Stack

| Layer | Component | Runs On | Notes |
|-------|-----------|---------|-------|
| Mic Capture | ES7210 dual mic, I2S RX | Tab5 | PCM 16-bit, 16kHz, mono |
| Transport | WebSocket binary frames | WiFi LAN | TCP, port 3502 |
| STT | Moonshine / Whisper.cpp / Vosk | Dragon | Configurable backend |
| LLM | Ollama / OpenRouter / LM Studio | Dragon | Streaming token generation |
| TTS | Piper / Kokoro / Edge TTS | Dragon | Sentence-level generation |
| Speaker | ES8388 codec + NS4150B amp | Tab5 | PCM playback via I2S TX |
| Control | Serial commands + WebSocket JSON | Both | voice_start, voice_stop |

---

## WebSocket Protocol (Port 3502)

The voice server uses a single WebSocket connection carrying both JSON control messages and binary audio frames.

> **Full spec:** [TinkerBox docs/protocol.md](https://github.com/lorcan35/TinkerBox/blob/main/docs/protocol.md) is the source of truth. The summary below is a quick reference for the core flow; the protocol has since gained rich-media (`media`, `card`, `audio_clip`, `text_update`), widget (`widget_live`, `widget_card`, `widget_list`, `widget_chart`, `widget_prompt`, `widget_action`), tool-call (`tool_call`, `tool_result`), receipt, and session-resume (`session_messages`) messages. See the canonical doc for the full list.

### Message Types

#### Client -> Server (Tab5 -> Dragon)

| Type | Format | Description |
|------|--------|-------------|
| Audio data | Binary frame | Raw PCM int16, 16kHz, mono. Sent in chunks during recording. |
| `{"type": "start"}` | JSON | Begin a voice interaction (mic recording started). |
| `{"type": "stop"}` | JSON | End of speech (mic recording stopped). Triggers STT processing. |
| `{"type": "cancel"}` | JSON | Abort current interaction. |
| `{"type": "ping"}` | JSON | Keepalive. Server responds with `{"type": "pong"}`. |

#### Server -> Client (Dragon -> Tab5)

| Type | Format | Description |
|------|--------|-------------|
| Audio data | Binary frame | PCM int16 TTS audio at configured sample rate (default 22050 Hz). |
| `{"type": "stt_result", "text": "..."}` | JSON | Final transcription of user speech. |
| `{"type": "stt_partial", "text": "..."}` | JSON | Partial/streaming transcription (Phase 2). |
| `{"type": "llm_token", "token": "..."}` | JSON | Individual LLM token (for display on Tab5 screen). |
| `{"type": "tts_start"}` | JSON | TTS audio is about to begin. |
| `{"type": "tts_end"}` | JSON | TTS audio stream complete. |
| `{"type": "error", "message": "..."}` | JSON | Error during processing. |
| `{"type": "pong"}` | JSON | Reply to ping. |

### Audio Format

| Direction | Sample Rate | Bit Depth | Channels | Encoding |
|-----------|-------------|-----------|----------|----------|
| Tab5 -> Dragon (mic) | 16000 Hz | 16-bit signed int | 1 (mono) | Raw PCM |
| Dragon -> Tab5 (TTS) | 22050 Hz | 16-bit signed int | 1 (mono) | Raw PCM |

Phase 2 will add OPUS encoding in both directions to reduce bandwidth from ~256 kbps to ~16-32 kbps.

### Interaction Flow

```
Tab5                              Dragon
  │                                  │
  │──── {"type": "start"} ──────────►│
  │──── [binary PCM audio] ─────────►│
  │──── [binary PCM audio] ─────────►│
  │──── [binary PCM audio] ─────────►│
  │──── {"type": "stop"} ───────────►│
  │                                  │  STT processing...
  │◄─── {"type":"stt_result"} ──────│
  │                                  │  LLM generation...
  │◄─── {"type":"llm_token"} ───────│  (repeated per token)
  │                                  │  TTS generation...
  │◄─── {"type":"tts_start"} ───────│
  │◄─── [binary PCM audio] ─────────│
  │◄─── [binary PCM audio] ─────────│
  │◄─── {"type":"tts_end"} ─────────│
  │                                  │
```

---

## Configuration Guide

All configuration lives in `dragon_voice/config.yaml`. Every value can be overridden via environment variables using the pattern `DRAGON_VOICE_{SECTION}_{KEY}`.

### config.yaml Explained

```yaml
server:
  host: "0.0.0.0"          # Bind address (0.0.0.0 = all interfaces)
  port: 3502                # WebSocket listen port

stt:
  backend: "whisper_cpp"    # Options: moonshine, whisper_cpp, vosk
  model: "tiny"             # Model size (tiny, base, small — depends on backend)
  language: "en"            # Language code
  moonshine_model_path: ""  # Path to Moonshine ONNX model (auto-download if empty)
  whisper_model_path: ""    # Path to whisper.cpp GGML model (auto-download if empty)
  vosk_model_path: ""       # Path to Vosk model directory

tts:
  backend: "piper"          # Options: piper, kokoro, edge_tts
  piper_model: "en_US-lessac-medium"  # Piper voice name
  piper_data_dir: ""        # Piper model directory (auto-download if empty)
  kokoro_model_path: ""     # Path to Kokoro ONNX model
  kokoro_voice: "af_heart"  # Kokoro voice ID
  edge_voice: "en-US-AriaNeural"  # Edge TTS voice name
  sample_rate: 22050        # TTS output sample rate

llm:
  backend: "ollama"         # Options: ollama, openrouter, lmstudio
  ollama_url: "http://localhost:11434"
  ollama_model: "gemma3:4b"
  openrouter_api_key: ""    # Required if backend=openrouter
  openrouter_model: "google/gemma-3-4b-it"
  openrouter_url: "https://openrouter.ai/api/v1"
  lmstudio_url: "http://localhost:1234/v1"
  lmstudio_model: "default"
  system_prompt: "You are Glyph, a helpful AI assistant..."
  max_tokens: 256           # Max response tokens
  temperature: 0.7

audio:
  input_sample_rate: 16000  # Mic capture rate (must be 16kHz for STT)
  input_channels: 1         # Mono
  output_sample_rate: 22050 # TTS playback rate
  vad_enabled: true         # Voice Activity Detection
  vad_silence_ms: 600       # Silence duration to trigger end-of-speech
```

### Environment Variable Overrides

Any config value can be overridden:

```bash
# Switch to Vosk STT
export DRAGON_VOICE_STT_BACKEND=vosk

# Use a different Ollama model
export DRAGON_VOICE_LLM_OLLAMAMODEL=phi3.5:3.8b

# Point to a specific config file
export DRAGON_VOICE_CONFIG=/path/to/custom-config.yaml
```

---

## Swapping Backends

### STT Backends

| Backend | Package | Install | Best For |
|---------|---------|---------|----------|
| **whisper_cpp** | pywhispercpp | `pip install pywhispercpp` | Default — good accuracy, batch mode |
| **moonshine** | sherpa-onnx | `pip install sherpa-onnx` | Fastest on ARM, streaming support |
| **vosk** | vosk | `pip install vosk` | Ultra-lightweight, real-time streaming |

To switch, edit `config.yaml`:
```yaml
stt:
  backend: "moonshine"  # change this line
```

Or via environment:
```bash
export DRAGON_VOICE_STT_BACKEND=moonshine
```

### TTS Backends

| Backend | Package | Install | Best For |
|---------|---------|---------|----------|
| **piper** | piper-tts | `pip install piper-tts` | Default — proven fast on ARM, many voices |
| **kokoro** | kokoro-onnx | `pip install kokoro-onnx` | Higher quality voice, needs more CPU |
| **edge_tts** | edge-tts | `pip install edge-tts` | Cloud fallback, best quality, needs internet |

### LLM Backends

| Backend | Service | Setup | Best For |
|---------|---------|-------|----------|
| **ollama** | Ollama (local) | `ollama pull gemma3:4b` | Default — fully local, no internet |
| **openrouter** | OpenRouter API | Set `openrouter_api_key` | Best models, needs internet + API key |
| **lmstudio** | LM Studio (local) | Run LM Studio server | Alternative local inference |

---

## Latency Budget

Target: first audio response within ~800ms of user finishing speech.

| Stage | Duration | Cumulative | Notes |
|-------|----------|------------|-------|
| VAD detects end-of-speech | 200 ms | 200 ms | Silence threshold (configurable) |
| Final STT result | 50 ms | 250 ms | Moonshine streaming already processed most audio |
| WiFi round trip | 10 ms | 260 ms | Local LAN |
| LLM time-to-first-token | 300 ms | 560 ms | Gemma3 4B Q4 on ARM |
| LLM first sentence (~50 tokens) | 200 ms | 760 ms | ~250 tok/s on Dragon ARM |
| TTS first sentence | 30 ms | 790 ms | Piper on ARM CPU |
| Network + decode + I2S start | 30 ms | 820 ms | Tab5 side |

**Bottleneck**: LLM inference dominates at ~500 ms to first sentence. Everything else combined is ~320 ms. Streaming STT saves ~500 ms vs batch processing.

Phase 1 (current) uses batch STT — expect 3-5 seconds total latency. Phase 2 streaming will bring this closer to the 800 ms target.

---

## RAM Budget on Dragon (12 GB)

| Component | RAM Usage | Notes |
|-----------|-----------|-------|
| Linux OS + services | ~1.0 GB | Base system |
| Ollama + Gemma3 4B Q4 | ~3.5 GB | Loaded permanently |
| STT model (Moonshine/Whisper tiny) | ~0.2 GB | Via ONNX runtime |
| TTS model (Piper medium) | ~0.5 GB | ONNX runtime + voice data |
| Python voice server | ~0.2 GB | asyncio + WebSocket + pipeline |
| Audio buffers | ~0.05 GB | PCM capture/playback |
| **Total** | **~5.5 GB** | |
| **Headroom** | **~6.5 GB** | Available for larger models or other services |

With 12 GB, there is room to run a larger LLM (e.g., Gemma3 8B Q4 at ~5 GB) or run Kokoro TTS alongside Piper.

---

## Phase Roadmap

### Phase 1: Basic Pipeline -- COMPLETE

- Push-to-talk from Tab5 (serial commands `voice_start`, `voice_stop`)
- Raw PCM 16kHz mono over WebSocket (no OPUS)
- Batch STT (Whisper.cpp tiny)
- Ollama LLM (Gemma3 4B)
- Piper TTS sentence generation
- PCM audio response streamed back to Tab5
- Expected latency: 3-5 seconds

### Phase 2: Streaming Pipeline

- OPUS encoding/decoding on both ends (reduces bandwidth ~10x)
- Moonshine v2 streaming STT (process audio in real-time as user speaks)
- Streaming LLM tokens with sentence-level TTS generation
- VAD on Tab5 (ESP-SR VADNet) to detect speech boundaries
- Expected latency: 1-2 seconds

### Phase 3: Wake Word + Production

- On-device wake word: ESP-SR WakeNet9 or Porcupine for "Hey Glyph"
- Acoustic Echo Cancellation (AEC) via ESP-SR AFE
- Barge-in support (interrupt TTS playback by speaking)
- Kokoro TTS upgrade for higher voice quality
- Conversation memory / context window management
- Expected latency: <1 second to first audio

---

## Quick Start

### Prerequisites

- Dragon running Linux with Python 3.10+
- Ollama installed and running (`ollama serve`)
- A model pulled (`ollama pull gemma3:4b`)

### Run the Voice Server

```bash
cd /path/to/TinkerTab/dragon_voice

# Install dependencies
pip install -r requirements.txt

# Install at least one STT backend
pip install pywhispercpp   # or: pip install sherpa-onnx / pip install vosk

# Install at least one TTS backend
pip install piper-tts      # or: pip install kokoro-onnx / pip install edge-tts

# Run the server
python -m dragon_voice
```

The server starts on `ws://0.0.0.0:3502` by default.

### Test with a WebSocket Client

```bash
# Using websocat (install: cargo install websocat)
echo '{"type": "start"}' | websocat ws://dragon-ip:3502
```

Or from Python:

```python
import asyncio
import aiohttp

async def test():
    async with aiohttp.ClientSession() as session:
        async with session.ws_connect("ws://dragon-ip:3502") as ws:
            # Send start
            await ws.send_json({"type": "start"})
            # Send audio (PCM int16, 16kHz, mono)
            with open("test.pcm", "rb") as f:
                await ws.send_bytes(f.read())
            # Send stop to trigger processing
            await ws.send_json({"type": "stop"})
            # Read responses
            async for msg in ws:
                print(msg)

asyncio.run(test())
```

### Tab5 Serial Commands

| Command | Description |
|---------|-------------|
| `voice_start` | Begin mic capture and stream to Dragon |
| `voice_stop` | Stop mic capture, trigger STT/LLM/TTS pipeline |

---

## Troubleshooting

### Server won't start

- **Port in use**: Another process on 3502. Check with `ss -tlnp | grep 3502` and change the port in `config.yaml`.
- **Missing dependency**: The error will name the missing package. Install it with pip.

### No audio from Tab5

- Verify the WebSocket connection: check Dragon server logs for "client connected".
- Confirm Tab5 WiFi is connected and can reach Dragon's IP.
- Test speaker independently with the `audio` serial command on Tab5.

### STT returns empty text

- Audio may be too quiet. Use the `mic` serial command on Tab5 to check RMS levels.
- Verify the audio format is PCM int16, 16kHz, mono. Wrong sample rate or bit depth produces garbage.
- Try a different STT backend to isolate the issue.

### High latency (>5 seconds)

- Check which STT backend is active. Whisper.cpp small/base models are slower; switch to tiny or Moonshine.
- Ensure Ollama model is already loaded (first request after idle has a cold-start penalty). Run `ollama run gemma3:4b "test"` to pre-warm.
- Monitor Dragon CPU: `htop`. If CPU is maxed, consider a smaller LLM model.

### LLM not responding

- Verify Ollama is running: `curl http://localhost:11434/api/tags`
- Check the model is pulled: `ollama list`
- If using OpenRouter, verify the API key is set and has credits.

### TTS sounds robotic or wrong voice

- Piper: try a different voice model (e.g., `en_US-amy-medium` instead of `en_US-lessac-medium`).
- Edge TTS: requires internet. Verify Dragon has connectivity.
- Check `tts.sample_rate` matches what the Tab5 expects (default 22050).

### WebSocket disconnects

- WiFi signal strength. Move Tab5 closer to the router or Dragon.
- Server crash: check Dragon logs for Python exceptions.
- Increase keepalive: send `{"type": "ping"}` every 30 seconds from Tab5.
