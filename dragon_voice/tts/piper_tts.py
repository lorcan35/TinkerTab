"""Piper TTS backend.

Piper is a fast, local neural TTS engine. On ARM it runs well with
the ONNX runtime. Supports auto-downloading voice models from the
Piper release repository.
"""

import asyncio
import io
import logging
import shutil
import struct
import subprocess
from pathlib import Path

import numpy as np

from dragon_voice.config import TTSConfig
from dragon_voice.tts.base import TTSBackend

logger = logging.getLogger(__name__)

_DATA_DIR = Path.home() / ".cache" / "dragon_voice" / "piper"
_PIPER_VOICES_URL = "https://huggingface.co/rhasspy/piper-voices/resolve/main"


class PiperBackend(TTSBackend):
    """TTS backend using Piper (piper-tts Python package or binary)."""

    def __init__(self, config: TTSConfig) -> None:
        self._config = config
        self._voice = None
        self._use_binary = False
        self._binary_path: str | None = None
        self._model_path: Path | None = None
        self._sample_rate = config.sample_rate or 22050
        self._lock = asyncio.Lock()

    async def initialize(self) -> None:
        """Load the Piper voice model.

        Tries the piper-tts Python package first. If not installed, falls
        back to the piper binary on PATH.
        """
        data_dir = Path(self._config.piper_data_dir) if self._config.piper_data_dir else _DATA_DIR
        data_dir.mkdir(parents=True, exist_ok=True)
        model_name = self._config.piper_model

        logger.info("Initializing Piper TTS — voice=%s", model_name)

        # Try Python package first
        try:
            import piper

            model_path = self._ensure_model(model_name, data_dir)
            loop = asyncio.get_running_loop()

            def _load():
                voice = piper.PiperVoice.load(str(model_path))
                return voice

            self._voice = await loop.run_in_executor(None, _load)
            self._model_path = model_path
            # Piper voices declare their sample rate in config
            config_path = model_path.with_suffix(model_path.suffix + ".json")
            if config_path.exists():
                import json
                with open(config_path) as f:
                    voice_cfg = json.load(f)
                self._sample_rate = voice_cfg.get("audio", {}).get("sample_rate", 22050)
            logger.info("Piper loaded via Python package (rate=%d)", self._sample_rate)
            return

        except ImportError:
            logger.info("piper-tts package not found, trying binary...")

        # Fallback: piper binary
        binary = shutil.which("piper")
        if binary is None:
            raise ImportError(
                "Neither piper-tts Python package nor piper binary found. "
                "Install one: pip install piper-tts  OR  download piper binary"
            )

        self._use_binary = True
        self._binary_path = binary
        self._model_path = self._ensure_model(model_name, data_dir)
        logger.info("Piper will use binary at %s", binary)

    @staticmethod
    def _ensure_model(model_name: str, data_dir: Path) -> Path:
        """Ensure the voice model ONNX file is downloaded.

        Piper model naming: en_US-lessac-medium -> en/en_US/lessac/medium/
        """
        # Check if already present
        onnx_file = data_dir / f"{model_name}.onnx"
        json_file = data_dir / f"{model_name}.onnx.json"

        if onnx_file.exists():
            logger.info("Using cached Piper model at %s", onnx_file)
            return onnx_file

        logger.info("Downloading Piper voice model '%s'...", model_name)

        # Parse model name for URL path: en_US-lessac-medium
        parts = model_name.split("-")
        if len(parts) >= 2:
            lang_code = parts[0]  # en_US
            lang_short = lang_code.split("_")[0]  # en
            speaker = parts[1]  # lessac
            quality = parts[2] if len(parts) > 2 else "medium"

            base_url = (
                f"{_PIPER_VOICES_URL}/{lang_short}/{lang_code}/{speaker}/{quality}"
            )
        else:
            base_url = f"{_PIPER_VOICES_URL}/en/en_US/{model_name}"

        for filename, target in [(f"{model_name}.onnx", onnx_file),
                                  (f"{model_name}.onnx.json", json_file)]:
            url = f"{base_url}/{filename}"
            logger.info("Downloading %s", url)
            try:
                subprocess.run(
                    ["wget", "-q", "-O", str(target), url],
                    check=True,
                    timeout=120,
                )
            except subprocess.CalledProcessError:
                logger.warning("Download failed for %s, trying alternate URL", url)
                # Try flat URL structure
                alt_url = f"{_PIPER_VOICES_URL}/{filename}"
                subprocess.run(
                    ["wget", "-q", "-O", str(target), alt_url],
                    check=True,
                    timeout=120,
                )

        if not onnx_file.exists():
            raise FileNotFoundError(f"Piper model download failed for {model_name}")

        return onnx_file

    async def synthesize(self, text: str) -> bytes:
        """Synthesize text to PCM int16 audio bytes."""
        if not text.strip():
            return b""

        async with self._lock:
            loop = asyncio.get_running_loop()

            if self._use_binary:
                return await loop.run_in_executor(None, self._synthesize_binary, text)
            else:
                return await loop.run_in_executor(None, self._synthesize_python, text)

    def _synthesize_python(self, text: str) -> bytes:
        """Synthesize using piper-tts Python package."""
        audio_buffer = io.BytesIO()
        try:
            # PiperVoice.synthesize yields AudioChunk objects with audio_int16_bytes
            for chunk in self._voice.synthesize(text):
                audio_buffer.write(chunk.audio_int16_bytes)
        except Exception:
            logger.exception("Piper Python synthesis failed")
            return b""

        pcm_data = audio_buffer.getvalue()
        logger.debug("Piper synthesized %d bytes for: %.40s...", len(pcm_data), text)
        return pcm_data

    def _synthesize_binary(self, text: str) -> bytes:
        """Synthesize using piper command-line binary."""
        try:
            result = subprocess.run(
                [
                    self._binary_path,
                    "--model", str(self._model_path),
                    "--output_raw",
                ],
                input=text.encode("utf-8"),
                capture_output=True,
                timeout=30,
            )
            if result.returncode != 0:
                logger.error("Piper binary error: %s", result.stderr.decode())
                return b""
            return result.stdout
        except subprocess.TimeoutExpired:
            logger.error("Piper binary timed out")
            return b""
        except Exception:
            logger.exception("Piper binary synthesis failed")
            return b""

    async def shutdown(self) -> None:
        self._voice = None
        logger.info("Piper TTS shut down")

    @property
    def sample_rate(self) -> int:
        return self._sample_rate

    @property
    def name(self) -> str:
        return f"Piper ({self._config.piper_model})"
