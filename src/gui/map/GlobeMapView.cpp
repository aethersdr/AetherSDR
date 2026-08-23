#include "GlobeMapView.h"

#include "MapHoverPathSelection.h"
#include "SolarTerminator.h"
#include "core/ThemeManager.h"

#include <QGeoView/QGVGlobal.h>

#include <QDateTime>
#include <QEasingCurve>
#include <QGestureEvent>
#include <QImageReader>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QOpenGLShaderProgram>
#include <QOpenGLTexture>
#include <QPainter>
#include <QPainterPath>
#include <QPinchGesture>
#include <QResizeEvent>
#include <QToolButton>
#include <QVariantAnimation>
#include <QWheelEvent>
#include <QtMath>

#include <algorithm>
#include <cmath>

namespace AetherSDR {

namespace {
constexpr int kAtlasZoom = 2;
constexpr int kTileCount = 1 << kAtlasZoom;
constexpr int kTileSize = 256;
constexpr int kAtlasSize = kTileCount * kTileSize;
constexpr int kLatitudeSegments = 96;
constexpr int kLongitudeSegments = 192;
constexpr int kMaximumConcurrentTileRequests = 4;
constexpr qint64 kMaximumTileBytes = 1024 * 1024;
constexpr float kMinimumCameraDistance = 1.55F;
constexpr float kMaximumCameraDistance = 6.0F;
constexpr float kDefaultCameraDistance = 3.1F;
constexpr float kZoomFactor = 0.78F;
constexpr int kGreatCircleSegments = 48;

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

QVector3D greatCirclePoint(const QVector3D& from, const QVector3D& to,
                           float amount)
{
    const float dot = std::clamp(QVector3D::dotProduct(from, to),
                                 -1.0F, 1.0F);
    const float angle = std::acos(dot);
    if (angle < 0.0001F) {
        return from;
    }
    const float sine = std::sin(angle);
    if (std::abs(sine) < 0.0001F) {
        return (from * (1.0F - amount) + to * amount).normalized();
    }
    return (from * (std::sin((1.0F - amount) * angle) / sine)
            + to * (std::sin(amount * angle) / sine)).normalized();
}
}

GlobeMapView::GlobeMapView(QWidget* parent)
    : QOpenGLWidget(parent)
    , m_atlas(kAtlasSize, kAtlasSize, QImage::Format_RGBA8888)
{
    setObjectName(QStringLiteral("pskReporterGlobe"));
    setAccessibleName(tr("PSK Reporter globe"));
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    grabGesture(Qt::PinchGesture);

    m_rotation = QQuaternion();
    m_atlasUploadTimer.setSingleShot(true);
    m_atlasUploadTimer.setInterval(50);
    connect(&m_atlasUploadTimer, &QTimer::timeout, this, [this] {
        m_atlasDirty = true;
        update();
    });

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
    if (!isValid()) {
        return;
    }
    makeCurrent();
    m_texture.reset();
    if (m_vertexBuffer.isCreated()) {
        m_vertexBuffer.destroy();
    }
    if (m_indexBuffer.isCreated()) {
        m_indexBuffer.destroy();
    }
    doneCurrent();
}

void GlobeMapView::initializeGL()
{
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
        qWarning("PSK Reporter globe shader setup failed: %s",
                 qPrintable(m_program->log()));
        m_program.reset();
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
    model.rotate(m_rotation);
    const QMatrix4x4 viewProjection = projection * view;
    const QMatrix4x4 matrix = viewProjection * model;

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
    m_program->disableAttributeArray(positionLocation);
    m_program->disableAttributeArray(uvLocation);
    m_indexBuffer.release();
    m_vertexBuffer.release();
    m_texture->release();
    m_program->release();

    glDisable(GL_DEPTH_TEST);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    paintPaths(painter, model, viewProjection);
    paintMarkers(painter, model, viewProjection);
    painter.end();
    glEnable(GL_DEPTH_TEST);
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

void GlobeMapView::requestAtlasTiles()
{
    m_pendingTiles.clear();
    for (int y = 0; y < kTileCount; ++y) {
        for (int x = 0; x < kTileCount; ++x) {
            m_pendingTiles.append({ x, y });
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
        const QPair<int, int> tile = m_pendingTiles.takeFirst();
        const QUrl url(QStringLiteral("https://tile.openstreetmap.org/%1/%2/%3.png")
            .arg(kAtlasZoom).arg(tile.first).arg(tile.second));
        QNetworkRequest request(url);
        request.setHeader(QNetworkRequest::UserAgentHeader,
                          QGV::getTileUserAgent());
        request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                             QNetworkRequest::PreferCache);
        QNetworkReply* reply = manager->get(request);
        ++m_activeTileRequests;
        connect(reply, &QNetworkReply::downloadProgress, reply,
                [reply](qint64 received, qint64) {
                    if (received > kMaximumTileBytes) {
                        reply->abort();
                    }
                });
        connect(reply, &QNetworkReply::finished, this,
                [this, reply, tile] {
                    --m_activeTileRequests;
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
                            QPainter atlasPainter(&m_atlas);
                            atlasPainter.drawImage(tile.first * kTileSize,
                                                   tile.second * kTileSize,
                                                   image);
                            scheduleAtlasUpload();
                        }
                    }
                    requestNextTiles();
                });
    }
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
    if (rotated.z() <= 0.015F) {
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
    QVector<Marker> paths;
    if (m_pathsVisible) {
        paths = m_markers;
    } else if (m_hoverMarker >= 0) {
        paths = MapHoverPathSelection::pathsForMarker(m_markers,
                                                       m_hoverMarker);
    }
    for (const Marker& marker : std::as_const(paths)) {
        if (marker.pathEnabled) {
            paintPathForMarker(painter, marker, model, viewProjection);
        }
    }
}

void GlobeMapView::paintPathForMarker(QPainter& painter, const Marker& marker,
                                      const QMatrix4x4& model,
                                      const QMatrix4x4& viewProjection)
{
    if (!marker.hasPathOrigin && !m_hasHome) {
        return;
    }
    const QVector3D from = marker.hasPathOrigin
        ? geoPoint(marker.pathFromLat, marker.pathFromLon)
        : geoPoint(m_homeLat, m_homeLon);
    const QVector3D to = geoPoint(marker.lat, marker.lon);
    QColor pathColor = marker.color;
    pathColor.setAlpha(185);
    QPen pen(pathColor, m_pathsVisible ? 1.25 : 2.4,
             Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter.setPen(pen);

    QPointF previous;
    bool previousVisible = false;
    for (int segment = 0; segment <= kGreatCircleSegments; ++segment) {
        const float amount = static_cast<float>(segment)
            / kGreatCircleSegments;
        QPointF point;
        const bool visible = projectPoint(
            greatCirclePoint(from, to, amount), model, viewProjection, &point);
        if (visible && previousVisible) {
            painter.drawLine(previous, point);
        }
        previous = point;
        previousVisible = visible;
    }
}

void GlobeMapView::paintMarkers(QPainter& painter, const QMatrix4x4& model,
                                const QMatrix4x4& viewProjection)
{
    m_projectedMarkers.resize(m_markers.size());
    for (int index = 0; index < m_markers.size(); ++index) {
        const Marker& marker = m_markers.at(index);
        QPointF point;
        const bool visible = projectPoint(geoPoint(marker.lat, marker.lon),
                                          model, viewProjection, &point);
        m_projectedMarkers[index] = { point, visible };
        if (!visible) {
            continue;
        }
        const qreal radius = marker.isMonitor ? 5.0 : 4.0;
        painter.setPen(QPen(m_backgroundColor, 1.2));
        painter.setBrush(marker.color);
        painter.drawEllipse(point, radius, radius);
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
    if (m_terminatorVisible == visible) {
        return;
    }
    m_terminatorVisible = visible;
    update();
}

void GlobeMapView::setLegend(
    const QVector<QPair<QString, QColor>>& entries)
{
    m_legendEntries = entries;
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
        const QVector3D home = geoPoint(m_homeLat, m_homeLon);
        m_rotation = QQuaternion::rotationTo(home, { 0.0F, 0.0F, 1.0F });
    } else {
        m_rotation = QQuaternion();
    }
    m_cameraDistance = kDefaultCameraDistance;
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
                update();
            });
    m_zoomAnimation->start();
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
    const QQuaternion yaw = QQuaternion::fromAxisAndAngle(
        { 0.0F, 1.0F, 0.0F }, static_cast<float>(delta.x())
                                      * degreesPerPixel);
    const QQuaternion pitch = QQuaternion::fromAxisAndAngle(
        { 1.0F, 0.0F, 0.0F }, static_cast<float>(delta.y())
                                      * degreesPerPixel);
    m_rotation = yaw * pitch * m_rotation;
    m_rotation.normalize();
}

void GlobeMapView::mouseMoveEvent(QMouseEvent* event)
{
    if (m_dragging) {
        const QPointF delta = event->position() - m_lastPointerPosition;
        if (QLineF(QPointF(), delta).length() >= 1.0) {
            m_hasMovedDuringDrag = true;
            applyDragDelta(delta);
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
        const float factor = std::exp(static_cast<float>(-delta) * 0.004F);
        m_cameraDistance = std::clamp(m_cameraDistance * factor,
                                      kMinimumCameraDistance,
                                      kMaximumCameraDistance);
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
            const float factor = std::exp(
                static_cast<float>(-gesture->value()) * 1.5F);
            m_cameraDistance = std::clamp(m_cameraDistance * factor,
                                          kMinimumCameraDistance,
                                          kMaximumCameraDistance);
            update();
            return true;
        }
    } else if (event->type() == QEvent::Gesture) {
        auto* gestureEvent = static_cast<QGestureEvent*>(event);
        if (auto* pinch = static_cast<QPinchGesture*>(
                gestureEvent->gesture(Qt::PinchGesture))) {
            const qreal scale = pinch->scaleFactor();
            if (scale > 0.0) {
                m_cameraDistance = std::clamp(
                    m_cameraDistance / static_cast<float>(scale),
                    kMinimumCameraDistance, kMaximumCameraDistance);
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

void GlobeMapView::layoutOverlays()
{
    constexpr int margin = 8;
    constexpr int gap = 6;
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
