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
