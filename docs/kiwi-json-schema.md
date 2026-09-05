# `kiwi.json` — schema 1

The reviewable, in-tree contract for the payload AetherSDR's directory mirror
publishes at `https://cdn.aethersdr.com/kiwi.json`. The producer (a Cloudflare
Worker that pulls `kiwisdr.com/public` hourly under a shared secret the KiwiSDR
maintainer provided) lives outside this repository, so this file is what
`KiwiPublicDirectory::kSupportedSchema` actually pins the client to.

**Changing anything below is a schema break.** Bump `schema`, and bump
`kSupportedSchema` in `src/core/KiwiPublicDirectory.h` in the same breath — the
client rejects an unrecognised `schema` outright rather than parsing on
hopefully, so an unannounced field change turns into "please update AetherSDR"
for every user rather than silent misbehaviour.

See `docs/kiwisdr-public-directory.md` for *why* the mirror exists and how
AetherSDR honors each operator's policy.

## Transport

| | |
|---|---|
| URL | `https://cdn.aethersdr.com/kiwi.json` |
| Content-Type | `application/json; charset=utf-8` |
| Cache-Control | `public, max-age=1800` — clients must not poll faster |
| CORS | open (`access-control-allow-origin: *`) |
| `x-receiver-count` | receiver count, mirrors `receiver_count` |
| `x-fetched-at` | mirror pull time, mirrors `fetched_at` |
| Typical size | ~824 KB raw, ~126 KB gzipped, ~870 receivers |

Mirror health, for humans rather than the client: `https://kiwi-status.aethersdr.com/`.

## Envelope

One JSON **object** — not an array. A top-level array is a hard parse failure.

| Field | Type | Required | Notes |
|---|---|---|---|
| `schema` | int | **yes** | `1`. Any other value is refused by the client. |
| `source` | string | no | Origin the mirror pulled, e.g. `https://files.kiwisdr.com/public/` |
| `fetched_at` | string | no | ISO-8601 UTC, e.g. `2026-09-05T14:00:01Z`. Converted with `toUTC()`, so an offset form is handled correctly. Drives the picker's staleness display at 360 minutes. |
| `receiver_count` | int | no | Advisory; the client trusts `receivers.length`. |
| `receivers` | array | **yes** | Objects. See below. |

Client-side bounds (`Principle VII`): body `> 32 MB` or `receivers.length >
20000` is refused, and an empty `receivers` array is reported as an *empty*
directory rather than a malformed one.

## Receiver objects

An entry with no `url` is skipped. Every other field is optional — the client
must never assume presence. Counts below are from the 870-receiver payload of
2026-09-05.

### Fields AetherSDR reads

| Field | Type | Present | Maps to | Notes |
|---|---|---|---|---|
| `url` | string | 870/870 | `url` | `http://host[:port]`. **Required**; may be a `*.proxy.kiwisdr.com` host. |
| `id` | string | 870/870 | `id` | Origin's stable id, e.g. `884aeaf59cc6` |
| `name` | string | 870/870 | `name` | Sysop description |
| `loc` | string | 870/870 | `location` | Free text |
| `antenna` | string | 870/870 | `antenna` | Free text |
| `sdr_hw` | string | 870/870 | `sdrHw` | Free text; a whitespace-delimited `Limits` **token** drives `advertisesConnectionLimit()` |
| `grid` | string | 855/870 | `grid` | Maidenhead. **Optional.** |
| `users` | int | 870/870 | `users` | Clamped to `>= 0` |
| `users_max` | int | 870/870 | `usersMax` | Clamped to `>= 0`; denominator of the policy classification |
| **`ext_api`** | int | **847/870** | `extApi` | **See below — the load-bearing field.** |
| `offline` | bool | 870/870 | `offline` | Real JSON bool. The string `"yes"`/`"true"` is still accepted for older snapshots. |
| `flagged` | bool | 870/870 | `flagged` | Origin marks the entry as bad; hidden by the picker (30/870) |
| `gps` | `[lat, lon]` | 870/870 | `gpsLat`/`gpsLon`/`hasGps` | Numbers. `[0, 0]` means *no fix*, not null island. Out-of-range values leave `hasGps` false. |
| `bands` | `[low, high]` | 870/870 | `bandLowHz`/`bandHighHz`/`hasBands` | Hz. `high <= low` leaves `hasBands` false. |
| `snr` | `[all, hf]` | 870/870 | `snrAll`/`snrHf` | dB. Falls back to the scalar `snr_all`/`snr_hf` keys. |
| `snr_all` | int | 870/870 | `snrAll` | Fallback for `snr[0]` |
| `snr_hf` | int | 870/870 | `snrHf` | Fallback for `snr[1]` |

Strings are truncated at 512 characters on the way in.

### `ext_api` — the field this contract exists for

`ext_api` is the operator's external-API allowance: the maximum number of
channels open to non-browser API clients. AetherSDR honors it *before* offering
a connection.

| Value | `apiPolicy()` | Offered? |
|---|---|---|
| `0` | `Disabled` — operator wants web-browser use only | no |
| `1 … users_max-1` | `Limited` | yes |
| `>= users_max` | `Open` | yes |
| **key absent** | `Unknown` — policy not published | no |

**A receiver that does not publish a policy omits the key entirely** — 23 of 870
today. The producer must keep it that way: emitting `"ext_api": 0` for those
receivers would tell every client that 23 operators *disabled* their API when in
fact they said nothing, and emitting `null` or a string would land them in
`Unknown` by a different route than the one that is tested.

On the client side this is why the parser reads
`extApiVal.isDouble() ? extApiVal.toInt(-1) : -1` rather than `toInt()`, and why
`tests/kiwi_public_directory_test.cpp` asserts on `extApi == -1` specifically.

### Fields the origin publishes that AetherSDR ignores

Present in all 870 entries and safe for the producer to keep emitting; the
client neither reads nor validates them, so adding to this set is **not** a
schema break:

`adc_ov`, `ant_connected`, `asl`, `avatar_ctime`, `clk_ext_freq`,
`clk_ext_gps`, `date`, `debian`, `dx_file`, `fixes`, `fixes_hour`, `fixes_min`,
`freq_offset`, `gps_date`, `gps_good`, `ip_blacklist`, `preempt`, `seq`,
`status`, `sw_version`, `tdoa_ch`, `tdoa_id`, `updated`, `uptime`

Also optional and unread: `mode` (847/870), `sm_cal` / `wf_cal` (845/870).

## Compatibility rules for the producer

1. **Adding an ignored field** — safe, no bump.
2. **Removing or retyping a field in "Fields AetherSDR reads"** — schema break.
3. **Emitting `ext_api: 0` where the key used to be absent** — schema break, and
   the most damaging one available: it silently misreports operator policy.
4. **Changing `gps`/`bands`/`snr` from pairs to anything else** — schema break.
5. **Changing `offline`/`flagged` away from JSON bools** — schema break.

## Example

```json
{
  "schema": 1,
  "source": "https://files.kiwisdr.com/public/",
  "fetched_at": "2026-09-05T14:00:01Z",
  "receiver_count": 870,
  "receivers": [
    {
      "id": "884aeaf59cc6",
      "url": "http://kiwisdr.areg.org.au:8074",
      "name": "2-30MHZ SDR #2, VK5ARG Remote Receiver Site",
      "loc": "Near Tarlee, South Australia",
      "antenna": "Broadband Monopole (\"J-Dart\")",
      "sdr_hw": "KiwiSDR 1 v1.902 Limits",
      "grid": "PF95JR",
      "gps": [-34.2737, 138.771],
      "bands": [2000000, 30000000],
      "snr": [42, 43],
      "users": 6,
      "users_max": 8,
      "ext_api": 4,
      "offline": false,
      "flagged": false
    }
  ]
}
```
