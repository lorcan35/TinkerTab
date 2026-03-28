import { StitchToolClient } from '@google/stitch-sdk';
import { writeFileSync } from 'fs';

const API_KEY = 'AQ.Ab8RN6KkZM69cwQWQ-JkjUueYCFtGSJzlD2FeC1sds_aJ7vP9w';
const OUT = '/home/rebelforce/projects/TinkerTab/stitch-designs/v3';

const projectIds = {
  home: '6999475150761745474',
  apps: '793150790330096540',
  dragon: '16475806898160222331',
  settings: '10511284532246208319'
};

async function run() {
  const client = new StitchToolClient({ apiKey: API_KEY });

  // First, let's understand what outputComponents actually contain
  // by listing all projects and their screens
  for (const [name, pid] of Object.entries(projectIds)) {
    console.log(`\n=== ${name.toUpperCase()} (project: ${pid}) ===`);
    try {
      // Get project info
      const proj = await client.callTool('get_project', { projectId: pid });
      console.log('  Project:', JSON.stringify(proj).substring(0, 300));

      // List screens
      const screens = await client.callTool('list_screens', { projectId: pid });
      console.log('  Screens:', JSON.stringify(screens).substring(0, 500));

      // Try to get each screen
      const screenList = screens?.screens || [];
      if (Array.isArray(screenList)) {
        for (const s of screenList) {
          const sid = (s.name || '').replace(/.*screens\//, '') || s.screenId;
          console.log(`  Fetching screen ${sid}...`);
          try {
            const screen = await client.callTool('get_screen', {
              projectId: pid,
              screenId: sid
            });
            console.log('  Screen data:', JSON.stringify(screen).substring(0, 500));

            // Look for image/HTML in any format
            if (screen?.content) {
              for (const item of screen.content) {
                if (item.type === 'image') {
                  writeFileSync(`${OUT}/${name}.png`, Buffer.from(item.data, 'base64'));
                  console.log('  SAVED PNG!');
                }
                if (item.type === 'text' && item.text?.includes('<')) {
                  writeFileSync(`${OUT}/${name}.html`, item.text);
                  console.log('  SAVED HTML!');
                }
              }
            }
            // Check for imageUri or screenshotUri
            if (screen?.imageUri || screen?.screenshotUri || screen?.thumbnailUri) {
              console.log('  URIs:', screen.imageUri, screen.screenshotUri, screen.thumbnailUri);
            }
          } catch(e) {
            console.log(`  Screen fetch error: ${e.message.substring(0, 100)}`);
          }
        }
      }
    } catch(e) {
      console.log(`  Error: ${e.message.substring(0, 200)}`);
    }
  }
}

run();
