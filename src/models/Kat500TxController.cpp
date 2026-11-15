#include "Kat500TxController.h"
#include "Kat500Model.h"
#include "RadioModel.h"
#include <QScopedValueRollback>
#include <cmath>
#include <utility>

namespace AetherSDR {
namespace {
int frequencyKhz(RadioModel* radio)
{
    if (!radio) { return 0; }
    const double mhz = radio->transmitModel().transmitFreq();
    return std::isfinite(mhz) && mhz >= 0.001 && mhz < 100.0
        ? static_cast<int>(std::llround(mhz * 1000.0)) : 0;
}
}
Kat500TxController::Kat500TxController(RadioModel* radio, Kat500Model* tuner, QObject* parent)
    : QObject(parent), m_radio(radio), m_tuner(tuner)
{
    m_deadline.setParent(this);
    m_deadline.setObjectName(QStringLiteral("katTxDeadline"));
    m_deadline.setSingleShot(true);
    m_watchdog.setInterval(100);
    connect(&m_deadline, &QTimer::timeout, this, [this] {
        if (m_phase == Phase::Cancelling) {
            m_cancelPending = false;
            stop(tr("Carrier stopped; KAT500 cancellation was not confirmed."), false);
        } else {
            stop(tr("KAT500 tune timed out; carrier stopped."), true);
        }
    });
    connect(&m_watchdog, &QTimer::timeout, this, &Kat500TxController::refresh);
    if (tuner) {
        connect(tuner, &Kat500Model::statusUpdated, this, &Kat500TxController::refresh);
        connect(tuner, &Kat500Model::availabilityChanged, this, &Kat500TxController::refresh);
        connect(tuner, &Kat500Model::operationFinished, this, &Kat500TxController::operationFinished);
        connect(tuner, &Kat500Model::operationFailed, this, [this](const QString& reason) {
            if (m_ownsOperation) { stop(reason, m_phase != Phase::Result && m_phase != Phase::Cancelling); }
        });
        connect(tuner, &Kat500Model::telemetryReceived, this, [this](const QString& command) { telemetry(command); });
        connect(tuner, &Kat500Model::disconnected, this, [this] {
            m_completed = false;
            stop(tr("KAT500 disconnected; carrier stopped."), false);
            emit tunerDisconnected();
        });
    }
    if (radio) {
        connect(radio, &RadioModel::connectionStateChanged, this, [this](bool connected) {
            m_bypassRequested = false;
            if (!connected) { stop(tr("Radio disconnected; tune canceled."), true); }
            refresh();
        });
        connect(radio, &RadioModel::capabilitiesChanged, this, &Kat500TxController::refresh);
        connect(&radio->transmitModel(), &TransmitModel::stateChanged, this, &Kat500TxController::refresh);
        connect(&radio->transmitModel(), &TransmitModel::atuStateChanged, this, &Kat500TxController::refresh);
        connect(&radio->transmitModel(), &TransmitModel::tuneChanged, this, &Kat500TxController::refresh);
    }
    m_watchdog.start();
    refresh();
}
Kat500TxController::~Kat500TxController()
{
    m_deadline.stop(); m_watchdog.stop();
    if (m_tuner) { disconnect(m_tuner, nullptr, this, nullptr); }
    if (m_radio) { disconnect(&m_radio->transmitModel(), nullptr, this, nullptr); }
    m_carrier.stop();
    if (m_tuner && m_searchRequested) { m_tuner->cancelTuneAfterPendingOperation(); }
    else if (m_tuner && m_ownsOperation) { m_tuner->cancelOperation(); }
}
bool Kat500TxController::internalBypassed() const
{
    if (!m_radio || !m_radio->isConnected()) { return false; }
    const TransmitModel& tx = m_radio->transmitModel();
    // The operational bypass status is authoritative. A radio can retain its
    // separate ATU-enabled flag while reporting a bypassed matching network.
    return !tx.hasTuner() || tx.atuStatus() == ATUStatus::Bypass
        || tx.atuStatus() == ATUStatus::ManualBypass;
}
void Kat500TxController::ensureBypass()
{
    if (m_phase != Phase::Bypass || !m_selected || !m_radio || !m_radio->isConnected()) { return; }
    if (internalBypassed()) { m_bypassRequested = false; return; }
    const auto& tx = m_radio->transmitModel();
    if (!m_bypassRequested && !tx.isTransmitting() && !tx.isTuning()) {
        m_bypassRequested = true;
        m_radio->transmitModel().atuBypass();
    }
}
QString Kat500TxController::unavailableReason() const
{
    if (!m_selected || !m_tuner) { return tr("KAT500 pre-tuning requires Auto or Manual mode."); }
    if (!m_radio || !m_radio->isConnected()) { return tr("Connect a radio first."); }
    if (busy()) { return tr("KAT500 operation in progress; click to cancel."); }
    const auto& tx = m_radio->transmitModel();
    if (!m_radio->backendCapabilities().canTransmit || !tx.tuneAvailable()) { return tr("Tune carrier is unavailable for this radio or mode."); }
    if (tx.isTransmitting() || tx.isTuning()) { return tr("Stop transmitting before tuning or saving a match."); }
    const QString reason = m_tuner->unavailableReason(true);
    if (!reason.isEmpty()) { return reason; }
    const auto& status = m_tuner->status();
    if (!status.mode || status.mode == Kat500::Mode::Bypass || m_tuner->fieldAgeMs("MD") < 0
        || m_tuner->fieldAgeMs("MD") > 15000) { return tr("Waiting for fresh KAT500 Auto or Manual mode readback."); }
    if (status.faultCode != 0) { return tr("Clear the KAT500 fault before tuning."); }
    if (!Kat500::isSupportedFrequency(frequencyKhz(m_radio))) { return tr("KAT500 requires a TX frequency from 1800 to 54000 kHz."); }
    return {};
}
bool Kat500TxController::matchContextCurrent() const
{
    if (!m_tuner || !m_tuner->isResponding() || frequencyKhz(m_radio) != m_frequencyKhz) { return false; }
    const auto& status = m_tuner->status();
    return status.powerOn == true && status.serial == m_matchContext.serial
        && status.mode == m_matchContext.mode && status.antenna == m_matchContext.antenna
        && status.bypassed == m_matchContext.bypassed && status.side == m_matchContext.side
        && status.capacitorMask == m_matchContext.capacitorMask && status.inductorMask == m_matchContext.inductorMask;
}
bool Kat500TxController::matched() const
{
    return m_selected && m_completed && m_tuner
        && matchContextCurrent() && m_tuner->status().faultCode == 0
        && m_tuner->status().swr.has_value() && frequencyKhz(m_radio) == m_frequencyKhz;
}
bool Kat500TxController::originalIntentValid() const
{
    if (!m_radio) { return false; }
    const auto& tx = m_radio->transmitModel();
    if (m_phase != Phase::Carrier && (tx.isTransmitting() || tx.isTuning())) { return false; }
    return m_selected && m_radio && m_radio->isConnected() && m_tuner && m_tuner->isResponding()
        && m_tuner->status().powerOn == true
        && m_tuner->status().mode && m_tuner->status().mode != Kat500::Mode::Bypass
        && m_tuner->status().serial == m_serial && m_tuner->status().faultCode == 0
        && frequencyKhz(m_radio) == m_frequencyKhz && m_carrier.valid()
        && m_carrier.belongsTo(m_radio);
}
void Kat500TxController::refresh()
{
    if (m_refreshing) { return; }
    QScopedValueRollback guard(m_refreshing, true);
    const bool previouslySelected = m_selected;
    if (!m_tuner || !m_tuner->isConnected()) { m_selected = false; }
    else if (m_tuner->isResponding() && m_tuner->status().mode) {
        // Physical BYP can be a successful match in Auto; only MD selects the route.
        m_selected = m_tuner->status().mode != Kat500::Mode::Bypass;
    }
    if (m_completed && !matchContextCurrent()) { m_completed = false; }
    if (previouslySelected != m_selected) { m_bypassRequested = false; m_completed = false; }
    if (busy() && m_phase != Phase::Cancelling && m_phase != Phase::Result) {
        if (!originalIntentValid() || (m_phase == Phase::Carrier &&
            (!m_radio->transmitModel().isTuning() || !internalBypassed()))) {
            stop(tr("Tune canceled: device, radio, frequency or input authorization changed."), true);
        }
    }
    if (m_phase == Phase::Result && (!m_selected || !m_tuner || !m_tuner->isConnected()
        || m_tuner->status().serial != m_serial || frequencyKhz(m_radio) != m_frequencyKhz)) {
        stop(tr("Tune result discarded: matching context changed."), false);
    }
    ensureBypass();
    if (m_phase == Phase::Bypass && internalBypassed()) { recallFrequency(); }
    if (m_phase == Phase::Cancelling) { pollCancellation(); }
    publish();
}
void Kat500TxController::publish()
{
    const Kat500::Status status = m_tuner ? m_tuner->status() : Kat500::Status{};
    const Presentation current{m_selected, busy(), matched(), m_tuner && m_tuner->isResponding(),
                               status.mode, status.bypassed, m_tuner && m_tuner->ampKeyBlockConfirmed(),
                               unavailableReason(), m_message};
    if (m_presentation == current) { return; }
    m_presentation = current;
    emit changed();
}
void Kat500TxController::setPhase(Phase phase, int timeoutMs)
{
    m_phase = phase;
    if (timeoutMs > 0) { m_deadline.start(timeoutMs); } else { m_deadline.stop(); }
    publish();
}
bool Kat500TxController::start(const TxController::Input& carrier)
{
    if (!unavailableReason().isEmpty() || !carrier.valid() || !carrier.belongsTo(m_radio)) { return false; }
    m_carrier = carrier;
    m_frequencyKhz = frequencyKhz(m_radio); m_serial = m_tuner->status().serial;
    m_completed = false;
    m_searchRequested = false; m_cancelPending = false;
    m_message = tr("Waiting for internal ATU bypass confirmation.");
    setPhase(Phase::Bypass, 3000);
    m_bypassRequested = false;
    ensureBypass();
    if (internalBypassed()) { recallFrequency(); }
    return busy();
}
void Kat500TxController::recallFrequency()
{
    if (!originalIntentValid()) { stop(tr("Tune input expired."), false); return; }
    m_message = tr("Selecting the KAT500 TX frequency.");
    setPhase(Phase::Frequency, 5000); m_ownsOperation = true;
    if (!m_tuner->recallFrequency(m_frequencyKhz)) { stop(tr("KAT500 frequency request was refused."), false); }
}
void Kat500TxController::beginSearch()
{
    if (!originalIntentValid() || !internalBypassed()) { stop(tr("Tune preflight changed."), false); return; }
    m_message = tr("Requesting a fresh KAT500 tune.");
    setPhase(Phase::Search, 5000); m_ownsOperation = true; m_searchRequested = true;
    const QPointer<Kat500TxController> self(this);
    if (!m_tuner->startTune(true, [self] {
        return self && self->m_phase == Phase::Search && self->originalIntentValid() && self->internalBypassed();
    })) { stop(tr("KAT500 tune request was refused."), true); }
}
void Kat500TxController::operationFinished()
{
    if (!m_ownsOperation) { return; }
    m_ownsOperation = false;
    switch (m_phase) {
    case Phase::Frequency: beginSearch(); break;
    case Phase::Search:
        if (!originalIntentValid() || !internalBypassed() || m_tuner->status().tuning != true) {
            stop(tr("KAT500 did not confirm a pending tune."), true); return;
        }
        m_message = tr("KAT500 tuning; click to cancel.");
        // TransmitModel::startTune publishes local Tune state synchronously for every
        // backend, before dispatch; backend status later reconciles that state.
        setPhase(Phase::Carrier, 30000);
        if (!m_carrier.start()) { stop(tr("Radio refused the tune carrier."), true); }
        break;
    case Phase::Result:
        m_matchContext = m_tuner->status();
        stop(tr("KAT500 tune completed; result readback confirmed."), false);
        m_completed = true;
        publish();
        break;
    case Phase::Cancelling: m_searchRequested = false; stop(tr("KAT500 tune canceled."), false); break;
    default: break;
    }
}
void Kat500TxController::telemetry(const QString& command)
{
    if (command == "FT" && m_phase == Phase::Carrier) {
        m_searchRequested = false;
        stop(tr("KAT500 tune completed; waiting for result readback."), false);
        setPhase(Phase::Result, 5000);
        m_ownsOperation = true;
        if (!m_tuner->readMatchStatus()) { stop(tr("KAT500 tune result could not be read."), false); }
    }
    publish();
}
void Kat500TxController::stop(const QString& reason, bool cancelSearch)
{
    const bool owns = std::exchange(m_ownsOperation, false);
    const bool search = m_searchRequested;
    m_phase = Phase::Idle; m_deadline.stop(); m_message = reason;
    const auto carrier = std::exchange(m_carrier, {});
    carrier.stop();
    if (owns && m_tuner) { m_tuner->cancelOperation(); }
    m_cancelPending = cancelSearch && search && m_tuner && m_tuner->isConnected();
    if (m_cancelPending) { setPhase(Phase::Cancelling, 5000); pollCancellation(); }
    else { m_searchRequested = false; }
    publish();
}
void Kat500TxController::pollCancellation()
{
    if (!m_cancelPending || !m_tuner || !m_tuner->isConnected() || m_tuner->status().serial != m_serial) { return; }
    if (m_tuner->operationBusy()) { return; }
    m_ownsOperation = true;
    if (m_tuner->cancelTune()) { m_cancelPending = false; }
    else { m_ownsOperation = false; }
}
void Kat500TxController::cancel()
{
    if (m_phase == Phase::Cancelling) { return; }
    stop(tr("Tune canceled; carrier stopped."), true);
}
} // namespace AetherSDR
