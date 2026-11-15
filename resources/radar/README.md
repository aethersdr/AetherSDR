# Bundled radar data

These files contain published data, not third-party executable code. They are
loaded from Qt resources; displaying coverage or legends makes no HTTP requests.

## Station snapshots

Retrieved 2026-09-14:

- `noaa-stations.json`: 208 records from <https://api.weather.gov/radar/stations>.
  Geometry and id/name/stationType only; volatile telemetry is deliberately omitted.
  NOAA/NWS public data: <https://www.weather.gov/disclaimer>.
- `opera-stations.json`: 218 records from the September 10 OPERA database export:
  <https://www.eumetnet.eu/wp-content/themes/aeron-child/observations-programme/current-activities/opera/database/OPERA_Database/Data/OPERA_RADARS_DB_10092026.json>.
  EUMETNET OPERA, CC BY 4.0; source and access information:
  <https://www.eumetnet.eu/observations/weather-radar-network/>.
  Retained fields: status, odimcode, location, country, latitude, longitude, maxrange.

The renderer validates both files, filters OPERA status=1 records, and uses only
published ranges. A station marked active in a dated catalog is not a claim of
current operational status. NOAA WSR-88D's 460 km maximum reflectivity range is
published by <https://www.weather.gov/roc/WindFarms>; other types get no inferred
range. This is a US/European site collection, not worldwide coverage.

To update, download the NOAA station API and follow the current download link on
the official OPERA database page. Keep only the fields above, retain attribution,
record the new snapshot date, and run `radar_coverage_test` plus the native map
coverage checks. Review added/removed stations and ranges before committing.
Do not replace a failed download with an empty catalog.

## Measurement palettes

`legends.json` records color and tick-label facts from these published sources,
retrieved 2026-09-14. It contains no provider implementation code.

- NOAA: the NWS radar client BNOB base-reflectivity legend (BREF.QCD), dBZ:
  <https://radar.weather.gov/cmi-radar/cmi-radar.eb3ff2a6.js>.
- ECCC: the `Radar-Rain` legend for `RADAR_1KM_RRAI`, mm/h:
  <https://geo.weather.gc.ca/geomet?version=1.3.0&service=WMS&request=GetLegendGraphic&sld_version=1.1.0&layer=RADAR_1KM_RRAI&format=image/png&STYLE=Radar-Rain>.
  Colors sampled at the published scale ticks. The map explicitly requests this
  style. The scale is nonlinear, as in the source legend.
  ECCC/MSC: <https://eccc-msc.github.io/open-data/licence/readme_en/>.
- LibreWXR: scheme 6, NEXRAD Level III, rain palette, dBZ equivalent:
  <https://github.com/JoshuaKimsey/LibreWXR/blob/main/src/librewxr/colors/color_table.csv>.
  The requested `0_0` options disable snow coloring. Reflectivity-equivalent
  colors do not turn the satellite/model portions into measured ground radar.
  LibreWXR output terms: <https://librewxr.net/terms.html>.
- OPERA: generated directly from `OperaRadarImage::colorForDbz`, the same
  function used to render the DBZH raster, so the legend cannot drift from it.

Update imagery style and its legend together. Never normalize these providers
through an assumed reflectivity-to-rain-rate conversion.
