"""OpenRouter LLM backend.

Uses the OpenRouter API (OpenAI-compatible) for cloud-based LLM inference.
Supports streaming via Server-Sent Events (SSE).
"""

import json
import logging
import os
from typing import AsyncIterator

import aiohttp

from dragon_voice.config import LLMConfig
from dragon_voice.llm.base import LLMBackend

logger = logging.getLogger(__name__)


class OpenRouterBackend(LLMBackend):
    """LLM backend using the OpenRouter API."""

    def __init__(self, config: LLMConfig) -> None:
        self._config = config
        self._base_url = config.openrouter_url.rstrip("/")
        self._model = config.openrouter_model
        self._api_key = config.openrouter_api_key or os.environ.get(
            "OPENROUTER_API_KEY", ""
        )
        self._session: aiohttp.ClientSession | None = None
        self._conversation: list[dict] = []

    async def initialize(self) -> None:
        """Verify API key is set and connectivity is available."""
        if not self._api_key:
            raise ValueError(
                "OpenRouter API key not configured. Set openrouter_api_key in "
                "config.yaml or OPENROUTER_API_KEY environment variable."
            )

        self._session = aiohttp.ClientSession(
            timeout=aiohttp.ClientTimeout(total=120, sock_read=60),
            headers={
                "Authorization": f"Bearer {self._api_key}",
                "Content-Type": "application/json",
                "HTTP-Referer": "https://tinkerclaw.local",
                "X-Title": "TinkerClaw Dragon Voice",
            },
        )

        logger.info(
            "Initializing OpenRouter backend — model=%s, url=%s",
            self._model,
            self._base_url,
        )

        # Quick connectivity check
        try:
            async with self._session.get(f"{self._base_url}/models") as resp:
                if resp.status == 200:
                    logger.info("OpenRouter API reachable")
                else:
                    logger.warning("OpenRouter returned status %d", resp.status)
        except aiohttp.ClientError as e:
            logger.warning("Cannot reach OpenRouter: %s", e)

    async def generate_stream(
        self, prompt: str, system_prompt: str = ""
    ) -> AsyncIterator[str]:
        """Stream tokens from OpenRouter using SSE.

        Uses the OpenAI-compatible /chat/completions endpoint.
        """
        if self._session is None or self._session.closed:
            await self.initialize()

        sys_prompt = system_prompt or self._config.system_prompt

        messages = []
        if sys_prompt:
            messages.append({"role": "system", "content": sys_prompt})
        messages.extend(self._conversation)
        messages.append({"role": "user", "content": prompt})

        payload = {
            "model": self._model,
            "messages": messages,
            "stream": True,
            "max_tokens": self._config.max_tokens,
            "temperature": self._config.temperature,
        }

        full_response = []

        try:
            async with self._session.post(
                f"{self._base_url}/chat/completions",
                json=payload,
            ) as resp:
                if resp.status != 200:
                    error_text = await resp.text()
                    logger.error(
                        "OpenRouter error %d: %s", resp.status, error_text[:300]
                    )
                    yield f"[OpenRouter error: {resp.status}]"
                    return

                # Parse SSE stream
                async for line in resp.content:
                    line = line.decode("utf-8", errors="replace").strip()
                    if not line or not line.startswith("data: "):
                        continue

                    data_str = line[6:]  # Strip "data: " prefix
                    if data_str == "[DONE]":
                        break

                    try:
                        chunk = json.loads(data_str)
                    except json.JSONDecodeError:
                        continue

                    choices = chunk.get("choices", [])
                    if not choices:
                        continue

                    delta = choices[0].get("delta", {})
                    token = delta.get("content", "")
                    if token:
                        full_response.append(token)
                        yield token

        except aiohttp.ClientError as e:
            logger.error("OpenRouter request failed: %s", e)
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

    def trim_history(self, max_turns: int = 10) -> None:
        """Keep only the last N turns."""
        max_messages = max_turns * 2
        if len(self._conversation) > max_messages:
            self._conversation = self._conversation[-max_messages:]

    async def shutdown(self) -> None:
        if self._session and not self._session.closed:
            await self._session.close()
        self._session = None
        logger.info("OpenRouter backend shut down")

    @property
    def name(self) -> str:
        return f"OpenRouter ({self._model})"
