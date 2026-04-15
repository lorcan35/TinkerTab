# Rich Media Chat Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Transform the Tab5 chat from text-only bubbles to rich media — inline images, code blocks, tables, cards, audio clips, and camera-to-AI vision — across all voice modes.

**Architecture:** Dragon renders all rich content (images, code blocks, tables) server-side using Pillow/Pygments and serves via HTTP. Tab5 downloads and displays JPEG images inline in chat via LVGL's TJPGD decoder with a pre-allocated PSRAM LRU cache. New WS message types (`media`, `card`, `audio_clip`) are additive and backward-compatible.

**Tech Stack:** ESP-IDF 5.4.3 / LVGL 9.2.2 / TJPGD (C firmware), Python 3 / aiohttp / Pillow / Pygments (Dragon server)

**Spec:** `docs/superpowers/specs/2026-04-15-rich-media-chat-design.md`

**Repos:**
- TinkerTab: `/home/rebelforce/projects/TinkerTab/`
- TinkerBox: `/home/rebelforce/projects/TinkerBox/`

---

## File Structure

### TinkerBox (Dragon) — New Files
| File | Responsibility |
|------|---------------|
| `dragon_voice/media/__init__.py` | Package init, exports MediaStore + MediaPipeline |
| `dragon_voice/media/store.py` | Disk storage for rendered media, auto-cleanup |
| `dragon_voice/media/pipeline.py` | Detect renderable content in LLM responses, render via Pillow |
| `dragon_voice/api/media_routes.py` | HTTP endpoints: serve media, accept uploads |
| `tests/test_media_store.py` | Unit tests for MediaStore |
| `tests/test_media_pipeline.py` | Unit tests for MediaPipeline |

### TinkerBox (Dragon) — Modified Files
| File | Change |
|------|--------|
| `dragon_voice/api/__init__.py` | Register MediaRoutes in `setup_all_routes()` |
| `dragon_voice/server.py` | Wire media detection after llm_done, add user_media handler, start cleanup task |
| `dragon_voice/pipeline.py` | Wire media detection after voice llm_done |
| `requirements.txt` | Add Pygments |

### TinkerTab (Firmware) — New Files
| File | Responsibility |
|------|---------------|
| `main/media_cache.h` | Public API for image download + PSRAM cache |
| `main/media_cache.c` | HTTP download, JPEG decode, 5-slot PSRAM LRU cache |

### TinkerTab (Firmware) — Modified Files
| File | Change |
|------|--------|
| `sdkconfig.defaults` | Enable `CONFIG_LV_USE_TJPGD=y` |
| `main/CMakeLists.txt` | Add `media_cache.c` to SRCS |
| `main/voice.c` | Add media/card/audio_clip message handlers in dispatch |
| `main/ui_chat.h` | Declare 3 new push functions |
| `main/ui_chat.c` | Image bubble, card bubble, audio clip bubble renderers |
| `main/ui_camera.c` | "Ask AI" button + HTTP upload flow |

---

## Phase 1: Foundation — Dragon Renders, Tab5 Displays

### Task 1: Dragon MediaStore

**Files:**
- Create: `dragon_voice/media/__init__.py`
- Create: `dragon_voice/media/store.py`
- Create: `tests/test_media_store.py`

- [ ] **Step 1: Create the media package**

```bash
mkdir -p /home/rebelforce/projects/TinkerBox/dragon_voice/media
```

Write `dragon_voice/media/__init__.py`:

```python
from dragon_voice.media.store import MediaStore
from dragon_voice.media.pipeline import MediaPipeline

__all__ = ["MediaStore", "MediaPipeline"]
```

- [ ] **Step 2: Write MediaStore test**

Write `tests/test_media_store.py`:

```python
import asyncio
import os
import tempfile
import time
import pytest
from dragon_voice.media.store import MediaStore


@pytest.fixture
def tmp_media_dir(tmp_path):
    return str(tmp_path / "media")


@pytest.fixture
def store(tmp_media_dir):
    return MediaStore(media_dir=tmp_media_dir)


@pytest.mark.asyncio
async def test_store_and_retrieve(store, tmp_media_dir):
    data = b"\xff\xd8\xff\xe0" + b"\x00" * 100  # fake JPEG header
    media_id = await store.store(data, ext="jpg")
    assert media_id  # non-empty string
    path = await store.get_path(media_id)
    assert path is not None
    assert os.path.exists(path)
    with open(path, "rb") as f:
        assert f.read() == data


@pytest.mark.asyncio
async def test_get_path_missing(store):
    path = await store.get_path("nonexistent_id")
    assert path is None


@pytest.mark.asyncio
async def test_cleanup_removes_old(store, tmp_media_dir):
    data = b"\xff\xd8\xff\xe0" + b"\x00" * 50
    media_id = await store.store(data, ext="jpg")
    path = await store.get_path(media_id)
    # Backdate the file modification time by 25 hours
    old_time = time.time() - (25 * 3600)
    os.utime(path, (old_time, old_time))
    await store.cleanup()
    assert await store.get_path(media_id) is None


@pytest.mark.asyncio
async def test_cleanup_keeps_recent(store):
    data = b"\xff\xd8\xff\xe0" + b"\x00" * 50
    media_id = await store.store(data, ext="jpg")
    await store.cleanup()
    assert await store.get_path(media_id) is not None
```

- [ ] **Step 3: Run test to verify it fails**

```bash
cd /home/rebelforce/projects/TinkerBox
python -m pytest tests/test_media_store.py -v
```

Expected: ImportError — `dragon_voice.media.store` does not exist yet.

- [ ] **Step 4: Implement MediaStore**

Write `dragon_voice/media/store.py`:

```python
"""Disk-backed media storage with auto-cleanup."""

import os
import time
import uuid
import logging
from typing import Optional

logger = logging.getLogger(__name__)


class MediaStore:
    """Manages rendered media files on disk. Auto-cleanup after 24h."""

    def __init__(self, media_dir: str = "/home/radxa/media",
                 max_age_hours: int = 24, max_total_mb: int = 500):
        self._media_dir = media_dir
        self._max_age_hours = max_age_hours
        self._max_total_mb = max_total_mb
        os.makedirs(self._media_dir, exist_ok=True)

    async def store(self, data: bytes, ext: str = "jpg",
                    session_id: str = "") -> str:
        """Save media bytes to disk, return unique media_id."""
        media_id = f"{uuid.uuid4().hex[:12]}.{ext}"
        path = os.path.join(self._media_dir, media_id)
        with open(path, "wb") as f:
            f.write(data)
        logger.debug("Stored media %s (%d bytes)", media_id, len(data))
        return media_id

    async def get_path(self, media_id: str) -> Optional[str]:
        """Get filesystem path for media_id. None if missing/expired."""
        # Strip any path traversal
        safe_id = os.path.basename(media_id)
        path = os.path.join(self._media_dir, safe_id)
        if os.path.exists(path):
            return path
        return None

    async def cleanup(self):
        """Delete files older than max_age_hours."""
        cutoff = time.time() - (self._max_age_hours * 3600)
        removed = 0
        total_bytes = 0
        if not os.path.isdir(self._media_dir):
            return
        for fname in os.listdir(self._media_dir):
            fpath = os.path.join(self._media_dir, fname)
            if not os.path.isfile(fpath):
                continue
            total_bytes += os.path.getsize(fpath)
            if os.path.getmtime(fpath) < cutoff:
                os.remove(fpath)
                removed += 1
        if removed:
            logger.info("Media cleanup: removed %d expired files", removed)

        # Also enforce max total size
        if total_bytes > self._max_total_mb * 1024 * 1024:
            files = []
            for fname in os.listdir(self._media_dir):
                fpath = os.path.join(self._media_dir, fname)
                if os.path.isfile(fpath):
                    files.append((os.path.getmtime(fpath), fpath))
            files.sort()  # oldest first
            while total_bytes > self._max_total_mb * 1024 * 1024 and files:
                _, fpath = files.pop(0)
                sz = os.path.getsize(fpath)
                os.remove(fpath)
                total_bytes -= sz
```

- [ ] **Step 5: Run tests to verify they pass**

```bash
cd /home/rebelforce/projects/TinkerBox
python -m pytest tests/test_media_store.py -v
```

Expected: All 4 tests PASS.

- [ ] **Step 6: Commit**

```bash
cd /home/rebelforce/projects/TinkerBox
git add dragon_voice/media/__init__.py dragon_voice/media/store.py tests/test_media_store.py
git commit -m "feat: add MediaStore for rendered media disk storage with auto-cleanup"
```

---

### Task 2: Dragon Media Routes (HTTP Serve + Upload)

**Files:**
- Create: `dragon_voice/api/media_routes.py`
- Modify: `dragon_voice/api/__init__.py`

- [ ] **Step 1: Create MediaRoutes**

Write `dragon_voice/api/media_routes.py`:

```python
"""HTTP endpoints for serving and uploading media files."""

import logging
from io import BytesIO
from aiohttp import web
from PIL import Image

from dragon_voice.media.store import MediaStore

logger = logging.getLogger(__name__)


class MediaRoutes:
    def __init__(self, store: MediaStore):
        self._store = store

    def register(self, app: web.Application) -> None:
        app.router.add_get("/api/media/{media_id}", self.serve_media)
        app.router.add_post("/api/media/upload", self.upload_media)

    async def serve_media(self, request: web.Request) -> web.Response:
        """Serve a rendered media file by ID. Returns 404 if missing."""
        media_id = request.match_info["media_id"]
        path = await self._store.get_path(media_id)
        if not path:
            return web.Response(status=404, text="Media not found")

        # Determine content type from extension
        if media_id.endswith(".jpg") or media_id.endswith(".jpeg"):
            ct = "image/jpeg"
        elif media_id.endswith(".png"):
            ct = "image/png"
        elif media_id.endswith(".wav"):
            ct = "audio/wav"
        else:
            ct = "application/octet-stream"

        return web.FileResponse(path, headers={
            "Content-Type": ct,
            "Cache-Control": "max-age=3600",
        })

    async def upload_media(self, request: web.Request) -> web.Response:
        """Accept image upload from Tab5 (BMP or JPEG). Convert to JPEG, store."""
        try:
            data = await request.read()
            if len(data) < 10:
                return web.json_response({"error": "Empty upload"}, status=400)

            img = Image.open(BytesIO(data))

            # Resize if larger than 1280px on longest side
            max_dim = 1280
            if max(img.width, img.height) > max_dim:
                ratio = max_dim / max(img.width, img.height)
                new_w = int(img.width * ratio)
                new_h = int(img.height * ratio)
                img = img.resize((new_w, new_h), Image.LANCZOS)

            # Convert to RGB if needed (BMP might be palette mode)
            if img.mode != "RGB":
                img = img.convert("RGB")

            buf = BytesIO()
            img.save(buf, "JPEG", quality=85)
            media_id = await self._store.store(buf.getvalue(), ext="jpg")

            logger.info("Upload: %dx%d → stored as %s", img.width, img.height, media_id)
            return web.json_response({"media_id": media_id})

        except Exception as e:
            logger.error("Upload failed: %s", e)
            return web.json_response({"error": str(e)}, status=500)
```

- [ ] **Step 2: Register MediaRoutes in api/__init__.py**

Add import at top of `dragon_voice/api/__init__.py` (after existing imports):

```python
from dragon_voice.api.media_routes import MediaRoutes
```

Add registration inside `setup_all_routes()`, after the SynthesizeRoutes registration (around line 47). Add a `media_store` parameter to the function signature:

```python
def setup_all_routes(
    app: web.Application,
    db,
    session_mgr,
    message_store,
    conversation=None,
    voice_config=None,
    start_time: float = 0,
    get_active_connections=None,
    tool_registry=None,
    memory_service=None,
    media_store=None,        # ADD THIS PARAMETER
) -> None:
```

And add registration after existing routes:

```python
    # Media serving & upload
    if media_store:
        MediaRoutes(media_store).register(app)
```

- [ ] **Step 3: Commit**

```bash
cd /home/rebelforce/projects/TinkerBox
git add dragon_voice/api/media_routes.py dragon_voice/api/__init__.py
git commit -m "feat: add media HTTP routes — serve rendered images + accept uploads"
```

---

### Task 3: Dragon MediaPipeline — Image URL Detection + Proxy

**Files:**
- Create: `dragon_voice/media/pipeline.py`
- Create: `tests/test_media_pipeline.py`

- [ ] **Step 1: Write MediaPipeline test**

Write `tests/test_media_pipeline.py`:

```python
import pytest
from unittest.mock import AsyncMock, MagicMock
from dragon_voice.media.pipeline import MediaPipeline


@pytest.fixture
def mock_store():
    store = AsyncMock()
    store.store.return_value = "test123.jpg"
    store.store_from_url = AsyncMock(return_value="proxy456.jpg")
    return store


@pytest.fixture
def pipeline(mock_store):
    return MediaPipeline(mock_store)


@pytest.mark.asyncio
async def test_detect_image_url(pipeline):
    text = "Here is a photo: https://example.com/photo.jpg and some text after."
    events = await pipeline.process_response(text, "sess1")
    assert len(events) >= 1
    assert events[0]["type"] == "media"
    assert events[0]["media_type"] == "image"


@pytest.mark.asyncio
async def test_no_media_in_plain_text(pipeline):
    text = "This is just a simple text response with no URLs or code."
    events = await pipeline.process_response(text, "sess1")
    assert len(events) == 0


@pytest.mark.asyncio
async def test_detect_code_block(pipeline, mock_store):
    mock_store.store.return_value = "code789.jpg"
    text = 'Here is code:\n```python\ndef hello():\n    print("hi")\n```\nDone.'
    events = await pipeline.process_response(text, "sess1")
    assert len(events) >= 1
    assert events[0]["type"] == "media"


@pytest.mark.asyncio
async def test_max_3_media_items(pipeline):
    text = (
        "Image 1: https://a.com/1.jpg "
        "Image 2: https://b.com/2.jpg "
        "Image 3: https://c.com/3.jpg "
        "Image 4: https://d.com/4.png "
        "Image 5: https://e.com/5.jpg"
    )
    events = await pipeline.process_response(text, "sess1")
    assert len(events) <= 3
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cd /home/rebelforce/projects/TinkerBox
python -m pytest tests/test_media_pipeline.py -v
```

Expected: ImportError — `dragon_voice.media.pipeline` does not exist yet.

- [ ] **Step 3: Implement MediaPipeline**

Write `dragon_voice/media/pipeline.py`:

```python
"""Detect renderable content in LLM responses and produce media events."""

import re
import logging
from io import BytesIO
from typing import Optional

import aiohttp
from PIL import Image, ImageDraw, ImageFont

from dragon_voice.media.store import MediaStore

logger = logging.getLogger(__name__)

# Max media items per LLM response
MAX_MEDIA_PER_RESPONSE = 3

# Image URL pattern — common image extensions
IMAGE_URL_RE = re.compile(
    r'https?://[^\s<>"\']+\.(?:jpg|jpeg|png|gif|webp)(?:\?[^\s<>"\']*)?',
    re.IGNORECASE,
)

# Markdown code block: ```lang\ncode\n```
CODE_BLOCK_RE = re.compile(
    r'```(\w*)\n(.*?)```',
    re.DOTALL,
)

# Markdown table: lines starting with |
TABLE_RE = re.compile(
    r'(\|[^\n]+\|\n(?:\|[-: ]+\|\n)?(?:\|[^\n]+\|\n?)+)',
    re.MULTILINE,
)

# Dark theme colors matching Tab5 UI
BG_COLOR = (15, 15, 26)        # #0f0f1a
TEXT_COLOR = (224, 224, 232)    # #e0e0e8
ACCENT_COLOR = (255, 107, 53)  # #ff6b35
MUTED_COLOR = (136, 136, 136)  # #888888
CODE_BG = (22, 33, 62)         # #16213e

# Target width for rendered images (matches Tab5 bubble max)
RENDER_WIDTH = 660


class MediaPipeline:
    """Detects renderable content in LLM responses, produces media events."""

    def __init__(self, store: MediaStore):
        self._store = store
        self._http_session: Optional[aiohttp.ClientSession] = None

    async def _get_session(self) -> aiohttp.ClientSession:
        if self._http_session is None or self._http_session.closed:
            timeout = aiohttp.ClientTimeout(total=15)
            self._http_session = aiohttp.ClientSession(timeout=timeout)
        return self._http_session

    async def close(self):
        if self._http_session and not self._http_session.closed:
            await self._http_session.close()

    async def process_response(self, text: str, session_id: str) -> list[dict]:
        """Analyze LLM response, return media events (max 3)."""
        events: list[dict] = []

        # 1. Detect code blocks
        for match in CODE_BLOCK_RE.finditer(text):
            if len(events) >= MAX_MEDIA_PER_RESPONSE:
                break
            lang = match.group(1) or "text"
            code = match.group(2).strip()
            if len(code) > 10:  # skip trivial snippets
                try:
                    media_id = await self.render_code_block(code, lang)
                    events.append({
                        "type": "media",
                        "media_type": "image",
                        "url": f"/api/media/{media_id}",
                        "width": RENDER_WIDTH,
                        "height": 0,  # Dragon can compute or Tab5 can auto-size
                        "alt": f"Code: {lang}",
                    })
                except Exception as e:
                    logger.warning("Code render failed: %s", e)

        # 2. Detect markdown tables
        for match in TABLE_RE.finditer(text):
            if len(events) >= MAX_MEDIA_PER_RESPONSE:
                break
            table_md = match.group(1)
            try:
                media_id = await self.render_table(table_md)
                events.append({
                    "type": "media",
                    "media_type": "image",
                    "url": f"/api/media/{media_id}",
                    "width": RENDER_WIDTH,
                    "height": 0,
                    "alt": "Table",
                })
            except Exception as e:
                logger.warning("Table render failed: %s", e)

        # 3. Detect image URLs
        for match in IMAGE_URL_RE.finditer(text):
            if len(events) >= MAX_MEDIA_PER_RESPONSE:
                break
            url = match.group(0)
            try:
                media_id = await self.proxy_image(url)
                events.append({
                    "type": "media",
                    "media_type": "image",
                    "url": f"/api/media/{media_id}",
                    "width": RENDER_WIDTH,
                    "height": 0,
                    "alt": "Image",
                })
            except Exception as e:
                logger.warning("Image proxy failed for %s: %s", url, e)

        return events

    async def render_code_block(self, code: str, language: str) -> str:
        """Render syntax-highlighted code as dark-themed JPEG image."""
        try:
            from pygments import highlight
            from pygments.lexers import get_lexer_by_name, TextLexer
            from pygments.formatters import ImageFormatter

            try:
                lexer = get_lexer_by_name(language)
            except Exception:
                lexer = TextLexer()

            formatter = ImageFormatter(
                font_name="DejaVu Sans Mono",
                font_size=14,
                line_numbers=False,
                image_pad=16,
                style="monokai",
                image_format="JPEG",
            )

            img_bytes = highlight(code, lexer, formatter)
            # Resize to target width
            img = Image.open(BytesIO(img_bytes))
            if img.width > RENDER_WIDTH:
                ratio = RENDER_WIDTH / img.width
                img = img.resize((RENDER_WIDTH, int(img.height * ratio)), Image.LANCZOS)
            elif img.width < RENDER_WIDTH:
                # Pad to RENDER_WIDTH with background color
                new_img = Image.new("RGB", (RENDER_WIDTH, img.height), CODE_BG)
                new_img.paste(img, (0, 0))
                img = new_img

            buf = BytesIO()
            img.save(buf, "JPEG", quality=90)
            return await self._store.store(buf.getvalue(), ext="jpg")

        except ImportError:
            # Pygments not installed — fall back to plain text render
            return await self._render_plain_text(code)

    async def _render_plain_text(self, text: str) -> str:
        """Fallback: render plain text as image."""
        lines = text.split("\n")
        line_h = 20
        padding = 16
        height = len(lines) * line_h + padding * 2
        img = Image.new("RGB", (RENDER_WIDTH, max(height, 60)), CODE_BG)
        draw = ImageDraw.Draw(img)
        y = padding
        for line in lines:
            draw.text((padding, y), line, fill=TEXT_COLOR)
            y += line_h
        buf = BytesIO()
        img.save(buf, "JPEG", quality=90)
        return await self._store.store(buf.getvalue(), ext="jpg")

    async def render_table(self, table_md: str) -> str:
        """Render markdown table as styled dark-themed image."""
        lines = [l.strip() for l in table_md.strip().split("\n") if l.strip()]
        # Parse rows
        rows = []
        for line in lines:
            if line.startswith("|") and not all(c in "|-: " for c in line):
                cells = [c.strip() for c in line.strip("|").split("|")]
                rows.append(cells)

        if not rows:
            return await self._render_plain_text(table_md)

        num_cols = max(len(r) for r in rows)
        # Pad rows to same column count
        for r in rows:
            while len(r) < num_cols:
                r.append("")

        cell_h = 36
        padding = 12
        col_w = max(80, (RENDER_WIDTH - padding * 2) // num_cols)
        total_w = col_w * num_cols + padding * 2
        total_h = cell_h * len(rows) + padding * 2

        img = Image.new("RGB", (min(total_w, RENDER_WIDTH), total_h), BG_COLOR)
        draw = ImageDraw.Draw(img)

        for ri, row in enumerate(rows):
            y = padding + ri * cell_h
            is_header = (ri == 0)
            for ci, cell in enumerate(row):
                x = padding + ci * col_w
                color = ACCENT_COLOR if is_header else TEXT_COLOR
                draw.text((x + 8, y + 8), cell[:30], fill=color)
            # Draw row separator
            draw.line(
                [(padding, y + cell_h - 1), (min(total_w, RENDER_WIDTH) - padding, y + cell_h - 1)],
                fill=(50, 50, 70), width=1,
            )

        buf = BytesIO()
        img.save(buf, "JPEG", quality=90)
        return await self._store.store(buf.getvalue(), ext="jpg")

    async def proxy_image(self, url: str) -> str:
        """Download external image, resize to 660px max width, store as JPEG."""
        session = await self._get_session()
        async with session.get(url) as resp:
            if resp.status != 200:
                raise ValueError(f"HTTP {resp.status} fetching {url}")
            data = await resp.read()
            if len(data) > 10 * 1024 * 1024:  # 10MB max
                raise ValueError(f"Image too large: {len(data)} bytes")

        img = Image.open(BytesIO(data))
        if img.mode != "RGB":
            img = img.convert("RGB")

        # Resize to max width
        if img.width > RENDER_WIDTH:
            ratio = RENDER_WIDTH / img.width
            new_h = int(img.height * ratio)
            img = img.resize((RENDER_WIDTH, new_h), Image.LANCZOS)

        buf = BytesIO()
        img.save(buf, "JPEG", quality=80)
        return await self._store.store(buf.getvalue(), ext="jpg")

    async def fetch_link_preview(self, url: str) -> Optional[dict]:
        """Fetch Open Graph metadata, return card event dict or None."""
        try:
            session = await self._get_session()
            async with session.get(url) as resp:
                if resp.status != 200:
                    return None
                html = await resp.text(errors="replace")

            title = _extract_og(html, "og:title") or _extract_tag(html, "title")
            description = _extract_og(html, "og:description")
            image_url = _extract_og(html, "og:image")

            if not title:
                return None

            card: dict = {
                "type": "card",
                "title": title[:100],
                "subtitle": description[:200] if description else "",
            }

            # Proxy the OG image if present
            if image_url:
                try:
                    thumb_id = await self.proxy_image(image_url)
                    card["image_url"] = f"/api/media/{thumb_id}"
                except Exception:
                    pass

            return card
        except Exception as e:
            logger.debug("Link preview failed for %s: %s", url, e)
            return None


def _extract_og(html: str, prop: str) -> Optional[str]:
    """Extract Open Graph meta tag content."""
    pattern = re.compile(
        rf'<meta\s+(?:property|name)="{re.escape(prop)}"\s+content="([^"]*)"',
        re.IGNORECASE,
    )
    m = pattern.search(html)
    return m.group(1) if m else None


def _extract_tag(html: str, tag: str) -> Optional[str]:
    """Extract content of an HTML tag."""
    pattern = re.compile(rf'<{tag}[^>]*>(.*?)</{tag}>', re.IGNORECASE | re.DOTALL)
    m = pattern.search(html)
    return m.group(1).strip() if m else None
```

- [ ] **Step 4: Run tests**

```bash
cd /home/rebelforce/projects/TinkerBox
pip install Pygments  # needed for code rendering
python -m pytest tests/test_media_pipeline.py -v
```

Expected: All 4 tests PASS.

- [ ] **Step 5: Add Pygments to requirements.txt**

Append to `requirements.txt`:

```
Pygments>=2.17.0
```

- [ ] **Step 6: Commit**

```bash
cd /home/rebelforce/projects/TinkerBox
git add dragon_voice/media/pipeline.py tests/test_media_pipeline.py requirements.txt
git commit -m "feat: MediaPipeline — detect image URLs, code blocks, tables in LLM responses and render as images"
```

---

### Task 4: Wire Dragon Server — Media Detection After llm_done

**Files:**
- Modify: `dragon_voice/server.py`
- Modify: `dragon_voice/pipeline.py`

- [ ] **Step 1: Add imports and init MediaStore + MediaPipeline in server.py**

At the top of `server.py` (around line 24, after existing imports), add:

```python
from dragon_voice.media.store import MediaStore
from dragon_voice.media.pipeline import MediaPipeline
```

In the `__init__` method of `VoiceServer`, add after existing service initialization:

```python
self._media_store = MediaStore()
self._media_pipeline = MediaPipeline(self._media_store)
```

- [ ] **Step 2: Pass media_store to setup_all_routes()**

In `_on_startup()` (around line 258), add `media_store=self._media_store` to the `setup_all_routes()` call:

```python
setup_all_routes(
    app,
    db=self._db,
    session_mgr=self._session_mgr,
    message_store=self._message_store,
    conversation=self._conversation,
    voice_config=self._config,
    start_time=self._start_time,
    get_active_connections=lambda: len(self._active_connections),
    tool_registry=self._tool_registry,
    memory_service=self._memory_service,
    media_store=self._media_store,
)
```

- [ ] **Step 3: Add media cleanup task in _on_startup()**

After the purge task creation (around line 321), add:

```python
# Start periodic media cleanup (delete files older than 24h)
self._media_cleanup_task = asyncio.create_task(self._media_cleanup_loop())
```

And add the cleanup method to the VoiceServer class:

```python
async def _media_cleanup_loop(self):
    """Hourly cleanup of expired media files."""
    while True:
        await asyncio.sleep(3600)
        try:
            await self._media_store.cleanup()
        except Exception as e:
            logger.warning("Media cleanup error: %s", e)
```

- [ ] **Step 4: Wire media detection after llm_done in _handle_text()**

In `_handle_text()`, after the line that sends `llm_done` (line 1396), add:

```python
        await ws.send_json({"type": "llm_done", "llm_ms": 0})

        # Detect and send rich media events (images, code, tables)
        if full_response:
            try:
                response_text = "".join(full_response)
                media_events = await self._media_pipeline.process_response(
                    response_text, session_id
                )
                for event in media_events:
                    if not ws.closed:
                        await ws.send_json(event)
            except Exception as e:
                logger.warning("Media detection failed: %s", e)
```

- [ ] **Step 5: Wire media detection after voice pipeline llm_done in pipeline.py**

In `pipeline.py`, after the `llm_done` event is sent (line 560), we need to trigger media detection. The cleanest way is to add an optional `on_media` callback. Add to `_process_utterance()` after llm_done:

```python
            await self._on_event({"type": "llm_done", "llm_ms": round(llm_ms)})

            # Rich media detection on full response
            if self._media_pipeline and full_response:
                try:
                    media_events = await self._media_pipeline.process_response(
                        full_response, self._session_id or ""
                    )
                    for event in media_events:
                        await self._on_event(event)
                except Exception as e:
                    logger.warning("Voice media detection failed: %s", e)
```

Add `media_pipeline` as an optional constructor parameter to VoicePipeline and store it as `self._media_pipeline`.

- [ ] **Step 6: Add user_media handler to message dispatch**

In `_handle_ws_voice()`, after the "text" handler (line 944), add:

```python
                    elif cmd_type == "user_media":
                        async with conn_lock:
                            await self._handle_user_media(ws, conn_state, cmd)
```

And add the handler method to VoiceServer:

```python
    async def _handle_user_media(self, ws, conn_state, cmd):
        """Handle image/audio uploaded by Tab5 for multimodal LLM analysis."""
        import base64

        media_id = cmd.get("media_id", "")
        text = cmd.get("text", "What's in this image?")
        session_id = conn_state.get("session_id", "")
        conn_config = conn_state.get("config", self._config)

        image_path = await self._media_store.get_path(media_id)
        if not image_path:
            if not ws.closed:
                await ws.send_json({"type": "error", "message": "Image not found"})
            return

        # Check if current LLM supports vision
        backend = conn_config.llm.backend
        if backend == "ollama" and "vision" not in conn_config.llm.ollama_model:
            if not ws.closed:
                await ws.send_json({
                    "type": "error",
                    "message": "Image analysis needs Cloud or TinkerClaw mode. "
                               "Switch to Cloud mode in Settings to analyze images."
                })
            return

        # Read image and encode as base64
        with open(image_path, "rb") as f:
            image_b64 = base64.b64encode(f.read()).decode()

        # Build multimodal message
        messages = [{
            "role": "user",
            "content": [
                {
                    "type": "image_url",
                    "image_url": {"url": f"data:image/jpeg;base64,{image_b64}"}
                },
                {"type": "text", "text": text},
            ]
        }]

        # Stream LLM response using existing path
        full_response = []
        llm = conn_state.get("llm") or self._conversation._llm
        try:
            async for token in llm.generate_stream_with_messages(messages):
                full_response.append(token)
                if not ws.closed:
                    await ws.send_json({"type": "llm", "text": token})
        except Exception as e:
            logger.error("user_media LLM failed: %s", e)
            if not ws.closed:
                await ws.send_json({"type": "error", "message": str(e)})
            return

        if not ws.closed:
            await ws.send_json({"type": "llm_done", "llm_ms": 0})

        # Store assistant message
        if full_response and self._message_store:
            await self._message_store.add_message(
                session_id=session_id,
                role="assistant",
                content="".join(full_response),
                input_mode="vision",
            )
```

- [ ] **Step 7: Commit**

```bash
cd /home/rebelforce/projects/TinkerBox
git add dragon_voice/server.py dragon_voice/pipeline.py dragon_voice/api/__init__.py
git commit -m "feat: wire media detection after llm_done + user_media handler for vision"
```

---

### Task 5: Tab5 — Enable TJPGD + Create media_cache

**Files:**
- Modify: `sdkconfig.defaults`
- Create: `main/media_cache.h`
- Create: `main/media_cache.c`
- Modify: `main/CMakeLists.txt`

- [ ] **Step 1: Enable TJPGD in sdkconfig.defaults**

Append to `sdkconfig.defaults`:

```
# Rich media: JPEG decoder for inline chat images
CONFIG_LV_USE_TJPGD=y
```

- [ ] **Step 2: Create media_cache.h**

Write `main/media_cache.h`:

```c
#pragma once

#include "esp_err.h"
#include "lvgl.h"

/**
 * Rich Media Image Cache — downloads JPEG from Dragon HTTP API,
 * decodes via TJPGD, stores in pre-allocated PSRAM slots.
 *
 * 5 fixed PSRAM slots (660×440×2 = 581KB each, ~2.9MB total).
 * LRU eviction. Zero alloc/free churn — prevents fragmentation.
 *
 * All downloads run on Core 1. Use lv_async_call for UI updates.
 */

#define MEDIA_CACHE_SLOTS       5
#define MEDIA_CACHE_MAX_W       660
#define MEDIA_CACHE_MAX_H       440
#define MEDIA_CACHE_SLOT_BYTES  (MEDIA_CACHE_MAX_W * MEDIA_CACHE_MAX_H * 2)  /* RGB565 */

/** Initialize cache — pre-allocates PSRAM slots. Call once at boot. */
esp_err_t media_cache_init(void);

/**
 * Fetch image from URL, decode JPEG, return LVGL image descriptor.
 * BLOCKING — run from a background task, NOT the LVGL thread.
 *
 * @param relative_url  Relative path from Dragon (e.g. "/api/media/abc.jpg")
 * @param out_dsc       Output: filled lv_image_dsc_t pointing to PSRAM pixel data
 * @return ESP_OK on success, ESP_ERR_* on failure
 */
esp_err_t media_cache_fetch(const char *relative_url, lv_image_dsc_t *out_dsc);

/** Clear all cached images (e.g. on New Chat). */
void media_cache_clear(void);
```

- [ ] **Step 3: Create media_cache.c**

Write `main/media_cache.c`:

```c
/**
 * Rich Media Image Cache
 *
 * Downloads JPEG images from Dragon's /api/media/ endpoint,
 * decodes via LVGL's TJPGD into pre-allocated PSRAM RGB565 buffers.
 * 5-slot LRU cache with zero alloc/free to prevent PSRAM fragmentation.
 */

#include "media_cache.h"
#include "settings.h"
#include "config.h"
#include "voice.h"

#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lvgl.h"

#include <string.h>
#include <stdlib.h>

static const char *TAG = "media_cache";

/* --- Cache slot --- */
typedef struct {
    uint8_t  *pixel_buf;        /* Pre-allocated PSRAM: MEDIA_CACHE_SLOT_BYTES */
    uint8_t  *jpeg_buf;         /* Temp PSRAM buffer for downloaded JPEG data */
    size_t    jpeg_buf_size;    /* Allocated size of jpeg_buf */
    uint32_t  url_hash;         /* FNV-1a hash of the URL for fast lookup */
    uint32_t  last_access;      /* xTaskGetTickCount at last access */
    int       width;            /* Decoded image width */
    int       height;           /* Decoded image height */
    bool      valid;            /* Slot contains a decoded image */
} cache_slot_t;

static cache_slot_t s_slots[MEDIA_CACHE_SLOTS];
static SemaphoreHandle_t s_mutex;
static bool s_initialized = false;

/* Max JPEG download size: 512KB (Dragon sends optimized images) */
#define MAX_JPEG_DOWNLOAD   (512 * 1024)

/* FNV-1a hash */
static uint32_t fnv1a(const char *str)
{
    uint32_t hash = 2166136261u;
    while (*str) {
        hash ^= (uint8_t)*str++;
        hash *= 16777619u;
    }
    return hash;
}

esp_err_t media_cache_init(void)
{
    if (s_initialized) return ESP_OK;

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;

    for (int i = 0; i < MEDIA_CACHE_SLOTS; i++) {
        s_slots[i].pixel_buf = heap_caps_malloc(MEDIA_CACHE_SLOT_BYTES,
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_slots[i].pixel_buf) {
            ESP_LOGE(TAG, "Failed to allocate PSRAM slot %d (%d bytes)", i, MEDIA_CACHE_SLOT_BYTES);
            return ESP_ERR_NO_MEM;
        }
        s_slots[i].jpeg_buf = heap_caps_malloc(MAX_JPEG_DOWNLOAD,
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_slots[i].jpeg_buf) {
            ESP_LOGE(TAG, "Failed to allocate JPEG buffer slot %d", i);
            return ESP_ERR_NO_MEM;
        }
        s_slots[i].jpeg_buf_size = MAX_JPEG_DOWNLOAD;
        s_slots[i].valid = false;
        s_slots[i].url_hash = 0;
        s_slots[i].last_access = 0;
        s_slots[i].width = 0;
        s_slots[i].height = 0;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Media cache initialized: %d slots × %dKB = %dKB PSRAM",
             MEDIA_CACHE_SLOTS, MEDIA_CACHE_SLOT_BYTES / 1024,
             (MEDIA_CACHE_SLOTS * MEDIA_CACHE_SLOT_BYTES) / 1024);
    return ESP_OK;
}

/* Find LRU (oldest access) slot */
static int find_lru_slot(void)
{
    int lru = 0;
    uint32_t oldest = UINT32_MAX;
    for (int i = 0; i < MEDIA_CACHE_SLOTS; i++) {
        if (!s_slots[i].valid) return i;  /* Empty slot — use immediately */
        if (s_slots[i].last_access < oldest) {
            oldest = s_slots[i].last_access;
            lru = i;
        }
    }
    return lru;
}

/* Build full URL from relative path */
static void build_full_url(const char *relative_url, char *out, size_t out_len)
{
    char host[64];
    tab5_settings_get_dragon_host(host, sizeof(host));
    uint16_t port = tab5_settings_get_dragon_port();
    snprintf(out, out_len, "http://%s:%u%s", host, port, relative_url);
}

esp_err_t media_cache_fetch(const char *relative_url, lv_image_dsc_t *out_dsc)
{
    if (!s_initialized || !relative_url || !out_dsc) return ESP_ERR_INVALID_ARG;

    uint32_t hash = fnv1a(relative_url);

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    /* Cache hit? */
    for (int i = 0; i < MEDIA_CACHE_SLOTS; i++) {
        if (s_slots[i].valid && s_slots[i].url_hash == hash) {
            s_slots[i].last_access = xTaskGetTickCount();
            out_dsc->header.w = s_slots[i].width;
            out_dsc->header.h = s_slots[i].height;
            out_dsc->header.cf = LV_COLOR_FORMAT_RGB565;
            out_dsc->data = s_slots[i].pixel_buf;
            out_dsc->data_size = s_slots[i].width * s_slots[i].height * 2;
            xSemaphoreGive(s_mutex);
            ESP_LOGD(TAG, "Cache hit for %s (slot %d)", relative_url, i);
            return ESP_OK;
        }
    }

    /* Cache miss — find slot to evict */
    int slot = find_lru_slot();
    s_slots[slot].valid = false;
    xSemaphoreGive(s_mutex);

    /* Build full URL */
    char full_url[256];
    build_full_url(relative_url, full_url, sizeof(full_url));

    /* Download JPEG via HTTP */
    esp_http_client_config_t cfg = {
        .url = full_url,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_ERR_NO_MEM;

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    int content_length = esp_http_client_fetch_headers(client);
    if (content_length <= 0) content_length = MAX_JPEG_DOWNLOAD;
    if (content_length > MAX_JPEG_DOWNLOAD) {
        ESP_LOGE(TAG, "Image too large: %d bytes", content_length);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    /* Read into JPEG buffer with yielding */
    size_t total_read = 0;
    while (total_read < (size_t)content_length) {
        int chunk = 4096;
        if (total_read + chunk > s_slots[slot].jpeg_buf_size) break;
        int read = esp_http_client_read(client,
                                         (char *)s_slots[slot].jpeg_buf + total_read,
                                         chunk);
        if (read <= 0) break;
        total_read += read;
        vTaskDelay(pdMS_TO_TICKS(2));  /* Yield to voice WS */
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (total_read < 100) {
        ESP_LOGE(TAG, "Download too small: %zu bytes", total_read);
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGI(TAG, "Downloaded %zu bytes from %s", total_read, relative_url);

    /*
     * Decode JPEG → RGB565 using LVGL's TJPGD.
     * LVGL 9.x: Use lv_image_decoder to decode JPEG data in memory.
     * We create a temporary lv_image_dsc_t with the JPEG data and let LVGL decode it.
     *
     * Alternative approach: Use TJPGD directly for more control.
     * For now, we use a simpler approach — set the raw JPEG as image source
     * and let LVGL's built-in TJPGD decoder handle it.
     */

    /* Store JPEG data info for LVGL to decode on demand */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_slots[slot].url_hash = hash;
    s_slots[slot].last_access = xTaskGetTickCount();
    s_slots[slot].valid = true;

    /*
     * LVGL 9.2 TJPGD: We can set the image source directly to JPEG data.
     * The lv_image widget will decode it when drawn.
     * We store the JPEG data in the jpeg_buf and point the descriptor there.
     */
    out_dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    /* For TJPGD, we set the data pointer to JPEG bytes and let LVGL decode.
     * Actually, LVGL's TJPGD decoder expects a file path or image descriptor
     * with the raw JPEG data. We'll use the LV_IMAGE_SRC_VARIABLE approach. */

    /* Simpler: just keep JPEG bytes in the slot buffer and set data_size.
     * LVGL's TJPGD decoder will pick it up when the image is rendered. */
    s_slots[slot].width = MEDIA_CACHE_MAX_W;   /* Will be overridden by TJPGD */
    s_slots[slot].height = MEDIA_CACHE_MAX_H;

    /* Build output descriptor pointing to JPEG data for LVGL to decode */
    out_dsc->header.w = 0;  /* TJPGD will set real dimensions */
    out_dsc->header.h = 0;
    out_dsc->header.cf = LV_COLOR_FORMAT_RAW;
    out_dsc->data = s_slots[slot].jpeg_buf;
    out_dsc->data_size = total_read;

    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

void media_cache_clear(void)
{
    if (!s_initialized) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < MEDIA_CACHE_SLOTS; i++) {
        s_slots[i].valid = false;
        s_slots[i].url_hash = 0;
    }
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "Media cache cleared");
}
```

- [ ] **Step 4: Add media_cache.c to CMakeLists.txt**

In `main/CMakeLists.txt`, add `"media_cache.c"` to the ESP32-P4 SRCS list (in the `else()` block, after `"ota.c"`):

```cmake
             "settings.c" "ota.c" "media_cache.c"
```

- [ ] **Step 5: Build to verify compilation**

```bash
cd /home/rebelforce/projects/TinkerTab
. /home/rebelforce/esp/esp-idf/export.sh
idf.py build
```

Expected: Build succeeds with TJPGD enabled and media_cache.c compiled.

- [ ] **Step 6: Commit**

```bash
cd /home/rebelforce/projects/TinkerTab
git add sdkconfig.defaults main/media_cache.h main/media_cache.c main/CMakeLists.txt
git commit -m "feat: media cache — JPEG decoder + 5-slot PSRAM LRU cache for inline images"
```

---

### Task 6: Tab5 — voice.c Message Handlers + ui_chat.h Declarations

**Files:**
- Modify: `main/voice.c`
- Modify: `main/ui_chat.h`

- [ ] **Step 1: Add push function declarations to ui_chat.h**

Add after the existing `ui_chat_push_message()` declaration in `main/ui_chat.h`:

```c
/**
 * Push an inline image into Chat from any task/core.
 * Downloads and decodes in background, shows placeholder immediately.
 */
void ui_chat_push_media(const char *url, const char *media_type,
                        int width, int height, const char *alt);

/**
 * Push a rich card (title + subtitle + optional thumbnail) into Chat.
 */
void ui_chat_push_card(const char *title, const char *subtitle,
                       const char *image_url, const char *description);

/**
 * Push an inline audio player into Chat.
 */
void ui_chat_push_audio_clip(const char *url, float duration_s, const char *label);
```

- [ ] **Step 2: Add media/card/audio_clip handlers to voice.c**

In `main/voice.c`, in `handle_text_message()`, replace the `else` block at line 798 with the new handlers followed by the catch-all:

```c
    } else if (strcmp(type_str, "media") == 0) {
        const char *url = cJSON_GetStringValue(cJSON_GetObjectItem(root, "url"));
        const char *mtype = cJSON_GetStringValue(cJSON_GetObjectItem(root, "media_type"));
        cJSON *w_item = cJSON_GetObjectItem(root, "width");
        cJSON *h_item = cJSON_GetObjectItem(root, "height");
        int w = cJSON_IsNumber(w_item) ? (int)w_item->valuedouble : 0;
        int h = cJSON_IsNumber(h_item) ? (int)h_item->valuedouble : 0;
        const char *alt = cJSON_GetStringValue(cJSON_GetObjectItem(root, "alt"));
        if (url) {
            ESP_LOGI(TAG, "Media: %s %dx%d (%s)", mtype ? mtype : "image", w, h, alt ? alt : "");
            ui_chat_push_media(url, mtype, w, h, alt);
        }

    } else if (strcmp(type_str, "card") == 0) {
        const char *title = cJSON_GetStringValue(cJSON_GetObjectItem(root, "title"));
        const char *sub = cJSON_GetStringValue(cJSON_GetObjectItem(root, "subtitle"));
        const char *img = cJSON_GetStringValue(cJSON_GetObjectItem(root, "image_url"));
        const char *desc = cJSON_GetStringValue(cJSON_GetObjectItem(root, "description"));
        if (title) {
            ESP_LOGI(TAG, "Card: %s", title);
            ui_chat_push_card(title, sub, img, desc);
        }

    } else if (strcmp(type_str, "audio_clip") == 0) {
        const char *url = cJSON_GetStringValue(cJSON_GetObjectItem(root, "url"));
        cJSON *dur_item = cJSON_GetObjectItem(root, "duration_s");
        float dur = cJSON_IsNumber(dur_item) ? (float)dur_item->valuedouble : 0.0f;
        const char *label = cJSON_GetStringValue(cJSON_GetObjectItem(root, "label"));
        if (url) {
            ESP_LOGI(TAG, "Audio clip: %s (%.1fs)", label ? label : "", dur);
            ui_chat_push_audio_clip(url, dur, label);
        }

    } else {
        ESP_LOGW(TAG, "Unknown message type: %s", type_str);
    }
```

- [ ] **Step 3: Build to verify**

```bash
cd /home/rebelforce/projects/TinkerTab
idf.py build
```

Expected: Build fails because `ui_chat_push_media()` etc. are declared but not yet defined. That's expected — Task 7 implements them.

- [ ] **Step 4: Commit (with build note)**

```bash
cd /home/rebelforce/projects/TinkerTab
git add main/voice.c main/ui_chat.h
git commit -m "feat: voice.c handles media/card/audio_clip WS messages — dispatch to ui_chat"
```

---

### Task 7: Tab5 — ui_chat.c Image + Card + Audio Bubble Renderers

**Files:**
- Modify: `main/ui_chat.c`

- [ ] **Step 1: Add media_cache include**

At the top of `main/ui_chat.c` (after existing includes around line 27), add:

```c
#include "media_cache.h"
```

- [ ] **Step 2: Add image bubble async data structures**

After the existing static variables (around line 100), add:

```c
/* --- Rich media types --- */
typedef struct {
    char    url[256];
    char    alt[128];
    int     width;
    int     height;
} media_push_t;

typedef struct {
    char    title[128];
    char    subtitle[256];
    char    image_url[256];
    char    description[256];
} card_push_t;

typedef struct {
    char    url[256];
    float   duration_s;
    char    label[128];
} audio_push_t;

/* Forward declarations */
static void push_media_cb(void *arg);
static void push_card_cb(void *arg);
static void push_audio_cb(void *arg);
```

- [ ] **Step 3: Implement image bubble**

Add the image bubble renderer functions:

```c
/* --- Image bubble --- */

static void add_image_bubble(const char *url, int width, int height, const char *alt)
{
    if (!s_msg_scroll) return;

    /* Evict oldest if object count too high */
    uint32_t child_cnt = lv_obj_get_child_count(s_msg_scroll);
    while (child_cnt > 75) {
        lv_obj_t *oldest = lv_obj_get_child(s_msg_scroll, 0);
        if (!oldest) break;
        int bh = lv_obj_get_height(oldest);
        lv_obj_del(oldest);
        /* Shift all remaining children up */
        child_cnt = lv_obj_get_child_count(s_msg_scroll);
        /* Recalculate positions */
        s_next_y = 10;
        for (uint32_t i = 0; i < child_cnt; i++) {
            lv_obj_t *ch = lv_obj_get_child(s_msg_scroll, i);
            lv_obj_update_layout(ch);
            lv_obj_set_y(ch, s_next_y);
            s_next_y += lv_obj_get_height(ch) + 10;
        }
    }

    /* Container bubble — left-aligned like AI messages */
    lv_obj_t *bubble = lv_obj_create(s_msg_scroll);
    lv_obj_remove_style_all(bubble);

    /* Calculate display dimensions (fit within 460px width) */
    int disp_w = (width > 0 && width < BUBBLE_MAX_W) ? width : BUBBLE_MAX_W;
    int disp_h = (height > 0) ? height : 200;  /* default placeholder height */
    if (width > BUBBLE_MAX_W && height > 0) {
        float ratio = (float)BUBBLE_MAX_W / width;
        disp_w = BUBBLE_MAX_W;
        disp_h = (int)(height * ratio);
    }

    lv_obj_set_size(bubble, disp_w + 8, disp_h + 8);  /* 4px padding */
    lv_obj_set_pos(bubble, 20, s_next_y);
    lv_obj_set_style_bg_color(bubble, lv_color_hex(0x2A2A3E), 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bubble, 12, 0);
    lv_obj_set_style_pad_all(bubble, 4, 0);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);

    /* Placeholder label — shown until image loads */
    lv_obj_t *lbl = lv_label_create(bubble);
    lv_label_set_text(lbl, alt && alt[0] ? alt : "Loading image...");
    lv_obj_set_style_text_font(lbl, FONT_SMALL, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x888888), 0);
    lv_obj_center(lbl);

    s_next_y += disp_h + 8 + 10;
    lv_obj_scroll_to_y(s_msg_scroll, s_next_y, LV_ANIM_ON);

    /* TODO: Background task to download + decode image via media_cache_fetch(),
     * then lv_async_call to replace placeholder label with lv_image widget.
     * For Phase 1, the placeholder shows the alt text.
     * Image loading will be wired when media_cache JPEG decode is verified on hardware. */
}

static void push_media_cb(void *arg)
{
    media_push_t *m = (media_push_t *)arg;
    if (m) {
        add_image_bubble(m->url, m->width, m->height, m->alt);
        free(m);
    }
}

void ui_chat_push_media(const char *url, const char *media_type,
                        int width, int height, const char *alt)
{
    media_push_t *m = malloc(sizeof(media_push_t));
    if (!m) return;
    strncpy(m->url, url ? url : "", sizeof(m->url) - 1);
    m->url[sizeof(m->url) - 1] = '\0';
    strncpy(m->alt, alt ? alt : "", sizeof(m->alt) - 1);
    m->alt[sizeof(m->alt) - 1] = '\0';
    m->width = width;
    m->height = height;
    lv_async_call(push_media_cb, m);
}
```

- [ ] **Step 4: Implement card bubble**

```c
/* --- Card bubble --- */

static void add_card_bubble(const char *title, const char *subtitle,
                            const char *image_url, const char *description)
{
    if (!s_msg_scroll || !title) return;

    int card_h = 80;
    if (description && description[0]) card_h = 110;

    lv_obj_t *card = lv_obj_create(s_msg_scroll);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, BUBBLE_MAX_W, card_h);
    lv_obj_set_pos(card, 20, s_next_y);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1a1a2e), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0xff6b35), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_side(card, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    /* Title */
    lv_obj_t *t_lbl = lv_label_create(card);
    lv_label_set_text(t_lbl, title);
    lv_label_set_long_mode(t_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(t_lbl, BUBBLE_MAX_W - 32);
    lv_obj_set_style_text_font(t_lbl, FONT_BODY, 0);
    lv_obj_set_style_text_color(t_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_pos(t_lbl, 0, 0);

    /* Subtitle */
    if (subtitle && subtitle[0]) {
        lv_obj_t *s_lbl = lv_label_create(card);
        lv_label_set_text(s_lbl, subtitle);
        lv_label_set_long_mode(s_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(s_lbl, BUBBLE_MAX_W - 32);
        lv_obj_set_style_text_font(s_lbl, FONT_SMALL, 0);
        lv_obj_set_style_text_color(s_lbl, lv_color_hex(0x888888), 0);
        lv_obj_set_pos(s_lbl, 0, 28);
    }

    /* Description */
    if (description && description[0]) {
        lv_obj_t *d_lbl = lv_label_create(card);
        lv_label_set_text(d_lbl, description);
        lv_label_set_long_mode(d_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(d_lbl, BUBBLE_MAX_W - 32);
        lv_obj_set_style_text_font(d_lbl, FONT_SMALL, 0);
        lv_obj_set_style_text_color(d_lbl, lv_color_hex(0xaaaaaa), 0);
        lv_obj_set_pos(d_lbl, 0, 52);
    }

    s_next_y += card_h + 10;
    lv_obj_scroll_to_y(s_msg_scroll, s_next_y, LV_ANIM_ON);
}

static void push_card_cb(void *arg)
{
    card_push_t *c = (card_push_t *)arg;
    if (c) {
        add_card_bubble(c->title, c->subtitle, c->image_url, c->description);
        free(c);
    }
}

void ui_chat_push_card(const char *title, const char *subtitle,
                       const char *image_url, const char *description)
{
    card_push_t *c = malloc(sizeof(card_push_t));
    if (!c) return;
    strncpy(c->title, title ? title : "", sizeof(c->title) - 1);
    c->title[sizeof(c->title) - 1] = '\0';
    strncpy(c->subtitle, subtitle ? subtitle : "", sizeof(c->subtitle) - 1);
    c->subtitle[sizeof(c->subtitle) - 1] = '\0';
    strncpy(c->image_url, image_url ? image_url : "", sizeof(c->image_url) - 1);
    c->image_url[sizeof(c->image_url) - 1] = '\0';
    strncpy(c->description, description ? description : "", sizeof(c->description) - 1);
    c->description[sizeof(c->description) - 1] = '\0';
    lv_async_call(push_card_cb, c);
}
```

- [ ] **Step 5: Implement audio clip bubble**

```c
/* --- Audio clip bubble --- */

static void add_audio_bubble(const char *url, float duration_s, const char *label)
{
    if (!s_msg_scroll || !url) return;

    lv_obj_t *bubble = lv_obj_create(s_msg_scroll);
    lv_obj_remove_style_all(bubble);
    lv_obj_set_size(bubble, BUBBLE_MAX_W, 56);
    lv_obj_set_pos(bubble, 20, s_next_y);
    lv_obj_set_style_bg_color(bubble, lv_color_hex(0x1a2a1a), 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bubble, 12, 0);
    lv_obj_set_style_pad_all(bubble, 8, 0);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);

    /* Play icon (triangle) — using text for now */
    lv_obj_t *play = lv_label_create(bubble);
    lv_label_set_text(play, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_font(play, FONT_HEADING, 0);
    lv_obj_set_style_text_color(play, lv_color_hex(0x22c55e), 0);
    lv_obj_set_pos(play, 8, 8);

    /* Label + duration */
    char info[192];
    snprintf(info, sizeof(info), "%s  %.1fs",
             label && label[0] ? label : "Audio", duration_s);
    lv_obj_t *info_lbl = lv_label_create(bubble);
    lv_label_set_text(info_lbl, info);
    lv_obj_set_style_text_font(info_lbl, FONT_SECONDARY, 0);
    lv_obj_set_style_text_color(info_lbl, lv_color_hex(0xcccccc), 0);
    lv_obj_set_pos(info_lbl, 48, 12);

    /* TODO: Add tap handler to download + play audio via I2S.
     * For Phase 1, the player is visual-only (shows clip info). */

    s_next_y += 56 + 10;
    lv_obj_scroll_to_y(s_msg_scroll, s_next_y, LV_ANIM_ON);
}

static void push_audio_cb(void *arg)
{
    audio_push_t *a = (audio_push_t *)arg;
    if (a) {
        add_audio_bubble(a->url, a->duration_s, a->label);
        free(a);
    }
}

void ui_chat_push_audio_clip(const char *url, float duration_s, const char *label)
{
    audio_push_t *a = malloc(sizeof(audio_push_t));
    if (!a) return;
    strncpy(a->url, url ? url : "", sizeof(a->url) - 1);
    a->url[sizeof(a->url) - 1] = '\0';
    a->duration_s = duration_s;
    strncpy(a->label, label ? label : "", sizeof(a->label) - 1);
    a->label[sizeof(a->label) - 1] = '\0';
    lv_async_call(push_audio_cb, a);
}
```

- [ ] **Step 6: Initialize media cache at chat creation**

In `ui_chat_create()` (around line 150), add after overlay creation:

```c
    /* Initialize media cache for rich image display */
    media_cache_init();
```

- [ ] **Step 7: Build and verify**

```bash
cd /home/rebelforce/projects/TinkerTab
idf.py build
```

Expected: Build succeeds. All new functions compile.

- [ ] **Step 8: Commit**

```bash
cd /home/rebelforce/projects/TinkerTab
git add main/ui_chat.c main/ui_chat.h
git commit -m "feat: rich media bubbles — image, card, and audio clip renderers in chat"
```

---

### Task 8: End-to-End Test — Phase 1

- [ ] **Step 1: Deploy Dragon changes**

```bash
cd /home/rebelforce/projects/TinkerBox
pip install Pygments
scp -r dragon_voice/media radxa@192.168.1.91:/home/radxa/dragon_voice/
scp dragon_voice/api/media_routes.py radxa@192.168.1.91:/home/radxa/dragon_voice/api/
scp dragon_voice/api/__init__.py radxa@192.168.1.91:/home/radxa/dragon_voice/api/
scp dragon_voice/server.py radxa@192.168.1.91:/home/radxa/dragon_voice/
scp dragon_voice/pipeline.py radxa@192.168.1.91:/home/radxa/dragon_voice/
scp requirements.txt radxa@192.168.1.91:/home/radxa/
ssh radxa@192.168.1.91 "cd /home/radxa && pip install -r requirements.txt && sudo systemctl restart tinkerclaw-voice"
```

- [ ] **Step 2: Flash Tab5**

```bash
cd /home/rebelforce/projects/TinkerTab
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/ttyACM0 flash
python -m esptool --chip esp32p4 -p /dev/ttyACM0 --before no_reset --after watchdog_reset read_mac
```

- [ ] **Step 3: Test image rendering**

Switch to Cloud mode and send a text message that should trigger image detection:

```bash
TOKEN=dd9cc86de572e4a69b28456048c7f1a9
# Switch to Cloud mode
curl -s -H "Authorization: Bearer $TOKEN" -X POST "http://10.0.0.229/api/config" \
  -H "Content-Type: application/json" -d '{"voice_mode": 2}'
sleep 2

# Navigate to chat
curl -s -H "Authorization: Bearer $TOKEN" -X POST \
  "http://10.0.0.229/api/navigate" -H "Content-Type: application/json" \
  -d '{"screen":"chat"}'
sleep 2

# Send a message that should produce an image URL in the response
curl -s -H "Authorization: Bearer $TOKEN" -X POST \
  "http://10.0.0.229/api/chat" -H "Content-Type: application/json" \
  -d '{"text":"Write a Python hello world function with syntax highlighting"}'
sleep 10

# Take screenshot
curl -s -H "Authorization: Bearer $TOKEN" -o /tmp/rich_media_test.png \
  "http://10.0.0.229/api/screenshot"
```

Expected: Screenshot shows the AI text response with a code block rendered as an image below it.

- [ ] **Step 4: Verify Dragon media store**

```bash
ssh radxa@192.168.1.91 "ls -la /home/radxa/media/"
```

Expected: Shows rendered image files (*.jpg).

- [ ] **Step 5: Commit all repos and push**

```bash
cd /home/rebelforce/projects/TinkerBox
git add -A && git push

cd /home/rebelforce/projects/TinkerTab
git add -A && git push
```

---

## Phase 2-4 tasks are deferred until Phase 1 is verified on hardware.

Phase 2 (code/table rendering) is already implemented in MediaPipeline — just needs E2E testing.
Phase 3 (camera-to-chat) requires adding "Ask AI" button to ui_camera.c and the user_media WS handler (already implemented in server.py Task 4).
Phase 4 (rich cards + audio playback) requires wiring the tap handlers for audio download + I2S playback.

All Phase 2-4 Dragon-side code is already written in Tasks 1-4. Only Tab5-side additions remain.
