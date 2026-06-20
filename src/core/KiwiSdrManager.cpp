#include "KiwiSdrManager.h"

#include "AppSettings.h"
#include "LogManager.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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
    loadSettings();
}

KiwiSdrManager::~KiwiSdrManager()
{
    disconnectAll();
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
    if (KiwiSdrClient* c = client(id)) {
        return c->state();
    }
    return KiwiSdrClient::State::Disconnected;
}

QString KiwiSdrManager::stateDetail(const QString& id) const
{
    return m_stateDetails.value(id);
}

KiwiSdrReceiverTelemetry KiwiSdrManager::telemetry(const QString& id) const
{
    if (KiwiSdrClient* c = client(id)) {
        return c->telemetry();
    }
    return m_telemetry.value(id);
}

bool KiwiSdrManager::waterfallAvailable(const QString& id) const
{
    if (KiwiSdrClient* c = client(id)) {
        return c->waterfallAvailable();
    }
    return m_waterfallAvailable.value(id, true);
}

QString KiwiSdrManager::waterfallDetail(const QString& id) const
{
    if (KiwiSdrClient* c = client(id)) {
        return c->waterfallAvailabilityDetail();
    }
    return m_waterfallDetails.value(id);
}

bool KiwiSdrManager::isConnected(const QString& id) const
{
    if (KiwiSdrClient* c = client(id)) {
        return c->isConnected();
    }
    return false;
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
        c->setWaterfallDisplayAdjustments(updated.waterfallCellDb,
                                          updated.waterfallFloorDb);
        c->setWaterfallRateOverride(updated.waterfallRate);
        if (endpointChanged && c->state() != KiwiSdrClient::State::Disconnected) {
            c->disconnectFromEndpoint();
            if (!updated.endpoint.isEmpty()) {
                c->connectToEndpoint(updated.endpoint);
            }
        }
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
    if (KiwiSdrClient* c = m_clients.take(id)) {
        c->deleteLater();
    }
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
    m_telemetry.remove(id);
    m_waterfallAvailable.remove(id);
    m_waterfallDetails.remove(id);
    m_profiles.removeAt(idx);
    saveSettings();
    // The client is already deleted above, so no further audio will be fed for
    // this id; free its audio-engine source state (disable alone leaves the
    // per-source entry allocated — #3668 review).
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
    if (c->state() == KiwiSdrClient::State::Connecting
        || c->state() == KiwiSdrClient::State::Connected) {
        return;
    }
    c->setOperatorCallsign(m_operatorCallsign);
    c->setWaterfallDisplayAdjustments(m_profiles[idx].waterfallCellDb,
                                      m_profiles[idx].waterfallFloorDb);
    c->setWaterfallRateOverride(m_profiles[idx].waterfallRate);
    if (assignedSliceForProfile(id) < 0) {
        c->setTrackedSlice(-1, 0.0, QString(), 0, 0, QString());
        emit profileNeedsInitialTracking(id);
    }
    if (!c->hasTrackedSlice()) {
        return;
    }
    qCInfo(lcKiwiSdr).noquote()
        << "Connecting" << m_profiles[idx].name
        << "->" << m_profiles[idx].endpoint;
    c->connectToEndpoint(m_profiles[idx].endpoint);
}

void KiwiSdrManager::disconnectProfile(const QString& id)
{
    cancelReconnect(id);
    if (KiwiSdrClient* c = client(id)) {
        qCInfo(lcKiwiSdr).noquote() << "Disconnecting" << displayName(id);
        c->disconnectFromEndpoint();
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
    for (KiwiSdrClient* c : std::as_const(m_clients)) {
        if (c) {
            c->disconnectFromEndpoint();
        }
    }
}

void KiwiSdrManager::setOperatorCallsign(const QString& callsign)
{
    m_operatorCallsign = callsign;
    for (KiwiSdrClient* c : std::as_const(m_clients)) {
        if (c) {
            c->setOperatorCallsign(callsign);
        }
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
        c->setTrackedSlice(sliceId, frequencyMhz, mode, filterLowHz,
                           filterHighHz, panId);
        c->setWaterfallLineDurationMs(lineDurationMs);
        if (!panId.isEmpty() && centerMhz > 0.0 && bandwidthMhz > 0.0) {
            c->setWaterfallView(panId, centerMhz, bandwidthMhz);
        }
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
            previousClient->setAudioActive(false);
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

    if (KiwiSdrClient* c = ensureClient(profileId)) {
        c->setTrackedSlice(sliceId, frequencyMhz, mode, filterLowHz,
                           filterHighHz, panId);
        c->setAudioActive(c->isConnected());
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
        c->setAudioActive(false);
    }
    emit sliceAssignmentChanged(sliceId, QString());
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
        c->setTrackedSlice(sliceId, frequencyMhz, mode, filterLowHz,
                           filterHighHz, panId);
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
        c->setWaterfallLineDurationMs(lineDurationMs);
        c->setWaterfallView(panId, centerMhz, bandwidthMhz);
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

    auto* c = new KiwiSdrClient(this);
    c->setDecodeAudioWhenInactive(false);
    c->setOperatorCallsign(m_operatorCallsign);
    m_clients.insert(id, c);
    connect(c, &KiwiSdrClient::stateChanged,
            this, [this, id, c](KiwiSdrClient::State state, const QString& detail) {
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
        if (state == KiwiSdrClient::State::Connected) {
            const int idx = profileIndex(id);
            if (idx >= 0) {
                c->setWaterfallDisplayAdjustments(m_profiles[idx].waterfallCellDb,
                                                  m_profiles[idx].waterfallFloorDb);
                c->setWaterfallRateOverride(m_profiles[idx].waterfallRate);
            }
            if (assignedSliceForProfile(id) >= 0) {
                c->setAudioActive(true);
                emit audioSourceEnabledChanged(id, true);
            }
        } else if (state == KiwiSdrClient::State::Disconnected
                   || state == KiwiSdrClient::State::Error) {
            emit audioSourceEnabledChanged(id, false);
            emit meterReadingReady(
                id,
                KiwiSdrProtocol::meterUnavailable(
                    KiwiSdrProtocol::MeterSource::Unknown,
                    detail));
        }
        emit profileStateChanged(id, state, detail);
    });
    connect(c, &KiwiSdrClient::recoverableDisconnect,
            this, [this, id](const QString&) {
        scheduleReconnect(id);
    });
    connect(c, &KiwiSdrClient::telemetryChanged, this, [this, id, c]() {
        m_telemetry.insert(id, c->telemetry());
        emit profileTelemetryChanged(id, m_telemetry.value(id));
    });
    connect(c, &KiwiSdrClient::waterfallAvailabilityChanged,
            this, [this, id](bool available, const QString& detail) {
        m_waterfallAvailable.insert(id, available);
        if (detail.isEmpty()) {
            m_waterfallDetails.remove(id);
        } else {
            m_waterfallDetails.insert(id, detail);
        }
        emit profileWaterfallAvailabilityChanged(id, available, detail);
    });
    connect(c, &KiwiSdrClient::decodedAudioReady,
            this, [this, id](const QByteArray& pcm) {
        emit decodedAudioReady(id, pcm);
    });
    connect(c, &KiwiSdrClient::waterfallRowReady,
            this, [this, id](const QString& panId, const QVector<float>& binsDbm,
                             double lowFreqMhz, double highFreqMhz,
                             quint32 timecode) {
        emit waterfallRowReady(id, panId, binsDbm, lowFreqMhz, highFreqMhz,
                               timecode);
    });
    connect(c, &KiwiSdrClient::meterReadingReady,
            this, [this, id](const KiwiSdrProtocol::MeterReading& reading) {
        emit meterReadingReady(id, reading);
    });
    return c;
}

KiwiSdrClient* KiwiSdrManager::client(const QString& id) const
{
    return m_clients.value(id, nullptr);
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
    if (!shouldMaintainProfileConnection(id)) {
        return;
    }

    QTimer* timer = m_reconnectTimers.value(id, nullptr);
    if (!timer) {
        timer = new QTimer(this);
        timer->setSingleShot(true);
        timer->setInterval(kRecoverableReconnectDelayMs);
        m_reconnectTimers.insert(id, timer);
        connect(timer, &QTimer::timeout, this, [this, id]() {
            if (shouldMaintainProfileConnection(id)) {
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
