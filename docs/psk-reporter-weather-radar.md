# PSK Reporter weather radar

Enable **Weather radar** in the PSK Reporter map to display NOAA/NWS radar over the
2D map or 3D globe. Coverage follows NOAA's service; it is not a worldwide
radar feed. No account or API key is required.

Use the playback button to loop through original NOAA observations. The
history selector requests 1, 2, or 4 hours, limited by the observations NOAA
currently retains. The **Speed** slider adjusts playback from 0.25× to 5×
without downloading the same frames again. The newest image remains visible
for one second before the loop restarts. Observation times use local time.

Radar visibility, selected history, and speed are saved with PSK Reporter
preferences. Playing/paused state is retained when switching between 2D and
globe during the session.

Downloads run in the background. The loading indicator shows completed/total
frames, and the last usable radar coverage stays visible while new coverage
loads. Zooming may require new, higher-resolution exports; cached data is
reused when it covers the requested view. Failed requests retry in the
background; playback failures show a brief notice, and expired observations leave the
loop as NOAA's catalog advances.

Radar imagery can be delayed or unavailable. Check the displayed observation
time and age during playback. Live exports do not include a verified scan time,
so live mode shows **Age unknown** rather than treating the request time as an
observation. Live loading failures remain visible while replacement imagery
is unavailable. This overlay is not a substitute for official weather warnings.

## Rendering invariants

- Playback presents one original observation at a time, without synthesized
  motion or intermediate storm imagery.
- Image pixels and export bounds are an inseparable cache/render identity.
  Exports use north-positive EPSG:3857 coordinates; only the 2D QGeoView
  boundary reflects the vertical axis. Never apply that reflection twice.
- Retain the displayed texture until its replacement is ready. A loop
  boundary, delayed upload, or failed request must not clear visible coverage.
- Texture bytes are premultiplied before filtering. Use the shared explicit
  upload helper rather than Qt's QImage texture overloads, which convert back
  to straight alpha.

Focused regression coverage lives in the weather_radar source, placement,
wrap-render, loading, and texture tests. Loading tests inject replies without
opening sockets or contacting NOAA. Native GPU cases require explicit opt-in;
an offscreen skip is not native rendering verification.

See [map provider retry protection](map-provider-retries.md) for shared NASA/NWS
cooldowns, `Retry-After` handling, recovery probes, and the limits of this protection.
