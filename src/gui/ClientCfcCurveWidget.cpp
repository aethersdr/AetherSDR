#include "ClientCfcCurveWidget.h"

#include "core/ThemeManager.h"

#include <QContextMenuEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace AetherSDR {

namespace {

constexpr double kMinAxisHz = 20.0;
constexpr double kMaxAxisHz = 12000.0;
constexpr double kAxisDb = 20.0;
constexpr double kHitRadius = 8.0;
constexpr double kPointRadius = 5.0;

QColor token(const QWidget* w, const char* name)
{
    return ThemeManager::instance().color(w, name);
}

}  // namespace

ClientCfcCurveWidget::ClientCfcCurveWidget(QWidget* parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setAccessibleName(tr("CFC compression and post-EQ curves"));
    m_bands = ClientCfc::defaultBands();
}

void ClientCfcCurveWidget::setBands(const std::vector<ClientCfc::Band>& bands)
{
    m_bands = bands;
    if (m_selected >= static_cast<int>(m_bands.size())) {
        setSelectedBand(static_cast<int>(m_bands.size()) - 1);
    }
    update();
}

void ClientCfcCurveWidget::setPostEqActive(bool active)
{
    m_postEqActive = active;
    update();
}

void ClientCfcCurveWidget::setCompression(const std::array<float, ClientCfc::kDisplayPoints>& db)
{
    m_compression = db;
    m_hasCompression = true;
    update();
}

void ClientCfcCurveWidget::clearCompression()
{
    m_hasCompression = false;
    update();
}

void ClientCfcCurveWidget::setSelectedBand(int index)
{
    if (index == m_selected) {
        return;
    }
    m_selected = index;
    update();
    emit selectedBandChanged(index);
}

QRectF ClientCfcCurveWidget::plotRect() const
{
    return QRectF(rect()).adjusted(34.0, 8.0, -8.0, -20.0);
}

double ClientCfcCurveWidget::freqToX(double hz) const
{
    const QRectF r = plotRect();
    const double t = std::log(std::clamp(hz, kMinAxisHz, kMaxAxisHz) / kMinAxisHz)
                     / std::log(kMaxAxisHz / kMinAxisHz);
    return r.left() + t * r.width();
}

double ClientCfcCurveWidget::xToFreq(double x) const
{
    const QRectF r = plotRect();
    const double t = std::clamp((x - r.left()) / r.width(), 0.0, 1.0);
    return kMinAxisHz * std::pow(kMaxAxisHz / kMinAxisHz, t);
}

double ClientCfcCurveWidget::dbToY(double db) const
{
    const QRectF r = plotRect();
    return r.center().y() - std::clamp(db, -kAxisDb, kAxisDb) / kAxisDb * (r.height() / 2.0);
}

double ClientCfcCurveWidget::yToDb(double y) const
{
    const QRectF r = plotRect();
    return std::clamp((r.center().y() - y) / (r.height() / 2.0) * kAxisDb, -kAxisDb, kAxisDb);
}

QPointF ClientCfcCurveWidget::pointFor(int band, Curve curve) const
{
    const ClientCfc::Band& b = m_bands[static_cast<std::size_t>(band)];
    return {freqToX(b.hz), dbToY(curve == Curve::Compression ? b.compDb : b.eqDb)};
}

int ClientCfcCurveWidget::hitTest(const QPointF& pos, Curve* curve) const
{
    double best = kHitRadius;
    int hit = -1;
    for (int i = 0; i < static_cast<int>(m_bands.size()); ++i) {
        for (Curve c : {Curve::Compression, Curve::PostEq}) {
            const QPointF d = pointFor(i, c) - pos;
            const double dist = std::hypot(d.x(), d.y());
            if (dist <= best) {
                best = dist;
                hit = i;
                *curve = c;
            }
        }
    }
    return hit;
}

double ClientCfcCurveWidget::interpolate(double hz, Curve curve) const
{
    const auto value = [curve](const ClientCfc::Band& b) {
        return static_cast<double>(curve == Curve::Compression ? b.compDb : b.eqDb);
    };
    if (m_bands.empty()) {
        return 0.0;
    }
    if (hz <= m_bands.front().hz) {
        return value(m_bands.front());
    }
    for (std::size_t i = 1; i < m_bands.size(); ++i) {
        if (hz <= m_bands[i].hz) {
            const double span = m_bands[i].hz - m_bands[i - 1].hz;
            const double t = span > 0.0 ? (hz - m_bands[i - 1].hz) / span : 0.0;
            return value(m_bands[i - 1]) + t * (value(m_bands[i]) - value(m_bands[i - 1]));
        }
    }
    return value(m_bands.back());
}

void ClientCfcCurveWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), token(this, "color.background.0"));
    const QRectF r = plotRect();

    // Grid: decades and the voice-band marks, and every 10 dB.
    p.setPen(QPen(token(this, "color.spectrum.grid"), 1.0));
    for (double hz : {50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0}) {
        p.drawLine(QPointF(freqToX(hz), r.top()), QPointF(freqToX(hz), r.bottom()));
    }
    for (double db : {-20.0, -10.0, 10.0, 20.0}) {
        p.drawLine(QPointF(r.left(), dbToY(db)), QPointF(r.right(), dbToY(db)));
    }
    p.setPen(QPen(token(this, "color.border.strong"), 1.0));
    p.drawLine(QPointF(r.left(), dbToY(0.0)), QPointF(r.right(), dbToY(0.0)));

    QFont small = font();
    small.setPointSizeF(std::max(6.5, small.pointSizeF() - 2.0));
    p.setFont(small);
    p.setPen(token(this, "color.text.label"));
    for (auto [hz, text] : std::initializer_list<std::pair<double, const char*>>{
             {50.0, "50"}, {100.0, "100"}, {200.0, "200"}, {500.0, "500"},
             {1000.0, "1k"}, {2000.0, "2k"}, {5000.0, "5k"}, {10000.0, "10k"}}) {
        p.drawText(QRectF(freqToX(hz) - 16.0, r.bottom() + 2.0, 32.0, 16.0),
                   Qt::AlignHCenter | Qt::AlignTop, QString::fromLatin1(text));
    }
    for (double db : {-20.0, 0.0, 20.0}) {
        p.drawText(QRectF(0.0, dbToY(db) - 8.0, r.left() - 4.0, 16.0),
                   Qt::AlignRight | Qt::AlignVCenter, QString::number(db, 'f', 0));
    }

    // Live compression, hanging below the 0 dB line.
    if (m_hasCompression) {
        QColor fill = token(this, "color.accent.danger");
        fill.setAlpha(90);
        QPainterPath area;
        area.moveTo(freqToX(kMinAxisHz), dbToY(0.0));
        for (int i = 0; i < ClientCfc::kDisplayPoints; ++i) {
            const double hz = 24000.0 * i / (ClientCfc::kDisplayPoints - 1);
            if (hz < kMinAxisHz || hz > kMaxAxisHz) {
                continue;
            }
            area.lineTo(freqToX(hz), dbToY(-m_compression[static_cast<std::size_t>(i)]));
        }
        area.lineTo(freqToX(kMaxAxisHz), dbToY(0.0));
        area.closeSubpath();
        p.fillPath(area, fill);
    }

    // The two curves, sampled on the log axis so cfcomp's linear-in-Hz
    // interpolation is drawn as it sounds.
    const auto drawCurve = [&](Curve curve, const QColor& color, bool dashed) {
        QPainterPath path;
        for (int px = 0; px <= static_cast<int>(r.width()); px += 2) {
            const double x = r.left() + px;
            const QPointF pt(x, dbToY(interpolate(xToFreq(x), curve)));
            if (px == 0) {
                path.moveTo(pt);
            } else {
                path.lineTo(pt);
            }
        }
        QPen pen(color, 2.0);
        if (dashed) {
            pen.setStyle(Qt::DashLine);
        }
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
    };
    const QColor compColor = token(this, "color.accent");
    QColor eqColor = token(this, "color.accent.warning");
    if (!m_postEqActive) {
        eqColor.setAlpha(110);
    }
    drawCurve(Curve::PostEq, eqColor, !m_postEqActive);
    drawCurve(Curve::Compression, compColor, false);

    for (int i = 0; i < static_cast<int>(m_bands.size()); ++i) {
        const bool selected = i == m_selected;
        const double radius = selected ? kPointRadius + 1.5 : kPointRadius;
        p.setPen(QPen(eqColor, selected ? 2.5 : 1.8));
        p.setBrush(token(this, "color.background.0"));
        p.drawEllipse(pointFor(i, Curve::PostEq), radius, radius);
        p.setPen(QPen(selected ? token(this, "color.text.primary") : compColor, selected ? 2.0 : 1.0));
        p.setBrush(compColor);
        p.drawEllipse(pointFor(i, Curve::Compression), radius, radius);
    }
}

void ClientCfcCurveWidget::mousePressEvent(QMouseEvent* ev)
{
    if (m_readOnly || ev->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(ev);
        return;
    }
    Curve curve = Curve::Compression;
    const int hit = hitTest(ev->position(), &curve);
    if (hit >= 0) {
        m_dragBand = hit;
        m_dragCurve = curve;
        setSelectedBand(hit);
        ev->accept();
    }
}

void ClientCfcCurveWidget::mouseMoveEvent(QMouseEvent* ev)
{
    if (m_readOnly || m_dragBand < 0) {
        Curve curve = Curve::Compression;
        setCursor(!m_readOnly && hitTest(ev->position(), &curve) >= 0 ? Qt::OpenHandCursor
                                                                       : Qt::ArrowCursor);
        return;
    }
    ClientCfc::Band& b = m_bands[static_cast<std::size_t>(m_dragBand)];
    // A band cannot pass its neighbours, so the order (and the index the
    // selection and the typed fields point at) holds during the drag.
    const double lo = m_dragBand > 0 ? m_bands[static_cast<std::size_t>(m_dragBand) - 1].hz + 1.0
                                     : ClientCfc::kMinHz;
    const double hi = m_dragBand + 1 < static_cast<int>(m_bands.size())
                          ? m_bands[static_cast<std::size_t>(m_dragBand) + 1].hz - 1.0
                          : ClientCfc::kMaxHz;
    b.hz = static_cast<float>(std::round(std::clamp(xToFreq(ev->position().x()), lo, hi)));
    const double db = std::round(yToDb(ev->position().y()) * 2.0) / 2.0;
    if (m_dragCurve == Curve::Compression) {
        b.compDb = static_cast<float>(std::clamp(db, 0.0, static_cast<double>(ClientCfc::kMaxCompDb)));
    } else {
        b.eqDb = static_cast<float>(db);
    }
    update();
    emit bandsEdited(m_bands);
}

void ClientCfcCurveWidget::mouseReleaseEvent(QMouseEvent* ev)
{
    m_dragBand = -1;
    QWidget::mouseReleaseEvent(ev);
}

void ClientCfcCurveWidget::mouseDoubleClickEvent(QMouseEvent* ev)
{
    Curve curve = Curve::Compression;
    if (m_readOnly || ev->button() != Qt::LeftButton
        || static_cast<int>(m_bands.size()) >= ClientCfc::kMaxBands
        || hitTest(ev->position(), &curve) >= 0 || !plotRect().contains(ev->position())) {
        QWidget::mouseDoubleClickEvent(ev);
        return;
    }
    const double hz = std::round(xToFreq(ev->position().x()));
    ClientCfc::Band band{static_cast<float>(hz),
                         static_cast<float>(interpolate(hz, Curve::Compression)),
                         static_cast<float>(interpolate(hz, Curve::PostEq))};
    auto at = std::upper_bound(m_bands.begin(), m_bands.end(), band.hz,
                               [](float f, const ClientCfc::Band& b) { return f < b.hz; });
    const int index = static_cast<int>(at - m_bands.begin());
    m_bands.insert(at, band);
    update();
    emit bandsEdited(m_bands);
    m_selected = -1;
    setSelectedBand(index);
}

void ClientCfcCurveWidget::contextMenuEvent(QContextMenuEvent* ev)
{
    Curve curve = Curve::Compression;
    const int hit = m_readOnly ? -1 : hitTest(ev->pos(), &curve);
    if (hit < 0) {
        QWidget::contextMenuEvent(ev);
        return;
    }
    QMenu menu(this);
    QAction* remove = menu.addAction(tr("Remove band"));
    remove->setEnabled(static_cast<int>(m_bands.size()) > ClientCfc::kMinBands);
    if (menu.exec(ev->globalPos()) == remove) {
        m_bands.erase(m_bands.begin() + hit);
        update();
        emit bandsEdited(m_bands);
        setSelectedBand(std::min(hit, static_cast<int>(m_bands.size()) - 1));
    }
}

}  // namespace AetherSDR
