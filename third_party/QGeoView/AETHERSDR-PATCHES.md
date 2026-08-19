# AetherSDR patches to vendored QGeoView 1.1.0

Upstream: https://github.com/AmonRaNet/QGeoView (tag 1.1.0, LGPL-3.0).
The `demo/` and `samples/` trees and `.github/` are dropped from the vendored
copy. The library is otherwise pristine except for the patches below — keep
this list current when updating the snapshot.

1. **`lib/CMakeLists.txt`** — `add_library(qgeoview SHARED)` → `STATIC`, and
   the private `QGV_EXPORT` define is replaced by a public empty
   `QGV_LIB_DECL=` so no DLL export/import decoration is emitted.

2. **`lib/src/QGVLayerTilesOnline.cpp`** — removed the hardcoded fake
   MSIE User-Agent (the OSM tile usage policy documents browser
   impersonation as a blocking offense) and the
   `QSslSocket::VerifyNone` peer-verification bypass. Tile requests now
   send `QGV::getTileUserAgent()` and use default TLS verification.

3. **`lib/include/QGeoView/QGVGlobal.h` / `lib/src/QGVGlobal.cpp`** — added
   `QGV::setTileUserAgent()` / `QGV::getTileUserAgent()` so the application
   can install its app-identifying UA (AetherSDR sets
   `AetherSDR/<version> (https://github.com/aethersdr/AetherSDR)`).

4. **`lib/src/QGVLayerOSM.cpp`** — default tile URL templates switched from
   `http://{a,b,c}.tile.openstreetmap.org` to
   `https://tile.openstreetmap.org` (subdomain aliases are deprecated by
   OSM and plain HTTP redirects anyway).

5. **`QGVGlobal`, `QGVMapQGView`, and the tile layers** — added optional
   horizontal world repetition (`setHorizontalWrapEnabled()`, off by default,
   so an unpatched caller behaves exactly as upstream). The camera keeps
   projection x continuous across the antimeridian — `QGVMapQGView` slides the
   scene rect to follow it rather than wrapping the coordinate — while logical
   tile x repeats and `QGV::wrapTileX()` maps it back onto the canonical XYZ
   range so network URLs and the HTTP cache never see the repetition.
   `wrapTileX` is used by the tile layer only; the camera does not need it.
   `QGVLayerTilesOnline` also gained a bounded decoded-image cache, so
   re-entering an area costs a memcpy instead of a fetch and a PNG decode.

6. **`lib/src/QGVMapQGView.cpp`** — normalized wheel and high-resolution
   touchpad zoom to one symmetric exponential curve. Upstream's asymmetric
   in/out exponents (`2^(1/2)` in, `2^(1/1.5)` out) meant equal numbers of
   notches in and out did not return the operator to the scale they started
   from — a mouse wheel lost about 11% of scale per in/out round trip.
   Magnitudes, for the record: for a mouse wheel, zoom-**in** is numerically
   identical to upstream (`2^(delta/240)`) and zoom-**out** is ~19% gentler
   (`0.707x` per notch instead of `0.630x`). The high-resolution path changes
   more — upstream fed `pixelDelta` through the same `/120` divisor and the
   same asymmetric exponents, so `kTrackpadPixelsPerDoubling = 120.0` makes
   trackpad zoom-in exactly **2x faster per pixel** than upstream.
   `map_wrap_test` pins the round-trip property.

7. **`lib/src/QGVGlobal.cpp` — `GeoTilePos::parent()` floors instead of
   truncating.** Upstream divides two `int`s and calls `qFloor` on the
   already-truncated quotient, which rounds toward zero. That is only
   reachable once patch 5 makes tile x negative in the world copy west of the
   base one, and `contains()` is built on `parent()`, so the truncated form
   reported a western-copy tile as a child of a base-world tile.
   `removeWhenCovered()` counts coverage through `contains()`, so it would
   evict a fallback tile while the base world behind it was still half loaded —
   a blank hole at the antimeridian, i.e. exactly the symptom patch 5 exists
   to remove. Identical results for x >= 0. Pinned in `map_wrap_test`.

8. **`lib/src/QGVLayerTiles.cpp` — off-screen tiles are culled at every zoom
   level, not just the current one.** Upstream culls only `mCurZoom` against
   the active rect; tiles at the retained neighbouring zoom levels are removed
   solely by coverage or by zoom distance. That was safe while tile x was
   clamped to `[0, 2^zoom]`, because the set was finite by construction. With
   patch 5 it is not: every world copy the camera crossed left behind another
   band of fallback tiles that nothing ever reclaimed. `overlapsActiveRect()`
   adds the positional test, and it runs only when wrap is enabled.

9. **`lib/src/QGVLayerTilesOnline.cpp` — in-flight requests are keyed on the
   canonical tile.** Upstream keys `mRequest` on the position it was asked
   for; with patch 5 the several visible copies of one tile all resolve to the
   same canonical URL, so that would issue one identical concurrent GET per
   copy. The finished reply is now fanned out to every copy waiting on it.
