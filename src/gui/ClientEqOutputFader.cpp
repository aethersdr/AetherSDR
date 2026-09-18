#include "ClientEqOutputFader.h"

#include <QEvent>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLinearGradient>
#include <QLocale>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include "SmartMtrGeometry.h"
#include "SmartMtrStyle.h"

#include <QHBoxLayout>
#include <QPainterPath>
#include <QPolygonF>
#include <QResizeEvent>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include "core/ThemeManager.h"

namespace AetherSDR {

namespace {

float linearToDb(float linear)
{
    if (linear <= 1e-6f) return -120.0f;
    return 20.0f * std::log10(linear);
}

float dbToLinear(float db)
{
    return std::pow(10.0f, db / 20.0f);
}

// Attack / release for the displayed peak — fast rise, slow fall so the
// bar tracks transients but doesn't flicker.
constexpr float kPeakAttack  = 0.6f;
constexpr float kPeakRelease = 0.08f;

} // namespace

ClientEqOutputFader::ClientEqOutputFader(QWidget* parent) : QWidget(parent)
{
    // Total width = label column + gap + bar + overhang each side.
    setFixedWidth(kLabelColW + kGap + kBarW + kHandleOverhang * 2 + 2);
    setMinimumHeight(160);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    setFocusPolicy(Qt::ClickFocus);
    setCursor(Qt::ArrowCursor);
    setMouseTracking(false);
    setToolTip(
        "Output gain (dB). Drag to set, wheel for fine step,\n"
        "double-click to reset to 0 dB.");

    m_endLabel = new QLabel("OUT");
    m_endLabel->setAlignment(Qt::AlignCenter);
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_endLabel, "QLabel { color: {{color.text.secondary}}; font-size: 10px; font-weight: bold;"
        " background: transparent; border: none; }");

    // Inline-editable value at the bottom of the fader.  Click to focus,
    // type a dB value, Enter or focus-out to commit (clamped to range).
    // Looks identical to a label until focused; subtle inset + cyan
    // border on focus indicates edit mode (matches ClientCompKnob).
    m_valueEdit = new QLineEdit;
    m_valueEdit->setAlignment(Qt::AlignCenter);
    m_valueEdit->setFrame(false);
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_valueEdit, "QLineEdit { color: {{color.text.primary}}; font-size: 10px; font-weight: bold;"
        " background: transparent; border: 1px solid transparent;"
        " border-radius: 2px; padding: 0;"
        " selection-background-color: {{color.background.2}}; }"
        "QLineEdit:focus { background: {{color.background.0}}; border: 1px solid {{color.accent}}; }");
    m_valueEdit->installEventFilter(this);

    // The reading, for strips that report rather than set. Centred in its own
    // cap so the digits sit still as the value changes width.
    m_levelLabel = new QLabel;
    m_levelLabel->setAlignment(Qt::AlignCenter);
    AetherSDR::ThemeManager::instance().applyStyleSheet(m_levelLabel,
        "QLabel { color: {{color.text.primary}}; font-size: 10px; font-weight: bold;"
        " background: transparent; border: none; }");
    m_levelLabel->setVisible(false);

    rebuildLabelLayout();

    connect(m_valueEdit, &QLineEdit::returnPressed, this, [this] {
        commitValueEdit();
        m_valueEdit->clearFocus();
    });
    connect(m_valueEdit, &QLineEdit::editingFinished, this, [this] {
        commitValueEdit();
    });

    refreshValueLabel();
}

void ClientEqOutputFader::commitValueEdit()
{
    if (!m_valueEdit) return;
    static thread_local bool s_committing = false;
    if (s_committing) return;
    s_committing = true;
    const QString raw = m_valueEdit->text().trimmed();
    bool ok = false;
    double v = QLocale().toDouble(raw, &ok);
    if (!ok) {
        QString cleaned;
        cleaned.reserve(raw.size());
        for (QChar c : raw) {
            if (c.isDigit() || c == QChar('.') || c == QChar('-')
                || c == QChar('+') || c == QChar('e') || c == QChar('E'))
                cleaned.append(c);
        }
        v = cleaned.toDouble(&ok);
    }
    if (ok) {
        const float db = std::clamp(static_cast<float>(v),
                                    kGainMinDb, kGainMaxDb);
        m_gain = dbToLinear(db);
        refreshValueLabel();
        emit gainChanged(m_gain);
        update();
    } else {
        refreshValueLabel();
    }
    s_committing = false;
}

bool ClientEqOutputFader::eventFilter(QObject* obj, QEvent* ev)
{
    if (obj == m_valueEdit) {
        if (ev->type() == QEvent::Wheel) {
            wheelEvent(static_cast<QWheelEvent*>(ev));
            return true;
        }
        if (ev->type() == QEvent::KeyPress) {
            auto* ke = static_cast<QKeyEvent*>(ev);
            if (ke->key() == Qt::Key_Escape) {
                QSignalBlocker b(m_valueEdit);
                refreshValueLabel();
                m_valueEdit->clearFocus();
                return true;
            }
        }
        if (ev->type() == QEvent::FocusIn) {
            // Show bare number on focus so the user types just digits.
            QSignalBlocker b(m_valueEdit);
            const float db = linearToDb(m_gain);
            m_valueEdit->setText(QString::number(db, 'f', 1));
            m_valueEdit->selectAll();
        } else if (ev->type() == QEvent::FocusOut) {
            refreshValueLabel();
        }
    }
    return QWidget::eventFilter(obj, ev);
}

void ClientEqOutputFader::setGainLinear(float linear)
{
    m_gain = std::clamp(linear, 0.0f, 4.0f);
    refreshValueLabel();
    update();
}

void ClientEqOutputFader::setPeakLinear(float peakLinear)
{
    const float peakDb = linearToDb(std::max(peakLinear, 1e-6f));
    const float alpha = (peakDb > m_smoothedPeak) ? kPeakAttack : kPeakRelease;
    m_smoothedPeak += alpha * (peakDb - m_smoothedPeak);

    // Same call pattern as the VFO flag's meter: record the raw level, then
    // tick the slew with however long it has actually been. The caller's rate
    // is the FFT timer's, and the tracker does not care as long as the elapsed
    // time is honest.
    if (!m_extremesClock.isValid()) {
        m_extremesClock.start();
    }
    const qint64 nowMs = m_extremesClock.elapsed();
    // Per instance, not static: the RX and TX faders both live in this process
    // and would otherwise trade elapsed times, leaving whichever painted second
    // with a dt of zero and markers that never slew.
    const qint64 dtMs = std::max<qint64>(0, nowMs - m_lastExtremesMs);
    m_lastExtremesMs = nowMs;
    m_extremes.record(peakDb, nowMs);
    m_extremes.tick(nowMs, dtMs, posUnitsForDb(m_smoothedPeak),
                    [this](double db) { return posUnitsForDb(db); });

    refreshLevelLabel();
    update();
}

// A level in dB to its hole-local position in SmartMTR UNITS. The scale band
// runs kScaleMin..kScaleMax inside the hole, leaving a margin at each end.
double ClientEqOutputFader::posUnitsForDb(double db) const
{
    using namespace SmartMtrUnits;
    const double norm = std::clamp(
        (db - kMeterMinDb) / (kMeterMaxDb - kMeterMinDb), 0.0, 1.0);
    return kScaleMin + norm * (kScaleMax - kScaleMin);
}

void ClientEqOutputFader::refreshLevelLabel()
{
    if (!m_levelLabel || m_gainControl) return;
    m_levelLabel->setText(m_smoothedPeak <= kMeterMinDb + 0.5f
        ? QStringLiteral("-inf")
        : QStringLiteral("%1 dB").arg(m_smoothedPeak, 0, 'f', 1));
}

void ClientEqOutputFader::refreshValueLabel()
{
    if (!m_valueEdit || m_valueEdit->hasFocus()) return;
    const float db = linearToDb(m_gain);
    QSignalBlocker b(m_valueEdit);
    if (db <= kGainMinDb + 0.05f) {
        m_valueEdit->setText("-inf");
    } else {
        m_valueEdit->setText(QString::asprintf("%+.1f dB", db));
    }
}

void ClientEqOutputFader::setGainFromPos(QPoint pos)
{
    // Vertical counts up from the bottom, horizontal along from the left.
    const int along = (m_orientation == Qt::Horizontal) ? pos.x() : pos.y();
    float norm = std::clamp(
        static_cast<float>(along - m_stripOrigin) / std::max(1, m_stripLength),
        0.0f, 1.0f);
    if (m_orientation == Qt::Vertical) {
        norm = 1.0f - norm;
    }
    const float db = kGainMinDb + norm * (kGainMaxDb - kGainMinDb);
    m_gain = dbToLinear(db);
    refreshValueLabel();
    emit gainChanged(m_gain);
    update();
}

void ClientEqOutputFader::setGainControlEnabled(bool enabled)
{
    if (m_gainControl == enabled) return;
    m_gainControl = enabled;

    if (m_valueEdit) {
        // Hidden rather than made read-only: a greyed field that cannot be
        // typed into still reads as a control someone has taken away.
        m_valueEdit->setVisible(enabled);
    }
    if (m_levelLabel) {
        m_levelLabel->setVisible(!enabled);
        refreshLevelLabel();
    }
    setCursor(Qt::ArrowCursor);
    setToolTip(enabled
        ? QStringLiteral("Output gain (dB). Drag to set, wheel for fine step,\n"
                         "double-click to reset to 0 dB.")
        : QStringLiteral("Post-EQ output level."));
    rebuildLabelLayout();
    update();
}

void ClientEqOutputFader::setOrientation(Qt::Orientation orientation)
{
    if (m_orientation == orientation) return;
    m_orientation = orientation;

    if (m_orientation == Qt::Horizontal) {
        // Bar, plus the scale figures printed beneath it, plus a little air.
        setMinimumWidth(240);
        setMaximumWidth(QWIDGETSIZE_MAX);
        setFixedHeight(44);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    } else {
        setMinimumHeight(160);
        setMaximumHeight(QWIDGETSIZE_MAX);
        setFixedWidth(kLabelColW + kGap + kBarW + kHandleOverhang * 2 + 2);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    }
    rebuildLabelLayout();
    update();
}

// The two caps -- "OUT" and the editable dB value -- sit at the ends of the
// strip, so which end depends on the orientation. Rebuilt rather than
// re-parented: a QLayout cannot change direction after the fact.
void ClientEqOutputFader::rebuildLabelLayout()
{
    delete layout();

    if (m_orientation == Qt::Horizontal) {
        auto* row = new QHBoxLayout(this);
        row->setContentsMargins(4, 0, 4, 0);
        row->setSpacing(6);
        row->addWidget(m_endLabel, 0, Qt::AlignVCenter);
        row->addStretch(1);
        if (m_gainControl) {
            m_valueEdit->setFixedWidth(46);
            row->addWidget(m_valueEdit, 0, Qt::AlignVCenter);
        } else {
            m_levelLabel->setFixedWidth(56);
            row->addWidget(m_levelLabel, 0, Qt::AlignVCenter);
        }
    } else {
        auto* col = new QVBoxLayout(this);
        col->setContentsMargins(0, 2, 0, 2);
        col->setSpacing(0);
        col->addWidget(m_endLabel);
        col->addStretch(1);
        if (m_gainControl) {
            m_valueEdit->setMinimumWidth(0);
            m_valueEdit->setMaximumWidth(QWIDGETSIZE_MAX);
            col->addWidget(m_valueEdit);
        } else {
            col->addWidget(m_levelLabel);
        }
    }
}

void ClientEqOutputFader::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    if (m_orientation == Qt::Horizontal) {
        paintHorizontal(p);
    } else {
        paintVertical(p);
    }
}

// Left to right under the graph, in the SmartMTR control's clothes: a recessed
// hole with rounded ends, the level as a red bar from the hole's left edge with
// a bright line at its head, scale ticks above, and the peak/trough sweep
// markers hanging inside the hole. Colours, proportions and the marker shape
// come from SmartMtrStyle.h, so this reads as the same instrument as the meter
// in the VFO flag.
//
// Everything is positioned in hole-local UNITS and mapped to pixels here, the
// way SmartMtrGeometry does it -- but with a separate scale per axis. The
// vertical keeps the design's proportions; the horizontal stretches to the
// panel's width, which a uniform fit would letterbox away, and that width is
// the whole reason this meter moved down here.
// The hole, in pixels. SmartMTR budgets 20 units above it for the scale labels
// and ticks; whatever that budget does not spend is dead space, and half of it
// is moved below the meter so the control sits nearer the middle of its band
// instead of riding high in it.
QRectF ClientEqOutputFader::holeRect() const
{
    using namespace SmartMtrUnits;
    if (m_orientation != Qt::Horizontal) return QRectF();

    const int gap = 8;
    const int left = (m_endLabel ? m_endLabel->geometry().right() : 0) + gap;
    const QWidget* cap = m_gainControl ? static_cast<QWidget*>(m_valueEdit)
                                       : static_cast<QWidget*>(m_levelLabel);
    const int right = ((cap && cap->isVisible()) ? cap->geometry().left()
                                                 : width()) - gap;
    const double holeW = std::max(1, right - left);
    const double unitY = double(height()) / kControlH;

    const double labelUnits =
        std::max(7.0, kLabelHeightNormal * unitY * 0.8) / std::max(0.01, unitY);
    const double needed = labelUnits + kLabelGap + kMarkerLargeH;
    const double spare = std::max(0.0, kHoleMargY - needed);
    const double topUnits = kHoleMargY - spare / 2.0;

    return QRectF(left, topUnits * unitY, holeW, kHoleH * unitY);
}

void ClientEqOutputFader::centreCapsOnHole()
{
    if (m_orientation != Qt::Horizontal) return;
    auto* row = qobject_cast<QHBoxLayout*>(layout());
    if (!row) return;

    // A layout centres its children in the band left by its margins, so shift
    // that band down by twice the offset between the widget's centre and the
    // hole's: the caps then line up with the meter, not with the widget.
    const QRectF hole = holeRect();
    if (hole.isEmpty()) return;
    const int delta = int(std::lround(2.0 * (hole.center().y() - height() / 2.0)));
    row->setContentsMargins(4, std::max(0, delta), 4, std::max(0, -delta));
}

void ClientEqOutputFader::resizeEvent(QResizeEvent* ev)
{
    QWidget::resizeEvent(ev);
    centreCapsOnHole();
}

void ClientEqOutputFader::paintHorizontal(QPainter& p)
{
    using namespace SmartMtrUnits;

    const QRectF holeR = holeRect();
    const double left = holeR.left();
    const double holeW = holeR.width();
    const double unitX = holeW / kHoleW;
    const double unitY = double(height()) / kControlH;
    const auto px = [&](double units) { return left + units * unitX; };
    // Mouse geometry is the scale band, not the whole hole: the bar's 0..10
    // stub is decoration, and dragging there should mean "minimum", not a
    // position off the bottom of the scale.
    m_stripOrigin = int(px(kScaleMin));
    m_stripLength = std::max(1, int(px(kScaleMax) - px(kScaleMin)));

    p.setRenderHint(QPainter::Antialiasing, true);
    const double radius = kHoleRadius * unitY;

    p.setPen(Qt::NoPen);
    p.setBrush(SmartMtrColors::kBackground);
    p.drawRoundedRect(holeR, radius, radius);

    QPainterPath clip;
    clip.addRoundedRect(holeR, radius, radius);

    // Level. The bar starts at the hole's left edge -- hole-local 0, not the
    // scale minimum -- so a silent channel still shows the short stub the VFO
    // meter shows rather than an empty slot.
    {
        const double pos = posUnitsForDb(m_smoothedPeak);
        p.save();
        p.setClipPath(clip);
        p.setBrush(SmartMtrColors::kForeground);
        p.drawRect(QRectF(px(0.0), holeR.top(), px(pos) - px(0.0), holeR.height()));
        const double lineW = std::max(1.0, kIndicatorLine * unitX);
        p.setBrush(SmartMtrColors::kIndicator);
        p.drawRect(QRectF(px(pos) - lineW, holeR.top(), lineW, holeR.height()));
        p.restore();
    }

    // Peak and trough sweep markers: a triangle with its summit on the hole's
    // top edge marking the exact position, body hanging inside the hole.
    if (m_extremes.hasData()) {
        p.save();
        p.setClipPath(clip);
        p.setBrush(SmartMtrColors::kExtreme);
        const auto drawTri = [&](double posUnits) {
            const double x = px(posUnits);
            const double apexY = holeR.top();
            const double baseY = holeR.top() + SmartMtrExtremes::kExtremeTriH * unitY;
            const double halfW = SmartMtrExtremes::kExtremeTriW / 2.0 * unitX;
            QPolygonF tri;
            tri << QPointF(x, apexY) << QPointF(x - halfW, baseY)
                << QPointF(x + halfW, baseY);
            p.drawPolygon(tri);
        };
        drawTri(m_extremes.maxPosUnits());
        drawTri(m_extremes.minPosUnits());
        p.restore();
    }

    // Scale ticks above the hole. Large ticks carry a label; the small ones in
    // between are drawn faint so they read as secondary, exactly as the flag's
    // meter grades its own scale.
    p.setRenderHint(QPainter::Antialiasing, false);
    QFont f = p.font();
    f.setPixelSize(std::max(7, int(kLabelHeightNormal * unitY * 0.8)));
    p.setFont(f);
    const QFontMetrics fm(f);

    // Labelled ticks carry the numbers an operator sets by; the unlabelled ones
    // in between are for reading the level against. Everything from 0 up is
    // red: past unity is the part worth noticing.
    struct Tick { float db; const char* label; bool high; };
    static constexpr Tick kTicks[] = {
        { -60.0f,   "-60", false }, { -55.0f, nullptr, false },
        { -50.0f,   "-50", false }, { -45.0f, nullptr, false },
        { -40.0f,   "-40", false }, { -35.0f, nullptr, false },
        { -30.0f,   "-30", false }, { -25.0f, nullptr, false },
        { -20.0f,   "-20", false }, { -16.0f, nullptr, false },
        { -12.0f,   "-12", false }, {  -9.0f, nullptr, false },
        {  -6.0f,    "-6", false }, {  -3.0f, nullptr, false },
        {   0.0f,     "0", true  }, {  +3.0f, nullptr, true  },
        {  +6.0f,    "+6", true  }, {  +9.0f, nullptr, true  },
        { +12.0f,   "+12", true  },
    };
    for (const auto& t : kTicks) {
        const double x = px(posUnitsForDb(t.db));
        const bool large = (t.label != nullptr);
        // Sub-ticks are two thirds of a labelled one. The large tick keeps the
        // flag's kMarkerLargeH, since that is what the label gap above is
        // budgeted against.
        const double tickH = (large ? kMarkerLargeH : kMarkerLargeH * 2.0 / 3.0) * unitY;
        // A pixel narrower than the design calls for, both kinds: at this
        // meter's width a unit is over two pixels, and the ladder reads better
        // finer than it does bolder.
        const double tickW = std::max(
            1.0, (large ? kMarkerLargeW * unitX - 2.0
                        : kMarkerSmallW * unitX - 1.0));
        const QColor colour = t.high ? SmartMtrColors::kMarkerHigh
                                     : SmartMtrColors::kMarkerNormal;

        p.setOpacity(large ? 1.0 : kMarkerSmallOpacity);
        // Above and below: the hole sits 20 units down a 35-unit control, so
        // the 5 below it are exactly a large tick's worth and the ladder can
        // run both sides without the widget growing.
        p.fillRect(QRectF(x - tickW / 2.0, holeR.top() - tickH, tickW, tickH),
                   colour);
        p.fillRect(QRectF(x - tickW / 2.0, holeR.bottom(), tickW, tickH),
                   colour);
        p.setOpacity(1.0);

        if (!large) continue;
        const QString label = QString::fromLatin1(t.label);
        const int tw = fm.horizontalAdvance(label);
        const int tx = std::clamp(int(x) - tw / 2, int(holeR.left()),
                                  int(holeR.right()) - tw);
        p.setPen(colour);
        p.drawText(tx, int(holeR.top() - kMarkerLargeH * unitY - kLabelGap * unitY),
                   label);
    }

    if (!m_gainControl) return;

    // The gain handle: the one thing here the flag's meter has no need of,
    // since that one only reports. White, standing across the whole hole, so it
    // reads as a setting rather than as part of the level.
    const float gainDb = std::clamp(linearToDb(m_gain),
                                    kGainMinDb, kGainMaxDb);
    const float gainNorm = (gainDb - kGainMinDb) / (kGainMaxDb - kGainMinDb);
    const double handleX = m_stripOrigin + gainNorm * m_stripLength;
    const double handleW = std::max(2.0, kMarkerLargeW * unitX);
    p.fillRect(QRectF(handleX - handleW / 2.0,
                      holeR.top() - kHandleOverhang,
                      handleW,
                      holeR.height() + kHandleOverhang * 2),
               SmartMtrColors::kIndicator);
}

void ClientEqOutputFader::paintVertical(QPainter& p)
{
    // The strip lives between the OUT label at the top and the value
    // label at the bottom — paintEvent receives the full widget rect and
    // we carve out a fixed vertical band in the middle.
    const int topLabelH = 16;
    const int botLabelH = 14;
    const int stripTop  = topLabelH + kStripTopPad;
    const int stripBot  = height() - botLabelH - kStripBottomPad;
    m_stripOrigin = stripTop;
    m_stripLength = std::max(1, stripBot - stripTop);

    const int barLeft = kLabelColW + kGap + kHandleOverhang;
    const QRect barR(barLeft, stripTop, kBarW, m_stripLength);

    // Background for the bar — dark inset.
    p.fillRect(barR, QColor("#06111c"));

    // Level fill — gradient bottom green → top red, clipped to the level
    // height derived from the smoothed peak.
    const float peakNorm = std::clamp(
        (m_smoothedPeak - kMeterMinDb) / (kMeterMaxDb - kMeterMinDb),
        0.0f, 1.0f);
    const int fillH = static_cast<int>(peakNorm * m_stripLength);
    if (fillH > 0) {
        const QRect fill(barR.x(), barR.y() + m_stripLength - fillH,
                         kBarW, fillH);
        QLinearGradient grad(0, barR.y() + m_stripLength, 0, barR.y());
        grad.setColorAt(0.0, QColor("#2f9e6a"));   // green bottom
        grad.setColorAt(0.55, QColor("#6cc56a"));  // lime
        grad.setColorAt(0.80, QColor("#e8b94c"));  // amber
        grad.setColorAt(0.95, QColor("#e8553c"));  // red top
        grad.setColorAt(1.0, QColor("#f2362a"));
        p.fillRect(fill, grad);
    }

    // Bar outline.
    p.setPen(QPen(QColor("#243a4e"), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(barR.adjusted(0, 0, -1, -1));

    // dB scale labels + tick marks on the left side.
    QFont f = p.font();
    f.setPixelSize(8);
    p.setFont(f);
    const QFontMetrics fm(f);
    const int textRight = kLabelColW - 2;

    struct Tick { float db; const char* label; };
    static constexpr Tick kTicks[] = {
        { +12.0f,  "+12" },
        {  +6.0f,  "+6" },
        {   0.0f,  "0" },
        {  -6.0f,  "-6" },
        { -12.0f,  "-12" },
        { -20.0f,  "-20" },
        { -40.0f,  "-40" },
    };
    for (const auto& t : kTicks) {
        const float norm = (t.db - kMeterMinDb) / (kMeterMaxDb - kMeterMinDb);
        const int y = stripTop + static_cast<int>((1.0f - norm) * m_stripLength);

        p.setPen(QColor("#7f93a5"));
        const QString s = QString::fromLatin1(t.label);
        const int tw = fm.horizontalAdvance(s);
        const int ty = std::clamp(y + fm.ascent() / 2 - 1,
                                  stripTop + fm.ascent() - 1,
                                  stripTop + m_stripLength - 1);
        p.drawText(textRight - tw, ty, s);

        p.setPen(QColor("#405060"));
        p.drawLine(textRight, y, barLeft - 1, y);
    }

    // Fader handle — horizontal bar that overhangs the meter on both
    // sides so it's easy to grab without covering the level colour.
    const float gainDb = std::clamp(linearToDb(m_gain),
                                    kGainMinDb, kGainMaxDb);
    const float gainNorm = (kGainMaxDb - gainDb) / (kGainMaxDb - kGainMinDb);
    const int handleY = stripTop + static_cast<int>(gainNorm * m_stripLength);
    const QRect handleR(barLeft - kHandleOverhang,
                        handleY - kHandleH / 2,
                        kBarW + kHandleOverhang * 2,
                        kHandleH);
    p.setPen(QPen(QColor("#0a1a28"), 1));
    p.setBrush(QColor("#d7e7f2"));   // cream / bright off-white
    p.drawRect(handleR);
    // Centre line — a single pixel on the handle so the exact gain level
    // reads clearly against the bar's colour.
    p.setPen(QColor("#1a2a3a"));
    p.drawLine(handleR.left() + 1, handleY,
               handleR.right() - 1, handleY);

    Q_UNUSED(barR);
}

void ClientEqOutputFader::mousePressEvent(QMouseEvent* ev)
{
    if (!m_gainControl) { QWidget::mousePressEvent(ev); return; }
    if (ev->button() == Qt::LeftButton) {
        m_dragging = true;
        setCursor(Qt::ClosedHandCursor);
        setGainFromPos(ev->pos());
        ev->accept();
        return;
    }
    QWidget::mousePressEvent(ev);
}

void ClientEqOutputFader::mouseMoveEvent(QMouseEvent* ev)
{
    if (m_gainControl && m_dragging) {
        setGainFromPos(ev->pos());
        ev->accept();
        return;
    }
    QWidget::mouseMoveEvent(ev);
}

void ClientEqOutputFader::mouseReleaseEvent(QMouseEvent* ev)
{
    if (m_dragging && ev->button() == Qt::LeftButton) {
        m_dragging = false;
        setCursor(Qt::ArrowCursor);
        ev->accept();
        return;
    }
    QWidget::mouseReleaseEvent(ev);
}

void ClientEqOutputFader::mouseDoubleClickEvent(QMouseEvent* ev)
{
    if (!m_gainControl) { QWidget::mouseDoubleClickEvent(ev); return; }
    if (ev->button() == Qt::LeftButton) {
        m_gain = 1.0f;  // 0 dB
        refreshValueLabel();
        emit gainChanged(m_gain);
        update();
        ev->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(ev);
}

void ClientEqOutputFader::wheelEvent(QWheelEvent* ev)
{
    if (!m_gainControl) { QWidget::wheelEvent(ev); return; }
    // 0.5 dB per notch (12 notches for a full deg of the wheel).
    const int notches = ev->angleDelta().y() / 120;
    if (notches == 0) { QWidget::wheelEvent(ev); return; }
    const float db = std::clamp(linearToDb(m_gain) + 0.5f * notches,
                                kGainMinDb, kGainMaxDb);
    m_gain = dbToLinear(db);
    refreshValueLabel();
    emit gainChanged(m_gain);
    update();
    ev->accept();
}

} // namespace AetherSDR
