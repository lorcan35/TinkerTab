#!/usr/bin/env node
/**
 * Glyph OS v3 — Research-Informed UI Generation via Google Stitch
 * Feeds full design system + competitive analysis into every prompt
 */

import { StitchToolClient } from '@anthropic-ai/stitch-sdk' || true;

const API_KEY = 'AQ.Ab8RN6KkZM69cwQWQ-JkjUueYCFtGSJzlD2FeC1sds_aJ7vP9w';
const OUT_DIR = '/home/rebelforce/projects/TinkerTab/stitch-designs/v3';

import { writeFileSync } from 'fs';
import { execSync } from 'child_process';

// Try dynamic import for stitch
let Stitch;
try {
  const mod = await import('@google/stitch-sdk');
  Stitch = mod.StitchToolClient || mod.default?.StitchToolClient || mod;
} catch {
  // fallback: use npx
}

const DESIGN_SYSTEM = `
DESIGN SYSTEM — "Glyph OS" for a 720x1280 portrait dark-theme AI companion device.

CRITICAL DIMENSIONS: Exactly 720px wide × 1280px tall. Portrait orientation. This is a 7-inch IPS touchscreen.

COLOR PALETTE (research-backed, NOT pure black):
- Background primary: #0D0F12 (deep blue-gray, avoids dead-screen look)
- Surface/cards: #161920 (elevated layer)
- Surface high: #1E222B (modals, active states)
- Surface max: #262B36 (highest elevation)
- Text primary: #E8ECF1 (87% white)
- Text secondary: #8B95A5 (60% white)
- Text disabled: #4A5568
- Accent: #00E5FF (Electric Cyan — clean tech, AI-native)
- Accent dim: #00A3B3
- Success: #34D399 (green)
- Warning: #FFB300 (amber)
- Error: #FF3D3D
- Borders: #1E222B (subtle), #2D3444 (default)

ELEVATION: Express depth through surface color lightness, NOT shadows. Each layer is slightly lighter.

TYPOGRAPHY:
- Font: Space Grotesk for headlines/labels, Inter for body text
- Display/Hero: 56px bold (clock)
- H1: 36px semibold (page titles)
- H2: 28px medium (section titles)
- Body: 20px regular
- Caption: 16px regular
- Overline/Label: 12px medium, tracking-widest, uppercase

DESIGN PRINCIPLES (from competitive research):
1. Nothing OS discipline — monochrome base, accent color ONLY where attention is needed
2. Rabbit R1 cards — each page is one context, minimal cognitive load
3. Echo Show ambient — idle state IS the primary UI, glanceable from across the room
4. Linear warmth — warm grays not cold blue-blacks, 3-4 gray levels for depth
5. eDEX-UI density — data displays can feel cinematic with right typography
6. Typography IS the design on dark screens
7. Animation communicates AI state (ready, listening, thinking, responding)
8. Touch targets: minimum 48px, recommended 56px, primary actions 64px
9. 8px spacing grid throughout
10. Border radius: 12px for cards, 8px for buttons, 9999px for pills

NAVIGATION: 4 vertical dots on right edge showing current page. Bottom nav bar with 4 icons.
STATUS BAR: Top bar with time (left), "GLYPH" badge, and wifi/signal/battery icons (right).
`;

const pages = [
  {
    name: 'home',
    prompt: `${DESIGN_SYSTEM}

SCREEN: HOME PAGE — The ambient/idle screen. This is what users see 90% of the time.

PHILOSOPHY: This is a living surface, not a dashboard. It should feel like a window into the AI's awareness. Inspired by Apple StandBy, Nothing OS lock screen, and Echo Show ambient mode.

EXACT LAYOUT (720x1280):
- TOP STATUS BAR (0-56px): Left: "21:42" in bold 18px. Center-left: "GLYPH" badge (tiny pill, cyan border, 10px uppercase tracking-widest). Right: wifi + signal + battery icons at 18px, #8B95A5 color.

- HERO SECTION (200-500px from top, centered):
  - Large clock: "21:42" in 56px Space Grotesk Bold, #E8ECF1, centered
  - Date below: "Thursday, March 27" in 18px Inter, #8B95A5, centered
  - 16px gap

- AI STATUS ORB (centered, below date, ~120x120px):
  - A circular orb/ring in Electric Cyan #00E5FF
  - Subtle outer ring (breathing animation implied)
  - Inside or below the orb: small text "Ready" in 12px uppercase tracking-widest, #00A3B3
  - The orb should look alive — think pulsing energy, not just a static circle
  - Inspired by: Siri's waveform, Humane AI Pin projection, Nothing Glyph lights

- CONTEXTUAL CARDS (below orb, ~640-900px from top):
  - Two cards side by side, each ~320x140px with 16px gap
  - Left card: Weather — "23°C" large, "Partly Cloudy" caption, small cloud icon, bg #161920, 12px radius
  - Right card: Next event — "Team Sync" title, "2:00 PM" time, calendar icon, bg #161920, 12px radius
  - Cards have subtle 1px border in #1E222B

- AI INSIGHT CARD (~920-1060px from top):
  - Full width (minus 32px margins), ~140px tall
  - bg #161920, 12px radius, 1px border #2D3444
  - Small cyan dot indicator top-left (8px)
  - Title: "AI Insight" in 12px uppercase cyan
  - Body: "Rain expected at 4pm — consider taking an umbrella" in 18px #E8ECF1

- PAGE INDICATOR (right edge, vertically centered):
  - 4 dots vertical, 4px gap
  - First dot: 8x8px filled cyan #00E5FF with subtle glow
  - Other 3: 6x6px #2D3444 with #1E222B border

- BOTTOM NAV BAR (fixed, 1216-1280px):
  - bg #0D0F12 with top 1px border #1E222B
  - 4 icons evenly spaced: home (filled, cyan), apps (grid, #8B95A5), dragon (monitor, #8B95A5), settings (gear, #8B95A5)
  - Icons at 24px, touch targets 56px
  - Home icon has subtle cyan glow underneath

NO gradients on large areas. NO shadows. NO blur. Keep it sharp, clean, and minimal. The beauty comes from spacing, typography, and restraint.`
  },
  {
    name: 'apps',
    prompt: `${DESIGN_SYSTEM}

SCREEN: APPS PAGE — The tool launcher. Pure function, efficient grid.

PHILOSOPHY: Get in, launch, get out. Nothing OS monochrome discipline meets Rabbit R1 simplicity. No decoration — every pixel serves a purpose.

EXACT LAYOUT (720x1280):
- TOP STATUS BAR (0-56px): Same as other pages — time, GLYPH badge, status icons.

- PAGE HEADER (72-120px):
  - "Apps" in 36px Space Grotesk Semibold, #E8ECF1, left-aligned with 24px left margin
  - Search icon (magnifying glass) top-right, 24px, #8B95A5, in a 40x40 pill bg #161920

- APP GRID (144-960px):
  - 4 columns × 4 rows visible (16 apps)
  - Grid area: 688px wide (720 - 32px margins), cells are 172px wide
  - Each cell: 130px tall
  - Icon: 64x64px rounded square (12px radius) centered in cell
  - Label: 14px Inter below icon, #8B95A5, centered, single line

  APP ICONS (monochrome outline style on colored bg):
  Row 1:
  - "AI Chat" — icon bg #00E5FF/15%, icon outline #00E5FF, chat bubble icon
  - "Browser" — icon bg #B388FF/15%, icon outline #B388FF, globe icon
  - "Camera" — icon bg #FF6B9D/15%, icon outline #FF6B9D, camera icon
  - "Voice" — icon bg #FFB300/15%, icon outline #FFB300, mic icon

  Row 2:
  - "Files" — icon bg #34D399/15%, icon outline #34D399, folder icon
  - "Automations" — icon bg #00E5FF/15%, icon outline #00E5FF, bolt icon
  - "Terminal" — icon bg #8B95A5/15%, icon outline #8B95A5, terminal/code icon
  - "Notes" — icon bg #FFB300/15%, icon outline #FFB300, pencil icon

  Row 3:
  - "IoT Hub" — icon bg #34D399/15%, icon outline #34D399, devices icon
  - "Music" — icon bg #FF6B9D/15%, icon outline #FF6B9D, music note icon
  - "Clock" — icon bg #B388FF/15%, icon outline #B388FF, clock icon
  - "Maps" — icon bg #00E5FF/15%, icon outline #00E5FF, map pin icon

  Row 4:
  - "VPN" — icon bg #34D399/15%, icon outline #34D399, shield icon
  - "Wallet" — icon bg #FFB300/15%, icon outline #FFB300, wallet icon
  - "Health" — icon bg #FF6B9D/15%, icon outline #FF6B9D, heart icon
  - "Add +" — icon bg #1E222B, icon outline #4A5568, plus icon (dashed border)

- SEARCH BAR (988-1048px):
  - Full width minus 32px margins
  - 56px tall, bg #161920, 12px radius, 1px border #2D3444
  - Left: search icon 20px #4A5568
  - Placeholder: "Ask Glyph anything..." in 16px Inter #4A5568
  - This doubles as AI command input

- PAGE INDICATOR: Right edge, dot 2 active (cyan)

- BOTTOM NAV BAR (1216-1280px): Same structure, apps icon now active (cyan), others dimmed.

Icons should feel unified — all same line weight (2px stroke), same corner radius. Monochrome outline style like Nothing OS icons. The colored tint backgrounds are the only color.`
  },
  {
    name: 'dragon',
    prompt: `${DESIGN_SYSTEM}

SCREEN: DRAGON PAGE — Remote desktop control for the connected mini-PC ("Dragon").

PHILOSOPHY: Maximum screen real estate for the remote stream. Controls auto-hide. Inspired by VNC mobile viewers and eDEX-UI's cinematic data density. This page shows a LIVE video stream of a remote desktop.

EXACT LAYOUT (720x1280):
- The stream fills the ENTIRE screen (720x1280), edge to edge. Everything else overlays on top.

- STREAM BACKGROUND: Show a realistic-looking remote desktop display — a dark Linux desktop with some windows open (a terminal and a browser). Make it look like an actual screenshot of a desktop OS. Slightly dimmed/desaturated to let overlays read clearly.

- TOP OVERLAY BAR (0-56px, semi-transparent bg #0D0F12 at 80% opacity):
  - Left: "Dragon" text 18px Space Grotesk Semibold #E8ECF1
  - Center: Connection status pill — green dot (8px, #34D399) + "Connected" 12px #34D399, bg #34D399/10%, 9999px radius, padding 8px 16px
  - Right: overflow menu icon (three dots) 24px #8B95A5

- STREAM ON/OFF TOGGLE (top-right area, below top bar, ~72px from top):
  - A prominent pill toggle: "LIVE" text + toggle switch
  - When ON: cyan accent, "LIVE" with pulsing dot
  - When OFF: gray, "OFF" text
  - This is the stream enable/disable control the user requested
  - Size: ~120x40px pill shape

- LATENCY INDICATOR (top-left, below top bar, ~72px):
  - Small pill: "12ms" in 12px monospace, #34D399 text, bg #0D0F12/60%
  - Shows real-time latency

- FPS COUNTER (next to latency):
  - "24 FPS" in 12px monospace, #8B95A5, bg #0D0F12/60%

- BOTTOM TOOLBAR (1216-1280px, semi-transparent bg #0D0F12 at 80% opacity):
  - 5 evenly spaced tool icons, each 48x48 touch target:
  - Back arrow icon — sends back key to remote
  - Keyboard icon — toggle on-screen keyboard
  - Mouse/touch mode icon — switch input mode (has a small indicator dot showing current mode)
  - Lightning bolt icon — quick actions (screenshot, lock, etc)
  - Power/stop icon — disconnect (in #FF3D3D subtle)
  - All icons 24px, #E8ECF1, with labels below in 10px #8B95A5

- PAGE INDICATOR: Right edge, dot 3 active

- BOTTOM NAV BAR: Visible but more subtle (lower opacity since we're in immersive mode). Dragon icon active (cyan).

- DISCONNECTED STATE (alternative): If not connected, show centered:
  - Dragon icon large (64px) in #4A5568
  - "Dragon Offline" in 24px #8B95A5
  - "Tap to connect" in 16px #4A5568
  - Subtle pulsing ring animation around icon

The key insight: This page should feel like you're looking through a window at another computer. The overlays are thin, translucent, and stay out of the way. The stream dominates.`
  },
  {
    name: 'settings',
    prompt: `${DESIGN_SYSTEM}

SCREEN: SETTINGS PAGE — Configuration and system info. Utilitarian, clean, scannable.

PHILOSOPHY: Nothing OS 3.0 settings style — grouped lists, no cards, clear hierarchy. Android Settings pattern with our design tokens. Every row is a clear touch target.

EXACT LAYOUT (720x1280):
- TOP STATUS BAR (0-56px): Same as other pages.

- PAGE HEADER (72-120px):
  - "Settings" in 36px Space Grotesk Semibold, #E8ECF1, left-aligned 24px margin

- SCROLLABLE SETTINGS LIST (136-1200px):
  All groups separated by 24px vertical gap. Group label: 12px uppercase tracking-widest, #00E5FF (cyan accent for group labels), 24px left margin, 8px bottom margin.

  GROUP 1: "NETWORK"
  Background: #161920, 12px radius for the group container
  - Row 1 (64px): Left: Wifi icon (20px, #00E5FF) + 12px gap + "Wi-Fi" (18px #E8ECF1). Right: "Home_5G" (14px #8B95A5) + chevron (16px #4A5568). Bottom 1px border #1E222B
  - Row 2 (64px): Bluetooth icon (#B388FF) + "Bluetooth". Right: toggle switch ON (cyan track #00E5FF, white thumb). Bottom border.
  - Row 3 (64px): Monitor icon (#34D399) + "Dragon Link". Right: green dot (8px #34D399) + "Connected" (14px #34D399) + chevron.

  GROUP 2: "DISPLAY"
  - Row 1 (64px): Sun icon (#FFB300) + "Brightness". Right: inline slider (track #2D3444, filled portion #00E5FF, thumb white 16px circle), slider width ~200px
  - Row 2 (64px): Moon icon (#B388FF) + "Night Mode". Right: toggle switch OFF (#2D3444 track, #4A5568 thumb)
  - Row 3 (64px): Timer icon (#8B95A5) + "Auto-Dim". Right: toggle ON
  - Row 4 (64px): Clock icon (#8B95A5) + "Screen Timeout". Right: "5 min" (14px #8B95A5) + chevron

  GROUP 3: "AI ASSISTANT"
  - Row 1 (64px): Waveform icon (#00E5FF) + "Voice Model". Right: "Glyph Neural v2" (14px #8B95A5) + chevron
  - Row 2 (64px): Mic icon (#FFB300) + "Wake Word". Right: "Hey Glyph" (14px #8B95A5) + chevron
  - Row 3 (64px): Brain icon (#B388FF) + "Proactive Mode". Right: toggle ON
  - Row 4 (64px): Layers icon (#34D399) + "AI Tier". Right: "Tier 3 — Cloud" (14px #8B95A5) + chevron

  GROUP 4: "SYSTEM"
  - Row 1 (64px): Storage icon (#8B95A5) + "Storage". Right: "12.4 GB / 32 GB" (14px #8B95A5) + chevron
  - Row 2 (64px): Info icon (#8B95A5) + "About Glyph OS". Right: "v0.1.0" (14px #8B95A5) + chevron
  - Row 3 (64px): Warning icon (#FF3D3D) + "Factory Reset" (text in #FF3D3D). Right: chevron in #FF3D3D/50%

- PAGE INDICATOR: Right edge, dot 4 active

- BOTTOM NAV BAR (1216-1280px): Settings gear icon active (cyan), others dimmed.

The settings should scroll naturally within the page. Keep it clean, utilitarian, and fast to scan. Every icon has a unique color to help quick visual scanning. Toggle switches are the primary interaction — big, easy to tap.`
  }
];

async function generateWithCLI() {
  console.log('Generating v3 designs via Stitch CLI...\n');

  for (const page of pages) {
    console.log(`Generating ${page.name}...`);

    // Write prompt to temp file to avoid shell escaping issues
    const promptFile = `/tmp/stitch-prompt-${page.name}.txt`;
    writeFileSync(promptFile, page.prompt);

    try {
      // Use the Stitch SDK via npx
      const script = `
const { StitchToolClient } = require('@google/stitch-sdk');

async function main() {
  const client = new StitchToolClient({ apiKey: '${API_KEY}' });

  // List tools to find the right one
  const tools = await client.listTools();
  const toolNames = tools.map(t => t.name);
  console.error('Available tools:', toolNames.join(', '));

  // Create project first if needed
  let projectId;
  try {
    const createResult = await client.callTool('create_project', { title: 'Glyph OS v3 - ${page.name}' });
    console.error('Project created:', JSON.stringify(createResult).substring(0, 200));
    // Try to extract project ID
    if (createResult && typeof createResult === 'object') {
      projectId = createResult.project_id || createResult.id || createResult.projectId;
      if (!projectId && createResult.content) {
        const text = typeof createResult.content === 'string' ? createResult.content : JSON.stringify(createResult.content);
        const match = text.match(/[a-f0-9-]{20,}/i);
        if (match) projectId = match[0];
      }
    }
  } catch(e) {
    console.error('Create project error:', e.message);
  }

  const prompt = require('fs').readFileSync('${promptFile}', 'utf8');

  // Generate the screen
  const genParams = {
    text: prompt,
    width: 720,
    height: 1280
  };
  if (projectId) genParams.project_id = projectId;

  // Try different tool names
  const genTools = ['generate_screen_from_text', 'generate_screen', 'generate_ui', 'generate'];
  let result;
  for (const toolName of genTools) {
    if (!toolNames.includes(toolName)) continue;
    try {
      console.error('Trying tool:', toolName);
      result = await client.callTool(toolName, genParams);
      console.error('Success with', toolName);
      break;
    } catch(e) {
      console.error(toolName, 'failed:', e.message);
    }
  }

  if (!result) {
    console.error('All generation tools failed');
    process.exit(1);
  }

  // Extract image and HTML
  const resultStr = JSON.stringify(result);
  console.error('Result keys:', Object.keys(result || {}));

  // Handle different result formats
  if (result.content && Array.isArray(result.content)) {
    for (const item of result.content) {
      if (item.type === 'image' && item.data) {
        require('fs').writeFileSync('${OUT_DIR}/${page.name}.png', Buffer.from(item.data, 'base64'));
        console.error('Saved PNG');
      } else if (item.type === 'text' && item.text) {
        if (item.text.includes('<') && item.text.includes('>')) {
          require('fs').writeFileSync('${OUT_DIR}/${page.name}.html', item.text);
          console.error('Saved HTML');
        }
      } else if (item.type === 'resource' && item.resource) {
        if (item.resource.mimeType?.startsWith('image')) {
          require('fs').writeFileSync('${OUT_DIR}/${page.name}.png', Buffer.from(item.resource.blob || item.resource.text, 'base64'));
          console.error('Saved PNG from resource');
        } else if (item.resource.mimeType?.includes('html')) {
          require('fs').writeFileSync('${OUT_DIR}/${page.name}.html', item.resource.text || Buffer.from(item.resource.blob, 'base64').toString());
          console.error('Saved HTML from resource');
        }
      }
    }
  } else if (result.image) {
    require('fs').writeFileSync('${OUT_DIR}/${page.name}.png', Buffer.from(result.image, 'base64'));
    console.error('Saved PNG direct');
  } else if (result.html) {
    require('fs').writeFileSync('${OUT_DIR}/${page.name}.html', result.html);
    console.error('Saved HTML direct');
  }

  // Output result summary
  console.log(JSON.stringify({ page: '${page.name}', success: true, resultType: typeof result }));
}

main().catch(e => { console.error('Fatal:', e.message); process.exit(1); });
`;

      const scriptFile = `/tmp/stitch-gen-${page.name}.cjs`;
      writeFileSync(scriptFile, script);

      const output = execSync(`node ${scriptFile}`, {
        timeout: 120000,
        encoding: 'utf8',
        stdio: ['pipe', 'pipe', 'pipe']
      });
      console.log(`  ${page.name} done:`, output.trim());

    } catch(e) {
      console.error(`  ${page.name} error:`, e.stderr?.substring(0, 500) || e.message);
    }
  }

  console.log('\nAll v3 designs generated!');
}

generateWithCLI();
