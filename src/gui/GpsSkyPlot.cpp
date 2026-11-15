#include "GpsSkyPlot.h"

#include "core/ThemeManager.h"

#include <QAccessible>
#include <QAccessibleWidget>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace AetherSDR {

namespace {

// The plot is painted, so a screen reader gets its content as the Value:
// the counts, then each satellite in the fix (docs/a11y.md, custom-painted
// widgets). The table beside it carries every row.
class GpsSkyPlotAccessible final : public QAccessibleWidget {
public:
    explicit GpsSkyPlotAccessible(GpsSkyPlot* plot) : QAccessibleWidget(plot, QAccessible::Graphic) {}
    QString text(QAccessible::Text role) const override
    {
        if (role == QAccessible::Value) {
            return widget()->accessibleDescription();
        }
        return QAccessibleWidget::text(role);
    }
};

QAccessibleInterface* skyPlotAccessibility(const QString&, QObject* object)
{
    if (auto* plot = qobject_cast<GpsSkyPlot*>(object)) {
        return new GpsSkyPlotAccessible(plot);
    }
    return nullptr;
}

}  // namespace

GpsSkyPlot::GpsSkyPlot(QWidget* parent)
    : QWidget(parent)
{
    static const bool installed = [] {
        QAccessible::installFactory(skyPlotAccessibility);
        return true;
    }();
    Q_UNUSED(installed);
    setObjectName(QStringLiteral("gpsSkyPlot"));
    setAccessibleName(tr("Satellite sky plot"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this,
            qOverload<>(&QWidget::update));
}

void GpsSkyPlot::setSatellites(const QList<GpsSatellite>& satellites)
{
    m_satellites = satellites;
    int heard = 0, used = 0;
    QStringList inFix;
    for (const GpsSatellite& s : satellites) {
        heard += s.heard();
        used += s.used;
        if (s.used) {
            inFix << tr("%1 %2").arg(gpsdsky::constellationName(gpsdsky::constellation(s))).arg(s.prn);
        }
    }
    QString description = tr("%n satellite(s) listed, %1 heard, %2 used in the fix.", "",
                             int(satellites.size()))
                              .arg(heard).arg(used);
    if (!inFix.isEmpty()) {
        description += QLatin1Char(' ') + tr("In the fix: %1.").arg(inFix.join(QStringLiteral(", ")));
    }
    if (description != accessibleDescription()) {
        setAccessibleDescription(description);
        QAccessibleValueChangeEvent event(this, description);
        QAccessible::updateAccessibility(&event);
    }
    update();
}

QPointF GpsSkyPlot::project(const QRectF& plot, double elevationDeg, double azimuthDeg)
{
    const double r = plot.width() / 2.0 * (90.0 - qBound(0.0, elevationDeg, 90.0)) / 90.0;
    const double a = azimuthDeg * M_PI / 180.0;
    return {plot.center().x() + r * std::sin(a), plot.center().y() - r * std::cos(a)};
}

QString GpsSkyPlot::colorToken(gpsdsky::Constellation c)
{
    using C = gpsdsky::Constellation;
    switch (c) {
    case C::Gps: return QStringLiteral("color.canon.category.cyan");
    case C::Glonass: return QStringLiteral("color.canon.category.rose");
    case C::Galileo: return QStringLiteral("color.canon.category.violet");
    case C::BeiDou: return QStringLiteral("color.canon.category.amber");
    case C::Qzss: return QStringLiteral("color.canon.category.teal");
    case C::Sbas: return QStringLiteral("color.canon.category.blue");
    case C::NavIc:
    case C::Imes:
    case C::Unknown: break;
    }
    return QStringLiteral("color.canon.category.aqua");
}

void GpsSkyPlot::paintEvent(QPaintEvent*)
{
    auto& tm = ThemeManager::instance();
    const QColor line = tm.color(this, QStringLiteral("color.canon.line"));
    const QColor muted = tm.color(this, QStringLiteral("color.canon.muted"));
    const QColor face = tm.color(this, QStringLiteral("color.canon.nested"));
    const QColor ink = tm.color(this, QStringLiteral("color.canon.ink"));

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QFontMetricsF fm(font());
    const double margin = fm.height() + 4.0;   // room for N/E/S/W outside the rim
    const double side = qMin(width(), height()) - 2.0 * margin;
    if (side < 40.0) {
        return;
    }
    const QRectF plot((width() - side) / 2.0, (height() - side) / 2.0, side, side);
    const QPointF c = plot.center();

    p.setPen(QPen(line, 1.0));
    p.setBrush(face);
    p.drawEllipse(plot);
    p.setBrush(Qt::NoBrush);
    for (double el : {30.0, 60.0}) {
        const double r = side / 2.0 * (90.0 - el) / 90.0;
        p.drawEllipse(c, r, r);
    }
    p.drawLine(QPointF(plot.left(), c.y()), QPointF(plot.right(), c.y()));
    p.drawLine(QPointF(c.x(), plot.top()), QPointF(c.x(), plot.bottom()));

    p.setPen(muted);
    const struct { const char* text; double az; } compass[] = {
        {QT_TR_NOOP("N"), 0}, {QT_TR_NOOP("E"), 90}, {QT_TR_NOOP("S"), 180}, {QT_TR_NOOP("W"), 270}};
    for (const auto& d : compass) {
        const QString t = tr(d.text);
        const double a = d.az * M_PI / 180.0, r = side / 2.0 + margin / 2.0;
        const QPointF at(c.x() + r * std::sin(a), c.y() - r * std::cos(a));
        const QSizeF sz(fm.horizontalAdvance(t), fm.height());
        p.drawText(QRectF(at - QPointF(sz.width() / 2.0, sz.height() / 2.0), sz), Qt::AlignCenter, t);
    }

    // Unheard first, then heard, then used, so the satellites in the fix sit on top.
    QList<const GpsSatellite*> order;
    for (const GpsSatellite& s : m_satellites) {
        if (s.hasSkyPosition()) {
            order.append(&s);
        }
    }
    std::stable_sort(order.begin(), order.end(), [](const GpsSatellite* a, const GpsSatellite* b) {
        return (a->used * 2 + a->heard()) < (b->used * 2 + b->heard());
    });
    const double dot = qBound(4.0, side / 45.0, 9.0);
    QFont small = font();
    small.setPointSizeF(qMax(6.0, small.pointSizeF() * 0.8));
    p.setFont(small);
    for (const GpsSatellite* s : order) {
        QColor color = tm.color(this, colorToken(gpsdsky::constellation(*s)));
        const QPointF at = project(plot, s->elevationDeg, s->azimuthDeg);
        if (!s->heard()) {
            color.setAlphaF(0.35);
        }
        p.setPen(QPen(color, s->used ? 1.0 : 2.0));
        p.setBrush(s->used ? QBrush(color) : Qt::NoBrush);
        p.drawEllipse(at, dot, dot);
        if (s->heard()) {
            p.setPen(ink);
            p.drawText(at + QPointF(dot + 2.0, dot / 2.0), QString::number(s->prn));
        }
    }
}

}  // namespace AetherSDR
