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
   horizontal world repetition. The camera stays continuous across the
   antimeridian, logical tile x positions repeat while network URLs remain
   canonical, and a bounded decoded-image cache avoids repeated PNG decode on
   nearby revisits.

6. **`lib/src/QGVMapQGView.cpp`** — normalized wheel and high-resolution
   touchpad zoom to one symmetric exponential curve. Upstream's asymmetric
   in/out exponents made navigation feel markedly different by direction.
