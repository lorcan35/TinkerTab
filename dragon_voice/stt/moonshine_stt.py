"""Moonshine STT backend using sherpa-onnx.

Moonshine is a lightweight speech recognition model optimized for edge devices.
Uses the sherpa-onnx runtime for efficient inference with ARM NEON support.
"""

import asyncio
import logging
import os
from pathlib import Path

import numpy as np

from dragon_voice.config import STTConfig
from dragon_voice.stt.base import STTBackend

logger = logging.getLogger(__name__)

# Default model download directory
_MODEL_DIR = Path.home() / ".cache" / "dragon_voice" / "moonshine"


class MoonshineBackend(STTBackend):
    """STT backend using sherpa-onnx with Moonshine models."""

    def __init__(self, config: STTConfig) -> None:
        self._config = config
        self._recognizer = None
        self._lock = asyncio.Lock()

    async def initialize(self) -> None:
        """Load the Moonshine model via sherpa-onnx.

        Auto-downloads the model from the sherpa-onnx model zoo if not
        already present locally.
        """
        try:
            import sherpa_onnx
        except ImportError:
            raise ImportError(
                "sherpa-onnx is required for the moonshine backend. "
                "Install it: pip install sherpa-onnx"
            )

        model_path = self._config.moonshine_model_path
        model_size = self._config.model  # "tiny" or "base"

        logger.info(
            "Initializing Moonshine STT — model=%s, path=%s",
            model_size,
            model_path or "(auto-download)",
        )

        loop = asyncio.get_running_loop()

        def _load():
            if model_path and Path(model_path).is_dir():
                base = Path(model_path)
            else:
                base = self._ensure_model(model_size)

            # Moonshine model layout in sherpa-onnx:
            #   preprocessor.onnx, encoder.onnx, uncached_decoder.onnx,
            #   cached_decoder.onnx, tokens.txt
            recognizer = sherpa_onnx.OfflineRecognizer.from_moonshine(
                preprocessor=str(base / "preprocess.onnx"),
                encoder=str(base / "encode.onnx"),
                uncached_decoder=str(base / "uncached_decode.onnx"),
                cached_decoder=str(base / "cached_decode.onnx"),
                tokens=str(base / "tokens.txt"),
                num_threads=4,
            )
            return recognizer

        try:
            self._recognizer = await loop.run_in_executor(None, _load)
            logger.info("Moonshine model loaded successfully")
        except Exception:
            logger.exception("Failed to load Moonshine model")
            raise

    @staticmethod
    def _ensure_model(model_size: str) -> Path:
        """Download the Moonshine model if not cached locally.

        Uses sherpa-onnx's built-in model download mechanism or falls back
        to manual download from the model zoo.
        """
        model_name = f"sherpa-onnx-moonshine-{model_size}-en-int8"
        model_dir = _MODEL_DIR / model_name

        if model_dir.exists() and any(model_dir.glob("*.onnx")):
            logger.info("Using cached Moonshine model at %s", model_dir)
            return model_dir

        logger.info("Downloading Moonshine %s model...", model_size)
        _MODEL_DIR.mkdir(parents=True, exist_ok=True)

        # Use sherpa-onnx's download utility if available
        try:
            from sherpa_onnx import cli as sherpa_cli

            sherpa_cli.download_model(model_name, str(_MODEL_DIR))
        except (ImportError, AttributeError):
            # Fallback: direct download
            import subprocess

            url = (
                f"https://github.com/k2-fsa/sherpa-onnx/releases/download/"
                f"asr-models/{model_name}.tar.bz2"
            )
            archive = _MODEL_DIR / f"{model_name}.tar.bz2"
            subprocess.run(["wget", "-q", "-O", str(archive), url], check=True)
            subprocess.run(
                ["tar", "xjf", str(archive), "-C", str(_MODEL_DIR)], check=True
            )
            archive.unlink(missing_ok=True)

        if not model_dir.exists():
            raise FileNotFoundError(
                f"Moonshine model download failed — expected at {model_dir}"
            )
        return model_dir

    async def transcribe(self, audio_bytes: bytes, sample_rate: int = 16000) -> str:
        """Transcribe PCM int16 audio to text using Moonshine."""
        if self._recognizer is None:
            raise RuntimeError("MoonshineBackend not initialized")

        audio_i16 = np.frombuffer(audio_bytes, dtype=np.int16)
        audio_f32 = audio_i16.astype(np.float32) / 32768.0

        if len(audio_f32) == 0:
            return ""

        async with self._lock:
            loop = asyncio.get_running_loop()

            def _transcribe():
                import sherpa_onnx

                stream = self._recognizer.create_stream()
                stream.accept_waveform(sample_rate, audio_f32)
                self._recognizer.decode_stream(stream)
                return stream.result.text.strip()

            try:
                text = await loop.run_in_executor(None, _transcribe)
            except Exception:
                logger.exception("Moonshine transcription failed")
                return ""

        logger.debug("Moonshine transcribed %d samples -> '%s'", len(audio_f32), text)
        return text

    async def shutdown(self) -> None:
        self._recognizer = None
        logger.info("Moonshine backend shut down")

    @property
    def name(self) -> str:
        return f"Moonshine ({self._config.model})"
