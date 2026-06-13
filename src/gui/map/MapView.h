#pragma once

#include <QWidget>
#include <QTimer>
#include <QColor>
#include <QVector>

#include <QGeoView/QGVGlobal.h>

class QGVMap;
class QGVLayer;
class QLabel;
class QToolButton;
class QVariantAnimation;

namespace AetherSDR {

class MapMarkerItem;
class MapPathItem;

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
        QString clickInfo;      // rich text shown on click (empty = none)
    };

    explicit MapView(QWidget* parent = nullptr);

    // Home position (e.g. radio GPS fix). Home key / resetToHome() recenters
    // here. Also draws/updates the home station marker when showMarker.
    void setHomePosition(double lat, double lon, const QString& label = {},
                         bool showMarker = true);
    bool hasHomePosition() const { return m_hasHome; }

    void setMarkers(const QVector<Marker>& markers);
    void clearMarkers();

    // Great-circle paths from home to every marker.
    void setPathsVisible(bool visible);
    bool pathsVisible() const { return m_pathsVisible; }

    // Color/label legend chip, lower-left. Empty list hides it.
    void setLegend(const QVector<QPair<QString, QColor>>& entries);

    double homeLat() const { return m_homeLat; }
    double homeLon() const { return m_homeLon; }

    QGVMap* map() const { return m_map; }

signals:
    void markerClicked(const MapView::Marker& marker);

public slots:
    void resetToHome();
    void zoomIn();
    void zoomOut();

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    // Install the process-wide tile network manager (disk cache + UA) on
    // first use. Safe to call repeatedly.
    static void ensureTileNetworkManager();

    void pan(double dxFraction, double dyFraction);
    QToolButton* makeOverlayButton(const QString& text, const QString& tip);
    void layoutOverlayButtons();
    void rebuildPaths();
    void clampMinZoomToViewport();
    // Instant hover tooltip driven by mouse-move (QGeoView's built-in
    // tooltip waits for the OS hover delay, which is too slow here).
    void showHoverTooltip(const QPointF& projPos);

    QGVMap*  m_map{nullptr};
    QGVLayer* m_markerLayer{nullptr};
    MapMarkerItem* m_homeMarker{nullptr};
    MapMarkerItem* m_hoverMarker{nullptr};
    QVector<MapMarkerItem*> m_markers;
    QVector<Marker> m_markerData;
    QVector<MapPathItem*> m_paths;
    bool m_pathsVisible{true};
    QLabel* m_legend{nullptr};

    double m_homeLat{0.0};
    double m_homeLon{0.0};
    QString m_homeLabel;
    bool   m_hasHome{false};
    bool   m_firstShow{true};

    // Zoom / recenter overlay buttons (upper-right).
    QToolButton* m_zoomInBtn{nullptr};
    QToolButton* m_zoomOutBtn{nullptr};
    QToolButton* m_homeBtn{nullptr};

    // Sonar pulse on the home marker: a short ring animation fired every
    // few seconds. The animation only runs for its ~1s duration, so the
    // idle cost is one timer tick every 3 s.
    QTimer* m_pulseTimer{nullptr};
    QVariantAnimation* m_pulseAnim{nullptr};
};

} // namespace AetherSDR
