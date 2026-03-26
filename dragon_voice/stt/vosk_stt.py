"""Vosk STT backend.

Vosk provides lightweight offline speech recognition with real-time
streaming capability. Good fallback for low-resource scenarios.
"""

import asyncio
import json
import logging
from pathlib import Path

import numpy as np

from dragon_voice.config import STTConfig
from dragon_voice.stt.base import STTBackend

logger = logging.getLogger(__name__)

_MODEL_DIR = Path.home() / ".cache" / "dragon_voice" / "vosk"

# Small English model — about 50MB
_MODEL_URLS = {
    "tiny": "https://alphacephei.com/vosk/models/vosk-model-small-en-us-0.15.zip",
    "small": "https://alphacephei.com/vosk/models/vosk-model-small-en-us-0.15.zip",
    "medium": "https://alphacephei.com/vosk/models/vosk-model-en-us-0.22.zip",
    "large": "https://alphacephei.com/vosk/models/vosk-model-en-us-0.22.zip",
}
_MODEL_DIRS = {
    "tiny": "vosk-model-small-en-us-0.15",
    "small": "vosk-model-small-en-us-0.15",
    "medium": "vosk-model-en-us-0.22",
    "large": "vosk-model-en-us-0.22",
}


class VoskBackend(STTBackend):
    """STT backend using Vosk for offline speech recognition."""

    def __init__(self, config: STTConfig) -> None:
        self._config = config
        self._model = None
        self._lock = asyncio.Lock()

    async def initialize(self) -> None:
        """Load the Vosk model, downloading if necessary."""
        try:
            import vosk
        except ImportError:
            raise ImportError(
                "vosk is required for the vosk backend. "
                "Install it: pip install vosk"
            )

        model_path = self._config.vosk_model_path
        model_size = self._config.model

        logger.info(
            "Initializing Vosk STT — model=%s, path=%s",
            model_size,
            model_path or "(auto-download)",
        )

        loop = asyncio.get_running_loop()

        def _load():
            vosk.SetLogLevel(-1)  # Suppress vosk's verbose logging

            if model_path and Path(model_path).is_dir():
                return vosk.Model(model_path)

            resolved = self._ensure_model(model_size)
            return vosk.Model(str(resolved))

        try:
            self._model = await loop.run_in_executor(None, _load)
            logger.info("Vosk model loaded successfully")
        except Exception:
            logger.exception("Failed to load Vosk model")
            raise

    @staticmethod
    def _ensure_model(model_size: str) -> Path:
        """Download the Vosk model if not cached."""
        dir_name = _MODEL_DIRS.get(model_size, _MODEL_DIRS["small"])
        model_dir = _MODEL_DIR / dir_name

        if model_dir.exists() and (model_dir / "conf").exists():
            logger.info("Using cached Vosk model at %s", model_dir)
            return model_dir

        url = _MODEL_URLS.get(model_size, _MODEL_URLS["small"])
        logger.info("Downloading Vosk model from %s...", url)
        _MODEL_DIR.mkdir(parents=True, exist_ok=True)

        import subprocess

        archive = _MODEL_DIR / "model.zip"
        subprocess.run(["wget", "-q", "-O", str(archive), url], check=True)
        subprocess.run(
            ["unzip", "-q", "-o", str(archive), "-d", str(_MODEL_DIR)],
            check=True,
        )
        archive.unlink(missing_ok=True)

        if not model_dir.exists():
            raise FileNotFoundError(
                f"Vosk model download failed — expected at {model_dir}"
            )
        return model_dir

    async def transcribe(self, audio_bytes: bytes, sample_rate: int = 16000) -> str:
        """Transcribe PCM int16 audio bytes using Vosk.

        Vosk expects raw PCM int16 bytes directly (not float32),
        so we pass them through without conversion.
        """
        if self._model is None:
            raise RuntimeError("VoskBackend not initialized")

        if len(audio_bytes) == 0:
            return ""

        async with self._lock:
            loop = asyncio.get_running_loop()

            def _transcribe():
                import vosk

                rec = vosk.KaldiRecognizer(self._model, sample_rate)
                rec.AcceptWaveform(audio_bytes)
                result = json.loads(rec.FinalResult())
                return result.get("text", "").strip()

            try:
                text = await loop.run_in_executor(None, _transcribe)
            except Exception:
                logger.exception("Vosk transcription failed")
                return ""

        logger.debug(
            "Vosk transcribed %d bytes -> '%s'", len(audio_bytes), text
        )
        return text

    async def shutdown(self) -> None:
        self._model = None
        logger.info("Vosk backend shut down")

    @property
    def name(self) -> str:
        return f"Vosk ({self._config.model})"
