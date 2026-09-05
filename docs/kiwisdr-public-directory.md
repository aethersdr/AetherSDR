# KiwiSDR public directory — honest, API-policy-aware access

AetherSDR can populate a picker of public KiwiSDR receivers. The list comes
from **AetherSDR's own mirror**, `https://cdn.aethersdr.com/kiwi.json`. Because
AetherSDR connects to a receiver via its native (WebSocket "external API")
protocol, it must respect each operator's choice about whether that API is
allowed — *before* offering or attempting a connection.

## Where the list comes from, and why

**The KiwiSDR maintainer asked us to mirror it.** AetherSDR clients were each
fetching `kiwisdr.com/public` for themselves, and the aggregate load on his
server was a problem. The mirror is his requested remedy: we pull once, cache,
and redistribute to our own users, so his server sees **one request per hour**
instead of hundreds of ad-hoc ones.

A Cloudflare Worker pulls the origin hourly under a shared secret he provided,
parses it into JSON, and publishes it:

| URL | What |
|---|---|
| `https://cdn.aethersdr.com/kiwi.json` | the receiver list (`schema: 1`) |
| `https://kiwi-status.aethersdr.com/` | mirror health, for humans |

`kiwi.json` is one object — `schema`, `source`, `fetched_at`, `receiver_count`
and a `receivers` array — served with `cache-control: public, max-age=1800`.

**There is deliberately no fallback to `kiwisdr.com`.** Keeping the old
HTML-scraping path as a CDN-outage fallback would look free, but it would mean
that the moment our CDN has a bad day, every AetherSDR install in the world
reverts to hitting his origin at once — recreating exactly the load he asked us
to remove, at the least convenient possible moment. When the mirror is
unreachable, AetherSDR shows the list it already has, or says plainly that the
directory is unavailable.

## The operator signal: `ext_api`

Every receiver entry publishes an `ext_api` value (it is also in each receiver's
`/status`). It is the operator's **external-API allowance** — the maximum number
of channels open to non-browser API clients:

| `ext_api` | meaning |
|---|---|
| `0` | **External API disabled** — operator wants web-browser use only |
| `1 … users_max-1` | API allowed, but some channels reserved for web users |
| `>= users_max` | all channels open to API |
| *key absent* | **policy not published** — not the same as `0` |

That last row is load-bearing. In `kiwi.json` a receiver that does not publish a
policy has **no `ext_api` key at all** (23 of ~870 entries today). Reading it as
`obj["ext_api"].toInt()` would yield `0` and silently reclassify every one of
those operators as having *disabled* the API. That fails safe for the connection
decision, but it misreports the operator's policy in the badge and in the
picker's hidden-receiver counts — the thing this code exists to get right. The
parser reads it as `-1` unless the key is actually present, and
`tests/kiwi_public_directory_test.cpp` asserts on `extApi == -1` specifically
(asserting only on `mayConnectViaApi()` would pass with the bug in place).

`src/core/KiwiPublicDirectory.{h,cpp}` exposes this as
`KiwiPublicReceiver::apiPolicy()` and the honor predicate `mayConnectViaApi()`
(`ext_api > 0`).

## How AetherSDR honors it

- **The receiver picker shows only API-permitted receivers.** Receivers with
  `ext_api == 0` are **filtered out entirely** — they are never presented to the
  user and AetherSDR never attempts a native connection to them. Receivers that
  publish no policy are likewise not offered, and are counted separately in the
  picker's status line. Entries the origin itself has `flagged` are also hidden.
- AetherSDR identifies **honestly** with an `AetherSDR/<version>` User-Agent and
  never spoofs a browser. With the mirror in place there is no interactive gate
  to pass and no one-time token to replay — the client makes one plain `GET`
  against our own CDN.
- **Clients never contact `kiwisdr.com`.** Only AetherSDR's mirror and, once the
  user chooses a receiver, that receiver itself.
- **Refresh respects the mirror's own 30-minute `max-age`.** Opening the picker
  re-serves the session's cached list while it is inside that window and fetches
  once it is past; "Refresh list" always fetches. We never poll faster than the
  data can actually change. The picker surfaces the list's age once it exceeds
  the mirror's 360-minute staleness threshold.
- The client validates the mirror's response at the boundary
  ([Principle VII](../CONSTITUTION.md#vii-untrusted-input-is-validated-at-the-boundary)):
  the body size and receiver count are capped, a `schema` other than `1` is
  rejected with a "please update AetherSDR" message rather than parsed
  hopefully, and a malformed body fails closed instead of yielding a partial
  list.
- Only server-published data is read — never the KiwiSDR source (clean-room,
  [Principle IV](../CONSTITUTION.md#iv-every-contribution-is-clean-room)).

## Proof of concept

`tools/kiwi_directory_poc.cpp` (target `kiwi_directory_poc`) fetches the mirror
and prints the per-operator policy breakdown — including the policy-unknown
count as its own category — and the honor decision for each `ext_api == 0`
receiver. `tests/kiwi_public_directory_test.cpp` locks the parser + policy logic
across all four `ext_api` regimes, and locks the boundary failures.

```
$ kiwi_directory_poc                 # fetch from the AetherSDR mirror
$ kiwi_directory_poc kiwi.json       # offline parse of a saved payload
```
