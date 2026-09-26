#include "AccessoryPanelWidgets.h"

#include "core/ThemeManager.h"

#include <QAccessible>
#include <QFocusEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>
#include <QtMath>

#include <cmath>

namespace AetherSDR {

namespace {

// Relay banks are 8-bit ladders: 0–255 inclusive.
constexpr int kRelayMax = 255;

// Conventional meter sweep: 0 at lower-left, full scale at lower-right,
// increasing clockwise. Qt measures painter angles counter-clockwise from
// 3 o'clock, so the start angle is the larger of the two.
constexpr double kSweepStartDeg = 225.0;
constexpr double kSweepSpanDeg  = 270.0;

double needleAngleDeg(int value)
{
    const double frac = std::clamp(static_cast<double>(value) / kRelayMax, 0.0, 1.0);
    return kSweepStartDeg - frac * kSweepSpanDeg;
}

}  // namespace

// ── Panel scaling ───────────────────────────────────────────────────────────

qreal panelContentScale(const QSize& available, qreal designWidth, qreal naturalHeight)
{
    if (available.width() <= 0 || available.height() <= 0) return 1.0;
    if (designWidth <= 1.0 || naturalHeight <= 1.0) return 1.0;
    return qBound(kPanelMinScale,
                  qMin(available.width() / designWidth,
                       (available.height() - kPanelBottomGap) / naturalHeight),
                  kPanelMaxScale);
}

QSize panelMinimumSize(qreal designWidth, qreal naturalHeight)
{
    return QSize(qRound(designWidth * kPanelMinScale),
                 qRound(naturalHeight * kPanelMinScale) + kPanelBottomGap);
}

QSize panelKeySize(int heightPx, int seedWidthPx, qreal scale)
{
    const int h = qMax(1, heightPx);
    const int w = qMax(qRound(h * kPanelKeyAspect), qRound(seedWidthPx * scale));
    return QSize(w, h);
}

// ── RelayDial ───────────────────────────────────────────────────────────────

RelayDial::RelayDial(const QString& label, QWidget* parent)
    : QWidget(parent)
    , m_label(label)
{

    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    setFocusPolicy(Qt::TabFocus);
    setToolTip(tr("Scroll or use Up/Down keys to adjust relay position"));
    setAccessibleName(label);
    refreshAccessibleValue();

    // Same debounce as RelayBar: a TGXL autotune sweep pushes relay positions
    // far faster than a screen reader can speak them, and an unthrottled
    // updateAccessibility() per step is the storm docs/a11y.md warns about.
    m_accessibilityTimer.setSingleShot(true);
    m_accessibilityTimer.setInterval(kAccessibilityAnnouncementIntervalMs);
    connect(&m_accessibilityTimer, &QTimer::timeout, this, [this]() {
        if (!hasFocus() || !QAccessible::isActive()) return;
        if (m_value == m_lastAccessibleValue) return;
        m_lastAccessibleValue = m_value;
        QAccessibleValueChangeEvent event(this, QVariant(m_value));
        QAccessible::updateAccessibility(&event);
    });
}

void RelayDial::setValue(int v)
{
    if (m_value == v) return;
    m_value = v;
    refreshAccessibleValue();
    update();
    if (hasFocus() && QAccessible::isActive() && !m_accessibilityTimer.isActive()) {
        m_accessibilityTimer.start();
    }
}

QSize RelayDial::sizeHint() const
{
    // Big enough for the needle sweep plus the name and the reading; the row
    // it sits in stays compact because this is what it asks for.
    return QSize(m_diameter, m_diameter);
}

void RelayDial::setPreferredDiameter(int px)
{
    const int clamped = qBound(36, px, 260);
    if (m_diameter == clamped) return;
    m_diameter = clamped;
    updateGeometry();
    update();
}

QSize RelayDial::minimumSizeHint() const
{
    // Fixed, not a fraction of the current diameter — see PanelKey's note.
    // A minimum that follows the current size makes the panel's own minimum
    // ratchet up every time it is enlarged.
    return QSize(28, 28);
}

void RelayDial::setScrollEnabled(bool on)
{
    m_scrollEnabled = on;
    setCursor(on ? Qt::SizeVerCursor : Qt::ArrowCursor);
}

void RelayDial::refreshAccessibleValue()
{
    // The needle position is the whole reading, and a dial has no text a
    // reader could fall back on — so the value goes in the description.
    setAccessibleDescription(tr("%1 of %2").arg(m_value).arg(kRelayMax));
}

void RelayDial::focusOutEvent(QFocusEvent* e)
{
    // Positions move on hardware pushes regardless of focus, so a value that
    // wandered away and back while unfocused must not be swallowed by the
    // dedup on the next focused change (RelayBar carries the same guard).
    m_accessibilityTimer.stop();
    m_lastAccessibleValue = std::numeric_limits<int>::min();
    QWidget::focusOutEvent(e);
}

void RelayDial::keyPressEvent(QKeyEvent* e)
{
    if (!m_scrollEnabled) { QWidget::keyPressEvent(e); return; }
    if (e->key() == Qt::Key_Up || e->key() == Qt::Key_Plus || e->key() == Qt::Key_Right) {
        emit relayAdjusted(+1);
        e->accept();
    } else if (e->key() == Qt::Key_Down || e->key() == Qt::Key_Minus || e->key() == Qt::Key_Left) {
        emit relayAdjusted(-1);
        e->accept();
    } else {
        QWidget::keyPressEvent(e);
    }
}

void RelayDial::wheelEvent(QWheelEvent* e)
{
    if (!m_scrollEnabled) { QWidget::wheelEvent(e); return; }
    // Clamp to ±1 per event: KDE/Cinnamon send 960 per notch (#504).
    m_angleAccum += e->angleDelta().y();
    constexpr int step = 120;
    int emitted = 0;
    while (m_angleAccum >= step && emitted == 0)  { m_angleAccum -= step; emit relayAdjusted(+1); ++emitted; }
    while (m_angleAccum <= -step && emitted == 0) { m_angleAccum += step; emit relayAdjusted(-1); ++emitted; }
    if (emitted) m_angleAccum = 0;  // discard leftover inflation
    e->accept();
}

void RelayDial::paintEvent(QPaintEvent*)
{
    auto& theme = AetherSDR::ThemeManager::instance();
    const QColor face   = theme.color(this, QStringLiteral("color.accessory.dial.face"));
    const QColor needle = theme.color(this, QStringLiteral("color.accessory.dial.needle"));
    const QColor rim    = theme.color(this, QStringLiteral("color.accessory.dial.rim"));
    const QColor text   = theme.color(this, QStringLiteral("color.text.primary"));
    const QColor label  = theme.color(this, QStringLiteral("color.text.secondary"));

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    // Square the dial inside the widget so a stretched cell keeps a circle.
    const int side = qMin(width(), height());
    const QRectF disc(QRectF(0, 0, side, side)
                          .translated((width() - side) / 2.0, (height() - side) / 2.0)
                          .adjusted(2, 2, -2, -2));
    const QPointF centre = disc.center();
    const double radius = disc.width() / 2.0;

    p.setBrush(face);
    p.setPen(QPen(rim, 1.5));
    p.drawEllipse(disc);

    // Focus ring — the dial is keyboard-adjustable, so focus has to be visible.
    if (hasFocus()) {
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(theme.color(this, QStringLiteral("color.accent")), 2.0));
        p.drawEllipse(disc.adjusted(-1.5, -1.5, 1.5, 1.5));
    }

    // Needle.
    const double angle = qDegreesToRadians(needleAngleDeg(m_value));
    const QPointF tip(centre.x() + std::cos(angle) * radius * 0.78,
                      centre.y() - std::sin(angle) * radius * 0.78);
    p.setPen(QPen(needle, qMax(1.5, radius * 0.06), Qt::SolidLine, Qt::RoundCap));
    p.drawLine(centre, tip);

    // Bank name above the hub, reading value below it. The hardware panel
    // labels neither, but an unlabelled dial cannot be read at a glance and
    // gives a screen-reader user nothing to anchor on.
    QFont f = font();
    f.setPixelSize(qMax(8, static_cast<int>(radius * 0.32)));
    f.setBold(true);
    p.setFont(f);
    const QFontMetrics fm(f);

    p.setPen(label);
    p.drawText(QRectF(disc.left(), centre.y() - radius * 0.72,
                      disc.width(), fm.height()),
               Qt::AlignHCenter | Qt::AlignVCenter, m_label);

    p.setPen(text);
    p.drawText(QRectF(disc.left(), centre.y() + radius * 0.34,
                      disc.width(), fm.height()),
               Qt::AlignHCenter | Qt::AlignVCenter, QString::number(m_value));
}

// ── PanelKey ────────────────────────────────────────────────────────────────

PanelKey::PanelKey(const QString& text, QWidget* parent)
    : QPushButton(text, parent)
{
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
}

void PanelKey::setTargetSize(const QSize& size)
{
    if (m_target == size) return;
    m_target = size;
    updateGeometry();
}

QSize PanelKey::minimumSizeHint() const
{
    // Deliberately not derived from the target, and deliberately smaller than
    // any caption needs: this figure becomes the panel's own minimum width,
    // and anything that tracks the current size makes that minimum ratchet
    // upward. A key squeezed this far is illegible, but the panel reaching
    // that size at all means the scale has already bottomed out.
    return QSize(24, 14);
}

// ── AccessoryPortRow ─────────────────────────────────────────────────────────────

AccessoryPortRow::AccessoryPortRow(const QString& portLetter, QWidget* parent)
    : QWidget(parent)
    , m_portLetter(portLetter)
{
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(6, 3, 6, 3);
    row->setSpacing(6);

    m_portLabel = new QLabel(portLetter, this);
    m_portLabel->setAlignment(Qt::AlignCenter);
    m_portLabel->setFixedWidth(18);

    m_pttLabel = new QLabel(tr("PTT"), this);
    m_pttLabel->setAlignment(Qt::AlignCenter);
    m_pttLabel->setFixedWidth(34);

    m_bandLabel = new QLabel(tr("N/A"), this);
    m_bandLabel->setTextFormat(Qt::PlainText);
    m_bandLabel->setAlignment(Qt::AlignCenter);
    m_bandLabel->setMinimumWidth(34);

    m_biasLabel = new QLabel(this);
    m_biasLabel->setTextFormat(Qt::PlainText);
    m_biasLabel->setAlignment(Qt::AlignCenter);
    m_biasLabel->setVisible(false);      // amplifier-only; see setBiasText

    m_sourceLabel = new QLabel(QStringLiteral("—"), this);
    // flexA/flexB come off the wire verbatim — see the note on the alert
    // overlay. The rest of this strip is text we format ourselves, but it
    // costs nothing to keep the whole row literal.
    m_sourceLabel->setTextFormat(Qt::PlainText);
    m_sourceLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    // Hidden by default: shown only when the port has confirmed live source
    // data. Avoids showing "RF SENSE" as an assumption before the trigger
    // mode is known.
    m_sourceLabel->setVisible(false);

    m_freqLabel = new QLabel(tr("N/A"), this);
    m_freqLabel->setTextFormat(Qt::PlainText);
    m_freqLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    m_stateLabel = new QLabel(QStringLiteral("—"), this);
    m_stateLabel->setAlignment(Qt::AlignCenter);
    m_stateLabel->setFixedWidth(38);

    row->addWidget(m_portLabel);
    row->addWidget(m_pttLabel);
    row->addWidget(m_bandLabel);
    row->addWidget(m_biasLabel);
    row->addWidget(m_sourceLabel);
    row->addStretch(1);
    row->addWidget(m_freqLabel);
    row->addWidget(m_stateLabel);

    applyTheme();
    updateAccessibleText();
}

void AccessoryPortRow::setPtt(bool keyed)
{
    if (m_ptt == keyed) return;
    m_ptt = keyed;
    applyTheme();
    updateAccessibleText();
}

void AccessoryPortRow::setBandText(const QString& band)
{
    const QString shown = band.trimmed().isEmpty() ? tr("N/A") : band.trimmed();
    if (m_bandLabel->text() == shown) return;
    m_bandLabel->setText(shown);
    applyTheme();
    updateAccessibleText();
}

void AccessoryPortRow::setBiasText(const QString& bias)
{
    const QString shown = bias.trimmed();
    if (m_biasLabel->text() == shown) return;
    m_biasLabel->setText(shown);
    m_biasLabel->setVisible(!shown.isEmpty());
    applyTheme();
    updateAccessibleText();
}

void AccessoryPortRow::setFrequencyVisible(bool visible)
{
    // isVisible() is false for every child of a widget that has not been shown
    // yet, so gating on it silently drops a hide issued during construction —
    // and the cell comes back the moment the panel is shown. isHidden() is the
    // widget's own flag rather than its effective visibility, which is the
    // question actually being asked here.
    if (m_freqLabel->isHidden() == !visible) return;
    m_freqLabel->setVisible(visible);
    updateAccessibleText();
}

void AccessoryPortRow::setSourceVisible(bool visible)
{
    if (m_sourceLabel->isHidden() == !visible) return;
    m_sourceLabel->setVisible(visible);
    updateAccessibleText();
}

void AccessoryPortRow::setSourceText(const QString& source)
{
    const QString shown = source.trimmed().isEmpty() ? QStringLiteral("—") : source.trimmed();
    if (m_sourceLabel->text() == shown) return;
    m_sourceLabel->setText(shown);
    updateAccessibleText();
}

void AccessoryPortRow::setFrequencyMhz(double mhz)
{
    QString shown = tr("N/A");
    if (mhz > 0.0) {
        // Same MHz.kHz.Hz grouping the VFO readout uses, so a frequency reads
        // identically wherever it appears in the app.
        const long long hz = static_cast<long long>(std::llround(mhz * 1e6));
        shown = QStringLiteral("%1.%2.%3")
                    .arg(hz / 1000000)
                    .arg((hz / 1000) % 1000, 3, 10, QChar('0'))
                    .arg(hz % 1000, 3, 10, QChar('0'));
    }
    if (m_freqLabel->text() == shown) return;
    m_freqLabel->setText(shown);
    applyTheme();
    updateAccessibleText();
}

void AccessoryPortRow::setStateText(const QString& state)
{
    if (m_stateLabel->text() == state) return;
    m_stateLabel->setText(state);
    m_stateLabel->setVisible(!state.isEmpty());
    applyTheme();
    updateAccessibleText();
}

void AccessoryPortRow::setBypassed(bool bypassed)
{
    if (m_bypassed == bypassed) return;
    m_bypassed = bypassed;
    applyTheme();
    updateAccessibleText();
}

void AccessoryPortRow::setActive(bool active)
{
    if (m_active == active) return;
    m_active = active;
    applyTheme();
    updateAccessibleText();
}

void AccessoryPortRow::setScale(qreal scale)
{
    const qreal clamped = qBound(0.6, scale, 4.0);
    if (qFuzzyCompare(m_scale, clamped)) return;
    m_scale = clamped;

    // Cell widths track the type inside them, or the text outgrows its box.
    m_portLabel->setFixedWidth(px(18));
    m_pttLabel->setFixedWidth(px(34));
    m_bandLabel->setMinimumWidth(px(34));
    m_biasLabel->setMinimumWidth(px(34));
    m_stateLabel->setFixedWidth(px(38));
    if (auto* row = qobject_cast<QHBoxLayout*>(layout())) {
        row->setContentsMargins(px(6), px(3), px(6), px(3));
        row->setSpacing(px(6));
    }
    applyTheme();
    updateGeometry();
}

int AccessoryPortRow::px(int base) const
{
    return qMax(1, qRound(base * m_scale));
}

void AccessoryPortRow::applyTheme()
{
    auto& theme = AetherSDR::ThemeManager::instance();

    // The row's own fill and frame are painted in paintEvent, not styled here
    // — see the note there. Only the children carry style sheets.
    update();

    // Children carry their own border:none — the container rule above would
    // otherwise be inherited and box every label inside the row.
    theme.applyStyleSheet(m_portLabel, QStringLiteral(
        "QLabel { border: none; background: transparent; color: {{color.text.primary}}; "
        "font-size: %1px; font-weight: bold; }").arg(px(14)));

    theme.applyStyleSheet(m_pttLabel, QStringLiteral(
        "QLabel { border: none; border-radius: 2px; padding: %1px %2px; "
        "background: %3; color: %4; font-size: %5px; font-weight: bold; }")
        .arg(px(1)).arg(px(3))
        .arg(m_ptt ? QStringLiteral("{{color.accent.danger}}")
                   : QStringLiteral("{{color.background.2}}"))
        .arg(m_ptt ? QStringLiteral("{{color.background.0}}")
                   : QStringLiteral("{{color.text.label}}"))
        .arg(px(10)));

    const bool haveBand = m_bandLabel->text() != tr("N/A");
    theme.applyStyleSheet(m_bandLabel, QStringLiteral(
        "QLabel { border: none; border-radius: 2px; padding: %1px %2px; "
        "background: %3; color: %4; font-size: %5px; font-weight: bold; }")
        .arg(px(1)).arg(px(4))
        .arg(haveBand ? QStringLiteral("{{color.background.success}}")
                      : QStringLiteral("{{color.background.2}}"))
        .arg(haveBand ? QStringLiteral("{{color.text.primary}}")
                      : QStringLiteral("{{color.text.label}}"))
        .arg(px(11)));

    // The bias profile reads as a setting rather than a reading, so it takes
    // the neutral chip treatment rather than the band's live-value green.
    theme.applyStyleSheet(m_biasLabel, QStringLiteral(
        "QLabel { border: none; border-radius: 2px; padding: %1px %2px; "
        "background: {{color.background.2}}; color: {{color.text.primary}}; "
        "font-size: %3px; font-weight: bold; }")
        .arg(px(1)).arg(px(4)).arg(px(11)));

    theme.applyStyleSheet(m_sourceLabel, QStringLiteral(
        "QLabel { border: none; background: transparent; color: {{color.text.primary}}; "
        "font-size: %1px; }").arg(px(12)));

    const bool haveFreq = m_freqLabel->text() != tr("N/A");
    const QString freqTone = !haveFreq ? QStringLiteral("{{color.text.label}}")
                           : m_bypassed ? QStringLiteral("{{color.accent.warning}}")
                                        : QStringLiteral("{{color.accent.success}}");
    theme.applyStyleSheet(m_freqLabel, QStringLiteral(
        "QLabel { border: none; background: transparent; color: %1; "
        "font-size: %2px; %3 }")
        .arg(freqTone).arg(px(13))
        .arg(haveFreq ? QStringLiteral("font-weight: bold;") : QString()));

    // OPR is the live state; BYP and STBY are not, so only OPR reads as such.
    theme.applyStyleSheet(m_stateLabel, QStringLiteral(
        "QLabel { border: none; background: transparent; color: %1; "
        "font-size: %2px; font-weight: bold; }")
        .arg(m_stateLabel->text() == QLatin1String("OPR")
                 ? QStringLiteral("{{color.accent.success}}")
                 : QStringLiteral("{{color.text.secondary}}"))
        .arg(px(11)));
}

void AccessoryPortRow::paintEvent(QPaintEvent*)
{
    // Painted rather than style-sheeted. A style sheet on this widget can only
    // select it as "QWidget" — which also matches every QLabel inside it — or
    // by a class name that Qt spells with the namespace mangled in
    // ("AetherSDR--AccessoryPortRow"), a selector that silently matches nothing the
    // moment the class moves namespace. Painting the frame is neither
    // ambiguous nor fragile, and the transmit-port outline is the one thing on
    // this strip that has to be unmistakable.
    auto& theme = AetherSDR::ThemeManager::instance();
    const QColor fill = theme.color(this, QStringLiteral("color.background.1"));
    const QColor frame = m_active
        ? theme.color(this, QStringLiteral("color.accent.danger"))
        : theme.color(this, QStringLiteral("color.border.subtle"));

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const qreal pen = m_active ? 2.0 : 1.0;
    const QRectF box = QRectF(rect()).adjusted(pen / 2.0, pen / 2.0, -pen / 2.0, -pen / 2.0);
    p.setBrush(fill);
    p.setPen(QPen(frame, pen));
    p.drawRoundedRect(box, 3.0, 3.0);
}

void AccessoryPortRow::updateAccessibleText()
{
    setAccessibleName(tr("Port %1").arg(m_portLetter));
    // One sentence covering the whole strip: a reader moving across six
    // separate labels loses which port they belong to.
    const QString state = m_bypassed ? tr("bypassed") : m_stateLabel->text();
    const QString bias = m_biasLabel->isVisible() && !m_biasLabel->text().isEmpty()
                             ? tr("bias %1, ").arg(m_biasLabel->text())
                             : QString();
    // A hidden frequency cell is one the device cannot report at all — the
    // amplifier's strips leave it out — so it is dropped from the sentence
    // rather than spoken as "N/A", which would claim a missing reading.
    const QString freq = !m_freqLabel->isHidden()
                             ? tr("%1, ").arg(m_freqLabel->text())
                             : QString();
    // One multi-argument arg(), not a chain of them. The source and band cells
    // carry device text verbatim (flexA / bandA off the wire), and a chain
    // substitutes left to right: a source name containing "%4" would land in
    // the string and then be replaced by the next .arg() in the chain. The
    // multi-argument form substitutes every placeholder in one pass, so text
    // that arrives from the device cannot reach into the format string
    // (Principle VII).
    // The state carries its own separator, the way the bias and frequency
    // pieces do. On the amplifier's strips the cell is deliberately empty
    // whenever there is nothing to act on, which is most of the time — a
    // separator left standing around it speaks as ", ,".
    const QString statePart = state.isEmpty() ? QString()
                                              : QStringLiteral("%1, ").arg(state);
    setAccessibleDescription(
        tr("%1, band %2, %6%7%3%4%5").arg(
            m_sourceLabel->text(),
            m_bandLabel->text(),
            statePart,
            m_ptt ? tr("transmitting") : tr("not transmitting"),
            m_active ? tr(", transmit port") : QString(),
            bias,
            freq));
}

}  // namespace AetherSDR
