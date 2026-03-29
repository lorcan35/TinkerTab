"""WebSocket server for Dragon Voice.

Serves the voice pipeline over WebSocket and provides HTTP endpoints
for health checks, status, and configuration management.
"""

import asyncio
import json
import logging
import time
from typing import Optional

from aiohttp import web, WSMsgType

from dragon_voice.config import VoiceConfig, config_to_dict, load_config
from dragon_voice.pipeline import VoicePipeline

logger = logging.getLogger(__name__)


class VoiceServer:
    """Aiohttp-based WebSocket server for the Dragon Voice pipeline."""

    def __init__(self, config: VoiceConfig) -> None:
        self._config = config
        self._app: Optional[web.Application] = None
        self._start_time = time.time()
        self._session_count = 0
        self._active_sessions: dict[str, VoicePipeline] = {}

        # A shared pipeline for the status page — backends are per-session
        # but we track the configured names here
        self._stt_name = config.stt.backend
        self._tts_name = config.tts.backend
        self._llm_name = config.llm.backend

    def create_app(self) -> web.Application:
        """Create and configure the aiohttp application."""
        app = web.Application()

        # HTTP routes
        app.router.add_get("/", self._handle_status)
        app.router.add_get("/health", self._handle_health)
        app.router.add_get("/api/config", self._handle_get_config)
        app.router.add_post("/api/config", self._handle_set_config)

        # WebSocket route
        app.router.add_get("/ws/voice", self._handle_ws_voice)

        # Lifecycle hooks
        app.on_shutdown.append(self._on_shutdown)

        self._app = app
        return app

    # ------------------------------------------------------------------ HTTP

    async def _handle_status(self, request: web.Request) -> web.Response:
        """Status page with backend info and uptime."""
        uptime = time.time() - self._start_time
        hours = int(uptime // 3600)
        minutes = int((uptime % 3600) // 60)
        seconds = int(uptime % 60)

        html = f"""<!DOCTYPE html>
<html>
<head><title>Dragon Voice Server</title>
<style>
  body {{ font-family: monospace; background: #1a1a2e; color: #e0e0e0; padding: 2em; }}
  h1 {{ color: #ff6b35; }}
  .info {{ background: #16213e; padding: 1em; border-radius: 8px; margin: 1em 0; }}
  .label {{ color: #0f3460; font-weight: bold; }}
  span.val {{ color: #53d769; }}
</style>
</head>
<body>
  <h1>Dragon Voice Server</h1>
  <div class="info">
    <p>STT Backend: <span class="val">{self._stt_name}</span></p>
    <p>TTS Backend: <span class="val">{self._tts_name}</span></p>
    <p>LLM Backend: <span class="val">{self._llm_name}</span></p>
    <p>Uptime: <span class="val">{hours}h {minutes}m {seconds}s</span></p>
    <p>Active Sessions: <span class="val">{len(self._active_sessions)}</span></p>
    <p>Total Sessions: <span class="val">{self._session_count}</span></p>
  </div>
</body>
</html>"""
        return web.Response(text=html, content_type="text/html")

    async def _handle_health(self, request: web.Request) -> web.Response:
        """Health check endpoint returning JSON."""
        return web.json_response(
            {
                "status": "ok",
                "uptime_seconds": round(time.time() - self._start_time, 1),
                "active_sessions": len(self._active_sessions),
                "backends": {
                    "stt": self._stt_name,
                    "tts": self._tts_name,
                    "llm": self._llm_name,
                },
            }
        )

    async def _handle_get_config(self, request: web.Request) -> web.Response:
        """Return current config with secrets redacted."""
        return web.json_response(
            config_to_dict(self._config, redact_secrets=True)
        )

    async def _handle_set_config(self, request: web.Request) -> web.Response:
        """Hot-reload configuration.

        Accepts a partial config JSON — only provided sections are updated.
        Swaps backends on active sessions if needed.
        """
        try:
            body = await request.json()
        except json.JSONDecodeError:
            return web.json_response(
                {"error": "Invalid JSON"}, status=400
            )

        logger.info("Config update requested: %s", list(body.keys()))

        try:
            # Reload full config from file first, then apply overrides
            new_config = load_config()

            # Apply overrides from the request body
            if "stt" in body:
                for k, v in body["stt"].items():
                    if hasattr(new_config.stt, k):
                        setattr(new_config.stt, k, v)
            if "tts" in body:
                for k, v in body["tts"].items():
                    if hasattr(new_config.tts, k):
                        setattr(new_config.tts, k, v)
            if "llm" in body:
                for k, v in body["llm"].items():
                    if hasattr(new_config.llm, k):
                        setattr(new_config.llm, k, v)
            if "audio" in body:
                for k, v in body["audio"].items():
                    if hasattr(new_config.audio, k):
                        setattr(new_config.audio, k, v)

            old_config = self._config
            self._config = new_config

            # Update displayed backend names
            self._stt_name = new_config.stt.backend
            self._tts_name = new_config.tts.backend
            self._llm_name = new_config.llm.backend

            # Swap backends on all active sessions
            swap_tasks = []
            for session_id, pipeline in self._active_sessions.items():
                logger.info("Swapping backends for session %s", session_id)
                swap_tasks.append(pipeline.swap_backends(new_config))

            if swap_tasks:
                await asyncio.gather(*swap_tasks, return_exceptions=True)

            return web.json_response(
                {
                    "status": "ok",
                    "message": f"Config updated, {len(swap_tasks)} sessions reloaded",
                    "backends": {
                        "stt": new_config.stt.backend,
                        "tts": new_config.tts.backend,
                        "llm": new_config.llm.backend,
                    },
                }
            )

        except Exception as e:
            logger.exception("Config update failed")
            return web.json_response(
                {"error": str(e)}, status=500
            )

    # --------------------------------------------------------------- WebSocket

    async def _handle_ws_voice(self, request: web.Request) -> web.WebSocketResponse:
        """Main voice WebSocket endpoint.

        Protocol:
          Client -> Server:
            - Binary frames: raw PCM int16 16kHz mono audio
            - Text frames (JSON):
              {"type": "start"}  — begin a new session / reset
              {"type": "stop"}   — end of speech, process now
              {"type": "cancel"} — abort current processing

          Server -> Client:
            - Binary frames: PCM int16 audio at TTS sample rate
            - Text frames (JSON):
              {"type": "stt", "text": "..."}
              {"type": "llm", "text": "..."}
              {"type": "tts_start"}
              {"type": "tts_end"}
              {"type": "error", "message": "..."}
        """
        ws = web.WebSocketResponse(
            max_msg_size=10 * 1024 * 1024,  # 10MB max message
            heartbeat=600.0,
        )
        await ws.prepare(request)

        session_id = f"s{self._session_count}"
        self._session_count += 1

        peer = request.remote or "unknown"
        logger.info("WebSocket connected: %s (session %s)", peer, session_id)

        # Callbacks for the pipeline
        async def on_audio(audio_bytes: bytes) -> None:
            if not ws.closed:
                try:
                    await ws.send_bytes(audio_bytes)
                except Exception:
                    logger.warning("Failed to send audio to %s", session_id)

        async def on_event(event: dict) -> None:
            if not ws.closed:
                try:
                    await ws.send_json(event)
                except Exception:
                    logger.warning("Failed to send event to %s", session_id)

        # Create pipeline for this session
        pipeline = VoicePipeline(self._config, on_audio, on_event)

        try:
            await pipeline.initialize()
        except Exception as e:
            logger.exception("Failed to initialize pipeline for %s", session_id)
            await ws.send_json(
                {"type": "error", "message": f"Pipeline init failed: {e}"}
            )
            await ws.close()
            return ws

        self._active_sessions[session_id] = pipeline

        # Send session info
        await ws.send_json(
            {
                "type": "session_start",
                "session_id": session_id,
                "stt": pipeline.stt_name,
                "tts": pipeline.tts_name,
                "llm": pipeline.llm_name,
                "tts_sample_rate": pipeline.tts_sample_rate,
            }
        )

        try:
            async for msg in ws:
                if msg.type == WSMsgType.BINARY:
                    # Raw PCM audio data
                    logger.debug("Session %s: binary frame %d bytes (buf=%d)",
                                 session_id, len(msg.data), len(pipeline._audio_buffer))
                    await pipeline.feed_audio(msg.data)

                elif msg.type == WSMsgType.TEXT:
                    try:
                        cmd = json.loads(msg.data)
                    except json.JSONDecodeError:
                        logger.warning("Invalid JSON from %s: %s", session_id, msg.data[:100])
                        continue

                    cmd_type = cmd.get("type", "")

                    if cmd_type == "start":
                        pipeline.clear_history()
                        pipeline._audio_buffer.clear()
                        logger.info("Session %s: start (history cleared)", session_id)

                    elif cmd_type == "stop":
                        buf_size = len(pipeline._audio_buffer)
                        logger.info("Session %s: stop (buffer=%d bytes, processing=%s)",
                                    session_id, buf_size, pipeline._processing)
                        await pipeline.start_processing()

                    elif cmd_type == "cancel":
                        logger.info("Session %s: cancel", session_id)
                        await pipeline.cancel()

                    else:
                        logger.warning("Unknown command from %s: %s", session_id, cmd_type)

                elif msg.type == WSMsgType.ERROR:
                    logger.error(
                        "WebSocket error for %s: %s",
                        session_id,
                        ws.exception(),
                    )
                    break

        except asyncio.CancelledError:
            pass
        except Exception:
            logger.exception("WebSocket handler error for %s", session_id)
        finally:
            # Clean up
            self._active_sessions.pop(session_id, None)
            await pipeline.shutdown()
            logger.info("WebSocket disconnected: %s (session %s)", peer, session_id)

        return ws

    # ---------------------------------------------------------------- Lifecycle

    async def _on_shutdown(self, app: web.Application) -> None:
        """Clean up all active sessions on server shutdown."""
        logger.info("Server shutting down — closing %d sessions", len(self._active_sessions))
        tasks = []
        for session_id, pipeline in list(self._active_sessions.items()):
            tasks.append(pipeline.shutdown())
        if tasks:
            await asyncio.gather(*tasks, return_exceptions=True)
        self._active_sessions.clear()


def run_server(config: VoiceConfig) -> None:
    """Start the voice server (blocking)."""
    server = VoiceServer(config)
    app = server.create_app()

    logger.info(
        "Starting Dragon Voice Server on %s:%d",
        config.server.host,
        config.server.port,
    )

    web.run_app(
        app,
        host=config.server.host,
        port=config.server.port,
        print=lambda msg: logger.info(msg),
    )
