import React from 'react';
import Link from '@docusaurus/Link';
import Layout from '@theme/Layout';
import styles from './404.module.css';

export default function NotFound() {
  return (
    <Layout title="404 — page not found" description="404 — TinkerTab Journal">
      <main className={styles.main}>
        <div className={styles.inner}>
          <div className={styles.masthead}>
            <span className={styles.mastheadVol}>VOL. 01 · ERRATA</span>
            <span className={styles.mastheadCenter}>
              T H E &nbsp; T I N K E R T A B &nbsp; J O U R N A L
            </span>
            <span className={styles.mastheadDate}>STATUS · 404</span>
          </div>

          <div className={styles.rule} aria-hidden="true" />

          <span className={styles.eyebrow}>NO. 404 · MISSING PAGE</span>

          <h1 className={styles.title}>
            <span className={styles.line1}>This page is not</span>
            <span className={styles.line2}>
              in <em>this volume.</em>
            </span>
          </h1>

          <p className={styles.lede}>
            We checked the index, the appendices, and the marginalia. The
            page you asked for is not bound here. Three honest reasons that
            tends to happen on a publication still going to press:
          </p>

          <ol className={styles.reasons}>
            <li>
              <span className={styles.reasonNum}>I.</span>
              <span className={styles.reasonBody}>
                <strong>You followed an old citation.</strong> Internal docs
                were re-folded into{' '}
                <Link to="/docs/intro">the canonical sidebar</Link> when the
                journal went to press; some older URLs no longer resolve.
              </span>
            </li>
            <li>
              <span className={styles.reasonNum}>II.</span>
              <span className={styles.reasonBody}>
                <strong>You typed the URL by hand.</strong> Paths use
                kebab-case, e.g. <code>/docs/firmware-reference/build-flash</code>.
                The casing is exact.
              </span>
            </li>
            <li>
              <span className={styles.reasonNum}>III.</span>
              <span className={styles.reasonBody}>
                <strong>The page hasn&apos;t been written yet.</strong> File
                an issue on{' '}
                <Link href="https://github.com/lorcan35/TinkerTab/issues">
                  TinkerTab
                </Link>{' '}
                or{' '}
                <Link href="https://github.com/lorcan35/TinkerBox/issues">
                  TinkerBox
                </Link>{' '}
                and we&apos;ll set it.
              </span>
            </li>
          </ol>

          <div className={styles.ctaRow}>
            <Link
              className={`button button--primary button--lg ${styles.cta}`}
              to="/">
              ← Back to the front page
            </Link>
            <Link
              className={`button button--secondary button--lg ${styles.cta}`}
              to="/docs/intro">
              Browse the volume
            </Link>
          </div>

          <div className={styles.rule} aria-hidden="true" />

          <p className={styles.colophon}>
            The journal sends its apologies. If this page should exist, write
            to the editors via the issue trackers above. Errata is filed every
            sprint.
          </p>
        </div>
      </main>
    </Layout>
  );
}
