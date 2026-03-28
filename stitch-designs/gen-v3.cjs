#!/usr/bin/env node
/**
 * Glyph OS v3 — Generate all 4 pages via Stitch SDK
 */
const { writeFileSync, readFileSync } = require('fs');
const { execSync } = require('child_process');

const API_KEY = 'AQ.Ab8RN6KkZM69cwQWQ-JkjUueYCFtGSJzlD2FeC1sds_aJ7vP9w';
const OUT = '/home/rebelforce/projects/TinkerTab/stitch-designs/v3';

const DS = `
DESIGN SYSTEM — "Glyph OS" for a 720x1280 portrait dark-theme AI companion device.
CRITICAL: Output must be exactly 720px wide × 1280px tall. Portrait. 7-inch IPS touchscreen.

COLOR PALETTE (research-backed):
- Background: #0D0F12 (deep blue-gray, NOT pure black)
- Surface/cards: #161920
- Surface high: #1E222B
- Text primary: #E8ECF1, secondary: #8B95A5, disabled: #4A5568
- Accent: #00E5FF (Electric Cyan)
- Accent dim: #00A3B3
- Success: #34D399, Warning: #FFB300, Error: #FF3D3D
- Borders: #1E222B subtle, #2D3444 default

ELEVATION via surface color lightness, NOT shadows. NO blur effects.
TYPOGRAPHY: Space Grotesk headlines, Inter body. Display 56px, H1 36px, H2 28px, Body 20px, Caption 16px, Overline 12px uppercase tracking-widest.
DESIGN: Nothing OS monochrome discipline + Rabbit R1 card simplicity + Linear warm dark grays. Typography IS the design. Accent ONLY where attention needed.
Touch targets: min 48px. 8px spacing grid. Border radius 12px cards, 9999px pills.
STATUS BAR top: time left, GLYPH badge center, wifi/signal/battery right.
PAGE DOTS: 4 vertical dots right edge. BOTTOM NAV: home/apps/dragon/settings icons.
`;

const pages = {
  home: DS + `
PAGE: HOME — Ambient idle screen. Users see this 90% of the time. Glanceable from 3 meters.

Layout top to bottom:
1. Status bar (0-56px): "21:42" bold left, tiny "GLYPH" cyan-bordered pill, wifi/signal/battery right
2. Large clock centered (200px from top): "21:42" in 56px bold #E8ECF1
3. Date: "Thursday, March 27" 18px #8B95A5
4. AI Status Orb (centered, 120x120px): Electric Cyan ring with inner glow, "Ready" label 12px below
5. Two contextual cards side-by-side (~640px from top): Weather "23°C Partly Cloudy" and Next Event "Team Sync 2:00 PM", each ~320x140px, bg #161920, 12px radius, 1px #1E222B border
6. AI Insight card full-width (~920px): "Rain expected at 4pm" with cyan dot indicator, bg #161920
7. Page dots right edge (dot 1 cyan), bottom nav (home icon cyan)

The orb is the centerpiece — a glowing cyan circle with radiating energy. Think Siri orb meets Nothing Glyph lights. It should feel ALIVE.`,

  apps: DS + `
PAGE: APPS — Tool launcher grid. Pure function, efficient.

Layout:
1. Status bar top
2. "Apps" 36px semibold left + search icon pill right (72-120px)
3. 4×4 app grid (144-960px): Each cell 172×130px. Icon: 64x64 rounded square with colored tint bg + monochrome outline icon. Label 14px below.
   Row 1: AI Chat (#00E5FF), Browser (#B388FF), Camera (#FF6B9D), Voice (#FFB300)
   Row 2: Files (#34D399), Automations (#00E5FF), Terminal (#8B95A5), Notes (#FFB300)
   Row 3: IoT Hub (#34D399), Music (#FF6B9D), Clock (#B388FF), Maps (#00E5FF)
   Row 4: VPN (#34D399), Wallet (#FFB300), Health (#FF6B9D), Add + (dashed #4A5568)
4. Search bar (988-1048px): "Ask Glyph anything..." placeholder, bg #161920, 12px radius
5. Page dots (dot 2 cyan), bottom nav (apps icon cyan)

Icons: monochrome outline style (2px stroke), colored tint background at 15% opacity. Nothing OS discipline — the color tint is subtle.`,

  dragon: DS + `
PAGE: DRAGON — Remote desktop stream from connected mini-PC.

The stream fills the ENTIRE 720x1280 screen edge-to-edge. Show a realistic dark Linux desktop (terminal + browser windows) as the stream content, slightly dimmed.

Overlays on the stream:
1. Top bar (0-56px, #0D0F12 at 80% opacity): "Dragon" left, green "Connected" pill center (#34D399), menu dots right
2. LIVE toggle (top-right, 72px from top): Prominent pill "LIVE" with pulsing cyan dot + toggle switch. ~120x40px, cyan when on
3. Latency pill (top-left, 72px): "12ms" in monospace, #34D399, semi-transparent bg
4. FPS counter next to latency: "24 FPS" #8B95A5
5. Bottom toolbar (1216-1280px, #0D0F12 at 80%): 5 icons — Back, Keyboard, Mouse mode, Quick actions (lightning), Disconnect (red). Each 48px target, 24px icon, 10px label below
6. Page dots (dot 3 cyan), bottom nav subtle (dragon icon cyan)

This should feel like looking THROUGH a window at another computer. Overlays are thin, translucent, minimal.`,

  settings: DS + `
PAGE: SETTINGS — System configuration. Clean grouped lists.

Layout:
1. Status bar top
2. "Settings" 36px semibold (72-120px)
3. Scrollable groups (136-1200px):

GROUP "NETWORK" (label 12px uppercase cyan):
Container bg #161920, 12px radius:
- Wi-Fi: wifi icon (#00E5FF) + label. Right: "Home_5G" + chevron. 64px row
- Bluetooth: bt icon (#B388FF) + label. Right: toggle ON (cyan). 64px
- Dragon Link: monitor icon (#34D399) + label. Right: green dot + "Connected" + chevron. 64px

GROUP "DISPLAY":
- Brightness: sun icon (#FFB300). Right: slider (track #2D3444, filled #00E5FF, white thumb)
- Night Mode: moon icon (#B388FF). Right: toggle OFF
- Auto-Dim: timer icon. Right: toggle ON
- Screen Timeout: clock icon. Right: "5 min" + chevron

GROUP "AI ASSISTANT":
- Voice Model: waveform (#00E5FF). Right: "Glyph Neural v2" + chevron
- Wake Word: mic (#FFB300). Right: "Hey Glyph" + chevron
- Proactive Mode: brain (#B388FF). Right: toggle ON
- AI Tier: layers (#34D399). Right: "Tier 3 — Cloud" + chevron

GROUP "SYSTEM":
- Storage: disk icon. Right: "12.4 / 32 GB" + chevron
- About: info icon. Right: "v0.1.0" + chevron
- Factory Reset: warning icon (#FF3D3D) + RED text. Right: chevron

4. Page dots (dot 4 cyan), bottom nav (settings icon cyan)

Each row 64px tall with 1px #1E222B divider. Icons 20px with unique colors for visual scanning. Toggle switches: 36x20px track.`
};

async function run() {
  const { StitchToolClient } = require('@google/stitch-sdk');
  const client = new StitchToolClient({ apiKey: API_KEY });

  const tools = await client.listTools();
  const names = tools.map(t => t.name);
  console.log('Tools:', names.join(', '));

  for (const [name, prompt] of Object.entries(pages)) {
    console.log(`\nGenerating ${name}...`);
    try {
      // Create project
      let projectId;
      try {
        const proj = await client.callTool('create_project', { title: `GlyphOS-v3-${name}` });
        if (proj?.content) {
          for (const c of proj.content) {
            if (c.text) {
              const m = c.text.match(/"project_id"\s*:\s*"([^"]+)"/);
              if (m) projectId = m[1];
            }
          }
        }
        console.log(`  project: ${projectId || 'unknown'}`);
      } catch(e) { console.log('  project err:', e.message); }

      // Generate
      const params = { text: prompt, width: 720, height: 1280 };
      if (projectId) params.project_id = projectId;

      let result;
      for (const tool of ['generate_screen_from_text', 'generate_screen', 'generate_ui']) {
        if (!names.includes(tool)) continue;
        try {
          result = await client.callTool(tool, params);
          console.log(`  generated via ${tool}`);
          break;
        } catch(e) { console.log(`  ${tool}: ${e.message}`); }
      }

      if (!result) { console.log('  FAILED'); continue; }

      // Save outputs
      if (result.content && Array.isArray(result.content)) {
        for (const item of result.content) {
          if (item.type === 'image' && item.data) {
            writeFileSync(`${OUT}/${name}.png`, Buffer.from(item.data, 'base64'));
            console.log('  saved PNG');
          } else if (item.type === 'text' && item.text?.includes('<')) {
            writeFileSync(`${OUT}/${name}.html`, item.text);
            console.log('  saved HTML');
          } else if (item.type === 'resource' && item.resource) {
            const r = item.resource;
            if (r.mimeType?.startsWith('image')) {
              writeFileSync(`${OUT}/${name}.png`, Buffer.from(r.blob || r.text, 'base64'));
              console.log('  saved PNG (resource)');
            } else if (r.mimeType?.includes('html')) {
              writeFileSync(`${OUT}/${name}.html`, r.text || Buffer.from(r.blob, 'base64').toString());
              console.log('  saved HTML (resource)');
            }
          }
        }
      }
    } catch(e) {
      console.error(`  ${name} fatal:`, e.message);
    }
  }
  console.log('\nDone!');
}

run().catch(e => console.error('Fatal:', e));
