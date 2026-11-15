# QGeoView (QGV)

![Logo](https://github.com/AmonRaNet/QGeoView/raw/master/market-place/logo_64_64.png) QGeoView is a geographic data widget.

## Headings

QGeoView allows to show different kind of geographic data, like tiled layers, raster or vector data.
Component is based on QGraphicsView and focused on usability and performance.
QGeoView public interface try to follow best practices from OpenLayers and Google Maps API and supports all basic map features, like:

 * EPSG3857 projection (others can be added)
 * Camera changes: scaling, rotation, moving
 * TMS (OSM, Google, Bing or custom)
 * Map control widgets

## Input data

QGeoView uses a hierarchical object storage system, which can store  both visual objects (for example, raster or shapes) and non-visual objects (for example, a layer of items). Override in paint event for the visual object allow you to show on the map any geo-data from any source. However, QGeoView itself does not provide methods for reading/parsing of geo-data sources (files). The only exception are TMS servers and bitmap images, for all other sources you need to extend solution.

## Links

Project repo: https://github.com/AmonRaNet/QGeoView

Demo video: [Youtube](https://youtu.be/t0D21r_s-8E)
