import React, {useCallback, useMemo, useRef, useState} from 'react';
import Link from '@docusaurus/Link';
import clsx from 'clsx';
import rulesFile from './rules.json';
import {
  analyzeLogs,
  compileRules,
  extractSummary,
  isZip,
  readSupportBundle,
  SEVERITIES,
} from './analyze.mjs';
import styles from './styles.module.css';

const SEVERITY_META = {
  problem: {heading: 'Problems', alert: 'danger', label: 'Problem'},
  warning: {heading: 'Warnings', alert: 'warning', label: 'Warning'},
  info: {heading: 'Information', alert: 'info', label: 'Info'},
};

const SUMMARY_FIELDS = [
  ['version', 'AetherSDR version'],
  ['os', 'Operating system'],
  ['qt', 'Qt version'],
  ['cpu', 'CPU'],
  ['ram', 'Memory'],
  ['gpu', 'Render GPU'],
  ['qtPlatform', 'Qt platform plugin'],
  ['radioModel', 'Radio model'],
  ['firmware', 'Radio firmware'],
];

// Extra summary patterns from rules.json, so the radio lines live next to
// the rules and are drift-checked with them.
function summaryPatterns() {
  return (rulesFile.summary || []).map((s) => ({key: s.key, re: new RegExp(s.regex), last: !!s.last}));
}

const MAX_BYTES = 200 * 1024 * 1024;

export default function LogAnalyzer() {
  const compiled = useMemo(() => compileRules(rulesFile.rules), []);
  const [pasted, setPasted] = useState('');
  const [dragging, setDragging] = useState(false);
  const [busy, setBusy] = useState(false);
  const [status, setStatus] = useState('');
  const [error, setError] = useState('');
  const [result, setResult] = useState(null);
  const fileInput = useRef(null);
  const resultsHeading = useRef(null);

  const finish = useCallback((logs, systemInfo, radioInfo, sourceLabel) => {
    const {findings, parsed} = analyzeLogs(compiled, logs);
    const allEntries = parsed.flatMap((p) => p.entries);
    const recognised = allEntries.filter((e) => e.level).length;
    const summary = extractSummary(allEntries, summaryPatterns(), systemInfo, radioInfo);
    setResult({findings, summary, sourceLabel, lineCount: allEntries.length, recognised,
      files: logs.map((l) => l.name)});
    const n = findings.filter((f) => f.severity !== 'info').length;
    setStatus(n === 0
      ? `Analysis complete: no known problems found in ${sourceLabel}.`
      : `Analysis complete: ${n} possible ${n === 1 ? 'problem' : 'problems'} found in ${sourceLabel}.`);
    setError('');
    // Move focus to the results so keyboard and screen-reader users land there.
    setTimeout(() => resultsHeading.current && resultsHeading.current.focus(), 0);
  }, [compiled]);

  const analyzeFile = useCallback(async (file) => {
    setBusy(true);
    setError('');
    setResult(null);
    setStatus(`Reading ${file.name}…`);
    try {
      if (file.size > MAX_BYTES) throw new Error('TOO_BIG');
      const bytes = new Uint8Array(await file.arrayBuffer());
      if (isZip(bytes) || /\.zip$/i.test(file.name)) {
        const bundle = await readSupportBundle(bytes);
        if (bundle.logs.length === 0) throw new Error('NO_LOGS_IN_ZIP');
        finish(bundle.logs, bundle.systemInfo, bundle.radioInfo,
          `${file.name} (${bundle.logs.length} ${bundle.logs.length === 1 ? 'log' : 'logs'})`);
      } else {
        const text = new TextDecoder().decode(bytes);
        finish([{name: file.name, text}], null, null, file.name);
      }
    } catch (e) {
      setStatus('');
      setError(errorText(e));
    } finally {
      setBusy(false);
    }
  }, [finish]);

  const onPasteAnalyze = useCallback(() => {
    if (!pasted.trim()) {
      setError('Paste some log text first, or choose a file.');
      return;
    }
    setResult(null);
    finish([{name: 'pasted text', text: pasted}], null, null, 'the pasted text');
  }, [pasted, finish]);

  const onDrop = useCallback((ev) => {
    ev.preventDefault();
    setDragging(false);
    const file = ev.dataTransfer && ev.dataTransfer.files && ev.dataTransfer.files[0];
    if (file) {
      analyzeFile(file);
      return;
    }
    const text = ev.dataTransfer && ev.dataTransfer.getData('text/plain');
    if (text) {
      setPasted(text);
      finish([{name: 'dropped text', text}], null, null, 'the dropped text');
    }
  }, [analyzeFile, finish]);

  const clear = () => {
    setPasted('');
    setResult(null);
    setStatus('');
    setError('');
    if (fileInput.current) fileInput.current.value = '';
  };

  return (
    <div className={styles.wrap}>
      <h1>Log Analyzer</h1>
      <p>
        Drop in an AetherSDR log or a support bundle and this page lists the known
        problems it finds, what each one means, and where the fix is documented.
      </p>
      <div className={styles.privacy} role="note">
        <strong>Your log stays on your computer.</strong> The analysis runs entirely in
        your browser. The file is never uploaded, and this page sends nothing anywhere.
        AetherSDR already masks personal details in its logs: IPv4 and MAC addresses (all but
        the last part), IPv6 addresses, host names it connects to, radio serial numbers (all
        but the last group), passwords, tokens and keys, names, email addresses, locations and
        grid squares, and your home directory.
      </div>

      <h2>Find your log</h2>
      <ul>
        <li>
          <strong>Help → Support &amp; Diagnostics...</strong> shows the log and opens its
          folder. Use the newest <code>aethersdr-*.log</code> file.
        </li>
        <li>
          <strong>Help → File an Issue...</strong> builds a support bundle
          (<code>support-bundle-*.zip</code> in the <code>support</code> folder next to
          the logs). You can drop the .zip here as it is.
        </li>
        <li>
          Log folders: Linux <code>~/.config/AetherSDR/logs/</code>, macOS{' '}
          <code>~/Library/Preferences/AetherSDR/logs/</code>, Windows{' '}
          <code>%LOCALAPPDATA%\AetherSDR\logs\</code>. See{' '}
          <Link to="/support-and-logging">Support and Logging</Link>.
        </li>
      </ul>

      <h2>Check a log</h2>
      <div
        className={clsx(styles.dropzone, dragging && styles.dropzoneActive)}
        onDragOver={(e) => { e.preventDefault(); setDragging(true); }}
        onDragLeave={() => setDragging(false)}
        onDrop={onDrop}>
        <p>Drag a <code>.log</code>, <code>.txt</code> or support-bundle <code>.zip</code> file here, or</p>
        <input
          ref={fileInput}
          id="log-analyzer-file"
          className={styles.fileInput}
          type="file"
          accept=".log,.txt,.zip,text/plain,application/zip"
          onChange={(e) => e.target.files[0] && analyzeFile(e.target.files[0])}
        />
        <button
          type="button"
          className="button button--primary"
          disabled={busy}
          onClick={() => fileInput.current && fileInput.current.click()}
          aria-describedby="log-analyzer-file-hint">
          Choose a file…
        </button>
        <p id="log-analyzer-file-hint" className="margin-top--sm margin-bottom--none">
          <small>The file is read by your browser only.</small>
        </p>
      </div>

      <p className="margin-top--md margin-bottom--xs">
        <label htmlFor="log-analyzer-paste"><strong>Or paste log text</strong></label>
      </p>
      <textarea
        id="log-analyzer-paste"
        className={styles.paste}
        value={pasted}
        onChange={(e) => setPasted(e.target.value)}
        placeholder="[12:34:56.789] WRN aether.audio: ..."
        spellCheck={false}
      />
      <div className={styles.actions}>
        <button type="button" className="button button--primary" onClick={onPasteAnalyze} disabled={busy}>
          Analyze pasted text
        </button>
        <button type="button" className="button button--secondary" onClick={clear} disabled={busy}>
          Clear
        </button>
      </div>

      <section aria-labelledby="log-analyzer-results">
        <h2 id="log-analyzer-results" tabIndex={-1} ref={resultsHeading}>Results</h2>
        {/* Only the one-line status is live; the findings are reached by the
            focus move to the Results heading rather than read out in full. */}
        <p role="status" aria-live="polite" className={status ? undefined : 'margin-bottom--none'}>{status}</p>
        {error && <div className="alert alert--danger" role="alert">{error}</div>}
        {!result && !status && !error && <p>No log checked yet.</p>}
        {result && <Results result={result} />}
      </section>

      <RuleIndex />
    </div>
  );
}

function errorText(e) {
  switch (e && e.message) {
    case 'NO_DECOMPRESSION_STREAM':
      return 'This browser cannot unpack .zip files. Extract the support bundle and drop aethersdr.log from it instead.';
    case 'NOT_A_ZIP':
    case 'BAD_ZIP':
      return 'That .zip file could not be read. Extract it and drop aethersdr.log from it instead.';
    case 'ENCRYPTED_ZIP':
      return 'That .zip file is password-protected. Extract it and drop aethersdr.log from it instead.';
    case 'UNSUPPORTED_ZIP_METHOD':
      return 'That .zip file uses a compression method this page cannot read. Extract it and drop aethersdr.log from it instead.';
    case 'NO_LOGS_IN_ZIP':
      return 'That .zip file contains no aethersdr*.log file. Is it an AetherSDR support bundle?';
    case 'TOO_BIG':
      return 'That file is larger than 200 MB. Drop the newest aethersdr-*.log on its own.';
    default:
      return `The file could not be read (${(e && e.message) || 'unknown error'}).`;
  }
}

function Summary({summary}) {
  const rows = SUMMARY_FIELDS.filter(([k]) => summary[k]);
  if (rows.length === 0) return null;
  return (
    <>
      <h3>System</h3>
      <dl className={styles.summary}>
        {rows.map(([k, label]) => (
          <React.Fragment key={k}>
            <dt>{label}</dt>
            <dd>{summary[k]}</dd>
          </React.Fragment>
        ))}
      </dl>
    </>
  );
}

function Results({result}) {
  const {findings, summary, lineCount, recognised} = result;
  return (
    <>
      <Summary summary={summary} />
      <p>
        Checked {lineCount.toLocaleString()} lines
        {result.files.length > 1 ? ` in ${result.files.join(', ')}` : ''}.
      </p>
      {lineCount > 0 && recognised === 0 && (
        <div className="alert alert--warning margin-bottom--md">
          None of these lines look like an AetherSDR log
          (<code>[HH:mm:ss.zzz] LEVEL category: message</code>). Results may be incomplete.
        </div>
      )}
      {findings.length === 0 && (
        <div className="alert alert--success margin-bottom--md">
          No known problems found. If AetherSDR still misbehaves, see{' '}
          <Link to="/troubleshooting">Troubleshooting</Link>, turn on the log category
          for the feature in <strong>Help → Support &amp; Diagnostics...</strong>,
          reproduce the problem and check the new log.
        </div>
      )}
      {SEVERITIES.map((sev) => {
        const group = findings.filter((f) => f.severity === sev);
        if (group.length === 0) return null;
        const meta = SEVERITY_META[sev];
        return (
          <section key={sev} aria-label={`${meta.heading} (${group.length})`}>
            <h3>{meta.heading} ({group.length})</h3>
            {group.map((f) => <Finding key={f.id} f={f} meta={meta} />)}
          </section>
        );
      })}
    </>
  );
}

function Finding({f, meta}) {
  return (
    <article className={clsx('alert', `alert--${meta.alert}`, styles.finding)}>
      <h4>
        <span className={styles.srOnly}>{meta.label}: </span>
        {f.title}{' '}
        <span className={styles.count}>
          ({f.count === 1 ? 'seen once' : `seen ${f.count} times`})
        </span>
      </h4>
      <p className="margin-bottom--sm">{f.explanation}</p>
      <ul className={styles.examples} aria-label={`Matching log lines, first ${f.examples.length}`}>
        {f.examples.map((x, i) => (
          <li key={i}>
            <span className={styles.lineNo}>
              {x.file && x.file !== 'pasted text' && x.file !== 'dropped text' ? `${x.file}:` : 'line '}{x.n}
            </span>
            {x.text}
          </li>
        ))}
      </ul>
      <Link to={f.docs}>How to fix: {docsLabel(f.docs)}</Link>
    </article>
  );
}

function docsLabel(to) {
  const [page, anchor] = to.replace(/^\//, '').split('#');
  const title = (s) => s.replace(/-/g, ' ').replace(/^\w/, (c) => c.toUpperCase());
  return anchor ? `${title(page)} → ${title(anchor)}` : title(page);
}

// Server-rendered list of every check. It documents what the analyzer looks
// for, and because the links are rendered at build time, the docs build's
// broken-link and broken-anchor checks cover every rule's docs link.
function RuleIndex() {
  return (
    <details className="margin-top--lg">
      <summary>What this page checks for ({rulesFile.rules.length} checks)</summary>
      <table className={styles.ruleList}>
        <thead>
          <tr><th scope="col">Check</th><th scope="col">Severity</th><th scope="col">Docs</th></tr>
        </thead>
        <tbody>
          {rulesFile.rules.map((r) => (
            <tr key={r.id}>
              <td>{r.title}</td>
              <td>{SEVERITY_META[r.severity].label}</td>
              <td><Link to={r.docs}>{docsLabel(r.docs)}</Link></td>
            </tr>
          ))}
        </tbody>
      </table>
      <p>
        Each check matches a message AetherSDR itself writes, and names the source line
        it comes from. Missing a problem you hit?{' '}
        <Link to="https://github.com/aethersdr/AetherSDR/issues">Open an issue</Link>.
      </p>
    </details>
  );
}
