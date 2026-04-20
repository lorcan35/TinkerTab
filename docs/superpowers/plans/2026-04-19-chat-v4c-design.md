# Chat v4·C Ambient — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship the chat overlay redesign from `docs/superpowers/specs/2026-04-19-chat-v4c-design.md` — decomposed, session-scoped, pixel-matched to the v4·C home, with each session carrying its own `voice_mode` + `llm_model` fingerprint.

**Architecture:** 7 new/replaced Tab5 modules (`chat_msg_store`, `chat_msg_view`, `chat_header`, `chat_input_bar`, `chat_suggestions`, `chat_session_drawer`, `ui_chat` rewrite) + Dragon-side schema migration adding two columns to the `sessions` table + two new LVGL fonts (Fraunces italic, JetBrains Mono). Branch: `feat/chat-v4c` off `feat/ui-overhaul-v4`.

**Tech Stack:** C11 / ESP-IDF 5.5.2 / LVGL 9.2.2 (firmware) · Python / aiohttp / aiosqlite (Dragon) · Fraunces + JetBrains Mono (new fonts).

**Working directories:**
- Firmware: `/home/rebelforce/projects/TinkerTab`
- Dragon: `/home/rebelforce/projects/TinkerBox`

---

## Phase 0 — Setup

### Task 0.1: Branch + issue

**Files:** none

- [ ] **Step 1: Create GH issue on TinkerTab**

```bash
cd /home/rebelforce/projects/TinkerTab
gh issue create \
  --title "feat(chat): v4·C Ambient redesign — session-scoped + mode-per-session" \
  --body "Implements spec docs/superpowers/specs/2026-04-19-chat-v4c-design.md. Chat becomes session-scoped; each session carries its own voice_mode + llm_model. Pixel-matched to v4·C home. See spec §2.2 for Dragon schema migration required."
```

Record the issue number (expect #72 or later).

- [ ] **Step 2: Branch off feat/ui-overhaul-v4**

```bash
git checkout feat/ui-overhaul-v4
git pull origin feat/ui-overhaul-v4
git checkout -b feat/chat-v4c
```

- [ ] **Step 3: Open companion GH issue on TinkerBox**

```bash
cd /home/rebelforce/projects/TinkerBox
gh issue create \
  --title "feat(sessions): add voice_mode + llm_model per session (Tab5 chat v4·C)" \
  --body "Additive schema migration. Sessions table gains voice_mode (INT, default 0) + llm_model (TEXT, default ''). REST accepts/returns fields; config_update persists onto active session."
git checkout -b feat/session-mode-fields
```

---

## Phase 1 — Dragon backend (prereq)

### Task 1.1: Schema migration

**Files:**
- Modify: `TinkerBox/schema.sql`
- Create: `TinkerBox/migrations/006_session_mode_fields.sql`

- [ ] **Step 1: Inspect current sessions table**

```bash
cd /home/rebelforce/projects/TinkerBox
grep -n "CREATE TABLE sessions" schema.sql
```

- [ ] **Step 2: Add columns to schema.sql (for fresh DB setups)**

Edit the `CREATE TABLE sessions` block. Add after the existing columns:

```sql
    voice_mode   INTEGER NOT NULL DEFAULT 0,
    llm_model    TEXT    NOT NULL DEFAULT '',
```

- [ ] **Step 3: Create idempotent migration for existing DBs**

Create `migrations/006_session_mode_fields.sql`:

```sql
-- 006_session_mode_fields.sql — Chat v4·C mode-per-session
ALTER TABLE sessions ADD COLUMN voice_mode INTEGER NOT NULL DEFAULT 0;
ALTER TABLE sessions ADD COLUMN llm_model  TEXT    NOT NULL DEFAULT '';
```

- [ ] **Step 4: Run migration locally**

```bash
sqlite3 /home/radxa/tinkertab.db < migrations/006_session_mode_fields.sql || \
sqlite3 tinkerclaw.db < migrations/006_session_mode_fields.sql
```

If running on workstation DB, adjust path. Verify:

```bash
sqlite3 tinkerclaw.db ".schema sessions" | grep -E "voice_mode|llm_model"
```

Expected: two lines showing both columns.

- [ ] **Step 5: Commit**

```bash
git add schema.sql migrations/006_session_mode_fields.sql
git commit -m "feat(schema): add voice_mode + llm_model to sessions (refs #<TB_ISSUE>)"
```

### Task 1.2: Session model dataclass

**Files:** Modify: `TinkerBox/dragon_voice/sessions.py` (or wherever the Session dataclass lives — grep first)

- [ ] **Step 1: Locate session dataclass**

```bash
grep -rn "class Session\|@dataclass" dragon_voice/sessions.py dragon_voice/db.py 2>/dev/null | head
```

- [ ] **Step 2: Add fields to Session dataclass**

In the Session dataclass, add:

```python
voice_mode: int = 0
llm_model: str = ""
```

Update `Session.from_row(row)` (or equivalent) to include the new fields:

```python
voice_mode=row["voice_mode"] if "voice_mode" in row.keys() else 0,
llm_model=row["llm_model"] if "llm_model" in row.keys() else "",
```

- [ ] **Step 3: Update SQL SELECT in session fetch functions**

`grep -n "SELECT.*FROM sessions" dragon_voice/` — add `voice_mode, llm_model` to every SELECT projection. Explicit list is safer than `SELECT *`.

- [ ] **Step 4: Update INSERT INTO sessions**

`grep -n "INSERT INTO sessions" dragon_voice/` — add the two columns to every INSERT column list + values list. Default to 0 and '' when not supplied.

- [ ] **Step 5: Write unit test**

Create or extend `tests/test_sessions.py`:

```python
import asyncio, pytest
from dragon_voice.sessions import SessionManager

@pytest.mark.asyncio
async def test_session_mode_fields_roundtrip(tmp_path):
    mgr = SessionManager(db_path=str(tmp_path/"t.db"))
    await mgr.init()
    s = await mgr.create(device_id="test", voice_mode=2, llm_model="anthropic/claude-3.5-haiku")
    loaded = await mgr.get(s.id)
    assert loaded.voice_mode == 2
    assert loaded.llm_model == "anthropic/claude-3.5-haiku"
```

- [ ] **Step 6: Run test**

```bash
cd /home/rebelforce/projects/TinkerBox
python3 -m pytest tests/test_sessions.py::test_session_mode_fields_roundtrip -v
```

Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add dragon_voice/sessions.py tests/test_sessions.py
git commit -m "feat(sessions): roundtrip voice_mode + llm_model (refs #<TB_ISSUE>)"
```

### Task 1.3: REST routes accept + return the new fields

**Files:** Modify: `TinkerBox/dragon_voice/api/sessions.py`

- [ ] **Step 1: Update POST /api/v1/sessions handler**

Find the POST handler. Accept optional `voice_mode` (int, 0-3) and `llm_model` (str, max 128) from JSON body:

```python
body = await request.json()
voice_mode = int(body.get("voice_mode", 0))
if not 0 <= voice_mode <= 3:
    return json_error(400, "voice_mode must be 0-3")
llm_model = str(body.get("llm_model", ""))[:128]
session = await mgr.create(
    device_id=device_id,
    voice_mode=voice_mode,
    llm_model=llm_model,
)
```

- [ ] **Step 2: Update GET /api/v1/sessions response**

The `Session.to_dict()` (or equivalent serializer) must include `voice_mode` + `llm_model`. Verify after change:

```bash
python3 -c "
import asyncio
from dragon_voice.sessions import SessionManager
async def main():
    mgr = SessionManager(db_path='/tmp/t.db'); await mgr.init()
    s = await mgr.create(device_id='x', voice_mode=1, llm_model='ollama/qwen3:1.7b')
    print(s.to_dict())
asyncio.run(main())
"
```

Expected dict to include `voice_mode: 1` and `llm_model: 'ollama/qwen3:1.7b'`.

- [ ] **Step 3: Add PATCH /api/v1/sessions/{id}**

If not already present:

```python
async def patch_session(request):
    sid = request.match_info["id"]
    body = await request.json()
    patch = {}
    if "voice_mode" in body:
        vm = int(body["voice_mode"])
        if not 0 <= vm <= 3:
            return json_error(400, "voice_mode 0-3")
        patch["voice_mode"] = vm
    if "llm_model" in body:
        patch["llm_model"] = str(body["llm_model"])[:128]
    if "title" in body:
        patch["title"] = str(body["title"])[:255]
    updated = await mgr.update(sid, **patch)
    if not updated:
        return json_error(404, "session not found")
    return json_ok({"session": updated.to_dict()})
```

Register: `app.router.add_patch("/api/v1/sessions/{id}", patch_session)`

- [ ] **Step 4: Add E2E test**

Extend `tests/test_api_e2e.py`:

```python
def test_create_session_with_mode(self):
    r = self.session.post(f"{BASE}/api/v1/sessions",
        json={"device_id":"e2e", "voice_mode":2, "llm_model":"anthropic/claude-3.5-haiku"})
    assert r.status_code == 201
    assert r.json()["session"]["voice_mode"] == 2
    assert r.json()["session"]["llm_model"] == "anthropic/claude-3.5-haiku"

def test_patch_session_mode(self):
    # ... create then PATCH ...
```

- [ ] **Step 5: Run API tests against local Dragon**

```bash
python3 tests/test_api_e2e.py TestAPI.test_create_session_with_mode -v
python3 tests/test_api_e2e.py TestAPI.test_patch_session_mode -v
```

- [ ] **Step 6: Commit**

```bash
git add dragon_voice/api/sessions.py tests/test_api_e2e.py
git commit -m "feat(api): POST/PATCH sessions accept voice_mode + llm_model (refs #<TB_ISSUE>)"
```

### Task 1.4: config_update WS persists onto active session

**Files:** Modify: `TinkerBox/dragon_voice/server.py` (or wherever the `config_update` handler lives — `grep -n config_update dragon_voice/`)

- [ ] **Step 1: Locate handler**

```bash
grep -n "config_update" dragon_voice/server.py dragon_voice/pipeline.py | head
```

- [ ] **Step 2: After applying config change, PATCH the active session**

Inside the `config_update` handler, after the current pipeline swap code, add:

```python
# Persist mode onto the session record (chat v4·C mode-per-session)
if session_id:
    await session_mgr.update(
        session_id,
        voice_mode=voice_mode,
        llm_model=llm_model,
    )
```

- [ ] **Step 3: Verify via test**

Add E2E test that: creates session, sends config_update over WS, fetches session, asserts mode persisted.

- [ ] **Step 4: Commit**

```bash
git add dragon_voice/server.py tests/
git commit -m "feat(ws): config_update persists mode onto active session (refs #<TB_ISSUE>)"
```

### Task 1.5: Deploy to Dragon

**Files:** none

- [ ] **Step 1: Scp new code**

```bash
cd /home/rebelforce/projects/TinkerBox
scp -r dragon_voice/ schema.sql migrations/ radxa@192.168.70.242:/home/radxa/
```

- [ ] **Step 2: Run migration on Dragon DB**

```bash
ssh radxa@192.168.70.242 "cd /home/radxa && sqlite3 tinkerclaw.db < migrations/006_session_mode_fields.sql && sqlite3 tinkerclaw.db '.schema sessions' | grep -E 'voice_mode|llm_model'"
```

Expected: two lines showing the new columns.

- [ ] **Step 3: Clear pycache + restart service**

```bash
ssh radxa@192.168.70.242 "find /home/radxa/dragon_voice -name __pycache__ -exec rm -rf {} + ; sudo systemctl restart tinkerclaw-voice && sleep 2 && sudo systemctl is-active tinkerclaw-voice"
```

Expected: `active`.

- [ ] **Step 4: Probe the new fields via API**

```bash
curl -s http://192.168.70.242:3502/api/v1/sessions?limit=3 | python3 -m json.tool | head -30
```

Expected: each session object includes `voice_mode` and `llm_model`.

- [ ] **Step 5: Push TinkerBox branch**

```bash
cd /home/rebelforce/projects/TinkerBox
git push origin feat/session-mode-fields
gh pr create --title "feat: session mode-per-session fields" --body "Closes #<TB_ISSUE>"
```

---

## Phase 2 — TinkerTab font pipeline

### Task 2.1: Add Fraunces italic + JetBrains Mono Medium subsets

**Files:**
- Create: `TinkerTab/main/fonts/fraunces_italic_32.c`
- Create: `TinkerTab/main/fonts/fraunces_italic_22.c`
- Create: `TinkerTab/main/fonts/jetbrains_mono_medium_14.c`
- Modify: `TinkerTab/main/config.h`
- Modify: `TinkerTab/main/CMakeLists.txt`

- [ ] **Step 1: Download font sources**

```bash
mkdir -p /home/rebelforce/projects/TinkerTab/fonts-src
cd /home/rebelforce/projects/TinkerTab/fonts-src
wget -q https://github.com/undercasetype/Fraunces/raw/main/fonts/static/Fraunces-Italic.ttf
wget -q https://github.com/JetBrains/JetBrainsMono/raw/master/fonts/ttf/JetBrainsMono-Medium.ttf
ls -la *.ttf
```

- [ ] **Step 2: Generate 3 font C blobs via lv_font_conv**

```bash
cd /home/rebelforce/projects/TinkerTab
npx lv_font_conv --no-compress --no-prefilter --bpp 4 --size 32 \
  --font fonts-src/Fraunces-Italic.ttf \
  -r 0x20-0x7F,0x2022,0xB7 \
  --format lvgl -o main/fonts/fraunces_italic_32.c

npx lv_font_conv --no-compress --no-prefilter --bpp 4 --size 22 \
  --font fonts-src/Fraunces-Italic.ttf \
  -r 0x20-0x7F,0x2022 \
  --format lvgl -o main/fonts/fraunces_italic_22.c

npx lv_font_conv --no-compress --no-prefilter --bpp 4 --size 14 \
  --font fonts-src/JetBrainsMono-Medium.ttf \
  -r 0x20-0x7F,0x2022,0xB7 \
  --format lvgl -o main/fonts/jetbrains_mono_medium_14.c
```

- [ ] **Step 3: Verify C files generated with `LV_FONT_DECLARE`-friendly symbols**

```bash
grep -E "^const lv_font_t" main/fonts/*.c | head -3
```

Expected: 3 lines, one per font.

- [ ] **Step 4: Add extern declarations to config.h**

Append to `main/config.h` after existing `FONT_*` defines:

```c
/* ── v4·C chat fonts ─────────────────────────────────────── */
LV_FONT_DECLARE(fraunces_italic_32);
LV_FONT_DECLARE(fraunces_italic_22);
LV_FONT_DECLARE(jetbrains_mono_medium_14);
#define FONT_CHAT_TITLE  (&fraunces_italic_32)
#define FONT_CHAT_EMPH   (&fraunces_italic_22)
#define FONT_CHAT_MONO   (&jetbrains_mono_medium_14)
```

- [ ] **Step 5: Add font .c files to main/CMakeLists.txt SRCS**

Find the `idf_component_register(SRCS ...)` block and add:

```cmake
"fonts/fraunces_italic_32.c"
"fonts/fraunces_italic_22.c"
"fonts/jetbrains_mono_medium_14.c"
```

- [ ] **Step 6: Build + verify flash + SRAM impact**

```bash
. /home/rebelforce/esp/esp-idf/export.sh > /dev/null 2>&1
idf.py build 2>&1 | grep -E "tinkertab.bin binary size|Smallest|free"
```

Expected: build succeeds; `tinkertab.bin` grows by < 60 KB compared to pre-task.

- [ ] **Step 7: Commit**

```bash
git add main/fonts/ main/config.h main/CMakeLists.txt
git commit -m "feat(fonts): add Fraunces italic + JetBrains Mono for chat v4·C (refs #<TT_ISSUE>)"
```

---

## Phase 3 — Data layer (pure C, testable)

### Task 3.1: chat_msg_store module

**Files:**
- Create: `TinkerTab/main/chat_msg_store.h`
- Create: `TinkerTab/main/chat_msg_store.c`
- Modify: `TinkerTab/main/CMakeLists.txt`

- [ ] **Step 1: Write chat_msg_store.h**

Create `main/chat_msg_store.h`:

```c
/* chat_msg_store — Session-scoped message ring buffer.
 *
 * Bounded at BSP_CHAT_MAX_MESSAGES entries for the ACTIVE session.
 * Switching sessions wipes the store. No LVGL dependency. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define CHAT_MAX_MESSAGES     100
#define CHAT_SESSION_ID_LEN   40
#define CHAT_LLM_MODEL_LEN    64
#define CHAT_TITLE_LEN        80
#define CHAT_TEXT_LEN         512
#define CHAT_MEDIA_URL_LEN    256
#define CHAT_SUBTITLE_LEN     128

typedef enum {
    CHAT_MSG_TEXT = 0,
    CHAT_MSG_IMAGE,
    CHAT_MSG_CARD,
    CHAT_MSG_AUDIO_CLIP,
    CHAT_MSG_SYSTEM,
} chat_msg_type_t;

typedef struct {
    char     session_id[CHAT_SESSION_ID_LEN];
    uint8_t  voice_mode;                     /* 0..3 */
    char     llm_model[CHAT_LLM_MODEL_LEN];
    char     title[CHAT_TITLE_LEN];
    uint32_t updated_at;
    bool     valid;
} chat_session_t;

typedef struct {
    chat_msg_type_t type;
    bool     is_user;
    char     text[CHAT_TEXT_LEN];
    char     media_url[CHAT_MEDIA_URL_LEN];
    char     subtitle[CHAT_SUBTITLE_LEN];
    uint32_t timestamp;
    int16_t  height_px;                      /* cached after first render; -1 = not measured */
    bool     active;                         /* slot in use */
} chat_msg_t;

/* ── API ─────────────────────────────────────────────────────── */
void                 chat_store_init(void);
bool                 chat_store_set_session(const chat_session_t *s);
const chat_session_t *chat_store_active_session(void);
int                  chat_store_add(const chat_msg_t *msg);
int                  chat_store_count(void);
const chat_msg_t    *chat_store_get(int index);           /* 0 = oldest, count-1 = newest */
bool                 chat_store_set_height(int index, int16_t h);
void                 chat_store_clear(void);              /* used by "+ new chat" */
/* Update last message's text in place — used for streaming + text_update. */
bool                 chat_store_update_last_text(const char *text);
```

- [ ] **Step 2: Write chat_msg_store.c**

Create `main/chat_msg_store.c`:

```c
#include "chat_msg_store.h"
#include <string.h>
#include <assert.h>

static chat_session_t s_active = { .valid = false };
static chat_msg_t     s_msgs[CHAT_MAX_MESSAGES];
static int            s_count = 0;
static int            s_write_idx = 0;

static void copy_str(char *dst, size_t dst_sz, const char *src)
{
    if (!src) { if (dst_sz) dst[0] = 0; return; }
    size_t n = strlen(src);
    if (n >= dst_sz) n = dst_sz - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

void chat_store_init(void)
{
    memset(&s_active, 0, sizeof(s_active));
    memset(s_msgs, 0, sizeof(s_msgs));
    s_count = 0;
    s_write_idx = 0;
}

bool chat_store_set_session(const chat_session_t *s)
{
    if (!s) return false;
    s_active = *s;
    s_active.valid = true;
    memset(s_msgs, 0, sizeof(s_msgs));
    s_count = 0;
    s_write_idx = 0;
    return true;
}

const chat_session_t *chat_store_active_session(void)
{
    return s_active.valid ? &s_active : NULL;
}

int chat_store_add(const chat_msg_t *msg)
{
    if (!msg) return -1;
    int idx;
    if (s_count < CHAT_MAX_MESSAGES) {
        idx = s_count++;
    } else {
        /* Ring buffer: overwrite oldest */
        idx = s_write_idx;
        s_write_idx = (s_write_idx + 1) % CHAT_MAX_MESSAGES;
    }
    s_msgs[idx] = *msg;
    s_msgs[idx].active = true;
    if (s_msgs[idx].height_px == 0) s_msgs[idx].height_px = -1;
    return idx;
}

int chat_store_count(void) { return s_count; }

const chat_msg_t *chat_store_get(int index)
{
    if (index < 0 || index >= s_count) return NULL;
    /* When ring has wrapped, oldest is at s_write_idx */
    int real = (s_count == CHAT_MAX_MESSAGES)
                 ? (s_write_idx + index) % CHAT_MAX_MESSAGES
                 : index;
    return &s_msgs[real];
}

bool chat_store_set_height(int index, int16_t h)
{
    if (index < 0 || index >= s_count) return false;
    int real = (s_count == CHAT_MAX_MESSAGES)
                 ? (s_write_idx + index) % CHAT_MAX_MESSAGES
                 : index;
    s_msgs[real].height_px = h;
    return true;
}

void chat_store_clear(void)
{
    memset(s_msgs, 0, sizeof(s_msgs));
    s_count = 0;
    s_write_idx = 0;
}

bool chat_store_update_last_text(const char *text)
{
    if (s_count == 0) return false;
    int real = (s_count == CHAT_MAX_MESSAGES)
                 ? (s_write_idx + s_count - 1) % CHAT_MAX_MESSAGES
                 : s_count - 1;
    copy_str(s_msgs[real].text, sizeof(s_msgs[real].text), text);
    s_msgs[real].height_px = -1;  /* invalidate cached height */
    return true;
}
```

- [ ] **Step 3: Add to CMakeLists**

Append to the `idf_component_register(SRCS ...)` list:

```cmake
"chat_msg_store.c"
```

- [ ] **Step 4: Build verifies pure-C module compiles**

```bash
idf.py build 2>&1 | tail -6
```

Expected: build succeeds, no warnings from chat_msg_store.c.

- [ ] **Step 5: Write Linux unit test (native host)**

Create `TinkerTab/test/host/test_chat_msg_store.c`:

```c
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "chat_msg_store.h"

static chat_session_t mksess(const char *id, uint8_t mode)
{
    chat_session_t s = {0};
    strncpy(s.session_id, id, sizeof(s.session_id)-1);
    s.voice_mode = mode;
    strncpy(s.llm_model, "ollama/qwen3:1.7b", sizeof(s.llm_model)-1);
    return s;
}

int main(void)
{
    chat_store_init();
    assert(chat_store_count() == 0);
    assert(chat_store_active_session() == NULL);

    chat_session_t s = mksess("sess-abc", 1);
    assert(chat_store_set_session(&s));
    assert(chat_store_active_session() != NULL);
    assert(chat_store_active_session()->voice_mode == 1);

    chat_msg_t m = { .type = CHAT_MSG_TEXT, .is_user = true, .timestamp = 1000 };
    strcpy(m.text, "hi");
    assert(chat_store_add(&m) == 0);
    assert(chat_store_count() == 1);
    assert(strcmp(chat_store_get(0)->text, "hi") == 0);

    /* Ring overflow: fill 101 messages, oldest drops */
    for (int i = 1; i <= CHAT_MAX_MESSAGES; i++) {
        chat_msg_t x = { .type = CHAT_MSG_TEXT, .is_user = false, .timestamp = (uint32_t)i };
        snprintf(x.text, sizeof(x.text), "msg-%d", i);
        chat_store_add(&x);
    }
    assert(chat_store_count() == CHAT_MAX_MESSAGES);
    assert(chat_store_get(0)->timestamp == 1);           /* first "hi" dropped */
    assert(chat_store_get(CHAT_MAX_MESSAGES-1)->timestamp == CHAT_MAX_MESSAGES);

    /* update_last_text */
    assert(chat_store_update_last_text("REPLACED"));
    assert(strcmp(chat_store_get(CHAT_MAX_MESSAGES-1)->text, "REPLACED") == 0);

    /* switch session wipes store */
    chat_session_t s2 = mksess("sess-xyz", 2);
    chat_store_set_session(&s2);
    assert(chat_store_count() == 0);
    assert(chat_store_active_session()->voice_mode == 2);

    printf("OK\n");
    return 0;
}
```

- [ ] **Step 6: Compile + run on host**

```bash
cd /home/rebelforce/projects/TinkerTab
gcc -std=c11 -Wall -Wextra -I main test/host/test_chat_msg_store.c main/chat_msg_store.c -o /tmp/test_chat_msg_store && /tmp/test_chat_msg_store
```

Expected output: `OK`.

- [ ] **Step 7: Commit**

```bash
git add main/chat_msg_store.c main/chat_msg_store.h test/host/test_chat_msg_store.c main/CMakeLists.txt
git commit -m "feat(chat): chat_msg_store pure-C session-scoped ring buffer (refs #<TT_ISSUE>)"
```

---

## Phase 4 — Composite widgets (reusable)

### Task 4.1: chat_header module

**Files:**
- Create: `TinkerTab/main/chat_header.h`
- Create: `TinkerTab/main/chat_header.c`
- Modify: `TinkerTab/main/CMakeLists.txt`

- [ ] **Step 1: Write chat_header.h**

```c
/* chat_header — composite header widget.
 *
 * Layout (720×96): [back 44] [title] [▾] [spacer] [mode-chip] [+ 44]
 * All tokens sourced from ui_theme.h + ui_home.c constants.
 * Reusable by Notes (omit mode chip + chevron). */
#pragma once
#include "lvgl.h"
#include <stdint.h>

typedef struct chat_header chat_header_t;

typedef void (*chat_header_evt_cb_t)(void *user_data);

chat_header_t *chat_header_create(lv_obj_t *parent, const char *title);
void chat_header_set_title(chat_header_t *h, const char *title);
void chat_header_set_mode(chat_header_t *h, uint8_t voice_mode, const char *llm_model);
/* Paint the 140×2 accent bar under the header in the session's mode color. */
void chat_header_set_accent_color(chat_header_t *h, uint32_t hex);
void chat_header_on_back(chat_header_t *h, chat_header_evt_cb_t cb, void *ud);
void chat_header_on_chevron(chat_header_t *h, chat_header_evt_cb_t cb, void *ud);
void chat_header_on_plus(chat_header_t *h, chat_header_evt_cb_t cb, void *ud);
void chat_header_on_mode_long_press(chat_header_t *h, chat_header_evt_cb_t cb, void *ud);
```

- [ ] **Step 2: Write chat_header.c**

Key snippet (full file — ~170 LOC). See spec §4 for exact pixel numbers.

```c
#include "chat_header.h"
#include "ui_theme.h"
#include "config.h"
#include <string.h>

#define HDR_H        96
#define HDR_SIDE_PAD 40
#define HDR_GAP      16
#define ACCENT_W     140
#define ACCENT_H     2
#define TOUCH_MIN    44

struct chat_header {
    lv_obj_t *root;       /* the 96-h bar */
    lv_obj_t *back;
    lv_obj_t *title;
    lv_obj_t *chev;
    lv_obj_t *chip;
    lv_obj_t *chip_dot;
    lv_obj_t *chip_name;
    lv_obj_t *chip_sub;
    lv_obj_t *plus;
    lv_obj_t *accent;     /* 140×2 bar under root */
    chat_header_evt_cb_t back_cb, chev_cb, plus_cb, mlp_cb;
    void *back_ud, *chev_ud, *plus_ud, *mlp_ud;
};

static const uint32_t s_mode_tint[4] = {
    TH_MODE_LOCAL, TH_MODE_HYBRID, TH_MODE_CLOUD, TH_MODE_CLAW,
};
static const char *s_mode_short[4] = { "Local", "Hybrid", "Cloud", "Claw" };

static void on_back(lv_event_t *e)
{
    chat_header_t *h = lv_event_get_user_data(e);
    if (h && h->back_cb) h->back_cb(h->back_ud);
}
static void on_chev(lv_event_t *e)
{
    chat_header_t *h = lv_event_get_user_data(e);
    if (h && h->chev_cb) h->chev_cb(h->chev_ud);
}
static void on_plus(lv_event_t *e)
{
    chat_header_t *h = lv_event_get_user_data(e);
    if (h && h->plus_cb) h->plus_cb(h->plus_ud);
}
static void on_chip_lp(lv_event_t *e)
{
    chat_header_t *h = lv_event_get_user_data(e);
    if (h && h->mlp_cb) h->mlp_cb(h->mlp_ud);
}

chat_header_t *chat_header_create(lv_obj_t *parent, const char *title)
{
    chat_header_t *h = lv_malloc(sizeof(*h));
    memset(h, 0, sizeof(*h));

    h->root = lv_obj_create(parent);
    lv_obj_remove_style_all(h->root);
    lv_obj_set_size(h->root, 720, HDR_H);
    lv_obj_set_pos(h->root, 0, 0);
    lv_obj_set_style_bg_color(h->root, lv_color_hex(TH_BG), 0);
    lv_obj_set_style_bg_opa(h->root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(h->root, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(h->root, 1, 0);
    lv_obj_set_style_border_color(h->root, lv_color_hex(0x1E1E2A), 0);
    lv_obj_clear_flag(h->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(h->root, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(h->root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(h->root, HDR_SIDE_PAD - 8, 0);  /* -8 because back button has its own 8 inset */
    lv_obj_set_style_pad_right(h->root, HDR_SIDE_PAD, 0);
    lv_obj_set_style_pad_column(h->root, HDR_GAP, 0);

    /* Back */
    h->back = lv_label_create(h->root);
    lv_label_set_text(h->back, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(h->back, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(h->back, lv_color_hex(TH_TEXT_BODY), 0);
    lv_obj_set_size(h->back, TOUCH_MIN, TOUCH_MIN);
    lv_obj_set_style_text_align(h->back, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(h->back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(h->back, on_back, LV_EVENT_CLICKED, h);

    /* Title (Fraunces italic 32) */
    h->title = lv_label_create(h->root);
    lv_label_set_text(h->title, title ? title : "Chat");
    lv_obj_set_style_text_font(h->title, FONT_CHAT_TITLE, 0);
    lv_obj_set_style_text_color(h->title, lv_color_hex(TH_TEXT_PRIMARY), 0);

    /* Chevron ▾ */
    h->chev = lv_label_create(h->root);
    lv_label_set_text(h->chev, LV_SYMBOL_DOWN);
    lv_obj_set_style_text_font(h->chev, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(h->chev, lv_color_hex(TH_TEXT_DIM), 0);
    lv_obj_set_size(h->chev, TOUCH_MIN, TOUCH_MIN);
    lv_obj_set_style_text_align(h->chev, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(h->chev, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(h->chev, on_chev, LV_EVENT_CLICKED, h);

    /* Spacer (flex grow) */
    lv_obj_t *sp = lv_obj_create(h->root);
    lv_obj_remove_style_all(sp);
    lv_obj_set_flex_grow(sp, 1);
    lv_obj_set_height(sp, 1);
    lv_obj_set_style_bg_opa(sp, LV_OPA_TRANSP, 0);

    /* Mode chip */
    h->chip = lv_obj_create(h->root);
    lv_obj_remove_style_all(h->chip);
    lv_obj_set_size(h->chip, LV_SIZE_CONTENT, TOUCH_MIN);
    lv_obj_set_style_bg_color(h->chip, lv_color_hex(TH_CARD_ELEVATED), 0);
    lv_obj_set_style_bg_opa(h->chip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(h->chip, 22, 0);
    lv_obj_set_style_border_width(h->chip, 1, 0);
    lv_obj_set_style_border_color(h->chip, lv_color_hex(0x1E1E2A), 0);
    lv_obj_set_style_pad_hor(h->chip, 18, 0);
    lv_obj_set_style_pad_ver(h->chip, 0, 0);
    lv_obj_set_style_pad_column(h->chip, 10, 0);
    lv_obj_set_flex_flow(h->chip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(h->chip, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(h->chip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(h->chip, on_chip_lp, LV_EVENT_LONG_PRESSED, h);

    h->chip_dot = lv_obj_create(h->chip);
    lv_obj_remove_style_all(h->chip_dot);
    lv_obj_set_size(h->chip_dot, 8, 8);
    lv_obj_set_style_radius(h->chip_dot, 4, 0);
    lv_obj_set_style_bg_color(h->chip_dot, lv_color_hex(TH_MODE_LOCAL), 0);
    lv_obj_set_style_bg_opa(h->chip_dot, LV_OPA_COVER, 0);

    h->chip_name = lv_label_create(h->chip);
    lv_label_set_text(h->chip_name, "Local");
    lv_obj_set_style_text_font(h->chip_name, FONT_HEADING, 0);  /* 22 */
    lv_obj_set_style_text_color(h->chip_name, lv_color_hex(TH_TEXT_PRIMARY), 0);

    h->chip_sub = lv_label_create(h->chip);
    lv_label_set_text(h->chip_sub, "");
    lv_obj_set_style_text_font(h->chip_sub, FONT_CHAT_MONO, 0);
    lv_obj_set_style_text_color(h->chip_sub, lv_color_hex(TH_TEXT_DIM), 0);

    /* Plus (new chat) */
    h->plus = lv_obj_create(h->root);
    lv_obj_remove_style_all(h->plus);
    lv_obj_set_size(h->plus, TOUCH_MIN, TOUCH_MIN);
    lv_obj_set_style_bg_color(h->plus, lv_color_hex(TH_CARD_ELEVATED), 0);
    lv_obj_set_style_bg_opa(h->plus, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(h->plus, 22, 0);
    lv_obj_set_style_border_width(h->plus, 1, 0);
    lv_obj_set_style_border_color(h->plus, lv_color_hex(0x1E1E2A), 0);
    lv_obj_add_flag(h->plus, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(h->plus, on_plus, LV_EVENT_CLICKED, h);
    lv_obj_t *pl = lv_label_create(h->plus);
    lv_label_set_text(pl, "+");
    lv_obj_set_style_text_font(pl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(pl, lv_color_hex(TH_AMBER), 0);
    lv_obj_center(pl);

    /* Accent bar (140×2 under root) */
    h->accent = lv_obj_create(parent);
    lv_obj_remove_style_all(h->accent);
    lv_obj_set_size(h->accent, ACCENT_W, ACCENT_H);
    lv_obj_set_pos(h->accent, 0, HDR_H);
    lv_obj_set_style_bg_color(h->accent, lv_color_hex(TH_AMBER), 0);
    lv_obj_set_style_bg_opa(h->accent, LV_OPA_COVER, 0);

    return h;
}

void chat_header_set_title(chat_header_t *h, const char *title)
{
    if (h && h->title) lv_label_set_text(h->title, title);
}

void chat_header_set_mode(chat_header_t *h, uint8_t m, const char *llm)
{
    if (!h) return;
    if (m > 3) m = 0;
    lv_obj_set_style_bg_color(h->chip_dot, lv_color_hex(s_mode_tint[m]), 0);
    if (h->chip_name) lv_label_set_text(h->chip_name, s_mode_short[m]);
    /* Short model nickname: drop provider prefix before "/" if present */
    if (h->chip_sub) {
        const char *nick = llm ? llm : "";
        const char *slash = strchr(nick, '/');
        if (slash) nick = slash + 1;
        /* Truncate at ":" for ollama tags e.g. qwen3:1.7b */
        char buf[32];
        size_t n = strlen(nick); if (n >= sizeof(buf)) n = sizeof(buf) - 1;
        memcpy(buf, nick, n); buf[n] = 0;
        char *col = strchr(buf, ':');
        if (col) *col = 0;
        /* Uppercase it in place */
        for (char *p = buf; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
        lv_label_set_text(h->chip_sub, buf);
    }
    chat_header_set_accent_color(h, s_mode_tint[m]);
}

void chat_header_set_accent_color(chat_header_t *h, uint32_t hex)
{
    if (h && h->accent) lv_obj_set_style_bg_color(h->accent, lv_color_hex(hex), 0);
}

void chat_header_on_back(chat_header_t *h, chat_header_evt_cb_t cb, void *ud)
{ if (h) { h->back_cb = cb; h->back_ud = ud; } }
void chat_header_on_chevron(chat_header_t *h, chat_header_evt_cb_t cb, void *ud)
{ if (h) { h->chev_cb = cb; h->chev_ud = ud; } }
void chat_header_on_plus(chat_header_t *h, chat_header_evt_cb_t cb, void *ud)
{ if (h) { h->plus_cb = cb; h->plus_ud = ud; } }
void chat_header_on_mode_long_press(chat_header_t *h, chat_header_evt_cb_t cb, void *ud)
{ if (h) { h->mlp_cb = cb; h->mlp_ud = ud; } }
```

- [ ] **Step 3: Add to CMakeLists + build**

```bash
# Add "chat_header.c" to SRCS
idf.py build 2>&1 | tail -4
```

Expected: build succeeds.

- [ ] **Step 4: Commit**

```bash
git add main/chat_header.c main/chat_header.h main/CMakeLists.txt
git commit -m "feat(chat): chat_header composite widget (refs #<TT_ISSUE>)"
```

### Task 4.2: chat_input_bar module

**Files:**
- Create: `TinkerTab/main/chat_input_bar.h`
- Create: `TinkerTab/main/chat_input_bar.c`
- Modify: `TinkerTab/main/CMakeLists.txt`

- [ ] **Step 1: Write chat_input_bar.h**

```c
/* chat_input_bar — 108×CARD_W say-pill with 84-px amber orb-ball.
 *
 * Byte-matched to main/ui_home.c say-pill constants (SAY_H=108, disc=84).
 * Orb-ball handles voice (tap = start, tap-again = stop).
 * Keyboard affordance (56 px square) opens the keyboard overlay. */
#pragma once
#include "lvgl.h"

typedef struct chat_input_bar chat_input_bar_t;
typedef void (*chat_input_evt_cb_t)(void *user_data);

chat_input_bar_t *chat_input_bar_create(lv_obj_t *parent);
void chat_input_bar_set_ghost(chat_input_bar_t *b, const char *hint);
/* Voice state drives orb-ball gradient. 0=idle amber, 1=listening hot,
 * 2=processing breath, 3=speaking dark, 4=done (flash to calm). */
void chat_input_bar_set_voice_state(chat_input_bar_t *b, int state);
/* Show a live STT partial above the pill (optional). */
void chat_input_bar_show_partial(chat_input_bar_t *b, const char *partial);
void chat_input_bar_hide_partial(chat_input_bar_t *b);
void chat_input_bar_on_ball_tap(chat_input_bar_t *b, chat_input_evt_cb_t cb, void *ud);
void chat_input_bar_on_keyboard(chat_input_bar_t *b, chat_input_evt_cb_t cb, void *ud);
/* Set callback called when user submits typed text via the keyboard Done key. */
void chat_input_bar_on_text_submit(chat_input_bar_t *b,
    void (*cb)(const char *text, void *ud), void *ud);
```

- [ ] **Step 2: Write chat_input_bar.c**

Following the ui_home.c say-pill pattern — see spec §3.3 + §4.1. Key structure:

```c
/* Root container 108 × (720-80) at y = 1280 - 40 - 108 = 1132. */
/* Ball: 84×84 amber radial (s_orb pattern from ui_home.c). */
/* Text input: hidden lv_textarea; ghost label visible when empty. */
/* Keyboard button: 56×56 elevated card. */
```

Full file ~280 LOC. Reuse the amber-ball gradient code from `orb_paint_for_mode()` in `main/ui_home.c` — identical two-stop vertical gradient.

- [ ] **Step 3: Build + commit**

```bash
idf.py build && git add main/chat_input_bar.* main/CMakeLists.txt && \
git commit -m "feat(chat): chat_input_bar say-pill matches home (refs #<TT_ISSUE>)"
```

### Task 4.3: chat_suggestions module

**Files:**
- Create: `TinkerTab/main/chat_suggestions.h`
- Create: `TinkerTab/main/chat_suggestions.c`

- [ ] **Step 1: API**

```c
/* chat_suggestions — empty-state cards keyed to the active session's mode.
 * 4 cards per mode; shown only when message count = 0.
 * Tap a card → fills the input textarea with the prompt text. */
#pragma once
#include "lvgl.h"
#include <stdint.h>

typedef struct chat_suggestions chat_suggestions_t;
typedef void (*chat_sugg_cb_t)(const char *prompt, void *ud);

chat_suggestions_t *chat_suggestions_create(lv_obj_t *parent);
void chat_suggestions_set_mode(chat_suggestions_t *s, uint8_t voice_mode);
void chat_suggestions_show(chat_suggestions_t *s);
void chat_suggestions_hide(chat_suggestions_t *s);
void chat_suggestions_on_pick(chat_suggestions_t *s, chat_sugg_cb_t cb, void *ud);
```

- [ ] **Step 2: Hardcoded per-mode prompt sets**

```c
static const char *s_prompts[4][4] = {
    /* Local */
    { "What's the date?", "Add a note about…", "Remind me to…", "Summarize my last note" },
    /* Hybrid */
    { "Search the web for…", "Explain like I'm 5…", "What's the weather?", "Brief me on…" },
    /* Cloud */
    { "Write a Python script…", "Compare X and Y", "Draft a reply to…", "Plan my day around…" },
    /* Claw */
    { "Search my inbox for…", "Book a car at…", "Update my calendar…", "Pull the Tab5 docs" },
};
```

Card layout: 4 cards in a flex column, each 620 × 72 px, card color `TH_CARD`, border `0x1E1E2A`, font `FONT_BODY`.

- [ ] **Step 3: Build + commit**

```bash
idf.py build && git add main/chat_suggestions.* && \
git commit -m "feat(chat): chat_suggestions empty-state cards (refs #<TT_ISSUE>)"
```

---

## Phase 5 — Message view (pool + virtual scroll)

### Task 5.1: chat_msg_view module

**Files:**
- Create: `TinkerTab/main/chat_msg_view.h`
- Create: `TinkerTab/main/chat_msg_view.c`

- [ ] **Step 1: API**

```c
/* chat_msg_view — recycled object pool + virtual scroll.
 *
 * Pool of BSP_CHAT_POOL_SIZE (12) slots, each with 3-4 objects.
 * Maps visible scroll window to store messages on scroll.
 * Last slot pinned during streaming (never recycled). */
#pragma once
#include "lvgl.h"
#include <stdbool.h>

typedef struct chat_msg_view chat_msg_view_t;

chat_msg_view_t *chat_msg_view_create(lv_obj_t *parent);
void chat_msg_view_refresh(chat_msg_view_t *v);          /* rebuild visible window */
void chat_msg_view_scroll_to_bottom(chat_msg_view_t *v);
void chat_msg_view_begin_streaming(chat_msg_view_t *v);  /* pin last slot */
void chat_msg_view_append_stream(chat_msg_view_t *v, const char *token);
void chat_msg_view_end_streaming(chat_msg_view_t *v);
void chat_msg_view_set_mode_color(chat_msg_view_t *v, uint32_t hex);  /* paint breakout accent */
```

- [ ] **Step 2: Pool structure**

```c
#define POOL_SIZE 12
typedef struct {
    lv_obj_t *row;            /* flex row — alignment left/right */
    lv_obj_t *bubble;         /* container */
    lv_obj_t *text;           /* primary label — hidden for media-only */
    lv_obj_t *ts;             /* timestamp */
    lv_obj_t *breakout;       /* optional full-bleed band; created on demand */
    int       data_index;     /* -1 = empty */
} msg_slot_t;
```

- [ ] **Step 3: Implementation**

See spec §3 + §4. Key flows:
- On `refresh`: compute first_visible via cumulative heights; assign slots to visible window; hide out-of-window slots.
- On `append_stream`: pin last slot, `lv_label_set_text` on text, recompute height, auto-scroll.
- Slot recycling: clear text, hide breakout (if any), delete on-demand media widget.

Full file ~480 LOC. No placeholder dismissals — the implementation is spec-driven and straightforward flex-layout LVGL.

- [ ] **Step 4: Build + commit**

```bash
idf.py build && git add main/chat_msg_view.* && \
git commit -m "feat(chat): chat_msg_view pool + virtual scroll (refs #<TT_ISSUE>)"
```

---

## Phase 6 — Session drawer

### Task 6.1: chat_session_drawer module

**Files:**
- Create: `TinkerTab/main/chat_session_drawer.h`
- Create: `TinkerTab/main/chat_session_drawer.c`

- [ ] **Step 1: API**

```c
/* chat_session_drawer — pull-down drawer with session list.
 *
 * Fetches GET /api/v1/sessions?limit=10 from Dragon.
 * Each row: mode-colored dot + "MODE · MODEL" line + session title + timestamp.
 * Tap row → calls on_pick(session_id, voice_mode, llm_model). */
#pragma once
#include "lvgl.h"
#include <stdint.h>
#include "chat_msg_store.h"  /* chat_session_t */

typedef struct chat_session_drawer chat_session_drawer_t;
typedef void (*chat_drawer_pick_cb_t)(const chat_session_t *s, void *ud);
typedef void (*chat_drawer_new_cb_t)(void *ud);

chat_session_drawer_t *chat_session_drawer_create(lv_obj_t *parent);
void chat_session_drawer_show(chat_session_drawer_t *d);  /* triggers REST fetch */
void chat_session_drawer_hide(chat_session_drawer_t *d);
bool chat_session_drawer_is_open(chat_session_drawer_t *d);
void chat_session_drawer_on_pick(chat_session_drawer_t *d, chat_drawer_pick_cb_t cb, void *ud);
void chat_session_drawer_on_new(chat_session_drawer_t *d, chat_drawer_new_cb_t cb, void *ud);
```

- [ ] **Step 2: REST fetch**

Use `esp_http_client_perform()` against `http://<dragon_host>:3502/api/v1/sessions?limit=10`. Parse response JSON with cJSON, populate a local array of `chat_session_t`. Rebuild drawer rows.

- [ ] **Step 3: Mode gradient spine**

Under the drawer's top edge (at y=96, 2px tall), paint four equal-width segments in mode colors (Local / Hybrid / Cloud / Claw). Pure lv_obj divs.

- [ ] **Step 4: Build + commit**

```bash
idf.py build && git add main/chat_session_drawer.* && \
git commit -m "feat(chat): chat_session_drawer pull-down with mode spine (refs #<TT_ISSUE>)"
```

---

## Phase 7 — Orchestrator rewrite

### Task 7.1: Replace ui_chat.c with orchestrator

**Files:**
- Modify: `TinkerTab/main/ui_chat.c` (rewrite, ~2500 LOC → ~350 LOC)
- Modify: `TinkerTab/main/ui_chat.h` (keep public API intact)

- [ ] **Step 1: Stash current ui_chat.c for reference**

```bash
cp main/ui_chat.c /tmp/ui_chat.old.c
```

- [ ] **Step 2: Rewrite ui_chat.c**

New structure:

```c
#include "ui_chat.h"
#include "chat_msg_store.h"
#include "chat_msg_view.h"
#include "chat_header.h"
#include "chat_input_bar.h"
#include "chat_suggestions.h"
#include "chat_session_drawer.h"
/* ... */

static lv_obj_t *s_overlay;
static chat_header_t          *s_hdr;
static chat_msg_view_t        *s_view;
static chat_input_bar_t       *s_input;
static chat_suggestions_t     *s_sugg;
static chat_session_drawer_t  *s_drawer;
static bool s_visible;

/* Callbacks wire header → drawer → pipeline */
static void on_back(void *ud)       { ui_chat_hide(); }
static void on_chev(void *ud)       { chat_session_drawer_show(s_drawer); }
static void on_plus(void *ud)       { /* create new session in current mode */ }
static void on_mode_lp(void *ud)    { /* cycle voice_mode, send config_update, patch session */ }
static void on_ball_tap(void *ud)   { /* start/stop voice recording */ }
static void on_text_submit(const char *t, void *ud) { /* voice_send_text(t) + add user bubble */ }
static void on_pick_session(const chat_session_t *s, void *ud) {
    /* atomic: config_update → load history → repaint header */
    voice_send_config_update(s->voice_mode, s->llm_model);
    chat_store_set_session(s);
    chat_view_refresh(s_view);
    chat_header_set_mode(s_hdr, s->voice_mode, s->llm_model);
    chat_session_drawer_hide(s_drawer);
}

lv_obj_t *ui_chat_create(void)
{
    if (s_overlay) { ui_chat_show(); return s_overlay; }
    s_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, 720, 1280);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(TH_BG), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);

    s_hdr    = chat_header_create(s_overlay, "Chat");
    s_view   = chat_msg_view_create(s_overlay);
    s_input  = chat_input_bar_create(s_overlay);
    s_sugg   = chat_suggestions_create(s_overlay);
    s_drawer = chat_session_drawer_create(s_overlay);

    chat_header_on_back(s_hdr, on_back, NULL);
    chat_header_on_chevron(s_hdr, on_chev, NULL);
    chat_header_on_plus(s_hdr, on_plus, NULL);
    chat_header_on_mode_long_press(s_hdr, on_mode_lp, NULL);
    chat_input_bar_on_ball_tap(s_input, on_ball_tap, NULL);
    chat_input_bar_on_text_submit(s_input, on_text_submit, NULL);
    chat_session_drawer_on_pick(s_drawer, on_pick_session, NULL);

    /* Initial state: show suggestions if store empty, else hide */
    if (chat_store_count() == 0) chat_suggestions_show(s_sugg);

    s_visible = true;
    return s_overlay;
}

void ui_chat_show(void) { if (s_overlay) { lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN); s_visible = true; } }
void ui_chat_hide(void) { if (s_overlay) { lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN); s_visible = false; } }
bool ui_chat_is_active(void) { return s_visible; }

/* Public push APIs — bridge to store + view */
void ui_chat_push_message(const char *role, const char *text)
{
    chat_msg_t m = { .type = CHAT_MSG_TEXT, .is_user = (strcmp(role, "user") == 0),
                     .timestamp = (uint32_t)time(NULL), .height_px = -1, .active = true };
    strncpy(m.text, text ? text : "", sizeof(m.text) - 1);
    chat_store_add(&m);
    if (s_view) chat_msg_view_refresh(s_view);
    if (s_sugg) chat_suggestions_hide(s_sugg);
}

void ui_chat_push_media(const char *url, const char *media_type, int w, int h, const char *alt)
{
    chat_msg_t m = { .type = CHAT_MSG_IMAGE, .is_user = false,
                     .timestamp = (uint32_t)time(NULL), .height_px = -1, .active = true };
    strncpy(m.text, alt ? alt : "", sizeof(m.text) - 1);
    strncpy(m.media_url, url ? url : "", sizeof(m.media_url) - 1);
    chat_store_add(&m);
    if (s_view) chat_msg_view_refresh(s_view);
}

void ui_chat_push_card(const char *title, const char *subtitle, const char *img, const char *desc) { /* analogous */ }
void ui_chat_push_audio_clip(const char *url, float duration_s, const char *label) { /* analogous */ }

void ui_chat_update_last_message(const char *text)
{
    chat_store_update_last_text(text);
    if (s_view) chat_msg_view_refresh(s_view);
}

void ui_chat_destroy(void)
{
    if (s_overlay) { lv_obj_del(s_overlay); s_overlay = NULL; }
    s_hdr = NULL; s_view = NULL; s_input = NULL; s_sugg = NULL; s_drawer = NULL;
    s_visible = false;
}
```

- [ ] **Step 3: Keep ui_chat.h unchanged**

Public API stays byte-identical — voice.c calls `ui_chat_push_message()` from WS handlers and won't notice the backing change.

- [ ] **Step 4: Build**

```bash
idf.py build 2>&1 | tail -6
```

Expected: succeeds. If link errors from removed functions, grep for callers and update.

- [ ] **Step 5: Commit**

```bash
git add main/ui_chat.c main/ui_chat.h
git commit -m "refactor(chat): rewrite ui_chat.c as orchestrator (refs #<TT_ISSUE>)"
```

---

## Phase 8 — Integration + on-device

### Task 8.1: Wire boot init

**Files:** Modify: `TinkerTab/main/main.c`

- [ ] **Step 1: Call chat_store_init() after ui_theme_init()**

```c
ui_theme_init();
chat_store_init();                // NEW
widget_store_init();
ui_home_create();
ui_voice_init();
```

- [ ] **Step 2: On `session_start` WS message, seed active session**

In `main/voice.c` `handle_text_message()` `"session_start"` branch, extract `voice_mode` + `llm_model` from the payload (Dragon now emits them) and call:

```c
chat_session_t s = {0};
strncpy(s.session_id, session_id_from_json, sizeof(s.session_id) - 1);
s.voice_mode = voice_mode_from_json;
strncpy(s.llm_model, llm_model_from_json, sizeof(s.llm_model) - 1);
s.valid = true;
chat_store_set_session(&s);
```

- [ ] **Step 3: Build**

```bash
idf.py build 2>&1 | tail -3
```

- [ ] **Step 4: Commit**

```bash
git add main/main.c main/voice.c
git commit -m "feat(chat): wire session init on boot + session_start WS (refs #<TT_ISSUE>)"
```

### Task 8.2: Rail "chat" opens the rewritten chat

**Files:** Modify: `TinkerTab/main/ui_home.c`

- [ ] **Step 1: Verify rail_threads_cb is already calling ui_chat_create**

From prior commit `75e4650` it should be. If any change needed, edit the rail handler to call `ui_chat_show()` when s_overlay exists, else `ui_chat_create()`.

- [ ] **Step 2: Build**

### Task 8.3: Full-screen flash + verify

**Files:** none

- [ ] **Step 1: Full clean build**

```bash
idf.py fullclean && idf.py build 2>&1 | tail -6
```

- [ ] **Step 2: Flash**

```bash
idf.py -p /dev/ttyACM0 flash 2>&1 | tail -3
```

- [ ] **Step 3: Wait for boot + take home screenshot**

```bash
sleep 22
export TOKEN=05eed3b13bf62d92cfd8ac424438b9f2
curl -s -H "Authorization: Bearer $TOKEN" -o .superpowers/brainstorm/ui-overhaul-v4/shots/chat-v4c-home.jpg http://192.168.70.128:8080/screenshot
```

- [ ] **Step 4: Navigate to chat + screenshot**

```bash
curl -s -H "Authorization: Bearer $TOKEN" -X POST "http://192.168.70.128:8080/navigate?screen=chat"
sleep 2
curl -s -H "Authorization: Bearer $TOKEN" -o .superpowers/brainstorm/ui-overhaul-v4/shots/chat-v4c-empty.jpg http://192.168.70.128:8080/screenshot
```

- [ ] **Step 5: Send a text message via /chat, then screenshot**

```bash
curl -s -H "Authorization: Bearer $TOKEN" -X POST http://192.168.70.128:8080/chat -d '{"text":"hi tinker"}'
sleep 8
curl -s -H "Authorization: Bearer $TOKEN" -o .superpowers/brainstorm/ui-overhaul-v4/shots/chat-v4c-first-turn.jpg http://192.168.70.128:8080/screenshot
```

- [ ] **Step 6: Visual pixel-diff against 09-chat-pixelperfect.html scene 1**

Manual: open both images side by side. Verify:
- Header 96 h, title in Fraunces italic
- Mode chip top-right with correct color
- 140 × 2 amber accent under header
- Bubbles at correct radius / color
- Say-pill 108 h with 84 amber disc
- Timestamps in JetBrains Mono

- [ ] **Step 7: Test drawer**

```bash
curl -s -H "Authorization: Bearer $TOKEN" -X POST http://192.168.70.128:8080/touch -d '{"x":220,"y":50,"action":"tap"}'  # chevron
sleep 2
curl -s -H "Authorization: Bearer $TOKEN" -o .superpowers/brainstorm/ui-overhaul-v4/shots/chat-v4c-drawer.jpg http://192.168.70.128:8080/screenshot
```

Verify the mode gradient spine + colored session rows.

- [ ] **Step 8: Mode switch mid-session**

Long-press mode chip. Fires `config_update`. Verify chip repaints + toast.

- [ ] **Step 9: Commit screenshots**

```bash
git add .superpowers/brainstorm/ui-overhaul-v4/shots/chat-v4c-*.jpg
git commit -m "docs(shots): chat v4·C device screenshots (refs #<TT_ISSUE>)"
```

### Task 8.4: Push branch + open PR

**Files:** none

- [ ] **Step 1: Push**

```bash
git push -u origin feat/chat-v4c
```

- [ ] **Step 2: Open PR**

```bash
gh pr create --title "feat(chat): v4·C Ambient redesign" \
  --body "Implements docs/superpowers/specs/2026-04-19-chat-v4c-design.md. Closes #<TT_ISSUE>."
```

---

## Phase 9 — Memory + docs update

### Task 9.1: Update CLAUDE.md + memory

**Files:**
- Modify: `TinkerTab/CLAUDE.md` (Chat UI overhaul section)
- Modify: `/home/rebelforce/.claude/projects/-home-rebelforce/memory/project_ui_overhaul_status.md`

- [ ] **Step 1: In TinkerTab/CLAUDE.md under "Key Fixes" add entry**

```
- **Chat v4·C Ambient:** Modular rewrite (chat_msg_store + 5 modules + orchestrator). Session-scoped message store with mode-per-session (voice_mode + llm_model stamped on every session). Pixel-matched to v4·C home (108/84 say-pill, 140×2 accent, Fraunces italic title). Spec: docs/superpowers/specs/2026-04-19-chat-v4c-design.md.
```

- [ ] **Step 2: Update project_ui_overhaul_status.md**

Append to "What shipped":
```
- Chat v4·C Ambient shipped on feat/chat-v4c (commit <SHA>) — spec docs/superpowers/specs/2026-04-19-chat-v4c-design.md, device-verified screenshots in .superpowers/brainstorm/ui-overhaul-v4/shots/chat-v4c-*.jpg
```

- [ ] **Step 3: Commit**

```bash
git add CLAUDE.md
git commit -m "docs: chat v4·C in CLAUDE.md (refs #<TT_ISSUE>)"
git push
```

---

## Self-review checklist (run before handoff)

- Spec §1.1 decisions → Task 3.1 (store) + Task 4.2 (input) + Task 6.1 (drawer) + Task 7.1 (orchestrator)
- Spec §2.2 Dragon changes → Tasks 1.1 / 1.2 / 1.3 / 1.4
- Spec §3.1 colors → used directly via TH_* tokens in every widget
- Spec §3.2 typography → Tasks 2.1 + chat_header/chat_input_bar font assignments
- Spec §3.3 spacing → matches Task 4.1 (header 96h, 140×2 accent) + 4.2 (pill 108/84) + 4.3
- Spec §4 scenes → screenshots in Task 8.3
- Spec §5.2 voice-from-chat → chat_input_bar_on_ball_tap wires to voice_start_listening
- Spec §5.4 drawer atomic → on_pick_session fires config_update + set_session + set_mode + hide
- Spec §6 object budget → chat_msg_view pool sized at 12
- Spec §7 migration → old build_home_panel deleted in Task 7.1
- Spec §8 risks → addressed by TDD on msg_store + LVGL lock discipline throughout
- Spec §9 testing → Task 3.1 unit + Task 8.3 integration on-device
