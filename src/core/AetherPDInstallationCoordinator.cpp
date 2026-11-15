#include "AetherPDInstallationCoordinator.h"
#include "AetherPDInstallRequestClient.h"
#include "AetherPDInstallationMonitor.h"
#include "AetherPDProtocol.h"
#include "AetherPDTransportPolicy.h"
#include "core/backends/AetherPDCarrierUpload.h"
#include "models/RadioModel.h"

#include <QCryptographicHash>
#include <QDir>
#include <QRandomGenerator>
#include <QThread>
#include <QtEndian>
#include <limits>
#include <utility>

namespace AetherSDR {
namespace {
QByteArray digest(const QByteArray& bytes) { return QCryptographicHash::hash(bytes, QCryptographicHash::Blake2b_256); }
QByteArray randomBytes()
{
    QByteArray bytes(32, '\0');
    do {
        for (int i = 0; i < 32; i += 4) { qToBigEndian(QRandomGenerator::system()->generate(), bytes.data() + i); }
    } while (bytes == QByteArray(32, '\0'));
    return bytes;
}
constexpr qint64 kLifetimeMs = 500'000;
constexpr qint64 kCleanupMs = 60'000;
}
std::optional<AetherPDInstallationCoordinator::Prepared> AetherPDInstallationCoordinator::prepare(
    const AetherPDPackage& package, const QString& serial)
{
    const AetherPDInstallUpload::Inputs inputs{package.m_worker, package.plan(serial), package.m_candidate, package.m_previous};
    const auto plan = aetherpd::parseInstallationPlan(inputs[1]);
    if (!plan || plan->profile != 1 || inputs[0].size() <= 184 || inputs[0].size() > 184 + 512 * 1024
        || inputs[2].size() < 432 || inputs[2].size() > 18 * 1024 * 1024
        || inputs[3].size() < 432 || inputs[3].size() > 18 * 1024 * 1024
        || digest(inputs[2]).toHex() != plan->candidateDigest.toLatin1()
        || digest(inputs[3]).toHex() != plan->previousDigest.toLatin1()) { return std::nullopt; }
    const auto attempt = aetherpd::installationAttempt(inputs[1], randomBytes());
    if (!attempt) { return std::nullopt; }
    Prepared prepared;
    prepared.m_started = std::make_shared<std::atomic_bool>(false);
    prepared.m_inputs = inputs;
    prepared.m_inputs[1] = *attempt;
    QByteArray receipt("PDDELIV1");
    receipt.resize(16);
    qToBigEndian<quint32>(64, receipt.data() + 8);
    qToBigEndian<quint32>(2, receipt.data() + 12);
    receipt += digest(inputs[0]) + digest(*attempt);
    prepared.m_privateConfig = QByteArray("PDXFER01") + randomBytes() + receipt;
    const auto publicBytes = AetherPDInstallRequestClient::publicRecord(prepared.m_privateConfig, *attempt);
    const auto image = publicBytes ? aetherpd::buildCarrierImage(package.m_code, *publicBytes, package.m_licenses) : std::nullopt;
    if (!image) { return std::nullopt; }
    prepared.m_image = *image;
    return prepared;
}
AetherPDInstallationCoordinator::AetherPDInstallationCoordinator(RadioModel& model, QObject* parent)
    : QObject(parent), m_model(&model), m_ownedCarrier(makeAetherPDCarrierUpload(model)), m_carrier(m_ownedCarrier.get()),
      m_request(std::make_unique<AetherPDInstallRequestClient>()), m_upload(std::make_unique<AetherPDInstallUpload>()),
      m_monitor(std::make_unique<AetherPDInstallationMonitor>()) { initialize(); }
AetherPDInstallationCoordinator::AetherPDInstallationCoordinator(RadioModel& model, QNetworkAccessManager& network,
    AetherPDInstallChannel& channel, AetherPDCarrierUpload& carrier, Clock clock, QObject* parent)
    : QObject(parent), m_model(&model), m_carrier(&carrier),
      m_request(std::make_unique<AetherPDInstallRequestClient>(network, clock)),
      m_upload(std::make_unique<AetherPDInstallUpload>(channel, clock)),
      m_monitor(std::make_unique<AetherPDInstallationMonitor>(network, clock)), m_now(std::move(clock)) { initialize(); }
void AetherPDInstallationCoordinator::initialize()
{
    m_elapsed.start();
    if (!m_now) { m_now = [this] { return m_elapsed.elapsed(); }; }
    m_timer.setInterval(100);
    connect(&m_timer, &QTimer::timeout, this, &AetherPDInstallationCoordinator::poll);
    connect(m_model, &RadioModel::connectionStateChanged, this, [this] { poll(); });
    connect(m_model, &QObject::destroyed, this, [this] { m_model = nullptr; poll(); });
    connect(m_model, &RadioModel::waveformContainersChanged, this, [this] { poll(); });
    connect(m_carrier, &AetherPDCarrierUpload::finished, this, &AetherPDInstallationCoordinator::carrierFinished);
    connect(m_carrier, &QObject::destroyed, this, [this] { m_carrier = nullptr; poll(); });
    connect(m_request.get(), &AetherPDInstallRequestClient::observed, this,
        [this](AetherPDInstallRequestClient::State state, int) {
            using R = AetherPDInstallRequestClient::State;
            if (m_state == State::Finished) { return; }
            if (!current()) { end(false); return; }
            if (state == R::Missing || state == R::Unavailable || state == R::DeliveryFinished) { m_receiverRetired = true; }
            if (m_state == State::Requesting && (state == R::AwaitingInputs || state == R::Running)) { startCarrier(); }
            else if (m_state == State::Requesting && m_receiverRetired) { clean(Outcome::Unconfirmed); }
        });
    connect(m_request.get(), &AetherPDInstallRequestClient::failed, this,
        [this](AetherPDInstallRequestClient::Reason reason) {
            if (m_state == State::Finished || m_state == State::Idle) { return; }
            using R = AetherPDInstallRequestClient::Reason;
            if (reason == R::Unauthorized || reason == R::TransportDestroyed || reason == R::InvalidClock) {
                clean(Outcome::Unconfirmed);
            }
        });
    connect(m_upload.get(), &AetherPDInstallUpload::finished, this,
        [this](AetherPDInstallUpload::State, AetherPDInstallUpload::Reason, int) {
            if (m_state == State::UploadingInputs) { setState(State::WaitingForCommit); }
        });
    connect(m_monitor.get(), &AetherPDInstallationMonitor::finished, this,
        [this](AetherPDInstallationMonitor::State state, AetherPDInstallationMonitor::Reason) {
            if (m_state == State::Cleaning || m_state == State::Finished) { return; }
            using S = AetherPDInstallationMonitor::State;
            clean(state == S::Committed ? Outcome::Committed : state == S::RolledBack ? Outcome::RolledBack : Outcome::Unconfirmed);
        });
}
AetherPDInstallationCoordinator::~AetherPDInstallationCoordinator()
{
    stopLocal();
    if (m_model) { m_model->finishFileTransfer(m_reservation, m_carrierStarted ? 5000 : 0); }
}
QString AetherPDInstallationCoordinator::requestId() const { return m_request->requestId(); }
bool AetherPDInstallationCoordinator::current() const
{
    return m_carrier && m_model && m_model->controlSessionCurrent(m_session)
        && m_model->serial() == m_serial && m_model->version() == m_firmware
        && m_model->fileTransferCurrent(m_reservation);
}
bool AetherPDInstallationCoordinator::carrierPresent(bool containersOnly) const
{
    if (!m_model) { return false; }
    for (const auto& entry : m_model->flexWaveformModel().waveforms()) {
        if (entry.name == carrierName() && (!containersOnly || entry.isContainer)) { return true; }
    }
    return false;
}
void AetherPDInstallationCoordinator::setState(State state)
{
    m_state = state;
    QMetaObject::invokeMethod(this, [this] { emit stateChanged(m_state); }, Qt::QueuedConnection);
}
bool AetherPDInstallationCoordinator::begin(const Prepared& approved, const RadioControlSession& session, const QString& token)
{
    if (QThread::currentThread() != thread() || m_state != State::Idle || !m_model
        || m_model->isRadioTransmitting() || !m_carrier || m_model->thread() != thread() || m_carrier->thread() != thread()
        || !m_model->controlSessionCurrent(session) || !aetherpd::validAdminToken(token)
        || !aetherpd::lanAddress(m_model->radioAddress())) { return false; }
    const auto plan = aetherpd::parseInstallationPlan(approved.m_inputs[1]);
    if (!plan || !approved.m_started || approved.used() || !approved.m_inputs[1].startsWith("PDUPGR02") || plan->serial != m_model->serial()
        || plan->firmware != m_model->version() || approved.m_image.archive.isEmpty()) { return false; }
    const qint64 now = m_now();
    if (now < 0 || now > std::numeric_limits<qint64>::max() - kLifetimeMs - kCleanupMs) { return false; }
    m_prepared = approved;
    if (carrierPresent()) { m_prepared = {}; return false; }
    m_reservation = m_model->beginFileTransfer(FileTransferPurpose::DockerWaveform, this, session, 600'000);
    if (!m_reservation) { m_prepared = {}; return false; }
    if (approved.m_started->exchange(true)) {
        m_model->finishFileTransfer(m_reservation); m_reservation.reset(); m_prepared = {}; return false;
    }
    m_session = session;
    m_serial = plan->serial; m_firmware = plan->firmware;
    m_token = token;
    m_lastNow = now;
    m_deadline = now + kLifetimeMs;
    m_file.setFileTemplate(QDir::tempPath() + QStringLiteral("/aetherpd-public-XXXXXX.tar.gz"));
    if (!m_file.open() || m_file.write(approved.m_image.archive) != approved.m_image.archive.size() || !m_file.flush()) {
        end(false); return false;
    }
    m_file.close();
    setState(State::Requesting);
    m_timer.start();
    m_requestStarted = true;
    if (!m_request->begin(m_model->radioAddress(), token, approved.m_privateConfig, approved.m_inputs[1])) {
        m_requestStarted = false; clean(Outcome::Unconfirmed); return false;
    }
    return true;
}
void AetherPDInstallationCoordinator::startCarrier()
{
    if (m_state != State::Requesting || !current()) { return; }
    if (carrierPresent()) { clean(Outcome::Unconfirmed); return; }
    m_carrierStarted = true;
    setState(State::UploadingCarrier);
    m_carrier->begin(m_file.fileName(), m_session, m_reservation);
}
void AetherPDInstallationCoordinator::carrierFinished(bool success)
{
    if (m_state != State::UploadingCarrier) { return; }
    if (!success) { clean(Outcome::Unconfirmed); return; }
    setState(State::WaitingForCarrier);
    poll();
}
void AetherPDInstallationCoordinator::startInputs()
{
    if (!current()) { end(false); return; }
    setState(State::UploadingInputs);
    if (!m_monitor->begin(m_model->radioAddress(), m_token, m_prepared.m_inputs[1], 300'000)) {
        clean(Outcome::Unconfirmed); return;
    }
    if (!m_upload->begin(m_model->radioAddress(), m_prepared.m_privateConfig, m_prepared.m_inputs)) {
        clean(Outcome::Unconfirmed);
    }
}
void AetherPDInstallationCoordinator::cancel()
{
    if (m_state != State::Idle && m_state != State::Finished) { clean(Outcome::Canceled); }
}
void AetherPDInstallationCoordinator::clean(Outcome outcome)
{
    if (m_state == State::Finished || m_state == State::Cleaning) { return; }
    m_outcome = outcome;
    setState(State::Cleaning);
    m_deadline = m_lastNow + kCleanupMs;
    if (m_carrier) { m_carrier->cancel(); }
    m_upload->cancel();
    m_monitor->cancel();
}
void AetherPDInstallationCoordinator::poll()
{
    if (m_state == State::Idle || m_state == State::Finished) { return; }
    const qint64 now = m_now();
    if (now < m_lastNow || now < 0 || now > std::numeric_limits<qint64>::max() - kCleanupMs || !current()) {
        m_outcome = Outcome::Unconfirmed; end(false); return;
    }
    m_lastNow = now;
    const bool present = carrierPresent(true);
    if (m_carrierStarted && present) { m_carrierSeen = true; }
    if (m_state == State::WaitingForCarrier && present) { startInputs(); return; }
    if (m_state == State::Cleaning) {
        if (!m_requestStarted) { m_receiverRetired = true; }
        if (m_requestStarted && !m_receiverRetired && !m_request->busy() && !m_stopSent) {
            m_stopSent = m_request->requestStop();
        }
        if (m_carrierSeen && present && !m_removeSent) {
            m_removeSent = true;
            const QPointer<AetherPDInstallationCoordinator> alive(this);
            m_model->requestWaveform({WaveformAction::RemoveContainer, carrierName()}, this, {});
            if (!alive || m_state == State::Finished) { return; }
        }
        const bool absent = !m_carrierStarted || (m_carrierSeen && m_removeSent && !present);
        if (absent && m_receiverRetired) { end(true); return; }
        if (now >= m_deadline) { end(false); return; }
    } else if (now >= m_deadline) { clean(Outcome::Unconfirmed); return; }
    if (m_requestStarted && !m_receiverRetired && !m_request->busy() && now >= m_nextInspect) {
        m_nextInspect = now + 500;
        m_request->inspect();
    }
}
void AetherPDInstallationCoordinator::stopLocal()
{
    m_timer.stop();
    if (m_carrier) { disconnect(m_carrier.data(), nullptr, this, nullptr); }
    disconnect(m_upload.get(), nullptr, this, nullptr);
    disconnect(m_monitor.get(), nullptr, this, nullptr);
    disconnect(m_request.get(), nullptr, this, nullptr);
    if (m_carrier) { m_carrier->cancel(); }
    m_upload->cancel();
    m_monitor->cancel();
    m_request->abandon();
    m_token.clear();
    m_prepared.m_privateConfig.fill('\0');
    m_prepared.m_privateConfig.clear();
    m_prepared.m_inputs = {};
    m_prepared.m_image.archive.clear();
    if (!m_file.fileName().isEmpty()) { m_file.remove(); }
}
void AetherPDInstallationCoordinator::end(bool cleaned)
{
    setState(State::Finished);
    stopLocal();
    if (m_model) { m_model->finishFileTransfer(m_reservation, cleaned ? 0 : 5000); }
    m_reservation.reset();
    emit finished(m_outcome, cleaned);
}
} // namespace AetherSDR
