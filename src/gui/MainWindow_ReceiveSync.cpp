#include "MainWindow.h"

#include "core/AudioEngine.h"
#include "core/AppSettings.h"
#include "core/KiwiSdrManager.h"
#include "models/SliceModel.h"

#include <QDateTime>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QMetaObject>
#include <QJsonObject>
#include <QSet>
#include <QStringList>
#include <QtConcurrent/QtConcurrentRun>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <utility>

namespace AetherSDR {
namespace {

constexpr int kDefaultReceivePresentationLatencyMs = 520;
constexpr int kDefaultReceivePresentationMaxOffsetMs = 3000;
constexpr int kReceiveSyncAnalysisRateHz = 12000;
constexpr int kReceiveSyncAnalysisWindowMs = 5000;
constexpr int kReceiveSyncEstimateIntervalMs = 1000;
constexpr int kReceiveSyncStableToleranceMs = 120;
constexpr int kReceiveSyncAppliedSettledToleranceMs = 25;
constexpr int kReceiveSyncCorrectionDeadbandMs = 35;
constexpr int kReceiveSyncFarRelockResidualMs = 250;
constexpr int kReceiveSyncStableLockCount = 3;
constexpr int kReceiveSyncWeakStableLockCount = 6;
constexpr int kReceiveSyncFarRelockCount = 6;
constexpr qint64 kReceiveSyncFrequencyToleranceHz = 50;
constexpr float kReceiveSyncStableLockMinConfidence = 0.18f;
constexpr float kReceiveSyncStableLockMinPeakCorrelation = 0.20f;
constexpr float kReceiveSyncWeakStableLockMinConfidence = 0.06f;
constexpr float kReceiveSyncWeakStableLockMinPeakCorrelation = 0.28f;
constexpr float kReceiveSyncImmediateLockMinConfidence = 0.55f;
constexpr float kReceiveSyncImmediateLockMinPeakCorrelation = 0.35f;
constexpr int kReceivePresentationAudioDrainGuardMs = 10;
constexpr int kReceivePresentationMaxPlaybackCompensationMs = 1000;
constexpr int kKiwiSdrMeterSndBlockCompensationMs = 45;
constexpr int kReceivePresentationAbruptDelayChangeMs = 250;
constexpr qsizetype kReceivePresentationVisualQueueMaxItems = 600;
constexpr qsizetype kReceivePresentationVisualMaxReleaseBatch = 24;
constexpr const char* kReceivePresentationSyncSettingsKey =
    "ReceivePresentationSync";
constexpr const char* kLegacyReceivePresentationSyncEnabledKey =
    "ReceivePresentationSyncEnabled";
constexpr const char* kLegacyReceivePresentationSyncModeKey =
    "ReceivePresentationSyncMode";
constexpr const char* kLegacyReceivePresentationSyncOffsetMsKey =
    "ReceivePresentationSyncOffsetMs";
constexpr const char* kLegacyReceivePresentationSyncLatencyMsKey =
    "ReceivePresentationSyncLatencyMs";
constexpr const char* kLegacyReceivePresentationSyncMaxOffsetMsKey =
    "ReceivePresentationSyncMaxOffsetMs";

bool settingIsTrue(const QString& value)
{
    return value.compare(QStringLiteral("True"), Qt::CaseInsensitive) == 0;
}

ReceiveSyncMode syncModeFromSetting(const QString& value)
{
    return value.compare(QStringLiteral("AutoAssist"), Qt::CaseInsensitive) == 0
        ? ReceiveSyncMode::AutoAssist
        : ReceiveSyncMode::Manual;
}

QString syncModeSettingValue(ReceiveSyncMode mode)
{
    return mode == ReceiveSyncMode::AutoAssist
        ? QStringLiteral("AutoAssist")
        : QStringLiteral("Manual");
}

ReceivePresentationSettings defaultReceivePresentationSettings()
{
    ReceivePresentationSettings settings;
    settings.enabled = true;
    settings.mode = ReceiveSyncMode::AutoAssist;
    settings.baseLatencyMs = kDefaultReceivePresentationLatencyMs;
    settings.manualOffsetMs = 0;
    settings.maxOffsetMs = kDefaultReceivePresentationMaxOffsetMs;
    return settings;
}

QJsonObject receivePresentationSettingsToJson(
    const ReceivePresentationSettings& settings)
{
    QJsonObject object;
    object.insert(QStringLiteral("enabled"), settings.enabled);
    object.insert(QStringLiteral("mode"), syncModeSettingValue(settings.mode));
    object.insert(QStringLiteral("manualOffsetMs"), settings.manualOffsetMs);
    object.insert(QStringLiteral("baseLatencyMs"), settings.baseLatencyMs);
    object.insert(QStringLiteral("maxOffsetMs"), settings.maxOffsetMs);
    return object;
}

QJsonObject parseReceivePresentationSettingsJson(const QString& value)
{
    QJsonParseError error;
    const QJsonDocument doc =
        QJsonDocument::fromJson(value.trimmed().toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        return {};
    }
    return doc.object();
}

ReceivePresentationSettings receivePresentationSettingsFromJson(
    const QJsonObject& object)
{
    ReceivePresentationSettings settings = defaultReceivePresentationSettings();
    if (object.contains(QStringLiteral("enabled"))) {
        settings.enabled = object.value(QStringLiteral("enabled")).toBool(
            settings.enabled);
    }
    const QString mode =
        object.value(QStringLiteral("mode")).toString(syncModeSettingValue(
            settings.mode));
    settings.mode = syncModeFromSetting(mode);
    settings.manualOffsetMs =
        object.value(QStringLiteral("manualOffsetMs")).toInt(
            settings.manualOffsetMs);
    settings.baseLatencyMs =
        object.value(QStringLiteral("baseLatencyMs")).toInt(
            settings.baseLatencyMs);
    settings.maxOffsetMs =
        object.value(QStringLiteral("maxOffsetMs")).toInt(
            settings.maxOffsetMs);
    return settings;
}

bool hasLegacyReceivePresentationSettings(const AppSettings& settings)
{
    return settings.contains(kLegacyReceivePresentationSyncEnabledKey)
        || settings.contains(kLegacyReceivePresentationSyncModeKey)
        || settings.contains(kLegacyReceivePresentationSyncOffsetMsKey)
        || settings.contains(kLegacyReceivePresentationSyncLatencyMsKey)
        || settings.contains(kLegacyReceivePresentationSyncMaxOffsetMsKey);
}

ReceivePresentationSettings receivePresentationSettingsFromLegacy(
    const AppSettings& settings)
{
    ReceivePresentationSettings receiveSettings =
        defaultReceivePresentationSettings();
    receiveSettings.enabled =
        settingIsTrue(settings.value(kLegacyReceivePresentationSyncEnabledKey,
                                     receiveSettings.enabled ? "True" : "False")
                          .toString());
    receiveSettings.mode = syncModeFromSetting(
        settings.value(kLegacyReceivePresentationSyncModeKey,
                       syncModeSettingValue(receiveSettings.mode))
            .toString());
    receiveSettings.baseLatencyMs =
        settings.value(kLegacyReceivePresentationSyncLatencyMsKey,
                       QString::number(receiveSettings.baseLatencyMs))
            .toInt();
    receiveSettings.manualOffsetMs =
        settings.value(kLegacyReceivePresentationSyncOffsetMsKey,
                       QString::number(receiveSettings.manualOffsetMs))
            .toInt();
    receiveSettings.maxOffsetMs =
        settings.value(kLegacyReceivePresentationSyncMaxOffsetMsKey,
                       QString::number(receiveSettings.maxOffsetMs))
            .toInt();
    return receiveSettings;
}

void removeLegacyReceivePresentationSettings(AppSettings& settings)
{
    settings.remove(kLegacyReceivePresentationSyncEnabledKey);
    settings.remove(kLegacyReceivePresentationSyncModeKey);
    settings.remove(kLegacyReceivePresentationSyncOffsetMsKey);
    settings.remove(kLegacyReceivePresentationSyncLatencyMsKey);
    settings.remove(kLegacyReceivePresentationSyncMaxOffsetMsKey);
}

void saveReceivePresentationSettings(
    const ReceivePresentationSettings& settings)
{
    AppSettings& appSettings = AppSettings::instance();
    const QJsonDocument doc(receivePresentationSettingsToJson(settings));
    appSettings.setValue(kReceivePresentationSyncSettingsKey,
                         QString::fromUtf8(
                             doc.toJson(QJsonDocument::Compact)));
    appSettings.save();
}

bool surfaceFollowsAudioPlayback(ReceivePresentationSurface surface)
{
    return surface == ReceivePresentationSurface::Waterfall
        || surface == ReceivePresentationSurface::Spectrum
        || surface == ReceivePresentationSurface::Meter;
}

bool surfaceUsesOrderedVisualQueue(ReceivePresentationSurface surface)
{
    return surface == ReceivePresentationSurface::Waterfall
        || surface == ReceivePresentationSurface::Spectrum
        || surface == ReceivePresentationSurface::Meter;
}

int sourceSurfaceCompensationMs(ReceivePresentationSource source,
                                ReceivePresentationSurface surface)
{
    if (source == ReceivePresentationSource::KiwiSdr
        && surface == ReceivePresentationSurface::Meter) {
        // The SND metadata meter describes the audio block that follows it.
        // Hold the GUI meter one observed 512-sample/12 kHz block so it lands
        // on the audible block instead of its WebSocket arrival time.
        return kKiwiSdrMeterSndBlockCompensationMs;
    }
    return 0;
}

bool kiwiPresentationSurface(ReceivePresentationSource source,
                             ReceivePresentationSurface surface)
{
    return source == ReceivePresentationSource::KiwiSdr
        && (surface == ReceivePresentationSurface::Audio
            || surface == ReceivePresentationSurface::Waterfall
            || surface == ReceivePresentationSurface::Spectrum
            || surface == ReceivePresentationSurface::Meter);
}

bool estimateMaintainsAutoLock(const ReceiveSyncEstimate& estimate,
                               float autoLockConfidence)
{
    return estimate.valid
        && (estimate.held || estimate.confidence >= autoLockConfidence);
}

qint64 receiveSyncSliceFrequencyHz(const SliceModel* slice)
{
    if (!slice) {
        return 0;
    }
    return static_cast<qint64>(
        std::llround(slice->frequency() * 1000000.0));
}

bool receiveSyncAudioAudible(bool muted, float gain)
{
    return !muted && gain > 0.0f;
}

QString receiveSyncStatusOverlayText(ReceiveSyncStatus status)
{
    switch (status) {
    case ReceiveSyncStatus::Off:
        return QStringLiteral("Off");
    case ReceiveSyncStatus::Manual:
        return QStringLiteral("Manual");
    case ReceiveSyncStatus::Searching:
        return QStringLiteral("Searching");
    case ReceiveSyncStatus::Holding:
        return QStringLiteral("Coasting");
    case ReceiveSyncStatus::Locked:
        return QStringLiteral("Locked");
    case ReceiveSyncStatus::LowConfidence:
        return QStringLiteral("Low conf");
    }
    return QStringLiteral("Off");
}

QString receiveSyncStatusName(ReceiveSyncStatus status)
{
    switch (status) {
    case ReceiveSyncStatus::Off:
        return QStringLiteral("off");
    case ReceiveSyncStatus::Manual:
        return QStringLiteral("manual");
    case ReceiveSyncStatus::Searching:
        return QStringLiteral("searching");
    case ReceiveSyncStatus::Holding:
        return QStringLiteral("coasting");
    case ReceiveSyncStatus::Locked:
        return QStringLiteral("locked");
    case ReceiveSyncStatus::LowConfidence:
        return QStringLiteral("lowConfidence");
    }
    return QStringLiteral("off");
}

} // namespace

void MainWindow::initReceivePresentationSync()
{
    AppSettings& s = AppSettings::instance();

    ReceivePresentationSettings settings = defaultReceivePresentationSettings();
    bool migratedLegacySettings = false;
    const QString persisted =
        s.value(kReceivePresentationSyncSettingsKey, QString{}).toString();
    const QJsonObject persistedObject =
        parseReceivePresentationSettingsJson(persisted);
    if (!persistedObject.isEmpty()) {
        settings = receivePresentationSettingsFromJson(persistedObject);
    } else if (hasLegacyReceivePresentationSettings(s)) {
        settings = receivePresentationSettingsFromLegacy(s);
        migratedLegacySettings = true;
    }

    m_receivePresentationSync.setSettings(settings);
    if (migratedLegacySettings) {
        removeLegacyReceivePresentationSettings(s);
        saveReceivePresentationSettings(m_receivePresentationSync.settings());
    }
    m_receiveAudioDelayEstimator.setConfig(
        ReceiveAudioDelayEstimator::Config{
            .sampleRateHz = kReceiveSyncAnalysisRateHz,
            .maxOffsetMs = settings.maxOffsetMs,
            .minOverlapMs = 750,
            .minPeakCorrelation = 0.12f,
                .minConfidence = 0.08f});
    m_receiveSyncEstimateTimer.start();
    m_receiveSyncDriftTimer.start();
    if (!m_receivePresentationVisualTimer) {
        m_receivePresentationVisualTimer = new QTimer(this);
        m_receivePresentationVisualTimer->setSingleShot(true);
        connect(m_receivePresentationVisualTimer, &QTimer::timeout,
                this, &MainWindow::drainReceivePresentationVisualQueue);
    }
}

ReceivePresentationSettings MainWindow::receivePresentationSettings() const
{
    return m_receivePresentationSync.settings();
}

ReceiveDelayBreakdown MainWindow::receivePresentationDelayBreakdown() const
{
    return m_receivePresentationSync.delayBreakdown();
}

QString MainWindow::receivePresentationOverlayStatsText() const
{
    const ReceivePresentationSettings settings =
        m_receivePresentationSync.settings();
    if (!settings.enabled) {
        return {};
    }

    const ReceiveSyncTarget target = resolveReceiveSyncTarget();
    const bool hasHeldDelayTarget =
        settings.mode == ReceiveSyncMode::AutoAssist
        && !receiveSyncDelayKiwiProfileId().isEmpty()
        && estimateMaintainsAutoLock(settings.autoEstimate,
                                     settings.autoLockConfidence);
    if (!target.usable() && !hasHeldDelayTarget) {
        return {};
    }

    const ReceiveDelayBreakdown delays =
        m_receivePresentationSync.delayBreakdown();

    const int delayMs = std::abs(delays.effectiveOffsetMs);
    const double delaySeconds = static_cast<double>(delayMs) / 1000.0;

    return QStringLiteral("Audio Sync: %1 / %2 sec delay")
        .arg(receiveSyncStatusOverlayText(delays.status))
        .arg(delaySeconds, 0, 'f', 2);
}

void MainWindow::setReceivePresentationSyncEnabled(bool enabled)
{
    ReceivePresentationSettings settings = m_receivePresentationSync.settings();
    settings.enabled = enabled;
    settings.autoEstimate = {};
    m_receivePresentationSync.setSettings(settings);
    resetReceivePresentationAutoAssistState(false);
    clearReceivePresentationVisualQueue();

    saveReceivePresentationSettings(m_receivePresentationSync.settings());
    syncReceivePresentationDelaysToAudioEngine();
}

void MainWindow::setReceivePresentationSyncMode(ReceiveSyncMode mode)
{
    ReceivePresentationSettings settings = m_receivePresentationSync.settings();
    settings.mode = mode;
    settings.autoEstimate = {};
    m_receivePresentationSync.setSettings(settings);
    resetReceivePresentationAutoAssistState(false);
    clearReceivePresentationVisualQueue();

    saveReceivePresentationSettings(m_receivePresentationSync.settings());
    syncReceivePresentationDelaysToAudioEngine();
}

void MainWindow::adjustReceivePresentationManualOffsetMs(int deltaMs)
{
    ReceivePresentationSettings settings = m_receivePresentationSync.settings();
    settings.manualOffsetMs += deltaMs;
    m_receivePresentationSync.setSettings(settings);
    settings = m_receivePresentationSync.settings();

    saveReceivePresentationSettings(settings);
    clearReceivePresentationVisualQueue();
    syncReceivePresentationDelaysToAudioEngine();
}

void MainWindow::resetReceivePresentationManualOffset()
{
    ReceivePresentationSettings settings = m_receivePresentationSync.settings();
    settings.manualOffsetMs = 0;
    m_receivePresentationSync.setSettings(settings);

    saveReceivePresentationSettings(m_receivePresentationSync.settings());
    clearReceivePresentationVisualQueue();
    syncReceivePresentationDelaysToAudioEngine();
}

void MainWindow::setReceivePresentationLatencyMs(int latencyMs)
{
    ReceivePresentationSettings settings = m_receivePresentationSync.settings();
    settings.baseLatencyMs = latencyMs;
    m_receivePresentationSync.setSettings(settings);
    settings = m_receivePresentationSync.settings();

    saveReceivePresentationSettings(settings);
    clearReceivePresentationVisualQueue();
    syncReceivePresentationDelaysToAudioEngine();
}

MainWindow::ReceiveSyncTarget MainWindow::resolveReceiveSyncTarget() const
{
    ReceiveSyncTarget target;
    if (!m_kiwiSdrManager) {
        target.reason = QStringLiteral("noKiwiManager");
        return target;
    }

    QVector<ReceiveSyncAudioPairEndpoint> flexCandidates;
    QVector<ReceiveSyncAudioPairEndpoint> kiwiCandidates;
    QSet<QString> seenKiwiProfiles;
    for (SliceModel* slice : m_radioModel.slices()) {
        if (!slice || !m_radioModel.sliceMayBelongToUs(slice->sliceId())) {
            continue;
        }

        const qint64 frequencyHz = receiveSyncSliceFrequencyHz(slice);
        if (frequencyHz <= 0) {
            continue;
        }

        const QString profile =
            m_kiwiSdrManager->assignedProfileForSlice(slice->sliceId());
        if (!profile.isEmpty()) {
            if (seenKiwiProfiles.contains(profile)
                || !m_kiwiSdrManager->isConnected(profile)
                || !receiveSyncAudioAudible(slice->audioMute(),
                                            slice->audioGain())) {
                continue;
            }
            seenKiwiProfiles.insert(profile);
            kiwiCandidates.append({.sliceId = slice->sliceId(),
                                   .frequencyHz = frequencyHz,
                                   .kiwiProfileId = profile});
            continue;
        }

        if (receiveSyncAudioAudible(slice->flexAudioMute(),
                                    slice->flexAudioGain())) {
            flexCandidates.append({.sliceId = slice->sliceId(),
                                   .frequencyHz = frequencyHz,
                                   .kiwiProfileId = QString()});
        }
    }

    const ReceiveSyncAudioPairSelection selection =
        selectReceiveSyncAudioPair(flexCandidates, kiwiCandidates,
                                   kReceiveSyncFrequencyToleranceHz);
    switch (selection.state) {
    case ReceiveSyncAudioPairSelection::State::None:
        target.state = ReceiveSyncTarget::State::None;
        break;
    case ReceiveSyncAudioPairSelection::State::Usable:
        target.state = ReceiveSyncTarget::State::Usable;
        break;
    case ReceiveSyncAudioPairSelection::State::Ambiguous:
        target.state = ReceiveSyncTarget::State::Ambiguous;
        break;
    }
    target.kiwiProfileId = selection.kiwiProfileId;
    target.flexSliceId = selection.flexSliceId;
    target.kiwiSliceId = selection.kiwiSliceId;
    target.frequencyHz = selection.frequencyHz;
    target.audibleFlexCount = selection.audibleFlexCount;
    target.audibleKiwiCount = selection.audibleKiwiCount;
    target.matchingPairCount = selection.matchingPairCount;
    target.reason = selection.reason;
    return target;
}

QString MainWindow::receiveSyncKiwiProfileId() const
{
    const ReceiveSyncTarget target = resolveReceiveSyncTarget();
    return target.usable() ? target.kiwiProfileId : QString();
}

QString MainWindow::receiveSyncDelayKiwiProfileId() const
{
    const ReceiveSyncTarget target = resolveReceiveSyncTarget();
    if (target.usable()) {
        return target.kiwiProfileId;
    }

    const ReceivePresentationSettings settings =
        m_receivePresentationSync.settings();
    if (settings.mode != ReceiveSyncMode::AutoAssist
        || m_receiveSyncKiwiProfileId.isEmpty()
        || !estimateMaintainsAutoLock(settings.autoEstimate,
                                      settings.autoLockConfidence)) {
        return {};
    }

    const int sliceId =
        m_kiwiSdrManager
            ? m_kiwiSdrManager->assignedSliceForProfile(m_receiveSyncKiwiProfileId)
            : -1;
    const SliceModel* slice = m_radioModel.slice(sliceId);
    if (!slice || !m_radioModel.sliceMayBelongToUs(sliceId)
        || !m_kiwiSdrManager->isConnected(m_receiveSyncKiwiProfileId)
        || !receiveSyncAudioAudible(slice->audioMute(), slice->audioGain())) {
        return {};
    }

    return m_receiveSyncKiwiProfileId;
}

qint64 MainWindow::receiveSyncTunedFrequencyHz() const
{
    const ReceiveSyncTarget target = resolveReceiveSyncTarget();
    if (target.usable()) {
        return target.frequencyHz;
    }

    return 0;
}

void MainWindow::holdReceivePresentationAutoAssistLock(bool clearVisualQueue)
{
    ReceivePresentationSettings settings =
        m_receivePresentationSync.settings();
    if (estimateMaintainsAutoLock(settings.autoEstimate,
                                  settings.autoLockConfidence)
        && !settings.autoEstimate.held) {
        settings.autoEstimate.held = true;
        m_receivePresentationSync.setSettings(settings);
    }

    m_receiveSyncFlexAudio.clear();
    m_receiveSyncKiwiAudio.clear();
    ++m_receiveSyncEstimateGeneration;
    m_receiveSyncHaveLastEstimate = false;
    m_receiveSyncStableEstimateCount = 0;
    if (clearVisualQueue) {
        clearReceivePresentationVisualQueue();
    }
    m_receiveSyncEstimateTimer.restart();
    m_receiveSyncDriftTimer.restart();
}

void MainWindow::resetReceivePresentationAutoAssistState(bool clearEstimate,
                                                        bool clearVisualQueue)
{
    if (clearEstimate) {
        ReceivePresentationSettings settings =
            m_receivePresentationSync.settings();
        settings.autoEstimate = {};
        m_receivePresentationSync.setSettings(settings);
    }

    m_receiveSyncFlexAudio.clear();
    m_receiveSyncKiwiAudio.clear();
    ++m_receiveSyncEstimateGeneration;
    m_receiveSyncKiwiProfileId.clear();
    m_receiveSyncHaveLastEstimate = false;
    m_receiveSyncStableEstimateCount = 0;
    m_receiveSyncLastCandidate = {};
    m_receiveSyncLastCandidateAbsoluteOffsetMs = 0;
    m_receiveSyncLastCandidateAvailable = false;
    m_receiveSyncLastAcceptedLock = false;
    m_receiveSyncLastNearAppliedLock = false;
    m_receiveSyncLastFarRelockEligible = false;
    m_receiveSyncLastFrequencyHz = 0;
    m_receiveSyncTargetUnavailable = false;
    if (clearVisualQueue) {
        clearReceivePresentationVisualQueue();
    }
    m_receiveSyncEstimateTimer.restart();
    m_receiveSyncDriftTimer.restart();
}

void MainWindow::feedReceivePresentationSyncAudio(
    ReceivePresentationSource source,
    const QByteArray& pcmStereoFloat,
    const QString& sourceId,
    int sampleRateHz)
{
    const ReceivePresentationSettings settings =
        m_receivePresentationSync.settings();
    if (!settings.enabled || settings.mode != ReceiveSyncMode::AutoAssist) {
        return;
    }

    const ReceiveSyncTarget target = resolveReceiveSyncTarget();
    if (!target.usable()) {
        if (!m_receiveSyncKiwiProfileId.isEmpty()
            && !m_receiveSyncTargetUnavailable) {
            const bool transientFrequencyMismatch =
                target.reason == QLatin1String("noFrequencyMatch")
                && m_receiveSyncLastFrequencyHz > 0;
            holdReceivePresentationAutoAssistLock(false);
            m_receiveSyncTargetUnavailable = true;
            syncReceivePresentationDelaysToAudioEngine(
                !transientFrequencyMismatch);
        }
        return;
    }

    if (source == ReceivePresentationSource::KiwiSdr
        && sourceId != target.kiwiProfileId) {
        return;
    }

    if (m_receiveSyncKiwiProfileId != target.kiwiProfileId) {
        const QString previousSyncProfile = m_receiveSyncKiwiProfileId;
        resetReceivePresentationAutoAssistState(true, false);
        if (!previousSyncProfile.isEmpty()) {
            clearReceivePresentationVisualQueueForSource(
                ReceivePresentationSource::KiwiSdr, previousSyncProfile);
            resetReceivePresentationAudioBuffersForKiwiSource(
                previousSyncProfile);
        }
        m_receiveSyncKiwiProfileId = target.kiwiProfileId;
        m_receiveSyncTargetUnavailable = false;
        syncReceivePresentationDelaysToAudioEngine();
    } else if (m_receiveSyncTargetUnavailable) {
        m_receiveSyncTargetUnavailable = false;
        m_receiveSyncFlexAudio.clear();
        m_receiveSyncKiwiAudio.clear();
        m_receiveSyncHaveLastEstimate = false;
        m_receiveSyncStableEstimateCount = 0;
        m_receiveSyncEstimateTimer.restart();
        m_receiveSyncDriftTimer.restart();
    }

    const qint64 tunedFrequencyHz = receiveSyncTunedFrequencyHz();
    if (tunedFrequencyHz > 0) {
        if (m_receiveSyncLastFrequencyHz == 0) {
            m_receiveSyncLastFrequencyHz = tunedFrequencyHz;
        } else if (m_receiveSyncLastFrequencyHz != tunedFrequencyHz) {
            holdReceivePresentationAutoAssistLock(false);
            m_receiveSyncLastFrequencyHz = tunedFrequencyHz;
            // A VFO knob can emit many small retunes per second. Flushing the
            // presentation audio buffers here forces repeated prebuffering, so
            // the operator hears dropouts while tuning. Clearing delayed visual
            // frames here also stalls the waterfall/FFT during knob movement.
            // Keep presentation queues flowing and only clear analysis state.
            syncReceivePresentationDelaysToAudioEngine(false);
            return;
        }
    }

    constexpr qsizetype kFrameBytes =
        2 * static_cast<qsizetype>(sizeof(float));
    const qsizetype frameCount = pcmStereoFloat.size() / kFrameBytes;
    if (frameCount <= 0) {
        return;
    }
    sampleRateHz = std::max(1, sampleRateHz);

    QVector<float>& dst =
        source == ReceivePresentationSource::Flex
            ? m_receiveSyncFlexAudio
            : m_receiveSyncKiwiAudio;
    const auto* samples =
        reinterpret_cast<const float*>(pcmStereoFloat.constData());
    const qsizetype outputSamples =
        std::max<qsizetype>(
            1,
            (frameCount * kReceiveSyncAnalysisRateHz) / sampleRateHz);
    dst.reserve(std::min<qsizetype>(
        dst.size() + outputSamples + 1,
        kReceiveSyncAnalysisRateHz * kReceiveSyncAnalysisWindowMs / 1000));

    for (qsizetype out = 0; out < outputSamples; ++out) {
        const qsizetype frameStart =
            (out * sampleRateHz) / kReceiveSyncAnalysisRateHz;
        qsizetype frameEnd =
            ((out + 1) * sampleRateHz) / kReceiveSyncAnalysisRateHz;
        frameEnd = std::clamp<qsizetype>(frameEnd, frameStart + 1, frameCount);
        const qsizetype clampedStart =
            std::clamp<qsizetype>(frameStart, 0, frameCount - 1);
        float monoSum = 0.0f;
        qsizetype count = 0;
        for (qsizetype frame = clampedStart; frame < frameEnd; ++frame) {
            const float left = samples[frame * 2];
            const float right = samples[frame * 2 + 1];
            monoSum += (std::isfinite(left) ? left : 0.0f) * 0.5f
                     + (std::isfinite(right) ? right : 0.0f) * 0.5f;
            ++count;
        }
        const float mono = count > 0 ? monoSum / static_cast<float>(count)
                                     : 0.0f;
        dst.append(std::clamp(mono, -1.0f, 1.0f));
    }

    const int maxSamples =
        kReceiveSyncAnalysisRateHz * kReceiveSyncAnalysisWindowMs / 1000;
    if (dst.size() > maxSamples) {
        dst.remove(0, dst.size() - maxSamples);
    }

    runReceivePresentationAutoAssist();
}

void MainWindow::runReceivePresentationAutoAssist()
{
    const ReceivePresentationSettings settings =
        m_receivePresentationSync.settings();
    if (!settings.enabled || settings.mode != ReceiveSyncMode::AutoAssist) {
        return;
    }
    if (!m_receiveSyncEstimateTimer.isValid()) {
        m_receiveSyncEstimateTimer.start();
    }
    if (m_receiveSyncEstimateTimer.elapsed() < kReceiveSyncEstimateIntervalMs) {
        return;
    }

    const int minSamples = kReceiveSyncAnalysisRateHz;
    if (m_receiveSyncFlexAudio.size() < minSamples
        || m_receiveSyncKiwiAudio.size() < minSamples) {
        return;
    }
    if (m_receiveSyncEstimateInFlight) {
        return;
    }

    m_receiveSyncEstimateTimer.restart();
    m_receiveSyncEstimateInFlight = true;
    const quint64 generation = ++m_receiveSyncEstimateGeneration;
    const QVector<float> flexAudio = m_receiveSyncFlexAudio;
    const QVector<float> kiwiAudio = m_receiveSyncKiwiAudio;
    const ReceiveAudioDelayEstimator::Config config =
        m_receiveAudioDelayEstimator.config();

    auto* watcher = new QFutureWatcher<ReceiveAudioDelayEstimate>(this);
    connect(watcher, &QFutureWatcher<ReceiveAudioDelayEstimate>::finished,
            this, [this, watcher, generation]() {
        const ReceiveAudioDelayEstimate estimate = watcher->result();
        watcher->deleteLater();
        m_receiveSyncEstimateInFlight = false;
        if (generation != m_receiveSyncEstimateGeneration) {
            return;
        }
        applyReceivePresentationAutoAssistEstimate(estimate);
    });
    watcher->setFuture(QtConcurrent::run(
        [config, flexAudio, kiwiAudio]() {
            ReceiveAudioDelayEstimator estimator(config);
            return estimator.estimate(flexAudio, kiwiAudio);
        }));
}

void MainWindow::applyReceivePresentationAutoAssistEstimate(
    const ReceiveAudioDelayEstimate& estimate)
{
    ReceivePresentationSettings settings = m_receivePresentationSync.settings();
    if (!settings.enabled || settings.mode != ReceiveSyncMode::AutoAssist) {
        return;
    }

    const ReceiveSyncEstimate previousEstimate = settings.autoEstimate;
    const bool hadAppliedAutoLock =
        estimateMaintainsAutoLock(previousEstimate,
                                  settings.autoLockConfidence);
    const int currentAppliedOffsetMs =
        m_receivePresentationSync.delayBreakdown().effectiveOffsetMs;
    const int maxCandidateOffsetMs =
        std::clamp(std::abs(settings.maxOffsetMs), 0,
                   ReceivePresentationSync::kMaxOffsetMs);
    const bool candidateAvailable =
        estimate.valid
        || estimate.peakCorrelation >= kReceiveSyncStableLockMinPeakCorrelation;
    int offsetMs = candidateAvailable
        ? std::clamp(currentAppliedOffsetMs + estimate.offsetMs,
                     -maxCandidateOffsetMs, maxCandidateOffsetMs)
        : settings.manualOffsetMs;
    m_receiveSyncLastCandidate = estimate;
    m_receiveSyncLastCandidateAvailable = candidateAvailable;
    m_receiveSyncLastCandidateAbsoluteOffsetMs = offsetMs;
    float confidence = estimate.confidence;
    int driftPpm = 0;
    bool acceptedLock = false;
    bool farRelockEligible = false;
    bool moderateCorrectionEligible = false;
    bool nearAppliedLock = !hadAppliedAutoLock;
    if (candidateAvailable) {
        bool stable = false;
        if (m_receiveSyncHaveLastEstimate && m_receiveSyncDriftTimer.isValid()) {
            stable =
                std::abs(offsetMs - m_receiveSyncLastEstimateOffsetMs)
                <= kReceiveSyncStableToleranceMs;
            const qint64 elapsedMs = m_receiveSyncDriftTimer.elapsed();
            if (elapsedMs > 0) {
                const int deltaMs =
                    offsetMs - m_receiveSyncLastEstimateOffsetMs;
                driftPpm = static_cast<int>(std::lround(
                    static_cast<double>(deltaMs) * 1000000.0
                    / static_cast<double>(elapsedMs)));
            }
        }
        m_receiveSyncStableEstimateCount =
            stable ? std::min(m_receiveSyncStableEstimateCount + 1,
                              kReceiveSyncFarRelockCount)
                   : 1;
        const bool stableLockEligible =
            m_receiveSyncStableEstimateCount >= kReceiveSyncStableLockCount
            && estimate.confidence >= kReceiveSyncStableLockMinConfidence
            && estimate.peakCorrelation >= kReceiveSyncStableLockMinPeakCorrelation;
        const bool weakStableLockEligible =
            m_receiveSyncStableEstimateCount >= kReceiveSyncWeakStableLockCount
            && estimate.confidence >= kReceiveSyncWeakStableLockMinConfidence
            && estimate.peakCorrelation
                   >= kReceiveSyncWeakStableLockMinPeakCorrelation;
        const bool immediateLockEligible =
            estimate.confidence >= kReceiveSyncImmediateLockMinConfidence
            && estimate.peakCorrelation >= kReceiveSyncImmediateLockMinPeakCorrelation;
        nearAppliedLock =
            !hadAppliedAutoLock
            || std::abs(offsetMs - previousEstimate.offsetMs)
                   <= kReceiveSyncStableToleranceMs;
        farRelockEligible =
            hadAppliedAutoLock
            && !nearAppliedLock
            && m_receiveSyncStableEstimateCount >= kReceiveSyncFarRelockCount
            && std::abs(estimate.offsetMs) >= kReceiveSyncFarRelockResidualMs
            && (immediateLockEligible || stableLockEligible);
        moderateCorrectionEligible =
            hadAppliedAutoLock
            && !nearAppliedLock
            && !farRelockEligible
            && std::abs(estimate.offsetMs) < kReceiveSyncFarRelockResidualMs
            && stableLockEligible;
        if (stableLockEligible || weakStableLockEligible) {
            confidence = std::max(confidence, settings.autoLockConfidence);
        } else if (!immediateLockEligible
                   && confidence >= settings.autoLockConfidence) {
            confidence = std::max(0.0f, settings.autoLockConfidence - 0.01f);
        }
        acceptedLock =
             (!hadAppliedAutoLock
              && (immediateLockEligible || stableLockEligible))
             || (hadAppliedAutoLock
                 && nearAppliedLock
                 && (stableLockEligible
                     || (weakStableLockEligible
                         && std::abs(estimate.offsetMs)
                                <= kReceiveSyncCorrectionDeadbandMs)))
             || moderateCorrectionEligible
             || farRelockEligible;
        if (!acceptedLock
            && confidence >= settings.autoLockConfidence) {
            confidence = std::max(0.0f, settings.autoLockConfidence - 0.01f);
        }
        m_receiveSyncLastEstimateOffsetMs = offsetMs;
        m_receiveSyncHaveLastEstimate = true;
        m_receiveSyncDriftTimer.restart();
    } else {
        m_receiveSyncStableEstimateCount = 0;
        m_receiveSyncHaveLastEstimate = false;
        m_receiveSyncDriftTimer.restart();
    }
    m_receiveSyncLastAcceptedLock = acceptedLock;
    m_receiveSyncLastNearAppliedLock = nearAppliedLock;
    m_receiveSyncLastFarRelockEligible = farRelockEligible;
    if (acceptedLock) {
        const bool resetAppliedOffset =
            !hadAppliedAutoLock || farRelockEligible;
        const bool keepAppliedOffset =
            hadAppliedAutoLock
            && std::abs(estimate.offsetMs) <= kReceiveSyncCorrectionDeadbandMs;
        const int appliedOffsetMs =
            keepAppliedOffset
                ? previousEstimate.offsetMs
                : ReceivePresentationSync::adjustedAutoOffsetMs(
                      previousEstimate.offsetMs, offsetMs, resetAppliedOffset);
        const bool stillSettling =
            !keepAppliedOffset
            && std::abs(offsetMs - appliedOffsetMs)
            > kReceiveSyncAppliedSettledToleranceMs;
        settings.autoEstimate = ReceiveSyncEstimate{
            .offsetMs = appliedOffsetMs,
            .confidence = confidence,
            .valid = true,
            .driftPpm = driftPpm,
            .held = stillSettling,
        };
    } else if (hadAppliedAutoLock) {
        settings.autoEstimate = previousEstimate;
        settings.autoEstimate.held = true;
    } else {
        settings.autoEstimate = ReceiveSyncEstimate{
            .offsetMs = offsetMs,
            .confidence = confidence,
            .valid = candidateAvailable,
            .driftPpm = driftPpm,
            .held = false,
        };
    }
    m_receivePresentationSync.setSettings(settings);
    syncReceivePresentationDelaysToAudioEngine();
}

QJsonObject MainWindow::automationReceiveSyncSnapshot() const
{
    const ReceivePresentationSettings settings =
        m_receivePresentationSync.settings();
    const ReceiveDelayBreakdown delays =
        m_receivePresentationSync.delayBreakdown();
    const ReceiveSyncTarget target = resolveReceiveSyncTarget();
    const AudioEngine::ReceivePresentationAudioQueues queues =
        m_audio ? m_audio->receivePresentationAudioQueues()
                : AudioEngine::ReceivePresentationAudioQueues{};
    const int flexDelayMs =
        receivePresentationDelayMs(ReceivePresentationSource::Flex,
                                   ReceivePresentationSurface::Audio);
    const int kiwiDelayMs =
        receivePresentationDelayMs(ReceivePresentationSource::KiwiSdr,
                                   ReceivePresentationSurface::Audio);

    return QJsonObject{
        {QStringLiteral("enabled"), settings.enabled},
        {QStringLiteral("mode"),
         settings.mode == ReceiveSyncMode::AutoAssist
             ? QStringLiteral("autoAssist")
             : QStringLiteral("manual")},
        {QStringLiteral("status"), receiveSyncStatusName(delays.status)},
        {QStringLiteral("statusText"),
         receiveSyncStatusOverlayText(delays.status)},
        {QStringLiteral("baseLatencyMs"), settings.baseLatencyMs},
        {QStringLiteral("manualOffsetMs"), settings.manualOffsetMs},
        {QStringLiteral("maxOffsetMs"), settings.maxOffsetMs},
        {QStringLiteral("autoLockConfidence"),
         settings.autoLockConfidence},
        {QStringLiteral("effectiveOffsetMs"), delays.effectiveOffsetMs},
        {QStringLiteral("flexDelayMs"), flexDelayMs},
        {QStringLiteral("kiwiDelayMs"), kiwiDelayMs},
        {QStringLiteral("rawFlexDelayMs"), delays.flexDelayMs},
        {QStringLiteral("rawKiwiDelayMs"), delays.kiwiDelayMs},
        {QStringLiteral("usableTarget"), target.usable()},
        {QStringLiteral("targetAmbiguous"), target.ambiguous()},
        {QStringLiteral("targetReason"), target.reason},
        {QStringLiteral("targetFlexSliceId"), target.flexSliceId},
        {QStringLiteral("targetKiwiSliceId"), target.kiwiSliceId},
        {QStringLiteral("targetResolvedKiwiProfileId"), target.kiwiProfileId},
        {QStringLiteral("targetDelayKiwiProfileId"),
         receiveSyncDelayKiwiProfileId()},
        {QStringLiteral("audibleFlexSliceCount"), target.audibleFlexCount},
        {QStringLiteral("audibleKiwiSliceCount"), target.audibleKiwiCount},
        {QStringLiteral("matchingPairCount"), target.matchingPairCount},
        {QStringLiteral("autoOffsetMs"), settings.autoEstimate.offsetMs},
        {QStringLiteral("autoConfidence"),
         settings.autoEstimate.confidence},
        {QStringLiteral("autoValid"), settings.autoEstimate.valid},
        {QStringLiteral("autoHeld"), settings.autoEstimate.held},
        {QStringLiteral("autoDriftPpm"), settings.autoEstimate.driftPpm},
        {QStringLiteral("candidateAvailable"),
         m_receiveSyncLastCandidateAvailable},
        {QStringLiteral("candidateResidualMs"),
         m_receiveSyncLastCandidate.offsetMs},
        {QStringLiteral("candidateAbsoluteOffsetMs"),
         m_receiveSyncLastCandidateAbsoluteOffsetMs},
        {QStringLiteral("candidateConfidence"),
         m_receiveSyncLastCandidate.confidence},
        {QStringLiteral("candidatePeakCorrelation"),
         m_receiveSyncLastCandidate.peakCorrelation},
        {QStringLiteral("candidateValid"),
         m_receiveSyncLastCandidate.valid},
        {QStringLiteral("stableEstimateCount"),
         m_receiveSyncStableEstimateCount},
        {QStringLiteral("lastAcceptedLock"), m_receiveSyncLastAcceptedLock},
        {QStringLiteral("lastNearAppliedLock"),
         m_receiveSyncLastNearAppliedLock},
        {QStringLiteral("lastFarRelockEligible"),
         m_receiveSyncLastFarRelockEligible},
        {QStringLiteral("haveLastEstimate"), m_receiveSyncHaveLastEstimate},
        {QStringLiteral("lastEstimateOffsetMs"),
         m_receiveSyncLastEstimateOffsetMs},
        {QStringLiteral("targetUnavailable"),
         m_receiveSyncTargetUnavailable},
        {QStringLiteral("targetKiwiProfileId"), m_receiveSyncKiwiProfileId},
        {QStringLiteral("frequencyHz"),
         static_cast<double>(m_receiveSyncLastFrequencyHz)},
        {QStringLiteral("flexAnalysisSamples"), m_receiveSyncFlexAudio.size()},
        {QStringLiteral("kiwiAnalysisSamples"), m_receiveSyncKiwiAudio.size()},
        {QStringLiteral("flexRawBufferMs"), queues.flexRawBufferMs},
        {QStringLiteral("flexOutputBufferMs"), queues.flexOutputBufferMs},
        {QStringLiteral("kiwiRawBufferMs"),
         std::max(queues.kiwiSdrRawBufferMs,
                  queues.externalKiwiRawBufferMs)},
        {QStringLiteral("kiwiOutputBufferMs"),
         std::max(queues.kiwiSdrOutputBufferMs,
                  queues.externalKiwiOutputBufferMs)},
        {QStringLiteral("playbackQueuedMs"), queues.playbackQueuedMs},
        {QStringLiteral("estimateTimerMs"),
         m_receiveSyncEstimateTimer.isValid()
             ? static_cast<int>(m_receiveSyncEstimateTimer.elapsed())
             : -1},
        {QStringLiteral("driftTimerMs"),
         m_receiveSyncDriftTimer.isValid()
             ? static_cast<int>(m_receiveSyncDriftTimer.elapsed())
             : -1},
    };
}

bool MainWindow::receivePresentationHasUsableSyncTarget() const
{
    return resolveReceiveSyncTarget().usable();
}

void MainWindow::resetReceivePresentationAudioBuffers()
{
    if (!m_audio) {
        return;
    }

    QMetaObject::invokeMethod(
        m_audio,
        [audio = m_audio]() {
            audio->resetReceivePresentationAudioBuffers();
        },
        Qt::QueuedConnection);
}

void MainWindow::resetReceivePresentationAudioBuffersForKiwiSource(
    const QString& sourceId)
{
    if (!m_audio || sourceId.trimmed().isEmpty()) {
        return;
    }

    QMetaObject::invokeMethod(
        m_audio,
        [audio = m_audio, sourceId]() {
            audio->resetReceivePresentationAudioBuffersForKiwiSource(
                sourceId);
        },
        Qt::QueuedConnection);
}

int MainWindow::receivePresentationDelayMs(
    ReceivePresentationSource source,
    ReceivePresentationSurface surface,
    const QString& sourceId) const
{
    const auto defaultKiwiDelayMs = [this, source, surface]() {
        int delayMs = kDefaultReceivePresentationLatencyMs;
        if (surfaceFollowsAudioPlayback(surface) && m_audio) {
            delayMs += std::clamp(m_audio->rxPlaybackQueuedMs()
                                      + kReceivePresentationAudioDrainGuardMs,
                                  0,
                                  kReceivePresentationMaxPlaybackCompensationMs);
        }
        delayMs += sourceSurfaceCompensationMs(source, surface);
        return delayMs;
    };

    const ReceivePresentationSettings settings =
        m_receivePresentationSync.settings();
    if (settings.enabled) {
        if (settings.mode == ReceiveSyncMode::AutoAssist) {
            const QString delayKiwiProfileId = receiveSyncDelayKiwiProfileId();
            if (delayKiwiProfileId.isEmpty()) {
                return 0;
            }
            const QString trimmedSourceId = sourceId.trimmed();
            if (kiwiPresentationSurface(source, surface)
                && !trimmedSourceId.isEmpty()
                && trimmedSourceId != delayKiwiProfileId) {
                return defaultKiwiDelayMs();
            }
        }
        int delayMs = m_receivePresentationSync.delayMs(source, surface);
        if (surfaceFollowsAudioPlayback(surface) && m_audio) {
            delayMs += std::clamp(m_audio->rxPlaybackQueuedMs()
                                      + kReceivePresentationAudioDrainGuardMs,
                                  0,
                                  kReceivePresentationMaxPlaybackCompensationMs);
        }
        delayMs += sourceSurfaceCompensationMs(source, surface);
        return delayMs;
    }

    // KiwiSDR audio is intentionally jitter-buffered before the final mix.
    // Even when cross-source sync is off, Kiwi visual surfaces should follow
    // that same presentation point instead of rendering directly on arrival.
    if (kiwiPresentationSurface(source, surface)) {
        return defaultKiwiDelayMs();
    }

    return 0;
}

void MainWindow::clearReceivePresentationVisualQueue()
{
    m_receivePresentationVisualQueue.clear();
    m_receivePresentationVisualLastDueMs.clear();
    if (m_receivePresentationVisualTimer) {
        m_receivePresentationVisualTimer->stop();
    }
}

void MainWindow::clearReceivePresentationVisualQueueForSource(
    ReceivePresentationSource source,
    const QString& sourceId)
{
    const QString trimmedSourceId = sourceId.trimmed();
    QSet<QString> sourceKeys;
    if (!trimmedSourceId.isEmpty()) {
        for (ReceivePresentationSurface surface :
             {ReceivePresentationSurface::Waterfall,
              ReceivePresentationSurface::Spectrum,
              ReceivePresentationSurface::Meter}) {
            sourceKeys.insert(
                receivePresentationVisualQueueKey(source, surface,
                                                  trimmedSourceId));
        }
    }

    const auto matchesSource = [&](const QString& key) {
        if (trimmedSourceId.isEmpty()) {
            const QString prefix =
                source == ReceivePresentationSource::Flex
                    ? QStringLiteral("flex:")
                    : QStringLiteral("kiwi:");
            return key.startsWith(prefix);
        }
        return sourceKeys.contains(key);
    };

    m_receivePresentationVisualQueue.removeIf(
        [&](const ReceivePresentationQueuedItem<std::function<void()>>& item) {
            return item.source == source && matchesSource(item.sourceId);
        });

    QStringList dueKeysToRemove;
    for (auto it = m_receivePresentationVisualLastDueMs.cbegin();
         it != m_receivePresentationVisualLastDueMs.cend(); ++it) {
        if (matchesSource(it.key())) {
            dueKeysToRemove.append(it.key());
        }
    }
    for (const QString& key : dueKeysToRemove) {
        m_receivePresentationVisualLastDueMs.remove(key);
    }
    scheduleReceivePresentationVisualQueue();
}

void MainWindow::scheduleReceivePresentationVisualQueue()
{
    if (m_receivePresentationVisualQueue.isEmpty()) {
        if (m_receivePresentationVisualTimer) {
            m_receivePresentationVisualTimer->stop();
        }
        return;
    }
    if (!m_receivePresentationVisualTimer) {
        m_receivePresentationVisualTimer = new QTimer(this);
        m_receivePresentationVisualTimer->setSingleShot(true);
        connect(m_receivePresentationVisualTimer, &QTimer::timeout,
                this, &MainWindow::drainReceivePresentationVisualQueue);
    }

    const std::optional<qint64> nextDue =
        m_receivePresentationVisualQueue.nextDueMs();
    if (!nextDue.has_value()) {
        m_receivePresentationVisualTimer->stop();
        return;
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const int intervalMs = static_cast<int>(
        std::clamp<qint64>(*nextDue - nowMs, 0, 60000));
    m_receivePresentationVisualTimer->start(intervalMs);
}

void MainWindow::drainReceivePresentationVisualQueue()
{
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const auto dueItems = m_receivePresentationVisualQueue.releaseDue(
        nowMs, kReceivePresentationVisualMaxReleaseBatch);
    for (const auto& item : dueItems) {
        if (item.payload) {
            item.payload();
        }
    }
    scheduleReceivePresentationVisualQueue();
}

void MainWindow::deferReceivePresentation(ReceivePresentationSource source,
                                          ReceivePresentationSurface surface,
                                          std::function<void()> apply,
                                          const QString& sourceId)
{
    if (!apply) {
        return;
    }

    const int delayMs = receivePresentationDelayMs(source, surface, sourceId);
    if (delayMs <= 0) {
        apply();
        return;
    }

    if (surfaceUsesOrderedVisualQueue(surface)) {
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        const QString key =
            receivePresentationVisualQueueKey(source, surface, sourceId);
        qint64 dueMs = nowMs + delayMs;
        const qint64 previousDueMs =
            m_receivePresentationVisualLastDueMs.value(key, 0);
        if (previousDueMs > 0 && dueMs <= previousDueMs) {
            dueMs = previousDueMs + 1;
        }
        m_receivePresentationVisualLastDueMs.insert(key, dueMs);
        m_receivePresentationVisualQueue.enqueue(
            {.source = source,
             .surface = surface,
             .sourceId = key,
             .arrivalMs = nowMs,
             .dueMs = dueMs,
             .sequence = ++m_receivePresentationVisualSequence,
             .payload = std::move(apply)});
        m_receivePresentationVisualQueue.trimToSize(
            kReceivePresentationVisualQueueMaxItems);
        scheduleReceivePresentationVisualQueue();
        return;
    }

    QTimer::singleShot(delayMs, this, [apply = std::move(apply)]() {
        apply();
    });
}

void MainWindow::syncReceivePresentationDelaysToAudioEngine(
    bool clearVisualQueueOnAbruptDelayChange)
{
    if (!m_audio) {
        return;
    }

    const int flexDelayMs =
        receivePresentationDelayMs(ReceivePresentationSource::Flex,
                                   ReceivePresentationSurface::Audio);
    const int kiwiDelayMs =
        receivePresentationDelayMs(ReceivePresentationSource::KiwiSdr,
                                   ReceivePresentationSurface::Audio);
    const ReceivePresentationSettings settings =
        m_receivePresentationSync.settings();
    const QString kiwiDelaySourceId =
        settings.enabled && settings.mode == ReceiveSyncMode::AutoAssist
            ? receiveSyncDelayKiwiProfileId()
            : QString();
    if (m_receivePresentationLastFlexAudioDelayMs >= 0
        && clearVisualQueueOnAbruptDelayChange) {
        if (std::abs(flexDelayMs - m_receivePresentationLastFlexAudioDelayMs)
            > kReceivePresentationAbruptDelayChangeMs) {
            clearReceivePresentationVisualQueueForSource(
                ReceivePresentationSource::Flex);
        }
        if (std::abs(kiwiDelayMs - m_receivePresentationLastKiwiAudioDelayMs)
            > kReceivePresentationAbruptDelayChangeMs) {
            clearReceivePresentationVisualQueueForSource(
                ReceivePresentationSource::KiwiSdr, kiwiDelaySourceId);
        }
    }
    m_receivePresentationLastFlexAudioDelayMs = flexDelayMs;
    m_receivePresentationLastKiwiAudioDelayMs = kiwiDelayMs;

    QMetaObject::invokeMethod(
        m_audio,
        [audio = m_audio, flexDelayMs, kiwiDelayMs, kiwiDelaySourceId]() {
            audio->setReceivePresentationDelays(
                flexDelayMs, kiwiDelayMs, kiwiDelaySourceId);
        },
        Qt::QueuedConnection);
}

} // namespace AetherSDR
