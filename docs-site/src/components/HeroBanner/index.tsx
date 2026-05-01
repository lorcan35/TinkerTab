import React from 'react';
import Link from '@docusaurus/Link';
import styles from './styles.module.css';

export default function HeroBanner() {
  const today = new Date().toLocaleDateString('en-US', {
    year: 'numeric',
    month: 'long',
    day: 'numeric',
  });

  return (
    <header className={styles.hero}>
      <div className={styles.masthead}>
        <span className={styles.mastheadVol}>VOL. 01 · ISSUE 04</span>
        <span className={styles.mastheadCenter}>
          T H E &nbsp; T I N K E R T A B &nbsp; J O U R N A L
        </span>
        <span className={styles.mastheadDate}>{today.toUpperCase()}</span>
      </div>

      <div className={styles.rule} aria-hidden="true" />

      <div className={styles.spread}>
        <div className={styles.copy}>
          <span className={styles.eyebrow}>NO. 001 · INTRODUCTION</span>

          <h1 className={styles.title}>
            <span className={styles.titleLine1}>A voice-first</span>
            <span className={styles.titleLine2}>
              home <em>assistant</em>
            </span>
            <span className={styles.titleLine3}>
              for hardware <span className={styles.titleAccent}>you own.</span>
            </span>
          </h1>

          <p className={styles.lede}>
            <strong>An exploration of voice, vision, and skills</strong> running
            on the M5Stack Tab5 + Dragon Q6A. Five voice modes — fully-local NPU
            inference to OpenRouter cloud — chosen per-turn by a capability-aware
            router. No cloud is required; none are forbidden.
          </p>

          <div className={styles.byline}>
            <div className={styles.bylineGroup}>
              <span className={styles.bylineLabel}>Hardware</span>
              <span className={styles.bylineValue}>M5Stack Tab5 · ESP32-P4</span>
            </div>
            <div className={styles.bylineGroup}>
              <span className={styles.bylineLabel}>Brain</span>
              <span className={styles.bylineValue}>Dragon Q6A · 12 GB · NPU</span>
            </div>
            <div className={styles.bylineGroup}>
              <span className={styles.bylineLabel}>Source</span>
              <span className={styles.bylineValue}>github.com/lorcan35</span>
            </div>
          </div>

          <div className={styles.ctaRow}>
            <Link
              className={`button button--primary button--lg ${styles.ctaPrimary}`}
              to="/docs/get-started/power-on">
              Begin reading →
            </Link>
            <Link
              className={`button button--secondary button--lg ${styles.ctaSecondary}`}
              to="/docs/architecture/tab5-firmware">
              Skip to architecture
            </Link>
          </div>
        </div>

        <figure className={styles.figure}>
          <div className={styles.figurePlate}>
            <img
              src="/img/tab5-illustration.svg"
              alt="Schematic illustration of the M5Stack Tab5"
              className={styles.figureImg}
            />
          </div>
          <figcaption className={styles.figureCaption}>
            <span className={styles.figureLabel}>FIG. I</span>
            <span className={styles.figureBody}>
              The Tab5, exploded. <em>Display, microphone array, voice orb,
              expansion bus.</em>
            </span>
          </figcaption>
        </figure>
      </div>

      <div className={styles.rule} aria-hidden="true" />

      <div className={styles.tableOfContents}>
        <span className={styles.tocLabel}>IN THIS VOLUME</span>
        <ol className={styles.tocList}>
          <li>
            <span className={styles.tocNum}>I.</span>
            <Link to="/docs/get-started/power-on" className={styles.tocLink}>
              Get Started
            </Link>
            <span className={styles.tocPages}>p. 03</span>
          </li>
          <li>
            <span className={styles.tocNum}>II.</span>
            <Link to="/docs/hardware/specs" className={styles.tocLink}>
              Hardware
            </Link>
            <span className={styles.tocPages}>p. 11</span>
          </li>
          <li>
            <span className={styles.tocNum}>III.</span>
            <Link to="/docs/tasks/voice-modes" className={styles.tocLink}>
              Tasks &amp; Skills
            </Link>
            <span className={styles.tocPages}>p. 23</span>
          </li>
          <li>
            <span className={styles.tocNum}>IV.</span>
            <Link to="/docs/architecture/tab5-firmware" className={styles.tocLink}>
              Architecture
            </Link>
            <span className={styles.tocPages}>p. 31</span>
          </li>
          <li>
            <span className={styles.tocNum}>V.</span>
            <Link to="/docs/dragon-reference/ws-messages" className={styles.tocLink}>
              Reference
            </Link>
            <span className={styles.tocPages}>p. 47</span>
          </li>
          <li>
            <span className={styles.tocNum}>VI.</span>
            <Link to="/docs/glossary" className={styles.tocLink}>
              Glossary &amp; Index
            </Link>
            <span className={styles.tocPages}>p. 63</span>
          </li>
        </ol>
      </div>
    </header>
  );
}
