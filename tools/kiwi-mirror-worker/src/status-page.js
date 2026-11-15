/**
 * The operator status page, served as one self-contained document.
 *
 * Ported from the aethersdr.ozy.us page. Four things changed in the move:
 *
 *  1. The stale-poll copy is generated from RUN_STALE_MIN instead of being
 *     written out by hand. The old page said "over an hour" while the threshold
 *     was 150 minutes, which is exactly the wrong thing to be wrong about on a
 *     page people read while triaging.
 *  2. The footer said the mirror polls every 30 minutes; it went hourly.
 *  3. SLOTS now matches the worker's HISTORY, so the strip stops showing four
 *     permanent "no poll recorded" ghosts it could never fill.
 *  4. The thresholds are stated in polls as well as minutes, since that is how
 *     they were actually reasoned about.
 */

export const STATUS_PAGE = `<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>KiwiSDR mirror status</title>
<meta name="robots" content="noindex">
<link rel="icon" href="data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 16 16'%3E%3Crect width='16' height='16' rx='3' fill='%23161d26'/%3E%3Crect x='3' y='4' width='2' height='8' fill='%233f7d5a'/%3E%3Crect x='7' y='6' width='2' height='6' fill='%23a8761f'/%3E%3Crect x='11' y='3' width='2' height='9' fill='%233f7d5a'/%3E%3C/svg%3E">
<style>
/* Static page. It reads health.json and status.json in the browser, so it is
   published once and never regenerated - the Worker spends no CPU and takes no
   extra write on it, and it is always showing live data.

   System fonts on purpose. This page is read when something is broken, quite
   possibly on a phone on a bad connection, so it must not depend on a font CDN
   being up. Everything below is self-contained: two fetches, no other requests. */
:root {
  --bezel:   #161d26;
  --bezel-2: #202a37;
  --face:    #f2efe6;
  --face-2:  #e6e1d3;
  --ink:     #1d242c;
  --ink-mid: #5c6672;
  --rule:    #cfc8b6;
  --good:    #3f7d5a;
  --amber:   #a8761f;
  --bad:     #a33f34;
  --unknown: #7c8794;
  --mono: ui-monospace, "SF Mono", "Cascadia Mono", "Roboto Mono", Menlo, Consolas, monospace;
  --sans: system-ui, -apple-system, "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif;
}
@media (prefers-color-scheme: dark) {
  :root {
    --face: #1a212a; --face-2: #212b36; --ink: #e8e4d8; --ink-mid: #98a3b0;
    --rule: #303c4a; --good: #6fbb8e; --amber: #d9a441; --bad: #e0776a; --unknown: #7c8794;
  }
}
* { box-sizing: border-box; }
body {
  margin: 0; background: var(--face); color: var(--ink);
  font-family: var(--sans); line-height: 1.55;
  -webkit-font-smoothing: antialiased;
}
.wrap { max-width: 60rem; margin: 0 auto; padding: 0 1.25rem 4rem; }

.bezel { background: var(--bezel); color: #d8dee6; border-bottom: 3px solid var(--bezel-2); }
.bezel .wrap {
  display: flex; flex-wrap: wrap; gap: .5rem 1.5rem;
  align-items: baseline; justify-content: space-between;
  padding-top: 1.15rem; padding-bottom: 1.15rem;
}
.bezel h1 { font-size: 1.05rem; font-weight: 600; margin: 0; letter-spacing: .01em; }
.bezel .host { font-family: var(--mono); font-size: .82rem; color: #8f9aa8; }

.readout { padding-top: 2.75rem; }
.verdict {
  display: inline-flex; align-items: center; gap: .6rem;
  font-size: .95rem; font-weight: 600; margin-bottom: 1.5rem;
}
.lamp {
  width: .7rem; height: .7rem; border-radius: 50%;
  background: var(--tone, var(--unknown));
  box-shadow: 0 0 0 4px color-mix(in srgb, var(--tone, var(--unknown)) 18%, transparent);
  flex: none;
}
.count { display: flex; align-items: baseline; gap: 1rem; flex-wrap: wrap; margin: 0 0 .35rem; }
.count b {
  font-family: var(--mono); font-size: clamp(3rem, 12vw, 5rem);
  font-weight: 500; line-height: .95; letter-spacing: -.03em;
  font-variant-numeric: tabular-nums;
}
.count span { font-size: 1rem; color: var(--ink-mid); }
.sub { color: var(--ink-mid); font-size: .92rem; margin: 0 0 1.75rem; }

.bar { height: 6px; background: var(--face-2); border-radius: 3px; overflow: hidden; max-width: 34rem; }
.bar i { display: block; height: 100%; width: 0; background: var(--tone, var(--unknown)); transition: width .6s ease; }
@media (prefers-reduced-motion: reduce) { .bar i { transition: none; } }
.barlab {
  display: flex; justify-content: space-between; max-width: 34rem;
  font-size: .8rem; color: var(--ink-mid); margin-top: .45rem; font-family: var(--mono);
}

.fault {
  margin: 2.5rem 0 0; padding: 1.1rem 1.25rem;
  border-left: 4px solid var(--tone, var(--unknown));
  background: var(--face-2); border-radius: 0 4px 4px 0;
}
.fault h2 { margin: 0 0 .3rem; font-size: 1rem; font-weight: 600; }
.fault p { margin: 0; color: var(--ink-mid); font-size: .92rem; }

section { margin-top: 3rem; }
section > h2 { font-size: .95rem; font-weight: 600; margin: 0 0 .2rem; }
section > p.hint { margin: 0 0 1rem; color: var(--ink-mid); font-size: .87rem; }

.strip { display: flex; gap: 3px; align-items: flex-end; height: 44px; }
.strip i {
  flex: 1 1 0; min-width: 4px; max-width: 22px; border-radius: 2px;
  background: var(--unknown); height: 60%;
}
.strip i[data-r="none"] { background: var(--rule); height: 18%; }
.strip i[data-r="published"]    { background: var(--good); height: 100%; }
.strip i[data-r="not-modified"] { background: var(--good); height: 62%; opacity: .6; }
.strip i[data-r="failed"]       { background: var(--bad);  height: 34%; }
.strip i[data-f="origin"]          { background: var(--amber); }
.strip i[data-f="auth"]            { background: var(--amber); }
.strip i[data-f="upstream-change"] { background: var(--amber); }
.striplab {
  display: flex; justify-content: space-between;
  font-size: .78rem; color: var(--ink-mid); margin-top: .4rem; font-family: var(--mono);
}

table { border-collapse: collapse; width: 100%; font-size: .88rem; }
th, td { text-align: left; padding: .45rem .75rem .45rem 0; border-bottom: 1px solid var(--rule); }
th { font-weight: 600; color: var(--ink-mid); font-size: .82rem; }
td.n { font-family: var(--mono); font-variant-numeric: tabular-nums; white-space: nowrap; }
td .tag { font-weight: 600; }
.tag[data-f="origin"], .tag[data-f="auth"], .tag[data-f="upstream-change"] { color: var(--amber); }
.tag[data-f="mirror"], .tag[data-f="needs-review"] { color: var(--bad); }
.ok { color: var(--good); }

dl.read { margin: 0; display: grid; grid-template-columns: minmax(9rem, auto) 1fr; gap: .4rem 1.25rem; font-size: .88rem; }
dl.read dt { color: var(--ink-mid); }
dl.read dd { margin: 0; font-family: var(--mono); font-variant-numeric: tabular-nums; }

.guide { font-size: .88rem; }
.guide li { margin-bottom: .5rem; }
.guide b { font-weight: 600; }

footer {
  margin-top: 3.5rem; padding-top: 1.25rem; border-top: 1px solid var(--rule);
  font-size: .82rem; color: var(--ink-mid);
}
footer a { color: inherit; }
noscript { display: block; margin-top: 1rem; padding: .9rem 1.1rem; background: var(--face-2); border-left: 4px solid var(--amber); font-size: .9rem; }
a:focus-visible, [tabindex]:focus-visible { outline: 2px solid var(--tone, var(--good)); outline-offset: 2px; }
@media (max-width: 34rem) {
  dl.read { grid-template-columns: 1fr; gap: .1rem; }
  dl.read dd { margin-bottom: .5rem; }
}
</style>
</head>
<body>

<div class="bezel">
  <div class="wrap">
    <h1>KiwiSDR receiver directory &mdash; mirror status</h1>
    <span class="host">cdn.aethersdr.com/kiwi.json</span>
  </div>
</div>

<div class="wrap">

  <div class="readout">
    <div class="verdict"><span class="lamp"></span><span id="verdict">Reading status&hellip;</span></div>
    <p class="count"><b id="count">&mdash;</b> <span id="countlab">receivers in the published list</span></p>
    <p class="sub" id="sub"></p>
    <div class="bar"><i id="bar"></i></div>
    <div class="barlab"><span id="barleft"></span><span id="barright"></span></div>
    <noscript>This page reads <a href="/health.json">health.json</a> and
      <a href="/status.json">status.json</a> in the browser, so it needs JavaScript to
      show anything. Both files are plain JSON and readable directly.</noscript>
  </div>

  <div class="fault" id="fault" hidden>
    <h2 id="faulth"></h2>
    <p id="faultp"></p>
  </div>

  <section>
    <h2>Recent polls</h2>
    <p class="hint">Hourly, oldest on the left. Tall green published a new list, short green means the
      origin confirmed nothing had changed, amber is a failure on kiwisdr.com's side, red is a failure on ours.</p>
    <div class="strip" id="strip" aria-hidden="true"></div>
    <div class="striplab"><span>oldest</span><span>newest</span></div>
    <table style="margin-top:1.5rem">
      <thead><tr><th>Time (UTC)</th><th>Result</th><th>Detail</th><th>Tries</th></tr></thead>
      <tbody id="rows"></tbody>
    </table>
  </section>

  <section>
    <h2>Readings</h2>
    <dl class="read" id="readings"></dl>
  </section>

  <section>
    <h2>How to read this</h2>
    <ul class="guide" id="guide"></ul>
  </section>

  <footer>
    <p>The mirror pulls <code>kiwisdr.com/public/</code> every hour, parses it and republishes it as
      <a href="https://cdn.aethersdr.com/kiwi.json">cdn.aethersdr.com/kiwi.json</a> so AetherSDR
      clients read our copy instead of the origin. Raw data:
      <a href="/health.json">health.json</a> &middot;
      <a href="/status.json">status.json</a>.
      This page refreshes itself every 60 seconds.</p>
  </footer>
</div>

<script>
const HEALTH = '/health.json';
const STATUS = '/status.json';

// The mirror polls hourly. Both thresholds below are really "how many polls may
// be missed", and are written that way so a cadence change cannot leave them
// quietly meaning something else. Must match ops/kuma-staleness-monitor.py, or
// the panel and the alarm disagree about the same mirror.
const POLL_MINUTES = 60;
const RUN_STALE_POLLS = 2.5;
const CONTENT_STALE_POLLS = 4;
const RUN_STALE_MIN = POLL_MINUTES * RUN_STALE_POLLS;      // 150
const CONTENT_STALE_MIN = POLL_MINUTES * CONTENT_STALE_POLLS; // 240
const NO_PUBLISH_MIN = 1440; // 24h - a frozen mirror, not a quiet one
const SLOTS = 20;            // must equal HISTORY in the Worker

const $ = (id) => document.getElementById(id);

/**
 * Escape before anything reaches innerHTML.
 *
 * Two of the values rendered on this page come straight off the wire from
 * kiwisdr.com: the ETag header (copied verbatim into status.json on every
 * successful publish) and the Location header of a redirect (copied into
 * health.json's detail). The fetch leg is plain http with no TLS - that is
 * accepted for the shared secret, but the same acceptance means an on-path
 * attacker controls both strings. Unescaped, an ETag containing markup runs
 * arbitrary script on this origin, in an operator's browser, at exactly the
 * moment they are triaging an incident.
 */
function esc(v) {
  return String(v ?? '')
    .replaceAll('&', '&amp;')
    .replaceAll('<', '&lt;')
    .replaceAll('>', '&gt;')
    .replaceAll('"', '&quot;')
    .replaceAll("'", '&#39;');
}

const mins = (iso) => (iso ? (Date.now() - Date.parse(iso)) / 60000 : null);

function ago(m) {
  if (m === null || !isFinite(m)) return 'never';
  if (m < 1) return 'just now';
  if (m < 90) return Math.round(m) + ' min ago';
  const h = m / 60;
  if (h < 36) return h.toFixed(1) + ' hours ago';
  return Math.round(h / 24) + ' days ago';
}

/** Spell a threshold the way a person would say it, from the constant itself. */
function spell(m) {
  if (m < 90) return m + ' minutes';
  const h = m / 60;
  return (Number.isInteger(h) ? h : h.toFixed(1)) + ' hours';
}

const FAULT = {
  origin: {
    tone: 'var(--amber)',
    title: 'kiwisdr.com is not answering us',
    body: 'The origin refused the connection or replied with something we would not publish. ' +
          'Nothing to fix here - the mirror keeps serving the last good list. It clears itself when the origin comes back.',
  },
  auth: {
    tone: 'var(--amber)',
    title: 'The shared secret is missing or no longer matches',
    body: 'Either no secret is configured on the Worker, or we are getting the click-through gate ' +
          'page instead of the directory. Set it with: wrangler secret put KIWI_SECRET. Needs a person either way.',
  },
  'upstream-change': {
    tone: 'var(--amber)',
    title: 'The source moved',
    body: 'kiwisdr.com answered with a redirect. We deliberately do not follow it, because silently ' +
          'mirroring something else would change the arrangement we made with the operator.',
  },
  'needs-review': {
    tone: 'var(--bad)',
    title: 'The list shrank sharply - someone has to decide',
    body: 'kiwisdr.com answered normally, with far fewer receivers than the list we ' +
          'last published. Nothing is broken, and this one does NOT clear on its own: ' +
          'the mirror keeps serving the old list until a person confirms the drop is ' +
          'real. If it is, raise KIWI_MIN_ENTRIES_FRACTION or clear the stored count.',
  },
  mirror: {
    tone: 'var(--bad)',
    title: 'This one is ours',
    body: 'The failure is in our Worker, its configuration or its storage - not on kiwisdr.com. ' +
          'If the reason is auth-misconfigured, the deploy is half-set-up: the auth transport is a ' +
          'Worker secret like the value, so it needs wrangler secret put KIWI_AUTH_MODE and ' +
          'KIWI_AUTH_NAME as well as KIWI_SECRET. Otherwise run wrangler tail kiwi-directory-mirror.',
  },
};

function verdictFor(h, s) {
  const runAge = mins(h && h.last_run_at);
  // last_success_at, NOT status.fetched_at. fetched_at is written only in the
  // publish branch of runPoll, so it is the age of the last PUBLISH; a 304
  // never rewrites it. Reading it here made four healthy 304s (240 min) turn
  // the page amber with "the list is old" while nothing was wrong, and made
  // contentAge identical to publishAge by construction — so the frozen-mirror
  // branch below, gated at 1440, could never be reached. last_success_at is
  // set on every non-failed poll, 304s included, which is exactly "the origin
  // confirmed our copy is current".
  const contentAge = mins(h && h.last_success_at);

  if (!h) return { tone: 'var(--unknown)', text: 'Cannot read the health record', fault: null };

  if (runAge === null || runAge > RUN_STALE_MIN)
    return {
      tone: 'var(--bad)', text: 'Nothing is running',
      fault: { tone: 'var(--bad)', title: 'No poll has happened in over ' + spell(RUN_STALE_MIN),
               body: 'The scheduler stopped. Either the Cloudflare cron is not firing or the Worker is gone. ' +
                     'The published list is whatever it was when the last poll succeeded.' },
    };

  if (h.last_run_result === 'failed')
    return { tone: (FAULT[h.fault] || FAULT.mirror).tone, text: 'Last poll failed', fault: FAULT[h.fault] || FAULT.mirror };

  if (contentAge !== null && contentAge > CONTENT_STALE_MIN)
    return { tone: 'var(--amber)', text: 'Polling, but the list is old', fault: null };

  // The panel has to know the same rule as the alarm, or the two disagree about
  // a frozen mirror. Every other signal here stays green through a permanent 304
  // stream, because a 304 IS a success.
  const publishAge = mins(h.last_publish_at);
  if (publishAge !== null && publishAge > NO_PUBLISH_MIN)
    return {
      tone: 'var(--amber)', text: 'Polling fine, but the list has not changed in a day',
      fault: { tone: 'var(--amber)', title: 'The list is frozen',
               body: 'Every poll is succeeding, but the published list has not actually ' +
                     'changed in over 24 hours. The usual cause is the origin answering ' +
                     '304 to everything. The mirror forces a full fetch after 24 of those, ' +
                     'so this should clear itself - if it does not, look at the ETag.' },
    };

  return { tone: 'var(--good)', text: 'Working', fault: null };
}

$('guide').innerHTML = [
  ['The list is old but polls are still happening, amber.',
   'kiwisdr.com is refusing us or answering with something we would not publish. Nothing to fix on ' +
   'our side. The mirror keeps serving the last good list rather than an empty one, which is the intended behaviour.'],
  ['The list is old but polls are still happening, red.',
   'Our Worker is failing. That one is ours to fix. Check <code>wrangler tail kiwi-directory-mirror</code>.'],
  ['No polls at all for over ' + spell(RUN_STALE_MIN) + '.',
   'Nothing ran. Either the Cloudflare cron stopped firing (it is best-effort on the free plan and has ' +
   'skipped a slot before) or the Worker is gone. At hourly polling that threshold is ' +
   RUN_STALE_POLLS + ' missed polls.'],
  ['Auth.',
   'The shared secret is unset or stopped matching - either it was rotated on kiwisdr.com or we lost ours. ' +
   'This one needs a person either way, so it is deliberately not blamed on a side.'],
].map(([b, t]) => '<li><b>' + b + '</b> ' + t + '</li>').join('');

async function grab(url) {
  try {
    const r = await fetch(url + '?t=' + Date.now(), { cache: 'no-store' });
    return r.ok ? await r.json() : null;
  } catch { return null; }
}

async function render() {
  const [h, s] = await Promise.all([grab(HEALTH), grab(STATUS)]);
  const v = verdictFor(h, s);
  document.documentElement.style.setProperty('--tone', v.tone);
  $('verdict').textContent = v.text;

  const count = (s && s.receiver_count) ?? (h && h.receiver_count);
  $('count').textContent = count == null ? '—' : count.toLocaleString('en-US');
  $('countlab').textContent = count === 1 ? 'receiver in the published list' : 'receivers in the published list';

  // Same reasoning as verdictFor: this line says "Origin confirmed the list",
  // so it has to read the confirmation, not the publish.
  const contentAge = mins(h && h.last_success_at);
  const runAge = mins(h && h.last_run_at);
  const publishAgeMin = mins(s && s.published_at);
  $('sub').textContent = s
    ? 'Checked ' + ago(runAge) + '. Origin confirmed the list ' + ago(contentAge) +
      '. The list itself last changed ' + ago(publishAgeMin) + '.'
    : 'The published list could not be read.';

  // The bar drains toward the point at which the alarm fires, so a glance says
  // how much rope is left rather than just how old the list is.
  const used = contentAge === null ? 1 : Math.min(contentAge / CONTENT_STALE_MIN, 1);
  $('bar').style.width = ((1 - used) * 100).toFixed(1) + '%';
  $('barleft').textContent = contentAge === null ? '' : Math.round(contentAge) + ' min old';
  $('barright').textContent = 'alarm at ' + CONTENT_STALE_MIN + ' min';

  if (v.fault) {
    $('faulth').textContent = v.fault.title;
    $('faultp').textContent = v.fault.body;
    $('fault').hidden = false;
  } else {
    $('fault').hidden = true;
  }

  const recent = (h && h.recent) || [];
  const oldestFirst = recent.slice().reverse();
  // Pad to the full window so the strip is always the same width. SLOTS equals
  // the Worker's HISTORY, so the ghosts only ever appear while a fresh mirror
  // fills its first day - never permanently.
  const ghosts = Math.max(0, SLOTS - oldestFirst.length);
  $('strip').innerHTML =
    '<i data-r="none" title="no poll recorded"></i>'.repeat(ghosts) +
    oldestFirst.map((r) => {
      const t = new Date(r.at).toISOString().slice(11, 16);
      const label = r.result + (r.fault ? ' (' + r.fault + ')' : '') + ' at ' + t + ' UTC';
      return '<i data-r="' + esc(r.result) + '"' + (r.fault ? ' data-f="' + esc(r.fault) + '"' : '') +
             ' title="' + esc(label) + '"></i>';
    }).join('');

  $('rows').innerHTML = recent.map((r) => {
    const t = r.at ? r.at.slice(11, 19) + ' ' + r.at.slice(0, 10) : '';
    const res = r.result === 'failed'
      ? '<span class="tag" data-f="' + esc(r.fault || 'mirror') + '">failed &mdash; ' + esc(r.fault || 'unknown') + '</span>'
      : '<span class="ok">' + esc(r.result) + '</span>';
    // r.detail can carry a redirect Location straight off the wire.
    const detail = r.result === 'failed'
      ? esc([r.reason, r.detail].filter(Boolean).join(' '))
      : (r.receivers ? esc(r.receivers.toLocaleString('en-US')) + ' receivers' : '');
    return '<tr><td class="n">' + esc(t) + '</td><td>' + res + '</td><td>' + detail +
           '</td><td class="n">' + esc(r.attempts ?? '') + '</td></tr>';
  }).join('') || '<tr><td colspan="4">No runs recorded yet.</td></tr>';

  const rows = [
    ['Last poll', h && h.last_run_at],
    ['Last success', h && h.last_success_at],
    ['Last new list', (h && h.last_publish_at) || (s && s.published_at)],
    ['Failures in a row', h && h.consecutive_failures],
    ['kiwisdr.com failures in a row', h && h.consecutive_origin_failures],
    ['Tries on the last poll', h && h.last_run_attempts],
    ['List size', s && s.bytes ? (s.bytes / 1048576).toFixed(2) + ' MB' : null],
    ['Entries skipped', s && s.skipped_entries],
    ['ETag from origin', s ? (s.etag || 'none sent') : null],
    // A long run of "off" while an ETag exists means conditional GET has quietly
    // stopped working and we are pulling the whole file every poll again.
    ['Conditional GET', s ? (s.conditional_get ? 'on' : 'off') : null],
    // Deliberately larger than the alarm above: we want to know the list is
    // going stale well before clients would start treating it as stale.
    ['Client stale threshold', s && s.stale_after_minutes ? s.stale_after_minutes + ' min' : null],
    ['Told to clients', s && s.source_status ? s.source_status : null],
    ['Source', s && s.source],
  ];
  $('readings').innerHTML = rows
    .filter(([, v2]) => v2 !== null && v2 !== undefined)
    // v2 includes s.etag, which is whatever the origin put in the header.
    .map(([k, v2]) => '<dt>' + esc(k) + '</dt><dd>' + esc(v2) + '</dd>')
    .join('');
}

render();
setInterval(render, 60000);
</script>
</body>
</html>`;
