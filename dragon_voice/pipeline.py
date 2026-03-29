"""Voice pipeline orchestrator: Audio -> STT -> LLM -> TTS -> Audio.

Receives raw PCM audio, detects end-of-speech via simple VAD, transcribes
with STT, streams LLM response, buffers until sentence boundaries, and
synthesizes each sentence with TTS. Results are delivered via async callback.
"""

import asyncio
import logging
import re
import time
from typing import Callable, Awaitable, Optional

import numpy as np

from dragon_voice.config import VoiceConfig
from dragon_voice.stt import create_stt, STTBackend
from dragon_voice.tts import create_tts, TTSBackend
from dragon_voice.llm import create_llm, LLMBackend

logger = logging.getLogger(__name__)

# Regex for sentence boundary detection
_SENTENCE_END = re.compile(r"[.!?]\s*$")
_SENTENCE_SPLIT = re.compile(r"(?<=[.!?])\s+")

# VAD constants
_SILENCE_THRESHOLD = 500  # RMS amplitude below this = silence (int16 range)


class VoicePipeline:
    """Orchestrates the full STT -> LLM -> TTS voice pipeline.

    One pipeline instance per WebSocket session. Manages audio buffering,
    VAD, transcription, LLM streaming, sentence buffering, and TTS synthesis.
    """

    def __init__(
        self,
        config: VoiceConfig,
        on_audio: Callable[[bytes], Awaitable[None]],
        on_event: Callable[[dict], Awaitable[None]],
    ) -> None:
        """Initialize the pipeline.

        Args:
            config: Full voice configuration.
            on_audio: Async callback invoked with PCM int16 audio chunks
                     to send back to the client.
            on_event: Async callback invoked with JSON-serializable event
                     dicts (stt results, llm tokens, status, errors).
        """
        self._config = config
        self._on_audio = on_audio
        self._on_event = on_event

        self._stt: Optional[STTBackend] = None
        self._tts: Optional[TTSBackend] = None
        self._llm: Optional[LLMBackend] = None

        # Audio buffer for incoming PCM data
        self._audio_buffer = bytearray()
        self._last_voice_time = 0.0
        self._is_speaking = False

        # Conversation history (last N turns)
        self._max_history = 10

        # Pipeline state
        self._processing = False
        self._cancelled = False
        self._process_task: Optional[asyncio.Task] = None

    async def initialize(self) -> None:
        """Create and initialize all backends."""
        logger.info("Initializing voice pipeline...")

        self._stt = create_stt(self._config.stt)
        self._tts = create_tts(self._config.tts)
        self._llm = create_llm(self._config.llm)

        # Initialize in parallel
        await asyncio.gather(
            self._stt.initialize(),
            self._tts.initialize(),
            self._llm.initialize(),
        )

        logger.info(
            "Pipeline ready — STT=%s, TTS=%s, LLM=%s",
            self._stt.name,
            self._tts.name,
            self._llm.name,
        )

    async def feed_audio(self, audio_bytes: bytes) -> None:
        """Feed incoming PCM int16 audio data into the pipeline.

        Buffers audio and uses simple VAD to detect end of speech.
        When silence is detected after speech, triggers processing.
        """
        if self._processing:
            # If already processing a previous utterance, ignore new audio
            # (or could queue it — for now, drop)
            return

        self._audio_buffer.extend(audio_bytes)

        if not self._config.audio.vad_enabled:
            return

        # Simple energy-based VAD
        audio_i16 = np.frombuffer(audio_bytes, dtype=np.int16)
        if len(audio_i16) == 0:
            return

        rms = np.sqrt(np.mean(audio_i16.astype(np.float32) ** 2))

        now = time.monotonic()

        if rms > _SILENCE_THRESHOLD:
            self._is_speaking = True
            self._last_voice_time = now
        elif self._is_speaking:
            # Check if silence duration exceeds threshold
            silence_ms = (now - self._last_voice_time) * 1000
            if silence_ms >= self._config.audio.vad_silence_ms:
                logger.debug(
                    "VAD: silence detected after %.0fms, processing %d bytes",
                    silence_ms,
                    len(self._audio_buffer),
                )
                self._is_speaking = False
                # Trigger processing
                audio_data = bytes(self._audio_buffer)
                self._audio_buffer.clear()
                self._process_task = asyncio.create_task(
                    self._process_utterance(audio_data)
                )

    async def start_processing(self) -> None:
        """Manually trigger processing of buffered audio (e.g. on "stop" command)."""
        if self._processing:
            return

        if len(self._audio_buffer) < 1600:  # Less than 50ms at 16kHz
            logger.debug("Audio buffer too small to process (%d bytes)", len(self._audio_buffer))
            return

        audio_data = bytes(self._audio_buffer)
        self._audio_buffer.clear()

        # Debug: save incoming audio as WAV for inspection
        import wave, os
        wav_path = "/tmp/tab5_mic_debug.wav"
        try:
            with wave.open(wav_path, "wb") as wf:
                wf.setnchannels(1)
                wf.setsampwidth(2)  # 16-bit
                wf.setframerate(self._config.audio.input_sample_rate)
                wf.writeframes(audio_data)
            logger.info("DEBUG: saved %d bytes mic audio to %s", len(audio_data), wav_path)
        except Exception as e:
            logger.warning("DEBUG: failed to save WAV: %s", e)
        self._is_speaking = False
        self._process_task = asyncio.create_task(
            self._process_utterance(audio_data)
        )

    async def cancel(self) -> None:
        """Cancel ongoing processing."""
        self._cancelled = True
        if self._process_task and not self._process_task.done():
            self._process_task.cancel()
            try:
                await self._process_task
            except asyncio.CancelledError:
                pass
        self._processing = False
        self._cancelled = False
        self._audio_buffer.clear()
        logger.info("Pipeline processing cancelled")

    async def _process_utterance(self, audio_data: bytes) -> None:
        """Run the full STT -> LLM -> TTS pipeline on a chunk of audio."""
        self._processing = True
        self._cancelled = False

        try:
            # Debug: save incoming audio as WAV for inspection
            import wave
            wav_path = "/tmp/tab5_mic_debug.wav"
            try:
                with wave.open(wav_path, "wb") as wf:
                    wf.setnchannels(1)
                    wf.setsampwidth(2)  # 16-bit
                    wf.setframerate(self._config.audio.input_sample_rate)
                    wf.writeframes(audio_data)
                logger.info("DEBUG: saved %d bytes mic audio to %s", len(audio_data), wav_path)
            except Exception as e:
                logger.warning("DEBUG: failed to save WAV: %s", e)

            # --- STT ---
            t0 = time.monotonic()
            transcript = await self._stt.transcribe(
                audio_data, self._config.audio.input_sample_rate
            )
            stt_ms = (time.monotonic() - t0) * 1000

            if not transcript.strip():
                logger.info("STT returned empty transcript, skipping (audio=%d bytes)", len(audio_data))
                return

            logger.info("STT (%.0fms): %s", stt_ms, transcript)
            await self._on_event({"type": "stt", "text": transcript})

            if self._cancelled:
                return

            # --- LLM (streaming) ---
            t0 = time.monotonic()
            sentence_buffer = ""
            full_response = ""

            async for token in self._llm.generate_stream(
                transcript, self._config.llm.system_prompt
            ):
                if self._cancelled:
                    return

                await self._on_event({"type": "llm", "text": token})
                sentence_buffer += token
                full_response += token

                # Check for sentence boundary — flush to TTS
                if _SENTENCE_END.search(sentence_buffer):
                    sentences = _SENTENCE_SPLIT.split(sentence_buffer)
                    # Send all complete sentences, keep incomplete tail
                    remainder = ""
                    for i, sentence in enumerate(sentences):
                        if i < len(sentences) - 1 or _SENTENCE_END.search(sentence):
                            if sentence.strip():
                                await self._synthesize_and_send(sentence.strip())
                        else:
                            # Last fragment is incomplete — keep buffering
                            remainder = sentence
                    sentence_buffer = remainder

            # Flush remaining text
            if sentence_buffer.strip() and not self._cancelled:
                await self._synthesize_and_send(sentence_buffer.strip())

            llm_ms = (time.monotonic() - t0) * 1000
            logger.info("LLM (%.0fms): %s", llm_ms, full_response[:80])

            # Trim conversation history on the LLM backend
            if hasattr(self._llm, "trim_history"):
                self._llm.trim_history(self._max_history)

        except asyncio.CancelledError:
            logger.info("Pipeline processing was cancelled")
        except Exception:
            logger.exception("Pipeline processing error")
            try:
                await self._on_event(
                    {"type": "error", "message": "Processing failed — see server logs"}
                )
            except Exception:
                pass
        finally:
            self._processing = False

    async def _synthesize_and_send(self, text: str) -> None:
        """Synthesize a sentence, resample to 16kHz, and stream back to client."""
        if self._cancelled:
            return

        try:
            await self._on_event({"type": "tts_start"})

            t0 = time.monotonic()
            audio_bytes = await self._tts.synthesize(text)
            tts_ms = (time.monotonic() - t0) * 1000

            if audio_bytes:
                # Resample from TTS sample rate to 16kHz for Tab5 playback
                tts_rate = self._tts.sample_rate if self._tts else 22050
                target_rate = self._config.audio.input_sample_rate  # 16000

                if tts_rate != target_rate:
                    audio_i16 = np.frombuffer(audio_bytes, dtype=np.int16)
                    # Simple linear interpolation resample
                    ratio = target_rate / tts_rate
                    new_len = int(len(audio_i16) * ratio)
                    indices = np.arange(new_len) / ratio
                    indices_floor = indices.astype(np.int32)
                    indices_floor = np.clip(indices_floor, 0, len(audio_i16) - 2)
                    frac = indices - indices_floor
                    resampled = (
                        audio_i16[indices_floor] * (1 - frac)
                        + audio_i16[indices_floor + 1] * frac
                    ).astype(np.int16)
                    audio_bytes = resampled.tobytes()

                logger.debug(
                    "TTS (%.0fms): %d bytes @ %dHz for '%.40s...'",
                    tts_ms,
                    len(audio_bytes),
                    target_rate,
                    text,
                )
                # Send audio in chunks to avoid overwhelming the WebSocket
                chunk_size = 4096
                for i in range(0, len(audio_bytes), chunk_size):
                    if self._cancelled:
                        return
                    chunk = audio_bytes[i : i + chunk_size]
                    await self._on_audio(chunk)

            await self._on_event({"type": "tts_end"})

        except Exception:
            logger.exception("TTS synthesis/send failed for: %.40s...", text)

    def clear_history(self) -> None:
        """Clear conversation history."""
        if hasattr(self._llm, "clear_history"):
            self._llm.clear_history()
        logger.info("Conversation history cleared")

    async def swap_backends(self, config: VoiceConfig) -> None:
        """Hot-swap backends based on new configuration.

        Only reinitializes backends that have actually changed.
        """
        old_config = self._config
        self._config = config

        tasks = []

        # Check if STT backend changed
        if (
            config.stt.backend != old_config.stt.backend
            or config.stt.model != old_config.stt.model
        ):
            logger.info("Swapping STT: %s -> %s", old_config.stt.backend, config.stt.backend)
            if self._stt:
                await self._stt.shutdown()
            self._stt = create_stt(config.stt)
            tasks.append(self._stt.initialize())

        # Check if TTS backend changed
        if config.tts.backend != old_config.tts.backend:
            logger.info("Swapping TTS: %s -> %s", old_config.tts.backend, config.tts.backend)
            if self._tts:
                await self._tts.shutdown()
            self._tts = create_tts(config.tts)
            tasks.append(self._tts.initialize())

        # Check if LLM backend changed
        if (
            config.llm.backend != old_config.llm.backend
            or config.llm.ollama_model != old_config.llm.ollama_model
        ):
            logger.info("Swapping LLM: %s -> %s", old_config.llm.backend, config.llm.backend)
            if self._llm:
                await self._llm.shutdown()
            self._llm = create_llm(config.llm)
            tasks.append(self._llm.initialize())

        if tasks:
            await asyncio.gather(*tasks)
            logger.info("Backend swap complete")

    @property
    def stt_name(self) -> str:
        return self._stt.name if self._stt else "none"

    @property
    def tts_name(self) -> str:
        return self._tts.name if self._tts else "none"

    @property
    def llm_name(self) -> str:
        return self._llm.name if self._llm else "none"

    @property
    def tts_sample_rate(self) -> int:
        return self._tts.sample_rate if self._tts else 22050

    @property
    def is_processing(self) -> bool:
        return self._processing

    async def shutdown(self) -> None:
        """Shut down all backends."""
        await self.cancel()
        tasks = []
        if self._stt:
            tasks.append(self._stt.shutdown())
        if self._tts:
            tasks.append(self._tts.shutdown())
        if self._llm:
            tasks.append(self._llm.shutdown())
        if tasks:
            await asyncio.gather(*tasks, return_exceptions=True)
        logger.info("Voice pipeline shut down")
