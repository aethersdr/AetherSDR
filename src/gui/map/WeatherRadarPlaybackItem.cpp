#include "WeatherRadarPlaybackItem.h"
#include "WeatherRadarPainterGeometry.h"
#include "WeatherRadarWorldWrap.h"
#include "WeatherRadarTexture.h"

#include <QGeoView/QGVMap.h>
#include <QGeoView/QGVMapQGItem.h>
#include <QGeoView/QGVMapQGView.h>

#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QLoggingCategory>
#include <QMetaObject>
#include <QOpenGLBuffer>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLTexture>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLWidget>
#include <QPaintDevice>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPolygonF>
#include <QTimer>
#include <QTransform>

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <memory>
#include <utility>
#include <vector>

namespace AetherSDR {

Q_LOGGING_CATEGORY(lcWeatherRadarFlatPlayback,
                   "aether.weather.radar.flat")

namespace {

constexpr double kWeatherRadarOpacity = 0.78;
constexpr int kRadarUploadStripeRows = 384;

struct RadarCompositeVertex {
    float position[2];
    float uv[2];
};

struct RadarTextureSlot {
    std::unique_ptr<QOpenGLTexture> texture;
    QDateTime frameTime;
    QRectF bounds;
    QSize imageSize;
    qint64 imageKey{0};

    RadarTextureSlot() = default;
    RadarTextureSlot(RadarTextureSlot&&) noexcept = default;
    RadarTextureSlot& operator=(RadarTextureSlot&&) noexcept = default;
    RadarTextureSlot(const RadarTextureSlot&) = delete;
    RadarTextureSlot& operator=(const RadarTextureSlot&) = delete;

    bool matches(const QImage& image, const QDateTime& time,
                 const QRectF& requestedBounds) const
    {
        return texture != nullptr && !image.isNull()
            && imageKey == image.cacheKey() && frameTime == time
            && bounds == requestedBounds && imageSize == image.size();
    }
};

enum class RadarUploadTarget {
    None,
    Current,
    Preloaded
};

struct RadarTextureUpload {
    RadarUploadTarget target{RadarUploadTarget::None};
    QImage image;
    QDateTime frameTime;
    QRectF bounds;
    qint64 imageKey{0};
    int nextRow{0};
    std::unique_ptr<QOpenGLTexture> texture;

    bool matches(RadarUploadTarget requestedTarget,
                 const QImage& requestedImage,
                 const QDateTime& requestedTime,
                 const QRectF& requestedBounds) const
    {
        return target == requestedTarget && !requestedImage.isNull()
            && imageKey == requestedImage.cacheKey()
            && frameTime == requestedTime && bounds == requestedBounds
            && image.size() == requestedImage.size();
    }

    bool active() const
    {
        return target != RadarUploadTarget::None;
    }
};

void disableDeviceCache(QGVDrawItem* drawItem, QGVMap* geoMap)
{
    for (QGraphicsItem* item : geoMap->geoView()->scene()->items()) {
        if (QGVMapQGItem::geoObjectFromQGItem(item) == drawItem) {
            // This item's pixels change every presentation tick. Rebuilding a
            // viewport-sized device cache before every paint adds a copy and
            // provides no reusable result.
            item->setCacheMode(QGraphicsItem::NoCache);
            return;
        }
    }
}

} // namespace

struct WeatherRadarPlaybackItem::OpenGlResources {
    QPointer<QOpenGLContext> context;
    QPointer<QOpenGLWidget> viewport;
    QMetaObject::Connection contextCleanupConnection;
    std::unique_ptr<QOpenGLShaderProgram> program;
    QOpenGLBuffer vertexBuffer{QOpenGLBuffer::VertexBuffer};
    QOpenGLBuffer indexBuffer{QOpenGLBuffer::IndexBuffer};
    QOpenGLVertexArrayObject vertexArray;
    RadarTextureSlot current;
    RadarTextureSlot preloaded;
    RadarTextureSlot spare;
    std::vector<RadarTextureSlot> retired;
    RadarTextureUpload upload;
    QSize indexGridSize;
    int indexCount{0};
    bool initializationFailed{false};
    bool firstCompositeDrawLogged{false};
    bool cleaningUp{false};
};

WeatherRadarPlaybackItem::WeatherRadarPlaybackItem()
{
    setSelectable(false);
}

WeatherRadarPlaybackItem::~WeatherRadarPlaybackItem()
{
    releaseOpenGlResources();
}

bool WeatherRadarPlaybackItem::setFrame(
    const QImage& image, const QDateTime& frameTime, const QRectF& bounds)
{
    const QRectF normalizedBounds = bounds.normalized();
    if (image.isNull() || !frameTime.isValid()
        || !normalizedBounds.isValid() || normalizedBounds.isEmpty()) {
        return false;
    }
    QOpenGLWidget* viewport = getMap() != nullptr && getMap()->geoView() != nullptr
        ? qobject_cast<QOpenGLWidget*>(getMap()->geoView()->viewport()) : nullptr;
    if (viewport == nullptr || !ready()
        || (m_openGlResources != nullptr && m_openGlResources->initializationFailed)) {
        // First paint may use the raster fallback while the GPU upload completes.
        applyFrame(image, frameTime, normalizedBounds);
        return true;
    }
    OpenGlResources* resources = m_openGlResources.get();
    if (resources == nullptr || resources->context != viewport->context()) {
        preloadFrame(image, frameTime, normalizedBounds);
        return false;
    }
    for (RadarTextureSlot* slot : { &resources->current, &resources->preloaded,
                                    &resources->spare }) {
        if (!slot->matches(image, frameTime, normalizedBounds)) {
            continue;
        }
        if (slot != &resources->current) {
            // Move ownership without destroying GL objects outside a paint.
            std::swap(resources->current, *slot);
        }
        m_preloadImage = {};
        m_preloadFrameTime = {};
        m_preloadBounds = {};
        applyFrame(image, frameTime, normalizedBounds);
        return true;
    }
    // Retain the complete displayed observation until the lookahead is ready.
    preloadFrame(image, frameTime, normalizedBounds);
    return false;
}

void WeatherRadarPlaybackItem::preloadFrame(
    const QImage& image, const QDateTime& frameTime, const QRectF& bounds)
{
    const QRectF normalizedBounds = bounds.normalized();
    if (image.isNull() || !frameTime.isValid()
        || !normalizedBounds.isValid() || normalizedBounds.isEmpty()) {
        return;
    }

    QOpenGLWidget* viewport = nullptr;
    if (getMap() != nullptr && getMap()->geoView() != nullptr) {
        viewport = qobject_cast<QOpenGLWidget*>(
            getMap()->geoView()->viewport());
    }
    if (viewport == nullptr
        || (m_openGlResources != nullptr
            && m_openGlResources->initializationFailed)) {
        QTimer::singleShot(0, this, [this, frameTime] {
            emit framePreloaded(frameTime);
        });
        return;
    }

    if (m_openGlResources != nullptr) {
        const OpenGlResources& resources = *m_openGlResources;
        const bool resident = resources.current.matches(
                image, frameTime, normalizedBounds)
            || resources.preloaded.matches(
                image, frameTime, normalizedBounds)
            || resources.spare.matches(
                image, frameTime, normalizedBounds);
        if (resident) {
            QTimer::singleShot(0, this, [this, frameTime] {
                emit framePreloaded(frameTime);
            });
            return;
        }
        if (resources.upload.matches(
                RadarUploadTarget::Preloaded, image, frameTime,
                normalizedBounds)) {
            // A striped upload advances only from paint callbacks. Ensure a
            // boundary retry keeps it scheduled even when the scene has
            // coalesced an earlier identical repaint request.
            repaint();
            return;
        }
    }
    if (m_preloadFrameTime == frameTime
        && m_preloadBounds == normalizedBounds
        && m_preloadImage.cacheKey() == image.cacheKey()) {
        repaint();
        return;
    }
    m_preloadImage = image;
    m_preloadFrameTime = frameTime;
    m_preloadBounds = normalizedBounds;
    repaint();
}

void WeatherRadarPlaybackItem::applyFrame(
    const QImage& current, const QDateTime& currentFrameTime,
    const QRectF& bounds)
{
    // Hidden QGeoView items do not receive onCamera(). On first Play or a
    // globe-to-flat switch, our stored camera can still describe the tiny
    // initial viewport around (0,0). Using it to build projShape() culls the
    // real radar AND the paints needed to preload the next texture, leaving
    // playback stuck until another zoom. Sample the actual camera here.
    const QRectF cameraBounds = getMap() != nullptr
        ? getMap()->getCamera().projRect().normalized() : m_cameraBounds;
    const bool geometryChanged = m_bounds != bounds
        || weatherRadarVisibleWorldOffsets(m_bounds, m_cameraBounds)
            != weatherRadarVisibleWorldOffsets(bounds, cameraBounds);
    m_cameraBounds = cameraBounds;
    m_current = current;
    m_currentFrameTime = currentFrameTime;
    m_bounds = bounds;
    if (geometryChanged) {
        resetBoundary();
        refresh();
    } else {
        repaint();
    }
}

void WeatherRadarPlaybackItem::acknowledgeFrame(quint64 presentationSequence)
{
    m_pendingPresentationSequence = presentationSequence;
    repaint();
}

void WeatherRadarPlaybackItem::clear()
{
    if (m_current.isNull()) {
        return;
    }
    m_current = {};
    m_preloadImage = {};
    m_currentFrameTime = {};
    m_preloadFrameTime = {};
    m_preloadBounds = {};
    m_pendingPresentationSequence = 0;
    repaint();
}

bool WeatherRadarPlaybackItem::ready() const
{
    return !m_current.isNull()
        && m_bounds.isValid() && !m_bounds.isEmpty();
}

void WeatherRadarPlaybackItem::onProjection(QGVMap* geoMap)
{
    QGVDrawItem::onProjection(geoMap);
    disableDeviceCache(this, geoMap);
    m_cameraBounds = geoMap->getCamera().projRect().normalized();
}

void WeatherRadarPlaybackItem::onCamera(
    const QGVCameraState& oldState, const QGVCameraState& newState)
{
    const QRectF nextBounds = newState.projRect().normalized();
    if (weatherRadarVisibleWorldOffsets(m_bounds, m_cameraBounds)
        != weatherRadarVisibleWorldOffsets(m_bounds, nextBounds)) {
        // Scene culling must know about every visible world copy, not just
        // the canonical source bbox. Re-home as the camera pans indefinitely.
        resetBoundary();
        m_cameraBounds = nextBounds;
        refresh();
    } else {
        m_cameraBounds = nextBounds;
    }
    QGVDrawItem::onCamera(oldState, newState);
}

QPainterPath WeatherRadarPlaybackItem::projShape() const
{
    QPainterPath path;
    for (const double offset : weatherRadarVisibleWorldOffsets(m_bounds, m_cameraBounds)) {
        path.addRect(m_bounds.translated(offset, 0));
    }
    return path;
}

void WeatherRadarPlaybackItem::projPaint(QPainter* painter)
{
    if (!ready()) {
        return;
    }
    for (const double offset : weatherRadarVisibleWorldOffsets(m_bounds, m_cameraBounds)) {
        painter->save();
        // Repeat the SAME texture/image at exact Mercator world-width
        // offsets. Never request different observations for different copies.
        painter->translate(offset, 0);
        painter->setClipRect(m_bounds, Qt::IntersectClip);
        painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter->setCompositionMode(QPainter::CompositionMode_SourceOver);
        if (!paintOpenGlComposite(painter)) {
            paintRasterFallback(painter);
        }
        painter->restore();
    }
    if (m_pendingPresentationSequence != 0) {
        const quint64 sequence = m_pendingPresentationSequence;
        m_pendingPresentationSequence = 0;
        emit presented(sequence);
    }
}

void WeatherRadarPlaybackItem::releaseOpenGlResources()
{
    if (m_openGlResources == nullptr
        || m_openGlResources->cleaningUp) {
        return;
    }
    m_openGlResources->cleaningUp = true;
    std::unique_ptr<OpenGlResources> resources =
        std::move(m_openGlResources);
    disconnect(resources->contextCleanupConnection);

    QOpenGLContext* targetContext = resources->context.data();
    QOpenGLWidget* viewport = resources->viewport.data();
    QOpenGLContext* previousContext = QOpenGLContext::currentContext();
    QSurface* previousSurface = previousContext != nullptr
        ? previousContext->surface() : nullptr;
    bool madeCurrent = false;
    if (targetContext != nullptr && targetContext->isValid()
        && previousContext != targetContext && viewport != nullptr) {
        viewport->makeCurrent();
        madeCurrent = QOpenGLContext::currentContext() == targetContext;
    }

    // Every wrapper below owns a GL object. Destroy it while the owning
    // QOpenGLWidget context is current, including from aboutToBeDestroyed.
    resources.reset();

    if (madeCurrent) {
        if (previousContext != nullptr && previousSurface != nullptr
            && previousContext->isValid()) {
            previousContext->makeCurrent(previousSurface);
        } else if (viewport != nullptr) {
            viewport->doneCurrent();
        }
    }
}

bool WeatherRadarPlaybackItem::paintOpenGlComposite(QPainter* painter)
{
    if (getMap() == nullptr || getMap()->geoView() == nullptr) {
        return false;
    }
    QOpenGLWidget* viewport = qobject_cast<QOpenGLWidget*>(
        getMap()->geoView()->viewport());
    if (viewport == nullptr) {
        if (m_preloadFrameTime.isValid()) {
            const QDateTime completedTime = m_preloadFrameTime;
            m_preloadImage = {};
            m_preloadFrameTime = {};
            m_preloadBounds = {};
            QTimer::singleShot(0, this, [this, completedTime] {
                emit framePreloaded(completedTime);
            });
        }
        return false;
    }
    if (!viewport->isValid() || viewport->context() == nullptr) {
        return false;
    }
    if (m_openGlResources != nullptr
        && m_openGlResources->context != viewport->context()) {
        releaseOpenGlResources();
    }

    painter->beginNativePainting();
    QOpenGLContext* context = QOpenGLContext::currentContext();
    if (context == nullptr || context != viewport->context()) {
        painter->endNativePainting();
        return false;
    }
    QOpenGLFunctions* functions = context->functions();
    functions->initializeOpenGLFunctions();

    if (m_openGlResources == nullptr) {
        m_openGlResources = std::make_unique<OpenGlResources>();
        OpenGlResources& resources = *m_openGlResources;
        resources.context = context;
        resources.viewport = viewport;
        resources.contextCleanupConnection = connect(
            context, &QOpenGLContext::aboutToBeDestroyed,
            this, &WeatherRadarPlaybackItem::releaseOpenGlResources,
            Qt::DirectConnection);

        static constexpr char kVertexShader[] = R"(
            attribute highp vec2 position;
            attribute highp vec2 textureCoordinate;
            varying highp vec2 uv;
            void main() {
                uv = textureCoordinate;
                gl_Position = vec4(position, 0.0, 1.0);
            }
        )";
        static const QByteArray kFragmentShader = QByteArray(R"(
            varying highp vec2 uv;
            uniform sampler2D currentFrame;
            uniform lowp float opacity;
        )") + WeatherRadarTexture::kFragmentFunctions + R"(
            void main() {
                gl_FragColor = radarSample(currentFrame, uv) * opacity;
            }
        )";
        resources.program = std::make_unique<QOpenGLShaderProgram>();
        const bool shadersReady =
            resources.program->addShaderFromSourceCode(
                QOpenGLShader::Vertex, kVertexShader)
            && resources.program->addShaderFromSourceCode(
                QOpenGLShader::Fragment, kFragmentShader);
        if (shadersReady) {
            resources.program->bindAttributeLocation("position", 0);
            resources.program->bindAttributeLocation("textureCoordinate", 1);
        }
        if (!shadersReady || !resources.program->link()
            || !resources.vertexArray.create()
            || !resources.vertexBuffer.create()
            || !resources.indexBuffer.create()) {
            const QString initializationLog = resources.program != nullptr
                ? resources.program->log() : QString{};
            qCWarning(lcWeatherRadarFlatPlayback)
                << "Native OpenGL radar compositor unavailable; using "
                   "the raster fallback"
                << initializationLog;
            resources.initializationFailed = true;
            resources.program.reset();
            painter->endNativePainting();
            if (m_preloadFrameTime.isValid()) {
                const QDateTime completedTime = m_preloadFrameTime;
                m_preloadImage = {};
                m_preloadFrameTime = {};
                m_preloadBounds = {};
                QTimer::singleShot(0, this, [this, completedTime] {
                    emit framePreloaded(completedTime);
                });
            }
            return false;
        }
        resources.vertexBuffer.setUsagePattern(
            QOpenGLBuffer::DynamicDraw);
        resources.indexBuffer.setUsagePattern(
            QOpenGLBuffer::StaticDraw);
    }

    OpenGlResources& resources = *m_openGlResources;
    if (resources.initializationFailed) {
        painter->endNativePainting();
        if (m_preloadFrameTime.isValid()) {
            const QDateTime completedTime = m_preloadFrameTime;
            m_preloadImage = {};
            m_preloadFrameTime = {};
            m_preloadBounds = {};
            QTimer::singleShot(0, this, [this, completedTime] {
                emit framePreloaded(completedTime);
            });
        }
        return false;
    }

    // Pointer moves happen in setFrame(), where no GL context is current.
    // Actual deletion is deferred to this paint callback.
    resources.retired.clear();

    const auto retireSlot = [&resources](RadarTextureSlot&& slot) {
        if (slot.texture == nullptr) {
            return;
        }
        if (resources.spare.texture == nullptr) {
            resources.spare = std::move(slot);
        } else {
            resources.retired.push_back(std::move(slot));
        }
    };
    const auto replaceSlot = [&retireSlot](RadarTextureSlot* destination,
                                            RadarTextureSlot&& replacement) {
        retireSlot(std::move(*destination));
        *destination = std::move(replacement);
    };

    // Reuse any exact retained slot before scheduling a transfer. This keeps
    // Stop/Play and the cached loop start from needlessly re-uploading a
    // decoded frame that is still resident in bounded GL storage.
    const QRectF normalizedBounds = m_bounds.normalized();
    if (!resources.current.matches(
            m_current, m_currentFrameTime, normalizedBounds)) {
        RadarTextureSlot* candidate = nullptr;
        for (RadarTextureSlot* slot : {
                 &resources.preloaded, &resources.spare }) {
            if (slot->matches(
                    m_current, m_currentFrameTime, normalizedBounds)) {
                candidate = slot;
                break;
            }
        }
        if (candidate != nullptr) {
            RadarTextureSlot replacement = std::move(*candidate);
            *candidate = {};
            replaceSlot(&resources.current, std::move(replacement));
        }
    }
    RadarUploadTarget requestedTarget = RadarUploadTarget::None;
    const QImage* requestedImage = nullptr;
    const QDateTime* requestedFrameTime = nullptr;
    const QRectF* requestedBounds = nullptr;
    if (!resources.current.matches(
            m_current, m_currentFrameTime, normalizedBounds)) {
        requestedTarget = RadarUploadTarget::Current;
        requestedImage = &m_current;
        requestedFrameTime = &m_currentFrameTime;
        requestedBounds = &normalizedBounds;
    } else if (!m_preloadImage.isNull()
               && m_preloadFrameTime.isValid()
               && !resources.current.matches(
                   m_preloadImage, m_preloadFrameTime,
                   m_preloadBounds)
               && !resources.preloaded.matches(
                   m_preloadImage, m_preloadFrameTime,
                   m_preloadBounds)
               && !resources.spare.matches(
                   m_preloadImage, m_preloadFrameTime,
                   m_preloadBounds)) {
        requestedTarget = RadarUploadTarget::Preloaded;
        requestedImage = &m_preloadImage;
        requestedFrameTime = &m_preloadFrameTime;
        requestedBounds = &m_preloadBounds;
    }

    QDateTime completedPreload;
    if (requestedTarget != RadarUploadTarget::None
        && requestedImage != nullptr && requestedFrameTime != nullptr
        && requestedBounds != nullptr) {
        if (!resources.upload.matches(
                requestedTarget, *requestedImage, *requestedFrameTime,
                *requestedBounds)) {
            if (resources.upload.texture != nullptr) {
                if (resources.spare.texture == nullptr
                    && resources.upload.texture->width()
                        == requestedImage->width()
                    && resources.upload.texture->height()
                        == requestedImage->height()) {
                    resources.spare.texture =
                        std::move(resources.upload.texture);
                    resources.spare.imageSize = requestedImage->size();
                } else {
                    resources.upload.texture.reset();
                }
            }
            resources.upload = {};
            resources.upload.target = requestedTarget;
            resources.upload.image = *requestedImage;
            resources.upload.frameTime = *requestedFrameTime;
            resources.upload.bounds = *requestedBounds;
            resources.upload.imageKey = requestedImage->cacheKey();
            if (resources.spare.texture != nullptr
                && resources.spare.texture->width()
                    == requestedImage->width()
                && resources.spare.texture->height()
                    == requestedImage->height()) {
                resources.upload.texture =
                    std::move(resources.spare.texture);
                resources.spare = {};
            } else {
                resources.spare = {};
                resources.upload.texture =
                    std::make_unique<QOpenGLTexture>(
                        QOpenGLTexture::Target2D);
                resources.upload.texture->setFormat(
                    QOpenGLTexture::RGBA8_UNorm);
                resources.upload.texture->setSize(
                    requestedImage->width(), requestedImage->height());
                resources.upload.texture->setMipLevels(1);
                resources.upload.texture->allocateStorage(
                    QOpenGLTexture::RGBA, QOpenGLTexture::UInt8);
            }
            if (resources.upload.texture != nullptr) {
                resources.upload.texture->setMinificationFilter(
                    QOpenGLTexture::Linear);
                resources.upload.texture->setMagnificationFilter(
                    QOpenGLTexture::Linear);
                resources.upload.texture->setWrapMode(
                    QOpenGLTexture::DirectionS,
                    QOpenGLTexture::ClampToEdge);
                resources.upload.texture->setWrapMode(
                    QOpenGLTexture::DirectionT,
                    QOpenGLTexture::ClampToEdge);
            }
        }

        if (resources.upload.texture == nullptr
            || !resources.upload.texture->isStorageAllocated()) {
            resources.upload = {};
            resources.initializationFailed = true;
        } else {
            const int remainingRows = resources.upload.image.height()
                                    - resources.upload.nextRow;
            const int rowCount = std::min(
                kRadarUploadStripeRows, remainingRows);
            if (!WeatherRadarTexture::uploadRows(*resources.upload.texture,
                    resources.upload.image, resources.upload.nextRow, rowCount)) {
                resources.upload = {};
                resources.initializationFailed = true;
            } else {
                resources.upload.nextRow += rowCount;
            }
            if (!resources.initializationFailed && resources.upload.nextRow
                >= resources.upload.image.height()) {
                RadarTextureSlot completed;
                completed.texture = std::move(resources.upload.texture);
                completed.frameTime = resources.upload.frameTime;
                completed.bounds = resources.upload.bounds;
                completed.imageSize = resources.upload.image.size();
                completed.imageKey = resources.upload.imageKey;
                const RadarUploadTarget completedTarget =
                    resources.upload.target;
                resources.upload = {};
                if (completedTarget == RadarUploadTarget::Current) {
                    replaceSlot(&resources.current,
                                std::move(completed));
                } else {
                    completedPreload = completed.frameTime;
                    replaceSlot(&resources.preloaded,
                                std::move(completed));
                }
            }
        }
    } else if (!m_preloadImage.isNull()
               && (resources.current.matches(
                       m_preloadImage, m_preloadFrameTime,
                       m_preloadBounds)
                   || resources.preloaded.matches(
                       m_preloadImage, m_preloadFrameTime,
                       m_preloadBounds)
                   || resources.spare.matches(
                       m_preloadImage, m_preloadFrameTime,
                       m_preloadBounds))) {
        completedPreload = m_preloadFrameTime;
    }

    if (completedPreload.isValid()
        && completedPreload == m_preloadFrameTime) {
        m_preloadImage = {};
        m_preloadFrameTime = {};
        m_preloadBounds = {};
    }

    const bool texturesReady = resources.current.matches(
            m_current, m_currentFrameTime, normalizedBounds);
    if (!texturesReady || resources.initializationFailed) {
        painter->endNativePainting();
        if (resources.initializationFailed
            && m_preloadFrameTime.isValid()) {
            completedPreload = m_preloadFrameTime;
            m_preloadImage = {};
            m_preloadFrameTime = {};
            m_preloadBounds = {};
        }
        if (!resources.initializationFailed
            && (resources.upload.active()
                || !m_preloadImage.isNull())) {
            repaint();
        }
        if (completedPreload.isValid()) {
            QTimer::singleShot(0, this, [this, completedPreload] {
                emit framePreloaded(completedPreload);
            });
        }
        return false;
    }

    const QSize gridSize(2, 2);
    GLint glViewport[4]{0, 0, 0, 0};
    functions->glGetIntegerv(GL_VIEWPORT, glViewport);
    QPaintDevice* paintDevice = painter->device();
    if (glViewport[2] <= 0 || glViewport[3] <= 0
        || paintDevice == nullptr
        || paintDevice->devicePixelRatioF() <= 0.0) {
        painter->endNativePainting();
        return false;
    }
    // GL reports a physical-pixel, bottom-left viewport. QPainter reports
    // physical-pixel, top-left positions; only the viewport origin needs
    // conversion. Its device transform already includes Retina/DPI scaling.
    const QRectF deviceViewport(
        glViewport[0],
        paintDevice->height() * paintDevice->devicePixelRatioF()
            - glViewport[1] - glViewport[3],
        glViewport[2], glViewport[3]);
    QVector<RadarCompositeVertex> vertices;
    vertices.reserve(gridSize.width() * gridSize.height());
    for (int y = 0; y < gridSize.height(); ++y) {
        const double v = static_cast<double>(y)
                       / (gridSize.height() - 1);
        for (int x = 0; x < gridSize.width(); ++x) {
            const double u = static_cast<double>(x)
                           / (gridSize.width() - 1);
            const QPointF normalized(u, v);
            const QPointF itemPoint(
                m_bounds.left() + u * m_bounds.width(),
                m_bounds.top() + v * m_bounds.height());
            const QPointF clipPosition = weatherRadarPainterClipPosition(
                *painter, itemPoint, deviceViewport);
            RadarCompositeVertex vertex{};
            vertex.position[0] = static_cast<float>(clipPosition.x());
            vertex.position[1] = static_cast<float>(clipPosition.y());
            vertex.uv[0] = static_cast<float>(u);
            vertex.uv[1] = static_cast<float>(v);
            vertices.append(vertex);
        }
    }

    resources.vertexArray.bind();
    resources.vertexBuffer.bind();
    resources.vertexBuffer.allocate(
        vertices.constData(),
        static_cast<int>(vertices.size() * sizeof(RadarCompositeVertex)));
    if (resources.indexGridSize != gridSize) {
        QVector<quint16> indices;
        indices.reserve(6 * (gridSize.width() - 1)
                        * (gridSize.height() - 1));
        for (int y = 0; y + 1 < gridSize.height(); ++y) {
            for (int x = 0; x + 1 < gridSize.width(); ++x) {
                const quint16 topLeft = static_cast<quint16>(
                    y * gridSize.width() + x);
                const quint16 topRight = topLeft + 1;
                const quint16 bottomLeft = static_cast<quint16>(
                    (y + 1) * gridSize.width() + x);
                const quint16 bottomRight = bottomLeft + 1;
                indices.append(topLeft);
                indices.append(topRight);
                indices.append(bottomRight);
                indices.append(topLeft);
                indices.append(bottomRight);
                indices.append(bottomLeft);
            }
        }
        resources.indexBuffer.bind();
        resources.indexBuffer.allocate(
            indices.constData(),
            static_cast<int>(indices.size() * sizeof(quint16)));
        resources.indexGridSize = gridSize;
        resources.indexCount = indices.size();
    } else {
        resources.indexBuffer.bind();
    }

    functions->glDisable(GL_DEPTH_TEST);
    functions->glDisable(GL_STENCIL_TEST);
    functions->glDisable(GL_SCISSOR_TEST);
    functions->glDisable(GL_CULL_FACE);
    functions->glEnable(GL_BLEND);
    functions->glBlendEquation(GL_FUNC_ADD);
    functions->glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    resources.program->bind();
    resources.program->setUniformValue("currentFrame", 1);
    resources.program->setUniformValue(
        "opacity", static_cast<float>(
            kWeatherRadarOpacity * painter->opacity()));
    resources.program->enableAttributeArray(0);
    resources.program->setAttributeBuffer(
        0, GL_FLOAT, offsetof(RadarCompositeVertex, position), 2,
        sizeof(RadarCompositeVertex));
    resources.program->enableAttributeArray(1);
    resources.program->setAttributeBuffer(
        1, GL_FLOAT, offsetof(RadarCompositeVertex, uv), 2,
        sizeof(RadarCompositeVertex));
    resources.current.texture->bind(1);
    functions->glDrawElements(GL_TRIANGLES, resources.indexCount,
                              GL_UNSIGNED_SHORT, nullptr);
    if (!resources.firstCompositeDrawLogged) {
        resources.firstCompositeDrawLogged = true;
        qCInfo(lcWeatherRadarFlatPlayback)
            << "2D radar playback is using the native OpenGL "
               "premultiplied compositor";
    }
    resources.current.texture->release(1);
    resources.program->disableAttributeArray(0);
    resources.program->disableAttributeArray(1);
    resources.program->release();
    resources.vertexArray.release();
    resources.indexBuffer.release();
    resources.vertexBuffer.release();
    painter->endNativePainting();

    if (resources.upload.active() || !m_preloadImage.isNull()) {
        repaint();
    }
    if (completedPreload.isValid()) {
        QTimer::singleShot(0, this, [this, completedPreload] {
            emit framePreloaded(completedPreload);
        });
    }
    return true;
}

void WeatherRadarPlaybackItem::paintRasterFallback(QPainter* painter)
{
    // Apply pan/zoom and DPR exactly once, including while striped GPU uploads
    // are pending. No intermediate image or offscreen morph is synthesized.
    painter->save();
    painter->setOpacity(painter->opacity() * kWeatherRadarOpacity);
    painter->drawImage(m_bounds, m_current);
    painter->restore();
}

} // namespace AetherSDR
