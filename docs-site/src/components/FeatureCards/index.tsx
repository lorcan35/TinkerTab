import React from 'react';
import Link from '@docusaurus/Link';
import styles from './styles.module.css';

type Feature = {
  title: string;
  body: string;
  icon: string;
  to: string;
};

const FEATURES: Feature[] = [
  {
    title: 'Voice-first',
    icon: '🎙',
    body:
      'Push the orb, talk, get answers. Five voice modes from fully-local NPU inference to cloud STT/TTS — pick your privacy + speed tradeoff.',
    to: '/docs/tasks/voice-modes',
  },
  {
    title: 'Vision & video',
    icon: '📷',
    body:
      'Snap a photo, ask a follow-up. Two-way video calls with peer broadcast. JPEG over WebSocket — no WebRTC sprawl.',
    to: '/docs/tasks/photos-vision',
  },
  {
    title: 'Skills you can write',
    icon: '🧩',
    body:
      'New features are Python files on the Dragon. Six widget types, typed contracts, no firmware reflash to ship a new skill.',
    to: '/docs/tasks/builtin-skills',
  },
  {
    title: 'Onboard LLM (K144)',
    icon: '🧠',
    body:
      'Stack the M5Stack LLM Module on top — qwen2.5-0.5B runs on the AX630C NPU, no Dragon needed for offline replies.',
    to: '/docs/hardware/k144',
  },
  {
    title: 'Open + auditable',
    icon: '🔓',
    body:
      'Standard tools — Ollama, esp-idf, Python. No vendor lock-in, no anonymous telemetry. The whole stack is yours.',
    to: '/docs/architecture/dragon-server',
  },
  {
    title: 'Hackable hardware',
    icon: '🔌',
    body:
      'Grove Port A I2C for sensors. Mate carrier for stackable modules. EXT5V_EN you control. We ship the pins, you ship the project.',
    to: '/docs/hardware/grove-port-a',
  },
];

export default function FeatureCards() {
  return (
    <section className={styles.section}>
      <div className={styles.inner}>
        <h2 className={styles.heading}>What it does</h2>
        <div className={styles.grid}>
          {FEATURES.map((f) => (
            <Link key={f.title} to={f.to} className={styles.card}>
              <div className={styles.icon} aria-hidden="true">
                {f.icon}
              </div>
              <h3 className={styles.cardTitle}>{f.title}</h3>
              <p className={styles.cardBody}>{f.body}</p>
              <span className={styles.cardArrow}>→</span>
            </Link>
          ))}
        </div>
      </div>
    </section>
  );
}
