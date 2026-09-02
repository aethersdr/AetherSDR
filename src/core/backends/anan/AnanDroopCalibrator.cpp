#include "core/backends/anan/AnanDroopCalibrator.h"

#include "core/backends/IRadioBackend.h"
#include "models/PanadapterModel.h"
#include "models/RadioModel.h"

#include "core/AppSettings.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QVariantList>
#include <QVariantMap>

#include <algorithm>
#include <cmath>

Q_LOGGING_CATEGORY(lcAnanDroopCal, "aether.anan.droopcal", QtWarningMsg)

namespace AetherSDR {

namespace {

float medianOf(std::vector<float> values)
{
    if (values.empty())
        return 0.0f;
    std::sort(values.begin(), values.end());
    const std::size_t n = values.size();
    const std::size_t mid = n / 2;
    if (n % 2 == 1)
        return values[mid];
    return 0.5f * (values[mid - 1] + values[mid]);
}

}  // namespace

AnanDroopCalibrator::AnanDroopCalibrator(RadioModel* radio, QObject* parent)
    : QObject(parent), m_radio(radio)
{
    m_pollTimer.setInterval(kPollIntervalMs);
    connect(&m_pollTimer, &QTimer::timeout, this, &AnanDroopCalibrator::advance);
}

// ---- pure math --------------------------------------------------------

AnanDroopCalibrator::Curve AnanDroopCalibrator::medianPowerCurve(const QVector<Curve>& captures)
{
    Curve result{};
    if (captures.isEmpty())
        return result;
    // Floor is a log10(0) guard only, not a plausible-signal floor -- real
    // edge-droop measurements on this radio run as low as -160 to -180 dBm
    // (bench-confirmed), whose linear power (1e-16 to 1e-18) is smaller than
    // a naively "tiny" 1e-12 floor. That floor previously clamped every
    // deep-droop bin up to exactly -120 dB, destroying the very signal this
    // function exists to measure. 1e-30 (-300 dB) is far below anything this
    // radio's ADC can produce, so it only ever guards the literal-zero case.
    static constexpr float kLog10Floor = 1.0e-30f;
    std::vector<float> powers(static_cast<std::size_t>(captures.size()));
    for (std::size_t k = 0; k < result.size(); ++k) {
        for (int c = 0; c < captures.size(); ++c)
            powers[static_cast<std::size_t>(c)] = std::pow(10.0f, captures[c][k] / 10.0f);
        result[k] = 10.0f * std::log10(std::max(medianOf(powers), kLog10Floor));
    }
    return result;
}

float AnanDroopCalibrator::referenceLevel(const Curve& curve, float windowFraction)
{
    const int n = static_cast<int>(curve.size());
    const int halfWidth = std::max(1, static_cast<int>(n * windowFraction / 2.0f));
    const int center = n / 2;
    const int lo = std::max(0, center - halfWidth);
    const int hi = std::min(n, center + halfWidth);
    std::vector<float> window(curve.begin() + lo, curve.begin() + hi);
    return medianOf(std::move(window));
}

anan::DroopCorrectionTable AnanDroopCalibrator::computeCorrection(
    const Curve& curve, float referenceDb, float capDb)
{
    anan::DroopCorrectionTable table{};
    for (std::size_t k = 0; k < curve.size(); ++k)
        table[k] = std::clamp(referenceDb - curve[k], 0.0f, capDb);
    return table;
}

// ---- persistence --------------------------------------------------------

QMap<int, anan::DroopCorrectionTable> AnanDroopCalibrator::loadTables(
    const RadioSettingsScope& scope)
{
    QMap<int, anan::DroopCorrectionTable> tables;
    if (!scope.isValid())
        return tables;
    const QJsonObject doc = scope.feature(QLatin1String(kFeature));
    for (auto it = doc.constBegin(); it != doc.constEnd(); ++it) {
        bool okRate = false;
        const int rateKsps = it.key().toInt(&okRate);
        const QJsonArray arr = it.value().toArray();
        if (!okRate || arr.size() != static_cast<int>(anan::kDroopCorrectionFftSize))
            continue;   // malformed entry -- skip, do not corrupt the rest
        anan::DroopCorrectionTable table{};
        for (int i = 0; i < arr.size(); ++i)
            table[static_cast<std::size_t>(i)] = static_cast<float>(arr[i].toDouble());
        tables.insert(rateKsps, table);
    }
    return tables;
}

QString AnanDroopCalibrator::saveTables(const RadioSettingsScope& scope,
                                       const QMap<int, anan::DroopCorrectionTable>& tables)
{
    if (tables.isEmpty())
        return QStringLiteral("no valid correction table to save");
    if (!scope.isValid())
        return QStringLiteral("no settings scope for this radio");

    int storedSchema = 0;
    // featureExact(), not feature(): this is a WRITER judging the row it is
    // about to replace, and feature()'s exact-radio -> family-wide fallback
    // would silently fold a shared default into this radio's own row
    // (PR #4614 review). An absent row reads back as schema 0.
    QJsonObject doc = scope.featureExact(QLatin1String(kFeature), &storedSchema);
    if (storedSchema > kSchemaVersion) {
        return QStringLiteral("stored calibration is schema v%1, newer than this "
                              "build understands (v%2) -- refusing to overwrite it")
            .arg(storedSchema)
            .arg(kSchemaVersion);
    }

    for (auto it = tables.constBegin(); it != tables.constEnd(); ++it) {
        QJsonArray arr;
        for (const float v : it.value())
            arr.append(static_cast<double>(v));
        doc.insert(QString::number(it.key()), arr);
    }

    // setFeature() refuses while the store is not ReadyToSave. Reporting that
    // as success is the #4621 failure shape -- nothing reads a correction
    // table back off the radio, so the operator would only discover it on the
    // next connect, as a droop that quietly returned.
    if (!scope.setFeature(QLatin1String(kFeature), kSchemaVersion, doc))
        return QStringLiteral("the settings store refused the write");
    AppSettings::instance().save();
    return {};
}

// ---- sweep control --------------------------------------------------------

void AnanDroopCalibrator::start()
{
    if (isRunning())
        return;
    if (!m_radio) {
        emit error(QStringLiteral("no radio model"));
        return;
    }
    PanadapterModel* pan = m_radio->activePanadapter();
    if (!pan) {
        qCWarning(lcAnanDroopCal) << "start() with no active panadapter";
        emit error(QStringLiteral("no active panadapter to sweep"));
        return;
    }

    m_originalRateKsps = static_cast<int>(std::lround(pan->bandwidthMhz() * 1000.0));
    m_measuredTables.clear();
    m_pendingCaptures.clear();
    m_haveLatestFrame = false;
    m_rateIdx = 0;

    // BEFORE the tap is connected and before the first rate is requested:
    // every frame this sweep ever sees must be uncorrected.
    setDspBypass(true);

    m_clock.start();
    m_spectrumConn = connect(m_radio, &RadioModel::panFeedSpectrumReady,
                             this, &AnanDroopCalibrator::onSpectrumFrame);
    m_pollTimer.start();
    m_phaseStartedAtMs = m_clock.elapsed();
    m_phase = Phase::WaitingForRateLanded;
    requestCurrentRate();

    emit started();
    emit progress(0, totalRates(), 0);
}

void AnanDroopCalibrator::stop()
{
    if (!isRunning())
        return;
    finishSweep(false);
}

void AnanDroopCalibrator::applyResult()
{
    if (isRunning() || m_measuredTables.isEmpty() || !m_radio)
        return;

    IRadioBackend* backend = m_radio->backend();
    if (!backend) {
        // invokeBackendExtension() is a documented no-op with nothing
        // connected, so this is the exact case that used to print
        // "Applied -- the measured correction is now live and saved" over a
        // radio that had gone away. The measurements are KEPT: reconnecting
        // and pressing Apply again is a real recovery.
        emit error(QStringLiteral("no radio connected -- the measured correction "
                                  "was not applied or saved"));
        emit finished(false);
        return;
    }

    QVariantMap byRate;
    for (auto it = m_measuredTables.constBegin(); it != m_measuredTables.constEnd(); ++it) {
        QVariantList list;
        list.reserve(static_cast<int>(it.value().size()));
        for (const float v : it.value())
            list.append(static_cast<double>(v));
        byRate.insert(QString::number(it.key()), list);
    }

    // Correlated, not fire-and-forget. AnanBackend's droop.apply handler
    // completes LOCALLY -- no device round trip, same as Hl2Backend's
    // freqcal.set -- so it emits its reply synchronously, inside the invoke
    // below, over a same-thread direct connection. Connecting first and
    // reading the flags after the call returns is therefore deterministic
    // rather than a race, and needs no timer: if nothing answered, the seam
    // did not behave as documented and that is itself reportable. The id only
    // has to be unique within that one synchronous window.
    const quint64 requestId = ++m_applyRequestId;
    bool replied = false;
    QString failure;
    const QMetaObject::Connection okConn = connect(
        backend, &IRadioBackend::extensionResult, this,
        [&replied, requestId](quint64 id, const QVariant&) {
            if (id == requestId)
                replied = true;
        });
    const QMetaObject::Connection errConn = connect(
        backend, &IRadioBackend::extensionError, this,
        [&replied, &failure, requestId](quint64 id, const QString& reason) {
            if (id == requestId) {
                replied = true;
                failure = reason;
            }
        });

    m_radio->invokeBackendExtension(QStringLiteral("anan"), QStringLiteral("droop.apply"),
                                    requestId, QVariant(byRate));

    QObject::disconnect(okConn);
    QObject::disconnect(errConn);

    if (!replied)
        failure = QStringLiteral("the backend did not answer the apply request");
    if (!failure.isEmpty()) {
        emit error(failure);
        emit finished(false);
        return;
    }
    emit finished(true);
}

void AnanDroopCalibrator::clear()
{
    if (isRunning())
        return;
    m_measuredTables.clear();
    m_pendingCaptures.clear();
}

// ---- phase state machine --------------------------------------------------

void AnanDroopCalibrator::setDspBypass(bool bypassed)
{
    if (!m_radio)
        return;
    // Fire-and-forget (requestId 0): the handler completes locally and there
    // is nothing to await. AnanBackend also clears the flag on disconnect, so
    // a radio that vanishes mid-sweep -- where this call reaches nothing --
    // still comes back with correction live.
    m_radio->invokeBackendExtension(QStringLiteral("anan"),
                                    QStringLiteral("droop.bypass"), 0, bypassed);
}

void AnanDroopCalibrator::requestCurrentRate()
{
    if (!m_radio)
        return;
    m_radio->setPanBandwidth(currentTargetRateKsps() / 1000.0);
}

int AnanDroopCalibrator::currentTargetRateKsps() const
{
    return (m_rateIdx >= 0 && m_rateIdx < static_cast<int>(kRatesKsps.size()))
        ? kRatesKsps[static_cast<std::size_t>(m_rateIdx)]
        : 0;
}

void AnanDroopCalibrator::finishSweep(bool applied)
{
    QObject::disconnect(m_spectrumConn);
    m_pollTimer.stop();
    // Every exit from a sweep comes through here -- completion, stop(), and
    // abortSweep() alike -- so the correction is restored on all of them.
    // Non-destructive by construction: the tables were hidden, never
    // overwritten, so there is nothing to restore FROM and no window in which
    // a crash could lose them.
    setDspBypass(false);
    if (m_radio && m_originalRateKsps > 0)
        m_radio->setPanBandwidth(m_originalRateKsps / 1000.0);   // best-effort, fire-and-forget
    m_phase = Phase::Idle;
    m_pendingCaptures.clear();
    m_haveLatestFrame = false;
    emit finished(applied);
}

void AnanDroopCalibrator::abortSweep(const QString& reason)
{
    qCWarning(lcAnanDroopCal) << "sweep aborted:" << reason;
    emit error(reason);
    finishSweep(false);
}

void AnanDroopCalibrator::advance()
{
    if (m_phase == Phase::Idle)
        return;
    PanadapterModel* pan = m_radio ? m_radio->activePanadapter() : nullptr;
    if (!pan) {
        abortSweep(QStringLiteral("panadapter disappeared mid-sweep"));
        return;
    }

    const qint64 now = m_clock.elapsed();
    switch (m_phase) {
    case Phase::Idle:
        return;

    case Phase::WaitingForRateLanded: {
        const double targetMhz = currentTargetRateKsps() / 1000.0;
        if (std::abs(pan->bandwidthMhz() - targetMhz) < 1.0e-6) {
            m_phase = Phase::Settling;
            m_phaseStartedAtMs = now;
        } else if (now - m_phaseStartedAtMs >= kRateWaitTimeoutMs) {
            abortSweep(QStringLiteral("rate change to %1 ksps did not land")
                          .arg(currentTargetRateKsps()));
        }
        break;
    }

    case Phase::Settling:
        if (now - m_phaseStartedAtMs >= kPostLandSettleMs) {
            m_phase = Phase::Sampling;
            m_phaseStartedAtMs = now;   // also the stall deadline -- see Sampling
            m_lastSampleAtMs = 0;       // capture immediately on the first Sampling tick
            // Drop whatever frame is in hand. It may predate the settle
            // window, or even the rate change -- frames for the PREVIOUS rate
            // are still in flight when this phase begins, and one of those
            // measured into this rate's table is the cross-rate corruption
            // the whole per-rate keying exists to prevent.
            m_haveLatestFrame = false;
        }
        break;

    case Phase::Sampling: {
        if (!m_haveLatestFrame) {
            // The only unbounded wait in the machine used to be right here.
            if (now - m_phaseStartedAtMs >= kSampleStallTimeoutMs) {
                abortSweep(QStringLiteral("no spectrum frame at %1 ksps for %2 s "
                                          "-- is the panadapter running?")
                              .arg(currentTargetRateKsps())
                              .arg(kSampleStallTimeoutMs / 1000));
            }
            break;
        }
        if (m_lastSampleAtMs != 0 && now - m_lastSampleAtMs < kSampleSpacingMs)
            break;
        m_lastSampleAtMs = now;
        m_phaseStartedAtMs = now;   // progress resets the stall deadline
        m_pendingCaptures.push_back(m_latestFrame);
        // Require a genuinely NEW frame for the next capture. Without this the
        // 8-sample median can be eight copies of one frame whenever the
        // spectrum FPS is below 1000/kSampleSpacingMs, which defeats the
        // outlier rejection medianPowerCurve() exists to provide.
        m_haveLatestFrame = false;
        emit rateSampled(currentTargetRateKsps(), m_pendingCaptures.size());

        const float rateProgress =
            static_cast<float>(m_pendingCaptures.size()) / kSamplesPerRate;
        const int overallPercent = static_cast<int>(
            100.0f * (static_cast<float>(m_rateIdx) + rateProgress) / totalRates());
        emit progress(m_rateIdx, totalRates(), overallPercent);

        if (m_pendingCaptures.size() >= kSamplesPerRate) {
            const Curve curve = medianPowerCurve(m_pendingCaptures);
            const float ref = referenceLevel(curve);
            const auto table = computeCorrection(curve, ref);
            m_measuredTables[currentTargetRateKsps()] = table;
            m_pendingCaptures.clear();
            ++m_rateIdx;
            if (m_rateIdx < totalRates()) {
                m_phase = Phase::WaitingForRateLanded;
                m_phaseStartedAtMs = now;
                m_haveLatestFrame = false;
                requestCurrentRate();
            } else {
                finishSweep(false);   // staged, not confirmed -- caller decides Apply
            }
        }
        break;
    }
    }
}

void AnanDroopCalibrator::onSpectrumFrame(quint32 streamId, const QVector<float>& binsDbm,
                                          qint64 emittedNs)
{
    Q_UNUSED(streamId);
    Q_UNUSED(emittedNs);
    if (m_phase == Phase::Idle)
        return;
    if (binsDbm.size() != static_cast<int>(anan::kDroopCorrectionFftSize))
        return;
    for (int i = 0; i < binsDbm.size(); ++i)
        m_latestFrame[static_cast<std::size_t>(i)] = binsDbm[i];
    m_haveLatestFrame = true;
}

}  // namespace AetherSDR
