import React from 'react';
import Link from '@docusaurus/Link';
import useDocusaurusContext from '@docusaurus/useDocusaurusContext';
import styles from './styles.module.css';

export default function HeroBanner() {
  const {siteConfig} = useDocusaurusContext();
  return (
    <header className={styles.hero}>
      <div className={styles.heroInner}>
        <div className={styles.heroDevice}>
          <img
            src="img/tab5-hero.svg"
            alt="M5Stack Tab5 device"
            className={styles.heroDeviceImg}
          />
        </div>
        <div className={styles.heroCopy}>
          <span className={styles.eyebrow}>TinkerOS · v0.7</span>
          <h1 className={styles.title}>{siteConfig.title}</h1>
          <p className={styles.tagline}>{siteConfig.tagline}</p>
          <div className={styles.ctaRow}>
            <Link
              className={`button button--primary button--lg ${styles.ctaPrimary}`}
              to="/docs/get-started/power-on">
              Get Started
            </Link>
            <Link
              className={`button button--secondary button--lg ${styles.ctaSecondary}`}
              href="https://github.com/lorcan35/TinkerTab">
              GitHub
            </Link>
            <Link
              className={`button button--link button--lg ${styles.ctaTertiary}`}
              to="/docs/intro">
              What is TinkerTab? →
            </Link>
          </div>
        </div>
      </div>
    </header>
  );
}
