#include "AetherPDModel.h"
#include "core/txlinearity/AetherPDCaptureSource.h"
#include "core/txlinearity/AetherPDTestClient.h"

#include <QPointer>
#include <QThread>
#include <memory>
#include <utility>

namespace AetherSDR {
namespace {
constexpr int kStatusFreshnessMs = 3'000;
}

AetherPDModel::AetherPDModel(QObject* parent) : QObject(parent), m_client(this)
{
    initialize();
}

AetherPDModel::AetherPDModel(QNetworkAccessManager& network, QObject* parent)
    : QObject(parent), m_client(network, this)
{
    initialize();
}

void AetherPDModel::initialize()
{
    m_poll.setParent(this);
    m_freshness.setParent(this);
    m_poll.setInterval(1'000);
    m_freshness.setInterval(kStatusFreshnessMs);
    m_freshness.setSingleShot(true);
    connect(&m_poll, &QTimer::timeout, this, [this] {
        if (!m_pending) {
            refresh();
        }
    });
    connect(&m_freshness, &QTimer::timeout, this, [this] {
        invalidateStatus();
        emit changed();
    });
    connect(&m_client, &AetherPDClient::statusReceived, this, &AetherPDModel::onStatus);
    connect(&m_client, &AetherPDClient::acknowledged, this, &AetherPDModel::onAcknowledgment);
    connect(&m_client, &AetherPDClient::failed, this, &AetherPDModel::onFailure);
}

void AetherPDModel::invalidateStatus()
{
    ++m_captureGeneration;
    m_freshness.stop();
    m_status.reset();
    m_statusAge.invalidate();
}

std::optional<aetherpd::Status> AetherPDModel::status() const
{
    return active() && m_statusAge.isValid() && m_statusAge.elapsed() < kStatusFreshnessMs
        ? m_status : std::nullopt;
}

void AetherPDModel::resetSession()
{
    ++m_generation;
    m_active = false;
    m_poll.stop();
    invalidateStatus();
    m_pending.reset();
    m_acknowledgment.reset();
    m_failure.reset();
    m_token.clear();
    m_address.clear();
    m_identity.reset();
    m_expectedIq.reset(); m_observedIq.reset(); m_bindingRejected = false;
    m_current = {};
    m_client.cancelPending();
}

void AetherPDModel::clearSession()
{
    resetSession();
    m_client.setRadioAddress({});
    emit changed();
}

void AetherPDModel::reconcileSession()
{
    if (m_active && !active()) {
        clearSession();
    }
}

void AetherPDModel::setSession(const QHostAddress& address, const QString& adminToken,
                              std::function<bool()> current,
                              std::optional<aetherpd::StartIdentity> identity)
{
    resetSession();
    m_client.setRadioAddress(address);
    m_current = std::move(current);
    m_token = adminToken;
    m_address = address;
    m_identity = std::move(identity);
    m_active = !address.isNull() && !adminToken.isEmpty();
    if (!active()) {
        clearSession();
        return;
    }
    const quint64 generation = m_generation;
    const QPointer<AetherPDModel> guard(this);
    emit changed();
    if (guard && m_generation == generation && m_active) {
        if (refresh() && guard && m_generation == generation && m_active) {
            m_poll.start();
        }
    }
}

bool AetherPDModel::refresh()
{
    return request(aetherpd::Operation::Status);
}

std::function<bool()> AetherPDModel::bindCaptureSource(txlin::AetherPDCaptureSource& source)
{
    if (QThread::currentThread() != thread() || source.thread() != thread()) { return {}; }
    source.clearSession();
    const std::function<bool()> current = measurementFence();
    if (current) {
        const std::optional<aetherpd::Status> observed = status();
        source.setSession(m_address, m_token, current);
        if (observed && observed->feedbackBinding && !source.setFeedbackBinding(*observed->feedbackBinding)) {
            source.clearSession(); return {};
        }
    }
    return current;
}

std::function<bool()> AetherPDModel::bindTestClient(txlin::AetherPDTestClient& client)
{
    if (QThread::currentThread() != thread() || client.thread() != thread()
        || client.state() != txlin::AetherPDTestClient::State::Idle) {
        return {};
    }
    client.clearSession();
    const std::function<bool()> current = measurementFence();
    if (!current || !client.setSession(m_address, m_token, current)) { return {}; }
    return current;
}

std::function<bool()> AetherPDModel::measurementFence()
{
    const std::optional<aetherpd::Status> observed = status();
    if (!active() || !m_current || !m_identity || !aetherpd::startBody(*m_identity)
        || !observed || !observed->available || !observed->registered
        || (observed->feedbackBinding && !observed->feedbackBinding->valid)) {
        return {};
    }
    const QPointer<AetherPDModel> owner(this);
    const quint64 generation = m_generation;
    const quint64 captureGeneration = m_captureGeneration;
    const std::function<bool()> current = [owner, generation, captureGeneration, live = std::make_shared<bool>(true)] {
        if (!owner || QThread::currentThread() != owner->thread()) { return false; }
        if (!*live || owner->m_generation != generation || owner->m_captureGeneration != captureGeneration
            || !owner->active()) { *live = false; return false; }
        const std::optional<aetherpd::Status> latest = owner->status();
        *live = latest && latest->available && latest->registered
            && (!latest->feedbackBinding || latest->feedbackBinding->valid);
        return *live;
    };
    return current;
}

bool AetherPDModel::request(aetherpd::Operation operation, std::optional<quint32> toneSpacingHz,
                            std::optional<aetherpd::IQSelection> selection)
{
    using aetherpd::Operation;
    using Failure = AetherPDClient::Failure;
    if (m_active && !active()) {
        clearSession();
        return false;
    }
    if (!m_active || !aetherpd::request(operation)
        || (toneSpacingHz && (operation != Operation::ArmMeasurement || !aetherpd::measurementToneBody(*toneSpacingHz)))) {
        emit operationFailed(operation, Failure::InvalidRequest);
        return false;
    }
    if (m_pending) {
        const bool preempts = (operation == Operation::Stop && *m_pending != Operation::Stop)
            || ((operation == Operation::Start || operation == Operation::ArmMeasurement
                    || operation == Operation::ArmCalibration || operation == Operation::EnableCorrection
                    || operation == Operation::BypassCorrection)
                && *m_pending == Operation::Status)
            || (operation == Operation::CancelCalibration
                && (*m_pending == Operation::Status || *m_pending == Operation::ArmCalibration || *m_pending == Operation::ArmMeasurement));
        if (!preempts) {
            emit operationFailed(operation, Failure::Busy);
            return false;
        }
    }
    if (selection && (operation != Operation::Start || !selection->valid())) {
        emit operationFailed(operation, Failure::InvalidRequest);
        return false;
    }
    const std::optional<aetherpd::Status> observed = status();
    if (toneSpacingHz && (!observed || !observed->measurementToneControl || !observed->measurementToneConfig)) {
        emit operationFailed(operation, Failure::InvalidRequest);
        return false;
    }
    if (operation == Operation::Start
        && (!m_identity || !aetherpd::startBody(*m_identity) || (observed && observed->available))) {
        emit operationFailed(operation, Failure::InvalidRequest);
        return false;
    }
    if ((operation == Operation::EnableCorrection || operation == Operation::BypassCorrection)
        && (!observed || !observed->available || !observed->registered || !observed->correctionControl
            || observed->keyed || (observed->feedbackBinding && !observed->feedbackBinding->valid)
            || observed->state == aetherpd::State::BlockedByRadioApd
            || observed->calibrationState == aetherpd::CalibrationState::Armed
            || observed->calibrationState == aetherpd::CalibrationState::Running)) {
        emit operationFailed(operation, Failure::InvalidRequest);
        return false;
    }
    if ((operation == Operation::ArmCalibration || operation == Operation::ArmMeasurement)
        && (!observed || !observed->available || !observed->registered
            || observed->keyed || !observed->calibrationControl
            || (operation == Operation::ArmCalibration && !observed->correctionEnabled)
            || (operation == Operation::ArmMeasurement && !observed->measurementControl)
            || (observed->feedbackBinding && !observed->feedbackBinding->valid)
            || observed->state == aetherpd::State::BlockedByRadioApd
            || observed->calibrationState == aetherpd::CalibrationState::Armed
            || observed->calibrationState == aetherpd::CalibrationState::Running)) {
        emit operationFailed(operation, Failure::InvalidRequest);
        return false;
    }
    std::optional<aetherpd::StartIdentity> startIdentity;
    if (operation == Operation::Start) {
        startIdentity = m_identity;
        startIdentity->iq = selection;
        m_expectedIq = selection; m_observedIq.reset(); m_bindingRejected = false;
    }
    m_pending = operation;
    m_failure.reset();
    if (operation != Operation::Status) {
        invalidateStatus();
        m_acknowledgment.reset();
    }
    const QPointer<AetherPDModel> guard(this);
    const bool submitted = m_client.submit(operation, m_token,
        startIdentity, toneSpacingHz);
    if (guard) {
        emit changed();
    }
    return submitted;
}

void AetherPDModel::onStatus(const aetherpd::Status& status)
{
    if (m_active && !active()) {
        clearSession();
        return;
    }
    if (!m_active || m_pending != aetherpd::Operation::Status) {
        return;
    }
    if (status.available) {
        const std::optional<aetherpd::IQSelection> observed = status.feedbackBinding
            ? std::optional<aetherpd::IQSelection>(aetherpd::IQSelection{status.feedbackBinding->channel, status.feedbackBinding->pan}) : std::nullopt;
        if ((m_expectedIq && observed != m_expectedIq) || (m_observedIq && observed != m_observedIq)
            || (status.feedbackBinding && m_identity && status.feedbackBinding->guiHandle != m_identity->guiHandle)
            || (m_bindingRejected && (!status.feedbackBinding || status.feedbackBinding->valid))) {
            m_bindingRejected = true;
            onFailure(aetherpd::Operation::Status, AetherPDClient::Failure::InvalidReply);
            return;
        }
        if (observed) { m_observedIq = observed; }
        if (status.feedbackBinding && status.feedbackBinding->failed) { m_bindingRejected = true; }
    } else if (status.state == aetherpd::State::Stopped || status.state == aetherpd::State::StoppedReceiveUnconfirmed
               || status.state == aetherpd::State::CleanupUnconfirmed) {
        m_expectedIq.reset(); m_observedIq.reset(); m_bindingRejected = false;
    }
    m_pending.reset();
    m_failure.reset();
    if (!status.available || !status.registered
        || (status.feedbackBinding && !status.feedbackBinding->valid)
        || (m_status && m_status->feedbackBinding != status.feedbackBinding)
        || (m_statusAge.isValid() && m_statusAge.elapsed() >= kStatusFreshnessMs)) {
        invalidateStatus();
    }
    m_status = status;
    m_statusAge.start();
    m_freshness.start();
    emit changed();
}

void AetherPDModel::onAcknowledgment(aetherpd::Operation operation,
                                    aetherpd::Acknowledgment acknowledgment)
{
    if (m_active && !active()) {
        clearSession();
        return;
    }
    if (!m_active || m_pending != operation) {
        return;
    }
    m_pending.reset();
    m_acknowledgment = acknowledgment;
    const quint64 generation = m_generation;
    const QPointer<AetherPDModel> guard(this);
    emit changed();
    // An ACK changes only the receipt. Read status without replaying control.
    if (guard && m_active && m_generation == generation && !m_pending) {
        refresh();
    }
}

void AetherPDModel::onFailure(aetherpd::Operation operation, AetherPDClient::Failure failure)
{
    if (m_active && !active()) {
        clearSession();
        return;
    }
    if (!m_active || m_pending != operation) {
        return;
    }
    m_pending.reset();
    m_failure = failure;
    invalidateStatus();
    if (failure == AetherPDClient::Failure::Unauthorized
        || failure == AetherPDClient::Failure::InvalidRequest) {
        m_active = false;
        m_token.clear();
        m_poll.stop();
    }
    const QPointer<AetherPDModel> guard(this);
    emit operationFailed(operation, failure);
    if (guard) {
        emit changed();
    }
}

} // namespace AetherSDR
