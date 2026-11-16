#include "AetherPDMeasurementModel.h"
#include "AetherPDRunLease.h"
#include "AetherPDModel.h"
#include "BandPlanManager.h"
#include "core/AppSettings.h"
#include "MeterModel.h"
#include "RadioModel.h"
#include "SliceModel.h"
#include "TransmitModel.h"
#include "core/TxCoordinator.h"
#include "core/txlinearity/AetherPDCaptureSource.h"
#include "core/txlinearity/AetherPDTestClient.h"

#include <QNetworkAccessManager>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCryptographicHash>
#include <QThread>
#include <QStringList>
#include <QtEndian>
#include <bit>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <utility>

namespace AetherSDR {
AetherPDMeasurementModel::AetherPDMeasurementModel(RadioModel& radio)
    : m_radio(&radio), m_telemetry(&radio.meterModel()), m_observer(this)
{
    initialize(nullptr);
}
AetherPDMeasurementModel::AetherPDMeasurementModel(RadioModel& radio, QNetworkAccessManager& network)
    : m_radio(&radio), m_telemetry(&radio.meterModel()), m_observer(this)
{
    initialize(&network);
}
void AetherPDMeasurementModel::initialize(QNetworkAccessManager* network)
{
    m_network = network;
    m_injectedNetwork = network != nullptr;
    std::unique_ptr<txlin::AetherPDCaptureSource> source = network
        ? std::make_unique<txlin::AetherPDCaptureSource>(*network)
        : std::make_unique<txlin::AetherPDCaptureSource>();
    m_capture = source.get();
    m_controller = std::make_unique<txlin::TxLinearityController>(std::move(source),
        txlin::TxLinearityController::Environment{
            [] { return TxCoordinator::monotonicMs(); },
            [this] { return current(); },
            [this](std::int64_t now) { return m_telemetry.read(now); }});
    // The authorization ceiling includes helper startup. Confirmed release
    // remains a separate state; cleanup requests never imply physical RF-off.
    txlin::SafetyLimits limits;
    limits.maxTransmitMs = txlin::TxLinearityController::kMaxTransmitMs;
    // FLEX-8600 firmware 4.3.16 publishes these vitals about every 2.06 s
    // (its meter declarations report fps=0). Leave power/SWR at 2 s.
    limits.vitalsMaxAgeMs = 3000;
    m_controller->setSafetyLimits(limits, true);
    m_controller->setCallbacks([this] { publish(); }, [this](const txlin::LinearityRunResult& measured) {
        m_result = Result{measured, m_config, m_request, m_serial, m_mode, m_antenna,
                          m_powerSetting, QDateTime::currentDateTimeUtc(), m_measurementSourceId};
        publish();
    });
    connect(m_radio, &RadioModel::infoChanged, this, &AetherPDMeasurementModel::poll);
    connect(&m_radio->transmitModel(), &TransmitModel::rfPowerChanged, this, &AetherPDMeasurementModel::poll);
    connect(&m_radio->transmitModel(), &TransmitModel::powerProvenanceChanged, this, &AetherPDMeasurementModel::poll);
    connect(&m_radio->aetherPDModel(), &AetherPDModel::changed, this, &AetherPDMeasurementModel::poll);
    connect(&m_radio->meterModel(), &MeterModel::txMetersChanged, this, &AetherPDMeasurementModel::poll);
    const auto watchTxSlice = [this](SliceModel* slice) {
        connect(slice, &SliceModel::txSliceChanged, this, &AetherPDMeasurementModel::poll);
    };
    for (SliceModel* slice : m_radio->slices()) { watchTxSlice(slice); }
    connect(m_radio, &RadioModel::sliceAdded, this, [this, watchTxSlice](SliceModel* slice) {
        watchTxSlice(slice);
        poll();
    });
    connect(m_radio, &RadioModel::sliceRemoved, this, &AetherPDMeasurementModel::poll);
    m_observer.setInterval(25);
    connect(&m_observer, &QTimer::timeout, this, &AetherPDMeasurementModel::poll);
}
AetherPDMeasurementModel::~AetherPDMeasurementModel()
{
    m_shuttingDown = true;
    blockSignals(true);
    m_observer.stop();
    // Destroy the controller while its telemetry and original-session fence
    // still exist. Its destructor releases the exact lease before capture.
    m_controller.reset();
}

bool AetherPDMeasurementModel::current() const
{
    if (m_shuttingDown || QThread::currentThread() != thread() || !m_radio) { return false; }
    const std::optional<aetherpd::Status> source = m_radio->aetherPDModel().status();
    // Unkey cancels the source. Completed frames keep their identity while the
    // exact operation releases; an ended source cannot reopen capture or key.
    const bool retiring = m_controller && (state() == State::Releasing || state() == State::Analyzing);
    return source && source->measurement && m_measurementSourceId
        && source->measurement->sourceId == m_measurementSourceId
        && source->sourcePurpose == aetherpd::SourcePurpose::Measurement
        && (source->calibrationState == aetherpd::CalibrationState::Armed
            || source->calibrationState == aetherpd::CalibrationState::Running || retiring)
        && m_slice && (!m_operatorCurrent || m_operatorCurrent())
        && (!m_bandPlanName || (*m_bandPlanName == AppSettings::instance().value("BandPlanName", "ARRL (US)").toString()
            && m_licenseClass == AppSettings::instance().value("OperatorLicenseClass", "").toString()))
        && (!m_injectedNetwork || m_network) && m_current && m_current()
        && m_radio->controlSessionCurrent(m_session) && !m_radio->fullDuplexEnabled()
        && m_radio->txSlice() == m_slice && m_slice->isTxSlice() && m_slice->frequency() == m_frequencyMHz
        && m_slice->mode() == m_mode && m_slice->txAntenna() == m_antenna
        && m_radio->transmitModel().haveTransmitStatus() && m_radio->transmitModel().rfPowerIsFromRadio()
        && m_radio->transmitModel().rfPower() == m_powerSetting;
}

bool AetherPDMeasurementModel::prepare(const RadioControlSession& session,
    const txlin::LinearityRunConfig& config, const txlin::RunRequest& request, bool dummyLoadConfirmed)
{
    if (m_shuttingDown || QThread::currentThread() != thread() || !m_radio || !dummyLoadConfirmed || state() != State::Idle
        || !m_radio->controlSessionCurrent(session) || m_radio->fullDuplexEnabled()
        || (m_injectedNetwork && (!m_network || m_network->thread() != thread()))) { return false; }
    const std::optional<aetherpd::Status> source = m_radio->aetherPDModel().status();
    if (!source || !source->measurementControl || !source->measurement || !source->measurement->sourceId
        || source->sourcePurpose != aetherpd::SourcePurpose::Measurement || source->keyed
        || source->calibrationState != aetherpd::CalibrationState::Armed
        || config.capture.sampleRateHz != 48000
        || std::abs(request.tone1Hz - (config.capture.centerFreqHz + 710)) > 0.5
        || std::abs(request.tone2Hz - (config.capture.centerFreqHz + 1900)) > 0.5
        || !std::any_of(request.allocations.begin(), request.allocations.end(), [&](const txlin::AllocationRange& band) {
            return band.lowHz <= config.capture.centerFreqHz - 4500 && band.highHz >= config.capture.centerFreqHz + 7500;
        })) { return false; }
    SliceModel* slice = m_radio->txSlice();
    if (!slice || !slice->isTxSlice() || !std::isfinite(slice->frequency())
        || !std::isfinite(config.capture.centerFreqHz) || !std::isfinite(config.maxForwardWatts)
        || m_radio->transmitModel().rfPower() < 0
        || m_radio->transmitModel().rfPower() > config.maxForwardWatts
        || std::abs(slice->frequency() * 1e6 - config.capture.centerFreqHz) > 0.5
        || !m_radio->transmitModel().haveTransmitStatus() || !m_radio->transmitModel().rfPowerIsFromRadio()) {
        return false;
    }
    const std::function<bool()> bound = m_radio->bindAetherPDCapture(session, *m_capture);
    if (!bound || !m_capture->setMeasurementSource(source->measurement->sourceId)) { return false; }
    const quint64 generation = ++m_generation;
    for (const QMetaObject::Connection& connection : m_stationConnections) { disconnect(connection); }
    m_stationConnections = {
        connect(slice, &SliceModel::frequencyChanged, this, &AetherPDMeasurementModel::poll),
        connect(slice, &SliceModel::modeChanged, this, &AetherPDMeasurementModel::poll),
        connect(slice, &SliceModel::txAntennaChanged, this, &AetherPDMeasurementModel::poll),
        connect(slice, &QObject::destroyed, this, &AetherPDMeasurementModel::poll)};
    m_operatorCurrent = {};
    m_bandPlanName.reset();
    m_session = session;
    m_current = bound;
    m_measurementSourceId = source->measurement->sourceId;
    m_slice = slice;
    m_frequencyMHz = slice->frequency();
    m_mode = slice->mode();
    m_antenna = slice->txAntenna();
    m_serial = m_radio->serial();
    m_powerSetting = m_radio->transmitModel().rfPower();
    m_config = config;
    m_request = request;
    m_notice.clear();
    m_result.reset();
    m_controller->confirmDummyLoad();
    const QPointer<AetherPDMeasurementModel> alive(this);
    const bool accepted = m_controller->prepare(config, request);
    if (!alive || generation != m_generation) { return false; }
    if (state() != State::Idle) { m_observer.start(); }
    publish();
    return alive && generation == m_generation && accepted;
}

bool AetherPDMeasurementModel::prepareCurrent(const RadioControlSession& session,
    double maxForwardWatts, bool dummyLoadConfirmed)
{
    if (QThread::currentThread() != thread() || !m_radio || !m_radio->txSlice()) { return false; }
    const std::optional<aetherpd::Status> source = m_radio->aetherPDModel().status();
    if (!source) { return false; }
    const AetherPDMeasurementSettings::ReadResult preferences = analysisOptions();
    if (!preferences.usable) { m_notice = preferences.reason; publish(); return false; }
    txlin::LinearityRunConfig config;
    config.capture.centerFreqHz = m_radio->txSlice()->frequency() * 1e6;
    config.capture.sampleRateHz = 48000;
    config.targetAverages = preferences.values.averages;
    config.window = preferences.values.window;
    config.imd.maxOrder = preferences.values.maxOrder;
    config.maxForwardWatts = maxForwardWatts;
    config.expectedCorrection = source->correctionEnabled;
    txlin::RunRequest request;
    request.maxOrder = preferences.values.maxOrder;
    request.tone1Hz = config.capture.centerFreqHz + 710;
    request.tone2Hz = config.capture.centerFreqHz + 1900;
    const QString planName = AppSettings::instance().value("BandPlanName", "ARRL (US)").toString();
    BandPlanManager plan;
    plan.loadPlans();
    const QString license = AppSettings::instance().value("OperatorLicenseClass", "").toString();
    for (const BandPlanManager::Region& region : plan.contiguousRegionsForBand(0, 1000, license)) {
        request.allocations.push_back({region.lowMhz * 1e6, region.highMhz * 1e6});
    }
    const QPointer<AetherPDMeasurementModel> alive(this);
    const bool accepted = prepare(session, config, request, dummyLoadConfirmed);
    if (alive && accepted) { m_bandPlanName = planName; m_licenseClass = license; }
    return alive && accepted;
}

AetherPDMeasurementSettings::ReadResult AetherPDMeasurementModel::analysisOptions() const
{
    if (!m_radio || QThread::currentThread() != thread()) {
        return {false, {}, QStringLiteral("Original radio is unavailable.")};
    }
    return AetherPDMeasurementSettings(m_radio->settingsScope()).load();
}

bool AetherPDMeasurementModel::setAnalysisOptions(const RadioControlSession& session, const AnalysisOptions& options)
{
    if (m_shuttingDown || QThread::currentThread() != thread() || !m_radio
        || !m_radio->controlSessionCurrent(session) || state() != State::Idle) { return false; }
    const QPointer<AetherPDMeasurementModel> alive(this);
    QString reason;
    const bool saved = AetherPDMeasurementSettings(m_radio->settingsScope()).save(options, reason);
    if (!alive) { return false; }
    m_notice = reason;
    publish();
    return alive && saved;
}

QByteArray AetherPDMeasurementModel::exportResultCsv() const
{
    return m_result ? m_result->toCsv() : QByteArray{};
}

QString AetherPDMeasurementModel::Result::sourceSummary() const
{
    if (measurement.captures.empty() || !measurement.captures.front().sourceStream) {
        return tr("DAX IQ stream identity not reported");
    }
    return tr("DAX IQ stream 0x%1").arg(measurement.captures.front().sourceStream->streamId, 8, 16, QLatin1Char('0'));
}

QByteArray AetherPDMeasurementModel::Result::toCsv() const
{
    QByteArray output("Measurement,Value,Unit,Reference,Status\r\n");
    const auto row = [&output](const QString& name, const QString& value, const QString& unit = {},
                             const QString& reference = {}, const QString& status = {}) {
        QStringList fields{name, value, unit, reference, status};
        for (QString& field : fields) {
            field.replace(u'"', QStringLiteral("\"\""));
            field = u'"' + field + u'"';
        }
        output += fields.join(u',').toUtf8() + "\r\n";
    };
    const auto text = [](QString value) {
        // Keep externally supplied metadata literal when opened in a spreadsheet.
        const QString trimmed = value.trimmed();
        if (!trimmed.isEmpty() && QStringLiteral("=+-@").contains(trimmed.front())) {
            value.prepend(u'\'');
        }
        return value;
    };
    const auto number = [](double value) {
        return std::isfinite(value) ? QString::number(value, 'g', 17) : QString{};
    };
    row(QStringLiteral("Schema"), QStringLiteral("aetherpd-linearity-csv-v1"));
    row(QStringLiteral("Radio serial"), text(serial));
    row(QStringLiteral("Completed UTC"), completedAtUtc.toString(Qt::ISODateWithMs));
    row(QStringLiteral("Source"), QStringLiteral("local-dax-iq"));
    row(QStringLiteral("Measurement source ID"), QStringLiteral("%1").arg(sourceId, 16, 16, QLatin1Char('0')));
    row(QStringLiteral("Mode"), text(mode));
    row(QStringLiteral("Transmit antenna"), text(transmitAntenna));
    row(QStringLiteral("Center frequency"), number(config.capture.centerFreqHz), QStringLiteral("Hz"));
    row(QStringLiteral("Sample rate"), number(config.capture.sampleRateHz), QStringLiteral("Hz"));
    row(QStringLiteral("Nominal power setting"), QString::number(powerSetting), QStringLiteral("W"), QStringLiteral("radio setting; not measured output"));
    row(QStringLiteral("Forward-power abort limit"), number(config.maxForwardWatts), QStringLiteral("W"));
    row(QStringLiteral("Window"), QString::fromStdString(txlin::windowKindKey(config.window)));
    row(QStringLiteral("Correction active"), measurement.correctionActive
        ? (*measurement.correctionActive ? QStringLiteral("yes") : QStringLiteral("no")) : QString{}, {}, {},
        measurement.correctionActive ? QStringLiteral("known") : QStringLiteral("unknown"));
    row(QStringLiteral("Absolute power and ADC headroom"), {}, {}, {}, QStringLiteral("uncalibrated"));
    for (const txlin::LinearityCaptureStamp& stamp : measurement.captures) {
        const QString id = text(QString::fromStdString(stamp.id));
        row(QStringLiteral("Capture ID"), id);
        row(QStringLiteral("Capture IQ stream ID"), stamp.sourceStream
            ? QStringLiteral("0x%1").arg(stamp.sourceStream->streamId, 8, 16, QLatin1Char('0')) : QString{},
            {}, id, stamp.sourceStream ? QStringLiteral("reported") : QStringLiteral("unknown"));
        row(QStringLiteral("Capture IQ generation"), stamp.sourceStream
            ? QString::number(stamp.sourceStream->generation) : QString{},
            {}, id, stamp.sourceStream ? QStringLiteral("reported") : QStringLiteral("unknown"));
    }
    if (!measurement.error.empty()) {
        row(QStringLiteral("Run error"), text(QString::fromStdString(measurement.error)), {}, {}, QStringLiteral("invalid"));
    }
    if (!measurement.measurement) {
        row(QStringLiteral("Measurement valid"), QStringLiteral("no"), {}, {}, QStringLiteral("unavailable"));
        return output;
    }
    const txlin::LinearityResult& result = *measurement.measurement;
    row(QStringLiteral("Measurement valid"), result.valid ? QStringLiteral("yes") : QStringLiteral("no"));
    row(QStringLiteral("Capture clipped"), result.clippingKnown
        ? (result.clipped ? QStringLiteral("yes") : QStringLiteral("no")) : QString{}, {}, {},
        result.clippingKnown ? QStringLiteral("known") : QStringLiteral("unknown"));
    if (!result.valid) {
        row(QStringLiteral("Invalid reason"), text(QString::fromStdString(result.invalidReason)), {}, {}, QStringLiteral("invalid"));
        return output;
    }
    row(QStringLiteral("Averages"), QString::number(result.averages));
    row(QStringLiteral("Tone spacing"), number(result.toneSpacingHz), QStringLiteral("Hz"));
    row(QStringLiteral("Tone imbalance"), number(result.toneImbalanceDb), QStringLiteral("dB"));
    row(QStringLiteral("Fundamental-derived PEP"), number(result.pepDb), QStringLiteral("dB raw IQ"), QStringLiteral("fundamental amplitudes"));
    row(QStringLiteral("Sampled PEP"), result.sampledPepDb ? number(*result.sampledPepDb) : QString{},
        QStringLiteral("dB raw IQ"), QStringLiteral("captured envelope peak"), result.sampledPepDb ? QStringLiteral("measured") : QStringLiteral("unavailable"));
    row(QStringLiteral("Instrument noise floor"), number(result.noiseFloorDb), QStringLiteral("dB raw IQ"), QStringLiteral("per analysis bin"));
    const auto ratios = [&row, &number](const QString& name, double perTone, double pep,
                                      const std::optional<double>& sampledPep, bool noiseLimited,
                                      const QString& band = {}) {
        const QString status = noiseLimited ? QStringLiteral("noise limited") : QStringLiteral("measured");
        row(name, noiseLimited ? QString{} : number(perTone), QStringLiteral("dBc"), QStringLiteral("one tone") + band, status);
        row(name, noiseLimited ? QString{} : number(pep), QStringLiteral("dBc"), QStringLiteral("fundamental-derived PEP") + band, status);
        row(name, !noiseLimited && sampledPep ? number(*sampledPep) : QString{}, QStringLiteral("dBc"),
            QStringLiteral("sampled envelope PEP") + band, noiseLimited ? status : sampledPep ? status : QStringLiteral("unavailable"));
    };
    for (const txlin::ImdProduct& product : result.products) {
        ratios(QStringLiteral("IMD%1 %2").arg(product.order).arg(product.upper ? QStringLiteral("upper") : QStringLiteral("lower")),
            product.dbcPerTone, product.dbcPep, product.dbcSampledPep, product.limitedByNoiseFloor);
    }
    const auto band = [&row, &ratios, &number](const QString& name, const std::optional<txlin::BandPower>& value) {
        if (!value) {
            row(name, {}, QStringLiteral("dBc"), {}, QStringLiteral("unavailable in capture bandwidth"));
            return;
        }
        ratios(name, value->dbcPerTone, value->dbcPep, value->dbcSampledPep, false,
            QStringLiteral("; %1 to %2 Hz offset").arg(number(value->lowHz), number(value->highHz)));
    };
    band(QStringLiteral("Shoulder lower"), result.shoulderLow);
    band(QStringLiteral("Shoulder upper"), result.shoulderHigh);
    band(QStringLiteral("ACPR lower"), result.acprLow);
    band(QStringLiteral("ACPR upper"), result.acprHigh);
    return output;
}

QByteArray AetherPDMeasurementModel::exportResult() const
{
    if (!m_result) { return {}; }
    const Result& r = *m_result;
    QJsonArray captures;
    for (const txlin::LinearityCaptureStamp& stamp : r.measurement.captures) {
        QByteArray raw(qsizetype(stamp.feedback.size()) * 8, Qt::Uninitialized);
        char* cursor = raw.data();
        for (const std::complex<float>& sample : stamp.feedback) {
            qToLittleEndian(std::bit_cast<std::uint32_t>(sample.real()), cursor);
            qToLittleEndian(std::bit_cast<std::uint32_t>(sample.imag()), cursor + 4);
            cursor += 8;
        }
        captures.append(QJsonObject{{"id", QString::fromStdString(stamp.id)},
            {"timestamp_ns", QString::number(stamp.timestampNs)},
            {"source_stream", stamp.sourceStream ? QJsonValue(QJsonObject{
                {"kind", "local_dax_iq"}, {"stream_id", qint64(stamp.sourceStream->streamId)},
                {"generation", QString::number(stamp.sourceStream->generation)}}) : QJsonValue()},
            {"sample_count", qint64(stamp.feedback.size())},
            {"iq_f32le", QString::fromLatin1(raw.toBase64())},
            {"sha256", QString::fromLatin1(QCryptographicHash::hash(raw, QCryptographicHash::Sha256).toHex())}});
    }
    QJsonArray allocations;
    for (const txlin::AllocationRange& band : r.request.allocations) {
        allocations.append(QJsonObject{{"low_hz", band.lowHz}, {"high_hz", band.highHz}});
    }
    QJsonObject out{{"schema", "aetherpd-linearity-v1"}, {"raw_iq_format", "f32le-iq-v1"}, {"radio_serial", r.serial},
        {"completed_at_utc", r.completedAtUtc.toString(Qt::ISODateWithMs)},
        {"source", "local-dax-iq"}, {"source_id", QStringLiteral("%1").arg(r.sourceId, 16, 16, QLatin1Char('0'))},
        {"mode", r.mode}, {"transmit_antenna", r.transmitAntenna},
        {"nominal_power_setting_w", r.powerSetting}, {"forward_power_limit_w", r.config.maxForwardWatts},
        {"center_hz", r.config.capture.centerFreqHz}, {"sample_rate_hz", r.config.capture.sampleRateHz},
        {"requested_tone1_hz", r.request.tone1Hz}, {"requested_tone2_hz", r.request.tone2Hz},
        {"window", QString::fromStdString(txlin::windowKindKey(r.config.window))},
        {"requested_averages", r.config.targetAverages}, {"full_scale_known", r.measurement.fullScaleKnown},
        {"correction_active", r.measurement.correctionActive ? QJsonValue(*r.measurement.correctionActive) : QJsonValue()},
        {"error", QString::fromStdString(r.measurement.error)}, {"captures", captures}, {"allocations", allocations},
        {"limitations", "Raw IQ levels are not calibrated dBFS or watts. Spectral validity does not prove ADC headroom. Timestamps are radio process monotonic receipt times, not FPGA timestamps. Null numbers are unavailable."}};
    if (r.measurement.measurement) {
        const txlin::LinearityResult& m = *r.measurement.measurement;
        const auto optional = [](const std::optional<double>& value) { return value ? QJsonValue(*value) : QJsonValue(); };
        const auto tone = [](const txlin::TonePeak& peak) {
            return QJsonObject{{"valid", peak.valid}, {"offset_hz", peak.freqHz}, {"power_db_raw_iq", peak.powerDb}};
        };
        const auto band = [&optional](const std::optional<txlin::BandPower>& value) -> QJsonValue {
            if (!value) { return {}; }
            return QJsonObject{{"low_offset_hz", value->lowHz}, {"high_offset_hz", value->highHz},
                {"power_db_raw_iq", value->powerDb}, {"dbc_per_tone", value->dbcPerTone},
                {"dbc_fundamental_pep", value->dbcPep}, {"dbc_sampled_pep", optional(value->dbcSampledPep)}};
        };
        QJsonArray products;
        for (const txlin::ImdProduct& product : m.products) {
            products.append(QJsonObject{{"order", product.order}, {"upper", product.upper}, {"offset_hz", product.freqHz},
                {"power_db_raw_iq", product.powerDb}, {"dbc_per_tone", product.dbcPerTone},
                {"dbc_fundamental_pep", product.dbcPep}, {"dbc_sampled_pep", optional(product.dbcSampledPep)},
                {"noise_limited", product.limitedByNoiseFloor}});
        }
        QJsonArray spectrum;
        for (float value : m.spectrumDb) { spectrum.append(double(value)); }
        out["measurement"] = QJsonObject{{"valid", m.valid}, {"invalid_reason", QString::fromStdString(m.invalidReason)},
            {"clipping_known", m.clippingKnown}, {"clipped", m.clipped}, {"fft_size", m.fftSize},
            {"bin_hz", m.binHz}, {"averages", m.averages}, {"tone1", tone(m.tone1)}, {"tone2", tone(m.tone2)},
            {"tone_imbalance_db", m.toneImbalanceDb}, {"fundamental_pep_db_raw_iq", m.pepDb},
            {"sampled_pep_db_raw_iq", optional(m.sampledPepDb)}, {"noise_floor_db_raw_iq_per_bin", m.noiseFloorDb},
            {"products", products}, {"shoulder_low", band(m.shoulderLow)}, {"shoulder_high", band(m.shoulderHigh)},
            {"acpr_low", band(m.acprLow)}, {"acpr_high", band(m.acprHigh)}, {"spectrum_db_raw_iq", spectrum}};
    }
    return QJsonDocument(out).toJson(QJsonDocument::Indented);
}

QString AetherPDMeasurementModel::preparationSummary() const
{
    if (state() == State::Idle) { return {}; }
    const std::optional<aetherpd::CaptureSourceIdentity> source = m_capture ? m_capture->receiveSourceIdentity() : std::nullopt;
    const QString identity = source ? tr("DAX IQ stream 0x%1").arg(source->streamId, 8, 16, QLatin1Char('0'))
        : state() == State::CheckingReceive ? tr("Checking DAX IQ stream identity") : tr("DAX IQ stream identity not reported");
    return tr("%1 MHz · %2 · %3 · nominal setting %4 W · abort limit %5 W\n%6 · %7 averages · through IMD%8\n%9")
        .arg(m_config.capture.centerFreqHz / 1e6, 0, 'f', 6).arg(m_mode).arg(m_antenna)
        .arg(m_powerSetting).arg(m_config.maxForwardWatts, 0, 'f', 1)
        .arg(QString::fromStdString(txlin::windowKindLabel(m_config.window))).arg(m_config.targetAverages).arg(m_config.imd.maxOrder).arg(identity);
}

QString AetherPDMeasurementModel::resultSummary() const
{
    if (!m_result) { return tr("No completed measurement. Results appear after confirmed transmit release."); }
    const Result& r = *m_result;
    QString text = tr("%1 MHz · %2 · nominal setting %3 W · %4\n")
        .arg(r.config.capture.centerFreqHz / 1e6, 0, 'f', 6).arg(r.transmitAntenna).arg(r.powerSetting)
        .arg(r.measurement.correctionActive ? (*r.measurement.correctionActive ? tr("corrected") : tr("uncorrected")) : tr("correction unknown"));
    text += r.sourceSummary() + QLatin1Char('\n');
    if (!r.measurement.error.empty()) { text += QString::fromStdString(r.measurement.error) + QLatin1Char('\n'); }
    if (r.measurement.measurement) {
        const txlin::LinearityResult& m = *r.measurement.measurement;
        if (!m.valid) { text += tr("Invalid: %1\n").arg(QString::fromStdString(m.invalidReason)); }
        else {
            text += tr("%1 averages · tone imbalance %2 dB\nIMD: dBc per tone / fundamental-derived PEP\n")
                .arg(m.averages).arg(m.toneImbalanceDb, 0, 'f', 2);
            for (const txlin::ImdProduct& product : m.products) {
                text += tr("IMD%1 %2: %3 / %4%5\n").arg(product.order)
                    .arg(product.upper ? tr("upper") : tr("lower"))
                    .arg(product.dbcPerTone, 0, 'f', 1).arg(product.dbcPep, 0, 'f', 1)
                    .arg(product.limitedByNoiseFloor ? tr(" (noise limited)") : QString{});
            }
        }
    }
    return text + tr("ADC headroom unknown; absolute power and raw IQ scale are uncalibrated.");
}

bool AetherPDMeasurementModel::start(const RadioControlSession& session, std::function<bool()> operatorCurrent)
{
    if (m_shuttingDown || QThread::currentThread() != thread() || session != m_session
        || state() != State::Ready || !current() || (operatorCurrent && !operatorCurrent())) { return false; }
    m_operatorCurrent = std::move(operatorCurrent);
    std::unique_ptr<txlin::AetherPDTestClient> helper;
    if (m_injectedNetwork) {
        helper = std::make_unique<txlin::AetherPDTestClient>(*m_network, [] { return TxCoordinator::monotonicMs(); });
    }
    const quint64 generation = m_generation;
    const QPointer<AetherPDMeasurementModel> original(this);
    const std::shared_ptr<AetherPDRunLease> lease = AetherPDRunLease::create(*m_radio, m_session, std::move(helper),
        [original, generation] { return original && original->m_generation == generation && original->current(); });
    if (!lease) { m_notice = QStringLiteral("Original measurement helper binding is unavailable"); publish(); return false; }
    const QPointer<AetherPDMeasurementModel> alive(this);
    const bool accepted = m_controller->startRun(lease);
    if (!alive || generation != m_generation) { return false; }
    if (!accepted && !lease->error().isEmpty()) { m_notice = lease->error(); }
    publish();
    return alive && generation == m_generation && accepted;
}

bool AetherPDMeasurementModel::stop(const RadioControlSession& session)
{
    if (m_shuttingDown || QThread::currentThread() != thread() || session != m_session) { return false; }
    const quint64 generation = m_generation;
    const QPointer<AetherPDMeasurementModel> alive(this);
    m_controller->stop();
    if (alive && generation == m_generation) { publish(); }
    return true;
}

void AetherPDMeasurementModel::poll()
{
    if (m_shuttingDown || QThread::currentThread() != thread()) { return; }
    const QPointer<AetherPDMeasurementModel> alive(this);
    m_controller->poll();
    if (alive && state() != m_publishedState) { publish(); }
}

AetherPDMeasurementModel::State AetherPDMeasurementModel::state() const { return m_controller->state(); }
QString AetherPDMeasurementModel::status() const
{
    return !m_notice.isEmpty() ? m_notice : QString::fromStdString(m_controller->status());
}
void AetherPDMeasurementModel::publish()
{
    m_publishedState = state();
    if (m_publishedState == State::Idle) { m_observer.stop(); }
    emit changed();
}
} // namespace AetherSDR
