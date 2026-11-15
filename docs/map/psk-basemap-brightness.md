# PSK Reporter basemap brightness prototype

Map brightness in the Map section adjusts the existing OpenStreetMap imagery
from 20% to 100%. The default is 100%, preserving the previous presentation.
It is a display preference, not a prediction of daylight or propagation.

The flat map composites black with opacity `1 - brightness / 100` above the
base tiles and below the day/night layer. Its bounds follow the camera, so
horizontal world copies are dimmed too. The globe multiplies base RGB by the
same factor in the shader shared by atlas and detail tiles, before applying
night shading. City lights, weather radar, report paths and markers are drawn
later. Map controls, attribution, and the footer remain outside the adjustment.

`MapDisplayWidget` owns the shared value and initializes the lazy globe with
it. The dialog persists `basemapBrightness` inside its existing `PskReporter`
settings object. Missing values default to 100 and values are clamped to
20–100. The control uses the sidebar wheel guard to preserve values while
scrolling; keyboard and pointer editing remain available.

This does not introduce a provider, tile style, credentials, cache variant,
or refresh timer. Tile URLs, original cached imagery, cache expiry, and OSM
attribution remain unchanged. Adjusting brightness repaints existing imagery.

This is dimmed standard cartography. It cannot independently brighten street
labels or select semantic colours as a true dark map style could. Evaluate
label readability and city-light contrast before choosing whether a separate
supported dark-cartography provider is warranted.

## Optional dark map

Dark map is a separate checkbox, off by default. It converts the existing OSM
cartography into a navy/grey ramp: pale backgrounds become dark and dark
printed labels become light. Map brightness remains an independent multiplier;
start at 100% when comparing the two styles. Day/night shading still applies.

The colour treatment uses inverted luminance, interpolated between
`color.map.darkBackground` and `color.map.darkDetail`. Both bundled themes
provide the palette, with the generated seed as the fallback for custom themes.
This is image colour remapping, not semantic cartography: roads, parks, water,
and label halos lose their original colours, and some details may be harder
to distinguish. It uses the same OSM provider and requests.

`BasemapStyle.h` implements the flat image transform. A display-tile factory
hook in QGeoView runs after the original image enters its decoded cache, for
both network and cache deliveries. `DarkBasemapLayer` retains shared originals
only for the live tiles and replaces their displayed images when toggled or
when the theme changes. It never writes recoloured images into the network or
decoded caches, and toggling back restores the originals without refetching.
The extra transformed-image storage expires with each tile. Panning, zooming,
and repeated world copies retain QGeoView's existing lifecycle.

The globe applies the same luminance ramp in its atlas/detail shader without
changing texture storage. Flat tiles are recoloured before image interpolation;
the globe recolours after sampling, so antialiased edges can differ slightly.
Both apply brightness and night shading after recolouring. When Dark map is
enabled, the shadow derives from the dark-map background, so the light app
theme cannot brighten the night side. City lights, radar,
reports, paths, the status footer, and attribution remain outside the transform.

The dialog retains the prototype's `basemapDarkTintEnabled` setting inside
`PskReporter` so existing local testers keep their choice. `MapDisplayWidget`
shares the state with both renderers, including a lazily created globe.
