#include "GlobeMapView.h"

#include "MapHoverPathSelection.h"
#include "SolarTerminator.h"
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
constexpr int kMaximumVisibleDetailTiles = 160;
constexpr int kMaximumCachedDetailTiles = 256;
constexpr int kDetailTileSegments = 8;
constexpr qint64 kMaximumTileBytes = 1024 * 1024;
constexpr float kMinimumCameraDistance = 1.55F;
constexpr float kMaximumCameraDistance = 6.0F;
constexpr float kDefaultCameraDistance = 3.8F;
constexpr float kZoomFactor = 0.78F;

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
}

GlobeMapView::GlobeMapView(QWidget* parent)
    : QOpenGLWidget(parent)
    , m_atlas(kAtlasSize, kAtlasSize, QImage::Format_RGBA8888)
{
    setObjectName(QStringLiteral("pskReporterGlobe"));
    setAccessibleName(tr("PSK Reporter globe"));
    setAccessibleDescription(tr(
        "Drag to rotate, Shift-drag or use left and right brackets to tilt "
        "the axis, pinch to zoom, and use Home to reset"));
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    grabGesture(Qt::PinchGesture);

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

    m_attribution = new QLabel(
        QStringLiteral("© OpenStreetMap contributors"), this);
    m_attribution->setAttribute(Qt::WA_TransparentForMouseEvents);
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
    if (m_vertexBuffer.isCreated()) {
        m_vertexBuffer.destroy();
    }
    if (m_indexBuffer.isCreated()) {
        m_indexBuffer.destroy();
    }
    // QOpenGLShaderProgram owns a GL program object and must be destroyed
    // before doneCurrent(), just like the textures and buffers above.
    m_program.reset();
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
        void main() {
            lowp vec4 mapColor = texture2D(atlas, uv);
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
    projection.perspective(34.0F, aspect, 0.1F, 20.0F);
    QMatrix4x4 view;
    view.lookAt({ 0.0F, 0.0F, m_cameraDistance }, { 0.0F, 0.0F, 0.0F },
                { 0.0F, 1.0F, 0.0F });
    QMatrix4x4 model;
    model.rotate(m_navigation.rotation());
    const QMatrix4x4 viewProjection = projection * view;
    const QMatrix4x4 matrix = viewProjection * model;
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
    const QColor night = m_nightColor;
    m_program->setUniformValue("nightColor", QVector4D(
        night.redF(), night.greenF(), night.blueF(), 0.62F));
    m_program->setUniformValue("terminatorEnabled",
                               m_terminatorVisible ? 1.0F : 0.0F);
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
    m_vectorOverlay->update();
}

void GlobeMapView::paintVectorOverlay(QPainter& painter)
{
    if (!m_overlayMatricesValid) {
        return;
    }
    paintPaths(painter, m_overlayModel, m_overlayViewProjection);
    paintMarkers(painter, m_overlayModel, m_overlayViewProjection);
}

void GlobeMapView::uploadAtlas()
{
    m_texture.reset();
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

int GlobeMapView::detailZoomLevel() const
{
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
            (*found)->loading = false;
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
                    { zoom, candidate.x, candidate.y, false });
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
        m_pendingTiles.append({ zoom, candidate.x, candidate.y, false });
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

void GlobeMapView::destroyDetailTile(DetailTile& tile)
{
    tile.texture.reset();
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
        if (!visible.contains(iterator.key()) && !iterator.value()->loading) {
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
            m_pendingTiles.append({ kAtlasZoom, x, y, true });
        }
    }
    requestNextTiles();
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
        const QUrl url(QStringLiteral("https://tile.openstreetmap.org/%1/%2/%3.png")
            .arg(tile.zoom).arg(tile.x).arg(tile.y));
        QNetworkRequest request(url);
        request.setHeader(QNetworkRequest::UserAgentHeader,
                          QGV::getTileUserAgent());
        request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                             QNetworkRequest::PreferCache);
        QNetworkReply* reply = manager->get(request);
        m_tileReplies.insert(reply);
        ++m_activeTileRequests;
        connect(reply, &QNetworkReply::downloadProgress, reply,
                [reply](qint64 received, qint64) {
                    if (received > kMaximumTileBytes) {
                        reply->abort();
                    }
                });
        connect(reply, &QObject::destroyed, this, [this, reply] {
            m_tileReplies.remove(reply);
        });
        connect(reply, &QNetworkReply::finished, this,
                [this, reply, tile] {
                    m_tileReplies.remove(reply);
                    m_activeTileRequests = std::max(
                        0, m_activeTileRequests - 1);
                    QByteArray bytes;
                    if (reply->error() == QNetworkReply::NoError
                        && reply->bytesAvailable() <= kMaximumTileBytes) {
                        bytes = reply->read(kMaximumTileBytes + 1);
                    }
                    reply->deleteLater();
                    if (!bytes.isEmpty() && bytes.size() <= kMaximumTileBytes) {
                        QImage image;
                        image.loadFromData(bytes, "PNG");
                        if (!image.isNull() && image.width() == kTileSize
                            && image.height() == kTileSize) {
                            if (tile.baseAtlas) {
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
                    if (!tile.baseAtlas) {
                        const QString key = detailTileKey(
                            tile.zoom, tile.x, tile.y);
                        const auto found = m_detailTiles.constFind(key);
                        if (found != m_detailTiles.cend()) {
                            (*found)->loading = false;
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
    m_activeTileRequests = 0;
    for (QNetworkReply* reply : replies) {
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
    }
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
    m_hoverMarker = -1;
    m_hoverCard->hide();
    update();
}

void GlobeMapView::clearMarkers()
{
    m_markers.clear();
    m_projectedMarkers.clear();
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
    const float degreesPerPixel = 0.28F;
    m_navigation.applyDragDelta(delta, degreesPerPixel);
    m_detailSelectionDirty = true;
}

void GlobeMapView::applyRollDelta(float degrees)
{
    m_navigation.applyRollDelta(degrees);
    m_detailSelectionDirty = true;
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
                applyDragDelta(delta);
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
    if (m_hoverMarker == closest) {
        if (closest >= 0) {
            showHoverCard(closest, position);
        }
        return;
    }
    m_hoverMarker = closest;
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
    m_textColor = theme.color(this, "color.text.primary");
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
    m_attribution->adjustSize();
    m_attribution->move(width() - m_attribution->width() - margin,
                        height() - m_attribution->height() - margin);
    m_attribution->raise();
    if (m_legend->isVisible()) {
        m_legend->move(margin, height() - m_legend->height() - margin);
        m_legend->raise();
    }
}

} // namespace AetherSDR
