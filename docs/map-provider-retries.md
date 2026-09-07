# Map provider retry protection

NASA GIBS and NWS imagery each have a process-wide cooldown, shared by flat
and globe views and by reopened dialogs. HTTP errors (including 429 and 503)
and transport failures pause new requests to that provider for at least
60 seconds. Repeated failed recovery attempts increase the delay to 2, 4, 8,
and 15 minutes, with up to 6 seconds of positive jitter. A valid longer
`Retry-After` delay, in seconds or HTTP-date form, takes precedence.

While cooling down, request attempts receive a local asynchronous failure;
no HTTP request is sent. Existing decoded imagery and playback frames remain
usable. At expiry only one recovery request is admitted. Its uncached success releases
the cooldown; cancellation releases the probe without counting as another
failure. Old in-flight successes cannot erase a more recent failure. Local
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
