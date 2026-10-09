import React from 'react';
import Layout from '@theme/Layout';
import LogAnalyzer from '@site/src/components/LogAnalyzer';

export default function LogAnalyzerPage() {
  return (
    <Layout
      title="Log Analyzer"
      description="Check an AetherSDR log or support bundle for known problems. Runs entirely in your browser; nothing is uploaded.">
      <main className="container margin-vert--lg">
        <LogAnalyzer />
      </main>
    </Layout>
  );
}
