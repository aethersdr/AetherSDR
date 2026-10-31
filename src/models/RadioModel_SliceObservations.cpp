#include "RadioModel.h"
#include "SliceModel.h"
#include "core/DigitalVoiceModeRegistry.h"

#include <QDateTime>
#include <QPointer>
#include <QScopeGuard>
#include <QThread>

namespace AetherSDR {

void RadioModel::initializeObservedSlice(SliceModel* slice)
{
    wireSliceObservationsAndTxIntentToBackend(slice);
    wireSliceReceiveIntentsToBackend(slice);
}

void RadioModel::retireObservedSlice(int id, bool keepForeign)
{
    QPointer<SliceModel> retired = slice(id);
    if (retired) { m_slices.removeAll(retired); }
    else { retired = m_staleSlices.take(id); }
    m_ownedSliceIds.remove(id);
    const bool foreignRemoved = !keepForeign && m_foreignSliceOwners.remove(id);
    const QPointer<RadioModel> alive(this);
    const RadioControlSession session = controlSession();
    if (retired) {
        // The lifecycle binding records retirement on this edge exactly once.
        retired->deleteLater();
        emit sliceRemoved(id);
    } else {
        noteSliceRetirement(id);
    }
    if (!alive || session != controlSession()) { return; }
    m_meterModel.setActiveTxSlice(activeTxSliceNum());
    syncDigitalVoiceTxSelection();
    if (!alive || session != controlSession()) { return; }
    if (!keepForeign && (retired || foreignRemoved)) { emit slotOccupancyChanged(id); }
}

void RadioModel::applySliceObservation(const SliceObservation& observation)
{
    if (QThread::currentThread() != thread() || !observation.current()) { return; }
    const int id = observation.sliceId;
    const quint64 revision = ++m_sliceObservationRevision;
    const SliceDelta& values = observation.values;
    const bool removed = values.inUse && !*values.inUse;
    const QPointer<RadioModel> alive(this);
    const RadioControlSession session = controlSession();
    m_activeSliceObservations.insert(id, revision);
    const auto release = qScopeGuard([this, alive, id, revision] {
        if (alive && m_activeSliceObservations.value(id) == revision) {
            m_activeSliceObservations.remove(id);
        }
    });
    const auto current = [&] {
        return alive && session == controlSession() && observation.current()
            && m_activeSliceObservations.value(id) == revision;
    };
    if (removed) {
        const bool modesChanged = m_rawSliceModeLists.remove(id) > 0;
        retireObservedSlice(id, false);
        if (current() && modesChanged) { emit rawSliceModeListsChanged(); }
        return;
    }
    if (observation.reportedModeList
        && m_rawSliceModeLists.value(id) != *observation.reportedModeList) {
        m_rawSliceModeLists.insert(id, *observation.reportedModeList);
        emit rawSliceModeListsChanged();
        if (!current()) { return; }
    }
    if (observation.ownerHandle && values.txSlice.value_or(false)) {
        auto client = m_clientInfoMap.find(*observation.ownerHandle);
        if (client != m_clientInfoMap.end()) {
            if (values.txAntenna) { client->txAntenna = *values.txAntenna; }
            if (values.frequency) { client->txFreqMhz = *values.frequency; }
        }
    }
    bool ownershipChanged = false;
    if (observation.ownerHandle && *observation.ownerHandle != 0) {
        const quint32 owner = *observation.ownerHandle;
        if (owner == clientHandle()) {
            ownershipChanged = !m_ownedSliceIds.contains(id);
            m_ownedSliceIds.insert(id);
            ownershipChanged = m_foreignSliceOwners.remove(id) || ownershipChanged;
        } else {
            const bool changed = m_foreignSliceOwners.value(id) != owner;
            m_foreignSliceOwners.insert(id, owner);
            if (slice(id) || m_staleSlices.contains(id) || changed) {
                retireObservedSlice(id, true);
            }
            if (current() && changed) { emit slotOccupancyChanged(id); }
            return;
        }
    }
    if (m_foreignSliceOwners.contains(id)
        || (!m_ownedSliceIds.isEmpty() && !m_ownedSliceIds.contains(id))) { return; }

    QPointer<SliceModel> s = slice(id);
    bool reclaimed = false;
    const bool creating = !s;
    if (!s) {
        // A partial report cannot materialize a receiver. Frequency may follow
        // presence in a later report; only the radio's in-use observation gates it.
        if (!values.inUse.value_or(false)) {
            if (ownershipChanged) { emit slotOccupancyChanged(id); }
            return;
        }
        if (auto it = m_staleSlices.find(id); it != m_staleSlices.end() && it.value()) {
            s = it.value();
            m_staleSlices.erase(it);
            reclaimed = true;
        } else {
            s = new SliceModel(id, this);
            if (values.squelchLevel) { s->setManualSquelchLevel(*values.squelchLevel); }
            // Forward slice commands to the radio
            connect(s, &SliceModel::commandReady, this, [this, s](const QString& cmd){
                sendSliceCommand(s, cmd);
            });
            // Initial lock observation and selection intents, wired at EVERY
            // construction site rather than only the backend-materialising one.
            initializeObservedSlice(s);
            connect(s, &SliceModel::digitalVoiceSliceDisplaced,
                    this, [this](int sliceId, const QString& previousMode) {
                SliceModel* displaced = slice(sliceId);
                if (!displaced
                    || !DigitalVoiceModeRegistry::modeForRadioMode(
                            displaced->mode()).has_value()) {
                    return;
                }
                QString restoreMode = previousMode.trimmed().toUpper();
                if (restoreMode.isEmpty()
                    || DigitalVoiceModeRegistry::modeForRadioMode(
                        restoreMode).has_value()) {
                    restoreMode = DigitalVoiceModeRegistry::descriptor(
                        DigitalVoiceModeId::DStar).underlyingMode;
                }
                displaced->setMode(restoreMode);
            });
            connect(s, &SliceModel::txSliceChanged, this, [this](bool) {
                m_meterModel.setActiveTxSlice(activeTxSliceNum());
            });

        }
        m_slices.append(s);
        s->setNrsLevelMaximum(backendCapabilities().nrsLevelControl
            .value_or(RadioCapabilities::NrsLevelControl{}).levelMaximum);
        if (!current() || !s || slice(id) != s) { return; }
        s->setRttyMarkDefault(m_rttyMarkDefault);
        if (!current() || !s || slice(id) != s) { return; }
    }
    const auto currentSlice = [&] { return current() && s && slice(id) == s; };
    s->setControlPolicy(m_backend ? m_backend->receiveControlPolicy() : ReceiveControlPolicy::Optimistic);
    s->applyChanges(values);
    if (!currentSlice()) { return; }
    selectSoleValidTxAntennaIfNeeded(s, values.txAntenna.has_value());
    if (!currentSlice()) { return; }
    m_meterModel.setActiveTxSlice(activeTxSliceNum());
    enforceTransmitInhibitForSlice(s);
    if (!currentSlice()) { return; }
    syncDigitalVoiceTxSelection();
    if (!currentSlice()) { return; }
    if (creating) {
        if (!reclaimed) { emit sliceAdded(s); }
        else if (s->isTxSlice() && transmitInhibitMessageForSlice(s).isEmpty()
                 && QDateTime::currentMSecsSinceEpoch() >= m_profileLoadRadioStateWriteHoldUntilMs) {
            requestTxSlice(s);
        }
        if (currentSlice()) { emit slotOccupancyChanged(id); }
        return;
    }
    if (ownershipChanged) {
        emit slotOccupancyChanged(id);
        if (!currentSlice()) { return; }
    }
    if (observation.internalPaPower
        && *observation.internalPaPower > m_transmitModel.maxPowerLevel()) {
        m_transmitModel.setMaxPowerLevel(*observation.internalPaPower);
        if (!currentSlice()) { return; }
    }
    if (isConnected()) {
        const QStringList commands = s->drainPendingCommands();
        for (const QString& command : commands) {
            if (!currentSlice()) { return; }
            sendSliceCommand(s, command);
        }
    }
}

} // namespace AetherSDR
