#pragma once

#include "MapView.h"
#include "GlobeNavigation.h"

#include <QHash>
#include <QImage>
#include <QMatrix4x4>
#include <QOpenGLBuffer>
#include <QOpenGLFunctions>
#include <QOpenGLWidget>
#include <QPointF>
#include <QQuaternion>
#include <QSet>
#include <QTimer>
#include <QVector>

#include <memory>

class QLabel;
class QNativeGestureEvent;
class QNetworkReply;
class QOpenGLShaderProgram;
class QOpenGLTexture;
class QPainter;
class QPinchGesture;
class QShowEvent;
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
    void rendererUnavailable(const QString& reason);

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
    void showEvent(QShowEvent* event) override;
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

    struct TileRequest {
        int zoom{0};
        int x{0};
        int y{0};
        bool baseAtlas{false};
    };

    struct DetailTile {
        int zoom{0};
        int x{0};
        int y{0};
        QImage image;
        std::unique_ptr<QOpenGLTexture> texture;
        QOpenGLBuffer vertexBuffer{QOpenGLBuffer::VertexBuffer};
        QOpenGLBuffer indexBuffer{QOpenGLBuffer::IndexBuffer};
        int indexCount{0};
        bool loading{false};
        quint64 lastUsedFrame{0};
    };

    void buildSphereMesh();
    void requestAtlasTiles();
    void requestNextTiles();
    void cancelTileRequests();
    void cleanupOpenGlResources();
    void reportRendererUnavailable(const QString& reason,
                                   const QString& detail = {});
    void scheduleAtlasUpload();
    void uploadAtlas();
    int detailZoomLevel() const;
    void refreshDetailTiles(const QMatrix4x4& model,
                            const QMatrix4x4& viewProjection);
    bool detailTileVisible(int zoom, int x, int y,
                           const QMatrix4x4& model,
                           const QMatrix4x4& viewProjection,
                           QPointF* priorityPoint) const;
    void uploadDetailTile(DetailTile& tile);
    void destroyDetailTile(DetailTile& tile);
    void evictDetailTiles();
    static QString detailTileKey(int zoom, int x, int y);
    void paintPaths(QPainter& painter, const QMatrix4x4& model,
                    const QMatrix4x4& viewProjection);
    void paintMarkers(QPainter& painter, const QMatrix4x4& model,
                      const QMatrix4x4& viewProjection);
    void paintVectorOverlay(QPainter& painter);
    bool projectPoint(const QVector3D& point, const QMatrix4x4& model,
                      const QMatrix4x4& viewProjection,
                      QPointF* screenPoint) const;
    QVector3D geoPoint(double lat, double lon) const;
    void updateHover(const QPointF& position);
    void showHoverCard(int markerIndex, const QPointF& position);
    void animateZoomTo(float distance);
    void applyDragDelta(const QPointF& delta);
    void applyRollDelta(float degrees);
    void beginTransientInteraction();
    bool useInteractionPreview() const;
    void layoutOverlays();
    QToolButton* makeOverlayButton(const QString& text, const QString& tip);
    void updateTheme();

    std::unique_ptr<QOpenGLShaderProgram> m_program;
    std::unique_ptr<QOpenGLTexture> m_texture;
    QOpenGLBuffer m_vertexBuffer{QOpenGLBuffer::VertexBuffer};
    QOpenGLBuffer m_indexBuffer{QOpenGLBuffer::IndexBuffer};
    int m_indexCount{0};

    QImage m_atlas;
    QVector<TileRequest> m_pendingTiles;
    QSet<QNetworkReply*> m_tileReplies;
    QHash<QString, std::shared_ptr<DetailTile>> m_detailTiles;
    QVector<QString> m_visibleDetailKeys;
    int m_activeTileRequests{0};
    quint64 m_detailFrame{0};
    QTimer m_atlasUploadTimer;
    QTimer m_terminatorTimer;
    QTimer m_interactionSettleTimer;
    bool m_atlasDirty{false};
    bool m_detailSelectionDirty{true};
    bool m_glInitializationAttempted{false};
    bool m_rendererUnavailableReported{false};
    bool m_cleaningOpenGlResources{false};

    QVector<Marker> m_markers;
    QVector<ProjectedMarker> m_projectedMarkers;
    int m_hoverMarker{-1};
    bool m_pathsVisible{true};
    bool m_terminatorVisible{true};

    double m_homeLat{0.0};
    double m_homeLon{0.0};
    double m_homeSpanDegrees{30.0};
    QString m_homeLabel;
    bool m_hasHome{false};
    bool m_homeMarkerShown{false};

    GlobeNavigation m_navigation;
    float m_cameraDistance{3.8F};
    QPointF m_lastPointerPosition;
    bool m_dragging{false};
    bool m_hasMovedDuringDrag{false};
    std::unique_ptr<QVariantAnimation> m_zoomAnimation;

    QLabel* m_attribution{nullptr};
    QWidget* m_vectorOverlay{nullptr};
    QLabel* m_legend{nullptr};
    QLabel* m_hoverCard{nullptr};
    QToolButton* m_zoomInButton{nullptr};
    QToolButton* m_zoomOutButton{nullptr};
    QToolButton* m_homeButton{nullptr};
    QColor m_backgroundColor;
    QColor m_nightColor;
    QColor m_textColor;
    QMatrix4x4 m_overlayModel;
    QMatrix4x4 m_overlayViewProjection;
    bool m_overlayMatricesValid{false};
};

} // namespace AetherSDR
