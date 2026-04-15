# Rich Media Chat — Design Spec

**Date:** 2026-04-15
**Repos:** TinkerTab (firmware), TinkerBox (Dragon), TinkerClaw (no changes)
**Goal:** Transform the Tab5 chat from text-only to rich media — inline images, code blocks, tables, cards, audio clips, and camera-to-AI vision. All modes (Local, Hybrid, Cloud, TinkerClaw).

## Architecture

**Dragon renders. Tab5 displays.** One pattern for everything.

Dragon intercepts LLM responses, detects renderable content (image URLs, code blocks, tables, markdown), renders them into optimized JPEG images server-side (Pillow, Pygments), and sends them to Tab5 via new WebSocket message types. Tab5 downloads images from Dragon's HTTP API, decodes via TJPGD (software JPEG decoder), and displays them inline in chat using `lv_image`. For user-sent media, Tab5 uploads camera frames to Dragon, which forwards them to multimodal LLMs (Claude/GPT-4o vision).

Zero TinkerClaw changes. Tab5 stays a thin client. Dragon does all rendering.

---

## 1. Protocol Changes

### 1.1 New WS Message Types: Dragon → Tab5

**`media`** — Inline image rendered/proxied by Dragon:
```json
{
  "type": "media",
  "media_type": "image",
  "url": "/api/media/abc123.jpg",
  "width": 660,
  "height": 400,
  "alt": "Weather forecast"
}
```

**`card`** — Rich preview card (link preview, search result, etc.):
```json
{
  "type": "card",
  "title": "Dublin Weather",
  "subtitle": "14°C · Light Rain",
  "image_url": "/api/media/icon123.jpg",
  "description": "Rain expected until 4pm",
  "source": "met.ie"
}
```

**`audio_clip`** — Non-TTS inline audio (pronunciation, music preview):
```json
{
  "type": "audio_clip",
  "url": "/api/media/pronunciation.wav",
  "duration_s": 2.3,
  "label": "French pronunciation"
}
```

### 1.2 New WS Message Types: Tab5 → Dragon

**`user_media`** — User sends photo/audio into conversation:
```json
{
  "type": "user_media",
  "media_id": "upload_abc123",
  "media_type": "image",
  "text": "What plant is this?"
}
```

### 1.3 New REST Endpoints (Dragon port 3502)

| Method | Path | Purpose |
|--------|------|---------|
| GET | `/api/media/{id}` | Serve rendered/cached media file |
| POST | `/api/media/upload` | Tab5 uploads camera frame, returns `{"media_id":"..."}` |

### 1.4 URL Resolution

Media URLs in `media`, `card`, and `audio_clip` messages are **relative to the Dragon host**. Tab5 constructs the full URL using the configured `dragon_host` and `dragon_port` from NVS settings:

```
url field: "/api/media/abc123.jpg"
full URL:  "http://{dragon_host}:{dragon_port}/api/media/abc123.jpg"
           e.g. "http://192.168.1.91:3502/api/media/abc123.jpg"
```

This avoids hardcoding Dragon's address in the protocol and works with both LAN and ngrok connections.

### 1.5 Backward Compatibility

Old firmware ignores unknown WS message types — voice.c's strcmp dispatch falls through silently. Dragon checks `firmware_ver` from the `register` message. If firmware < v0.9.0, Dragon skips media events and sends text-only responses. All new REST endpoints are additive.

### 1.5 Media Event Timing

Media events are sent AFTER `llm_done`, not interleaved with `llm` tokens. This avoids buffering latency — text streams immediately as before. After the full response is available, Dragon runs media detection and sends `media`/`card` events. Tab5 appends them below the text bubble. Natural loading behavior — like a web page where text appears first, then images load in.

Sequence:
```
{"type":"llm","text":"Here's"} → {"type":"llm","text":" a Japanese maple:"} → ...
{"type":"llm_done","llm_ms":1523}
{"type":"media","media_type":"image","url":"/api/media/maple001.jpg","width":660,"height":440,"alt":"Japanese maple in autumn"}
```

---

## 2. Tab5 Firmware Changes

### 2.1 Enable JPEG Decoder

Add to `sdkconfig.defaults`:
```
CONFIG_LV_USE_TJPGD=y
```

TJPGD (Tiny JPEG Decoder) is built into LVGL. ~8KB code size. Decodes JPEG → RGB565 in PSRAM. ~200ms for a 660×400 image (software). Hardware JPEG decoder on ESP32-P4 is a future performance optimization — not needed for Phase 1.

After enabling, `lv_image_set_src(img, &jpeg_dsc)` works with JPEG data buffers.

### 2.2 New File: media_cache.c / media_cache.h

HTTP image downloader + PSRAM LRU cache. Runs downloads on Core 1 (same as httpd), uses lv_async_call to update UI on Core 0.

**Public API:**
```c
esp_err_t media_cache_init(void);

// Fetch image from Dragon HTTP API, decode JPEG, return LVGL image descriptor.
// Blocking call — run from a background task, NOT the LVGL thread.
// On success, out_dsc points to PSRAM-backed RGB565 pixel data.
esp_err_t media_cache_fetch(const char *url, lv_image_dsc_t *out_dsc);

void media_cache_evict_oldest(void);
void media_cache_clear(void);
```

**Cache design:**
- 5 pre-allocated PSRAM slots at boot, each 660×440×2 = 581KB = ~2.9MB total
- Pre-allocation eliminates alloc/free fragmentation — slots are never freed, just overwritten
- LRU tracking: array of 5 entries with `url_hash`, `last_access_tick`, `valid` flag
- Cache hit: return existing slot's lv_image_dsc_t pointer
- Cache miss: evict LRU slot, download JPEG via esp_http_client, decode via TJPGD into slot buffer
- Download uses 4KB chunks with `vTaskDelay(pdMS_TO_TICKS(5))` between chunks to yield to voice WS
- If voice is active (state != READY && state != IDLE), pause download until voice completes

**esp_http_client usage (same pattern as ota.c):**
```c
esp_http_client_config_t cfg = {
    .url = full_url,  // "http://{dragon_host}:{dragon_port}/api/media/{id}"
    .timeout_ms = 10000,
};
esp_http_client_handle_t client = esp_http_client_init(&cfg);
esp_http_client_perform(client);
// Read response body into PSRAM buffer
```

### 2.3 voice.c Changes

Add 3 new cases to `handle_text_message()` (after line 797, the tool_result handler):

```c
} else if (strcmp(type, "media") == 0) {
    const char *url = cJSON_GetStringValue(cJSON_GetObjectItem(root, "url"));
    const char *mtype = cJSON_GetStringValue(cJSON_GetObjectItem(root, "media_type"));
    int w = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(root, "width"));
    int h = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(root, "height"));
    const char *alt = cJSON_GetStringValue(cJSON_GetObjectItem(root, "alt"));
    if (url) ui_chat_push_media(url, mtype, w, h, alt);

} else if (strcmp(type, "card") == 0) {
    const char *title = cJSON_GetStringValue(cJSON_GetObjectItem(root, "title"));
    const char *sub = cJSON_GetStringValue(cJSON_GetObjectItem(root, "subtitle"));
    const char *img = cJSON_GetStringValue(cJSON_GetObjectItem(root, "image_url"));
    const char *desc = cJSON_GetStringValue(cJSON_GetObjectItem(root, "description"));
    if (title) ui_chat_push_card(title, sub, img, desc);

} else if (strcmp(type, "audio_clip") == 0) {
    const char *url = cJSON_GetStringValue(cJSON_GetObjectItem(root, "url"));
    float dur = (float)cJSON_GetNumberValue(cJSON_GetObjectItem(root, "duration_s"));
    const char *label = cJSON_GetStringValue(cJSON_GetObjectItem(root, "label"));
    if (url) ui_chat_push_audio_clip(url, dur, label);
}
```

### 2.4 ui_chat.c Changes

Three new bubble types alongside existing `ui_chat_add_message()`:

**Image bubble (`add_image_bubble`):**
- Objects: 2 (container + lv_image) or 3 (container + placeholder label + lv_image after load)
- Container: same style as AI bubble (dark bg, 16px radius), max width 460px
- On push: create container with "Loading..." label at s_next_y
- Spawn background task: `media_cache_fetch(url, &dsc)` → on success → `lv_async_call` → replace label with `lv_image`
- Image scaled to fit within 460px width, maintain aspect ratio
- Tap handler: create fullscreen overlay with lv_image at original resolution (reuse voice overlay pattern)

**Card bubble (`add_card_bubble`):**
- Objects: 3-4 (container + title label + subtitle label + optional thumbnail image)
- Container: slightly different style — border accent color, 12px radius
- Title: FONT_BODY (20px), white
- Subtitle: FONT_SMALL (14px), #888
- Source attribution: FONT_CAPTION (16px), #666, bottom-right
- Max width: 460px

**Audio clip bubble (`add_audio_bubble`):**
- Objects: 3 (container + play/pause button + label with duration)
- Container: compact, 460px × 56px
- Play button: 40×40, toggles play/pause
- On tap: download audio via media_cache (reuse HTTP client), play via `tab5_audio_play_raw()` (existing I2S pipeline)
- Only one audio clip plays at a time — tapping a new one stops the current

**Message limit adjustment:**
- Current: MAX_MESSAGES = 30
- With media: track object count. If total LVGL children in s_msg_scroll exceeds 80, evict oldest messages until under 80
- This dynamically adapts: text-heavy conversations keep ~30 messages, image-heavy ones keep ~15-20

### 2.5 ui_chat.h Additions

```c
void ui_chat_push_media(const char *url, const char *media_type,
                        int width, int height, const char *alt);
void ui_chat_push_card(const char *title, const char *subtitle,
                       const char *image_url, const char *description);
void ui_chat_push_audio_clip(const char *url, float duration_s, const char *label);
```

### 2.6 Camera-to-Chat (ui_camera.c)

Add "Ask AI" button to camera capture preview (shown after user takes a photo):
- Button appears next to existing "Save" button
- On tap: upload current RGB565 frame as BMP to Dragon `/api/media/upload` via HTTP POST
- Dragon converts BMP → JPEG server-side (Pillow, trivial)
- On upload success: navigate to Chat, send `{"type":"user_media","media_id":"...","text":""}` via WS
- Show user's photo as image bubble (right-aligned, user color)
- Keyboard appears for optional text prompt ("What is this?")
- If no text provided within 3s, auto-send with default prompt "What's in this image?"

**Upload flow:**
```c
// Capture frame (existing)
tab5_cam_frame_t frame;
tab5_camera_capture(&frame);

// HTTP POST multipart upload to Dragon
esp_http_client_config_t cfg = { .url = upload_url, .method = HTTP_METHOD_POST };
esp_http_client_handle_t client = esp_http_client_init(&cfg);
esp_http_client_set_header(client, "Content-Type", "application/octet-stream");
esp_http_client_set_post_field(client, frame.data, frame.size);
esp_http_client_perform(client);
// Parse response JSON for media_id
```

BMP frame is ~1.8MB for 1280×720 RGB565. On LAN this downloads in <1s. Dragon converts to JPEG immediately.

### 2.7 sdkconfig.defaults Changes

```
# Add:
CONFIG_LV_USE_TJPGD=y
```

No other sdkconfig changes needed. esp_http_client is already linked via OTA dependencies.

---

## 3. Dragon Server Changes (TinkerBox)

### 3.1 New File: dragon_voice/media/store.py

```python
class MediaStore:
    """Manages rendered media files on disk. Auto-cleanup after 24h."""

    MEDIA_DIR = "/home/radxa/media"
    MAX_AGE_HOURS = 24
    MAX_TOTAL_MB = 500

    async def store(self, data: bytes, ext: str = "jpg", session_id: str = "") -> str:
        """Save media bytes, return unique media_id."""

    async def get_path(self, media_id: str) -> Optional[str]:
        """Get filesystem path for media_id. None if expired/missing."""

    async def store_from_url(self, url: str, max_width: int = 660) -> str:
        """Download external image, resize, store. Return media_id."""

    async def cleanup(self):
        """Delete files older than MAX_AGE_HOURS. Called hourly."""
```

### 3.2 New File: dragon_voice/media/pipeline.py

```python
class MediaPipeline:
    """Detects renderable content in LLM responses, produces media events."""

    def __init__(self, store: MediaStore):
        self._store = store

    async def process_response(self, text: str, session_id: str) -> list[dict]:
        """Analyze complete LLM response text, return list of media events to send.

        Detects:
        1. Image URLs (jpg/png/gif/webp) → download, resize, store
        2. Code blocks (```lang...```) → syntax highlight render
        3. Markdown tables (| col |) → styled table render
        4. Plain URLs → Open Graph fetch → card event

        Returns list of dicts ready to send via ws.send_json().
        Max 3 media items per response.
        """

    async def render_code_block(self, code: str, language: str) -> str:
        """Render code with syntax highlighting. Returns media_id."""

    async def render_table(self, table_md: str) -> str:
        """Render markdown table as styled image. Returns media_id."""

    async def render_markdown(self, md: str) -> str:
        """Render formatted markdown as image. Returns media_id."""

    async def proxy_image(self, url: str) -> str:
        """Download external image, resize to 660px max, store. Returns media_id."""

    async def fetch_link_preview(self, url: str) -> Optional[dict]:
        """Fetch Open Graph metadata, return card event dict."""
```

**Rendering implementations (all Pillow-based, no Playwright needed for Phase 1):**

**Image proxy:** `PIL.Image.open(BytesIO(data)).resize((max_w, proportional_h)).save(path, "JPEG", quality=80)`

**Code highlighting:** Pygments lexer → HTML tokens → Pillow `ImageDraw.text()` with monospace font and syntax colors on dark background. Output: 660px wide JPEG.

**Table rendering:** Parse markdown table → calculate column widths → Pillow draws grid lines, headers (bold), cells. Dark theme matching Tab5 UI. Output: 660px wide JPEG.

**Link preview:** HTTP GET url → parse `<meta property="og:title">`, `og:description`, `og:image` → return card event with fetched thumbnail.

### 3.3 New File: dragon_voice/api/media_routes.py

```python
class MediaRoutes:
    def __init__(self, store: MediaStore):
        self._store = store

    def register(self, app: web.Application) -> None:
        app.router.add_get("/api/media/{media_id}", self.serve_media)
        app.router.add_post("/api/media/upload", self.upload_media)

    async def serve_media(self, request: web.Request) -> web.Response:
        """Serve rendered media file. Cache-Control: max-age=3600."""
        media_id = request.match_info["media_id"]
        path = await self._store.get_path(media_id)
        if not path:
            return web.Response(status=404)
        return web.FileResponse(path, headers={
            "Cache-Control": "max-age=3600",
        })

    async def upload_media(self, request: web.Request) -> web.Response:
        """Tab5 uploads camera frame (BMP/JPEG). Convert + store. Return media_id."""
        data = await request.read()
        # Detect format, convert BMP→JPEG if needed via Pillow
        img = Image.open(BytesIO(data))
        img = img.resize((min(img.width, 1280), ...), Image.LANCZOS)
        buf = BytesIO()
        img.save(buf, "JPEG", quality=85)
        media_id = await self._store.store(buf.getvalue(), ext="jpg")
        return web.json_response({"media_id": media_id})
```

### 3.4 server.py Changes

**After `llm_done` is sent (in both `_handle_text` and pipeline voice flow), run media detection:**

```python
# After sending llm_done:
if full_response and media_pipeline:
    response_text = "".join(full_response)
    media_events = await media_pipeline.process_response(response_text, session_id)
    for event in media_events:
        if not ws.closed:
            await ws.send_json(event)
```

**Handle `user_media` message type:**

```python
elif cmd_type == "user_media":
    media_id = cmd.get("media_id")
    text = cmd.get("text", "What's in this image?")
    image_path = await media_store.get_path(media_id)
    if not image_path:
        await ws.send_json({"type": "error", "message": "Image not found"})
        return

    # Check if current LLM supports vision
    if conn_config.llm.backend in ("ollama",) and "vision" not in conn_config.llm.ollama_model:
        await ws.send_json({"type": "error",
            "message": "Image analysis needs Cloud or TinkerClaw mode"})
        return

    image_b64 = base64.b64encode(open(image_path, "rb").read()).decode()
    messages = [{
        "role": "user",
        "content": [
            {"type": "image_url", "image_url": {"url": f"data:image/jpeg;base64,{image_b64}"}},
            {"type": "text", "text": text}
        ]
    }]
    # Stream response via existing LLM path
```

### 3.5 New Dependencies

Add to `requirements.txt`:
```
Pygments>=2.17.0    # Syntax highlighting for code blocks
```

Pillow is already listed. No other new dependencies for Phase 1.

### 3.6 Media Cleanup

Add hourly cleanup task in server.py `_on_startup`:
```python
async def _media_cleanup_loop(self):
    while True:
        await asyncio.sleep(3600)
        await self._media_store.cleanup()
```

---

## 4. What Does NOT Change

- **TinkerClaw gateway** — zero modifications. Dragon intercepts and renders. TinkerClaw just returns text.
- **Voice pipeline** — PTT, dictation, TTS all unchanged. Media is chat-only.
- **Existing WS protocol** — all current message types preserved. New types are additive.
- **NVS settings** — no new settings keys needed.
- **Nav bar, home screen, notes, settings** — unchanged.
- **Hide/show overlay pattern** — chat overlay still hides, not destroys.

---

## 5. Hardware Constraints & Mitigations

| Constraint | Value | Mitigation |
|------------|-------|------------|
| No JPEG decoder enabled | sdkconfig default | Enable CONFIG_LV_USE_TJPGD=y (1 line) |
| JPEG decode ~200ms (software) | TJPGD on RISC-V 400MHz | Show placeholder, decode in background task, swap via lv_async_call |
| 720px display width | Max 660px image after padding | Dragon resizes all images to 660px max before serving |
| ~55 LVGL objects per screen | Each image = 2 objects | Dynamic message limit: evict oldest when object count > 80 |
| Internal SRAM fragmentation | 512KB total, ~42KB free | Pre-allocate 5 fixed PSRAM image slots at boot. Never alloc/free. |
| WiFi shared with voice WS | ESP32-C6 SDIO | Image downloads at priority 2, yield between chunks, pause during voice |
| No video decoder | LVGL has no H.264/VP9 | Dragon extracts key frames → sends as images |
| No HTML renderer | LVGL is not a browser | Dragon screenshots HTML via Playwright (Phase 2) or Pillow renders |
| Camera outputs BMP not JPEG | RGB565 raw format | Upload BMP to Dragon, Dragon converts to JPEG via Pillow |
| Local LLMs don't support vision | qwen3:1.7b is text-only | Dragon rejects user_media in Local mode with helpful error message |
| 32MB PSRAM total | ~25MB+ free after boot | 5-image cache = ~3MB. Huge headroom. |
| Max 3 media per response | Prevent UI flooding | Dragon caps media_pipeline output at 3 items |

---

## 6. Risk Register

| Risk | Severity | Likelihood | Mitigation |
|------|----------|------------|------------|
| PSRAM fragmentation from image buffers | High | Low | Pre-allocate 5 fixed slots at boot. No alloc/free churn. |
| HTTP download stalls voice WS | High | Medium | Priority scheduling. Pause downloads during active voice. 4KB chunks with yield. |
| LLM returns many image URLs | Medium | Medium | Cap at 3 media items per response. Remainder as text links. |
| Dragon media store fills disk | Low | Low | Auto-cleanup >24h files. Max 500MB directory. Hourly sweep. |
| TJPGD decode fails on malformed JPEG | Medium | Low | Catch decode error, show "Image unavailable" placeholder. Don't crash. |
| Multimodal LLM rejects base64 image | Medium | Low | Validate image size <5MB. Resize to 1280px max. Match LLM provider's format requirements. |
| User spams camera uploads | Low | Low | Debounce upload button 2s. Max 1 pending upload at a time. |

---

## 7. Implementation Phases

### Phase 1: Foundation
Enable TJPGD. Create media_cache.c. Add media/card/audio_clip to voice.c dispatcher. Add image bubble to ui_chat.c. Create media_store.py + media_routes.py + media_pipeline.py (image proxy only) on Dragon. Wire post-llm_done media detection in server.py.

**Test:** Ask "Show me a picture of a Japanese maple" in Cloud/TinkerClaw mode → image appears inline in chat.

### Phase 2: Code & Table Rendering
Add Pygments. Code block detection + rendering. Table detection + rendering. Basic markdown formatting.

**Test:** Ask "Write a Python function to sort a list" → syntax-highlighted code image. Ask "Compare iPhone vs Pixel specs" → styled table image.

### Phase 3: Camera-to-Chat
"Ask AI" button on camera. HTTP upload flow. user_media WS message. Dragon base64 forwarding to multimodal LLM. Mode validation (Cloud/TinkerClaw only).

**Test:** Take photo of a plant → "What plant is this?" → AI identifies it with the photo visible in chat.

### Phase 4: Rich Cards & Audio
Card bubble renderer. Audio clip player. Open Graph link previews. Search result cards from web_search tool results.

**Test:** Paste a URL → link preview card appears. Ask for pronunciation → audio clip plays inline.

---

## 8. Files Changed/Created

### TinkerTab (firmware)
| File | Action | Lines |
|------|--------|-------|
| sdkconfig.defaults | Edit | +1 (TJPGD enable) |
| main/media_cache.c | **Create** | ~300 |
| main/media_cache.h | **Create** | ~30 |
| main/voice.c | Edit | +40 (3 new message handlers) |
| main/ui_chat.c | Edit | +200 (image/card/audio bubble renderers) |
| main/ui_chat.h | Edit | +10 (3 new push functions) |
| main/ui_camera.c | Edit | +50 (Ask AI button + upload) |
| main/CMakeLists.txt | Edit | +1 (add media_cache.c to SRCS) |

### TinkerBox (Dragon)
| File | Action | Lines |
|------|--------|-------|
| dragon_voice/media/__init__.py | **Create** | ~5 |
| dragon_voice/media/store.py | **Create** | ~100 |
| dragon_voice/media/pipeline.py | **Create** | ~250 |
| dragon_voice/api/media_routes.py | **Create** | ~80 |
| dragon_voice/api/__init__.py | Edit | +5 (register media routes) |
| dragon_voice/server.py | Edit | +60 (media detection + user_media handler) |
| requirements.txt | Edit | +1 (Pygments) |
