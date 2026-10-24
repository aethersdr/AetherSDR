#include "gui/UpdateStage.h"

#include "core/ThemeManager.h"

#include <QEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QResizeEvent>
#include <QtMath>

#include <cmath>

namespace AetherSDR {

namespace {

constexpr int kSparkLapMs = 7000;     // the brand spark, as everywhere in the canon
constexpr qreal kBreatheMs = 4500.0;
constexpr qreal kDriftMs = 6000.0;
constexpr int kTraceLayers = 7;
constexpr int kTimelineBand = 44;     // bottom strip the timeline owns

QColor token(const QWidget* scope, const char* name)
{
    return ThemeManager::instance().color(scope, QString::fromLatin1(name));
}

// A modelled band: a noise floor plus signal peaks, for decoration only.
// Deterministic, so the stage looks the same every time it opens.
qreal traceHeight(qreal x, int layer, bool calm)
{
    struct Peak { qreal at; qreal height; qreal width; };
    static const Peak kBusy[] = {
        {0.17, 46, 4}, {0.27, 30, 3}, {0.38, 88, 3}, {0.45, 40, 5}, {0.55, 120, 2},
        {0.64, 58, 4}, {0.73, 34, 3}, {0.85, 72, 3}, {0.93, 28, 4},
    };
    static const Peak kQuiet[] = {{0.35, 26, 5}, {0.68, 18, 4}};
    const qreal noise = (std::sin(x * 0.37 + layer * 1.7) + std::sin(x * 0.113 + layer * 0.9) * 0.6
                         + std::sin(x * 0.71 + layer * 3.1) * 0.35) * 3.2;
    qreal y = 6.0 + noise;
    // Low enough that the tallest peak stays under the release name above it.
    const qreal amp = calm ? 0.40 : 0.72;
    const auto add = [&](const Peak* peaks, int n) {
        for (int i = 0; i < n; ++i) {
            const qreal d = (x - peaks[i].at * 520.0 - layer * 3.0) / peaks[i].width;
            y += peaks[i].height * amp * std::exp(-d * d) * (1.0 - layer * 0.09);
        }
    };
    if (calm) {
        add(kQuiet, 2);
    } else {
        add(kBusy, 9);
    }
    return y;
}

} // namespace

UpdateStage::UpdateStage(QWidget* parent)
    : SparkWidget(kSparkLapMs, parent)
{
    setMinimumHeight(kHeight);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setFocusPolicy(Qt::NoFocus);
    m_clock.start();
    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this] {
        rebuildLayers();
        update();
    });
}

QSize UpdateStage::sizeHint() const
{
    return {760, kHeight};
}

void UpdateStage::setMood(Mood mood)
{
    if (m_mood == mood) {
        return;
    }
    m_mood = mood;
    rebuildLayers();
    update();
}

void UpdateStage::setTimeline(const QString& installed, const QString& latest)
{
    m_installed = installed;
    m_latest = latest;
    update();
}

qreal UpdateStage::phase(qreal periodMs) const
{
    if (!isAnimating()) {
        return 0.25;   // a resting pose: glow at full, traces at the top of their drift
    }
    return std::fmod(static_cast<qreal>(m_clock.elapsed()), periodMs) / periodMs;
}

QPointF UpdateStage::coreCentre() const
{
    return {width() / 2.0, (height() - kTimelineBand) * 0.84};
}

void UpdateStage::resizeEvent(QResizeEvent* event)
{
    SparkWidget::resizeEvent(event);
    rebuildLayers();
}

void UpdateStage::changeEvent(QEvent* event)
{
    SparkWidget::changeEvent(event);
    if (event->type() == QEvent::DevicePixelRatioChange) {
        rebuildLayers();
    }
}

void UpdateStage::rebuildLayers()
{
    if (width() <= 0 || height() <= 0) {
        return;
    }
    const qreal dpr = devicePixelRatioF();
    const bool calm = m_mood == Mood::Calm;
    const QPointF c = coreCentre();

    m_rings = QPixmap(size() * dpr);
    m_rings.setDevicePixelRatio(dpr);
    m_rings.fill(Qt::transparent);
    {
        QPainter p(&m_rings);
        p.setRenderHint(QPainter::Antialiasing);
        QColor ring = token(this, "color.canon.cyan");
        for (int i = 0; i < 6; ++i) {
            const qreal rx = width() * (0.17 + i * 0.066);
            ring.setAlphaF(0.16 - i * 0.02);
            QPen pen(ring, 1.0);
            if (i % 2) {
                pen.setDashPattern({2.0, 6.0});
            }
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(c, rx, rx * 0.28);
        }
    }

    // Seven layers, back to front: further layers sit higher, to the right,
    // smaller and fainter, which reads as a waterfall seen in depth.
    m_traces = QPixmap(size() * dpr);
    m_traces.setDevicePixelRatio(dpr);
    m_traces.fill(Qt::transparent);
    {
        QPainter p(&m_traces);
        p.setRenderHint(QPainter::Antialiasing);
        const qreal span = width() * 0.68;
        const qreal left = (width() - span) / 2.0;
        const QColor deep = token(this, "color.canon.category.blue");
        const QColor tint = token(this, calm ? "color.canon.aqua" : "color.canon.cyan");
        for (int k = kTraceLayers - 1; k >= 0; --k) {
            const qreal scale = 1.0 - k * 0.07;
            const qreal shiftX = k * 9.0;
            const qreal base = c.y() - k * 10.0;
            QPainterPath path;
            for (qreal x = 0; x <= 520.0; x += 4.0) {
                const qreal px = c.x() + (left + x / 520.0 * span - c.x()) * scale + shiftX;
                const qreal py = base - traceHeight(x, k, calm) * scale;
                if (x == 0) {
                    path.moveTo(px, py);
                } else {
                    path.lineTo(px, py);
                }
            }
            if (k == 0) {
                QColor glow = tint;
                glow.setAlphaF(0.45);
                p.setPen(QPen(glow, 5.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                p.drawPath(path);
                const QBrush brand = ThemeManager::instance().brush(
                    QStringLiteral("color.brand.gradient"), path.boundingRect().toRect());
                p.setPen(QPen(calm ? QBrush(tint) : brand, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                p.drawPath(path);
            } else {
                QColor line = k > 3 ? deep : tint;
                line.setAlphaF(0.12 + (kTraceLayers - 1 - k) * 0.09);
                p.setPen(QPen(line, 1.0));
                p.drawPath(path);
            }
        }
    }
}

void UpdateStage::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QPointF c = coreCentre();
    const bool calm = m_mood == Mood::Calm;

    p.drawPixmap(0, 0, m_rings);

    // The core glow breathes between 55 % and 100 %.
    const qreal breathe = 0.775 + 0.225 * std::sin(2.0 * M_PI * phase(kBreatheMs));
    QColor glow = token(this, calm ? "color.canon.aqua" : "color.canon.category.blue");
    QRadialGradient g(c, width() * 0.40);
    glow.setAlphaF(0.50 * breathe);
    g.setColorAt(0.0, glow);
    glow.setAlphaF(0.09 * breathe);
    g.setColorAt(0.6, glow);
    glow.setAlphaF(0.0);
    g.setColorAt(1.0, glow);
    p.save();
    p.translate(c);
    p.scale(1.0, 0.32);
    p.translate(-c);
    p.setPen(Qt::NoPen);
    p.setBrush(g);
    p.drawEllipse(c, width() * 0.40, width() * 0.40);
    p.restore();

    // The traces drift 3 px up and down.
    const qreal drift = -3.0 * std::sin(2.0 * M_PI * phase(kDriftMs));
    p.drawPixmap(QPointF(0, drift), m_traces);

    // The spark circles the outermost solid orbit.
    const qreal rx = width() * 0.434;
    QPainterPath orbit;
    orbit.addEllipse(c, rx, rx * 0.28);
    paintSpark(p, orbit, c, token(this, "color.canon.cyan"), token(this, "color.canon.sparkHot"),
               6.0, 0.35, 1.6);

    // Timeline: the installed release, and the newest one when it differs.
    const qreal y = height() - kTimelineBand / 2.0 - 6.0;
    const qreal x0 = 40.0;
    const qreal x1 = width() - 40.0;
    QFont f = font();
    f.setPointSizeF(f.pointSizeF() * 0.9);
    p.setFont(f);
    const QColor muted = token(this, "color.canon.muted");
    const QColor inkSoft = token(this, "color.canon.inkSoft");
    if (!m_latest.isEmpty()) {
        QLinearGradient run(QPointF(x0, y), QPointF(x1, y));
        run.setColorAt(0.0, token(this, "color.canon.category.blue"));
        run.setColorAt(1.0, token(this, "color.canon.aqua"));
        p.setPen(QPen(QBrush(run), 2.0));
        p.drawLine(QPointF(x0, y), QPointF(x1, y));
        QColor halo = token(this, "color.canon.aqua");
        halo.setAlphaF(0.35);
        p.setPen(Qt::NoPen);
        p.setBrush(halo);
        p.drawEllipse(QPointF(x1, y), 11.0, 11.0);
        p.setBrush(token(this, "color.canon.aqua"));
        p.drawEllipse(QPointF(x1, y), 6.5, 6.5);
        p.setPen(token(this, "color.canon.aqua"));
        p.drawText(QRectF(x1 - 120.0, y + 9.0, 120.0 + 18.0, 18.0), Qt::AlignRight | Qt::AlignTop,
                   QStringLiteral("v%1 · new").arg(m_latest));
    } else {
        QColor line = muted;
        line.setAlphaF(0.35);
        p.setPen(QPen(line, 1.0));
        p.drawLine(QPointF(x0, y), QPointF(x1, y));
    }
    if (!m_installed.isEmpty()) {
        p.setPen(QPen(inkSoft, 2.0));
        p.setBrush(token(this, "color.canon.ground"));
        p.drawEllipse(QPointF(x0, y), 5.5, 5.5);
        p.setPen(m_latest.isEmpty() ? inkSoft : muted);
        p.drawText(QRectF(x0 - 18.0, y + 9.0, 200.0, 18.0), Qt::AlignLeft | Qt::AlignTop,
                   QStringLiteral("v%1 · installed").arg(m_installed));
    }
}

} // namespace AetherSDR
