#include "AetherPDMeasurementModel.h"
#include "AetherPDRunLease.h"
#include "AetherPDModel.h"
#include "MeterModel.h"
#include "RadioModel.h"
#include "SliceModel.h"
#include "TransmitModel.h"
#include "core/TxCoordinator.h"
#include "core/txlinearity/AetherPDCaptureSource.h"
#include "core/txlinearity/AetherPDTestClient.h"

#include <QNetworkAccessManager>
#include <QThread>
#include <cmath>
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
    m_controller->setCallbacks([this] { publish(); }, [this](const txlin::LinearityRunResult& measured) {
        m_result = Result{measured, m_config, m_request, m_serial, m_mode, m_antenna,
                          m_powerSetting, QDateTime::currentDateTimeUtc()};
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
    return !m_shuttingDown && QThread::currentThread() == thread() && m_radio && m_slice
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
    if (!bound) { return false; }
    const quint64 generation = ++m_generation;
    for (const QMetaObject::Connection& connection : m_stationConnections) { disconnect(connection); }
    m_stationConnections = {
        connect(slice, &SliceModel::frequencyChanged, this, &AetherPDMeasurementModel::poll),
        connect(slice, &SliceModel::modeChanged, this, &AetherPDMeasurementModel::poll),
        connect(slice, &SliceModel::txAntennaChanged, this, &AetherPDMeasurementModel::poll),
        connect(slice, &QObject::destroyed, this, &AetherPDMeasurementModel::poll)};
    m_session = session;
    m_current = bound;
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

bool AetherPDMeasurementModel::start(const RadioControlSession& session)
{
    if (m_shuttingDown || QThread::currentThread() != thread() || session != m_session
        || state() != State::Ready || !current()) { return false; }
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
