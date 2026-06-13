#pragma once

#include <QWidget>
#include <QColor>
#include <QVector>

#include <QGeoView/QGVGlobal.h>

class QGVMap;
class QGVLayer;

namespace AetherSDR {

class MapMarkerItem;

// Reusable OpenStreetMap slippy-map widget (#mapping-engine).
//
// Wraps the vendored QGeoView QGVMap with:
//   * A policy-compliant OSM tile layer — shared QNetworkAccessManager with
//     a QNetworkDiskCache (HTTP cache headers honored, OSM requires >= 7
//     days) and an app-identifying User-Agent.
//   * Keyboard navigation: arrow keys pan, +/- (and =) zoom, Home recenters
//     on the home position (the radio's GPS fix for the PSK Reporter map).
//   * A simple marker API (MapView::Marker) used by the PSK Reporter map
//     and, in the future, the AetherModem APRS tab.
//   * The mandatory "© OpenStreetMap contributors" attribution overlay.
class MapView : public QWidget {
    Q_OBJECT

public:
    struct Marker {
        double  lat{0.0};
        double  lon{0.0};
        QString label;       // short text drawn next to the dot
        QString tooltip;     // hover detail
        QColor  color{Qt::red};
        bool    isHome{false};  // drawn as a distinct station marker
    };

    explicit MapView(QWidget* parent = nullptr);

    // Home position (e.g. radio GPS fix). Home key / resetToHome() recenters
    // here. Also draws/updates the home station marker when showMarker.
    void setHomePosition(double lat, double lon, const QString& label = {},
                         bool showMarker = true);
    bool hasHomePosition() const { return m_hasHome; }

    void setMarkers(const QVector<Marker>& markers);
    void clearMarkers();

    QGVMap* map() const { return m_map; }

public slots:
    void resetToHome();
    void zoomIn();
    void zoomOut();

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    // Install the process-wide tile network manager (disk cache + UA) on
    // first use. Safe to call repeatedly.
    static void ensureTileNetworkManager();

    void pan(double dxFraction, double dyFraction);

    QGVMap*  m_map{nullptr};
    QGVLayer* m_markerLayer{nullptr};
    MapMarkerItem* m_homeMarker{nullptr};
    QVector<MapMarkerItem*> m_markers;

    double m_homeLat{0.0};
    double m_homeLon{0.0};
    QString m_homeLabel;
    bool   m_hasHome{false};
    bool   m_firstShow{true};
};

} // namespace AetherSDR
