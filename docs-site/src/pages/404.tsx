import React from 'react';
import Link from '@docusaurus/Link';
import Layout from '@theme/Layout';
import styles from './404.module.css';

export default function NotFound() {
  return (
    <Layout title="Not found" description="404 — page not found on TinkerTab docs">
      <main className={styles.main}>
        <div className={styles.inner}>
          <div className={styles.code}>404</div>
          <h1 className={styles.title}>This page doesn&apos;t exist (yet).</h1>
          <p className={styles.body}>
            Three reasons this might happen on a docs site that&apos;s still settling in:
          </p>
          <ul className={styles.list}>
            <li>
              You followed a link from an older revision of the firmware or server. Most internal
              docs moved into <Link to="/docs/intro">the canonical sidebar</Link>.
            </li>
            <li>
              You typed a URL by hand. The path is case-sensitive and uses kebab-case
              (e.g. <code>/docs/firmware-reference/build-flash</code>).
            </li>
            <li>
              You hit search for something that hasn&apos;t been written up yet. File an issue
              on{' '}
              <Link href="https://github.com/lorcan35/TinkerTab/issues">TinkerTab</Link> or{' '}
              <Link href="https://github.com/lorcan35/TinkerBox/issues">TinkerBox</Link> and
              we&apos;ll add it.
            </li>
          </ul>
          <div className={styles.ctaRow}>
            <Link
              className={`button button--primary button--lg ${styles.cta}`}
              to="/">
              ← Back to home
            </Link>
            <Link
              className={`button button--secondary button--lg ${styles.cta}`}
              to="/docs/intro">
              Browse the docs
            </Link>
          </div>
        </div>
      </main>
    </Layout>
  );
}
