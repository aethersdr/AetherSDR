/**
 * Exercises runPoll against a stubbed origin and an in-memory KV. The point is
 * the failure paths: every one of them must leave the previously published list
 * untouched, because a mirror that serves a truncated list is worse than one
 * that serves an old list.
 */
import { readFileSync } from 'node:fs';
import assert from 'node:assert/strict';
import { runPoll } from '../src/index.js';

const SAMPLE = readFileSync(process.argv[2], 'utf8');

// Derived, never hardcoded. The fixture is a trimmed slice of a real origin
// page chosen to cover every ext_api regime (including the key being absent),
// plus flagged/offline/multi-range-bands/3-value-snr shapes the full snapshot
// happened not to contain. Hardcoding its length is what let the old 870 rot.
const SAMPLE_COUNT = (SAMPLE.match(/<div class='cl-entry/g) || []).length;

function makeKV(seed = {}) {
  const store = new Map(Object.entries(seed));
  return {
    store,
    get: async (k, type) => {
      const v = store.get(k);
      if (v === undefined) return null;
      return type === 'json' ? JSON.parse(v) : v;
    },
    put: async (k, v) => { store.set(k, v); },
  };
}

const baseEnv = (kv, over = {}) => ({
  KIWI: kv,
  KIWI_SOURCE_URL: 'https://files.kiwisdr.com/public/',
  KIWI_USER_AGENT: 'test',
  KIWI_SECRET: 'shhh',
  KIWI_AUTH_MODE: 'query',
  KIWI_AUTH_NAME: 'key',
  KIWI_MIN_ENTRIES_FRACTION: '0.5',
  KIWI_STALE_AFTER_MINUTES: '360',
  ...over,
});

let calls = [];
const stub = (fn) => { calls = []; globalThis.fetch = async (req) => { calls.push(req); return fn(req); }; };
const ok = (body, headers = {}) => new Response(body, { status: 200, headers });

const results = [];
const t = async (name, fn) => {
  try { await fn(); results.push(['PASS', name]); }
  catch (e) { results.push(['FAIL', name + ' -> ' + e.message]); }
};

// 1. No secret: must not touch the origin at all.
await t('no secret -> auth fault, zero origin requests', async () => {
  const kv = makeKV();
  stub(() => ok('should never be called'));
  const h = await runPoll(baseEnv(kv, { KIWI_SECRET: '' }));
  assert.equal(h.last_run_result, 'failed');
  assert.equal(h.fault, 'auth');
  assert.equal(h.last_run_reason, 'no-secret');
  assert.equal(calls.length, 0, 'made ' + calls.length + ' origin requests');
});

// 2. Happy path.
await t('good fetch -> publishes every entry in the fixture', async () => {
  const kv = makeKV();
  stub(() => ok(SAMPLE, { etag: 'W/"abc"' }));
  const h = await runPoll(baseEnv(kv));
  assert.equal(h.last_run_result, 'published');
  assert.equal(h.receiver_count, SAMPLE_COUNT);
  const payload = JSON.parse(kv.store.get('directory:payload'));
  assert.equal(payload.receivers.length, SAMPLE_COUNT);
  assert.equal(JSON.parse(kv.store.get('directory:status')).etag, 'W/"abc"');
});

// 3. Secret is presented as configured.
await t('secret goes on the query string as configured', async () => {
  const kv = makeKV();
  stub(() => ok(SAMPLE));
  await runPoll(baseEnv(kv));
  assert.equal(new URL(calls[0].url).searchParams.get('key'), 'shhh');
});

// 4. 304 keeps the old list.
await t('304 -> not-modified, payload untouched', async () => {
  const kv = makeKV({
    'directory:payload': JSON.stringify({ receivers: [1, 2, 3] }),
    'directory:status': JSON.stringify({ receiver_count: SAMPLE_COUNT, etag: 'W/"abc"' }),
  });
  stub(() => new Response(null, { status: 304 }));
  const h = await runPoll(baseEnv(kv));
  assert.equal(h.last_run_result, 'not-modified');
  assert.equal(h.consecutive_failures, 0);
  assert.equal(JSON.parse(kv.store.get('directory:payload')).receivers.length, 3);
});

// 5. The guard that matters most.
await t('truncated page -> needs-review, old list preserved', async () => {
  // The guard compares a RATIO against the last published count, so the
  // baseline is the fixture's own length — a small fixture exercises it
  // identically, and deriving it stops this rotting the way the old 870 did.
  const kv = makeKV({
    'directory:payload': JSON.stringify({ receivers: new Array(SAMPLE_COUNT).fill(0) }),
    'directory:status': JSON.stringify({ receiver_count: SAMPLE_COUNT }),
  });
  const firstTwo = SAMPLE.split("<div class='cl-entry").slice(0, 3).join("<div class='cl-entry");
  stub(() => ok(firstTwo));
  const h = await runPoll(baseEnv(kv));
  assert.equal(h.last_run_result, 'failed');
  assert.equal(h.fault, 'needs-review');
  assert.equal(JSON.parse(kv.store.get('directory:payload')).receivers.length, SAMPLE_COUNT,
    'old list was overwritten');
});

// 6. Redirects are recorded, never followed.
await t('redirect -> upstream-change, not followed', async () => {
  const kv = makeKV();
  stub(() => new Response(null, { status: 302, headers: { location: 'https://elsewhere.example/' } }));
  const h = await runPoll(baseEnv(kv));
  assert.equal(h.fault, 'upstream-change');
  assert.equal(h.last_run_detail, 'https://elsewhere.example/');
  assert.equal(calls.length, 1, 'retried a redirect');
});

// 7. The gate page returns 200, so only the body gives it away.
await t('gate page -> auth fault despite HTTP 200', async () => {
  const kv = makeKV();
  stub(() => ok('<html><body>Please click to agree and continue</body></html>'));
  const h = await runPoll(baseEnv(kv));
  assert.equal(h.fault, 'auth');
  assert.equal(h.last_run_reason, 'gate-page');
});

// 8. 5xx retries, 4xx does not.
await t('500 retries to the limit; 403 gives up at once', async () => {
  const kv = makeKV();
  stub(() => new Response('err', { status: 500 }));
  let h = await runPoll(baseEnv(kv));
  assert.equal(h.last_run_attempts, 3, 'expected 3 attempts on 5xx');
  assert.equal(h.fault, 'origin');
  stub(() => new Response('denied', { status: 403 }));
  h = await runPoll(baseEnv(kv));
  assert.equal(calls.length, 1, 'retried a 403');
  assert.equal(h.last_run_detail, '403');
});

// 9. History is bounded and newest-first, matching the page's SLOTS.
await t('history caps at 20, newest first', async () => {
  const kv = makeKV();
  stub(() => ok(SAMPLE));
  let h;
  // Distinct, strictly increasing stamps. The old loop used hour `i % 24` over
  // 25 iterations, so `at` was not monotonic and no honest ordering assert
  // could hold — and the assert it did make was `... || h.recent.length === 20`,
  // whose right operand the line above had just proven, so reversing the
  // history would have left it green.
  for (let i = 0; i < 25; i++) h = await runPoll(baseEnv(kv), new Date(Date.UTC(2026, 8, 5, 0, i)));
  assert.equal(h.recent.length, 20);
  assert.ok(h.recent[0].at > h.recent[1].at, 'history is not newest-first');
});

// 13. A mode originRequest cannot speak is OUR misconfiguration, not the
// origin's. Without membership validation it reached the `else throw` inside
// the retry loop and was reported as fault 'origin', three times over.
await t('an unknown auth mode -> mirror fault, zero origin requests', async () => {
  const kv = makeKV();
  stub(() => ok('should never be called'));
  const h = await runPoll(baseEnv(kv, { KIWI_AUTH_MODE: 'Header' }));
  assert.equal(h.fault, 'mirror');
  assert.equal(h.last_run_reason, 'auth-misconfigured');
  assert.equal(h.last_run_attempts, 0);
  assert.equal(calls.length, 0, 'made ' + calls.length + ' origin requests');
});

// 14. A 304 is a success, so it must refresh last_success_at — the status page
// reads that as "the origin confirmed our copy", and reading the publish time
// instead made healthy 304 streams look stale and hid the frozen-mirror alarm.
await t('304 refreshes last_success_at without republishing', async () => {
  const kv = makeKV({
    'directory:payload': JSON.stringify({ receivers: [1, 2, 3] }),
    'directory:status': JSON.stringify({ receiver_count: SAMPLE_COUNT, etag: 'W/"abc"',
                                         fetched_at: '2026-09-01T00:00:00.000Z' }),
  });
  stub(() => new Response(null, { status: 304 }));
  const h = await runPoll(baseEnv(kv), new Date(Date.UTC(2026, 8, 5, 12)));
  assert.equal(h.last_run_result, 'not-modified');
  assert.equal(h.last_success_at, '2026-09-05T12:00:00.000Z');
  // The publish record must NOT move: nothing was republished.
  assert.equal(JSON.parse(kv.store.get('directory:status')).fetched_at,
               '2026-09-01T00:00:00.000Z');
});

// 15. The URL cross-check the parser's comment promises.
await t('an entry whose anchors disagree is skipped, not silently reurled', async () => {
  const kv = makeKV();
  // Derived from the fixture, not hardcoded: rewrite only the FIRST anchor of
  // the first entry so its two anchors disagree. A hardcoded host silently
  // stops testing anything the moment the fixture is retrimmed.
  const first = SAMPLE.match(/<a\s+href='([^']+)'\s+target='_blank'>/);
  assert.ok(first, 'fixture has no receiver anchor');
  const swapped = SAMPLE.replace(first[0], first[0].replace(first[1], 'http://swapped.example:8073'));
  assert.notEqual(swapped, SAMPLE, 'fixture rewrite was a no-op');
  stub(() => ok(swapped));
  const h = await runPoll(baseEnv(kv));
  assert.equal(h.receiver_count, SAMPLE_COUNT - 1, 'disagreeing entry was not skipped');
});

// 10. The weak-validator bug: Cloudflare hands us W/"x" for a gzip response the
// origin ETags as "x". Sending the W/ form back matches nothing and we
// republish an identical list every hour.
await t('If-None-Match is sent without the W/ prefix', async () => {
  const kv = makeKV({ 'directory:status': JSON.stringify({ receiver_count: SAMPLE_COUNT, etag: 'W/"abc123"' }) });
  stub(() => new Response(null, { status: 304 }));
  await runPoll(baseEnv(kv));
  assert.equal(calls[0].headers.get('if-none-match'), '"abc123"');
});

// 11. A strong ETag must survive untouched.
await t('a strong ETag is sent verbatim', async () => {
  const kv = makeKV({ 'directory:status': JSON.stringify({ receiver_count: SAMPLE_COUNT, etag: '"plain"' }) });
  stub(() => new Response(null, { status: 304 }));
  await runPoll(baseEnv(kv));
  assert.equal(calls[0].headers.get('if-none-match'), '"plain"');
});

// 12. The auth transport is a Worker secret like the value, so a deploy can be
// half-configured. That is OUR mistake: it must be reported as a mirror fault
// rather than an origin one, and must not spend a single request — let alone
// three retries — discovering it on his server.
await t('missing auth transport -> mirror fault, zero origin requests', async () => {
  const kv = makeKV();
  stub(() => ok('should never be called'));
  const h = await runPoll(baseEnv(kv, { KIWI_AUTH_MODE: '' }));
  assert.equal(h.last_run_result, 'failed');
  assert.equal(h.fault, 'mirror');
  assert.equal(h.last_run_reason, 'auth-misconfigured');
  assert.equal(h.last_run_attempts, 0);
  assert.equal(calls.length, 0, 'made ' + calls.length + ' origin requests');
});

for (const [s, n] of results) console.log(s === 'PASS' ? '  ok   ' + n : '  FAIL ' + n);
const failed = results.filter(([s]) => s === 'FAIL').length;
console.log('\n' + (results.length - failed) + '/' + results.length + ' passed');
process.exit(failed ? 1 : 0);
