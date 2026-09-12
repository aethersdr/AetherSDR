#include "GlobeMapView.h"
#include "BasemapStyle.h"
#include "CityLightsShading.h"

#include "MapHoverPathSelection.h"
#include "SolarTerminator.h"
#include "WeatherRadarStyle.h"
#include "WeatherRadarTexture.h"
#include "WeatherRadarViewGeometry.h"
#include "core/ThemeManager.h"

#include <QGeoView/QGVGlobal.h>

#include <QDateTime>
#include <QEasingCurve>
#include <QGestureEvent>
#include <QHash>
#include <QKeyEvent>
#include <QLabel>
#include <QLoggingCategory>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QOpenGLContext>
#include <QOpenGLShaderProgram>
#include <QOpenGLTexture>
#include <QPainter>
#include <QPaintEvent>
#include <QPainterPath>
#include <QSurfaceFormat>
#include <QPinchGesture>
#include <QResizeEvent>
#include <QSet>
#include <QShowEvent>
#include <QToolButton>
#include <QVariantAnimation>
#include <QWheelEvent>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace AetherSDR {

Q_LOGGING_CATEGORY(lcPskReporterGlobe, "aether.pskreporter.globe")

namespace {
constexpr int kAtlasZoom = 2;
constexpr int kTileCount = 1 << kAtlasZoom;
constexpr int kTileSize = 256;
constexpr int kAtlasSize = kTileCount * kTileSize;
constexpr int kLatitudeSegments = 96;
constexpr int kLongitudeSegments = 192;
constexpr int kMaximumConcurrentTileRequests = 4;
// Qt's unified animation clock advances in roughly 16 ms steps. Using an
// exact multiple prevents chained frames from alternating between different
// tick counts, while keeping the requested playback brisk.
constexpr int kWeatherRadarTransitionMs = 240;
constexpr int kMaximumVisibleDetailTiles = 160;
constexpr int kMaximumCachedDetailTiles = 256;
constexpr int kDetailTileSegments = 8;
constexpr qint64 kMaximumTileBytes = 2 * 1024 * 1024;
constexpr int kRadarUploadStripeRows = 384;
constexpr double kWebMercatorExtent = 20037508.342789244;
// The globe radius is 1.0. Keep the camera just outside the surface while
// allowing a neighbourhood-scale view comparable to the flat map.
constexpr float kMinimumCameraDistance = 1.08F;
constexpr float kMaximumCameraDistance = 6.0F;
constexpr float kDefaultCameraDistance = 3.8F;
constexpr float kZoomFactor = 0.78F;
constexpr float kVerticalFieldOfViewDegrees = 34.0F;

class GlobeVectorOverlay final : public QWidget {
public:
    GlobeVectorOverlay(std::function<void(QPainter&)> paintFunction,
                       QWidget* parent)
        : QWidget(parent)
        , m_paintFunction(std::move(paintFunction))
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_TranslucentBackground);
        setAutoFillBackground(false);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        m_paintFunction(painter);
    }

private:
    std::function<void(QPainter&)> m_paintFunction;
};

QVector3D geoVector(double latitudeDegrees, double longitudeDegrees)
{
    const float latitude = qDegreesToRadians(
        static_cast<float>(latitudeDegrees));
    const float longitude = qDegreesToRadians(
        static_cast<float>(longitudeDegrees));
    const float latitudeCosine = std::cos(latitude);
    return { latitudeCosine * std::sin(longitude), std::sin(latitude),
             latitudeCosine * std::cos(longitude) };
}

QVector3D greatCircleAxis(const QVector3D& from, const QVector3D& to)
{
    QVector3D axis = QVector3D::crossProduct(from, to);
    if (axis.lengthSquared() < 0.000001F) {
        axis = QVector3D::crossProduct(from, { 0.0F, 1.0F, 0.0F });
    }
    if (axis.lengthSquared() < 0.000001F) {
        axis = QVector3D::crossProduct(from, { 1.0F, 0.0F, 0.0F });
    }
    return axis.normalized();
}

double mercatorTileLatitude(double tileY, int zoom)
{
    const double tileCount = static_cast<double>(1 << zoom);
    return qRadiansToDegrees(std::atan(std::sinh(
        M_PI * (1.0 - 2.0 * tileY / tileCount))));
}

double mercatorTileLongitude(double tileX, int zoom)
{
    return tileX / static_cast<double>(1 << zoom) * 360.0 - 180.0;
}

QVector4D normalizedRadarTextureBounds(const QRectF& bounds)
{
    const QRectF normalized = bounds.normalized();
    const double worldSize = 2.0 * kWebMercatorExtent;
    const float minimumU = static_cast<float>(
        (normalized.left() + kWebMercatorExtent) / worldSize);
    const float maximumU = static_cast<float>(
        (normalized.right() + kWebMercatorExtent) / worldSize);
    const float minimumV = static_cast<float>(
        (kWebMercatorExtent - normalized.bottom()) / worldSize);
    const float maximumV = static_cast<float>(
        (kWebMercatorExtent - normalized.top()) / worldSize);
    return { minimumU, minimumV, maximumU, maximumV };
}

std::unique_ptr<QOpenGLTexture> makeRadarTexture(const QImage& image)
{
    return WeatherRadarTexture::makeTexture(image);
}

bool radarTextureMatches(const std::unique_ptr<QOpenGLTexture>& texture,
                         const QImage& image)
{
    return texture != nullptr && !image.isNull()
        && texture->width() == image.width()
        && texture->height() == image.height();
}
}

GlobeMapView::GlobeMapView(QWidget* parent)
    : QOpenGLWidget(parent)
    , m_atlas(kAtlasSize, kAtlasSize, QImage::Format_RGBA8888)
    , m_weatherRadarAtlas(kAtlasSize, kAtlasSize, WeatherRadarTexture::kImageFormat)
    , m_pendingWeatherRadarAtlas(kAtlasSize, kAtlasSize,
                                 WeatherRadarTexture::kImageFormat)
{
    QSurfaceFormat surfaceFormat = format();
    surfaceFormat.setStencilBufferSize(8);
    setFormat(surfaceFormat);
    setObjectName(QStringLiteral("pskReporterGlobe"));
    setAccessibleName(tr("PSK Reporter globe"));
    setAccessibleDescription(tr(
        "Drag to rotate, Shift-drag or use left and right brackets to tilt "
        "the axis, pinch to zoom, and use Home to reset"));
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    grabGesture(Qt::PinchGesture);

    connect(this, &QOpenGLWidget::frameSwapped, this, [this] {
        if (m_pendingWeatherRadarPresentationSequence == 0) {
            return;
        }
        const quint64 sequence =
            m_pendingWeatherRadarPresentationSequence;
        m_pendingWeatherRadarPresentationSequence = 0;
        emit weatherRadarPlaybackPresented(sequence);
    });

    m_atlasUploadTimer.setSingleShot(true);
    m_atlasUploadTimer.setInterval(50);
    connect(&m_atlasUploadTimer, &QTimer::timeout, this, [this] {
        m_atlasDirty = true;
        update();
    });
    m_terminatorTimer.setInterval(60 * 1000);
    connect(&m_terminatorTimer, &QTimer::timeout,
            this, [this] { update(); });
    m_interactionSettleTimer.setSingleShot(true);
    m_interactionSettleTimer.setInterval(120);
    connect(&m_interactionSettleTimer, &QTimer::timeout,
            this, [this] { update(); });
    m_weatherRadarPlaybackViewTimer.setSingleShot(true);
    m_weatherRadarPlaybackViewTimer.setInterval(600);
    connect(&m_weatherRadarPlaybackViewTimer, &QTimer::timeout,
            this, [this] {
                emit imageOverlayViewChanged();
                if (m_weatherRadarPlaybackActive) {
                    emit weatherRadarPlaybackViewChanged();
                }
            });
    m_weatherRadarTransition = new QVariantAnimation(this);
    m_weatherRadarTransition->setDuration(kWeatherRadarTransitionMs);
    m_weatherRadarTransition->setStartValue(0.0);
    m_weatherRadarTransition->setEndValue(1.0);
    // Chained frame blends must maintain a constant temporal velocity.
    // Ease-in/out reaches zero velocity at every source frame and looks like
    // a pause even when the next transition starts immediately.
    m_weatherRadarTransition->setEasingCurve(QEasingCurve::Linear);
    connect(m_weatherRadarTransition, &QVariantAnimation::valueChanged, this,
            [this](const QVariant& value) {
                m_weatherRadarTransitionProgress = value.toFloat();
                update();
            });
    connect(m_weatherRadarTransition, &QVariantAnimation::finished, this,
            [this] {
                m_weatherRadarTransitionProgress = 1.0F;
                // QOpenGLTexture destruction needs a current context. Defer
                // release to paintGL(), where QOpenGLWidget guarantees one.
                m_releasePreviousRadarTextures = true;
                update();
                emit weatherRadarFrameLoaded(
                    m_weatherRadarSource.frameTime());
            });

    m_attribution = new QLabel(
        QStringLiteral("© <a href=\"https://www.openstreetmap.org/copyright\">OpenStreetMap</a> contributors"), this);
    m_attribution->setObjectName(QStringLiteral("globeMapAttribution"));
    m_attribution->setOpenExternalLinks(true);
    m_legend = new QLabel(this);
    m_legend->setTextFormat(Qt::RichText);
    m_legend->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_legend->hide();
    m_hoverCard = new QLabel(this);
    m_hoverCard->setObjectName(QStringLiteral("pskGlobeHoverCard"));
    m_hoverCard->setTextFormat(Qt::RichText);
    m_hoverCard->setWordWrap(false);
    m_hoverCard->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_hoverCard->hide();
    m_vectorOverlay = new GlobeVectorOverlay(
        [this](QPainter& painter) { paintVectorOverlay(painter); }, this);

    m_zoomInButton = makeOverlayButton(QStringLiteral("+"), tr("Zoom in"));
    m_zoomInButton->setObjectName(QStringLiteral("globeZoomInButton"));
    connect(m_zoomInButton, &QToolButton::clicked,
            this, &GlobeMapView::zoomIn);
    m_zoomOutButton = makeOverlayButton(QStringLiteral("−"), tr("Zoom out"));
    m_zoomOutButton->setObjectName(QStringLiteral("globeZoomOutButton"));
    connect(m_zoomOutButton, &QToolButton::clicked,
            this, &GlobeMapView::zoomOut);
    m_homeButton = makeOverlayButton(QStringLiteral("⌂"),
        tr("Reset to my location (Home)"));
    m_homeButton->setObjectName(QStringLiteral("globeHomeButton"));
    connect(m_homeButton, &QToolButton::clicked,
            this, &GlobeMapView::resetToHome);

    updateTheme();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged,
            this, [this] {
                updateTheme();
                update();
            });

    m_atlas.fill(m_backgroundColor);
    m_weatherRadarAtlas.fill(Qt::transparent);
    m_pendingWeatherRadarAtlas.fill(Qt::transparent);
    requestAtlasTiles();
}

GlobeMapView::~GlobeMapView()
{
    cancelTileRequests();
    cleanupOpenGlResources();
}

void GlobeMapView::cleanupOpenGlResources()
{
    if (m_cleaningOpenGlResources) {
        return;
    }
    m_cleaningOpenGlResources = true;
    QOpenGLContext* glContext = context();
    if (glContext == nullptr || !glContext->isValid()) {
        m_cleaningOpenGlResources = false;
        return;
    }
    // The QOpenGLWidget base destroys its context after this derived
    // destructor has run. Disconnect now so that late context teardown cannot
    // re-enter a GlobeMapView whose members have already been destroyed.
    disconnect(glContext, &QOpenGLContext::aboutToBeDestroyed,
               this, &GlobeMapView::cleanupOpenGlResources);
    makeCurrent();
    for (const std::shared_ptr<DetailTile>& tile :
         std::as_const(m_detailTiles)) {
        destroyDetailTile(*tile);
    }
    m_detailTiles.clear();
    m_texture.reset();
    m_cityLightsTexture.reset();
    m_cityLightsProgram.reset();
    m_radarTexture.reset();
    m_previousRadarTexture.reset();
    m_preloadedRadarTexture.reset();
    m_spareRadarTexture.reset();
    if (m_vertexBuffer.isCreated()) {
        m_vertexBuffer.destroy();
    }
    if (m_indexBuffer.isCreated()) {
        m_indexBuffer.destroy();
    }
    // QOpenGLShaderProgram owns a GL program object and must be destroyed
    // before doneCurrent(), just like the textures and buffers above.
    m_program.reset();
    m_radarProgram.reset();
    m_radarCoverageProgram.reset();
    if (m_radarCoverageBuffer.isCreated()) {
        m_radarCoverageBuffer.destroy();
    }
    m_radarCoverageDirty = true;
    m_indexCount = 0;
    m_overlayMatricesValid = false;
    doneCurrent();
    m_cleaningOpenGlResources = false;
}

void GlobeMapView::initializeGL()
{
    m_glInitializationAttempted = true;
    connect(context(), &QOpenGLContext::aboutToBeDestroyed,
            this, &GlobeMapView::cleanupOpenGlResources,
            Qt::DirectConnection);
    initializeOpenGLFunctions();
    glEnable(GL_DEPTH_TEST);

    m_program = std::make_unique<QOpenGLShaderProgram>();
    static constexpr char kVertexShader[] = R"(
        attribute highp vec3 position;
        attribute highp vec2 textureCoordinate;
        uniform highp mat4 matrix;
        varying highp vec2 uv;
        varying highp vec3 earthNormal;
        void main() {
            uv = textureCoordinate;
            earthNormal = position;
            gl_Position = matrix * vec4(position, 1.0);
        }
    )";
    static constexpr char kFragmentShader[] = R"(
        varying highp vec2 uv;
        varying highp vec3 earthNormal;
        uniform sampler2D atlas;
        uniform highp vec3 sunDirection;
        uniform lowp vec4 nightColor;
        uniform lowp float terminatorEnabled;
        uniform lowp float basemapBrightness;
        uniform lowp float darkBasemapEnabled;
        uniform lowp vec3 darkBasemapBackground;
        uniform lowp vec3 darkBasemapDetail;
        void main() {
            lowp vec4 mapColor = texture2D(atlas, uv);
            if (darkBasemapEnabled > 0.5) {
                highp float inverse = 1.0 - dot(mapColor.rgb,
                    vec3(0.2126, 0.7152, 0.0722));
                mapColor.rgb = mix(darkBasemapBackground, darkBasemapDetail,
                                    inverse);
            }
            mapColor.rgb *= basemapBrightness;
            highp float daylight = smoothstep(-0.018, 0.018,
                dot(normalize(earthNormal), normalize(sunDirection)));
            lowp float nightAmount = (1.0 - daylight)
                * terminatorEnabled * nightColor.a;
            gl_FragColor = vec4(mix(mapColor.rgb, nightColor.rgb,
                                    nightAmount), 1.0);
        }
    )";
    if (!m_program->addShaderFromSourceCode(QOpenGLShader::Vertex,
                                             kVertexShader)
        || !m_program->addShaderFromSourceCode(QOpenGLShader::Fragment,
                                                kFragmentShader)
        || !m_program->link()) {
        const QString shaderLog = m_program->log();
        m_program.reset();
        reportRendererUnavailable(
            tr("The globe renderer is unavailable because OpenGL shaders "
               "could not be initialized."),
            shaderLog);
        return;
    }
    static constexpr char kRadarVertexShader[] = R"(
        attribute highp vec3 position;
        attribute highp vec2 textureCoordinate;
        uniform highp mat4 matrix;
        varying highp vec2 uv;
        void main() {
            uv = textureCoordinate;
            gl_Position = matrix * vec4(position * 1.0004, 1.0);
        }
    )";
    static const QByteArray kRadarFragmentShader = QByteArray(R"(
        varying highp vec2 uv;
        uniform sampler2D atlas;
        uniform sampler2D previousAtlas;
        uniform lowp float opacity;
        uniform highp float transitionBlend;
        uniform highp vec4 atlasBounds;
        uniform highp vec4 previousAtlasBounds;
    )") + WeatherRadarTexture::kFragmentFunctions + R"(
        void main() {
            highp vec2 currentUv = (uv - atlasBounds.xy)
                / (atlasBounds.zw - atlasBounds.xy);
            highp vec2 previousUv = (uv - previousAtlasBounds.xy)
                / (previousAtlasBounds.zw - previousAtlasBounds.xy);

            // Each texture keeps its own geographic bounds. Never displace
            // observations: playback presents original NOAA images only.
            gl_FragColor = radarComposite(
                radarSample(previousAtlas, previousUv),
                radarSample(atlas, currentUv),
                transitionBlend, opacity);
        }
    )";
    m_radarProgram = std::make_unique<QOpenGLShaderProgram>();
    if (!m_radarProgram->addShaderFromSourceCode(QOpenGLShader::Vertex,
                                                  kRadarVertexShader)
        || !m_radarProgram->addShaderFromSourceCode(
            QOpenGLShader::Fragment, kRadarFragmentShader)
        || !m_radarProgram->link()) {
        const QString shaderLog = m_radarProgram->log();
        m_radarProgram.reset();
        reportRendererUnavailable(
            tr("The globe renderer is unavailable because its weather "
               "overlay shader could not be initialized."),
            shaderLog);
        return;
    }
    buildSphereMesh();
    m_atlasDirty = true;
}

void GlobeMapView::buildSphereMesh()
{
    QVector<Vertex> vertices;
    vertices.reserve((kLatitudeSegments + 1)
                     * (kLongitudeSegments + 1));
    for (int latitudeIndex = 0; latitudeIndex <= kLatitudeSegments;
         ++latitudeIndex) {
        const double latitude = -90.0
            + 180.0 * latitudeIndex / kLatitudeSegments;
        const double mercatorLatitude = std::clamp(latitude, -85.05112878,
                                                   85.05112878);
        const double mercatorRadians = qDegreesToRadians(mercatorLatitude);
        const float v = static_cast<float>((1.0
            - std::asinh(std::tan(mercatorRadians)) / M_PI) / 2.0);
        for (int longitudeIndex = 0;
             longitudeIndex <= kLongitudeSegments; ++longitudeIndex) {
            const double longitude = -180.0
                + 360.0 * longitudeIndex / kLongitudeSegments;
            const float u = static_cast<float>(longitudeIndex)
                / kLongitudeSegments;
            vertices.append({ geoVector(latitude, longitude), { u, v } });
        }
    }

    QVector<quint32> indices;
    indices.reserve(kLatitudeSegments * kLongitudeSegments * 6);
    const int rowWidth = kLongitudeSegments + 1;
    for (int y = 0; y < kLatitudeSegments; ++y) {
        for (int x = 0; x < kLongitudeSegments; ++x) {
            const quint32 topLeft = static_cast<quint32>(y * rowWidth + x);
            const quint32 bottomLeft = topLeft + rowWidth;
            indices.append(topLeft);
            indices.append(bottomLeft);
            indices.append(topLeft + 1);
            indices.append(topLeft + 1);
            indices.append(bottomLeft);
            indices.append(bottomLeft + 1);
        }
    }
    m_indexCount = indices.size();

    m_vertexBuffer.create();
    m_vertexBuffer.bind();
    m_vertexBuffer.allocate(vertices.constData(),
                            vertices.size() * sizeof(Vertex));
    m_vertexBuffer.release();
    m_indexBuffer.create();
    m_indexBuffer.bind();
    m_indexBuffer.allocate(indices.constData(),
                           indices.size() * sizeof(quint32));
    m_indexBuffer.release();
}

void GlobeMapView::resizeGL(int width, int height)
{
    glViewport(0, 0, width, height);
    m_detailSelectionDirty = true;
}

void GlobeMapView::paintGL()
{
    if (m_releasePlaybackRadarTextures) {
        // Stop keeps the currently displayed frame in place until the live
        // NOAA replacement is ready, but every inactive playback texture can
        // be released now that QOpenGLWidget has made the context current.
        m_previousRadarTexture.reset();
        m_preloadedRadarTexture.reset();
        m_spareRadarTexture.reset();
        m_releasePreviousRadarTextures = false;
        m_releasePlaybackRadarTextures = false;
    }
    if (m_releasePreviousRadarTextures) {
        // Keep the full-globe texture as a reusable back buffer during
        // playback. Reallocating a 4096x4096 texture at every boundary was
        // the largest remaining cadence stall. Detail-tile textures are live
        // only, so their completed-transition copies can still be released.
        if (!m_weatherRadarPlaybackActive) {
            m_previousRadarTexture.reset();
        }
        for (const std::shared_ptr<DetailTile>& tile :
             std::as_const(m_detailTiles)) {
            tile->previousRadarTexture.reset();
        }
        m_releasePreviousRadarTextures = false;
    }
    glClearColor(m_backgroundColor.redF(), m_backgroundColor.greenF(),
                 m_backgroundColor.blueF(), 1.0F);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (m_program == nullptr || m_indexCount == 0) {
        return;
    }
    if (m_atlasDirty || m_texture == nullptr) {
        uploadAtlas();
    }
    if (m_texture == nullptr) {
        return;
    }

    QMatrix4x4 projection;
    const float aspect = height() > 0
        ? static_cast<float>(width()) / static_cast<float>(height()) : 1.0F;
    // At maximum zoom the globe surface is only 0.08 units from the camera.
    // A small near plane prevents the foreground from being clipped.
    projection.perspective(kVerticalFieldOfViewDegrees, aspect, 0.015F, 20.0F);
    QMatrix4x4 view;
    view.lookAt({ 0.0F, 0.0F, m_cameraDistance }, { 0.0F, 0.0F, 0.0F },
                { 0.0F, 1.0F, 0.0F });
    QMatrix4x4 model;
    model.rotate(m_navigation.rotation());
    const QMatrix4x4 viewProjection = projection * view;
    const QMatrix4x4 matrix = viewProjection * model;
    const bool vectorOverlayTransformChanged = !m_overlayMatricesValid
        || m_overlayModel != model
        || m_overlayViewProjection != viewProjection;
    m_overlayModel = model;
    m_overlayViewProjection = viewProjection;
    m_overlayMatricesValid = true;

    m_program->bind();
    m_program->setUniformValue("matrix", matrix);
    const SolarTerminator::Position sun = SolarTerminator::positionAt(
        QDateTime::currentDateTimeUtc());
    m_program->setUniformValue("sunDirection", geoVector(
        qRadiansToDegrees(sun.declinationRad),
        qRadiansToDegrees(sun.subsolarLonRad)));
    const QColor night = m_basemapDarkEnabled
        ? BasemapStyle::nightColor(m_basemapBackground) : m_nightColor;
    m_program->setUniformValue("nightColor", QVector4D(
        night.redF(), night.greenF(), night.blueF(), 0.62F));
    m_program->setUniformValue("terminatorEnabled",
                               m_terminatorVisible ? 1.0F : 0.0F);
    m_program->setUniformValue("darkBasemapEnabled", m_basemapDarkEnabled ? 1.0F : 0.0F);
    m_program->setUniformValue("darkBasemapBackground", QVector3D(
        m_basemapBackground.redF(), m_basemapBackground.greenF(), m_basemapBackground.blueF()));
    m_program->setUniformValue("darkBasemapDetail", QVector3D(
        m_basemapDetail.redF(), m_basemapDetail.greenF(), m_basemapDetail.blueF()));
    m_program->setUniformValue("basemapBrightness", m_basemapBrightness / 100.0F);
    m_program->setUniformValue("atlas", 0);
    m_texture->bind(0);
    m_vertexBuffer.bind();
    m_indexBuffer.bind();
    const int positionLocation = m_program->attributeLocation("position");
    const int uvLocation = m_program->attributeLocation("textureCoordinate");
    m_program->enableAttributeArray(positionLocation);
    m_program->setAttributeBuffer(positionLocation, GL_FLOAT,
        offsetof(Vertex, position), 3, sizeof(Vertex));
    m_program->enableAttributeArray(uvLocation);
    m_program->setAttributeBuffer(uvLocation, GL_FLOAT,
        offsetof(Vertex, uv), 2, sizeof(Vertex));
    glDrawElements(GL_TRIANGLES, m_indexCount, GL_UNSIGNED_INT, nullptr);
    m_indexBuffer.release();
    m_vertexBuffer.release();
    m_texture->release();

    if (!useInteractionPreview() && m_detailSelectionDirty) {
        refreshDetailTiles(model, viewProjection);
    }
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(-1.0F, -1.0F);
    for (const QString& key : std::as_const(m_visibleDetailKeys)) {
        const auto found = m_detailTiles.find(key);
        if (found == m_detailTiles.end()) {
            continue;
        }
        DetailTile& tile = **found;
        tile.lastUsedFrame = m_detailFrame;
        if (tile.texture == nullptr && !tile.image.isNull()) {
            uploadDetailTile(tile);
        }
        if (tile.texture == nullptr || tile.indexCount == 0) {
            continue;
        }
        tile.texture->bind(0);
        tile.vertexBuffer.bind();
        tile.indexBuffer.bind();
        m_program->setAttributeBuffer(positionLocation, GL_FLOAT,
            offsetof(Vertex, position), 3, sizeof(Vertex));
        m_program->setAttributeBuffer(uvLocation, GL_FLOAT,
            offsetof(Vertex, uv), 2, sizeof(Vertex));
        glDrawElements(GL_TRIANGLES, tile.indexCount, GL_UNSIGNED_INT,
                       nullptr);
        tile.indexBuffer.release();
        tile.vertexBuffer.release();
        tile.texture->release();
    }
    glDisable(GL_POLYGON_OFFSET_FILL);
    m_program->disableAttributeArray(positionLocation);
    m_program->disableAttributeArray(uvLocation);
    m_program->release();

    drawCityLights(matrix);

    if (m_weatherRadarVisible && m_radarProgram != nullptr) {
        if (m_pendingWeatherRadarPlaybackFrameDirty) {
            uploadPendingWeatherRadarPlaybackFrame();
        } else if (!m_weatherRadarPlaybackActive
                   && (m_weatherRadarAtlasDirty || m_radarTexture == nullptr)) {
            uploadWeatherRadarAtlas();
        }
        if (m_preloadedWeatherRadarAtlasDirty) {
            uploadPreloadedWeatherRadarAtlas();
        }
        if (m_radarTexture != nullptr) {
            // Radar is a composited map overlay, not another terrain surface.
            // The coarse globe triangles sag below finer basemap triangles;
            // a small radial/GL polygon offset cannot reliably overcome that
            // geometric difference. Depth-testing against the basemap cuts
            // triangular holes (or hides a whole patch) as detail loads.
            // Draw over terrain, but cull the far hemisphere so transparent
            // near-side weather cannot expose rain on the back of the globe.
            glDisable(GL_DEPTH_TEST);
            glEnable(GL_CULL_FACE);
            glCullFace(GL_BACK);
            // Global mesh rows run south->north, hence clockwise front faces.
            glFrontFace(GL_CW);
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            glEnable(GL_POLYGON_OFFSET_FILL);
            glPolygonOffset(-2.0F, -2.0F);
            m_radarProgram->bind();
            m_radarProgram->setUniformValue("matrix", matrix);
            m_radarProgram->setUniformValue("atlas", 0);
            m_radarProgram->setUniformValue("previousAtlas", 1);
            m_radarProgram->setUniformValue("opacity", float(kWeatherRadarOpacity));
            m_radarProgram->setUniformValue(
                "transitionBlend", m_weatherRadarTransitionProgress);
            m_radarProgram->setUniformValue(
                "atlasBounds", m_weatherRadarTextureBounds);
            m_radarProgram->setUniformValue(
                "previousAtlasBounds", m_previousRadarTextureBounds);
            m_radarTexture->bind(0);
            QOpenGLTexture* previousAtlas = m_previousRadarTexture != nullptr
                ? m_previousRadarTexture.get() : m_radarTexture.get();
            previousAtlas->bind(1);
            m_vertexBuffer.bind();
            m_indexBuffer.bind();
            const int radarPosition = m_radarProgram->attributeLocation(
                "position");
            const int radarUv = m_radarProgram->attributeLocation(
                "textureCoordinate");
            m_radarProgram->enableAttributeArray(radarPosition);
            m_radarProgram->setAttributeBuffer(radarPosition, GL_FLOAT,
                offsetof(Vertex, position), 3, sizeof(Vertex));
            m_radarProgram->enableAttributeArray(radarUv);
            m_radarProgram->setAttributeBuffer(radarUv, GL_FLOAT,
                offsetof(Vertex, uv), 2, sizeof(Vertex));
            glDrawElements(GL_TRIANGLES, m_indexCount, GL_UNSIGNED_INT,
                           nullptr);

            glPolygonOffset(-3.0F, -3.0F);
            // XYZ detail rows instead run north->south: opposite winding.
            glFrontFace(GL_CCW);
            const QVector<QString> radarDetailKeys =
                m_weatherRadarPlaybackActive
                    ? QVector<QString>{} : m_visibleDetailKeys;
            for (const QString& key : radarDetailKeys) {
                const auto found = m_detailTiles.find(key);
                if (found == m_detailTiles.end()) {
                    continue;
                }
                DetailTile& tile = **found;
                if (!tile.radarImage.isNull()
                    && tile.radarFrameId == m_loadedWeatherRadarFrameId) {
                    uploadWeatherRadarDetailTile(tile);
                }
                if (tile.radarTexture == nullptr || tile.indexCount == 0) {
                    continue;
                }
                tile.radarTexture->bind(0);
                QOpenGLTexture* previousTile =
                    tile.previousRadarTexture != nullptr
                        ? tile.previousRadarTexture.get()
                        : tile.radarTexture.get();
                previousTile->bind(1);
                m_radarProgram->setUniformValue(
                    "atlasBounds", QVector4D(0.0F, 0.0F, 1.0F, 1.0F));
                m_radarProgram->setUniformValue(
                    "previousAtlasBounds",
                    QVector4D(0.0F, 0.0F, 1.0F, 1.0F));
                tile.vertexBuffer.bind();
                tile.indexBuffer.bind();
                m_radarProgram->setAttributeBuffer(radarPosition, GL_FLOAT,
                    offsetof(Vertex, position), 3, sizeof(Vertex));
                m_radarProgram->setAttributeBuffer(radarUv, GL_FLOAT,
                    offsetof(Vertex, uv), 2, sizeof(Vertex));
                glDrawElements(GL_TRIANGLES, tile.indexCount,
                               GL_UNSIGNED_INT, nullptr);
                tile.indexBuffer.release();
                tile.vertexBuffer.release();
                previousTile->release(1);
                tile.radarTexture->release(0);
            }
            m_radarProgram->disableAttributeArray(radarPosition);
            m_radarProgram->disableAttributeArray(radarUv);
            m_indexBuffer.release();
            m_vertexBuffer.release();
            previousAtlas->release(1);
            m_radarTexture->release(0);
            m_radarProgram->release();
            glDisable(GL_POLYGON_OFFSET_FILL);
            glDisable(GL_BLEND);
            glDisable(GL_CULL_FACE);
            glEnable(GL_DEPTH_TEST);
        }
    }
    drawRadarCoverage(matrix);
    if (vectorOverlayTransformChanged || m_vectorOverlayDirty) {
        m_vectorOverlay->update();
        m_vectorOverlayDirty = false;
    }
}

void GlobeMapView::setRadarSites(const QVector<RadarSite>& sites, bool visible)
{
    m_radarSites = sites;
    m_radarCoverageVisible = visible;
    if (m_hoverMarker < 0) {
        m_hoverCard->hide();
    }
    m_radarCoverageVertices.clear();
    for (const RadarSite& site : sites) {
        const QVector3D center = geoPoint(site.lat, site.lon);
        for (qsizetype i = 1; i < site.ring.size(); ++i) {
            m_radarCoverageVertices.append(center);
            m_radarCoverageVertices.append(geoPoint(site.ring[i - 1].y(), site.ring[i - 1].x()));
            m_radarCoverageVertices.append(geoPoint(site.ring[i].y(), site.ring[i].x()));
        }
    }
    m_radarCoverageDirty = true;
    m_vectorOverlayDirty = true;
    updateMapAttribution();
    update();
}

void GlobeMapView::paintVectorOverlay(QPainter& painter)
{
    if (!m_overlayMatricesValid) {
        return;
    }
    paintPaths(painter, m_overlayModel, m_overlayViewProjection);
    paintMarkers(painter, m_overlayModel, m_overlayViewProjection);
}

void GlobeMapView::drawRadarCoverage(const QMatrix4x4& matrix)
{
    if (!m_radarCoverageVisible || m_radarCoverageVertices.isEmpty()) {
        return;
    }
    if (m_radarCoverageProgram == nullptr) {
        m_radarCoverageProgram = std::make_unique<QOpenGLShaderProgram>();
        static constexpr char vertex[] = R"(
            attribute highp vec3 position;
            uniform highp mat4 matrix;
            void main() { gl_Position = matrix * vec4(position, 1.0); }
        )";
        static constexpr char fragment[] = R"(
            uniform lowp vec4 coverageColor;
            void main() { gl_FragColor = coverageColor; }
        )";
        if (!m_radarCoverageProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, vertex)
            || !m_radarCoverageProgram->addShaderFromSourceCode(QOpenGLShader::Fragment, fragment)
            || !m_radarCoverageProgram->link()) {
            qCWarning(lcPskReporterGlobe) << "Radar coverage shader:" << m_radarCoverageProgram->log();
            return;
        }
    }
    if (!m_radarCoverageProgram->isLinked()) {
        return;
    }
    if (!m_radarCoverageBuffer.isCreated()) {
        m_radarCoverageBuffer.create();
        m_radarCoverageDirty = true;
    }
    m_radarCoverageBuffer.bind();
    if (m_radarCoverageDirty) {
        m_radarCoverageBuffer.allocate(m_radarCoverageVertices.constData(),
            int(m_radarCoverageVertices.size() * sizeof(QVector3D)));
        m_radarCoverageDirty = false;
    }
    // Keep coverage in the same frame and transform as the globe. The site
    // geometry stays resident during navigation; only the matrix changes.
    // Stencil shades each covered pixel once, including overlapping sites.
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CW);
    glStencilMask(0xff);
    glClearStencil(0);
    glClear(GL_STENCIL_BUFFER_BIT);
    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_EQUAL, 0, 0xff);
    glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    m_radarCoverageProgram->bind();
    m_radarCoverageProgram->setUniformValue("matrix", matrix);
    const QColor color = ThemeManager::instance().color("color.text.secondary");
    constexpr float alpha = 12.0F / 255.0F;
    m_radarCoverageProgram->setUniformValue("coverageColor", QVector4D(
        color.redF() * alpha, color.greenF() * alpha, color.blueF() * alpha, alpha));
    const int position = m_radarCoverageProgram->attributeLocation("position");
    m_radarCoverageProgram->enableAttributeArray(position);
    m_radarCoverageProgram->setAttributeBuffer(position, GL_FLOAT, 0, 3, sizeof(QVector3D));
    glDrawArrays(GL_TRIANGLES, 0, int(m_radarCoverageVertices.size()));
    m_radarCoverageProgram->disableAttributeArray(position);
    m_radarCoverageProgram->release();
    m_radarCoverageBuffer.release();
    glDisable(GL_BLEND);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glFrontFace(GL_CCW);
    glEnable(GL_DEPTH_TEST);
}

void GlobeMapView::uploadAtlas()
{
    m_texture.reset();
    // Basemap refreshes must retain the independent city-light GPU resources.
    m_texture = std::make_unique<QOpenGLTexture>(m_atlas);
    m_texture->setMinificationFilter(QOpenGLTexture::LinearMipMapLinear);
    m_texture->setMagnificationFilter(QOpenGLTexture::Linear);
    m_texture->setWrapMode(QOpenGLTexture::DirectionS,
                           QOpenGLTexture::Repeat);
    m_texture->setWrapMode(QOpenGLTexture::DirectionT,
                           QOpenGLTexture::ClampToEdge);
    m_texture->generateMipMaps();
    m_atlasDirty = false;
}

void GlobeMapView::uploadWeatherRadarAtlas()
{
    if (m_replaceWeatherRadarTexture) {
        // A regional playback export cannot inherit the full-world live
        // texture as its blend source. Replace it atomically inside paintGL,
        // where the OpenGL context is current, while keeping the old texture
        // alive until the replacement has been created.
        std::unique_ptr<QOpenGLTexture> nextTexture = makeRadarTexture(m_weatherRadarAtlas);
        if (nextTexture == nullptr) {
            return;
        }
        m_spareRadarTexture = std::move(m_radarTexture);
        m_previousRadarTexture.reset();
        m_radarTexture = std::move(nextTexture);
        m_radarTextureImageKey = 0; // Live atlases are not playback observations.
        m_previousRadarTextureFrameTime = {};
        m_radarTextureFrameTime = m_weatherRadarSource.frameTime();
        m_weatherRadarTextureBounds = m_pendingRadarTextureBounds;
        m_previousRadarTextureBounds = m_weatherRadarTextureBounds;
        m_radarTexture->setMinificationFilter(QOpenGLTexture::Linear);
        m_radarTexture->setMagnificationFilter(QOpenGLTexture::Linear);
        m_radarTexture->setWrapMode(QOpenGLTexture::DirectionS,
                                    QOpenGLTexture::ClampToEdge);
        m_radarTexture->setWrapMode(QOpenGLTexture::DirectionT,
                                    QOpenGLTexture::ClampToEdge);
        m_weatherRadarTransitionProgress = 1.0F;
        m_weatherRadarAtlasDirty = false;
        m_replaceWeatherRadarTexture = false;
        QTimer::singleShot(0, this, [this] {
            emit weatherRadarFrameLoaded(m_weatherRadarSource.frameTime());
        });
        return;
    }
    const bool hasCurrentFrame = m_radarTexture != nullptr;
    const QDateTime oldCurrentFrameTime = m_radarTextureFrameTime;
    if (hasCurrentFrame && m_previousRadarTexture != nullptr
        && m_weatherRadarPlaybackActive
        && m_previousRadarTexture->width() == m_weatherRadarAtlas.width()
        && m_previousRadarTexture->height()
            == m_weatherRadarAtlas.height()) {
        // Upload into the inactive texture's existing storage, then make the
        // old front texture the blend source. On this Mac that removes the
        // per-frame allocation bubble while preserving the 4096 atlas.
        // Use the raw level-zero overload. The QImage convenience overload
        // also tries to redefine format, size, and mip levels, which forces
        // validation warnings on immutable storage and defeats fast reuse.
        if (!WeatherRadarTexture::uploadRows(*m_previousRadarTexture,
                m_weatherRadarAtlas, 0, m_weatherRadarAtlas.height())) {
            return;
        }
        std::swap(m_radarTexture, m_previousRadarTexture);
    } else {
        std::unique_ptr<QOpenGLTexture> nextTexture = WeatherRadarTexture::makeTexture(
            m_weatherRadarAtlas, !m_weatherRadarPlaybackActive);
        if (nextTexture == nullptr) {
            return;
        }
        if (hasCurrentFrame) {
            m_previousRadarTexture = std::move(m_radarTexture);
        }
        m_radarTexture = std::move(nextTexture);
    }
    m_previousRadarTextureBounds = m_weatherRadarTextureBounds;
    m_radarTextureImageKey = 0;
    m_weatherRadarTextureBounds = m_pendingRadarTextureBounds;
    m_previousRadarTextureFrameTime = hasCurrentFrame
        ? oldCurrentFrameTime : QDateTime{};
    m_radarTextureFrameTime = m_weatherRadarSource.frameTime();
    m_radarTexture->setMinificationFilter(m_weatherRadarPlaybackActive
        ? QOpenGLTexture::Linear : QOpenGLTexture::LinearMipMapLinear);
    m_radarTexture->setMagnificationFilter(QOpenGLTexture::Linear);
    m_radarTexture->setWrapMode(QOpenGLTexture::DirectionS,
                                QOpenGLTexture::Repeat);
    m_radarTexture->setWrapMode(QOpenGLTexture::DirectionT,
                                QOpenGLTexture::ClampToEdge);
    // Generating a complete 4096x4096 mip chain on every playback
    // boundary stalls the render thread. Live radar keeps mipmaps for the
    // best stationary zoom quality; animation uses the full-resolution
    // texture with linear sampling and avoids that per-frame GPU bubble.
    // Live mipmaps were already allocated and generated by makeTexture().
    m_weatherRadarAtlasDirty = false;
    if (hasCurrentFrame && !m_weatherRadarPlaybackActive) {
        m_weatherRadarTransition->stop();
        m_weatherRadarTransitionProgress = 0.0F;
        m_weatherRadarTransition->start();
    } else if (hasCurrentFrame) {
        m_weatherRadarTransitionProgress = 0.0F;
    } else {
        m_weatherRadarTransitionProgress = 1.0F;
        QTimer::singleShot(0, this, [this] {
            emit weatherRadarFrameLoaded(m_weatherRadarSource.frameTime());
        });
    }
}

void GlobeMapView::uploadPendingWeatherRadarPlaybackFrame()
{
    if (!m_pendingWeatherRadarPlaybackFrameDirty
        || m_pendingCurrentPlaybackRadarAtlas.isNull()) {
        m_pendingWeatherRadarPlaybackFrameDirty = false;
        return;
    }

    const QVector4D bounds = m_pendingRadarTextureBounds;
    std::unique_ptr<QOpenGLTexture> oldCurrent =
        std::move(m_radarTexture);
    std::unique_ptr<QOpenGLTexture> oldPrevious =
        std::move(m_previousRadarTexture);
    const QDateTime oldCurrentTime = m_radarTextureFrameTime;
    const QVector4D oldCurrentBounds = m_weatherRadarTextureBounds;

    std::unique_ptr<QOpenGLTexture> nextCurrent;
    if (m_preloadedRadarTexture != nullptr
        && !m_preloadedWeatherRadarAtlasDirty
        && m_preloadedRadarImageKey == m_pendingCurrentPlaybackRadarAtlas.cacheKey()
        && m_preloadedWeatherRadarFrameTime
            == m_pendingCurrentPlaybackFrameTime
        && m_preloadedRadarTextureBounds == bounds
        && radarTextureMatches(m_preloadedRadarTexture,
                               m_pendingCurrentPlaybackRadarAtlas)) {
        nextCurrent = std::move(m_preloadedRadarTexture);
    } else if (m_radarTextureImageKey == m_pendingCurrentPlaybackRadarAtlas.cacheKey()
               && oldCurrentTime == m_pendingCurrentPlaybackFrameTime
               && oldCurrentBounds == bounds
               && radarTextureMatches(
                   oldCurrent, m_pendingCurrentPlaybackRadarAtlas)) {
        nextCurrent = std::move(oldCurrent);
    } else {
        nextCurrent = makeRadarTexture(
            m_pendingCurrentPlaybackRadarAtlas);
    }

    if (nextCurrent == nullptr) {
        // Allocation/upload failure is not a transparent NOAA observation.
        // Keep the last complete front texture and retry on a later paint.
        m_radarTexture = std::move(oldCurrent);
        m_previousRadarTexture = std::move(oldPrevious);
        return;
    }

    if (oldCurrent != nullptr) {
        m_spareRadarTexture = std::move(oldCurrent);
    } else if (oldPrevious != nullptr) {
        m_spareRadarTexture = std::move(oldPrevious);
    }
    m_previousRadarTexture.reset();
    m_radarTexture = std::move(nextCurrent);
    m_radarTextureImageKey = m_pendingCurrentPlaybackRadarAtlas.cacheKey();
    m_previousRadarTextureFrameTime = {};
    m_radarTextureFrameTime = m_pendingCurrentPlaybackFrameTime;
    m_previousRadarTextureBounds = bounds;
    m_weatherRadarTextureBounds = bounds;
    m_weatherRadarTransitionProgress = 1.0F;

    const QDateTime firstVisibleFrame = m_pendingCurrentPlaybackFrameTime;
    m_pendingCurrentPlaybackRadarAtlas = {};
    m_pendingCurrentPlaybackFrameTime = {};
    m_pendingWeatherRadarPlaybackFrameDirty = false;
    m_weatherRadarAtlasDirty = false;
    m_replaceWeatherRadarTexture = false;
    m_preloadedWeatherRadarAtlas = {};
    m_preloadedWeatherRadarFrameTime = {};
    m_preloadedRadarImageKey = 0;
    m_preloadedWeatherRadarAtlasDirty = false;
    m_preloadedWeatherRadarUploadRow = 0;

    // Start the clock only after the original observation is resident.
    // Keep the old texture alive until then, including at loop restart.
    QTimer::singleShot(0, this, [this, firstVisibleFrame] {
        emit weatherRadarFrameLoaded(firstVisibleFrame);
    });
}

void GlobeMapView::uploadPreloadedWeatherRadarAtlas()
{
    if (m_preloadedWeatherRadarAtlas.isNull()) {
        m_preloadedWeatherRadarAtlasDirty = false;
        return;
    }
    if (m_preloadedRadarTexture != nullptr
        && (m_preloadedRadarTexture->width()
                != m_preloadedWeatherRadarAtlas.width()
            || m_preloadedRadarTexture->height()
                != m_preloadedWeatherRadarAtlas.height())) {
        // paintGL() owns the current context, so it is safe to discard
        // incompatible storage here.
        m_preloadedRadarTexture.reset();
    }
    if (m_preloadedRadarTexture == nullptr) {
        if (m_spareRadarTexture != nullptr
            && m_spareRadarTexture->width()
                == m_preloadedWeatherRadarAtlas.width()
            && m_spareRadarTexture->height()
                == m_preloadedWeatherRadarAtlas.height()) {
            m_preloadedRadarTexture = std::move(m_spareRadarTexture);
        } else {
            m_spareRadarTexture.reset();
            m_preloadedRadarTexture = std::make_unique<QOpenGLTexture>(
                QOpenGLTexture::Target2D);
            m_preloadedRadarTexture->setFormat(
                QOpenGLTexture::RGBA8_UNorm);
            m_preloadedRadarTexture->setSize(
                m_preloadedWeatherRadarAtlas.width(),
                m_preloadedWeatherRadarAtlas.height());
            m_preloadedRadarTexture->setMipLevels(1);
            m_preloadedRadarTexture->allocateStorage(
                QOpenGLTexture::RGBA, QOpenGLTexture::UInt8);
        }
        m_preloadedRadarTexture->setMinificationFilter(
            QOpenGLTexture::Linear);
        m_preloadedRadarTexture->setMagnificationFilter(
            QOpenGLTexture::Linear);
        m_preloadedRadarTexture->setWrapMode(
            QOpenGLTexture::DirectionS, QOpenGLTexture::ClampToEdge);
        m_preloadedRadarTexture->setWrapMode(
            QOpenGLTexture::DirectionT, QOpenGLTexture::ClampToEdge);
    }
    const int remainingRows = m_preloadedWeatherRadarAtlas.height()
                            - m_preloadedWeatherRadarUploadRow;
    const int rows = std::min(kRadarUploadStripeRows, remainingRows);
    if (!WeatherRadarTexture::uploadRows(*m_preloadedRadarTexture,
            m_preloadedWeatherRadarAtlas, m_preloadedWeatherRadarUploadRow, rows)) {
        return;
    }
    m_preloadedWeatherRadarUploadRow += rows;
    if (m_preloadedWeatherRadarUploadRow
        >= m_preloadedWeatherRadarAtlas.height()) {
        const QDateTime preloadedFrameTime =
            m_preloadedWeatherRadarFrameTime;
        m_preloadedWeatherRadarAtlas = {};
        m_preloadedWeatherRadarAtlasDirty = false;
        m_preloadedWeatherRadarUploadRow = 0;
        emit weatherRadarPlaybackFramePreloaded(preloadedFrameTime);
    } else {
        // Upload one bounded stripe per presentation. This prevents a 64 MiB
        // texture transfer from freezing the animation clock mid-crossfade.
        update();
    }
}

int GlobeMapView::detailZoomLevel() const
{
    if (m_cameraDistance <= 1.28F) {
        return 7;
    }
    if (m_cameraDistance <= 1.68F) {
        return 6;
    }
    if (m_cameraDistance <= 2.05F) {
        return 5;
    }
    if (m_cameraDistance <= 2.65F) {
        return 4;
    }
    if (m_cameraDistance <= 3.35F) {
        return 3;
    }
    return kAtlasZoom;
}

QString GlobeMapView::detailTileKey(int zoom, int x, int y)
{
    return QStringLiteral("%1/%2/%3").arg(zoom).arg(x).arg(y);
}

bool GlobeMapView::detailTileVisible(
    int zoom, int x, int y, const QMatrix4x4& model,
    const QMatrix4x4& viewProjection, QPointF* priorityPoint) const
{
    const QPointF viewportCenter(width() * 0.5, height() * 0.5);
    double bestDistance = std::numeric_limits<double>::max();
    QPointF bestPoint;
    bool visible = false;
    for (int sampleY = 0; sampleY <= 2; ++sampleY) {
        const double tileY = y + sampleY * 0.5;
        const double latitude = mercatorTileLatitude(tileY, zoom);
        for (int sampleX = 0; sampleX <= 2; ++sampleX) {
            const double tileX = x + sampleX * 0.5;
            const double longitude = mercatorTileLongitude(tileX, zoom);
            QPointF screenPoint;
            if (!projectPoint(geoPoint(latitude, longitude), model,
                              viewProjection, &screenPoint)) {
                continue;
            }
            constexpr double kViewportMargin = 48.0;
            if (screenPoint.x() < -kViewportMargin
                || screenPoint.x() > width() + kViewportMargin
                || screenPoint.y() < -kViewportMargin
                || screenPoint.y() > height() + kViewportMargin) {
                continue;
            }
            visible = true;
            const QPointF delta = screenPoint - viewportCenter;
            const double distance = QPointF::dotProduct(delta, delta);
            if (distance < bestDistance) {
                bestDistance = distance;
                bestPoint = screenPoint;
            }
        }
    }
    if (visible && priorityPoint != nullptr) {
        *priorityPoint = bestPoint;
    }
    return visible;
}

void GlobeMapView::refreshDetailTiles(
    const QMatrix4x4& model, const QMatrix4x4& viewProjection)
{
    struct Candidate {
        int x{0};
        int y{0};
        double priority{0.0};
    };

    ++m_detailFrame;
    m_detailSelectionDirty = false;
    for (const TileRequest& request : std::as_const(m_pendingTiles)) {
        if (request.baseAtlas) {
            continue;
        }
        const auto found = m_detailTiles.find(detailTileKey(
            request.zoom, request.x, request.y));
        if (found != m_detailTiles.end()) {
            // This request had not started yet (active requests have already
            // been removed from the queue). Make it eligible for the newly
            // selected viewport or for cache eviction.
            if (request.weatherRadar) {
                (*found)->radarLoading = false;
            } else {
                (*found)->loading = false;
            }
        }
    }
    m_pendingTiles.erase(std::remove_if(m_pendingTiles.begin(),
                                        m_pendingTiles.end(),
        [](const TileRequest& request) { return !request.baseAtlas; }),
        m_pendingTiles.end());

    const int zoom = detailZoomLevel();
    if (zoom <= kAtlasZoom) {
        m_visibleDetailKeys.clear();
        evictDetailTiles();
        return;
    }

    QVector<Candidate> candidates;
    const int tileCount = 1 << zoom;
    const QPointF viewportCenter(width() * 0.5, height() * 0.5);
    for (int y = 0; y < tileCount; ++y) {
        for (int x = 0; x < tileCount; ++x) {
            QPointF priorityPoint;
            if (!detailTileVisible(zoom, x, y, model, viewProjection,
                                   &priorityPoint)) {
                continue;
            }
            const QPointF delta = priorityPoint - viewportCenter;
            candidates.append({ x, y,
                QPointF::dotProduct(delta, delta) });
        }
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& lhs, const Candidate& rhs) {
                  return lhs.priority < rhs.priority;
              });
    if (candidates.size() > kMaximumVisibleDetailTiles) {
        candidates.resize(kMaximumVisibleDetailTiles);
    }

    m_visibleDetailKeys.clear();
    m_visibleDetailKeys.reserve(candidates.size());
    for (const Candidate& candidate : std::as_const(candidates)) {
        const QString key = detailTileKey(zoom, candidate.x, candidate.y);
        m_visibleDetailKeys.append(key);
        const auto existing = m_detailTiles.find(key);
        if (existing != m_detailTiles.end()) {
            (*existing)->lastUsedFrame = m_detailFrame;
            if ((*existing)->texture == nullptr
                && (*existing)->image.isNull()
                && !(*existing)->loading) {
                (*existing)->loading = true;
                m_pendingTiles.append(
                    { zoom, candidate.x, candidate.y, false, false, {} });
            }
            if (m_weatherRadarVisible && !m_weatherRadarPlaybackActive
                && (*existing)->radarFrameId
                    != m_weatherRadarSource.frameId()
                && !(*existing)->radarLoading) {
                (*existing)->radarLoading = true;
                m_pendingTiles.append({ zoom, candidate.x, candidate.y,
                    false, true, m_weatherRadarSource.frameId() });
            }
            continue;
        }
        auto tile = std::make_shared<DetailTile>();
        tile->zoom = zoom;
        tile->x = candidate.x;
        tile->y = candidate.y;
        tile->loading = true;
        tile->lastUsedFrame = m_detailFrame;
        m_detailTiles.insert(key, tile);
        m_pendingTiles.append(
            { zoom, candidate.x, candidate.y, false, false, {} });
        if (m_weatherRadarVisible && !m_weatherRadarPlaybackActive) {
            tile->radarLoading = true;
            m_pendingTiles.append({ zoom, candidate.x, candidate.y,
                false, true, m_weatherRadarSource.frameId() });
        }
    }
    evictDetailTiles();
    requestNextTiles();
}

void GlobeMapView::uploadDetailTile(DetailTile& tile)
{
    if (tile.image.isNull()) {
        return;
    }
    tile.texture = std::make_unique<QOpenGLTexture>(tile.image);
    tile.texture->setMinificationFilter(QOpenGLTexture::LinearMipMapLinear);
    tile.texture->setMagnificationFilter(QOpenGLTexture::Linear);
    tile.texture->setWrapMode(QOpenGLTexture::ClampToEdge);
    tile.texture->generateMipMaps();

    QVector<Vertex> vertices;
    vertices.reserve((kDetailTileSegments + 1)
                     * (kDetailTileSegments + 1));
    for (int row = 0; row <= kDetailTileSegments; ++row) {
        const double localV = static_cast<double>(row)
                            / kDetailTileSegments;
        const double latitude = mercatorTileLatitude(tile.y + localV,
                                                      tile.zoom);
        for (int column = 0; column <= kDetailTileSegments; ++column) {
            const double localU = static_cast<double>(column)
                                / kDetailTileSegments;
            const double longitude = mercatorTileLongitude(
                tile.x + localU, tile.zoom);
            vertices.append({ geoVector(latitude, longitude) * 1.0002F,
                              { static_cast<float>(localU),
                                static_cast<float>(localV) } });
        }
    }

    QVector<quint32> indices;
    indices.reserve(kDetailTileSegments * kDetailTileSegments * 6);
    const int rowWidth = kDetailTileSegments + 1;
    for (int row = 0; row < kDetailTileSegments; ++row) {
        for (int column = 0; column < kDetailTileSegments; ++column) {
            const quint32 topLeft = static_cast<quint32>(
                row * rowWidth + column);
            const quint32 bottomLeft = topLeft + rowWidth;
            indices.append(topLeft);
            indices.append(bottomLeft);
            indices.append(topLeft + 1);
            indices.append(topLeft + 1);
            indices.append(bottomLeft);
            indices.append(bottomLeft + 1);
        }
    }
    tile.indexCount = indices.size();
    tile.vertexBuffer.create();
    tile.vertexBuffer.bind();
    tile.vertexBuffer.allocate(vertices.constData(),
                               vertices.size() * sizeof(Vertex));
    tile.vertexBuffer.release();
    tile.indexBuffer.create();
    tile.indexBuffer.bind();
    tile.indexBuffer.allocate(indices.constData(),
                              indices.size() * sizeof(quint32));
    tile.indexBuffer.release();
    tile.image = {};
}

void GlobeMapView::uploadWeatherRadarDetailTile(DetailTile& tile)
{
    if (tile.radarImage.isNull()) {
        return;
    }
    std::unique_ptr<QOpenGLTexture> texture =
        WeatherRadarTexture::makeTexture(tile.radarImage, true);
    if (texture == nullptr) {
        return;
    }
    tile.previousRadarTexture = std::move(tile.radarTexture);
    tile.radarTexture = std::move(texture);
    tile.radarImage = {};
}

void GlobeMapView::destroyDetailTile(DetailTile& tile)
{
    tile.texture.reset();
    tile.radarTexture.reset();
    tile.previousRadarTexture.reset();
    if (tile.vertexBuffer.isCreated()) {
        tile.vertexBuffer.destroy();
    }
    if (tile.indexBuffer.isCreated()) {
        tile.indexBuffer.destroy();
    }
    tile.indexCount = 0;
}

void GlobeMapView::evictDetailTiles()
{
    if (m_detailTiles.size() <= kMaximumCachedDetailTiles) {
        return;
    }
    QSet<QString> visible;
    visible.reserve(m_visibleDetailKeys.size());
    for (const QString& key : std::as_const(m_visibleDetailKeys)) {
        visible.insert(key);
    }
    QVector<QString> candidates;
    for (auto iterator = m_detailTiles.cbegin();
         iterator != m_detailTiles.cend(); ++iterator) {
        if (!visible.contains(iterator.key()) && !iterator.value()->loading
            && !iterator.value()->radarLoading) {
            candidates.append(iterator.key());
        }
    }
    std::sort(candidates.begin(), candidates.end(),
              [this](const QString& lhs, const QString& rhs) {
                  return m_detailTiles.value(lhs)->lastUsedFrame
                       < m_detailTiles.value(rhs)->lastUsedFrame;
              });
    while (m_detailTiles.size() > kMaximumCachedDetailTiles
           && !candidates.isEmpty()) {
        const QString key = candidates.takeFirst();
        const std::shared_ptr<DetailTile> tile = m_detailTiles.take(key);
        destroyDetailTile(*tile);
    }
}

void GlobeMapView::requestAtlasTiles()
{
    m_pendingTiles.clear();
    for (int y = 0; y < kTileCount; ++y) {
        for (int x = 0; x < kTileCount; ++x) {
            m_pendingTiles.append(
                { kAtlasZoom, x, y, true, false, {} });
        }
    }
    requestNextTiles();
}

void GlobeMapView::requestWeatherRadarAtlas()
{
    const int radarAtlasSize=kTileCount*m_weatherRadarSource.tilePixelSize();
    m_weatherRadarLoadFailed = false;
    const QString frameId = m_weatherRadarSource.frameId();
    m_pendingTiles.erase(std::remove_if(m_pendingTiles.begin(),
                                        m_pendingTiles.end(),
        [](const TileRequest& request) { return request.weatherRadar; }),
        m_pendingTiles.end());
    // Only retain failed tiles from another full-world live atlas. Playback
    // exports have regional dimensions and bounds; treating one as the next
    // global atlas stretches stale U.S. pixels across unrelated countries.
    if (!m_weatherRadarPlaybackActive
        && m_weatherRadarAtlas.size() == QSize(radarAtlasSize, radarAtlasSize)
        && m_weatherRadarTextureBounds
            == QVector4D(0.0F, 0.0F, 1.0F, 1.0F)) {
        m_pendingWeatherRadarAtlas = m_weatherRadarAtlas;
    } else {
        m_pendingWeatherRadarAtlas = QImage(
            radarAtlasSize, radarAtlasSize, WeatherRadarTexture::kImageFormat);
        m_pendingWeatherRadarAtlas.fill(Qt::transparent);
    }
    m_pendingWeatherRadarFrameId = frameId;
    m_pendingWeatherRadarTileCount = kTileCount * kTileCount;
    m_pendingWeatherRadarAtlasFailed = false;
    for (int y = 0; y < kTileCount; ++y) {
        for (int x = 0; x < kTileCount; ++x) {
            m_pendingTiles.append(
                { kAtlasZoom, x, y, true, true, frameId });
        }
    }
    requestNextTiles();
}

int GlobeMapView::pendingWeatherRadarRequests() const
{
    if (!m_weatherRadarVisible || m_weatherRadarPlaybackActive) {
        return 0;
    }
    return m_weatherRadarReplies.size() + std::count_if(m_pendingTiles.cbegin(),
        m_pendingTiles.cend(), [](const TileRequest& tile) { return tile.weatherRadar; });
}

void GlobeMapView::requestNextTiles()
{
    QNetworkAccessManager* manager = QGV::getNetworkManager();
    if (manager == nullptr) {
        return;
    }
    while (m_activeTileRequests < kMaximumConcurrentTileRequests
           && !m_pendingTiles.isEmpty()) {
        const TileRequest tile = m_pendingTiles.takeFirst();
        const QUrl url = tile.weatherRadar
            ? m_weatherRadarSource.tileUrl(tile.zoom, tile.x, tile.y)
            : QUrl(QStringLiteral(
                "https://tile.openstreetmap.org/%1/%2/%3.png")
                .arg(tile.zoom).arg(tile.x).arg(tile.y));
        const bool nativeRadar = tile.weatherRadar && url.host() == QStringLiteral("image")
            && (url.scheme() == QStringLiteral("opera-radar")
                || url.scheme() == QStringLiteral("radar-composite"));
        if (!url.isValid() || (url.scheme() != QStringLiteral("https") && !nativeRadar)) {
            continue;
        }
        const int tilePixelSize=tile.weatherRadar ? m_weatherRadarSource.tilePixelSize() : kTileSize;
        QNetworkRequest request(url);
        request.setHeader(QNetworkRequest::UserAgentHeader,
                          QGV::getTileUserAgent());
        request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                             QNetworkRequest::PreferCache);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::SameOriginRedirectPolicy);
        QNetworkReply* reply = manager->get(request);
        m_tileReplies.insert(reply);
        if (tile.weatherRadar) {
            m_weatherRadarReplies.insert(reply);
        }
        ++m_activeTileRequests;
        connect(reply, &QNetworkReply::downloadProgress, reply,
                [reply](qint64 received, qint64) {
                    if (received > kMaximumTileBytes) {
                        reply->abort();
                    }
                });
        connect(reply, &QObject::destroyed, this, [this, reply] {
            m_tileReplies.remove(reply);
            m_weatherRadarReplies.remove(reply);
        });
        connect(reply, &QNetworkReply::finished, this,
                [this, reply, tile, tilePixelSize] {
                    m_tileReplies.remove(reply);
                    m_weatherRadarReplies.remove(reply);
                    m_activeTileRequests = std::max(
                        0, m_activeTileRequests - 1);
                    QByteArray bytes;
                    if (reply->error() == QNetworkReply::NoError
                        && reply->bytesAvailable() <= kMaximumTileBytes) {
                        bytes = reply->read(kMaximumTileBytes + 1);
                    }
                    reply->deleteLater();
                    bool validImage = false;
                    if (!bytes.isEmpty() && bytes.size() <= kMaximumTileBytes) {
                        QImage image;
                        image.loadFromData(bytes, "PNG");
                        if (!image.isNull() && image.width() == tilePixelSize
                            && image.height() == tilePixelSize) {
                            validImage = true;
                            if (tile.weatherRadar) {
                                if (tile.baseAtlas && tile.radarFrameId
                                    == m_pendingWeatherRadarFrameId) {
                                    QPainter radarPainter(
                                        &m_pendingWeatherRadarAtlas);
                                    radarPainter.setCompositionMode(
                                        QPainter::CompositionMode_Source);
                                    radarPainter.drawImage(
                                        tile.x * tilePixelSize,
                                        tile.y * tilePixelSize, image);
                                    radarPainter.end();
                                    if (m_loadedWeatherRadarFrameId.isEmpty()
                                        && !m_weatherRadarPlaybackActive) {
                                        // Initial coverage need not wait for the
                                        // slowest of 16 atlas tiles. Later time
                                        // updates retain the prior full atlas.
                                        m_weatherRadarAtlas = m_pendingWeatherRadarAtlas;
                                        m_weatherRadarAtlasDirty = true;
                                        m_replaceWeatherRadarTexture = true;
                                        update();
                                    }
                                } else if (!tile.baseAtlas
                                    && tile.radarFrameId
                                        == m_weatherRadarSource.frameId()) {
                                    const QString key = detailTileKey(
                                        tile.zoom, tile.x, tile.y);
                                    const auto found =
                                        m_detailTiles.constFind(key);
                                    if (found != m_detailTiles.cend()) {
                                        (*found)->radarImage =
                                            WeatherRadarTexture::prepareImage(image);
                                        (*found)->radarLoading = false;
                                        (*found)->radarFrameId =
                                            tile.radarFrameId;
                                        update();
                                    }
                                }
                            } else if (tile.baseAtlas) {
                                QPainter atlasPainter(&m_atlas);
                                atlasPainter.drawImage(tile.x * kTileSize,
                                                       tile.y * kTileSize,
                                                       image);
                                scheduleAtlasUpload();
                            } else {
                                const QString key = detailTileKey(
                                    tile.zoom, tile.x, tile.y);
                                const auto found = m_detailTiles.constFind(key);
                                if (found != m_detailTiles.cend()) {
                                    (*found)->image = std::move(image);
                                    (*found)->loading = false;
                                    update();
                                }
                            }
                        }
                    }
                    if (tile.weatherRadar && !validImage) {
                        m_weatherRadarLoadFailed = true;
                    }
                    if (!tile.baseAtlas && !tile.weatherRadar) {
                        const QString key = detailTileKey(
                            tile.zoom, tile.x, tile.y);
                        const auto found = m_detailTiles.constFind(key);
                        if (found != m_detailTiles.cend()) {
                            (*found)->loading = false;
                        }
                    }
                    if (tile.weatherRadar && !tile.baseAtlas) {
                        const QString key = detailTileKey(
                            tile.zoom, tile.x, tile.y);
                        const auto found = m_detailTiles.constFind(key);
                        if (found != m_detailTiles.cend()) {
                            (*found)->radarLoading = false;
                        }
                    }
                    if (tile.weatherRadar && tile.baseAtlas
                        && tile.radarFrameId
                            == m_pendingWeatherRadarFrameId) {
                        m_pendingWeatherRadarTileCount = std::max(
                            0, m_pendingWeatherRadarTileCount - 1);
                        m_pendingWeatherRadarAtlasFailed |= !validImage;
                        if (m_pendingWeatherRadarTileCount == 0
                            && !m_pendingWeatherRadarAtlasFailed
                            && !m_weatherRadarPlaybackActive) {
                            // "All replies finished" is not "all tiles loaded".
                            // Failed tiles leave transparent holes in the staging
                            // atlas; publishing it can erase the entire radar.
                            // A valid transparent PNG still counts as loaded.
                            m_weatherRadarAtlas =
                                m_pendingWeatherRadarAtlas;
                            m_loadedWeatherRadarFrameId =
                                m_pendingWeatherRadarFrameId;
                            m_weatherRadarAtlasDirty = true;
                            update();
                        }
                    }
                    requestNextTiles();
                });
    }
}

void GlobeMapView::cancelTileRequests()
{
    m_pendingTiles.clear();
    const QSet<QNetworkReply*> replies = m_tileReplies;
    m_tileReplies.clear();
    m_weatherRadarReplies.clear();
    m_activeTileRequests = 0;
    for (QNetworkReply* reply : replies) {
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
    }
}

void GlobeMapView::cancelWeatherRadarRequests()
{
    m_pendingTiles.erase(std::remove_if(m_pendingTiles.begin(),
                                        m_pendingTiles.end(),
        [](const TileRequest& request) { return request.weatherRadar; }),
        m_pendingTiles.end());
    const QSet<QNetworkReply*> replies = m_weatherRadarReplies;
    m_weatherRadarReplies.clear();
    m_activeTileRequests = std::max(
        0, m_activeTileRequests - static_cast<int>(replies.size()));
    for (QNetworkReply* reply : replies) {
        m_tileReplies.remove(reply);
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
    }
    for (const std::shared_ptr<DetailTile>& tile :
         std::as_const(m_detailTiles)) {
        tile->radarLoading = false;
    }
    requestNextTiles();
}

void GlobeMapView::reportRendererUnavailable(const QString& reason,
                                             const QString& detail)
{
    if (m_rendererUnavailableReported) {
        return;
    }
    m_rendererUnavailableReported = true;
    if (detail.isEmpty()) {
        qCWarning(lcPskReporterGlobe).noquote() << reason;
    } else {
        qCWarning(lcPskReporterGlobe).noquote() << reason << detail;
    }
    QTimer::singleShot(0, this, [this, reason] {
        emit rendererUnavailable(reason);
    });
}

void GlobeMapView::scheduleAtlasUpload()
{
    if (!m_atlasUploadTimer.isActive()) {
        m_atlasUploadTimer.start();
    }
}

QVector3D GlobeMapView::geoPoint(double lat, double lon) const
{
    return geoVector(lat, lon);
}

bool GlobeMapView::projectPoint(const QVector3D& point,
                                const QMatrix4x4& model,
                                const QMatrix4x4& viewProjection,
                                QPointF* screenPoint) const
{
    const QVector3D rotated = model.mapVector(point);
    // Perspective visibility ends at the camera/sphere tangent plane, not at
    // the geometric front hemisphere. For a unit sphere and camera at +Z,
    // dot(normal, camera - point) > 0 reduces to z > 1 / distance.
    // Culling there keeps far-side markers and path segments inside the limb.
    if (rotated.z() <= 1.0F / m_cameraDistance + 0.002F) {
        return false;
    }
    const QVector4D clip = viewProjection * QVector4D(rotated, 1.0F);
    if (clip.w() <= 0.0F) {
        return false;
    }
    const QVector3D ndc = clip.toVector3DAffine();
    *screenPoint = { (ndc.x() * 0.5F + 0.5F) * width(),
                     (0.5F - ndc.y() * 0.5F) * height() };
    return true;
}

void GlobeMapView::paintPaths(QPainter& painter, const QMatrix4x4& model,
                              const QMatrix4x4& viewProjection)
{
    const QVector<Marker> hoverPaths = !m_pathsVisible && m_hoverMarker >= 0
        ? MapHoverPathSelection::pathsForMarker(m_markers, m_hoverMarker)
        : QVector<Marker>{};
    const QVector<Marker>& paths = m_pathsVisible ? m_markers : hoverPaths;
    const bool interactionPreview = useInteractionPreview();
    const bool densePathLayer = m_pathsVisible && paths.size() > 500;
    const int previewStride = interactionPreview
        ? qMax(1, (paths.size() + 119) / 120) : 1;
    const int segmentCount = interactionPreview ? 12
                           : densePathLayer ? 24 : 48;
    QHash<QRgb, QPainterPath> pathsByColor;
    for (int index = 0; index < paths.size(); index += previewStride) {
        const Marker& marker = paths.at(index);
        if (!marker.pathEnabled
            || (!marker.hasPathOrigin && !m_hasHome)) {
            continue;
        }
        const QVector3D from = marker.hasPathOrigin
            ? geoPoint(marker.pathFromLat, marker.pathFromLon)
            : geoPoint(m_homeLat, m_homeLon);
        const QVector3D to = geoPoint(marker.lat, marker.lon);
        QColor pathColor = marker.color;
        pathColor.setAlpha(185);
        QPainterPath& path = pathsByColor[pathColor.rgba()];
        QVector3D pointOnGlobe = from;
        const QVector3D rotationAxis = greatCircleAxis(from, to);
        const float angle = std::acos(std::clamp(
            QVector3D::dotProduct(from, to), -1.0F, 1.0F));
        const float stepAngle = angle / segmentCount;
        const float stepCosine = std::cos(stepAngle);
        const float stepSine = std::sin(stepAngle);
        bool previousVisible = false;
        for (int segment = 0; segment <= segmentCount; ++segment) {
            QPointF point;
            const bool visible = projectPoint(
                pointOnGlobe, model, viewProjection, &point);
            if (visible) {
                if (previousVisible) {
                    path.lineTo(point);
                } else {
                    path.moveTo(point);
                }
            }
            previousVisible = visible;
            // Rotate by one fixed great-circle step. Computing the axis and
            // trigonometric terms once per path avoids doing acos/sin for
            // every one of thousands of path segments on every frame.
            pointOnGlobe = pointOnGlobe * stepCosine
                + QVector3D::crossProduct(rotationAxis, pointOnGlobe)
                    * stepSine;
        }
    }
    // Thousands of overlapping global paths make QPainter's CPU antialiasing
    // dominate the GUI thread. At this density its subpixel treatment is not
    // perceptible. Keep it for targeted and hover paths, where line quality
    // remains visible.
    if (densePathLayer) {
        painter.setRenderHint(QPainter::Antialiasing, false);
    }
    for (auto iterator = pathsByColor.cbegin();
         iterator != pathsByColor.cend(); ++iterator) {
        painter.setPen(QPen(QColor::fromRgba(iterator.key()),
                            m_pathsVisible ? 1.25 : 2.4,
                            Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(iterator.value());
    }
    painter.setRenderHint(QPainter::Antialiasing, true);
}

void GlobeMapView::paintMarkers(QPainter& painter, const QMatrix4x4& model,
                                const QMatrix4x4& viewProjection)
{
    m_projectedMarkers.resize(m_markers.size());
    const bool interactionPreview = useInteractionPreview();
    const int previewStride = interactionPreview
        ? qMax(1, (m_markers.size() + 799) / 800) : 1;
    QHash<quint64, QPainterPath> markerBatches;
    for (int index = 0; index < m_markers.size(); ++index) {
        const Marker& marker = m_markers.at(index);
        if (previewStride > 1 && index % previewStride != 0
            && marker.label.isEmpty() && !marker.isHome) {
            m_projectedMarkers[index] = {};
            continue;
        }
        QPointF point;
        const bool visible = projectPoint(geoPoint(marker.lat, marker.lon),
                                          model, viewProjection, &point);
        m_projectedMarkers[index] = { point, visible };
        if (!visible) {
            continue;
        }
        const qreal radius = marker.isMonitor ? 5.0 : 4.0;
        const quint64 batchKey = static_cast<quint64>(marker.color.rgba())
            | (static_cast<quint64>(marker.isMonitor) << 32);
        markerBatches[batchKey].addEllipse(point, radius, radius);
    }
    for (auto iterator = markerBatches.cbegin();
         iterator != markerBatches.cend(); ++iterator) {
        painter.setPen(QPen(m_backgroundColor, 1.2));
        painter.setBrush(QColor::fromRgba(
            static_cast<QRgb>(iterator.key() & 0xffffffffULL)));
        painter.drawPath(iterator.value());
    }

    for (int index = 0; index < m_markers.size(); ++index) {
        const Marker& marker = m_markers.at(index);
        const ProjectedMarker& projected = m_projectedMarkers.at(index);
        if (!projected.visible) {
            continue;
        }
        const QPointF point = projected.point;
        const qreal radius = marker.isMonitor ? 5.0 : 4.0;
        if (marker.isHome) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(marker.color, 2.0));
            painter.drawEllipse(point, radius + 3.0, radius + 3.0);
        }
        if (!marker.label.isEmpty()) {
            painter.setPen(m_textColor);
            painter.drawText(point + QPointF(radius + 3.0, 4.0),
                             marker.label);
        }
    }

    if (m_hasHome && m_homeMarkerShown) {
        QPointF homePoint;
        if (projectPoint(geoPoint(m_homeLat, m_homeLon), model,
                         viewProjection, &homePoint)) {
            const QColor homeColor = ThemeManager::instance().color(
                this, "color.accent.bright");
            painter.setPen(QPen(homeColor, 2.0));
            painter.setBrush(m_backgroundColor);
            painter.drawEllipse(homePoint, 6.5, 6.5);
            painter.setBrush(homeColor);
            painter.drawEllipse(homePoint, 2.5, 2.5);
            if (!m_homeLabel.isEmpty()) {
                painter.setPen(m_textColor);
                painter.drawText(homePoint + QPointF(9.0, 4.0), m_homeLabel);
            }
        }
    }
}

void GlobeMapView::setHomePosition(double lat, double lon,
                                   const QString& label, bool showMarker)
{
    const bool firstHome = !m_hasHome;
    m_homeLat = std::clamp(lat, -90.0, 90.0);
    m_homeLon = SolarTerminator::normalizeDegrees(lon);
    m_homeLabel = label;
    m_homeMarkerShown = showMarker;
    m_hasHome = true;
    m_vectorOverlayDirty = true;
    if (firstHome) {
        resetToHome();
    } else {
        update();
    }
}

void GlobeMapView::setHomeSpanDegrees(double spanDegrees)
{
    if (std::isfinite(spanDegrees) && spanDegrees > 0.0) {
        m_homeSpanDegrees = std::clamp(spanDegrees, 0.002, 120.0);
    }
}

void GlobeMapView::setMarkers(const QVector<Marker>& markers)
{
    m_markers = markers;
    m_vectorOverlayDirty = true;
    m_hoverMarker = -1;
    m_hoverCard->hide();
    update();
}

void GlobeMapView::clearMarkers()
{
    m_markers.clear();
    m_projectedMarkers.clear();
    m_vectorOverlayDirty = true;
    m_hoverMarker = -1;
    m_hoverCard->hide();
    update();
}

void GlobeMapView::setPathsVisible(bool visible)
{
    if (m_pathsVisible == visible) {
        return;
    }
    m_pathsVisible = visible;
    m_vectorOverlayDirty = true;
    update();
}

void GlobeMapView::setDayNightTerminatorVisible(bool visible)
{
    m_terminatorVisible = visible;
    if (visible) {
        m_terminatorTimer.start();
    } else {
        m_terminatorTimer.stop();
    }
    update();
}

void GlobeMapView::setCityLightsVisible(bool visible)
{
    if (m_cityLightsVisible == visible) {
        return;
    }
    m_cityLightsVisible = visible;
    updateMapAttribution();
    update();
}

void GlobeMapView::setCityLightsWarmth(int percent)
{
    m_cityLightsWarmth = std::clamp(percent, 0, 100) / 100.0F;
    update();
}

void GlobeMapView::setCityLightsFaintLights(int percent)
{
    m_cityLightsGamma = float(CityLightsShading::faintLightsGamma(percent));
    update();
}

void GlobeMapView::setBasemapDarkEnabled(bool enabled)
{
    m_basemapDarkEnabled = enabled;
    update();
}

void GlobeMapView::setBasemapBrightness(int percent)
{
    m_basemapBrightness = std::clamp(percent, 20, 100);
    update();
}

void GlobeMapView::setCityLightsBrightness(int percent)
{
    m_cityLightsOpacity = std::clamp(percent, 0, 100) / 100.0F;
    update();
}

void GlobeMapView::setCityLightsImage(const QImage& image, const QRectF& bounds)
{
    if (image.cacheKey() == m_cityLightsImage.cacheKey() && bounds == m_cityLightsBounds) {
        return;
    }
    m_cityLightsImage = image;
    m_cityLightsBounds = bounds;
    m_cityLightsDirty = true;
    update();
}

void GlobeMapView::setDetailedAttributionVisible(bool visible)
{
    m_detailedAttributionVisible = visible;
    updateMapAttribution();
}

void GlobeMapView::updateMapAttribution()
{
    QString text = QStringLiteral("© <a href=\"https://www.openstreetmap.org/copyright\">OpenStreetMap</a> contributors");
    if (m_detailedAttributionVisible && m_cityLightsVisible) {
        text += QStringLiteral(" · Lights: NASA/GSFC, 2016");
    }
    if (m_detailedAttributionVisible && m_weatherRadarVisible) {
        text += QStringLiteral(" · ") + m_weatherRadarSource.attribution();
    }
    if (m_detailedAttributionVisible && m_radarCoverageVisible) {
        text += QStringLiteral(" · Sites: NOAA/NWS, EUMETNET OPERA · nominal range");
    }
    m_attribution->setText(text);
    layoutOverlays();
}

void GlobeMapView::drawCityLights(const QMatrix4x4& matrix)
{
    if (!m_cityLightsVisible || m_cityLightsImage.isNull() || m_cityLightsBounds.isEmpty()) {
        return;
    }
    if (m_cityLightsProgram == nullptr) {
        m_cityLightsProgram = std::make_unique<QOpenGLShaderProgram>();
        static constexpr char vertex[] = R"(
            attribute highp vec3 position;
            uniform highp mat4 matrix;
            varying highp vec3 earthPosition;
            void main() {
                earthPosition = position;
                gl_Position = matrix * vec4(position, 1.0);
            }
        )";
        if (!m_cityLightsProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, vertex)
            || !m_cityLightsProgram->addShaderFromSourceCode(QOpenGLShader::Fragment,
                                                             CityLightsShading::fragmentShaderSource())
            || !m_cityLightsProgram->link()) {
            qCWarning(lcPskReporterGlobe) << "City lights shader:" << m_cityLightsProgram->log();
            return;
        }
    }
    if (!m_cityLightsProgram->isLinked()) {
        return;
    }
    if (m_cityLightsDirty || m_cityLightsTexture == nullptr) {
        m_cityLightsTexture = WeatherRadarTexture::makeTexture(m_cityLightsImage);
        m_cityLightsDirty = false;
    }
    if (m_cityLightsTexture == nullptr) {
        return;
    }
    // Same surface-order rule as radar: the globe's coarse mesh is below
    // basemap detail triangles. Cull the back hemisphere, not the base map.
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CW);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    m_cityLightsProgram->bind();
    m_cityLightsProgram->setUniformValue("matrix", matrix);
    m_cityLightsProgram->setUniformValue("lights", 0);
    m_cityLightsProgram->setUniformValue("bounds", normalizedRadarTextureBounds(m_cityLightsBounds));
    m_cityLightsProgram->setUniformValue("opacity", m_cityLightsOpacity);
    m_cityLightsProgram->setUniformValue("lightsGamma", m_cityLightsGamma);
    m_cityLightsProgram->setUniformValue("warmth", m_cityLightsWarmth);
    const SolarTerminator::Position sun = SolarTerminator::positionAt(QDateTime::currentDateTimeUtc());
    m_cityLightsProgram->setUniformValue("sunDirection", geoVector(
        qRadiansToDegrees(sun.declinationRad), qRadiansToDegrees(sun.subsolarLonRad)));
    m_cityLightsProgram->setUniformValue("nightOnly", m_terminatorVisible ? 1.0F : 0.0F);
    m_cityLightsTexture->bind(0);
    m_vertexBuffer.bind();
    m_indexBuffer.bind();
    const int position = m_cityLightsProgram->attributeLocation("position");
    m_cityLightsProgram->enableAttributeArray(position);
    m_cityLightsProgram->setAttributeBuffer(position, GL_FLOAT,
        offsetof(Vertex, position), 3, sizeof(Vertex));
    glDrawElements(GL_TRIANGLES, m_indexCount, GL_UNSIGNED_INT, nullptr);
    m_cityLightsProgram->disableAttributeArray(position);
    m_indexBuffer.release();
    m_vertexBuffer.release();
    m_cityLightsTexture->release();
    m_cityLightsProgram->release();
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glFrontFace(GL_CCW);
    glEnable(GL_DEPTH_TEST);
}

void GlobeMapView::setWeatherRadarVisible(bool visible)
{
    if (m_weatherRadarVisible == visible) {
        return;
    }
    m_weatherRadarVisible = visible;
    updateMapAttribution();
    if (visible) {
        m_detailSelectionDirty = true;
        setWeatherRadarSource(m_weatherRadarSource);
    } else {
        cancelWeatherRadarRequests();
        update();
    }
}

void GlobeMapView::refreshWeatherRadar()
{
    if (!m_weatherRadarVisible) {
        return;
    }
    setWeatherRadarSource(m_weatherRadarSource.latestFrame());
}

void GlobeMapView::setWeatherRadarSource(
    const WeatherRadarSource& source)
{
    if (source.provider() != m_weatherRadarSource.provider()
        || source.enabledProviders() != m_weatherRadarSource.enabledProviders()) {
        cancelWeatherRadarRequests();
        m_weatherRadarAtlas.fill(Qt::transparent);
        m_pendingWeatherRadarAtlas.fill(Qt::transparent);
        m_loadedWeatherRadarFrameId.clear();
        m_weatherRadarAtlasDirty = true;
        m_replaceWeatherRadarTexture = true;
        m_pendingRadarTextureBounds = QVector4D(0.0F, 0.0F, 1.0F, 1.0F);
    }
    if (m_weatherRadarPlaybackActive) {
        // Catalog/view synchronization must never start a live atlas request
        // which can later overwrite the playback front texture. Originals
        // enter only via show/preload; Stop explicitly exits playback first.
        m_weatherRadarSource = source;
        updateMapAttribution();
        return;
    }
    if (!m_weatherRadarVisible) {
        m_weatherRadarSource = source;
        updateMapAttribution();
        return;
    }
    if (source.frameId() == m_loadedWeatherRadarFrameId
        && !m_weatherRadarAtlasDirty) {
        QTimer::singleShot(0, this, [this, source] {
            emit weatherRadarFrameLoaded(source.frameTime());
        });
        return;
    }
    m_weatherRadarSource = source;
    updateMapAttribution();
    m_pendingRadarTextureBounds = QVector4D(0.0F, 0.0F, 1.0F, 1.0F);
    m_detailSelectionDirty = true;
    requestWeatherRadarAtlas();
    update();
}

QRectF GlobeMapView::weatherRadarPlaybackBounds() const
{
    const QRectF world(-kWebMercatorExtent, -kWebMercatorExtent,
                       2.0 * kWebMercatorExtent,
                       2.0 * kWebMercatorExtent);
    if (m_visibleDetailKeys.isEmpty() || detailZoomLevel() <= kAtlasZoom) {
        return world;
    }

    bool foundTile = false;
    QRectF bounds;
    double padding = 0.0;
    for (const QString& key : m_visibleDetailKeys) {
        const QStringList parts = key.split('/');
        if (parts.size() != 3) {
            continue;
        }
        bool zoomOk = false;
        bool xOk = false;
        bool yOk = false;
        const int zoom = parts.at(0).toInt(&zoomOk);
        const int x = parts.at(1).toInt(&xOk);
        const int y = parts.at(2).toInt(&yOk);
        if (!zoomOk || !xOk || !yOk || zoom < 0) {
            continue;
        }
        const double tileSpan = 2.0 * kWebMercatorExtent / (1 << zoom);
        const double minimumX = -kWebMercatorExtent + x * tileSpan;
        const double maximumY = kWebMercatorExtent - y * tileSpan;
        const QRectF tileBounds(minimumX, maximumY - tileSpan,
                                tileSpan, tileSpan);
        bounds = foundTile ? bounds.united(tileBounds) : tileBounds;
        padding = std::max(padding, tileSpan);
        foundTile = true;
    }
    if (!foundTile || bounds.width() > world.width() * 0.75) {
        return world;
    }
    bounds.adjust(-padding, -padding, padding, padding);
    return bounds.intersected(world).normalized();
}

QSize GlobeMapView::weatherRadarPlaybackSize(const QRectF& bounds) const
{
    const QRectF normalized = bounds.normalized();
    if (!normalized.isValid() || normalized.isEmpty()) {
        return QSize(1024, 1024);
    }
    // A whole-world Mercator texture uses only a fraction of its pixels on
    // the visible hemisphere. Keep the same detail as the live radar atlas
    // even in a small window; four megapixels also fits native export limits.
    if (normalized.width() >= 2*kWebMercatorExtent-1
        && normalized.height() >= 2*kWebMercatorExtent-1) {
        return QSize(2048,2048);
    }
    constexpr int kMaximumDimension = 4096;
    constexpr int kMinimumLongDimension = 1024;
    constexpr int kMinimumShortDimension = 512;
    constexpr qreal kPlaybackOversampling = 1.25;
    const int physicalViewportLongDimension = qRound(
        std::max(width(), height()) * devicePixelRatioF()
        * kPlaybackOversampling);
    const int longDimension = std::clamp(
        physicalViewportLongDimension, kMinimumLongDimension,
        kMaximumDimension);
    if (normalized.width() >= normalized.height()) {
        return weatherRadarLimitedSize(QSize(longDimension,
            std::clamp(qRound(longDimension * normalized.height()
                              / normalized.width()),
                       kMinimumShortDimension, kMaximumDimension)));
    }
    return weatherRadarLimitedSize(QSize(
        std::clamp(qRound(longDimension * normalized.width()
                          / normalized.height()),
                   kMinimumShortDimension, kMaximumDimension),
        longDimension));
}


bool GlobeMapView::showWeatherRadarPlaybackFrame(
    const QImage& image, const QDateTime& frameTime, const QRectF& bounds)
{
    const QRectF normalizedBounds = bounds.normalized();
    if (image.isNull() || !frameTime.isValid()
        || !normalizedBounds.isValid() || normalizedBounds.isEmpty()) {
        return false;
    }
    const bool startingPlayback = !m_weatherRadarPlaybackActive;
    if (startingPlayback) {
        cancelWeatherRadarRequests();
        m_weatherRadarTransition->stop();
        m_weatherRadarPlaybackActive = true;
    }
    const QVector4D textureBounds = normalizedRadarTextureBounds(normalizedBounds);
    m_weatherRadarTransitionProgress = 1.0F;

    if (m_radarTextureImageKey == image.cacheKey()
        && m_radarTextureFrameTime == frameTime
        && m_weatherRadarTextureBounds == textureBounds
        && radarTextureMatches(m_radarTexture, image)) {
        if (startingPlayback) {
            QTimer::singleShot(0, this, [this, frameTime] {
                emit weatherRadarFrameLoaded(frameTime);
            });
        }
        return true;
    }
    if (!m_preloadedWeatherRadarAtlasDirty
        && m_preloadedRadarImageKey == image.cacheKey()
        && m_preloadedWeatherRadarFrameTime == frameTime
        && m_preloadedRadarTextureBounds == textureBounds
        && radarTextureMatches(m_preloadedRadarTexture, image)) {
        // Swap only fully uploaded original observations. Never move the
        // texture bounds independently of the pixels or expose partial rows.
        // The old texture becomes reusable upload storage; destruction stays
        // on the GL paint path, where the context is current.
        std::swap(m_radarTexture, m_preloadedRadarTexture);
        m_radarTextureImageKey = m_preloadedRadarImageKey;
        m_preloadedRadarImageKey = 0;
        m_radarTextureFrameTime = frameTime;
        m_weatherRadarTextureBounds = textureBounds;
        m_preloadedWeatherRadarFrameTime = {};
        m_preloadedWeatherRadarAtlas = {};
        m_preloadedWeatherRadarUploadRow = 0;
        m_weatherRadarSource = m_weatherRadarSource.historicalFrame(frameTime);
        m_loadedWeatherRadarFrameId = m_weatherRadarSource.frameId();
        update();
        return true;
    }
    if (startingPlayback) {
        // Install the first original in paintGL; the live radar remains
        // visible until the replacement and its bounds are resident.
        m_weatherRadarSource = m_weatherRadarSource.historicalFrame(frameTime);
        m_pendingCurrentPlaybackRadarAtlas = image;
        m_pendingCurrentPlaybackFrameTime = frameTime;
        m_pendingRadarTextureBounds = textureBounds;
        m_pendingWeatherRadarPlaybackFrameDirty = true;
        m_replaceWeatherRadarTexture = false;
        update();
    } else {
        preloadWeatherRadarPlaybackFrame(image, frameTime, normalizedBounds);
    }
    return false; // Retain the last complete image on a slow upload.
}

void GlobeMapView::acknowledgeWeatherRadarPlaybackFrame(quint64 presentationSequence)
{
    if (!m_weatherRadarPlaybackActive) {
        return;
    }
    // No fractional blend, easing, optical flow or AI-generated pixels.
    m_weatherRadarTransitionProgress = 1.0F;
    m_pendingWeatherRadarPresentationSequence = presentationSequence;
    update();
}

void GlobeMapView::preloadWeatherRadarPlaybackFrame(
    const QImage& image, const QDateTime& frameTime,
    const QRectF& bounds)
{
    const QRectF normalizedBounds = bounds.normalized();
    if (!m_weatherRadarPlaybackActive || image.isNull()
        || !frameTime.isValid() || !normalizedBounds.isValid()
        || normalizedBounds.isEmpty()) {
        return;
    }
    const QVector4D textureBounds = normalizedRadarTextureBounds(
        normalizedBounds);
    if ((m_radarTextureImageKey == image.cacheKey()
         && frameTime == m_radarTextureFrameTime
         && textureBounds == m_weatherRadarTextureBounds
         && radarTextureMatches(m_radarTexture, image))
        || (m_preloadedRadarImageKey == image.cacheKey()
            && frameTime == m_preloadedWeatherRadarFrameTime
            && textureBounds == m_preloadedRadarTextureBounds
            && (m_preloadedWeatherRadarAtlasDirty
                || radarTextureMatches(m_preloadedRadarTexture, image)))
        || (m_pendingWeatherRadarPlaybackFrameDirty
            && image.cacheKey() == m_pendingCurrentPlaybackRadarAtlas.cacheKey()
            && frameTime == m_pendingCurrentPlaybackFrameTime
            && textureBounds == m_pendingRadarTextureBounds)) {
        return;
    }
    m_preloadedWeatherRadarAtlas = image;
    m_preloadedRadarImageKey = image.cacheKey();
    m_preloadedWeatherRadarFrameTime = frameTime;
    m_preloadedRadarTextureBounds = textureBounds;
    m_preloadedWeatherRadarUploadRow = 0;
    m_preloadedWeatherRadarAtlasDirty = true;
    update();
}

void GlobeMapView::clearWeatherRadarPlayback()
{
    m_weatherRadarTransition->stop();
    m_pendingWeatherRadarPresentationSequence = 0;
    m_weatherRadarPlaybackActive = false;
    m_pendingWeatherRadarPlaybackFrameDirty = false;
    m_replaceWeatherRadarTexture = false;
    m_pendingCurrentPlaybackRadarAtlas = {};
    m_pendingCurrentPlaybackFrameTime = {};
    m_previousRadarTextureFrameTime = {};
    m_releasePlaybackRadarTextures = true;
    m_preloadedWeatherRadarAtlas = {};
    m_preloadedWeatherRadarFrameTime = {};
    m_preloadedRadarImageKey = 0;
    m_preloadedWeatherRadarAtlasDirty = false;
    m_preloadedWeatherRadarUploadRow = 0;
    update();
}

void GlobeMapView::setLegend(
    const QVector<QPair<QString, QColor>>& entries)
{
    if (entries.isEmpty()) {
        m_legend->hide();
        return;
    }
    QString html;
    for (const auto& entry : entries) {
        if (!html.isEmpty()) {
            html += QStringLiteral("&nbsp;&nbsp;");
        }
        html += QStringLiteral("<span style=\"color:%1;\">&#9679;</span> %2")
                    .arg(entry.second.name(), entry.first.toHtmlEscaped());
    }
    m_legend->setText(html);
    m_legend->adjustSize();
    m_legend->show();
    layoutOverlays();
}

void GlobeMapView::resetToHome()
{
    if (m_hasHome) {
        m_navigation.reset(m_homeLat, m_homeLon);
    } else {
        m_navigation.reset(0.0, 0.0);
    }
    m_cameraDistance = kDefaultCameraDistance;
    m_detailSelectionDirty = true;
    scheduleWeatherRadarPlaybackViewRefresh();
    update();
}

void GlobeMapView::zoomIn()
{
    animateZoomTo(m_cameraDistance * kZoomFactor);
}

void GlobeMapView::zoomOut()
{
    animateZoomTo(m_cameraDistance / kZoomFactor);
}

void GlobeMapView::animateZoomTo(float distance)
{
    distance = std::clamp(distance, kMinimumCameraDistance,
                          kMaximumCameraDistance);
    m_zoomAnimation = std::make_unique<QVariantAnimation>();
    m_zoomAnimation->setStartValue(m_cameraDistance);
    m_zoomAnimation->setEndValue(distance);
    m_zoomAnimation->setDuration(220);
    m_zoomAnimation->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_zoomAnimation.get(), &QVariantAnimation::valueChanged,
            this, [this](const QVariant& value) {
                m_cameraDistance = value.toFloat();
                m_detailSelectionDirty = true;
                scheduleWeatherRadarPlaybackViewRefresh();
                update();
            });
    connect(m_zoomAnimation.get(), &QVariantAnimation::finished,
            this, [this] { update(); });
    m_zoomAnimation->start();
}

void GlobeMapView::beginTransientInteraction()
{
    m_interactionSettleTimer.start();
}

void GlobeMapView::scheduleWeatherRadarPlaybackViewRefresh()
{
    if (m_weatherRadarPlaybackActive || m_cityLightsVisible) {
        m_weatherRadarPlaybackViewTimer.start();
    }
}

bool GlobeMapView::useInteractionPreview() const
{
    return m_dragging || m_interactionSettleTimer.isActive()
        || (m_zoomAnimation != nullptr
            && m_zoomAnimation->state() == QAbstractAnimation::Running);
}

void GlobeMapView::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        m_dragging = true;
        m_hasMovedDuringDrag = false;
        m_lastPointerPosition = event->position();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    QOpenGLWidget::mousePressEvent(event);
}

void GlobeMapView::applyDragDelta(const QPointF& delta)
{
    const float degreesPerPixel =
        GlobeNavigation::screenTrackedDegreesPerPixel(
            m_cameraDistance, height(), kVerticalFieldOfViewDegrees);
    m_navigation.applyDragDelta(delta, degreesPerPixel);
    m_detailSelectionDirty = true;
    scheduleWeatherRadarPlaybackViewRefresh();
}

void GlobeMapView::applyRollDelta(float degrees)
{
    m_navigation.applyRollDelta(degrees);
    m_detailSelectionDirty = true;
    scheduleWeatherRadarPlaybackViewRefresh();
}

void GlobeMapView::mouseMoveEvent(QMouseEvent* event)
{
    if (m_dragging) {
        const QPointF delta = event->position() - m_lastPointerPosition;
        if (QLineF(QPointF(), delta).length() >= 1.0) {
            m_hasMovedDuringDrag = true;
            if (event->modifiers().testFlag(Qt::ShiftModifier)) {
                applyRollDelta(static_cast<float>(-delta.x()) * 0.28F);
            } else {
                m_navigation.applyScreenDrag(
                    m_lastPointerPosition, event->position(), size(),
                    m_cameraDistance, kVerticalFieldOfViewDegrees);
                m_detailSelectionDirty = true;
                scheduleWeatherRadarPlaybackViewRefresh();
            }
            m_lastPointerPosition = event->position();
            m_hoverMarker = -1;
            m_hoverCard->hide();
            update();
        }
        event->accept();
        return;
    }
    updateHover(event->position());
    QOpenGLWidget::mouseMoveEvent(event);
}

void GlobeMapView::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && m_dragging) {
        m_dragging = false;
        unsetCursor();
        if (!m_hasMovedDuringDrag) {
            updateHover(event->position());
            if (m_hoverMarker >= 0 && m_hoverMarker < m_markers.size()) {
                emit markerClicked(m_markers.at(m_hoverMarker));
            }
        } else {
            update();
        }
        event->accept();
        return;
    }
    QOpenGLWidget::mouseReleaseEvent(event);
}

void GlobeMapView::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        animateZoomTo(m_cameraDistance * kZoomFactor);
        event->accept();
        return;
    }
    QOpenGLWidget::mouseDoubleClickEvent(event);
}

void GlobeMapView::wheelEvent(QWheelEvent* event)
{
    const QPoint pixelDelta = event->pixelDelta();
    const QPoint angleDelta = event->angleDelta();
    const qreal delta = !pixelDelta.isNull()
        ? pixelDelta.y() : angleDelta.y() / 8.0;
    if (!qFuzzyIsNull(delta)) {
        beginTransientInteraction();
        const float factor = std::exp(static_cast<float>(-delta) * 0.004F);
        m_cameraDistance = std::clamp(m_cameraDistance * factor,
                                      kMinimumCameraDistance,
                                      kMaximumCameraDistance);
        m_detailSelectionDirty = true;
        scheduleWeatherRadarPlaybackViewRefresh();
        update();
        event->accept();
        return;
    }
    QOpenGLWidget::wheelEvent(event);
}

bool GlobeMapView::event(QEvent* event)
{
    if (event->type() == QEvent::NativeGesture) {
        auto* gesture = static_cast<QNativeGestureEvent*>(event);
        if (gesture->gestureType() == Qt::ZoomNativeGesture) {
            beginTransientInteraction();
            const float factor = std::exp(
                static_cast<float>(-gesture->value()) * 1.5F);
            m_cameraDistance = std::clamp(m_cameraDistance * factor,
                                          kMinimumCameraDistance,
                                          kMaximumCameraDistance);
            m_detailSelectionDirty = true;
            update();
            return true;
        } else if (gesture->gestureType() == Qt::RotateNativeGesture) {
            const float degrees = static_cast<float>(gesture->value());
            if (!qFuzzyIsNull(degrees)) {
                beginTransientInteraction();
                // Native positive rotation is reported in the opposite
                // direction from the globe's camera-facing Z axis.
                applyRollDelta(-degrees);
                update();
            }
            return true;
        }
    } else if (event->type() == QEvent::Gesture) {
        auto* gestureEvent = static_cast<QGestureEvent*>(event);
        if (auto* pinch = static_cast<QPinchGesture*>(
                gestureEvent->gesture(Qt::PinchGesture))) {
            const qreal scale = pinch->scaleFactor();
            if (pinch->changeFlags().testFlag(
                    QPinchGesture::RotationAngleChanged)) {
                applyRollDelta(static_cast<float>(
                    pinch->lastRotationAngle() - pinch->rotationAngle()));
            }
            if (scale > 0.0) {
                beginTransientInteraction();
                m_cameraDistance = std::clamp(
                    m_cameraDistance / static_cast<float>(scale),
                    kMinimumCameraDistance, kMaximumCameraDistance);
                m_detailSelectionDirty = true;
                scheduleWeatherRadarPlaybackViewRefresh();
                update();
            }
            gestureEvent->accept(pinch);
            return true;
        }
    }
    return QOpenGLWidget::event(event);
}

void GlobeMapView::keyPressEvent(QKeyEvent* event)
{
    switch (event->key()) {
    case Qt::Key_Left:
        applyDragDelta({ -20.0, 0.0 });
        break;
    case Qt::Key_Right:
        applyDragDelta({ 20.0, 0.0 });
        break;
    case Qt::Key_Up:
        applyDragDelta({ 0.0, -20.0 });
        break;
    case Qt::Key_Down:
        applyDragDelta({ 0.0, 20.0 });
        break;
    case Qt::Key_Plus:
    case Qt::Key_Equal:
        zoomIn();
        return;
    case Qt::Key_Minus:
        zoomOut();
        return;
    case Qt::Key_Home:
        resetToHome();
        return;
    case Qt::Key_BracketLeft:
        applyRollDelta(5.0F);
        break;
    case Qt::Key_BracketRight:
        applyRollDelta(-5.0F);
        break;
    default:
        QOpenGLWidget::keyPressEvent(event);
        return;
    }
    update();
    event->accept();
}

void GlobeMapView::leaveEvent(QEvent* event)
{
    if (m_hoverMarker >= 0) {
        m_vectorOverlayDirty = true;
    }
    m_hoverMarker = -1;
    m_hoverCard->hide();
    update();
    QOpenGLWidget::leaveEvent(event);
}

void GlobeMapView::updateHover(const QPointF& position)
{
    int closest = -1;
    qreal closestDistance = 10.0;
    for (int index = 0; index < m_projectedMarkers.size(); ++index) {
        const ProjectedMarker& marker = m_projectedMarkers.at(index);
        if (!marker.visible) {
            continue;
        }
        const qreal distance = QLineF(position, marker.point).length();
        if (distance < closestDistance) {
            closest = index;
            closestDistance = distance;
        }
    }
    if (closest < 0 && m_radarCoverageVisible && m_overlayMatricesValid) {
        for (const RadarSite& site : m_radarSites) {
            QPointF center;
            if (projectPoint(geoPoint(site.lat, site.lon), m_overlayModel, m_overlayViewProjection, &center)
                && QLineF(center, position).length() < 8) {
                m_hoverMarker = -1;
                m_hoverCard->setTextFormat(Qt::PlainText);
                m_hoverCard->setText(site.description());
                m_hoverCard->adjustSize();
                m_hoverCard->move(position.toPoint() + QPoint(12, 12));
                m_hoverCard->show(); m_hoverCard->raise();
                return;
            }
        }
    }
    m_hoverCard->setTextFormat(Qt::AutoText);
    if (m_hoverMarker == closest) {
        if (closest >= 0) {
            showHoverCard(closest, position);
        } else {
            m_hoverCard->hide();
        }
        return;
    }
    m_hoverMarker = closest;
    m_vectorOverlayDirty = true;
    if (closest >= 0) {
        showHoverCard(closest, position);
    } else {
        m_hoverCard->hide();
    }
    update();
}

void GlobeMapView::showHoverCard(int markerIndex, const QPointF& position)
{
    const QString tooltip = m_markers.at(markerIndex).tooltip;
    if (tooltip.isEmpty()) {
        m_hoverCard->hide();
        return;
    }
    m_hoverCard->setText(tooltip);
    m_hoverCard->adjustSize();
    const int x = std::clamp(static_cast<int>(position.x()) + 12, 4,
                             std::max(4, width() - m_hoverCard->width() - 4));
    const int y = std::clamp(static_cast<int>(position.y()) + 12, 4,
                             std::max(4, height() - m_hoverCard->height() - 4));
    m_hoverCard->move(x, y);
    m_hoverCard->show();
    m_hoverCard->raise();
}

QToolButton* GlobeMapView::makeOverlayButton(const QString& text,
                                              const QString& tip)
{
    auto* button = new QToolButton(this);
    button->setText(text);
    button->setToolTip(tip);
    button->setFixedSize(30, 30);
    button->setFocusPolicy(Qt::NoFocus);
    return button;
}

void GlobeMapView::updateTheme()
{
    ThemeManager& theme = ThemeManager::instance();
    m_backgroundColor = theme.color(this, "color.background.0");
    m_nightColor = theme.color(this, "color.background.0");
    m_basemapBackground = theme.color(BasemapStyle::kBackgroundToken);
    m_basemapDetail = theme.color(BasemapStyle::kDetailToken);
    m_textColor = theme.color(this, "color.text.primary");
    m_vectorOverlayDirty = true;
    const QString overlayStyle = QStringLiteral(
        "QLabel { background-color: {{color.background.1}};"
        " color: {{color.text.primary}}; border: 1px solid {{color.border.subtle}};"
        " border-radius: 4px; padding: 4px 6px; font-size: 10px; }");
    theme.applyStyleSheet(m_attribution, overlayStyle);
    theme.applyStyleSheet(m_legend, overlayStyle);
    theme.applyStyleSheet(m_hoverCard, overlayStyle);
    const QString buttonStyle = QStringLiteral(
        "QToolButton { background-color: {{color.background.1}};"
        " color: {{color.text.primary}}; border: 1px solid {{color.border.subtle}};"
        " border-radius: 4px; font-size: 16px; font-weight: bold; }"
        "QToolButton:hover { background-color: {{color.background.2}}; }"
        "QToolButton:pressed { background-color: {{color.background.0}}; }");
    for (QToolButton* button : { m_zoomInButton, m_zoomOutButton,
                                 m_homeButton }) {
        if (button != nullptr) {
            theme.applyStyleSheet(button, buttonStyle);
        }
    }
}

void GlobeMapView::resizeEvent(QResizeEvent* event)
{
    QOpenGLWidget::resizeEvent(event);
    layoutOverlays();
    scheduleWeatherRadarPlaybackViewRefresh();
}

void GlobeMapView::showEvent(QShowEvent* event)
{
    QOpenGLWidget::showEvent(event);
    QTimer::singleShot(250, this, [this] {
        if (isVisible() && !m_glInitializationAttempted && !isValid()) {
            reportRendererUnavailable(
                tr("The globe renderer is unavailable because an OpenGL "
                   "context could not be created."));
        }
    });
}

void GlobeMapView::layoutOverlays()
{
    constexpr int margin = 8;
    constexpr int gap = 6;
    m_vectorOverlay->setGeometry(rect());
    m_vectorOverlay->raise();
    int y = margin;
    for (QToolButton* button : { m_zoomInButton, m_zoomOutButton,
                                 m_homeButton }) {
        button->move(width() - button->width() - margin, y);
        button->raise();
        y += button->height() + gap;
    }
    m_attribution->setWordWrap(false);
    m_attribution->setMinimumWidth(0);
    m_attribution->setMaximumWidth(std::max(1, width() - 2 * margin));
    m_attribution->adjustSize();
    m_attribution->setFixedWidth(m_attribution->width());
    m_attribution->setWordWrap(true);
    m_attribution->adjustSize();
    m_attribution->move(width() - m_attribution->width() - margin,
                        height() - m_attribution->height() - margin);
    m_attribution->raise();
    if (m_legend->isVisible()) {
        m_legend->setWordWrap(false);
        m_legend->setMinimumWidth(0);
        m_legend->setMaximumWidth(std::max(1, width() - 2 * margin));
        m_legend->adjustSize();
        m_legend->setFixedWidth(m_legend->width());
        m_legend->setWordWrap(true);
        m_legend->adjustSize();
        int bottom = height() - margin;
        if (m_legend->width() + m_attribution->width() + gap
                > width() - 2 * margin) {
            bottom -= m_attribution->height() + gap;
        }
        m_legend->move(margin, bottom - m_legend->height());
        m_legend->raise();
    }
}

} // namespace AetherSDR
