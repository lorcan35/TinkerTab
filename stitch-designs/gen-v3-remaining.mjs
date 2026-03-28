import { StitchToolClient } from '@google/stitch-sdk';
import { writeFileSync } from 'fs';
import { execSync } from 'child_process';

const API_KEY = 'AQ.Ab8RN6KkZM69cwQWQ-JkjUueYCFtGSJzlD2FeC1sds_aJ7vP9w';
const OUT = '/home/rebelforce/projects/TinkerTab/stitch-designs/v3';

const DS = `DESIGN SYSTEM — "Glyph OS" 720x1280 portrait dark AI companion device.
Output exactly 720px wide × 1280px tall. Portrait. 7-inch touchscreen.
COLORS: bg #0D0F12, surface #161920, surface-high #1E222B, text #E8ECF1, text-secondary #8B95A5, accent Electric Cyan #00E5FF, success #34D399, warning #FFB300, error #FF3D3D, borders #1E222B/#2D3444.
ELEVATION via surface lightness, no shadows. FONTS: Space Grotesk headlines, Inter body. Nothing OS monochrome discipline + Linear warm darks. Accent only where needed. 8px grid. 12px radius. Touch min 48px.`;

const remaining = [
  { name: 'apps', prompt: DS + `\n\nAPPS PAGE — 4x4 grid launcher. Status bar top (time, GLYPH badge, icons). "Apps" 36px header + search icon. Grid: 64x64 rounded icons with colored tint bg. Row1: AI Chat(cyan), Browser(purple #B388FF), Camera(pink #FF6B9D), Voice(amber #FFB300). Row2: Files(green #34D399), Automations(cyan), Terminal(gray), Notes(amber). Row3: IoT(green), Music(pink), Clock(purple), Maps(cyan). Row4: VPN(green), Wallet(amber), Health(pink), Add+(dashed). Search bar "Ask Glyph anything..." bottom. Page dots dot 2 cyan. Bottom nav apps cyan.` },
  { name: 'dragon', prompt: DS + `\n\nDRAGON PAGE — Full-screen remote desktop. Dark Linux desktop filling entire screen, dimmed. Overlays: Top bar semi-transparent: "Dragon" + green "Connected" pill + menu. LIVE toggle pill cyan top-right. "12ms" latency + "24 FPS" counters. Bottom toolbar semi-transparent: Back/Keyboard/Mouse/Actions/Disconnect. Page dots dot 3. Nav dragon cyan.` },
  { name: 'settings', prompt: DS + `\n\nSETTINGS PAGE — Grouped lists. "Settings" header. Groups cyan labels: NETWORK (WiFi, Bluetooth toggle, Dragon Link), DISPLAY (Brightness slider, Night Mode, Auto-Dim, Timeout), AI ASSISTANT (Voice Model, Wake Word, Proactive, AI Tier), SYSTEM (Storage, About, Factory Reset red). 64px rows, unique-color icons. Page dots dot 4. Nav settings cyan.` }
];

function downloadFile(url, path) {
  try {
    execSync(`curl -sL -o "${path}" "${url}"`, { timeout: 30000 });
    return true;
  } catch { return false; }
}

const sleep = ms => new Promise(r => setTimeout(r, ms));

async function generateOne(client, name, prompt, retries = 3) {
  for (let attempt = 1; attempt <= retries; attempt++) {
    try {
      console.log(`  Attempt ${attempt}/${retries}...`);

      const proj = await client.callTool('create_project', { title: `GlyphOS-v3-${name}-${attempt}` });
      const pid = (proj?.name || '').replace('projects/', '');
      console.log(`  Project: ${pid}`);

      const result = await client.callTool('generate_screen_from_text', {
        projectId: pid,
        prompt: prompt,
        deviceType: 'MOBILE',
        modelId: 'GEMINI_3_1_PRO'
      });

      const designComp = result?.outputComponents?.find(c => c.design);
      if (!designComp?.design?.screens?.length) {
        console.log('  No screens, retrying...');
        await sleep(10000);
        continue;
      }

      const screen = designComp.design.screens[0];
      console.log(`  Screen: ${screen.title} (${screen.width}x${screen.height}) ${screen.screenMetadata?.status}`);

      let ok = false;
      if (screen.screenshot?.downloadUrl) {
        if (downloadFile(screen.screenshot.downloadUrl, `${OUT}/${name}.png`)) {
          console.log('  PNG saved!');
          ok = true;
        }
      }
      if (screen.htmlCode?.downloadUrl) {
        if (downloadFile(screen.htmlCode.downloadUrl, `${OUT}/${name}.html`)) {
          console.log('  HTML saved!');
        }
      }
      if (screen.theme?.designMd) {
        writeFileSync(`${OUT}/${name}-design-system.md`, screen.theme.designMd);
      }
      if (ok) return true;

    } catch(e) {
      console.log(`  Error: ${e.message.substring(0, 100)}`);
      if (attempt < retries) {
        const wait = attempt * 15000;
        console.log(`  Waiting ${wait/1000}s before retry...`);
        await sleep(wait);
      }
    }
  }
  return false;
}

async function run() {
  const client = new StitchToolClient({ apiKey: API_KEY });

  for (const page of remaining) {
    console.log(`\n=== ${page.name.toUpperCase()} ===`);
    const ok = await generateOne(client, page.name, page.prompt);
    if (!ok) console.log(`  FAILED after all retries`);
    // Delay between pages
    await sleep(5000);
  }

  console.log('\n✓ Done!');
}

run();
