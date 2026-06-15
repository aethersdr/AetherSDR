#include "WebSdrWaterfallView.h"

#include "core/ThemeManager.h"

#include <QAccessible>
#include <QAccessibleWidget>
#include <QPainter>
#include <QMouseEvent>
#include <QFont>

#include <cmath>
#include <cstring>

namespace AetherSDR {

// Accessibility: the widget overrides paintEvent, so a11y rules require a
// QAccessibleInterface. Expose it as a chart; the name/description come from
// the widget's accessibleName/accessibleDescription via QAccessibleWidget.
namespace {

class WaterfallAccessible : public QAccessibleWidget {
public:
    explicit WaterfallAccessible(QWidget* w)
        : QAccessibleWidget(w, QAccessible::Chart) {}
};

QAccessibleInterface* waterfallFactory(const QString& key, QObject* obj)
{
    if (key == QLatin1String("AetherSDR::WebSdrWaterfallView")) {
        return new WaterfallAccessible(qobject_cast<QWidget*>(obj));
    }
    return nullptr;
}

double niceStep(double range)
{
    if (range <= 0) {
        return 1.0;
    }
    const double raw = range / 6.0;
    const double mag = std::pow(10.0, std::floor(std::log10(raw)));
    const double n = raw / mag;
    const double step = (n < 1.5) ? 1.0 : (n < 3.5) ? 2.0 : (n < 7.5) ? 5.0 : 10.0;
    return step * mag;
}

} // namespace

WebSdrWaterfallView::WebSdrWaterfallView(QWidget* parent) : QWidget(parent)
{
    setMinimumHeight(160);
    setAccessibleName(tr("WebSDR waterfall"));
    setAccessibleDescription(tr("Live spectrum waterfall; click to tune"));

    static bool s_factoryInstalled = false;
    if (!s_factoryInstalled) {
        s_factoryInstalled = true;
        QAccessible::installFactory(waterfallFactory);
    }

    m_img = QImage(1024, 280, QImage::Format_RGB32);
    m_img.fill(Qt::black);
    m_cropR = m_img.width();
}

void WebSdrWaterfallView::setGeometryInfo(double centerKHz, double srKHz)
{
    if (centerKHz != m_centerKHz || srKHz != m_srKHz) {
        m_img.fill(Qt::black);          // clear stale data on band change
        m_colAvg.clear();               // re-learn the guard edges
        m_cropL = 0;
        m_cropR = m_img.width();
    }
    m_centerKHz = centerKHz;
    m_srKHz = srKHz;
    update();
}

void WebSdrWaterfallView::setTuning(double freqKHz, double loKHz, double hiKHz)
{
    m_tunedKHz = freqKHz;
    m_loKHz = loKHz;
    m_hiKHz = hiKHz;
    m_haveTuning = true;
    update();
}

void WebSdrWaterfallView::addRow(const QImage& row)
{
    QImage r = row.convertToFormat(QImage::Format_RGB32);
    if (m_img.width() != r.width()) {
        m_img = QImage(qMax(1, r.width()), m_img.height(), QImage::Format_RGB32);
        m_img.fill(Qt::black);
        m_colAvg.clear();
        m_cropL = 0;
        m_cropR = m_img.width();
    }
    const int w = m_img.width();
    const int h = m_img.height();
    const int bpl = m_img.bytesPerLine();
    uchar* base = m_img.bits();
    std::memmove(base + bpl, base, static_cast<size_t>(bpl) * (h - 1));
    std::memcpy(base, r.constBits(), static_cast<size_t>(bpl));

    // Track per-column activity (EMA of luminance) to locate the live region.
    if (static_cast<int>(m_colAvg.size()) != w) {
        m_colAvg.assign(w, 0.0);
    }
    const QRgb* px = reinterpret_cast<const QRgb*>(r.constBits());
    for (int c = 0; c < w; ++c) {
        const double lum = qGray(px[c]);
        m_colAvg[c] = 0.92 * m_colAvg[c] + 0.08 * lum;
    }
    if (++m_rowsSinceCrop >= 16) {
        m_rowsSinceCrop = 0;
        recomputeCrop();
    }
    update();
}

void WebSdrWaterfallView::recomputeCrop()
{
    const int w = m_img.width();
    if (static_cast<int>(m_colAvg.size()) != w) {
        m_cropL = 0;
        m_cropR = w;
        return;
    }
    const double thresh = 3.0;        // dead (index 2) ~0.6, live (index 16+) ~5+
    int l = 0;
    while (l < w && m_colAvg[l] < thresh) {
        ++l;
    }
    int rgt = w;
    while (rgt > l && m_colAvg[rgt - 1] < thresh) {
        --rgt;
    }
    if (rgt - l < w / 8) {   // sanity: too little live, keep the full view
        m_cropL = 0;
        m_cropR = w;
    } else {
        m_cropL = l;
        m_cropR = rgt;
    }
}

double WebSdrWaterfallView::freqAtCol(int c) const
{
    const int w = qMax(1, m_img.width());
    return m_centerKHz - m_srKHz / 2.0 + (static_cast<double>(c) / w) * m_srKHz;
}

double WebSdrWaterfallView::freqLeft()  const { return freqAtCol(m_cropL); }
double WebSdrWaterfallView::freqRight() const { return freqAtCol(m_cropR); }

double WebSdrWaterfallView::xOf(double freqKHz) const
{
    const double fl = freqLeft();
    const double fr = freqRight();
    return (fr > fl) ? (freqKHz - fl) / (fr - fl) * width() : 0.0;
}

void WebSdrWaterfallView::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    const int scaleH = 16;
    const int wfTop = scaleH;
    const int wfH = qMax(1, height() - scaleH);
    const int cropW = qMax(1, m_cropR - m_cropL);
    p.drawImage(QRect(0, wfTop, width(), wfH), m_img,
                QRect(m_cropL, 0, cropW, m_img.height()));

    if (m_srKHz <= 0.0) {
        return;
    }
    auto& theme = ThemeManager::instance();
    if (m_haveTuning) {
        const double xl = xOf(m_tunedKHz + m_loKHz);
        const double xh = xOf(m_tunedKHz + m_hiKHz);
        p.fillRect(QRectF(xl, wfTop, qMax(1.0, xh - xl), wfH),
                   theme::withAlpha("color.text.primary", 50));
        const double xc = xOf(m_tunedKHz);
        p.setPen(QPen(theme.color("color.accent.danger"), 1));
        p.drawLine(QPointF(xc, wfTop), QPointF(xc, height()));
    }
    // frequency scale strip on top (panadapter-style), over the live span
    p.fillRect(QRect(0, 0, width(), scaleH), theme::withAlpha("color.background.0", 180));
    p.setPen(theme.color("color.text.secondary"));
    QFont f = p.font();
    f.setPixelSize(9);   // scale annotations: 9px (style guide §Typography)
    p.setFont(f);
    const double lo = freqLeft();
    const double hi = freqRight();
    const double step = niceStep(hi - lo);
    const double first = std::ceil(lo / step) * step;
    for (double fk = first; fk <= hi; fk += step) {
        const double x = xOf(fk);
        p.drawText(QRectF(x - 28, 0, 56, scaleH - 3),
                   Qt::AlignHCenter | Qt::AlignBottom,
                   QString::number(fk / 1000.0, 'f', 3));
        p.drawLine(QPointF(x, scaleH - 3), QPointF(x, scaleH));
    }
}

void WebSdrWaterfallView::mousePressEvent(QMouseEvent* e)
{
    if (m_onClick && width() > 0 && m_srKHz > 0.0) {
        const double fl = freqLeft();
        const double fr = freqRight();
        m_onClick(fl + (static_cast<double>(e->position().x()) / width()) * (fr - fl));
    }
}

} // namespace AetherSDR
