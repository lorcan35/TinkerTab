# Glyph OS — UI/UX Research Report
## 720x1280 Portrait Touchscreen on ESP32-P4 with LVGL

**Date:** 2026-03-26
**Target Device:** 7" IPS LCD, 720x1280 portrait, ESP32-P4, LVGL v9
**Purpose:** Actionable design guidance for a standalone AI companion device

---

## Table of Contents
1. [Smart Display & AI Device UI Patterns](#1-smart-display--ai-device-ui-patterns)
2. [LVGL Best Practices for Production UIs](#2-lvgl-best-practices-for-production-uis)
3. [Embedded Touchscreen UX Guidelines](#3-embedded-touchscreen-ux-guidelines)
4. [Dark UI Design System](#4-dark-ui-design-system)
5. [AI Companion Device UX](#5-ai-companion-device-ux)
6. [Page-Specific Design Recommendations](#6-page-specific-design-recommendations)
7. [Glyph OS Design Tokens](#7-glyph-os-design-tokens)

---

## 1. Smart Display & AI Device UI Patterns

### Rabbit R1
- **Card-based UI:** rabbitOS 2 (Sept 2025) moved to a colorful card-based system inspired by a deck of playing cards, similar to iPhone's Wallet app. Cards are swiped through using scroll wheel or touch.
- **Generative UI:** Users can generate entirely custom interfaces via text prompts — the UI itself becomes AI-driven. Themes inspired by Sonic, Zelda, Windows XP were demoed.
- **Key takeaway:** On a small device, a card metaphor reduces cognitive load. One card = one context. Swipe to explore, tap to act.
- Sources: [Neowin - Generative UI](https://www.neowin.net/news/the-rabbit-r1-now-lets-you-generate-its-whole-interface-with-ai/), [Yanko Design - rabbitOS 2](https://www.yankodesign.com/2025/09/09/rabbit-r1s-os-update-features-a-new-interface-and-speech-to-vibe-coding-abilities/), [Rapid Innovation - UX Norms](https://www.rapidinnovation.io/post/how-rabbit-r1-is-redefining-ux-design-norms)

### Humane AI Pin
- **Gesture-based navigation:** Projected UI on palm; tilt hand to highlight, pinch to select, close fist to go back. Carousel scrolling via hand movement toward/away.
- **Voice-first, screen-second:** The Pin demonstrates that AI devices should default to voice and use visual UI only for confirmation, disambiguation, or browsing results.
- **Key takeaway:** Even with a full touchscreen, Glyph OS should treat the screen as a response surface, not a command surface. Voice initiates, screen confirms.
- Sources: [HUDS+GUIS Analysis](https://www.hudsandguis.com/home/2024/humane-ai), [Core77 - UI Critique](https://www.core77.com/posts/131842/The-Horrific-UIUX-Design-of-Humanes-AI-Pin), [UX Collective - Ambient Interactions](https://uxdesign.cc/ambient-interactions-c88ada2c6b21)

### Amazon Echo Show / Google Nest Hub
- **Tab-based navigation:** Nest Hub splits into contextual tabs — "Your Morning/Evening," wellness, home control, media, games. Echo Show organizes around Alexa-driven suggestions.
- **Ambient information:** Both display contextual info (weather, calendar, photos) when idle, transitioning to interactive mode on touch/voice.
- **Proactive content:** Alexa+ (2025) provides overviews and proactive suggestions. Nest Hub uses photo frame mode + widgets as ambient state.
- **Key takeaway:** The idle state IS the primary UI. Most of the time the device is glanced at, not interacted with. Information density at-a-glance matters more than navigation depth.
- Sources: [TechRadar - Smart Display Comparison](https://www.techradar.com/home/smart-home/a-new-google-nest-hub-is-finally-coming-heres-what-it-needs-to-compete-with-amazons-stunning-new-echo-show), [Android Authority - Nest Hub Needs](https://www.androidauthority.com/nest-hub-upgrades-google-needs-2026-3630929/)

### Nothing Phone / Teenage Engineering
- **Monochrome discipline:** Nothing OS uses monochrome icon sets, removing "the crutch of colour cues" to focus the mind. Color is used sparingly and intentionally.
- **Glyph interface:** LED lighting accents serve as ambient notification indicators — functional minimalism where light patterns replace screen notifications.
- **Transparency and honesty:** Design shows internals, embraces raw materials, celebrates engineering rather than hiding it.
- **Key takeaway for Glyph OS:** Adopt the Nothing philosophy — monochrome base with a single accent color. Let the AI content be the color; the chrome should be neutral.
- Sources: [Yanko Design - Minimalist Phone](https://www.yankodesign.com/2026/01/15/minimalist-phone-takes-on-teenage-engineering-inspired-design-to-offer-hyper-functionality/), [Wallpaper - Nothing Phone 2](https://www.wallpaper.com/tech/nothing-phone-2-review), [Dezeen - Phone 1](https://www.dezeen.com/2022/08/19/nothing-phone-1-design/)

### reMarkable Tablet
- **Distraction-free:** No social media, no email, no games. The interface exists solely to serve the primary function (writing/reading).
- **Paper-like restraint:** Minimal chrome, maximum content area. The UI disappears when you are working.
- **Key takeaway:** For Glyph OS, each page should have one clear purpose. Remove everything that does not serve that purpose. The HOME page is for ambient awareness. The APPS page is for launching tools. Mixing purposes creates clutter.
- Sources: [reMarkable Official](https://remarkable.com/), [UI UX Showcase](https://uiuxshowcase.com/industrial-design/remarkable-digital-paper-tablet/), [Medium - UX Review](https://medium.com/@tarekPixels/ux-review-remarkable-tablet-92690a44bc2e)

---

## 2. LVGL Best Practices for Production UIs

### Architecture & Project Structure
- Use LVGL v9's XML-based component system for maintainability. The LVGL Pro Editor (v1.0 targeted Oct 2025) supports drag-and-drop layout, Figma sync, data binding, and animation editing.
- Figma plugin available to extract styles directly into LVGL-compatible formats.
- Sources: [LVGL Pro Best Practices](https://docs.lvgl.io/master/xml/best_practices.html), [LVGL Official](https://lvgl.io/)

### ESP32-P4 Specific Performance
| Setting | Recommended Value | Impact |
|---------|-------------------|--------|
| CPU Frequency | **360 MHz** (P4 max) | Critical |
| Compiler Optimization | `CONFIG_COMPILER_OPTIMIZATION_PERF=y` | +30% speed via SIMD |
| LVGL Fast Memory | `CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM=y` | Moderate |
| PSRAM Speed | `CONFIG_SPIRAM_SPEED_200M=y` | High for P4 |
| Draw Buffer Alignment | `CONFIG_LV_DRAW_BUF_ALIGN=64` | Required for PPA |
| PPA Burst Length | `CONFIG_LV_PPA_BURST_LENGTH=128` | Prevents PSRAM+PPA degradation |
| Display Refresh Period | 10ms (vs 30ms default) | Variable |

**Buffer sizing for 720x1280:**
- Full frame = 720 x 1280 x 2 bytes (RGB565) = ~1.8 MB
- Recommended: 15-25% of screen = 270KB-460KB per buffer
- Use double buffering in PSRAM with DMA for P4
- Internal SRAM buffers outperform PSRAM, but P4's PSRAM DMA capability narrows the gap
- Real-world P4 benchmarks show ~55 FPS vs ~22 FPS on S3

**Critical P4 note:** When using framebuffer in PSRAM, call `esp_cache_msync()` or DMA will see stale data. This is a known gotcha.

Sources: [LVGL ESP32 Tips & Tricks](https://docs.lvgl.io/master/integration/chip_vendors/espressif/tips_and_tricks.html), [esp-bsp Performance Guide](https://github.com/espressif/esp-bsp/blob/master/components/esp_lvgl_port/docs/performance.md), [Espressif GUI Solutions](https://docs.espressif.com/projects/esp-iot-solution/en/latest/display/lcd/gui_solution.html)

### Tileview for Page Navigation
LVGL's `lv_tileview` is the ideal widget for Glyph OS's 4-page structure:
```
Column layout (vertical swipe):
  Row 0: HOME (ambient/idle)
  Row 1: APPS (grid of tools)
  Row 2: DRAGON (remote desktop)
  Row 3: SETTINGS
```
- Create with `lv_tileview_add_tile(tv, 0, row, LV_DIR_TOP | LV_DIR_BOTTOM)`
- Switching: `lv_tileview_set_tile_by_index(tv, 0, row, LV_ANIM_ON)`
- Listen for `LV_EVENT_VALUE_CHANGED` to update status indicators
- Supports scroll chaining for nested scrollable content (e.g., settings lists inside a tile)

Sources: [LVGL Tileview Docs](https://docs.lvgl.io/master/widgets/tileview.html), [LVGL Gestures](https://docs.lvgl.io/master/main-modules/indev/gestures.html)

### Production Case Studies
- **Xiaomi Watch S1 Pro / S3 / S4 / Smart Band 9:** All run LVGL via HyperOS. Smooth animations, customizable watch faces, health data visualization. Won IDO Design Award.
- **IDO Smartwatches:** 10+ products with reusable LVGL widget library across hardware variants.
- **NXP Smartwatch Demo:** 25 screens (clock, music, fitness, weather, payment) on i.MX RT595.
- **Thermostat UI:** Pure LVGL rendering (no images) — rounded rectangles, circles, gradients, shadows, borders. Proves LVGL can create professional UIs without bitmap assets.

Sources: [LVGL Xiaomi Case Study](https://lvgl.io/case-studies/xiaomi), [LVGL Case Studies](https://lvgl.io/case-studies), [LVGL Demos](https://lvgl.io/demos)

### M5Stack Tab5 Existing Examples
- **Tab5 Home Assistant HMI:** Dashboard with sidebar navigation, 3 pages (Dashboard, Lights, Climate). LVGL 9.4 on ESP32-P4.
- **Arduino Basic LVGL Demo:** Includes SquareLine Studio project for visual editing.
- **ESPHome integration:** `mipi_dsi` display driver + full LVGL widget toolkit.
- Tab5 display: 1280x720 IPS (same panel, your 720x1280 is portrait orientation).

Sources: [Tab5 HA HMI Demo](https://docs.m5stack.com/en/homeassistant/kit/tab5_ha_hmi), [Arduino LVGL Demo](https://github.com/nikthefix/M5Stack_Tab5_Arduino_Basic_LVGL_Demo), [Tab5 HA Display](https://github.com/GalusPeres/Tab5-HomeAssistant-Display)

---

## 3. Embedded Touchscreen UX Guidelines

### Touch Target Sizes (for 720x1280 at ~210 PPI on 7")

| Element | Minimum | Recommended | Notes |
|---------|---------|-------------|-------|
| Primary buttons | 44x44 px | 56x56 px | Apple HIG: 44pt, Material: 48dp |
| Icon tap targets | 48x48 px | 64x64 px | Actual icon can be smaller; pad the touch area |
| List item height | 48 px | 56-64 px | Include padding for fat-finger tolerance |
| Spacing between targets | 8 px | 12 px | Google: 8dp minimum separation |
| Settings toggle row | 56 px height | 64 px | Toggle itself: 36x20 px, but row is the target |

**Physical equivalents at ~210 PPI (7" diagonal, 720x1280):**
- 1 px ≈ 0.12 mm
- 48 px ≈ 5.8 mm (absolute minimum for touch)
- 64 px ≈ 7.7 mm (comfortable)
- 80 px ≈ 9.6 mm (generous, primary actions)
- Finger pad width: ~16-20 mm = ~133-166 px

Sources: [NN/g Touch Target Size](https://www.nngroup.com/articles/touch-target-size/), [Smashing Magazine - Accessible Target Sizes](https://www.smashingmagazine.com/2023/04/accessible-tap-target-sizes-rage-taps-clicks/), [W3C WCAG 2.5.8](https://www.w3.org/WAI/WCAG22/Understanding/target-size-minimum.html), [Galaxy UX Studio](https://www.galaxyux.studio/blog/ux-design-for-small-touchscreens-navigating-space-accessibility-and-efficiency)

### Typography Hierarchy for 720x1280

At 210 PPI, use these pixel sizes (roughly 1:1 with CSS px on mobile):

| Role | Size | Weight | Use Case |
|------|------|--------|----------|
| Display / Hero | 48-56 px | Bold (700) | Clock on home screen, page titles |
| H1 / Section Title | 32-36 px | Semi-Bold (600) | "Apps", "Settings" headers |
| H2 / Card Title | 24-28 px | Medium (500) | App names, setting groups |
| Body / Primary | 18-20 px | Regular (400) | Setting descriptions, content text |
| Caption / Secondary | 14-16 px | Regular (400) | Timestamps, status indicators |
| Overline / Label | 12 px | Medium (500) | Category labels, badges |

**Font recommendations for LVGL:**
- Use a single font family with multiple weights (saves flash/RAM vs multiple families)
- Montserrat is bundled with LVGL and works well for UI
- For a more distinctive Glyph OS feel, consider: Inter, JetBrains Mono (for code/data), or a geometric sans like Outfit
- Pre-render only needed sizes/characters to save memory — LVGL supports font subsetting
- Line height: 1.4-1.5x font size for body text
- Contrast ratio: minimum 4.5:1 for body text, 3:1 for large text (>24px bold)

Sources: [Medium - Font Size Usage](https://medium.com/design-bootcamp/font-size-usage-in-ui-ux-design-web-mobile-tablet-52a9e17c16ce), [Toptal - Mobile Typography](https://www.toptal.com/designers/typography/typography-for-mobile-apps), [UXmatters - Type Sizes](https://www.uxmatters.com/mt/archives/2015/09/type-sizes-for-every-device.php)

### Gesture Patterns

| Gesture | Action | LVGL Implementation |
|---------|--------|---------------------|
| Swipe up/down | Navigate between pages (Home/Apps/Dragon/Settings) | `lv_tileview` with `LV_DIR_TOP/BOTTOM` |
| Swipe left/right | Secondary navigation within a page (e.g., sub-tabs) | Nested tileview or `lv_tabview` |
| Tap | Select / activate | `LV_EVENT_CLICKED` |
| Long press (500ms+) | Context menu / edit mode | `LV_EVENT_LONG_PRESSED` |
| Scroll (drag) | Scroll lists, scroll settings | Native LVGL scrolling with `LV_OBJ_FLAG_SCROLLABLE` |
| Double tap | Reserved for zoom or special action in Dragon page | Custom gesture handler |

**Swipe navigation indicator:** Use a thin vertical dot strip (4 dots) on the right edge to show current page position, similar to smartphone page indicators.

Sources: [Material Design 3 - Gestures](https://m3.material.io/foundations/interaction/gestures), [Smashing Magazine - C-Swipe](https://www.smashingmagazine.com/2013/03/c-swipe-navigation-on-android/), [LVGL Gesture Docs](https://docs.lvgl.io/master/main-modules/indev/gestures.html)

### Animation Performance Budget

On ESP32-P4 at 720x1280 with LVGL:
- **Target:** 30 FPS minimum for UI transitions, 60 FPS for idle animations
- **Page transition:** Use slide animation (200-300ms ease-out). Avoid opacity fades on full-screen elements — they require full buffer redraws.
- **Micro-animations:** Keep to small regions. A pulsing dot (AI status) is cheap. A full-screen wipe is expensive.
- **Avoid:** Drop shadows on moving elements, blur effects, transparency overlays on large areas, simultaneous animations on multiple large objects.
- **Prefer:** Color transitions, position animations, clip-path reveals, single-property animations.
- **Rule of thumb:** Animate only the dirty region. LVGL's partial rendering is your friend.

Sources: [LVGL Forum - ESP32 Low FPS](https://forum.lvgl.io/t/esp32-320x480-low-fps-animating/11799), [Seeed Studio - LVGL Optimization](https://wiki.seeedstudio.com/round_display_animation_workshop/), [Punch Through - 5 Tips](https://punchthrough.com/5-tips-for-building-embedded-ui-with-lvgl-and-platformio/)

---

## 4. Dark UI Design System

### Color Foundation

**Do NOT use pure black (#000000).** Use dark gray as the base surface. Pure black creates harsh contrast and makes the UI feel like a void rather than a space.

| Token | Hex | Usage |
|-------|-----|-------|
| `--bg-primary` | `#0D0F12` | Main background (deepest layer) |
| `--bg-surface` | `#161920` | Cards, panels, elevated surfaces |
| `--bg-surface-high` | `#1E222B` | Higher elevation (modals, popovers) |
| `--bg-surface-highest` | `#262B36` | Highest elevation (active states) |
| `--text-primary` | `#E8ECF1` | Primary text (87% white equivalent) |
| `--text-secondary` | `#8B95A5` | Secondary text (60% white equivalent) |
| `--text-disabled` | `#4A5568` | Disabled text (38% white equivalent) |
| `--border-subtle` | `#1E222B` | Subtle dividers |
| `--border-default` | `#2D3444` | Default borders |

### Accent Color Strategy

Choose ONE primary accent color. Use it sparingly — only for:
- Active/selected states
- Primary action buttons
- AI status indicators
- Current page dot in navigation

**Recommended accent options for Glyph OS:**

| Name | Hex | Vibe |
|------|-----|------|
| Electric Cyan | `#00E5FF` | Clean tech, information-forward |
| Phosphor Green | `#39FF14` | Terminal/hacker, AI-native |
| Amber Glow | `#FFB300` | Warm, approachable AI companion |
| Soft Violet | `#B388FF` | Unique, Nothing-esque |
| Signal Red | `#FF3D3D` | For errors/alerts only |

**Recommendation:** Electric Cyan (`#00E5FF`) — it reads well on dark surfaces, has strong tech/AI associations, and remains legible at small sizes. Use a dimmed variant (`#00A3B3`) for less prominent elements.

### Elevation via Overlay (Material Design 3 Pattern)

Rather than using shadows (expensive to render on LVGL), express elevation through surface color lightness:

| Elevation Level | Overlay Opacity | Resulting Color (on #0D0F12) | Use |
|----------------|-----------------|------------------------------|-----|
| 0 (base) | 0% | `#0D0F12` | Background |
| 1 | 5% | `#141720` | Cards at rest |
| 2 | 7% | `#181C26` | Raised cards |
| 3 | 8% | `#1A1F2A` | App bar |
| 4 | 9% | `#1C212D` | Modals |
| 5 | 11% | `#1F2532` | Active selection |

This is computationally free — just different background colors. No blend operations needed.

### Contrast Ratios

| Combination | Ratio | WCAG Level |
|-------------|-------|------------|
| Primary text on bg-primary | 15.2:1 | AAA |
| Primary text on bg-surface | 12.8:1 | AAA |
| Secondary text on bg-primary | 7.1:1 | AAA |
| Accent cyan on bg-primary | 10.3:1 | AAA |
| Accent cyan on bg-surface | 8.7:1 | AAA |

### OLED/LCD Considerations

- Dark themes on IPS LCD save less power than on OLED, but still reduce eye strain significantly in dim environments
- On LCD, very dark grays and true black look identical — no need to optimize for per-pixel power
- Add a subtle warm tint to the darkest colors (shift toward blue-gray rather than pure gray) to avoid a "dead screen" appearance
- Night mode: shift accent color from cyan to dim red/amber (like Apple StandBy night mode) to preserve night vision

Sources: [Material Design - Dark Theme](https://m2.material.io/design/color/dark-theme.html), [UI Deploy - Dark Mode Guide](https://ui-deploy.com/blog/complete-dark-mode-design-guide-ui-patterns-and-implementation-best-practices-2025), [Toptal - Dark UI Design](https://www.toptal.com/designers/ui/dark-ui-design), [Night Eye - Dark UI Best Practices](https://nighteye.app/dark-ui-design/)

---

## 5. AI Companion Device UX

### What Makes AI Devices Different from Phones

1. **Intent, not navigation.** On a phone, users open apps and navigate to features. On an AI device, users state what they want and the device resolves it. The UI should reflect this — minimize hierarchical navigation, maximize direct action.

2. **The device should feel alive.** Unlike a phone that waits passively, an AI companion should exhibit ambient awareness — subtle animations, breathing effects, contextual status changes that say "I'm here, I'm ready."

3. **Conversation is the primary interface.** Touch augments voice. The screen shows AI responses, confirms actions, displays visual results. It should NOT replicate a phone app launcher.

4. **Proactive over reactive.** The device should surface relevant information before being asked — weather before you leave, reminders before meetings, news summaries at your preferred time.

### Voice + Touch Hybrid Patterns

| Scenario | Primary Input | Screen Role |
|----------|---------------|-------------|
| Ask a question | Voice | Display answer, sources, visuals |
| Browse apps | Touch (tap grid) | Full interactive mode |
| Control Dragon (remote PC) | Touch (direct interaction) | 1:1 remote display |
| Adjust settings | Touch (toggles, sliders) | Standard form UI |
| Quick command while cooking | Voice | Brief confirmation animation |
| Read long content | Touch (scroll) | Reading mode, large text |

**Key principle — "Orchestration Layer":** When voice and touch happen simultaneously, the system needs clear priority rules:
- Voice overrides touch during active listening
- Touch overrides voice when user is scrolling/interacting
- Visual feedback must clearly show which mode is active (mic icon pulsing = listening, touch ripple = touch mode)

### Ambient Computing Principles

- **Peripheral awareness:** Information should be perceivable at a glance from across the room. Clock, weather icon, AI status should be readable at 2-3 meters.
- **Progressive disclosure:** Idle state shows minimal info. Approaching or tapping reveals more. Full interaction requires intentional engagement.
- **Context sensitivity:** Time of day should influence what's displayed (morning = calendar + weather, evening = relaxation mode, night = dim clock only).
- **Respectful notifications:** No buzzing, no popup storms. Use gentle color changes, subtle animations, or quiet chimes. The AI should curate which notifications deserve attention.

### Proactive AI Notification Design

Instead of traditional notification banners, use:
- **Subtle glow changes** on the home screen accent elements
- **A "thought bubble" indicator** — small icon that appears when AI has something to share, user taps to reveal
- **Timed cards** — weather card appears 30 min before usual departure, disappears after
- **Priority filtering** — AI categorizes notifications into "act now", "when convenient", and "FYI" — only "act now" gets visual prominence

Sources: [FuseLab - Multimodal AI Interfaces](https://fuselabcreative.com/designing-multimodal-ai-interfaces-interactive/), [Composite - Ambient and Multimodal UX](https://www.composite.global/news/the-rise-of-ambient-and-multimodal-ux), [Algoworks - Zero UI](https://www.algoworks.com/blog/zero-ui-designing-screenless-interfaces-in-2025/), [Emergn - Smarter Notifications](https://www.emergn.com/insights/smarter-approach-notifications-ml-ai/)

---

## 6. Page-Specific Design Recommendations

### HOME Page (Ambient / Idle)

**Philosophy:** This is a living surface, not a dashboard. It should feel like a window into the AI's awareness.

**Layout (720x1280 portrait):**
```
┌──────────────────────────┐
│                          │
│      [Time - Large]      │  ← 56px bold, centered
│      [Date - Small]      │  ← 18px secondary text
│                          │
│   ┌──────────────────┐   │
│   │  AI Status Orb   │   │  ← Animated circle/glyph
│   │  "Ready" / anim  │   │    120x120px, breathing glow
│   └──────────────────┘   │
│                          │
│  ┌─────────┐ ┌─────────┐ │
│  │ Weather │ │ Next Evt │ │  ← Contextual info cards
│  │  23°C ☀ │ │ 2pm Meet │ │    ~320x160px each
│  └─────────┘ └─────────┘ │
│                          │
│  ┌──────────────────────┐│
│  │ AI Insight Card      ││  ← Proactive suggestion
│  │ "Rain expected at 4pm││    Full width, ~160px tall
│  │  Take an umbrella"   ││
│  └──────────────────────┘│
│                          │
│         · ○ · ·          │  ← Page indicator (dot 1 = HOME)
└──────────────────────────┘
```

**Design details:**
- **AI Status Orb:** Central visual element. A circle or custom glyph that subtly animates (breathing glow in accent color). States: Ready (slow pulse), Listening (fast pulse), Thinking (orbiting particles), Speaking (waveform).
- **Clock:** Large, clean, monospace or geometric sans. Time is the anchor — visible from 3 meters.
- **Contextual cards:** 2-column grid below the orb. Show 2-4 cards based on context. Cards fade/rotate based on relevance. Swipe horizontally for more cards.
- **AI Insight Card:** Bottom section. Appears when AI has a proactive suggestion. Gentle slide-up animation. Tappable to expand.
- **Night mode:** At night (or on schedule), dim everything. Clock turns amber/red. Orb becomes a dim pulse. Cards hidden.
- **Idle timeout:** After 60s of no interaction, reduce brightness to 30%. After 5 min, show only clock + orb at minimum brightness.

**Inspiration:** Apple StandBy mode (glanceable clock + widgets), Google Nest Hub ambient mode (photo frame + contextual info), Nothing Glyph (light as status).

Sources: [Apple StandBy](https://support.apple.com/guide/iphone/use-standby-iph878d77632/ios), [Cult of Mac - StandBy](https://www.cultofmac.com/how-to/iphone-standby-mode)

### APPS Page (Tool Grid)

**Philosophy:** Efficient launcher. Get in, launch, get out. No decoration — pure function.

**Grid Math for 720x1280:**
```
Available width: 720px - 32px margin (16px each side) = 688px
Available height: 1280px - 80px header - 48px indicator = 1152px

Option A — 4 columns × 5 rows (20 apps visible):
  Icon area: 688 / 4 = 172px per cell
  Icon: 80x80px centered in cell
  Label below: 14px, 1 line
  Cell height: 80 + 8 + 18 + 24(padding) = 130px
  5 rows × 130px = 650px → fits with room to spare

Option B — 3 columns × 4 rows (12 apps visible, larger icons):
  Icon area: 688 / 3 = 229px per cell
  Icon: 96x96px centered
  Label: 16px, 1 line
  Cell height: 96 + 8 + 20 + 32(padding) = 156px
  4 rows × 156px = 624px → fits, more spacious

RECOMMENDED: Option A (4×5) for more apps visible.
If you have fewer than 12 apps, use Option B for a cleaner look.
```

**Layout:**
```
┌──────────────────────────┐
│  Apps              [🔍]  │  ← 32px title + search icon
│──────────────────────────│
│  ┌──┐  ┌──┐  ┌──┐  ┌──┐ │
│  │📱│  │💬│  │📷│  │🎵│ │  ← Row 1
│  │Chat│ │Msg │ │Cam │ │Mus│ │
│  ┌──┐  ┌──┐  ┌──┐  ┌──┐ │
│  │📝│  │🌐│  │📊│  │⏰│ │  ← Row 2
│  │Note│ │Web │ │Data│ │Alrm│
│  ┌──┐  ┌──┐  ┌──┐  ┌──┐ │
│  │⚙️│  │📁│  │🔧│  │🎮│ │  ← Row 3
│  │Util│ │File│ │Dev │ │Game│
│  ┌──┐  ┌──┐  ┌──┐  ┌──┐ │
│  │🤖│  │📡│  │🔐│  │➕│ │  ← Row 4
│  │AI  │ │IoT │ │VPN │ │Add │
│  ┌──┐  ┌──┐  ┌──┐  ┌──┐ │
│  │   │  │   │  │   │  │   │ │  ← Row 5 (if needed)
│                          │
│         · · ○ ·          │  ← Page indicator
└──────────────────────────┘
```

**Design details:**
- **Icons:** Monochrome outline style (Nothing-inspired) with accent color for active/selected state. 80x80px icon area, 2px stroke weight.
- **Grid:** `lv_obj` with flex layout, wrap enabled. Or use `lv_gridnav` for keyboard/encoder support.
- **Scrollable:** If more than 20 apps, the grid scrolls vertically within the tile. Use LVGL's native scroll with scroll snap.
- **Search:** Top-right search icon opens a text input + filtered grid. On voice-enabled devices, voice search takes priority.
- **Long press on icon:** Show options (delete, move, info).
- **Add app:** Last grid position is a "+" to install/create new apps (future: AI-generated micro-apps like Rabbit's generative UI).
- **No folders.** Keep it flat. Categories can be expressed through icon grouping or a horizontal category filter bar.

Sources: [Infinum - Mobile Layouts](https://infinum.com/blog/mobile-layouts-and-grids/), [Glance - Spacing Rules](https://thisisglance.com/learning-centre/what-spacing-rules-create-better-mobile-app-layouts), [Android Settings Pattern](https://developer.android.com/design/ui/mobile/guides/patterns/settings)

### DRAGON Page (Remote Desktop Stream)

**Philosophy:** Maximum screen real estate for the remote display. Controls must be available but not intrusive.

**Layout:**
```
┌──────────────────────────┐
│ [Dragon] [●Connected] [⋮]│  ← Top bar: 48px, auto-hides
│                          │
│                          │
│                          │
│   LIVE REMOTE DESKTOP    │  ← Full-screen VNC/RDP stream
│   STREAM FROM DRAGON     │    720x1280 scaled to fit
│   MINI-PC                │
│                          │
│                          │
│                          │
│                          │
│                          │
│  [🔙] [⌨️] [🖱️] [⚡] [⏹]│  ← Bottom bar: 56px, auto-hides
└──────────────────────────┘
```

**Auto-hiding controls pattern (like VNC mobile viewers):**
- Top and bottom bars auto-hide after 3 seconds of no interaction
- Single tap on stream area toggles bar visibility
- Double tap to toggle fullscreen (hide even page indicators)
- Bars use semi-transparent dark overlay (`#0D0F12` at 80% opacity) so stream content shows through

**Top bar elements:**
| Element | Size | Function |
|---------|------|----------|
| "Dragon" label | 18px | Page identification |
| Status indicator | 12px dot + text | Green = connected, Yellow = degraded, Red = disconnected |
| Overflow menu (⋮) | 48x48 tap target | Resolution, quality, reconnect, display settings |

**Bottom bar elements (toolbar):**
| Icon | Function | Implementation |
|------|----------|----------------|
| 🔙 Back | Send Back key to remote | Single tap |
| ⌨️ Keyboard | Toggle on-screen keyboard for remote input | Toggle state |
| 🖱️ Mouse mode | Switch between touch-as-mouse and touch-as-scroll | Toggle with indicator |
| ⚡ Quick actions | Macro menu (screenshot, lock, terminal) | Opens popup |
| ⏹ Disconnect | End remote session | Long press to confirm |

**Stream rendering considerations:**
- Dragon mini-PC likely outputs at 1920x1080 or higher
- Scale to fit 720px width → effective height = 405px (16:9) or letterboxed
- If Dragon is configured for portrait (1080x1920), it maps more naturally to 720x1280
- Use hardware-accelerated scaling if available via PPA (Pixel Processing Accelerator)
- Aim for low latency over high resolution — reduce color depth to RGB565 if needed
- Show a subtle frame rate counter in debug mode

**Connection status overlay:**
- When disconnected: Full-screen message with animated reconnect spinner + "Connecting to Dragon..." text
- When degraded: Subtle yellow border around stream area
- Latency indicator: Optional small text showing ping (e.g., "12ms")

Sources: [RealVNC Mobile Viewer](https://help.realvnc.com/hc/en-us/articles/360018541231-Using-RealVNC-Viewer-for-Mobile-to-control-a-remote-device), [UltraVNC GUI](https://uvnc.com/docs/ultravnc-viewer/71-ultravnc-viewer-gui.html)

### SETTINGS Page

**Philosophy:** Grouped list with clear hierarchy. No cards — use simple rows with dividers. Settings should feel utilitarian, not decorative.

**Layout:**
```
┌──────────────────────────┐
│  ← Settings              │  ← 56px header with back arrow
│──────────────────────────│
│                          │
│  NETWORK                 │  ← Group label: 12px overline
│  ┌──────────────────────┐│
│  │ Wi-Fi          On  ⟩ ││  ← 64px row, chevron = sub-page
│  │ Bluetooth      Off ⟩ ││
│  │ Dragon Link   ●   ⟩ ││  ← Green dot = connected
│  └──────────────────────┘│
│                          │
│  DISPLAY                 │
│  ┌──────────────────────┐│
│  │ Brightness    ━━━━○  ││  ← Inline slider
│  │ Night Mode     [⬤] ││  ← Toggle switch
│  │ Auto-Dim       [⬤] ││
│  │ Screen Timeout   ⟩  ││  ← Opens picker
│  └──────────────────────┘│
│                          │
│  AI ASSISTANT            │
│  ┌──────────────────────┐│
│  │ Voice Model      ⟩  ││
│  │ Wake Word        ⟩  ││
│  │ Proactive Mode [⬤] ││
│  │ AI Tier          ⟩  ││  ← Shows current: "Tier 3"
│  └──────────────────────┘│
│                          │
│  SYSTEM                  │
│  ┌──────────────────────┐│
│  │ Storage         ⟩   ││
│  │ About           ⟩   ││
│  │ Factory Reset   ⟩   ││  ← Red text for destructive
│  └──────────────────────┘│
│                          │
│         · · · ○          │
└──────────────────────────┘
```

**Design details:**
- **Group headers:** 12px overline text in secondary color, 24px top margin from previous group, 8px bottom margin to first item.
- **Row structure:** 64px tall. Left: icon (24x24, optional) + label (18px). Right: value/control + chevron if navigable.
- **Toggle switches:** 36x20px track, 16px diameter thumb. Accent color when on, `--bg-surface-highest` when off. Animate thumb position (150ms ease-out).
- **Sliders:** Full-width minus 32px padding. Track height: 4px. Thumb: 20px diameter. Active portion in accent color.
- **Destructive actions:** Use `--signal-red` text color. Require confirmation dialog.
- **Sub-pages:** Slide in from right (standard mobile navigation pattern). Use LVGL screen transition with `lv_screen_load_anim(LV_SCR_LOAD_ANIM_MOVE_LEFT)`.
- **Scrollable:** The entire settings content area scrolls vertically within the tile. Use `lv_obj_set_scroll_snap_y(LV_SCROLL_SNAP_NONE)` for free scrolling.

Sources: [Material Design - Settings Pattern](https://m1.material.io/patterns/settings.html), [Android Settings Guide](https://developer.android.com/design/ui/mobile/guides/patterns/settings), [NN/g - Toggle Guidelines](https://www.nngroup.com/articles/toggle-switch-guidelines/), [Material Design 3 - Switch](https://m3.material.io/components/switch/guidelines)

---

## 7. Glyph OS Design Tokens

### Complete Token Reference

These tokens can be directly mapped to LVGL style properties.

```c
/* ============================================
 * GLYPH OS DESIGN TOKENS
 * For LVGL v9 on ESP32-P4, 720x1280 portrait
 * ============================================ */

/* --- Colors --- */
#define GLYPH_BG_PRIMARY        lv_color_hex(0x0D0F12)
#define GLYPH_BG_SURFACE        lv_color_hex(0x161920)
#define GLYPH_BG_SURFACE_HIGH   lv_color_hex(0x1E222B)
#define GLYPH_BG_SURFACE_MAX    lv_color_hex(0x262B36)

#define GLYPH_TEXT_PRIMARY      lv_color_hex(0xE8ECF1)
#define GLYPH_TEXT_SECONDARY    lv_color_hex(0x8B95A5)
#define GLYPH_TEXT_DISABLED     lv_color_hex(0x4A5568)

#define GLYPH_ACCENT            lv_color_hex(0x00E5FF)  /* Electric Cyan */
#define GLYPH_ACCENT_DIM        lv_color_hex(0x00A3B3)
#define GLYPH_ACCENT_GLOW       lv_color_hex(0x00E5FF)  /* For animations */

#define GLYPH_SUCCESS           lv_color_hex(0x34D399)
#define GLYPH_WARNING           lv_color_hex(0xFFB300)
#define GLYPH_ERROR             lv_color_hex(0xFF3D3D)

#define GLYPH_BORDER_SUBTLE     lv_color_hex(0x1E222B)
#define GLYPH_BORDER_DEFAULT    lv_color_hex(0x2D3444)

/* --- Typography (pixel sizes for 720x1280 @ ~210ppi) --- */
#define GLYPH_FONT_DISPLAY      56   /* Clock, hero numbers */
#define GLYPH_FONT_H1           36   /* Page titles */
#define GLYPH_FONT_H2           28   /* Section titles */
#define GLYPH_FONT_BODY         20   /* Primary content */
#define GLYPH_FONT_CAPTION      16   /* Secondary info */
#define GLYPH_FONT_OVERLINE     12   /* Labels, badges */

/* --- Spacing (8px grid) --- */
#define GLYPH_SPACE_XS          4
#define GLYPH_SPACE_SM          8
#define GLYPH_SPACE_MD          16
#define GLYPH_SPACE_LG          24
#define GLYPH_SPACE_XL          32
#define GLYPH_SPACE_XXL         48

/* --- Touch Targets --- */
#define GLYPH_TOUCH_MIN         48   /* Absolute minimum */
#define GLYPH_TOUCH_DEFAULT     56   /* Standard interactive */
#define GLYPH_TOUCH_LARGE       64   /* Primary actions */
#define GLYPH_TOUCH_GAP         12   /* Between targets */

/* --- Layout --- */
#define GLYPH_MARGIN_PAGE       16   /* Page edge margin */
#define GLYPH_RADIUS_SM         8    /* Small border radius */
#define GLYPH_RADIUS_MD         12   /* Medium border radius */
#define GLYPH_RADIUS_LG         16   /* Large border radius */
#define GLYPH_RADIUS_FULL       9999 /* Pill shape */

/* --- Animation Timing (ms) --- */
#define GLYPH_ANIM_FAST         150  /* Toggle, small state change */
#define GLYPH_ANIM_NORMAL       250  /* Page transition, card appear */
#define GLYPH_ANIM_SLOW         400  /* Modal open, elaborate effect */
#define GLYPH_ANIM_BREATHING    2000 /* AI orb breathing cycle */

/* --- App Grid --- */
#define GLYPH_GRID_COLS         4
#define GLYPH_GRID_ICON_SIZE    80
#define GLYPH_GRID_CELL_WIDTH   (720 - 2 * GLYPH_MARGIN_PAGE) / GLYPH_GRID_COLS
#define GLYPH_GRID_CELL_HEIGHT  130
#define GLYPH_GRID_LABEL_SIZE   GLYPH_FONT_CAPTION

/* --- Dragon Overlay --- */
#define GLYPH_DRAGON_BAR_H      56
#define GLYPH_DRAGON_BAR_ALPHA  200  /* 0-255, ~78% opacity */
#define GLYPH_DRAGON_AUTOHIDE   3000 /* ms before bars hide */
```

### Navigation Architecture (Tileview Map)

```
          col=0
row=0  [  HOME    ]   ← Default landing, ambient
row=1  [  APPS    ]   ← Swipe up from HOME
row=2  [  DRAGON  ]   ← Swipe up from APPS
row=3  [  SETTINGS]   ← Swipe up from DRAGON

Navigation: Vertical swipe only (LV_DIR_TOP | LV_DIR_BOTTOM)
Page indicator: Right-edge vertical dots
```

### Key Design Principles Summary

1. **Dark-first, accent-sparse.** The UI is 95% dark surfaces and neutral text. Accent color appears only where attention is needed.
2. **Glanceable home, functional everywhere else.** HOME is for peripheral awareness. Other pages are for intentional interaction.
3. **Voice initiates, touch navigates.** The AI listens always; the screen responds. Touch is for browsing, selecting, and adjusting.
4. **Animate small, render smart.** Keep animations to small regions. Use LVGL's partial rendering. Avoid full-screen opacity fades.
5. **One purpose per page.** HOME = awareness. APPS = launching. DRAGON = remote control. SETTINGS = configuration. No mixing.
6. **Nothing-inspired restraint.** Monochrome icons, minimal chrome, let content breathe. The AI's responses are the main visual event.
7. **Progressive disclosure.** Idle = minimal. Approach = contextual. Touch = full interactive. This creates a natural hierarchy of engagement.

---

## Sources Index

### Smart Display & AI Device UI
- [Neowin - Rabbit R1 Generative UI](https://www.neowin.net/news/the-rabbit-r1-now-lets-you-generate-its-whole-interface-with-ai/)
- [Yanko Design - rabbitOS 2](https://www.yankodesign.com/2025/09/09/rabbit-r1s-os-update-features-a-new-interface-and-speech-to-vibe-coding-abilities/)
- [Rapid Innovation - Rabbit R1 UX Norms](https://www.rapidinnovation.io/post/how-rabbit-r1-is-redefining-ux-design-norms)
- [HUDS+GUIS - Humane AI Pin](https://www.hudsandguis.com/home/2024/humane-ai)
- [Core77 - Humane AI Pin UI Critique](https://www.core77.com/posts/131842/The-Horrific-UIUX-Design-of-Humanes-AI-Pin)
- [UX Collective - Ambient Interactions](https://uxdesign.cc/ambient-interactions-c88ada2c6b21)
- [TechRadar - Smart Display Comparison](https://www.techradar.com/home/smart-home/a-new-google-nest-hub-is-finally-coming-heres-what-it-needs-to-compete-with-amazons-stunning-new-echo-show)
- [Yanko Design - Nothing Minimalist Phone](https://www.yankodesign.com/2026/01/15/minimalist-phone-takes-on-teenage-engineering-inspired-design-to-offer-hyper-functionality/)
- [Wallpaper - Nothing Phone 2](https://www.wallpaper.com/tech/nothing-phone-2-review)
- [reMarkable Official](https://remarkable.com/)
- [Apple StandBy Mode Guide](https://support.apple.com/guide/iphone/use-standby-iph878d77632/ios)

### LVGL Technical
- [LVGL Pro Best Practices](https://docs.lvgl.io/master/xml/best_practices.html)
- [LVGL ESP32 Tips & Tricks](https://docs.lvgl.io/master/integration/chip_vendors/espressif/tips_and_tricks.html)
- [LVGL Tileview Widget](https://docs.lvgl.io/master/widgets/tileview.html)
- [LVGL Gestures](https://docs.lvgl.io/master/main-modules/indev/gestures.html)
- [LVGL Color Docs](https://docs.lvgl.io/master/main-modules/color.html)
- [LVGL Themes](https://docs.lvgl.io/master/common-widget-features/styles/themes.html)
- [esp-bsp Performance Guide](https://github.com/espressif/esp-bsp/blob/master/components/esp_lvgl_port/docs/performance.md)
- [Espressif GUI Solutions](https://docs.espressif.com/projects/esp-iot-solution/en/latest/display/lcd/gui_solution.html)
- [LVGL Xiaomi Case Study](https://lvgl.io/case-studies/xiaomi)
- [Tab5 HA HMI Demo](https://docs.m5stack.com/en/homeassistant/kit/tab5_ha_hmi)
- [Tab5 Arduino LVGL Demo](https://github.com/nikthefix/M5Stack_Tab5_Arduino_Basic_LVGL_Demo)
- [Tab5 HA Display (LVGL 9.4)](https://github.com/GalusPeres/Tab5-HomeAssistant-Display)

### UX Guidelines
- [NN/g - Touch Target Size](https://www.nngroup.com/articles/touch-target-size/)
- [Smashing Magazine - Accessible Target Sizes](https://www.smashingmagazine.com/2023/04/accessible-tap-target-sizes-rage-taps-clicks/)
- [W3C WCAG 2.5.8 Target Size](https://www.w3.org/WAI/WCAG22/Understanding/target-size-minimum.html)
- [Galaxy UX Studio - Small Touchscreens](https://www.galaxyux.studio/blog/ux-design-for-small-touchscreens-navigating-space-accessibility-and-efficiency)
- [Material Design 3 - Gestures](https://m3.material.io/foundations/interaction/gestures)
- [Material Design - Settings](https://m1.material.io/patterns/settings.html)
- [Android Settings Guide](https://developer.android.com/design/ui/mobile/guides/patterns/settings)
- [NN/g - Toggle Guidelines](https://www.nngroup.com/articles/toggle-switch-guidelines/)
- [Material Design 3 - Switch](https://m3.material.io/components/switch/guidelines)

### Dark UI & Color
- [Material Design - Dark Theme](https://m2.material.io/design/color/dark-theme.html)
- [UI Deploy - Dark Mode Guide 2025](https://ui-deploy.com/blog/complete-dark-mode-design-guide-ui-patterns-and-implementation-best-practices-2025)
- [Toptal - Dark UI Design](https://www.toptal.com/designers/ui/dark-ui-design)
- [Night Eye - Dark UI Best Practices](https://nighteye.app/dark-ui-design/)
- [Infinum - Mobile Layouts](https://infinum.com/blog/mobile-layouts-and-grids/)

### AI Companion UX
- [FuseLab - Multimodal AI Interfaces](https://fuselabcreative.com/designing-multimodal-ai-interfaces-interactive/)
- [Composite - Ambient and Multimodal UX](https://www.composite.global/news/the-rise-of-ambient-and-multimodal-ux)
- [Algoworks - Zero UI / Screenless Interfaces](https://www.algoworks.com/blog/zero-ui-designing-screenless-interfaces-in-2025/)
- [Emergn - Smarter Notifications with AI](https://www.emergn.com/insights/smarter-approach-notifications-ml-ai/)
- [Medium - Typography for UI/UX](https://medium.com/design-bootcamp/font-size-usage-in-ui-ux-design-web-mobile-tablet-52a9e17c16ce)
