"""Moonshine STT backend using moonshine-voice native runtime.

Moonshine V2 streaming models provide Whisper-level accuracy at 20x+ speed
on ARM. Uses the ONNX-based C++ runtime with NEON acceleration.
"""

import asyncio
import logging
import struct
from typing import Optional

import numpy as np

from dragon_voice.config import STTConfig
from dragon_voice.stt.base import STTBackend

logger = logging.getLogger(__name__)

# Model name -> (moonshine-voice download name, ModelArch enum value)
_MODEL_MAP = {
    "tiny": ("tiny-streaming-en", "TINY_STREAMING"),
    "small": ("small-streaming-en", "SMALL_STREAMING"),
    "medium": ("medium-streaming-en", "MEDIUM_STREAMING"),
    "base": ("base-en", "BASE"),
}


class MoonshineBackend(STTBackend):
    """STT backend using moonshine-voice native C++ runtime."""

    def __init__(self, config: STTConfig) -> None:
        self._config = config
        self._transcriber = None
        self._model_arch = None
        self._lock = asyncio.Lock()

    async def initialize(self) -> None:
        """Load the Moonshine model."""
        try:
            from moonshine_voice import ModelArch, get_model_path, download
            from moonshine_voice.transcriber import Transcriber
        except ImportError:
            raise ImportError(
                "moonshine-voice is required for the moonshine backend. "
                "Install it: pip install moonshine-voice"
            )

        model_size = self._config.model  # "tiny", "small", "medium", "base"
        if model_size not in _MODEL_MAP:
            raise ValueError(
                f"Unknown Moonshine model {model_size}. "
                f"Available: {', '.join(_MODEL_MAP.keys())}"
            )

        model_name, arch_name = _MODEL_MAP[model_size]
        self._model_arch = getattr(ModelArch, arch_name)

        logger.info("Initializing Moonshine STT — model=%s", model_size)

        loop = asyncio.get_running_loop()

        def _load():
            import pathlib
            cache_dir = pathlib.Path.home() / ".cache" / "moonshine_voice"
            model_dir = cache_dir / "download.moonshine.ai" / "model" / model_name / "quantized"

            if not model_dir.exists():
                # Download model — API changed in v0.0.51+
                model_url = f"https://download.moonshine.ai/model/{model_name}/quantized"
                try:
                    download.download_model(model_url, model_name)
                except TypeError:
                    # Fallback for older API: download.download_model(model_name)
                    download.download_model(model_name)

            if not model_dir.exists():
                # Last resort: try the assets path
                model_dir = get_model_path(model_name)

            model_path = str(model_dir)
            logger.info("Loading model from %s", model_path)
            return Transcriber(
                model_path=model_path,
                model_arch=self._model_arch,
            )

        try:
            self._transcriber = await loop.run_in_executor(None, _load)
            logger.info("Moonshine model loaded successfully")
        except Exception:
            logger.exception("Failed to load Moonshine model")
            raise

    async def transcribe(self, audio_bytes: bytes, sample_rate: int = 16000) -> str:
        """Transcribe PCM int16 audio to text using Moonshine."""
        if self._transcriber is None:
            raise RuntimeError("MoonshineBackend not initialized")

        audio_i16 = np.frombuffer(audio_bytes, dtype=np.int16)
        audio_f32 = (audio_i16.astype(np.float32) / 32768.0).tolist()

        if len(audio_f32) == 0:
            return ""

        async with self._lock:
            loop = asyncio.get_running_loop()

            def _transcribe():
                result = self._transcriber.transcribe_without_streaming(
                    audio_f32, sample_rate=sample_rate
                )
                # Combine all transcript lines
                texts = [line.text.strip() for line in result.lines if line.text.strip()]
                return " ".join(texts)

            try:
                text = await loop.run_in_executor(None, _transcribe)
            except Exception:
                logger.exception("Moonshine transcription failed")
                return ""

        logger.debug("Moonshine transcribed %d samples -> %s", len(audio_f32), text)
        return text

    async def shutdown(self) -> None:
        if self._transcriber is not None:
            self._transcriber.close()
            self._transcriber = None
        logger.info("Moonshine backend shut down")

    @property
    def name(self) -> str:
        return f"Moonshine ({self._config.model})"
