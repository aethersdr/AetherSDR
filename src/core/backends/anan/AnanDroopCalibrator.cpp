#include "core/backends/anan/AnanDroopCalibrator.h"

#include "models/PanadapterModel.h"
#include "models/RadioModel.h"

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

    QVariantMap byRate;
    for (auto it = m_measuredTables.constBegin(); it != m_measuredTables.constEnd(); ++it) {
        QVariantList list;
        list.reserve(static_cast<int>(it.value().size()));
        for (const float v : it.value())
            list.append(static_cast<double>(v));
        byRate.insert(QString::number(it.key()), list);
    }
    // Fire-and-forget (requestId 0): completes locally, same as
    // Hl2Backend's freqcal.set -- no device round trip to await.
    m_radio->invokeBackendExtension(QStringLiteral("anan"), QStringLiteral("droop.apply"),
                                    0, QVariant(byRate));
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
            m_lastSampleAtMs = 0;   // capture immediately on the first Sampling tick
        }
        break;

    case Phase::Sampling: {
        if (!m_haveLatestFrame)
            break;
        if (m_lastSampleAtMs != 0 && now - m_lastSampleAtMs < kSampleSpacingMs)
            break;
        m_lastSampleAtMs = now;
        m_pendingCaptures.push_back(m_latestFrame);
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
