import React from 'react';
import Link from '@docusaurus/Link';
import styles from './styles.module.css';

type Feature = {
  num: string;
  category: string;
  title: string;
  body: React.ReactNode;
  to: string;
  cta: string;
};

const FEATURES: Feature[] = [
  {
    num: '01',
    category: 'voice',
    title: 'Push-to-talk, by design',
    body: (
      <>
        No wake word, no always-listening; tap the orb and hold while you
        speak. Five voice modes route the same gesture through fully-local
        NPU inference, OpenRouter cloud, or a stacked K144 module —{' '}
        <em>privacy and speed are settings, not assumptions.</em>
      </>
    ),
    to: '/docs/tasks/voice-modes',
    cta: 'Voice pipeline',
  },
  {
    num: '02',
    category: 'vision',
    title: 'Photographs that thread',
    body: (
      <>
        Snap a frame from the camera, ask a question, then ask another.
        The router picks a vision-capable model per turn; MessageStore
        rehydrates the image into context so <em>&ldquo;what color was
        the chair?&rdquo;</em> resolves against the photo from three
        turns back.
      </>
    ),
    to: '/docs/tasks/photos-vision',
    cta: 'Cross-modal flow',
  },
  {
    num: '03',
    category: 'skills',
    title: 'Python files, not firmware reflashes',
    body: (
      <>
        New features are 50-line Python skills on the Dragon. Six widget
        types — live, card, list, chart, media, prompt — let a skill
        emit typed state that the Tab5 renders opinionatedly. Restart
        the daemon; no toolchain involved.
      </>
    ),
    to: '/docs/dragon-reference/skill-sdk',
    cta: 'Skill SDK',
  },
  {
    num: '04',
    category: 'offline',
    title: 'A second brain on the stack',
    body: (
      <>
        Stack the M5Stack LLM Module on the Mate carrier. <em>qwen2.5-0.5B</em>
        runs on the AX630C NPU at ~8 tok/s. When the Dragon goes down, the
        next text turn fails over to the K144 transparently — toast on screen,
        conversation continues.
      </>
    ),
    to: '/docs/hardware/k144',
    cta: 'K144 module',
  },
  {
    num: '05',
    category: 'open',
    title: 'Standard tools, no lock-in',
    body: (
      <>
        Ollama for local inference. Piper for TTS. Moonshine for STT.
        Standard Python, ESP-IDF, and SQLite under the hood — every layer
        is the obvious open-source choice. No proprietary cloud, no
        anonymous telemetry, no &ldquo;trust us.&rdquo;
      </>
    ),
    to: '/docs/architecture/dragon-server',
    cta: 'Dragon server',
  },
  {
    num: '06',
    category: 'hackable',
    title: 'A Grove port, a stack connector, your project',
    body: (
      <>
        Plug a Grove BME280 into Port A; the sensor reading lands in the
        LLM&apos;s system prompt without a tool call. Stack a custom
        Module13.2 board on top; bring up its UART in <em>service_dragon</em>.
        We ship the pins; you ship the project.
      </>
    ),
    to: '/docs/hardware/grove-port-a',
    cta: 'Grove Port A',
  },
];

export default function FeatureCards() {
  return (
    <section className={styles.section}>
      <div className={styles.inner}>
        <header className={styles.sectionHeader}>
          <span className={styles.sectionNum}>NO. 002</span>
          <h2 className={styles.sectionTitle}>
            Six dispatches from the workbench.
          </h2>
          <p className={styles.sectionLede}>
            What the system actually does — and where in the journal to read
            the long-form for each.
          </p>
        </header>

        <div className={styles.grid}>
          {FEATURES.map((f, i) => (
            <article
              key={f.num}
              className={styles.article}
              style={{['--ttDelay' as string]: `${0.3 + i * 0.08}s`}}>
              <div className={styles.articleMeta}>
                <span className={styles.articleNum}>N° {f.num}</span>
                <span className={styles.articleCategory}>· {f.category}</span>
              </div>
              <h3 className={styles.articleTitle}>{f.title}</h3>
              <p className={styles.articleBody}>{f.body}</p>
              <Link to={f.to} className={styles.articleCta}>
                {f.cta}
                <span className={styles.articleArrow} aria-hidden="true">
                  ↗
                </span>
              </Link>
            </article>
          ))}
        </div>

        <footer className={styles.sectionFooter}>
          <span className={styles.colophon}>
            Set in <em>Fraunces</em> &amp; <em>Newsreader</em>, with{' '}
            <em>JetBrains Mono</em> for marginalia.
          </span>
          <Link to="/docs/intro" className={styles.colophonLink}>
            Continue to the introduction →
          </Link>
        </footer>
      </div>
    </section>
  );
}
