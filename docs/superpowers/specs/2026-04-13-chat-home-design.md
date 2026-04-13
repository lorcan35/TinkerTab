# Chat Home Screen — Design Spec

**Date:** 2026-04-13
**Scope:** Replace single-conversation Chat with two-screen flow: Chat Home (session list) → Conversation view
**File:** `main/ui_chat.c` (complete rewrite of create/show flow)
**Constraint:** Max ~55 LVGL objects per screen, manual Y positioning, 720x1280 portrait

## Architecture

Two sub-screens within the Chat overlay:
1. **Chat Home** — session list + quick actions + model picker
2. **Conversation** — current chat bubbles (existing, improved)

Navigation: `Nav "Chat" → Chat Home → tap session → Conversation → back → Chat Home → back → Home`

Both are children of the same `s_overlay` to avoid the overlay rendering issues.

## Chat Home Screen Layout (720x1280)

```
┌─────────────────────────── 720px ────────────────────────────┐
│ [←]  Conversations          [mode badge]  [+ New]            │ 60px topbar
├──────────────────────────────────────────────────────────────┤
│  Model: [claude-3.5-haiku ▼]     ● Ready                    │ 44px model bar
├──────────────────────────────────────────────────────────────┤
│  🔍 Search  |  🌐 Web  |  ⏰ Timer  |  💭 Remember          │ 48px quick actions
├──────────────────────────────────────────────────────────────┤
│  ┌─────────────────────────────────────────────────────┐    │
│  │ Today 16:29                               Cloud  4→ │    │ Session card
│  │ "I'm an AI assistant capable of helping you..."     │    │ ~100px each
│  └─────────────────────────────────────────────────────┘    │
│  ┌─────────────────────────────────────────────────────┐    │
│  │ Today 16:14                              Hybrid  2→ │    │
│  │ "Hello! How can I assist you today? 😊"             │    │
│  └─────────────────────────────────────────────────────┘    │
│  ┌─────────────────────────────────────────────────────┐    │
│  │ Apr 12, 22:18                             Local  6→ │    │
│  │ "LNG prices soaring for Europe and Asia..."         │    │
│  └─────────────────────────────────────────────────────┘    │
│                        (scrollable)                          │
├──────────────────────────────────────────────────────────────┤
│  💡 Tinker remembers: "Emile likes sushi" + 9 more          │ 40px memory bar
├──────────────────────────────────────────────────────────────┤
│  [🎵 mic]  [  Type a message...  ]  [Send]                  │ 80px input bar
└──────────────────────────────────────────────────────────────┘
```

## Components

### 1. Top Bar (60px)
- Back button → Home
- "Conversations" title
- Mode badge (tappable cycle: Local/Hybrid/Cloud)
- "+ New" button → creates session, enters Conversation immediately

### 2. Model Bar (44px)
- Dropdown showing current LLM model
- Models populated from Dragon `/api/v1/backends` endpoint
- Local models: qwen3:0.6b, qwen3:1.7b, qwen3:4b
- Cloud models: claude-3.5-haiku, claude-sonnet-4, gpt-4o-mini
- Only enabled when mode = Cloud (2)
- Connection status dot + state text on right side

### 3. Quick Actions Bar (48px)
- 4 horizontal pill buttons:
  - 🌐 "Web" → sends "Search the web for [prompt]" 
  - ⏰ "Timer" → sends "Set a timer for [duration]"
  - 💭 "Remember" → sends "Remember that [fact]"
  - 🔍 "Search" → searches through session history
- Tapping opens a mini prompt inline, then sends to current/new session

### 4. Session List (scrollable, ~800px)
- Cards loaded from Dragon: `GET /api/v1/sessions?limit=20`
- Each card shows:
  - Date/time (relative: "Today 16:29", "Yesterday", "Apr 12")
  - Mode badge (Local/Hybrid/Cloud with color)
  - Message count with arrow (→)
  - Last message preview (truncated 1 line)
  - Session title if set
- Tap card → enter Conversation view with that session
- Swipe left on card → delete option

### 5. Memory Preview Bar (40px)
- Shows 1 random fact from Dragon memory
- "Tinker remembers: {fact}" + "+ N more"
- Tappable → shows all facts in a popup

### 6. Input Bar (80px)
- Same as current: mic button + textarea + send button
- Sending from Chat Home creates new session automatically
- Quick way to start a conversation without tapping "+ New" first

## Conversation View (existing, improved)

When user taps a session card, transition to the current Chat conversation view:
- Load session messages from Dragon: `GET /api/v1/sessions/{id}/messages`
- Show existing bubbles
- Back button returns to Chat Home (not Home screen)
- Status bar, mode badge, tool indicators — all existing features preserved

## Data Flow

1. Chat Home opens → `GET /api/v1/sessions?limit=20` → render cards
2. User taps card → `GET /api/v1/sessions/{id}/messages` → render bubbles
3. User sends message → existing `voice_send_text()` flow
4. User taps "+ New" → `POST /api/v1/sessions` → enter Conversation
5. User taps back from Conversation → return to Chat Home, refresh list
6. Model change → `voice_send_config_update()` → badge updates

## LVGL Object Budget

Chat Home objects: ~30
- Topbar: 5 (container, back btn, title, mode badge, new btn)
- Model bar: 3 (container, dropdown label, status)
- Quick actions: 5 (container, 4 buttons)
- Session cards: 5 × 3 = 15 (5 visible cards, each = card + preview + meta)
- Memory bar: 2 (container, label)
- Input bar: 4 (container, mic, textarea, send)
Total: ~34 objects — within 55 limit

Conversation view: ~25 (existing, already within budget)

## Implementation Order

1. Refactor ui_chat.c: split into `show_chat_home()` and `show_conversation(session_id)`
2. Implement session card rendering from Dragon API
3. Add model bar with dropdown
4. Add quick actions bar
5. Add memory preview bar  
6. Wire up transitions (card tap → conversation, back → chat home)
7. Test full flow

## Verification

1. Open Chat → see session list with real sessions from Dragon
2. Tap a session → see previous messages load
3. Send a message → response streams in
4. Back → return to Chat Home, session updated in list
5. "+ New" → fresh conversation
6. Change model → verify Dragon config updates
7. Quick action "Web" → new session with web search
8. Memory bar shows real facts from Dragon
