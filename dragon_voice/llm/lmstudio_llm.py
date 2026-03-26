"""LM Studio LLM backend.

Connects to a local LM Studio server running an OpenAI-compatible API.
Very similar to OpenRouter but targeting localhost with no auth required.
"""

import json
import logging
from typing import AsyncIterator

import aiohttp

from dragon_voice.config import LLMConfig
from dragon_voice.llm.base import LLMBackend

logger = logging.getLogger(__name__)


class LMStudioBackend(LLMBackend):
    """LLM backend using LM Studio's local OpenAI-compatible API."""

    def __init__(self, config: LLMConfig) -> None:
        self._config = config
        self._base_url = config.lmstudio_url.rstrip("/")
        self._model = config.lmstudio_model
        self._session: aiohttp.ClientSession | None = None
        self._conversation: list[dict] = []

    async def initialize(self) -> None:
        """Verify LM Studio is reachable."""
        self._session = aiohttp.ClientSession(
            timeout=aiohttp.ClientTimeout(total=120, sock_read=60),
            headers={"Content-Type": "application/json"},
        )

        logger.info(
            "Initializing LM Studio backend — model=%s, url=%s",
            self._model,
            self._base_url,
        )

        try:
            async with self._session.get(f"{self._base_url}/models") as resp:
                if resp.status == 200:
                    data = await resp.json()
                    models = [m.get("id", "") for m in data.get("data", [])]
                    logger.info("LM Studio reachable — models: %s", models[:5])
                else:
                    logger.warning(
                        "LM Studio returned status %d — may not be running",
                        resp.status,
                    )
        except aiohttp.ClientError as e:
            logger.warning(
                "Cannot reach LM Studio at %s: %s — will retry on first request",
                self._base_url,
                e,
            )

    async def generate_stream(
        self, prompt: str, system_prompt: str = ""
    ) -> AsyncIterator[str]:
        """Stream tokens from LM Studio using SSE."""
        if self._session is None or self._session.closed:
            self._session = aiohttp.ClientSession(
                timeout=aiohttp.ClientTimeout(total=120, sock_read=60),
                headers={"Content-Type": "application/json"},
            )

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
                        "LM Studio error %d: %s", resp.status, error_text[:300]
                    )
                    yield f"[LM Studio error: {resp.status}]"
                    return

                async for line in resp.content:
                    line = line.decode("utf-8", errors="replace").strip()
                    if not line or not line.startswith("data: "):
                        continue

                    data_str = line[6:]
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
            logger.error("LM Studio request failed: %s", e)
            yield f"[Connection error: {e}]"
            return

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
        logger.info("LM Studio backend shut down")

    @property
    def name(self) -> str:
        return f"LM Studio ({self._model})"
