#pragma once

#include "models/AetherPDMeasurementModel.h"

#include <QWidget>
#include <QRectF>
#include <optional>

class QPainter;

namespace AetherSDR {
// Static display of a released measurement. No capture or radio control lives here.
class AetherPDMeasurementSpectrum final : public QWidget {
    Q_OBJECT
public:
    explicit AetherPDMeasurementSpectrum(QWidget* parent = nullptr);
    void setResult(const AetherPDMeasurementModel::Result* result);
    QByteArray exportPng();
    bool hasResult() const { return m_result.has_value(); }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    double x(double hz, const QRectF& area) const;
    double y(double db, const QRectF& area) const;
    void drawSpectrum(QPainter& painter, const QRectF& area) const;
    std::optional<txlin::LinearityResult> m_result;
    QString m_caption;
    double m_centerHz{0};
    double m_spanHz{1};
    double m_topDb{0};
    double m_bottomDb{-100};
};
} // namespace AetherSDR
