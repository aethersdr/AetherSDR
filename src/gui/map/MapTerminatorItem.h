#pragma once

#include <QGeoView/QGVDrawItem.h>

#include <QDateTime>
#include <QFutureWatcher>
#include <QImage>

#include <atomic>
#include <memory>

namespace AetherSDR {

class MapBatchItemTestAccess;

class MapTerminatorItem : public QGVDrawItem {
    Q_OBJECT

public:
    MapTerminatorItem();
    ~MapTerminatorItem() override;
    void setDateTime(const QDateTime& dateTime);

private:
    friend class MapBatchItemTestAccess;
    void onProjection(QGVMap* geoMap) override;
    QPainterPath projShape() const override;
    void projPaint(QPainter* painter) override;
    void rebuildImage();
    static QImage renderImage(
        QRectF worldRect, QDateTime dateTime, QColor night,
        std::shared_ptr<std::atomic_bool> cancelled);

    QDateTime m_dateTime{QDateTime::currentDateTimeUtc()};
    QRectF m_worldRect;
    QImage m_image;
    quint64 m_renderGeneration{0};
    std::shared_ptr<std::atomic_bool> m_renderCancelled;
};

} // namespace AetherSDR
