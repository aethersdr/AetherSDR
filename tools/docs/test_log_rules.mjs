#!/usr/bin/env node
// Tests for the docs-site Log Analyzer (docs/user/src/pages/log-analyzer.js).
//
//   node tools/docs/test_log_rules.mjs
//
// Checks, for every rule in docs/user/src/components/LogAnalyzer/rules.json:
//   1. it compiles and its id is unique;
//   2. DRIFT: every `source` entry's file still contains its `text` (the
//      literal the app writes), so a reworded or removed log message fails
//      here the way stale generated docs fail their drift check. A message
//      that only MOVED is reported but does not fail; run with --update to
//      rewrite the cited line numbers in rules.json;
//   3. a substring rule's needle is part of one of its source literals, so a
//      rule cannot quietly match something the app never writes;
//   4. it fires on its positive sample docs/user/log-samples/rules/<id>.log;
//   5. its docs link points at an existing page and heading anchor, in the
//      Stable snapshot for /<page> or in docs/ for /next/<page>.
// And, for the analyzer as a whole:
//   6. no rule fires on docs/user/log-samples/clean.log;
//   7. no sample file is orphaned (every rules/*.log belongs to a rule);
//   8. a support-bundle .zip built like SupportBundle.cpp builds one is read
//      back, analyzed, and its summary (version, OS, Qt, radio) extracted.
//
// Stdlib only (Node 20+). Exits non-zero on the first failing group.

import fs from 'node:fs';
import path from 'node:path';
import zlib from 'node:zlib';
import {fileURLToPath, pathToFileURL} from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const repo = path.resolve(here, '..', '..');
const site = path.join(repo, 'docs', 'user');
const samples = path.join(site, 'log-samples');
const engine = await import(pathToFileURL(path.join(site, 'src/components/LogAnalyzer/analyze.mjs')).href);
const rulesFile = JSON.parse(fs.readFileSync(path.join(site, 'src/components/LogAnalyzer/rules.json'), 'utf8'));
const rules = rulesFile.rules;

let failures = 0;
let checks = 0;
function check(cond, msg) {
  checks++;
  if (!cond) {
    failures++;
    console.error(`FAIL: ${msg}`);
  }
}

// ── 1. compile, unique ids ────────────────────────────────────────────────
let compiled;
try {
  compiled = engine.compileRules(rules);
  check(true, 'compile');
} catch (e) {
  check(false, `rules do not compile: ${e.message}`);
  process.exit(1);
}
const ids = new Set();
for (const r of rules) {
  check(!ids.has(r.id), `duplicate rule id ${r.id}`);
  ids.add(r.id);
  for (const k of ['id', 'severity', 'title', 'explanation', 'docs', 'source']) {
    check(r[k] !== undefined && r[k] !== '', `rule ${r.id} missing ${k}`);
  }
  check(Array.isArray(r.source) && r.source.length > 0, `rule ${r.id} needs at least one source`);
}

// ── 2/3. source drift ─────────────────────────────────────────────────────
const update = process.argv.includes('--update');
const staleLines = [];
let updated = false;
const fileCache = new Map();
function lines(rel) {
  if (!fileCache.has(rel)) {
    const p = path.join(repo, rel);
    fileCache.set(rel, fs.existsSync(p) ? fs.readFileSync(p, 'utf8').split('\n') : null);
  }
  return fileCache.get(rel);
}
function checkSource(owner, s, external = false) {
  const src = lines(s.file);
  if (!src) { check(false, `${owner}: source file ${s.file} does not exist`); return; }
  if (external) {
    // origin=external: an OS-loader message quoted by a GENERATED docs page.
    // Its line moves whenever the wiki changes above it, so only require the
    // text somewhere in the file.
    check(src.some((l) => l.includes(s.text)), `${owner}: ${s.file} no longer quotes ${JSON.stringify(s.text)}`);
    return;
  }
  const line = src[s.line - 1];
  if (line !== undefined && line.includes(s.text)) { check(true, ''); return; }
  // The message still exists but moved: not a failure (edits elsewhere in a
  // busy file such as RadioModel.cpp would otherwise redden Static checks on
  // unrelated PRs). Report it, and let --update refresh the line number.
  const moved = src.map((l, i) => (l.includes(s.text) ? i + 1 : 0)).filter(Boolean);
  if (moved.length) {
    const nearest = moved.reduce((a, b) => (Math.abs(b - s.line) < Math.abs(a - s.line) ? b : a));
    staleLines.push(`${owner}: ${s.file}:${s.line} → ${nearest}`);
    if (update) { s.line = nearest; updated = true; }
    check(true, '');
    return;
  }
  check(false, `${owner}: ${s.file} no longer contains ${JSON.stringify(s.text)}`
    + ' — the message was reworded or removed; update or drop the rule');
}
for (const r of rules) {
  for (const s of r.source || []) checkSource(`rule ${r.id}`, s, r.origin === 'external');
  if (r.origin !== 'external') {
    check((r.source || []).every((s) => s.file.startsWith('src/')), `rule ${r.id}: app rules must cite src/`);
  }
  if (r.match && typeof r.match.substring === 'string') {
    const inSource = (r.source || []).some((s) => s.text.includes(r.match.substring)
      || r.match.substring.includes(s.text));
    check(inSource, `rule ${r.id}: substring ${JSON.stringify(r.match.substring)} overlaps none of its source texts`);
  }
}
for (const s of rulesFile.summary || []) {
  for (const src of s.source || []) checkSource(`summary ${s.key}`, src);
}
if (staleLines.length) {
  console.log(`NOTE: ${staleLines.length} cited line number(s) moved${update ? ' (updated)' : ' — run with --update to refresh'}:`);
  for (const l of staleLines) console.log(`  ${l}`);
}
if (updated) {
  fs.writeFileSync(path.join(site, 'src/components/LogAnalyzer/rules.json'), JSON.stringify(rulesFile, null, 2) + '\n');
}

// ── 4. positive samples ───────────────────────────────────────────────────
const ruleSampleDir = path.join(samples, 'rules');
for (const r of rules) {
  const p = path.join(ruleSampleDir, `${r.id}.log`);
  if (!fs.existsSync(p)) { check(false, `rule ${r.id}: no positive sample ${path.relative(repo, p)}`); continue; }
  const {findings} = engine.analyzeLogs(compiled, [{name: `${r.id}.log`, text: fs.readFileSync(p, 'utf8')}]);
  check(findings.some((f) => f.id === r.id), `rule ${r.id} does not fire on its positive sample`);
}

// ── 7. orphaned samples ───────────────────────────────────────────────────
for (const f of fs.readdirSync(ruleSampleDir)) {
  if (!f.endsWith('.log')) continue;
  check(ids.has(f.replace(/\.log$/, '')), `sample ${f} matches no rule id`);
}

// ── 6. clean log ──────────────────────────────────────────────────────────
{
  const text = fs.readFileSync(path.join(samples, 'clean.log'), 'utf8');
  const {findings, parsed} = engine.analyzeLogs(compiled, [{name: 'clean.log', text}]);
  check(findings.length === 0, `clean.log fires: ${findings.map((f) => f.id).join(', ')}`);
  const s = engine.extractSummary(parsed[0].entries, summaryPatterns());
  check(s.version === '26.10.1', `clean.log summary version: ${s.version}`);
  check(s.os && s.os.startsWith('Ubuntu'), `clean.log summary os: ${s.os}`);
  check(s.radioModel === 'FLEX-8600', `clean.log summary radio model: ${s.radioModel}`);
  check(s.firmware && s.firmware.startsWith('4.'), `clean.log summary firmware: ${s.firmware}`);
}

function summaryPatterns() {
  return (rulesFile.summary || []).map((s) => ({key: s.key, re: new RegExp(s.regex), last: !!s.last}));
}

// ── 5. docs links ─────────────────────────────────────────────────────────
// Docusaurus heading ids: github-slugger over the heading's text content.
function slugify(text) {
  return text
    .replace(/`/g, '')
    .replace(/\[([^\]]*)\]\([^)]*\)/g, '$1')
    .replace(/[*_]/g, '')
    .trim()
    .toLowerCase()
    .replace(/[^\p{L}\p{M}\p{N}\p{Pc}\- ]/gu, '')
    .replace(/ /g, '-');
}
// A rule links /<page> on the Stable docs (the site's default version, the
// one users reading a released build need) or /next/<page> on Next (main);
// each is checked against the tree that version is built from.
function readPages(docsDir) {
  const pages = new Map();
  if (!fs.existsSync(docsDir)) return pages;
  for (const f of fs.readdirSync(docsDir)) {
    if (!f.endsWith('.md') && !f.endsWith('.mdx')) continue;
    const text = fs.readFileSync(path.join(docsDir, f), 'utf8');
    const fm = /^---\n([\s\S]*?)\n---/.exec(text);
    let slug = '/' + f.replace(/\.mdx?$/, '');
    if (fm) {
      const m = /^slug:\s*"?([^"\n]+)"?\s*$/m.exec(fm[1]);
      if (m) slug = m[1].startsWith('/') ? m[1] : '/' + m[1];
    }
    const anchors = new Set();
    const seen = new Map();
    let inFence = false;
    for (const l of text.split('\n')) {
      if (/^\s*(```|~~~)/.test(l)) { inFence = !inFence; continue; }
      if (inFence) continue;
      const h = /^#{1,6}\s+(.*?)\s*#*\s*$/.exec(l);
      if (!h) continue;
      const explicit = /\{#([^}]+)\}\s*$/.exec(h[1]);
      let id = explicit ? explicit[1] : slugify(h[1]);
      const n = seen.get(id) || 0;
      seen.set(id, n + 1);
      if (n > 0 && !explicit) id = `${id}-${n}`;
      anchors.add(id);
    }
    pages.set(slug, anchors);
  }
  return pages;
}
const versions = {
  stable: {dir: 'versioned_docs/version-stable', pages: readPages(path.join(site, 'versioned_docs', 'version-stable'))},
  next: {dir: 'docs', pages: readPages(path.join(site, 'docs'))},
};
check(versions.stable.pages.size > 0, 'the stable snapshot docs/user/versioned_docs/version-stable has no pages');
for (const r of rules) {
  const [link, anchor] = r.docs.split('#');
  const v = link.startsWith('/next/') ? versions.next : versions.stable;
  const page = link.startsWith('/next/') ? link.slice('/next'.length) : link;
  const anchors = v.pages.get(page);
  check(!!anchors, `rule ${r.id}: docs page ${page} does not exist in docs/user/${v.dir}`);
  if (anchors && anchor) check(anchors.has(anchor), `rule ${r.id}: anchor #${anchor} not found on ${page} in docs/user/${v.dir}`);
}

// ── 8. support bundle round trip ──────────────────────────────────────────
function crc32(buf) {
  let c, crc = 0xffffffff;
  for (let n = 0; n < buf.length; n++) {
    c = (crc ^ buf[n]) & 0xff;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    crc = (crc >>> 8) ^ c;
  }
  return (crc ^ 0xffffffff) >>> 0;
}
// Mirrors src/core/ZipArchive.cpp writeDeflatedZip: method 8, sizes in the
// local header, no data descriptor.
function makeZip(entries) {
  const locals = [];
  const central = [];
  let offset = 0;
  for (const {name, data} of entries) {
    const raw = Buffer.from(data);
    const comp = zlib.deflateRawSync(raw);
    const nameBuf = Buffer.from(name);
    const crc = crc32(raw);
    const lh = Buffer.alloc(30);
    lh.writeUInt32LE(0x04034b50, 0); lh.writeUInt16LE(20, 4); lh.writeUInt16LE(0, 6);
    lh.writeUInt16LE(8, 8); lh.writeUInt32LE(crc, 14);
    lh.writeUInt32LE(comp.length, 18); lh.writeUInt32LE(raw.length, 22);
    lh.writeUInt16LE(nameBuf.length, 26);
    locals.push(lh, nameBuf, comp);
    const ch = Buffer.alloc(46);
    ch.writeUInt32LE(0x02014b50, 0); ch.writeUInt16LE(20, 4); ch.writeUInt16LE(20, 6);
    ch.writeUInt16LE(8, 10); ch.writeUInt32LE(crc, 16);
    ch.writeUInt32LE(comp.length, 20); ch.writeUInt32LE(raw.length, 24);
    ch.writeUInt16LE(nameBuf.length, 28); ch.writeUInt32LE(offset, 42);
    central.push(ch, nameBuf);
    offset += 30 + nameBuf.length + comp.length;
  }
  const cd = Buffer.concat(central);
  const end = Buffer.alloc(22);
  end.writeUInt32LE(0x06054b50, 0);
  end.writeUInt16LE(entries.length, 8); end.writeUInt16LE(entries.length, 10);
  end.writeUInt32LE(cd.length, 12); end.writeUInt32LE(offset, 16);
  return new Uint8Array(Buffer.concat([...locals, cd, end]));
}
{
  const problem = fs.readFileSync(path.join(ruleSampleDir, `${rules[0].id}.log`), 'utf8');
  const clean = fs.readFileSync(path.join(samples, 'clean.log'), 'utf8');
  const zip = makeZip([
    {name: 'aethersdr-1.log', data: clean},
    {name: 'aethersdr.log', data: problem},
    {name: 'system-info.json', data: JSON.stringify({aetherVersion: '26.10.1', qtVersion: '6.12.0', os: 'Windows 11 Version 25H2'})},
    {name: 'radio-info.json', data: JSON.stringify({connected: true, model: 'FLEX-6600', firmware: '4.1.5.39794'})},
    {name: 'settings.txt', data: 'not a log'},
  ]);
  check(engine.isZip(zip), 'bundle: isZip');
  const bundle = await engine.readSupportBundle(zip);
  check(bundle.logs.map((l) => l.name).join(',') === 'aethersdr.log,aethersdr-1.log',
    `bundle: logs newest first, got ${bundle.logs.map((l) => l.name)}`);
  const {findings, parsed} = engine.analyzeLogs(compiled, bundle.logs);
  check(findings.some((f) => f.id === rules[0].id && f.examples[0].file === 'aethersdr.log'),
    'bundle: finding from aethersdr.log with file name on the example');
  const s = engine.extractSummary(parsed.flatMap((p) => p.entries), summaryPatterns(), bundle.systemInfo, bundle.radioInfo);
  check(s.qt === '6.12.0' && s.os === 'Windows 11 Version 25H2' && s.radioModel === 'FLEX-6600'
    && s.firmware === '4.1.5.39794', `bundle: summary from JSON, got ${JSON.stringify(s)}`);
}

// ── engine details ────────────────────────────────────────────────────────
{
  const e = engine.parseLog('[01:02:03.004] WRN aether.audio: hello\ncontinued\r\n');
  check(e.length === 2 && e[0].level === 'WRN' && e[0].category === 'aether.audio' && e[1].n === 2 && !e[1].level,
    'parseLog: prefixed and continuation lines');
}

const total = rules.length;
if (failures) {
  console.error(`\n${failures} of ${checks} checks failed (${total} rules).`);
  process.exit(1);
}
console.log(`OK: ${checks} checks passed for ${total} rules.`);
