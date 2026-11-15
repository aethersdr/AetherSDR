#include "RadioModel.h"
#include "core/LogManager.h"
#include <QDateTime>
#include <QPointer>
#include <QVector>

namespace AetherSDR {

void RadioModel::resetRadioObservations()
{
    ++m_radioObservationGeneration;
    m_clientLifecycles.clear();
    m_liveClientHandles.clear();
    m_retiredClientHandles.clear();
    m_clientHistoryComplete = true;
    m_oscState.clear();
    m_oscSetting = QStringLiteral("auto");
    m_oscLocked = false;
    m_extPresent = false;
    m_gpsdoPresent = false;
    m_tcxoPresent = false;
    m_staticIp.clear();
    m_staticNetmask.clear();
    m_staticGateway.clear();
    m_hasStaticIp = false;
    // Neither the reference nor the static address carries into the next radio.
    const QPointer<RadioModel> alive(this);
    emit oscillatorChanged();
    if (alive) { emit infoChanged(); }
}

void RadioModel::applyRadioObservation(const RadioStatusObservation& observation)
{
    const QPointer<RadioModel> alive(this);
    const RadioControlSession session = controlSession();
    const quint64 generation = m_radioObservationGeneration;
    const quint64 epoch = observation.statusEpoch;
    const auto current = [alive, session, generation, epoch] {
        return alive && alive->observationSessionCurrent(epoch) && alive->controlSession() == session
            && alive->m_radioObservationGeneration == generation;
    };
    if (!current()) { return; }
    const quint32 localHandle = clientHandle();
    switch (observation.kind) {
    case RadioStatusObservation::Kind::Radio:
        applyRadioChanges(observation.radio, current);
        return;
    case RadioStatusObservation::Kind::Oscillator: {
        const OscillatorStatusValues& values = observation.oscillator;
        if (values.state) { m_oscState = *values.state; }
        if (values.setting) { m_oscSetting = *values.setting; }
        if (values.locked) { m_oscLocked = *values.locked; }
        if (values.externalPresent) { m_extPresent = *values.externalPresent; }
        if (values.gpsPresent) { m_gpsdoPresent = *values.gpsPresent; }
        if (values.tcxoPresent) { m_tcxoPresent = *values.tcxoPresent; }
        emit oscillatorChanged();
        if (current()) { emit infoChanged(); }
        return;
    }
    case RadioStatusObservation::Kind::StaticNetwork:
        if (observation.network.ip) { m_staticIp = *observation.network.ip; }
        if (observation.network.netmask) { m_staticNetmask = *observation.network.netmask; }
        if (observation.network.gateway) { m_staticGateway = *observation.network.gateway; }
        m_hasStaticIp = !m_staticIp.isEmpty();
        emit infoChanged();
        return;
    case RadioStatusObservation::Kind::Client:
        break;
    }

    const ClientStatusValues& report = observation.client;
    const quint32 handle = report.handle;
    if (handle == 0) { return; }
    if (report.action == ClientStatusValues::Action::Disconnected) {
        const bool known = m_clientInfoMap.contains(handle);
        m_clientStations.remove(handle);
        m_clientInfoMap.remove(handle);
        m_clientLifecycles.remove(handle);
        m_liveClientHandles.remove(handle);
        m_announcedClientConnections.remove(handle);
        m_startupClientConnections.remove(handle);
        if (m_retiredClientHandles.size() < kMaxRetiredClients) {
            m_retiredClientHandles.insert(handle);
        } else {
            m_clientHistoryComplete = false;
        }
        QVector<int> released;
        bool modesChanged = false;
        for (auto it = m_foreignSliceOwners.begin(); it != m_foreignSliceOwners.end();) {
            if (it.value() == handle) {
                released.append(it.key());
                modesChanged = m_rawSliceModeLists.remove(it.key()) > 0 || modesChanged;
                it = m_foreignSliceOwners.erase(it);
            } else { ++it; }
        }
        // Publish only after every old marker is gone. Callbacks may insert
        // replacement owners; never erase or dereference them after notification.
        for (int sliceId : released) {
            emit slotOccupancyChanged(sliceId);
            if (!current()) { return; }
        }
        if (modesChanged) {
            emit rawSliceModeListsChanged();
            if (!current()) { return; }
        }
        if (known) { emitOtherClientsChanged(); }
        if (!current() || m_clientLifecycles.contains(handle)) { return; }
        if (localHandle != 0 && handle == localHandle) {
            if (report.duplicateClientId) { handleDuplicateClientIdDisconnect(); }
            else if (report.forced) { handleForcedClientDisconnect(); }
        }
        return;
    }

    const bool known = m_clientInfoMap.contains(handle);
    if (!known && (report.action != ClientStatusValues::Action::Connected
        || m_clientInfoMap.size() >= kMaxObservedClients)) { return; }
    ClientInfo client = m_clientInfoMap.value(handle);
    const bool replaced = known && report.clientId && !report.clientId->isEmpty()
        && !client.clientId.isEmpty() && client.clientId != *report.clientId;
    const bool newConnection = !m_liveClientHandles.contains(handle) || replaced;
    if (replaced) {
        client = {};
        m_announcedClientConnections.remove(handle);
        m_startupClientConnections.remove(handle);
    }
    if (report.clientId) { client.clientId = *report.clientId; }
    if (report.program) { client.program = *report.program; }
    if (report.station) { client.station = *report.station; }
    if (report.source) { client.source = *report.source; }
    if (report.localPtt) { client.localPtt = *report.localPtt; }
    // A full "connected" report is the client's whole state: no local_ptt
    // means none (FlexLib Radio.cs ParseClientStatus defaults it false).
    else if (report.action == ClientStatusValues::Action::Connected) { client.localPtt = false; }
    if (!known || replaced) {
        if (!report.program) { client.program = QStringLiteral("Unknown"); }
        if (!report.station) { client.station = client.program; }
    }
    if (client.source.isEmpty() && m_wanConn) { client.source = QStringLiteral("SmartLink"); }
    if (newConnection || !m_clientLifecycles.contains(handle)) {
        m_clientLifecycles[handle] = ++m_clientLifecycleRevision;
    }
    const quint64 lifecycle = m_clientLifecycles.value(handle);
    m_retiredClientHandles.remove(handle);
    m_liveClientHandles.insert(handle);
    const ClientInfo before = m_clientInfoMap.value(handle);
    const bool changed = !known || before.clientId != client.clientId || before.program != client.program
        || before.station != client.station || before.source != client.source || before.localPtt != client.localPtt;
    m_clientStations[handle] = client.station;
    m_clientInfoMap[handle] = client;
    if (newConnection && report.action == ClientStatusValues::Action::Connected && handle != localHandle) {
        m_lastMultiFlexClientConnectMs = QDateTime::currentMSecsSinceEpoch();
    }
    if (changed) { emitOtherClientsChanged(); }
    if (!current() || m_clientLifecycles.value(handle) != lifecycle) { return; }
    if (newConnection && report.action == ClientStatusValues::Action::Connected
        && handle != localHandle && !shouldSuppressClientConnectionNotice(handle)) {
        announceClientConnection(handle, client.source, client.station, client.program);
    }
}
} // namespace AetherSDR
