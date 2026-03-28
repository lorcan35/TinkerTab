import { StitchToolClient } from '@google/stitch-sdk';
import { writeFileSync } from 'fs';
import { execSync } from 'child_process';

const API_KEY = 'AQ.Ab8RN6KkZM69cwQWQ-JkjUueYCFtGSJzlD2FeC1sds_aJ7vP9w';
const OUT = '/home/rebelforce/projects/TinkerTab/stitch-designs/v3';

const DS = `DESIGN SYSTEM — "Glyph OS" 720x1280 portrait dark AI companion device.
Output exactly 720px wide × 1280px tall. Portrait. 7-inch touchscreen.
COLORS: bg #0D0F12, surface #161920, surface-high #1E222B, text #E8ECF1, text-secondary #8B95A5, accent Electric Cyan #00E5FF, success #34D399, warning #FFB300, error #FF3D3D, borders #1E222B/#2D3444.
ELEVATION via surface lightness, no shadows. FONTS: Space Grotesk headlines, Inter body. Nothing OS monochrome discipline + Linear warm darks. Accent only where needed. 8px grid. 12px radius. Touch min 48px.`;

const prompts = {
  home: DS + `\n\nHOME PAGE — Ambient idle. Status bar top (time "21:42" left, "GLYPH" cyan badge, wifi/battery right). Large "21:42" 56px bold clock centered. "Thursday, March 27" date below. AI Status Orb: 120px Electric Cyan #00E5FF glowing circle with radiating energy, "Ready" label. Two side-by-side cards: Weather "23°C" + Next Event "Team Sync 2PM", bg #161920. AI Insight card full-width: "Rain expected at 4pm". 4 vertical page dots right edge (dot 1 cyan). Bottom nav: home(cyan)/apps/dragon/settings.`,

  apps: DS + `\n\nAPPS PAGE — 4x4 grid launcher. Status bar. "Apps" 36px header + search icon. Grid: 64x64 rounded icons with colored tint bg. Row1: AI Chat(cyan), Browser(purple #B388FF), Camera(pink #FF6B9D), Voice(amber #FFB300). Row2: Files(green #34D399), Automations(cyan), Terminal(gray), Notes(amber). Row3: IoT(green), Music(pink), Clock(purple), Maps(cyan). Row4: VPN(green), Wallet(amber), Health(pink), Add+(dashed gray). Search bar "Ask Glyph anything..." at bottom. Page dots dot 2. Nav: apps cyan.`,

  dragon: DS + `\n\nDRAGON PAGE — Full-screen remote desktop stream. Show a dark Linux desktop filling the entire screen, slightly dimmed. Overlays: Top bar (semi-transparent #0D0F12): "Dragon" + green "Connected" pill + menu. LIVE toggle pill (cyan, top-right). "12ms" latency + "24 FPS" counters. Bottom toolbar (semi-transparent): Back/Keyboard/Mouse/Actions/Disconnect icons. Page dots dot 3. Nav: dragon cyan.`,

  settings: DS + `\n\nSETTINGS PAGE — Grouped lists. "Settings" header. Groups with cyan uppercase labels: NETWORK (WiFi "Home_5G", Bluetooth toggle, Dragon Link "Connected"), DISPLAY (Brightness slider, Night Mode toggle, Auto-Dim, Screen Timeout "5min"), AI ASSISTANT (Voice Model, Wake Word "Hey Glyph", Proactive toggle, AI Tier "Tier 3"), SYSTEM (Storage "12.4/32GB", About "v0.1.0", Factory Reset red). 64px rows, icons with unique colors. Page dots dot 4. Nav: settings cyan.`
};

function downloadFile(url, path) {
  try {
    execSync(`curl -sL -o "${path}" "${url}"`, { timeout: 30000 });
    return true;
  } catch { return false; }
}

async function run() {
  const client = new StitchToolClient({ apiKey: API_KEY });

  for (const [name, prompt] of Object.entries(prompts)) {
    console.log(`\n=== ${name.toUpperCase()} ===`);

    const proj = await client.callTool('create_project', { title: `GlyphOS-v3-${name}` });
    const pid = (proj?.name || '').replace('projects/', '');
    console.log(`  Project: ${pid}`);

    const result = await client.callTool('generate_screen_from_text', {
      projectId: pid,
      prompt: prompt,
      deviceType: 'MOBILE',
      modelId: 'GEMINI_3_1_PRO'
    });

    // Find the design component with screens
    const designComp = result?.outputComponents?.find(c => c.design);
    if (!designComp?.design?.screens?.length) {
      console.log('  No screens in result!');
      continue;
    }

    const screen = designComp.design.screens[0];
    console.log(`  Screen: ${screen.title} (${screen.width}x${screen.height})`);
    console.log(`  Status: ${screen.screenMetadata?.status}`);

    // Download screenshot
    if (screen.screenshot?.downloadUrl) {
      console.log('  Downloading screenshot...');
      if (downloadFile(screen.screenshot.downloadUrl, `${OUT}/${name}.png`)) {
        console.log('  PNG saved!');
      } else {
        console.log('  PNG download failed');
      }
    }

    // Download HTML
    if (screen.htmlCode?.downloadUrl) {
      console.log('  Downloading HTML...');
      if (downloadFile(screen.htmlCode.downloadUrl, `${OUT}/${name}.html`)) {
        console.log('  HTML saved!');
      } else {
        console.log('  HTML download failed');
      }
    }

    // Also save the design system markdown
    if (screen.theme?.designMd) {
      writeFileSync(`${OUT}/${name}-design-system.md`, screen.theme.designMd);
      console.log('  Design system saved!');
    }
  }

  console.log('\n✓ All v3 designs generated!');
}

run();
