#pragma once

#include "CityLightsShading.h"
#include "MapView.h"
#include "GlobeNavigation.h"
#include "WeatherRadarSource.h"

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
#include <QVector2D>
#include <QVector4D>

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
    friend class WeatherRadarLoadingTest;

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
    void setCityLightsVisible(bool visible);
    void setCityLightsImage(const QImage& image, const QRectF& bounds);
    void setBasemapDarkEnabled(bool enabled);
    void setBasemapBrightness(int percent);
    void setCityLightsBrightness(int percent);
    void setCityLightsFaintLights(int percent);
    void setCityLightsWarmth(int percent);
    void setRadarSites(const QVector<RadarSite>& sites, bool visible);
    void setDetailedAttributionVisible(bool visible);
    void setWeatherRadarVisible(bool visible);
    bool weatherRadarVisible() const { return m_weatherRadarVisible; }
    int pendingWeatherRadarRequests() const;
    bool weatherRadarLoadFailed() const { return m_weatherRadarLoadFailed; }
    void refreshWeatherRadar();
    void setWeatherRadarSource(const WeatherRadarSource& source);
    QRectF weatherRadarPlaybackBounds() const;
    QSize weatherRadarPlaybackSize(const QRectF& bounds) const;
    // Atomic presentation of a single original NOAA observation.
    bool showWeatherRadarPlaybackFrame(
        const QImage& image, const QDateTime& frameTime, const QRectF& bounds);
    void acknowledgeWeatherRadarPlaybackFrame(quint64 presentationSequence);
    void preloadWeatherRadarPlaybackFrame(const QImage& image,
                                          const QDateTime& frameTime,
                                          const QRectF& bounds);
    void clearWeatherRadarPlayback();
    void setLegend(const QVector<QPair<QString, QColor>>& entries);

signals:
    void imageOverlayViewChanged();
    void markerClicked(const GlobeMapView::Marker& marker);
    void rendererUnavailable(const QString& reason);
    void weatherRadarFrameLoaded(const QDateTime& frameTime);
    void weatherRadarPlaybackPresented(quint64 presentationSequence);
    void weatherRadarPlaybackFramePreloaded(const QDateTime& frameTime);
    void weatherRadarPlaybackViewChanged();

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
        bool weatherRadar{false};
        QString radarFrameId;
    };

    struct DetailTile {
        int zoom{0};
        int x{0};
        int y{0};
        QImage image;
        std::unique_ptr<QOpenGLTexture> texture;
        QImage radarImage;
        std::unique_ptr<QOpenGLTexture> radarTexture;
        std::unique_ptr<QOpenGLTexture> previousRadarTexture;
        QOpenGLBuffer vertexBuffer{QOpenGLBuffer::VertexBuffer};
        QOpenGLBuffer indexBuffer{QOpenGLBuffer::IndexBuffer};
        int indexCount{0};
        bool loading{false};
        bool radarLoading{false};
        QString radarFrameId;
        quint64 lastUsedFrame{0};
    };

    void buildSphereMesh();
    void requestAtlasTiles();
    void requestWeatherRadarAtlas();
    void cancelWeatherRadarRequests();
    void requestNextTiles();
    void cancelTileRequests();
    void cleanupOpenGlResources();
    void drawCityLights(const QMatrix4x4& matrix);
    void drawRadarCoverage(const QMatrix4x4& matrix);
    void updateMapAttribution();
    void reportRendererUnavailable(const QString& reason,
                                   const QString& detail = {});
    void scheduleAtlasUpload();
    void uploadAtlas();
    void uploadWeatherRadarAtlas();
    void uploadPendingWeatherRadarPlaybackFrame();
    void uploadPreloadedWeatherRadarAtlas();
    int detailZoomLevel() const;
    void refreshDetailTiles(const QMatrix4x4& model,
                            const QMatrix4x4& viewProjection);
    bool detailTileVisible(int zoom, int x, int y,
                           const QMatrix4x4& model,
                           const QMatrix4x4& viewProjection,
                           QPointF* priorityPoint) const;
    void uploadDetailTile(DetailTile& tile);
    void uploadWeatherRadarDetailTile(DetailTile& tile);
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
    void scheduleWeatherRadarPlaybackViewRefresh();
    bool useInteractionPreview() const;
    void layoutOverlays();
    QToolButton* makeOverlayButton(const QString& text, const QString& tip);
    void updateTheme();

    QImage m_cityLightsImage;
    QRectF m_cityLightsBounds;
    std::unique_ptr<QOpenGLTexture> m_cityLightsTexture;
    std::unique_ptr<QOpenGLShaderProgram> m_cityLightsProgram;
    bool m_cityLightsVisible{false};
    bool m_cityLightsDirty{false};
    float m_cityLightsOpacity{CityLightsShading::kDefaultBrightness / 100.0F};
    float m_cityLightsGamma{float(CityLightsShading::faintLightsGamma(CityLightsShading::kDefaultFaintLights))};
    float m_cityLightsWarmth{CityLightsShading::kDefaultWarmth / 100.0F};
    std::unique_ptr<QOpenGLShaderProgram> m_program;
    std::unique_ptr<QOpenGLShaderProgram> m_radarProgram;
    std::unique_ptr<QOpenGLShaderProgram> m_radarCoverageProgram;
    QOpenGLBuffer m_radarCoverageBuffer{QOpenGLBuffer::VertexBuffer};
    QVector<QVector3D> m_radarCoverageVertices;
    bool m_radarCoverageDirty{false};
    std::unique_ptr<QOpenGLTexture> m_texture;
    std::unique_ptr<QOpenGLTexture> m_radarTexture;
    std::unique_ptr<QOpenGLTexture> m_previousRadarTexture;
    std::unique_ptr<QOpenGLTexture> m_preloadedRadarTexture;
    std::unique_ptr<QOpenGLTexture> m_spareRadarTexture;
    QOpenGLBuffer m_vertexBuffer{QOpenGLBuffer::VertexBuffer};
    QOpenGLBuffer m_indexBuffer{QOpenGLBuffer::IndexBuffer};
    int m_indexCount{0};

    QImage m_atlas;
    QImage m_weatherRadarAtlas;
    QImage m_pendingCurrentPlaybackRadarAtlas;
    QImage m_preloadedWeatherRadarAtlas;
    QImage m_pendingWeatherRadarAtlas;
    bool m_weatherRadarLoadFailed{false};
    QDateTime m_previousRadarTextureFrameTime;
    QDateTime m_radarTextureFrameTime;
    QDateTime m_pendingCurrentPlaybackFrameTime;
    QDateTime m_preloadedWeatherRadarFrameTime;
    // Pixel identity travels with each texture, not with its reusable storage.
    // Time/extent/size alone cannot distinguish a replacement image.
    qint64 m_radarTextureImageKey{0};
    qint64 m_preloadedRadarImageKey{0};
    WeatherRadarSource m_weatherRadarSource;
    QString m_pendingWeatherRadarFrameId;
    QString m_loadedWeatherRadarFrameId;
    int m_pendingWeatherRadarTileCount{0};
    bool m_pendingWeatherRadarAtlasFailed{false};
    QVector<TileRequest> m_pendingTiles;
    QSet<QNetworkReply*> m_tileReplies;
    QSet<QNetworkReply*> m_weatherRadarReplies;
    QHash<QString, std::shared_ptr<DetailTile>> m_detailTiles;
    QVector<QString> m_visibleDetailKeys;
    int m_activeTileRequests{0};
    quint64 m_detailFrame{0};
    QTimer m_atlasUploadTimer;
    QTimer m_terminatorTimer;
    QTimer m_interactionSettleTimer;
    QTimer m_weatherRadarPlaybackViewTimer;
    bool m_atlasDirty{false};
    bool m_weatherRadarAtlasDirty{false};
    bool m_pendingWeatherRadarPlaybackFrameDirty{false};
    bool m_replaceWeatherRadarTexture{false};
    bool m_preloadedWeatherRadarAtlasDirty{false};
    int m_preloadedWeatherRadarUploadRow{0};
    QVector4D m_weatherRadarTextureBounds{0.0F, 0.0F, 1.0F, 1.0F};
    QVector4D m_previousRadarTextureBounds{0.0F, 0.0F, 1.0F, 1.0F};
    QVector4D m_preloadedRadarTextureBounds{0.0F, 0.0F, 1.0F, 1.0F};
    QVector4D m_pendingRadarTextureBounds{0.0F, 0.0F, 1.0F, 1.0F};
    float m_weatherRadarTransitionProgress{1.0F};
    quint64 m_pendingWeatherRadarPresentationSequence{0};
    bool m_releasePreviousRadarTextures{false};
    bool m_releasePlaybackRadarTextures{false};
    bool m_detailSelectionDirty{true};
    bool m_glInitializationAttempted{false};
    bool m_rendererUnavailableReported{false};
    bool m_cleaningOpenGlResources{false};

    QVector<RadarSite> m_radarSites;
    bool m_radarCoverageVisible{false};
    QVector<Marker> m_markers;
    QVector<ProjectedMarker> m_projectedMarkers;
    int m_hoverMarker{-1};
    bool m_pathsVisible{true};
    bool m_basemapDarkEnabled{false};
    int m_basemapBrightness{100};
    bool m_terminatorVisible{true};
    bool m_weatherRadarVisible{false};
    bool m_weatherRadarPlaybackActive{false};

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
    QVariantAnimation* m_weatherRadarTransition{nullptr};

    QLabel* m_attribution{nullptr};
    bool m_detailedAttributionVisible{true};
    QWidget* m_vectorOverlay{nullptr};
    QLabel* m_legend{nullptr};
    QLabel* m_hoverCard{nullptr};
    QToolButton* m_zoomInButton{nullptr};
    QToolButton* m_zoomOutButton{nullptr};
    QToolButton* m_homeButton{nullptr};
    QColor m_backgroundColor;
    QColor m_nightColor;
    QColor m_basemapBackground;
    QColor m_basemapDetail;
    QColor m_textColor;
    QMatrix4x4 m_overlayModel;
    QMatrix4x4 m_overlayViewProjection;
    bool m_overlayMatricesValid{false};
    bool m_vectorOverlayDirty{true};
};

} // namespace AetherSDR
