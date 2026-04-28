# TinkerTab OS — UI/UX Design Specification v1.0

**Target Device:** M5Stack Tab5 (ESP32-P4, 720x1280 portrait touchscreen, 5")
**Rendering:** Local React web app on Dragon Q6A, streamed as MJPEG via CDP Page.startScreencast
**Input:** Capacitive touch (forwarded as CDP mouse events), voice (Moonshine STT on Dragon)
**Resolution:** 720 x 1280 px, portrait-locked
**DPI:** ~293 PPI (treat as 2x — design at 360x640 logical points, render at 720x1280 physical)
**Frame Rate:** 10-15 fps MJPEG stream (design for clarity over animation smoothness)
**Touch Latency:** ~80-150ms round trip (design for visual feedback on touch-down, not release)

---

## Table of Contents

1. [Boot Sequence & Splash Screen](#1-boot-sequence--splash-screen)
2. [Home Screen / Launcher](#2-home-screen--launcher)
3. [Notification Panel](#3-notification-panel)
4. [Chat (Command Center)](#4-chat-command-center)
5. [Browse (Browser App)](#5-browse-browser-app)
6. [App Gallery](#6-app-gallery)
7. [Settings](#7-settings)
8. [Camera / Vision](#8-camera--vision)
9. [Task Manager](#9-task-manager)
10. [Voice Assistant (System Overlay)](#10-voice-assistant-system-overlay)
11. [Gesture System (Global)](#11-gesture-system-global)
12. [Navigation & App Lifecycle](#12-navigation--app-lifecycle)
13. [Design System](#13-design-system)
14. [First-Run / Onboarding](#14-first-run--onboarding)

---

## 1. Boot Sequence & Splash Screen

### 1.1 Power-On to Home Screen Timeline

```
t=0s     Power button pressed
t=0-2s   ESP32-P4 hardware init (black screen)
t=2s     Tab5 firmware boots, shows BOOT LOGO (stored in flash)
t=2-5s   Tab5 connects to Dragon WiFi, establishes MJPEG stream
t=5s     Dragon sends first frame — SPLASH SCREEN begins
t=5-8s   Splash screen with animated loading (rendered by Dragon)
t=8s     Home screen appears (or First-Run if unconfigured)
```

### 1.2 Boot Logo (ESP32-P4 Local, t=2-5s)

Rendered locally by Tab5 firmware, not streamed. Static image stored in flash partition.

```
┌──────────────────────────────────┐
│                                  │
│                                  │
│                                  │
│                                  │
│                                  │
│            ╔══════╗              │
│            ║  TC  ║              │
│            ╚══════╝              │
│                                  │
│         TINKERCLAW               │
│                                  │
│                                  │
│                                  │
│                                  │
│      Connecting to Dragon...     │
│      ░░░░░░░░░░░░░░░░░░░░       │
│                                  │
│                                  │
└──────────────────────────────────┘
```

**Layout:**
- Full black background (#000000)
- TinkerClaw logo: centered, 120x120px, white monogram "TC" inside rounded square
- Brand name "TINKERCLAW" centered below logo, 24px, font-weight 300, white (#FFFFFF), letter-spacing 8px
- Status text: 16px, #666666, centered, 200px from bottom
- Progress bar: 240px wide, 4px tall, centered, 180px from bottom

**Status Text States:**
- `Connecting to Dragon...` — searching for Dragon on local network
- `Stream starting...` — WiFi connected, MJPEG handshake in progress
- `Dragon not found` — after 10s timeout, with "Tap to retry" below
- `Wrong network?` — WiFi connected but Dragon unreachable after 15s

**Progress Bar States:**
- Indeterminate animation (sliding highlight) during connection
- Fills to 100% once MJPEG stream is established
- On error: bar turns red (#EF4444), stops animating

### 1.3 Splash Screen (Dragon-Rendered, t=5-8s)

First frame streamed from Dragon. The Dragon web app is loading its React bundle and services.

```
┌──────────────────────────────────┐
│                                  │
│                                  │
│                                  │
│                                  │
│            ╔══════╗              │
│            ║  TC  ║              │
│            ╚══════╝              │
│                                  │
│         TINKERCLAW               │
│                                  │
│    ● Dragon connected            │
│    ● Loading AI models...        │
│    ○ Starting PingApps engine    │
│    ○ Ready                       │
│                                  │
│      ████████████░░░░░░░░        │
│                                  │
│                                  │
└──────────────────────────────────┘
```

**Layout:**
- Same logo and brand treatment as boot logo for visual continuity
- Checklist items: left-aligned block, centered horizontally, 14px, mono-spaced
  - `●` = completed (green #22C55E)
  - `○` = pending (gray #444444)
  - Active item has a subtle pulse animation
- Progress bar: 280px wide, 6px tall, rounded-full, fills proportionally

**Checklist Steps:**
1. `Dragon connected` — immediately checked when splash starts
2. `Loading AI models...` — Ollama model warm-up (Qwen 3.5 4B)
3. `Starting PingApps engine` — PingOS Gateway initialization
4. `Ready` — all services healthy

**Timing:** Each step transitions as the real backend reports readiness via WebSocket. If a step takes >5s, append `(this may take a moment)` in gray below it.

**Error Handling:** If any step fails, the checklist item turns red with an error message. A "Retry" button (280px wide, 48px tall) appears below the progress bar.

### 1.4 Transition to Home Screen

Once all checklist items are green, the splash holds for 500ms, then fades out (300ms opacity transition) into the home screen. Total boot-to-usable target: under 10 seconds.

---

## 2. Home Screen / Launcher

### 2.1 Full Layout

```
┌──────────────────────────────────┐  0px
│ 9:41    ⚡ 🐉 ● ▼ WiFi  85%     │  STATUS BAR (40px)
├──────────────────────────────────┤  40px
│                                  │
│                                  │
│                                  │
│           9:41                   │
│        Wednesday                 │
│       March 26, 2026             │
│                                  │
│    "Good morning. Dragon is      │
│     running Qwen 3.5 locally."   │
│                                  │
│  ┌──────┐ ┌──────┐ ┌──────┐     │
│  │ Chat │ │Browse│ │Gallery│    │
│  │  💬  │ │  🌐  │ │  📦  │     │
│  └──────┘ └──────┘ └──────┘     │
│                                  │
│  ┌──────┐ ┌──────┐ ┌──────┐     │
│  │ Cam  │ │Tasks │ │ Set  │     │
│  │  📷  │ │  ⚙️  │ │  ⚙   │     │
│  └──────┘ └──────┘ └──────┘     │
│                                  │
│  ┌──────┐ ┌──────┐ ┌──────┐     │
│  │ YT   │ │ Ali  │ │ Maps │     │
│  │  ▶️  │ │  🛒  │ │  🗺   │     │
│  └──────┘ └──────┘ └──────┘     │
│                                  │
│  ┌──────┐ ┌──────┐ ┌──────┐     │
│  │ GPT  │ │Gemini│ │Claude│     │
│  │  🤖  │ │  ✨  │ │  🟠  │     │
│  └──────┘ └──────┘ └──────┘     │
│                                  │
├──────────────────────────────────┤  1200px
│  💬Chat  🌐Browse  📦Apps  ⚙Set │  DOCK (80px)
└──────────────────────────────────┘  1280px
```

### 2.2 Status Bar (0-40px)

Fixed at top of every screen in the OS. 40px tall, semi-transparent black (#000000CC) with blur backdrop.

```
┌──────────────────────────────────┐
│ 9:41    ⚡ 🐉 ● ▼ WiFi  85%     │
└──────────────────────────────────┘
 ^time   ^charging ^dragon ^ai ^wifi ^battery
```

**Left cluster (left-aligned, 16px padding):**
- **Time:** 14px, font-weight 600, white. Format: `H:MM` (24h or 12h per settings)

**Right cluster (right-aligned, 16px padding, 8px gap between icons):**
- **Charging indicator:** lightning bolt icon, yellow (#FACC15), only visible when charging
- **Dragon status:** dragon icon (🐉 as SVG), color-coded:
  - Green (#22C55E): connected, all services healthy
  - Yellow (#FACC15): connected, some services loading
  - Red (#EF4444): disconnected
  - Gray (#666666): never connected
- **AI tier indicator:** filled circle, color = active tier:
  - Blue (#3B82F6): local (Tier 2, Dragon Ollama)
  - Purple (#8B5CF6): browser AI (Tier 3, PingApps)
  - Orange (#F97316): cloud (Tier 4, OpenRouter)
  - Gray (#666666): idle / no active inference
- **WiFi:** standard WiFi icon, signal strength mapped to 0-3 bars
- **Battery:** percentage + icon, color shifts:
  - White: >20%
  - Yellow (#FACC15): 10-20%
  - Red (#EF4444): <10%

**Tap behavior:** Tapping the status bar opens the Notification Panel (same as swipe-down).

### 2.3 Clock & Greeting Area (40-360px)

```
┌──────────────────────────────────┐
│                                  │
│           9:41                   │  <- 72px, font-weight 200, white
│        Wednesday                 │  <- 18px, font-weight 400, #999
│       March 26, 2026             │  <- 16px, font-weight 300, #777
│                                  │
│    "Good morning. Dragon is      │  <- 14px, italic, #22C55E
│     running Qwen 3.5 locally."   │
│                                  │
└──────────────────────────────────┘
```

**Clock:** Centered, 72px font-size, font-weight 200 (ultralight), white (#FFFFFF). Updates every minute.

**Date:** Two lines below clock:
- Day of week: 18px, font-weight 400, #999999
- Full date: 16px, font-weight 300, #777777

**AI Status Quote:** A one-line contextual message from Dragon's local AI, 14px, italic, color matches the AI tier that generated it. Updates on home screen load. Examples:
- `"Good morning. Dragon is running Qwen 3.5 locally."` (green, local)
- `"3 PingApps active. ChatGPT session healthy."` (purple, browser)
- `"All quiet. Tap Chat to start."` (gray, idle)

The quote area is tappable — tapping it opens the Chat app.

### 2.4 App Grid (360-1120px)

3 columns, up to 4 visible rows. Scrollable vertically if more than 12 apps installed.

**Grid Math:**
- Grid area: 720px wide, 760px tall (360px to 1120px)
- Column width: 720 / 3 = 240px per cell
- Row height: 760 / 4 = 190px per cell
- Icon size: 72x72px, centered in cell
- Label: 13px, font-weight 400, white, centered below icon, max 2 lines, ellipsis overflow
- Badge (notification count): 20px diameter red (#EF4444) circle, top-right of icon, white text, 11px font

**Cell Layout:**
```
┌─────────────────────┐
│                      │
│      ┌────────┐      │
│      │  ICON  │ (3)  │  <- 72x72 icon, badge top-right
│      │  72x72 │      │
│      └────────┘      │
│       App Name       │  <- 13px label
│                      │
└─────────────────────┘
        240 x 190px
```

**Default App Order (page 1):**
| Row | Col 1 | Col 2 | Col 3 |
|-----|-------|-------|-------|
| 1 | Chat | Browse | Gallery |
| 2 | Camera | Tasks | Settings |
| 3 | YouTube | AliExpress | Maps |
| 4 | ChatGPT | Gemini | Claude |

**Icon Design:**
Each app has a rounded-square icon (border-radius 16px) with a gradient background and a centered glyph. Icons are rendered as SVG for crispness at any DPI.

| App | Background Gradient | Glyph |
|-----|-------------------|-------|
| Chat | #3B82F6 to #1D4ED8 | Speech bubble |
| Browse | #06B6D4 to #0891B2 | Globe |
| Gallery | #8B5CF6 to #7C3AED | Grid squares |
| Camera | #F97316 to #EA580C | Camera |
| Tasks | #64748B to #475569 | List with checkmarks |
| Settings | #6B7280 to #4B5563 | Gear |
| YouTube | #EF4444 to #DC2626 | Play triangle |
| AliExpress | #F97316 to #C2410C | Shopping bag |
| Maps | #22C55E to #16A34A | Pin |
| ChatGPT | #10B981 to #059669 | OpenAI swirl |
| Gemini | #3B82F6 to #6366F1 | Sparkle |
| Claude | #F97316 to #EA580C | Anthropic mark |

**Gestures on Grid:**
- **Tap:** Launch app
- **Long press (500ms):** Enter rearrange mode — all icons start jiggling (CSS animation: slight rotation oscillation, 2deg, 150ms). A small "X" delete button appears on each non-system app. Tap empty area or press "Done" to exit.
- **Drag (in rearrange mode):** Move icon to new position. Other icons shift to make room.
- **Vertical scroll:** If more than 12 apps, scroll the grid. Page dots appear at bottom of grid area.

### 2.5 Dock (1200-1280px)

Fixed bottom bar, 80px tall, semi-transparent black (#000000CC) with blur backdrop. Contains 4 pinned app shortcuts.

```
┌──────────────────────────────────┐
│  💬Chat  🌐Browse  📦Apps  ⚙Set │
└──────────────────────────────────┘
   180px    180px    180px   180px
```

**Dock Item Layout:**
- 4 items, each 180px wide (720/4)
- Icon: 28px, centered
- Label: 11px, font-weight 500, centered below icon
- Active indicator: 4px wide dot below label, accent color of that app
- Tap: launch app (or focus if already open)

**Default Dock:**
1. Chat (always pinned, cannot be removed)
2. Browse
3. Apps (Gallery)
4. Settings

Dock items are configurable in Settings except Chat which is permanently pinned to position 1.

### 2.6 Wallpaper

- Default: deep space gradient (#0A0A1A at top to #1A1A2E at bottom) with subtle dot grid pattern (1px dots, 40px spacing, #FFFFFF08)
- Custom wallpapers can be set from Settings or Camera app
- Wallpaper is rendered behind all home screen elements with a 40% dark overlay to ensure text readability
- Parallax: slight vertical parallax shift (20px range) when scrolling the app grid, for depth

---

## 3. Notification Panel

### 3.1 Trigger & Animation

**Open:** Swipe down from top 60px of screen (the status bar zone), OR tap the status bar. Panel slides down from top with spring easing (300ms).

**Close:** Swipe up on the panel, tap outside the panel (on the dimmed background), or swipe down again.

**Background:** When panel is open, content behind dims to 50% black overlay.

### 3.2 Full Layout

```
┌──────────────────────────────────┐
│ 9:41    ⚡ 🐉 ● ▼ WiFi  85%     │  STATUS BAR (40px)
├──────────────────────────────────┤
│                                  │
│  ┌─WiFi─┐ ┌─Bright┐ ┌─Voice─┐  │  QUICK TOGGLES ROW 1
│  │  ON   │ │  70%  │ │  ON   │  │
│  └───────┘ └───────┘ └───────┘  │
│                                  │
│  ┌─AI Tier┐ ┌─DND──┐ ┌─Theme─┐ │  QUICK TOGGLES ROW 2
│  │ Local  │ │  OFF │ │ Dark  │  │
│  └────────┘ └──────┘ └───────┘  │
│                                  │
│  ═══ Brightness Slider ════════  │  BRIGHTNESS SLIDER
│                                  │
│ ┌────────────────────────────┐   │  NOTIFICATION 1
│ │ 🤖 ChatGPT PingApp         │   │
│ │ Session refreshed. 47 msgs │   │
│ │ remaining today.       2m  │   │
│ └────────────────────────────┘   │
│                                  │
│ ┌────────────────────────────┐   │  NOTIFICATION 2
│ │ 💬 Chat                    │   │
│ │ Gemini finished: "Here are │   │
│ │ the top 5 results..."  5m  │   │
│ └────────────────────────────┘   │
│                                  │
│ ┌────────────────────────────┐   │  NOW PLAYING (conditional)
│ │ ▶ YouTube — Lo-fi beats    │   │
│ │ advancement radio    advancement │   │
│ │ advancement advancement ▐▐  advancement advancement│   │
│ └────────────────────────────┘   │
│                                  │
│         Clear All                │
│                                  │
├── ── ── ── ── ── ── ── ── ── ──┤
│       (dimmed home screen)       │
└──────────────────────────────────┘
```

### 3.3 Quick Toggles

Two rows of 3 toggles each. Each toggle is a 208x64px rounded rectangle (border-radius 12px).

**Toggle States:**
- **Active:** filled with accent color, white icon + label
- **Inactive:** #1E1E2E background, #888888 icon + label
- **Tap:** Toggle on/off (instant state change)
- **Long press:** Open related settings page (e.g., long-press WiFi opens WiFi settings)

**Toggle Definitions:**

| Toggle | Icon | Active State | Inactive State | Long Press |
|--------|------|-------------|----------------|------------|
| WiFi | WiFi bars | Blue, shows SSID | Gray, "Off" | WiFi settings |
| Brightness | Sun | Yellow, shows % | Gray, "Auto" | Display settings |
| Voice | Microphone | Green, "On" | Gray, "Off" | Sound settings |
| AI Tier | Brain | Tier color, tier name | Gray, "Auto" | AI preferences |
| DND | Moon | Purple, "On" | Gray, "Off" | Notification settings |
| Theme | Circle (half) | Current theme color | Gray, "Auto" | Display settings |

**AI Tier Toggle Cycle:**
Tapping cycles through: Auto -> Local Only -> Browser Only -> Cloud Only -> Auto. Each state shows its corresponding color and label.

### 3.4 Brightness Slider

Full-width slider (688px, 16px padding each side), 40px touch target height, 4px visible track.

- Track background: #333333
- Filled portion: yellow gradient (#FACC15 to #FDE68A)
- Thumb: 24px white circle with shadow
- Dragging sends brightness commands to Tab5 via WebSocket in real-time
- Sun icon on left (dim), sun icon on right (bright)

### 3.5 Notifications List

Scrollable area below toggles and slider. Each notification is a card.

**Notification Card:**
```
┌────────────────────────────────────┐
│ [AppIcon] App Name              2m │  <- 13px, icon 20x20
│ Notification body text goes here   │  <- 14px, white, max 2 lines
│ and can wrap to a second line.     │
└────────────────────────────────────┘
```

- Card: 688px wide (16px margin each side), background #1A1A2E, border-radius 12px, padding 12px
- App icon: 20x20px rounded square, left of app name
- App name: 13px, font-weight 600, #AAAAAA
- Timestamp: 13px, #666666, right-aligned
- Body: 14px, font-weight 400, white, max 2 lines with ellipsis
- **Tap:** Opens the source app at the relevant context
- **Swipe left:** Dismiss notification (slides out with 200ms ease, red background revealed underneath with trash icon)
- **Swipe right:** Mark as read (slides out with blue background, checkmark icon)

**Notification Types:**
- **PingApp status:** session health, quota remaining, errors
- **Chat response:** AI finished a long-running query
- **System:** updates available, Dragon connection changes, low battery
- **App-specific:** any PingApp can push notifications

### 3.6 Now Playing Card (Conditional)

Only shown when a media PingApp (YouTube, Spotify, etc.) is actively playing audio/video.

```
┌────────────────────────────────────┐
│ ▶  Track/Video Title               │  <- 14px, white, bold
│    Artist / Channel Name           │  <- 13px, #999999
│ ──────●──────────────────────── 🔊 │  <- progress bar + volume
│        advancement ◀◀  ▐▐  ▶▶  advancement │  <- controls centered
└────────────────────────────────────┘
```

- Same card styling as notifications but with 16px padding
- Controls: Previous, Play/Pause, Next — 40x40px touch targets
- Progress bar: same style as brightness slider but blue (#3B82F6)
- Volume icon: tap to mute/unmute
- Tap anywhere else on card: open the media app

### 3.7 Clear All Button

Centered text button below notifications list: "Clear All" in 14px, #666666. Tap dismisses all notifications with a staggered fade-out animation (50ms delay between each card).

---

## 4. Chat (Command Center)

### 4.1 Overview

Chat is the primary app and the AI command center. It looks and feels like iMessage/WhatsApp but with AI superpowers. The user talks to TinkerClaw's AI, which can answer questions, execute tasks, spawn PingApps, control the browser, and more.

### 4.2 Full Layout

```
┌──────────────────────────────────┐
│ 9:41    ⚡ 🐉 ● ▼ WiFi  85%     │  STATUS BAR
├──────────────────────────────────┤
│ ← Chat            🐉 🔍  ⋮      │  APP BAR (56px)
├──────────────────────────────────┤
│                                  │
│              Today 9:30 AM       │  <- date separator
│                                  │
│  ┌────────────────────────┐      │
│  │ Search AliExpress for  │  ME  │  <- right-aligned, blue
│  │ ESP32-S3 dev boards    │      │
│  │ under $5               │      │
│  └────────────────────────┘      │
│                                  │
│      ┌────────────────────────┐  │
│  AI  │ 🔍 Searching via      │   │  <- left-aligned, dark
│  🟣  │ AliExpress PingApp... │   │
│      │                       │   │
│      │ ┌──────────────────┐  │   │
│      │ │ [Product Card 1] │  │   │  <- rich card embed
│      │ │ ESP32-S3 DevKit  │  │   │
│      │ │ $3.42  ★4.8  🛒  │  │   │
│      │ └──────────────────┘  │   │
│      │ ┌──────────────────┐  │   │
│      │ │ [Product Card 2] │  │   │
│      │ │ XIAO ESP32-S3    │  │   │
│      │ │ $4.99  ★4.9  🛒  │  │   │
│      │ └──────────────────┘  │   │
│      │                       │   │
│      │ Found 2 options. Want │   │
│      │ me to compare specs?  │   │
│      └────────────────────────┘  │
│                                  │
├──────────────────────────────────┤
│ [Compare] [Add to cart] [More]   │  QUICK ACTIONS (48px)
├──────────────────────────────────┤
│ ┌────────────────────────┐  🎤   │  INPUT BAR (56px)
│ │ Message TinkerClaw...  │       │
│ └────────────────────────┘       │
└──────────────────────────────────┘
```

### 4.3 App Bar (56px)

```
┌──────────────────────────────────┐
│ ←  Chat               🐉 🔍  ⋮  │
└──────────────────────────────────┘
```

- **Back arrow (←):** 48x48px touch target, returns to home screen
- **Title "Chat":** 20px, font-weight 600, white
- **Dragon icon (🐉):** 24x24px, shows current AI connection status (same color coding as status bar)
- **Search (🔍):** 48x48px, opens conversation search overlay
- **Overflow (⋮):** 48x48px, opens menu: New Conversation, Conversation History, Export, Settings

### 4.4 Message Area (Scrollable)

**Date Separators:**
- Centered pill: `Today 9:30 AM` or `Yesterday` or `Mar 24, 2026`
- Background: #1A1A2E, border-radius: 12px, padding: 4px 12px
- Text: 12px, #888888

**User Messages (Right-Aligned):**
```
                    ┌────────────────────────┐
                    │ Message text here      │
                    │ that can wrap to       │
                    │ multiple lines         │
                    └────────────────────────┘
                                   9:41 AM ✓
```

- Background: #3B82F6 (blue)
- Text: 15px, white, font-weight 400
- Border-radius: 16px 16px 4px 16px (squared bottom-right corner)
- Max width: 85% of screen (612px)
- Min width: fit content
- Padding: 12px 16px
- Timestamp: 11px, #FFFFFF88, right-aligned below bubble
- Delivery indicator: `✓` sent, `✓✓` received, `⏳` pending

**AI Messages (Left-Aligned):**
```
┌────────────────────────┐
│ AI response text here  │
│ with potentially long  │
│ formatted content      │
└────────────────────────┘
🟢 Local 9:42 AM
```

- Background: #1E1E2E (dark gray)
- Text: 15px, #E5E5E5, font-weight 400
- Border-radius: 16px 16px 16px 4px (squared bottom-left corner)
- Max width: 85% of screen (612px)
- Padding: 12px 16px

**AI Tier Badge:**
Below each AI message, left-aligned:
- Colored circle (8px) + tier label + timestamp
- Colors: Green (#22C55E) = Local, Purple (#8B5CF6) = Browser, Orange (#F97316) = Cloud
- Labels: `Local`, `ChatGPT`, `Gemini`, `Claude`, `OpenRouter`
- Font: 11px, color matches tier

**Typing Indicator:**
When AI is processing, show a left-aligned bubble with animated dots:
```
┌──────────┐
│ ● ● ●    │  <- dots pulse sequentially, 300ms offset
└──────────┘
🟢 Thinking...
```

**Streaming Responses:**
For real-time token streaming (local Ollama and OpenRouter), text appears word by word in the AI bubble. A blinking cursor (|) appears at the end of the text during streaming.

### 4.5 Rich Response Types

**Product Card:**
```
┌────────────────────────────────┐
│ ┌────────┐                     │
│ │ [IMG]  │ Product Name        │
│ │ 80x80  │ $3.42  ★4.8 (312)  │
│ │        │ Free shipping       │
│ └────────┘                     │
│           [View] [Add to Cart] │
└────────────────────────────────┘
```
- Card: #252536 background, border-radius 12px, padding 12px
- Image: 80x80px, rounded 8px, left
- Title: 14px, white, font-weight 500, max 2 lines
- Price: 16px, #22C55E (green), font-weight 700
- Rating: 13px, #FACC15 (yellow star) + #999 count
- Action buttons: 32px tall, border-radius 8px, font 13px

**Link Preview Card:**
```
┌────────────────────────────────┐
│ ┌────────────────────────────┐ │
│ │ [Preview Image / Favicon]  │ │
│ └────────────────────────────┘ │
│ Page Title                     │
│ domain.com                     │
│ Brief description text...      │
└────────────────────────────────┘
```
- Preview image: full width of card, 160px tall, rounded top corners
- Domain: 12px, #888888

**Action Button Group:**
```
┌──────────┐ ┌──────────┐ ┌──────────┐
│  Open    │ │  Share   │ │  Save    │
└──────────┘ └──────────┘ └──────────┘
```
- Inline buttons within AI message bubble
- Background: #FFFFFF15, border-radius 8px, padding 8px 16px
- Text: 13px, white
- Tap: executes the action (may spawn another app)

**Code Block:**
```
┌────────────────────────────────┐
│ ```python                  📋  │
│ def hello():                   │
│     print("Hello TinkerClaw")  │
│ ```                            │
└────────────────────────────────┘
```
- Background: #0D0D1A, border-radius 8px, padding 12px
- Font: 13px, monospace (JetBrains Mono or Fira Code)
- Copy button (📋): top-right, 32x32px
- Syntax highlighting via lightweight highlighter

**Image Response:**
- Inline image, max width 85%, border-radius 12px
- Tap to open in Camera/Vision gallery viewer
- Long press: save, share, or "Ask AI about this image"

### 4.6 Quick Actions Bar (48px)

Horizontal scrollable row of pill-shaped action buttons above the input bar. Context-sensitive — changes based on the last AI response.

```
┌──────────────────────────────────┐
│ [Compare] [Add to cart] [More ▶] │
└──────────────────────────────────┘
```

- Pills: background #1E1E2E, border 1px #333, border-radius 20px, padding 8px 16px
- Text: 13px, #CCCCCC
- Horizontal scroll with momentum, no scrollbar visible
- Fade-out gradient on right edge to indicate more items

**Default Quick Actions (when no context):**
- `Search the web`
- `What can you do?`
- `Open Browse`
- `Check PingApp status`
- `Take a screenshot`

**Contextual Quick Actions (examples):**
- After product search: `Compare`, `Add to cart`, `See more`, `Different search`
- After code generation: `Run this`, `Explain`, `Modify`, `Copy`
- After error: `Retry`, `Try different model`, `Report bug`

### 4.7 Input Bar (56px + keyboard)

```
┌──────────────────────────────────┐
│ ┌──────────────────────────┐ 🎤  │
│ │ Message TinkerClaw...    │     │
│ └──────────────────────────┘     │
└──────────────────────────────────┘
```

**Text Input:**
- Background: #1A1A2E, border-radius 24px, padding 12px 16px
- Placeholder: "Message TinkerClaw..." in #666666
- Text: 15px, white
- Expands vertically up to 4 lines, then scrolls internally
- When focused, on-screen keyboard slides up from bottom (320px tall)

**Voice Button (🎤):**
- 48x48px circle, right of text input
- Default state: #333333 background, white mic icon
- Tap and hold: activates voice input (see Section 10)
- Single tap: quick voice — records until silence detected (2s of silence), then sends
- While recording: button pulses red (#EF4444), ring animation around it

**Send Button:**
- Replaces voice button when text is entered
- Blue (#3B82F6) circle with white arrow-up icon
- 48x48px, appears with a quick scale animation (150ms)
- Tap: sends message, clears input, scrolls to bottom

**Attachment Button:**
- Small "+" icon, 32x32px, left of text input (inside the field)
- Tap opens attachment menu: Camera, Screenshot, File, PingApp action

### 4.8 Conversation Management

**New Conversation:**
- Via overflow menu or swipe right from left edge of Chat
- Conversations are stored on Dragon in a local SQLite database
- Each conversation has: title (auto-generated from first message), timestamp, message count, last AI tier used

**Conversation List:**
```
┌──────────────────────────────────┐
│ ← Conversations         + New   │
├──────────────────────────────────┤
│                                  │
│ ┌────────────────────────────┐   │
│ │ ESP32 dev board search     │   │
│ │ Found 2 options. Want...   │   │
│ │ 🟣 ChatGPT    Today 9:42  │   │
│ └────────────────────────────┘   │
│                                  │
│ ┌────────────────────────────┐   │
│ │ Recipe for hummus          │   │
│ │ Sure! Here's a classic...  │   │
│ │ 🟢 Local    Yesterday      │   │
│ └────────────────────────────┘   │
│                                  │
│ ┌────────────────────────────┐   │
│ │ Fix Python import error    │   │
│ │ The issue is that your...  │   │
│ │ 🟠 Cloud     Mar 24        │   │
│ └────────────────────────────┘   │
│                                  │
└──────────────────────────────────┘
```

- Each card: 688px wide, background #1A1A2E, border-radius 12px, padding 12px
- Title: 15px, white, font-weight 500
- Preview: 13px, #999999, max 1 line, ellipsis
- Tier badge + timestamp: 12px, bottom-right
- Swipe left on card: Delete conversation (with confirmation)
- Tap: Open conversation

### 4.9 Chat App Spawning

When the AI determines it needs another app to fulfill a request, it shows an inline spawning notification:

```
┌────────────────────────────────┐
│ 🚀 Launching AliExpress       │
│    PingApp...                  │
│ ━━━━━━━━━━━━░░░░░░░░ 60%      │
└────────────────────────────────┘
```

- Card with animated progress bar
- Once the PingApp completes its task, results appear as rich cards in the chat
- User can tap "View in Browse" to see the actual browser tab the PingApp used

---

## 5. Browse (Browser App)

### 5.1 Overview

Browse provides direct access to the Dragon's Chromium browser. The user sees a live MJPEG sub-stream of a browser tab. This is the raw web browsing experience, enhanced with AI annotations.

Browse has two modes:
- **LIVE:** Real-time browser view. Touch = click, swipe = scroll
- **ANNOTATE:** Freeze the current frame. Draw on it. Submit drawing + text to vision AI for interpretation

### 5.2 LIVE Mode Layout

```
┌──────────────────────────────────┐
│ 9:41    ⚡ 🐉 ● ▼ WiFi  85%     │  STATUS BAR
├──────────────────────────────────┤
│ ←  →  🏠  [google.com_______] ⊞ │  NAV BAR (48px)
├──────────────────────────────────┤
│                                  │
│                                  │
│    (Live browser content)        │
│    (720px wide, scaled from      │
│     actual browser viewport)     │
│                                  │
│                                  │
│                                  │
│                                  │
│                                  │
│                                  │
│                                  │
│                                  │
│                                  │
│                                  │
│                                  │
│                                  │
│                                  │
├──────────────────────────────────┤
│ 🤖 "Click 'Sign In' to login"   │  AI BAR (40px, optional)
├──────────────────────────────────┤
│ ✏️Annotate  🔖Mark  ⊞Tabs  ⋮    │  TOOLBAR (48px)
└──────────────────────────────────┘
```

### 5.3 Navigation Bar (48px)

```
┌──────────────────────────────────┐
│ ← → 🏠 [https://google.com__] ⊞ │
└──────────────────────────────────┘
```

- **Back (←):** 40x40px, goes to previous page in browser history
- **Forward (→):** 40x40px, grayed out if no forward history
- **Home (🏠):** 40x40px, navigates to configured home page (default: Google)
- **URL bar:** flex-grow, height 36px, background #1A1A2E, border-radius 18px, padding 0 12px
  - Shows current URL, truncated with ellipsis
  - Tap: opens full-screen URL editor with keyboard
  - Text: 13px, #CCCCCC
  - When loading: thin blue progress bar (2px) slides across top of URL bar
- **Tabs (⊞):** 40x40px, shows tab count in a badge. Tap opens tab switcher

### 5.4 Browser Content Area

The main viewport streams the browser tab content.

**LIVE Mode Touch Mapping:**
- **Tap:** Sends CDP `Input.dispatchMouseEvent` (mousePressed + mouseReleased) at the scaled coordinates
- **Swipe up/down:** Sends CDP `Input.dispatchMouseEvent` with scroll delta (`Input.dispatchMouseEvent` type: mouseWheel)
- **Swipe left/right:** Horizontal scroll (if page supports it)
- **Pinch (if multitouch works):** Sends CDP zoom commands. If multitouch is not reliable at 10fps, this can be replaced with zoom buttons in toolbar
- **Long press:** Sends CDP right-click, which may trigger browser context menu (rendered in the stream)

**Visual Feedback:**
- On tap: a brief ripple animation (expanding circle, 200ms, semi-transparent white) at the touch point, rendered as an overlay on Tab5 side (not in the stream), to give instant feedback despite stream latency
- On scroll: momentum indicator arrows at screen edges

### 5.5 AI Suggestion Bar (40px, Conditional)

Appears when Dragon's AI has analyzed the current page and has a suggestion.

```
┌──────────────────────────────────┐
│ 🤖 "Click 'Add to Cart' ↗"   ✕  │
└──────────────────────────────────┘
```

- Background: #1A1A2EEE, semi-transparent
- AI icon: 20px, tier-colored
- Suggestion text: 14px, white, max 1 line
- Dismiss (✕): 32x32px, right side
- Tap on suggestion text: highlights the suggested element on the page (if element coordinates are known, draw a pulsing border around it in the stream)
- Auto-dismisses after 10 seconds if not interacted with

**When suggestions appear:**
- Login pages: "Enter your email" / "Click Sign In"
- Shopping pages: "This looks like a good price" / "Click Add to Cart"
- Form pages: "Fill in your shipping address"
- The AI periodically screenshots the current tab and runs vision analysis (every 5-10 seconds when user is actively browsing)

### 5.6 ANNOTATE Mode

Activated by tapping "Annotate" in the toolbar. Freezes the current frame.

```
┌──────────────────────────────────┐
│ 9:41    ⚡ 🐉 ● ▼ WiFi  85%     │  STATUS BAR
├──────────────────────────────────┤
│ [Cancel]  ANNOTATE   [Send 🤖]  │  ANNOTATE BAR (48px)
├──────────────────────────────────┤
│                                  │
│                                  │
│    (Frozen browser screenshot)   │
│                                  │
│         ╭─── user drew ───╮      │
│         │   a red circle  │      │
│         │   around this   │      │
│         ╰─────────────────╯      │
│                                  │
│                                  │
│                                  │
│                                  │
│                                  │
│                                  │
├──────────────────────────────────┤
│ 🖊Pen  ⭕Circle  ▬Line  🗑Clear  │  DRAW TOOLS (48px)
├──────────────────────────────────┤
│ ┌────────────────────────┐  🎤   │  TEXT INPUT (56px)
│ │ What is this element?  │       │
│ └────────────────────────┘       │
└──────────────────────────────────┘
```

**Annotate Bar:**
- Cancel: returns to LIVE mode, discards annotation
- Title "ANNOTATE": centered, 16px, font-weight 600, #FF6B6B (red accent)
- Send: submits frozen frame + annotations + text to vision AI

**Draw Tools:**
- Pen: freehand drawing, 3px red (#EF4444) stroke
- Circle: tap and drag to draw ellipse, 3px red stroke
- Line: tap start and end points, 3px red stroke with arrow
- Clear: removes all drawings

**Drawing canvas:** Transparent overlay on the frozen screenshot. Touch events draw instead of clicking.

**Text Input:** Same as Chat input bar. User can type or voice a question about the annotated area.

**Send Flow:**
1. User taps Send
2. Frozen frame + annotation overlay composited into single image
3. Image + text sent to vision AI (local Qwen vision or escalated to Gemini/Claude vision)
4. Result appears as a modal overlay:
   ```
   ┌────────────────────────────────┐
   │ 🤖 AI Analysis                 │
   │                                │
   │ This appears to be the "Add to │
   │ Cart" button for the ESP32-S3  │
   │ development board. The price   │
   │ is $3.42 with free shipping.   │
   │                                │
   │ [Click it] [Back to Browse]    │
   └────────────────────────────────┘
   ```
5. Tapping "Click it" returns to LIVE mode and programmatically clicks the identified element

### 5.7 Tab Switcher

Opened by tapping the tab button (⊞) in the nav bar.

```
┌──────────────────────────────────┐
│ Tabs (3)              + New   ✕  │
├──────────────────────────────────┤
│                                  │
│  ┌──────────────────────────┐    │
│  │ [Tab Preview Thumbnail]  │ ✕  │
│  │  Google - Search          │   │
│  └──────────────────────────┘    │
│                                  │
│  ┌──────────────────────────┐    │
│  │ [Tab Preview Thumbnail]  │ ✕  │
│  │  AliExpress - ESP32       │   │  <- active tab highlighted
│  └──────────────────────────┘    │
│                                  │
│  ┌──────────────────────────┐    │
│  │ [Tab Preview Thumbnail]  │ ✕  │
│  │  YouTube - Lo-fi beats    │   │
│  └──────────────────────────┘    │
│                                  │
└──────────────────────────────────┘
```

- Vertical scrollable list of tab cards
- Each card: 640px wide, thumbnail (640x360px, scaled screenshot), title below
- Active tab: blue border (#3B82F6, 2px)
- Close (✕): 32x32px, top-right of each card
- New tab (+): opens a new tab at home page
- Tap card: switches to that tab, returns to LIVE mode
- PingApp-controlled tabs show a colored badge with the PingApp name

### 5.8 Bookmark Management

Accessed via overflow menu (⋮) in toolbar -> Bookmarks.

```
┌──────────────────────────────────┐
│ ← Bookmarks              + Add  │
├──────────────────────────────────┤
│                                  │
│ ┌────────────────────────────┐   │
│ │ 🌐 Google                  │   │
│ │    google.com              │   │
│ └────────────────────────────┘   │
│ ┌────────────────────────────┐   │
│ │ 🛒 AliExpress              │   │
│ │    aliexpress.com          │   │
│ └────────────────────────────┘   │
│                                  │
└──────────────────────────────────┘
```

- List of bookmarks with favicon + title + domain
- Tap: navigate to bookmark
- Long press: edit or delete
- Add: bookmarks current page

---

## 6. App Gallery

### 6.1 Overview

The App Gallery is the "app store" for PingApps. Users can browse, install, and manage PingApps that automate web services.

### 6.2 Main Gallery View

```
┌──────────────────────────────────┐
│ 9:41    ⚡ 🐉 ● ▼ WiFi  85%     │  STATUS BAR
├──────────────────────────────────┤
│ ← App Gallery           🔍      │  APP BAR (56px)
├──────────────────────────────────┤
│                                  │
│ ┌─Featured──────────────────┐    │
│ │ [Banner: YouTube PingApp] │    │  FEATURED CAROUSEL
│ │  Watch anything. AI picks │    │
│ │  what to play next.       │    │
│ └───────────────────────────┘    │
│         ● ○ ○ ○                  │  <- page dots
│                                  │
│ AI Chat                          │  CATEGORY HEADER
│ ┌──────┐ ┌──────┐ ┌──────┐      │
│ │ChatGP│ │Gemini│ │Claude│ >>>  │  HORIZONTAL SCROLL
│ │  T   │ │      │ │      │      │
│ └──────┘ └──────┘ └──────┘      │
│                                  │
│ Shopping                         │  CATEGORY HEADER
│ ┌──────┐ ┌──────┐ ┌──────┐      │
│ │AliEx │ │Amazon│ │eBay  │ >>>  │
│ │press │ │      │ │      │      │
│ └──────┘ └──────┘ └──────┘      │
│                                  │
│ Social                           │
│ ┌──────┐ ┌──────┐ ┌──────┐      │
│ │Twitte│ │Reddit│ │Telegr│ >>>  │
│ │r / X │ │      │ │am    │      │
│ └──────┘ └──────┘ └──────┘      │
│                                  │
│ Productivity                     │
│ ┌──────┐ ┌──────┐ ┌──────┐      │
│ │Google│ │Notion│ │GitHub│ >>>  │
│ │Drive │ │      │ │      │      │
│ └──────┘ └──────┘ └──────┘      │
│                                  │
│ Entertainment                    │
│ ┌──────┐ ┌──────┐ ┌──────┐      │
│ │YouTu│ │Spoti │ │Twitch│ >>>  │
│ │be    │ │fy    │ │      │      │
│ └──────┘ └──────┘ └──────┘      │
│                                  │
└──────────────────────────────────┘
```

### 6.3 Featured Carousel

- Full-width banner cards, 688px x 200px (16px margin each side)
- Auto-rotates every 5 seconds
- Manual swipe left/right
- Page indicator dots below: active = white, inactive = #444444
- Each banner: gradient background + app icon + title + tagline
- Tap: opens app detail page

### 6.4 Category Rows

**Category Header:**
- Text: 18px, font-weight 600, white, left-aligned
- "See All >" link: 14px, #3B82F6, right-aligned, opens full category grid
- 16px left padding

**App Tiles in Row:**
- Horizontal scroll row
- Each tile: 160x160px
- Layout: 72x72px icon centered, 13px label below, 12px status text below label
- Status: "Installed" in green, "Free" in white, or version number
- Gap between tiles: 12px
- First tile has 16px left margin, last has 16px right margin
- Fade gradient on right edge to indicate scrollability

### 6.5 App Detail Page

Opened by tapping any app tile.

```
┌──────────────────────────────────┐
│ 9:41    ⚡ 🐉 ● ▼ WiFi  85%     │  STATUS BAR
├──────────────────────────────────┤
│ ← App Gallery                    │  APP BAR
├──────────────────────────────────┤
│                                  │
│  ┌────────┐                      │
│  │ [ICON] │  ChatGPT PingApp     │  <- 72x72 icon + name 20px
│  │ 72x72  │  by TinkerClaw       │  <- author 14px, #999
│  │        │  ★ 4.8 (142 users)   │  <- rating 14px
│  └────────┘                      │
│                                  │
│  ┌──────────────────────────┐    │
│  │     [Install / Open]     │    │  <- 688x48px button
│  └──────────────────────────┘    │
│                                  │
│  ┌──────┐ ┌──────┐ ┌──────┐     │  SCREENSHOTS (horizontal)
│  │[SS1] │ │[SS2] │ │[SS3] │>>>  │
│  │      │ │      │ │      │     │
│  └──────┘ └──────┘ └──────┘     │
│                                  │
│  Description                     │
│  ─────────                       │
│  Automates ChatGPT's free tier   │
│  through browser control. Send   │
│  messages, receive responses,    │
│  manage conversations — all      │
│  automated via CDP.              │
│                                  │
│  Requires                        │
│  ─────────                       │
│  • Browser tab (1 dedicated)     │
│  • ChatGPT account (free tier)   │
│  • Google SSO login              │
│                                  │
│  Permissions                     │
│  ───────────                     │
│  • Browser automation            │
│  • Read/write cookies            │
│  • Network requests              │
│  • Notification access           │
│                                  │
│  Version History                 │
│  ───────────────                 │
│  v1.2.0 — Improved session       │
│  recovery, added multi-turn      │
│  v1.1.0 — Added image support    │
│  v1.0.0 — Initial release        │
│                                  │
└──────────────────────────────────┘
```

**Install/Open Button:**
- Not installed: Blue (#3B82F6) background, white text "Install", 688x48px
- Installing: Progress bar fills the button, "Installing..." text
- Installed: Green (#22C55E) outline, "Open" text
- Update available: Blue background, "Update" text
- Tap Install: downloads PingApp definition from Dragon's app registry, registers with PingOS Gateway

**Screenshots:**
- Horizontal scroll of preview images
- Each: 200x356px (9:16 ratio), border-radius 8px
- Tap: opens fullscreen preview with swipe navigation

### 6.6 Installed Apps Management

Accessible via tab or filter at top of Gallery.

```
┌──────────────────────────────────┐
│  [Discover]  [Installed]  [Updates]│
├──────────────────────────────────┤
```

**Installed tab:**
- List view of installed PingApps
- Each row: icon (48x48) + name + version + size + status
- Swipe left: uninstall
- Tap: opens app detail page
- Status indicators: Running (green dot), Stopped (gray dot), Error (red dot)

**Updates tab:**
- Shows PingApps with available updates
- "Update All" button at top
- Each row shows current version -> new version

---

## 7. Settings

### 7.1 Main Settings Screen

```
┌──────────────────────────────────┐
│ 9:41    ⚡ 🐉 ● ▼ WiFi  85%     │  STATUS BAR
├──────────────────────────────────┤
│ ← Settings                       │  APP BAR
├──────────────────────────────────┤
│                                  │
│  ┌────────────────────────────┐  │
│  │ 📶  WiFi                   │  │
│  │     Connected: TinkerNet   │  │
│  └────────────────────────────┘  │
│  ┌────────────────────────────┐  │
│  │ 🐉  Dragon Connection      │  │
│  │     Connected: 192.168.1.5 │  │
│  └────────────────────────────┘  │
│  ┌────────────────────────────┐  │
│  │ 🧠  AI Preferences         │  │
│  │     Auto routing enabled   │  │
│  └────────────────────────────┘  │
│  ┌────────────────────────────┐  │
│  │ 🖥️  Display                 │  │
│  │     Dark theme, 70%        │  │
│  └────────────────────────────┘  │
│  ┌────────────────────────────┐  │
│  │ 🔊  Sound                   │  │
│  │     Volume 60%, Wake ON    │  │
│  └────────────────────────────┘  │
│  ┌────────────────────────────┐  │
│  │ 🔔  Notifications           │  │
│  │     All enabled            │  │
│  └────────────────────────────┘  │
│  ┌────────────────────────────┐  │
│  │ ℹ️  About                    │  │
│  │     TinkerTab OS v1.0.0    │  │
│  └────────────────────────────┘  │
│  ┌────────────────────────────┐  │
│  │ 🛠️  Developer Mode          │  │
│  │     Disabled               │  │
│  └────────────────────────────┘  │
│                                  │
└──────────────────────────────────┘
```

**Settings Row:**
- Height: 72px, full width
- Left icon: 24x24px, 16px left padding
- Title: 16px, white, font-weight 500
- Subtitle: 13px, #999999
- Right chevron (>): 16px right padding, #666666
- Divider: 1px #1E1E2E between rows
- Tap: navigates to sub-settings page

### 7.2 WiFi Settings

```
┌──────────────────────────────────┐
│ ← WiFi                          │
├──────────────────────────────────┤
│                                  │
│  WiFi                      [ON]  │  <- toggle switch
│                                  │
│  Current Network                 │
│  ┌────────────────────────────┐  │
│  │ 📶 TinkerNet          ✓   │  │  <- connected, checkmark
│  │    Signal: Excellent       │  │
│  │    IP: 192.168.1.42        │  │
│  └────────────────────────────┘  │
│                                  │
│  Available Networks              │
│  ┌────────────────────────────┐  │
│  │ 📶 Neighbor_5G         🔒  │  │
│  └────────────────────────────┘  │
│  ┌────────────────────────────┐  │
│  │ 📶 CoffeeShop_Free        │  │
│  └────────────────────────────┘  │
│                                  │
│  [Scan for Networks]             │
│                                  │
└──────────────────────────────────┘
```

- Toggle switch: 52x28px, green when on, gray when off, thumb slides with 200ms ease
- Networks list auto-refreshes every 10 seconds
- Tap a network: opens password dialog (if secured) or connects immediately
- Lock icon: indicates password-protected
- Signal strength: shown as icon with 1-3 bars
- Current network is expandable — tap to see IP, MAC, DNS, gateway

**WiFi Password Dialog:**
```
┌────────────────────────────────────┐
│         Connect to Network         │
│         "Neighbor_5G"              │
│                                    │
│  ┌──────────────────────────────┐  │
│  │ Password                     │  │
│  └──────────────────────────────┘  │
│  ☐ Show password                   │
│                                    │
│     [Cancel]        [Connect]      │
└────────────────────────────────────┘
```

### 7.3 Dragon Connection

```
┌──────────────────────────────────┐
│ ← Dragon Connection              │
├──────────────────────────────────┤
│                                  │
│  Status: ● Connected             │  <- green dot
│                                  │
│  Dragon IP    192.168.1.5        │
│  Stream Port  8554               │
│  API Port     3000               │
│  Latency      12ms               │
│  Stream FPS   14                 │
│  Resolution   720x1280           │
│                                  │
│  Services                        │
│  ┌────────────────────────────┐  │
│  │ ● Ollama       Running     │  │
│  │ ● PingOS GW    Running     │  │
│  │ ● Moonshine    Running     │  │
│  │ ● Piper TTS    Running     │  │
│  │ ○ OpenRouter   Not config  │  │
│  └────────────────────────────┘  │
│                                  │
│  [Test Connection]               │
│  [Change Dragon IP]              │
│  [Restart Services]              │
│                                  │
└──────────────────────────────────┘
```

- Each service: colored dot (green=running, yellow=starting, red=error, gray=not configured) + name + status text
- Test Connection: pings Dragon, checks all services, shows results
- Change Dragon IP: text input for manual IP entry
- Restart Services: restarts all Dragon services (with confirmation dialog)

### 7.4 AI Preferences

```
┌──────────────────────────────────┐
│ ← AI Preferences                 │
├──────────────────────────────────┤
│                                  │
│  Routing Mode                    │
│  ┌─────────────────────────────┐ │
│  │ ● Auto (recommended)       │ │
│  │ ○ Local only               │ │
│  │ ○ Browser AI only          │ │
│  │ ○ Cloud only               │ │
│  └─────────────────────────────┘ │
│                                  │
│  Local Model                     │
│  ┌─────────────────────────────┐ │
│  │ Qwen 3.5 4B            ▼   │ │  <- dropdown
│  └─────────────────────────────┘ │
│                                  │
│  Browser AI Services             │
│  ┌─────────────────────────────┐ │
│  │ ☑ ChatGPT (free tier)      │ │
│  │ ☑ Gemini (free tier)       │ │
│  │ ☑ Claude (free tier)       │ │
│  └─────────────────────────────┘ │
│                                  │
│  Cloud (OpenRouter)              │
│  ┌─────────────────────────────┐ │
│  │ API Key: ●●●●●●●●●abc  ✏️  │ │
│  │ Balance: $4.32              │ │
│  │ Spending Limit: $5/day  ✏️  │ │
│  │ Preferred Model:           │ │
│  │ Llama 3.3 70B          ▼   │ │
│  └─────────────────────────────┘ │
│                                  │
│  Multi-Model Mode          [OFF] │
│  Ask all AI services at once     │
│  and show best response          │
│                                  │
│  Smart Escalation          [ON]  │
│  Auto-upgrade to better model    │
│  if local can't answer well      │
│                                  │
└──────────────────────────────────┘
```

**Routing Mode:** Radio buttons. Auto = the orchestrator decides based on query complexity.

**Local Model Dropdown:** Lists all models available in Ollama on Dragon. Shows model size and quantization.

**Browser AI Checkboxes:** Toggle which browser AI services to use. Each shows session status (healthy/expired/not logged in).

**OpenRouter Section:**
- API key: masked, tap pencil to edit
- Balance: fetched from OpenRouter API
- Spending limit: daily cap, tap pencil to edit
- Preferred model: dropdown of OpenRouter models, sorted by cost

### 7.5 Display Settings

```
┌──────────────────────────────────┐
│ ← Display                        │
├──────────────────────────────────┤
│                                  │
│  Brightness                      │
│  ☀ ━━━━━━━━━●━━━━━━━━━━━━━ ☀    │  <- slider
│  Auto brightness           [ON]  │
│                                  │
│  Theme                           │
│  ┌───────┐ ┌───────┐ ┌───────┐  │
│  │ Dark  │ │ Light │ │ Auto  │  │  <- segmented control
│  │  ●    │ │       │ │       │  │
│  └───────┘ └───────┘ └───────┘  │
│                                  │
│  Accent Color                    │
│  ● ● ● ● ● ● ● ●               │  <- color swatches
│  B P G O R Y T W                 │
│                                  │
│  Wallpaper                       │
│  ┌──────┐ ┌──────┐ ┌──────┐     │
│  │[WP1] │ │[WP2] │ │[WP3] │>>> │  <- wallpaper picker
│  └──────┘ └──────┘ └──────┘     │
│                                  │
│  Screen Timeout                  │
│  [30s] [1m] [2m] [5m] [Never]   │
│                                  │
│  Clock Format                    │
│  [12h] [24h]                     │
│                                  │
└──────────────────────────────────┘
```

**Theme:** Segmented control (3 options), selected = filled with accent color.

**Accent Color Swatches:**
- 8 color circles, 40x40px, 12px gap
- Blue (#3B82F6), Purple (#8B5CF6), Green (#22C55E), Orange (#F97316), Red (#EF4444), Yellow (#FACC15), Teal (#14B8A6), White (#FFFFFF)
- Selected swatch has a white ring around it
- Accent color is used for buttons, links, active indicators throughout the OS

**Wallpaper Picker:**
- Horizontal scroll of preset wallpapers + "Custom" tile (camera icon)
- Custom opens Camera app or file picker
- Selected wallpaper has blue border

### 7.6 Sound Settings

```
┌──────────────────────────────────┐
│ ← Sound                          │
├──────────────────────────────────┤
│                                  │
│  Volume                          │
│  🔇 ━━━━━━━━━━●━━━━━━━━━ 🔊     │
│                                  │
│  Wake Word                 [ON]  │
│  Say "Hey Tinker" to activate    │
│  voice assistant                 │
│                                  │
│  Wake Word Sensitivity           │
│  Low ━━━━━━━●━━━━━━━━━ High      │
│                                  │
│  TTS Voice                       │
│  ┌─────────────────────────────┐ │
│  │ Amy (English, US)       ▼   │ │
│  │ [▶ Preview]                 │ │
│  └─────────────────────────────┘ │
│                                  │
│  TTS Speed                       │
│  Slow ━━━━━━●━━━━━━━━━ Fast      │
│                                  │
│  Notification Sound        [ON]  │
│  Touch Sound               [OFF] │
│                                  │
└──────────────────────────────────┘
```

**TTS Voice Dropdown:** Lists all Piper voices installed on Dragon. Preview button plays a sample sentence.

### 7.7 Notification Settings

```
┌──────────────────────────────────┐
│ ← Notifications                   │
├──────────────────────────────────┤
│                                  │
│  Show Notifications        [ON]  │
│                                  │
│  Per-App Settings                │
│  ┌────────────────────────────┐  │
│  │ 💬 Chat              [ON]  │  │
│  │ 🌐 Browse            [ON]  │  │
│  │ 🤖 ChatGPT PingApp   [ON]  │  │
│  │ ✨ Gemini PingApp     [ON]  │  │
│  │ 🟠 Claude PingApp     [OFF] │  │
│  │ 🛒 AliExpress PingApp [ON]  │  │
│  │ ▶️ YouTube PingApp    [OFF] │  │
│  └────────────────────────────┘  │
│                                  │
│  Notification History            │
│  [View All Past Notifications]   │
│                                  │
└──────────────────────────────────┘
```

### 7.8 About

```
┌──────────────────────────────────┐
│ ← About                          │
├──────────────────────────────────┤
│                                  │
│         ╔══════╗                 │
│         ║  TC  ║                 │
│         ╚══════╝                 │
│       TinkerTab OS               │
│       Version 1.0.0              │
│                                  │
│  Device                          │
│  Model: M5Stack Tab5             │
│  Chip: ESP32-P4                  │
│  RAM: 32MB PSRAM                 │
│  Display: 720x1280 5"            │
│  WiFi MAC: AA:BB:CC:DD:EE:FF    │
│  IP: 192.168.1.42               │
│                                  │
│  Dragon                          │
│  Model: Q6A                      │
│  CPU: RK3588S (8-core ARM)      │
│  RAM: 8GB                        │
│  Storage: 64GB eMMC              │
│  IP: 192.168.1.5                │
│  Ollama: v0.6.2                 │
│  PingOS: v2.1.0                 │
│                                  │
│  Firmware                        │
│  Tab5 FW: 1.0.0-rc3             │
│  Dragon OS: Ubuntu 24.04        │
│  Stream Server: v1.0.0          │
│                                  │
│  [Check for Updates]             │
│  [Factory Reset]                 │
│  [Open Source Licenses]          │
│                                  │
└──────────────────────────────────┘
```

### 7.9 Developer Mode

Hidden by default. Enabled by tapping "TinkerTab OS Version 1.0.0" in About screen 7 times rapidly. Shows a toast "Developer mode enabled!" after the 7th tap.

```
┌──────────────────────────────────┐
│ ← Developer Mode                  │
├──────────────────────────────────┤
│                                  │
│  Developer Mode            [ON]  │
│                                  │
│  Serial Console                  │
│  [Open Terminal]                 │
│  Connects to Dragon via SSH      │
│                                  │
│  CDP Inspector                   │
│  [Open Inspector]                │
│  View/control browser tabs via   │
│  Chrome DevTools Protocol        │
│                                  │
│  Stream Debug Overlay      [OFF] │
│  Show FPS, latency, frame size   │
│  on-screen                       │
│                                  │
│  Touch Debug              [OFF]  │
│  Show touch coordinates and      │
│  CDP events on-screen            │
│                                  │
│  AI Debug Log             [OFF]  │
│  Show model routing decisions    │
│  and token counts                │
│                                  │
│  PingApp Console                 │
│  [View Logs]                     │
│  Real-time PingApp execution     │
│  logs from PingOS Gateway        │
│                                  │
│  Export Diagnostics              │
│  [Generate Report]               │
│  Exports system state, logs,     │
│  and configuration as ZIP        │
│                                  │
└──────────────────────────────────┘
```

**Serial Console:**
Opens a full-screen terminal emulator (xterm.js-style) with SSH connection to Dragon. Black background, green text (classic terminal aesthetic). 13px monospace font.

**CDP Inspector:**
Opens a simplified DevTools panel showing:
- Active tabs and their URLs
- Console output from PingApps
- Network requests being made by PingApps
- DOM snapshots

---

## 8. Camera / Vision

### 8.1 Overview

The Camera/Vision app is not a traditional camera (Tab5 has no camera). Instead, it captures screenshots of the current screen or browser, and provides AI-powered visual analysis tools.

### 8.2 Main Screen

```
┌──────────────────────────────────┐
│ 9:41    ⚡ 🐉 ● ▼ WiFi  85%     │  STATUS BAR
├──────────────────────────────────┤
│ ← Camera / Vision                │  APP BAR
├──────────────────────────────────┤
│                                  │
│  ┌──────────────────────────┐    │
│  │                          │    │
│  │   [Current Screen or     │    │
│  │    Browser Preview]      │    │  LIVE PREVIEW (360x640)
│  │                          │    │
│  │                          │    │
│  └──────────────────────────┘    │
│                                  │
│  ┌──────────┐  ┌──────────┐     │
│  │ 📸       │  │ 🤖       │     │
│  │Screenshot│  │ What is  │     │  ACTION BUTTONS
│  │          │  │ this?    │     │
│  └──────────┘  └──────────┘     │
│                                  │
│  ┌──────────┐  ┌──────────┐     │
│  │ 📱       │  │ 🖼️       │     │
│  │ QR Scan  │  │ Gallery  │     │
│  └──────────┘  └──────────┘     │
│                                  │
└──────────────────────────────────┘
```

**Live Preview:**
- Shows a scaled-down view of the current active browser tab or the home screen
- Updates in real-time (same MJPEG stream, scaled to fit)
- Tapping the preview opens it fullscreen

**Action Buttons:**
- 4 large buttons in a 2x2 grid
- Each: 328x100px, background #1A1A2E, border-radius 16px
- Icon: 32px, left-aligned
- Label: 16px, white, font-weight 500

### 8.3 Screenshot Flow

1. Tap "Screenshot"
2. Screen flashes white briefly (100ms, mimics camera shutter)
3. Screenshot captured from Dragon's current browser state
4. Preview shown with options:

```
┌──────────────────────────────────┐
│ ← Screenshot                     │
├──────────────────────────────────┤
│                                  │
│  ┌──────────────────────────┐    │
│  │                          │    │
│  │  [Full Screenshot]       │    │
│  │  720x1280                │    │
│  │                          │    │
│  │                          │    │
│  │                          │    │
│  └──────────────────────────┘    │
│                                  │
│  ┌──────┐ ┌──────┐ ┌──────┐     │
│  │ 🤖   │ │ ✏️   │ │ 💾   │     │
│  │ Ask  │ │ Edit │ │ Save │     │
│  │ AI   │ │      │ │      │     │
│  └──────┘ └──────┘ └──────┘     │
│                                  │
│  ┌──────┐ ┌──────┐              │
│  │ 📤   │ │ 🗑️   │              │
│  │Share │ │Delete│              │
│  └──────┘ └──────┘              │
│                                  │
└──────────────────────────────────┘
```

- **Ask AI:** Sends screenshot to vision AI with prompt "What's on this screen?" — opens a Chat conversation with the image embedded
- **Edit:** Opens annotation mode (same as Browse annotate — draw tools)
- **Save:** Saves to gallery on Dragon
- **Share:** Share via chat or export
- **Delete:** Removes with confirmation

### 8.4 "What Is This?" (Vision AI)

Quick flow for identifying anything on screen:
1. Tap "What is this?"
2. Screen freezes, crosshair cursor appears
3. User draws a rectangle around area of interest (drag to select)
4. Selection zoomed + sent to vision AI
5. Response appears as overlay:

```
┌────────────────────────────────────┐
│ 🤖 Vision AI                      │
│                                    │
│ ┌──────────────────────────────┐   │
│ │ [Selected Area Image]        │   │
│ └──────────────────────────────┘   │
│                                    │
│ This is the ESP32-S3-WROOM-1      │
│ module. It's a dual-core WiFi +   │
│ BLE microcontroller by Espressif. │
│ Price shown: $3.42.               │
│                                    │
│ [Ask More] [Open in Chat] [Done]  │
└────────────────────────────────────┘
```

### 8.5 QR Scanner

Uses the browser tab's content (not a physical camera). Scans QR codes visible in the browser stream.

1. Tap "QR Scan"
2. Crosshair overlay appears on the live browser preview
3. Dragon runs QR detection on the current frame (using a JavaScript QR library)
4. If QR found: highlights it with green corners, decodes, shows result:

```
┌────────────────────────────────────┐
│ QR Code Detected                   │
│                                    │
│ Type: URL                          │
│ Value: https://example.com/abc     │
│                                    │
│ [Open in Browse] [Copy] [Done]    │
└────────────────────────────────────┘
```

- For URLs: "Open in Browse" button
- For text: "Copy" button
- For WiFi: "Connect" button (parses WiFi QR format)

### 8.6 Gallery

```
┌──────────────────────────────────┐
│ ← Gallery                        │
├──────────────────────────────────┤
│                                  │
│  Today                           │
│  ┌──────┐ ┌──────┐ ┌──────┐     │
│  │[IMG1]│ │[IMG2]│ │[IMG3]│     │  3-column grid
│  └──────┘ └──────┘ └──────┘     │
│  ┌──────┐ ┌──────┐              │
│  │[IMG4]│ │[IMG5]│              │
│  └──────┘ └──────┘              │
│                                  │
│  Yesterday                       │
│  ┌──────┐ ┌──────┐ ┌──────┐     │
│  │[IMG6]│ │[IMG7]│ │[IMG8]│     │
│  └──────┘ └──────┘ └──────┘     │
│                                  │
└──────────────────────────────────┘
```

- 3-column grid, square thumbnails (228x228px with 4px gap)
- Grouped by date
- Tap: opens fullscreen viewer with swipe navigation
- Long press: multi-select mode (checkboxes appear, action bar shows Delete/Share)
- Fullscreen viewer: pinch to zoom, swipe left/right to navigate, swipe down to dismiss

---

## 9. Task Manager

### 9.1 Main View

```
┌──────────────────────────────────┐
│ 9:41    ⚡ 🐉 ● ▼ WiFi  85%     │  STATUS BAR
├──────────────────────────────────┤
│ ← Task Manager                   │  APP BAR
├──────────────────────────────────┤
│                                  │
│  ┌─ AI Usage ────────────────┐   │
│  │ Today                     │   │
│  │ 🟢 Local:  142 queries    │   │  USAGE SUMMARY
│  │ 🟣 Browser: 23 queries    │   │
│  │ 🟠 Cloud:   5 queries     │   │
│  │ 💰 Spent:   $0.03         │   │
│  └───────────────────────────┘   │
│                                  │
│  Running Apps (4)                │
│  ┌────────────────────────────┐  │
│  │ 💬 Chat                    │  │
│  │ Active conversation        │  │
│  │ Memory: 12MB  CPU: 2%     │  │
│  │                    [Kill]  │  │
│  └────────────────────────────┘  │
│  ┌────────────────────────────┐  │
│  │ 🤖 ChatGPT PingApp         │  │
│  │ Session active, 47/50 msgs │  │
│  │ Tab: chatgpt.com           │  │
│  │           [View] [Restart] │  │
│  └────────────────────────────┘  │
│  ┌────────────────────────────┐  │
│  │ ✨ Gemini PingApp           │  │
│  │ Session active, healthy    │  │
│  │ Tab: gemini.google.com     │  │
│  │           [View] [Restart] │  │
│  └────────────────────────────┘  │
│  ┌────────────────────────────┐  │
│  │ 🌐 Browse                   │  │
│  │ 3 tabs open                │  │
│  │ Memory: 45MB              │  │
│  │                    [Kill]  │  │
│  └────────────────────────────┘  │
│                                  │
│  [Kill All Non-Essential]        │
│                                  │
└──────────────────────────────────┘
```

### 9.2 AI Usage Summary

Card at top showing daily AI usage statistics.

- Background: #1A1A2E, border-radius 12px, padding 16px
- Each tier: colored dot + label + query count
- Cloud spending: dollar amount, turns red if approaching daily limit
- Tap card: opens detailed usage history (chart view with daily/weekly/monthly breakdown)

**Detailed Usage View:**
```
┌──────────────────────────────────┐
│ ← AI Usage History               │
├──────────────────────────────────┤
│                                  │
│  This Week                       │
│  ┌────────────────────────────┐  │
│  │  M  T  W  T  F  S  S      │  │
│  │  █  █  █  ▄  ░  ░  ░      │  │  <- bar chart
│  │  █  █  █  █  ░  ░  ░      │  │
│  │  █  █  █  █  ░  ░  ░      │  │
│  └────────────────────────────┘  │
│  🟢 Local   🟣 Browser  🟠 Cloud │
│                                  │
│  Total This Week                 │
│  Queries: 847                    │
│  Cloud Spend: $0.12              │
│  Avg Response: 1.8s              │
│                                  │
└──────────────────────────────────┘
```

### 9.3 Running App Cards

Each running app/PingApp gets a card.

**Card Layout:**
- Background: #1A1A2E, border-radius 12px, padding 12px
- Row 1: App icon (24x24) + App name (16px, white, bold)
- Row 2: Status text (14px, #999)
- Row 3: Resource info (13px, #777) — memory, CPU, tab URL
- Action buttons: right-aligned, 36x36px, border-radius 8px

**Action Buttons per App Type:**
- System apps (Chat, Browse): `Kill` (with confirmation — "This will close the app")
- PingApps: `View` (switches to Browse showing the PingApp's tab), `Restart` (kills and relaunches), `Kill`
- Chat: cannot be killed — always running (Kill button disabled/hidden)

**PingApp Status Indicators:**
- Green text "Session active" — healthy, logged in, operational
- Yellow text "Session expiring" — login may be stale, auto-refresh attempted
- Red text "Session error" — login failed or site blocked, needs manual intervention
- Gray text "Stopped" — not running

### 9.4 Kill All Button

- 688x48px button, red outline (#EF4444), "Kill All Non-Essential" text
- Keeps Chat and core services running
- Kills all PingApps and Browse tabs
- Confirmation dialog: "This will stop all PingApps and close all browser tabs. Continue?"

---

## 10. Voice Assistant (System-Level Overlay)

### 10.1 Activation Methods

1. **Hold mic button:** In Chat app, hold the 🎤 button
2. **Wake word:** Say "Hey Tinker" (detected by Dragon's Moonshine STT, always listening when enabled)
3. **Floating mic button:** A persistent small mic FAB (floating action button) on all screens except Chat (where it's integrated into the input bar)

### 10.2 Floating Mic Button

```
         ┌────┐
         │ 🎤 │   <- 56x56px, bottom-right, 16px from edge
         └────┘       semi-transparent (#1A1A2ECC)
                      draggable to any screen edge
```

- Always visible on non-Chat screens
- Position: bottom-right by default, 16px from edges
- Draggable: user can drag to any edge position (snaps to edge with 16px margin)
- Tap: activate voice assistant
- Long press: drag to reposition
- During voice input: expands to voice overlay

### 10.3 Voice Overlay (Listening State)

When voice is activated, a bottom sheet slides up from the bottom:

```
┌──────────────────────────────────┐
│                                  │
│    (current screen, dimmed)      │
│                                  │
│                                  │
│                                  │
│                                  │
│                                  │
├──────────────────────────────────┤
│                                  │
│         🎤 Listening...          │  <- 16px, white, pulsing
│                                  │
│    ∿∿∿∿∿∿∿∿∿∿∿∿∿∿∿∿∿∿∿∿∿∿      │  <- audio waveform
│                                  │
│   "Search AliExpress for..."     │  <- live transcription
│                                  │
│         [Cancel]                 │  <- 14px, #999
│                                  │
└──────────────────────────────────┘
```

**Bottom Sheet:**
- Height: 320px (slides up with spring easing, 300ms)
- Background: #0D0D1A with 24px top border-radius
- Drag handle: 40px wide, 4px tall, #333333, centered, 8px from top

**States:**

**Listening:**
- Mic icon: 48px, white, pulsing (scale 1.0 to 1.1, 800ms)
- Status: "Listening..." in 16px white, pulsing opacity
- Waveform: real-time audio visualization (Canvas element, 688px wide, 48px tall)
  - Color: accent blue (#3B82F6)
  - Style: smooth sine wave that reacts to audio amplitude
  - When silent: flat line with subtle idle animation
  - When speaking: amplitude follows voice loudness
- Live transcription: 18px, white, center-aligned, updates word-by-word as Moonshine STT processes
- Cancel button: 14px, #999999, tap to dismiss overlay

**Processing:**
```
┌──────────────────────────────────┐
│                                  │
│         🧠 Processing...         │
│                                  │
│   "Search AliExpress for        │  <- final transcription
│    ESP32-S3 dev boards"          │
│                                  │
│   Routing to AliExpress          │  <- intent display
│   PingApp...                     │
│                                  │
│         [Cancel]                 │
│                                  │
└──────────────────────────────────┘
```

- Brain icon replaces mic
- Status: "Processing..." in 16px
- Final transcription shown (no longer updating)
- Intent routing: 14px, accent color, shows which AI/app will handle the request

**Result:**
```
┌──────────────────────────────────┐
│                                  │
│         ✓ Done                   │
│                                  │
│   Found 12 ESP32-S3 boards      │  <- brief result
│   on AliExpress.                 │
│                                  │
│   [View in Chat] [Dismiss]      │
│                                  │
└──────────────────────────────────┘
```

- Checkmark: green (#22C55E), 32px
- Brief result summary: 16px, white
- Action buttons: navigate to the app that handled the request, or dismiss
- Auto-dismisses after 5 seconds if not interacted with

### 10.4 Wake Word Behavior

When "Hey Tinker" is detected:
1. Short haptic feedback (if Tab5 hardware supports it) + a brief chime sound
2. Voice overlay slides up immediately in Listening state
3. If on home screen: overlay appears
4. If in any app: overlay appears on top of current app
5. After processing, result is either shown inline (if the current app is Chat) or as the overlay result state

### 10.5 Voice Error States

- **No speech detected (5s timeout):** "I didn't catch that. Try again?" with retry button
- **Moonshine STT error:** "Voice recognition unavailable. Type your message instead." with "Open Chat" button
- **Dragon disconnected:** "Can't reach Dragon. Check your connection." with "Settings" button

---

## 11. Gesture System (Global)

### 11.1 Gesture Map

All gestures are detected at the system level (in the React app's root layout) and take priority over in-app touch handling.

```
┌──────────────────────────────────┐
│▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓│ <- TOP EDGE ZONE (60px)
│▒                                ▒│ <- LEFT EDGE (24px) / RIGHT EDGE
│▒                                ▒│
│▒                                ▒│
│▒       CONTENT AREA             ▒│
│▒       (app-specific touch)     ▒│
│▒                                ▒│
│▒                                ▒│
│▒                                ▒│
│▒                                ▒│
│▒                                ▒│
│▒                                ▒│
│▒                                ▒│
│▒                                ▒│
│▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓│ <- BOTTOM EDGE ZONE (60px)
└──────────────────────────────────┘
```

### 11.2 Gesture Definitions

| Gesture | Zone | Action | Details |
|---------|------|--------|---------|
| Swipe down | Top edge (y < 60px) | Open notification panel | Minimum 80px drag distance, triggers at release |
| Swipe up | Bottom edge (y > 1220px) | Go home / Open app drawer | Short swipe (80-200px) = go home. Long swipe (>200px) = open app drawer |
| Swipe from left | Left edge (x < 24px) | Back / Navigate back | 80px minimum drag, triggers at release. Works in all apps. On home screen: no-op |
| Long press | Anywhere | Context menu | 500ms hold. In home grid: rearrange mode. In Chat: message options. In Browse: right-click |
| Pinch in/out | Content area | Zoom (Browse only) | If multitouch is supported. Otherwise disabled |
| Three-finger swipe down | Anywhere | Screenshot | Captures current screen, saves to gallery, shows brief toast |
| Double-tap | Status bar | Toggle AI debug overlay | Developer mode only |

### 11.3 Edge Zone Conflict Resolution

Since Tab5 forwards all touch as CDP mouse events, edge gestures must be intercepted before being sent to Dragon. The system uses a two-phase approach:

1. **Touch start in edge zone:** Start a 150ms timer. If the touch moves significantly inward (>24px from edge), it's a gesture — do NOT forward to CDP.
2. **Touch start in edge zone but stays put or moves along edge:** After 150ms, if no significant inward movement, treat as a normal touch and forward to CDP.
3. **Touch start in content area:** Always forward to CDP immediately (no gesture detection delay).

This prevents accidental gesture triggering when the user is trying to tap near screen edges in Browse.

### 11.4 App Drawer (Swipe Up, Long)

```
┌──────────────────────────────────┐
│ 9:41    ⚡ 🐉 ● ▼ WiFi  85%     │  STATUS BAR
├──────────────────────────────────┤
│ ┌────────────────────────────┐   │
│ │ 🔍 Search apps...          │   │  SEARCH BAR
│ └────────────────────────────┘   │
│                                  │
│  ┌──────┐ ┌──────┐ ┌──────┐     │
│  │ Ali  │ │Browse│ │Camera│     │  ALPHABETICAL GRID
│  └──────┘ └──────┘ └──────┘     │  (all installed apps)
│  ┌──────┐ ┌──────┐ ┌──────┐     │
│  │ Chat │ │ChatGP│ │Claude│     │
│  └──────┘ └──────┘ └──────┘     │
│  ┌──────┐ ┌──────┐ ┌──────┐     │
│  │Galery│ │Gemini│ │ Maps │     │
│  └──────┘ └──────┘ └──────┘     │
│  ┌──────┐ ┌──────┐ ┌──────┐     │
│  │ Set  │ │Tasks │ │ YT   │     │
│  └──────┘ └──────┘ └──────┘     │
│                                  │
└──────────────────────────────────┘
```

- Full-screen overlay that slides up from bottom (400ms spring easing)
- Background: #0A0A1A (solid, not transparent)
- Search bar: 688px wide, 44px tall, top of screen
- Grid: 3 columns, alphabetically sorted, same tile layout as home grid
- Scrollable if more apps than fit
- Swipe down or tap home area dismisses

### 11.5 Gesture Visual Feedback

All gestures produce immediate visual feedback on the Tab5 side (overlay, not in MJPEG stream) to feel responsive despite stream latency:

| Gesture | Feedback |
|---------|----------|
| Swipe down (notification) | Semi-transparent panel peeks from top, follows finger |
| Swipe up (home/drawer) | Slight upward shift of content + peek of drawer |
| Swipe from left (back) | Right edge of previous screen peeks in from left, shadow overlay |
| Long press | Slight scale-down of pressed element (0.95x) + subtle shadow |
| Tap | Ripple animation at touch point (expanding circle, 200ms) |
| Screenshot (3-finger) | Screen flash white (100ms) + shutter sound + toast |

---

## 12. Navigation & App Lifecycle

### 12.1 App States

Each app exists in one of these states:

```
[Not Installed] → [Installed/Stopped] → [Backgrounded] → [Foregrounded]
                                       ↑                 ↓
                                       └─────────────────┘
```

- **Not Installed:** App definition exists in Gallery but not locally registered
- **Installed/Stopped:** Registered but no active process/tab
- **Backgrounded:** Running (browser tab exists) but not visible
- **Foregrounded:** Visible on screen, receiving touch events

### 12.2 App Launch Sequence

1. User taps app icon on home screen or dock
2. **If first launch:** Show loading screen (app icon centered, spinning loading indicator below)
3. **If backgrounded:** Immediately switch to the app's view (200ms cross-fade transition)
4. **If PingApp:** Ensure browser tab is allocated and PingApp script is injected
5. App's React route renders in the main viewport

**Launch Loading Screen:**
```
┌──────────────────────────────────┐
│ 9:41    ⚡ 🐉 ● ▼ WiFi  85%     │
├──────────────────────────────────┤
│                                  │
│                                  │
│                                  │
│                                  │
│           ┌────────┐             │
│           │ [ICON] │             │
│           │ 72x72  │             │
│           └────────┘             │
│            App Name              │
│                                  │
│         ◌ Loading...             │  <- spinning ring, 32px
│                                  │
│                                  │
│                                  │
│                                  │
└──────────────────────────────────┘
```

Timeout: If app doesn't load within 10 seconds, show error with Retry button.

### 12.3 Back Stack

The navigation system maintains a back stack (array of route states). This works like Android's back stack.

```
Home → Chat → Browse (spawned from Chat) → Annotate Mode
 [3]    [2]           [1]                      [0] ← current

Swipe-back from Annotate → Browse
Swipe-back from Browse → Chat
Swipe-back from Chat → Home
Swipe-back from Home → no-op (already at root)
```

**Rules:**
- Home screen is always the root of the back stack
- Launching an app pushes it onto the stack
- Back gesture (swipe from left) pops the stack
- Going Home (swipe up from bottom) clears the stack to root but does NOT kill background apps
- If the same app is launched while already in the stack, bring it to front (don't create duplicate)

### 12.4 Recent Apps / Multi-Tasking

Accessed by a slow, deliberate swipe up from bottom and hold (swipe up 100px and pause for 300ms).

```
┌──────────────────────────────────┐
│ 9:41    ⚡ 🐉 ● ▼ WiFi  85%     │
├──────────────────────────────────┤
│                                  │
│  ┌─────────────────────┐         │
│  │                     │         │
│  │  [Chat Preview]     │  ←      │  CARD STACK (horizontally scrollable)
│  │  scaled 0.7x        │         │
│  │                     │         │
│  │                     │         │
│  └─────────────────────┘         │
│         Chat                     │
│                                  │
│              ┌─────────────────────┐
│              │                     │
│              │  [Browse Preview]   │
│              │  scaled 0.7x        │
│              │                     │
│              │                     │
│              └─────────────────────┘
│                     Browse       │
│                                  │
│        [Clear All]               │
│                                  │
└──────────────────────────────────┘
```

- Cards: scaled-down app screenshots (504x896 at 0.7x scale), border-radius 16px
- Horizontally scrollable, most recent on left
- App name below each card, 14px, white
- **Tap card:** Switch to that app
- **Swipe card up:** Close/kill that app (card flies off top with 200ms ease)
- **Clear All:** Kills all non-essential apps, returns to home
- Background: #0A0A1A with 60% opacity over current screen

### 12.5 App-to-App Communication

Apps communicate through the TinkerClaw orchestrator on Dragon. Communication is message-based.

**Chat spawning Browse:**
1. Chat AI determines it needs to open a URL
2. Chat sends `{action: "open_url", url: "https://...", source: "chat"}` to orchestrator
3. Orchestrator launches Browse (or brings to foreground) with the URL
4. Browse opens the URL, pushes onto back stack after Chat
5. User can swipe back to return to Chat

**Chat spawning PingApp:**
1. Chat AI determines it needs a PingApp (e.g., "Search AliExpress")
2. Chat sends `{action: "run_pingapp", app: "aliexpress", params: {query: "ESP32-S3"}}` to orchestrator
3. Orchestrator launches PingApp in background (browser tab)
4. PingApp runs its automation, sends results back to orchestrator
5. Orchestrator forwards results to Chat, which renders them as rich cards
6. User stays in Chat — PingApp ran invisibly in the background

**Browse requesting AI analysis:**
1. User enters Annotate mode in Browse
2. Annotated image + query sent to orchestrator
3. Orchestrator routes to vision AI (local Qwen vision → escalate to Gemini/Claude if needed)
4. Result returned to Browse's annotate overlay

### 12.6 Deep Linking

Apps can be opened with specific parameters via deep links:

```
tinkertab://chat?message=Search+for+ESP32
tinkertab://browse?url=https://google.com
tinkertab://gallery?app=aliexpress
tinkertab://settings?section=ai
```

Deep links are used by:
- Notifications (tap notification → open app at relevant state)
- Quick actions in Chat
- AI-generated action buttons

---

## 13. Design System

### 13.1 Color Palette

**Base Colors (Dark Mode Primary):**

| Token | Hex | Usage |
|-------|-----|-------|
| `bg-primary` | `#0A0A1A` | Main background, home screen |
| `bg-secondary` | `#0D0D1E` | App backgrounds |
| `bg-surface` | `#1A1A2E` | Cards, inputs, elevated surfaces |
| `bg-surface-elevated` | `#252536` | Modals, dialogs, popovers |
| `bg-overlay` | `#000000CC` | Dimming overlays (80% black) |
| `bg-status-bar` | `#000000CC` | Status bar, dock |
| `border-default` | `#2A2A3E` | Card borders, dividers |
| `border-subtle` | `#1E1E2E` | Subtle separators |

**Text Colors:**

| Token | Hex | Usage |
|-------|-----|-------|
| `text-primary` | `#FFFFFF` | Headings, primary content |
| `text-secondary` | `#E5E5E5` | Body text, AI responses |
| `text-tertiary` | `#999999` | Subtitles, metadata |
| `text-muted` | `#666666` | Timestamps, disabled text |
| `text-placeholder` | `#444444` | Input placeholders |

**Accent Colors:**

| Token | Hex | Usage |
|-------|-----|-------|
| `accent-blue` | `#3B82F6` | Primary accent, links, user messages, active states |
| `accent-purple` | `#8B5CF6` | Browser AI tier, secondary accent |
| `accent-green` | `#22C55E` | Success, local AI tier, connected states |
| `accent-orange` | `#F97316` | Cloud AI tier, warnings |
| `accent-red` | `#EF4444` | Errors, destructive actions, recording |
| `accent-yellow` | `#FACC15` | Ratings, brightness, charging |
| `accent-teal` | `#14B8A6` | Info, alternative accent |

**AI Tier Colors (Consistent Across All Screens):**

| Tier | Color | Hex |
|------|-------|-----|
| Local (Dragon Ollama) | Green | `#22C55E` |
| Browser AI (PingApps) | Purple | `#8B5CF6` |
| Cloud (OpenRouter) | Orange | `#F97316` |
| Idle / None | Gray | `#666666` |

**Light Mode Overrides:**
If light mode is implemented, the following tokens swap:

| Token | Dark | Light |
|-------|------|-------|
| `bg-primary` | `#0A0A1A` | `#F8F9FA` |
| `bg-secondary` | `#0D0D1E` | `#FFFFFF` |
| `bg-surface` | `#1A1A2E` | `#F0F0F5` |
| `text-primary` | `#FFFFFF` | `#1A1A2E` |
| `text-secondary` | `#E5E5E5` | `#333333` |

Accent colors remain unchanged in light mode.

### 13.2 Typography

Base: system sans-serif stack. Load two fonts:
- **Primary:** Inter (or system default sans-serif)
- **Monospace:** JetBrains Mono (for code blocks, terminal, debug overlays)

**Type Scale (at 720px physical width):**

| Token | Size | Weight | Line Height | Usage |
|-------|------|--------|-------------|-------|
| `display-xl` | 72px | 200 | 1.0 | Home screen clock |
| `display-lg` | 32px | 300 | 1.2 | Section headings (onboarding) |
| `heading-lg` | 24px | 600 | 1.3 | Page titles |
| `heading-md` | 20px | 600 | 1.3 | App bar titles |
| `heading-sm` | 18px | 600 | 1.3 | Category headers |
| `body-lg` | 16px | 400 | 1.5 | Settings labels, descriptions |
| `body-md` | 15px | 400 | 1.5 | Chat messages, main content |
| `body-sm` | 14px | 400 | 1.4 | Cards, secondary content |
| `caption-lg` | 13px | 400 | 1.3 | App labels, metadata, URL bar |
| `caption-sm` | 12px | 400 | 1.3 | Date separators, fine print |
| `caption-xs` | 11px | 500 | 1.2 | AI tier badges, timestamps, dock labels |
| `mono-md` | 13px | 400 | 1.5 | Code blocks |
| `mono-sm` | 12px | 400 | 1.4 | Debug overlays, terminal |

**Readability at Arm's Length:**
The Tab5 is likely held at ~40-60cm. At 293 PPI, the minimum comfortable reading size is 13px (physical ~1.1mm cap height). Nothing in the UI uses text smaller than 11px, and only badges/timestamps use 11px.

### 13.3 Spacing & Grid System

**Base Unit:** 4px. All spacing derives from this.

| Token | Value | Usage |
|-------|-------|-------|
| `space-1` | 4px | Minimum gap, icon padding |
| `space-2` | 8px | Tight spacing, between related items |
| `space-3` | 12px | Card padding, gap between cards |
| `space-4` | 16px | Standard padding, screen margins |
| `space-5` | 20px | Section spacing |
| `space-6` | 24px | Large gaps between sections |
| `space-8` | 32px | Extra-large spacing |
| `space-10` | 40px | Status bar height, major gaps |
| `space-12` | 48px | Button height, toolbar height |
| `space-14` | 56px | App bar height, input bar height |
| `space-16` | 64px | Toggle height |
| `space-20` | 80px | Dock height |

**Screen Margins:**
- Left/right margin: 16px (giving 688px content width)
- Exception: full-bleed elements (status bar, dock, Browse content) use 0px margin

**Grid:**
- App grid: 3 columns, 240px each, no gap (icons centered within cells)
- Gallery grid: 3 columns, 228px each, 4px gap
- Settings list: single column, full width

### 13.4 Component Library

#### Buttons

**Primary Button:**
```
┌────────────────────────────────┐
│           Button Text          │  688x48px (full-width)
└────────────────────────────────┘   or auto-width with 24px horizontal padding
```
- Background: `accent-blue` (#3B82F6)
- Text: white, 15px, font-weight 600, centered
- Border-radius: 12px
- Height: 48px
- Active state: darken 10% (filter: brightness(0.9))
- Disabled state: opacity 0.4

**Secondary Button (Outline):**
- Background: transparent
- Border: 1.5px `accent-blue`
- Text: `accent-blue`, 15px, font-weight 600
- Same dimensions as primary

**Destructive Button:**
- Background: `accent-red` (#EF4444)
- Text: white
- Used for delete, kill, factory reset

**Ghost Button:**
- Background: transparent
- Text: `text-tertiary` (#999999), 14px
- No border
- Used for Cancel, Dismiss, secondary actions

**Pill Button (Quick Actions):**
- Background: `bg-surface` (#1A1A2E)
- Border: 1px `border-default`
- Border-radius: 20px (fully rounded)
- Padding: 8px 16px
- Text: 13px, `text-secondary`
- Height: 36px

#### Cards

**Standard Card:**
```
┌────────────────────────────────┐
│  (card content)                │
└────────────────────────────────┘
```
- Background: `bg-surface` (#1A1A2E)
- Border-radius: 12px
- Padding: 12px (compact) or 16px (standard)
- Border: none (or 1px `border-default` for interactive cards)
- Shadow: none (dark mode doesn't need shadows — use bg contrast)
- Width: 688px (full-width minus margins) or per-context

**Elevated Card (modals, popovers):**
- Background: `bg-surface-elevated` (#252536)
- Border-radius: 16px
- Padding: 16px-24px
- Shadow: 0 8px 32px rgba(0,0,0,0.5)

#### Inputs

**Text Input:**
```
┌────────────────────────────────┐
│ Placeholder text...            │
└────────────────────────────────┘
```
- Background: `bg-surface` (#1A1A2E)
- Border: 1.5px transparent (unfocused), 1.5px `accent-blue` (focused)
- Border-radius: 12px (standard) or 24px (chat input, search)
- Height: 44px (standard) or 48px (large)
- Padding: 0 16px
- Text: 15px, white
- Placeholder: `text-placeholder` (#444444)

**Toggle Switch:**
```
  OFF: ┌──────○┐     ON: ┌●──────┐
       └───────┘          └───────┘
```
- Size: 52x28px
- Track: rounded-full
- OFF: track #333333, thumb #888888 (left)
- ON: track `accent-green` (#22C55E), thumb white (right)
- Transition: 200ms ease
- Touch target: 52x48px (extended vertically for easier tapping)

**Slider:**
```
  ────────●────────────
```
- Track height: 4px, rounded-full
- Track background: #333333
- Filled track: accent color gradient
- Thumb: 24px white circle
- Touch target: full track width, 48px tall
- Drag: updates value in real-time

**Dropdown:**
```
┌──────────────────────────────┐
│ Selected Value            ▼  │
└──────────────────────────────┘
```
- Same styling as text input
- Chevron (▼): right side, 16px, `text-muted`
- Tap: opens a bottom sheet with scrollable option list (not a native select)

**Radio Buttons:**
```
● Option A (selected)
○ Option B
○ Option C
```
- Selected: filled circle, accent color
- Unselected: 1.5px border, #444444
- Circle size: 20px
- Label: 15px, white, 8px left of circle
- Row height: 48px (touch target)

**Checkboxes:**
```
☑ Enabled option
☐ Disabled option
```
- Selected: filled square with checkmark, accent color
- Unselected: 1.5px border, #444444
- Size: 20px
- Label: 15px, white, 8px right
- Row height: 48px

#### Modals / Dialogs

```
┌────────────────────────────────────┐
│                                    │
│           Dialog Title             │  <- 18px, white, bold, centered
│                                    │
│   Dialog message text explaining   │  <- 15px, #E5E5E5, centered
│   what is happening or asking      │
│   for confirmation.                │
│                                    │
│     [Cancel]        [Confirm]      │  <- buttons, 48px tall
│                                    │
└────────────────────────────────────┘
```

- Background: `bg-surface-elevated` (#252536)
- Border-radius: 20px
- Padding: 24px
- Width: 640px (32px margin each side)
- Centered vertically on screen
- Overlay: `bg-overlay` (#000000CC) behind dialog
- Entrance: scale from 0.9 to 1.0 + fade in, 200ms
- Exit: scale to 0.9 + fade out, 150ms

#### Toasts

```
                 ┌──────────────────────┐
                 │ ✓ Screenshot saved   │
                 └──────────────────────┘
```

- Position: bottom-center, 120px from bottom (above dock)
- Background: `bg-surface-elevated` (#252536)
- Border-radius: 12px
- Padding: 12px 20px
- Text: 14px, white
- Icon: left of text, color matches type (green=success, red=error, blue=info)
- Animation: slide up + fade in (200ms), auto-dismiss after 3 seconds (slide down + fade out, 200ms)
- Max width: 600px

#### Badges

**Notification Badge:**
- Size: 20px diameter circle (or pill if count > 9: min-width 20px, height 20px, padding 0 6px)
- Background: `accent-red` (#EF4444)
- Text: 11px, white, font-weight 700, centered
- Position: top-right of icon, offset -4px from edge

**Status Badge (inline):**
- Colored dot (8px) + label text
- Used for AI tier indicators, service status

### 13.5 Animation & Transition Guidelines

Given the 10-15fps MJPEG stream, animations must be designed to look good at low frame rates. Favor simple, short animations over complex multi-step ones.

**Principles:**
1. **Prefer opacity and transform over layout changes.** These look acceptable even at 10fps.
2. **Keep durations short:** 150-300ms for most transitions. At 10fps, 300ms = 3 frames.
3. **Use easing:** `ease-out` for entrances, `ease-in` for exits. Spring physics for interactive gestures.
4. **Avoid continuous animations:** Spinning loaders look terrible at 10fps. Use pulsing opacity or progress bars instead.
5. **Provide instant visual feedback:** Use Tab5-local overlays (rendered in firmware, not in stream) for touch ripples and gesture peeks.

**Standard Transitions:**

| Transition | Type | Duration | Easing |
|------------|------|----------|--------|
| Page push (navigate forward) | Slide from right | 250ms | ease-out |
| Page pop (navigate back) | Slide to right | 200ms | ease-in |
| Modal open | Scale 0.9→1.0 + fade | 200ms | ease-out |
| Modal close | Scale 1.0→0.9 + fade | 150ms | ease-in |
| Bottom sheet open | Slide up | 300ms | spring (damping 0.8) |
| Bottom sheet close | Slide down | 200ms | ease-in |
| Toast appear | Slide up + fade | 200ms | ease-out |
| Toast dismiss | Slide down + fade | 200ms | ease-in |
| Notification panel | Slide down, follows finger | 300ms | spring |
| App launch | Cross-fade | 200ms | ease-in-out |
| Card delete (swipe) | Slide out + shrink height | 200ms | ease-in |

**Loading Indicators:**
- **Progress bar:** Thin horizontal bar (4px), fills left to right. Good at low fps.
- **Pulsing dot:** Single dot that fades in/out (opacity 0.3 to 1.0, 800ms). Better than spinner at 10fps.
- **Skeleton screens:** Shimmer gradient that slides across placeholder shapes. Use a slow shimmer (2s cycle) that looks fine at any frame rate.

### 13.6 Touch Target Sizes

**Minimum touch target: 44x44px.** Preferred: 48x48px.

| Element | Visual Size | Touch Target |
|---------|------------|--------------|
| App icon (grid) | 72x72px | 240x190px (full cell) |
| Dock item | 28px icon | 180x80px (full cell) |
| Button (standard) | 48px tall | 48px tall, full width |
| Toggle switch | 52x28px | 52x48px |
| Navigation icon | 24px | 48x48px |
| Quick toggle | 208x64px | 208x64px |
| List row | 72px tall | 720x72px (full width) |
| Notification card | Variable | Full width of card |
| Close/dismiss (X) | 16px icon | 32x32px minimum |

### 13.7 Iconography

**Icon Style:**
- Outlined (not filled) for navigation and system icons
- Filled for status indicators and active states
- 24px standard size, 2px stroke weight
- Rounded line caps and joins
- Consistent 24x24px bounding box

**Icon Set Required:**
Navigation: back, forward, home, search, menu/overflow, close, settings/gear
Status: wifi (0-3 bars), battery (0-100%), charging, dragon, AI brain, mic, speaker
Actions: send, attach, camera, screenshot, annotate, bookmark, share, delete, copy, edit
Apps: chat bubble, globe, grid, list, play, shopping bag, pin, plus
Media: play, pause, skip-forward, skip-back, volume, mute

Use a consistent icon library (Lucide, Phosphor, or custom SVG set).

---

## 14. First-Run / Onboarding

### 14.1 Overview

The onboarding flow runs once on first boot (or after factory reset). It guides the user through essential setup steps. Each step is a full-screen page with large text, clear illustrations, and simple actions.

Total onboarding: 6 screens. Estimated time: 2-3 minutes.

### 14.2 Screen 1: Welcome

```
┌──────────────────────────────────┐
│                                  │
│                                  │
│                                  │
│            ╔══════╗              │
│            ║  TC  ║              │
│            ╚══════╝              │
│                                  │
│         Welcome to               │  <- 24px, #999
│        TinkerClaw                │  <- 32px, white, bold
│                                  │
│   Your personal AI appliance.    │  <- 16px, #999, centered
│   Let's get you set up.         │
│                                  │
│                                  │
│                                  │
│                                  │
│  ┌──────────────────────────┐    │
│  │       Get Started        │    │  <- primary button
│  └──────────────────────────┘    │
│                                  │
│       Select Language ▼          │  <- 14px, #666, tappable
│                                  │
└──────────────────────────────────┘
```

- Language selector at bottom: opens dropdown with available languages
- Defaults to English
- "Get Started" proceeds to Screen 2

### 14.3 Screen 2: WiFi Setup

```
┌──────────────────────────────────┐
│                                  │
│    Step 1 of 4                   │  <- 14px, #666
│                                  │
│    Connect to WiFi               │  <- 24px, white, bold
│    TinkerClaw needs WiFi to      │  <- 15px, #999
│    connect to your Dragon.       │
│                                  │
│  Available Networks              │
│  ┌────────────────────────────┐  │
│  │ 📶 TinkerNet           🔒  │  │
│  └────────────────────────────┘  │
│  ┌────────────────────────────┐  │
│  │ 📶 HomeWiFi_5G         🔒  │  │
│  └────────────────────────────┘  │
│  ┌────────────────────────────┐  │
│  │ 📶 CoffeeShop             │  │
│  └────────────────────────────┘  │
│                                  │
│  [Scanning...]                   │  <- or "Scan Again" button
│                                  │
│                                  │
│  ┌──────────────────────────┐    │
│  │        Continue          │    │  <- disabled until connected
│  └──────────────────────────┘    │
│                                  │
│  [Skip for now]                  │  <- ghost button
│                                  │
└──────────────────────────────────┘
```

- Same WiFi list as Settings, but simplified
- Tap network: password dialog (same as Settings)
- Once connected: network shows green checkmark, "Continue" becomes active
- Skip: warns that Dragon connection will fail without WiFi

### 14.4 Screen 3: Dragon Pairing

```
┌──────────────────────────────────┐
│                                  │
│    Step 2 of 4                   │
│                                  │
│    Find Your Dragon              │  <- 24px, white, bold
│    Make sure your Dragon Q6A     │  <- 15px, #999
│    is powered on and connected   │
│    to the same WiFi network.     │
│                                  │
│         ┌──────────┐             │
│         │   🐉     │             │  <- large dragon icon, pulsing
│         │ Scanning │             │
│         └──────────┘             │
│                                  │
│    Searching for Dragon on       │
│    TinkerNet...                  │  <- 14px, #999
│                                  │
│    ─────── OR ───────            │
│                                  │
│    Enter Dragon IP manually:     │
│    ┌──────────────────────────┐  │
│    │ 192.168.1.___            │  │  <- text input
│    └──────────────────────────┘  │
│                                  │
│  ┌──────────────────────────┐    │
│  │        Continue          │    │  <- disabled until found
│  └──────────────────────────┘    │
│                                  │
└──────────────────────────────────┘
```

**Auto-Discovery:**
Dragon runs an mDNS service (`_tinkerclaw._tcp`). Tab5 scans for it on the local network. When found:

```
│         ┌──────────┐             │
│         │   🐉 ✓   │             │  <- green checkmark
│         │  Found!  │             │
│         └──────────┘             │
│                                  │
│    Dragon Q6A                    │
│    192.168.1.5                   │
│    Ollama: Ready                 │
│    PingOS: Ready                 │
```

**Manual Entry:**
If auto-discovery fails after 15 seconds, the manual IP input is highlighted and auto-discovery status shows "Not found automatically. Try entering the IP."

### 14.5 Screen 4: AI Configuration

```
┌──────────────────────────────────┐
│                                  │
│    Step 3 of 4                   │
│                                  │
│    Set Up Your AI                │  <- 24px, white, bold
│    Choose how TinkerClaw uses    │  <- 15px, #999
│    AI to help you.               │
│                                  │
│  ┌────────────────────────────┐  │
│  │ 🟢 Local AI (Free)         │  │
│  │ Fast responses from Dragon │  │
│  │ Qwen 3.5 running locally  │  │
│  │ Always enabled        ✓   │  │
│  └────────────────────────────┘  │
│                                  │
│  ┌────────────────────────────┐  │
│  │ 🟣 Browser AI (Free)       │  │
│  │ Use ChatGPT, Gemini, and  │  │
│  │ Claude through browser     │  │
│  │ [Enable]  Requires login  │  │
│  └────────────────────────────┘  │
│                                  │
│  ┌────────────────────────────┐  │
│  │ 🟠 Cloud AI (Pay per use)  │  │
│  │ OpenRouter for fast cloud  │  │
│  │ AI. ~$0.10-0.50 per 1M    │  │
│  │ tokens                     │  │
│  │ [Add API Key]             │  │
│  └────────────────────────────┘  │
│                                  │
│  ┌──────────────────────────┐    │
│  │        Continue          │    │
│  └──────────────────────────┘    │
│                                  │
│  You can change these anytime    │
│  in Settings > AI Preferences    │
│                                  │
└──────────────────────────────────┘
```

- Local AI: always checked, shown as enabled (can't be disabled here)
- Browser AI: "Enable" button opens a sub-flow for Google SSO login (opens Browse with login page)
- Cloud AI: "Add API Key" opens text input for OpenRouter key
- All optional — user can skip and configure later
- Continue: always active (local AI is always available)

### 14.6 Screen 5: Quick Tutorial

```
┌──────────────────────────────────┐
│                                  │
│    Step 4 of 4                   │
│                                  │
│    How to Use TinkerClaw         │  <- 24px, white, bold
│                                  │
│  ┌────────────────────────────┐  │
│  │         ┌─────┐            │  │
│  │    ←    │     │            │  │  <- gesture illustration
│  │  SWIPE  │     │  BACK     │  │
│  │         └─────┘            │  │
│  └────────────────────────────┘  │
│                                  │
│    Swipe from the left edge      │  <- 15px, white
│    to go back.                   │
│                                  │
│       ● ○ ○ ○                    │  <- 4 tutorial pages
│                                  │
│  ┌──────────────────────────┐    │
│  │          Next            │    │
│  └──────────────────────────┘    │
│                                  │
│  [Skip Tutorial]                 │
│                                  │
└──────────────────────────────────┘
```

**Tutorial Pages (swipeable carousel):**

1. **Back Gesture:** "Swipe from the left edge to go back." (Illustration: hand swiping from left edge of phone)
2. **Notifications:** "Swipe down from the top for quick settings." (Illustration: hand swiping down from top)
3. **Voice:** "Tap the mic or say 'Hey Tinker' to talk to AI." (Illustration: mic icon with waveform)
4. **Chat is Command Center:** "Open Chat to ask anything. AI will use the right tools automatically." (Illustration: chat bubble spawning other app icons)

Each page: centered illustration (200x200px), title, description. Page dots below illustration.

### 14.7 Screen 6: Ready

```
┌──────────────────────────────────┐
│                                  │
│                                  │
│                                  │
│                                  │
│            ╔══════╗              │
│            ║  TC  ║              │
│            ╚══════╝              │
│                                  │
│        You're all set!           │  <- 24px, white, bold
│                                  │
│   🟢 WiFi: TinkerNet             │  <- 14px, green
│   🟢 Dragon: Connected           │  <- 14px, green
│   🟢 Local AI: Qwen 3.5          │  <- 14px, green
│   🟣 Browser AI: 3 services      │  <- 14px, purple (or gray if skipped)
│   🟠 Cloud AI: Configured        │  <- 14px, orange (or gray if skipped)
│                                  │
│                                  │
│  ┌──────────────────────────┐    │
│  │      Start Using         │    │
│  │      TinkerClaw          │    │  <- primary button, large
│  └──────────────────────────┘    │
│                                  │
│                                  │
└──────────────────────────────────┘
```

- Summary checklist of configured services
- Skipped items shown in gray
- "Start Using TinkerClaw" transitions to the home screen with a celebratory animation (brief confetti burst or subtle sparkle, 500ms)
- Onboarding flag set in local storage — won't show again unless factory reset

---

## Appendix A: Screen Inventory Summary

| Screen | Route | Always Available | Notes |
|--------|-------|-----------------|-------|
| Boot Logo | (firmware) | Yes | ESP32-P4 local rendering |
| Splash | `/splash` | Yes | First Dragon frame |
| Onboarding | `/onboarding/*` | First run only | 6 steps |
| Home | `/` | Yes | Root route |
| Notification Panel | (overlay) | Yes | System-level overlay |
| App Drawer | (overlay) | Yes | System-level overlay |
| Recent Apps | (overlay) | Yes | System-level overlay |
| Chat | `/chat` | Yes | Primary app |
| Chat History | `/chat/history` | Yes | Conversation list |
| Browse | `/browse` | Yes | Browser viewer |
| Browse Annotate | `/browse/annotate` | Yes | Annotation mode |
| Browse Tabs | `/browse/tabs` | Yes | Tab switcher |
| App Gallery | `/gallery` | Yes | App store |
| App Detail | `/gallery/:appId` | Yes | Per-app page |
| Settings | `/settings` | Yes | Main settings |
| Settings (sub) | `/settings/:section` | Yes | Each settings page |
| Camera/Vision | `/camera` | Yes | Screenshot + AI |
| Gallery | `/camera/gallery` | Yes | Image gallery |
| Task Manager | `/tasks` | Yes | Running apps |
| AI Usage | `/tasks/usage` | Yes | Usage charts |
| Voice Overlay | (overlay) | Yes | System-level overlay |
| Developer Console | `/dev/terminal` | Dev mode only | SSH terminal |
| CDP Inspector | `/dev/cdp` | Dev mode only | DevTools |

## Appendix B: Keyboard Layout

The on-screen keyboard occupies the bottom 320px when active. It uses a standard QWERTY layout optimized for the 720px width.

```
┌──────────────────────────────────┐  y=960px
│ q  w  e  r  t  y  u  i  o  p    │  ROW 1 (65px)
│                                  │
│  a  s  d  f  g  h  j  k  l      │  ROW 2 (65px)
│                                  │
│ ⬆  z  x  c  v  b  n  m  ⌫      │  ROW 3 (65px)
│                                  │
│ 123  🌐  [________space________] │  ROW 4 (65px) — numbers, globe, space
│          .    ↵                   │
└──────────────────────────────────┘  y=1280px
```

**Key Sizing:**
- Letter keys: 65x55px (720px / 10 keys per row, with 2px gap)
- Space bar: ~360px wide
- Special keys (shift, backspace, enter): 96px wide
- Key background: #252536
- Key text: 18px, white, centered
- Pressed state: key lightens to #3A3A4E

**Keyboard Features:**
- Tap-and-hold on keys: show accented character popup
- Swipe across keys: gesture typing (if implemented)
- Number/symbol mode (tap "123"): standard number/symbol layout
- Globe key: switches input language (if multiple configured)
- Keyboard slides up with 250ms ease, pushes content up (doesn't overlay)

## Appendix C: Error States

Every screen must handle these error conditions gracefully:

| Error | Display | Recovery |
|-------|---------|----------|
| Dragon disconnected | Red banner at top: "Dragon connection lost. Reconnecting..." | Auto-retry every 5s. Manual: tap banner → Settings |
| WiFi disconnected | Red banner: "WiFi disconnected" | Auto-retry. Manual: tap → WiFi settings |
| PingApp crash | Toast: "[App] encountered an error" + notification | Tap notification → Task Manager → Restart |
| AI timeout (>30s) | In Chat: "This is taking longer than expected. [Cancel] [Wait]" | Cancel returns to input. Wait continues. |
| OpenRouter balance depleted | Dialog: "Cloud AI budget reached for today. Using local AI." | Settings → AI → increase limit |
| Stream degradation | Debug overlay (dev mode): "FPS: 4 (low)" | Auto — Dragon adjusts quality |
| Full storage | Toast: "Dragon storage full. Some features may not work." | Settings → About → storage management |

## Appendix D: Accessibility

- All interactive elements have `aria-label` attributes
- Minimum contrast ratio: 4.5:1 for normal text, 3:1 for large text (18px+)
- Focus indicators: 2px blue outline for keyboard/sequential navigation
- Touch targets: minimum 44x44px as per WCAG 2.1
- Voice control: all primary actions accessible via voice commands
- Reduced motion: respect `prefers-reduced-motion` — disable all animations, use instant transitions
- Screen reader: basic support via semantic HTML (headings, buttons, lists, landmarks)

---

*End of TinkerTab OS UI/UX Design Specification v1.0*
