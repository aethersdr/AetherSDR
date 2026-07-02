#include "KiwiSdrManager.h"

#include "AppSettings.h"
#include "LogManager.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QMetaType>
#include <QThread>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <utility>

namespace AetherSDR {
namespace {

constexpr const char* kKiwiSdrRxAntennasSettingsKey = "KiwiSdrRxAntennas";
constexpr const char* kVirtualAntennaPrefix = "KIWI:";
constexpr int kRecoverableReconnectDelayMs = 3000;
constexpr int kKiwiSdrProfileNameMaxChars = 16;
constexpr int kKiwiSdrWaterfallRateMax = 4;

QString normalizedProfileEndpoint(const QString& endpoint)
{
    return KiwiSdrClient::normalizeEndpoint(endpoint);
}

} // namespace

KiwiSdrManager::KiwiSdrManager(QObject* parent)
    : QObject(parent)
{
    qRegisterMetaType<AetherSDR::KiwiSdrClient::State>(
        "AetherSDR::KiwiSdrClient::State");
    qRegisterMetaType<AetherSDR::KiwiSdrReceiverTelemetry>(
        "AetherSDR::KiwiSdrReceiverTelemetry");
    qRegisterMetaType<AetherSDR::KiwiSdrProtocol::ReceiverMetadata>(
        "AetherSDR::KiwiSdrProtocol::ReceiverMetadata");
    qRegisterMetaType<AetherSDR::KiwiSdrProtocol::ProtocolState>(
        "AetherSDR::KiwiSdrProtocol::ProtocolState");
    qRegisterMetaType<AetherSDR::KiwiSdrProtocol::MeterReading>(
        "AetherSDR::KiwiSdrProtocol::MeterReading");
    qRegisterMetaType<QVector<float>>("QVector<float>");
    loadSettings();
}

KiwiSdrManager::~KiwiSdrManager()
{
    disconnectAll();
    const QStringList ids = m_clients.keys();
    for (const QString& id : ids) {
        destroyClient(id, true);
    }
    if (m_clientThread) {
        m_clientThread->quit();
        if (!m_clientThread->wait(3000)) {
            qCWarning(lcKiwiSdr)
                << "KiwiSDR client thread did not stop during manager teardown";
            m_clientThread->setParent(nullptr);
            connect(m_clientThread, &QThread::finished,
                    m_clientThread, &QObject::deleteLater);
        }
    }
}

KiwiSdrAntennaProfile KiwiSdrManager::profile(const QString& id) const
{
    const int idx = profileIndex(id);
    return idx >= 0 ? m_profiles[idx] : KiwiSdrAntennaProfile{};
}

bool KiwiSdrManager::hasProfile(const QString& id) const
{
    return profileIndex(id) >= 0;
}

QString KiwiSdrManager::displayName(const QString& id) const
{
    const KiwiSdrAntennaProfile p = profile(id);
    if (!p.name.trimmed().isEmpty()) {
        return p.name.trimmed();
    }
    if (!p.endpoint.isEmpty()) {
        return p.endpoint;
    }
    return tr("KiwiSDR");
}

QString KiwiSdrManager::virtualAntennaToken(const QString& id) const
{
    return QStringLiteral("%1%2").arg(QString::fromLatin1(kVirtualAntennaPrefix), id);
}

QString KiwiSdrManager::profileIdForVirtualAntennaToken(const QString& token) const
{
    return token.startsWith(QString::fromLatin1(kVirtualAntennaPrefix))
        ? token.mid(QString::fromLatin1(kVirtualAntennaPrefix).size())
        : QString();
}

QStringList KiwiSdrManager::virtualAntennaTokens() const
{
    QStringList tokens;
    tokens.reserve(m_profiles.size());
    for (const KiwiSdrAntennaProfile& p : m_profiles) {
        tokens.append(virtualAntennaToken(p.id));
    }
    return tokens;
}

QStringList KiwiSdrManager::virtualAntennaLabels() const
{
    QStringList labels;
    labels.reserve(m_profiles.size());
    for (const KiwiSdrAntennaProfile& p : m_profiles) {
        labels.append(displayName(p.id));
    }
    return labels;
}

KiwiSdrClient::State KiwiSdrManager::state(const QString& id) const
{
    return m_states.value(id, KiwiSdrClient::State::Disconnected);
}

QString KiwiSdrManager::stateDetail(const QString& id) const
{
    return m_stateDetails.value(id);
}

KiwiSdrReceiverTelemetry KiwiSdrManager::telemetry(const QString& id) const
{
    return m_telemetry.value(id);
}

KiwiSdrProtocol::ReceiverMetadata KiwiSdrManager::receiverMetadata(
    const QString& id) const
{
    return m_telemetry.value(id).metadata;
}

KiwiSdrProtocol::ProtocolState KiwiSdrManager::protocolState(
    const QString& id) const
{
    return m_telemetry.value(id).protocol;
}

bool KiwiSdrManager::waterfallAvailable(const QString& id) const
{
    return m_waterfallAvailable.value(id, true);
}

QString KiwiSdrManager::waterfallDetail(const QString& id) const
{
    return m_waterfallDetails.value(id);
}

bool KiwiSdrManager::isConnected(const QString& id) const
{
    return KiwiSdrClient::stateHasReceiveAudio(state(id));
}

bool KiwiSdrManager::reconnectRecommended(const QString& id) const
{
    if (state(id) != KiwiSdrClient::State::Waiting) {
        return false;
    }

    const KiwiSdrProtocol::ReceiverMetadata metadata =
        m_telemetry.value(id).metadata;
    return metadata.hasCampQueueReloadRecommended
        && metadata.campQueueReloadRecommended;
}

QString KiwiSdrManager::assignedProfileForSlice(int sliceId) const
{
    return m_sliceAssignments.value(sliceId);
}

int KiwiSdrManager::assignedSliceForProfile(const QString& id) const
{
    for (auto it = m_sliceAssignments.constBegin(); it != m_sliceAssignments.constEnd(); ++it) {
        if (it.value() == id) {
            return it.key();
        }
    }
    return -1;
}

QString KiwiSdrManager::addProfile(const QString& name, const QString& endpoint)
{
    const QString normalizedEndpoint = normalizedProfileEndpoint(endpoint);
    const QString displayName = name.trimmed().left(kKiwiSdrProfileNameMaxChars);
    if (displayName.isEmpty() || normalizedEndpoint.isEmpty()) {
        return QString();
    }

    KiwiSdrAntennaProfile profile;
    profile.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    profile.endpoint = normalizedEndpoint;
    profile.name = displayName;
    m_profiles.append(profile);
    saveSettings();
    qCInfo(lcKiwiSdr).noquote()
        << "Profile added" << profile.name << "endpoint=" << profile.endpoint
        << "id=" << profile.id;
    emit profilesChanged();
    return profile.id;
}

void KiwiSdrManager::updateProfile(const KiwiSdrAntennaProfile& profile)
{
    const int idx = profileIndex(profile.id);
    if (idx < 0) {
        return;
    }

    KiwiSdrAntennaProfile updated = profile;
    updated.endpoint = normalizedProfileEndpoint(profile.endpoint);
    updated.name = sanitizedName(profile.name, updated.endpoint);
    updated.waterfallCellDb = std::clamp(updated.waterfallCellDb, -30, 30);
    updated.waterfallFloorDb = std::clamp(updated.waterfallFloorDb, -30, 30);
    updated.waterfallRate =
        std::clamp(updated.waterfallRate, 0, kKiwiSdrWaterfallRateMax);
    const QString oldEndpoint = m_profiles[idx].endpoint;
    m_profiles[idx] = updated;
    saveSettings();
    const bool endpointChanged = oldEndpoint != updated.endpoint;
    if (endpointChanged) {
        cancelReconnect(updated.id);
        emit profileStreamReset(updated.id);
    }

    if (KiwiSdrClient* c = client(updated.id)) {
        Q_UNUSED(c);
        invokeClient(updated.id, [cellDb = updated.waterfallCellDb,
                                  floorDb = updated.waterfallFloorDb,
                                  rate = updated.waterfallRate,
                                  endpointChanged,
                                  endpoint = updated.endpoint,
                                  reconnect = state(updated.id)
                                      != KiwiSdrClient::State::Disconnected](
                                     KiwiSdrClient* client) {
            client->setWaterfallDisplayAdjustments(cellDb, floorDb);
            client->setWaterfallRateOverride(rate);
            if (endpointChanged && reconnect) {
                client->disconnectFromEndpoint();
                if (!endpoint.isEmpty()) {
                    client->connectToEndpoint(endpoint);
                }
            }
        });
    }
    emit profilesChanged();
}

void KiwiSdrManager::removeProfile(const QString& id)
{
    const int idx = profileIndex(id);
    if (idx < 0) {
        return;
    }

    qCInfo(lcKiwiSdr).noquote() << "Profile removed" << displayName(id) << "id=" << id;
    cancelReconnect(id);
    disconnectProfile(id);
    destroyClient(id);
    if (QTimer* timer = m_reconnectTimers.take(id)) {
        timer->deleteLater();
    }

    const QList<int> assignedSlices = m_sliceAssignments.keys(id);
    for (int sliceId : assignedSlices) {
        m_sliceAssignments.remove(sliceId);
        emit audioSourceEnabledChanged(id, false);
        emit sliceAssignmentChanged(sliceId, QString());
    }

    m_stateDetails.remove(id);
    m_states.remove(id);
    m_clientHasTrackedSlice.remove(id);
    m_telemetry.remove(id);
    m_waterfallAvailable.remove(id);
    m_waterfallDetails.remove(id);
    m_profiles.removeAt(idx);
    saveSettings();
    // The client deletion is already scheduled above, so no further audio will
    // be fed for this id; free its audio-engine source state (disable alone
    // leaves the per-source entry allocated — #3668 review).
    emit audioSourceRemoved(id);
    emit profilesChanged();
}

void KiwiSdrManager::connectProfile(const QString& id)
{
    const int idx = profileIndex(id);
    if (idx < 0) {
        return;
    }

    cancelReconnect(id);
    KiwiSdrClient* c = ensureClient(id);
    if (!c || m_profiles[idx].endpoint.isEmpty()) {
        return;
    }
    const KiwiSdrClient::State currentState = state(id);
    const bool waitingReconnect = reconnectRecommended(id);
    if (currentState == KiwiSdrClient::State::Connecting
        || (currentState == KiwiSdrClient::State::Waiting && !waitingReconnect)
        || KiwiSdrClient::stateHasReceiveAudio(currentState)) {
        return;
    }
    invokeClient(id, [callsign = m_operatorCallsign,
                      cellDb = m_profiles[idx].waterfallCellDb,
                      floorDb = m_profiles[idx].waterfallFloorDb,
                      rate = m_profiles[idx].waterfallRate](
                         KiwiSdrClient* client) {
        client->setOperatorCallsign(callsign);
        client->setWaterfallDisplayAdjustments(cellDb, floorDb);
        client->setWaterfallRateOverride(rate);
    });
    if (assignedSliceForProfile(id) < 0) {
        m_clientHasTrackedSlice.insert(id, false);
        invokeClient(id, [](KiwiSdrClient* client) {
            client->setTrackedSlice(-1, 0.0, QString(), 0, 0, QString());
        });
        emit profileNeedsInitialTracking(id);
    }
    if (!m_clientHasTrackedSlice.value(id, false)) {
        return;
    }
    qCInfo(lcKiwiSdr).noquote()
        << "Connecting" << m_profiles[idx].name
        << "->" << m_profiles[idx].endpoint;
    m_states.insert(id, KiwiSdrClient::State::Connecting);
    invokeClient(id, [endpoint = m_profiles[idx].endpoint](KiwiSdrClient* client) {
        client->connectToEndpoint(endpoint);
    });
}

void KiwiSdrManager::disconnectProfile(const QString& id)
{
    cancelReconnect(id);
    if (KiwiSdrClient* c = client(id)) {
        Q_UNUSED(c);
        qCInfo(lcKiwiSdr).noquote() << "Disconnecting" << displayName(id);
        invokeClient(id, [](KiwiSdrClient* client) {
            client->disconnectFromEndpoint();
        });
    }
    emit audioSourceEnabledChanged(id, false);
}

void KiwiSdrManager::disconnectAll()
{
    for (QTimer* timer : std::as_const(m_reconnectTimers)) {
        if (timer) {
            timer->stop();
        }
    }
    for (auto it = m_clients.constBegin(); it != m_clients.constEnd(); ++it) {
        invokeClient(it.key(), [](KiwiSdrClient* client) {
            client->disconnectFromEndpoint();
        });
    }
}

void KiwiSdrManager::setOperatorCallsign(const QString& callsign)
{
    m_operatorCallsign = callsign;
    for (auto it = m_clients.constBegin(); it != m_clients.constEnd(); ++it) {
        invokeClient(it.key(), [callsign](KiwiSdrClient* client) {
            client->setOperatorCallsign(callsign);
        });
    }
}

void KiwiSdrManager::startAutoConnect()
{
    for (const KiwiSdrAntennaProfile& p : std::as_const(m_profiles)) {
        if (p.autoConnect && !p.endpoint.isEmpty()) {
            connectProfile(p.id);
        }
    }
}

void KiwiSdrManager::primeProfileTracking(const QString& id, int sliceId,
                                          double frequencyMhz,
                                          const QString& mode,
                                          int filterLowHz,
                                          int filterHighHz,
                                          const QString& panId,
                                          double centerMhz,
                                          double bandwidthMhz,
                                          int lineDurationMs)
{
    if (!hasProfile(id) || sliceId < 0 || frequencyMhz <= 0.0) {
        return;
    }

    if (KiwiSdrClient* c = ensureClient(id)) {
        Q_UNUSED(c);
        m_clientHasTrackedSlice.insert(id, true);
        invokeClient(id, [sliceId, frequencyMhz, mode, filterLowHz,
                          filterHighHz, panId, lineDurationMs,
                          centerMhz, bandwidthMhz](KiwiSdrClient* client) {
            client->setTrackedSlice(sliceId, frequencyMhz, mode, filterLowHz,
                                    filterHighHz, panId);
            client->setWaterfallLineDurationMs(lineDurationMs);
            if (!panId.isEmpty() && centerMhz > 0.0 && bandwidthMhz > 0.0) {
                client->setWaterfallView(panId, centerMhz, bandwidthMhz);
            }
        });
    }
}

void KiwiSdrManager::assignSliceToProfile(int sliceId, const QString& profileId,
                                          double frequencyMhz,
                                          const QString& mode,
                                          int filterLowHz, int filterHighHz,
                                          const QString& panId)
{
    if (sliceId < 0 || !hasProfile(profileId)) {
        clearSliceAssignment(sliceId);
        return;
    }

    const QString previousProfile = m_sliceAssignments.value(sliceId);
    if (!previousProfile.isEmpty() && previousProfile != profileId) {
        emit audioSourceEnabledChanged(previousProfile, false);
        if (KiwiSdrClient* previousClient = client(previousProfile)) {
            Q_UNUSED(previousClient);
            invokeClient(previousProfile, [](KiwiSdrClient* client) {
                client->setAudioActive(false);
            });
        }
    }

    const QList<int> otherSlices = m_sliceAssignments.keys(profileId);
    for (int otherSliceId : otherSlices) {
        if (otherSliceId == sliceId) {
            continue;
        }
        m_sliceAssignments.remove(otherSliceId);
        emit sliceAssignmentChanged(otherSliceId, QString());
    }

    m_sliceAssignments.insert(sliceId, profileId);
    qCInfo(lcKiwiSdr).noquote()
        << "Slice" << sliceId << "assigned to" << displayName(profileId)
        << "freq=" << frequencyMhz << "MHz mode=" << mode;
    emit sliceAssignmentChanged(sliceId, profileId);

    // Switching this slice to a different Kiwi: release the previous one if no
    // other slice still uses it and it isn't auto-connect, so we don't squat the
    // receiver's user slot / burn its per-IP time budget (#3950). Must come after
    // the insert() above so shouldMaintainProfileConnection() sees the new map.
    if (!previousProfile.isEmpty() && previousProfile != profileId
        && !shouldMaintainProfileConnection(previousProfile)) {
        disconnectProfile(previousProfile);
    }

    if (KiwiSdrClient* c = ensureClient(profileId)) {
        Q_UNUSED(c);
        m_clientHasTrackedSlice.insert(profileId, sliceId >= 0 && frequencyMhz > 0.0);
        const bool connected =
            KiwiSdrClient::stateHasReceiveAudio(state(profileId));
        invokeClient(profileId, [sliceId, frequencyMhz, mode, filterLowHz,
                                 filterHighHz, panId, connected](
                                    KiwiSdrClient* client) {
            client->setTrackedSlice(sliceId, frequencyMhz, mode, filterLowHz,
                                    filterHighHz, panId);
            client->setAudioActive(connected);
        });
    }
    connectProfile(profileId);
    emit audioSourceEnabledChanged(profileId, true);
}

void KiwiSdrManager::clearSliceAssignment(int sliceId)
{
    const QString previousProfile = m_sliceAssignments.take(sliceId);
    if (previousProfile.isEmpty()) {
        return;
    }

    qCInfo(lcKiwiSdr).noquote()
        << "Slice" << sliceId << "cleared from" << displayName(previousProfile);
    emit audioSourceEnabledChanged(previousProfile, false);
    if (KiwiSdrClient* c = client(previousProfile)) {
        Q_UNUSED(c);
        invokeClient(previousProfile, [](KiwiSdrClient* client) {
            client->setAudioActive(false);
        });
    }
    emit sliceAssignmentChanged(sliceId, QString());

    // The slice no longer uses this Kiwi (antenna reverted to Flex, or the slice
    // was closed). Release it once nothing else needs it, so it stops holding the
    // receiver's user slot / per-IP time budget (#3950). take() above already
    // updated the map, so shouldMaintainProfileConnection() reflects reality here.
    if (!shouldMaintainProfileConnection(previousProfile)) {
        disconnectProfile(previousProfile);
    }
}

void KiwiSdrManager::updateSliceTracking(int sliceId, double frequencyMhz,
                                         const QString& mode,
                                         int filterLowHz, int filterHighHz,
                                         const QString& panId)
{
    const QString profileId = m_sliceAssignments.value(sliceId);
    if (profileId.isEmpty()) {
        return;
    }
    if (KiwiSdrClient* c = ensureClient(profileId)) {
        Q_UNUSED(c);
        m_clientHasTrackedSlice.insert(profileId, sliceId >= 0 && frequencyMhz > 0.0);
        invokeClient(profileId, [sliceId, frequencyMhz, mode,
                                 filterLowHz, filterHighHz, panId](
                                    KiwiSdrClient* client) {
            client->setTrackedSlice(sliceId, frequencyMhz, mode, filterLowHz,
                                    filterHighHz, panId);
        });
    }
}

void KiwiSdrManager::updateWaterfallView(int sliceId, const QString& panId,
                                         double centerMhz, double bandwidthMhz,
                                         int lineDurationMs)
{
    const QString profileId = m_sliceAssignments.value(sliceId);
    if (profileId.isEmpty()) {
        return;
    }
    if (KiwiSdrClient* c = ensureClient(profileId)) {
        Q_UNUSED(c);
        invokeClient(profileId, [panId, centerMhz, bandwidthMhz,
                                 lineDurationMs](KiwiSdrClient* client) {
            client->setWaterfallLineDurationMs(lineDurationMs);
            client->setWaterfallView(panId, centerMhz, bandwidthMhz);
        });
    }
}

void KiwiSdrManager::setReceiverControlsForSlice(
    int sliceId, const KiwiSdrReceiverControls& controls)
{
    const QString profileId = m_sliceAssignments.value(sliceId);
    if (profileId.isEmpty()) {
        return;
    }

    if (KiwiSdrClient* c = ensureClient(profileId)) {
        Q_UNUSED(c);
        invokeClient(profileId, [controls](KiwiSdrClient* client) {
            client->setReceiverControls(controls);
        });
    }
}

void KiwiSdrManager::setProfileWaterfallSettings(const QString& id, int cellDb,
                                                 int floorDb, int rate)
{
    const int idx = profileIndex(id);
    if (idx < 0) {
        return;
    }

    KiwiSdrAntennaProfile p = m_profiles[idx];
    p.waterfallCellDb = std::clamp(cellDb, -30, 30);
    p.waterfallFloorDb = std::clamp(floorDb, -30, 30);
    p.waterfallRate = std::clamp(rate, 0, kKiwiSdrWaterfallRateMax);
    updateProfile(p);
}

KiwiSdrClient* KiwiSdrManager::ensureClient(const QString& id)
{
    if (KiwiSdrClient* existing = client(id)) {
        return existing;
    }

    if (!hasProfile(id)) {
        return nullptr;
    }

    ensureClientThread();
    auto* c = new KiwiSdrClient;
    c->setDecodeAudioWhenInactive(false);
    c->setOperatorCallsign(m_operatorCallsign);
    c->moveToThread(m_clientThread);
    connect(m_clientThread, &QThread::finished, c, &QObject::deleteLater);
    m_clients.insert(id, c);
    m_states.insert(id, KiwiSdrClient::State::Disconnected);
    m_clientHasTrackedSlice.insert(id, false);
    connect(c, &KiwiSdrClient::stateChanged,
            this, [this, id, c](KiwiSdrClient::State state, const QString& detail) {
        if (client(id) != c) {
            return;
        }
        m_states.insert(id, state);
        m_stateDetails.insert(id, detail);
        qCInfo(lcKiwiSdr).noquote()
            << "State" << displayName(id) << "->" << static_cast<int>(state)
            << (detail.isEmpty() ? QString() : QStringLiteral("(") + detail + QStringLiteral(")"));
        if (state == KiwiSdrClient::State::Connecting) {
            m_telemetry.insert(id, {});
            m_waterfallAvailable.insert(id, true);
            m_waterfallDetails.remove(id);
            emit profileTelemetryChanged(id, m_telemetry.value(id));
            emit profileWaterfallAvailabilityChanged(id, true, QString());
        }
        if (KiwiSdrClient::stateHasReceiveAudio(state)) {
            const int idx = profileIndex(id);
            const bool hasAssignedSlice = assignedSliceForProfile(id) >= 0;
            if (idx >= 0 || hasAssignedSlice) {
                const int cellDb = idx >= 0 ? m_profiles[idx].waterfallCellDb : 0;
                const int floorDb = idx >= 0 ? m_profiles[idx].waterfallFloorDb : 0;
                const int rate = idx >= 0 ? m_profiles[idx].waterfallRate : 0;
                const bool normalReceiver =
                    KiwiSdrClient::stateAllowsReceiverControl(state);
                invokeClient(id, [idx, cellDb, floorDb, rate, hasAssignedSlice,
                                  normalReceiver](
                                     KiwiSdrClient* client) {
                    if (idx >= 0 && normalReceiver) {
                        client->setWaterfallDisplayAdjustments(cellDb, floorDb);
                        client->setWaterfallRateOverride(rate);
                    }
                    if (hasAssignedSlice) {
                        client->setAudioActive(true);
                    }
                });
            }
            if (hasAssignedSlice) {
                emit audioSourceEnabledChanged(id, true);
            }
        } else if (state != KiwiSdrClient::State::Connecting) {
            emit audioSourceEnabledChanged(id, false);
            emit meterReadingReady(
                id,
                KiwiSdrProtocol::meterUnavailable(
                    KiwiSdrProtocol::MeterSource::Unknown,
                    detail));
        }
        emit profileStateChanged(id, state, detail);
        scheduleWaitingReconnectIfRecommended(id);
    }, Qt::QueuedConnection);
    connect(c, &KiwiSdrClient::recoverableDisconnect,
            this, [this, id, c](const QString&) {
        if (client(id) != c) {
            return;
        }
        scheduleReconnect(id);
    }, Qt::QueuedConnection);
    connect(c, &KiwiSdrClient::telemetryChanged, this,
            [this, id, c](const KiwiSdrReceiverTelemetry& telemetry) {
        if (client(id) != c) {
            return;
        }
        m_telemetry.insert(id, telemetry);
        emit profileTelemetryChanged(id, telemetry);
        scheduleWaitingReconnectIfRecommended(id);
    }, Qt::QueuedConnection);
    connect(c, &KiwiSdrClient::waterfallAvailabilityChanged,
            this, [this, id, c](bool available, const QString& detail) {
        if (client(id) != c) {
            return;
        }
        m_waterfallAvailable.insert(id, available);
        if (detail.isEmpty()) {
            m_waterfallDetails.remove(id);
        } else {
            m_waterfallDetails.insert(id, detail);
        }
        emit profileWaterfallAvailabilityChanged(id, available, detail);
    }, Qt::QueuedConnection);
    connect(c, &KiwiSdrClient::decodedAudioReady,
            this, [this, id, c](const QByteArray& pcm) {
        if (client(id) != c) {
            return;
        }
        emit decodedAudioReady(id, pcm);
    }, Qt::QueuedConnection);
    connect(c, &KiwiSdrClient::waterfallRowReady,
            this, [this, id, c](const QString& panId, const QVector<float>& binsDbm,
                             double lowFreqMhz, double highFreqMhz,
                             quint32 timecode) {
        if (client(id) != c) {
            return;
        }
        emit waterfallRowReady(id, panId, binsDbm, lowFreqMhz, highFreqMhz,
                               timecode);
    }, Qt::QueuedConnection);
    connect(c, &KiwiSdrClient::meterReadingReady,
            this, [this, id, c](const KiwiSdrProtocol::MeterReading& reading) {
        if (client(id) != c) {
            return;
        }
        emit meterReadingReady(id, reading);
    }, Qt::QueuedConnection);
    return c;
}

KiwiSdrClient* KiwiSdrManager::client(const QString& id) const
{
    return m_clients.value(id, nullptr);
}

void KiwiSdrManager::ensureClientThread()
{
    if (m_clientThread) {
        return;
    }

    m_clientThread = new QThread(this);
    m_clientThread->setObjectName(QStringLiteral("KiwiSdrClients"));
    m_clientThread->start();
}

void KiwiSdrManager::invokeClient(
    const QString& id,
    std::function<void(KiwiSdrClient*)> fn)
{
    KiwiSdrClient* c = client(id);
    if (!c) {
        return;
    }

    QMetaObject::invokeMethod(c, [c, fn = std::move(fn)]() {
        fn(c);
    }, Qt::QueuedConnection);
}

void KiwiSdrManager::destroyClient(const QString& id, bool blocking)
{
    KiwiSdrClient* c = m_clients.take(id);
    if (!c) {
        return;
    }

    c->disconnect(this);
    m_states.remove(id);
    m_stateDetails.remove(id);
    m_clientHasTrackedSlice.remove(id);
    m_telemetry.remove(id);
    m_waterfallAvailable.remove(id);
    m_waterfallDetails.remove(id);
    const Qt::ConnectionType connectionType =
        blocking && c->thread() != QThread::currentThread()
            ? Qt::BlockingQueuedConnection
            : Qt::QueuedConnection;
    QMetaObject::invokeMethod(c, [c]() {
        c->disconnectFromEndpoint();
        c->deleteLater();
    }, connectionType);
}

int KiwiSdrManager::profileIndex(const QString& id) const
{
    for (int i = 0; i < m_profiles.size(); ++i) {
        if (m_profiles[i].id == id) {
            return i;
        }
    }
    return -1;
}

void KiwiSdrManager::loadSettings()
{
    m_profiles.clear();
    const QString raw = AppSettings::instance()
        .value(kKiwiSdrRxAntennasSettingsKey, "{}").toString();
    const QJsonObject root = QJsonDocument::fromJson(raw.toUtf8()).object();
    const QJsonArray profiles = root.value(QStringLiteral("profiles")).toArray();
    for (const QJsonValue& value : profiles) {
        const QJsonObject obj = value.toObject();
        KiwiSdrAntennaProfile p;
        p.id = obj.value(QStringLiteral("id")).toString();
        if (p.id.isEmpty()) {
            p.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        }
        p.endpoint = normalizedProfileEndpoint(
            obj.value(QStringLiteral("endpoint")).toString());
        p.name = sanitizedName(obj.value(QStringLiteral("name")).toString(),
                               p.endpoint);
        p.autoConnect = obj.value(QStringLiteral("autoConnect")).toBool(false);
        p.waterfallCellDb = std::clamp(
            obj.value(QStringLiteral("waterfallCellDb")).toInt(0), -30, 30);
        p.waterfallFloorDb = std::clamp(
            obj.value(QStringLiteral("waterfallFloorDb")).toInt(0), -30, 30);
        p.waterfallRate = std::clamp(
            obj.value(QStringLiteral("waterfallRate")).toInt(0),
            0,
            kKiwiSdrWaterfallRateMax);
        m_profiles.append(p);
    }
}

void KiwiSdrManager::saveSettings() const
{
    QJsonArray profiles;
    for (const KiwiSdrAntennaProfile& p : m_profiles) {
        QJsonObject obj;
        obj.insert(QStringLiteral("id"), p.id);
        obj.insert(QStringLiteral("name"), p.name);
        obj.insert(QStringLiteral("endpoint"), normalizedProfileEndpoint(p.endpoint));
        obj.insert(QStringLiteral("autoConnect"), p.autoConnect);
        obj.insert(QStringLiteral("waterfallCellDb"), std::clamp(p.waterfallCellDb, -30, 30));
        obj.insert(QStringLiteral("waterfallFloorDb"), std::clamp(p.waterfallFloorDb, -30, 30));
        obj.insert(QStringLiteral("waterfallRate"),
                   std::clamp(p.waterfallRate, 0, kKiwiSdrWaterfallRateMax));
        profiles.append(obj);
    }

    QJsonObject root;
    root.insert(QStringLiteral("profiles"), profiles);
    auto& settings = AppSettings::instance();
    settings.setValue(kKiwiSdrRxAntennasSettingsKey, QString::fromUtf8(
        QJsonDocument(root).toJson(QJsonDocument::Compact)));
    settings.save();
}

bool KiwiSdrManager::shouldMaintainProfileConnection(const QString& id) const
{
    const int idx = profileIndex(id);
    if (idx < 0 || m_profiles[idx].endpoint.trimmed().isEmpty()) {
        return false;
    }

    return m_profiles[idx].autoConnect || assignedSliceForProfile(id) >= 0;
}

void KiwiSdrManager::scheduleReconnect(const QString& id)
{
    if (!shouldMaintainProfileConnection(id) && !reconnectRecommended(id)) {
        return;
    }

    QTimer* timer = m_reconnectTimers.value(id, nullptr);
    if (!timer) {
        timer = new QTimer(this);
        timer->setSingleShot(true);
        timer->setInterval(kRecoverableReconnectDelayMs);
        m_reconnectTimers.insert(id, timer);
        connect(timer, &QTimer::timeout, this, [this, id]() {
            if (shouldMaintainProfileConnection(id)
                || reconnectRecommended(id)) {
                connectProfile(id);
            }
        });
    }

    if (!timer->isActive()) {
        qCInfo(lcKiwiSdr).noquote()
            << "Reconnect scheduled for" << displayName(id)
            << "in" << kRecoverableReconnectDelayMs << "ms";
        timer->start();
    }
}

void KiwiSdrManager::scheduleWaitingReconnectIfRecommended(const QString& id)
{
    if (!reconnectRecommended(id)) {
        return;
    }

    scheduleReconnect(id);
}

void KiwiSdrManager::cancelReconnect(const QString& id)
{
    if (QTimer* timer = m_reconnectTimers.value(id, nullptr)) {
        timer->stop();
    }
}

QString KiwiSdrManager::sanitizedName(const QString& name,
                                      const QString& endpoint)
{
    const QString trimmed = name.trimmed();
    if (!trimmed.isEmpty()) {
        return trimmed.left(kKiwiSdrProfileNameMaxChars);
    }
    if (!endpoint.trimmed().isEmpty()) {
        return endpoint.trimmed().left(kKiwiSdrProfileNameMaxChars);
    }
    return QStringLiteral("KiwiSDR");
}

} // namespace AetherSDR
