"""Whisper.cpp STT backend using pywhispercpp.

Uses the whisper.cpp library with ARM NEON acceleration on the Dragon.
Auto-downloads the model on first use if no explicit path is provided.
"""

import asyncio
import logging
from pathlib import Path

import numpy as np

from dragon_voice.config import STTConfig
from dragon_voice.stt.base import STTBackend

logger = logging.getLogger(__name__)


class WhisperCppBackend(STTBackend):
    """STT backend using pywhispercpp (whisper.cpp Python bindings)."""

    def __init__(self, config: STTConfig) -> None:
        self._config = config
        self._model = None
        self._lock = asyncio.Lock()

    async def initialize(self) -> None:
        """Load the whisper.cpp model.

        If whisper_model_path is set, uses that file directly.
        Otherwise, lets pywhispercpp auto-download the requested model size.
        """
        try:
            from pywhispercpp.model import Model
        except ImportError:
            raise ImportError(
                "pywhispercpp is required for the whisper_cpp backend. "
                "Install it: pip install pywhispercpp"
            )

        model_path = self._config.whisper_model_path
        model_size = self._config.model

        logger.info(
            "Initializing whisper.cpp — model=%s, path=%s, language=%s",
            model_size,
            model_path or "(auto-download)",
            self._config.language,
        )

        def _load():
            if model_path and Path(model_path).exists():
                return Model(model_path, n_threads=4)
            else:
                # pywhispercpp auto-downloads by model name
                return Model(model_size, n_threads=4)

        loop = asyncio.get_running_loop()
        self._model = await loop.run_in_executor(None, _load)
        logger.info("whisper.cpp model loaded successfully")

    async def transcribe(self, audio_bytes: bytes, sample_rate: int = 16000) -> str:
        """Transcribe PCM int16 audio bytes to text.

        Converts the raw bytes to a float32 numpy array normalized to [-1, 1],
        which is what whisper.cpp expects.
        """
        if self._model is None:
            raise RuntimeError("WhisperCppBackend not initialized — call initialize() first")

        # Convert PCM int16 bytes to float32 array
        audio_i16 = np.frombuffer(audio_bytes, dtype=np.int16)
        audio_f32 = audio_i16.astype(np.float32) / 32768.0

        if len(audio_f32) == 0:
            return ""

        async with self._lock:
            loop = asyncio.get_running_loop()

            def _transcribe():
                segments = self._model.transcribe(
                    audio_f32,
                    language=self._config.language,
                )
                return " ".join(seg.text for seg in segments).strip()

            try:
                text = await loop.run_in_executor(None, _transcribe)
            except Exception:
                logger.exception("whisper.cpp transcription failed")
                return ""

        logger.debug("Transcribed %d samples -> '%s'", len(audio_f32), text)
        return text

    async def shutdown(self) -> None:
        """Release model resources."""
        self._model = None
        logger.info("whisper.cpp backend shut down")

    @property
    def name(self) -> str:
        return f"whisper.cpp ({self._config.model})"
