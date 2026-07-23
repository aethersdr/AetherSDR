#include "SMeterWidget.h"
#include "SMeterWidgetAccessible.h"
#include "core/LogManager.h"
#include "core/ThemeManager.h"

#include <QAccessible>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QSet>
#include <QtMath>
#include <QFontMetrics>

#include <algorithm>
#include <cmath>
#include <optional>

namespace AetherSDR {

SMeterWidgetAccessible::SMeterWidgetAccessible(QWidget* widget)
    : QAccessibleWidget(widget, QAccessible::Indicator)
{
}

QString SMeterWidgetAccessible::text(QAccessible::Text textType) const
{
    const SMeterWidget* meter = qobject_cast<const SMeterWidget*>(widget());
    if (meter && textType == QAccessible::Value) {
        return meter->accessibleValueText();
    }
    return QAccessibleWidget::text(textType);
}

QAccessibleInterface* sMeterAccessibleFactory(const QString& key, QObject* object)
{
    if (key == QLatin1String("AetherSDR::SMeterWidget")) {
        return new SMeterWidgetAccessible(qobject_cast<QWidget*>(object));
    }
    return nullptr;
}

// --- Construction ------------------------------------------------------------

SMeterWidget::SMeterWidget(QWidget* parent)
    : QWidget(parent), m_geometry(SMeterGeometry::fallback())
{
    static bool s_accessibilityFactoryInstalled = false;
    if (!s_accessibilityFactoryInstalled) {
        s_accessibilityFactoryInstalled = true;
        QAccessible::installFactory(sMeterAccessibleFactory);
    }

    QString geometryError;
    const SMeterGeometry loadedGeometry = SMeterGeometry::loadResource(&geometryError);
    if (loadedGeometry.isValid()) {
        m_geometry = loadedGeometry;
    } else {
        qCWarning(lcMeters).noquote()
            << "SMeterWidget: using fallback geometry:" << geometryError;
    }

    QString themeError;
    m_faceThemes = AnalogMeterFaceThemeCatalog::loadResource(&themeError);
    if (!m_faceThemes.isValid()) {
        qCWarning(lcMeters).noquote()
            << "SMeterWidget: using fallback face themes:" << themeError;
        m_faceThemes = AnalogMeterFaceThemeCatalog::fallback();
    }

    const float minimumDbm = static_cast<float>(m_geometry.rxScale.minimumDbm);
    m_levelDbm = minimumDbm;
    m_peakDbm = minimumDbm;
    m_peakHoldDbm = minimumDbm;
    m_peakHoldDecayStartDbm = minimumDbm;

    setMinimumSize(minimumSizeHint());
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    setFocusPolicy(Qt::TabFocus);
    setProperty("faceTheme", faceThemeId());

    connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this, [this]() {
        if (m_faceTheme == AnalogMeterFaceTheme::AetherDefault) {
            m_backgroundCacheValid = false;
            update();
        }
    });

    m_needleFraction = dbmToFraction(m_levelDbm);
    m_targetNeedleFraction = m_needleFraction;
    m_radioSwrClock.start();
    m_needleAnimation.setTimerType(Qt::PreciseTimer);
    m_needleAnimation.setInterval(kNeedleAnimationIntervalMs);
    connect(&m_needleAnimation, &QTimer::timeout, this, &SMeterWidget::animateNeedle);

    m_accessibilityTimer.setSingleShot(true);
    m_accessibilityTimer.setInterval(kAccessibilityAnnouncementIntervalMs);
    connect(&m_accessibilityTimer, &QTimer::timeout,
            this, &SMeterWidget::publishAccessibleValue);

    // Peak hold decay: drops 0.5 dB every 50 ms after a new peak
    m_peakDecay.setInterval(50);
    connect(&m_peakDecay, &QTimer::timeout, this, [this]() {
        m_peakDbm -= 0.5f;
        if (m_peakDbm < m_levelDbm) {
            m_peakDbm = m_levelDbm;
            m_peakDecay.stop();
        }
        updateNeedleTarget();
        update();
        scheduleAccessibleValue();
    });

    // Hard reset peak hold every 10 seconds
    m_peakReset.setInterval(10000);
    m_peakReset.start();
    connect(&m_peakReset, &QTimer::timeout, this, [this]() {
        m_peakDbm = m_levelDbm;
        updateNeedleTarget();
        update();
        scheduleAccessibleValue();
    });
}

// --- Public interface --------------------------------------------------------

void SMeterWidget::setFloating(bool floating)
{
    setSizePolicy(floating ? QSizePolicy::Expanding : QSizePolicy::Preferred,
                  floating ? QSizePolicy::Expanding : QSizePolicy::Fixed);
    updateGeometry();
}

QString SMeterWidget::faceThemeId() const
{
    return analogMeterFaceThemeId(m_faceTheme);
}

QString SMeterWidget::accessibleValueText() const
{
    if (m_transmitting) {
        switch (m_txMode) {
        case TxMode::Power:
            return tr("Transmit power, %1 watts").arg(m_txPower, 0, 'f', 0);
        case TxMode::SWR:
            return tr("Transmit SWR, %1").arg(m_txSwr, 0, 'f', 1);
        case TxMode::Level:
            return tr("Transmit level, %1 dB").arg(m_micLevel, 0, 'f', 0);
        case TxMode::Compression:
            return tr("Transmit compression, %1 dB").arg(-m_compLevel, 0, 'f', 0);
        }
    }

    if (usesUnavailableRxMeter()) {
        return unavailableRxMeterLabel();
    }
    const float displayDbm =
        m_rxMode == RxMode::SMeterPeak ? m_peakDbm : m_levelDbm;
    return tr("%1, %2 dBm")
        .arg(sUnitsTextFor(displayDbm))
        .arg(static_cast<int>(displayDbm));
}

void SMeterWidget::setFaceTheme(AnalogMeterFaceTheme theme)
{
    if (m_faceTheme == theme) {
        return;
    }
    m_faceTheme = theme;
    m_backgroundCacheValid = false;
    setProperty("faceTheme", faceThemeId());
    update();
}

void SMeterWidget::setLevel(float dbm) // a11y-check: skip -- settled update is timer-throttled
{
    m_receiveMeterReadingActive = false;
    m_levelDbm = dbm;

    // Peak hold (existing needle/triangle behavior)
    if (dbm > m_peakDbm) {
        m_peakDbm = dbm;
        m_peakDecay.start();
    }

    // Configurable peak hold line
    if (m_peakHoldEnabled) {
        if (dbm > m_peakHoldDbm) {
            m_peakHoldDbm = dbm;
            m_peakHoldDecayStartDbm = dbm;
            m_peakHoldTimer.start();
            m_peakHoldTimerRunning = true;
        }
        updatePeakHoldValue();
    }

    updateNeedleTarget();

    if (!m_transmitting) {
        update();
        scheduleAccessibleValue();
    }
}

void SMeterWidget::setReceiveMeterReading(
    const KiwiSdrProtocol::MeterReading& reading)
{
    m_receiveMeterReading = reading;
    m_receiveMeterReadingActive = true;

    const bool hasDisplayDbm =
        (reading.capability == KiwiSdrProtocol::MeterCapability::CalibratedSndMeter
         || reading.capability == KiwiSdrProtocol::MeterCapability::Experimental)
        && reading.hasDbm;

    if (hasDisplayDbm) {
        m_levelDbm = reading.dbm;
        if (reading.dbm > m_peakDbm) {
            m_peakDbm = reading.dbm;
            m_peakDecay.start();
        }
        if (m_peakHoldEnabled && reading.dbm > m_peakHoldDbm) {
            m_peakHoldDbm = reading.dbm;
            m_peakHoldDecayStartDbm = reading.dbm;
            m_peakHoldTimer.start();
            m_peakHoldTimerRunning = true;
        }
    } else {
        const float minimumDbm = static_cast<float>(m_geometry.rxScale.minimumDbm);
        m_levelDbm = minimumDbm;
        m_peakDbm = minimumDbm;
        m_peakHoldDbm = minimumDbm;
        m_peakHoldDecayStartDbm = minimumDbm;
        m_peakHoldTimerRunning = false;
    }

    updateNeedleTarget();
    if (!m_transmitting) {
        update();
        scheduleAccessibleValue();
    }
}

void SMeterWidget::setTxMeters(float fwdPower, float swr)
{
    m_txPower = fwdPower;
    m_txSwr = swr;
    m_radioSwrFilter.reset();
    setProperty("txSwr", m_txSwr);
    setProperty("txSwrSource", QStringLiteral("external"));
    setProperty("txSwrHeld", false);
    setProperty("txSwrPowerEnvelopeWatts", 0.0);
    setProperty("txSwrMinimumForwardWatts", 0.05);

    finishTxMeterUpdate();
}

void SMeterWidget::setRadioTxMeters(float fwdPower, float fwdPowerInstant, float swr)
{
    m_txPower = fwdPower;
    updateRadioSwr(fwdPowerInstant, swr, m_radioSwrClock.elapsed());

    finishTxMeterUpdate();
}

void SMeterWidget::finishTxMeterUpdate()
{
    updateNeedleTarget();

    // Repaint whenever TX power data arrives — either because moxChanged set
    // m_transmitting, or because RF power is flowing regardless (e.g. VOX,
    // hardware-keyed CW, or interlock race where setTransmitting arrives late).
    if (m_transmitting || m_txPower > 0.5f) {
        update();
    }
    if (m_transmitting) {
        scheduleAccessibleValue();
    }
}

void SMeterWidget::updateRadioSwr(
    float forwardPowerInstant, float swr, qint64 timestampMs)
{
    const RadioSwrValidityFilter::Result result =
        m_radioSwrFilter.update(
            forwardPowerInstant, swr, timestampMs,
            static_cast<float>(m_geometry.swrScale.maximum));
    m_txSwr = result.displayedSwr;

    setProperty("txSwr", m_txSwr);
    setProperty("txSwrRaw", swr);
    setProperty("txSwrForwardWatts", forwardPowerInstant);
    setProperty("txSwrPowerEnvelopeWatts", result.forwardEnvelopeWatts);
    setProperty("txSwrMinimumForwardWatts", result.minimumForwardWatts);
    setProperty("txSwrSource", QStringLiteral("radio"));
    setProperty("txSwrHeld", result.held);
}

void SMeterWidget::setMicMeters(float micLevel, float compLevel, float micPeak, float compPeak)
{
    Q_UNUSED(compLevel);
    m_micLevel = micLevel;   // MIC meter — live level, matches Phone/CW Level gauge
    m_micPeak  = micPeak;    // MICPEAK meter — stored for future peak-tick rendering
    // MeterModel normalizes COMPPEAK to a 0..25 dB compression amount.
    m_compLevel = qBound(0.0f, compPeak, 25.0f);

    updateNeedleTarget();

    if (m_transmitting && (m_txMode == TxMode::Level || m_txMode == TxMode::Compression)) {
        update();
        scheduleAccessibleValue();
    }
}

void SMeterWidget::setTransmitting(bool tx)
{
    m_transmitting = tx;
    setProperty("transmitting", tx);
    if (!tx) {
        // Clear TX values immediately on un-key so the RX reading becomes the
        // animation target as soon as transmit ends.
        m_txPower = 0.0f;
        m_txSwr   = 1.0f;
        m_radioSwrFilter.reset();
        setProperty("txSwr", m_txSwr);
        setProperty("txSwrHeld", false);
        setProperty("txSwrPowerEnvelopeWatts", 0.0);
        setProperty("txSwrMinimumForwardWatts", 0.05);
    }
    updateNeedleTarget();
    update();
    scheduleAccessibleValue();
}

void SMeterWidget::setTxMode(const QString& mode)
{
    if (mode == "Power")            m_txMode = TxMode::Power;
    else if (mode == "SWR")         m_txMode = TxMode::SWR;
    else if (mode == "Level")       m_txMode = TxMode::Level;
    else if (mode == "Compression") m_txMode = TxMode::Compression;
    setProperty("txMode", mode);
    updateNeedleTarget();
    update();
    scheduleAccessibleValue();
}

void SMeterWidget::setRxMode(const QString& mode)
{
    if (mode == "S-Meter") {
        m_rxMode = RxMode::SMeter;
        m_source = "S-Meter";
    } else {
        m_rxMode = RxMode::SMeterPeak;
        m_source = "S-Meter Peak";
    }
    updateNeedleTarget();
    update();
    scheduleAccessibleValue();
}

void SMeterWidget::updateNeedleTarget()
{
    updatePeakHoldValue();

    if (m_transmitting) {
        m_targetNeedleFraction = txValueToFraction(currentTxValue());
    } else if (usesUnavailableRxMeter()) {
        m_targetNeedleFraction = 0.0f;
    } else if (m_rxMode == RxMode::SMeterPeak) {
        m_targetNeedleFraction = dbmToFraction(m_peakDbm);
    } else {
        m_targetNeedleFraction = dbmToFraction(m_levelDbm);
    }

    const bool needleAtTarget = qAbs(m_targetNeedleFraction - m_needleFraction) <= kNeedleSnapEpsilon;
    if (needleAtTarget) {
        m_needleFraction = m_targetNeedleFraction;
    }

    const bool peakHoldAnimating = m_peakHoldEnabled
        && m_peakHoldTimerRunning
        && m_peakHoldTimer.elapsed() > m_peakHoldTimeMs
        && m_peakHoldDbm > m_levelDbm + 0.01f;

    if (needleAtTarget && !peakHoldAnimating) {
        if (m_needleAnimation.isActive()) {
            m_needleAnimation.stop();
        }
        return;
    }

    if (!m_needleAnimation.isActive()) {
        m_needleElapsed.restart();
        m_needleAnimation.start();
    }
}

void SMeterWidget::animateNeedle()
{
    const qint64 elapsedMs = m_needleElapsed.restart();
    if (elapsedMs <= 0) {
        return;
    }

    updatePeakHoldValue();

    const float delta = m_targetNeedleFraction - m_needleFraction;
    const float elapsedSeconds = static_cast<float>(elapsedMs) / 1000.0f;
    const float timeConstant = (delta >= 0.0f) ? kNeedleAttackTimeSeconds
                                                : kNeedleReleaseTimeSeconds;
    const float alpha = 1.0f - std::exp(-elapsedSeconds / timeConstant);
    const bool needleAtTarget = qAbs(delta) <= kNeedleSnapEpsilon;
    if (needleAtTarget) {
        m_needleFraction = m_targetNeedleFraction;
    } else {
        m_needleFraction += delta * alpha;
    }

    const bool peakHoldAnimating = m_peakHoldEnabled
        && m_peakHoldTimerRunning
        && m_peakHoldTimer.elapsed() > m_peakHoldTimeMs
        && m_peakHoldDbm > m_levelDbm + 0.01f;

    const bool settled = needleAtTarget && !peakHoldAnimating;
    if (settled) {
        m_needleAnimation.stop();
    }

    update();
}

bool SMeterWidget::usesUnavailableRxMeter() const
{
    return m_receiveMeterReadingActive
        && !(m_receiveMeterReading.valid
            && (m_receiveMeterReading.capability
                    == KiwiSdrProtocol::MeterCapability::CalibratedSndMeter
                || m_receiveMeterReading.capability
                    == KiwiSdrProtocol::MeterCapability::Experimental)
            && m_receiveMeterReading.hasDbm);
}

QString SMeterWidget::unavailableRxMeterLabel() const
{
    if (!m_receiveMeterReadingActive) {
        return QStringLiteral("Meter unavailable");
    }
    if (m_receiveMeterReading.capability
        == KiwiSdrProtocol::MeterCapability::RawSndMeter) {
        return QStringLiteral("Raw S-meter");
    }
    return QStringLiteral("Meter unavailable");
}

void SMeterWidget::scheduleAccessibleValue()
{
    // Meter streams can update tens of times per second. Publish at most one
    // current value per interval instead of turning a focused screen reader
    // into a sample-by-sample audio stream. The timeout always reads current
    // widget state, so a burst collapses to its latest RX/TX value or mode.
    if (hasFocus() && QAccessible::isActive() && !m_accessibilityTimer.isActive()) {
        m_accessibilityTimer.start();
    }
}

void SMeterWidget::publishAccessibleValue()
{
    if (!hasFocus() || !QAccessible::isActive()) {
        return;
    }

    const QString value = accessibleValueText();
    if (value == m_lastAccessibleValue) {
        return;
    }
    m_lastAccessibleValue = value;
    QAccessibleValueChangeEvent event(this, value);
    QAccessible::updateAccessibility(&event);
}

void SMeterWidget::updatePeakHoldValue()
{
    if (!m_peakHoldEnabled || !m_peakHoldTimerRunning) {
        return;
    }

    const qint64 elapsedMs = m_peakHoldTimer.elapsed();
    if (elapsedMs <= m_peakHoldTimeMs) {
        return;
    }

    const float decayElapsedSeconds =
        static_cast<float>(elapsedMs - m_peakHoldTimeMs) / 1000.0f;
    m_peakHoldDbm = m_peakHoldDecayStartDbm - (m_peakDecayDbPerSec * decayElapsedSeconds);
    if (m_peakHoldDbm <= m_levelDbm) {
        m_peakHoldDbm = m_levelDbm;
    }
}

QString SMeterWidget::sUnitsText() const
{
    return sUnitsTextFor(m_levelDbm);
}

QString SMeterWidget::sUnitsTextFor(float dbm) const
{
    const SMeterGeometry::RxScale& scale = m_geometry.rxScale;
    if (dbm <= scale.minimumDbm) {
        return QStringLiteral("S0");
    }
    if (dbm <= scale.s9Dbm) {
        const int s = qRound((dbm - scale.minimumDbm) / scale.dbPerSUnit);
        return QStringLiteral("S%1").arg(qBound(0, s, 9));
    }
    return QStringLiteral("S9+%1").arg(qRound(dbm - scale.s9Dbm));
}

// --- Mapping -----------------------------------------------------------------

float SMeterWidget::dbmToFraction(float dbm) const
{
    return static_cast<float>(m_geometry.rxFraction(dbm));
}

float SMeterWidget::txValueToFraction(float value) const
{
    switch (m_txMode) {
    case TxMode::Power:
        return qBound(0.0f, value / m_powerScaleMax, 1.0f);
    case TxMode::SWR:
        return static_cast<float>(m_geometry.scaleFraction(m_geometry.swrScale, value));
    case TxMode::Level:
        return static_cast<float>(m_geometry.scaleFraction(m_geometry.levelScale, value));
    case TxMode::Compression:
        return static_cast<float>(m_geometry.scaleFraction(m_geometry.compressionScale, value));
    }
    return 0.0f;
}

float SMeterWidget::currentTxValue() const
{
    switch (m_txMode) {
    case TxMode::Power:       return m_txPower;
    case TxMode::SWR:         return m_txSwr;
    case TxMode::Level:       return m_micLevel;
    case TxMode::Compression: return m_compLevel;
    }
    return 0.0f;
}

// --- Paint -------------------------------------------------------------------

void SMeterWidget::rebuildBackgroundLayer()
{
    const qreal dpr = devicePixelRatioF();
    const QSize pixelSize(qMax(1, qRound(width() * dpr)),
                          qMax(1, qRound(height() * dpr)));
    m_backgroundLayer = QPixmap(pixelSize);
    m_backgroundLayer.setDevicePixelRatio(dpr);
    m_backgroundLayer.fill(Qt::transparent);

    QPainter painter(&m_backgroundLayer);
    painter.setRenderHint(QPainter::Antialiasing);
    if (m_faceTheme == AnalogMeterFaceTheme::AetherDefault) {
        painter.fillRect(QRectF(QPointF(0.0, 0.0), QSizeF(size())),
                         QColor(0x0f, 0x0f, 0x1a));
    } else {
        m_faceThemes.drawBackground(
            painter, QRectF(QPointF(0.0, 0.0), QSizeF(size())), m_faceTheme);
    }

    m_backgroundCacheSize = size();
    m_backgroundCacheDpr = dpr;
    m_backgroundCacheTheme = m_faceTheme;
    m_backgroundCacheValid = true;
}

void SMeterWidget::drawPhysicalNeedle(
    QPainter& painter, const QPointF& pivot, const QPointF& tip,
    const QSizeF& faceSize,
    const AnalogMeterFaceThemeCatalog::Palette& palette) const
{
    const QPointF movement = tip - pivot;
    const double length = std::hypot(movement.x(), movement.y());
    if (!(length > 0.0)) {
        return;
    }

    const QPointF direction = movement / length;
    QPointF normal(-direction.y(), direction.x());
    if (QPointF::dotProduct(normal, QPointF(-1.0, -1.0)) < 0.0) {
        normal = -normal;
    }
    const double materialScale = std::max(
        0.65, std::min(faceSize.width() / 280.0, faceSize.height() / 140.0));
    const double bodyWidth = m_geometry.needle.lineWidthPixels * materialScale;
    const double taperLength = 5.0 * materialScale;
    const QPointF taperBase = tip - direction * taperLength;
    const QPointF contactOffset = m_geometry.needle.shadowOffset * materialScale;
    const QPointF softOffset = QPointF(2.2, 2.8) * materialScale;

    painter.setPen(QPen(palette.needleSoftShadow, bodyWidth * 3.0,
                        Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(pivot + softOffset, tip + softOffset);
    painter.setPen(QPen(palette.needleShadow, bodyWidth * 1.8,
                        Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(pivot + contactOffset, tip + contactOffset);

    painter.setPen(QPen(palette.needle, bodyWidth,
                        Qt::SolidLine, Qt::FlatCap));
    painter.drawLine(pivot, taperBase);
    QPainterPath taperedTip;
    taperedTip.moveTo(tip);
    taperedTip.lineTo(taperBase + normal * bodyWidth * 0.5);
    taperedTip.lineTo(taperBase - normal * bodyWidth * 0.5);
    taperedTip.closeSubpath();
    painter.setPen(Qt::NoPen);
    painter.setBrush(palette.needle);
    painter.drawPath(taperedTip);

    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(palette.needleEdge, qMax(0.55, bodyWidth * 0.34),
                        Qt::SolidLine, Qt::FlatCap));
    painter.drawLine(pivot - normal * bodyWidth * 0.34,
                     taperBase - normal * bodyWidth * 0.34);
    painter.setPen(QPen(palette.needleHighlight, qMax(0.45, bodyWidth * 0.27),
                        Qt::SolidLine, Qt::FlatCap));
    painter.drawLine(pivot + normal * bodyWidth * 0.27,
                     taperBase + normal * bodyWidth * 0.27);
}

void SMeterWidget::drawPhysicalMask(
    QPainter& painter, const QRectF& face,
    const AnalogMeterFaceThemeCatalog::Palette& palette) const
{
    painter.setPen(Qt::NoPen);
    painter.setBrush(palette.maskFill);
    painter.drawPath(m_faceThemes.lowerMaskPath(face));

    const QVector<QPointF> boundary = m_faceThemes.lowerMaskBoundary(face);
    const double scale = std::max(
        0.75, std::min(face.width() / 280.0, face.height() / 140.0));
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(palette.maskEdge,
                        m_geometry.pivot.rimWidthPixels * scale,
                        Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawPolyline(QPolygonF(boundary));
}

void SMeterWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const int w = width();
    const int h = height();

    const qreal dpr = devicePixelRatioF();
    if (!m_backgroundCacheValid || m_backgroundCacheSize != size()
        || !qFuzzyCompare(m_backgroundCacheDpr, dpr)
        || m_backgroundCacheTheme != m_faceTheme) {
        rebuildBackgroundLayer();
    }
    p.drawPixmap(0, 0, m_backgroundLayer);

    const bool physicalTheme = m_faceTheme != AnalogMeterFaceTheme::AetherDefault;
    const AnalogMeterFaceThemeCatalog::Palette* physicalPalette =
        physicalTheme ? &m_faceThemes.palette(m_faceTheme) : nullptr;
    const QColor rxNormal = physicalTheme ? physicalPalette->text
                                          : QColor(0xc8, 0xd8, 0xe8);
    const QColor warning = physicalTheme ? physicalPalette->scaleCalibration
                                         : QColor(0xff, 0x44, 0x44);
    const QColor txNormal = physicalTheme ? physicalPalette->scaleInner
                                          : QColor(0x00, 0x80, 0xd0);
    const QColor tickNormal = physicalTheme ? physicalPalette->majorTick
                                            : QColor(0xc8, 0xd8, 0xe8);

    // -- Arc geometry ---------------------------------------------------------
    // Large radius with center far below widget -> shallow ~70 deg arc segment.
    const SMeterGeometry::Layout layout = m_geometry.layoutFor(QSizeF(w, h));
    const QRectF face = layout.viewport;
    const double faceLeft = face.left();
    const double faceRight = face.right();
    const double faceTop = face.top();
    const double faceBottom = face.bottom();
    const double faceCenterX = face.center().x();
    p.setClipRect(face);
    const float cx = static_cast<float>(layout.centerX);
    const float radius = static_cast<float>(layout.radius);
    const float cy = static_cast<float>(layout.centerY);
    const float needleCy = static_cast<float>(layout.needlePivotY);

    // QPainter arc endpoints remain in degrees; moving elements use radians.
    const float arcStartDeg = static_cast<float>(m_geometry.arc.startDegrees);
    const float arcEndDeg = static_cast<float>(m_geometry.arc.endDegrees);
    const bool unavailableRxScale = usesUnavailableRxMeter();
    const bool calibratedRxScale = !unavailableRxScale;

    // fraction 0.0 -> left end, fraction 1.0 -> right end
    auto fractionToAngle = [&](float frac) -> float {
        return static_cast<float>(m_geometry.fractionToRadians(frac));
    };

    // -- Draw colored outer arc (RX scale) ------------------------------------
    {
        const QRectF outerArc(cx - radius, cy - radius, radius * 2, radius * 2);
        // RX face always uses the existing Flex-style S scale. Uncalibrated
        // Kiwi fallback readings do not move the needle or show fake dBm.
        const float s9Deg = qRadiansToDegrees(
            fractionToAngle(static_cast<float>(m_geometry.rxScale.s9Fraction)));

        QPen whitePen(rxNormal, m_geometry.arc.lineWidthPixels);
        p.setPen(whitePen);
        p.drawArc(outerArc,
                  static_cast<int>(s9Deg * 16),
                  static_cast<int>((arcEndDeg - s9Deg) * 16));

        QPen redPen(warning, m_geometry.arc.lineWidthPixels);
        p.setPen(redPen);
        p.drawArc(outerArc,
                  static_cast<int>(arcStartDeg * 16),
                  static_cast<int>((s9Deg - arcStartDeg) * 16));
    }

    // -- Draw colored inner arc (TX scale) -- 6px gap -------------------------
    const float arcGap = static_cast<float>(m_geometry.arc.innerGapPixels);
    const QColor blueColor = txNormal;
    const QColor redColor = warning;
    {
        const float innerR = radius - arcGap;
        const QRectF innerArc(cx - innerR, cy - innerR, innerR * 2, innerR * 2);

        // Determine where the arc color splits (fraction where red begins)
        float redFrac = -1.0f;  // -1 = no red zone
        switch (m_txMode) {
        case TxMode::Power:       redFrac = m_powerRedStart / m_powerScaleMax; break;
        case TxMode::SWR:
            redFrac = static_cast<float>(m_geometry.scaleFraction(
                m_geometry.swrScale, m_geometry.swrScale.warningStart));
            break;
        case TxMode::Level:
            redFrac = static_cast<float>(m_geometry.scaleFraction(
                m_geometry.levelScale, m_geometry.levelScale.warningStart));
            break;
        case TxMode::Compression: redFrac = -1.0f; break; // all blue
        }

        if (redFrac < 0.0f) {
            // Entire arc is blue
            p.setPen(QPen(blueColor, m_geometry.arc.lineWidthPixels));
            p.drawArc(innerArc,
                      static_cast<int>(arcStartDeg * 16),
                      static_cast<int>((arcEndDeg - arcStartDeg) * 16));
        } else {
            const float splitDeg = qRadiansToDegrees(fractionToAngle(redFrac));
            p.setPen(QPen(blueColor, m_geometry.arc.lineWidthPixels));
            p.drawArc(innerArc,
                      static_cast<int>(splitDeg * 16),
                      static_cast<int>((arcEndDeg - splitDeg) * 16));
            p.setPen(QPen(redColor, m_geometry.arc.lineWidthPixels));
            p.drawArc(innerArc,
                      static_cast<int>(arcStartDeg * 16),
                      static_cast<int>((splitDeg - arcStartDeg) * 16));
        }
    }

    // -- Tick drawing helpers -------------------------------------------------
    QFont tickFont = font();
    tickFont.setPixelSize(layout.tickFontPixels);
    tickFont.setBold(m_geometry.tickStyle.bold);
    p.setFont(tickFont);
    const QFontMetrics tfm(tickFont);

    // Outside tick (RX): extends outward from the arc, label above
    auto drawOutsideTick = [&](float frac, const QString& label, const QColor& color,
                               bool showLabel) {
        const SMeterGeometry::MovementRay ray =
            m_geometry.movementRayFor(QSizeF(w, h), frac);
        const double arcX = ray.scalePoint.x();
        const double arcY = ray.scalePoint.y();
        const double ux = ray.direction.x();
        const double uy = ray.direction.y();

        const QPointF inner(arcX + m_geometry.tickStyle.startOffsetPixels * ux,
                            arcY + m_geometry.tickStyle.startOffsetPixels * uy);
        const QPointF outer(arcX + m_geometry.tickStyle.endOffsetPixels * ux,
                            arcY + m_geometry.tickStyle.endOffsetPixels * uy);

        p.setPen(QPen(color, m_geometry.tickStyle.lineWidthPixels));
        p.drawLine(inner, outer);

        if (showLabel) {
            const QPointF labelPt(arcX + m_geometry.tickStyle.labelOffsetPixels * ux,
                                   arcY + m_geometry.tickStyle.labelOffsetPixels * uy);
            const int tw = tfm.horizontalAdvance(label);
            p.setPen(color);
            p.drawText(QPointF(labelPt.x() - tw / 2.0,
                               labelPt.y() + tfm.ascent() / 2.0), label);
        }
    };

    // Inside tick (TX): extends inward from the inner colored arc
    const float innerArcR = radius - arcGap;
    auto drawInsideTick = [&](float frac, const QString& label,
                              const QColor& tickColor, const QColor& labelColor,
                              bool showLabel) {
        const SMeterGeometry::MovementRay ray =
            m_geometry.movementRayFor(QSizeF(w, h), frac);
        const std::optional<QPointF> innerArcPoint =
            m_geometry.movementRayCircleIntersection(QSizeF(w, h), frac, innerArcR);
        if (!innerArcPoint) {
            return;
        }
        const double iArcX = innerArcPoint->x();
        const double iArcY = innerArcPoint->y();
        const double ux = ray.direction.x();
        const double uy = ray.direction.y();

        const QPointF outer(iArcX - m_geometry.tickStyle.startOffsetPixels * ux,
                            iArcY - m_geometry.tickStyle.startOffsetPixels * uy);
        const QPointF inner(iArcX - m_geometry.tickStyle.endOffsetPixels * ux,
                            iArcY - m_geometry.tickStyle.endOffsetPixels * uy);

        p.setPen(QPen(tickColor, m_geometry.tickStyle.lineWidthPixels));
        p.drawLine(inner, outer);

        if (showLabel) {
            const QPointF labelPt(iArcX - m_geometry.tickStyle.labelOffsetPixels * ux,
                                   iArcY - m_geometry.tickStyle.labelOffsetPixels * uy);
            const int tw = tfm.horizontalAdvance(label);
            p.setPen(labelColor);
            p.drawText(QPointF(labelPt.x() - tw / 2.0,
                               labelPt.y() + tfm.ascent() / 2.0), label);
        }
    };

    const QColor whiteColor = tickNormal;
    const QColor labelColor = physicalTheme ? physicalPalette->text : whiteColor;

    // -- Outside ticks (RX) ---------------------------------------------------
    for (const SMeterGeometry::Tick& tick : m_geometry.rxScale.ticks) {
        const QColor& color = (tick.value > m_geometry.rxScale.s9Dbm) ? redColor : whiteColor;
        drawOutsideTick(dbmToFraction(static_cast<float>(tick.value)), tick.label, color, true);
    }

    // -- Inside ticks (TX): scale depends on TX mode --------------------------
    switch (m_txMode) {
    case TxMode::Power: {
        // Dynamic scale based on m_powerScaleMax
        int maxW = static_cast<int>(m_powerScaleMax);
        int redW = static_cast<int>(m_powerRedStart);
        const SMeterGeometry::PowerTickPolicy& policy =
            m_geometry.powerTickPolicy(m_powerScaleMax);
        const int tickStep = policy.tickStepWatts;
        const int labelStep = policy.labelStepWatts;
        for (int w = 0; w <= maxW; w += tickStep) {
            const float frac = static_cast<float>(w) / m_powerScaleMax;
            const QColor& tc = (w >= redW) ? redColor : blueColor;
            const QColor& lc = (w >= redW) ? redColor : labelColor;
            bool isLabeled = (w % labelStep == 0) || w == maxW || w == redW;
            QString label = (w >= 1000) ? QString("%1k").arg(w / 1000.0f, 0, 'f', (w % 1000) ? 1 : 0)
                                        : QString::number(w);
            drawInsideTick(frac, label, tc, lc, isLabeled);
        }
        break;
    }
    case TxMode::SWR: {
        for (const SMeterGeometry::Tick& tick : m_geometry.swrScale.ticks) {
            const float frac = static_cast<float>(
                m_geometry.scaleFraction(m_geometry.swrScale, tick.value));
            const bool red = m_geometry.swrScale.hasWarning
                && tick.value >= m_geometry.swrScale.warningStart;
            const QColor& tc = red ? redColor : blueColor;
            const QColor& lc = red ? redColor : labelColor;
            drawInsideTick(frac, tick.label, tc, lc, true);
        }
        break;
    }
    case TxMode::Level: {
        for (const SMeterGeometry::Tick& tick : m_geometry.levelScale.ticks) {
            const float frac = static_cast<float>(
                m_geometry.scaleFraction(m_geometry.levelScale, tick.value));
            const bool red = m_geometry.levelScale.hasWarning
                && tick.value >= m_geometry.levelScale.warningStart;
            const QColor& tc = red ? redColor : blueColor;
            const QColor& lc = red ? redColor : labelColor;
            drawInsideTick(frac, tick.label, tc, lc, true);
        }
        break;
    }
    case TxMode::Compression: {
        for (const SMeterGeometry::Tick& tick : m_geometry.compressionScale.ticks) {
            const float frac = static_cast<float>(
                m_geometry.scaleFraction(m_geometry.compressionScale, tick.value));
            drawInsideTick(frac, tick.label, blueColor, labelColor, true);
        }
        break;
    }
    }

    // Pivot cover radius — shared by the backlight glow and the cover itself.
    const float pivotCoverR = static_cast<float>(layout.pivotRadius);

    // -- Pivot backlight glow -------------------------------------------------
    // A warm radial glow behind the pivot, as if a lamp sits behind the mask.
    // Drawn before the needle/cover; the cover masks the bright centre, leaving
    // a soft halo spilling out from behind the moulding.
    if (!physicalTheme) {
        const float glowR = pivotCoverR * static_cast<float>(m_geometry.pivot.glowRadiusFactor);
        const float edge = pivotCoverR / glowR;
        const float mid = edge + (1.0f - edge)
            * static_cast<float>(m_geometry.pivot.glowMiddleFactor);
        QColor glowColor =
            ThemeManager::instance().color(QStringLiteral("color.meter.pivot.glow"));
        auto glowAlpha = [&glowColor](int a) {
            QColor c = glowColor;
            c.setAlpha(a);
            return c;
        };
        QRadialGradient glow(QPointF(cx, faceBottom), glowR,
                             QPointF(cx, faceBottom));
        glow.setColorAt(0.0f, glowAlpha(m_geometry.pivot.glowCenterAlpha));
        glow.setColorAt(edge, glowAlpha(m_geometry.pivot.glowCenterAlpha));
        glow.setColorAt(mid, glowAlpha(m_geometry.pivot.glowMiddleAlpha));
        glow.setColorAt(1.0f,  glowAlpha(0));
        p.setPen(Qt::NoPen);
        p.setBrush(glow);
        p.drawChord(QRectF(cx - glowR, faceBottom - glowR,
                          glowR * 2.0f, glowR * 2.0f),
                    0, 180 * 16);
    }

    // -- Draw needle ----------------------------------------------------------
    // Needle originates from needleCy (just below widget) rather than the
    // arc center, so the pivot is barely out of frame.
    // When transmitting, needle tracks the selected TX meter instead of RX.
    {
        const QPointF tip = m_geometry.needleTip(QSizeF(w, h), m_needleFraction);
        const QPointF pivot(cx, needleCy);
        if (physicalTheme) {
            drawPhysicalNeedle(p, pivot, tip, face.size(), *physicalPalette);
        } else {
            // Preserve the established Aether-default needle exactly.
            p.setPen(QPen(QColor(0, 0, 0, 80), m_geometry.needle.shadowWidthPixels));
            p.drawLine(pivot + m_geometry.needle.shadowOffset,
                       tip + m_geometry.needle.shadowOffset);
            p.setPen(QPen(QColor(0xff, 0xff, 0xff), m_geometry.needle.lineWidthPixels));
            p.drawLine(pivot, tip);
        }
    }

    // -- Needle pivot cover ---------------------------------------------------
    // A filled half-disc at the bottom-centre hides where the needle pivots,
    // like the moulded bump on a classic analog VU meter. Drawn after the
    // needle so it masks the base; the needle appears to emerge from under it.
    if (physicalTheme) {
        drawPhysicalMask(p, face, *physicalPalette);
    } else {
        const float coverR = pivotCoverR;
        const QRectF coverRect(cx - coverR, faceBottom - coverR,
                               coverR * 2.0f, coverR * 2.0f);
        p.setPen(Qt::NoPen);
        p.setBrush(ThemeManager::instance().color(
            QStringLiteral("color.meter.pivot.fill")));   // moulding
        p.drawChord(coverRect, 0, 180 * 16);              // upper half-disc (flat edge at bottom)
        // Subtle glossy rim along the curved top edge.
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(ThemeManager::instance().color(
            QStringLiteral("color.meter.pivot.rim")), m_geometry.pivot.rimWidthPixels));
        p.drawArc(coverRect, 0, 180 * 16);
    }

    // Draw peak marker (small triangle) — only in RX S-Meter Peak mode
    if (!m_transmitting && calibratedRxScale && m_rxMode == RxMode::SMeterPeak
        && m_peakDbm > m_levelDbm + m_geometry.peakMarker.minimumLeadDb) {
        const float frac = dbmToFraction(m_peakDbm);
        const float markerR = radius - static_cast<float>(m_geometry.peakMarker.radiusInsetPixels);
        const SMeterGeometry::MovementRay ray =
            m_geometry.movementRayFor(QSizeF(w, h), frac);
        const std::optional<QPointF> markerPoint =
            m_geometry.movementRayCircleIntersection(QSizeF(w, h), frac, markerR);
        if (markerPoint) {
            p.setPen(Qt::NoPen);
            p.setBrush(physicalTheme ? physicalPalette->scaleCalibration
                                     : QColor(0xff, 0xaa, 0x00));
            const QPointF perpendicular(-ray.direction.y(), ray.direction.x());
            const float halfWidth = static_cast<float>(m_geometry.peakMarker.halfWidthPixels);
            const float markerLength = static_cast<float>(m_geometry.peakMarker.lengthPixels);
            QPainterPath tri;
            tri.moveTo(*markerPoint);
            tri.lineTo(*markerPoint - markerLength * ray.direction + halfWidth * perpendicular);
            tri.lineTo(*markerPoint - markerLength * ray.direction - halfWidth * perpendicular);
            tri.closeSubpath();
            p.drawPath(tri);
        }
    }

    // -- Draw peak hold line (configurable overlay, independent of RX mode) ---
    if (m_peakHoldEnabled && !m_transmitting && calibratedRxScale
        && m_peakHoldDbm > m_geometry.rxScale.minimumDbm
            + m_geometry.peakHold.visibleAboveMinimumDb) {
        float frac = dbmToFraction(m_peakHoldDbm);
        if (m_peakHoldDbm <= m_levelDbm + 0.01f) {
            frac = m_needleFraction;
        } else {
            frac = qMax(frac, m_needleFraction);
        }
        const float peakInnerRadius =
            radius + static_cast<float>(m_geometry.peakHold.innerRadiusOffsetPixels);
        const float peakOuterRadius =
            radius + static_cast<float>(m_geometry.peakHold.outerRadiusOffsetPixels);
        const std::optional<QPointF> inner =
            m_geometry.movementRayCircleIntersection(QSizeF(w, h), frac, peakInnerRadius);
        const std::optional<QPointF> outer =
            m_geometry.movementRayCircleIntersection(QSizeF(w, h), frac, peakOuterRadius);
        if (inner && outer) {
            QColor peakColor = physicalTheme ? physicalPalette->scaleCalibration
                                             : QColor(0xff, 0x44, 0x44, 0xcc);
            if (physicalTheme) {
                peakColor.setAlpha(0xcc);
            }
            p.setPen(QPen(peakColor,
                          m_geometry.peakHold.lineWidthPixels));
            p.drawLine(*inner, *outer);
        }
    }

    // -- Text readout -- aligned top edges with graph clearance ----------------
    QFont srcFont = font();
    srcFont.setPixelSize(layout.sourceFontPixels);
    const QFontMetrics sfm(srcFont);

    QFont valFont = font();
    valFont.setPixelSize(layout.valueFontPixels);
    valFont.setBold(true);
    const QFontMetrics vfm(valFont);
    // Keep the larger side values clear of the top edge, then align the
    // smaller centered label by its font-box top. Center alignment still left
    // the source label too close to the calibrated arc numbers below it.
    const int valueBaseline = qRound(faceTop) + std::max(sfm.ascent(), vfm.ascent())
        + m_geometry.readout.topExtraPixels;
    const int sourceBaseline =
        valueBaseline - vfm.ascent() + sfm.ascent();
    const QColor sourceColor = physicalTheme ? physicalPalette->secondaryText
                                             : QColor(0x80, 0x90, 0xa0);
    const QColor accentColor = physicalTheme ? physicalPalette->text
                                             : QColor(0x00, 0xb4, 0xd8);
    const QColor valueColor = physicalTheme ? physicalPalette->text
                                            : QColor(0xc8, 0xd8, 0xe8);

    if (m_transmitting) {
        // TX mode: show TX source label (center), mode name (left), value (right)
        static const char* txLabels[] = {"Power", "SWR", "Level", "Compression"};
        const QString srcLabel = txLabels[static_cast<int>(m_txMode)];
        p.setFont(srcFont);
        p.setPen(sourceColor);
        p.drawText(qRound(faceCenterX - sfm.horizontalAdvance(srcLabel) / 2.0),
                   sourceBaseline, srcLabel);

        p.setFont(valFont);
        // Left: mode name in cyan
        p.setPen(accentColor);
        p.drawText(qRound(faceLeft) + m_geometry.readout.sideMarginPixels,
                   valueBaseline, "TX");

        // Right: formatted value
        QString valText;
        switch (m_txMode) {
        case TxMode::Power:       valText = QString("%1 W").arg(m_txPower, 0, 'f', 0); break;
        case TxMode::SWR:         valText = QString("%1").arg(m_txSwr, 0, 'f', 1); break;
        case TxMode::Level:       valText = QString("%1 dB").arg(m_micLevel, 0, 'f', 0); break;
        case TxMode::Compression: valText = QString("%1 dB").arg(-m_compLevel, 0, 'f', 0); break;
        }
        p.setPen(valueColor);
        p.drawText(qRound(faceRight) - vfm.horizontalAdvance(valText)
                       - m_geometry.readout.sideMarginPixels,
                   valueBaseline, valText);
    } else {
        // RX mode: show source label (center), S-units (left), dBm (right)
        if (unavailableRxScale) {
            const QString sourceLabel = unavailableRxMeterLabel();
            p.setFont(srcFont);
            p.setPen(sourceColor);
            p.drawText(qRound(faceCenterX - sfm.horizontalAdvance(sourceLabel) / 2.0),
                       sourceBaseline, sourceLabel);

            p.setFont(valFont);
            p.setPen(accentColor);
            p.drawText(qRound(faceLeft) + m_geometry.readout.sideMarginPixels,
                       valueBaseline,
                       QStringLiteral("---"));

            const QString rightText = QStringLiteral("---");
            p.setPen(valueColor);
            p.drawText(qRound(faceRight) - vfm.horizontalAdvance(rightText)
                           - m_geometry.readout.sideMarginPixels,
                       valueBaseline, rightText);
            return;
        }

        p.setFont(srcFont);
        p.setPen(sourceColor);
        p.drawText(qRound(faceCenterX - sfm.horizontalAdvance(m_source) / 2.0),
                   sourceBaseline, m_source);

        const float displayDbm = (m_rxMode == RxMode::SMeterPeak) ? m_peakDbm : m_levelDbm;

        p.setFont(valFont);
        p.setPen(accentColor);
        // Show S-units based on the displayed value
        const QString sText = sUnitsTextFor(displayDbm);
        p.drawText(qRound(faceLeft) + m_geometry.readout.sideMarginPixels,
                   valueBaseline, sText);

        const QString dbmText = QString("%1 dBm").arg(displayDbm, 0, 'f', 0);
        p.setPen(valueColor);
        p.drawText(qRound(faceRight) - vfm.horizontalAdvance(dbmText)
                       - m_geometry.readout.sideMarginPixels,
                   valueBaseline, dbmText);
    }
}

void SMeterWidget::setPowerScale(int maxWatts, bool hasAmplifier)
{
    if (hasAmplifier) {
        m_powerScaleMax = 2000.0f;
        m_powerRedStart = 1500.0f;
    } else if (maxWatts > 100) {
        m_powerScaleMax = 600.0f;
        m_powerRedStart = 500.0f;
    } else {
        m_powerScaleMax = 120.0f;
        m_powerRedStart = 100.0f;
    }
    updateNeedleTarget();
    update();
}

// --- Peak hold configuration -------------------------------------------------

void SMeterWidget::setPeakHoldEnabled(bool enabled)
{
    m_peakHoldEnabled = enabled;
    m_peakHoldDbm = m_levelDbm;
    m_peakHoldDecayStartDbm = m_levelDbm;
    m_peakHoldTimerRunning = false;
    updateNeedleTarget();
    update();
}

void SMeterWidget::setPeakHoldTimeMs(int ms)
{
    m_peakHoldTimeMs = qBound(100, ms, 2000);
}

void SMeterWidget::setPeakDecayRate(DecayRate rate)
{
    switch (rate) {
    case DecayRate::Fast:   m_peakDecayDbPerSec = 20.0f; break;
    case DecayRate::Medium: m_peakDecayDbPerSec = 10.0f; break;
    case DecayRate::Slow:   m_peakDecayDbPerSec = 5.0f;  break;
    }
}

void SMeterWidget::setPeakDecayRate(const QString& rate)
{
    if (rate == "Fast")        setPeakDecayRate(DecayRate::Fast);
    else if (rate == "Slow")   setPeakDecayRate(DecayRate::Slow);
    else                       setPeakDecayRate(DecayRate::Medium);
}

void SMeterWidget::resetPeak()
{
    m_peakHoldDbm = m_levelDbm;
    m_peakHoldDecayStartDbm = m_levelDbm;
    m_peakHoldTimerRunning = false;
    updateNeedleTarget();
    update();
}

} // namespace AetherSDR
