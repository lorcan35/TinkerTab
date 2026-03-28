import { StitchToolClient } from '@google/stitch-sdk';
import { writeFileSync } from 'fs';

const API_KEY = 'AQ.Ab8RN6KkZM69cwQWQ-JkjUueYCFtGSJzlD2FeC1sds_aJ7vP9w';
const OUT = '/home/rebelforce/projects/TinkerTab/stitch-designs/v3';

const DS = `DESIGN SYSTEM — "Glyph OS" 720x1280 portrait dark AI companion device.
Output must be exactly 720px wide × 1280px tall. Portrait orientation. 7-inch touchscreen.

COLORS: bg #0D0F12 (deep blue-gray, NOT pure black), surface #161920, surface-high #1E222B, text #E8ECF1, text-secondary #8B95A5, text-disabled #4A5568, accent Electric Cyan #00E5FF, accent-dim #00A3B3, success #34D399, warning #FFB300, error #FF3D3D, border-subtle #1E222B, border #2D3444.

ELEVATION via surface lightness (no shadows/blur). FONTS: Space Grotesk headlines, Inter body. Display 56px, H1 36px, H2 28px, Body 20px, Caption 16px, Overline 12px uppercase tracking-widest.
STYLE: Nothing OS monochrome discipline + Linear warm darks. Accent only where attention needed. 8px grid. 12px radius cards. 9999px pills. Min touch targets 48px.`;

const pages = {
  home: DS + `

HOME PAGE — Ambient idle screen. What users see 90% of the time. Glanceable from 3 meters.

TOP STATUS BAR (h56px): "21:42" bold left, tiny "GLYPH" cyan pill badge, wifi/signal/battery icons right #8B95A5.

MAIN CONTENT centered:
- Large "21:42" clock 56px bold #E8ECF1 centered
- "Thursday, March 27" 18px #8B95A5 below
- AI STATUS ORB: 120x120px Electric Cyan #00E5FF glowing circle/ring with radiating energy. "Ready" 12px label below in #00A3B3. Think Siri orb meets sci-fi reactor.
- Two cards side-by-side: Weather "23°C Partly Cloudy" + Next Event "Team Sync 2:00 PM". Each ~320x140px, bg #161920, 12px radius.
- AI Insight card full-width: cyan dot + "Rain expected at 4pm — take an umbrella" bg #161920.

RIGHT EDGE: 4 vertical page dots, dot 1 filled cyan.
BOTTOM NAV (h64px): home(filled cyan)/apps/dragon/settings icons, bg #0D0F12.`,

  apps: DS + `

APPS PAGE — Tool launcher grid. Pure function, efficient.

TOP: Status bar. "Apps" 36px semibold left + search pill icon right.
GRID: 4 columns × 4 rows. Cell 172x130px. Icon: 64x64 rounded-square with 15% colored tint bg + outline icon.
Row1: AI Chat(#00E5FF), Browser(#B388FF), Camera(#FF6B9D), Voice(#FFB300)
Row2: Files(#34D399), Automations(#00E5FF), Terminal(#8B95A5), Notes(#FFB300)
Row3: IoT Hub(#34D399), Music(#FF6B9D), Clock(#B388FF), Maps(#00E5FF)
Row4: VPN(#34D399), Wallet(#FFB300), Health(#FF6B9D), Add+(dashed #4A5568)

SEARCH BAR bottom: "Ask Glyph anything..." bg #161920, 12px radius.
Labels 14px #8B95A5. Monochrome outline icons 2px stroke.
Page dots: dot 2 cyan. Bottom nav: apps icon cyan.`,

  dragon: DS + `

DRAGON PAGE — Remote desktop stream, full-screen immersive.

BACKGROUND: Full 720x1280 filled with a dark Linux desktop (terminal + browser), slightly dimmed.

OVERLAYS:
- Top bar (h56px, #0D0F12 at 80%): "Dragon" left, green "Connected" pill, menu dots right.
- LIVE toggle (top-right, 72px top): Cyan pill "LIVE" with dot + switch, 120x40px.
- "12ms" latency pill top-left #34D399. "24 FPS" next to it #8B95A5.
- Bottom toolbar (h64px, #0D0F12 at 80%): Back, Keyboard, Mouse, Lightning, Stop(#FF3D3D). 48px targets.
- Page dots: dot 3 cyan. Bottom nav: dragon icon cyan.

Feel: Looking through window at another computer. Thin translucent overlays.`,

  settings: DS + `

SETTINGS PAGE — Grouped lists, utilitarian.

TOP: Status bar. "Settings" 36px semibold.

GROUPS (12px cyan uppercase label, #161920 container 12px radius):

"NETWORK": Wi-Fi(#00E5FF icon, "Home_5G" + chevron), Bluetooth(#B388FF, toggle ON), Dragon Link(#34D399, green dot "Connected")

"DISPLAY": Brightness(#FFB300, slider), Night Mode(#B388FF, toggle OFF), Auto-Dim(toggle ON), Screen Timeout("5 min" + chevron)

"AI ASSISTANT": Voice Model(#00E5FF, "Glyph Neural v2"), Wake Word(#FFB300, "Hey Glyph"), Proactive Mode(#B388FF, toggle ON), AI Tier(#34D399, "Tier 3 — Cloud")

"SYSTEM": Storage("12.4/32 GB"), About("v0.1.0"), Factory Reset(#FF3D3D text)

Each row 64px, 1px #1E222B divider. Icons 20px unique colors. Toggles 36x20.
Page dots: dot 4 cyan. Bottom nav: settings cyan.`
};

async function run() {
  const client = new StitchToolClient({ apiKey: API_KEY });

  for (const [name, prompt] of Object.entries(pages)) {
    console.log(`\n=== ${name.toUpperCase()} ===`);
    try {
      // Create project
      const proj = await client.callTool('create_project', { title: `GlyphOS-v3-${name}` });
      // Extract project ID from "projects/NNNN" format
      const projName = proj?.name || '';
      const projectId = projName.replace('projects/', '');
      console.log(`  project: ${projectId}`);

      if (!projectId) {
        console.log('  No project ID, skipping');
        continue;
      }

      // Generate screen
      const result = await client.callTool('generate_screen_from_text', {
        projectId: projectId,
        prompt: prompt,
        deviceType: 'MOBILE',
        modelId: 'GEMINI_3_1_PRO'
      });

      console.log(`  generated! Keys: ${Object.keys(result || {})}`);

      // Save outputs - check different formats
      let savedPng = false, savedHtml = false;

      // The result might have content array or direct properties
      const content = result?.content || result?.outputComponents || [];
      if (Array.isArray(content)) {
        for (const item of content) {
          // Image data
          if (item.type === 'image' && item.data) {
            writeFileSync(`${OUT}/${name}.png`, Buffer.from(item.data, 'base64'));
            savedPng = true;
          }
          // HTML/text
          if (item.type === 'text' && item.text) {
            if (item.text.includes('<!DOCTYPE') || item.text.includes('<html') || item.text.includes('<div')) {
              writeFileSync(`${OUT}/${name}.html`, item.text);
              savedHtml = true;
            } else {
              console.log(`  text: ${item.text.substring(0, 200)}`);
            }
          }
          // Resource
          if (item.type === 'resource' && item.resource) {
            const r = item.resource;
            if (r.mimeType?.startsWith('image')) {
              writeFileSync(`${OUT}/${name}.png`, Buffer.from(r.blob || r.text, 'base64'));
              savedPng = true;
            } else if (r.mimeType?.includes('html') || r.text?.includes('<')) {
              writeFileSync(`${OUT}/${name}.html`, r.text || Buffer.from(r.blob, 'base64').toString());
              savedHtml = true;
            }
          }
        }
      }

      // If we got a result but no image, try get_screen to fetch the rendered output
      if (!savedPng && result?.projectId) {
        console.log('  Trying get_screen for rendered output...');
        try {
          // List screens in the project
          const screens = await client.callTool('list_screens', { projectId: projectId });
          console.log(`  screens: ${JSON.stringify(screens).substring(0, 300)}`);

          // Try to get the first screen
          const screenList = screens?.screens || [];
          if (screenList.length > 0) {
            const screenName = screenList[0].name || screenList[0].screenId;
            const screenId = screenName?.replace(/.*screens\//, '') || screenList[0].screenId;
            const screen = await client.callTool('get_screen', {
              projectId: projectId,
              screenId: screenId
            });

            // Check for image in screen response
            const screenContent = screen?.content || screen?.outputComponents || [];
            for (const item of (Array.isArray(screenContent) ? screenContent : [])) {
              if (item.type === 'image' && item.data) {
                writeFileSync(`${OUT}/${name}.png`, Buffer.from(item.data, 'base64'));
                savedPng = true;
              }
              if (item.type === 'text' && item.text?.includes('<')) {
                writeFileSync(`${OUT}/${name}.html`, item.text);
                savedHtml = true;
              }
            }

            // Also check direct properties
            if (screen?.imageUri) console.log(`  imageUri: ${screen.imageUri}`);
            if (screen?.html) {
              writeFileSync(`${OUT}/${name}.html`, screen.html);
              savedHtml = true;
            }
          }
        } catch(e) { console.log(`  get_screen err: ${e.message.substring(0, 100)}`); }
      }

      console.log(`  saved: png=${savedPng} html=${savedHtml}`);

      // Log full result structure for debugging
      if (!savedPng && !savedHtml) {
        console.log(`  Full result: ${JSON.stringify(result).substring(0, 500)}`);
      }

    } catch(e) {
      console.error(`  ERROR: ${e.message}`);
    }
  }
  console.log('\n✓ All done!');
}

run();
