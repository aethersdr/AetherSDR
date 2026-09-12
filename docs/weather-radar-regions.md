# Regional weather radar

The PSK Reporter map uses LibreWXR as its free primary precipitation source,
with NOAA, ECCC and OPERA as regional backups. The master weather overlay is
opt-in. The primary and each backup have independent checkboxes; disabling the
primary restores the combined direct regional radar display. Existing regional
preferences are preserved. `AppSettings["PskReporter"]` owns `useLibreWxr`
(default true), `weatherRadarRegions` (regional bit mask 1/2/4) and
`showRadarCoverage` (default false). No account, subscription, server or API key
is required. Runtime/cache masks add bit 8 for the primary.

## Global primary: LibreWXR

The [public LibreWXR service](https://librewxr.net/terms.html) permits ordinary
interactive application use, including animation, free of charge. It is shared
community infrastructure with no availability guarantee. Requests use an
identifying AetherSDR User-Agent, four simultaneous tile downloads at most,
shared in-flight requests, local caching and provider-wide outage backoff.
There is no full-archive mirroring or background whole-world prefetch.

**This is mixed precipitation data, not worldwide ground radar.** Its public
raster API combines regional radar with satellite-estimated and modelled
precipitation. Neither the documented request parameters nor the inspected
upstream route provide a radar-only/source filter. The sidebar's Sources & licenses
panel identifies this mixture. The optional site shading continues to use our separate
NOAA/OPERA station catalogs; LibreWXR's coverage endpoint is not used because its
inspected implementation masks nonzero precipitation rather than instrument range.

Metadata comes from `https://api.librewxr.net/public/weather-maps.json`.
Only `radar.past` is admitted: exact host/path validation, bounded JSON/frame
counts, increasing integral UTC epochs, no future frames, and a latest frame
no older than 30 minutes. Global frame clocks are composite timestamps; individual
radars and estimates may have different observation ages. Metadata is cached for
five minutes. The currently advertised history is two hours at ten-minute steps;
selecting four hours does not fabricate extra global observations. Nowcast frames
are excluded.

Playback pins the source set returned with its initial catalog: global-only
when the primary is available, otherwise the enabled regional feeds. Zooming
and background catalog refreshes preserve that selection. A failed global
frame is not replaced by a regional-only image: an existing original stays
visible during a failed detail refresh, and unavailable new observations are
skipped. If global images cannot start a movie despite a healthy catalog, the
entire movie switches to the enabled regional feeds before playback starts.
Stop and restart playback to choose sources again. Live viewing still
uses per-request regional fallbacks.

Globe exports share the native renderer's four-megapixel budget. Viewport
dimensions are bounded before caching decisions; optional padding is omitted
when it would exceed the budget, preserving the visible image's resolution.
Coverage shading uses cached three-dimensional geometry in the globe's own
render pass. Overlapping coverage is shaded once, and rotation does not rebuild
or upload the site geometry.

Canonical frame tiles use `/v2/radar/{epoch}/512/{z}/{x}/{y}/6/0_0.png`.
The app stitches Web Mercator tiles into the existing flat/globe image exports,
including date-line wrapping, with smooth downsampling. Flat-map tiles for all providers
carry 512 by 512 pixels, and the globe uses a 2048 by 2048 radar atlas for
both live and whole-world playback, including in smaller windows.
A 2048-square overview retains its display resolution with 16 source tiles.
Exports contain at most 32 tiles; source zoom is capped at 8 and output remains
capped at four megapixels. Millimetre rounding in export bounds is snapped at
XYZ boundaries to avoid requesting almost-empty neighbouring tiles.

Individual PNG inputs are capped at 2 MiB and checked for exactly 512 by 512
pixels before decoding. The decoded tile cache is 64 MiB; the persistent disk
cache is 256 MiB. Validated timestamped tiles are saved as local frame snapshots
until five hours after their timestamp, subject to cache eviction. That includes
transparent clear-weather tiles. A provider may revise an already published
frame: a cached snapshot deliberately remains stable for playback; discovery
still checks for new frame timestamps every five minutes. Invalid, failed,
canceled and expired tiles are not reused.

Cache keys include timestamp, tile coordinates, resolution and rendering options.
On a miss, the renderer can build an overview tile from four cached children
(up to two zoom levels), including disk-cached children after an app restart.
These derived tiles are also cached. It never enlarges a cached overview instead
of fetching genuinely finer detail. Concurrent consumers share downloads;
canceling one view only stops a download after its last consumer retires. Cache
budgets are bounded, so revisiting evicted imagery can require a new request.

A valid primary image, including completely transparent clear-weather pixels,
finishes the request without contacting regional backups. Catalog, transfer or
image-validation failure triggers enabled regional backups for a live export;
playback uses the fixed source selection described above.
Historical backups select original observations at or before the requested clock
with the existing ten-minute tolerance. Outside backup coverage, an outage stays
unavailable. A primary HTTP 200 cannot reveal an upstream agency outage hidden
inside LibreWXR's composite; the client does not claim to detect those outages.

### Credits and licensing

Weather data via LibreWXR (librewxr.net). Source credits: NOAA/NCEP/NESDIS, Iowa
Environmental Mesonet, ECCC/MSC, EUMETNET OPERA, Radar-DPC, MARN/SNET, CWA, JMA,
MET Malaysia and PAGASA. Model credits include NOAA, ECCC, DMI, DWD, Météo-France,
SMN, JMA and ECMWF via Open-Meteo. The UI includes these credits in the sources
link tooltip and links to the current provider list.

The public API declares CC BY 4.0 except stricter contributing terms; tiles that
include Italian Radar-DPC data carry CC BY-SA 4.0. Redistributed derivatives of
those tiles must retain ShareAlike and Radar-DPC credit. Consult the
[current terms](https://librewxr.net/terms.html) and
[source list](https://github.com/JoshuaKimsey/LibreWXR#data-sources).
This integration consumes the API; it does not incorporate LibreWXR's AGPL server
code or deploy that server.

## Sources and attribution

| Region | Public source | Measurement | Timing |
| --- | --- | --- | --- |
| US regions | [NOAA/NWS radar services](https://mapservices.weather.noaa.gov/eventdriven/rest/services/radar) | Base reflectivity, dBZ | Existing live exports and observation catalog |
| Canada / North America | [ECCC GeoMet](https://eccc-msc.github.io/open-data/msc-geomet/readme_en/) WMS `RADAR_1KM_RRAI` | Rainfall rate, mm/h | Six-minute scans; three-hour catalog |
| Europe | [EUMETNET OPERA Open Radar Data](https://eumetnet.github.io/openradardata-documentation/2-ORD-API-discovering-and-accessing-data/) CIRRUS `DBZH` GeoTIFF | Maximum reflectivity, dBZ | Five-minute scans; four hours queried from the public 24-hour store |

Direct regional HTTP images also use a 256 MiB disk cache and prefer fresh
cached responses; OPERA retains its separate 512 MiB source-raster disk cache
and now prefers cached timestamped GeoTIFFs before contacting the server.
OPERA exports use smooth downsampling. Existing playback caching reuses a
same-observation image that covers the new view at sufficient resolution.

These backup products do not provide complete worldwide ground-radar coverage. Large areas of
Asia, Africa, South America and Oceania have no imagery from these adapters.
A blank area does not establish clear weather. Additional countries require
additional authorized public feeds and provider adapters. No Zoom Earth imagery,
private endpoint, or credentials are used.

Weather, radar-site and NASA/GSFC city-light credits are consolidated in the
sidebar's expandable Sources & licenses panel, collapsed by default. The small
linked OpenStreetMap credit remains visible on both maps, as required by the
[OSM tile policy](https://operations.osmfoundation.org/policies/tiles/).
Provider outage status remains visible outside the collapsed details.
OPERA composite data is CC BY 4.0. ECCC data
uses the [Open Government Licence – Canada](https://open.canada.ca/en/open-government-licence-canada)
and [MSC data usage guidance](https://eccc-msc.github.io/open-data/licence/readme_en/).
Provider measurement names are available in Sources & licenses. Data from different measurements is
never numerically averaged: the returned color rasters are overlaid, with NOAA
pixels taking precedence over ECCC pixels where the North American products
overlap. Their palettes and units are not interchangeable.

## Native European processing

OPERA discovery uses the documented anonymous S3 listing interface at
`s3.waw3-1.cloudferro.com/openradar-24h`, with a date/OPERA/COMP prefix and a
`start-after` bound. Two date prefixes handle UTC midnight. Listings are capped
at 1,000 keys / 512 KiB; truncated or malformed catalogs fail closed. Only exact
OPERA DBZH GeoTIFF keys on the approved host are accepted. The MeteoGate discovery
API is not required; anonymous API rate limits were observed during integration.

`OperaRadarImage` reads the current bounded CIRRUS GeoTIFF profile: classic TIFF,
3,800 × 4,400 pixels, tiled 512 × 512, two Float32 bands, interleaved samples,
Deflate without predictor. Only reflectivity is rendered; NaN/undetect and
no-data remain transparent. Compressed input is limited to 16 MiB and each tile
decompresses into a fixed-size buffer. Unsupported profiles are rejected rather
than guessed. WGS84 ellipsoidal LAEA georeferencing is checked before conversion
to a 2,560 × 3,072 Web Mercator display raster. This display reprojection is
coarser than the underlying 1 km measurement grid.

Decoding runs through QtConcurrent, one frame at a time. The adapter uses the
project's existing zlib, supporting both bundled and system-zlib builds. There
is no GDAL, libtiff, Python, or hosted processing dependency in the app.
The dBZ palette progresses from blue at −30 through cyan, green, yellow and red,
then fades to white at 80 dBZ. `colorForDbz` is the numeric palette definition.

European caches are bounded to 70 MiB for projected images, 128 MiB for compressed
files in memory, and 512 MiB for Qt's on-disk HTTP cache. Regional exports use two
active jobs, at most 128 queued requests, 64 MiB of compressed output cache, and
at most four megapixels per export. The existing map playback cache is separate.
Provider network failures honor the existing shared backoff/Retry-After policy.
Turning off the overlay cancels its queued consumers; an already-running bounded
European decode can finish and populate its cache.

## Regional playback and outages

When the primary is disabled or unavailable, the combined regional playback clock advances in five-minute steps. For each enabled
region, the renderer selects the newest published observation at or before that
clock, at most ten minutes earlier. It preserves NOAA's exact raster IDs and
Canadian/European observation times; no forecast or interpolated storm frames
are generated. The tooltip explains the regional time offset. Canada's oldest
hour is absent from a four-hour loop because its catalog provides three hours.

A missing region stays transparent while other successful regions continue to
render. The sidebar reports unavailable regional data. A complete failure keeps
the existing failure/retry behavior. Disabling or re-enabling a region retires
the previous composite, its playback catalog and its frame cache before changing
attribution. Cache identity includes the enabled-region mask.

## Optional coverage shading

Coverage is a separate checkbox. The overlay is a faint fill using the theme's
secondary text color at 12/255 alpha, with no circle outlines. Overlapping
footprints are painted once with winding fill, so overlaps do not accumulate
opacity. The flat view unwraps geodesics across the date line; the globe clips
the visible footprint at the limb. Hovering near a catalog site's center shows
its identity and nominal range.

Site positions come from the [NOAA radar station API](https://api.weather.gov/radar/stations)
and the [OPERA radar database](https://www.eumetnet.eu/wp-content/themes/aeron-child/observations-programme/current-activities/opera/database/OPERA_Database/).
The current OPERA adapter uses its published June 17, 2026 JSON snapshot and only
active (`status=1`) records. Published site ranges are used. NOAA WSR-88D sites
use the [ROC maximum reflectivity range of 460 km](https://www.weather.gov/roc/WindFarms).
Unknown ranges are never inferred and produce no shaded disc. The site count
includes valid catalog sites even when their range is unknown.

This describes nominal instrument reach, not measured present-day coverage,
terrain visibility, precipitation presence, or live operational status. The
catalog currently contains US and European sites; Canadian site footprints are
not yet supplied by this adapter. Catalog requests happen only when coverage is
enabled, with bounded responses and cached results. The OPERA snapshot URL should
be updated when the official database publishes a replacement.

## Verification

Socket-free tests cover regional URL construction, catalog bounds and scope,
provider cache separation, actual-observation selection, source composition,
provider-switch retirement, all-regions-disabled transparency, geodesic range
math, and the bounded TIFF decoder. The TIFF test constructs a small compressed
fixture in memory and checks both valid rendering and malformed input. Projection
coordinates were independently checked against pyproj 3.8.0 / PROJ 9.8.1. A live
OPERA GeoTIFF was also decoded locally.

Run the focused suite with CTest for `weather_radar_source_test`,
`regional_radar_source_test`, `radar_coverage_test`, `opera_radar_image_test`, and
`weather_radar_loading_test`, plus `libre_radar_test` for the primary adapter. The loading test includes the original playback/rendering
regressions and the new composite cases. No new test joins the frozen PR CI gate.
The globe live-tile regression verifies that native composite requests reach the
network adapter and that returned pixels reach the globe atlas; it failed before
the native-scheme admission fix and passes afterward.

Native macOS verification uses the exact worktree's ARM64 executable and the
MCP automation bridge with TX disabled and `DEMO-0001`. The flat map's GPU surface
is absent from a generic bridge widget grab; verify it with a native screen
capture of that exact application path. Never resolve the app by the generic
name: an older `/Applications/AetherSDR.app` may also be installed.

Local verification passed all five focused CTest targets on the ARM64 build.
MCP and native capture verified simultaneous regional imagery on the flat map,
historical playback, live globe imagery, coverage shading on/off, and regional
checkbox persistence across an application restart. The final globe run remained
disconnected from radio hardware.

The injected-network LibreWXR tests cover primary success and clear pixels
without backup traffic, primary failure switching to NOAA, primary-disabled
operation, cancellation, primary-only catalog requests, XYZ placement/wrapping,
bounded requests, malformed images, tile caching and shared-consumer cancellation.
No test binds a socket or manipulates radio hardware.

LibreWXR verification on the ARM64 build passed all six focused suites. MCP
verified live primary imagery in the globe, native screen capture verified
worldwide flat-map playback, and a full restart preserved primary-disabled
preferences. Disabling LibreWXR visibly returned to direct regional radar;
the primary was re-enabled afterward. Fault injection exercised automatic
primary-to-NOAA fallback and proved clear primary pixels do not fetch backups.
The test instances stayed disconnected from radio hardware with TX automation
disabled, and were stopped after verification.

The resolution/cache regression tests verify 16 full-resolution overview tiles,
single-tile boundary requests at zooms 0 through 8, four-colour placement when
building overview tiles, zero new downloads for zoom-out/return after renderer
restart, clear-weather disk reuse, invalid/expired cache rejection, and fresh
requests for new timestamps and uncached detail. Deliberately restoring the
256-pixel planning constant or bypassing disk reuse makes the respective test
fail. The globe loading test exercises both 512-pixel regional and primary images, including the last tile in each atlas.

The final ARM64 build passed all six focused suites; the loading suite was rerun
after the whole-world playback size fix and passed. MCP/native captures checked
regional globe playback, primary global flat-map playback, source switching and
zoom controls. The final globe run used 2048-square whole-world playback with
LibreWXR disabled, matching the operator's direct-regional setting. Test instances
remained disconnected from radio hardware with TX automation disabled and were
stopped afterward. These changes remain local and uncommitted.
