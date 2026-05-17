import type {ReactNode} from 'react';
import useDocusaurusContext from '@docusaurus/useDocusaurusContext';
import Layout from '@theme/Layout';
import HeroBanner from '@site/src/components/HeroBanner';
import FeatureCards from '@site/src/components/FeatureCards';

export default function Home(): ReactNode {
  const {siteConfig} = useDocusaurusContext();
  return (
    <Layout
      title="TinkerTab — voice-first home assistant"
      description={siteConfig.tagline}>
      <HeroBanner />
      <main>
        <FeatureCards />
      </main>
    </Layout>
  );
}
