"""Dragon Voice Server — STT/LLM/TTS pipeline for TinkerClaw.

Runs on the Dragon ARM mini-PC, receives audio from Tab5 via WebSocket,
processes through configurable STT -> LLM -> TTS backends, and streams
audio back.
"""

__version__ = "0.1.0"
__all__ = ["__version__"]
