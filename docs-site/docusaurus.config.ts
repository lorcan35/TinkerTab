import {themes as prismThemes} from 'prism-react-renderer';
import type {Config} from '@docusaurus/types';
import type * as Preset from '@docusaurus/preset-classic';

const config: Config = {
  title: 'TinkerTab',
  tagline: 'A voice-first home assistant for the M5Stack Tab5, paired with the Dragon LLM server.',
  favicon: 'img/favicon.ico',

  future: {
    v4: true,
  },

  url: 'http://tinker.local',
  baseUrl: '/',

  organizationName: 'lorcan35',
  projectName: 'TinkerTab',

  onBrokenLinks: 'warn',

  i18n: {
    defaultLocale: 'en',
    locales: ['en'],
  },

  markdown: {
    mermaid: true,
    hooks: {
      onBrokenMarkdownLinks: 'warn',
    },
  },
  themes: ['@docusaurus/theme-mermaid'],

  presets: [
    [
      'classic',
      {
        docs: {
          sidebarPath: './sidebars.ts',
          editUrl: 'https://github.com/lorcan35/TinkerTab/edit/main/docs-site/',
          showLastUpdateTime: true,
        },
        blog: false,
        theme: {
          customCss: './src/css/custom.css',
        },
      } satisfies Preset.Options,
    ],
  ],

  themeConfig: {
    // OG card lands in PR 4 polish; LAN-only site doesn't need it day-one.
    colorMode: {
      defaultMode: 'dark',
      respectPrefersColorScheme: true,
    },
    navbar: {
      title: 'TinkerTab',
      logo: {
        alt: 'TinkerTab logo',
        src: 'img/logo.svg',
      },
      items: [
        {
          type: 'docSidebar',
          sidebarId: 'mainSidebar',
          position: 'left',
          label: 'Docs',
        },
        {
          to: '/docs/get-started/power-on',
          label: 'Get Started',
          position: 'left',
        },
        {
          href: 'https://github.com/lorcan35/TinkerTab',
          label: 'GitHub',
          position: 'right',
        },
      ],
    },
    footer: {
      style: 'dark',
      links: [
        {
          title: 'Get Started',
          items: [
            {label: 'Power on the Tab5', to: '/docs/get-started/power-on'},
            {label: 'Connect to Dragon', to: '/docs/get-started/connect-to-dragon'},
            {label: "What's in the box", to: '/docs/overview/whats-in-the-box'},
          ],
        },
        {
          title: 'Reference',
          items: [
            {label: 'Architecture', to: '/docs/architecture/tab5-firmware'},
            {label: 'WS protocol', to: '/docs/dragon-reference/ws-messages'},
            {label: 'Glossary', to: '/docs/glossary'},
          ],
        },
        {
          title: 'Code',
          items: [
            {label: 'TinkerTab (firmware)', href: 'https://github.com/lorcan35/TinkerTab'},
            {label: 'TinkerBox (Dragon server)', href: 'https://github.com/lorcan35/TinkerBox'},
            {label: 'OpenClaw (upstream)', href: 'https://docs.openclaw.ai'},
          ],
        },
      ],
      copyright: `© ${new Date().getFullYear()} TinkerTab — Built with Docusaurus on a DGX in the closet.`,
    },
    prism: {
      theme: prismThemes.github,
      darkTheme: prismThemes.dracula,
      additionalLanguages: ['c', 'cpp', 'bash', 'json', 'yaml', 'ini', 'diff'],
    },
  } satisfies Preset.ThemeConfig,
};

export default config;
