"""LLM backend factory."""

from dragon_voice.config import LLMConfig
from dragon_voice.llm.base import LLMBackend

_BACKENDS = {
    "ollama": "dragon_voice.llm.ollama_llm.OllamaBackend",
    "openrouter": "dragon_voice.llm.openrouter_llm.OpenRouterBackend",
    "lmstudio": "dragon_voice.llm.lmstudio_llm.LMStudioBackend",
}


def create_llm(config: LLMConfig) -> LLMBackend:
    """Create an LLM backend instance from configuration.

    Args:
        config: LLM section of the voice config.

    Returns:
        An uninitialized LLMBackend — caller must await .initialize().

    Raises:
        ValueError: If the requested backend name is unknown.
    """
    backend_name = config.backend.lower()
    if backend_name not in _BACKENDS:
        available = ", ".join(sorted(_BACKENDS.keys()))
        raise ValueError(
            f"Unknown LLM backend '{backend_name}'. Available: {available}"
        )

    module_path, class_name = _BACKENDS[backend_name].rsplit(".", 1)

    import importlib

    module = importlib.import_module(module_path)
    cls = getattr(module, class_name)
    return cls(config)


__all__ = ["create_llm", "LLMBackend"]
