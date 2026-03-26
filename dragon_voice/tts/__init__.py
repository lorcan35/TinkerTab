"""TTS backend factory."""

from dragon_voice.config import TTSConfig
from dragon_voice.tts.base import TTSBackend

_BACKENDS = {
    "piper": "dragon_voice.tts.piper_tts.PiperBackend",
    "kokoro": "dragon_voice.tts.kokoro_tts.KokoroBackend",
    "edge_tts": "dragon_voice.tts.edge_tts_backend.EdgeTTSBackend",
}


def create_tts(config: TTSConfig) -> TTSBackend:
    """Create a TTS backend instance from configuration.

    Args:
        config: TTS section of the voice config.

    Returns:
        An uninitialized TTSBackend — caller must await .initialize().

    Raises:
        ValueError: If the requested backend name is unknown.
        ImportError: If the required package for the backend is not installed.
    """
    backend_name = config.backend.lower()
    if backend_name not in _BACKENDS:
        available = ", ".join(sorted(_BACKENDS.keys()))
        raise ValueError(
            f"Unknown TTS backend '{backend_name}'. Available: {available}"
        )

    module_path, class_name = _BACKENDS[backend_name].rsplit(".", 1)

    import importlib

    try:
        module = importlib.import_module(module_path)
    except ImportError as e:
        raise ImportError(
            f"Cannot load TTS backend '{backend_name}': {e}. "
            f"Install the required package — see requirements.txt."
        ) from e

    cls = getattr(module, class_name)
    return cls(config)


__all__ = ["create_tts", "TTSBackend"]
