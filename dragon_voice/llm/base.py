"""Abstract base class for LLM backends."""

from abc import ABC, abstractmethod
from typing import AsyncIterator


class LLMBackend(ABC):
    """Interface that all LLM backends must implement."""

    @abstractmethod
    async def initialize(self) -> None:
        """Verify connectivity and prepare for inference. Called once at startup."""
        ...

    @abstractmethod
    async def generate_stream(
        self, prompt: str, system_prompt: str = ""
    ) -> AsyncIterator[str]:
        """Stream text tokens from the LLM.

        Args:
            prompt: The user's message / transcribed speech.
            system_prompt: System prompt for the conversation. If empty,
                          the backend should use its configured default.

        Yields:
            Text tokens (strings) as they arrive from the model.
        """
        ...

    @abstractmethod
    async def shutdown(self) -> None:
        """Release resources. Called once at server shutdown."""
        ...

    @property
    @abstractmethod
    def name(self) -> str:
        """Human-readable backend name for logging and status pages."""
        ...
