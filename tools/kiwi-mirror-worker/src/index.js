/**
 * KiwiSDR public directory mirror.
 *
 * A cron pulls kiwisdr.com's public receiver directory, parses it to JSON and
 * publishes it so AetherSDR clients read our copy instead of the origin. Two
 * hostnames are served from this one Worker:
 *
 *   cdn.aethersdr.com/kiwi.json      the receiver list
 *   kiwi-status.aethersdr.com/       the operator status page (+ its two JSONs)
 *
 * The guiding rule throughout: never replace a good list with a bad one. Every
 * failure path leaves the last known-good payload in place and records why, so
 * a broken origin degrades to "stale but serving" rather than "empty".
 */

import { parseDirectory } from './parse.js';
import { STATUS_PAGE } from './status-page.js';

const KEY_PAYLOAD = 'directory:payload';
const KEY_STATUS = 'directory:status';
const KEY_HEALTH = 'mirror:health';

/** How many poll results the status page gets to draw. */
const HISTORY = 20;

/** Give up on a single poll after this many attempts. */
const MAX_ATTEMPTS = 3;

/**
 * A run of 304s this long forces an unconditional fetch. If our stored ETag
 * ever stops matching reality, conditional GET would otherwise freeze the
 * mirror silently and every signal would stay green.
 */
const FORCE_FULL_AFTER_304 = 24;

const json = (body, extra = {}) =>
  new Response(JSON.stringify(body, null, 2), {
    headers: {
      'content-type': 'application/json; charset=utf-8',
      'access-control-allow-origin': '*',
      ...extra,
    },
  });

const notFound = () =>
  new Response('Not found\n', { status: 404, headers: { 'content-type': 'text/plain; charset=utf-8' } });

/* ── origin ──────────────────────────────────────────────────────────────── */

/**
 * Build the origin request. The directory is behind a shared secret agreed with
 * the KiwiSDR operator; how that secret is presented is configuration, not a
 * constant, so it can be corrected without a code change if the arrangement
 * changes.
 *
 * Mode and name are Worker secrets like the value itself — not because the
 * transport is a credential, but because this repo is public and the
 * arrangement is not. There is deliberately no default: a wrong default in
 * public source is its own kind of disclosure, and a silent one. pollOrigin()
 * rejects a missing transport before we ever reach here.
 */
/** The transports originRequest() knows how to speak. */
const AUTH_MODES = new Set(['query', 'header', 'cookie', 'bearer']);

function originRequest(env, etag) {
  const url = new URL(env.KIWI_SOURCE_URL);
  const headers = { 'user-agent': env.KIWI_USER_AGENT };

  const mode = env.KIWI_AUTH_MODE;
  const name = env.KIWI_AUTH_NAME;
  if (mode === 'query') url.searchParams.set(name, env.KIWI_SECRET);
  else if (mode === 'header') headers[name] = env.KIWI_SECRET;
  else if (mode === 'cookie') headers.cookie = `${name}=${env.KIWI_SECRET}`;
  else if (mode === 'bearer') headers.authorization = `Bearer ${env.KIWI_SECRET}`;
  else throw new Error(`unknown KIWI_AUTH_MODE: ${mode}`);

  // Strip the weak-validator prefix before asking. The origin sends a strong
  // ETag, but our own fetch decompresses the gzip response on the way in and
  // Cloudflare weakens the validator to W/"..." to reflect that the bytes we
  // hold are no longer the bytes it hashed. Sending that back never matches:
  // the origin compares strictly, answers 200, and we republish an identical
  // 800 KB list every hour while `not-modified` stays dead code.
  if (etag) headers['if-none-match'] = etag.replace(/^W\//, '');

  // redirect:manual — following a redirect would silently mirror something
  // other than what we agreed to mirror.
  return new Request(url, { headers, redirect: 'manual' });
}

/**
 * The origin answers a click-through gate page instead of the directory when
 * the secret is wrong. It returns 200, so only the body distinguishes it.
 */
function looksLikeGate(html) {
  return !html.includes("class='cl-entry") && /click|agree|accept|continue/i.test(html.slice(0, 4000));
}

async function pollOrigin(env, state) {
  if (!env.KIWI_SECRET) {
    // Deliberately no request: hammering a third party with hourly 403s while
    // the secret is unset would be rude and tells us nothing we don't know.
    return { result: 'failed', reason: 'no-secret', fault: 'auth', attempts: 0 };
  }

  // Membership, not merely non-empty: a typo'd or stale mode would otherwise
  // reach originRequest()'s `else throw`, which runs INSIDE the retry loop's
  // try, so it lands in the catch as fault 'origin' and gets repeated three
  // times. The operator page would then say kiwisdr.com is not answering and
  // that it clears itself when the origin comes back — both false, and the
  // fault taxonomy is the whole reason that page exists. With this check the
  // `else throw` is genuinely unreachable, which is the right shape for it.
  if (!AUTH_MODES.has(env.KIWI_AUTH_MODE) || !env.KIWI_AUTH_NAME) {
    // A half-configured deploy is OUR mistake, so it must not be reported as
    // an origin failure and must not burn three retries against his server
    // discovering that we forgot to finish setting ourselves up.
    return { result: 'failed', reason: 'auth-misconfigured', fault: 'mirror', attempts: 0 };
  }

  const force = (state.consecutive_not_modified || 0) >= FORCE_FULL_AFTER_304;
  const etag = force ? null : state.etag;
  let lastError = null;

  for (let attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {
    try {
      const res = await fetch(originRequest(env, etag));

      if (res.status === 304) return { result: 'not-modified', attempts: attempt };

      if (res.status >= 300 && res.status < 400) {
        return {
          result: 'failed', reason: 'redirect', fault: 'upstream-change',
          detail: res.headers.get('location') || String(res.status), attempts: attempt,
        };
      }

      if (!res.ok) {
        lastError = { reason: 'bad-status', fault: 'origin', detail: String(res.status) };
        // 4xx is a decision, not a hiccup; retrying will not change it.
        if (res.status < 500) break;
        continue;
      }

      const html = await res.text();
      if (looksLikeGate(html)) {
        return { result: 'failed', reason: 'gate-page', fault: 'auth', attempts: attempt };
      }

      return {
        result: 'fetched', attempts: attempt, html,
        etag: res.headers.get('etag'),
        conditional_get: Boolean(res.headers.get('etag')),
        forced_full: force,
      };
    } catch (err) {
      lastError = { reason: 'fetch-failed', fault: 'origin', detail: String(err && err.message || err).slice(0, 200) };
    }
  }

  return { result: 'failed', attempts: MAX_ATTEMPTS, ...(lastError || { reason: 'unknown', fault: 'mirror' }) };
}

/* ── the poll ────────────────────────────────────────────────────────────── */

export async function runPoll(env, now = new Date()) {
  const at = now.toISOString();
  const health = (await env.KIWI.get(KEY_HEALTH, 'json')) || {
    schema: 1, recent: [], consecutive_failures: 0, consecutive_origin_failures: 0,
  };
  const prevStatus = (await env.KIWI.get(KEY_STATUS, 'json')) || {};
  const state = {
    etag: prevStatus.etag || null,
    consecutive_not_modified: health.consecutive_not_modified || 0,
  };

  const poll = await pollOrigin(env, state);
  let entry;

  if (poll.result === 'failed') {
    entry = { at, result: 'failed', reason: poll.reason, detail: poll.detail, fault: poll.fault, attempts: poll.attempts };
  } else if (poll.result === 'not-modified') {
    entry = { at, result: 'not-modified', attempts: poll.attempts, receivers: prevStatus.receiver_count };
    health.consecutive_not_modified = (health.consecutive_not_modified || 0) + 1;
  } else {
    const { receivers, skipped } = parseDirectory(poll.html);

    if (receivers.length === 0) {
      entry = { at, result: 'failed', reason: 'no-entries', fault: 'origin', attempts: poll.attempts };
    } else {
      // The origin occasionally serves a truncated page. Publishing it would
      // erase most of the directory for every client, so a sharp drop parks the
      // mirror on the old list until a person agrees the drop is real.
      const floor = Number(env.KIWI_MIN_ENTRIES_FRACTION || '0.5');
      const previous = prevStatus.receiver_count || 0;
      if (previous && receivers.length < previous * floor) {
        entry = {
          at, result: 'failed', reason: 'too-few-entries', fault: 'needs-review',
          detail: `${receivers.length} vs ${previous}`, attempts: poll.attempts,
        };
      } else {
        const payload = JSON.stringify({
          schema: 1,
          source: env.KIWI_SOURCE_URL,
          fetched_at: at,
          receiver_count: receivers.length,
          receivers,
        });

        const status = {
          schema: 1,
          source: env.KIWI_SOURCE_URL,
          fetched_at: at,
          published_at: at,
          receiver_count: receivers.length,
          skipped_entries: skipped,
          bytes: payload.length,
          etag: poll.etag,
          not_modified: false,
          source_status: 'ok',
          stale_after_minutes: Number(env.KIWI_STALE_AFTER_MINUTES || '360'),
          conditional_get: poll.conditional_get,
          forced_full: poll.forced_full || false,
          mirror: 'cloudflare-worker/kiwi-directory-mirror',
        };

        await env.KIWI.put(KEY_PAYLOAD, payload);
        await env.KIWI.put(KEY_STATUS, JSON.stringify(status));

        entry = { at, result: 'published', attempts: poll.attempts, receivers: receivers.length };
        health.consecutive_not_modified = 0;
        health.last_publish_at = at;
      }
    }
  }

  const failed = entry.result === 'failed';
  health.schema = 1;
  health.last_run_at = at;
  health.last_run_result = entry.result;
  health.last_run_reason = entry.reason ?? null;
  health.last_run_detail = entry.detail ?? null;
  health.last_run_attempts = entry.attempts;
  health.fault = entry.fault ?? null;
  health.consecutive_failures = failed ? (health.consecutive_failures || 0) + 1 : 0;
  health.consecutive_origin_failures =
    failed && entry.fault !== 'mirror' ? (health.consecutive_origin_failures || 0) + 1 : 0;
  if (!failed) health.last_success_at = at;
  health.receiver_count = (await env.KIWI.get(KEY_STATUS, 'json'))?.receiver_count ?? health.receiver_count ?? null;
  health.recent = [entry, ...(health.recent || [])].slice(0, HISTORY);

  await env.KIWI.put(KEY_HEALTH, JSON.stringify(health));
  return health;
}

/* ── serving ─────────────────────────────────────────────────────────────── */

async function serve(request, env) {
  const url = new URL(request.url);
  const host = url.hostname;
  const path = url.pathname.replace(/\/+$/, '') || '/';

  if (request.method !== 'GET' && request.method !== 'HEAD') {
    return new Response('Method not allowed\n', { status: 405, headers: { allow: 'GET, HEAD' } });
  }

  // The CDN host serves exactly one thing.
  if (host === env.KIWI_CDN_HOST) {
    if (path !== '/kiwi.json') return notFound();
    const payload = await env.KIWI.get(KEY_PAYLOAD);
    if (!payload) {
      return json({ error: 'no list published yet' }, { 'cache-control': 'no-store' });
    }
    const status = (await env.KIWI.get(KEY_STATUS, 'json')) || {};
    return new Response(payload, {
      headers: {
        'content-type': 'application/json; charset=utf-8',
        'access-control-allow-origin': '*',
        // Half the poll interval: a client refetching on expiry never sits more
        // than one cycle behind, and the origin sees no extra load either way.
        'cache-control': 'public, max-age=1800',
        'x-receiver-count': String(status.receiver_count ?? ''),
        'x-fetched-at': status.fetched_at ?? '',
      },
    });
  }

  if (host === env.KIWI_STATUS_HOST) {
    if (path === '/' || path === '/index.html' || path === '/status.html') {
      return new Response(STATUS_PAGE, {
        headers: { 'content-type': 'text/html; charset=utf-8', 'cache-control': 'no-cache' },
      });
    }
    if (path === '/health.json') {
      return json((await env.KIWI.get(KEY_HEALTH, 'json')) || { error: 'no health record yet' },
        { 'cache-control': 'no-store' });
    }
    if (path === '/status.json') {
      return json((await env.KIWI.get(KEY_STATUS, 'json')) || { error: 'nothing published yet' },
        { 'cache-control': 'no-store' });
    }
    return notFound();
  }

  return notFound();
}

export default {
  fetch: (request, env) => serve(request, env),
  scheduled: (event, env, ctx) => ctx.waitUntil(runPoll(env, new Date(event.scheduledTime))),
};
