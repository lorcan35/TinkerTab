"""Ollama LLM backend.

Connects to a local Ollama server via its REST API. Supports streaming
token generation and conversation history management.
"""

import json
import logging
from typing import AsyncIterator

import aiohttp

from dragon_voice.config import LLMConfig
from dragon_voice.llm.base import LLMBackend

logger = logging.getLogger(__name__)


class OllamaBackend(LLMBackend):
    """LLM backend using Ollama's REST API."""

    def __init__(self, config: LLMConfig) -> None:
        self._config = config
        self._base_url = config.ollama_url.rstrip("/")
        self._model = config.ollama_model
        self._session: aiohttp.ClientSession | None = None
        self._conversation: list[dict] = []

    async def initialize(self) -> None:
        """Verify Ollama is reachable and the model is available."""
        self._session = aiohttp.ClientSession(
            timeout=aiohttp.ClientTimeout(total=120, sock_read=60)
        )

        logger.info(
            "Initializing Ollama backend — url=%s, model=%s",
            self._base_url,
            self._model,
        )

        try:
            async with self._session.get(f"{self._base_url}/api/tags") as resp:
                if resp.status != 200:
                    logger.warning(
                        "Ollama returned status %d — server may not be ready",
                        resp.status,
                    )
                    return

                data = await resp.json()
                models = [m["name"] for m in data.get("models", [])]

                # Normalize model names for comparison (strip :latest tag)
                model_base = self._model.split(":")[0]
                available = [m.split(":")[0] for m in models]

                if model_base not in available:
                    logger.warning(
                        "Model '%s' not found in Ollama. Available: %s. "
                        "Will attempt to pull on first request.",
                        self._model,
                        models[:10],
                    )
                else:
                    logger.info("Ollama model '%s' is available", self._model)

        except aiohttp.ClientError as e:
            logger.warning(
                "Cannot reach Ollama at %s: %s — will retry on first request",
                self._base_url,
                e,
            )

    async def generate_stream(
        self, prompt: str, system_prompt: str = ""
    ) -> AsyncIterator[str]:
        """Stream tokens from Ollama using the /api/chat endpoint.

        Maintains conversation history for multi-turn dialogue.
        """
        if self._session is None or self._session.closed:
            self._session = aiohttp.ClientSession(
                timeout=aiohttp.ClientTimeout(total=120, sock_read=60)
            )

        sys_prompt = system_prompt or self._config.system_prompt

        # Build messages list with history
        messages = []
        if sys_prompt:
            messages.append({"role": "system", "content": sys_prompt})
        messages.extend(self._conversation)
        messages.append({"role": "user", "content": prompt})

        payload = {
            "model": self._model,
            "messages": messages,
            "stream": True,
            "options": {
                "num_predict": self._config.max_tokens,
                "temperature": self._config.temperature,
            },
        }

        full_response = []

        try:
            async with self._session.post(
                f"{self._base_url}/api/chat",
                json=payload,
            ) as resp:
                if resp.status != 200:
                    error_text = await resp.text()
                    logger.error("Ollama error %d: %s", resp.status, error_text[:200])
                    yield f"[Ollama error: {resp.status}]"
                    return

                async for raw_line in resp.content:
                    line = raw_line.decode("utf-8", errors="replace").strip()
                    if not line:
                        continue
                    try:
                        chunk = json.loads(line)
                    except json.JSONDecodeError:
                        continue

                    if chunk.get("done"):
                        break

                    token = chunk.get("message", {}).get("content", "")
                    if token:
                        full_response.append(token)
                        yield token

        except aiohttp.ClientError as e:
            logger.error("Ollama request failed: %s", e)
            yield f"[Connection error: {e}]"
            return

        # Update conversation history
        self._conversation.append({"role": "user", "content": prompt})
        self._conversation.append(
            {"role": "assistant", "content": "".join(full_response)}
        )

    def clear_history(self) -> None:
        """Clear conversation history."""
        self._conversation.clear()
        logger.debug("Ollama conversation history cleared")

    def trim_history(self, max_turns: int = 10) -> None:
        """Keep only the last N turns (each turn = user + assistant)."""
        max_messages = max_turns * 2
        if len(self._conversation) > max_messages:
            self._conversation = self._conversation[-max_messages:]

    async def shutdown(self) -> None:
        if self._session and not self._session.closed:
            await self._session.close()
        self._session = None
        logger.info("Ollama backend shut down")

    @property
    def name(self) -> str:
        return f"Ollama ({self._model})"
