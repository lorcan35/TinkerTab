"""Edge TTS backend using Microsoft's free TTS API.

Requires internet connectivity. Falls back gracefully when offline.
Uses the edge-tts package for async streaming synthesis.
"""

import asyncio
import io
import logging
import struct

import numpy as np

from dragon_voice.config import TTSConfig
from dragon_voice.tts.base import TTSBackend

logger = logging.getLogger(__name__)

_EDGE_TTS_SAMPLE_RATE = 24000  # Edge TTS outputs 24kHz audio


class EdgeTTSBackend(TTSBackend):
    """TTS backend using Microsoft Edge TTS (free, cloud-based)."""

    def __init__(self, config: TTSConfig) -> None:
        self._config = config
        self._voice = config.edge_voice
        self._available = False

    async def initialize(self) -> None:
        """Verify edge-tts is installed and reachable."""
        try:
            import edge_tts
        except ImportError:
            raise ImportError(
                "edge-tts is required for the edge_tts backend. "
                "Install it: pip install edge-tts"
            )

        logger.info("Initializing Edge TTS — voice=%s", self._voice)

        # Quick connectivity test: try listing voices
        try:
            voices = await edge_tts.list_voices()
            voice_names = [v["ShortName"] for v in voices]
            if self._voice not in voice_names:
                logger.warning(
                    "Voice '%s' not found in Edge TTS. Available: %s... "
                    "Will attempt to use it anyway.",
                    self._voice,
                    voice_names[:5],
                )
            self._available = True
            logger.info("Edge TTS initialized — %d voices available", len(voices))
        except Exception:
            logger.warning(
                "Edge TTS connectivity check failed — synthesis will be "
                "attempted but may fail if offline"
            )
            self._available = True  # Still allow attempts

    async def synthesize(self, text: str) -> bytes:
        """Synthesize text to PCM int16 audio via Edge TTS.

        Edge TTS returns MP3 data, which we decode to raw PCM.
        """
        if not text.strip():
            return b""

        try:
            import edge_tts
        except ImportError:
            logger.error("edge-tts not installed")
            return b""

        try:
            communicate = edge_tts.Communicate(text, self._voice)

            # Collect MP3 chunks
            mp3_buffer = io.BytesIO()
            async for chunk in communicate.stream():
                if chunk["type"] == "audio":
                    mp3_buffer.write(chunk["data"])

            mp3_data = mp3_buffer.getvalue()
            if not mp3_data:
                logger.warning("Edge TTS returned no audio for: %.40s...", text)
                return b""

            # Decode MP3 to PCM using numpy/subprocess
            pcm = await self._decode_mp3(mp3_data)
            logger.debug(
                "Edge TTS synthesized %d PCM bytes for: %.40s...", len(pcm), text
            )
            return pcm

        except Exception:
            logger.exception("Edge TTS synthesis failed")
            return b""

    @staticmethod
    async def _decode_mp3(mp3_data: bytes) -> bytes:
        """Decode MP3 bytes to PCM int16 using ffmpeg.

        Returns raw PCM 16-bit signed integer data at 24kHz mono.
        """
        loop = asyncio.get_running_loop()

        def _ffmpeg_decode():
            import subprocess

            result = subprocess.run(
                [
                    "ffmpeg",
                    "-i", "pipe:0",
                    "-f", "s16le",
                    "-acodec", "pcm_s16le",
                    "-ar", str(_EDGE_TTS_SAMPLE_RATE),
                    "-ac", "1",
                    "pipe:1",
                ],
                input=mp3_data,
                capture_output=True,
                timeout=15,
            )
            if result.returncode != 0:
                logger.error("ffmpeg decode failed: %s", result.stderr.decode()[:200])
                return b""
            return result.stdout

        return await loop.run_in_executor(None, _ffmpeg_decode)

    async def shutdown(self) -> None:
        self._available = False
        logger.info("Edge TTS shut down")

    @property
    def sample_rate(self) -> int:
        return _EDGE_TTS_SAMPLE_RATE

    @property
    def name(self) -> str:
        return f"Edge TTS ({self._voice})"
