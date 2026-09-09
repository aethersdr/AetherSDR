# Map provider retry protection

NASA GIBS and NWS imagery each have a process-wide cooldown, shared by flat
and globe views and by reopened dialogs. HTTP errors (including 429 and 503)
and transport failures pause new requests to that provider for at least
60 seconds. Repeated failed recovery attempts increase the delay to 2, 4, 8,
and 15 minutes, with up to 6 seconds of positive jitter. A valid longer
`Retry-After` delay, in seconds or HTTP-date form, takes precedence up to a
one-day cap (including overflowing numeric values). City-light and radar-history
retry timers wait 66 seconds, covering the initial cooldown's full jitter range.
Live-tile readiness uses a separate short retry clock that panning cannot reset;
the shared network gate still enforces the provider cooldown.

While cooling down, cache-eligible requests may read the HTTP disk cache in
cache-only mode; misses receive a local failure and never start HTTP. Explicit
network-only requests receive a local cooldown error. Local denials do not evict
cached city-light images; invalid image payloads still do. Existing decoded
imagery and playback frames remain usable. At expiry only one recovery request
is admitted. Its uncached success releases the cooldown; cancellation releases
the probe without counting as another failure. Old in-flight successes cannot erase a more recent failure. Late
failures can extend the deadline using their server's bounded `Retry-After`,
but simultaneous failures within a cooldown do not multiply the backoff. Local
cooldown responses do not extend the deadline. Switching projections, panning,
and reopening the map cannot bypass this shared gate.

These controls follow the [NWS appropriate-use guidance](https://www.weather.gov/abusive-user-block)
for one-minute outage retries. NASA also publishes
[GIBS download guidance](https://nasa-gibs.github.io/gibs-api-docs/map-library-usage/).
They reduce retry storms; they are not a guarantee against IP blocking or an
aggregate limit across multiple app processes, machines, or users sharing a
public IP. Cooldowns are in memory and reset on process restart. Normal
successful-request caching and concurrency limits remain unchanged; other
providers (including OSM and KiwiSDR) are unaffected.
