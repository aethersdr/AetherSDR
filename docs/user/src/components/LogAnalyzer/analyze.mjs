// AetherSDR log analyzer engine. Pure functions, no DOM, no network: the
// page and the rule tests (tools/docs/test_log_rules.mjs) both import this
// file, so what the tests check is exactly what runs in the browser.
//
// Log line format (src/core/AsyncLogWriter.cpp formatLine):
//   [HH:mm:ss.zzz] LVL category: message
// LVL is DBG, INF, WRN, CRT or FTL. A message can span several lines; the
// continuation lines have no prefix and belong to the line above.

export const SEVERITIES = ['problem', 'warning', 'info'];

const LINE_RE = /^\[(\d{2}:\d{2}:\d{2}\.\d{3})\] (DBG|INF|WRN|CRT|FTL|\?\?\?) ([^\s:]+): ?(.*)$/;

/** Split log text into entries. Each entry keeps its 1-based line number. */
export function parseLog(text) {
  const lines = text.replace(/\r\n?/g, '\n').split('\n');
  if (lines.length && lines[lines.length - 1] === '') lines.pop();
  const entries = [];
  for (let i = 0; i < lines.length; i++) {
    const raw = lines[i];
    const m = LINE_RE.exec(raw);
    if (m) {
      entries.push({n: i + 1, raw, time: m[1], level: m[2], category: m[3], msg: m[4]});
    } else {
      // Not in the app's format: a continuation line, a pasted fragment, or
      // stderr from a library. Keep it so substring rules still see it.
      entries.push({n: i + 1, raw, time: null, level: null, category: null, msg: raw});
    }
  }
  return entries;
}

function compileMatcher(spec) {
  if (!spec) return null;
  if (typeof spec.substring === 'string') {
    const needle = spec.substring;
    return (s) => s.includes(needle);
  }
  if (typeof spec.regex === 'string') {
    const re = new RegExp(spec.regex, spec.flags || '');
    return (s) => re.test(s);
  }
  throw new Error('rule matcher needs "substring" or "regex"');
}

/** Compile rules once; throws on a malformed rule. */
export function compileRules(rules) {
  return rules.map((rule) => {
    const match = compileMatcher(rule.match);
    if (!match) throw new Error(`rule ${rule.id} has no match`);
    if (!SEVERITIES.includes(rule.severity)) {
      throw new Error(`rule ${rule.id} has unknown severity ${rule.severity}`);
    }
    return {
      rule,
      match,
      unless: compileMatcher(rule.unless),
      levels: rule.levels ? new Set(rule.levels) : null,
      minCount: rule.minCount || 1,
    };
  });
}

function entryText(e) {
  // Match against the whole raw line so a rule can include the category.
  return e.raw;
}

/**
 * Run every rule over the parsed entries.
 * Returns findings sorted problem -> warning -> info, each with up to
 * `maxExamples` matching lines and the total count.
 */
export function runRules(compiled, entries, maxExamples = 3) {
  const findings = [];
  for (const c of compiled) {
    const hits = [];
    let count = 0;
    let lastIdx = -1;
    for (let i = 0; i < entries.length; i++) {
      const e = entries[i];
      if (c.levels && e.level && !c.levels.has(e.level)) continue;
      if (!c.match(entryText(e))) continue;
      count++;
      lastIdx = i;
      if (hits.length < maxExamples) hits.push({n: e.n, text: e.raw});
    }
    if (count < c.minCount) continue;
    if (c.unless) {
      // "and not followed by": a later recovery line clears the finding.
      let recovered = false;
      for (let i = lastIdx + 1; i < entries.length; i++) {
        if (c.unless(entryText(entries[i]))) { recovered = true; break; }
      }
      if (recovered) continue;
    }
    findings.push({
      id: c.rule.id,
      severity: c.rule.severity,
      title: c.rule.title,
      explanation: c.rule.explanation,
      docs: c.rule.docs,
      count,
      examples: hits,
    });
  }
  findings.sort((a, b) => SEVERITIES.indexOf(a.severity) - SEVERITIES.indexOf(b.severity));
  return findings;
}

/**
 * Analyze several logs (a support bundle carries up to three). Each log is
 * checked on its own, so "not followed by" never crosses a file boundary;
 * findings for the same rule are merged and each example names its file.
 */
export function analyzeLogs(compiled, logs, maxExamples = 3) {
  const merged = new Map();
  const parsed = [];
  for (const log of logs) {
    const entries = parseLog(log.text);
    parsed.push({name: log.name, entries});
    for (const f of runRules(compiled, entries, maxExamples)) {
      const ex = f.examples.map((x) => ({...x, file: log.name}));
      const prev = merged.get(f.id);
      if (!prev) {
        merged.set(f.id, {...f, examples: ex});
      } else {
        prev.count += f.count;
        prev.examples = prev.examples.concat(ex).slice(0, maxExamples);
      }
    }
  }
  const findings = [...merged.values()];
  findings.sort((a, b) => SEVERITIES.indexOf(a.severity) - SEVERITIES.indexOf(b.severity));
  return {findings, parsed};
}

// ── Summary header ─────────────────────────────────────────────────────────

const SUMMARY_PATTERNS = [
  // main.cpp: qDebug() << "Starting AetherSDR" << app.applicationVersion();
  {key: 'version', re: /Starting AetherSDR "?v?([0-9][^"\s]*)"?/},
  // SystemInventory.cpp: "OS:" <prettyProductName> "kernel" <ver> "arch" <arch>
  {key: 'os', re: /aether\.sysinfo: OS: (.+?)(?: kernel .*)?$/},
  {key: 'cpu', re: /aether\.sysinfo: CPU: (.+)$/},
  {key: 'ram', re: /aether\.sysinfo: RAM: (\d+) MB/, fmt: (v) => `${v} MB`},
  // main.cpp: "GpuSelector: render GPU ->" <summary>
  {key: 'gpu', re: /GpuSelector: render GPU -> (.+)$/},
];

/**
 * Pull the app version, OS, Qt version, radio model and firmware out of the
 * log and, for a support bundle, its system-info.json / radio-info.json.
 * JSON values win over log lines because they are exact.
 */
export function extractSummary(entries, extraPatterns = [], systemInfo = null, radioInfo = null) {
  const out = {};
  const patterns = SUMMARY_PATTERNS.concat(extraPatterns);
  for (const e of entries) {
    for (const p of patterns) {
      if (out[p.key] && !p.last) continue;
      const m = p.re.exec(e.raw);
      if (m) {
        const v = m[1].trim();
        out[p.key] = p.fmt ? p.fmt(v) : v;
      }
    }
  }
  if (systemInfo) {
    if (systemInfo.aetherVersion) out.version = systemInfo.aetherVersion;
    if (systemInfo.qtVersion) out.qt = systemInfo.qtVersion;
    if (systemInfo.os) out.os = systemInfo.os;
    if (systemInfo.cpu) out.cpu = systemInfo.cpu;
    if (systemInfo.ram) out.ram = systemInfo.ram;
    if (systemInfo.gpu) out.gpu = systemInfo.gpu;
  }
  if (radioInfo && radioInfo.connected) {
    if (radioInfo.model) out.radioModel = radioInfo.model;
    if (radioInfo.firmware) out.firmware = radioInfo.firmware;
  }
  return out;
}

// ── Support bundle (.zip) ──────────────────────────────────────────────────
// The bundle (src/core/SupportBundle.cpp) is a plain ZIP of stored (0) or
// raw-deflate (8) entries. Read through the central directory, inflate with
// the platform DecompressionStream('deflate-raw'). No dependency.

function u16(b, o) { return b[o] | (b[o + 1] << 8); }
function u32(b, o) { return (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)) >>> 0; }

async function inflateRaw(bytes) {
  if (typeof DecompressionStream === 'undefined') {
    throw new Error('NO_DECOMPRESSION_STREAM');
  }
  const stream = new Blob([bytes]).stream().pipeThrough(new DecompressionStream('deflate-raw'));
  return new Uint8Array(await new Response(stream).arrayBuffer());
}

export function isZip(bytes) {
  return bytes.length >= 4 && u32(bytes, 0) === 0x04034b50;
}

/** List and extract the entries of a ZIP. Returns [{name, bytes}]. */
export async function unzip(bytes, wanted = () => true) {
  // End of central directory: scan back from the end (comment <= 64 KiB).
  let eocd = -1;
  for (let i = bytes.length - 22; i >= Math.max(0, bytes.length - 22 - 0xffff); i--) {
    if (u32(bytes, i) === 0x06054b50) { eocd = i; break; }
  }
  if (eocd < 0) throw new Error('NOT_A_ZIP');
  const count = u16(bytes, eocd + 10);
  let p = u32(bytes, eocd + 16);
  const dec = new TextDecoder();
  const out = [];
  for (let k = 0; k < count; k++) {
    if (u32(bytes, p) !== 0x02014b50) throw new Error('BAD_ZIP');
    const flags = u16(bytes, p + 8);
    const method = u16(bytes, p + 10);
    const csize = u32(bytes, p + 20);
    const nameLen = u16(bytes, p + 28);
    const extraLen = u16(bytes, p + 30);
    const commentLen = u16(bytes, p + 32);
    const local = u32(bytes, p + 42);
    const name = dec.decode(bytes.subarray(p + 46, p + 46 + nameLen));
    p += 46 + nameLen + extraLen + commentLen;
    if (name.endsWith('/') || !wanted(name)) continue;
    if (flags & 1) throw new Error('ENCRYPTED_ZIP');
    const lNameLen = u16(bytes, local + 26);
    const lExtraLen = u16(bytes, local + 28);
    const start = local + 30 + lNameLen + lExtraLen;
    const data = bytes.subarray(start, start + csize);
    let body;
    if (method === 0) body = data;
    else if (method === 8) body = await inflateRaw(data);
    else throw new Error('UNSUPPORTED_ZIP_METHOD');
    out.push({name, bytes: body});
  }
  return out;
}

const BASENAME = (n) => n.split('/').pop();

/**
 * Turn a support bundle into {logs: [{name, text}], systemInfo, radioInfo}.
 * Logs come newest first: aethersdr.log, then aethersdr-1.log, aethersdr-2.log.
 */
export async function readSupportBundle(bytes) {
  const entries = await unzip(bytes, (n) => /\.(log|txt|json)$/i.test(n));
  const dec = new TextDecoder();
  const logs = [];
  let systemInfo = null;
  let radioInfo = null;
  for (const e of entries) {
    const base = BASENAME(e.name);
    const text = dec.decode(e.bytes);
    if (base === 'system-info.json') {
      try { systemInfo = JSON.parse(text); } catch { /* ignore */ }
    } else if (base === 'radio-info.json') {
      try { radioInfo = JSON.parse(text); } catch { /* ignore */ }
    } else if (/^aethersdr.*\.log$/i.test(base)) {
      logs.push({name: base, text});
    }
  }
  const order = (n) => (n === 'aethersdr.log' ? 0 : parseInt((/-(\d+)\.log$/.exec(n) || [0, 99])[1], 10));
  logs.sort((a, b) => order(a.name) - order(b.name));
  return {logs, systemInfo, radioInfo};
}
