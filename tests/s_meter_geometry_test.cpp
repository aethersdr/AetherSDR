#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "gui/AnalogMeterFaceTheme.h"
#include "gui/RadioSwrValidityFilter.h"
#include "gui/SMeterGeometry.h"
#include "gui/SMeterWidget.h"

#include <QApplication>
#include <QAccessible>
#include <QBuffer>
#include <QCryptographicHash>
#include <QEventLoop>
#include <QFile>
#include <QFont>
#include <QFontMetrics>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QtMath>

#include <cmath>
#include <iostream>
#include <optional>

namespace {

int g_failures = 0;

struct AccessibleValueUpdate {
    QObject* object{nullptr};
    QString value;
};

QVector<AccessibleValueUpdate>* g_accessibleValueUpdates = nullptr;

void captureAccessibleValueUpdate(QAccessibleEvent* event)
{
    if (!g_accessibleValueUpdates || event->type() != QAccessible::ValueChanged) {
        return;
    }

    const QAccessibleValueChangeEvent* valueEvent =
        static_cast<const QAccessibleValueChangeEvent*>(event);
    g_accessibleValueUpdates->push_back({event->object(), valueEvent->value().toString()});
}

void expect(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++g_failures;
    }
}

void expect(bool condition, const QString& message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message.toStdString() << '\n';
        ++g_failures;
    }
}

bool near(double actual, double expected, double tolerance = 1e-9)
{
    return std::abs(actual - expected) <= tolerance;
}

void waitForEvents(int milliseconds)
{
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec(QEventLoop::ExcludeUserInputEvents);
}

void expectSingleAccessibleValue(
    const QVector<AccessibleValueUpdate>& updates,
    const AetherSDR::SMeterWidget& meter,
    const QStringList& expectedFragments,
    const QString& context)
{
    expect(updates.size() == 1,
           context + QStringLiteral(" emits exactly one settled value update"));
    if (updates.size() != 1) {
        return;
    }

    expect(updates.constFirst().object == &meter,
           context + QStringLiteral(" targets the S-meter widget"));
    for (const QString& fragment : expectedFragments) {
        expect(updates.constFirst().value.contains(fragment),
               context + QStringLiteral(" contains '%1'").arg(fragment));
    }
}

void testAccessibilityAnnouncements()
{
    QVector<AccessibleValueUpdate> updates;
    g_accessibleValueUpdates = &updates;
    const QAccessible::UpdateHandler previousHandler =
        QAccessible::installUpdateHandler(captureAccessibleValueUpdate);
    const bool wasAccessible = QAccessible::isActive();
    QAccessible::setActive(true);

    AetherSDR::SMeterWidget meter;
    meter.show();
    meter.activateWindow();
    meter.setFocus(Qt::OtherFocusReason);
    QApplication::processEvents();
    expect(meter.hasFocus(), "accessibility announcement fixture receives focus");

    updates.clear();
    meter.setLevel(-110.0f);
    meter.setLevel(-90.0f);
    meter.setLevel(-61.0f);
    waitForEvents(45);
    expect(updates.isEmpty(),
           "RX burst waits for the throttled accessibility interval");
    waitForEvents(90);
    expectSingleAccessibleValue(
        updates, meter,
        {QStringLiteral("S9+12"), QStringLiteral("-61 dBm")},
        QStringLiteral("RX burst"));

    updates.clear();
    meter.setLevel(-61.0f);
    waitForEvents(135);
    expect(updates.isEmpty(),
           "unchanged RX display value is not announced repeatedly");

    updates.clear();
    meter.setTransmitting(true);
    meter.setTxMode(QStringLiteral("Power"));
    meter.setTxMeters(15.0f, 1.2f);
    meter.setTxMeters(65.0f, 1.7f);
    waitForEvents(135);
    expectSingleAccessibleValue(
        updates, meter,
        {QStringLiteral("Transmit power"), QStringLiteral("65 watts")},
        QStringLiteral("TX power burst and transmit transition"));

    updates.clear();
    meter.setTxMode(QStringLiteral("SWR"));
    meter.setTxMeters(65.0f, 1.7f);
    waitForEvents(135);
    expectSingleAccessibleValue(
        updates, meter,
        {QStringLiteral("Transmit SWR"), QStringLiteral("1.7")},
        QStringLiteral("TX SWR mode change"));

    updates.clear();
    meter.setTxMode(QStringLiteral("Level"));
    meter.setMicMeters(-12.0f, 0.0f, -10.0f, 4.0f);
    waitForEvents(135);
    expectSingleAccessibleValue(
        updates, meter,
        {QStringLiteral("Transmit level"), QStringLiteral("-12 dB")},
        QStringLiteral("TX level mode change and meter burst"));

    updates.clear();
    meter.setTxMode(QStringLiteral("Compression"));
    meter.setMicMeters(-12.0f, 0.0f, -10.0f, 4.0f);
    waitForEvents(135);
    expectSingleAccessibleValue(
        updates, meter,
        {QStringLiteral("Transmit compression"), QStringLiteral("-4 dB")},
        QStringLiteral("TX compression mode change and meter burst"));

    updates.clear();
    meter.setTransmitting(false);
    waitForEvents(135);
    expectSingleAccessibleValue(
        updates, meter,
        {QStringLiteral("S9+12"), QStringLiteral("-61 dBm")},
        QStringLiteral("return to receive transition"));

    meter.close();
    QAccessible::installUpdateHandler(previousHandler);
    QAccessible::setActive(wasAccessible);
    g_accessibleValueUpdates = nullptr;
}

void testRadioSwrValidityFilter()
{
    AetherSDR::SMeterWidget meter;
    meter.setTxMode(QStringLiteral("SWR"));
    meter.setTransmitting(true);

    meter.setRadioTxMeters(12.0f, 12.0f, 2.8f);
    expect(meter.accessibleValueText().contains(QStringLiteral("2.8")),
           "powered native radio SWR is displayed directly");
    expect(meter.property("txSwrSource").toString() == QStringLiteral("radio"),
           "standard S-meter identifies native radio SWR as its source");
    expect(!meter.property("txSwrHeld").toBool(),
           "powered native radio SWR is not marked held");
    expect(near(meter.property("txSwrMinimumForwardWatts").toDouble(), 2.4, 1e-5),
           "native SWR validity follows the measured TX envelope peak");

    meter.setRadioTxMeters(12.0f, 12.0f, 1.0859375f);
    expect(meter.accessibleValueText().contains(QStringLiteral("2.8")),
           "an early near-unity packet waits for forward-power confirmation");
    expect(meter.property("txSwrHeld").toBool(),
           "an early near-unity packet is identified as held");

    meter.setRadioTxMeters(10.0f, 0.004f, 1.0859375f);
    expect(meter.accessibleValueText().contains(QStringLiteral("2.8")),
           "low-forward-power radio SWR does not replace the last valid ratio");
    expect(meter.property("txSwrHeld").toBool(),
           "low-forward-power radio SWR is identified as held");

    meter.setRadioTxMeters(10.0f, 1.0f, 5.0f);
    expect(meter.accessibleValueText().contains(QStringLiteral("5.0")),
           "a worsening native SWR warning survives low forward-power foldback");

    meter.setRadioTxMeters(10.0f, 10.0f, 1.0f);
    meter.setRadioTxMeters(10.0f, 10.0f, 1.0f);
    expect(meter.accessibleValueText().contains(QStringLiteral("5.0")),
           "two powered unity packets do not reproduce the tune-reset artifact");
    meter.setRadioTxMeters(10.0f, 10.0f, 1.0f);
    expect(meter.accessibleValueText().contains(QStringLiteral("1.0")),
           "sustained powered unity SWR is accepted after confirmation");

    meter.setRadioTxMeters(10.0f, 10.0f, 2.6f);
    meter.setRadioTxMeters(10.0f, 10.0f, -25.0f);
    expect(meter.accessibleValueText().contains(QStringLiteral("2.6")),
           "one invalid below-unity radio sample cannot flash the upper stop");
    expect(meter.property("txSwrHeld").toBool(),
           "a transient below-unity radio sample is identified as held");
    expect(near(meter.property("txSwrRaw").toDouble(), -25.0),
           "diagnostics preserve the raw below-unity radio sample");

    meter.setTransmitting(false);
    meter.setTransmitting(true);
    expect(meter.accessibleValueText().contains(QStringLiteral("1.0")),
           "un-key resets the held native radio SWR state");
    expect(!meter.property("txSwrHeld").toBool(),
           "un-key clears the native radio SWR hold marker");
}

void testRadioSwrValidityFilterAdaptsToSustainedLowerPower()
{
    AetherSDR::RadioSwrValidityFilter filter;

    AetherSDR::RadioSwrValidityFilter::Result result =
        filter.update(100.0f, 3.0f, 0, 3.0f);
    expect(near(result.displayedSwr, 3.0)
               && near(result.forwardEnvelopeWatts, 100.0)
               && near(result.minimumForwardWatts, 20.0),
           "native SWR filter attacks immediately to a powered envelope peak");

    result = filter.update(10.0f, 1.0f, 200, 3.0f);
    expect(result.held && near(result.displayedSwr, 3.0)
               && result.minimumForwardWatts > 10.0f,
           "brief lower-power near-unity sample remains held");

    result = filter.update(0.0f, 5.0f, 250, 3.0f);
    expect(result.held && near(result.displayedSwr, 3.0),
           "zero-power high-SWR noise cannot raise the held reading");

    result = filter.update(10.0f, 1.5f, 1000, 3.0f);
    expect(!result.held && near(result.displayedSwr, 1.5)
               && result.minimumForwardWatts < 10.0f,
           "sustained lower-power operating point becomes authoritative");

    filter.reset();
    filter.update(100.0f, 5.0f, 0, 3.0f);
    result = filter.update(10.0f, 2.0f, 100, 3.0f);
    expect(result.held && near(result.displayedSwr, 5.0)
               && result.minimumForwardWatts > 10.0f,
           "brief measurable-power SWR recovery remains held");
    result = filter.update(10.0f, 2.1f, 300, 3.0f);
    expect(result.held && near(result.displayedSwr, 5.0),
           "measurable-power recovery remains held before its time window");
    result = filter.update(10.0f, 2.0f, 350, 3.0f);
    expect(!result.held && near(result.displayedSwr, 2.0)
               && result.minimumForwardWatts > 10.0f,
           "confirmed SWR recovery replaces a stale warning before envelope release");

    result = filter.update(0.0017f, 1.125f, 700, 3.0f);
    expect(result.held && near(result.displayedSwr, 2.0),
           "real-radio no-carrier SWR artifact remains rejected");
    result = filter.update(0.0017f, 1.125f, 2000, 3.0f);
    expect(result.held && near(result.displayedSwr, 2.0),
           "no-carrier packets cannot satisfy a timed recovery window");

    filter.reset();
    filter.update(100.0f, 5.0f, 0, 3.0f);
    result = filter.update(0.1f, 1.1f, 100, 3.0f);
    expect(result.held && near(result.displayedSwr, 5.0)
               && result.minimumForwardWatts > 0.1f,
           "brief near-unity recovery remains held in the low-power twilight zone");
    result = filter.update(0.1f, 1.15f, 300, 3.0f);
    expect(result.held && near(result.displayedSwr, 5.0)
               && result.minimumForwardWatts > 0.1f,
           "near-unity recovery remains held before its wall-clock confirmation");
    result = filter.update(0.1f, 1.1f, 350, 3.0f);
    expect(!result.held && near(result.displayedSwr, 1.1, 1e-5)
               && result.minimumForwardWatts > 0.1f,
           "confirmed near-unity recovery replaces stale SWR before envelope release");

    for (const float boundarySwr : {1.0f, 1.2f}) {
        filter.reset();
        filter.update(100.0f, 5.0f, 0, 3.0f);
        result = filter.update(0.1f, boundarySwr, 100, 3.0f);
        expect(result.held && near(result.displayedSwr, 5.0),
               QStringLiteral("low-power SWR %1 starts bounded confirmation")
                   .arg(boundarySwr));
        result = filter.update(0.1f, boundarySwr, 350, 3.0f);
        expect(!result.held
                   && near(result.displayedSwr, boundarySwr, 1e-5),
               QStringLiteral("low-power SWR %1 is accepted at the confirmation boundary")
                   .arg(boundarySwr));
    }

    filter.reset();
    filter.update(100.0f, 5.0f, 0, 3.0f);
    result = filter.update(0.1f, 1.1f, 100, 3.0f);
    expect(result.held && near(result.displayedSwr, 5.0),
           "near-unity recovery starts a confirmation window");
    result = filter.update(0.0017f, 1.1f, 300, 3.0f);
    expect(result.held && near(result.displayedSwr, 5.0),
           "loss of measurable carrier interrupts near-unity confirmation");
    result = filter.update(0.1f, 1.1f, 400, 3.0f);
    expect(result.held && near(result.displayedSwr, 5.0),
           "powered near-unity recovery restarts after an interruption");
    result = filter.update(0.1f, 1.1f, 649, 3.0f);
    expect(result.held && near(result.displayedSwr, 5.0),
           "restarted recovery cannot reuse elapsed time from before the interruption");
    result = filter.update(0.1f, 1.1f, 650, 3.0f);
    expect(!result.held && near(result.displayedSwr, 1.1, 1e-5),
           "restarted recovery becomes authoritative after a fresh confirmation window");

    filter.reset();
    filter.update(100.0f, 2.6f, 0, 3.0f);
    result = filter.update(10.0f, -25.0f, 100, 3.0f);
    expect(result.held && near(result.displayedSwr, 2.6, 1e-5),
           QStringLiteral("one below-unity sentinel cannot peg the SWR display "
                          "(held=%1 displayed=%2)")
               .arg(result.held)
               .arg(result.displayedSwr));
    result = filter.update(10.0f, -25.0f, 300, 3.0f);
    expect(result.held && near(result.displayedSwr, 2.6, 1e-5),
           QStringLiteral("below-unity sentinel remains held before its time window "
                          "(held=%1 displayed=%2)")
               .arg(result.held)
               .arg(result.displayedSwr));
    result = filter.update(10.0f, -25.0f, 350, 3.0f);
    expect(!result.held && near(result.displayedSwr, 3.0),
           "persistent measurable-power sentinel reaches the upper stop during foldback");

    filter.reset();
    filter.update(100.0f, 2.0f, 0, 3.0f);
    result = filter.update(10.0f, 5.0f, 100, 3.0f);
    expect(!result.held && near(result.displayedSwr, 5.0),
           "powered foldback still surfaces a worsening native warning");

    result = filter.update(10.0f, 1.0f, 1000, 3.0f);
    expect(result.held && near(result.displayedSwr, 5.0),
           "first powered near-unity sample remains held after envelope release");
    result = filter.update(10.0f, 1.0f, 1010, 3.0f);
    expect(result.held && near(result.displayedSwr, 5.0),
           "second powered near-unity sample remains held");
    result = filter.update(10.0f, 1.0f, 1020, 3.0f);
    expect(!result.held && near(result.displayedSwr, 1.0),
           "third powered near-unity sample is accepted");

    filter.reset();
    result = filter.update(0.0f, 5.0f, 0, 3.0f);
    expect(!result.hasReading && !result.held && near(result.displayedSwr, 1.0),
           "reset filter ignores an unpowered first SWR sample");
}

void testRadioSwrValidityFilterInterruptedBelowUnityDoesNotPeg()
{
    // Regression: a run of below-unity sentinels that is broken by a genuine
    // in-range reading must NOT peg the meter to full scale. The below-unity
    // confirmation window has to restart when the streak is interrupted,
    // otherwise a stale start time can fire the peg on a non-continuous run.
    AetherSDR::RadioSwrValidityFilter filter;

    // Latch a 5.0 fault at full power.
    filter.update(100.0f, 5.0f, 0, 6.0f);

    // A sub-unity sentinel under a weak carrier opens the below-unity window.
    AetherSDR::RadioSwrValidityFilter::Result result =
        filter.update(10.0f, 0.5f, 100, 6.0f);
    expect(result.held && near(result.displayedSwr, 5.0),
           "one below-unity sample under weak power is held, not pegged");

    // A genuine 2.0 reading in the twilight zone interrupts the sentinel run.
    result = filter.update(10.0f, 2.0f, 200, 6.0f);
    expect(result.held && near(result.displayedSwr, 5.0),
           "an interrupting in-range recovery sample is held");

    // A later below-unity sample — long after the first one — must not peg to
    // full scale, because the interrupting reading restarted the window.
    result = filter.update(10.0f, 0.5f, 500, 6.0f);
    expect(result.held && near(result.displayedSwr, 5.0),
           "an interrupted below-unity streak cannot peg the meter to full scale");
}

bool sameTicks(const QVector<AetherSDR::SMeterGeometry::Tick>& first,
               const QVector<AetherSDR::SMeterGeometry::Tick>& second)
{
    if (first.size() != second.size()) {
        return false;
    }
    for (qsizetype index = 0; index < first.size(); ++index) {
        if (!near(first.at(index).value, second.at(index).value)
            || first.at(index).label != second.at(index).label) {
            return false;
        }
    }
    return true;
}

bool sameStaticScale(const AetherSDR::SMeterGeometry::StaticScale& first,
                     const AetherSDR::SMeterGeometry::StaticScale& second)
{
    return near(first.minimum, second.minimum) && near(first.maximum, second.maximum)
        && near(first.warningStart, second.warningStart)
        && first.hasWarning == second.hasWarning && sameTicks(first.ticks, second.ticks);
}

bool sameGeometry(const AetherSDR::SMeterGeometry& first,
                  const AetherSDR::SMeterGeometry& second)
{
    if (first.formatVersion != second.formatVersion
        || first.designVersion != second.designVersion
        || first.sizing.preferred != second.sizing.preferred
        || first.sizing.minimum != second.sizing.minimum
        || !near(first.sizing.minimumAspectRatio,
                 second.sizing.minimumAspectRatio)
        || !near(first.sizing.maximumAspectRatio,
                 second.sizing.maximumAspectRatio)
        || !near(first.arc.centerXWidthFactor, second.arc.centerXWidthFactor)
        || !near(first.arc.radiusWidthFactor, second.arc.radiusWidthFactor)
        || !near(first.arc.centerYHeightFactor, second.arc.centerYHeightFactor)
        || !near(first.arc.startDegrees, second.arc.startDegrees)
        || !near(first.arc.endDegrees, second.arc.endDegrees)
        || !near(first.arc.innerGapPixels, second.arc.innerGapPixels)
        || !near(first.arc.lineWidthPixels, second.arc.lineWidthPixels)
        || !near(first.tickStyle.startOffsetPixels, second.tickStyle.startOffsetPixels)
        || !near(first.tickStyle.endOffsetPixels, second.tickStyle.endOffsetPixels)
        || !near(first.tickStyle.labelOffsetPixels, second.tickStyle.labelOffsetPixels)
        || !near(first.tickStyle.lineWidthPixels, second.tickStyle.lineWidthPixels)
        || first.tickStyle.fontMinimumPixels != second.tickStyle.fontMinimumPixels
        || !near(first.tickStyle.fontHeightFactor, second.tickStyle.fontHeightFactor)
        || first.tickStyle.bold != second.tickStyle.bold
        || !near(first.rxScale.minimumDbm, second.rxScale.minimumDbm)
        || !near(first.rxScale.s9Dbm, second.rxScale.s9Dbm)
        || !near(first.rxScale.maximumDbm, second.rxScale.maximumDbm)
        || !near(first.rxScale.dbPerSUnit, second.rxScale.dbPerSUnit)
        || !near(first.rxScale.s9Fraction, second.rxScale.s9Fraction)
        || !sameTicks(first.rxScale.ticks, second.rxScale.ticks)
        || !sameStaticScale(first.swrScale, second.swrScale)
        || !sameStaticScale(first.levelScale, second.levelScale)
        || !sameStaticScale(first.compressionScale, second.compressionScale)
        || first.powerTickPolicies.size() != second.powerTickPolicies.size()) {
        return false;
    }
    for (qsizetype index = 0; index < first.powerTickPolicies.size(); ++index) {
        const AetherSDR::SMeterGeometry::PowerTickPolicy& firstPolicy =
            first.powerTickPolicies.at(index);
        const AetherSDR::SMeterGeometry::PowerTickPolicy& secondPolicy =
            second.powerTickPolicies.at(index);
        if (!near(firstPolicy.minimumScaleWatts, secondPolicy.minimumScaleWatts)
            || firstPolicy.tickStepWatts != secondPolicy.tickStepWatts
            || firstPolicy.labelStepWatts != secondPolicy.labelStepWatts) {
            return false;
        }
    }
    return near(first.needle.pivotYBelowWidgetPixels,
                second.needle.pivotYBelowWidgetPixels)
        && near(first.needle.tipExtensionPixels, second.needle.tipExtensionPixels)
        && near(first.needle.lineWidthPixels, second.needle.lineWidthPixels)
        && near(first.needle.shadowWidthPixels, second.needle.shadowWidthPixels)
        && first.needle.shadowOffset == second.needle.shadowOffset
        && near(first.pivot.minimumRadiusPixels, second.pivot.minimumRadiusPixels)
        && near(first.pivot.radiusWidthFactor, second.pivot.radiusWidthFactor)
        && near(first.pivot.glowRadiusFactor, second.pivot.glowRadiusFactor)
        && near(first.pivot.glowMiddleFactor, second.pivot.glowMiddleFactor)
        && first.pivot.glowCenterAlpha == second.pivot.glowCenterAlpha
        && first.pivot.glowMiddleAlpha == second.pivot.glowMiddleAlpha
        && near(first.pivot.rimWidthPixels, second.pivot.rimWidthPixels)
        && near(first.peakMarker.radiusInsetPixels, second.peakMarker.radiusInsetPixels)
        && near(first.peakMarker.lengthPixels, second.peakMarker.lengthPixels)
        && near(first.peakMarker.halfWidthPixels, second.peakMarker.halfWidthPixels)
        && near(first.peakMarker.minimumLeadDb, second.peakMarker.minimumLeadDb)
        && near(first.peakHold.innerRadiusOffsetPixels,
                second.peakHold.innerRadiusOffsetPixels)
        && near(first.peakHold.outerRadiusOffsetPixels,
                second.peakHold.outerRadiusOffsetPixels)
        && near(first.peakHold.lineWidthPixels, second.peakHold.lineWidthPixels)
        && near(first.peakHold.visibleAboveMinimumDb,
                second.peakHold.visibleAboveMinimumDb)
        && first.readout.sourceFontMinimumPixels == second.readout.sourceFontMinimumPixels
        && near(first.readout.sourceFontHeightDivisor,
                second.readout.sourceFontHeightDivisor)
        && first.readout.valueFontMinimumPixels == second.readout.valueFontMinimumPixels
        && near(first.readout.valueFontHeightDivisor,
                second.readout.valueFontHeightDivisor)
        && first.readout.topExtraPixels == second.readout.topExtraPixels
        && first.readout.sideMarginPixels == second.readout.sideMarginPixels;
}

AetherSDR::SMeterGeometry loadObject(const QJsonObject& object, QString* error)
{
    QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::ReadOnly);
    return AetherSDR::SMeterGeometry::load(buffer, error);
}

void expectRejected(const QJsonObject& object, const QString& expectedError,
                    const QString& message)
{
    QString error;
    const AetherSDR::SMeterGeometry geometry = loadObject(object, &error);
    expect(!geometry.isValid(), message + QStringLiteral(" is rejected"));
    expect(error.contains(expectedError),
           message + QStringLiteral(" reports its failing field or invariant"));
}

double pointLineDistance(const QPointF& point,
                         const AetherSDR::SMeterGeometry::MovementRay& ray)
{
    const QPointF relative = point - ray.pivot;
    return std::abs(relative.x() * ray.direction.y()
                    - relative.y() * ray.direction.x());
}

bool hasBrightPixelNear(const QImage& image, const QPointF& point, int radius)
{
    const int centerX = qRound(point.x());
    const int centerY = qRound(point.y());
    for (int y = centerY - radius; y <= centerY + radius; ++y) {
        if (y < 0 || y >= image.height()) {
            continue;
        }
        for (int x = centerX - radius; x <= centerX + radius; ++x) {
            if (x < 0 || x >= image.width()) {
                continue;
            }
            const QColor color = image.pixelColor(x, y);
            if (color.red() >= 220 && color.green() >= 220 && color.blue() >= 220) {
                return true;
            }
        }
    }
    return false;
}

QByteArray imageDigest(const QImage& image)
{
    return QCryptographicHash::hash(
        QByteArrayView(reinterpret_cast<const char*>(image.constBits()), image.sizeInBytes()),
        QCryptographicHash::Sha256);
}

QImage render(AetherSDR::SMeterWidget& meter, const QSize& size)
{
    meter.resize(size);
    meter.ensurePolished();
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    meter.render(&painter);
    return image;
}

int changedPixelCount(const QImage& image)
{
    const QRgb background = qRgb(0x0f, 0x0f, 0x1a);
    int changed = 0;
    for (int y = 0; y < image.height(); ++y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            if ((row[x] & 0x00ffffffU) != (background & 0x00ffffffU)) {
                ++changed;
            }
        }
    }
    return changed;
}

double averageLuminance(const QImage& image, const QRect& requestedRect)
{
    const QRect rect = requestedRect.intersected(image.rect());
    if (rect.isEmpty()) {
        return 0.0;
    }

    double total = 0.0;
    qsizetype count = 0;
    for (int y = rect.top(); y <= rect.bottom(); ++y) {
        for (int x = rect.left(); x <= rect.right(); ++x) {
            const QColor color = image.pixelColor(x, y);
            total += 0.2126 * color.red() + 0.7152 * color.green()
                + 0.0722 * color.blue();
            ++count;
        }
    }
    return count > 0 ? total / static_cast<double>(count) : 0.0;
}

QImage renderBackground(const AetherSDR::AnalogMeterFaceThemeCatalog& catalog,
                        AetherSDR::AnalogMeterFaceTheme theme, const QSize& size)
{
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    catalog.drawBackground(
        painter, QRectF(QPointF(0.0, 0.0), QSizeF(size)), theme);
    return image;
}

void saveProofFromEnvironment(const char* variable, const QImage& image)
{
    const QString path = qEnvironmentVariable(variable);
    if (!path.isEmpty()) {
        expect(image.save(path),
               QStringLiteral("proof image saves to %1").arg(path));
    }
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile settingsProfile(QStringLiteral("aether-s-meter-geometry-test"));
    if (!settingsProfile.isValid()) {
        std::fprintf(stderr, "[FAIL] could not create temporary settings profile\n");
        return 1;
    }
    QApplication app(argc, argv);
    AetherSDR::AppSettings::instance().load();

    QString error;
    const AetherSDR::SMeterGeometry geometry =
        AetherSDR::SMeterGeometry::loadResource(&error);
    expect(error.isEmpty(), "shipping S-meter resource loads without an error");
    expect(geometry.isValid(), "shipping S-meter geometry validates");
    expect(geometry.formatVersion == 1, "format version is 1");
    expect(geometry.designVersion == 5,
           "design version includes bounded detached rendering");
    expect(geometry.sizing.preferred == QSize(280, 140), "preferred size is preserved");
    expect(geometry.sizing.minimum == QSize(200, 100), "minimum size is preserved");
    expect(near(geometry.sizing.minimumAspectRatio, 1.75)
               && near(geometry.sizing.maximumAspectRatio, 4.0),
           "detached face aspect limits are data driven");
    expect(sameGeometry(geometry, AetherSDR::SMeterGeometry::fallback()),
           "shipping JSON and compiled fallback are exactly equivalent");
    expect(geometry.readout.topExtraPixels == 4,
           "readout retains the approved top margin");
    for (const QSizeF size :
         {QSizeF(200.0, 100.0), QSizeF(280.0, 140.0),
          QSizeF(560.0, 280.0), QSizeF(200.0, 2000.0),
          QSizeF(2000.0, 100.0)}) {
        const AetherSDR::SMeterGeometry::Layout layout =
            geometry.layoutFor(size);
        QFont sourceFont = QApplication::font();
        sourceFont.setPixelSize(layout.sourceFontPixels);
        const QFontMetrics sourceMetrics(sourceFont);
        QFont valueFont = QApplication::font();
        valueFont.setPixelSize(layout.valueFontPixels);
        valueFont.setBold(true);
        const QFontMetrics valueMetrics(valueFont);
        const int valueBaseline =
            qRound(layout.viewport.top())
            + std::max(sourceMetrics.ascent(), valueMetrics.ascent())
            + geometry.readout.topExtraPixels;
        const int sourceBaseline =
            valueBaseline - valueMetrics.ascent()
            + sourceMetrics.ascent();
        const int sourceTop = sourceBaseline - sourceMetrics.ascent();
        const int valueTop = valueBaseline - valueMetrics.ascent();
        const QString context =
            QStringLiteral("%1x%2 readout").arg(size.width()).arg(size.height());
        expect(valueBaseline - valueMetrics.ascent()
                   >= qRound(layout.viewport.top())
                       + geometry.readout.topExtraPixels,
               context + QStringLiteral(" clears the value text from the top"));
        expect(sourceTop == valueTop,
               context + QStringLiteral(" top-aligns the source and values"));
        expect(sourceBaseline < valueBaseline,
               context + QStringLiteral(" shifts the smaller source label upward"));
    }

    QString themeError;
    const AetherSDR::AnalogMeterFaceThemeCatalog themes =
        AetherSDR::AnalogMeterFaceThemeCatalog::loadResource(&themeError);
    expect(themeError.isEmpty(), "shared analog face theme resource loads without an error");
    expect(themes.isValid(), "shared analog face theme resource validates");
    expect(themes.formatVersion == 1, "shared analog face theme format is versioned");
    QByteArray invalidThemeBytes("{}");
    QBuffer invalidThemeBuffer(&invalidThemeBytes);
    invalidThemeBuffer.open(QIODevice::ReadOnly);
    QString invalidThemeError;
    expect(!AetherSDR::AnalogMeterFaceThemeCatalog::load(
                invalidThemeBuffer, &invalidThemeError).isValid()
               && !invalidThemeError.isEmpty(),
           "malformed shared theme data is rejected with an error");
    const AetherSDR::AnalogMeterFaceThemeCatalog fallbackThemes =
        AetherSDR::AnalogMeterFaceThemeCatalog::fallback();
    expect(fallbackThemes.isValid(),
           "complete compiled theme fallback validates");
    expect(themes == fallbackThemes,
           "shipping shared theme JSON and compiled fallback are exactly equivalent");

    AetherSDR::AnalogMeterFaceThemeCatalog changedGradient = fallbackThemes;
    changedGradient.uplightGradient.haloRadius += 0.01;
    expect(changedGradient != fallbackThemes,
           "theme equality includes every material gradient field");
    AetherSDR::AnalogMeterFaceThemeCatalog changedPalette = fallbackThemes;
    changedPalette.darkPalette.needle = QColor(QStringLiteral("#123456"));
    expect(changedPalette != fallbackThemes,
           "theme equality includes every palette field");
    AetherSDR::AnalogMeterFaceThemeCatalog changedReference = fallbackThemes;
    changedReference.referenceFace.translate(1.0, 0.0);
    expect(changedReference != fallbackThemes,
           "theme equality includes the reference face");
    AetherSDR::AnalogMeterFaceThemeCatalog changedMask = fallbackThemes;
    changedMask.normalizedMaskBoundary[0].setY(
        changedMask.normalizedMaskBoundary.at(0).y() - 0.01);
    expect(changedMask != fallbackThemes,
           "theme equality includes every lower-mask boundary point");

    AetherSDR::AnalogMeterFaceThemeCatalog offCenterMask = fallbackThemes;
    const qsizetype centerIndex = offCenterMask.normalizedMaskBoundary.size() / 2;
    offCenterMask.normalizedMaskBoundary[centerIndex].setX(0.51);
    QString offCenterMaskError;
    expect(!offCenterMask.isValid(&offCenterMaskError)
               && offCenterMaskError.contains(QStringLiteral("not symmetric")),
           "off-center lower-mask midpoint is rejected as asymmetric");
    expect(themes.normalizedMaskBoundary.size() == 9,
           "physical themes retain the approved nine-point lower mask");
    for (qsizetype index = 0; index < themes.normalizedMaskBoundary.size() / 2;
         ++index) {
        const QPointF left = themes.normalizedMaskBoundary.at(index);
        const QPointF right = themes.normalizedMaskBoundary.at(
            themes.normalizedMaskBoundary.size() - 1 - index);
        expect(near(left.x() + right.x(), 1.0, 1e-9)
                   && near(left.y(), right.y(), 1e-9),
               QStringLiteral("physical mask point pair %1 is symmetric").arg(index));
    }

    for (const QSizeF& size : {QSizeF(200.0, 100.0), QSizeF(280.0, 140.0),
                               QSizeF(560.0, 140.0), QSizeF(560.0, 280.0)}) {
        const QRectF face(QPointF(0.0, 0.0), size);
        const QVector<QPointF> boundary = themes.lowerMaskBoundary(face);
        expect(boundary.size() == 9,
               QStringLiteral("physical mask maps all points at %1x%2")
                   .arg(size.width()).arg(size.height()));
        for (qsizetype index = 0; index < boundary.size() / 2; ++index) {
            const QPointF left = boundary.at(index);
            const QPointF right = boundary.at(boundary.size() - 1 - index);
            expect(near(left.x() + right.x(), size.width(), 1e-7)
                       && near(left.y(), right.y(), 1e-7),
                   QStringLiteral("mapped mask pair %1 stays symmetric at %2x%3")
                       .arg(index).arg(size.width()).arg(size.height()));
        }
        expect(!themes.lowerMaskPath(face).isEmpty(),
               QStringLiteral("physical mask path remains drawable at %1x%2")
                   .arg(size.width()).arg(size.height()));
    }

    const QSize materialSize(560, 280);
    const QImage classicMaterial = renderBackground(
        themes, AetherSDR::AnalogMeterFaceTheme::ClassicWarm, materialSize);
    const QImage uplightMaterial = renderBackground(
        themes, AetherSDR::AnalogMeterFaceTheme::DarkRoomUplight, materialSize);
    const QImage darkMaterial = renderBackground(
        themes, AetherSDR::AnalogMeterFaceTheme::GraphiteDark, materialSize);
    expect(imageDigest(classicMaterial) != imageDigest(uplightMaterial)
               && imageDigest(uplightMaterial) != imageDigest(darkMaterial)
               && imageDigest(classicMaterial) != imageDigest(darkMaterial),
           "the three physical face materials are visually distinct");
    const QRect topCenter(210, 10, 140, 35);
    const QRect lowerCenter(210, 220, 140, 35);
    expect(averageLuminance(uplightMaterial, lowerCenter)
               > averageLuminance(uplightMaterial, topCenter) + 45.0,
           "dark-room uplight brightens realistically from top to lower center");
    expect(averageLuminance(darkMaterial, topCenter) < 80.0
               && averageLuminance(darkMaterial, lowerCenter) < 100.0,
           "graphite material remains dark across the complete face");
    expect(imageDigest(uplightMaterial)
               == imageDigest(renderBackground(
                   themes, AetherSDR::AnalogMeterFaceTheme::DarkRoomUplight,
                   materialSize)),
           "fixed-seed paper grain renders deterministically");
    expect(themes.classicPalette.needle.lightness() < 70
               && themes.uplightPalette.needle.lightness() < 70,
           "classic and uplight faces use approved dark needle material");
    expect(themes.darkPalette.needle.lightness() > 140,
           "graphite face keeps its contrasting metallic needle material");

    QFile resource(QStringLiteral(":/meterfaces/s-meter-v1.json"));
    expect(resource.open(QIODevice::ReadOnly), "shipping S-meter JSON can be reopened");
    const QJsonDocument shippingDocument = QJsonDocument::fromJson(resource.readAll());
    expect(shippingDocument.isObject(), "shipping S-meter JSON has an object root");
    const QJsonObject shippingRoot = shippingDocument.object();

    const AetherSDR::SMeterGeometry::Layout nominal =
        geometry.layoutFor(QSizeF(280.0, 140.0));
    expect(near(nominal.centerX, 140.0), "nominal arc center X is preserved");
    expect(near(nominal.radius, 238.0), "nominal arc radius is preserved");
    expect(near(nominal.centerY, 287.0), "nominal arc center Y is preserved");
    expect(near(nominal.needlePivotY, 146.0), "nominal needle pivot Y is preserved");
    expect(near(nominal.innerRadius, 232.0), "nominal inner radius is preserved");
    expect(near(nominal.pivotRadius, 27.3), "nominal pivot radius is preserved");
    expect(nominal.tickFontPixels == 14, "nominal tick font is preserved");
    expect(nominal.sourceFontPixels == 10, "nominal source font is preserved");
    expect(nominal.valueFontPixels == 17, "nominal value font is preserved");

    const AetherSDR::SMeterGeometry::Layout minimum =
        geometry.layoutFor(QSizeF(200.0, 100.0));
    expect(near(minimum.centerX, 100.0), "minimum arc center X is responsive");
    expect(near(minimum.radius, 170.0), "minimum arc radius is responsive");
    expect(near(minimum.centerY, 205.0), "minimum arc center Y is responsive");
    expect(near(minimum.needlePivotY, 106.0), "minimum needle pivot is responsive");
    expect(near(minimum.pivotRadius, 19.5), "minimum pivot radius is responsive");
    expect(minimum.tickFontPixels == 10, "minimum tick font floor is preserved");
    expect(minimum.sourceFontPixels == 9, "minimum source font floor is preserved");
    expect(minimum.valueFontPixels == 13, "minimum value font floor is preserved");

    const AetherSDR::SMeterGeometry::Layout wideShort =
        geometry.layoutFor(QSizeF(560.0, 140.0));
    expect(near(wideShort.pivotRadius, nominal.pivotRadius),
           "wide short windows do not inflate the pivot mask");
    const AetherSDR::SMeterGeometry::Layout uniformDouble =
        geometry.layoutFor(QSizeF(560.0, 280.0));
    expect(near(uniformDouble.pivotRadius, nominal.pivotRadius * 2.0),
           "pivot mask still scales with a uniformly enlarged face");
    const AetherSDR::SMeterGeometry::Layout tallNarrow =
        geometry.layoutFor(QSizeF(280.0, 280.0));
    expect(near(tallNarrow.pivotRadius, nominal.pivotRadius),
           "tall narrow windows keep the width-limited pivot mask");

    const AetherSDR::SMeterGeometry::Layout extremeTall =
        geometry.layoutFor(QSizeF(200.0, 2000.0));
    expect(near(extremeTall.viewport.left(), 0.0)
               && near(extremeTall.viewport.top(), 6600.0 / 7.0)
               && near(extremeTall.viewport.width(), 200.0)
               && near(extremeTall.viewport.height(), 800.0 / 7.0),
           "pathological tall windows center a minimum-aspect face viewport");
    expect(near(extremeTall.centerX, 100.0)
               && near(extremeTall.radius, 170.0)
               && near(extremeTall.centerY, 8070.0 / 7.0)
               && near(extremeTall.needlePivotY, 7442.0 / 7.0),
           "tall viewport keeps the movement pivot inside its printed arcs");
    expect(extremeTall.tickFontPixels == 11
               && extremeTall.sourceFontPixels == 9
               && extremeTall.valueFontPixels == 14,
           "tall viewport bounds label growth before readout and scale text collide");

    const AetherSDR::SMeterGeometry::Layout extremeWide =
        geometry.layoutFor(QSizeF(2000.0, 100.0));
    expect(extremeWide.viewport == QRectF(800.0, 0.0, 400.0, 100.0),
           "pathological wide windows center a maximum-aspect face viewport");
    expect(near(extremeWide.centerX, 1000.0)
               && near(extremeWide.radius, 340.0)
               && near(extremeWide.centerY, 375.0)
               && near(extremeWide.needlePivotY, 106.0),
           "wide viewport keeps every calibrated arc endpoint on the face");

    expect(near(qRadiansToDegrees(geometry.fractionToRadians(0.0)), 125.0),
           "fraction zero maps to the left endpoint");
    expect(near(qRadiansToDegrees(geometry.fractionToRadians(1.0)), 55.0),
           "fraction one maps to the right endpoint");
    expect(near(geometry.rxFraction(-127.0), 0.0), "S0 maps to fraction zero");
    expect(near(geometry.rxFraction(-73.0), 0.6), "S9 maps to fraction 0.6");
    expect(near(geometry.rxFraction(-13.0), 1.0), "S9+60 maps to fraction one");
    expect(near(geometry.rxFraction(-200.0), 0.0), "RX scale clamps below S0");
    expect(near(geometry.rxFraction(20.0), 1.0), "RX scale clamps above S9+60");
    expect(near(geometry.scaleFraction(geometry.swrScale, 1.5), 0.25),
           "SWR calibration is preserved");
    expect(near(geometry.scaleFraction(geometry.levelScale, 0.0), 40.0 / 45.0),
           "level calibration is preserved");
    expect(near(geometry.scaleFraction(geometry.compressionScale, 12.5), 0.5),
           "compression calibration is preserved");

    const AetherSDR::SMeterGeometry::PowerTickPolicy& barefoot =
        geometry.powerTickPolicy(120.0);
    const AetherSDR::SMeterGeometry::PowerTickPolicy& aurora =
        geometry.powerTickPolicy(600.0);
    const AetherSDR::SMeterGeometry::PowerTickPolicy& amplifier =
        geometry.powerTickPolicy(2000.0);
    expect(barefoot.tickStepWatts == 10 && barefoot.labelStepWatts == 40,
           "120 W power tick policy is preserved");
    expect(aurora.tickStepWatts == 50 && aurora.labelStepWatts == 100,
           "600 W power tick policy is preserved");
    expect(amplifier.tickStepWatts == 100 && amplifier.labelStepWatts == 500,
           "2 kW power tick policy is preserved");

    const QPointF leftTip = geometry.needleTip(QSizeF(280.0, 140.0), 0.0);
    const QPointF rightTip = geometry.needleTip(QSizeF(280.0, 140.0), 1.0);
    expect(near(leftTip.x() + rightTip.x(), 280.0, 1e-8),
           "needle endpoint construction is horizontally symmetric");
    expect(near(leftTip.y(), rightTip.y(), 1e-8),
           "needle endpoint construction is vertically symmetric");

    QVector<double> calibratedFractions = {0.0, 0.5, 1.0};
    for (const AetherSDR::SMeterGeometry::Tick& tick : geometry.rxScale.ticks) {
        calibratedFractions.push_back(geometry.rxFraction(tick.value));
    }
    for (const AetherSDR::SMeterGeometry::StaticScale* scale :
         {&geometry.swrScale, &geometry.levelScale, &geometry.compressionScale}) {
        for (const AetherSDR::SMeterGeometry::Tick& tick : scale->ticks) {
            calibratedFractions.push_back(geometry.scaleFraction(*scale, tick.value));
        }
    }

    const QVector<QSizeF> movementSizes = {
        QSizeF(200.0, 100.0), QSizeF(260.0, 140.0), QSizeF(280.0, 140.0),
        QSizeF(420.0, 140.0), QSizeF(560.0, 280.0), QSizeF(320.0, 180.0),
        QSizeF(200.0, 2000.0), QSizeF(2000.0, 100.0)};
    for (const QSizeF& size : movementSizes) {
        const AetherSDR::SMeterGeometry::Layout layout = geometry.layoutFor(size);
        const QPointF center(layout.centerX, layout.centerY);
        for (const double fraction : calibratedFractions) {
            const AetherSDR::SMeterGeometry::MovementRay ray =
                geometry.movementRayFor(size, fraction);
            const QPointF tip = geometry.needleTip(size, fraction);
            const std::optional<QPointF> innerPoint =
                geometry.movementRayCircleIntersection(size, fraction, layout.innerRadius);
            const QString context = QStringLiteral("%1x%2 at fraction %3")
                                        .arg(size.width())
                                        .arg(size.height())
                                        .arg(fraction, 0, 'f', 4);
            expect(near(std::hypot(ray.direction.x(), ray.direction.y()), 1.0, 1e-9),
                   context + QStringLiteral(" has a unit movement direction"));
            expect(pointLineDistance(ray.scalePoint, ray) < 1e-8,
                   context + QStringLiteral(" outer scale point is on the movement ray"));
            expect(pointLineDistance(tip, ray) < 1e-8,
                   context + QStringLiteral(" needle tip is on the movement ray"));
            expect(near(std::hypot(tip.x() - ray.scalePoint.x(),
                                   tip.y() - ray.scalePoint.y()),
                        geometry.needle.tipExtensionPixels, 1e-8),
                   context + QStringLiteral(" needle reaches the outer tick endpoint"));
            expect(innerPoint.has_value(),
                   context + QStringLiteral(" intersects the inner TX arc"));
            if (innerPoint) {
                expect(pointLineDistance(*innerPoint, ray) < 1e-8,
                       context + QStringLiteral(" inner TX point is on the movement ray"));
                expect(near(std::hypot(innerPoint->x() - center.x(),
                                       innerPoint->y() - center.y()),
                            layout.innerRadius, 1e-8),
                       context + QStringLiteral(" inner TX point is on the printed arc"));
            }
        }
    }

    QJsonObject missingNeedle = shippingRoot;
    missingNeedle.remove(QStringLiteral("needle"));
    expectRejected(missingNeedle, QStringLiteral("needle must be an object"),
                   QStringLiteral("missing needle section"));

    QJsonObject wrongArcType = shippingRoot;
    QJsonObject wrongArc = wrongArcType.value(QStringLiteral("arc")).toObject();
    wrongArc.insert(QStringLiteral("start_degrees"), QStringLiteral("55"));
    wrongArcType.insert(QStringLiteral("arc"), wrongArc);
    expectRejected(wrongArcType, QStringLiteral("arc.start_degrees"),
                   QStringLiteral("mistyped arc field"));

    QJsonObject zeroSize = shippingRoot;
    QJsonObject zeroSizing = zeroSize.value(QStringLiteral("sizing")).toObject();
    zeroSizing.insert(QStringLiteral("preferred"), QJsonArray{0, 0});
    zeroSizing.insert(QStringLiteral("minimum"), QJsonArray{0, 0});
    zeroSize.insert(QStringLiteral("sizing"), zeroSizing);
    expectRejected(zeroSize, QStringLiteral("sizing is invalid"),
                   QStringLiteral("zero-sized face"));

    QJsonObject invalidAspect = shippingRoot;
    QJsonObject invalidAspectSizing =
        invalidAspect.value(QStringLiteral("sizing")).toObject();
    invalidAspectSizing.insert(QStringLiteral("minimum_aspect_ratio"), 5.0);
    invalidAspect.insert(QStringLiteral("sizing"), invalidAspectSizing);
    expectRejected(invalidAspect, QStringLiteral("sizing is invalid"),
                   QStringLiteral("inverted face aspect limits"));

    QJsonObject excludesPreferredAspect = shippingRoot;
    QJsonObject excludesPreferredSizing =
        excludesPreferredAspect.value(QStringLiteral("sizing")).toObject();
    excludesPreferredSizing.insert(QStringLiteral("minimum_aspect_ratio"), 2.25);
    excludesPreferredAspect.insert(QStringLiteral("sizing"), excludesPreferredSizing);
    expectRejected(excludesPreferredAspect, QStringLiteral("sizing is invalid"),
                   QStringLiteral("face aspect limits exclude preferred geometry"));

    QJsonObject negativeInnerRadius = shippingRoot;
    QJsonObject oversizedGap = negativeInnerRadius.value(QStringLiteral("arc")).toObject();
    oversizedGap.insert(QStringLiteral("inner_gap_pixels"), 1000.0);
    negativeInnerRadius.insert(QStringLiteral("arc"), oversizedGap);
    expectRejected(negativeInnerRadius, QStringLiteral("derived layout is invalid"),
                   QStringLiteral("negative derived inner radius"));

    QJsonObject invalidGlow = shippingRoot;
    QJsonObject shortGlow = invalidGlow.value(QStringLiteral("pivot")).toObject();
    shortGlow.insert(QStringLiteral("glow_radius_factor"), 1.0);
    invalidGlow.insert(QStringLiteral("pivot"), shortGlow);
    expectRejected(invalidGlow, QStringLiteral("needle or pivot is invalid"),
                   QStringLiteral("non-expanding pivot glow"));

    QJsonObject invalidCalibration = shippingRoot;
    QJsonObject inconsistentRx = invalidCalibration.value(QStringLiteral("rx_scale")).toObject();
    inconsistentRx.insert(QStringLiteral("s9_dbm"), -72.0);
    invalidCalibration.insert(QStringLiteral("rx_scale"), inconsistentRx);
    expectRejected(invalidCalibration, QStringLiteral("RX scale is invalid"),
                   QStringLiteral("inconsistent S-unit calibration"));

    QByteArray malformedJson("{ definitely not JSON");
    QBuffer malformedBuffer(&malformedJson);
    malformedBuffer.open(QIODevice::ReadOnly);
    QString malformedError;
    const AetherSDR::SMeterGeometry malformed =
        AetherSDR::SMeterGeometry::load(malformedBuffer, &malformedError);
    expect(!malformed.isValid(), "malformed JSON is rejected");
    expect(!malformedError.isEmpty(), "malformed JSON reports an error");
    expect(AetherSDR::SMeterGeometry::fallback().isValid(), "compiled fallback validates");

    AetherSDR::SMeterWidget meter;
    expect(meter.geometry().isValid(), "widget owns valid loaded geometry");
    expect(meter.sUnitsText() == QStringLiteral("S0"), "widget initializes at geometry S0");
    expect(meter.faceTheme() == AetherSDR::AnalogMeterFaceTheme::AetherDefault
               && meter.faceThemeId() == QStringLiteral("aether-default"),
           "existing Aether S-meter remains the new-user default");
    QAccessibleInterface* accessible =
        QAccessible::queryAccessibleInterface(&meter);
    expect(accessible && accessible->role() == QAccessible::Indicator,
           "custom-painted S-meter exposes an accessible Indicator role");
    expect(accessible
               && accessible->text(QAccessible::Value).contains(QStringLiteral("S0")),
           "accessible S-meter value describes the calibrated reading");
    testAccessibilityAnnouncements();
    testRadioSwrValidityFilter();
    testRadioSwrValidityFilterAdaptsToSustainedLowerPower();
    testRadioSwrValidityFilterInterruptedBelowUnityDoesNotPeg();
    expect(meter.sizePolicy().verticalPolicy() == QSizePolicy::Fixed,
           "docked widget keeps its compact fixed-height policy");
    meter.setFloating(true);
    expect(meter.sizePolicy().horizontalPolicy() == QSizePolicy::Expanding
               && meter.sizePolicy().verticalPolicy() == QSizePolicy::Expanding,
           "floating widget expands in both dimensions");
    meter.setFloating(false);
    expect(meter.sizePolicy().horizontalPolicy() == QSizePolicy::Preferred
               && meter.sizePolicy().verticalPolicy() == QSizePolicy::Fixed,
           "re-docked widget restores its sidebar size policy");

    const QSize liveSize(260, 140);
    const QImage initial = render(meter, liveSize);
    const AetherSDR::SMeterGeometry::MovementRay initialRay =
        geometry.movementRayFor(QSizeF(liveSize), 0.0);
    const QPointF initialTip = geometry.needleTip(QSizeF(liveSize), 0.0);
    const QPointF alignedSample = initialRay.pivot + 0.70 * (initialTip - initialRay.pivot);
    expect(hasBrightPixelNear(initial, alignedSample, 2),
           "live-size rendering places the initial needle on its calibrated movement ray");

    const AetherSDR::SMeterGeometry::Layout liveLayout = geometry.layoutFor(QSizeF(liveSize));
    const double oldAngle = geometry.fractionToRadians(0.0);
    const double oldTipRadius = liveLayout.radius + geometry.needle.tipExtensionPixels;
    const QPointF oldRadialTip(liveLayout.centerX + oldTipRadius * std::cos(oldAngle),
                               liveLayout.centerY - oldTipRadius * std::sin(oldAngle));
    const QPointF oldTrajectorySample =
        initialRay.pivot + 0.70 * (oldRadialTip - initialRay.pivot);
    expect(!hasBrightPixelNear(initial, oldTrajectorySample, 1),
           "live-size rendering no longer uses the misaligned arc-center trajectory");

    for (const QSize extremeSize : {QSize(200, 2000), QSize(2000, 100)}) {
        const QImage extreme = render(meter, extremeSize);
        const AetherSDR::SMeterGeometry::MovementRay ray =
            geometry.movementRayFor(QSizeF(extremeSize), 0.0);
        const QPointF tip = geometry.needleTip(QSizeF(extremeSize), 0.0);
        const QPointF sample = ray.pivot + 0.70 * (tip - ray.pivot);
        expect(hasBrightPixelNear(extreme, sample, 2),
               QStringLiteral("%1x%2 render keeps the needle on its bounded movement ray")
                   .arg(extremeSize.width())
                   .arg(extremeSize.height()));
    }

    meter.setLevel(-73.0f);
    QVector<QImage> themedFaces;
    const QVector<AetherSDR::AnalogMeterFaceTheme> physicalThemes = {
        AetherSDR::AnalogMeterFaceTheme::ClassicWarm,
        AetherSDR::AnalogMeterFaceTheme::DarkRoomUplight,
        AetherSDR::AnalogMeterFaceTheme::GraphiteDark};
    for (const AetherSDR::AnalogMeterFaceTheme theme : physicalThemes) {
        meter.setFaceTheme(theme);
        const QImage first = render(meter, QSize(280, 140));
        const QImage second = render(meter, QSize(280, 140));
        const QString id = AetherSDR::analogMeterFaceThemeId(theme);
        expect(meter.faceTheme() == theme && meter.faceThemeId() == id
                   && meter.property("faceTheme").toString() == id,
               QStringLiteral("widget exposes selected %1 theme").arg(id));
        expect(imageDigest(first) == imageDigest(second),
               QStringLiteral("%1 widget render is deterministic").arg(id));
        themedFaces.push_back(first);

        const QImage parked = [&meter, theme]() {
            meter.setLevel(-127.0f);
            meter.setFaceTheme(theme);
            return render(meter, QSize(280, 140));
        }();
        meter.setLevel(-13.0f);
        const QImage fullScale = render(meter, QSize(280, 140));
        expect(imageDigest(parked) != imageDigest(fullScale),
               QStringLiteral("%1 needle moves across the calibrated sweep").arg(id));
        meter.setLevel(-73.0f);
    }
    expect(imageDigest(themedFaces.at(0)) != imageDigest(themedFaces.at(1))
               && imageDigest(themedFaces.at(1)) != imageDigest(themedFaces.at(2))
               && imageDigest(themedFaces.at(0)) != imageDigest(themedFaces.at(2)),
           "all three physical S-meter themes remain visibly distinct");
    saveProofFromEnvironment("AETHER_S_METER_CLASSIC_PROOF", themedFaces.at(0));
    saveProofFromEnvironment("AETHER_S_METER_UPLIGHT_PROOF", themedFaces.at(1));
    saveProofFromEnvironment("AETHER_S_METER_DARK_PROOF", themedFaces.at(2));

    meter.setFaceTheme(AetherSDR::AnalogMeterFaceTheme::DarkRoomUplight);
    const QImage detachedWide = render(meter, QSize(560, 140));
    const QImage detachedLarge = render(meter, QSize(560, 280));
    expect(detachedWide.pixelColor(555, 70).alpha() == 255
               && detachedLarge.pixelColor(555, 140).alpha() == 255,
           "physical face background fills resized detached windows");
    expect(imageDigest(detachedWide) != imageDigest(detachedLarge),
           "physical face cache rebuilds for detached-window resize");

    meter.setFaceTheme(AetherSDR::AnalogMeterFaceTheme::AetherDefault);

    const QImage rx = render(meter, QSize(280, 140));
    expect(changedPixelCount(rx) > 500, "RX face renders visible scale content");

    meter.setTransmitting(true);
    meter.setTxMode(QStringLiteral("Power"));
    meter.setPowerScale(120, false);
    meter.setTxMeters(60.0f, 1.5f);
    const QImage power = render(meter, QSize(280, 140));

    meter.setTxMode(QStringLiteral("SWR"));
    const QImage swr = render(meter, QSize(280, 140));

    meter.setTxMode(QStringLiteral("Level"));
    meter.setMicMeters(-10.0f, 0.0f, -8.0f, 5.0f);
    const QImage level = render(meter, QSize(280, 140));

    meter.setTxMode(QStringLiteral("Compression"));
    const QImage compression = render(meter, QSize(280, 140));

    expect(imageDigest(power) != imageDigest(swr), "Power and SWR faces render differently");
    expect(imageDigest(swr) != imageDigest(level), "SWR and Level faces render differently");
    expect(imageDigest(level) != imageDigest(compression),
           "Level and Compression faces render differently");

    const QImage minimumRender = render(meter, QSize(200, 100));
    const QImage enlargedRender = render(meter, QSize(560, 280));
    expect(changedPixelCount(minimumRender) > 250, "minimum-size face renders");
    expect(changedPixelCount(enlargedRender) > 1000, "enlarged face renders");

    if (g_failures == 0) {
        std::cout << "All standard S-meter geometry checks passed\n";
    }
    return g_failures == 0 ? 0 : 1;
}
