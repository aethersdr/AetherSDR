# kiwi-directory-mirror

Mirrors the KiwiSDR public receiver directory into JSON and serves it from
Cloudflare, so AetherSDR clients read our copy instead of the origin.

| URL | What |
| --- | --- |
| `https://cdn.aethersdr.com/kiwi.json` | The receiver list, full fidelity |
| `https://kiwi-status.aethersdr.com/` | Operator status page |
| `https://kiwi-status.aethersdr.com/health.json` | Poll history and fault state |
| `https://kiwi-status.aethersdr.com/status.json` | What was last published |

One Worker serves both hostnames, routing on `Host`. An hourly cron polls the
origin, parses the HTML directory to JSON and writes it to KV.

## Auth

The origin requires a shared secret supplied by the KiwiSDR maintainer. The
mirror exists at his request: AetherSDR clients were putting too much load on
his server individually, so we pull once an hour and redistribute to our own
users.

**How the secret is presented is verified against the live origin, and is not
recorded in this repository.** The transport is configuration
(`KIWI_AUTH_MODE`, `KIWI_AUTH_NAME`), and all three values are Worker secrets:

```sh
wrangler secret put KIWI_SECRET
wrangler secret put KIWI_AUTH_MODE
wrangler secret put KIWI_AUTH_NAME
```

The transport is **not a credential** — knowing it grants nobody access, and the
secret stays a secret regardless. It lives outside the repo because
`aethersdr/AetherSDR` is public and the arrangement with the maintainer is not.
Treat it as need-to-know rather than as something to defend.

There is deliberately **no default** for either. A wrong default in public
source discloses just as confidently as a right one, and silently: the Worker
would poll with a guess and blame the origin for refusing it. Instead a missing
transport is refused before any request, as a `mirror` fault
(`auth-misconfigured`) — our mistake, costing his server nothing.

If the secret is ever unset, the Worker records an `auth` fault and **makes no
origin request at all** — hourly 403s against his server would be rude and would
tell us nothing we do not already know. A missing transport is refused the same
way, as a `mirror` fault, because that one is ours.

The origin serves `content-encoding: gzip`, which the Workers runtime unwraps on
its own. Nothing in this repo needs to decompress anything.

## Conditional GET, and the weak-validator trap

The origin sends a strong ETag and honours `If-None-Match` correctly. But our
`fetch()` decompresses the gzip response on the way in, so Cloudflare weakens the
validator it hands us to `W/"..."` — the bytes we hold are no longer the bytes
the origin hashed.

Sending that weak form straight back matches nothing. The origin compares
strictly, answers `200`, and we republish an identical 800 KB list every single
hour while `not-modified` never once appears in the history. The previous
`aethersdr.ozy.us` mirror had exactly this behaviour.

`originRequest()` therefore strips the `W/` prefix before asking. Two tests lock
it. If you ever see a long run of `published` with an unchanging receiver count
and no `not-modified` in between, look here first.

## The published list

`kiwi.json` carries every field the origin sends, typed where the meaning is
unambiguous and left as a string where it is not:

```json
{
  "schema": 1,
  "source": "https://files.kiwisdr.com/public/",
  "fetched_at": "2026-09-05T14:00:01Z",
  "receiver_count": 870,
  "receivers": [
    {
      "id": "884aeaf59cc6",
      "name": "2-30MHZ SDR #2, VK5ARG Remote Receiver Site | Near Tarlee, South Australia",
      "url": "http://kiwisdr.areg.org.au:8074",
      "gps": [-34.2737, 138.771],
      "bands": [2000000, 30000000],
      "snr": [42, 43],
      "users": 6,
      "users_max": 8,
      "flagged": false
    }
  ]
}
```

870 receivers is about 0.77 MB raw, 126 KB gzipped. Three fields are ours, not
the origin's: `url` (from the entry's anchor), `seq`/`snr_all`/`snr_hf` (from the
entry's CSS classes), and `flagged` (true when the origin paints the row red with
`cl-err-entry` — its own signal that something is wrong with the entry).

## Refusing to publish

Every failure leaves the last good list in place. The one worth knowing about:
if a poll returns fewer than `KIWI_MIN_ENTRIES_FRACTION` (0.5) of the last
published count, the mirror parks on the old list and raises `needs-review`.
That fault **does not clear on its own** — the origin has served truncated pages
before, and publishing one would erase most of the directory for every client.
If the drop is real, raise the fraction or clear `directory:status` in KV.

Conditional GET is used when the origin sends an ETag, with an unconditional
fetch forced after 24 consecutive 304s so a stale stored ETag cannot freeze the
mirror silently.

## Thresholds

The status page and any external alarm must agree, or they will disagree about
the same mirror at the worst moment. Both are expressed as missed polls:

| | Value | Meaning |
| --- | --- | --- |
| `RUN_STALE_MIN` | 150 (2.5 polls) | Nothing is running |
| `CONTENT_STALE_MIN` | 240 (4 polls) | Polling, but the list is old |
| `NO_PUBLISH_MIN` | 1440 | Frozen — succeeding but never changing |
| `stale_after_minutes` | 360 | What clients are told; deliberately looser |

The page derives its own wording from these constants, so changing a number
changes the prose with it.

## Tests

```sh
npm test
```

Nine cases over a real 870-receiver capture, weighted toward the failure paths:
no-secret, 304, truncated page, redirect, gate page, 5xx retry vs 4xx give-up,
and history bounding.

## Still to do

- Retire the `aethersdr.ozy.us` mirror now that this one is publishing. It lives
  in a different Cloudflare account and cannot be reached from here.
- Point `ops/kuma-staleness-monitor.py` at the new `health.json`.
- Update the AetherSDR client to read `cdn.aethersdr.com/kiwi.json` instead of
  fetching the origin directly — that is the whole point of the exercise, and
  until it ships the load the maintainer complained about is still there.
