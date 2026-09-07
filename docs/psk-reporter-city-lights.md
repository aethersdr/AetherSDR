# PSK Reporter city lights

Enable **City lights** to overlay NASA/GSFC's historical 2016 VIIRS night-light
composite on the flat map or globe. This is historical imagery, not current
activity or an outage monitor. Some light sources are not cities.

With **Day/night** enabled, lights fade in smoothly between sunset and the end
of civil twilight (solar elevation 0 to -6 degrees). With Day/night disabled,
lights appear worldwide. The **City lights brightness** slider, shown when the
layer is enabled, adjusts intensity from 0 to 100 percent. Visibility defaults
to off; brightness defaults to 70 percent. Both preferences are saved in the
existing `PskReporter` settings object. The layer is independent of weather
radar and its playback clock; twilight always follows current UTC.

Lights appear above night shading, below radar, report paths and station
markers. OSM attribution remains visible alongside NASA attribution. Missing
imagery leaves the existing map usable; a status beside the brightness slider
reports loading and retries. Closing the map or disabling lights stops requests
and the solar refresh timer. Previously decoded imagery remains available for
reuse when the map is reopened.

## Source and rendering contract

- Provider: NASA Global Imagery Browse Services (GIBS), layer
  `VIIRS_Night_Lights`, fixed `TIME=2016-01-01`, PNG, EPSG:3857.
- Public WMS 1.1.1 GetMap endpoint:
  `https://gibs.earthdata.nasa.gov/wms/epsg3857/best/wms.cgi`.
  No account, key, or new package dependency is required.
- Verified against the live GIBS Web Mercator WMTS capabilities and a WMS
  sample on 2026-09-07. The dataset is available for 2012 and 2016; 2016 is
  selected explicitly. `GoogleMapsCompatible_Level8` bounds its native detail.
- Exports are debounced, padded and bounded to 2048 pixels per axis, with
  resolution capped at the layer's zoom-8 Web Mercator pixel size. Crossing
  the dateline uses a canonical full-width export, repeated in the flat map;
  that view can be less detailed than a regional export. The globe omits lights
  beyond the provider's +/-85.051129-degree latitude coverage.
- Both renderers consume the same masked pixels and their immutable,
  north-positive EPSG:3857 bounds. Only the flat renderer reflects the Y axis.
- Although the PNG is a lights-only product, its land background can contain
  opaque black pixels. The display mapping preserves premultiplied RGB and
  replaces alpha with the maximum RGB channel, making black transparent and
  faint lights translucent. Brightness scales the resulting premultiplied
  image once. This is a visual overlay, not a calibrated radiance plot.
- The globe shares the radar implementation's premultiplied texture uploader
  and surface ordering. Its UV coordinates are derived per fragment from the
  geographic surface normal; no lights are projected onto the far hemisphere.
- Download limit: 16 MiB, 15-second timeout, one active HTTP request. PNG
  dimensions are checked before decoding. Failures retain the previous image
  and retry after 30 seconds. The dedicated HTTP disk cache is bounded to
  64 MiB and follows response cache headers. CPU image work uses the existing
  Qt global worker pool, with at most one twilight-mask job plus a coalesced
  follow-up.

References:
- [GIBS available visualizations](https://nasa-gibs.github.io/gibs-api-docs/available-visualizations/)
- [GIBS Web Mercator capabilities](https://gibs.earthdata.nasa.gov/wmts/epsg3857/best/1.0.0/WMTSCapabilities.xml)
- [NASA Earth at Night maps and credits](https://science.nasa.gov/earth/earth-observatory/earth-at-night/maps/)

This feature is stacked on weather radar PR #5477. The optional controls and
rendering choices are subject to maintainer design review before merging.
