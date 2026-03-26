"""STT backend factory."""

from dragon_voice.config import STTConfig
from dragon_voice.stt.base import STTBackend

_BACKENDS = {
    "whisper_cpp": "dragon_voice.stt.whisper_cpp.WhisperCppBackend",
    "moonshine": "dragon_voice.stt.moonshine_stt.MoonshineBackend",
    "vosk": "dragon_voice.stt.vosk_stt.VoskBackend",
}


def create_stt(config: STTConfig) -> STTBackend:
    """Create an STT backend instance from configuration.

    Args:
        config: STT section of the voice config.

    Returns:
        An uninitialized STTBackend — caller must await .initialize().

    Raises:
        ValueError: If the requested backend name is unknown.
        ImportError: If the required package for the backend is not installed.
    """
    backend_name = config.backend.lower()
    if backend_name not in _BACKENDS:
        available = ", ".join(sorted(_BACKENDS.keys()))
        raise ValueError(
            f"Unknown STT backend '{backend_name}'. Available: {available}"
        )

    module_path, class_name = _BACKENDS[backend_name].rsplit(".", 1)

    import importlib

    try:
        module = importlib.import_module(module_path)
    except ImportError as e:
        raise ImportError(
            f"Cannot load STT backend '{backend_name}': {e}. "
            f"Install the required package — see requirements.txt."
        ) from e

    cls = getattr(module, class_name)
    return cls(config)


__all__ = ["create_stt", "STTBackend"]
