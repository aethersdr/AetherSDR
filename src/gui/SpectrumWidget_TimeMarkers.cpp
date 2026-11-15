#include "SpectrumWidget.h"
#include "DisplaySettings.h"
#include "core/ThemeManager.h"

#include <QAccessible>
#include <QDateTime>
#include <QFont>
#include <QFontMetrics>
#include <QPainter>
#include <QRectF>
#include <QTimeZone>
#include <QtMath>
#include <algorithm>

namespace AetherSDR {

void SpectrumWidget::setWaterfallTimeMarkerSeconds(int seconds)
{
    seconds = validWaterfallMarkerInterval(seconds);
    if (m_wfTimeMarkerSeconds == seconds) {
        return;
    }
    m_wfTimeMarkerSeconds = seconds;
    DisplaySettings::setWaterfallTimeMarkerSeconds(m_panIndex, seconds);
    QAccessibleValueChangeEvent event(this, seconds);
    QAccessible::updateAccessibility(&event);
    update();
}

QVector<WaterfallTimeMarker> SpectrumWidget::visibleWaterfallTimeMarkers(qreal height) const
{
    if (m_wfVisibleTimeRows.size() != m_waterfall.height()) {
        return {};
    }
    const qreal offset = m_waterfallScrollClock.isValid()
        ? m_waterfallScrollDistanceRows - waterfallScrollProgressRows() : 0.0;
    return waterfallTimeMarkers(m_wfVisibleTimeRows, m_wfWriteRow,
                                m_wfTimeMarkerSeconds, offset, height);
}

void SpectrumWidget::prepareWaterfallTimeMarkerAtlas(
    const QVector<WaterfallTimeMarker>& markers)
{
    QFont labelFont = font();
    labelFont.setPointSize(8);
    labelFont.setBold(false);
    const QFontMetrics metrics(labelFont);
    const int labelHeight = metrics.height() + 2;
    QVector<qint64> labels;
    qreal previousY = -labelHeight - 4;
    for (const WaterfallTimeMarker& marker : markers) {
        if (marker.y - previousY >= labelHeight + 4) {
            labels.push_back(marker.timestampMs);
            previousY = marker.y;
        }
    }
    const qreal dpr = devicePixelRatioF();
    const int atlasWidth = metrics.horizontalAdvance(QStringLiteral("00:00:00Z")) + 8;
    const QSize pixels(qCeil(atlasWidth * dpr),
                       qCeil((2 + labels.size() * labelHeight) * dpr));
    if (!m_wfTimeMarkerAtlasDirty && labels == m_wfTimeMarkerLabels
        && m_wfTimeMarkerAtlasFont == labelFont
        && m_wfTimeMarkerAtlas.size() == pixels
        && m_wfTimeMarkerAtlas.devicePixelRatio() == dpr) {
        return;
    }
    m_wfTimeMarkerLabels = labels;
    m_wfTimeMarkerAtlasFont = labelFont;
    m_wfTimeMarkerLabelHeight = labelHeight;
    m_wfTimeMarkerAtlas = QImage(pixels, QImage::Format_RGBA8888_Premultiplied);
    m_wfTimeMarkerAtlas.setDevicePixelRatio(dpr);
    m_wfTimeMarkerAtlas.fill(Qt::transparent);
    QPainter painter(&m_wfTimeMarkerAtlas);
    painter.setFont(labelFont);
    ThemeManager& theme = ThemeManager::instance();
    const QColor foreground = theme.color("color.waterfall.timeMarker.foreground");
    QColor line = foreground;
    line.setAlphaF(line.alphaF() * 0.55);
    painter.fillRect(QRect(0, 0, atlasWidth, 2), line);
    for (int index = 0; index < labels.size(); ++index) {
        const QRect box(0, 2 + index * labelHeight, atlasWidth, labelHeight);
        painter.fillRect(box, theme.color("color.waterfall.timeMarker.background"));
        painter.setPen(foreground);
        painter.drawText(box.adjusted(4, 0, -4, 0), Qt::AlignVCenter | Qt::AlignLeft,
            QDateTime::fromMSecsSinceEpoch(labels[index], QTimeZone::utc())
                .toString(QStringLiteral("HH:mm:ss'Z'")));
    }
    m_wfTimeMarkerAtlasDirty = false;
#ifdef AETHER_GPU_SPECTRUM
    // Row motion normally updates only vertex data. The position-dependent
    // label set can also change at visibility/crowding thresholds, requiring
    // an atlas upload, as do font, theme, and scale changes. None of these
    // updates rebuilds the full-screen static overlay.
    if (m_wfTimeMarkerTexture && m_wfTimeMarkerTexture->pixelSize() != pixels) {
        m_wfTimeMarkerTexture->setPixelSize(pixels);
        if (!m_wfTimeMarkerTexture->create()
            || (m_wfTimeMarkerSrb && !m_wfTimeMarkerSrb->create())) {
            releaseWaterfallTimeMarkersGpu();
        }
    }
#endif
}

void SpectrumWidget::drawWaterfallTimeMarkers(QPainter& painter, const QRect& rect)
{
    const QVector<WaterfallTimeMarker> markers = visibleWaterfallTimeMarkers(rect.height());
    if (markers.isEmpty()) {
        return;
    }
    prepareWaterfallTimeMarkerAtlas(markers);
    const qreal dpr = m_wfTimeMarkerAtlas.devicePixelRatio();
    const qreal labelWidth = m_wfTimeMarkerAtlas.width() / dpr;
    painter.save();
    painter.setClipRect(rect);
    for (const WaterfallTimeMarker& marker : markers) {
        const qreal y = rect.y() + marker.y;
        painter.drawImage(QRectF(rect.x(), y, rect.width(), 1), m_wfTimeMarkerAtlas,
                          QRectF(0, 0, m_wfTimeMarkerAtlas.width(), dpr));
        const int index = m_wfTimeMarkerLabels.indexOf(marker.timestampMs);
        if (index >= 0) {
            painter.drawImage(QRectF(rect.x() + 3, y + 2, labelWidth, m_wfTimeMarkerLabelHeight),
                m_wfTimeMarkerAtlas,
                QRectF(0, (2 + index * m_wfTimeMarkerLabelHeight) * dpr,
                       m_wfTimeMarkerAtlas.width(), m_wfTimeMarkerLabelHeight * dpr));
        }
    }
    painter.restore();
}

#ifdef AETHER_GPU_SPECTRUM
void SpectrumWidget::releaseWaterfallTimeMarkersGpu()
{
    delete m_wfTimeMarkerSrb;
    m_wfTimeMarkerSrb = nullptr;
    delete m_wfTimeMarkerTexture;
    m_wfTimeMarkerTexture = nullptr;
    delete m_wfTimeMarkerVbo;
    m_wfTimeMarkerVbo = nullptr;
    m_wfTimeMarkerQuadCount = 0;
}

void SpectrumWidget::prepareWaterfallTimeMarkersGpu(
    QRhiResourceUpdateBatch* batch, const QRect& rect, const QSize& logicalSize)
{
    m_wfTimeMarkerQuadCount = 0;
    const QVector<WaterfallTimeMarker> markers = visibleWaterfallTimeMarkers(rect.height());
    if (markers.isEmpty() || !m_ovPipeline || !m_ovSampler) {
        return;
    }
    const qint64 previousCacheKey = m_wfTimeMarkerAtlas.cacheKey();
    prepareWaterfallTimeMarkerAtlas(markers);
    bool upload = previousCacheKey != m_wfTimeMarkerAtlas.cacheKey();
    if (!m_wfTimeMarkerTexture) {
        m_wfTimeMarkerTexture = rhi()->newTexture(QRhiTexture::RGBA8, m_wfTimeMarkerAtlas.size());
        if (!m_wfTimeMarkerTexture->create()) {
            releaseWaterfallTimeMarkersGpu();
            return;
        }
        m_wfTimeMarkerSrb = rhi()->newShaderResourceBindings();
        m_wfTimeMarkerSrb->setBindings({QRhiShaderResourceBinding::sampledTexture(
            1, QRhiShaderResourceBinding::FragmentStage, m_wfTimeMarkerTexture, m_ovSampler)});
        if (!m_wfTimeMarkerSrb->create()) {
            releaseWaterfallTimeMarkersGpu();
            return;
        }
        upload = true;
    }
    if (upload) {
        batch->uploadTexture(m_wfTimeMarkerTexture, m_wfTimeMarkerAtlas);
    }
    QVector<float> vertices;
    const qreal dpr = m_wfTimeMarkerAtlas.devicePixelRatio();
    const qreal atlasWidth = m_wfTimeMarkerAtlas.width() / dpr;
    const qreal atlasHeight = m_wfTimeMarkerAtlas.height() / dpr;
    // Clip both geometry and UVs on the CPU. This reuses the existing overlay
    // pipeline without changing its scissor state for unrelated drawing.
    const auto addQuad = [&](const QRectF& destination, const QRectF& source) {
        const QRectF clipped = destination.intersected(QRectF(rect));
        if (clipped.isEmpty()) {
            return;
        }
        const qreal u0 = (source.x() + (clipped.left() - destination.left())
            / destination.width() * source.width()) / atlasWidth;
        const qreal u1 = (source.x() + (clipped.right() - destination.left())
            / destination.width() * source.width()) / atlasWidth;
        const qreal v0 = (source.y() + (clipped.top() - destination.top())
            / destination.height() * source.height()) / atlasHeight;
        const qreal v1 = (source.y() + (clipped.bottom() - destination.top())
            / destination.height() * source.height()) / atlasHeight;
        const float x0 = 2.0 * clipped.left() / logicalSize.width() - 1.0;
        const float x1 = 2.0 * clipped.right() / logicalSize.width() - 1.0;
        const float y0 = 1.0 - 2.0 * clipped.top() / logicalSize.height();
        const float y1 = 1.0 - 2.0 * clipped.bottom() / logicalSize.height();
        vertices << x0 << y1 << float(u0) << float(v1)
                 << x1 << y1 << float(u1) << float(v1)
                 << x0 << y0 << float(u0) << float(v0)
                 << x1 << y0 << float(u1) << float(v0);
        ++m_wfTimeMarkerQuadCount;
    };
    for (const WaterfallTimeMarker& marker : markers) {
        const qreal y = rect.y() + marker.y;
        addQuad(QRectF(rect.x(), y, rect.width(), 1), QRectF(0, 0.5, atlasWidth, 0));
        const int index = m_wfTimeMarkerLabels.indexOf(marker.timestampMs);
        if (index >= 0) {
            addQuad(QRectF(rect.x() + 3, y + 2, atlasWidth, m_wfTimeMarkerLabelHeight),
                    QRectF(0, 2 + index * m_wfTimeMarkerLabelHeight,
                           atlasWidth, m_wfTimeMarkerLabelHeight));
        }
    }
    if (vertices.isEmpty()) {
        return;
    }
    const int bytes = vertices.size() * int(sizeof(float));
    if (!m_wfTimeMarkerVbo) {
        m_wfTimeMarkerVbo = rhi()->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, bytes);
        if (!m_wfTimeMarkerVbo->create()) {
            releaseWaterfallTimeMarkersGpu();
            return;
        }
    } else if (m_wfTimeMarkerVbo->size() < static_cast<quint32>(bytes)) {
        m_wfTimeMarkerVbo->setSize(bytes);
        if (!m_wfTimeMarkerVbo->create()) {
            releaseWaterfallTimeMarkersGpu();
            return;
        }
    }
    batch->updateDynamicBuffer(m_wfTimeMarkerVbo, 0, bytes, vertices.constData());
}

void SpectrumWidget::drawWaterfallTimeMarkersGpu(QRhiCommandBuffer* cb)
{
    if (m_wfTimeMarkerQuadCount == 0 || !m_wfTimeMarkerSrb || !m_wfTimeMarkerVbo) {
        return;
    }
    cb->setGraphicsPipeline(m_ovPipeline);
    cb->setShaderResources(m_wfTimeMarkerSrb);
    const QSize output = renderTarget()->pixelSize();
    cb->setViewport({0, 0, float(output.width()), float(output.height())});
    const QRhiCommandBuffer::VertexInput binding(m_wfTimeMarkerVbo, 0);
    cb->setVertexInput(0, 1, &binding);
    for (int index = 0; index < m_wfTimeMarkerQuadCount; ++index) {
        cb->draw(4, 1, index * 4);
    }
}
#endif

} // namespace AetherSDR
