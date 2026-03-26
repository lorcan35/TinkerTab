"""Configuration management for Dragon Voice Server.

Loads from config.yaml with environment variable overrides.
All config sections are dataclass-based for type safety and IDE support.
"""

import logging
import os
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

import yaml

logger = logging.getLogger(__name__)

# Default config path: config.yaml next to this file
_DEFAULT_CONFIG_PATH = Path(__file__).parent / "config.yaml"


@dataclass
class ServerConfig:
    host: str = "0.0.0.0"
    port: int = 3502


@dataclass
class STTConfig:
    backend: str = "whisper_cpp"
    model: str = "tiny"
    language: str = "en"
    moonshine_model_path: str = ""
    whisper_model_path: str = ""
    vosk_model_path: str = ""


@dataclass
class TTSConfig:
    backend: str = "piper"
    piper_model: str = "en_US-lessac-medium"
    piper_data_dir: str = ""
    kokoro_model_path: str = ""
    kokoro_voice: str = "af_heart"
    edge_voice: str = "en-US-AriaNeural"
    sample_rate: int = 22050


@dataclass
class LLMConfig:
    backend: str = "ollama"
    ollama_url: str = "http://localhost:11434"
    ollama_model: str = "gemma3:4b"
    openrouter_api_key: str = ""
    openrouter_model: str = "google/gemma-3-4b-it"
    openrouter_url: str = "https://openrouter.ai/api/v1"
    lmstudio_url: str = "http://localhost:1234/v1"
    lmstudio_model: str = "default"
    system_prompt: str = (
        "You are Glyph, a helpful AI assistant on a portable device called "
        "TinkerClaw. Keep responses concise and conversational — they will "
        "be spoken aloud."
    )
    max_tokens: int = 256
    temperature: float = 0.7


@dataclass
class AudioConfig:
    input_sample_rate: int = 16000
    input_channels: int = 1
    output_sample_rate: int = 22050
    vad_enabled: bool = True
    vad_silence_ms: int = 600


@dataclass
class VoiceConfig:
    """Top-level configuration container."""

    server: ServerConfig = field(default_factory=ServerConfig)
    stt: STTConfig = field(default_factory=STTConfig)
    tts: TTSConfig = field(default_factory=TTSConfig)
    llm: LLMConfig = field(default_factory=LLMConfig)
    audio: AudioConfig = field(default_factory=AudioConfig)


# Mapping of env vars to config paths — allows overriding any setting
# Format: DRAGON_VOICE_{SECTION}_{KEY} e.g. DRAGON_VOICE_STT_BACKEND
_ENV_PREFIX = "DRAGON_VOICE_"


def _apply_env_overrides(raw: dict) -> dict:
    """Override config values from environment variables.

    Environment variables follow the pattern DRAGON_VOICE_SECTION_KEY,
    e.g. DRAGON_VOICE_STT_BACKEND=vosk overrides stt.backend.
    """
    for key, value in os.environ.items():
        if not key.startswith(_ENV_PREFIX):
            continue
        parts = key[len(_ENV_PREFIX) :].lower().split("_", 1)
        if len(parts) != 2:
            continue
        section, field_name = parts
        if section in raw and isinstance(raw[section], dict):
            # Attempt type coercion based on existing value
            existing = raw[section].get(field_name)
            if isinstance(existing, bool):
                raw[section][field_name] = value.lower() in ("true", "1", "yes")
            elif isinstance(existing, int):
                try:
                    raw[section][field_name] = int(value)
                except ValueError:
                    logger.warning("Cannot convert env %s=%s to int", key, value)
            elif isinstance(existing, float):
                try:
                    raw[section][field_name] = float(value)
                except ValueError:
                    logger.warning("Cannot convert env %s=%s to float", key, value)
            else:
                raw[section][field_name] = value
            logger.debug("Env override: %s.%s = %s", section, field_name, value)
    return raw


def _dict_to_dataclass(section_cls, data: dict):
    """Create a dataclass instance from a dict, ignoring unknown keys."""
    known_fields = {f.name for f in section_cls.__dataclass_fields__.values()}
    filtered = {k: v for k, v in data.items() if k in known_fields}
    return section_cls(**filtered)


def load_config(path: Optional[str] = None) -> VoiceConfig:
    """Load configuration from YAML file with environment variable overrides.

    Args:
        path: Path to config.yaml. Falls back to DRAGON_VOICE_CONFIG env var,
              then to the default config.yaml bundled with the package.

    Returns:
        Fully populated VoiceConfig instance.
    """
    config_path = Path(
        path
        or os.environ.get("DRAGON_VOICE_CONFIG", "")
        or str(_DEFAULT_CONFIG_PATH)
    )

    raw: dict = {}
    if config_path.exists():
        logger.info("Loading config from %s", config_path)
        with open(config_path, "r") as f:
            raw = yaml.safe_load(f) or {}
    else:
        logger.warning(
            "Config file not found at %s, using defaults", config_path
        )

    # Ensure all sections exist
    for section in ("server", "stt", "tts", "llm", "audio"):
        raw.setdefault(section, {})

    # Apply environment variable overrides
    raw = _apply_env_overrides(raw)

    # Build typed config
    return VoiceConfig(
        server=_dict_to_dataclass(ServerConfig, raw["server"]),
        stt=_dict_to_dataclass(STTConfig, raw["stt"]),
        tts=_dict_to_dataclass(TTSConfig, raw["tts"]),
        llm=_dict_to_dataclass(LLMConfig, raw["llm"]),
        audio=_dict_to_dataclass(AudioConfig, raw["audio"]),
    )


def config_to_dict(config: VoiceConfig, redact_secrets: bool = False) -> dict:
    """Serialize config back to a plain dict, optionally redacting secrets."""
    from dataclasses import asdict

    d = asdict(config)
    if redact_secrets:
        # Redact anything that looks like an API key
        for section in d.values():
            if isinstance(section, dict):
                for key in section:
                    if "api_key" in key and section[key]:
                        section[key] = "***redacted***"
    return d
