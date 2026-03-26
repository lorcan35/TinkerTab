"""Kokoro TTS backend using kokoro-onnx.

Kokoro is a high-quality neural TTS model with small footprint,
suitable for on-device synthesis. Outputs at 24000 Hz.
"""

import asyncio
import logging
from pathlib import Path

import numpy as np

from dragon_voice.config import TTSConfig
from dragon_voice.tts.base import TTSBackend

logger = logging.getLogger(__name__)

_CACHE_DIR = Path.home() / ".cache" / "dragon_voice" / "kokoro"
_KOKORO_SAMPLE_RATE = 24000


class KokoroBackend(TTSBackend):
    """TTS backend using kokoro-onnx."""

    def __init__(self, config: TTSConfig) -> None:
        self._config = config
        self._kokoro = None
        self._lock = asyncio.Lock()

    async def initialize(self) -> None:
        """Load the Kokoro model."""
        try:
            import kokoro_onnx
        except ImportError:
            raise ImportError(
                "kokoro-onnx is required for the kokoro backend. "
                "Install it: pip install kokoro-onnx"
            )

        model_path = self._config.kokoro_model_path
        voice = self._config.kokoro_voice

        logger.info(
            "Initializing Kokoro TTS — voice=%s, path=%s",
            voice,
            model_path or "(default)",
        )

        loop = asyncio.get_running_loop()

        def _load():
            if model_path and Path(model_path).exists():
                return kokoro_onnx.Kokoro(model_path)
            else:
                # kokoro-onnx auto-downloads the default model
                return kokoro_onnx.Kokoro.from_pretrained()

        try:
            self._kokoro = await loop.run_in_executor(None, _load)
            logger.info("Kokoro TTS loaded successfully")
        except Exception:
            logger.exception("Failed to initialize Kokoro TTS")
            raise

    async def synthesize(self, text: str) -> bytes:
        """Synthesize text to PCM int16 audio bytes at 24kHz."""
        if not text.strip():
            return b""

        if self._kokoro is None:
            raise RuntimeError("KokoroBackend not initialized")

        voice = self._config.kokoro_voice

        async with self._lock:
            loop = asyncio.get_running_loop()

            def _synthesize():
                # kokoro-onnx returns (audio_float32, sample_rate)
                audio_f32, sr = self._kokoro.create(
                    text,
                    voice=voice,
                    speed=1.0,
                )
                # Convert float32 [-1, 1] to int16
                audio_i16 = (audio_f32 * 32767).clip(-32768, 32767).astype(np.int16)
                return audio_i16.tobytes()

            try:
                pcm = await loop.run_in_executor(None, _synthesize)
            except Exception:
                logger.exception("Kokoro synthesis failed")
                return b""

        logger.debug("Kokoro synthesized %d bytes for: %.40s...", len(pcm), text)
        return pcm

    async def shutdown(self) -> None:
        self._kokoro = None
        logger.info("Kokoro TTS shut down")

    @property
    def sample_rate(self) -> int:
        return _KOKORO_SAMPLE_RATE

    @property
    def name(self) -> str:
        return f"Kokoro ({self._config.kokoro_voice})"
