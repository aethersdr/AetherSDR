#pragma once

#include "MapView.h"

#include <QImage>
#include <QMatrix4x4>
#include <QOpenGLBuffer>
#include <QOpenGLFunctions>
#include <QOpenGLWidget>
#include <QPointF>
#include <QQuaternion>
#include <QTimer>
#include <QVector>

#include <memory>

class QLabel;
class QNativeGestureEvent;
class QOpenGLShaderProgram;
class QOpenGLTexture;
class QPainter;
class QPinchGesture;
class QToolButton;
class QVariantAnimation;

namespace AetherSDR {

// Interactive orthographic globe renderer for PSK Reporter. The sphere is
// drawn by OpenGL; labels, markers and paths are composited by QPainter so the
// existing MapView marker contract stays authoritative for both projections.
class GlobeMapView final : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT

public:
    using Marker = MapView::Marker;

    explicit GlobeMapView(QWidget* parent = nullptr);
    ~GlobeMapView() override;

    void setHomePosition(double lat, double lon, const QString& label = {},
                         bool showMarker = true);
    void setHomeSpanDegrees(double spanDegrees);
    bool hasHomePosition() const { return m_hasHome; }
    double homeLat() const { return m_homeLat; }
    double homeLon() const { return m_homeLon; }

    void setMarkers(const QVector<Marker>& markers);
    void clearMarkers();
    void setPathsVisible(bool visible);
    bool pathsVisible() const { return m_pathsVisible; }
    void setDayNightTerminatorVisible(bool visible);
    bool dayNightTerminatorVisible() const { return m_terminatorVisible; }
    void setLegend(const QVector<QPair<QString, QColor>>& entries);

signals:
    void markerClicked(const GlobeMapView::Marker& marker);

public slots:
    void resetToHome();
    void zoomIn();
    void zoomOut();

protected:
    void initializeGL() override;
    void resizeGL(int width, int height) override;
    void paintGL() override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    bool event(QEvent* event) override;

private:
    struct Vertex {
        QVector3D position;
        QVector2D uv;
    };

    struct ProjectedMarker {
        QPointF point;
        bool visible{false};
    };

    void buildSphereMesh();
    void requestAtlasTiles();
    void requestNextTiles();
    void scheduleAtlasUpload();
    void uploadAtlas();
    void paintPaths(QPainter& painter, const QMatrix4x4& model,
                    const QMatrix4x4& viewProjection);
    void paintMarkers(QPainter& painter, const QMatrix4x4& model,
                      const QMatrix4x4& viewProjection);
    void paintPathForMarker(QPainter& painter, const Marker& marker,
                            const QMatrix4x4& model,
                            const QMatrix4x4& viewProjection);
    bool projectPoint(const QVector3D& point, const QMatrix4x4& model,
                      const QMatrix4x4& viewProjection,
                      QPointF* screenPoint) const;
    QVector3D geoPoint(double lat, double lon) const;
    void updateHover(const QPointF& position);
    void showHoverCard(int markerIndex, const QPointF& position);
    void animateZoomTo(float distance);
    void applyDragDelta(const QPointF& delta);
    void layoutOverlays();
    QToolButton* makeOverlayButton(const QString& text, const QString& tip);
    void updateTheme();

    std::unique_ptr<QOpenGLShaderProgram> m_program;
    std::unique_ptr<QOpenGLTexture> m_texture;
    QOpenGLBuffer m_vertexBuffer{QOpenGLBuffer::VertexBuffer};
    QOpenGLBuffer m_indexBuffer{QOpenGLBuffer::IndexBuffer};
    int m_indexCount{0};

    QImage m_atlas;
    QVector<QPair<int, int>> m_pendingTiles;
    int m_activeTileRequests{0};
    QTimer m_atlasUploadTimer;
    bool m_atlasDirty{false};

    QVector<Marker> m_markers;
    QVector<ProjectedMarker> m_projectedMarkers;
    QVector<QPair<QString, QColor>> m_legendEntries;
    int m_hoverMarker{-1};
    bool m_pathsVisible{true};
    bool m_terminatorVisible{true};

    double m_homeLat{0.0};
    double m_homeLon{0.0};
    double m_homeSpanDegrees{30.0};
    QString m_homeLabel;
    bool m_hasHome{false};
    bool m_homeMarkerShown{false};

    QQuaternion m_rotation;
    float m_cameraDistance{3.1F};
    QPointF m_lastPointerPosition;
    bool m_dragging{false};
    bool m_hasMovedDuringDrag{false};
    qreal m_nativeGestureStartDistance{0.0};
    std::unique_ptr<QVariantAnimation> m_zoomAnimation;

    QLabel* m_attribution{nullptr};
    QLabel* m_legend{nullptr};
    QLabel* m_hoverCard{nullptr};
    QToolButton* m_zoomInButton{nullptr};
    QToolButton* m_zoomOutButton{nullptr};
    QToolButton* m_homeButton{nullptr};
    QColor m_backgroundColor;
    QColor m_nightColor;
    QColor m_textColor;
};

} // namespace AetherSDR
