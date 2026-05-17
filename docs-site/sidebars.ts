import type {SidebarsConfig} from '@docusaurus/plugin-content-docs';

const sidebars: SidebarsConfig = {
  mainSidebar: [
    'intro',
    {
      type: 'category',
      label: 'Overview',
      collapsed: false,
      items: [
        'overview/what-is-tinkertab',
        'overview/whats-in-the-box',
      ],
    },
    {
      type: 'category',
      label: '🟢 Get Started',
      collapsed: false,
      items: [
        'get-started/power-on',
        'get-started/connect-to-dragon',
        'get-started/first-voice-command',
        'get-started/take-a-photo',
        'get-started/set-a-reminder',
      ],
    },
    {
      type: 'category',
      label: '🟢 Tab5 Hardware',
      collapsed: true,
      items: [
        'hardware/specs',
        'hardware/touch-display',
        'hardware/audio',
        'hardware/camera',
        'hardware/k144',
        'hardware/grove-port-a',
        'hardware/buttons-resets',
        'hardware/power',
      ],
    },
    {
      type: 'category',
      label: '🟢 Tasks & Skills',
      collapsed: true,
      items: [
        'tasks/voice-modes',
        'tasks/builtin-skills',
        'tasks/memory-facts',
        'tasks/photos-vision',
        'tasks/reminders-timers',
        'tasks/video-calling',
      ],
    },
    {
      type: 'category',
      label: '🟢 Dragon Server',
      collapsed: true,
      items: [
        'dragon/install',
        'dragon/configuration',
        'dragon/llm-backends',
        'dragon/voice-modes-config',
        'dragon/logs-monitoring',
      ],
    },
    {
      type: 'category',
      label: '⚙️ Architecture',
      collapsed: true,
      items: [
        'architecture/tab5-firmware',
        'architecture/dragon-server',
        'architecture/voice-pipeline',
        'architecture/cross-modal',
        'architecture/video-calling-stack',
        'architecture/skills',
      ],
    },
    {
      type: 'category',
      label: '⚙️ Firmware Reference',
      collapsed: true,
      items: [
        'firmware-reference/build-flash',
        'firmware-reference/debug-server',
        'firmware-reference/nvs-keys',
        'firmware-reference/ws-protocol',
        'firmware-reference/lvgl-conventions',
        'firmware-reference/stability-guide',
        'firmware-reference/wave-log',
      ],
    },
    {
      type: 'category',
      label: '⚙️ Dragon Reference',
      collapsed: true,
      items: [
        'dragon-reference/module-map',
        'dragon-reference/ws-messages',
        'dragon-reference/rest-endpoints',
        'dragon-reference/skill-sdk',
        'dragon-reference/messagestore',
        'dragon-reference/observability',
      ],
    },
    'glossary',
    'troubleshooting',
    'contributing',
    'changelog',
  ],
};

export default sidebars;
