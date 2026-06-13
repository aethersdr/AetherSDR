# How to

![Logo](market-place/logo_64_64.png) QGeoView is a Qt / C ++ widget for visualizing geographic data.

### Basic usage

Minimal project for QGeoView in [basic](samples/basic)

Google Maps, Bing, OpenStreetMap, custom XYZ as background layer in [background](samples/background)

Maps widgets (compass, zoom, scale) in [widgets](samples/widgets)

Programmatically rotate, scale or move geo-map in [camera-actions](samples/camera-actions)

Geo-map mouse actions in [mouse-actions](samples/mouse-actions):
 - map move
 - map zoom
 - map rotation
 - context menu

Special flags for draw items in [flags](samples/flags):
 - items with fixed size/azimuth
 - items with mouse hover
 - items with tooltip
 - items with selection capability
 - items with custom transformations
 - individual mouse action actions

Map with several object-layers in [layers](samples/layers)

### Performance

Background map has biggest impact to performance because it covers whole map area all the time.
QGVLayerTiles always calculates needed set of tiles for current camera state and performance parameters
will adjust algorithm.

Example for performance tuning can be found in [performance](samples/performance)

### Debug and logging

How to catch debug info in qDebug or visually on map [debug](samples/debug)

### Advanced usage

Example with moving object on geo-map in [moving-objects](samples/moving-objects)

Example with custom tile layer in [custom-tiles](samples/custom-tiles)

Small funny project :) in [fun](samples/fun)
