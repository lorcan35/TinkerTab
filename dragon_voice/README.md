# Dragon Voice Server

Python WebSocket server that runs on the Dragon ARM mini-PC, providing the voice AI backend for TinkerClaw. Receives microphone audio from the Tab5 over WiFi, processes it through configurable STT (speech-to-text), LLM (language model), and TTS (text-to-speech) backends, then streams the spoken response back. The server is entirely config-driven via `config.yaml` with environment variable overrides.

## Quick Start

```bash
# Install core dependencies
pip install -r requirements.txt

# Install at least one STT + TTS backend
pip install pywhispercpp piper-tts

# Make sure Ollama is running with a model loaded
ollama pull gemma3:4b

# Run the server (default: ws://0.0.0.0:3502)
python -m dragon_voice
```

## Configuration

The default config file is `config.yaml` in this directory. Override the path:

```bash
# Via environment variable
export DRAGON_VOICE_CONFIG=/path/to/custom-config.yaml

# Or via CLI argument
python -m dragon_voice --config /path/to/custom-config.yaml
```

Individual settings can be overridden via environment variables using the pattern `DRAGON_VOICE_{SECTION}_{KEY}`:

```bash
export DRAGON_VOICE_STT_BACKEND=moonshine
export DRAGON_VOICE_LLM_OLLAMAMODEL=phi3.5:3.8b
export DRAGON_VOICE_SERVER_PORT=3503
```

## Available Backends

### STT (Speech-to-Text)

| Backend | Config Value | Package | Install |
|---------|-------------|---------|---------|
| Whisper.cpp | `whisper_cpp` | pywhispercpp | `pip install pywhispercpp` |
| Moonshine | `moonshine` | sherpa-onnx | `pip install sherpa-onnx` |
| Vosk | `vosk` | vosk | `pip install vosk` |

### TTS (Text-to-Speech)

| Backend | Config Value | Package | Install |
|---------|-------------|---------|---------|
| Piper | `piper` | piper-tts | `pip install piper-tts` |
| Kokoro | `kokoro` | kokoro-onnx | `pip install kokoro-onnx` |
| Edge TTS | `edge_tts` | edge-tts | `pip install edge-tts` |

### LLM (Language Model)

| Backend | Config Value | Service | Setup |
|---------|-------------|---------|-------|
| Ollama | `ollama` | Local Ollama | `ollama pull gemma3:4b` |
| OpenRouter | `openrouter` | Cloud API | Set `openrouter_api_key` in config |
| LM Studio | `lmstudio` | Local LM Studio | Start LM Studio server on port 1234 |

## WebSocket Protocol

The server listens on `ws://{host}:{port}` (default `ws://0.0.0.0:3502`).

### Client -> Server

| Message | Format | Description |
|---------|--------|-------------|
| `{"type": "start"}` | JSON | Begin voice interaction |
| Binary audio data | Binary | PCM int16, 16kHz, mono chunks |
| `{"type": "stop"}` | JSON | End of speech, trigger processing |
| `{"type": "cancel"}` | JSON | Abort current interaction |
| `{"type": "ping"}` | JSON | Keepalive |

### Server -> Client

| Message | Format | Description |
|---------|--------|-------------|
| `{"type": "stt_result", "text": "..."}` | JSON | Transcription result |
| `{"type": "llm_token", "token": "..."}` | JSON | Streamed LLM token |
| `{"type": "tts_start"}` | JSON | TTS audio about to begin |
| Binary audio data | Binary | PCM int16 TTS audio at configured sample rate |
| `{"type": "tts_end"}` | JSON | TTS audio complete |
| `{"type": "error", "message": "..."}` | JSON | Error |
| `{"type": "pong"}` | JSON | Keepalive reply |

## API Endpoints

| Endpoint | Protocol | Description |
|----------|----------|-------------|
| `ws://:3502/` | WebSocket | Main voice pipeline (audio + control) |

## Project Structure

```
dragon_voice/
├── __init__.py          # Package metadata
├── __main__.py          # Entry point (python -m dragon_voice)
├── config.py            # Configuration loader (YAML + env overrides)
├── config.yaml          # Default configuration
├── requirements.txt     # Python dependencies
├── stt/
│   ├── __init__.py      # STT backend factory
│   ├── base.py          # Abstract STT interface
│   └── whisper_cpp.py   # Whisper.cpp backend
├── tts/                 # TTS backends (same pattern as stt/)
└── llm/                 # LLM backends (same pattern as stt/)
```

## Full Documentation

See [docs/VOICE_PIPELINE.md](../docs/VOICE_PIPELINE.md) for the complete voice pipeline architecture, latency analysis, RAM budget, and phase roadmap.
