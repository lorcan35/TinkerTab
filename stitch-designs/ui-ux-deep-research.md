# Glyph OS Deep UI/UX Research Report
## 720x1280 Portrait Touchscreen AI Companion on ESP32-P4 + LVGL v9

**Research Date**: 2026-03-27
**Target**: Production-quality dark UI for 4-page system (Home/Apps/Dragon/Settings)

---

# 1. LVGL Production UI Teardowns

## 1.1 Xiaomi Watch S1 Pro / S3 / S4 — LVGL in Production

**Source**: https://lvgl.io/case-studies/xiaomi

Xiaomi's smartwatch lineup (S1 Pro, S3, S4, Smart Band 9) all run LVGL. Key takeaways:

- **60 FPS target** maintained across all devices with responsive touch
- GPU-assisted **asynchronous rendering** minimizes CPU load — the GPU renders while the CPU prepares the next frame
- **Reusable widget components** deployed across 10+ product variants with different hardware
- Xiaomi made **direct contributions to LVGL's codebase** to align the framework with their specific needs
- Custom watch faces leverage LVGL's data visualization capabilities for health/fitness tracking
- The "premium feel" comes from: smooth animations, rapid response times, and deep customization

**What makes it feel premium**: Xiaomi's secret is NOT complex widgets — it's consistent 60fps with zero dropped frames during touch interactions. The perception of quality comes from animation fluidity, not visual complexity.

## 1.2 IDO Smartwatch — LVGL Mass Market Implementation

**Source**: https://lvgl.io/case-studies/ido

Two hardware variants shipping with LVGL:
- **GTX10**: ACTIONS ATS3085S (ARM, 192MHz single-core, 1168KB RAM, 22uA at 192MHz)
- **GTX12**: Silfi SF32LB563 (Arm Cortex-M33, 240MHz dual-core, 800KB RAM)

Key technical wins:
- 60 FPS with touch responsiveness even on the weaker GTX10
- GPU support for async rendering to minimize CPU load
- Bluetooth peripheral integration
- Widget customization for proprietary design requirements
- Cross-platform scalability across product lines

**Lesson for Glyph OS**: Even a 192MHz single-core ARM can hit 60fps with LVGL if you optimize rendering. ESP32-P4 at 400MHz with PPA should be overkill.

## 1.3 NXP i.MX RT595 Smartwatch Demo

**Source**: https://forum.lvgl.io/t/how-to-obtain-the-source-code-for-the-official-perfect-smartwatch-gui-demo/17160

The famous LVGL smartwatch demo that's shown on lvgl.io/demos:
- **25 screens** covering clock, music player, fitness, weather, payment
- Swipe up/down for main activities, left/right for sub-activities
- Created using **SquareLine Studio** (now separated from LVGL)
- Performance on RT595: **25-30 FPS** for widgets demo, ~10 FPS for complex containers (software rendering only, no GPU)

**How to access the source**:
1. NXP's **GUI Guider** tool (v8.3.10) — available as application template
2. GitHub: `apps_zephyr/apps/SmartWatch/src/UI_SW` on the SmartWatch branch
3. LVGL port repo: https://github.com/lvgl/lv_port_nxp_imx_rt500_evk

## 1.4 M5Stack Tab5 — ESP32-P4 with LVGL (Our Closest Reference)

**Source**: https://github.com/nikthefix/M5Stack_Tab5_Arduino_Basic_LVGL_Demo

The Tab5 is the closest production device to our hardware:
- ESP32-P4 with MIPI-DSI display
- Arduino-based LVGL demo with SquareLine Studio project included
- Touch and display working

**Tab5 Home Assistant Display** (most advanced Tab5 LVGL project):
- Source: https://github.com/GalusPeres/Tab5-HomeAssistant-Display
- **LVGL 9.4** on ESP32-P4
- Tile-based interface with 3 tabs (Home, Weather, Game)
- Uses PSRAM allocation utilities (`lvgl_psram_alloc.cpp/h`)
- WebSocket connectivity for real-time data
- Supported widget types: Sensors, Switches, Scenes as interactive tiles

**M5Stack Official User Demo**: https://github.com/m5stack/M5Tab5-UserDemo
- Reference implementation for hardware evaluation

## 1.5 Elecrow CrowPanel Advanced 7" — ESP32-P4 HMI

**Source**: https://www.cnx-software.com/2025/12/23/crowpanel-advanced-7inch-review-part-1-esp32-p4-hmi-ai-display-hands-on-with-lvgl-factory-firmware/

- 1024x600 IPS touch, ESP32-P4, 16MB Flash, 32MB PSRAM
- Factory firmware uses **ESP-Brookesia** as launcher
- Built-in apps: Calculator, Music Player, Settings, 2048, Camera
- SquareLine Studio for custom UI design
- Review describes touch as "smooth and fluid"
- Integrated 2MP MIPI-CSI camera for AI vision

## 1.6 LVGL Community Showcase Projects

**Source**: https://forum.lvgl.io/t/list-of-lvgl-important-links-and-project/23611

Notable community projects:
- **NMEA0183_mfd** — Marine navigation display using LVGL with ESP32-P4 and J1060p470 display (real commercial use!)
- **lvgl-round-thermostat-ui** — Two premium round thermostat UIs
- **lvgl-esp32-smartthings-controller** — Samsung SmartThings controller with LVGL + ESP32
- **metronome_ui_lvgl_pro** — Musical metronome with LVGL Pro + XML
- **battery-indicator-lvgl-editor** — iOS-inspired battery indicator
- **awesome-lvgl** curated list: https://github.com/aptumfr/awesome-lvgl

## 1.7 What Makes LVGL UI Feel "Premium" vs "Hobby"

Based on analyzing all production implementations:

| Aspect | Hobby Feel | Premium Feel |
|--------|-----------|--------------|
| Animation | Jerky, inconsistent FPS | Locked 60fps, ease-in-out curves |
| Touch | Delayed response, no feedback | Immediate response, haptic-like visual feedback |
| Typography | Single font size, no hierarchy | 3-4 sizes, weight variation, anti-aliased (4bpp+) |
| Color | Flat colors, high saturation | Gradients, muted palette, consistent system |
| Spacing | Random padding | Consistent 8px/12px grid system |
| Transitions | Instant screen switches | 300ms animated transitions with easing |
| Shadows | None or harsh | Subtle, layered, consistent light source |
| Icons | Mismatched styles | Unified icon family, consistent weight |

---

# 2. Pixel-Level Teardowns of AI Companion Devices

## 2.1 Rabbit R1 — RabbitOS 2

**Sources**:
- https://www.rabbit.tech/newsroom/rabbitos-2-launch
- https://partofstyle.com/rabbitos-2-reimagines-the-rabbit-r1/

**Hardware**: 2.88-inch touchscreen, 78mm x 78mm x 13mm body, MediaTek Helio P35 (2.3GHz), 4GB RAM, 128GB storage

**RabbitOS 2 UI System** (September 2025 redesign):

**Card Stack Architecture**:
- Every feature gets its own "card" in a vertical stack
- Visual metaphor: deck of colorful playing cards / Rolodex
- Swipe up from bottom or use scroll wheel to browse cards
- Tap any card to launch its app
- Close an app with left swipe
- "Fresh, colorful, playful, tactile, and engaging" aesthetic

**Gesture System**:
- Swipe down from top: brightness, volume, quick settings, camera
- Swipe up from bottom: open card stack
- Left swipe: close current app
- Bottom icons: mute, type follow-up, launch camera
- Scroll wheel: navigate card stack

**Conversation UI**:
- Real-time text display of conversation flow
- Multi-modal: voice, text, and images in same query
- Visual confirmation of what AI heard before acting

**Generative UI Feature**:
- Users type prompts to generate entirely new interfaces
- Takes 30+ seconds to generate a screen
- Themed interfaces (Sonic, Zelda, Windows XP demonstrated)

**Takeaway for Glyph OS**: The card-stack metaphor is compelling for an AI device. Each "capability" as a distinct visual card that you swipe through. The key insight is making the AI conversation visually present at all times — showing what it heard, what it's thinking.

## 2.2 Humane AI Pin — Cosmos OS

**Sources**:
- https://humane.com/aipin/cosmos
- https://dribbble.com/shots/23033970-Humane-Cosmos-Analysis

**Key Design Decisions**:
- **Screenless**: Laser projector onto palm/surface (radical simplicity)
- AI Bus eliminates traditional apps — no install/download paradigm
- Device-agnostic OS (runs on cars, phones, TVs, speakers)
- 34g weight, three color variants (Eclipse, Equinox, Lunar)
- Gesture control for projected interface

**Takeaway for Glyph OS**: The "no apps, just AI" philosophy. Instead of a traditional app grid, the AI figures out what service to connect to. Our Apps page could take inspiration: instead of rigid app icons, show AI-suggested actions contextually.

## 2.3 Amazon Echo Show 15/21 — 2025-2026 Redesign

**Sources**:
- https://smartbuy.alibaba.com/trends/everything-announced-at-amazons-alexa-event-today-alexa-plus-new-echo-show-ui-and-more
- https://www.the-ambient.com/reviews/amazon-echo-show-15-2nd-gen-review/

**New Dynamic Home Screen** (OTA to existing devices Jan 2026):
- **Time-of-day adaptation**: Morning shows weather, calendar, traffic; evening shifts to entertainment and smart home status
- **Presence-aware**: Adapts content based on user proximity using on-device computer vision (Echo Show 8 "Adaptive Content")
- **Widget Grid**: Customizable sections for calendars, shopping lists, to-dos, smart home favorites
- Swipe down from top for Widget Gallery
- 4 dedicated home screen sections with custom UIs per category

**Widget Types Available**:
- Calendar widgets, shopping lists, to-do lists
- Smart home device favorites
- Media recommendations
- Weather, traffic, schedule composites
- Fire TV content integration

**Pages/Tabs**:
1. Morning/Afternoon/Evening (contextual)
2. Home Control (smart devices)
3. Media (music, video, podcasts, news)
4. Communicate (calls, broadcasts, contacts)

**Takeaway for Glyph OS**: The time-of-day adaptive home screen is brilliant. Our Home page should shift content based on context. Morning: weather + calendar + AI morning briefing. Evening: ambient mode + dimmed display + sleep sounds.

## 2.4 Google Nest Hub — Ambient Mode

**Source**: https://support.google.com/googlenest/answer/9137130

- **Ambient EQ**: Dynamically adjusts color temperature and brightness to match room lighting
- Photo Frame mode cycles through Google Photos or Art Gallery
- Fullscreen clock option
- Pages: Schedule/Weather, Home Control, Media, Communicate
- Cards can be dismissed (some are persistent)
- Limited customization of home screen tabs/widgets

**Takeaway for Glyph OS**: Ambient EQ is a great feature to implement — use the ambient light sensor to shift display warmth. The persistent clock/photo frame as ambient mode is the baseline expectation.

## 2.5 Nothing OS 3.0 — Design Language

**Sources**:
- https://us.nothing.tech/pages/nothing-os-3
- https://thenextgadgets.com/nothing-os-3-a-big-leap-in-minimalism-functionality-personalization/

**Visual Language**:
- Monochrome tones, simple shapes, low clutter
- **Dot Engine**: Dot-based animations on fingerprint reader and UI elements
- Dot-matrix typography for retro-digital feel (signature aesthetic)
- Newer builds mix conventional fonts with dot elements strategically
- Custom icon themes: monochrome, minimal, or dynamic designs
- Redesigned Quick Settings with full icon size customization

**Widget System**:
- Date countdown widgets
- Shared Widgets (sync content between users)
- Enhanced widget customization options
- New widget sizes and interaction patterns

**Takeaway for Glyph OS**: The dot-matrix as accent typography is genius for embedded. We could use a dot-matrix or pixel font for status indicators and secondary text while keeping a clean sans-serif for primary content. The monochrome-with-one-accent-color approach maps perfectly to dark embedded UI.

## 2.6 Teenage Engineering OP-1 / EP-133 K.O.II — Tiny Screen Premium

**Sources**:
- https://teenage.engineering/guides/ep-133/screen
- Figma community files for OP-1 and EP-133

**OP-1 Display**: 320 x 160 pixel OLED
- Each mode has a **unique graphical representation** — not generic widgets
- Mix of traditional symbols (ADSR envelope) and unconventional illustrations (a boxer for "punch" effect)
- Monochrome with selective color accents
- Every screen is a **custom illustration**, not a generic UI layout
- The constraint (tiny screen) forces creative visual communication

**EP-133 K.O.II Display**:
- 66 unique icons, each representing a different feature
- Icon-based navigation system
- Custom display layout per mode
- Figma replica available: https://www.figma.com/community/file/1374736675420546476

**Takeaway for Glyph OS**: Each page should have its own distinct visual personality. The Home page could use an illustrative orb, the Apps page a clean grid, the Dragon page a technical overlay, and Settings a minimal list. Don't make every page look the same — make each one feel like opening a different "chapter."

---

# 3. Embedded Animation Patterns That Actually Work

## 3.1 ESP32-P4 Performance Baseline

**PPA Hardware Acceleration** (Source: https://docs.lvgl.io/master/integration/chip_vendors/espressif/hardware_accelerator_ppa.html):

Enable in `sdkconfig.defaults`:
```
CONFIG_LV_USE_PPA=y
CONFIG_LV_DRAW_BUF_ALIGN=64
```

**Performance gains**:
- **30% faster render times** for rectangle fills and image blending
- **Up to 9x speedup** for full-screen fills matching display dimensions
- **40% time savings** for hardware rotation
- Experimental — some tearing issues with PPA queue timing (fixed in PR #9162)

**Resolution confirmed working**: 800x1280 on JC1060P4A1 MIPI-DSI panel at 60Hz (very close to our 720x1280 target!)

**Optimal sdkconfig for ESP32-P4 + LVGL** (Source: https://github.com/arendst/Tasmota/discussions/24448):
```
# Compiler
CONFIG_COMPILER_OPTIMIZATION_PERF=y

# Cache
CONFIG_CACHE_L2_CACHE_128KB=y
CONFIG_CACHE_L2_CACHE_SIZE=0x20000

# Display ISR Safety (prevents glitches during flash operations)
CONFIG_LCD_DSI_ISR_IRAM_SAFE=y
CONFIG_LCD_DSI_ISR_CACHE_SAFE=y
CONFIG_GDMA_CTRL_FUNC_IN_IRAM=y
CONFIG_GDMA_ISR_IRAM_SAFE=y
CONFIG_TOUCH_ISR_IRAM_SAFE=y
CONFIG_I2C_ISR_IRAM_SAFE=y

# CPU
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_400=y
```

**LVGL Buffer Configuration for smoothness**:
```c
LV_DRAW_LAYER_SIMPLE_BUF_SIZE    (4096 * 1024)   // 4MB layer buffer
LV_DRAW_THREAD_STACK_SIZE        (128 * 1024)     // 128KB thread stack
LV_IMG_CACHE_DEF_SIZE_PSRAM      (8192 * 1024)    // 8MB image cache
LV_FREETYPE_CACHE_FT_GLYPH_CNT  256              // FreeType glyph cache

// Double buffering with full-screen PSRAM buffers
bsp_display_cfg_t cfg = {
    .buffer_size = BSP_LCD_H_RES * BSP_LCD_V_RES,
    .double_buffer = 1,
    .flags.buff_spiram = true,
};
```

**General LVGL optimization rules** (Source: https://github.com/espressif/esp-bsp/blob/master/components/esp_lvgl_port/docs/performance.md):
- Buffer 10-25% of screen pixels (below 10% = severe degradation, above 25% = diminishing returns)
- Set refresh period to 10ms: `CONFIG_LV_DEF_REFR_PERIOD=10`
- Enable fast memory: `CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM=y`
- Use second core for LVGL task: `CONFIG_ESP_MAIN_TASK_AFFINITY_CPU1=y`

## 3.2 Breathing / Pulsing Orb Animation

This is the core animation for the Glyph OS Home page. Here's the exact LVGL v9 implementation pattern:

```c
// Breathing orb using reverse + infinite repeat
static void anim_size_cb(void *var, int32_t v) {
    lv_obj_set_size((lv_obj_t *)var, v, v);
}

static void anim_opa_cb(void *var, int32_t v) {
    lv_obj_set_style_opa((lv_obj_t *)var, v, 0);
}

static void anim_shadow_cb(void *var, int32_t v) {
    lv_obj_set_style_shadow_width((lv_obj_t *)var, v, 0);
}

void create_breathing_orb(lv_obj_t *parent) {
    lv_obj_t *orb = lv_obj_create(parent);
    lv_obj_remove_flag(orb, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(orb, 120, 120);
    lv_obj_center(orb);

    // Style: circular with glow
    lv_obj_set_style_radius(orb, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(orb, lv_color_hex(0x6C5CE7), 0);
    lv_obj_set_style_bg_opa(orb, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_color(orb, lv_color_hex(0x6C5CE7), 0);
    lv_obj_set_style_shadow_width(orb, 40, 0);
    lv_obj_set_style_shadow_spread(orb, 10, 0);
    lv_obj_set_style_shadow_opa(orb, LV_OPA_60, 0);
    lv_obj_set_style_border_width(orb, 0, 0);

    // Size breathing: 120px -> 140px -> 120px
    lv_anim_t a_size;
    lv_anim_init(&a_size);
    lv_anim_set_var(&a_size, orb);
    lv_anim_set_values(&a_size, 120, 140);
    lv_anim_set_duration(&a_size, 2000);
    lv_anim_set_reverse_duration(&a_size, 2000);
    lv_anim_set_reverse_delay(&a_size, 0);
    lv_anim_set_repeat_count(&a_size, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_repeat_delay(&a_size, 0);
    lv_anim_set_path_cb(&a_size, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&a_size, anim_size_cb);
    lv_anim_start(&a_size);

    // Shadow glow breathing: 40px -> 60px -> 40px
    lv_anim_t a_glow;
    lv_anim_init(&a_glow);
    lv_anim_set_var(&a_glow, orb);
    lv_anim_set_values(&a_glow, 40, 60);
    lv_anim_set_duration(&a_glow, 2000);
    lv_anim_set_reverse_duration(&a_glow, 2000);
    lv_anim_set_repeat_count(&a_glow, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a_glow, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&a_glow, anim_shadow_cb);
    lv_anim_start(&a_glow);
}
```

**Performance note**: Shadow rendering is the most expensive part. On ESP32-P4 with PPA, a 140px circle with 60px shadow at 720x1280 should render within frame budget. If not, reduce shadow_width or use a pre-rendered radial gradient image instead.

## 3.3 Page Transition Animations

**Available screen transitions in LVGL v9**:

```c
// Slide transitions (best for swipe-based navigation)
lv_screen_load_anim(new_screen, LV_SCREEN_LOAD_ANIM_MOVE_LEFT, 300, 0, true);
lv_screen_load_anim(new_screen, LV_SCREEN_LOAD_ANIM_MOVE_RIGHT, 300, 0, true);

// Overlay transitions (new screen slides over old)
lv_screen_load_anim(new_screen, LV_SCREEN_LOAD_ANIM_OVER_LEFT, 300, 0, true);

// Fade transitions (good for Home -> AI mode)
lv_screen_load_anim(new_screen, LV_SCREEN_LOAD_ANIM_FADE_IN, 400, 0, true);

// Parameters: (screen, anim_type, duration_ms, delay_ms, auto_delete_old)
```

**Full list of transition types**:
- `LV_SCREEN_LOAD_ANIM_NONE` — instant switch
- `LV_SCREEN_LOAD_ANIM_OVER_LEFT/RIGHT/TOP/BOTTOM` — new slides over old
- `LV_SCREEN_LOAD_ANIM_OUT_LEFT/RIGHT/TOP/BOTTOM` — old slides away
- `LV_SCREEN_LOAD_ANIM_MOVE_LEFT/RIGHT/TOP/BOTTOM` — both screens move
- `LV_SCREEN_LOAD_ANIM_FADE_IN/FADE_OUT` — opacity crossfade

**Recommended for Glyph OS**:
- Between adjacent pages: `MOVE_LEFT/RIGHT` at 300ms (feels like swiping)
- Home to AI active: `FADE_IN` at 400ms (feels like awakening)
- Settings overlays: `OVER_BOTTOM` at 250ms (slide-up sheet)

**Tileview alternative** (better for swipe-based multi-page):
```c
lv_obj_t *tv = lv_tileview_create(lv_screen_active());

lv_obj_t *home = lv_tileview_add_tile(tv, 0, 0, LV_DIR_RIGHT);
lv_obj_t *apps = lv_tileview_add_tile(tv, 1, 0, LV_DIR_LEFT | LV_DIR_RIGHT);
lv_obj_t *dragon = lv_tileview_add_tile(tv, 2, 0, LV_DIR_LEFT | LV_DIR_RIGHT);
lv_obj_t *settings = lv_tileview_add_tile(tv, 3, 0, LV_DIR_LEFT);

// Navigate programmatically:
lv_tileview_set_tile_by_index(tv, 1, 0, LV_ANIM_ON);

// Detect page changes:
// LV_EVENT_VALUE_CHANGED fires after tile change
// lv_tileview_get_tile_active(tv) returns current tile
```

## 3.4 Lottie Animation on ESP32

**Source**: https://github.com/0015/esp_rlottie, https://docs.lvgl.io/master/widgets/lottie.html

**LVGL v9 native Lottie support via ThorVG**:
- Built-in `lv_lottie` widget renders Lottie JSON files
- Backed by ThorVG rendering engine
- More efficient than the older rlottie approach

**ESP32 Limitations**:
- Original ESP32: max ~120x120 Lottie animations
- ESP32-S3: can handle larger (up to ~240x320 tested)
- **ESP32-P4**: Should handle significantly larger due to 400MHz + PPA, but untested at full 720x1280

**IDF Component**: https://components.espressif.com/components/espressif2022/lottie_player

**Recommendation for Glyph OS**: Use Lottie for small accent animations (loading spinners, icon transitions, success/error indicators). For the main orb animation, use native LVGL animations — they're more efficient and don't require the Lottie overhead.

## 3.5 Particle Effects via Canvas

LVGL's Canvas widget allows direct pixel manipulation:
- Drawing functions work directly on Canvas (layer at 0,0 regardless of position)
- Custom draw events: `LV_EVENT_DRAW_POST` for overlay effects
- For particles: render to canvas buffer, update positions each frame

**Practical approach for ESP32-P4**:
```c
// Create canvas for particle layer
lv_obj_t *canvas = lv_canvas_create(parent);
lv_canvas_set_buffer(canvas, buf, 200, 200, LV_COLOR_FORMAT_ARGB8888);

// In timer callback (every 33ms for ~30fps particles):
lv_canvas_fill_bg(canvas, lv_color_hex(0x000000), LV_OPA_TRANSP);
for (int i = 0; i < num_particles; i++) {
    update_particle_position(&particles[i]);
    lv_canvas_set_px_color(canvas, particles[i].x, particles[i].y,
                           particles[i].color);
}
lv_obj_invalidate(canvas);
```

**Performance reality**: Full-screen particle effects will tank FPS. Keep particle canvas small (200x200 max) and overlay it on a specific area. 30-50 particles at 30fps should be achievable.

## 3.6 Smartwatch Second-Hand Animation

Community project FASTSHIFT/WatchX achieves 60fps+ smooth animation on LVGL:
- Source: https://github.com/FASTSHIFT/WatchX
- Key technique: only invalidate the changed region (dirty rectangles), not full screen
- For second hand: invalidate only the arc swept between old and new position
- Use `lv_obj_invalidate_area()` for precise dirty region control

---

# 4. Dark UI Masterclass — Beyond Basics

## 4.1 The Premium Dark Color System

**Near-Black Backgrounds** (Source: Multiple dark UI design guides):

| Purpose | Hex Value | Notes |
|---------|-----------|-------|
| Primary Background | `#0D0D0F` | Near-black with slight blue undertone |
| Card/Surface | `#1A1A1E` | Elevated surface |
| Elevated Surface | `#242428` | Modals, dropdowns |
| Subtle Border | `#2A2A2E` | 1px borders for depth |
| Muted Text | `#6B6B76` | Secondary information |
| Body Text | `#B4B4BE` | Primary readable text |
| Bright Text | `#EFEFEF` | Headlines, emphasis |
| Accent | `#6C5CE7` | Purple (our brand) |
| Accent Glow | `#6C5CE7` at 30% opa | For shadow/glow effects |

**Why NOT pure black (#000000)**: Creates "edge vibration" with bright text on OLED/LCD. Pure black with white text causes eye strain. A near-black (`#0D0D0F` to `#121212`) paired with softened light text (`#EFEFEF` not `#FFFFFF`) performs better for extended use.

**Google's Material Design dark surface**: `#121212` is the established standard.

## 4.2 Linear Design Trend Analysis (2025-2026)

**Source**: https://blog.logrocket.com/ux-design/linear-design/

The "Linear design" aesthetic (named after Linear app) defines premium SaaS/tool UI in 2025-2026:

**Core characteristics**:
- Dark mode as default (not optional)
- Bold sans-serif typefaces (but the trend is maturing — differentiation needed)
- Gradients: simple for content-heavy screens, complex for marketing
- Glassmorphism: "sleek glass effect" creating "detail with readability"
- Noisy overlays (Raycast's 2025 redesign)
- Monochrome with minimal accent color

**Linear's evolution**:
- 2024: Dull, monochrome blue palette
- 2025: Shifted to monochrome black/white with even fewer bold colors
- Key insight: **Less color = more premium**

**Raycast**: Bold red accent, abstract visuals with noisy grain overlays

**Arc Browser**: Clean design, soft gradients, purposeful typography, generous spacing

**Takeaway for Glyph OS**: Follow the "monochrome + single accent" formula. Our purple accent against near-black backgrounds, with everything else in grayscale. Add subtle grain texture to backgrounds for depth.

## 4.3 Glassmorphism on LVGL v9 — It's Now Possible!

**LVGL v9.5 introduced native blur support** (Source: https://lvgl.io/blog/release-v9-5):

```c
// Glassmorphism panel
lv_obj_t *glass_panel = lv_obj_create(parent);
lv_obj_set_size(glass_panel, 300, 200);

// Semi-transparent background
lv_obj_set_style_bg_color(glass_panel, lv_color_hex(0x1A1A1E), 0);
lv_obj_set_style_bg_opa(glass_panel, LV_OPA_70, 0);  // 70% opacity

// Backdrop blur (frosted glass effect)
lv_obj_set_style_blur_backdrop(glass_panel, true, 0);
lv_obj_set_style_blur_radius(glass_panel, 20, 0);
lv_obj_set_style_blur_quality(glass_panel, LV_BLUR_QUALITY_AUTO, 0);

// Subtle border (characteristic of glassmorphism)
lv_obj_set_style_border_color(glass_panel, lv_color_hex(0xFFFFFF), 0);
lv_obj_set_style_border_opa(glass_panel, LV_OPA_10, 0);  // 10% white border
lv_obj_set_style_border_width(glass_panel, 1, 0);

// Rounded corners
lv_obj_set_style_radius(glass_panel, 16, 0);
```

**Blur quality options**:
- `LV_BLUR_QUALITY_SPEED` — faster, lower quality
- `LV_BLUR_QUALITY_PRECISION` — slower, higher quality
- `LV_BLUR_QUALITY_AUTO` — automatically selected

**Performance warning**: Blur creates a layer, renders the widget + children, then blurs the result. This is expensive. Use sparingly — maybe for notification overlays or the AI conversation panel, not for every card.

## 4.4 Gradient Techniques for ESP32-P4

**LVGL v9 supports 5 gradient types** (requires `LV_USE_DRAW_SW_COMPLEX_GRADIENTS=y`):

1. **Horizontal/Vertical** (cheap): `lv_style_set_bg_grad_dir(LV_GRAD_DIR_HOR/VER)`
2. **Skew** (moderate)
3. **Radial** (moderate-expensive): Perfect for orb glow backgrounds
4. **Conical** (expensive): Metallic knob effects

**Radial gradient for orb background**:
```c
static lv_grad_dsc_t grad;
static const lv_color_t grad_colors[3] = {
    lv_color_hex(0x6C5CE7),  // Purple center
    lv_color_hex(0x2D1B69),  // Dark purple mid
    lv_color_hex(0x0D0D0F),  // Near-black edge
};
lv_grad_init_stops(&grad, grad_colors, NULL, NULL, 3);
lv_grad_radial_init(&grad, LV_GRAD_CENTER, LV_GRAD_CENTER,
                    LV_GRAD_RIGHT, LV_GRAD_BOTTOM, LV_GRAD_EXTEND_PAD);
lv_style_set_bg_grad(&style, &grad);
```

**Conical gradient for metallic/premium accents** (from official example):
```c
// 8-stop conical gradient creates metallic knob appearance
static const lv_color_t grad_colors[8] = {
    LV_COLOR_MAKE(0xe8, 0xe8, 0xe8),
    LV_COLOR_MAKE(0xff, 0xff, 0xff),
    LV_COLOR_MAKE(0xfa, 0xfa, 0xfa),
    LV_COLOR_MAKE(0x79, 0x79, 0x79),
    LV_COLOR_MAKE(0x48, 0x48, 0x48),
    LV_COLOR_MAKE(0x4b, 0x4b, 0x4b),
    LV_COLOR_MAKE(0x70, 0x70, 0x70),
    LV_COLOR_MAKE(0xe8, 0xe8, 0xe8),
};
lv_grad_conical_init(&grad, LV_GRAD_CENTER, LV_GRAD_CENTER,
                     0, 120, LV_GRAD_EXTEND_REFLECT);
```

Set `LV_GRADIENT_MAX_STOPS` to 8 in lv_conf.h for full gradient capability.

## 4.5 Apple Dynamic Island — Lessons for AI Orb

**Source**: https://medium.com/@shubhamdeepgupta/apple-dynamic-island-wave-micro-interaction-ios-design-user-delight-experience-665fefa13025

**Key animation principles**:
- Morphs between 3 states: compact pill, medium banner, large card
- Transitions: **0.3 to 0.5 seconds** — fast enough to feel responsive, slow enough to track visually
- Animation feels like **liquid** — makes it feel alive, not mechanical
- During music: wave pulses sync with audio, adapts to album art gradient colors
- Each context has **unique visual identity** while feeling like a family

**Applicable to Glyph OS AI orb**:
- Idle: small breathing circle (compact)
- Listening: expand to medium with waveform visualization (medium)
- Responding: full card with text output (large)
- Use 300-500ms transitions between states
- Add slight overshoot (`lv_anim_path_overshoot`) for organic feel

## 4.6 Blend Modes in LVGL v9

Available blend modes for layering effects:
- `LV_BLEND_MODE_NORMAL` — standard rendering
- `LV_BLEND_MODE_ADDITIVE` — adds colors (great for glow/light effects)
- `LV_BLEND_MODE_SUBTRACTIVE` — darkens
- `LV_BLEND_MODE_MULTIPLY` — multiplies colors (overlay effect)
- `LV_BLEND_MODE_DIFFERENCE` — inverts where overlapping

```c
lv_obj_set_style_blend_mode(glow_layer, LV_BLEND_MODE_ADDITIVE, 0);
```

**Note**: Non-normal blend modes trigger layer creation (performance cost). Use on images or small elements, not full-screen.

## 4.7 Complete LVGL Style Properties Reference for Premium UI

**Shadow (for glow/depth)**:
```c
lv_style_set_shadow_width(&s, 30);        // Blur radius
lv_style_set_shadow_spread(&s, 5);        // Extend shadow area
lv_style_set_shadow_offset_x(&s, 0);      // X offset
lv_style_set_shadow_offset_y(&s, 4);      // Y offset (subtle downward)
lv_style_set_shadow_color(&s, color);      // Shadow color
lv_style_set_shadow_opa(&s, LV_OPA_30);   // 30% opacity for subtlety
```

**Opacity layers**:
```c
lv_style_set_bg_opa(&s, LV_OPA_70);       // Background opacity
lv_style_set_opa(&s, LV_OPA_COVER);       // Widget opacity (scales children)
lv_style_set_opa_layered(&s, 200);         // Layer-based opacity (0-255)
```

**Border (subtle glass edges)**:
```c
lv_style_set_border_width(&s, 1);
lv_style_set_border_color(&s, lv_color_hex(0xFFFFFF));
lv_style_set_border_opa(&s, LV_OPA_10);   // Nearly invisible white line
lv_style_set_border_side(&s, LV_BORDER_SIDE_FULL);
```

---

# 5. Touch + Voice Hybrid UX Deep Dive

## 5.1 Amazon Echo Show Voice/Touch Handoff

**Source**: https://futurumgroup.com/insights/ai-enabled-features-in-amazon-echo-products-point-to-the-future-of-ux/

**Key UX patterns**:
- **Proactive AI**: System anticipates needs, doesn't just react to prompts
- **Adaptive Content**: On-device computer vision adjusts content based on user proximity
  - Far away: large clock, ambient photos
  - Approaching: shows relevant widgets (weather, calendar)
  - Close up: interactive controls become visible
- **Tap-to-Alexa**: Alternative input for non-voice situations
- **Gesture recognition**: Raise hand to stop timer, tap to interact
- **Consolidated captions**: Visual text feedback for voice responses

**Takeaway for Glyph OS**: Implement proximity-based UI adaptation if hardware allows. At minimum: dim to ambient mode after timeout, brighten and show interactive elements on touch/proximity.

## 5.2 Visual Indicators for "AI Readiness"

**Source**: Multiple VUI design resources

**The 4 AI States and Their Visual Feedback**:

| State | Visual | Audio | Duration |
|-------|--------|-------|----------|
| **Idle** | Breathing orb, subtle glow | Silent | Indefinite |
| **Listening** | Orb expands, waveform appears, color intensifies | Subtle chime | Until silence detected |
| **Processing** | Orb pulses faster, loading animation | None | Variable |
| **Responding** | Orb stabilizes, text appears, color shifts | Voice output | Until complete |

**Amazon Echo approach**: Blue ring light swirls when listening, pulses during processing, solid when responding.

**Siri wave approach** (Source: https://github.com/kopiro/siriwave):
- 5 layered sine curves with different attenuation values
- Speed parameter (default 0.2) controls animation velocity
- Amplitude parameter (default 1) indicates listening intensity
- Frequency parameter (default 6) affects oscillation
- `globalCompositeOperation: "lighter"` for additive blending
- Curves defined with attenuation and opacity:
  ```
  { attenuation: -2, lineWidth: 1, opacity: 0.1 }
  { attenuation: -6, lineWidth: 1, opacity: 0.2 }
  { attenuation:  4, lineWidth: 1, opacity: 0.4 }
  { attenuation:  2, lineWidth: 1, opacity: 0.6 }
  { attenuation:  1, lineWidth: 1.5, opacity: 1.0 }
  ```

**Implementation for LVGL**: Render Siri-style waveform on a Canvas widget. Use `LV_BLEND_MODE_ADDITIVE` for the glow effect. Update at 30fps via timer callback.

## 5.3 Notification Priority System for Smart Displays

**Source**: https://www.nngroup.com/articles/smart-home-notifications/ (Nielsen Norman Group)

**7 Core Principles**:

1. **Timely**: Reactive = instant; Proactive = advance warning; Optimization = contextual
2. **Relevant**: Core functions take priority; allow customization for secondary alerts
3. **Specific**: "Person at front door" beats "Motion detected"
4. **Right Intensity**: Match urgency to visual prominence
   - Critical: Full-screen takeover, red accent, audio
   - Important: Banner at top, amber accent
   - Informational: Subtle indicator, no interruption
5. **Right Frequency**: Threshold-based filtering prevents fatigue
6. **Right Channel**: Push for urgent, on-device for proximity-aware, in-app for browsing
7. **Adaptable**: Location-based, snooze-able, routine-customizable

**Notification Type Matrix for Glyph OS**:

| Type | Visual Treatment | Interrupts? |
|------|-----------------|-------------|
| AI Response Ready | Orb color shift + gentle pulse | No |
| Incoming Call | Full overlay + vibration | Yes |
| Timer/Alarm | Expanding ring animation | Yes |
| Smart Home Alert | Banner slide-down, 5s auto-dismiss | Light |
| System Update | Dot indicator on Settings icon | No |
| Weather Change | Subtle Home page update | No |

## 5.4 UX for Dual-Purpose Device (Remote Desktop + AI Assistant)

**Key challenge**: The Dragon page (remote desktop viewer) serves a completely different use case than the AI assistant pages.

**Remote desktop touch patterns** (Source: Various RDP client UX research):
- **Two modes**: Touchpad mode (cursor follows finger like laptop trackpad) vs Touchscreen mode (tap = click at position)
- **Pan/scroll**: Two-finger drag for scrolling, pinch for zoom
- **Overlay controls**: Floating toolbar with keyboard toggle, disconnect, settings
- **Edge gestures**: Swipe from edge to reveal controls, swipe back to dismiss

**Recommended UX for Dragon page**:
1. Full-screen stream by default (maximizes usable area on 720x1280)
2. Swipe from right edge: floating control bar (keyboard, mouse mode, disconnect)
3. Swipe from left edge: return to Glyph OS (previous page)
4. Pinch to zoom for detail work
5. Long press: right-click
6. Double tap: double-click
7. Status bar at top: connection quality, latency, back button

---

# 6. Competitive Product Deep Dive

## 6.1 Brilliant Smart Home Control

**Source**: https://www.brilliant.tech/

**Hardware**: 5" LCD touchscreen, motion sensor, integrated camera with privacy shutter

**UI/UX Analysis**:
- "Designed so everyone can quickly understand how it works with no training"
- **Gesture-first**: Slide finger up/down on screen to dim/brighten lights
- Multi-switch: Multiple touch sliders visible simultaneously
- 4 customizable home screen sections: shortcuts, scenes, device access
- Category-dependent custom UIs (lighting UI differs from thermostat UI)
- Built-in Alexa with visual responses
- **Works without WiFi** via local mesh network (critical reliability)

**Design lessons**: The simplicity is the product. One gesture (slide up/down) does the primary thing. No menu diving for the #1 use case.

**Takeaway for Glyph OS**: The Home page should have ONE primary interaction that's immediately obvious. For us: tap the orb to talk to AI. Everything else is secondary.

## 6.2 Home Assistant Tablet Dashboards

**Source**: https://smarthomescene.com/guides/mushroom-cards-complete-guide-to-a-clean-minimalistic-home-assistant-ui/

**Mushroom Cards** (the gold standard for HA dashboards):
- Visual editor — no code needed for basic setup
- Card types: Light, Climate, Media, Entity, Template, Chips
- Sections layout: drag-and-drop grid system
- Conditional visibility for context-aware display

**Design principles from best dashboards**:
- "Quick information at a glance about current state"
- "Take action in just a click globally across the home"
- Screen size determines information density — 7" tablet needs less than 27" monitor
- Consistent card sizing with flexible width slider
- Accent colors for entity states (on=warm, off=gray)

## 6.3 Elgato Stream Deck + (Touchscreen)

**Source**: https://docs.elgato.com/streamdeck/sdk/references/touchscreen-layout/

**Hardware**: 8 LCD keys + 4 encoders + 108x14mm LCD touch strip

**UI Patterns**:
- Each key: crisp resolution, supports static or dynamic icons
- Up to 10 swipeable pages
- Custom touchscreen layouts beyond built-in templates
- Grid alignment for easy reach, dials for thumb access

**Key lesson**: The power is in customizable, context-switching grids. Each page is a different "context" (streaming, editing, gaming) — similar to our 4-page concept.

## 6.4 Tesla Touchscreen UI

**Source**: https://medium.com/@ethanwwm/a-deep-dive-into-teslas-user-interface-9c4aa3e6a4ab

**Design Philosophy**:
- Minimalist touchscreen replaces ALL physical buttons (radical commitment)
- Dark UI reduces eye strain while driving at night
- **Near-black background** (not pure black) allows shadow depth and elevation
- Text = 80%+ of design surface — typography is critical
- One dominant primary action per view
- Secondary actions: lower luminance emphasis
- Edge vibration avoided by using `#121212`-range backgrounds with `#E0E0E0`-range text

**Typography hierarchy in dark mode**:
- Bright object on dark background has strong visual pull
- Use this sparingly — only the most important element gets high contrast
- Secondary elements get progressively lower contrast

## 6.5 Framework Laptop 16 LED Matrix

**Source**: https://frame.work/products/16-led-matrix

- 9x34 (306 LEDs) matrix, 256 brightness levels
- RP2040 MCU + IS31FL3741A LED controller
- USB Serial API for host control
- Use cases: battery level, timer, notifications, scrolling text, Snake game

**Design lesson**: Even a 9x34 LED grid can communicate useful information. Our 720x1280 display is luxury by comparison — but the principle holds: every pixel should earn its place.

## 6.6 ESP32 + LVGL Commercial Products

**Confirmed shipping commercial products using ESP32 + LVGL**:

1. **WT-86-32-3ZW1 Intelligent Panel** — ESP32-S2 driving 320x320 3.92" touch screen, sold as smart home panel
2. **Elecrow CrowPanel series** — Multiple sizes (3.5" to 7"), ESP32-S3 and ESP32-P4, retail products
3. **ESP32-S3-BOX-3** — Espressif's reference design: 320x240 SPI touchscreen, 2 mics, speaker, sold retail
4. **ESP32-S3-Korvo-2** — Multimedia board with LCD, camera, mics, for audio/video products
5. **IDO Smartwatch line** — Mass-market smartwatches running LVGL on ARM (not ESP32 but LVGL proof)
6. **Xiaomi Watch S1 Pro/S3/S4** — Consumer electronics running LVGL at scale

---

# 7. Implementation Recipes for Glyph OS

## 7.1 Typography System

**Font rendering on ESP32-P4**:
- Use **FreeType** for runtime TTF rendering (ESP32-P4 has enough power)
- Alternative: Pre-render fonts with LVGL online converter at 4bpp for anti-aliasing
- Subpixel rendering available (3x horizontal resolution) but needs 3x memory per glyph

**Recommended type scale** (8px base grid):
```
Heading XL:   32px, Bold    — Page titles
Heading L:    24px, Semibold — Section headers
Body:         16px, Regular  — Primary text
Body Small:   14px, Regular  — Secondary text
Caption:      12px, Regular  — Labels, timestamps
Mono/Status:  12px, Mono     — System status, dot-matrix accent
```

**Font recommendation**: Inter or Montserrat (LVGL ships with Montserrat built-in). For the dot-matrix accent: use a custom pixel font for status indicators.

## 7.2 8px Spacing System

```
XS:   4px   — tight padding within components
S:    8px   — standard inner padding
M:    16px  — card padding, section gaps
L:    24px  — between major sections
XL:   32px  — page margins (left/right on 720px = 656px content width)
XXL:  48px  — top/bottom safe zones
```

## 7.3 LVGL Design System Setup

```c
// Theme initialization
lv_theme_t *th = lv_theme_default_init(
    display,
    lv_color_hex(0x6C5CE7),    // Primary (purple)
    lv_color_hex(0x00D2D3),    // Secondary (cyan)
    true,                       // Dark mode
    &lv_font_montserrat_16     // Default font
);

// Global styles
static lv_style_t style_bg;
lv_style_init(&style_bg);
lv_style_set_bg_color(&style_bg, lv_color_hex(0x0D0D0F));
lv_style_set_text_color(&style_bg, lv_color_hex(0xB4B4BE));

static lv_style_t style_card;
lv_style_init(&style_card);
lv_style_set_bg_color(&style_card, lv_color_hex(0x1A1A1E));
lv_style_set_bg_opa(&style_card, LV_OPA_COVER);
lv_style_set_radius(&style_card, 12);
lv_style_set_border_width(&style_card, 1);
lv_style_set_border_color(&style_card, lv_color_hex(0x2A2A2E));
lv_style_set_border_opa(&style_card, LV_OPA_COVER);
lv_style_set_pad_all(&style_card, 16);
lv_style_set_shadow_width(&style_card, 20);
lv_style_set_shadow_color(&style_card, lv_color_hex(0x000000));
lv_style_set_shadow_opa(&style_card, LV_OPA_20);
lv_style_set_shadow_offset_y(&style_card, 4);

static lv_style_t style_accent;
lv_style_init(&style_accent);
lv_style_set_bg_color(&style_accent, lv_color_hex(0x6C5CE7));
lv_style_set_text_color(&style_accent, lv_color_hex(0xFFFFFF));
```

## 7.4 Page Architecture with Tileview

```c
// 4-page horizontal tileview
lv_obj_t *tv = lv_tileview_create(lv_screen_active());
lv_obj_set_style_bg_color(tv, lv_color_hex(0x0D0D0F), 0);

// Page 0: Home (can swipe right to Apps)
lv_obj_t *home_tile = lv_tileview_add_tile(tv, 0, 0, LV_DIR_RIGHT);
build_home_page(home_tile);

// Page 1: Apps (can swipe left to Home, right to Dragon)
lv_obj_t *apps_tile = lv_tileview_add_tile(tv, 1, 0,
    (lv_dir_t)(LV_DIR_LEFT | LV_DIR_RIGHT));
build_apps_page(apps_tile);

// Page 2: Dragon (can swipe left to Apps, right to Settings)
lv_obj_t *dragon_tile = lv_tileview_add_tile(tv, 2, 0,
    (lv_dir_t)(LV_DIR_LEFT | LV_DIR_RIGHT));
build_dragon_page(dragon_tile);

// Page 3: Settings (can swipe left to Dragon)
lv_obj_t *settings_tile = lv_tileview_add_tile(tv, 3, 0, LV_DIR_LEFT);
build_settings_page(settings_tile);

// Page indicator dots
lv_obj_add_event_cb(tv, page_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
```

## 7.5 LED Widget for Quick Status Indicators

```c
lv_obj_t *led = lv_led_create(parent);
lv_led_set_color(led, lv_palette_main(LV_PALETTE_GREEN));
lv_led_set_brightness(led, 200);  // 0-255 range
// LV_LED_BRIGHT_MIN = 80, LV_LED_BRIGHT_MAX = 255

// Animate brightness for breathing status LED:
lv_anim_t a;
lv_anim_init(&a);
lv_anim_set_var(&a, led);
lv_anim_set_values(&a, 80, 255);
lv_anim_set_duration(&a, 1500);
lv_anim_set_reverse_duration(&a, 1500);
lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_led_set_brightness);
lv_anim_start(&a);
```

## 7.6 Image Asset Pipeline

**Supported formats in LVGL v9**:
- PNG via LodePNG decoder (`LV_USE_LODEPNG=y`)
- SJPG (Split JPEG) — progressive decode, only loads visible portion
- Raw C arrays — compiled into firmware, fastest access
- BIN files — stored on filesystem, loaded on demand

**Recommended workflow**:
1. Design assets in Figma at 720x1280
2. Export icons as PNG with alpha channel
3. Use LVGL Online Image Converter to generate C arrays for small/frequent icons
4. Store larger images (backgrounds, photos) as SJPG on SPIFFS/LittleFS
5. Set `LV_IMG_CACHE_DEF_SIZE_PSRAM=8MB` for smooth image switching

---

# 8. Sources and References

## LVGL Documentation
- Animation system: https://docs.lvgl.io/master/main-modules/animation.html
- Style properties: https://docs.lvgl.io/master/common-widget-features/styles/style-properties.html
- Screen transitions: https://docs.lvgl.io/master/common-widget-features/screens.html
- Tileview widget: https://docs.lvgl.io/master/widgets/tileview.html
- LED widget: https://docs.lvgl.io/master/widgets/led.html
- Lottie player: https://docs.lvgl.io/master/widgets/lottie.html
- PPA acceleration: https://docs.lvgl.io/master/integration/chip_vendors/espressif/hardware_accelerator_ppa.html
- Blur/glassmorphism: https://lvgl.io/blog/release-v9-5
- Gradient examples: https://github.com/lvgl/lvgl/blob/master/examples/styles/lv_example_style_17.c
- FreeType fonts: https://docs.lvgl.io/8/overview/font.html
- Image decoders: https://docs.lvgl.io/master/main-modules/images/decoders.html
- Themes: https://docs.lvgl.io/master/common-widget-features/styles/themes.html

## ESP32-P4 Performance
- LVGL port performance guide: https://github.com/espressif/esp-bsp/blob/master/components/esp_lvgl_port/docs/performance.md
- Tasmota MIPI-DSI performance notes: https://github.com/arendst/Tasmota/discussions/24448
- PPA tearing issue/fix: https://github.com/lvgl/lvgl/issues/9046
- ESP32-P4 vs S3 comparison: https://viewedisplay.com/esp32-p4-vs-esp32-s3/

## Case Studies
- Xiaomi LVGL: https://lvgl.io/case-studies/xiaomi
- IDO smartwatch LVGL: https://lvgl.io/case-studies/ido
- NXP smartwatch demo: https://forum.lvgl.io/t/how-to-obtain-the-source-code-for-the-official-perfect-smartwatch-gui-demo/17160

## ESP32-P4 Projects
- Tab5 Arduino LVGL: https://github.com/nikthefix/M5Stack_Tab5_Arduino_Basic_LVGL_Demo
- Tab5 Home Assistant: https://github.com/GalusPeres/Tab5-HomeAssistant-Display
- Tab5 User Demo: https://github.com/m5stack/M5Tab5-UserDemo
- Wokwi MIPI-DSI demo: https://github.com/wokwi/esp32p4-mipi-dsi-panel-demo
- Elecrow CrowPanel P4: https://www.elecrow.com/wiki/CrowPanel_Advanced_7inch_ESP32-P4_HMI_AI_Display_1024x600_IPS_Touch_Screen_with_WiFi6_Compatible_with_ArduinoLVGL.html
- ESP rlottie component: https://github.com/0015/esp_rlottie

## AI Companion Devices
- Rabbit R1 OS2: https://www.rabbit.tech/newsroom/rabbitos-2-launch
- Humane Cosmos OS: https://humane.com/aipin/cosmos
- Echo Show widgets: https://www.amazon.com/gp/help/customer/display.html?nodeId=G43W5FSE9NHDWUQV
- Nest Hub display: https://support.google.com/googlenest/answer/9137130

## Design References
- Linear design trend: https://blog.logrocket.com/ux-design/linear-design/
- Nothing OS 3.0: https://us.nothing.tech/pages/nothing-os-3
- Teenage Engineering EP-133: https://teenage.engineering/guides/ep-133/screen
- OP-1 screen graphics Figma: https://www.figma.com/community/file/1107218520343060211
- Dynamic Island analysis: https://medium.com/@shubhamdeepgupta/apple-dynamic-island-wave-micro-interaction-ios-design-user-delight-experience-665fefa13025
- Dark UI best practices: https://www.uinkits.com/blog-post/best-dark-mode-ui-design-examples-and-best-practices-in-2025
- Dark glassmorphism 2026: https://medium.com/@developer_89726/dark-glassmorphism-the-aesthetic-that-will-define-ui-in-2026-93aa4153088f
- Siri waveform JS: https://github.com/kopiro/siriwave

## Smart Display UX
- NN/G smart home notifications: https://www.nngroup.com/articles/smart-home-notifications/
- Alexa voice design guide: https://developer.amazon.com/en-US/blogs/alexa/post/ea99c8a1-36fa-4778-bbc3-56a6cee6e3b9/announcing-the-amazon-alexa-voice-design-guid
- Echo Show gestures: https://www.amazon.com/gp/help/customer/display.html?nodeId=TuZOXliWxqinuMRSS3

## Competitive Products
- Brilliant: https://www.brilliant.tech/
- Mushroom Cards: https://github.com/piitaya/lovelace-mushroom
- Stream Deck SDK layouts: https://docs.elgato.com/streamdeck/sdk/references/touchscreen-layout/
- Framework LED Matrix: https://frame.work/products/16-led-matrix
- awesome-lvgl: https://github.com/aptumfr/awesome-lvgl
